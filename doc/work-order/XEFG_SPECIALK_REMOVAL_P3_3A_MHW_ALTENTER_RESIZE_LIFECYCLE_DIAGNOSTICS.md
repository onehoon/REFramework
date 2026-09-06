# XeFG Special K Removal — P3.3A MHW Alt+Enter Resize Lifecycle Diagnostics

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch for implementation: `master`

## 1. Purpose

P2 through P2.2 established a working Special-K-free Intel XeFG presentation path. P3.1 added strong ownership and complete active binding identity. P3.2 added safe atomic replacement for a changed XeFG internal presentation binding.

The target topology remains intentionally narrow:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (this fork)
Special K    = absent
FG Output    = Intel XeFG
Renderer     = D3D12
Primary game = Monster Hunter Wilds
```

After P3.2 runtime validation, Monster Hunter Wilds exposed a reproducible Alt+Enter failure. A direct P3.1 comparison then reproduced the same failure class and the same hook-monitor recovery cadence.

Therefore this is **not a P3.2 atomic-rebind regression**.

The current evidence supports this classification:

```text
MHW Alt+Enter crash
    = reproducible on P3.1
    = reproducible on P3.2
    = P3.2 BindingGate/Rebind path not exercised in either failing session
    = pre-existing XeFG fullscreen/resize lifecycle problem
```

P3.3A is a **diagnostic-only PR**.

Its purpose is not to guess at a fix. Its purpose is to produce enough structured runtime evidence to answer the following question unambiguously:

> During the MHW Alt+Enter transition, does REFramework release its D3D12 backbuffer references at `ResizeTarget`, reacquire them on an intermediate Present, and then still hold them when a later resize path fails with `E_PENDING`; or is the failing resize outside REFramework's tracked internal swapchain / caused by another owner?

The desired evidence timeline is:

```text
Alt+Enter
    -> ResizeTarget entry
    -> REFramework reset begins
    -> REF backbuffer ownership before reset
    -> REF backbuffer ownership after reset
    -> original ResizeTarget return
    -> first Present/Present1 after ResizeTarget
    -> renderer callback before/after resource reacquisition
    -> exact backbuffer pointers reacquired by REF
    -> later ResizeBuffers / ResizeBuffers1 entry, if REFramework sees it
    -> REF ownership before/after the resize reset callback
    -> original resize return
    -> presentation continuation or stop
    -> hook-monitor Last chance / rehook only as downstream evidence
```

Do **not** implement transition suppression, delayed renderer recreation, reset coalescing, hook-monitor suppression, or any other behavior change in P3.3A.

The next functional P3.3 step must be designed from the evidence produced by this PR.

---

## 2. Required Base

Write the implementation against current `master`:

```text
3d91a22908239bc70c168323180544e9731d4133
P3.2: replace changed XeFG bindings safely
```

P3.2 PR:

```text
#12
P3.2: replace changed XeFG bindings safely
```

If `master` advances before implementation, rebase onto the then-current `master` and preserve all merged XeFG behavior.

Read before editing:

```text
doc/REFramework_XeFG_P3_LIFECYCLE_ROBUSTNESS_PLAN_2026-09-06.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P2_2_RESIZEBUFFERS1_PRE_RESET.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P3_1_BINDING_OWNERSHIP_IDENTITY.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P3_2_ATOMIC_BINDING_REPLACEMENT.md
src/D3D12Hook.cpp
src/D3D12Hook.hpp
src/REFramework.cpp
src/REFramework.hpp
```

P3.3A must preserve the P3.2 active-binding transaction exactly. Do not refactor P3.2 while adding diagnostics.

---

## 3. Runtime Evidence That Justifies P3.3A

### 3.1 P3.2 MHW failure

The P3.2 MHW session shows:

```text
Hooking D3D12      = 6
Unhooking D3D12    = 5
Last chance        = 5
rehook request     = 5
BindingGate        = 0
Rebind             = 0
ResizeBuffers1     = 6/6 S_OK
```

Recovery requests occurred at approximately:

```text
10:57:45.130
10:57:56.775   +11.645 s
10:58:07.785   +11.010 s
10:58:18.795   +11.010 s
10:58:29.804   +11.009 s
```

The first failure boundary is the important part:

```text
10:57:38.330  REF: D3D12 resize target called
10:57:38.330  REF: Reset!
10:57:38.356  REF: VR on_device_reset
10:57:38.373  REF: renderer reinitialization completed

10:57:38.374  Opti: last observed successful LocalPresent original result = 0

10:57:38.882  Opti: Back buffers have outstanding references
10:57:38.882  Opti: hkResizeBuffers Result: 8000000A

10:57:44.124  REF: Last chance encountered for hooking
10:57:45.130  REF: Sending rehook request for D3D
```

The crash artifact reports:

```text
ExceptionCode    : C0000005
ExceptionAddress : 000000014CD94BF0
Message          : Fatal D3D error (8, E_PENDING, 0x8000000a)
```

The text crash stack does not directly identify `dinput8.dll` or `dxgi.dll` as the crashing frame. Do not claim DLL ownership from this artifact alone.

### 3.2 P3.1 MHW Alt+Enter reproduces the same class

A dedicated P3.1 MHW Alt+Enter run also reproduces:

```text
Last chance      = 6
rehook request   = 6
Unhook -> Hook   = 6
ResizeBuffers1   = 6/6 S_OK
Opti same-output recreate warnings = 3
```

Its recovery cadence is effectively the same:

```text
first interval   ~= 11.400 s
later intervals  ~= 11.010 - 11.012 s
```

The P3.1 crash artifact has the same relevant fields:

```text
ExceptionCode    : C0000005
ExceptionAddress : 000000014CD94BF0
Message          : Fatal D3D error (8, E_PENDING, 0x8000000a)
```

This is strong evidence that the MHW Alt+Enter failure predates P3.2.

### 3.3 P3.2 changed-binding code is not implicated by runtime coverage

Both the P3.1 and P3.2 failing MHW sessions have:

```text
BindingGate = 0
Rebind      = 0
```

Therefore no observed failure went through:

```text
[XeFG][BindingGate] action = rebind
[XeFG][Rebind] ...
```

Do not modify or revert P3.2 replacement logic as part of this work order.

### 3.4 DD2 is not part of this work order

A temporary DD2 DLSS capability discrepancy was re-tested and is no longer reproducible after cleaning the REFramework-generated `storage` directory when switching REF versions.

Current project decision:

```text
DD2 DLSS issue -> ignore for now
MHW Alt+Enter  -> highest priority
```

Do not add DD2-specific code or Streamline/DLSS capability changes to P3.3A.

When swapping test DLLs in future A/B testing, archive or clear generated REFramework storage as appropriate so stale generated files do not confound binary comparisons. This is test hygiene only, not an implementation requirement.

---

## 4. Current Code Facts P3.3A Must Preserve

### 4.1 Active XeFG instance hook

The current active XeFG internal swapchain hooks:

```text
Present[8]
ResizeBuffers[13]
ResizeTarget[14]
Present1[22]
ResizeBuffers1[39]
```

P3.3A must not add another active swapchain hook or alter any original call arguments.

### 4.2 Resize callbacks currently converge on `REFramework::on_reset()`

`REFramework::hook_d3d12()` registers:

```cpp
m_d3d12_hook->on_resize_buffers([this](D3D12Hook& hook) { on_reset(); });
m_d3d12_hook->on_resize_target([this](D3D12Hook& hook) { on_reset(); });
```

`ResizeBuffers1[39]` also invokes the existing resize callback before calling the original method when the active XeFG path is rendering rather than observe-only.

Therefore all three relevant resize entry points can cause renderer reset behavior.

### 4.3 `ResizeBuffers1` currently has the proven P2.2 ordering

For the tracked XeFG internal instance:

```text
ResizeBuffers1 enter
    -> pre_reset_begin
    -> m_on_resize_buffers(...)
    -> REFramework::on_reset()
    -> pre_reset_end
    -> original ResizeBuffers1
    -> original_return
```

P2.2 established that this ordering is required to avoid `DXGI_ERROR_INVALID_CALL` from outstanding REF resources on the observed internal path.

P3.3A must not change it.

### 4.4 Generic `ResizeBuffers[13]` already calls the reset callback before the original

Current code does:

```text
ResizeBuffers entry
    -> m_on_resize_buffers(...)
    -> original ResizeBuffers
    -> log error only if result != S_OK
```

This matters to the diagnosis.

If REFramework sees the same failing `ResizeBuffers` that OptiScaler reports as `E_PENDING`, then the REF reset callback should already have run before the original call.

If the structured P3.3A log shows that REF **does not** receive a `ResizeBuffers[13]` entry at the failure boundary, that strongly suggests the failing OptiScaler `hkResizeBuffers` call is occurring on another wrapper/proxy/object path outside the currently tracked internal swapchain hook.

This distinction is one of the primary deliverables of P3.3A.

### 4.5 `ResizeTarget[14]` currently resets before the original

Current code does:

```text
ResizeTarget entry
    -> m_on_resize_target(...)
    -> REFramework::on_reset()
    -> original ResizeTarget
```

The MHW runtime evidence shows a renderer reinitialization and successful presentation after this reset but before the later Opti `hkResizeBuffers E_PENDING` event.

This is the main reason the intermediate renderer-reacquisition hypothesis is plausible.

### 4.6 REFramework strongly owns swapchain backbuffers in `m_d3d12.rts`

During D3D12 initialization, REFramework enumerates swapchain backbuffers using:

```cpp
swapchain->GetBuffer(i, IID_PPV_ARGS(&m_d3d12.rts[i]))
```

The corresponding `ComPtr<ID3D12Resource>` values are strong COM references.

`deinit_d3d12()` eventually clears the D3D12 state with:

```cpp
m_d3d12 = {};
```

Therefore P3.3A can directly prove whether **REFramework itself** holds swapchain backbuffers by logging the raw pointers stored in the backbuffer `m_d3d12.rts[]` slots before reset, after deinit/reset, and after a subsequent D3D12 renderer initialization.

Do not use speculative private Intel or OptiScaler object-layout offsets for this purpose.

### 4.7 Present can trigger renderer recreation immediately after reset

The XeFG `Present` / `Present1` path converges through `present_common()`, which runs the registered REFramework present callback when render callbacks are enabled.

That callback reaches the D3D12 renderer path and may initialize/reinitialize D3D12 resources after a reset.

This is exactly the transition that must be correlated with the later failing resize.

### 4.8 Hook-monitor recovery is currently downstream evidence

The generic hook monitor behaves approximately as:

```text
~5 seconds without recognized present activity
    -> Last chance
~1 second later
    -> rehook request
```

The repeated ~11 second loop begins only after presentation has already stopped.

P3.3A must not change monitor timing or suppress recovery. It should only preserve/log the existing recovery evidence.

---

## 5. Working Hypotheses

P3.3A exists to distinguish these hypotheses. Do not hard-code any of them as truth.

### Hypothesis A — REF reacquires backbuffers between `ResizeTarget` and a later resize

Possible sequence:

```text
ResizeTarget
    -> REF on_reset
    -> REF releases m_d3d12.rts backbuffers
    -> original ResizeTarget succeeds
    -> intermediate Present/Present1
    -> REF renderer initializes again
    -> REF GetBuffer reacquires backbuffers
    -> later ResizeBuffers path
    -> outstanding references exist
    -> E_PENDING
```

This hypothesis is plausible because the P3.2 MHW timeline shows renderer reinitialization / successful presentation before the Opti `hkResizeBuffers E_PENDING` boundary.

It is **not proven** yet.

### Hypothesis B — REF sees the failing `ResizeBuffers`, resets, and still gets `E_PENDING`

Possible sequence:

```text
REF ResizeBuffers[13] entry
    -> REF reset
    -> REF backbuffer slots become empty
    -> original ResizeBuffers
    -> E_PENDING
```

If this is observed, REFramework's own backbuffer references are unlikely to be the complete explanation. Another owner may still hold references, or the failing call may have additional constraints unrelated to COM ownership.

Do not implement delayed REF renderer recreation based only on this outcome.

### Hypothesis C — the failing Opti `hkResizeBuffers` is outside the REF-tracked internal instance

Possible sequence:

```text
REF ResizeTarget on internal presentation swapchain
    -> REF reset/reinit
    -> Opti/Streamline/game calls ResizeBuffers on outer wrapper/proxy/another swapchain
    -> REF active internal ResizeBuffers[13] hook never sees it
    -> E_PENDING
```

If this happens, the next step is not generic reset coalescing. We need to identify which presentation-layer object receives the failing call and how its lifecycle relates to the REF-owned internal presentation swapchain.

P3.3A should provide enough identity/timing evidence to justify a later targeted diagnostic hook if required.

### Hypothesis D — swapchain or presentation identity changes without a P3.2 candidate publication

The current P3.1/P3.2 failures show no `BindingGate/Rebind` event. However a fullscreen transition might still involve a wrapper/proxy lifecycle boundary not visible through the current `InitFromSwapChainDesc -> CreateSwapChainForHwnd` candidate path.

If resize diagnostics show a different COM identity or a call outside the active internal instance, later P3 work may need lifecycle observation at another boundary.

Do not add such a hook in P3.3A.

---

## 6. Scope

### Explicitly in scope

```text
MHW Alt+Enter only as the reproducer
structured XeFG ResizeTarget diagnostics
structured XeFG ResizeBuffers diagnostics
structured XeFG ResizeBuffers1 diagnostics
active swapchain / queue / device / generation correlation
thread-id correlation
original method owner and return-value correlation
REF D3D12 backbuffer ownership snapshots
REF D3D12 deinit/reset before/after snapshots
REF D3D12 backbuffer reacquisition logging
first bounded Present/Present1 activity after a resize event
existing hook-monitor snapshot correlation
clear diagnostics sufficient to decide the next functional P3.3 step
```

### Explicitly out of scope

```text
delaying renderer reinitialization
adding a fullscreen transition state machine
coalescing Reset! callbacks
suppressing Reset! callbacks
changing ResizeTarget behavior
changing ResizeBuffers behavior
changing ResizeBuffers1 behavior
changing original resize arguments or return values
hooking SetFullscreenState
hooking additional public/proxy swapchains
changing hook-monitor timeout
suppressing hook-monitor recovery
changing P3.2 BindingGate/Rebind behavior
forcing a changed binding for test coverage
changing OptiScaler
changing Streamline
changing XeFG runtime DLLs
DD2 DLSS changes
Special K compatibility
Resident Evil Requiem stutter work
native/no-FG acceptance
FSRFG
large D3D12 refactor
COM refcount polling on every frame
new worker threads or polling loops
```

---

## 7. Core Diagnostic Design — One Resize Timeline Namespace

Add one structured diagnostic namespace:

```text
[XeFG][ResizeLifecycle]
```

Every MHW-relevant XeFG resize event should be correlatable using a monotonically increasing diagnostic event ID.

Recommended state in `D3D12Hook`:

```cpp
uint64_t m_xefg_resize_event_id{0};
uint64_t m_xefg_last_resize_event_id{0};
std::chrono::steady_clock::time_point m_xefg_last_resize_event_time{};
```

If useful, also store a small enum/string-compatible kind:

```cpp
enum class XefgResizeEventKind : uint8_t {
    None,
    ResizeTarget,
    ResizeBuffers,
    ResizeBuffers1,
};
```

This is diagnostic correlation state only.

Do not turn it into a functional transition state machine.

The event counter semantics should be simple:

```text
every top-level tracked XeFG ResizeTarget entry   -> ++event_id
every top-level tracked XeFG ResizeBuffers entry  -> ++event_id
every top-level tracked XeFG ResizeBuffers1 entry -> ++event_id
nested recursive calls                            -> no new lifecycle event unless needed for an explicit nested log
```

All structured logs for one original call use the same event ID.

Do not reset P3.2 `m_xefg_binding_generation` when creating a resize event. Binding generation and resize event ID are separate concepts.

---

## 8. Required Change A — Structured `ResizeTarget[14]` Diagnostics

Keep the existing function and original call behavior.

For the active XeFG internal tracked instance, add a structured top-level sequence around the existing callback/original call.

Required fields at `enter`:

```text
event_id
kind = ResizeTarget
stage = enter
thread_id
swapchain
tracked_swapchain
hook_instance
binding_generation
command_queue
device
observe_only
new_width
new_height
refresh_numerator
refresh_denominator
format
scanline_ordering
scaling
original_fn
original_owner
```

If `new_target_parameters == nullptr`, do not dereference it. Log null safely and preserve the original call unchanged.

Before the existing `m_on_resize_target()` callback, log:

```text
stage = pre_reset
```

Then request a REFramework resource snapshot described later in this work order.

After the callback returns, log:

```text
stage = post_reset
```

and request another resource snapshot.

After calling the original method, always log:

```text
stage = original_return
result = 0x........
```

Do not log only failures. `S_OK` is important evidence.

The conceptual shape is:

```cpp
const auto event_id = begin_resize_diag(ResizeTarget, swap_chain);
log_resize_target_enter(...);

log_resource_snapshot("resize_target_pre_reset", event_id);
if (d3d12->m_on_resize_target) {
    d3d12->m_on_resize_target(*d3d12);
}
log_resource_snapshot("resize_target_post_reset", event_id);

++g_resize_target_depth;
const auto result = original(...);
--g_resize_target_depth;

log_resize_original_return(event_id, "ResizeTarget", result);
return result;
```

This is conceptual only. Preserve current recursion protection and existing safety logic.

---

## 9. Required Change B — Structured `ResizeBuffers[13]` Diagnostics

This is a critical part of P3.3A.

We need to know whether REFramework's active internal swapchain hook sees the resize that OptiScaler later reports as:

```text
Back buffers have outstanding references
hkResizeBuffers Result: 8000000A
```

For top-level `ResizeBuffers[13]` on the active XeFG internal instance, add structured logs with:

```text
event_id
kind = ResizeBuffers
stage = enter
thread_id
swapchain
tracked_swapchain
hook_instance
binding_generation
command_queue
device
observe_only
buffer_count
width
height
format
flags
original_fn
original_owner
```

Before `m_on_resize_buffers()`:

```text
stage = pre_reset
```

Take a REF resource snapshot.

After the callback:

```text
stage = post_reset
```

Take another snapshot.

After the original call:

```text
stage = original_return
result = 0x........
```

Always record the result, including `S_OK`, `E_PENDING (0x8000000A)`, and any other failure.

Preserve the current recursion guard behavior exactly.

### Required negative evidence

The runtime analysis must be able to state one of these explicitly:

```text
REF ResizeBuffers[13] entry observed at the failure boundary
```

or:

```text
No REF ResizeBuffers[13] entry was observed near the Opti hkResizeBuffers E_PENDING boundary
```

The absence of the event is itself a major diagnostic result.

Do not synthesize or force a ResizeBuffers call.

---

## 10. Required Change C — Extend `ResizeBuffers1[39]` Diagnostics Without Changing P2.2 Behavior

Keep the current P2.2 semantics exactly:

```text
tracked XeFG internal instance
    -> optional pre-original reset when rendering
    -> original ResizeBuffers1
    -> unchanged return value
```

Extend the existing `[XeFG][ResizeBuffers1]` data or additionally emit `[XeFG][ResizeLifecycle]` records so the event can be correlated with the same fields used by the other resize methods.

Required correlation fields:

```text
event_id
kind = ResizeBuffers1
thread_id
swapchain
tracked_swapchain
hook_instance
binding_generation
command_queue
device
observe_only
buffer_count
width
height
format
flags
creation_node_mask
present_queues
original_fn
original_owner
```

Required stages:

```text
enter
pre_reset
post_reset
original_return
```

Keep the existing P2.2 logs if useful for continuity:

```text
[XeFG][ResizeBuffers1] stage = enter
[XeFG][ResizeBuffers1] stage = pre_reset_begin
[XeFG][ResizeBuffers1] stage = pre_reset_end
[XeFG][ResizeBuffers1] stage = original_return
```

Do not remove those markers solely for logging style consistency.

---

## 11. Required Change D — Add a Read-Only REFramework D3D12 Resource Snapshot Helper

Add a small diagnostic helper owned by `REFramework`, because `D3D12Hook` must not directly reach into `REFramework` private renderer state.

Recommended API shape:

```cpp
void REFramework::log_d3d12_resize_snapshot(
    std::string_view stage,
    uint64_t event_id) const;
```

The name may differ, but it must be read-only and diagnostic-only.

At minimum log:

```text
[XeFG][RendererState]
stage
event_id
initialized
renderer_type
backbuffer_ref_count
backbuffer_0 pointer
backbuffer_1 pointer
backbuffer_2 pointer
... only through the configured backbuffer slots
rtv_desc_heap pointer
srv_desc_heap pointer
command_context_count
graphics_memory pointer
imgui_backend_data_0
imgui_backend_data_1
```

### What counts as a REF-held backbuffer

Only count the backbuffer slots:

```text
D3D12::RTV::BACKBUFFER_0
...
D3D12::RTV::BACKBUFFER_LAST
```

Do not count the `IMGUI` or `BLANK` resources as swapchain backbuffers.

For each non-null backbuffer `ComPtr`, log the raw resource pointer.

This proves whether REFramework itself holds a strong reference to the swapchain buffer.

### No synthetic COM refcount probing in P3.3A

Do **not** repeatedly call `AddRef()/Release()` merely to estimate the COM reference count.

Reasons:

```text
COM return counts are not a portable ownership oracle
balanced AddRef/Release still perturbs the critical timing path
we only need to prove REF's own strong ownership first
```

If later evidence requires total COM refcount diagnostics, that should be a separate targeted step.

---

## 12. Required Change E — Log Backbuffer Release in `deinit_d3d12()`

The strongest evidence should come from the actual owner of `m_d3d12.rts`.

Add bounded structured logs around D3D12 deinitialization when a XeFG resize diagnostic event is active or when the active swapchain source is `XeFGInternal`.

Before releasing/resetting the D3D12 state, log:

```text
[XeFG][RendererState]
stage = deinit_before_release
event_id = last resize event
backbuffer_ref_count = N
backbuffer pointers = ...
```

After the code has cleared the D3D12 state (`m_d3d12 = {}`), log:

```text
stage = deinit_after_release
backbuffer_ref_count = 0
```

If the existing deinit path has intermediate resource releases before `m_d3d12 = {}`, do not reorder them for diagnostics.

Do not change ImGui shutdown ordering, command-context reset ordering, descriptor-heap lifetime, or graphics-memory lifetime.

The diagnostic must observe the existing behavior, not redesign it.

---

## 13. Required Change F — Log Backbuffer Reacquisition in `init_d3d12()`

This is the second critical part of the hypothesis test.

When `init_d3d12()` initializes the active XeFG internal renderer after a resize event, emit a structured sequence.

Before the swapchain `GetBuffer` loop:

```text
[XeFG][RendererAcquire]
stage = begin
event_id = last resize event
last_resize_kind
swapchain
binding_generation
buffer_count
current_backbuffer_index if cheaply available
```

For each successful backbuffer acquisition:

```text
stage = get_buffer_success
event_id
buffer_index
resource pointer
```

For a failure:

```text
stage = get_buffer_failed
event_id
buffer_index
HRESULT
```

After the backbuffer loop / renderer initialization completes:

```text
stage = complete
event_id
backbuffer_ref_count
backbuffer pointers
```

The existing `GetBuffer` calls must not be duplicated solely for logging. Log the pointers/results from the calls the renderer already performs.

### Evidence we need

The final runtime log must make this possible to determine:

```text
ResizeTarget event N
    -> post_reset backbuffer_ref_count = 0
    -> Present after event N
    -> RendererAcquire event N
    -> backbuffer_ref_count = 3 (example)
    -> later resize event N+1 or Opti E_PENDING
```

or show that this sequence does not occur.

---

## 14. Required Change G — Bounded Present-After-Resize Correlation

We need to know exactly when presentation resumes after each resize event, but we must not add per-frame logging indefinitely.

Add bounded diagnostic state in `D3D12Hook` so the first few recognized Present/Present1 calls after each top-level XeFG resize event are logged.

Recommended budget:

```text
first 3 Present/Present1 entries after each resize event
```

Maximum 5 is acceptable. Do not log every frame.

Example:

```text
[XeFG][ResizeLifecycle]
event_id = 17
kind = Present1
stage = present_after_resize
present_ordinal = 1
elapsed_ms_since_resize = 4
swapchain = ...
tracked_swapchain = ...
binding_generation = ...
thread_id = ...
```

The `Present`/`Present1` original call behavior must remain unchanged.

If convenient, take a renderer-state snapshot immediately before and after the first post-resize render callback only:

```text
present_pre_render_callback
present_post_render_callback
```

This is useful because the post-callback snapshot can prove that `on_frame_d3d12()` reacquired swapchain buffers.

Do not take full resource snapshots on every Present.

---

## 15. Required Change H — Preserve and Correlate P3.2 Binding Generation

Every structured resize log should include:

```text
binding_generation = m_xefg_binding_generation
```

Expected normal failing MHW session based on current evidence:

```text
initial binding generation = 1
BindingGate/Rebind          = 0
resize events               = generation 1
```

If generation unexpectedly changes during the Alt+Enter sequence, that is important evidence and must be visible.

Do not change generation semantics:

```text
initial successful XeFG binding -> 1
identical publication           -> unchanged
successful P3.2 replacement     -> +1
failed replacement              -> unchanged
unhook/full teardown            -> 0
```

---

## 16. Required Change I — Identity Logging Must Distinguish Raw Pointer and Hooked Instance

For each resize event, record at least:

```text
incoming swapchain pointer
m_swap_chain / tracked swapchain pointer
m_swapchain_hook->get_instance()
m_xefg_bound_swapchain.Get()
```

If practical without broad refactoring, also capture canonical COM identity for the incoming swapchain using a temporary `IUnknown` `ComPtr`.

Example field:

```text
swapchain_identity = 0x...
```

This QueryInterface is acceptable because resize events are rare and the temporary strong reference is balanced automatically.

Do not use RTTI/module owner as a functional routing decision.

Module/vtable ownership remains diagnostic only.

---

## 17. Required Change J — Original Method Owner and Return Value

For `ResizeTarget`, `ResizeBuffers`, and `ResizeBuffers1`, log the original function pointer and its module owner using the existing diagnostic helper where possible.

Example:

```text
original_fn = 0x...
original_owner = C:\...\dxgi.dll | libxess... | other module
```

This is diagnostic only.

Do not change behavior based on the module name.

Always log the original HRESULT in structured form:

```text
result = 0x00000000
result = 0x8000000A
...
```

The caller must receive the exact original HRESULT as before.

---

## 18. Locking and Concurrency Requirements

Do not introduce a new active-lifecycle mutex.

The existing relevant entry points already use:

```cpp
g_framework->get_hook_monitor_mutex()
```

P3.3A diagnostic state associated with the active XeFG binding should be read/written while this lifecycle mutex is held where possible.

`REFramework` renderer snapshots may also take the existing `m_imgui_mtx` if required by current ownership rules. Avoid lock-order inversion.

### Lock-order rule

Inspect the current code before adding snapshot calls.

If a resize hook holds `hook_monitor_mutex` and then invokes a callback that takes `m_imgui_mtx`, preserve that established order. Do not add a path that takes `m_imgui_mtx` first and then blocks on `hook_monitor_mutex` if the reverse order already exists elsewhere.

If a public resource-snapshot helper needs `m_imgui_mtx`, prefer calling it from the same established lifecycle context and keep it read-only/short.

Do not perform expensive stack walking or module enumeration while holding extra locks beyond what the existing resize hooks already do.

The existing resize stacktrace logging may remain.

---

## 19. Logging Volume Requirements

P3.3A is diagnostic, but it must remain bounded.

Allowed high-detail events:

```text
ResizeTarget top-level entry/return
ResizeBuffers top-level entry/return
ResizeBuffers1 top-level entry/return
pre/post reset snapshots
D3D12 deinit before/after release
D3D12 init/GetBuffer reacquisition
first 3-5 Presents after each resize event
hook-monitor existing events
```

Do not add:

```text
per-frame full COM snapshots
per-frame stacktraces
per-frame descriptor dumps
per-frame AddRef/Release probes
unbounded vtable dumps
continuous polling
```

The final MHW log should be readable enough to reconstruct a sub-second resize transition without generating tens of thousands of new diagnostic lines per second.

---

## 20. Explicit Non-Behavioral Requirements

P3.3A must not change any of these semantics:

```text
Present args
Present1 args
ResizeTarget args
ResizeBuffers args
ResizeBuffers1 args
original method selection
original HRESULT propagation
P2.2 pre-ResizeBuffers1 reset
P3.1 strong COM ownership
P3.2 binding generation
P3.2 same-object rebind behavior
P3.2 different-object atomic rebind behavior
presentation queue authority
observe-only behavior
hook-monitor thresholds
hook-monitor recovery sequence
ImGui/mod reset callback ordering
```

If implementation requires changing behavior to obtain the diagnostic, stop and explain why rather than silently broadening the PR.

---

## 21. Files Expected to Change

Expected primary files:

```text
src/D3D12Hook.cpp
src/D3D12Hook.hpp
src/REFramework.cpp
src/REFramework.hpp
```

A smaller file set is fine if the same evidence can be captured cleanly.

Do not edit unrelated game/mod files.

Do not edit OptiScaler.

### Size target

This should remain a focused diagnostic PR.

Target:

```text
~150-250 functional LOC
```

Prefer below 250 functional LOC.

If the cleanest implementation reaches roughly 250-350 LOC because of structured resource logging, keep it focused rather than compressing it into unreadable code.

Avoid exceeding ~350 functional LOC. If the work naturally grows beyond that, stop and split the diagnostic.

---

## 22. Suggested Implementation Helpers

These are suggestions, not mandatory names.

### In `D3D12Hook`

```cpp
uint64_t begin_xefg_resize_diagnostic(
    XefgResizeEventKind kind,
    IDXGISwapChain3* swapchain);

void log_xefg_resize_event(
    uint64_t event_id,
    XefgResizeEventKind kind,
    std::string_view stage,
    IDXGISwapChain3* swapchain,
    HRESULT result = S_OK,
    bool has_result = false) const;
```

A small helper to return diagnostic context is also acceptable:

```cpp
uint64_t get_xefg_last_resize_event_id() const;
XefgResizeEventKind get_xefg_last_resize_event_kind() const;
```

Do not expose mutable active binding state unnecessarily.

### In `REFramework`

```cpp
void log_d3d12_resize_snapshot(
    std::string_view stage,
    uint64_t event_id) const;
```

Potential helper to count only swapchain backbuffer slots:

```cpp
size_t count_d3d12_backbuffer_refs() const;
```

Keep helpers diagnostic and local to the feature.

---

## 23. Expected Healthy Diagnostic Sequence

A healthy transition might look like:

```text
[XeFG][ResizeLifecycle] event_id=10 kind=ResizeTarget stage=enter ... generation=1
[XeFG][RendererState] event_id=10 stage=resize_target_pre_reset backbuffer_ref_count=3 ...
[XeFG][ResizeLifecycle] event_id=10 kind=ResizeTarget stage=pre_reset
[XeFG][RendererState] event_id=10 stage=deinit_before_release backbuffer_ref_count=3 ...
[XeFG][RendererState] event_id=10 stage=deinit_after_release backbuffer_ref_count=0
[XeFG][RendererState] event_id=10 stage=resize_target_post_reset backbuffer_ref_count=0
[XeFG][ResizeLifecycle] event_id=10 kind=ResizeTarget stage=original_return result=0x00000000

[XeFG][ResizeLifecycle] event_id=10 kind=Present1 stage=present_after_resize present_ordinal=1 ...
[XeFG][RendererAcquire] event_id=10 stage=begin buffer_count=3 ...
[XeFG][RendererAcquire] event_id=10 stage=get_buffer_success buffer_index=0 resource=0x...
[XeFG][RendererAcquire] event_id=10 stage=get_buffer_success buffer_index=1 resource=0x...
[XeFG][RendererAcquire] event_id=10 stage=get_buffer_success buffer_index=2 resource=0x...
[XeFG][RendererAcquire] event_id=10 stage=complete backbuffer_ref_count=3 ...
```

Then one of the following must become visible.

### Case A — REF sees a later `ResizeBuffers`

```text
[XeFG][ResizeLifecycle] event_id=11 kind=ResizeBuffers stage=enter ...
[XeFG][RendererState] event_id=11 stage=resize_buffers_pre_reset backbuffer_ref_count=3
[XeFG][RendererState] event_id=11 stage=deinit_after_release backbuffer_ref_count=0
[XeFG][ResizeLifecycle] event_id=11 kind=ResizeBuffers stage=original_return result=0x8000000A
```

Interpretation:

```text
REF did release its own tracked backbuffers before the failing original ResizeBuffers.
REF ownership alone does not explain E_PENDING.
Investigate another owner / wrapper / call constraint next.
```

### Case B — Opti logs E_PENDING but REF has no corresponding `ResizeBuffers` event

Interpretation:

```text
The failing ResizeBuffers is not passing through the active REF internal swapchain ResizeBuffers[13] hook.
The next diagnostic should identify the outer/wrapper/proxy resize object.
Do not implement generic REF reset coalescing yet.
```

### Case C — REF reacquisition happens after ResizeTarget and no later REF reset occurs before E_PENDING

Interpretation:

```text
REF is a plausible contributor to outstanding backbuffer ownership at the failing outer resize boundary.
A later functional P3.3 change may need to suppress/defer renderer reacquisition across this specific fullscreen transition.
Do not implement that suppression in P3.3A.
```

### Case D — no REF backbuffer reacquisition occurs before E_PENDING

Interpretation:

```text
The primary REF reacquisition hypothesis is falsified.
Prioritize Opti/Streamline/game ownership or swapchain identity investigation.
```

### Case E — binding generation / object identity changes unexpectedly

Interpretation:

```text
The failure may involve a lifecycle boundary not currently published through P3.2 candidate discovery.
Use the exact identity evidence to design the next targeted lifecycle diagnostic.
```

---

## 24. Hook-Monitor Interpretation Rule

Do not treat this pattern as the root cause:

```text
Last chance
    -> rehook request
    -> Unhooking D3D12
    -> Hooking D3D12
```

For the current MHW failure, the evidence is:

```text
resize/presentation failure first
    -> recognized Present activity stops
    -> hook monitor becomes stale
    -> recovery loop begins
```

P3.3A should preserve current hook-monitor snapshots so the final timeline can show:

```text
last successful Present age
active swapchain/queue/device before first Last chance
state after first recovery
```

But no code in the hook monitor should change in this PR.

---

## 25. Static / Build Validation for Codex

Codex must perform normal project validation after implementing the diagnostics.

Required:

```text
cmake --preset vs2022
cmake --build build --config Release --target REFramework
python dev/audit_direct_access_clang.py
git diff --check
```

If an existing local build directory is damaged or inaccessible, an isolated clean configure/build directory is acceptable, but the PR description must state exactly what was run.

The Release artifact should be identified clearly, normally:

```text
build/bin/REFramework/dinput8.dll
```

or the equivalent path from the isolated build directory.

Codex must not claim MHW runtime success.

---

## 26. Hardware Validation — User Performed, Not Codex

The user performs the real Intel hardware test.

Use current P3.2-based diagnostic build with:

```text
Monster Hunter Wilds
OptiScaler 0.9.5-pre4 / same known configuration
XeFG output
interpolation 2x
Special K absent
REFramework = P3.3A diagnostic build
```

Recommended test sequence:

```text
1. Start MHW normally.
2. Confirm XeFG is active.
3. Confirm OptiScaler overlay works.
4. Confirm REFramework Insert overlay works.
5. Leave the game stable for ~15-30 seconds.
6. Trigger Alt+Enter once.
7. Do not spam repeated mode changes after the failure boundary.
8. If the game crashes, preserve the first crash artifacts immediately.
9. Preserve both REF and OptiScaler logs from the same exact session.
10. Preserve CrashReport.txt and minidump if generated.
11. Record whether the REF overlay was visible immediately before Alt+Enter and whether any frame was visibly presented after the transition.
```

Because the failure is already reliably reproduced, one clean transition is preferable to repeated transitions that make the log harder to interpret.

### Test artifacts

Collect together under one session folder:

```text
re2_framework_log.txt
OptiScaler.log
CrashReport.txt (if generated)
minidump (if generated)
brief user-observed note
```

Suggested folder name:

```text
P3.3A/mhw-altenter
```

When changing REFramework builds, keep test storage hygiene consistent so stale generated `storage` contents do not create unrelated A/B ambiguity.

---

## 27. Runtime Acceptance Criteria for P3.3A

P3.3A is accepted when the log can answer all of these questions without guessing:

```text
[ ] Which resize method entered first during Alt+Enter?
[ ] What was the active XeFG swapchain pointer and binding generation?
[ ] How many REF backbuffer ComPtrs were non-null before the first reset?
[ ] Did the reset/deinit path reduce REF backbuffer ownership to zero?
[ ] Did a Present/Present1 occur after ResizeTarget and before the failure boundary?
[ ] Did the REF renderer reacquire swapchain buffers during that intermediate Present?
[ ] Which exact resource pointers did REF reacquire?
[ ] Did REFramework receive a top-level ResizeBuffers[13] call near the Opti E_PENDING event?
[ ] If yes, did REF release its own backbuffers before calling the original ResizeBuffers?
[ ] What HRESULT did the original REF-hooked ResizeBuffers return?
[ ] Did ResizeBuffers1 remain S_OK?
[ ] Did swapchain identity remain the same across the transition?
[ ] Did binding generation remain 1 or unexpectedly change?
[ ] What was the last successful Present/Present1 before presentation stopped?
[ ] How long after that did hook-monitor Last chance occur?
```

The PR can be successful even if the game still crashes. The diagnostic objective is evidence quality.

---

## 28. P3.3A Failure Conditions

Do not merge the diagnostic PR if it:

```text
changes resize behavior
changes renderer reinitialization timing
changes hook-monitor behavior
changes P3.2 rebind behavior
causes new Present/resize HRESULTs
logs every Present indefinitely
cannot correlate resource snapshots to resize event IDs
cannot distinguish REF ResizeBuffers visibility vs absence
cannot prove whether REF backbuffer ComPtrs are cleared/reacquired
adds unbounded COM refcount probing
adds private Intel/Opti offsets
adds game-specific behavioral hacks
```

Compile warnings/errors or static-audit regressions are also blocking.

---

## 29. Expected PR Description

The PR description should state clearly:

```text
- This is P3.3A diagnostic-only instrumentation for the MHW Alt+Enter E_PENDING failure.
- The failure reproduces on both P3.1 and P3.2, so it is not classified as a P3.2 atomic-rebind regression.
- P3.2 BindingGate/Rebind was not exercised in the failing runs.
- The PR adds correlated resize-event IDs, active binding identity, REF backbuffer ownership snapshots, renderer reacquisition logs, and bounded post-resize Present logging.
- It does not suppress resets, delay renderer init, change resize arguments, alter HRESULTs, or modify hook-monitor policy.
- Release build / static audit / diff-check results are listed.
- No MHW runtime success is claimed by Codex.
```

Do not claim the root cause until the P3.3A runtime log proves it.

---

## 30. What Comes After P3.3A

Do not pre-implement P3.3B in this PR.

Choose the next step from the evidence.

### If REF reacquisition is proven to overlap a later resize that REF cannot intercept

Design a narrow functional transition rule to keep REF renderer/backbuffer resources released across that specific fullscreen resize boundary, then recreate on a proven safe completion signal.

Do not use an arbitrary sleep as the completion mechanism if a real lifecycle signal can be observed.

### If REF sees the failing ResizeBuffers and has already released all its backbuffers

Do not suppress renderer recreation blindly. Investigate the other outstanding owner and the wrapper/proxy path.

### If the failing ResizeBuffers occurs on another swapchain/wrapper

Add only the smallest additional diagnostic needed to identify that object's identity and relationship to the active internal swapchain. A later targeted hook may be justified, but do not globally hook every DXGI swapchain.

### If object/binding identity changes

Compare the transition to P3.2 candidate publication. Determine why no `BindingGate/Rebind` event was generated and whether lifecycle discovery needs another boundary.

### If MHW becomes stable without reproducing the boundary

Repeat only enough to confirm whether the diagnostic instrumentation changed timing materially. P3.3A should not be treated as a fix solely because one diagnostic run did not crash.

---

## 31. Non-Goals Reminder

P3.3A is not:

```text
a MHW game-specific hack
a hook-monitor fix
a P3.2 rollback
a P3.2 rebind acceptance test
a DD2 DLSS fix
a generic fullscreen framework
a Special K emulation layer
an OptiScaler patch
a Requiem stutter fix
```

It is one evidence-driven step in the existing P3 lifecycle plan.

---

## 32. Completion Checklist

### Code review

```text
[ ] current master used as base
[ ] no behavior changes
[ ] no P3.2 rebind changes
[ ] no hook-monitor changes
[ ] structured event ID for top-level XeFG resize calls
[ ] ResizeTarget enter/pre-reset/post-reset/original-return logs
[ ] ResizeBuffers enter/pre-reset/post-reset/original-return logs
[ ] ResizeBuffers1 correlation fields and original-return logs
[ ] active swapchain/hook instance/owned swapchain identities logged
[ ] binding generation logged
[ ] queue/device/thread IDs logged
[ ] original function pointer/owner logged
[ ] REF backbuffer ownership snapshot helper added
[ ] only BACKBUFFER_* slots counted as swapchain references
[ ] deinit before/after release snapshots
[ ] init/GetBuffer reacquisition logs
[ ] bounded first Present/Present1 after resize logs
[ ] no per-frame unbounded diagnostics
[ ] no AddRef/Release refcount polling
[ ] original HRESULTs propagated unchanged
```

### Static/build

```text
[ ] cmake configure succeeded
[ ] Release REFramework build succeeded
[ ] dev/audit_direct_access_clang.py passed
[ ] git diff --check passed
[ ] artifact path reported
```

### Runtime handoff

```text
[ ] Codex does not claim MHW success
[ ] user is instructed to perform one clean MHW Alt+Enter reproduction
[ ] REF + Opti logs are collected from the same run
[ ] crash artifacts are collected if present
[ ] final analysis distinguishes log-proven from user-observed facts
```

---

## 33. Definition of Done

P3.3A is done when:

```text
1. The implementation builds cleanly and passes static validation.
2. No functional graphics lifecycle behavior was intentionally changed.
3. One MHW Alt+Enter runtime run produces a correlated resize timeline.
4. The timeline proves or falsifies REF backbuffer reacquisition before E_PENDING.
5. The timeline proves whether the failing ResizeBuffers passes through REF's active internal ResizeBuffers[13] hook.
6. The next functional P3.3 design can be written from evidence rather than speculation.
```

The key principle is:

> Diagnose the actual ownership and object boundary first. Fix only the lifecycle edge that the runtime evidence proves is broken.
