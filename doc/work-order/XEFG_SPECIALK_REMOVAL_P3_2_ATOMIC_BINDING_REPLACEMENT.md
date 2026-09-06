# XeFG Special K Removal — P3.2 Atomic Binding Replacement

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch: `master`

## 1. Purpose

P2 through P2.2 established the first working Special-K-free Intel XeFG render path, and P3.1 hardened the binding representation so the active XeFG swapchain, presentation queue, and device remain strongly owned while REFramework has a `VtableHook` installed.

The target topology remains intentionally narrow:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (this fork)
Special K    = absent
FG Output    = Intel XeFG
Renderer     = D3D12
```

P3.1 intentionally still rejects every changed active XeFG binding with:

```text
[XeFG][BindingGate] action = defer
```

P3.2 is the first lifecycle PR that must replace that defer behavior with a safe, serialized binding replacement transaction.

The goal is:

```text
validated new XeFG binding
    -> compare full binding identity
    -> identical: no-op
    -> changed: replace safely
    -> release old renderer resources before detaching old presentation binding
    -> keep old hooked COM object alive until its VtableHook is removed
    -> commit the new swapchain / queue / mode without half-committed state
    -> allow the next Present / Present1 to recreate REFramework rendering
```

P3.2 is **not** the general fullscreen/resize/Alt+Tab hardening phase. It should do exactly one thing well: safely replace an already-active XeFG internal presentation binding when a new validated binding arrives.

---

## 2. Required Base

The work order is written against current `master`:

```text
747894520cb45087cc94b319e75bd674f2e87099
Merge pull request #11 from onehoon/refactor/xefg-p3-1-binding-ownership-identity
P3.1: harden XeFG binding ownership and identity
```

P3.1 functional commit on the merged branch:

```text
135a816963cc5f0b8e66469948eae2ea75d80805
```

If `master` advances before implementation, rebase onto current `master` and preserve all merged P2/P2.1/P2.2/P3.1 behavior.

Read before editing:

```text
doc/REFramework_XeFG_P3_LIFECYCLE_ROBUSTNESS_PLAN_2026-09-06.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P3_1_BINDING_OWNERSHIP_IDENTITY.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P2_2_RESIZEBUFFERS1_PRE_RESET.md
src/D3D12Hook.cpp
src/D3D12Hook.hpp
src/REFramework.cpp
```

Also keep in mind the currently pinned `cursey/kananlib` `VtableHook` behavior:

```text
VtableHook(Address target)
    -> immediately replaces the target object's vtable pointer with a copied vtable

hook_method(index, fn)
    -> edits that copied vtable entry

~VtableHook()
    -> remove()
    -> restores the old vtable only while the target object is still valid
```

The target COM object is not `AddRef`'d by `VtableHook` itself.

---

## 3. P3.1 State That P3.2 Must Treat as Established

### 3.1 Pending binding is strongly owned

`PendingXefgBinding` now uses:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
Microsoft::WRL::ComPtr<ID3D12CommandQueue> selected_queue;
```

A validated candidate can therefore survive the XeFG-state handoff until the framework decides what to do with it.

### 3.2 Active XeFG binding is strongly owned

`D3D12Hook` now owns:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> m_xefg_bound_swapchain;
Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_xefg_bound_queue;
Microsoft::WRL::ComPtr<ID3D12Device4> m_xefg_bound_device;
```

The existing generic fields remain non-owning aliases for the renderer path:

```cpp
m_swap_chain
m_command_queue
m_device
```

### 3.3 Hook-before-COM-release ordering exists

P3.1 established:

```text
old XeFG strong ownership remains alive
    -> m_swapchain_hook is removed
    -> only then old XeFG ComPtrs are released
```

`unhook()` follows the same ordering.

P3.2 must preserve this invariant during replacement.

### 3.4 Full binding identity exists

P3.1 compares:

```text
swapchain pointer
selected command queue pointer
source
observe/render mode
```

For an already-active `XeFGInternal` binding, `publish_xefg_candidate()` currently classifies:

```text
identical
swapchain_changed
queue_changed
mode_changed
multiple_fields_changed
```

but every changed case still returns without modifying the active binding.

### 3.5 P2.2 render path must remain untouched

The proven active XeFG instance still hooks:

```text
Present[8]
Present1[22]
ResizeBuffers[13]
ResizeTarget[14]
ResizeBuffers1[39]
```

The confirmed Intel render queue remains the actual presentation-creation queue selected in P2.1, not the outer XeFG init queue.

The P2.2 `ResizeBuffers1` ordering remains:

```text
ResizeBuffers1 entry
    -> REFramework::on_reset()
    -> release REF backbuffer/RTV references
    -> original ResizeBuffers1 unchanged
    -> next Present/Present1 recreates renderer
```

Do not redesign any of that in P3.2.

---

## 4. Current Master Replacement Gap

Current `publish_xefg_candidate()` does this for an active XeFG binding:

```text
compare old/new binding identity
    -> log BindingGate
    -> if changed: defer
    -> if identical: unchanged
    -> return
```

This prevents unsafe replacement but means REFramework remains bound to stale presentation state when XeFG re-initialization legitimately changes:

```text
internal swapchain
presentation queue
observe/render mode
or more than one of those fields
```

P3.2 must remove only this deliberate lifecycle limitation.

Do not broaden the PR into generic D3D12 recovery.

---

## 5. Core P3.2 Design Decision — Split Same-Object and New-Object Replacement

P3.2 must not use one blind rebind sequence for every changed identity.

There are two materially different cases.

### Case A — Different swapchain object

```text
old_swapchain != new_swapchain
```

A new `VtableHook` can be prepared on the new object while the old object remains hooked, because they are distinct COM instances.

The safe sequence is:

```text
validate / strongly own new binding
    -> prepare a complete VtableHook on the NEW swapchain
    -> if preparation fails: destroy provisional new hook and keep old binding untouched
    -> reset old REFramework renderer resources
    -> remove old hook while old swapchain is still strongly owned
    -> commit new owned swapchain / queue / device / mode / hook
    -> release old strong ownership
    -> next Present/Present1 recreates renderer
```

### Case B — Same swapchain object, queue and/or mode changed

```text
old_swapchain == new_swapchain
```

Do **not** construct a second `VtableHook` while the existing hook is still installed on that same object.

The current `VtableHook` constructor copies the target's current vtable. If the current vtable is already REFramework's detoured copy, constructing another hook from it can preserve REFramework detours as the new "original" methods and create recursion / incorrect restoration behavior.

For same-object replacement:

```text
keep the existing VtableHook installed
    -> reset REFramework renderer resources
    -> replace strong queue/device ownership as needed
    -> update raw aliases
    -> update observe/render mode
    -> keep the swapchain hook intact
    -> next Present/Present1 continues through the same hook with the new queue/mode
```

This same-object path is mandatory for:

```text
queue_changed
mode_changed
queue + mode changed
```

when the swapchain pointer itself is unchanged.

---

## 6. Scope

### Explicitly in scope

```text
replace the P3.1 changed-binding defer gate
safe different-swapchain XeFG rebind
safe same-swapchain queue/mode update without rebuilding the hook
renderer reset before changing an active render binding
strong ownership throughout rebind
new-hook preparation before old-binding destruction where the swapchain changes
explicit failure behavior
binding generation tracking
bounded Rebind diagnostics
preserve P2.2 Present/Present1/ResizeBuffers1 path
preserve presentation-queue authority
```

### Explicitly out of scope

```text
ResizeBuffers/ResizeTarget/ResizeBuffers1 redesign
coalescing duplicate reset callbacks
fullscreen transition state machine
window/borderless policy
Alt+Tab detection
minimize/restore hook-monitor suppression
changing hook-monitor timeout
XeFG module unload/reload lifecycle
XeFG public proxy rendering
multiple simultaneous XeFG context architecture
Resident Evil Requiem stutter investigation
native/no-FG acceptance work
FSRFG
DLSS-G work
OptiScaler changes
Special K compatibility/emulation
NVIDIA MHW release-storm work
large generic D3D12 refactor
```

P3.3 and later own those areas.

---

## 7. Required Change A — Turn BindingGate `defer` Into Rebind Dispatch

Keep the P3.1 identity calculation and machine-readable reason classification.

For an active XeFG binding:

```cpp
const bool swapchain_changed = ...;
const bool queue_changed = ...;
const bool mode_changed = ...;
const bool changed = swapchain_changed || queue_changed || mode_changed;
```

### Identical binding

If `changed == false`:

```text
[XeFG][BindingGate] action = unchanged, reason = identical
```

then return exactly as P3.1 does now.

Do not reset or reinstall anything.

### Changed binding

If `changed == true`, change the gate behavior from:

```text
action = defer
return
```

to:

```text
action = rebind
call the XeFG replacement path
```

Recommended log:

```text
[XeFG][BindingGate] action = rebind, reason = swapchain_changed|queue_changed|mode_changed|multiple_fields_changed, ...
```

The exact reason must remain the same classification P3.1 introduced.

Do not route a changed active XeFG binding back through the generic initial `bind_external_swapchain()` path without an explicit replacement transaction.

---

## 8. Required Change B — Add an Explicit XeFG Replacement Method

Prefer a dedicated member such as:

```cpp
bool D3D12Hook::replace_xefg_binding(
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* command_queue,
    bool observe_only,
    const char* reason);
```

Naming may differ, but the semantics must be explicit.

The method is for an already-active `SwapchainSource::XeFGInternal` binding only.

Preconditions:

```text
hook_monitor_mutex is already held by publish_xefg_candidate()
current binding is active XeFGInternal
incoming swapchain and queue are validated, non-null
incoming queue/device relationship already passed publish_xefg_candidate() validation
```

The method must still defensively obtain the incoming swapchain D3D12 device before mutating active state.

Do not add a second active-binding mutex.

All replacement serialization remains under:

```cpp
g_framework->get_hook_monitor_mutex()
```

This is the same lifecycle lock used by Present/Present1 and hook-monitor replacement.

---

## 9. Required Change C — Prepare the New Binding Before Destroying the Old One

Before any destructive active-state change, create local strong references:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> next_swapchain = swapchain;
Microsoft::WRL::ComPtr<ID3D12CommandQueue> next_queue = command_queue;
Microsoft::WRL::ComPtr<ID3D12Device4> next_device;
```

Obtain `next_device` from the incoming swapchain and fail before reset/detach if that lookup fails.

The old active fields must remain unchanged during preparation:

```text
m_xefg_bound_swapchain
m_xefg_bound_queue
m_xefg_bound_device
m_swapchain_hook
m_swap_chain
m_command_queue
m_device
m_xefg_p21_observe_only
```

### Different-swapchain preparation

When `swapchain != m_swap_chain`, create a provisional new instance hook before resetting/removing the old binding.

Conceptual shape:

```cpp
auto next_hook = std::make_unique<VtableHook>(Address{next_swapchain.Get()});

const bool present_ok = next_hook->hook_method(8, ...);
const bool present1_ok = next_hook->hook_method(22, ...);
const bool resize_buffers_ok = next_hook->hook_method(13, ...);
const bool resize_target_ok = next_hook->hook_method(14, ...);
const bool resize_buffers1_ok = next_hook->hook_method(39, ...);

if (!(present_ok && present1_ok && resize_buffers_ok
      && resize_target_ok && resize_buffers1_ok)) {
    // next_hook destruction restores the new object's original vtable.
    // The old binding is still active and untouched.
    return false;
}
```

Use the exact existing hook targets:

```text
D3D12Hook::present
D3D12Hook::present1
D3D12Hook::resize_buffers
D3D12Hook::resize_target
D3D12Hook::resize_buffers1
```

Do not omit slot 39.

If a small private helper for installing/validating the five XeFG instance slots makes the code less duplicated, that is acceptable. Do not turn it into a generic graphics-hook framework.

### Why preparation happens first

If the new hook cannot be prepared, P3.2 must leave the proven old binding fully usable.

Do not call `on_reset()` and do not remove the old hook before this preparation succeeds.

---

## 10. Required Change D — Renderer Reset Before Active Binding Mutation

Once the incoming binding is fully prepared/validated, but before old renderer resources can observe a new queue or swapchain, call:

```cpp
g_framework->on_reset();
```

for every actual changed active XeFG binding.

Use the simple invariant:

```text
changed active XeFG binding
    -> reset renderer once before committing the changed binding
```

Do not try to optimize this away based on whether the old/new mode is observe-only in P3.2. A binding replacement is a rare lifecycle event, and one explicit reset keeps renderer/backbuffer/queue ownership unambiguous.

P3.3 may later determine from runtime evidence whether reset normalization/coalescing is useful.

Required ordering:

```text
new binding prepared
    -> [XeFG][Rebind] old_renderer_reset
    -> on_reset()
    -> detach/update binding
```

Do not move `on_reset()` after old hook removal.

---

## 11. Required Change E — Different-Swapchain Commit Transaction

For:

```text
new_swapchain != old_swapchain
```

use this exact conceptual ordering.

### Stage 1 — Begin

Old binding is still fully active and strongly owned.

Log:

```text
[XeFG][Rebind] stage = begin
```

### Stage 2 — Prepare new owned objects and provisional hook

No old-state mutation yet.

If this stage fails:

```text
provisional new hook is removed/restored by destruction
old binding remains active
renderer is not reset
return false
```

### Stage 3 — Reset old renderer resources

```cpp
g_framework->on_reset();
```

Log:

```text
stage = old_renderer_reset
```

### Stage 4 — Remove old instance hook while old swapchain is still strongly owned

The P3.1 lifetime invariant remains mandatory:

```text
old m_xefg_bound_swapchain alive
    -> old m_swapchain_hook removed
    -> only afterwards may old ownership be released
```

Recommended shape:

```cpp
auto old_hook = std::move(m_swapchain_hook);

// Strong old swapchain ownership is still in m_xefg_bound_swapchain.
old_hook.reset();
```

Do not clear `m_xefg_bound_swapchain` first.

Log:

```text
stage = old_hook_removed
```

### Stage 5 — Commit new binding

Only after the old hook is detached:

```cpp
m_xefg_bound_swapchain = std::move(next_swapchain);
m_xefg_bound_queue = std::move(next_queue);
m_xefg_bound_device = std::move(next_device);

m_swap_chain = m_xefg_bound_swapchain.Get();
m_command_queue = m_xefg_bound_queue.Get();
m_device = m_xefg_bound_device.Get();

m_swapchain_hook = std::move(next_hook);
m_swapchain_source = SwapchainSource::XeFGInternal;
m_xefg_p21_observe_only = observe_only;
m_xefg_p21_render_boundary_logged = false;
m_is_phase_1 = false;
m_hooked = true;
```

Preserve other existing active-state semantics unless a field is directly tied to the binding.

Do not route the new XeFG queue through the legacy private `s_command_queue_offset` discovery.

### Stage 6 — Generation increment / commit log

Increment the XeFG binding generation only after the new active binding is committed successfully.

Log:

```text
stage = new_binding_committed
```

At this point future Present/Present1 calls should naturally use the new instance hook and new authoritative presentation queue.

---

## 12. Required Change F — Same-Swapchain Queue/Mode Replacement

For:

```text
new_swapchain == old_swapchain
```

and at least one of:

```text
queue_changed
mode_changed
```

keep the existing `m_swapchain_hook` installed.

Do not construct a second `VtableHook` on the same object.

Required ordering:

```text
validate / strongly own incoming queue/device
    -> renderer reset
    -> keep existing swapchain hook
    -> update selected queue ownership/alias
    -> update device ownership/alias if necessary
    -> update observe/render mode
    -> reset one-shot render-boundary diagnostic state
    -> increment binding generation
```

Conceptual shape:

```cpp
// Same swapchain: m_xefg_bound_swapchain and m_swapchain_hook remain intact.

g_framework->on_reset();

m_xefg_bound_queue = std::move(next_queue);
m_xefg_bound_device = std::move(next_device);

m_swap_chain = m_xefg_bound_swapchain.Get();
m_command_queue = m_xefg_bound_queue.Get();
m_device = m_xefg_bound_device.Get();

m_xefg_p21_observe_only = observe_only;
m_xefg_p21_render_boundary_logged = false;
```

If the incoming `ComPtr<IDXGISwapChain3>` is the same raw interface pointer as the active swapchain, do not replace/reset the active swapchain strong reference merely for style.

### Queue-only change

```text
same swapchain
new selected presentation queue
same mode
```

must be supported without hook rebuild.

### Mode-only change

```text
same swapchain
same selected queue
observe_only toggles
```

must be supported without hook rebuild.

### Queue + mode change

Same behavior: one renderer reset, one in-place binding update, one generation increment.

---

## 13. Required Change G — Binding Generation

Add a small XeFG binding generation counter to `D3D12Hook`.

Recommended member:

```cpp
uint64_t m_xefg_binding_generation{0};
```

Semantics:

```text
initial successful XeFG binding -> generation = 1
identical publication -> generation unchanged
successful changed binding replacement -> generation += 1
failed replacement -> generation unchanged
unhook / full active XeFG teardown -> generation = 0
```

If current control flow makes setting generation 1 inside the initial `bind_external_swapchain()` cleaner, do so only for `XeFGInternal`.

Do not add a process-global generation/state machine.

This counter exists to correlate runtime lifecycle evidence, not to drive unrelated logic.

Optional getter:

```cpp
uint64_t get_xefg_binding_generation() const;
```

Only add it if required for logging outside the class.

---

## 14. Required Change H — Explicit Failure Semantics

P3.2 must not silently leave half-committed active state.

### Failure before renderer reset

Examples:

```text
incoming device unavailable
new different-swapchain VtableHook preparation failed
one required hook_method failed
```

Required result:

```text
old binding remains active
old hook remains active
old strong ownership remains active
renderer was not reset
binding generation unchanged
new provisional ownership/hook is released normally
```

Log:

```text
[XeFG][Rebind] stage = failed, reason = ...
```

Suggested reasons:

```text
new_device_unavailable
new_hook_create_failed
new_hook_method_failed
```

Do not add broad fallback paths.

### Failure after renderer reset

Design P3.2 so there are no expected fallible preparation steps after the renderer reset.

In particular:

```text
prepare the different-swapchain hook BEFORE on_reset()
validate all required vtable slots BEFORE on_reset()
obtain new device BEFORE on_reset()
```

After reset begins, the transaction should be limited to deterministic ownership/hook moves and field updates.

Do not intentionally create a rollback state machine for pathological allocator/exception scenarios that the current codebase does not normally model.

If a standard C++ allocation exception occurs, existing process behavior may apply; do not add large defensive machinery solely for theoretical failure interleavings.

---

## 15. VtableHook Removal Semantics

The pinned `VtableHook::remove()` restores the old vtable only when the target object's current vtable still points to the hook's copied vtable.

P3.2 does not need a complex recovery mechanism around this.

For a different-swapchain replacement:

- keep the old COM object strongly owned while removing/destroying the old hook,
- then release old ownership,
- commit the already-prepared new hook.

It is acceptable to rely on the current destructor-driven `remove()` behavior as P3.1 already does.

If implementation chooses to call `remove()` explicitly for diagnostics, do not treat a `false` result as an automatic reason to restore the old active binding after the new binding is already prepared. A `false` result can also mean another layer changed the vtable after REFramework's hook.

Avoid adding speculative recovery complexity without a runtime failure proving it is needed.

---

## 16. Required Diagnostics

Keep P3 diagnostics state-change oriented and bounded.

### 16.1 Binding gate

Preserve the existing P3.1 fields but use `rebind` for changed cases:

```text
[XeFG][BindingGate]
action = unchanged | rebind
reason = identical | swapchain_changed | queue_changed | mode_changed | multiple_fields_changed
old_swapchain = ...
new_swapchain = ...
old_queue = ...
new_queue = ...
old_observe_only = ...
new_observe_only = ...
```

### 16.2 Rebind stages

Add:

```text
[XeFG][Rebind]
stage = begin | new_hook_prepared | old_renderer_reset | old_hook_removed | same_object_updated | new_binding_committed | failed
reason = swapchain_changed | queue_changed | mode_changed | multiple_fields_changed | <failure_reason>
generation = ...
old_swapchain = ...
new_swapchain = ...
old_queue = ...
new_queue = ...
old_observe_only = ...
new_observe_only = ...
```

Not every stage needs every field repeated if one concise helper makes the log clearer, but the begin and commit/failure records must contain enough identity to correlate the transition.

### 16.3 Different-object preparation

Log once when all five new hook slots have been installed successfully:

```text
stage = new_hook_prepared
```

### 16.4 Same-object update

Log:

```text
stage = same_object_updated
```

and make clear that the existing VtableHook was retained.

### 16.5 Preserve existing diagnostics

Do not remove or rename:

```text
[XeFG][InitDesc]
[XeFG][InternalSwapchain]
[XeFG][QueueIdentity]
[XeFG][Bind]
[XeFG][P2.1Probe]
[D3D12][ExternalBind]
[D3D12][PresentEntry]
[XeFG][ResizeBuffers1]
[D3D12][HookMonitor]
```

The Special-K-removal investigation is still active and these markers remain valuable.

---

## 17. Initial Bind vs Replacement

Do not unnecessarily rewrite the proven initial XeFG bind path.

Preferred separation:

```text
no active XeFG binding
    -> existing bind_external_swapchain() path

active XeFG binding + identical identity
    -> no-op

active XeFG binding + changed identity
    -> new P3.2 replace_xefg_binding() path
```

It is acceptable to extract a very small helper for installing the five XeFG instance hook slots if both initial bind and replacement can use it without broadening the diff.

Do not force a large refactor merely to deduplicate a few lines.

---

## 18. P2.2 / P3.1 Behavior That Must Not Regress

### Presentation queue remains authoritative

For the confirmed Intel topology:

```text
init queue         = direct priority 0
presentation queue = direct priority 100
relation           = distinct_same_device
```

Continue using the selected presentation queue for REFramework rendering.

Never overwrite it in `present_common()` from `s_command_queue_offset` for `XeFGInternal`.

### Present / Present1 behavior

Do not change the callback ordering or original-call forwarding.

### ResizeBuffers1 behavior

Keep:

```text
pre-original on_reset()
original arguments unchanged
next Present/Present1 renderer recreation
```

### Observe-only behavior

Keep the existing semantics:

```text
observe_only == true
    -> renderer/mod callbacks suppressed
    -> original Present/Present1 still forwarded
    -> hook monitor liveness maintained
```

P3.2 only allows the mode to change safely during a validated binding replacement.

### Hook monitor

Do not suppress or extend the hook monitor in this PR.

A successful rebind should naturally resume Present/Present1 activity.

---

## 19. Locking Requirements

All active binding identity comparison and replacement remains serialized under:

```cpp
g_framework->get_hook_monitor_mutex()
```

Do not add a new active binding mutex.

Do not hold `g_xefg_state_mutex` while:

```text
calling g_framework->on_reset()
constructing/removing active VtableHooks
committing active D3D12Hook fields
```

The pending/transaction state lock remains for XeFG capture bookkeeping only.

Maintain the existing lock ordering and avoid introducing a reverse order between:

```text
g_xefg_state_mutex
hook_monitor_mutex
```

Do not hold either lock while calling the original outer XeFG initialization API.

---

## 20. Files Expected in Scope

Primary:

```text
src/D3D12Hook.cpp
src/D3D12Hook.hpp
```

`src/REFramework.cpp` should not require behavioral changes. Touch it only if a tiny existing reset/logging interface is genuinely required.

Expected functional change:

```text
roughly 120-250 LOC
prefer <250 LOC
avoid >300 LOC
```

One focused PR is preferred.

---

## 21. Non-Goals — Must Not Sneak Into P3.2

Do not implement:

```text
ResizeTarget/ResizeBuffers reset coalescing
fullscreen/window-mode generation state
Alt+Tab/minimize state tracking
hook-monitor pause/suppression
module unload/reload cleanup
XeFG context destroy API tracking
public proxy generation state
Requiem-specific behavior
frame pacing changes
latency changes
DRED
private Intel offsets
manual COM refcount instrumentation
new polling/background threads
native/FSRFG acceptance expansion
OptiScaler source changes
```

If hardware testing exposes one of these issues, preserve the log and handle it in the corresponding later P3 work order.

---

## 22. Implementation Order

Implement in this order.

### Step 1 — Add generation and replacement method skeleton

- add XeFG binding generation state,
- keep initial bind behavior unchanged,
- no behavior change yet.

### Step 2 — Convert BindingGate changed case to replacement dispatch

- identical remains immediate no-op,
- changed reasons are preserved,
- changed case calls the explicit replacement method.

### Step 3 — Implement same-swapchain path

- no new VtableHook,
- reset renderer once,
- update queue/device/mode ownership and aliases,
- generation increment/log.

### Step 4 — Implement different-swapchain preparation

- strongly own new binding,
- create provisional new hook,
- validate all five required slots,
- fail without touching old binding if preparation fails.

### Step 5 — Implement different-swapchain commit

- reset renderer,
- remove old hook while old swapchain remains owned,
- commit new strong ownership and hook,
- increment generation,
- log.

### Step 6 — Build/static validation

Do not add P3.3 behavior during this step.

---

## 23. Static Review Checklist

Before opening/updating the PR, verify each item directly from the final diff.

### Identity / gate

```text
[ ] identical active binding is still a no-op
[ ] swapchain-only change enters replacement
[ ] queue-only change enters replacement
[ ] mode-only change enters replacement
[ ] multiple-field change enters replacement
[ ] BindingGate reason remains machine-readable
```

### Same-object path

```text
[ ] same swapchain does not create a second VtableHook
[ ] existing instance hook remains installed
[ ] renderer reset occurs before queue/mode mutation
[ ] selected queue strong ownership is updated
[ ] raw command_queue alias follows the new owned queue
[ ] observe/render mode is updated
[ ] generation increments exactly once
```

### Different-object path

```text
[ ] new swapchain/queue/device are strongly owned locally before mutation
[ ] provisional new VtableHook is prepared before renderer reset
[ ] all five required XeFG slots are validated
[ ] preparation failure leaves old active binding untouched
[ ] renderer reset occurs only after new hook preparation succeeds
[ ] old hook is removed while old swapchain strong ownership still exists
[ ] old ownership is not released before old hook removal
[ ] new hook/ownership/pointers are committed together
[ ] generation increments only after successful commit
```

### P2.2 preservation

```text
[ ] Present[8] preserved
[ ] Present1[22] preserved
[ ] ResizeBuffers[13] preserved
[ ] ResizeTarget[14] preserved
[ ] ResizeBuffers1[39] preserved
[ ] ResizeBuffers1 pre-reset preserved
[ ] presentation queue remains authoritative
[ ] observe-only Present forwarding/liveness preserved
[ ] hook-monitor timing/policy unchanged
```

### Scope

```text
[ ] no P3.3/P3.4/P3.5 behavior added
[ ] no OptiScaler changes
[ ] no native/FSRFG expansion
[ ] no large generic refactor
```

---

## 24. Build / Static Validation

Codex must run the current repository equivalent of:

```text
cmake --preset vs2022
cmake --build build --config Release --target REFramework
python dev/audit_direct_access_clang.py
git diff --check
```

If the repository's normal CI uses a different generated build directory because of an existing local environment issue, run an isolated equivalent Release configure/build as needed and report exactly what happened.

Do not replace the Release build with Debug-only validation.

Expected artifact:

```text
build/bin/REFramework/dinput8.dll
```

or the exact isolated-build equivalent if required.

Codex must not claim DD2/MHW runtime success.

---

## 25. P3.2 Code Acceptance Criteria

The PR is code-complete when all of the following are true:

```text
[ ] current master / P3.1 ownership model is preserved
[ ] changed XeFG binding is no longer unconditionally deferred
[ ] identical XeFG binding remains a no-op
[ ] different-swapchain replacement prepares a new hook before touching the old binding
[ ] failed new-hook preparation leaves old binding active
[ ] same-swapchain queue/mode changes do not rebuild the vtable hook
[ ] renderer resources are reset before any changed active binding is committed
[ ] old hooked swapchain remains strongly owned until old VtableHook removal
[ ] actual selected presentation queue remains authoritative
[ ] new active queue/device/swapchain are strongly owned
[ ] no half-committed active pointer state is intentionally reachable in normal replacement flow
[ ] successful replacement increments binding generation
[ ] failed/identical publications do not increment generation
[ ] bounded BindingGate/Rebind logs exist
[ ] P2.2 ResizeBuffers1 behavior is unchanged
[ ] Release build succeeds
[ ] static audit succeeds
[ ] git diff --check succeeds
[ ] no hardware-runtime claim is made by Codex
```

---

## 26. Required PR Report

The PR description/final Codex report must state only verifiable facts:

```text
base commit used
files changed
how changed BindingGate behavior now dispatches replacement
same-swapchain update behavior
new-swapchain transactional preparation/commit behavior
VtableHook-before-COM-release ordering
binding generation semantics
failure behavior before renderer reset
P2.2 behaviors intentionally preserved
Release build result
static audit result
git diff --check result
artifact path
```

Explicitly state:

```text
No DD2 or MHW runtime validation was performed by Codex.
P3.2 implements binding replacement only.
Resize/fullscreen normalization, Alt+Tab/minimize policy, and XeFG module teardown remain deferred to later P3 work.
```

---

## 27. User Hardware Validation After PR Review

Codex does not perform this section.

The user should test the P3.2 artifact on Intel hardware.

### Test A — Dragon's Dogma 2 baseline regression

Topology:

```text
OptiScaler = dxgi.dll
REFramework = P3.2 dinput8.dll
Special K = absent
FG Output = XeFG
GPU = Intel
```

First confirm the proven baseline:

```text
game launches
XeFG remains active
OptiScaler overlay visible
REFramework overlay visible
Present/Present1 continues
ResizeBuffers1 remains S_OK
no DEVICE_REMOVED regression
no ACCESS_DENIED regression
no INVALID_CALL regression caused by REF backbuffer ownership
no recurring hook-monitor recovery during active rendering
```

### Test B — Deliberately exercise lifecycle changes

Perform where supported:

```text
change resolution at least twice
switch fullscreen/window/borderless modes
change a graphics option known to recreate/resize the presentation path
Alt+Tab out/back several times
```

The purpose of P3.2 is specifically to catch a second validated XeFG binding if one is produced.

### Required log evidence if a changed binding occurs

Expect a sequence similar to:

```text
[XeFG][BindingGate] action = rebind, reason = ...
[XeFG][Rebind] stage = begin, generation = N, ...

// different swapchain only:
[XeFG][Rebind] stage = new_hook_prepared, ...

[XeFG][Rebind] stage = old_renderer_reset, ...

// different swapchain only:
[XeFG][Rebind] stage = old_hook_removed, ...
[XeFG][Rebind] stage = new_binding_committed, generation = N+1, ...

// or same swapchain:
[XeFG][Rebind] stage = same_object_updated, generation = N+1, ...
```

After commit:

```text
Present/Present1 must continue
REF renderer should reinitialize on the new/current binding
REF overlay should return/remain visible
Opti overlay/XeFG should remain functional
```

### If no changed binding naturally occurs

Do not invent a synthetic production code path just to force it.

Report that the baseline survived the lifecycle test and that only identical publications were observed, if applicable.

Keep the logs for ChatGPT analysis before P3.3.

---

## 28. Monster Hunter Wilds Follow-up

After DD2 passes the corresponding P3.2 test, run an Intel MHW smoke/lifecycle test with the same build.

MHW should be used as a second XeFG lifecycle workload.

Requirements:

```text
initial XeFG direct binding still works
REF overlay visible
Opti overlay/XeFG remain active
same-object publications do not rebuild the hook unnecessarily
changed internal binding, if observed, follows the P3.2 rebind transaction
no repeated hook teardown/install on identical binding
```

Do not mix this test with the separate NVIDIA/MHW OptiScaler release-storm investigation.

---

## 29. Completion Gate for P3.3

Do not design P3.3 from assumptions alone.

After P3.2 code review and user hardware logs, decide whether the existing resize transition behavior actually needs normalization.

Possible outcomes:

```text
A. P3.2 handles binding replacement and current reset behavior is stable
   -> P3.3 may be diagnostics-only or skipped

B. logs show repeated ResizeTarget/ResizeBuffers1 reset callbacks cause a real issue
   -> write a narrow P3.3 normalization work order from that evidence

C. logs show Alt+Tab/minimize triggers healthy-binding hook-monitor recovery
   -> preserve evidence for P3.4; do not fix it inside P3.3 unless scope is explicitly revised
```

Resident Evil Requiem remains deferred until P3 lifecycle acceptance is complete.

---

## 30. Core Rule

P3.2 should be judged by one invariant:

> A newly validated XeFG presentation binding may replace the active binding without ever requiring REFramework to dereference a dead old hooked swapchain, without rebuilding a hook on the same already-hooked object, and without exposing the renderer to a new swapchain/queue before its old resources are reset.

Keep the implementation small, explicit, and reviewable.