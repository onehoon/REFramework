# Work Order: XeFG Refactor R6 — Active Binding Strong Ownership and Identity

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `d470bee7eb6f369bbef8983982902520e0c0572c` (`Refactor R5: extract XeFG pending candidate handoff (#23)`)

Relevant merged refactor baseline:

- R1 / PR #17: `65f9b3ee81971c3e2aac6df49518fa2dd588365d` — exact-HMODULE XeFG runtime registry extraction
- R2 / PR #18: `aa3a53e516b882a77d399c929efa0ef29d1426b0` — XeFG loader / probe handoff isolation
- PR #19: `3f83b8af0184f931daec44dc45257f7fc46966a4` — tracked `CMakeLists.txt` compatibility-source registration
- PR #20: `74042e1686f62a54a50540e1a113a3ae648778c1` — MHW-only XeFG ResizeTarget transition hold
- R3 / PR #21: `cfb6efe5102f330c9ddf6fdadb7e9af984ce0678` — InitDesc observation transaction + temporary factory capture extraction
- R4 / PR #22: `1eab453505fe3c75bb7ed45715ce7a1d2c6cc35f` — queue/device/HWND validation + strongly-owned binding candidate construction
- R5 / PR #23: `d470bee7eb6f369bbef8983982902520e0c0572c` — pending candidate lifecycle handoff extraction

Related documents:

- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_ARCHITECTURE_2026-09-06.md`
- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_PR_SPLIT_PLAN_2026-09-06.md`
- `doc/work-order/XEFG_REFACTOR_R1_RUNTIME_REGISTRY_EXTRACTION.md`
- `doc/work-order/XEFG_REFACTOR_R2_LOADER_PROBE_HANDOFF.md`
- `doc/work-order/XEFG_REFACTOR_R3_INIT_TRANSACTION_FACTORY_CAPTURE.md`
- `doc/work-order/XEFG_REFACTOR_R4_QUEUE_VALIDATION_CANDIDATE.md`
- `doc/work-order/XEFG_REFACTOR_R5_PENDING_CANDIDATE_HANDOFF.md`

This work order implements **R6 only** from the fine-grained XeFG refactor plan.

The fork is still **unreleased**. Therefore this PR does not need compatibility aliases for intermediate fork-only field names. Once all call sites are migrated in the same PR, delete the old R5-era XeFG ownership fields rather than preserving temporary duplicate state.

That freedom applies only to internal structure. Runtime behavior and original REFramework functionality remain the compatibility contract.

---

# 1. Recommended PR Identity

Suggested branch:

```text
refactor/xefg-r6-active-binding-state
```

Suggested PR title:

```text
Refactor R6: extract XeFG active binding ownership and identity
```

Suggested commit title:

```text
refactor: extract XeFG active binding state
```

This PR has one primary responsibility:

> Move the semantic state and strong COM ownership of the currently active XeFG binding out of scattered `D3D12Hook` fields into one dedicated `XeFGBinding` object, while preserving the existing physical hook install/rebind/unhook ordering exactly.

Do **not** begin the R7 physical bind/rebind protocol rewrite in this PR.

---

# 2. Release / Development Policy for R6

The project is not yet distributed to end users. The intended sequence remains:

```text
R6 active state extraction
R7 physical initial bind + transactional rebind isolation
R8 resize lifecycle state extraction
R9 Present/resize callback cleanup
R10 hook-monitor/upstream-surface cleanup
R11 final logging + persistent Debug Logging UI
release only after R11 and final runtime validation
```

Because the code is unreleased, R6 may prefer a clean internal cut over temporary compatibility scaffolding.

Allowed convenience:

- add `XeFGBinding.hpp/.cpp` now;
- remove the old fork-only ownership fields completely in the same PR;
- migrate all internal reads/writes to the new object in one pass;
- move the binding identity comparison helper out of `XeFGCandidateHandoff` if that produces a cleaner ownership boundary;
- use a narrow friend/internal bridge where necessary rather than creating public compatibility APIs that will immediately be removed.

Still forbidden:

- combining R6 with R7 merely because the fork is unreleased;
- changing physical DXGI hook slots;
- changing renderer reset order;
- changing resize hold policy;
- reducing diagnostic logging before the final logging PR;
- broad unrelated cleanup.

The unreleased status is permission to avoid transitional clutter, **not** permission to change working compatibility behavior casually.

---

# 3. Current State After R5

R1–R5 have now separated the front half of the XeFG compatibility path:

```text
XeFGRuntimeRegistry                 [R1]
    -> exact runtime/export ownership

XeFGCompatibility                  [R2]
    -> loader/probe handoff

XeFGDiscovery::observe_init()       [R3]
    -> raw internal swapchain + queue observation

XeFGDiscovery::build_binding_candidate() [R4]
    -> validated strong XeFGBindingCandidate

XeFGCandidateHandoff               [R5]
    -> live-hook delivery or single pending slot
```

However, once the candidate reaches a live `D3D12Hook`, the active XeFG identity is still stored as separate fields inside the generic hook object.

Current master contains:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> m_xefg_bound_swapchain{};
Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_xefg_bound_queue{};
Microsoft::WRL::ComPtr<ID3D12Device4> m_xefg_bound_device{};
uint64_t m_xefg_binding_generation{0};
bool m_xefg_p21_observe_only{false};
```

while generic/native renderer code continues to use raw aliases:

```cpp
IDXGISwapChain3* m_swap_chain{};
ID3D12CommandQueue* m_command_queue{};
ID3D12Device4* m_device{};
```

The active XeFG binding therefore currently has two representations:

```text
strong semantic ownership
    m_xefg_bound_swapchain
    m_xefg_bound_queue
    m_xefg_bound_device
    m_xefg_binding_generation
    m_xefg_p21_observe_only

legacy D3D12Hook aliases used by renderer/callback code
    m_swap_chain
    m_command_queue
    m_device
```

R6 keeps the second representation for upstream compatibility but consolidates the first representation into one object.

---

# 4. Why R6 Is Separate From R7

R6 answers:

> What is the currently active XeFG binding, who strongly owns its COM objects, what is its generation/mode, and does its semantic identity match another binding?

R7 answers:

> In what exact order should physical instance hooks and renderer state be replaced when that binding changes?

These are related but different failure domains.

R6 failure examples:

- strong ownership released too early;
- raw aliases drift from the owned objects;
- generation is not updated consistently;
- observe-only mode is stored in two places and diverges;
- unhook clears aliases/ownership in the wrong relation.

R7 failure examples:

- new `VtableHook` installed incompletely;
- old hook removed before candidate preparation succeeds;
- renderer reset occurs at wrong point;
- failed replacement destroys the old good binding.

Do not mix those failure domains in one PR.

---

# 5. Fundamental COM Lifetime Invariant

This invariant is merge-blocking:

> The COM object targeted by an active `VtableHook` must remain strongly owned until that `VtableHook` has been removed.

`kananlib::VtableHook` does not make the target COM object lifetime safe for us by itself.

For XeFG changed-object replacement, current working order is conceptually:

```text
old XeFG strong ownership exists
old VtableHook exists on old swapchain

prepare new candidate strong ownership locally
prepare complete new VtableHook
reset renderer
remove old VtableHook
ONLY THEN replace/release old active strong ownership
commit new hook + aliases + generation
```

R6 must preserve this order.

For unhook:

```text
active XeFG strong ownership exists
active VtableHook exists

reset/remove VtableHook
clear raw aliases that point into XeFG ownership
ONLY THEN clear XeFG strong ownership
```

If an R6 implementation calls `XeFGBinding::clear()` before `m_swapchain_hook.reset()`, it is a **blocking defect**.

---

# 6. Strict R6 Scope

## 6.1 Move into `XeFGBinding`

Move semantic ownership of:

- strong `ComPtr<IDXGISwapChain3>`;
- strong selected `ComPtr<ID3D12CommandQueue>`;
- strong `ComPtr<ID3D12Device4>`;
- binding generation;
- active/inactive semantic state;
- observe-only/render mode;
- binding identity comparison helpers;
- complete-owned-state predicate;
- raw object getters used to synchronize D3D12Hook aliases;
- clear/reset of semantic active state.

## 6.2 Keep in `D3D12Hook` in R6

Keep physical/generic hook state:

- `m_swap_chain` raw alias;
- `m_command_queue` raw alias;
- `m_device` raw alias;
- `m_swapchain_hook`;
- `m_present_hook`;
- `m_hooked`;
- `m_is_phase_1`;
- `m_swapchain_source`;
- native D3D12 swapchain discovery;
- physical hook callback functions;
- physical hook slot installation;
- renderer reset calls;
- the current initial-bind sequence;
- the current same-object update sequence;
- the current changed-object replacement sequence;
- resize lifecycle state;
- hook-monitor logic.

## 6.3 Keep outside `XeFGBinding`

Do not move into the semantic binding object:

- `std::unique_ptr<VtableHook>`;
- callback function addresses;
- DXGI slot numbers;
- renderer callbacks;
- `g_framework` access;
- lifecycle mutexes;
- pending candidate storage;
- discovery observation;
- runtime registry state;
- resize transition state.

`XeFGBinding` is semantic state + strong lifetime, not the physical hook implementation.

---

# 7. Recommended New Files

Add:

```text
src/compatibility/xefg/XeFGBinding.hpp
src/compatibility/xefg/XeFGBinding.cpp
```

Expected modifications:

```text
MODIFY src/D3D12Hook.hpp
MODIFY src/D3D12Hook.cpp
MODIFY src/compatibility/xefg/XeFGCandidateHandoff.cpp   # only if identity helper moves
MODIFY CMakeLists.txt                                    # register new files
ADD    src/compatibility/xefg/XeFGBinding.hpp
ADD    src/compatibility/xefg/XeFGBinding.cpp
```

Normally unchanged:

```text
src/REFramework.cpp
src/compatibility/xefg/XeFGCompatibility.*
src/compatibility/xefg/XeFGRuntimeRegistry.*
src/compatibility/xefg/XeFGDiscovery.*
cmake.toml
Present/resize behavior
```

`cmake.toml` already recursively describes `src/**`. Do not modify it for these files.

As established by PR #19, the checked-in `CMakeLists.txt` must explicitly register the two new files.

Preferred surgical addition:

```cmake
"src/compatibility/xefg/XeFGBinding.cpp"
"src/compatibility/xefg/XeFGBinding.hpp"
```

placed with the other XeFG compatibility sources.

Do not broadly regenerate `CMakeLists.txt`.

---

# 8. Recommended `XeFGBinding` Shape

A clean R6 target is:

```cpp
#pragma once

#include <cstdint>
#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_4.h>

class XeFGBinding {
public:
    struct IdentityChange {
        bool swapchain_changed{};
        bool queue_changed{};
        bool mode_changed{};

        bool changed() const noexcept {
            return swapchain_changed || queue_changed || mode_changed;
        }

        const char* reason() const noexcept;
    };

    bool complete() const noexcept;
    bool active() const noexcept;

    IDXGISwapChain3* swapchain() const noexcept;
    ID3D12CommandQueue* queue() const noexcept;
    ID3D12Device4* device() const noexcept;
    bool observe_only() const noexcept;
    uint64_t generation() const noexcept;

    bool matches(
        IDXGISwapChain3* swapchain,
        ID3D12CommandQueue* queue,
        bool observe_only) const noexcept;

    IdentityChange compare(
        IDXGISwapChain3* swapchain,
        ID3D12CommandQueue* queue,
        bool observe_only) const noexcept;

    bool aliases_match(
        IDXGISwapChain3* swapchain,
        ID3D12CommandQueue* queue,
        ID3D12Device4* device) const noexcept;

    // Semantic commits only. Physical hook sequencing remains in D3D12Hook.
    void commit_initial(
        Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain,
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue,
        Microsoft::WRL::ComPtr<ID3D12Device4> device,
        bool observe_only);

    void commit_same_swapchain_update(
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue,
        Microsoft::WRL::ComPtr<ID3D12Device4> device,
        bool observe_only);

    void commit_replacement(
        Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain,
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue,
        Microsoft::WRL::ComPtr<ID3D12Device4> device,
        bool observe_only);

    void clear() noexcept;

private:
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue{};
    Microsoft::WRL::ComPtr<ID3D12Device4> m_device{};
    uint64_t m_generation{};
    bool m_observe_only{};
};
```

Exact method names are flexible.

The important design is:

```text
XeFGBinding
    owns COM lifetime + semantic identity

D3D12Hook
    owns physical hook and renderer sequencing
```

---

# 9. Active and Complete Semantics

Use two distinct concepts.

Recommended:

```cpp
bool XeFGBinding::complete() const noexcept {
    return m_swapchain != nullptr
        && m_queue != nullptr
        && m_device != nullptr;
}

bool XeFGBinding::active() const noexcept {
    return complete() && m_generation != 0;
}
```

Do not add a second independent `bool m_active` unless necessary.

Generation already gives us the semantic inactive state:

```text
generation == 0
    -> inactive / no committed XeFG active binding

generation >= 1
    -> active semantic binding
```

Avoid redundant state that can diverge.

---

# 10. Identity Semantics

The current active binding identity is determined by:

```text
swapchain object
selected queue object
observe-only mode
```

Same swapchain with a changed queue is a real binding change.

Same swapchain + same queue with changed mode is also a real binding change.

Recommended:

```cpp
XeFGBinding::IdentityChange XeFGBinding::compare(
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* queue,
    bool observe_only) const noexcept {
    return {
        .swapchain_changed = m_swapchain.Get() != swapchain,
        .queue_changed = m_queue.Get() != queue,
        .mode_changed = m_observe_only != observe_only,
    };
}
```

Reason mapping should preserve current strings exactly:

```cpp
const char* XeFGBinding::IdentityChange::reason() const noexcept {
    const auto count = static_cast<int>(swapchain_changed)
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

Do not add HWND/relation/public proxy into active identity in R6.

Those values are useful discovery/candidate metadata but are not part of the current active-binding replacement gate.

Changing identity criteria belongs to a separate behavioral change, not this refactor.

---

# 11. D3D12Hook Member Migration

Replace:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> m_xefg_bound_swapchain{};
Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_xefg_bound_queue{};
Microsoft::WRL::ComPtr<ID3D12Device4> m_xefg_bound_device{};
uint64_t m_xefg_binding_generation{0};
bool m_xefg_p21_observe_only{false};
```

with:

```cpp
XeFGBinding m_xefg_binding{};
```

Keep:

```cpp
IDXGISwapChain3* m_swap_chain{};
ID3D12CommandQueue* m_command_queue{};
ID3D12Device4* m_device{};
SwapchainSource m_swapchain_source{SwapchainSource::Native};
bool m_xefg_p21_render_boundary_logged{false};
```

and all resize lifecycle fields.

Because the fork is unreleased, do not keep deprecated aliases such as:

```cpp
m_xefg_bound_swapchain
m_xefg_bound_queue
m_xefg_bound_device
m_xefg_binding_generation
m_xefg_p21_observe_only
```

once all current call sites compile against `m_xefg_binding`.

At PR completion, searching for those old names should return zero implementation references.

---

# 12. Keep Legacy Raw D3D12 Aliases Synchronized

Do **not** rewrite normal REFramework renderer code to directly consume `XeFGBinding` in R6.

While XeFG is active:

```text
m_swap_chain    == m_xefg_binding.swapchain()
m_command_queue == m_xefg_binding.queue()
m_device        == m_xefg_binding.device()
```

This protects the upstream-sensitive renderer surface.

Recommended narrow helper in `D3D12Hook`:

```cpp
void D3D12Hook::sync_xefg_binding_aliases() noexcept {
    m_swap_chain = m_xefg_binding.swapchain();
    m_command_queue = m_xefg_binding.queue();
    m_device = m_xefg_binding.device();
}
```

This helper should only be called after a semantic XeFG commit.

Do not use it on the native D3D12 path.

---

# 13. `has_active_xefg_instance_binding()` Migration

Current health predicate combines physical hook state and semantic ownership.

Preserve that distinction.

Recommended shape:

```cpp
bool has_active_xefg_instance_binding() const noexcept {
    return m_hooked
        && !m_is_phase_1
        && m_swapchain_source == SwapchainSource::XeFGInternal
        && m_swapchain_hook != nullptr
        && m_xefg_binding.active()
        && m_xefg_binding.aliases_match(
            m_swap_chain,
            m_command_queue,
            m_device);
}
```

Do not move `m_swapchain_hook != nullptr`, `m_hooked`, `m_is_phase_1`, or source checks into `XeFGBinding`.

Those are physical hook state.

Do not weaken this predicate.

Hook-monitor preservation relies on it.

---

# 14. Existing Public/Diagnostic Getters

Migrate the current API without changing behavior:

```cpp
uint64_t get_xefg_binding_generation() const {
    return m_xefg_binding.generation();
}

bool is_xefg_observe_only() const {
    return m_xefg_binding.observe_only();
}
```

`get_swap_chain()`, `get_command_queue()`, and `get_device()` continue to return legacy raw aliases.

Do not change their public semantics in R6.

This avoids unnecessary changes in:

- REFramework renderer code;
- hook monitor;
- diagnostics;
- XeFGCandidateHandoff;
- Present/resize callbacks.

---

# 15. Candidate Handoff Identity Helper Migration

R5 currently owns `binding_change_reason()` locally.

R6 may move this identity knowledge into `XeFGBinding` because identity is now its responsibility.

Preferred R6 cleanup:

```cpp
const auto change = hook.m_xefg_binding.compare(
    candidate.swapchain.Get(),
    candidate.selected_queue.Get(),
    candidate.observe_only);

const auto changed = change.changed();
const auto reason = change.reason();
```

The existing friend relationship:

```cpp
friend class XeFGCandidateHandoff;
```

is sufficient for this narrow use.

Do not make `XeFGBinding` or internal mutation functions broadly public solely for the handoff layer.

If moving this helper creates disproportionate churn, it is acceptable to leave the reason helper in R5 temporarily; however the new `XeFGBinding` must still provide deterministic identity/match semantics used by at least the D3D12 active-binding path.

---

# 16. Initial XeFG Bind — Preserve Current Physical Order

Current `bind_external_swapchain()` must remain structurally equivalent.

R6 should change storage calls, not the physical sequence.

Conceptual R6 translation:

```cpp
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

// Preserve current physical order.
m_present_hook.reset();
m_swapchain_hook.reset();
m_xefg_binding.clear();

if (source == SwapchainSource::XeFGInternal) {
    m_xefg_binding.commit_initial(
        std::move(next_xefg_swapchain),
        std::move(next_xefg_queue),
        std::move(next_xefg_device),
        xefg_p21_observe_only);
    sync_xefg_binding_aliases();
} else {
    // Existing native raw-alias behavior remains unchanged.
    m_swap_chain = swapchain;
    m_command_queue = command_queue;
    m_device = device.Get();
}

m_swapchain_source = source;
m_is_phase_1 = false;

// Existing VtableHook creation and exact slot installation remain here.
...

m_hooked = true;
clear_xefg_resize_transition_hold("external_bind");
```

Important:

- do not make this transaction safer/different yet;
- do not prepare the complete new hook before clearing the old binding in this generic initial-bind path as part of R6;
- that physical mutation protocol is the R7 task;
- preserve native behavior.

The only semantic difference in source should be that generation/mode now come from `XeFGBinding` rather than separate fields.

---

# 17. Initial Generation Semantics

Current behavior:

```cpp
m_xefg_binding_generation = source == SwapchainSource::XeFGInternal ? 1 : 0;
```

Preserve exactly.

Recommended:

```cpp
void XeFGBinding::commit_initial(..., bool observe_only) {
    m_swapchain = std::move(swapchain);
    m_queue = std::move(queue);
    m_device = std::move(device);
    m_observe_only = observe_only;
    m_generation = 1;
}
```

For native source, call:

```cpp
m_xefg_binding.clear();
```

which resets generation to `0`.

Do not introduce a process-global monotonically increasing generation in R6.

Generation remains per active binding lifetime as it is today.

---

# 18. Same-Swapchain Update — Preserve Current Sequence

Current `replace_xefg_binding()` same-object path does:

```text
strongly hold next queue/device locally
log begin
renderer reset
replace strong queue/device ownership
re-sync raw aliases
update observe-only mode
reset render-boundary diagnostic flag
increment generation
clear resize transition hold
log completion
```

Preserve exactly.

Recommended semantic call at the same point:

```cpp
g_framework->on_reset();

m_xefg_binding.commit_same_swapchain_update(
    std::move(next_queue),
    std::move(next_device),
    observe_only);

sync_xefg_binding_aliases();
m_xefg_p21_render_boundary_logged = false;

clear_xefg_resize_transition_hold("binding_replaced");
```

`commit_same_swapchain_update()` should:

- retain existing active swapchain ownership;
- replace selected queue ownership;
- replace device ownership;
- update observe-only mode;
- increment generation by one.

Do not reset generation to 1 on same-object update.

---

# 19. Changed-Swapchain Replacement — Preserve Current Lifetime Boundary

This is the most important R6 mutation site.

Current proven order must remain:

```text
next swapchain/queue/device strongly held in locals
    ↓
prepare complete next VtableHook
    ↓ failure => return, old binding untouched
renderer reset
    ↓
remove old VtableHook while old XeFG binding still strongly owns old swapchain
    ↓
commit new strong binding ownership
    ↓
synchronize raw aliases
    ↓
commit next VtableHook and physical flags
    ↓
increment generation semantics
```

With `XeFGBinding`, preferred source shape is conceptually:

```cpp
// Old m_xefg_binding remains untouched while new hook is prepared.
auto next_swapchain = ...;
auto next_queue = ...;
auto next_device = ...;
auto next_hook = prepare_current_slots(...);

if (!next_hook) {
    return false;
}

g_framework->on_reset();

m_present_hook.reset();
m_swapchain_hook.reset();

// ONLY NOW may replacing semantic ownership release the old swapchain.
m_xefg_binding.commit_replacement(
    std::move(next_swapchain),
    std::move(next_queue),
    std::move(next_device),
    observe_only);

sync_xefg_binding_aliases();
m_swapchain_hook = std::move(next_hook);
m_swapchain_source = SwapchainSource::XeFGInternal;
m_xefg_p21_render_boundary_logged = false;
m_is_phase_1 = false;
m_hooked = true;
```

`commit_replacement()` should increment the existing generation:

```cpp
const auto next_generation = m_generation + 1;
...
m_generation = next_generation;
```

Do not call `clear()` before `m_swapchain_hook.reset()` in this path.

Do not reset generation to 1 on replacement.

Do not move physical `VtableHook` ownership into `XeFGBinding`.

---

# 20. `commit_replacement()` Generation Edge

Avoid accidental generation loss when overwriting the whole semantic object.

Bad pattern:

```cpp
m_xefg_binding = XeFGBinding{...}; // generation may silently reset
```

Preferred:

```cpp
void XeFGBinding::commit_replacement(..., bool observe_only) {
    const auto next_generation = m_generation + 1;

    m_swapchain = std::move(swapchain);
    m_queue = std::move(queue);
    m_device = std::move(device);
    m_observe_only = observe_only;
    m_generation = next_generation;
}
```

R6 should preserve diagnostic generation behavior because resize/rebind logs and hook-monitor diagnostics already expose it.

---

# 21. Unhook — Exact Required Ownership Order

Current unhook behavior must be translated carefully.

Preferred R6 pattern:

```cpp
clear_xefg_resize_transition_hold("unhook");

if (!m_hooked && !m_xefg_binding.complete()) {
    return true;
}

const auto* owned_swapchain = m_xefg_binding.swapchain();
const auto* owned_queue = m_xefg_binding.queue();
const auto* owned_device = m_xefg_binding.device();

// PHYSICAL HOOKS FIRST.
m_present_hook.reset();
m_swapchain_hook.reset();

// Then clear aliases only when they point into XeFG-owned objects.
if (m_swap_chain == owned_swapchain) {
    m_swap_chain = nullptr;
}
if (m_command_queue == owned_queue) {
    m_command_queue = nullptr;
}
if (m_device == owned_device) {
    m_device = nullptr;
}

// STRONG OWNERSHIP LAST.
m_xefg_binding.clear();

m_hooked = false;
m_is_phase_1 = true;
```

This order is mandatory.

Blocking:

```cpp
m_xefg_binding.clear();
m_swapchain_hook.reset();
```

because the hook target could be released before the hook restores/removes the old vtable.

---

# 22. `clear()` Semantics

Recommended:

```cpp
void XeFGBinding::clear() noexcept {
    m_swapchain.Reset();
    m_queue.Reset();
    m_device.Reset();
    m_generation = 0;
    m_observe_only = false;
}
```

Do not retain stale mode or generation after ownership is gone.

Do not clear D3D12Hook raw aliases from inside `XeFGBinding::clear()`.

The semantic object does not know which raw aliases are native versus XeFG aliases.

Alias synchronization/cleanup remains D3D12Hook responsibility in R6.

---

# 23. `external_binding_matches()` Migration

This function supports generic source matching, so do not convert it into an XeFG-only function.

Preserve native behavior.

Recommended logic:

```cpp
bool D3D12Hook::external_binding_matches(
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* command_queue,
    SwapchainSource source,
    bool xefg_observe_only) const {

    if (!m_hooked
        || m_swapchain_hook == nullptr
        || m_swapchain_source != source
        || m_swap_chain != swapchain
        || m_command_queue != command_queue) {
        return false;
    }

    if (source != SwapchainSource::XeFGInternal) {
        return true;
    }

    return m_xefg_binding.matches(
        swapchain,
        command_queue,
        xefg_observe_only);
}
```

Do not alter what constitutes a native match.

---

# 24. Observe-Only Call-Site Migration

All current uses of the old field:

```cpp
m_xefg_p21_observe_only
```

must migrate to one source of truth.

Preferred internal read:

```cpp
m_xefg_binding.observe_only()
```

or existing helper:

```cpp
is_xefg_observe_only()
```

Do not leave a mirrored boolean in `D3D12Hook`.

The same applies to generation:

```cpp
m_xefg_binding.generation()
```

There must not be two mutable copies.

---

# 25. Do Not Move Render-Boundary Diagnostic State Yet

Keep:

```cpp
m_xefg_p21_render_boundary_logged
```

in `D3D12Hook` for now.

It is callback/logging state, not active binding identity.

Similarly keep:

```text
m_present_entry_count
m_last_present_entry_time
m_last_logged_present_*
```

out of `XeFGBinding`.

Logging cleanup is intentionally deferred to the final pre-release logging PR.

---

# 26. Resize State Is Explicitly Out of Scope

Do not move in R6:

```cpp
m_xefg_resize_event_id
m_xefg_resize_transition_hold
m_xefg_resize_transition_hold_event_id
m_xefg_resize_transition_suppressed_present_count
m_xefg_last_resize_kind
m_xefg_last_resize_event_time
m_xefg_post_resize_present_budget
m_xefg_post_resize_present_ordinal
```

R8 owns semantic resize lifecycle extraction.

Do not use R6 as an excuse to combine active binding and resize state because both are XeFG fields.

They have different failure domains.

---

# 27. MHW-Only Resize Hold Is Frozen

The PR #20 policy remains authoritative.

R6 must not change the condition equivalent to:

```cpp
event_id != 0
&& renderer_reset_performed
&& !is_xefg_observe_only()
&& sdk::GameIdentity::get().is_mhwilds()
```

If the old field is replaced by a getter during migration, that is fine:

```cpp
!d3d12->is_xefg_observe_only()
```

but the behavioral condition must remain identical.

No other game should gain the ResizeTarget hold in R6.

---

# 28. Present / Present1 Behavior Is Frozen

Do not change:

- which swapchain is forwarded to original Present;
- Present vs Present1 slot choice;
- render callback suppression during XeFG resize hold;
- observe-only callback policy;
- phase-1 handling;
- current `m_swapchain_hook` usage;
- present liveness bookkeeping.

Direct field-to-accessor migration is allowed only where necessary to remove duplicated active-state fields.

For example:

```cpp
if (!d3d12->is_xefg_observe_only()) {
    ...
}
```

is fine.

Changing when that branch is taken is not.

---

# 29. Hook-Monitor Policy Is Frozen

`REFramework::hook_monitor()` currently relies on:

```cpp
d3d12->has_active_xefg_instance_binding()
```

R6 may change the implementation of that predicate to use `XeFGBinding`, but must not change the caller policy.

No R6 changes to:

- present timeout duration;
- last-chance timing;
- preserve/re-hook decision;
- D3D11/D3D12 selection;
- hook-monitor logging cadence.

Hook-monitor isolation is R10.

---

# 30. Do Not Change Physical Hook Slots

The active XeFG instance hook remains:

```text
Present             [8]
ResizeBuffers       [13]
ResizeTarget        [14]
Present1            [22]
ResizeBuffers1      [39]
```

R6 must not add/remove/change these slots.

Do not add `Release` hooking.

Do not replace `VtableHook` with another hooking implementation.

Do not move these slot constants into `XeFGBinding`; it has no physical-hook responsibility.

---

# 31. Candidate Device vs Active Device

R4 candidate already strongly owns a validated device.

R5 pending storage keeps that candidate device alive until delivery.

However current active binding functions still obtain their active device from the swapchain:

```cpp
swapchain->GetDevice(IID_PPV_ARGS(&device))
```

R6 should preserve this existing handoff boundary.

Do not redesign bind signatures to pass the candidate device into active binding solely to save one `GetDevice()` call.

That would couple R4/R5 candidate representation to R6/R7 active mutation unnecessarily.

After active commit, `XeFGBinding` strongly owns the device acquired by the existing active-bind path.

---

# 32. Failure Behavior Must Remain Unchanged

R6 is not a retry/error-policy PR.

Preserve:

```text
GetDevice failure during bind
    -> bind fails
    -> no retry worker

new hook preparation failure during changed-object rebind
    -> existing old binding stays active

same-object update
    -> current reset/update behavior

pending consume bind failure
    -> candidate is not automatically requeued
```

Do not add:

- retries;
- sleeps;
- timers;
- background threads;
- generation replay;
- automatic pending restoration.

---

# 33. Logging Policy for R6

**Do not perform logging cleanup in this PR.**

The project will be released only after the final logging work is complete.

Therefore during R6:

- preserve existing `[XeFG][Bind]` logs;
- preserve `[XeFG][Rebind]` logs;
- preserve generation values;
- preserve hook-monitor diagnostics;
- preserve queue/discovery diagnostics;
- do not demote existing logs to debug yet;
- do not add the final Debug Logging checkbox yet.

Small wording changes caused strictly by field ownership movement are acceptable, but avoid churn.

The final logging PR will decide which messages remain normal-user support logs and which become debug-only.

Keeping diagnostics intact until then is useful while R6–R10 are still changing lifecycle ownership.

---

# 34. Recommended `XeFGBinding.cpp` Skeleton

Conceptual example:

```cpp
#include "XeFGBinding.hpp"

bool XeFGBinding::complete() const noexcept {
    return m_swapchain != nullptr
        && m_queue != nullptr
        && m_device != nullptr;
}

bool XeFGBinding::active() const noexcept {
    return complete() && m_generation != 0;
}

IDXGISwapChain3* XeFGBinding::swapchain() const noexcept {
    return m_swapchain.Get();
}

ID3D12CommandQueue* XeFGBinding::queue() const noexcept {
    return m_queue.Get();
}

ID3D12Device4* XeFGBinding::device() const noexcept {
    return m_device.Get();
}

bool XeFGBinding::observe_only() const noexcept {
    return m_observe_only;
}

uint64_t XeFGBinding::generation() const noexcept {
    return m_generation;
}

bool XeFGBinding::matches(
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* queue,
    bool observe_only) const noexcept {
    return active()
        && m_swapchain.Get() == swapchain
        && m_queue.Get() == queue
        && m_observe_only == observe_only;
}

bool XeFGBinding::aliases_match(
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* queue,
    ID3D12Device4* device) const noexcept {
    return complete()
        && m_swapchain.Get() == swapchain
        && m_queue.Get() == queue
        && m_device.Get() == device;
}

void XeFGBinding::commit_initial(
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain,
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue,
    Microsoft::WRL::ComPtr<ID3D12Device4> device,
    bool observe_only) {
    m_swapchain = std::move(swapchain);
    m_queue = std::move(queue);
    m_device = std::move(device);
    m_observe_only = observe_only;
    m_generation = 1;
}

void XeFGBinding::commit_same_swapchain_update(
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue,
    Microsoft::WRL::ComPtr<ID3D12Device4> device,
    bool observe_only) {
    m_queue = std::move(queue);
    m_device = std::move(device);
    m_observe_only = observe_only;
    ++m_generation;
}

void XeFGBinding::commit_replacement(
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain,
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue,
    Microsoft::WRL::ComPtr<ID3D12Device4> device,
    bool observe_only) {
    const auto next_generation = m_generation + 1;
    m_swapchain = std::move(swapchain);
    m_queue = std::move(queue);
    m_device = std::move(device);
    m_observe_only = observe_only;
    m_generation = next_generation;
}

void XeFGBinding::clear() noexcept {
    m_swapchain.Reset();
    m_queue.Reset();
    m_device.Reset();
    m_observe_only = false;
    m_generation = 0;
}
```

This is an example, not a mandate to copy blindly.

If the implementation adds defensive validity checks, they must not alter the current physical failure behavior or throw exceptions into game code.

---

# 35. Suggested `D3D12Hook.hpp` End State

Conceptually:

```cpp
#include "compatibility/xefg/XeFGBinding.hpp"
#include "compatibility/xefg/XeFGDiscovery.hpp"

class D3D12Hook {
    ...

    bool has_active_xefg_instance_binding() const noexcept {
        return m_hooked
            && !m_is_phase_1
            && m_swapchain_source == SwapchainSource::XeFGInternal
            && m_swapchain_hook != nullptr
            && m_xefg_binding.active()
            && m_xefg_binding.aliases_match(
                m_swap_chain,
                m_command_queue,
                m_device);
    }

    uint64_t get_xefg_binding_generation() const {
        return m_xefg_binding.generation();
    }

    bool is_xefg_observe_only() const {
        return m_xefg_binding.observe_only();
    }

protected:
    void sync_xefg_binding_aliases() noexcept;

    ID3D12Device4* m_device{};
    IDXGISwapChain3* m_swap_chain{};
    ID3D12CommandQueue* m_command_queue{};

    XeFGBinding m_xefg_binding{};

    // Resize/present state remains outside.
    ...
};
```

Do not make the generic renderer's raw aliases references into `XeFGBinding` or convert them into wrapper types.

Keep the upstream-sensitive surface simple.

---

# 36. No Duplicate Mutable Active State at PR End

This is a required static review condition.

After R6 there must be one mutable source of truth for:

```text
XeFG owned swapchain
XeFG owned selected queue
XeFG owned device
XeFG generation
XeFG observe-only mode
```

Bad final state:

```text
XeFGBinding m_xefg_binding
+
m_xefg_bound_swapchain
+
m_xefg_binding_generation
+
m_xefg_p21_observe_only
```

Do not leave both new and old fields synchronized manually.

Because the code is unreleased, delete the obsolete fork-only fields now.

Legacy raw **generic D3D12 aliases** are the deliberate exception:

```text
m_swap_chain
m_command_queue
m_device
```

They remain because the native/upstream renderer architecture still uses them.

---

# 37. Expected Diff Size

R6 will touch many direct field references, so GitHub add/delete may look larger than the semantic change.

Expected:

```text
effective implementation: ~160–280 LOC
GitHub add+delete:          ~300–550 LOC
```

The soft LOC ceiling remains guidance, not a reason to leave duplicate ownership fields behind.

Since the fork is unreleased, prefer finishing the semantic ownership cut cleanly rather than preserving a half-migrated compatibility layer solely to keep diff count low.

Still do not pull R7 work into this PR.

---

# 38. Required Build / Static Validation

Before PR completion:

```text
cmake -S . -B build
cmake --build build --config Release --target REFramework -- /m:4
git diff --check
```

Run the repository's existing direct-struct-field CI audit as applicable.

Also perform explicit source audits.

## 38.1 Old active-field removal audit

Search for:

```text
m_xefg_bound_swapchain
m_xefg_bound_queue
m_xefg_bound_device
m_xefg_binding_generation
m_xefg_p21_observe_only
```

Expected after R6:

```text
zero active implementation references
```

Historical docs may still contain old names; do not rewrite old work orders merely to make search globally empty.

## 38.2 New binding ownership audit

Verify `XeFGBinding` does **not** contain:

```text
VtableHook
PointerHook
REFramework*
g_framework
resize hold state
Present callbacks
pending candidate
runtime module state
```

## 38.3 Raw alias audit

For every semantic commit:

```text
commit_initial
commit_same_swapchain_update
commit_replacement
```

verify D3D12Hook aliases are synchronized immediately afterward before normal renderer use.

## 38.4 Unhook lifetime audit

Verify source order is:

```text
m_swapchain_hook.reset()
    BEFORE
m_xefg_binding.clear()
```

for the active unhook path.

## 38.5 Changed-object rebind lifetime audit

Verify old `m_xefg_binding` ownership remains untouched until after:

```text
m_swapchain_hook.reset()
```

on the changed-swapchain replacement path.

---

# 39. Native REFramework Regression Audit

R6 is not an XeFG-only success if the original native D3D12 path changes behavior.

Review explicitly:

- native dummy swapchain discovery unchanged;
- `hook_impl()` unchanged except necessary includes/member access;
- native `SwapchainSource::Native` binding path still uses existing raw aliases;
- D3D11 untouched;
- Lua/plugins/mods untouched;
- Streamline handling untouched;
- FSRFG detection untouched;
- normal overlay callback behavior untouched.

Do not make native D3D12 depend on `XeFGBinding` being active.

`XeFGBinding` must be inert when XeFG is not active.

---

# 40. XeFG Static Scenario Matrix

Review the implementation against these scenarios.

## Scenario A — no XeFG

Expected:

```text
XeFGBinding inactive
generation = 0
no owned XeFG objects
native D3D12 behavior unchanged
```

## Scenario B — first valid XeFG candidate

Expected:

```text
strong swapchain/queue/device committed
generation = 1
observe-only matches candidate
raw aliases match binding
physical current slots installed exactly as before
```

## Scenario C — identical candidate

Expected:

```text
handoff identifies unchanged binding
no renderer reset
no generation increment
no physical rehook
```

## Scenario D — same swapchain, queue changed

Expected:

```text
real change
renderer reset once
swapchain strong ownership retained
queue/device replaced
generation +1
raw aliases synchronized
existing physical swapchain hook retained
```

## Scenario E — same swapchain/queue, mode changed

Expected:

```text
real change
same-object update path
generation +1
observe-only changes
physical swapchain hook retained
```

## Scenario F — changed swapchain, new hook preparation succeeds

Expected:

```text
old binding strongly owns old target while next hook prepares
renderer reset
old hook removed
then old strong ownership released through semantic replacement
new ownership committed
new physical hook committed
generation +1
```

## Scenario G — changed swapchain, new hook preparation fails

Expected:

```text
old physical hook remains
old XeFGBinding ownership remains
old aliases remain
old generation/mode remain
candidate local ownership releases on return
```

## Scenario H — unhook

Expected:

```text
physical hook removed first
XeFG aliases cleared when they reference owned objects
XeFGBinding cleared last
generation = 0
observe-only = false
```

---

# 41. Runtime Validation Guidance

The formal high-risk binding runtime wave remains after R7 because R7 changes the physical mutation protocol.

For R6 itself, if runtime testing is available, a short smoke is strongly recommended because COM lifetime was moved:

```text
Dragon's Dogma 2 + OptiScaler + XeFG
    launch
    confirm XeFG initializes
    confirm REFramework overlay appears
    confirm OptiScaler overlay appears
    open/close REFramework menu
    basic Alt+Tab
    exit cleanly
```

If Monster Hunter Wilds is used, additionally ensure the existing MHW-only ResizeTarget hold behavior does not regress.

Do not claim runtime validation in the PR unless actually performed.

A local Release build does not count as runtime validation.

---

# 42. Forbidden Changes in R6

Blocking scope creep includes:

1. moving `m_swapchain_hook` into `XeFGBinding`;
2. changing hook slot list/order;
3. rewriting `replace_xefg_binding()` into a new transaction protocol;
4. changing new-hook failure behavior;
5. changing renderer reset ordering;
6. changing pending-candidate semantics;
7. changing R4 queue/device validation policy;
8. changing R3 factory observation;
9. changing loader/runtime registry behavior;
10. changing resize lifecycle semantics;
11. broad Present/Present1 cleanup;
12. hook-monitor policy changes;
13. adding generic FG provider architecture;
14. FSRFG/DLSSG refactors;
15. logging reduction/debug UI work;
16. threads/timers/polling/sleeps;
17. private OptiScaler hooks;
18. public XeFG proxy becoming render authority.

---

# 43. Review Blockers

Treat the following as merge-blocking defects.

## Ownership / lifetime

- `XeFGBinding::clear()` occurs before active `m_swapchain_hook.reset()`;
- changed-object replacement releases old binding ownership before old hook removal;
- new semantic object stores only raw pointers instead of strong COM ownership;
- one of swapchain/queue/device loses strong ownership while active;
- `VtableHook` is incorrectly treated as the COM owner.

## Identity / generation

- same swapchain + changed queue is treated as unchanged;
- mode-only change is treated as unchanged;
- initial generation is not 1;
- same-object/replacement generation does not increment once;
- generation resets unexpectedly on replacement;
- inactive clear leaves nonzero generation.

## Alias correctness

- active XeFG raw aliases no longer equal strong-owned objects;
- aliases are cleared while physical hook still needs the target;
- native raw aliases are accidentally redirected through XeFG state.

## Scope

- R7 physical protocol is redesigned;
- ResizeTarget MHW-only behavior changes;
- Present forwarding/suppression changes;
- hook-monitor caller policy changes;
- logging cleanup begins early.

---

# 44. Non-Blocking Findings by Themselves

Do not block solely for:

- naming `commit_initial()` differently;
- making simple getters inline;
- putting `IdentityChange::reason()` in `.cpp` versus header;
- using a small private helper to assign ComPtrs;
- modest add/delete count caused by field migration;
- moving `binding_change_reason` from R5 into R6 versus leaving it temporarily, provided there is still one authoritative active identity definition;
- comment wording/style.

As always, review blockers should be tied to realistic compatibility, lifetime, race, or scope risk.

---

# 45. Suggested Implementation Sequence

Recommended coding order:

## Step 1 — Add semantic state type

Add:

```text
XeFGBinding.hpp
XeFGBinding.cpp
```

with getters, complete/active, identity comparison, commit helpers, clear.

Do not modify physical hook code yet.

## Step 2 — Register build sources

Surgically add both files to checked-in `CMakeLists.txt`.

Do not modify `cmake.toml`.

## Step 3 — Replace D3D12Hook ownership fields

Replace old strong fields/generation/mode with:

```cpp
XeFGBinding m_xefg_binding{};
```

Add alias-sync helper.

## Step 4 — Migrate read-only getters / health predicate

Migrate:

```text
has_active_xefg_instance_binding()
get_xefg_binding_generation()
is_xefg_observe_only()
external_binding_matches() XeFG mode check
```

## Step 5 — Migrate initial bind storage

Translate strong field assignments to `commit_initial()` without changing physical hook sequencing.

## Step 6 — Migrate same-object update

Use `commit_same_swapchain_update()` at the exact current semantic-commit point.

## Step 7 — Migrate changed-object replacement

Use `commit_replacement()` only **after old hook removal**, preserving old lifetime until then.

## Step 8 — Migrate unhook

Snapshot owned raw pointers, remove hooks, clear aliases, then clear semantic binding.

## Step 9 — Optional identity-helper cleanup

Move R5 `binding_change_reason()` into `XeFGBinding::IdentityChange` if straightforward.

## Step 10 — Static audit

Search for old fields and verify no duplicate mutable XeFG active state remains.

## Step 11 — Build + diff checks

Run mandatory validation.

---

# 46. Expected Final Architecture After R6

```text
XeFGRuntimeRegistry
    exact runtime hook ownership

XeFGCompatibility
    loader/probe handoff

XeFGDiscovery
    InitDesc observation + candidate validation

XeFGCandidateHandoff
    pending/live lifecycle delivery

XeFGBinding                     <-- R6
    strong active swapchain ownership
    strong active selected queue ownership
    strong active device ownership
    generation
    observe-only mode
    semantic identity comparison
    active/complete state

D3D12Hook
    native/generic D3D12 raw aliases
    physical VtableHook
    physical callback functions
    current initial bind protocol
    current rebind protocol
    resize/present lifecycle
```

The raw alias relationship remains deliberately:

```text
active XeFG:
    D3D12Hook::m_swap_chain
        == XeFGBinding::swapchain()

    D3D12Hook::m_command_queue
        == XeFGBinding::queue()

    D3D12Hook::m_device
        == XeFGBinding::device()
```

This is intentional until later cleanup. Do not try to remove all raw D3D12 fields in R6.

---

# 47. Definition of Done

R6 is complete only when all of the following are true:

```text
[ ] XeFGBinding.hpp/.cpp added
[ ] tracked CMakeLists.txt registers both files
[ ] old m_xefg_bound_swapchain removed
[ ] old m_xefg_bound_queue removed
[ ] old m_xefg_bound_device removed
[ ] old m_xefg_binding_generation removed
[ ] old m_xefg_p21_observe_only removed
[ ] one XeFGBinding object owns active XeFG semantic state
[ ] strong swapchain/queue/device lifetime preserved
[ ] initial generation remains 1
[ ] update/replacement generation increments once
[ ] observe-only is single-source
[ ] active raw aliases synchronize with binding
[ ] changed-object old ownership survives through old hook removal
[ ] unhook removes physical hook before semantic ownership clear
[ ] native D3D12 path remains behaviorally unchanged
[ ] R5 pending semantics unchanged
[ ] R4 candidate semantics unchanged
[ ] ResizeTarget MHW-only policy unchanged
[ ] Present/Present1 behavior unchanged
[ ] hook-monitor policy unchanged
[ ] no logging cleanup / Debug Logging UI work
[ ] no generic FG abstraction
[ ] Release build passes
[ ] static field/lifetime audits pass
[ ] git diff --check passes
```

---

# 48. PR Description Template

Suggested PR body:

```markdown
## Summary

- Add `XeFGBinding` as the single semantic owner of the active XeFG swapchain, selected queue, device, generation, and observe-only mode.
- Keep D3D12Hook raw renderer aliases synchronized with the strong XeFG binding.
- Preserve the existing initial bind, same-object update, changed-object hook replacement, unhook, resize, Present, and hook-monitor ordering.
- Remove the old duplicate fork-only XeFG active-state fields.

## Scope

R6 only. This PR does not redesign physical VtableHook installation/replacement, resize lifecycle, Present callbacks, hook-monitor policy, or logging.

## Validation

- `cmake -S . -B build`
- `cmake --build build --config Release --target REFramework -- /m:4`
- `git diff --check`
- audited changed-object and unhook COM lifetime ordering
- audited removal of old XeFG ownership/generation/mode fields

Runtime smoke: <state honestly if performed>
```

---

# 49. Final Instruction to Implementer

Treat this PR as an **ownership relocation, not a behavior redesign**.

The critical mental model is:

```text
candidate/pending objects own the next potential binding

XeFGBinding owns the currently committed XeFG binding

D3D12Hook VtableHook physically targets that committed swapchain

therefore:
    old XeFGBinding ownership must survive until old VtableHook removal
```

Use the fact that the fork is unreleased to remove duplicate intermediate state cleanly.

Do **not** use that freedom to combine R7–R11 early.

Keep all diagnostics intact until the final logging PR, because the project will not be released before logging cleanup is complete.
