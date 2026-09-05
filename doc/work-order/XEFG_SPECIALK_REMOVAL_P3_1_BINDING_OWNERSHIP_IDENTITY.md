# XeFG Special K Removal — P3.1 Binding Ownership and Identity Foundation

## 1. Purpose

P2 through P2.2 established a working REFramework render path for the target topology:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (this fork)
Special K    = absent
FG Output    = Intel XeFG
Renderer     = D3D12
```

The P2.2 Intel + Dragon's Dogma 2 run now launches successfully, keeps XeFG alive, shows both OptiScaler and REFramework overlays, survives the observed `ResizeBuffers1` paths, and does not reproduce the earlier device-removal / invalid-call failures.

The next risk is no longer basic rendering. It is **binding lifetime and identity during XeFG re-initialization**.

Current P2 code deliberately rejects a different XeFG internal swapchain with:

```text
reason = p3_rebind_deferred
```

That defer was correct for P2, because the old active binding is represented mostly by raw COM pointers and the installed `VtableHook` does not own its target COM object.

P3.1 must establish the ownership and identity foundation required for a later safe rebind.

P3.1 must **NOT enable actual XeFG binding replacement yet**.

The required high-level behavior is:

```text
validated XeFG candidate
        ↓
construct a strongly-owned pending binding
        ↓
compare against the active binding identity
        ↓
identical binding
    -> no-op / keep active binding

changed binding
    -> classify exact change
    -> keep active binding unchanged
    -> drop pending strong references normally
    -> log explicit defer
```

At the same time, the currently active XeFG binding must strongly own the swapchain, selected queue, and device for at least as long as REFramework has a vtable hook installed on that swapchain.

This is a **code-change-only** work order.

Codex must:

```text
modify the code
build Release REFramework
run the required static validation
create/update the PR
produce the dinput8.dll artifact
stop
```

Codex must **not** run Dragon's Dogma 2 or Monster Hunter Wilds, must not claim runtime success, and must not implement P3.2 rebind behavior.

The user performs the real-hardware test. ChatGPT analyzes the resulting logs before the P3.2 work order is written.

---

## 2. Repository / Required Base

Repository:

```text
onehoon/REFramework
```

Required base at the time this work order was written:

```text
master
93cb0f18572e66a47643c9bf98b07bdae1f51d89
docs: add XeFG P3 lifecycle robustness plan
```

The functional P2.2 base immediately below it is:

```text
517cf67533babedbd89131b65da5e41241589437
feat: handle XeFG ResizeBuffers1 resets (#10)
```

If `master` advances, rebase onto current `master` and preserve all merged P2/P2.1/P2.2 behavior.

Read before editing:

```text
doc/REFramework_XeFG_P3_LIFECYCLE_ROBUSTNESS_PLAN_2026-09-06.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P2_2_RESIZEBUFFERS1_PRE_RESET.md
src/D3D12Hook.cpp
src/D3D12Hook.hpp
src/REFramework.cpp              // read for callback/lifecycle context; changes should not be required
```

Primary files expected to change:

```text
src/D3D12Hook.cpp
src/D3D12Hook.hpp
```

Prefer keeping the functional change below approximately 220 LOC.

Do not perform unrelated cleanup or generic D3D12 refactoring.

---

## 3. Why P3.1 Exists

### 3.1 Current P2 defer only detects a changed swapchain pointer

Current `publish_xefg_candidate()` contains this intentional P2 guard:

```cpp
if (hook->is_hooked()
    && hook->get_swap_chain() != nullptr
    && hook->get_swapchain_source() == SwapchainSource::XeFGInternal
    && hook->get_swap_chain() != pending.swapchain) {
    // Full XeFG swapchain recreation/rebind is P3 work.
    spdlog::warn(
        "[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = p3_rebind_deferred",
        reinterpret_cast<uintptr_t>(pending.swapchain));
    return;
}
```

This catches only:

```text
active swapchain != new swapchain
```

It does **not** catch:

```text
same swapchain + changed selected presentation queue
same swapchain + same queue + changed observe/render mode
same swapchain + changed queue + changed mode
```

A same-swapchain / changed-queue XeFG re-init can currently fall through to `bind_external_swapchain()` without the explicit renderer-reset/rebind transaction that P3 requires.

P3.1 must close this hole by treating the complete binding identity as:

```text
swapchain
selected command queue
source
observe/render mode
```

### 3.2 Current `bind_external_swapchain()` idempotence ignores observe/render mode

Current shape:

```cpp
if (m_swapchain_source == source
    && m_swap_chain == swapchain
    && m_command_queue == command_queue
    && m_swapchain_hook != nullptr
    && m_hooked) {
    return true;
}
```

For `XeFGInternal`, this is incomplete because it ignores:

```cpp
m_xefg_p21_observe_only
```

Therefore a changed diagnostic/render mode can be mistaken for an already-identical binding.

P3.1 must make the no-op comparison complete.

### 3.3 The active XeFG binding is raw-pointer based

Current members include:

```cpp
ID3D12Device4* m_device{ nullptr };
IDXGISwapChain3* m_swap_chain{ nullptr };
ID3D12CommandQueue* m_command_queue{ nullptr };
```

Those raw aliases are heavily used by existing REFramework code and do not need to be globally redesigned in P3.1.

However, the XeFG path needs explicit strong ownership while an instance hook is installed.

### 3.4 `VtableHook` does not own the target COM object

The repository currently pins `cursey/kananlib` at:

```text
8c27b656734355db0f2893581fd62e838fa130ad
```

Its `VtableHook` stores only the target address. It does not call COM `AddRef`.

Its destructor calls `remove()`, and `remove()` may dereference the original target to restore its old vtable:

```cpp
VtableHook::~VtableHook() {
    remove();
}

bool VtableHook::remove() {
    if (m_vtable_ptr != nullptr
        && IsBadReadPtr(m_vtable_ptr.ptr(), sizeof(void*)) == FALSE
        && m_vtable_ptr.to<void*>() == m_new_vtable) {
        *m_vtable_ptr.as<Address*>() = m_old_vtable;
        return true;
    }

    return false;
}
```

This establishes a hard P3 lifetime invariant:

```text
old XeFG swapchain strong reference MUST remain alive
        ↓
old VtableHook is removed/restored
        ↓
ONLY THEN may the old swapchain strong reference be released
```

Do not rely on XeFG or OptiScaler to keep the old object alive long enough for REFramework's hook destructor.

---

## 4. P3.1 Scope

### Explicitly in scope

```text
strong ownership for pending XeFG binding swapchain/selected queue
strong ownership for active XeFG swapchain/selected queue/device
safe VtableHook-before-COM-release ordering
complete XeFG binding identity comparison
complete bind idempotence including observe/render mode
explicit defer of ANY changed active XeFG binding
machine-readable BindingGate logging
preserving current P2.2 render/resize behavior
```

### Explicitly out of scope

```text
actually replacing an active XeFG swapchain
actually changing an active XeFG queue after a changed binding is detected
actual same-object queue/mode rebind
binding generation commit state machine
new renderer-reset/rebind transaction
ResizeBuffers redesign
ResizeBuffers1 redesign
ResizeTarget redesign
fullscreen lifecycle redesign
Alt+Tab/minimize handling
hook-monitor timeout/policy changes
XeFG DLL unload/reload handling
public XeFG proxy rendering
native/no-FG acceptance work
FSRFG
Special K compatibility
OptiScaler source changes
DD2 runtime testing by Codex
MHW runtime testing by Codex
```

If a changed binding is observed in P3.1, the required action is still:

```text
DEFER
```

P3.2 will enable replacement after P3.1 is reviewed and validated.

---

## 5. Preserve the Proven P2.2 Path

Do not regress these behaviors.

### Queue selection

For the confirmed Intel relation:

```text
relation = distinct_same_device
```

continue selecting:

```text
presentation_queue
```

with:

```text
render_callbacks = true
```

Do not revert to the outer `InitFromSwapChainDesc` queue.

### Active XeFG instance hooks

Continue installing on the validated internal presentation swapchain:

```text
Present[8]
ResizeBuffers[13]
ResizeTarget[14]
Present1[22]
ResizeBuffers1[39]
```

### ResizeBuffers1

Keep P2.2 ordering unchanged:

```text
ResizeBuffers1 entry
    -> on_reset()
    -> release REFramework D3D12/backbuffer references
    -> original ResizeBuffers1 with all arguments unchanged
    -> next Present/Present1 reinitializes renderer
```

### Present lifecycle

Do not change `present_common()` behavior in this PR except where compilation mechanically requires it.

### Existing diagnostics

Preserve:

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

---

## 6. Required Change A — Strongly Own the Pending Binding

Current `PendingXefgBinding` uses raw pointers:

```cpp
struct PendingXefgBinding {
    IDXGISwapChain3* swapchain{};
    ID3D12CommandQueue* selected_queue{};
    HWND hwnd{};
    XefgQueueRelation relation{ XefgQueueRelation::InitQueueUnavailable };
    bool observe_only{ true };

    bool valid() const {
        return swapchain != nullptr && selected_queue != nullptr;
    }
};
```

Change the two COM members to `Microsoft::WRL::ComPtr`.

Recommended shape:

```cpp
struct PendingXefgBinding {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> selected_queue{};
    HWND hwnd{};
    XefgQueueRelation relation{ XefgQueueRelation::InitQueueUnavailable };
    bool observe_only{ true };

    bool valid() const {
        return swapchain != nullptr && selected_queue != nullptr;
    }
};
```

The reason is not style. A pending binding can cross the XeFG-state lock / framework lifecycle handoff before `D3D12Hook::consume_pending_xefg_binding()` consumes it.

The pending object should keep the exact validated candidate and selected queue alive during that handoff.

### Publishing a render binding

Current code conceptually does:

```cpp
pending = {
    candidate.Get(),
    presentation_queue.queue,
    transaction.hwnd,
    relation,
    false,
};
```

With `ComPtr` members, use an explicit assignment shape so ownership is obvious:

```cpp
pending.swapchain = candidate;
pending.selected_queue = presentation_queue.queue; // ComPtr assignment AddRefs the interface
pending.hwnd = transaction.hwnd;
pending.relation = relation;
pending.observe_only = false;
```

For the observe-only path:

```cpp
pending.swapchain = candidate;
pending.selected_queue = init_queue.queue;
pending.hwnd = transaction.hwnd;
pending.relation = relation;
pending.observe_only = true;
```

Do not call `AddRef()` or `Release()` manually.

### Logging pending pointers

Any pointer logging must use `.Get()`:

```cpp
reinterpret_cast<uintptr_t>(pending.swapchain.Get())
reinterpret_cast<uintptr_t>(pending.selected_queue.Get())
```

### Consuming a pending binding

Use:

```cpp
return pending.has_value()
    && hook.bind_external_swapchain(
        pending->swapchain.Get(),
        pending->selected_queue.Get(),
        SwapchainSource::XeFGInternal,
        pending->observe_only);
```

Do not detach the `ComPtr` to transfer ownership. `bind_external_swapchain()` will establish its own active strong ownership for the XeFG binding.

---

## 7. Required Change B — Strongly Own the Active XeFG Binding

Do not convert all existing generic D3D12 fields to `ComPtr` in this PR. That would expand the review surface unnecessarily.

Instead add XeFG-specific strong ownership next to the existing raw aliases.

In `D3D12Hook.hpp`, add:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> m_xefg_bound_swapchain{};
Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_xefg_bound_queue{};
Microsoft::WRL::ComPtr<ID3D12Device4> m_xefg_bound_device{};
```

`D3D12Hook.hpp` may need:

```cpp
#include <wrl/client.h>
```

Keep the existing fields for existing call sites:

```cpp
ID3D12Device4* m_device{ nullptr };
IDXGISwapChain3* m_swap_chain{ nullptr };
ID3D12CommandQueue* m_command_queue{ nullptr };
```

For an active `XeFGInternal` binding, these raw fields become non-owning aliases of the strong fields:

```text
m_swap_chain    == m_xefg_bound_swapchain.Get()
m_command_queue == m_xefg_bound_queue.Get()
m_device        == m_xefg_bound_device.Get()
```

This minimizes changes to the working renderer while providing the required COM lifetime.

### Recommended preparation in `bind_external_swapchain()`

After validating the incoming swapchain/device but **before destroying any old hook**, prepare the next strong references locally:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> next_xefg_swapchain;
Microsoft::WRL::ComPtr<ID3D12CommandQueue> next_xefg_queue;
Microsoft::WRL::ComPtr<ID3D12Device4> next_xefg_device;

if (source == SwapchainSource::XeFGInternal) {
    next_xefg_swapchain = swapchain;
    next_xefg_queue = command_queue;
    next_xefg_device = device;
}
```

Do not overwrite active strong ownership yet.

The old active ownership must remain alive through old-hook removal.

---

## 8. Required Change C — Enforce Hook Removal Before XeFG COM Release

This ordering is mandatory.

### Wrong ordering — do not do this

```cpp
m_xefg_bound_swapchain.Reset(); // may destroy the COM object
m_xefg_bound_queue.Reset();
m_xefg_bound_device.Reset();

m_swapchain_hook.reset(); // destructor may now touch stale target memory
```

### Required ordering

```cpp
// Old XeFG strong references still exist here.
m_present_hook.reset();
m_swapchain_hook.reset(); // VtableHook::remove() runs while old object is still alive.

// Only after the old instance hook is removed:
m_xefg_bound_swapchain.Reset();
m_xefg_bound_queue.Reset();
m_xefg_bound_device.Reset();
```

When committing a new initial XeFG binding after that point:

```cpp
m_xefg_bound_swapchain = std::move(next_xefg_swapchain);
m_xefg_bound_queue = std::move(next_xefg_queue);
m_xefg_bound_device = std::move(next_xefg_device);

m_swap_chain = m_xefg_bound_swapchain.Get();
m_command_queue = m_xefg_bound_queue.Get();
m_device = m_xefg_bound_device.Get();
```

For non-XeFG sources, preserve the current native behavior. Do not invent native strong-ownership semantics as part of P3.1.

### `unhook()` ordering

Current `D3D12Hook::unhook()` already invalidates `g_d3d12_hook` under `hook_monitor_mutex` and removes its hook objects.

Extend the XeFG cleanup so the order is:

```text
1. invalidate global active-hook pointer as current code already does
2. remove/reset PointerHook if present
3. remove/reset VtableHook while XeFG swapchain is still strongly owned
4. clear raw aliases that point at the owned XeFG objects
5. release the XeFG strong ComPtrs
6. mark hook state unhooked as existing semantics require
```

A safe explicit shape is:

```cpp
const auto* owned_swapchain = m_xefg_bound_swapchain.Get();
const auto* owned_queue = m_xefg_bound_queue.Get();
const auto* owned_device = m_xefg_bound_device.Get();

m_present_hook.reset();
m_swapchain_hook.reset();

if (m_swap_chain == owned_swapchain) {
    m_swap_chain = nullptr;
}
if (m_command_queue == owned_queue) {
    m_command_queue = nullptr;
}
if (m_device == owned_device) {
    m_device = nullptr;
}

m_xefg_bound_swapchain.Reset();
m_xefg_bound_queue.Reset();
m_xefg_bound_device.Reset();
```

Adapt this to the actual surrounding function instead of mechanically pasting it.

Do not clear unrelated native pointers if the active binding is not XeFG-owned.

Do not manually call COM `Release()`.

---

## 9. Required Change D — Complete the Binding Identity

Add a small helper or equivalent logic so `bind_external_swapchain()` can decide whether the requested binding is truly identical.

Recommended helper in `D3D12Hook`:

```cpp
bool external_binding_matches(
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* command_queue,
    SwapchainSource source,
    bool xefg_observe_only) const;
```

Recommended implementation semantics:

```cpp
bool D3D12Hook::external_binding_matches(
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* command_queue,
    SwapchainSource source,
    bool xefg_observe_only) const {

    const bool normalized_observe_only =
        source == SwapchainSource::XeFGInternal && xefg_observe_only;

    if (!m_hooked
        || m_swapchain_hook == nullptr
        || m_swapchain_source != source
        || m_swap_chain != swapchain
        || m_command_queue != command_queue) {
        return false;
    }

    if (source == SwapchainSource::XeFGInternal
        && m_xefg_p21_observe_only != normalized_observe_only) {
        return false;
    }

    return true;
}
```

Then replace the current incomplete early return with:

```cpp
if (external_binding_matches(
        swapchain,
        command_queue,
        source,
        xefg_p21_observe_only)) {
    return true;
}
```

The helper name is not mandatory. The semantics are.

Do not broaden this into a generic COM-identity abstraction in P3.1.

For this binding gate, conservative raw interface-pointer comparison is acceptable and intentional because the exact interface pointer is the object address currently targeted by `VtableHook`.

---

## 10. Required Change E — Replace `p3_rebind_deferred` With a Complete Binding Gate

P3.1 must prevent **all** changed active XeFG bindings from falling through to `bind_external_swapchain()`.

Do this inside `publish_xefg_candidate()` while already holding:

```cpp
g_framework->get_hook_monitor_mutex()
```

Do not add another active-binding mutex.

### 10.1 Add an observe-mode getter

Add a small getter to `D3D12Hook.hpp` so `publish_xefg_candidate()` can compare the active mode:

```cpp
bool is_xefg_observe_only() const {
    return m_xefg_p21_observe_only;
}
```

Alternative naming is acceptable if equally clear.

### 10.2 Compare all identity fields

When there is an active hooked `XeFGInternal` binding:

```cpp
const bool swapchain_changed =
    hook->get_swap_chain() != pending.swapchain.Get();

const bool queue_changed =
    hook->get_command_queue() != pending.selected_queue.Get();

const bool mode_changed =
    hook->is_xefg_observe_only() != pending.observe_only;
```

If all three are false:

```text
action = unchanged
reason = identical
```

The active binding remains untouched.

If any field differs:

```text
action = defer
```

and P3.1 returns without rebinding.

### 10.3 Classify the exact change

Use these machine-readable reasons:

```text
swapchain_changed
queue_changed
mode_changed
multiple_fields_changed
```

Recommended helper:

```cpp
const char* binding_change_reason(
    bool swapchain_changed,
    bool queue_changed,
    bool mode_changed) {

    const auto count =
        static_cast<int>(swapchain_changed)
        + static_cast<int>(queue_changed)
        + static_cast<int>(mode_changed);

    if (count > 1) {
        return "multiple_fields_changed";
    }
    if (swapchain_changed) {
        return "swapchain_changed";
    }
    if (queue_changed) {
        return "queue_changed";
    }
    if (mode_changed) {
        return "mode_changed";
    }
    return "identical";
}
```

Do not add a complex class hierarchy or state machine for this.

### 10.4 Required log

Add one bounded state-change log at publish/binding-gate time:

```text
[XeFG][BindingGate] action = unchanged|defer, reason = ..., old_swapchain = 0x..., new_swapchain = 0x..., old_queue = 0x..., new_queue = 0x..., old_observe_only = true|false, new_observe_only = true|false
```

Suggested code:

```cpp
spdlog::info(
    "[XeFG][BindingGate] action = {}, reason = {}, "
    "old_swapchain = 0x{:x}, new_swapchain = 0x{:x}, "
    "old_queue = 0x{:x}, new_queue = 0x{:x}, "
    "old_observe_only = {}, new_observe_only = {}",
    changed ? "defer" : "unchanged",
    reason,
    reinterpret_cast<uintptr_t>(hook->get_swap_chain()),
    reinterpret_cast<uintptr_t>(pending.swapchain.Get()),
    reinterpret_cast<uintptr_t>(hook->get_command_queue()),
    reinterpret_cast<uintptr_t>(pending.selected_queue.Get()),
    hook->is_xefg_observe_only(),
    pending.observe_only);
```

If `changed == true`:

```cpp
return;
```

Do **not** call:

```cpp
g_framework->on_reset();
hook->bind_external_swapchain(...);
hook->unhook();
```

for the changed binding in P3.1.

P3.2 owns that behavior.

### 10.5 Identical case

For an identical active binding, either:

```text
log unchanged and return
```

or allow the existing complete idempotence check to consume it as a no-op.

Prefer the simpler explicit return after logging because it avoids unnecessary publish/bind work and makes runtime evidence unambiguous.

---

## 11. Recommended `publish_xefg_candidate()` Gate Shape

This is an illustrative integration shape, not a requirement to copy formatting exactly:

```cpp
if (g_framework != nullptr) {
    std::unique_lock<std::recursive_mutex> framework_lock{
        g_framework->get_hook_monitor_mutex()
    };

    if (auto* hook = g_d3d12_hook; hook != nullptr) {
        const bool has_active_xefg =
            hook->is_hooked()
            && hook->get_swap_chain() != nullptr
            && hook->get_swapchain_source() == SwapchainSource::XeFGInternal;

        if (has_active_xefg) {
            const bool swapchain_changed =
                hook->get_swap_chain() != pending.swapchain.Get();
            const bool queue_changed =
                hook->get_command_queue() != pending.selected_queue.Get();
            const bool mode_changed =
                hook->is_xefg_observe_only() != pending.observe_only;

            const bool changed =
                swapchain_changed || queue_changed || mode_changed;

            const auto reason = binding_change_reason(
                swapchain_changed,
                queue_changed,
                mode_changed);

            spdlog::info(
                "[XeFG][BindingGate] action = {}, reason = {}, "
                "old_swapchain = 0x{:x}, new_swapchain = 0x{:x}, "
                "old_queue = 0x{:x}, new_queue = 0x{:x}, "
                "old_observe_only = {}, new_observe_only = {}",
                changed ? "defer" : "unchanged",
                reason,
                reinterpret_cast<uintptr_t>(hook->get_swap_chain()),
                reinterpret_cast<uintptr_t>(pending.swapchain.Get()),
                reinterpret_cast<uintptr_t>(hook->get_command_queue()),
                reinterpret_cast<uintptr_t>(pending.selected_queue.Get()),
                hook->is_xefg_observe_only(),
                pending.observe_only);

            // P3.1 does not replace an active XeFG binding.
            return;
        }

        const auto replacing_active_non_xefg =
            hook->is_hooked()
            && hook->get_swap_chain() != nullptr
            && hook->get_swapchain_source() != SwapchainSource::XeFGInternal;

        if (replacing_active_non_xefg) {
            // Preserve existing P2 behavior.
            spdlog::info(
                "[XeFG][Bind] resetting active D3D12 renderer before XeFG bind");
            g_framework->on_reset();
        }

        if (!hook->bind_external_swapchain(
                pending.swapchain.Get(),
                pending.selected_queue.Get(),
                SwapchainSource::XeFGInternal,
                pending.observe_only)) {
            spdlog::warn(
                "[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = external_bind_failed",
                reinterpret_cast<uintptr_t>(pending.swapchain.Get()));
        }

        return;
    }

    std::scoped_lock state_lock{g_xefg_state_mutex};
    g_pending_xefg_binding = pending; // ComPtr copy keeps candidate/queue alive.
    return;
}
```

Important:

- The existing lock ordering must be preserved.
- Do not acquire `g_xefg_state_mutex` and `hook_monitor_mutex` in a new reverse order that introduces a deadlock.
- The active-binding comparison occurs under `hook_monitor_mutex` because hook-monitor recovery can destroy/replace `D3D12Hook` under that same mutex.

---

## 12. Recommended Initial XeFG Bind Ownership Shape

The following illustrates the required lifetime ordering inside `bind_external_swapchain()`.

Adapt it to current code rather than copying blindly:

```cpp
bool D3D12Hook::bind_external_swapchain(
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* command_queue,
    SwapchainSource source,
    bool xefg_p21_observe_only) {

    if (swapchain == nullptr || command_queue == nullptr) {
        return false;
    }

    if (external_binding_matches(
            swapchain,
            command_queue,
            source,
            xefg_p21_observe_only)) {
        return true;
    }

    Microsoft::WRL::ComPtr<ID3D12Device4> device;
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device)))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain3> next_xefg_swapchain;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> next_xefg_queue;
    Microsoft::WRL::ComPtr<ID3D12Device4> next_xefg_device;

    if (source == SwapchainSource::XeFGInternal) {
        next_xefg_swapchain = swapchain;
        next_xefg_queue = command_queue;
        next_xefg_device = device;
    }

    // CRITICAL: old XeFG ownership remains alive while the old VtableHook is removed.
    m_present_hook.reset();
    m_swapchain_hook.reset();

    // Old hook is gone. It is now safe to release old XeFG ownership.
    m_xefg_bound_swapchain.Reset();
    m_xefg_bound_queue.Reset();
    m_xefg_bound_device.Reset();

    if (source == SwapchainSource::XeFGInternal) {
        m_xefg_bound_swapchain = std::move(next_xefg_swapchain);
        m_xefg_bound_queue = std::move(next_xefg_queue);
        m_xefg_bound_device = std::move(next_xefg_device);

        m_swap_chain = m_xefg_bound_swapchain.Get();
        m_command_queue = m_xefg_bound_queue.Get();
        m_device = m_xefg_bound_device.Get();
    } else {
        // Preserve existing native alias behavior.
        m_swap_chain = swapchain;
        m_command_queue = command_queue;
        m_device = device.Get();
    }

    m_swapchain_source = source;
    m_xefg_p21_observe_only =
        source == SwapchainSource::XeFGInternal && xefg_p21_observe_only;
    m_xefg_p21_render_boundary_logged = false;
    m_is_phase_1 = false;

    // Preserve current hook installation exactly, including XeFG slot 39.
    ...
}
```

P3.1 does not need to redesign hook-install failure semantics. P3.2 will own the atomic replacement transaction.

However, do not introduce a new half-committed state or set `m_hooked = true` earlier than current behavior.

---

## 13. Non-Goals / Things That Must Not Sneak Into This PR

Do not implement any of the following even if they seem nearby:

```text
removing `p3_rebind_deferred` by actually rebinding
binding generation counter used to commit replacements
renderer reset on changed XeFG binding
same-object queue update
same-object observe-mode update
fullscreen event state machine
Alt+Tab/minimize detection
longer hook-monitor timeout
new background/polling thread
XeFG public proxy as render target
XeFG module unload hook destruction
DRED
manual COM refcount diagnostics
manual AddRef/Release logging
changes to OptiScaler
changes to Special K behavior
```

P3.1 should make P3.2 safer and easier to review, not implement P3.2 early.

---

## 14. Required Diagnostics

### New log

At an active XeFG publish boundary, emit exactly one bounded binding-gate log:

```text
[XeFG][BindingGate] action = unchanged, reason = identical, old_swapchain = 0x..., new_swapchain = 0x..., old_queue = 0x..., new_queue = 0x..., old_observe_only = false, new_observe_only = false
```

or:

```text
[XeFG][BindingGate] action = defer, reason = swapchain_changed, ...
```

Allowed defer reasons:

```text
swapchain_changed
queue_changed
mode_changed
multiple_fields_changed
```

This is emitted only when a validated new candidate is compared against an already-active XeFG binding.

Do not emit the log every Present frame.

### Preserve existing logs

Do not remove or rename the existing P2/P2.1/P2.2 markers listed earlier.

---

## 15. Static Review Checklist Codex Must Perform

Before opening/updating the PR, inspect the final diff specifically for these invariants.

### COM ownership

```text
PendingXefgBinding swapchain is a ComPtr
PendingXefgBinding selected_queue is a ComPtr
active XeFG swapchain is strongly owned
active XeFG selected queue is strongly owned
active XeFG device is strongly owned
no manual Release() added
no manual AddRef() added
```

### Hook lifetime

```text
m_swapchain_hook.reset()/remove occurs while old XeFG swapchain strong ref still exists
old XeFG swapchain ComPtr is reset only after vtable-hook removal
unhook follows the same ordering
```

### Binding identity

```text
swapchain participates
selected queue participates
source participates
observe/render mode participates
identical binding is a no-op
```

### Defer gate

```text
swapchain-only change -> defer
queue-only change -> defer
mode-only change -> defer
multiple changes -> defer
identical -> unchanged/no-op
active binding is untouched on defer
```

### P2.2 preservation

```text
presentation queue selection unchanged
Present/Present1 hooks unchanged
ResizeBuffers1[39] hook unchanged
pre-ResizeBuffers1 on_reset unchanged
observe-only callback suppression unchanged
hook-monitor liveness path unchanged
```

---

## 16. Build / Static Validation

Codex must run:

```text
cmake --preset vs2022
cmake --build build --config Release --target REFramework
python dev/audit_direct_access_clang.py
git diff --check
```

Release artifact:

```text
build/bin/REFramework/dinput8.dll
```

If the repository's current CI/build process has advanced, use the current equivalent in addition to the commands above, but do not replace the required Release build with a Debug-only build.

---

## 17. P3.1 Acceptance Criteria

P3.1 is accepted by code/build/static evidence when all of the following are true:

```text
[ ] Pending XeFG binding strongly owns swapchain and selected queue.
[ ] Active XeFG binding strongly owns swapchain, selected queue, and device.
[ ] Existing generic raw getters continue to expose the same objects to the proven renderer path.
[ ] VtableHook removal occurs before release of the XeFG swapchain strong reference.
[ ] D3D12Hook::unhook() follows that ordering.
[ ] Binding idempotence includes swapchain, queue, source, and observe/render mode.
[ ] Active XeFG swapchain-only change is deferred.
[ ] Active XeFG queue-only change is deferred.
[ ] Active XeFG mode-only change is deferred.
[ ] Multiple-field changes are deferred.
[ ] Identical active XeFG binding is a no-op.
[ ] Deferred candidates do not reset or mutate the active renderer/binding.
[ ] P2.1 presentation-queue selection is preserved.
[ ] P2.2 ResizeBuffers1 behavior is preserved.
[ ] Required BindingGate logging is present and bounded.
[ ] Release build succeeds.
[ ] static audit succeeds.
[ ] git diff --check succeeds.
[ ] No DD2/MHW runtime-success claim is made by Codex.
```

Runtime stability is **not** a Codex acceptance criterion.

---

## 18. Required PR Report

The PR description/final Codex report should state only verifiable implementation/build facts:

```text
files changed
how PendingXefgBinding now owns COM interfaces
how active XeFG binding ownership is represented
VtableHook-before-COM-release ordering
binding identity fields compared
BindingGate defer reasons implemented
whether any behavior outside P3.1 changed (expected: no)
Release build result
static audit result
git diff --check result
artifact path
```

Explicitly state:

```text
No DD2 or MHW runtime validation was performed by Codex.
Changed active XeFG bindings remain intentionally deferred in P3.1.
Actual XeFG rebind behavior is reserved for P3.2 after P3.1 review/runtime evidence.
```

---

## 19. User Runtime Test After PR Review

Codex does not perform this section.

After the PR passes code review and a build artifact is available, the user may run the P3.1 build on Intel hardware.

The first runtime goal is only regression confirmation of the already-working P2.2 path:

```text
DD2 launches
XeFG active
OptiScaler overlay visible
REFramework overlay visible
ResizeBuffers1 still succeeds
Present1 continues
no DEVICE_REMOVED / ACCESS_DENIED / INVALID_CALL regression
no recurring hook-monitor recovery loop
```

If the game naturally produces a second XeFG init/candidate during the test, preserve the logs. `BindingGate` should tell us whether the new candidate was:

```text
identical
swapchain_changed
queue_changed
mode_changed
multiple_fields_changed
```

ChatGPT will analyze those logs.

Do not write or implement P3.2 based only on CI. P3.2 should be written after P3.1 is reviewed and the current runtime evidence is understood.
