# Work Order: XeFG Refactor R7 — Initial Bind and Transactional Rebind Protocol

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `be062b529df67c79571d84d896d994efc8125bf3` (`refactor: centralize XeFG active binding ownership (#24)`)

Relevant merged refactor baseline:

- R1 / PR #17: `65f9b3ee81971c3e2aac6df49518fa2dd588365d` — exact-HMODULE XeFG runtime registry extraction
- R2 / PR #18: `aa3a53e516b882a77d399c929efa0ef29d1426b0` — XeFG loader/probe handoff isolation
- PR #19: `3f83b8af0184f931daec44dc45257f7fc46966a4` — tracked `CMakeLists.txt` compatibility-source registration
- PR #20: `74042e1686f62a54a50540e1a113a3ae648778c1` — MHW-only XeFG ResizeTarget transition hold
- R3 / PR #21: `cfb6efe5102f330c9ddf6fdadb7e9af984ce0678` — InitDesc observation transaction + temporary factory capture extraction
- R4 / PR #22: `1eab453505fe3c75bb7ed45715ce7a1d2c6cc35f` — queue/device/HWND validation + strongly-owned candidate construction
- R5 / PR #23: `d470bee7eb6f369bbef8983982902520e0c0572c` — pending candidate lifecycle handoff extraction
- R6 / PR #24: `be062b529df67c79571d84d896d994efc8125bf3` — active XeFG strong ownership/generation/mode/identity centralized in `XeFGBinding`

Related documents:

- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_ARCHITECTURE_2026-09-06.md`
- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_PR_SPLIT_PLAN_2026-09-06.md`
- `doc/work-order/XEFG_REFACTOR_R1_RUNTIME_REGISTRY_EXTRACTION.md`
- `doc/work-order/XEFG_REFACTOR_R2_LOADER_PROBE_HANDOFF.md`
- `doc/work-order/XEFG_REFACTOR_R3_INIT_TRANSACTION_FACTORY_CAPTURE.md`
- `doc/work-order/XEFG_REFACTOR_R4_QUEUE_VALIDATION_CANDIDATE.md`
- `doc/work-order/XEFG_REFACTOR_R5_PENDING_CANDIDATE_HANDOFF.md`
- `doc/work-order/XEFG_REFACTOR_R6_ACTIVE_BINDING_OWNERSHIP_IDENTITY.md`

This work order implements **R7 only** from the fine-grained XeFG refactor plan.

The fork is still **unreleased**. Therefore this PR may remove temporary R5/R6 bridge APIs or duplicated decision logic in the same PR when doing so makes the transaction boundary cleaner. Do not preserve fork-only compatibility wrappers that have no external consumer.

That freedom applies to internal structure only. The working runtime behavior remains the contract.

---

# 1. Recommended PR Identity

Suggested branch:

```text
refactor/xefg-r7-binding-transaction
```

Suggested PR title:

```text
Refactor R7: isolate XeFG initial bind and transactional rebind
```

Suggested commit title:

```text
refactor: isolate XeFG binding transactions
```

R7 is the **highest-risk refactor PR** in the sequence.

The one primary responsibility is:

> Make the physical XeFG instance-hook mutation path one explicit transaction around the R6 `XeFGBinding` state, so initial bind and changed-object rebind prepare everything non-destructively before mutating the current renderer/hook/binding.

This PR must make failure behavior obvious:

```text
prepare failure
    -> no renderer reset
    -> no old-hook removal
    -> no active binding mutation
    -> no alias mutation
    -> no generation increment
    -> no resize-state mutation
```

Do not start R8 resize-state extraction or R9 callback cleanup here.

---

# 2. Release / Development Policy

The project is intentionally still unreleased. Planned sequence remains:

```text
R7 physical binding transaction
R8 resize lifecycle state extraction
R9 Present/resize callback cleanup
R10 hook-monitor/upstream-surface cleanup
R11 final logging + persistent Debug Logging UI
final runtime validation
release
```

Therefore R7 may make a clean internal cut now, including:

- replacing raw XeFG bind/rebind entry points with a candidate-based internal entry point;
- removing R5 temporary identity comparison code once `XeFGBinding::compare()` becomes authoritative;
- moving the non-XeFG -> XeFG renderer-reset decision from R5 handoff into the R7 transaction;
- adding a private prepared-transaction structure inside `D3D12Hook`;
- removing now-unused R5 helpers rather than preserving aliases.

Still forbidden:

- combining R7 with R8/R9/R10;
- changing queue-selection policy from R4;
- changing DXGI hook slots;
- changing MHW resize-hold policy;
- changing Present/Present1 forwarding/suppression behavior;
- changing hook-monitor preservation behavior;
- log-level cleanup before R11;
- generic frame-generation abstractions;
- FSRFG/DLSSG/Streamline redesign;
- hooking-library replacement.

---

# 3. Current State After R6

The front half of XeFG compatibility is now well separated:

```text
R1 XeFGRuntimeRegistry
    -> exact runtime/module dispatch

R2 XeFGCompatibility
    -> loader/probe handoff

R3 XeFGDiscovery::observe_init()
    -> bounded InitDesc + factory observation

R4 XeFGDiscovery::build_binding_candidate()
    -> validated strongly-owned XeFGBindingCandidate

R5 XeFGCandidateHandoff
    -> live delivery or one pending slot

R6 XeFGBinding
    -> active strong COM ownership
    -> generation
    -> observe-only mode
    -> semantic identity
```

The remaining high-risk mutation logic is still physically spread through `D3D12Hook.cpp` and partially interpreted by `XeFGCandidateHandoff.cpp`.

Current R6 `XeFGBinding` already provides:

```cpp
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

void commit_initial(...);
void commit_same_swapchain_update(...);
void commit_replacement(...);
void clear() noexcept;
```

However, current physical mutation still has three separate shapes:

1. R5 handoff manually decides whether the active binding changed;
2. `D3D12Hook::replace_xefg_binding()` contains same-object update and changed-object replacement;
3. `D3D12Hook::bind_external_swapchain()` contains the initial XeFG hook path together with generic/native external binding behavior.

R7 should isolate those three XeFG mutation cases into one explicit candidate-driven protocol.

---

# 4. Important Current Weakness: Initial XeFG Bind Is Not Fully Prepared Before Commit

The changed-object rebind path already does the important safety step:

```text
create next VtableHook
hook Present[8]
hook Present1[22]
hook ResizeBuffers[13]
hook ResizeTarget[14]
hook ResizeBuffers1[39]
verify all hook_method() calls succeeded
THEN reset/remove old state
```

The current initial `bind_external_swapchain()` path is weaker:

```text
acquire local COM ownership
remove existing hook objects
clear active XeFG ownership
commit new XeFGBinding / aliases
construct VtableHook
call hook_method() for slots
set m_hooked = true
```

and it does not currently use the same all-five-method success gate used by changed-object rebind.

R7 should correct this asymmetry.

For XeFG initial bind, the new instance hook must be completely prepared **before any destructive mutation of the current D3D12Hook state**.

This is not a generalized behavior change. It is the explicit R7 safety objective already defined by the architecture/split plan.

---

# 5. Fundamental R7 Transaction Invariant

For every changed-object XeFG bind/rebind:

```text
VALIDATE
    -> ACQUIRE STRONG NEW OWNERSHIP
    -> PREPARE COMPLETE NEW INSTANCE HOOK
    -> only if preparation succeeded:
         RESET OLD RENDERER if required
         REMOVE OLD PHYSICAL HOOK while old COM remains alive
         COMMIT NEW XeFGBinding
         SYNC RAW ALIASES
         COMMIT NEW PHYSICAL HOOK
         UPDATE SOURCE / PHASE / MODE-DEPENDENT FLAGS
         UPDATE GENERATION through XeFGBinding
```

Failure before the destructive phase must leave the old state untouched.

Merge-blocking invariant:

> `XeFGBinding::clear()` or `commit_replacement()` must never release the COM object targeted by the old `VtableHook` before that old hook has been removed.

Another merge-blocking invariant:

> `g_framework->on_reset()` must not run merely because a candidate was presented. It should run only after the new changed-object hook has been completely prepared and the transaction is committed to proceed.

---

# 6. Preferred R7 Ownership Boundary

## 6.1 `XeFGCandidateHandoff` after R7

R5 handoff should return to being a pure delivery component:

```text
validated candidate
    -> live D3D12Hook exists
         -> hook.apply_xefg_candidate(candidate)

validated candidate
    -> live hook does not exist
         -> store one pending candidate

D3D12Hook::hook()
    -> consume pending
    -> hook.apply_xefg_candidate(candidate)
```

It should no longer decide:

- whether the current XeFG identity changed;
- whether same-object vs changed-object applies;
- whether a non-XeFG renderer must be reset;
- how the physical hook is replaced.

Those are R7 active mutation decisions.

## 6.2 `D3D12Hook` after R7

`D3D12Hook` remains the owner of physical callback/VtableHook mechanics.

Preferred internal entry point:

```cpp
bool apply_xefg_candidate(const XeFGBindingCandidate& candidate);
```

This method should own the full decision tree:

```text
candidate invalid
    -> false

active XeFG binding
    -> compare via XeFGBinding::compare()
    -> unchanged -> true
    -> same swapchain -> same-object update
    -> changed swapchain -> transactional replacement

no active XeFG binding
    -> transactional initial XeFG bind
```

This gives one physical mutation boundary without creating a generic event system.

---

# 7. Preferred File Strategy

Expected modified files:

```text
MODIFY src/D3D12Hook.hpp
MODIFY src/D3D12Hook.cpp
MODIFY src/compatibility/xefg/XeFGCandidateHandoff.cpp
```

Normally unchanged:

```text
src/compatibility/xefg/XeFGBinding.hpp
src/compatibility/xefg/XeFGBinding.cpp
src/compatibility/xefg/XeFGDiscovery.*
src/compatibility/xefg/XeFGCompatibility.*
src/compatibility/xefg/XeFGRuntimeRegistry.*
src/REFramework.cpp
CMakeLists.txt
cmake.toml
```

**Preferred: do not add another compatibility source file for R7.**

The architecture intentionally keeps:

```text
XeFGBinding = semantic state + strong COM lifetime
D3D12Hook   = physical VtableHook/callback ownership
```

A new `XeFGPhysicalHookManager` or generic hook provider would blur that boundary and increase upstream surface.

A small private/nested transaction structure inside `D3D12Hook` is preferred.

If implementation proves materially cleaner with a tiny dedicated helper file, it is allowed because the fork is unreleased, but it must remain XeFG-specific and must not own callback implementations or renderer policy. Such a deviation should be justified in the PR body.

---

# 8. Recommended Private Transaction Structure

A prepared XeFG transaction should keep the candidate COM objects alive independently of both the old active binding and the caller's candidate object.

Conceptual shape:

```cpp
struct PreparedXeFGBinding {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue{};
    Microsoft::WRL::ComPtr<ID3D12Device4> device{};
    std::unique_ptr<VtableHook> hook{};
    bool observe_only{true};

    bool complete() const noexcept {
        return swapchain != nullptr
            && queue != nullptr
            && device != nullptr
            && hook != nullptr;
    }
};
```

This structure is **temporary transaction state**, not active state.

Do not put it in `XeFGBinding`.

Do not persist it after commit.

Why local strong ownership matters:

```text
candidate caller may return
old binding may later be removed
VtableHook destructor may need target object alive during rollback
```

The prepared object guarantees the new target remains alive throughout hook preparation and rollback.

---

# 9. Prefer Candidate-Based Internal APIs

R4 already gives us a validated strongly-owned candidate:

```cpp
XeFGBindingCandidate {
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12CommandQueue> selected_queue;
    ComPtr<ID3D12Device4> device;
    HWND hwnd;
    XeFGQueueRelation relation;
    bool observe_only;
};
```

R7 should preferably stop converting this back into a raw XeFG API too early.

Recommended internal API:

```cpp
bool D3D12Hook::apply_xefg_candidate(
    const XeFGBindingCandidate& candidate);
```

and a preparation helper:

```cpp
std::optional<PreparedXeFGBinding>
D3D12Hook::prepare_xefg_binding(
    const XeFGBindingCandidate& candidate);
```

The prepared copy should take its own strong references:

```cpp
PreparedXeFGBinding prepared{};
prepared.swapchain = candidate.swapchain;
prepared.queue = candidate.selected_queue;
prepared.device = candidate.device;
prepared.observe_only = candidate.observe_only;
```

Do not re-run R4 queue policy.

Do not substitute another queue.

Do not use the public XeFG proxy.

### Device sanity

Because R4 already validated and strongly owns the candidate device, R7 may reuse `candidate.device` directly.

A cheap defensive consistency check is acceptable if desired, but do not create a second competing device-selection policy.

---

# 10. Complete Hook Preparation Helper

The helper must prepare the same five current XeFG instance slots:

```text
Present[8]
ResizeBuffers[13]
ResizeTarget[14]
Present1[22]
ResizeBuffers1[39]
```

Recommended helper skeleton:

```cpp
std::optional<D3D12Hook::PreparedXeFGBinding>
D3D12Hook::prepare_xefg_binding(
    const XeFGBindingCandidate& candidate) {
    if (!candidate.valid()) {
        return std::nullopt;
    }

    PreparedXeFGBinding prepared{};
    prepared.swapchain = candidate.swapchain;
    prepared.queue = candidate.selected_queue;
    prepared.device = candidate.device;
    prepared.observe_only = candidate.observe_only;

    try {
        auto hook = std::make_unique<VtableHook>(
            Address{prepared.swapchain.Get()});

        const bool present_ok = hook->hook_method(
            8, Address{reinterpret_cast<void*>(&D3D12Hook::present)});
        const bool resize_buffers_ok = hook->hook_method(
            13, Address{reinterpret_cast<void*>(&D3D12Hook::resize_buffers)});
        const bool resize_target_ok = hook->hook_method(
            14, Address{reinterpret_cast<void*>(&D3D12Hook::resize_target)});
        const bool present1_ok = hook->hook_method(
            22, Address{reinterpret_cast<void*>(&D3D12Hook::present1)});
        const bool resize_buffers1_ok = hook->hook_method(
            39, Address{reinterpret_cast<void*>(&D3D12Hook::resize_buffers1)});

        if (!(present_ok
            && resize_buffers_ok
            && resize_target_ok
            && present1_ok
            && resize_buffers1_ok)) {
            return std::nullopt;
        }

        prepared.hook = std::move(hook);
        return prepared;
    } catch (...) {
        return std::nullopt;
    }
}
```

Exact slot-call ordering may preserve the current implementation ordering instead of the example ordering above. **Do not change the set of slots.**

Important rollback property:

```text
partial hook_method success
    -> helper returns failure
    -> local VtableHook destructs
    -> target remains alive via prepared.swapchain
    -> old active binding was never modified
```

No physical preparation failure may partially commit active state.

---

# 11. R5 Handoff Simplification

Current R5 code manually performs:

```cpp
const auto swapchain_changed = ...;
const auto queue_changed = ...;
const auto mode_changed = ...;
const auto changed = ...;
const auto reason = binding_change_reason(...);
```

R6 already added `XeFGBinding::compare()` and `IdentityChange::reason()` for this semantic purpose.

R7 should remove the duplicate R5 comparison helper if practical.

Preferred result:

```cpp
bool XeFGCandidateHandoff::consume_pending(D3D12Hook& hook) {
    std::optional<XeFGBindingCandidate> pending;
    {
        std::scoped_lock lock{s_pending_mutex};
        pending = std::move(s_pending_candidate);
        s_pending_candidate.reset();
    }

    if (!pending) {
        return false;
    }

    return hook.apply_xefg_candidate(*pending);
}
```

and live delivery conceptually:

```cpp
void XeFGCandidateHandoff::apply_to_live_hook(
    D3D12Hook& hook,
    const XeFGBindingCandidate& candidate) {
    if (!hook.apply_xefg_candidate(candidate)) {
        spdlog::warn(
            "[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = apply_failed",
            reinterpret_cast<uintptr_t>(candidate.swapchain.Get()));
    }
}
```

The existing detailed binding-gate log may be emitted inside `D3D12Hook::apply_xefg_candidate()` instead of being deleted.

**Do not clean up log volume in R7.** R11 handles that.

---

# 12. Move Non-XeFG -> XeFG Reset Into the Transaction

Current R5 live delivery does this before calling the bind function:

```cpp
if (replacing_active_non_xefg) {
    g_framework->on_reset();
}

hook.bind_external_swapchain(...);
```

That means renderer reset currently happens before the initial XeFG instance hook has been proven installable.

R7 should move this decision into the initial-bind commit phase.

Required ordering:

```text
candidate delivered
-> prepare complete XeFG VtableHook
-> if preparation failed:
     return false
     DO NOT reset renderer
-> if preparation succeeded and an active non-XeFG renderer is being replaced:
     g_framework->on_reset()
-> remove old physical hook(s)
-> commit new XeFG state/hook
```

After R7, `XeFGCandidateHandoff` should not call `g_framework->on_reset()` for binding mutation.

This is an intentional R7 ownership correction, not R5 scope creep.

---

# 13. `apply_xefg_candidate()` Decision Tree

Recommended high-level implementation:

```cpp
bool D3D12Hook::apply_xefg_candidate(
    const XeFGBindingCandidate& candidate) {
    if (!candidate.valid()) {
        return false;
    }

    const bool active_xefg =
        m_hooked
        && !m_is_phase_1
        && m_swapchain_source == SwapchainSource::XeFGInternal
        && m_swapchain_hook != nullptr
        && m_xefg_binding.active()
        && m_xefg_binding.aliases_match(
            m_swap_chain, m_command_queue, m_device);

    if (!active_xefg) {
        return bind_initial_xefg_candidate(candidate);
    }

    const auto change = m_xefg_binding.compare(
        candidate.swapchain.Get(),
        candidate.selected_queue.Get(),
        candidate.observe_only);

    log binding gate using current strings;

    if (!change.changed()) {
        return true;
    }

    if (!change.swapchain_changed) {
        return update_same_xefg_swapchain(candidate, change.reason());
    }

    return replace_xefg_swapchain(candidate, change.reason());
}
```

Exact helper names are flexible.

The behavior is not.

### Important

Do not use only `m_swap_chain == candidate.swapchain.Get()` to define active XeFG health.

R6 added a complete semantic binding object for a reason. The gate should require the same healthy binding invariants already used by `has_active_xefg_instance_binding()` or reuse that predicate where appropriate.

---

# 14. Initial XeFG Bind Transaction

Initial bind means:

```text
there is no healthy active XeFG binding
```

There may still be:

- a native D3D12 physical hook;
- a generic/native external swapchain hook;
- phase-1 hook state;
- no hook at all.

R7 must not alter native setup semantics except at the exact XeFG transition point.

Recommended flow:

```cpp
bool D3D12Hook::bind_initial_xefg_candidate(
    const XeFGBindingCandidate& candidate) {
    auto prepared = prepare_xefg_binding(candidate);
    if (!prepared) {
        return false;
    }

    const bool replacing_active_non_xefg =
        m_hooked
        && m_swap_chain != nullptr
        && m_swapchain_source != SwapchainSource::XeFGInternal;

    // Destructive phase starts only here.
    if (replacing_active_non_xefg) {
        spdlog::info(
            "[XeFG][Bind] resetting active D3D12 renderer before XeFG bind");
        g_framework->on_reset();
    }

    // Remove old physical hooks only after new XeFG hook is complete.
    m_present_hook.reset();
    m_swapchain_hook.reset();

    // If stale XeFG semantic state somehow exists, old physical hook is already gone.
    m_xefg_binding.clear();

    m_xefg_binding.commit_initial(
        std::move(prepared->swapchain),
        std::move(prepared->queue),
        std::move(prepared->device),
        prepared->observe_only);

    sync_xefg_binding_aliases();
    m_swapchain_hook = std::move(prepared->hook);
    m_swapchain_source = SwapchainSource::XeFGInternal;
    m_xefg_p21_render_boundary_logged = false;
    m_is_phase_1 = false;
    m_hooked = true;

    clear_xefg_resize_transition_hold("external_bind");
    return true;
}
```

The exact stale-state handling may differ, but **never clear an old XeFG binding before its old VtableHook has been removed**.

### Initial bind failure contract

If preparation fails:

```text
m_present_hook          unchanged
m_swapchain_hook        unchanged
m_xefg_binding          unchanged
m_swap_chain            unchanged
m_command_queue         unchanged
m_device                unchanged
m_swapchain_source      unchanged
m_is_phase_1            unchanged
m_hooked                unchanged
resize state            unchanged
renderer resources      not reset
```

This contract is merge-blocking.

---

# 15. Same-Swapchain Queue/Mode Update

Same swapchain + changed queue or mode does **not** require a new VtableHook.

Preserve current proven behavior:

```text
same swapchain
changed queue and/or mode
-> reset renderer once
-> retain existing physical instance hook
-> replace strong queue/device ownership
-> update mode
-> synchronize raw aliases
-> increment generation exactly once
-> clear stale resize transition hold
```

Recommended:

```cpp
bool D3D12Hook::update_same_xefg_swapchain(
    const XeFGBindingCandidate& candidate,
    const char* reason) {
    if (candidate.swapchain.Get() != m_xefg_binding.swapchain()) {
        return false;
    }

    const auto old_generation = m_xefg_binding.generation();
    const auto old_queue = m_xefg_binding.queue();
    const auto old_mode = m_xefg_binding.observe_only();

    log_xefg_rebind("begin", ...);
    spdlog::info(
        "[XeFG][Rebind] stage = old_renderer_reset, reason = {}, generation = {}",
        reason, old_generation);

    g_framework->on_reset();

    m_xefg_binding.commit_same_swapchain_update(
        candidate.selected_queue,
        candidate.device,
        candidate.observe_only);
    sync_xefg_binding_aliases();

    m_xefg_p21_render_boundary_logged = false;
    clear_xefg_resize_transition_hold("binding_replaced");

    // generation == old_generation + 1
    log same_object_updated;
    return true;
}
```

Do not recreate or reset `m_swapchain_hook` for same-object queue/mode changes.

Do not increment generation more than once.

Do not change the hook slots.

---

# 16. Changed-Swapchain Transactional Replacement

This is the most important path.

Required exact phase structure:

## Phase A — Non-destructive preparation

```text
validate candidate
acquire own strong new swapchain/queue/device references
construct new VtableHook on new swapchain
hook all five required slots
verify every hook_method result
```

Old active state remains authoritative throughout Phase A.

If anything fails:

```text
local prepared hook destructs
new local COM refs release after hook rollback
old active XeFGBinding remains untouched
old m_swapchain_hook remains untouched
old aliases remain untouched
generation unchanged
renderer not reset
resize state unchanged
return false
```

## Phase B — Commit begins

Only after Phase A succeeds:

```text
reset renderer once
```

## Phase C — Retire old physical hook safely

```text
old XeFGBinding still strongly owns old swapchain
m_present_hook.reset()
m_swapchain_hook.reset()
```

Only after `m_swapchain_hook.reset()` may the old active strong ownership be replaced.

## Phase D — Commit new active semantic + physical state

```text
m_xefg_binding.commit_replacement(...)
sync_xefg_binding_aliases()
m_swapchain_hook = std::move(prepared.hook)
m_swapchain_source = XeFGInternal
m_xefg_p21_render_boundary_logged = false
m_is_phase_1 = false
m_hooked = true
clear stale resize transition hold
generation incremented exactly once by XeFGBinding
```

Recommended skeleton:

```cpp
bool D3D12Hook::replace_xefg_swapchain(
    const XeFGBindingCandidate& candidate,
    const char* reason) {
    const auto old_swapchain = m_xefg_binding.swapchain();
    const auto old_queue = m_xefg_binding.queue();
    const auto old_mode = m_xefg_binding.observe_only();
    const auto old_generation = m_xefg_binding.generation();

    auto prepared = prepare_xefg_binding(candidate);
    if (!prepared) {
        log_xefg_rebind(
            "failed", "new_hook_prepare_failed", old_generation,
            old_swapchain, candidate.swapchain.Get(),
            old_queue, candidate.selected_queue.Get(),
            old_mode, candidate.observe_only);
        return false;
    }

    log_xefg_rebind("new_hook_prepared", ...);

    g_framework->on_reset();

    // Old COM ownership MUST still be active here.
    m_present_hook.reset();
    m_swapchain_hook.reset();

    log_xefg_rebind("old_hook_removed", ...);

    m_xefg_binding.commit_replacement(
        std::move(prepared->swapchain),
        std::move(prepared->queue),
        std::move(prepared->device),
        prepared->observe_only);
    sync_xefg_binding_aliases();
    m_swapchain_hook = std::move(prepared->hook);

    m_swapchain_source = SwapchainSource::XeFGInternal;
    m_xefg_p21_render_boundary_logged = false;
    m_is_phase_1 = false;
    m_hooked = true;

    clear_xefg_resize_transition_hold("binding_replaced");

    log_xefg_rebind("new_binding_committed", ...);
    return true;
}
```

Use the current log reason strings where possible. Do not use R7 as an excuse to redesign logging terminology.

---

# 17. Partial New-Hook Failure Must Be Safe

A particularly important failure case is:

```text
Present[8] hook succeeds
Present1[22] hook succeeds
ResizeBuffers[13] succeeds
ResizeTarget[14] fails
```

Expected behavior:

```text
prepare helper returns failure
local VtableHook destructor restores any modified slots
new target COM stays alive during destructor because PreparedXeFGBinding owns it
old active hook remains installed
old active XeFGBinding remains alive
generation unchanged
no renderer reset
```

Do not manually remove individual successful slots using a separate rollback system unless `VtableHook` actually requires it. Prefer its scoped lifetime if that matches current library behavior.

Do not release `prepared.swapchain` before `prepared.hook` is destroyed.

### Member destruction-order note

If `PreparedXeFGBinding` relies on automatic destruction, ensure object/member ordering cannot destroy the strong swapchain reference before the `VtableHook` destructor needs the target.

Safest options:

1. explicitly `prepared.hook.reset()` before prepared COM references leave scope on failure; or
2. declare members / write a destructor so the hook is guaranteed to be destroyed while `swapchain` is still alive.

Do not assume C++ member destruction order incorrectly.

This should be explicitly reviewed.

---

# 18. Important C++ Destruction-Order Requirement

C++ destroys data members in **reverse declaration order**.

Therefore this shape:

```cpp
struct PreparedXeFGBinding {
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device4> device;
    std::unique_ptr<VtableHook> hook;
};
```

is good for rollback lifetime because destruction is:

```text
hook
then device
then queue
then swapchain
```

The target swapchain remains strongly owned while `VtableHook::~VtableHook()` runs.

Do not reorder the members to:

```cpp
std::unique_ptr<VtableHook> hook;
ComPtr<IDXGISwapChain3> swapchain;
```

without an explicit destructor, because then the swapchain would be released before the hook is destroyed.

Treat this as a merge-blocking lifetime detail.

---

# 19. Keep Generic/Native `bind_external_swapchain()` Stable

`D3D12Hook::bind_external_swapchain()` is an upstream-sensitive/general D3D12 method and currently supports both source values.

R7 should minimize native-path risk.

Preferred choices, in order:

### Option A — Preferred if call-site audit confirms safe

Add a separate XeFG-specific internal path:

```cpp
bool apply_xefg_candidate(const XeFGBindingCandidate& candidate);
```

and leave the native/general `bind_external_swapchain()` behavior unchanged for non-XeFG callers.

If no remaining XeFG caller needs the public/raw XeFG branch, remove only that XeFG branch after confirming all call sites.

### Option B — Acceptable

Keep the method signature for now but immediately delegate XeFG to the new transaction:

```cpp
if (source == SwapchainSource::XeFGInternal) {
    // construct/pass the already validated candidate through the XeFG-specific path
}

// old native behavior below unchanged
```

This is less preferred because it can force reconstruction of a candidate from raw pointers.

### Mandatory audit

Before removing or changing `bind_external_swapchain()`:

```text
search entire source tree for bind_external_swapchain(
classify every caller
prove native/Streamline/FSR3 path is not changed accidentally
```

Do not assume from one translation unit that the function has no other consumer.

---

# 20. Keep `XeFGBinding` Semantic — Do Not Put the VtableHook Into It

R6 established:

```text
XeFGBinding = semantic active state + strong COM lifetime
```

R7 must not turn it into:

```text
XeFGBinding = COM lifetime + VtableHook + callbacks + renderer reset + lifecycle mutex
```

That would defeat the architecture boundary.

Keep:

```cpp
std::unique_ptr<VtableHook> m_swapchain_hook;
```

in `D3D12Hook`.

Keep callback functions:

```text
present
present1
resize_buffers
resize_target
resize_buffers1
```

in `D3D12Hook`.

R9 is where callback bodies become thinner.

---

# 21. Keep R8 Resize Lifecycle Out of R7

R7 may continue calling existing:

```cpp
clear_xefg_resize_transition_hold("external_bind");
clear_xefg_resize_transition_hold("binding_replaced");
```

at the exact same semantic commit points.

Do not extract or redesign:

```text
m_xefg_resize_event_id
m_xefg_resize_transition_hold
m_xefg_resize_transition_hold_event_id
m_xefg_resize_transition_suppressed_present_count
m_xefg_last_resize_kind
m_xefg_last_resize_event_time
m_xefg_post_resize_present_budget
m_xefg_post_resize_present_ordinal
```

That is R8.

Do not add timeout/sleep/Present-count fallback behavior.

PR #20's MHW-only hold condition remains untouched:

```cpp
event_id != 0
&& renderer_reset_performed
&& !observe_only
&& sdk::GameIdentity::get().is_mhwilds()
```

---

# 22. Keep Present / Present1 / Resize Callback Behavior Frozen

Do not change:

- original Present forwarding;
- original Present1 forwarding;
- render callback suppression condition;
- `m_is_phase_1` behavior;
- tracked swapchain checks;
- ResizeBuffers pre/post behavior;
- ResizeBuffers1 renderer-reset timing;
- ResizeTarget renderer-reset timing;
- MHW transition hold arm/clear semantics;
- liveness accounting;
- callback hook slot indices.

R7 may update only the way the active hook object/state reaches those callbacks.

If a callback-body cleanup seems obvious, defer it to R9.

---

# 23. Keep Hook-Monitor Behavior Frozen

Do not change `REFramework::hook_monitor()` in R7.

Current preservation policy remains:

```text
healthy active XeFG binding
    -> generic present-timeout recovery must preserve it
```

`has_active_xefg_instance_binding()` remains the semantic predicate.

R10 handles hook-monitor isolation and final upstream surface cleanup.

---

# 24. Logging Policy for R7

Do **not** reduce current diagnostics yet.

R7 is the highest-risk transaction refactor and the project is still unreleased. Existing detailed logs are useful until the binding/lifecycle waves are complete.

Allowed:

- move existing binding logs with the transaction code;
- add a small number of stage/failure logs needed to distinguish preparation failure vs commit failure;
- keep current reason strings such as:
  - `swapchain_changed`
  - `queue_changed`
  - `mode_changed`
  - `multiple_fields_changed`
  - `new_hook_method_failed`
  - `new_hook_create_failed`
  - `external_bind_failed`
  - `rebind_failed`.

Do not:

- introduce the final debug setting here;
- downgrade/remove verbose logs here;
- rename the entire diagnostic vocabulary;
- delete logs because they look excessive to end users.

R11 will perform the release-facing logging pass:

```text
Debug Logging default OFF
normal log = concise support-critical events
Debug Logging ON = detailed discovery/bind/resize diagnostics
```

---

# 25. Error Handling Rules

R7 is compatibility code inside a game process. A XeFG compatibility failure must not crash the game merely because REFramework could not install its overlay hook.

Required behavior:

```text
invalid candidate
    -> fail XeFG bind cleanly

VtableHook construction throws
    -> catch
    -> old binding untouched
    -> return false

any required hook_method fails
    -> rollback prepared hook
    -> old binding untouched
    -> return false

same-object update precondition fails
    -> return false without mutation
```

Do not:

- terminate;
- throw across XeFG API boundary;
- spin/retry;
- schedule background retries;
- sleep and retry;
- partially commit a hook.

R5 pending consume semantics remain destructive-before-bind: a failed pending candidate is not automatically requeued.

---

# 26. Recommended Header Shape

Conceptual `D3D12Hook.hpp` additions/replacements:

```cpp
class XeFGCandidateHandoff;

class D3D12Hook {
public:
    friend class XeFGCandidateHandoff;

    // existing public API...

protected:
    struct PreparedXeFGBinding {
        Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain{};
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue{};
        Microsoft::WRL::ComPtr<ID3D12Device4> device{};
        std::unique_ptr<VtableHook> hook{};
        bool observe_only{true};
    };

    bool apply_xefg_candidate(
        const XeFGBindingCandidate& candidate);

    std::optional<PreparedXeFGBinding> prepare_xefg_binding(
        const XeFGBindingCandidate& candidate);

    bool bind_initial_xefg_candidate(
        const XeFGBindingCandidate& candidate);

    bool update_same_xefg_swapchain(
        const XeFGBindingCandidate& candidate,
        const char* reason);

    bool replace_xefg_swapchain(
        const XeFGBindingCandidate& candidate,
        const char* reason);

    void sync_xefg_binding_aliases() noexcept;

    // existing physical callbacks remain here
};
```

Exact access level and helper naming may differ.

Because the fork is unreleased, remove old raw XeFG-only methods if they become unused after the migration rather than keeping redundant wrappers.

Do not make these transaction methods broad public APIs.

---

# 27. Suggested `XeFGCandidateHandoff.cpp` End State

Conceptually the live path should become small:

```cpp
void XeFGCandidateHandoff::apply_to_live_hook(
    D3D12Hook& hook,
    const XeFGBindingCandidate& candidate) {
    if (!hook.apply_xefg_candidate(candidate)) {
        spdlog::warn(
            "[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = apply_failed",
            reinterpret_cast<uintptr_t>(candidate.swapchain.Get()));
    }
}
```

and pending consume:

```cpp
bool XeFGCandidateHandoff::consume_pending(D3D12Hook& hook) {
    std::optional<XeFGBindingCandidate> pending;
    {
        std::scoped_lock lock{s_pending_mutex};
        pending = std::move(s_pending_candidate);
        s_pending_candidate.reset();
    }

    if (!pending) {
        return false;
    }

    return hook.apply_xefg_candidate(*pending);
}
```

Preserve the R5 concurrency invariant:

```text
publish with framework available
    lifecycle mutex acquired
    current hook read under mutex
    if none -> pending stored before lifecycle mutex is released
```

R7 must not disturb this.

---

# 28. Suggested Binding-Gate Logging Using R6 Identity

Inside `apply_xefg_candidate()`:

```cpp
const auto change = m_xefg_binding.compare(
    candidate.swapchain.Get(),
    candidate.selected_queue.Get(),
    candidate.observe_only);

spdlog::info(
    "[XeFG][BindingGate] action = {}, reason = {}, "
    "old_swapchain = 0x{:x}, new_swapchain = 0x{:x}, "
    "old_queue = 0x{:x}, new_queue = 0x{:x}, "
    "old_observe_only = {}, new_observe_only = {}",
    change.changed() ? "rebind" : "unchanged",
    change.reason(),
    reinterpret_cast<uintptr_t>(m_xefg_binding.swapchain()),
    reinterpret_cast<uintptr_t>(candidate.swapchain.Get()),
    reinterpret_cast<uintptr_t>(m_xefg_binding.queue()),
    reinterpret_cast<uintptr_t>(candidate.selected_queue.Get()),
    m_xefg_binding.observe_only(),
    candidate.observe_only);
```

This removes the duplicate R5 `binding_change_reason()` implementation and uses the semantic identity owner added in R6.

---

# 29. State Commit Matrix

Use this table during implementation and review.

| Scenario | Prepare new hook? | Renderer reset? | Remove old hook? | Commit XeFGBinding? | Generation |
|---|---:|---:|---:|---:|---:|
| invalid candidate | no | no | no | no | unchanged |
| no active XeFG, prepare fails | yes/attempt | no | no | no | unchanged |
| no active XeFG, initial bind succeeds | yes | only if replacing active non-XeFG | yes if old hook exists | `commit_initial` | `1` |
| active XeFG, identical candidate | no | no | no | no | unchanged |
| active XeFG, same swapchain + queue change | no | once | no | `commit_same_swapchain_update` | `+1` |
| active XeFG, same swapchain + mode change | no | once | no | `commit_same_swapchain_update` | `+1` |
| active XeFG, changed swapchain prepare fails | yes/attempt | no | no | no | unchanged |
| active XeFG, changed swapchain succeeds | yes | once | yes | `commit_replacement` | `+1` |
| unhook | n/a | existing behavior | hook removed first | `clear` after hook removal | `0` |

No other generation transitions should be introduced in R7.

---

# 30. Static Review Scenarios

Review the code path-by-path, not only by diff appearance.

## Scenario A — First validated XeFG candidate before D3D12 hook creation

```text
R5 stores pending candidate
D3D12Hook::hook()
consume_pending()
apply_xefg_candidate()
prepare all five slots
commit initial binding
generation = 1
```

Expected:

- no lost candidate;
- no pending mutex held during physical bind;
- all-five-method gate applies to initial bind.

## Scenario B — Live native hook -> first XeFG candidate

Expected:

```text
prepare XeFG hook completely
only then reset native renderer
only then remove old physical hooks
commit XeFG binding
```

If prepare fails, native hook/renderer state is not destructively changed by R7.

## Scenario C — Identical active XeFG candidate

Expected:

- no renderer reset;
- no VtableHook recreation;
- no generation increment;
- no resize-state clear.

## Scenario D — Same swapchain, changed selected queue

Expected:

- existing instance hook retained;
- renderer reset once;
- queue/device ownership updated;
- aliases synchronized;
- generation +1;
- resize hold cleared.

## Scenario E — Same swapchain, changed observe-only mode

Same as D except mode changes.

## Scenario F — New swapchain, full hook preparation succeeds

Expected exact order:

```text
new strong COM refs
new VtableHook complete
renderer reset
old hook removed while old binding still owns target
new semantic binding commit
aliases sync
new hook commit
generation +1
```

## Scenario G — New swapchain, one hook_method fails

Expected:

- partial new hook rolled back;
- old binding remains usable;
- old hook remains installed;
- generation unchanged;
- renderer not reset;
- aliases unchanged;
- resize state unchanged.

## Scenario H — New VtableHook construction throws

Same preservation requirements as G.

## Scenario I — `unhook()` after R7

R6 lifetime order must remain:

```text
m_present_hook.reset()
m_swapchain_hook.reset()
clear raw aliases that alias XeFGBinding
m_xefg_binding.clear()
```

Do not regress this while reorganizing transaction helpers.

---

# 31. Native / Upstream Compatibility Audit

This project exists to preserve REFramework behavior while adding XeFG compatibility.

Before considering R7 complete, inspect the diff specifically for native impact.

Required checks:

```text
No change to D3D11
No change to native swapchain discovery
No change to dummy D3D12 discovery
No change to Streamline hook logic
No change to FSRFG detection
No change to public renderer callback registration
No change to Lua/plugin/mod paths
No change to normal phase-1 Present hook behavior
No generic DXGI event bus
```

If `bind_external_swapchain()` is split/refactored, compare the non-XeFG branch line-by-line with pre-R7 master.

Avoid cleanup in that branch even if it looks ugly. Native behavior matters more than stylistic consistency.

---

# 32. Required Build / Static Validation

Minimum commands:

```text
cmake -S . -B build
cmake --build build --config Release --target REFramework -- /m:4
git diff --check
```

Also perform source audits:

```text
1. search all bind_external_swapchain call sites
2. search all old replace_xefg_binding call sites if method is removed/renamed
3. verify all five XeFG instance slots are installed in exactly one preparation helper
4. verify initial bind checks all five hook_method() return values
5. verify changed-object failure paths do not call on_reset()
6. verify changed-object failure paths do not reset old m_swapchain_hook
7. verify old XeFGBinding survives until old hook removal
8. verify PreparedXeFGBinding destroys hook before swapchain ownership
9. verify same-object update does not recreate hook
10. verify generation transitions match the matrix above
11. verify R5 lifecycle/pending lock ordering is unchanged
12. verify MHW-only resize condition unchanged
13. verify REFramework::hook_monitor() untouched
14. verify no logging cleanup / Debug Logging UI added
```

If a direct-struct-access CI audit exists, it must also pass.

---

# 33. Runtime Wave Gate After R7 — Required Before Moving On

R7 is one of the designated runtime-wave checkpoints. Do not treat build success as enough.

Primary test:

```text
Dragon's Dogma 2
+ REFramework latest R7 build
+ OptiScaler
+ Intel XeFG output
```

Minimum runtime checks:

1. game launches normally;
2. XeFG initializes;
3. REFramework overlay appears;
4. OptiScaler overlay appears;
5. no startup crash;
6. no repeated destructive bind/rebind loop;
7. selected presentation queue remains the R4-proven queue;
8. active binding generation reaches `1` on initial bind;
9. Present/Present1 continue forwarding;
10. native REFramework features used in the smoke test still work.

Recommended lifecycle smoke:

```text
Alt+Tab out/in several times
window/fullscreen transition if the game supports it safely
```

Watch for:

- crash/hang;
- overlay disappearing permanently;
- repeated `new_binding_committed` loops;
- generation increasing continuously without real identity change;
- hook-monitor destructive recovery while a healthy binding exists.

Do not claim a rebind path was runtime-tested unless logs actually show the relevant identity change/rebind event.

### Multi-runtime wave when available

For Pragmata or another known multi-runtime case, if currently testable:

- exact-HMODULE runtime dispatch remains correct;
- candidate from the active runtime reaches R7;
- no cross-runtime original-function corruption;
- first valid binding still succeeds.

If unavailable, record **not tested**, not PASS.

### MHW optional regression smoke

If Monster Hunter Wilds is readily available, a short smoke is useful because PR #20 has a game-specific ResizeTarget hold. R7 must not alter that policy.

Do not make MHW runtime availability a merge blocker if DD2 is the established primary test environment.

---

# 34. Merge-Blocking Findings for R7 Review

Treat the following as blocking:

- initial XeFG bind commits active state before all five hook slots succeed;
- any required slot result is ignored on initial bind;
- renderer reset occurs before new changed-object hook preparation succeeds;
- old `m_swapchain_hook` is removed before new changed-object hook is complete;
- old `XeFGBinding` ownership is cleared/replaced before old hook removal;
- prepared new swapchain COM lifetime can end before prepared VtableHook destruction;
- failed prepare mutates aliases/generation/mode/source/resize state;
- same-object update recreates the physical hook without a proven need;
- identical candidate increments generation or resets renderer;
- queue policy is re-evaluated differently from R4;
- public XeFG proxy becomes renderer target;
- hook slots change from 8/13/14/22/39;
- native D3D12 branch behavior changes materially;
- R5 lost-candidate race protection is weakened;
- PR #20 MHW-only resize hold is broadened or removed;
- hook-monitor policy changes;
- R8/R9/R10/R11 work is mixed in.

---

# 35. Non-Blocking / Theoretical Findings Guidance

Per project review policy, do not block R7 for purely theoretical concerns with no realistic harmful path.

Examples generally non-blocking unless concrete impact is demonstrated:

- helper naming/style preferences;
- whether transaction helpers are nested vs private free helpers when lifetime/order is correct;
- slightly different but equivalent stage-log placement;
- speculative callback races already serialized by the existing lifecycle mutex;
- generalized exception-safety improvements unrelated to the actual XeFG transaction;
- abstract desire for a generic hook-manager framework.

For review findings, classify them explicitly as:

```text
blocking
non-blocking
or theoretical
```

A blocker should describe an actual failure sequence.

---

# 36. Forbidden Scope Expansion

Do not add in R7:

- `IFrameGenerationProvider`;
- generic frame-generation registry;
- FSRFG compatibility abstraction;
- DLSSG/Streamline abstraction;
- new worker thread;
- polling;
- timers;
- sleep/retry;
- Release hook solely for lifetime;
- public XeFG proxy binding;
- Intel-private offsets;
- OptiScaler-private class/symbol hooks;
- resize lifecycle object;
- Present callback refactor;
- hook-monitor façade;
- final Debug Logging option;
- general log pruning;
- unrelated C++ modernization.

---

# 37. Expected PR Size

R7 is intentionally the largest/highest-risk remaining transaction PR.

Planning target:

```text
effective implementation: ~220–340 LOC
GitHub add+delete:          ~350–650 LOC
```

Because the fork is unreleased, a somewhat larger diff is acceptable if it cleanly removes temporary R5 bridge logic and produces one obvious transaction boundary.

Do **not** split the changed-object safety transaction in half merely to hit an LOC target.

If the PR grows substantially beyond this because resize/callback/hook-monitor work is entering the diff, stop and move that work back to R8/R9/R10.

---

# 38. Recommended PR Description Template

```markdown
## Summary

- Centralize XeFG active mutation behind one candidate-driven D3D12Hook transaction.
- Prepare all five XeFG instance hooks before destructive initial bind or changed-object replacement.
- Preserve old binding/hook/renderer state when hook preparation fails.
- Keep same-swapchain queue/mode updates on the existing physical hook.
- Reduce XeFGCandidateHandoff back to delivery/pending ownership only.

## Safety invariants

- New swapchain/queue/device are strongly owned before VtableHook preparation.
- Present[8], ResizeBuffers[13], ResizeTarget[14], Present1[22], and ResizeBuffers1[39] must all hook successfully before commit.
- Renderer reset and old-hook removal happen only after successful preparation.
- Old XeFG COM ownership survives until the old VtableHook is removed.
- Failed preparation leaves binding identity, aliases, generation, hooks, renderer, and resize state untouched.

## Out of scope

- Resize lifecycle extraction (R8)
- Present/resize callback cleanup (R9)
- Hook-monitor cleanup (R10)
- Logging/Debug Logging UI cleanup (R11)
- FSRFG/DLSSG/general FG abstraction

## Validation

- `cmake -S . -B build`
- `cmake --build build --config Release --target REFramework -- /m:4`
- `git diff --check`
- call-site and five-slot static audits
- DD2 + OptiScaler + XeFG runtime wave: <PASS / NOT RUN>
- multi-runtime/Pragmata wave: <PASS / NOT RUN>
```

---

# 39. Completion Checklist

Implementation is complete only when all applicable items are satisfied:

- [ ] latest master was used as the implementation base;
- [ ] one `apply_xefg_candidate()`-style physical mutation boundary exists;
- [ ] R5 handoff no longer duplicates binding identity policy;
- [ ] R5 lost-candidate lifecycle ordering is preserved;
- [ ] non-XeFG -> XeFG reset is inside the post-preparation commit phase;
- [ ] new candidate COM objects are strongly owned during preparation;
- [ ] all five required hook slots are checked on initial bind;
- [ ] all five required hook slots are checked on changed-object replacement;
- [ ] partial preparation rolls back before old state mutation;
- [ ] prepared hook destructs before prepared target COM release;
- [ ] same-object update keeps current VtableHook;
- [ ] identical candidate causes no mutation;
- [ ] old active XeFG ownership survives old-hook removal;
- [ ] generation increments exactly once per successful real binding change;
- [ ] initial generation is exactly 1;
- [ ] raw aliases synchronize only at semantic commit;
- [ ] failed prepare leaves aliases unchanged;
- [ ] failed prepare leaves resize state unchanged;
- [ ] native/general D3D12 binding path is unchanged or proven equivalent;
- [ ] Present/Present1 behavior unchanged;
- [ ] resize behavior unchanged;
- [ ] MHW-only hold condition unchanged;
- [ ] hook-monitor behavior unchanged;
- [ ] no logging cleanup or Debug Logging UI work included;
- [ ] Release build passes;
- [ ] `git diff --check` passes;
- [ ] existing CI/static audits pass;
- [ ] DD2 + OptiScaler + XeFG runtime wave performed and result recorded, or explicitly marked NOT RUN before merge decision;
- [ ] no unrelated cleanup included.

---

# 40. Stop Condition Before R8

Do not start R8 until R7 has been reviewed and the R7 runtime wave is acceptable.

R8 begins only after we are confident that:

```text
validated candidate
-> live/pending handoff
-> initial physical bind
-> same-object update
-> changed-object transactional replacement
```

is structurally isolated and still works with OptiScaler + XeFG.

If R7 runtime testing shows:

- startup crash;
- REFramework overlay missing;
- OptiScaler overlay missing;
- repeated rebind loop;
- binding generation drift;
- failure to preserve old binding after preparation failure;
- new hook-monitor recovery behavior;

stop and fix R7. Do not attempt to hide the regression inside R8 resize-state work.

---

# 41. Final R7 Architectural End State

After R7, the intended ownership chain is:

```text
XeFGRuntimeRegistry           R1
    -> exact API dispatch

XeFGCompatibility            R2
    -> loader/probe handoff

XeFGDiscovery                R3 + R4
    -> observe internal XeFG presentation object
    -> validate queue/device/HWND
    -> build strong candidate

XeFGCandidateHandoff         R5, simplified by R7
    -> deliver candidate now
    -> or retain one pending candidate

D3D12Hook R7 transaction
    -> apply_xefg_candidate(candidate)
       -> unchanged
       -> same-object semantic update
       -> prepared initial bind
       -> prepared changed-object replacement

XeFGBinding                  R6
    -> active strong COM ownership
    -> generation/mode/identity

D3D12Hook
    -> physical VtableHook
    -> physical callbacks
    -> legacy raw aliases
    -> current resize/Present lifecycle until R8/R9
```

The central safety rule remains:

```text
new path is fully prepared before old path is destroyed,
and every VtableHook target remains strongly owned for the entire lifetime of that hook.
```

That is the R7 merge gate.