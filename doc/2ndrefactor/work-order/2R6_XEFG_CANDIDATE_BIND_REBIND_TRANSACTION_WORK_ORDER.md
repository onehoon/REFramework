# Work Order — 2R6: Consolidate XeFG Candidate Apply / Bind / Rebind Into One Session-Directed Transaction

Date: 2026-09-09  
Repository: `onehoon/REFramework`  
PR base branch: `REFforXeFG`  
Planning baseline reviewed: `REFforXeFG` @ `e426fba2f5e3a87426e0e6f6f05838710f96deb9`  
Architecture authority: `doc/2ndrefactor/REFramework_OPTISCALER_XEFG_SECOND_STAGE_REFACTOR_DESIGN_2026-09-09.md`  
Previous stage: conceptual 2R5 completed through PR #46

---

## 1. Objective

Implement conceptual **2R6 — Consolidate XeFG Candidate Apply / Bind / Rebind Transaction Around Session Authority**.

This is the first second-stage PR that is not primarily a simple state extraction. It changes the internal transaction shape, so the implementation must be substantially more conservative than 2R3–2R5.

The goal is:

> Route every XeFG binding candidate through one semantic planning path owned by `XeFGPresentationSession`, then have `D3D12Hook` execute exactly one physical transaction according to that plan. Semantic binding mutation must occur only after all required physical preconditions have been prepared successfully.

The PR must remove the current split where:

```text
live candidate
    -> XeFGCandidateHandoff::apply_to_live_hook()
    -> D3D12Hook::apply_xefg_candidate()

pending candidate
    -> XeFGCandidateHandoff::consume_pending()
    -> D3D12Hook::bind_external_swapchain(... XeFGInternal ...)
```

Both live and pending candidates must converge on the same candidate transaction entry.

Likewise, a direct `bind_external_swapchain(..., SwapchainSource::XeFGInternal, ...)` request must not retain a second independent XeFG semantic mutation path.

This remains a **behavior-preserving compatibility refactor**. Do not treat 2R6 as an opportunity to redesign native D3D12, generic external binding, monitor recovery, or XeFG ownership.

---

## 2. Why 2R6 Is One Atomic PR

The current code has three closely coupled XeFG mutation paths:

```text
1. initial XeFG bind
2. same-swapchain queue/mode update
3. changed-swapchain replacement
```

Splitting semantic planning and physical transaction wiring across separate mergeable PRs would temporarily create two authorities and increase the chance that one candidate path commits with different generation/reset/hold semantics.

For that reason, implement 2R6 as one focused PR with:

```text
session plan
-> physical preparation
-> destructive physical phase
-> explicit session commit
-> physical publication / alias synchronization
-> semantic post-commit cleanup
```

Do not begin 2R7 cleanup beyond dead code that becomes directly unreachable because 2R6 eliminates the duplicate XeFG path.

---

## 3. Current Code Reviewed

Baseline: `e426fba2f5e3a87426e0e6f6f05838710f96deb9`.

### 3.1 Current live candidate path

`D3D12Hook::apply_xefg_candidate()` currently does:

```cpp
bool D3D12Hook::apply_xefg_candidate(const XeFGBindingCandidate& candidate) {
    if (candidate.swapchain == nullptr || candidate.selected_queue == nullptr) {
        return false;
    }

    if (!m_hooked
        || m_swapchain_source != SwapchainSource::XeFGInternal
        || !m_xefg_session.binding().active()) {
        return bind_external_swapchain(
            candidate.swapchain.Get(),
            candidate.selected_queue.Get(),
            SwapchainSource::XeFGInternal,
            candidate.observe_only,
            candidate.runtime);
    }

    const auto change = m_xefg_session.binding().compare(
        candidate.swapchain.Get(),
        candidate.selected_queue.Get(),
        candidate.observe_only);

    if (!change.changed()) {
        m_xefg_session.binding().refresh_runtime_identity(candidate.runtime);
        m_xefg_session.detached_state() = {};
        clear_xefg_monitor_state();
        return true;
    }

    return replace_xefg_binding(
        candidate.swapchain.Get(),
        candidate.selected_queue.Get(),
        candidate.observe_only,
        change.reason(),
        candidate.runtime);
}
```

### 3.2 Current pending candidate path is different

`XeFGCandidateHandoff::consume_pending()` currently bypasses `apply_xefg_candidate()`:

```cpp
return hook.bind_external_swapchain(
    pending->swapchain.Get(),
    pending->selected_queue.Get(),
    D3D12Hook::SwapchainSource::XeFGInternal,
    pending->observe_only,
    pending->runtime);
```

This means live and pending candidates do not enter the same semantic classification path.

2R6 must remove this split.

### 3.3 Current same-object vs changed-object branch

`apply_xefg_candidate()` first compares the **semantic** binding, but `replace_xefg_binding()` decides whether the hook can be retained using the **physical renderer alias**:

```cpp
const auto change = m_xefg_session.binding().compare(...);

// later in replace_xefg_binding()
auto* const old_swapchain = m_swap_chain;
const auto same_swapchain = old_swapchain == swapchain;

if (same_swapchain) {
    // retain existing VtableHook
    ...
}
```

This distinction matters.

Do not casually replace it with:

```cpp
m_xefg_session.binding().swapchain() == candidate.swapchain.Get()
```

unless the transaction plan receives the physical renderer swapchain and intentionally preserves the current branch semantics.

### 3.4 Current changed-swapchain preparation ordering

`replace_xefg_binding()` currently prepares the new swapchain hook completely before destructive work:

```cpp
std::unique_ptr<VtableHook> next_hook;
try {
    next_hook = std::make_unique<VtableHook>(Address{next_swapchain.Get()});

    const auto present_ok = next_hook->hook_method(8, ...);
    const auto present1_ok = next_hook->hook_method(22, ...);
    const auto resize_buffers_ok = next_hook->hook_method(13, ...);
    const auto resize_target_ok = next_hook->hook_method(14, ...);
    const auto resize_buffers1_ok = next_hook->hook_method(39, ...);

    if (!(present_ok
        && present1_ok
        && resize_buffers_ok
        && resize_target_ok
        && resize_buffers1_ok)) {
        return false;
    }
} catch (...) {
    return false;
}

// only after preparation succeeds:
g_framework->on_reset();
m_present_hook.reset();
m_swapchain_hook.reset();
...
```

This fail-before-destruction property is mandatory.

### 3.5 Active swapchain ownership is borrowed

Current `XeFGBinding` ownership must remain:

```cpp
IDXGISwapChain3* m_swapchain{}; // borrowed
Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue{};
Microsoft::WRL::ComPtr<ID3D12Device4> m_device{};
```

A local `ComPtr<IDXGISwapChain3>` may temporarily keep a candidate or old active swapchain alive through hook removal, but the committed active binding must remain borrowed.

---

## 4. Non-Negotiable Ownership / Locking / Lifecycle Invariants

### 4.1 COM ownership

Preserve exactly:

```text
candidate swapchain            = strong while candidate/transaction local exists
active XeFG swapchain          = borrowed raw pointer
active queue                   = strong ComPtr
active device                  = strong ComPtr
old swapchain during teardown  = bounded local ComPtr only
```

Do not:

- store a persistent `ComPtr<IDXGISwapChain3>` in `XeFGBinding`;
- introduce Release-until-refcount loops;
- inspect or drain COM refcounts;
- keep the old swapchain alive after the transaction returns.

### 4.2 Candidate-handoff lock order

Preserve:

```text
hook-monitor/lifecycle mutex
    -> pending-candidate mutex
```

Never invert it.

`consume_pending()` must continue to:

```text
lock pending mutex
-> move candidate out
-> clear optional
-> unlock pending mutex
-> apply candidate
```

Do not apply a candidate while `s_pending_mutex` is held.

Do not allow candidate COM destruction to occur while `s_pending_mutex` is held if that destruction could re-enter XeFG code.

### 4.3 Runtime-transition behavior

Do not change:

```text
lifecycle lock
-> discard matching pending
-> detach matching active binding
-> unlock
-> Intel Init/Destroy call
```

2R6 does not modify runtime transition matching or Destroy reconciliation.

### 4.4 Physical hook ownership

Keep these in `D3D12Hook`:

```text
m_present_hook
m_swapchain_hook
VtableHook construction
hook_method calls
hook reset/destruction
renderer reset callback execution
raw renderer-facing alias fields
```

`XeFGPresentationSession` must not own `VtableHook` or `PointerHook`.

---

## 5. Target Transaction Classification

Add one XeFG-specific semantic plan to `XeFGPresentationSession`.

Recommended enum:

```cpp
enum class CandidateDisposition : uint8_t {
    Reject,
    NoActiveBinding,
    Identical,
    SameSwapchainUpdate,
    ChangedSwapchainReplacement,
};
```

Recommended plan:

```cpp
struct CandidatePlan {
    CandidateDisposition disposition{CandidateDisposition::Reject};
    const char* reason{"candidate_invalid"};
    XeFGBinding::RuntimeLifecycleSnapshot previous{};
};
```

Use a narrow session API conceptually similar to:

```cpp
CandidatePlan plan_candidate(
    const PhysicalBindingView& physical,
    IDXGISwapChain3* candidate_swapchain,
    ID3D12CommandQueue* candidate_queue,
    bool candidate_observe_only) const noexcept;
```

Reusing the existing `PhysicalBindingView` is acceptable because the current routing depends on both semantic binding and physical D3D12 state.

Do not make the session inspect `VtableHook` itself. `D3D12Hook` supplies the physical view.

---

## 6. Required Planning Semantics

The plan must preserve the current entry-routing behavior, including edge cases.

A recommended implementation is:

```cpp
XeFGPresentationSession::CandidatePlan
XeFGPresentationSession::plan_candidate(
    const PhysicalBindingView& physical,
    IDXGISwapChain3* candidate_swapchain,
    ID3D12CommandQueue* candidate_queue,
    bool candidate_observe_only) const noexcept {

    CandidatePlan plan{};
    plan.previous = m_binding.lifecycle_snapshot();

    if (candidate_swapchain == nullptr || candidate_queue == nullptr) {
        plan.disposition = CandidateDisposition::Reject;
        plan.reason = "candidate_invalid";
        return plan;
    }

    // Preserve the current apply_xefg_candidate() route exactly:
    // replacement logic is only used when physical XeFG hook state and
    // semantic binding are both active.
    const bool active_xefg_path = physical.hook_active
        && physical.xefg_source
        && plan.previous.active;

    if (!active_xefg_path) {
        plan.disposition = CandidateDisposition::NoActiveBinding;
        plan.reason = "no_active_binding";
        return plan;
    }

    const auto change = m_binding.compare(
        candidate_swapchain,
        candidate_queue,
        candidate_observe_only);

    if (!change.changed()) {
        plan.disposition = CandidateDisposition::Identical;
        plan.reason = "identical";
        return plan;
    }

    // Current replace_xefg_binding() rejects a changed candidate if the
    // active physical instance hook is unavailable.
    if (!physical.swapchain_hook_present || physical.renderer_swapchain == nullptr) {
        plan.disposition = CandidateDisposition::Reject;
        plan.reason = "active_physical_binding_unavailable";
        return plan;
    }

    plan.reason = change.reason();

    // IMPORTANT: preserve the current physical same-object test.
    plan.disposition = physical.renderer_swapchain == candidate_swapchain
        ? CandidateDisposition::SameSwapchainUpdate
        : CandidateDisposition::ChangedSwapchainReplacement;

    return plan;
}
```

The exact names may differ, but these semantics are important.

### Why physical `renderer_swapchain` is used for SameSwapchainUpdate

Current code decides hook retention using:

```cpp
m_swap_chain == candidate_swapchain
```

not solely the semantic `XeFGBinding::swapchain()` pointer.

Do not silently change that behavior during 2R6.

---

## 7. Identical Candidate Must Remain Non-Destructive

Current identical behavior is:

```text
no GetDevice requirement
no new hook preparation
no renderer reset
no hook reset
no generation increment
refresh runtime identity
clear detached uncertainty
clear monitor state
keep resize hold state unchanged
keep first-render-boundary state unchanged
```

Preserve all of it.

Recommended explicit session commit:

```cpp
struct CandidateCommitResult {
    bool committed{};
    uint64_t generation{};
    bool clear_resize_hold{};
    const char* resize_hold_reason{};
};

CandidateCommitResult commit_identical_candidate(
    const CandidatePlan& plan,
    XeFGBinding::RuntimeIdentity runtime) noexcept {

    if (plan.disposition != CandidateDisposition::Identical) {
        return {};
    }

    m_binding.refresh_runtime_identity(runtime);
    m_detached_state = {};
    clear_monitor_state();

    return {
        true,
        m_binding.generation(),
        false,
        nullptr,
    };
}
```

Do **not** reset `m_render_boundary_logged` for an identical candidate.

Do **not** clear the resize hold for an identical candidate.

Do **not** increment binding generation.

---

## 8. Non-Identical Candidates Must Revalidate the Device Before Destructive Work

For:

```text
NoActiveBinding
SameSwapchainUpdate
ChangedSwapchainReplacement
```

preserve the current physical validation:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> next_swapchain = candidate_swapchain;
Microsoft::WRL::ComPtr<ID3D12CommandQueue> next_queue = candidate_queue;
Microsoft::WRL::ComPtr<ID3D12Device4> next_device;

if (FAILED(next_swapchain->GetDevice(IID_PPV_ARGS(&next_device)))) {
    return false;
}
```

`XeFGBindingCandidate` already contains a `device` field, but do not simply substitute it for this current `GetDevice()` validation in 2R6.

That would change the physical validation contract.

The candidate's strong `device` may remain a discovery artifact; the active transaction still obtains the device from the candidate swapchain as current code does.

---

## 9. Centralize Five-Method XeFG Hook Preparation

The current initial-bind and changed-replacement paths both prepare the same five methods but duplicate the code.

Create one XeFG-specific physical helper in `D3D12Hook`.

Recommended shape:

```cpp
struct XeFGHookPreparation {
    std::unique_ptr<VtableHook> hook{};
    const char* failure_reason{};

    bool ready() const noexcept {
        return hook != nullptr;
    }
};

XeFGHookPreparation prepare_xefg_instance_hook(IDXGISwapChain3* swapchain);
```

Conceptual implementation:

```cpp
D3D12Hook::XeFGHookPreparation
D3D12Hook::prepare_xefg_instance_hook(IDXGISwapChain3* swapchain) {
    XeFGHookPreparation result{};

    if (swapchain == nullptr) {
        result.failure_reason = "candidate_swapchain_null";
        return result;
    }

    try {
        auto hook = std::make_unique<VtableHook>(Address{swapchain});

        const auto present_ok = hook->hook_method(
            8, Address{reinterpret_cast<void*>(&D3D12Hook::present)});
        const auto present1_ok = hook->hook_method(
            22, Address{reinterpret_cast<void*>(&D3D12Hook::present1)});
        const auto resize_buffers_ok = hook->hook_method(
            13, Address{reinterpret_cast<void*>(&D3D12Hook::resize_buffers)});
        const auto resize_target_ok = hook->hook_method(
            14, Address{reinterpret_cast<void*>(&D3D12Hook::resize_target)});
        const auto resize_buffers1_ok = hook->hook_method(
            39, Address{reinterpret_cast<void*>(&D3D12Hook::resize_buffers1)});

        if (!(present_ok
            && present1_ok
            && resize_buffers_ok
            && resize_target_ok
            && resize_buffers1_ok)) {
            result.failure_reason = "new_hook_method_failed";
            return result;
        }

        result.hook = std::move(hook);
        return result;
    } catch (...) {
        result.failure_reason = "new_hook_create_failed";
        return result;
    }
}
```

The exact helper type may differ.

Mandatory slot set:

```text
Present[8]
Present1[22]
ResizeBuffers[13]
ResizeTarget[14]
ResizeBuffers1[39]
```

### Preparation rule by disposition

```text
Identical                     -> no new hook
SameSwapchainUpdate           -> no new hook
NoActiveBinding               -> prepare all five before destructive work
ChangedSwapchainReplacement   -> prepare all five before destructive work
Reject                        -> no work
```

If hook preparation fails:

```text
old renderer state unchanged
old m_present_hook unchanged
old m_swapchain_hook unchanged
old semantic binding unchanged
old raw aliases unchanged
old resize hold unchanged
```

A local candidate hook object may clean itself up on failure; the active old binding must not be touched.

---

## 10. Session Commit for Prepared Candidates

After physical preconditions succeed, semantic mutation should be explicit and centralized in the session.

Recommended API:

```cpp
CandidateCommitResult commit_prepared_candidate(
    const CandidatePlan& plan,
    IDXGISwapChain3* candidate_swapchain,
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue,
    Microsoft::WRL::ComPtr<ID3D12Device4> device,
    bool observe_only,
    XeFGBinding::RuntimeIdentity runtime) noexcept;
```

Recommended behavior:

```cpp
XeFGPresentationSession::CandidateCommitResult
XeFGPresentationSession::commit_prepared_candidate(
    const CandidatePlan& plan,
    IDXGISwapChain3* candidate_swapchain,
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue,
    Microsoft::WRL::ComPtr<ID3D12Device4> device,
    bool observe_only,
    XeFGBinding::RuntimeIdentity runtime) noexcept {

    CandidateCommitResult result{};

    switch (plan.disposition) {
    case CandidateDisposition::NoActiveBinding:
        // Preserve current initial-bind generation semantics even if stale
        // incomplete/old semantic state exists.
        m_binding.clear();
        m_binding.commit_initial(
            candidate_swapchain,
            std::move(queue),
            std::move(device),
            observe_only,
            runtime);
        result.clear_resize_hold = true;
        result.resize_hold_reason = "external_bind";
        break;

    case CandidateDisposition::SameSwapchainUpdate:
        m_binding.commit_same_swapchain_update(
            std::move(queue),
            std::move(device),
            observe_only,
            runtime);
        result.clear_resize_hold = true;
        result.resize_hold_reason = "binding_replaced";
        break;

    case CandidateDisposition::ChangedSwapchainReplacement:
        m_binding.commit_replacement(
            candidate_swapchain,
            std::move(queue),
            std::move(device),
            observe_only,
            runtime);
        result.clear_resize_hold = true;
        result.resize_hold_reason = "binding_replaced";
        break;

    default:
        return result;
    }

    m_render_boundary_logged = false;
    m_detached_state = {};
    clear_monitor_state();

    result.committed = true;
    result.generation = m_binding.generation();
    return result;
}
```

Do not perform physical hook ownership changes in this method.

Do not put `g_framework->on_reset()` in the session.

Do not clear the resize hold inside this method if doing so would bypass the existing D3D12Hook logging bridge. Returning the existing reason is preferable.

---

## 11. Generation Semantics Must Remain Exact

Preserve:

```text
NoActiveBinding / initial commit
    -> generation = 1

Identical
    -> generation unchanged

SameSwapchainUpdate
    -> generation += 1

ChangedSwapchainReplacement
    -> generation += 1

binding clear/unhook
    -> generation = 0
```

Do not change generation to a global monotonic counter.

Do not increment generation merely because runtime identity refreshed.

---

## 12. Recommended Unified D3D12 Transaction Entry

Create one internal XeFG transaction orchestrator in `D3D12Hook`.

Recommended concept:

```cpp
bool apply_xefg_binding_request(
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* command_queue,
    bool observe_only,
    XeFGBinding::RuntimeIdentity runtime);
```

`apply_xefg_candidate()` should become a narrow public compatibility bridge:

```cpp
bool D3D12Hook::apply_xefg_candidate(const XeFGBindingCandidate& candidate) {
    if (candidate.swapchain == nullptr || candidate.selected_queue == nullptr) {
        return false;
    }

    return apply_xefg_binding_request(
        candidate.swapchain.Get(),
        candidate.selected_queue.Get(),
        candidate.observe_only,
        candidate.runtime);
}
```

The transaction entry should:

```text
1. ask session for CandidatePlan
2. Reject -> return false
3. Identical -> session runtime-only commit -> return true
4. for all other plans, create local strong next swapchain/queue
5. GetDevice from next swapchain
6. if NoActiveBinding or ChangedSwapchainReplacement, prepare all five new hooks
7. after successful preparation, capture old bounded keepalive as required
8. execute renderer reset at the same current point
9. remove old physical hooks only for plans that currently remove them
10. commit semantic binding through session
11. mirror semantic aliases into D3D12Hook
12. publish prepared hook if required
13. update physical source/hooked/phase flags exactly as current flow
14. clear resize hold with the existing reason returned by commit
15. emit existing bind/rebind diagnostics
```

---

## 13. Detailed Transaction Matrix

### 13.1 Reject

```text
CandidatePlan::Reject
    -> no GetDevice required
    -> no renderer reset
    -> no hook preparation/removal
    -> no semantic mutation
    -> return false
```

### 13.2 Identical

```text
CandidatePlan::Identical
    -> refresh runtime identity only
    -> detached state cleared
    -> monitor state cleared
    -> generation unchanged
    -> existing hook retained
    -> renderer not reset
    -> resize hold unchanged
    -> first-render-boundary flag unchanged
    -> return true
```

### 13.3 SameSwapchainUpdate

Current behavior to preserve:

```text
candidate queue/mode changed
AND physical m_swap_chain == candidate swapchain
    -> GetDevice before destructive work
    -> create bounded old swapchain keepalive as current path does
    -> renderer reset
    -> DO NOT remove existing VtableHook
    -> semantic commit_same_swapchain_update
    -> sync aliases
    -> render-boundary flag reset false
    -> detached cleared
    -> monitor cleared
    -> resize hold clear("binding_replaced")
    -> generation += 1
```

Recommended physical branch:

```cpp
if (plan.disposition == CandidateDisposition::SameSwapchainUpdate) {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> old_keepalive;
    if (plan.previous.active && m_swap_chain != nullptr) {
        old_keepalive = m_swap_chain;
    }

    spdlog::info(
        "[XeFG][Rebind] stage = old_renderer_reset, reason = {}, generation = {}",
        plan.reason,
        plan.previous.generation);

    g_framework->on_reset();

    const auto commit = m_xefg_session.commit_prepared_candidate(
        plan,
        swapchain,
        std::move(next_queue),
        std::move(next_device),
        observe_only,
        runtime);

    if (!commit.committed) {
        return false; // should be structurally unreachable after a valid plan
    }

    sync_xefg_binding_aliases();

    if (commit.clear_resize_hold) {
        clear_xefg_resize_transition_hold(commit.resize_hold_reason);
    }

    return true;
}
```

Do not recreate the hook for a same-object update.

### 13.4 ChangedSwapchainReplacement

Required ordering:

```text
plan changed replacement
-> GetDevice
-> prepare all five new hooks
-> bounded old swapchain keepalive
-> renderer reset
-> reset old m_present_hook
-> reset old m_swapchain_hook
-> semantic commit_replacement
-> sync aliases
-> move prepared hook into m_swapchain_hook
-> source = XeFGInternal
-> phase1 = false
-> hooked = true
-> clear resize hold("binding_replaced")
-> return true
```

Recommended skeleton:

```cpp
if (plan.disposition == CandidateDisposition::ChangedSwapchainReplacement) {
    auto prepared = prepare_xefg_instance_hook(next_swapchain.Get());
    if (!prepared.ready()) {
        log_xefg_rebind(
            "failed",
            prepared.failure_reason,
            plan.previous.generation,
            m_swap_chain,
            swapchain,
            m_command_queue,
            command_queue,
            plan.previous.observe_only,
            observe_only);
        return false;
    }

    auto* const old_swapchain = m_swap_chain;
    auto* const old_queue = m_command_queue;

    Microsoft::WRL::ComPtr<IDXGISwapChain3> old_keepalive;
    if (plan.previous.active && old_swapchain != nullptr) {
        old_keepalive = old_swapchain;
    }

    g_framework->on_reset();

    m_present_hook.reset();
    m_swapchain_hook.reset();

    const auto commit = m_xefg_session.commit_prepared_candidate(
        plan,
        swapchain,
        std::move(next_queue),
        std::move(next_device),
        observe_only,
        runtime);

    if (!commit.committed) {
        // Do not attempt an ad-hoc rollback here. A valid plan/commit pairing
        // should make this branch unreachable before destructive work starts.
        return false;
    }

    sync_xefg_binding_aliases();
    m_swapchain_hook = std::move(prepared.hook);
    m_swapchain_source = SwapchainSource::XeFGInternal;
    m_is_phase_1 = false;
    m_hooked = true;

    if (commit.clear_resize_hold) {
        clear_xefg_resize_transition_hold(commit.resize_hold_reason);
    }

    return true;
}
```

### 13.5 NoActiveBinding / initial install

Preserve current behavior when no usable active XeFG path exists.

This includes takeover from an active non-XeFG physical hook.

Required ordering:

```text
plan NoActiveBinding
-> GetDevice
-> prepare all five new XeFG hooks
-> capture old semantic swapchain keepalive if semantic binding was active
-> if old semantic XeFG binding active: renderer reset
-> if replacing active non-XeFG hook: renderer reset
-> reset old physical hooks
-> semantic clear + commit_initial (generation 1)
-> sync aliases
-> publish prepared XeFG hook
-> source = XeFGInternal
-> phase1 = false
-> hooked = true
-> clear resize hold("external_bind")
```

Important edge-preservation rule:

The current code has two independent reset conditions:

```cpp
if (m_xefg_session.binding().active() && g_framework != nullptr) {
    g_framework->on_reset();
}

if (replacing_active_non_xefg && g_framework != nullptr) {
    g_framework->on_reset();
}
```

Do not deduplicate them merely because both being true looks unusual.

If an inconsistent state makes both true, current behavior can reset twice. Changing that is outside 2R6.

---

## 14. Bounded Old-Swapchain Keepalive Rules

Preserve current route-specific semantics.

### Changed / same-object active XeFG route

Use the physical current swapchain as current `replace_xefg_binding()` does:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> old_keepalive;
if (plan.previous.active && m_swap_chain != nullptr) {
    old_keepalive = m_swap_chain;
}
```

### NoActiveBinding route

Current initial-bind code keeps the semantic active swapchain alive if one exists:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> old_keepalive;
if (plan.previous.active && plan.previous.swapchain != nullptr) {
    old_keepalive = plan.previous.swapchain;
}
```

Do not convert either into persistent active ownership.

Do not keep `old_keepalive` in the session.

---

## 15. `bind_external_swapchain()` XeFG Routing

After 2R6, `bind_external_swapchain(... XeFGInternal ...)` must not execute a second XeFG mutation implementation.

Recommended structure:

```cpp
bool D3D12Hook::bind_external_swapchain(
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* command_queue,
    SwapchainSource source,
    bool xefg_observe_only,
    XeFGBinding::RuntimeIdentity runtime) {

    if (swapchain == nullptr || command_queue == nullptr) {
        return false;
    }

    if (source == SwapchainSource::XeFGInternal) {
        return apply_xefg_binding_request(
            swapchain,
            command_queue,
            xefg_observe_only,
            runtime);
    }

    // Existing non-XeFG external binding implementation continues below.
    ...
}
```

This early route is important because it guarantees:

```text
live candidate
pending candidate
explicit XeFG external bind
```

all use the same session plan and physical transaction.

### Non-XeFG path

Do not refactor native / non-XeFG external binding behavior except for removing XeFG-only branches that become provably unreachable after the early XeFG route.

If a cleanup is not directly required to eliminate the duplicate XeFG transaction, defer it to 2R7.

---

## 16. Candidate Handoff Must Use One Public Narrow Bridge

`XeFGCandidateHandoff` should no longer need friend access merely to call protected/private candidate application logic.

Make a narrow candidate application method public or expose an equivalent compatibility bridge:

```cpp
bool apply_xefg_candidate(const XeFGBindingCandidate& candidate);
```

Then change both handoff paths.

### Live candidate

```cpp
void XeFGCandidateHandoff::apply_to_live_hook(
    D3D12Hook& hook,
    const XeFGBindingCandidate& candidate) {

    if (!hook.apply_xefg_candidate(candidate)) {
        spdlog::warn(...);
    }
}
```

### Pending candidate

Replace:

```cpp
return hook.bind_external_swapchain(
    pending->swapchain.Get(),
    pending->selected_queue.Get(),
    D3D12Hook::SwapchainSource::XeFGInternal,
    pending->observe_only,
    pending->runtime);
```

with:

```cpp
return hook.apply_xefg_candidate(*pending);
```

The pending mutex must already be released at this point.

If `friend class XeFGCandidateHandoff;` has no remaining purpose after this change, remove only that friend declaration.

Do not remove `XeFGCompatibility` friendship in this PR unless separately proven unused and necessary to compile the intended boundary; 2R7 is the general friend-surface cleanup stage.

---

## 17. Preserve Pending Candidate Semantics

Do not change:

```text
publish while lifecycle mutex held
consume move-out under pending mutex
failed pending bind is not requeued
discard moves ownership out before COM destruction
COM destruction occurs after pending mutex unlock
```

The only intended handoff change is:

```text
pending candidate now enters the same apply_xefg_candidate transaction as live candidate
```

Do not change pending replacement policy from one pending candidate to a queue.

Do not add retries.

---

## 18. Existing Log Semantics to Preserve

Keep existing high-value log families and reason strings where practical:

```text
[XeFG][Bind]
[XeFG][Rebind]
[D3D12][ExternalBind]
```

Especially preserve established reasons:

```text
swapchain_changed
queue_changed
mode_changed
multiple_fields_changed
new_device_unavailable
new_hook_method_failed
new_hook_create_failed
external_bind
binding_replaced
```

It is acceptable to add one debug-only transaction-plan log such as:

```cpp
if (XeFGCompatibility::is_debug_log_enabled()) {
    spdlog::info(
        "[XeFG][CandidateTransaction] disposition = {}, reason = {}, previous_generation = {}, candidate_swapchain = 0x{:x}, candidate_queue = 0x{:x}, observe_only = {}",
        ...);
}
```

Do not add noisy always-on per-Present logging.

Do not downgrade existing failure logs that are needed for field diagnostics.

---

## 19. Do Not Add Rollback After Destructive Work

The intended safety model is:

> make commit failure structurally impossible after destructive work begins by validating the plan and preparing all required resources first.

Do not add a complicated rollback system that attempts to reconstruct an old `VtableHook` after it has been destroyed.

The correct ordering is:

```text
plan valid
+ device valid
+ new hook fully prepared if required
    -> only then destructive work starts
```

If implementation design makes `commit_prepared_candidate()` realistically fail after old hooks are removed, stop and redesign the transaction boundary instead of adding rollback complexity.

---

## 20. Physical / Semantic Commit Ordering

For changed-object and initial-bind transactions, preserve the current broad ordering:

```text
new physical hook object already fully prepared locally
-> old renderer reset
-> old physical hook ownership removed
-> semantic binding committed
-> raw aliases synchronized
-> prepared hook ownership published to m_swapchain_hook
-> physical flags finalized
```

Do not move semantic commit before new-hook preparation.

Do not clear the old semantic binding before new-hook preparation succeeds.

Do not publish the prepared new hook while the old physical transaction is still active.

All of this remains inside the existing lifecycle serialization context.

---

## 21. Direct Session State Cleanup After Commit

After 2R6, D3D12Hook should not manually duplicate all of these after every candidate branch:

```cpp
m_xefg_session.set_render_boundary_logged(false);
m_xefg_session.detached_state() = {};
clear_xefg_monitor_state();
```

For non-identical commits, move these semantic post-commit mutations into the session commit API.

For identical commit, preserve its narrower behavior exactly:

```text
clear detached
clear monitor
DO NOT reset render-boundary flag
DO NOT clear resize hold
```

The D3D12 layer may still call the existing resize-hold logging bridge when the session commit result requests:

```text
external_bind
binding_replaced
```

---

## 22. `XeFGBinding` Should Remain a Focused State Object

Do not move physical transaction logic into `XeFGBinding`.

Keep its responsibilities:

```text
compare identity fields
commit_initial
commit_same_swapchain_update
commit_replacement
refresh_runtime_identity
clear
lifecycle snapshot
```

`XeFGPresentationSession` should orchestrate which `XeFGBinding` commit is semantically appropriate.

Do not merge `XeFGBinding` into the session in 2R6.

---

## 23. Files Expected

Primary expected changes:

```text
src/D3D12Hook.hpp
src/D3D12Hook.cpp
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp
src/compatibility/xefg/XeFGCandidateHandoff.cpp
```

Possible but normally unnecessary:

```text
src/compatibility/xefg/XeFGCandidateHandoff.hpp
```

Expected unchanged unless a compelling compile-only reason exists:

```text
src/compatibility/xefg/XeFGBinding.hpp
src/compatibility/xefg/XeFGBinding.cpp
src/compatibility/xefg/XeFGResizeLifecycle.hpp
src/compatibility/xefg/XeFGResizeLifecycle.cpp
src/compatibility/xefg/XeFGDiscovery.hpp
src/compatibility/xefg/XeFGDiscovery.cpp
src/compatibility/xefg/XeFGCompatibility.cpp
src/REFramework.cpp
```

No CMake changes are expected.

If the implementation starts modifying candidate discovery, runtime registry, Present/Resize policy, monitor recovery, or generic D3D12 hook code, stop and reassess.

---

## 24. Explicitly Forbidden Changes

Do not:

- change candidate discovery / queue selection policy;
- change `XeFGQueueRelation` interpretation;
- trust `candidate.device` instead of preserving current swapchain `GetDevice()` validation;
- change active swapchain to strong persistent ownership;
- add COM refcount-drain loops;
- alter MHW resize-hold policy;
- alter Present/Present1 callback order;
- alter ResizeBuffers/ResizeBuffers1/ResizeTarget behavior;
- alter hook-monitor threshold/timing/classification;
- alter runtime detach/Destroy matching;
- change pending mutex/lifecycle mutex order;
- requeue failed candidates;
- add retry/timer/background recovery;
- move `VtableHook` ownership into the session;
- introduce a provider/frame-generation abstraction;
- generalize this transaction to Streamline/DLSSG/FSRFG;
- refactor native D3D12 phase-1 behavior;
- remove existing diagnostics merely to shorten the diff.

---

## 25. Required Static Review Matrix

Review the final code branch-by-branch.

### Case A — invalid candidate

```text
swapchain null OR queue null
-> Reject
-> no state change
```

### Case B — initial XeFG bind

```text
no usable active XeFG path
-> NoActiveBinding
-> GetDevice
-> prepare 5 hooks
-> only then reset/remove old physical state
-> commit_initial generation 1
-> borrowed active swapchain
```

### Case C — identical live candidate

```text
same swapchain + same queue + same mode
-> Identical
-> runtime identity refresh
-> no GetDevice requirement
-> no reset
-> no hook replacement
-> no generation change
```

### Case D — same swapchain, queue changed

```text
semantic change present
physical m_swap_chain == candidate swapchain
-> SameSwapchainUpdate
-> GetDevice
-> renderer reset
-> existing hook retained
-> generation +1
```

### Case E — same swapchain, mode changed

Same as Case D.

### Case F — changed swapchain

```text
-> ChangedSwapchainReplacement
-> GetDevice
-> prepare 5 hooks
-> failure before destructive work leaves old binding intact
-> bounded old keepalive
-> renderer reset
-> remove old hooks
-> commit replacement generation +1
-> publish new hook
```

### Case G — pending candidate

```text
move pending out under pending mutex
-> unlock pending mutex
-> same apply_xefg_candidate path as live candidate
```

### Case H — non-XeFG external bind

```text
existing non-XeFG behavior materially unchanged
```

---

## 26. Build / Static Validation

Run:

```text
cmake -S . -B build-2r6 -G "Visual Studio 17 2022" -A x64
cmake --build build-2r6 --config Release --target REFramework --parallel 4
python dev/audit_direct_access_clang.py
git diff --check
```

Also perform a source audit proving:

```text
1. pending and live candidates use the same transaction entry;
2. no XeFGInternal branch in bind_external_swapchain performs an independent semantic commit;
3. all five hooks are fully prepared before changed-object/initial destructive work;
4. same-object update does not recreate m_swapchain_hook;
5. identical candidate does not reset renderer or increment generation;
6. failed GetDevice leaves old state unchanged;
7. failed hook preparation leaves old state unchanged;
8. old swapchain keepalive is bounded local scope only;
9. active binding swapchain remains raw borrowed;
10. queue/device remain strong ComPtr;
11. resize hold reasons remain external_bind / binding_replaced at the same semantic points;
12. Present/Resize files show no behavioral reordering outside transaction helper movement;
13. XeFGCandidateHandoff lock ordering is unchanged;
14. failed pending application is not requeued;
15. non-XeFG external binding remains materially unchanged.
```

---

## 27. Runtime Gate — Mandatory for 2R6 Merge

The architecture document explicitly assigns a stronger runtime gate to 2R6 than the earlier extraction PRs.

Unlike 2R3–2R5, **do not treat Release build + static audit alone as sufficient evidence for final merge of 2R6**.

Before final merge, run the practical XeFG lifecycle matrix available to the project.

Minimum:

```text
A. REF only / native D3D12
   - normal launch
   - overlay/menu still works
   - no XeFG candidate activity

B. REF + OptiScaler with XeFG not selected
   - normal hook behavior
   - no accidental XeFG semantic takeover

C. DD2 + OptiScaler + Intel XeFG
   - initial bind
   - gameplay Present/Present1
   - Alt+Tab out/in
   - resize / swapchain lifecycle if naturally triggered
   - no overlay regression

D. MHW + OptiScaler + Intel XeFG
   - initial bind
   - known ResizeTarget -> ResizeBuffers/ResizeBuffers1 lifecycle
   - MHW-only hold still behaves correctly
   - Alt+Tab
   - no fatal D3D regression attributable to the transaction change
```

If a same-swapchain update or changed-swapchain replacement is naturally produced, capture debug logs showing the selected transaction disposition and resulting generation.

Do not add synthetic runtime mutation solely to force a disposition.

If runtime testing cannot be performed in the implementation environment, mark the PR as needing runtime validation rather than claiming the full 2R6 gate passed.

---

## 28. Suggested Debug Evidence

If a debug-only candidate-plan log is added, useful values are:

```text
disposition
reason
previous generation
previous semantic swapchain
physical renderer swapchain
candidate swapchain
previous queue
candidate queue
old/new observe_only
runtime slot/context
```

Avoid logging on every Present.

The transaction log should be candidate-triggered only.

---

## 29. Suggested PR

Branch:

```text
refactor/xefg-2r6-candidate-transaction
```

PR title:

```text
XeFG 2R6: consolidate candidate bind/rebind transaction
```

Base:

```text
REFforXeFG
```

---

## 30. Stop Conditions

Stop and report rather than broadening the PR if implementation appears to require:

- moving physical VtableHook ownership into `XeFGPresentationSession`;
- adding persistent swapchain ownership;
- changing pending candidate lock order;
- changing candidate discovery / queue selection;
- changing Intel runtime Init/Destroy behavior;
- changing Present/Resize ordering;
- changing hook-monitor recovery;
- adding rollback reconstruction of destroyed old hooks;
- adding asynchronous retries;
- touching Streamline/DLSSG/FSRFG transaction paths;
- refactoring generic/native external bind beyond directly eliminating unreachable XeFG duplication;
- changing generation semantics.

If a valid candidate plan can fail only after destructive work starts, stop and redesign the prepare/commit boundary before continuing.

---

## 31. Definition of Done

2R6 is complete when:

- `XeFGPresentationSession` classifies candidate changes into one explicit transaction plan;
- `D3D12Hook` performs one physical XeFG candidate transaction according to that plan;
- live and pending candidates use the same public candidate-application bridge;
- direct XeFG external binding does not retain an independent semantic mutation path;
- identical candidate behavior remains non-destructive and generation-stable;
- same-swapchain update retains the existing hook;
- changed-swapchain replacement prepares all five hooks before destructive work;
- initial bind prepares all five hooks before destructive work;
- failed device/hook preparation leaves the previous active state unchanged;
- old active swapchain keepalive is bounded to transaction scope;
- committed active swapchain remains borrowed;
- queue/device ownership stays strong;
- semantic render-boundary/detached/monitor cleanup is centralized in session commit logic without changing branch-specific behavior;
- resize-hold clear reasons and timing are preserved;
- pending lock order and COM-release behavior are preserved;
- non-XeFG external binding remains materially unchanged;
- Release build, direct-access audit, and diff-check pass;
- the required 2R6 runtime lifecycle gate is completed before final merge.

After 2R6, proceed to **2R7 — collapse transitional XeFG surface in `D3D12Hook` and perform the final non-regression audit**.