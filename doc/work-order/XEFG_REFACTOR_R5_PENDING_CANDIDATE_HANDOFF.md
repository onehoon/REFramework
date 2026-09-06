# Work Order: XeFG Refactor R5 — Pending Candidate Handoff Extraction

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `1eab453505fe3c75bb7ed45715ce7a1d2c6cc35f` (`Refactor R4: extract XeFG queue validation and binding candidate (#22)`)

Relevant merged refactor baseline:

- R1 / PR #17: `65f9b3ee81971c3e2aac6df49518fa2dd588365d` — exact-HMODULE XeFG runtime registry extraction
- R2 / PR #18: `aa3a53e516b882a77d399c929efa0ef29d1426b0` — XeFG loader / probe handoff isolation
- PR #19: `3f83b8af0184f931daec44dc45257f7fc46966a4` — tracked `CMakeLists.txt` compatibility-source registration
- PR #20: `74042e1686f62a54a50540e1a113a3ae648778c1` — MHW-only XeFG ResizeTarget transition hold
- R3 / PR #21: `cfb6efe5102f330c9ddf6fdadb7e9af984ce0678` — InitDesc observation transaction + temporary factory capture extraction
- R4 / PR #22: `1eab453505fe3c75bb7ed45715ce7a1d2c6cc35f` — queue/device/HWND validation + strong binding candidate construction extraction

Related documents:

- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_ARCHITECTURE_2026-09-06.md`
- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_PR_SPLIT_PLAN_2026-09-06.md`
- `doc/work-order/XEFG_REFACTOR_R1_RUNTIME_REGISTRY_EXTRACTION.md`
- `doc/work-order/XEFG_REFACTOR_R2_LOADER_PROBE_HANDOFF.md`
- `doc/work-order/XEFG_REFACTOR_R3_INIT_TRANSACTION_FACTORY_CAPTURE.md`
- `doc/work-order/XEFG_REFACTOR_R4_QUEUE_VALIDATION_CANDIDATE.md`

This work order implements **R5 only** from the fine-grained XeFG refactor plan.

---

# 1. Recommended PR Identity

Suggested branch:

```text
refactor/xefg-r5-pending-candidate-handoff
```

Suggested PR title:

```text
Refactor R5: extract XeFG pending candidate handoff
```

Suggested commit title:

```text
refactor: extract XeFG candidate handoff
```

This is a **behavior-preserving lifecycle ownership extraction**.

The one primary responsibility is:

> Move the mechanics that deliver a validated XeFG binding candidate either to the currently live `D3D12Hook` or to a single pending slot consumed by the next `D3D12Hook::hook()` call.

Do not begin R6 active binding-state extraction or R7 bind/rebind redesign here.

---

# 2. Current State After R4

R4 now cleanly produces a strongly-owned, validated candidate:

```cpp
struct XeFGBindingCandidate {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> selected_queue{};
    Microsoft::WRL::ComPtr<ID3D12Device4> device{};
    HWND hwnd{};
    XeFGQueueRelation relation{XeFGQueueRelation::InitQueueUnavailable};
    bool observe_only{true};

    bool valid() const noexcept {
        return swapchain != nullptr
            && selected_queue != nullptr
            && device != nullptr;
    }
};
```

The discovery/validation flow is now:

```text
XeFGRuntimeRegistry
    -> exact InitDesc trampoline

XeFGDiscovery::observe_init()                 [R3]
    -> raw observation

XeFGDiscovery::build_binding_candidate()      [R4]
    -> accepted strong XeFGBindingCandidate
       OR deterministic rejection
```

However, `D3D12Hook.cpp` still owns the next independent subsystem: **candidate lifecycle handoff**.

Current master still contains, conceptually and literally:

```cpp
struct PendingXefgBinding {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> selected_queue{};
    HWND hwnd{};
    XeFGQueueRelation relation{XeFGQueueRelation::InitQueueUnavailable};
    bool observe_only{true};
};

std::optional<PendingXefgBinding> g_pending_xefg_binding{};
```

`D3D12Hook::publish_xefg_candidate()` currently does two different jobs:

```text
A. candidate result logging
B. lifecycle delivery
   - if a live D3D12Hook exists: apply immediately
   - if no live D3D12Hook exists: store one pending binding
```

and `D3D12Hook::hook()` begins with:

```cpp
if (consume_pending_xefg_binding(*this)) {
    spdlog::info("Hooked DirectX 12 through pending XeFG binding");
    return true;
}
```

R5 extracts **B only**.

---

# 3. Why R5 Is Separate

R4 answered:

> Is this discovery observation a valid XeFG binding candidate, and what queue/mode should it use?

R5 answers:

> When and where should this already-valid candidate be delivered?

Later PRs answer:

```text
R6: how active XeFG binding state owns identity/COM objects
R7: how initial bind and transactional rebind physically mutate hooks
```

Keeping R5 separate makes failures attributable:

```text
R4 regression
    = validation/candidate construction

R5 regression
    = candidate delivery / lifecycle race / pending-slot behavior

R6/R7 regression
    = active ownership or physical bind/rebind
```

Do not collapse these stages.

---

# 4. Current Handoff Semantics That Are Proven and Must Be Preserved

The current working lifecycle logic has three cases.

## Case A — Valid candidate, framework and live D3D12Hook already exist

```text
validated candidate
    -> acquire REFramework hook-monitor lifecycle mutex
    -> read current g_d3d12_hook only AFTER mutex acquisition
    -> if active XeFG binding exists:
         compare swapchain / selected queue / observe-only mode
         unchanged -> log and return
         changed   -> call existing replace_xefg_binding()
    -> else:
         if active non-XeFG renderer exists:
             reset renderer once
         call existing bind_external_swapchain()
```

## Case B — Valid candidate, framework exists, but live D3D12Hook does not yet exist

```text
validated candidate
    -> acquire hook-monitor lifecycle mutex
    -> confirm current hook == nullptr
    -> while STILL holding lifecycle mutex:
         publish candidate into pending slot
    -> release lifecycle mutex
```

This ordering is critical.

The pending slot must be populated **before** another hook-creation path protected by the same lifecycle mutex can proceed.

## Case C — Constructor/startup timing before `g_framework` is published

```text
validated candidate
    -> framework pointer unavailable
    -> store candidate directly in pending slot
    -> later D3D12Hook::hook() consumes it
```

This fallback is required for the existing early-init ordering.

R5 must preserve all three cases.

---

# 5. The Most Important R5 Invariant: No Lost Capture-Before-Hook Candidate

The central race to prevent is:

```text
Thread A: XeFG InitDesc discovers candidate
Thread B: REFramework creates D3D12Hook
```

Broken ordering example:

```text
A checks current hook == nullptr
A releases lifecycle protection
B creates hook
B checks pending -> empty
A stores pending

result:
    live hook exists
    candidate is pending forever / initial XeFG bind missed
```

The current code prevents this by storing pending while the lifecycle mutex is still held.

R5 must preserve the equivalent transaction:

```text
lock lifecycle mutex
    read current hook
    if hook exists:
        immediate delivery
    else:
        lock pending mutex
        store/replace pending candidate
        unlock pending mutex
unlock lifecycle mutex
```

Do **not** split the `hook == nullptr` observation and pending publication across the lifecycle lock boundary.

This is a merge-blocking invariant.

---

# 6. Strict R5 Scope

## 6.1 Move into the R5 handoff component

Move ownership of:

- pending XeFG candidate storage;
- pending candidate mutex;
- pending replacement semantics;
- immediate delivery to an existing `D3D12Hook`;
- active-XeFG identity-change gate used before calling current rebind;
- `binding_change_reason()` helper;
- immediate non-XeFG -> XeFG handoff reset decision;
- consume-on-`D3D12Hook::hook()` behavior;
- constructor-time fallback pending publication;
- lifecycle ordering around live-hook lookup and pending publication.

## 6.2 Keep in `D3D12Hook` in R5

Do **not** move or rewrite:

- `bind_external_swapchain()` implementation;
- `replace_xefg_binding()` implementation;
- active strong COM fields;
- binding generation;
- `has_active_xefg_instance_binding()` semantics;
- physical `VtableHook` creation/removal;
- Present / Present1 callbacks;
- ResizeBuffers / ResizeTarget / ResizeBuffers1 callbacks;
- MHW-only ResizeTarget hold condition;
- renderer callback suppression;
- hook-monitor preservation policy;
- native D3D12 hook discovery;
- Streamline / FSRFG logic;
- R1 runtime registry;
- R2 loader façade;
- R3 factory transaction;
- R4 validation policy.

R5 **calls** existing active bind/rebind methods. It does not redesign them.

---

# 7. Preferred Component

Recommended new files:

```text
src/compatibility/xefg/XeFGCandidateHandoff.hpp
src/compatibility/xefg/XeFGCandidateHandoff.cpp
```

Preferred responsibility comment:

```cpp
// Delivers an already-validated XeFG binding candidate to the current D3D12Hook
// or stores one pending candidate for the next hook creation. This component
// owns lifecycle handoff only; it does not implement active binding mutation.
```

Do **not** create `XeFGBinding` in this PR.

`XeFGBinding` is the R6 semantic active-state object and should remain a separate ownership change.

Do not call the R5 class a generic frame-generation handoff manager.

This is XeFG-specific.

---

# 8. Reuse the R4 Candidate Directly — Remove the Duplicate Pending Shape

R4 already gives downstream code a strong candidate with all relevant identity:

```cpp
XeFGBindingCandidate
```

R5 should preferably store that type directly rather than maintain a second almost-identical `PendingXefgBinding`.

Recommended pending state:

```cpp
static std::mutex s_pending_mutex;
static std::optional<XeFGBindingCandidate> s_pending_candidate;
```

Advantages:

- no candidate -> pending field-copy translation;
- pending slot retains strong swapchain ownership;
- pending slot retains strong selected-queue ownership;
- pending slot also retains the R4-validated device ownership;
- relation/HWND/mode stay attached to the candidate identity;
- no second structure can drift from the R4 candidate contract.

This does **not** mean R6 active ownership has started.

The device `ComPtr` here belongs to the **pending candidate lifetime only**. The current active binding methods remain authoritative after delivery.

Do not make the pending object the active binding object.

---

# 9. Pending Slot Semantics: One Slot, Latest Valid Candidate Wins

Current behavior is assignment:

```cpp
g_pending_xefg_binding = pending;
```

Therefore R5 must preserve **single-slot replacement**, not queueing.

Required behavior:

```text
pending empty + candidate A
    -> pending = A

pending A + candidate B before hook consumes
    -> pending = B
    -> A released after replacement
```

Do not introduce:

- `std::queue`;
- `std::deque`;
- unbounded candidate history;
- per-runtime pending lists;
- generation-based replay;
- delayed retries.

The validated candidate representing the newest observed usable XeFG initialization should replace the previous pending candidate.

---

# 10. Recommended API

Conceptual header:

```cpp
#pragma once

#include <mutex>
#include <optional>

#include "XeFGDiscovery.hpp"

class D3D12Hook;

class XeFGCandidateHandoff {
public:
    // Candidate is already accepted by XeFGDiscovery::build_binding_candidate().
    static void publish(XeFGBindingCandidate candidate);

    // Called at the current early point in D3D12Hook::hook().
    // Returns true only when a pending candidate was consumed and the existing
    // bind_external_swapchain() path succeeded.
    static bool consume_pending(D3D12Hook& hook);

private:
    static void store_pending(XeFGBindingCandidate candidate);
    static void apply_to_live_hook(
        D3D12Hook& hook,
        const XeFGBindingCandidate& candidate);

    static std::mutex s_pending_mutex;
    static std::optional<XeFGBindingCandidate> s_pending_candidate;
};
```

Exact names are flexible.

The semantics are not.

---

# 11. Recommended D3D12Hook Bridge — Keep `g_d3d12_hook` Private to Its Existing Translation Unit

Current `g_d3d12_hook` is a file-scope implementation detail in `D3D12Hook.cpp`.

Do **not** make it a new exported global just so the R5 component can read it.

Preferred narrow bridge:

```cpp
class XeFGCandidateHandoff;

class D3D12Hook {
    friend class XeFGCandidateHandoff;

    // Internal bridge only. Caller must already hold REFramework's hook-monitor
    // lifecycle mutex before using the returned pointer as stable.
    static D3D12Hook* current_xefg_handoff_target() noexcept;

    // existing protected/private methods remain unchanged
};
```

Implementation in `D3D12Hook.cpp`:

```cpp
D3D12Hook* D3D12Hook::current_xefg_handoff_target() noexcept {
    return g_d3d12_hook;
}
```

The handoff component must call this accessor **after** acquiring the lifecycle mutex.

Do not create a generic global `D3D12Hook::current()` API for unrelated consumers.

Do not move generic D3D12 current-hook ownership into the XeFG component.

---

# 12. Why a Friend Bridge Is Acceptable Here

R5 needs to preserve the current immediate path that can call:

```cpp
replace_xefg_binding(...)
```

That method is intentionally not broad public API.

A narrow friend relationship:

```cpp
friend class XeFGCandidateHandoff;
```

is preferable to:

- making all active binding mutation methods public;
- exposing `g_d3d12_hook` globally;
- introducing a generic event bus;
- introducing `std::function` callback registration solely for this refactor;
- moving the active binding implementation early into R5.

Keep the friend limited to the XeFG handoff component.

---

# 13. `publish()` Required Algorithm

Conceptual implementation:

```cpp
void XeFGCandidateHandoff::publish(XeFGBindingCandidate candidate) {
    if (g_framework != nullptr) {
        std::unique_lock<std::recursive_mutex> lifecycle_lock{
            g_framework->get_hook_monitor_mutex()
        };

        // CRITICAL: read current target only after lifecycle lock acquisition.
        if (auto* hook = D3D12Hook::current_xefg_handoff_target(); hook != nullptr) {
            apply_to_live_hook(*hook, candidate);
            return;
        }

        // CRITICAL: store before lifecycle_lock is released.
        store_pending(std::move(candidate));
        return;
    }

    // Existing early-constructor fallback.
    store_pending(std::move(candidate));
}
```

Important:

- `publish()` receives only an **accepted** R4 candidate;
- no validation policy is reimplemented here;
- no polling or worker is introduced;
- no sleep or retry is introduced;
- no pending lock is held while active bind/rebind code runs.

---

# 14. `apply_to_live_hook()` Must Be a Literal Behavior Move

The immediate path is not an opportunity to improve bind logic.

Preserve current behavior exactly.

Conceptual implementation:

```cpp
void XeFGCandidateHandoff::apply_to_live_hook(
    D3D12Hook& hook,
    const XeFGBindingCandidate& candidate) {

    const auto has_active_xefg = hook.is_hooked()
        && hook.get_swap_chain() != nullptr
        && hook.get_swapchain_source() == D3D12Hook::SwapchainSource::XeFGInternal;

    if (has_active_xefg) {
        const auto swapchain_changed =
            hook.get_swap_chain() != candidate.swapchain.Get();
        const auto queue_changed =
            hook.get_command_queue() != candidate.selected_queue.Get();
        const auto mode_changed =
            hook.is_xefg_observe_only() != candidate.observe_only;

        const auto changed =
            swapchain_changed || queue_changed || mode_changed;
        const auto reason = binding_change_reason(
            swapchain_changed, queue_changed, mode_changed);

        spdlog::info(
            "[XeFG][BindingGate] action = {}, reason = {}, "
            "old_swapchain = 0x{:x}, new_swapchain = 0x{:x}, "
            "old_queue = 0x{:x}, new_queue = 0x{:x}, "
            "old_observe_only = {}, new_observe_only = {}",
            changed ? "rebind" : "unchanged",
            reason,
            reinterpret_cast<uintptr_t>(hook.get_swap_chain()),
            reinterpret_cast<uintptr_t>(candidate.swapchain.Get()),
            reinterpret_cast<uintptr_t>(hook.get_command_queue()),
            reinterpret_cast<uintptr_t>(candidate.selected_queue.Get()),
            hook.is_xefg_observe_only(),
            candidate.observe_only);

        if (changed
            && !hook.replace_xefg_binding(
                candidate.swapchain.Get(),
                candidate.selected_queue.Get(),
                candidate.observe_only,
                reason)) {
            spdlog::warn(
                "[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = rebind_failed",
                reinterpret_cast<uintptr_t>(candidate.swapchain.Get()));
        }

        return;
    }

    const auto replacing_active_non_xefg = hook.is_hooked()
        && hook.get_swap_chain() != nullptr
        && hook.get_swapchain_source() != D3D12Hook::SwapchainSource::XeFGInternal;

    if (replacing_active_non_xefg) {
        spdlog::info(
            "[XeFG][Bind] resetting active D3D12 renderer before XeFG bind");
        g_framework->on_reset();
    }

    if (!hook.bind_external_swapchain(
            candidate.swapchain.Get(),
            candidate.selected_queue.Get(),
            D3D12Hook::SwapchainSource::XeFGInternal,
            candidate.observe_only)) {
        spdlog::warn(
            "[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = external_bind_failed",
            reinterpret_cast<uintptr_t>(candidate.swapchain.Get()));
    }
}
```

This is intentionally close to the current code.

R5 should not make this prettier at the cost of semantic changes.

---

# 15. Preserve the Existing Meaning of “Unchanged”

Current active-XeFG identity comparison uses exactly:

```text
swapchain raw identity
selected command queue raw identity
observe-only mode
```

Do not expand R5 identity comparison to include:

- HWND;
- relation enum;
- candidate device pointer;
- runtime module;
- discovery context;
- resize generation.

Those may be useful semantic fields later, but changing binding identity belongs to R6/R7 only if justified.

For R5, preserve:

```cpp
changed = swapchain_changed || queue_changed || mode_changed;
```

---

# 16. Move `binding_change_reason()` with the Handoff Policy

Current helper:

```cpp
const char* binding_change_reason(
    bool swapchain_changed,
    bool queue_changed,
    bool mode_changed);
```

is only part of the immediate candidate-to-live-binding gate.

Move it into `XeFGCandidateHandoff.cpp` anonymous namespace or private implementation scope.

Preserve current reason strings:

```text
multiple_fields_changed
swapchain_changed
queue_changed
mode_changed
identical
```

Do not rewrite log taxonomy in R5.

Logging cleanup is deferred to the final logging PR.

---

# 17. Pending Storage Must Not Be Held Across Bind Calls

Required `store_pending()` shape:

```cpp
void XeFGCandidateHandoff::store_pending(XeFGBindingCandidate candidate) {
    std::scoped_lock lock{s_pending_mutex};
    s_pending_candidate = std::move(candidate);
}
```

Required `consume_pending()` shape:

```cpp
bool XeFGCandidateHandoff::consume_pending(D3D12Hook& hook) {
    std::optional<XeFGBindingCandidate> pending;

    {
        std::scoped_lock lock{s_pending_mutex};
        pending = std::move(s_pending_candidate);
        s_pending_candidate.reset();
    }

    if (!pending.has_value()) {
        return false;
    }

    return hook.bind_external_swapchain(
        pending->swapchain.Get(),
        pending->selected_queue.Get(),
        D3D12Hook::SwapchainSource::XeFGInternal,
        pending->observe_only);
}
```

Critical lock rule:

> Release the pending mutex before calling `bind_external_swapchain()`, `replace_xefg_binding()`, `g_framework->on_reset()`, or any renderer/hook mutation.

The pending mutex protects only the pending slot.

It must never become the active binding mutex.

---

# 18. Consume Semantics Must Remain Destructive-Before-Bind

Current code does:

```text
copy pending locally
clear global pending
call bind_external_swapchain()
```

Therefore R5 must preserve:

```text
pending candidate exists
    -> remove it from shared pending state
    -> attempt bind once
    -> if bind fails, candidate is NOT automatically requeued
```

Do not add:

- restore-on-bind-failure;
- retry count;
- delayed retry;
- fallback queue;
- background reattempt.

Those would change failure behavior and hide R7 issues.

If pending bind fails, `D3D12Hook::hook()` should continue through its current normal/native setup path exactly as it does now when `consume_pending_xefg_binding()` returns false.

---

# 19. Immediate Bind/Rebind Failure Must Also Remain Non-Requeued

Current live path does not convert a failed immediate bind/rebind into a pending retry.

Preserve:

```text
active XeFG + changed candidate + rebind fails
    -> log rebind_failed
    -> return
    -> do not place candidate in pending slot

live hook + initial external bind fails
    -> log external_bind_failed
    -> return
    -> do not place candidate in pending slot
```

R5 must not invent a “safer” retry mechanism.

R7 owns bind/rebind robustness.

---

# 20. D3D12Hook Call-Site Simplification

After R5, `D3D12Hook::publish_xefg_candidate()` should stop owning lifecycle delivery.

Conceptual before:

```cpp
void D3D12Hook::publish_xefg_candidate(const Observation& observation) {
    auto decision = build_binding_candidate(...);
    // logs...

    PendingXefgBinding pending = ...;

    if (g_framework != nullptr) {
        // lifecycle lock
        // g_d3d12_hook lookup
        // rebind / bind / pending store
    } else {
        // pending store
    }
}
```

Conceptual after:

```cpp
void D3D12Hook::publish_xefg_candidate(
    const XeFGDiscovery::Observation& observation) {

    XeFGBindingCandidateResult decision{};
    {
        // Keep the current local state-lock scope unless separately required.
        std::scoped_lock lock{g_xefg_state_mutex};
        decision = XeFGDiscovery::build_binding_candidate(observation);

        // Keep existing accept/reject/probe logs here in R5.
        // Logging ownership cleanup is not this PR.
    }

    if (!decision.accepted()) {
        return;
    }

    XeFGCandidateHandoff::publish(
        std::move(*decision.candidate));
}
```

Important:

- R5 should **not** move R4 candidate validation back into `D3D12Hook`;
- R5 should **not** move R4 validation into the handoff component;
- R5 should **not** move logging just because delivery moves.

The handoff component receives an accepted candidate only.

---

# 21. D3D12Hook::hook() Change Must Stay Tiny

Current:

```cpp
if (consume_pending_xefg_binding(*this)) {
    spdlog::info("Hooked DirectX 12 through pending XeFG binding");
    return true;
}
```

Target:

```cpp
if (XeFGCandidateHandoff::consume_pending(*this)) {
    spdlog::info("Hooked DirectX 12 through pending XeFG binding");
    return true;
}
```

Do not move the rest of `D3D12Hook::hook()` into the compatibility component.

Do not change the point at which pending consumption happens.

It must remain before the known-pointer/native dummy discovery path.

---

# 22. What to Remove from D3D12Hook in R5

Expected removals from `D3D12Hook.cpp`:

```text
PendingXefgBinding
g_pending_xefg_binding
binding_change_reason()
D3D12Hook::consume_pending_xefg_binding()
lifecycle/immediate delivery body from publish_xefg_candidate()
```

Expected removal from `D3D12Hook.hpp`:

```cpp
static bool consume_pending_xefg_binding(D3D12Hook& hook);
```

Expected narrow additions:

```text
forward declaration: class XeFGCandidateHandoff;
friend class XeFGCandidateHandoff;
private/protected current_xefg_handoff_target() bridge
```

Do not remove active XeFG fields in R5.

Those are R6.

---

# 23. Do Not Opportunistically Delete `g_xefg_state_mutex`

After pending storage moves out, current master still uses `g_xefg_state_mutex` around other XeFG-adjacent code, including the public-proxy diagnostic and the candidate-build/log scope.

Even if some of those locks now appear redundant after R3/R4, R5 is not the cleanup PR for them.

Therefore:

```text
move pending synchronization -> XeFGCandidateHandoff::s_pending_mutex
leave remaining g_xefg_state_mutex uses unchanged
```

Do not merge the pending mutex with the discovery transaction mutex.

Do not use the pending mutex as a replacement for `g_xefg_state_mutex` globally.

One mutex should have one ownership domain.

---

# 24. Lock Ordering Contract

R5 should make lock ordering explicit in comments.

## Publish with framework available

Allowed order:

```text
XeFG observation transaction lock    [already held by caller scope]
    -> REFramework hook-monitor lifecycle mutex
        -> pending mutex only if no live hook exists
```

The handoff component must **not** acquire pending mutex first and then lifecycle mutex.

## Consume path

The existing `D3D12Hook::hook()` lifecycle contract is authoritative.

Within consume:

```text
caller lifecycle contract
    -> pending mutex
    -> copy/move candidate locally
    -> release pending mutex
    -> bind_external_swapchain()
```

Never call active mutation while pending mutex is held.

## Immediate live path

```text
lifecycle mutex held
    -> no pending mutex required
    -> on_reset / bind / rebind
```

This avoids new lock inversion.

---

# 25. Lifecycle Mutex Contract Must Not Be Replaced

The current race protection relies on:

```cpp
g_framework->get_hook_monitor_mutex()
```

Do not introduce a second “XeFG lifecycle mutex” in R5.

That would create two authorities over hook creation/replacement.

The pending mutex protects only pending storage.

The REFramework hook-monitor mutex remains the authority for:

- current `g_d3d12_hook` stability;
- hook-monitor recovery replacement;
- immediate XeFG delivery vs no-hook pending publication boundary.

---

# 26. Constructor-Time Fallback Must Stay

The code path where:

```cpp
g_framework == nullptr
```

is not theoretical cleanup noise. Current code explicitly supports it.

R5 must keep:

```cpp
if (g_framework == nullptr) {
    store_pending(std::move(candidate));
    return;
}
```

Do not spin waiting for `g_framework`.

Do not sleep.

Do not create a thread.

Do not invoke REFramework construction from the compatibility layer.

---

# 27. Candidate Lifetime Through Handoff

R4 candidate provides strong COM ownership.

R5 must preserve candidate lifetime across each delivery mode.

## Pending mode

```text
s_pending_candidate
    owns swapchain
    owns selected queue
    owns validated device
```

until consumed or replaced.

## Consume mode

Move pending candidate to a local variable before clearing shared state:

```cpp
pending = std::move(s_pending_candidate);
s_pending_candidate.reset();
```

The local candidate then keeps objects alive through the call to existing bind logic.

## Immediate mode

The `publish()` parameter/local candidate remains alive through:

```text
identity comparison
on_reset if needed
bind_external_swapchain / replace_xefg_binding
```

Do not pass raw pointers obtained from a temporary candidate that is destroyed before the bind method returns.

---

# 28. R5 Must Not Start R6 Active Ownership

It may be tempting to make `XeFGCandidateHandoff` retain the most recently applied live candidate after successful bind.

Do not.

After immediate or consumed bind succeeds:

```text
XeFGCandidateHandoff
    should not become active binding owner
```

Current `D3D12Hook` active fields remain authoritative:

```cpp
m_xefg_bound_swapchain
m_xefg_bound_queue
m_xefg_bound_device
m_xefg_binding_generation
m_xefg_p21_observe_only
```

R6 will move semantic active ownership deliberately.

R5 only owns candidates while they are **in transit / pending**.

---

# 29. Active Bind/Rebind Methods Are Black Boxes in R5

Treat these as existing behavior:

```cpp
bind_external_swapchain(...)
replace_xefg_binding(...)
```

R5 may invoke them but must not alter:

- input validation;
- device acquisition;
- VtableHook slot set;
- renderer reset ordering inside rebind;
- old hook removal ordering;
- strong active COM lifetime;
- generation increments;
- resize hold clear behavior;
- failure logging.

Any material edit inside these methods is R7 leakage unless strictly required to compile a narrow friend bridge.

---

# 30. PR #20 MHW-Only Resize Policy Remains Frozen

Latest master still intentionally limits ResizeTarget transition hold to Monster Hunter Wilds.

R5 must not change the condition equivalent to:

```cpp
event_id != 0
&& renderer_reset_performed
&& !m_xefg_p21_observe_only
&& sdk::GameIdentity::get().is_mhwilds()
```

No edits to:

- `arm_xefg_resize_transition_hold()`;
- `complete_xefg_resize_transition_hold()`;
- `clear_xefg_resize_transition_hold()`;
- Present suppression during the hold;
- resize callback ordering.

If the R5 diff touches these regions, treat it as scope leakage.

---

# 31. Build-System Requirement for the New R5 Files

`cmake.toml` already has recursive source/header globs:

```toml
[target.REFramework]
sources = ["src/**.cpp", "src/**.c"]
headers = ["src/**.hpp", "src/**.h"]
```

However, this fork's CI also depends on the tracked generated `CMakeLists.txt` explicit source list.

PR #19 demonstrated that new compatibility `.cpp/.hpp` files must be registered there.

If R5 adds:

```text
src/compatibility/xefg/XeFGCandidateHandoff.cpp
src/compatibility/xefg/XeFGCandidateHandoff.hpp
```

then add only these entries to tracked `CMakeLists.txt`, adjacent to the other XeFG files.

Example:

```cmake
"src/compatibility/xefg/XeFGCompatibility.cpp"
"src/compatibility/xefg/XeFGCompatibility.hpp"
"src/compatibility/xefg/XeFGCandidateHandoff.cpp"
"src/compatibility/xefg/XeFGCandidateHandoff.hpp"
"src/compatibility/xefg/XeFGDiscovery.cpp"
"src/compatibility/xefg/XeFGDiscovery.hpp"
"src/compatibility/xefg/XeFGRuntimeRegistry.cpp"
"src/compatibility/xefg/XeFGRuntimeRegistry.hpp"
```

Do not modify `cmake.toml` for this addition.

Do not broadly regenerate `CMakeLists.txt` and create unrelated generated-file churn.

---

# 32. Expected File Set

Preferred R5 diff:

```text
ADD    src/compatibility/xefg/XeFGCandidateHandoff.hpp
ADD    src/compatibility/xefg/XeFGCandidateHandoff.cpp
MODIFY src/D3D12Hook.cpp
MODIFY src/D3D12Hook.hpp
MODIFY CMakeLists.txt
```

Normally unchanged:

```text
src/REFramework.cpp
src/REFramework.hpp
src/compatibility/xefg/XeFGCompatibility.*
src/compatibility/xefg/XeFGDiscovery.*
src/compatibility/xefg/XeFGRuntimeRegistry.*
cmake.toml
src/mods/*
shared/*
```

If `XeFGDiscovery` must be modified only to support moving the candidate type, stop and reconsider. The R4 type is already usable by R5.

Do not create a generic shared compatibility-types hierarchy in this PR.

---

# 33. Expected D3D12Hook Surface After R5

The desired direction is that `D3D12Hook` no longer knows how a candidate waits for a hook to exist.

Conceptually:

```text
D3D12Hook
    knows:
        how to bind a supplied external swapchain
        how to rebind an active XeFG instance
        physical Present/resize hooks

XeFGCandidateHandoff
    knows:
        whether a live hook currently exists under lifecycle protection
        whether to apply immediately or store pending
        how to consume one pending candidate at hook creation
```

This removes pending-candidate policy from an upstream-sensitive file without moving physical D3D12 hook ownership yet.

---

# 34. Forbidden R5 Changes

Do not introduce any of the following:

- generic `IFrameGenerationProvider`;
- generic candidate event bus;
- `std::function` subscriber system;
- background worker;
- polling loop;
- sleep/retry logic;
- candidate queue/history;
- module-based candidate routing;
- public XeFG proxy as fallback render target;
- FSRFG changes;
- DLSSG/Streamline changes;
- D3D11 changes;
- new Release hook;
- VtableHook library replacement;
- active `XeFGBinding` state object;
- bind/rebind rewrite;
- resize lifecycle extraction;
- hook-monitor cleanup;
- Debug Logging UI;
- log-level cleanup;
- unrelated upstream REFramework modernization.

---

# 35. Failure Semantics Matrix

R5 review should compare behavior against this table.

| Situation | Required R5 behavior |
|---|---|
| R4 candidate rejected | handoff not called |
| framework null | overwrite pending slot with candidate |
| framework exists, hook null | under lifecycle mutex, overwrite pending slot before unlock |
| hook exists, active XeFG unchanged | log unchanged, no bind/rebind call, no pending store |
| hook exists, active XeFG changed | call existing rebind once |
| active XeFG rebind fails | log failure, do not queue retry |
| hook exists, active non-XeFG | reset renderer once, call existing external bind |
| external bind fails | log failure, do not queue retry |
| pending exists when `hook()` starts | remove pending from shared slot, bind once |
| pending bind succeeds | `hook()` returns XeFG-hooked success path |
| pending bind fails | pending stays cleared; `hook()` continues normal D3D12 path |
| two candidates arrive before hook | latest candidate replaces previous pending candidate |

Any deviation should be justified as a separate behavior change and therefore is normally out of R5 scope.

---

# 36. Static Review Checklist

Reviewer should verify all of the following.

## Ownership

- [ ] `PendingXefgBinding` removed from `D3D12Hook.cpp`.
- [ ] pending storage lives in the XeFG handoff component.
- [ ] pending storage uses R4 `XeFGBindingCandidate` directly or an exactly justified equivalent.
- [ ] R5 component does not retain successful live bindings.

## Lifecycle race

- [ ] live hook pointer is read only after lifecycle mutex acquisition.
- [ ] if live hook is null, pending publication happens before lifecycle mutex release.
- [ ] no check-unlock-store gap exists.
- [ ] no background worker replaces synchronous handoff.

## Pending mutex

- [ ] pending mutex protects only pending slot.
- [ ] pending mutex is released before bind/rebind/reset.
- [ ] consume removes pending before binding.
- [ ] pending replacement remains single-slot last-wins.

## Active behavior

- [ ] unchanged active identity remains swapchain + queue + mode.
- [ ] unchanged active candidate does not trigger rebind.
- [ ] changed active XeFG candidate calls existing `replace_xefg_binding()`.
- [ ] active non-XeFG path resets renderer before existing external bind.
- [ ] bind/rebind failures are not automatically requeued.

## Scope

- [ ] no R6 active ownership object.
- [ ] no R7 mutation-protocol rewrite.
- [ ] no resize/Present changes.
- [ ] no MHW-only hold changes.
- [ ] no Streamline/FSRFG/D3D11 changes.

## Build

- [ ] new files added to tracked `CMakeLists.txt`.
- [ ] `cmake.toml` unchanged.
- [ ] no broad generated CMake churn.

---

# 37. Mandatory Build / Static Validation

Run from latest master + R5 branch:

```text
cmake -S . -B build
cmake --build build --config Release --target REFramework
```

Also run:

```text
git diff --check
```

Audit changed files:

```text
Expected:
    CMakeLists.txt
    src/D3D12Hook.cpp
    src/D3D12Hook.hpp
    src/compatibility/xefg/XeFGCandidateHandoff.cpp
    src/compatibility/xefg/XeFGCandidateHandoff.hpp
```

Unexpected functional changes should be removed.

A successful local build is insufficient if the new `.cpp` is absent from tracked `CMakeLists.txt`.

---

# 38. Recommended Focused Tests / Reasoning Checks

There may not be an existing unit-test harness for this exact native lifecycle code. At minimum, reason explicitly through the following sequences and document the result in the PR body.

## Sequence 1 — Candidate before hook

```text
publish A
framework exists
current hook == null
store A while lifecycle lock held
later D3D12Hook::hook()
consume A
bind succeeds
```

Expected:

```text
no native dummy path before pending bind
no lost candidate
```

## Sequence 2 — Two candidates before hook

```text
publish A -> pending A
publish B -> pending B
hook starts -> consumes B
```

Expected:

```text
A released
B bound once
```

## Sequence 3 — Candidate while live native hook exists

```text
publish candidate
lifecycle lock
live hook found
renderer reset once
external XeFG bind once
no pending value created
```

## Sequence 4 — Candidate while same XeFG binding already active

```text
same swapchain
same queue
same mode
```

Expected:

```text
BindingGate unchanged
no renderer reset
no rebind
no pending publication
```

## Sequence 5 — Changed active XeFG candidate

```text
swapchain or queue or mode changed
```

Expected:

```text
existing replace_xefg_binding called once
R7 protocol untouched
```

## Sequence 6 — Pending bind failure

```text
consume pending
pending slot cleared
bind_external_swapchain returns false
```

Expected:

```text
consume returns false
D3D12Hook::hook() continues existing native setup
candidate not requeued
```

---

# 39. Runtime Validation Position

The fine-grained plan does not require a full dedicated runtime wave after R5; the next major binding wave is after R7.

However, if runtime testing is convenient, a short DD2 + OptiScaler + XeFG launch smoke is useful because R5 touches initial delivery timing.

Optional smoke expectations:

```text
DD2 + XeFG
    game launches
    XeFG initializes
    REFramework overlay appears
    OptiScaler overlay appears
    no repeated bind loop
    no missed initial XeFG binding
```

Do not expand R5 implementation scope to fix unrelated runtime findings. If a failure is found, first determine whether it is an R5 handoff regression or a pre-existing active binding issue.

---

# 40. Blocking Review Findings

Request changes for any of the following:

1. Candidate can be lost between `hook == nullptr` check and pending publication.
2. Current hook pointer is read before lifecycle mutex acquisition and then used afterward.
3. Pending mutex is held while calling bind/rebind/reset.
4. Pending candidate is automatically retried/requeued on bind failure.
5. Multiple pending candidates are queued instead of latest-one replacement.
6. `bind_external_swapchain()` or `replace_xefg_binding()` behavior is materially rewritten.
7. R6 active ownership fields are moved prematurely.
8. R7 physical VtableHook transaction is redesigned.
9. MHW-only resize hold behavior changes.
10. New source file is omitted from tracked `CMakeLists.txt`.
11. `g_d3d12_hook` is exposed as a new broad global API.
12. Generic polling/event/provider infrastructure is introduced.

---

# 41. Non-Blocking / Theoretical Findings

Do not block R5 solely for:

- naming preference between `publish`, `deliver`, or `handoff`;
- private helper layout;
- whether fixed log strings use a small helper;
- minor include ordering;
- an extra defensive `candidate.valid()` assertion that cannot change valid-input behavior;
- theoretical multi-thread timing with no concrete path under the existing lifecycle contract;
- style cleanup unrelated to handoff correctness.

Review according to the project rule:

> Block realistic, materially harmful defects. Classify speculative/pathological concerns as non-blocking or theoretical.

---

# 42. Expected Size

Fine-grained plan estimate:

```text
effective implementation: ~100–180 LOC
GitHub add+delete:          ~180–320 LOC
```

Because a new component is added and equivalent logic is deleted from `D3D12Hook.cpp`, GitHub add+delete may be somewhat higher without representing a larger semantic change.

Do not artificially combine R6 because R5 looks small.

One ownership change per PR remains the rule.

---

# 43. Suggested PR Description

```markdown
## Summary

- extract XeFG validated-candidate lifecycle handoff from `D3D12Hook`
- move the single pending candidate slot and synchronization into `XeFGCandidateHandoff`
- preserve immediate live-hook bind/rebind behavior under the existing hook-monitor lifecycle mutex
- preserve capture-before-hook pending delivery without a lost-candidate window
- consume the same pending candidate at the existing early point in `D3D12Hook::hook()`
- keep active bind/rebind implementation and presentation lifecycle unchanged

## Critical invariants

- current hook pointer is read only under the existing lifecycle mutex
- no-hook pending publication occurs before that lifecycle mutex is released
- pending storage remains single-slot / latest-candidate-wins
- pending mutex is never held during active bind/rebind/reset
- failed immediate or pending bind is not automatically retried
- R6/R7 active binding behavior is unchanged

## Not in scope

- active XeFG binding-state extraction (R6)
- initial bind/rebind protocol rewrite (R7)
- resize lifecycle extraction (R8)
- Present/resize callback cleanup (R9)
- hook-monitor isolation (R10)
- logging cleanup / Debug Logging UI

## Validation

- Release build: PASS
- `git diff --check`: PASS
- tracked CMake source registration: PASS
- lifecycle/pending race audit: PASS
```

---

# 44. Completion Criteria

R5 is complete only when all are true:

```text
[ ] latest master used as base
[ ] PendingXefgBinding removed from D3D12Hook implementation
[ ] pending state owned by XeFGCandidateHandoff
[ ] R4 XeFGBindingCandidate reused for handoff
[ ] current hook read occurs under lifecycle mutex
[ ] hook-null -> pending store is atomic with respect to lifecycle mutex
[ ] constructor-time framework-null fallback preserved
[ ] pending slot is last-wins, not a queue
[ ] consume clears shared pending before bind
[ ] pending mutex released before bind/rebind/reset
[ ] immediate active-XeFG unchanged/rebind gate preserved
[ ] active non-XeFG reset + external bind preserved
[ ] failed bind/rebind does not create retry behavior
[ ] bind_external_swapchain implementation unchanged
[ ] replace_xefg_binding implementation unchanged
[ ] active COM ownership fields unchanged
[ ] MHW-only resize hold unchanged
[ ] new source files registered in tracked CMakeLists.txt
[ ] cmake.toml unchanged
[ ] Release build passes
[ ] git diff --check passes
[ ] no unrelated cleanup
```

---

# 45. Stop Condition / Handoff to R6

Stop R5 as soon as candidate delivery ownership has moved and behavior matches current master.

Do **not** continue into active binding cleanup just because the handoff component now has a validated candidate.

The intended boundary after R5 is:

```text
XeFGDiscovery
    -> validated strong candidate

XeFGCandidateHandoff
    -> immediate delivery OR one pending slot

D3D12Hook
    -> existing active bind/rebind implementation
    -> existing strong active COM fields
```

Then R6 can separately introduce the active XeFG binding-state object:

```text
m_xefg_bound_swapchain
m_xefg_bound_queue
m_xefg_bound_device
binding generation
mode / identity / health
```

That separation is intentional and should be preserved.
