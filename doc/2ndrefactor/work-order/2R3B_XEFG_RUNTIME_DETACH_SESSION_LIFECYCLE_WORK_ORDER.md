# Work Order — 2R3B: Move XeFG Runtime-Detach Semantic Lifecycle Behind `XeFGPresentationSession`

Date: 2026-09-09  
Repository: `onehoon/REFramework`  
PR base branch: `REFforXeFG`  
Planning baseline reviewed: `REFforXeFG` @ `f7b96d103803c6c670ea33fbf5007974b0a09f5f`  
Architecture authority: `doc/2ndrefactor/REFramework_OPTISCALER_XEFG_SECOND_STAGE_REFACTOR_DESIGN_2026-09-09.md`  
Previous steps: 2R1, 2R2, and 2R3A merged

---

## 1. Objective

Implement the next deliberately small step of the second-stage OptiScaler + Intel XeFG refactor.

This PR is **2R3B**, the runtime-transition half of the broader conceptual 2R3.

The goal is:

> Make `XeFGPresentationSession` own runtime-identity matching, detached-uncertainty lifecycle state, and Destroy-result reconciliation, while leaving all physical D3D12 teardown/reset mechanics in `D3D12Hook` exactly where they are today.

This is a behavior-preserving refactor.

Do **not** move the high-level hook-monitor recovery action classifier in this PR. `XeFGCompatibility::evaluate_hook_monitor_timeout()` should remain behaviorally and structurally unchanged unless a tiny compile-only adjustment is unavoidable.

The PR should stay small enough that the runtime-detach behavior can be reviewed line-by-line against the current implementation.

---

## 2. Why This Is a Separate PR

2R3A already moved these monitor/session mechanics behind `XeFGPresentationSession`:

- active binding consistency interpretation;
- monitor-state presence;
- detached uncertainty read;
- monitor key construction;
- timeout sampling/counting;
- monitor-action deduplication;
- monitor reset.

The remaining runtime-transition logic still lives directly inside `D3D12Hook`:

```cpp
detach_xefg_binding_for_runtime_transition(...)
note_xefg_destroy_result(...)
```

That code currently mixes two different responsibilities:

```text
semantic lifecycle policy
    runtime identity match
    same-HWND fallback match
    detached-state creation
    matching Destroy-result acceptance
    detached-state clearing

physical D3D12 mechanism
    renderer reset
    bounded swapchain keepalive
    present/swapchain hook removal
    raw alias clearing
    source/hooked/phase state reset
```

2R3B separates those responsibilities without changing their order or meaning.

---

## 3. Branch / PR Rules

Create a fresh implementation branch from the current `REFforXeFG` tip after this work-order commit is present.

Suggested branch name:

```text
refactor/xefg-2r3b-runtime-detach-session
```

Open the PR against:

```text
base: REFforXeFG
```

Do not target `master`.

Do not rebase onto upstream `praydog/REFramework` as part of this PR.

Keep the PR focused. Intermediate architecture may remain transitional because this branch is unreleased.

---

## 4. Current Code Reviewed

At baseline `f7b96d103803c6c670ea33fbf5007974b0a09f5f`, `D3D12Hook::detach_xefg_binding_for_runtime_transition()` currently performs all of the following itself:

1. snapshot the active `XeFGBinding`;
2. evaluate exact runtime match;
3. evaluate optional same-HWND match;
4. reject if no match;
5. reject if source is not `XeFGInternal`;
6. reject if semantic binding is not active;
7. create `XeFGDetachedState`;
8. clear XeFG monitor state;
9. take a bounded local `ComPtr<IDXGISwapChain3>` keepalive;
10. reset the renderer;
11. clear XeFG resize-transition hold;
12. remove physical Present/swapchain hooks;
13. clear matching renderer-facing raw aliases;
14. clear semantic XeFG binding.

Current Destroy reconciliation then does:

```text
if detached state inactive
OR result < 0
OR runtime slot mismatch
OR runtime context mismatch
    -> do nothing

otherwise
    -> log matching Destroy success
    -> clear detached state
    -> set swapchain source Native
    -> set hooked false
    -> set phase1 true
    -> clear raw swapchain/queue/device aliases
    -> clear monitor state
```

These semantics are the required baseline.

---

## 5. Non-Negotiable Scope

This PR is XeFG runtime-transition lifecycle only.

Do not intentionally modify:

- D3D11;
- DLSSG / Streamline;
- FSRFG;
- native D3D12 discovery;
- native Present / Present1 behavior;
- XeFG Present / Present1 behavior;
- ResizeBuffers / ResizeTarget / ResizeBuffers1 behavior;
- MHW-specific resize-hold policy;
- candidate handoff behavior;
- pending-candidate lock behavior;
- initial XeFG bind transaction;
- XeFG rebind transaction;
- queue/device selection;
- COM ownership model;
- physical hook ownership;
- Intel runtime hook installation;
- loader behavior;
- anti-tamper/integrity behavior;
- hook-monitor watchdog timing;
- hook-monitor recovery action ordering;
- runtime-transition depth implementation.

Do not introduce a generic frame-generation lifecycle abstraction.

Do not move `m_swapchain_hook` or `m_present_hook` into `XeFGPresentationSession`.

---

# 6. Target Responsibility Boundary

After this PR:

```text
XeFGPresentationSession
    owns/decides:
        runtime identity matching
        exact-runtime vs same-HWND match classification
        whether semantic detach is eligible
        detached uncertainty creation
        detached-state runtime/generation/reason
        matching Destroy-result reconciliation
        detached uncertainty clearing
        monitor-state clearing associated with semantic transitions
        semantic binding clear at the same current lifecycle point

D3D12Hook
    continues to execute:
        physical renderer reset
        bounded local old-swapchain keepalive
        resize-hold clear call
        m_present_hook reset
        m_swapchain_hook reset
        renderer-facing raw alias clearing
        m_swapchain_source physical state reset
        m_hooked / m_is_phase_1 physical state reset
```

`XeFGCompatibility` remains the runtime façade/orchestrator and continues to enforce lifecycle locking and vendor-call boundaries.

---

# 7. Add a Narrow Runtime-Detach Evaluation Type

Add a XeFG-specific result type under `XeFGPresentationSession`.

Recommended conceptual shape:

```cpp
class XeFGPresentationSession {
public:
    enum class RuntimeDetachMatch : uint8_t {
        None,
        ExactRuntime,
        SameHwnd,
    };

    struct RuntimeDetachEvaluation {
        bool accepted{};
        RuntimeDetachMatch match{RuntimeDetachMatch::None};
        XeFGBinding::RuntimeLifecycleSnapshot binding{};
    };

    ...
};
```

Exact names may differ.

Do not expose `D3D12Hook::SwapchainSource` through this type.

The session should receive a simple `bool xefg_source` from `D3D12Hook` rather than depending on the generic D3D12 enum.

---

# 8. Runtime-Detach Match Semantics Must Remain Exact

Move the current matching rules into a session method such as:

```cpp
RuntimeDetachEvaluation evaluate_runtime_detach(
    size_t runtime_slot,
    void* context,
    HWND hwnd,
    bool allow_same_hwnd_match,
    bool xefg_source) const noexcept;
```

Required semantics:

```cpp
const auto snapshot = m_binding.lifecycle_snapshot();

const bool exact_runtime_match = snapshot.active
    && context != nullptr
    && snapshot.runtime.slot == runtime_slot
    && snapshot.runtime.context == context;

const bool same_hwnd_match = allow_same_hwnd_match
    && hwnd != nullptr
    && snapshot.active
    && snapshot.runtime.hwnd == hwnd;
```

Acceptance must remain equivalent to current code:

```text
accepted only when:
    (exact runtime match OR allowed same-HWND match)
    AND xefg_source == true
    AND active semantic binding exists
```

Exact-runtime match must continue to take precedence over same-HWND when both are true.

Do not broaden matching to:

- HWND-only matching when `allow_same_hwnd_match == false`;
- slot-only matching;
- context-only matching without slot;
- swapchain pointer matching;
- queue/device matching;
- public proxy identity.

Do not make null context an exact-runtime match.

---

# 9. Preserve Current Diagnostic Ordering

Current detach flow logs the `evaluate` stage before detached state is mutated.

Prefer preserving that ordering.

A good pattern is two-phase semantic handling:

```cpp
const auto evaluation = m_xefg_session.evaluate_runtime_detach(...);

// Existing evaluate log using evaluation.binding + evaluation.match.

if (!evaluation.accepted) {
    return false;
}

m_xefg_session.begin_runtime_detach(evaluation, reason);
```

`begin_runtime_detach(...)` should only perform semantic state mutation:

```text
set detached state active
copy previous runtime identity
copy previous generation
store current reason string pointer
clear monitor timeout/action state
```

It must not:

- reset renderer state;
- remove hooks;
- clear D3D12 raw aliases;
- call Intel APIs;
- AddRef the active swapchain persistently.

The exact API shape may differ, but avoid duplicating runtime-match logic in both session and `D3D12Hook`.

---

# 10. Preserve Physical Detach Ordering Exactly

After semantic detach is accepted/recorded, `D3D12Hook` must continue to execute the current physical sequence.

Required order:

```text
accepted semantic detach
-> clear monitor state as part of semantic detach recording
-> create bounded local ComPtr keepalive from the borrowed active swapchain
-> renderer reset if g_framework != nullptr
-> clear XeFG resize-transition hold("runtime_transition")
-> reset m_present_hook
-> reset m_swapchain_hook
-> clear raw swapchain alias only if it equals old binding swapchain
-> clear raw queue alias only if it equals old binding queue
-> clear raw device alias only if it equals old binding device
-> clear semantic active XeFG binding
-> return true
```

The bounded keepalive must remain local to `D3D12Hook`.

Do not move it into `XeFGPresentationSession` as persistent state.

Do not clear semantic binding before physical hook removal.

Do not remove the conditional raw-alias equality checks.

---

# 11. Add a Session Method for Final Semantic Binding Clear

After physical hook removal and raw alias clearing, the semantic binding should be cleared through the session.

A narrow method is preferred, for example:

```cpp
void complete_runtime_detach() noexcept {
    m_binding.clear();
}
```

This should preserve current generation-reset semantics because `XeFGBinding::clear()` remains unchanged.

Do not clear detached uncertainty here.

After a pre-Destroy/pre-reinit detach, detached uncertainty must remain active until a later lifecycle event resolves it.

---

# 12. Move Destroy-Result Reconciliation Into the Session

The session should decide whether a Destroy result resolves detached uncertainty.

Current semantic rule is:

```text
resolve only if:
    detached state is active
    AND XeFG result is non-failure (result >= 0)
    AND previous runtime slot == supplied slot
    AND previous runtime context == supplied context
```

A negative result must leave detached uncertainty untouched.

Use the existing `XeFGResult.hpp` semantics.

Do not regress to:

```cpp
result == 0
```

or:

```cpp
SUCCEEDED(result)
```

XeFG result contract remains:

```text
result >= 0 -> non-failure
result < 0  -> failure
```

---

# 13. Prefer Two-Phase Destroy Reconciliation to Preserve Logging

Current code logs the matching Destroy success before clearing the detached state.

Prefer a small evaluation/commit pair so diagnostics remain ordered as today.

Conceptual API:

```cpp
struct DestroyReconciliation {
    bool accepted{};
    uint64_t previous_generation{};
};

DestroyReconciliation evaluate_destroy_result(
    size_t runtime_slot,
    void* context,
    int32_t result) const noexcept;

void commit_destroy_reconciliation(
    const DestroyReconciliation& reconciliation) noexcept;
```

`evaluate_destroy_result(...)` must be read-only.

`commit_destroy_reconciliation(...)` should only:

```text
clear detached state
clear monitor state
```

It must not manipulate:

- `m_swapchain_source`;
- `m_hooked`;
- `m_is_phase_1`;
- renderer-facing aliases;
- physical hooks.

Those remain `D3D12Hook` responsibilities.

If implementation can preserve the same log ordering with a smaller equivalent API, that is acceptable.

---

# 14. `D3D12Hook::note_xefg_destroy_result()` Becomes a Thin Physical Bridge

Keep the method for this PR so `XeFGCompatibility.cpp` does not need to change unnecessarily.

Target conceptual shape:

```cpp
void D3D12Hook::note_xefg_destroy_result(
    size_t runtime_slot,
    void* context,
    int32_t result) noexcept {

    const auto reconciliation =
        m_xefg_session.evaluate_destroy_result(runtime_slot, context, result);

    if (!reconciliation.accepted) {
        return;
    }

    // Preserve existing log and previous_generation value.

    m_xefg_session.commit_destroy_reconciliation(reconciliation);

    // Physical D3D12 state remains owned here.
    m_swapchain_source = SwapchainSource::Native;
    m_hooked = false;
    m_is_phase_1 = true;
    m_swap_chain = nullptr;
    m_command_queue = nullptr;
    m_device = nullptr;
}
```

Do not move those physical assignments into the session.

Do not restore hooks on negative Destroy.

---

# 15. `XeFGCompatibility.cpp` Should Ideally Remain Unchanged

The current runtime transition orchestration is correct and should be preserved.

In particular:

```text
RuntimeTransitionScope begins
-> lifecycle logging
-> acquire hook-monitor/lifecycle mutex
-> discard matching pending candidate
-> detach active XeFG binding
-> release lifecycle mutex
-> call Intel Init/Destroy vendor API
-> for Destroy, reacquire lifecycle mutex after vendor return
-> reconcile Destroy result
-> release mutex
-> RuntimeTransitionScope ends at function return
```

Do not hold the lifecycle mutex across Intel vendor calls.

Do not invert lock order.

Required lock direction remains:

```text
hook-monitor/lifecycle mutex
    -> pending-candidate mutex
```

`XeFGCandidateHandoff::discard_pending_for_runtime_transition()` behavior must remain unchanged, including COM releases after pending-mutex unlock.

Because the existing public bridge methods can remain, `XeFGCompatibility.cpp` should not need functional edits in 2R3B.

If it changes, explain why in the PR description and keep the diff minimal.

---

# 16. Hook-Monitor Recovery Classification Is Out of Scope

Do not move or rewrite this ordering in `XeFGCompatibility::evaluate_hook_monitor_timeout()`:

```text
runtime transition active
-> suppress runtime transition

no XeFG monitor state
-> allow generic recovery

detached uncertain
-> suppress detached uncertain

inconsistent active binding
-> quarantine inconsistent state

sustained timeout
-> quarantine sustained timeout

otherwise
-> preserve grace
```

Do not change:

- action enum values;
- reason strings;
- monitor action names;
- timeout thresholds;
- generic watchdog cadence;
- REFramework.cpp recovery timing.

A later cleanup may reduce the remaining wrapper surface, but not in this PR unless required by compilation.

---

# 17. COM Ownership Invariants

These rules are mandatory.

## Active XeFG swapchain remains borrowed

Keep in `XeFGBinding`:

```cpp
IDXGISwapChain3* m_swapchain{}; // borrowed
```

Do not convert active binding storage to `ComPtr`.

## Queue/device remain strong

Keep:

```cpp
ComPtr<ID3D12CommandQueue>
ComPtr<ID3D12Device4>
```

## Detach keepalive remains bounded/local

Current local keepalive around physical teardown remains:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> old_keepalive = snapshot.swapchain;
```

Do not retain it after detach returns.

## Forbidden

Do not add:

- force-Release loops;
- refcount draining;
- manual AddRef balancing tricks;
- COM probing of a stale borrowed proxy;
- persistent swapchain ownership in the session.

---

# 18. Failure / Fail-Closed Rules

Preserve all fail-closed behavior.

### No matching runtime

Do not detach unrelated active XeFG state.

### Negative Destroy

Keep detached uncertainty active.

Do not:

- clear detached state;
- mark the D3D12 hook Native;
- re-enable generic recovery;
- restore the old swapchain hook.

### Mismatched successful Destroy

A non-negative Destroy result for a different slot/context must not clear the detached state belonging to another runtime identity.

### Re-init

Same-HWND matching remains allowed only where the current caller explicitly passes `allow_same_hwnd_match = true`.

Destroy continues to pass `false`.

---

# 19. Expected Files

Expected primary implementation files:

```text
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp
src/D3D12Hook.cpp
src/D3D12Hook.hpp        # only if declarations are required
```

Prefer **no functional change** to:

```text
src/compatibility/xefg/XeFGCompatibility.cpp
src/compatibility/xefg/XeFGCompatibility.hpp
src/compatibility/xefg/XeFGCandidateHandoff.cpp
src/compatibility/xefg/XeFGDiscovery.cpp
src/compatibility/xefg/XeFGBinding.cpp
src/compatibility/xefg/XeFGResizeLifecycle.cpp
src/REFramework.cpp
```

No new source file should be necessary.

Therefore no CMake regeneration should normally be required.

---

# 20. Static Review Checklist

Before opening the PR, verify all of the following.

### Runtime matching

- exact-runtime match still requires non-null context;
- exact match still checks both slot and context;
- same-HWND fallback still requires explicit allow flag;
- exact match still wins over same-HWND for diagnostics;
- non-XeFG physical source rejects detach;
- inactive semantic binding rejects detach.

### Detach ordering

- detached state is recorded before physical teardown;
- monitor state is cleared at the same lifecycle point;
- bounded old-swapchain keepalive exists before hook removal;
- renderer reset remains before hook removal;
- resize hold clear remains before hook removal;
- raw aliases clear only on equality with the old binding snapshot;
- semantic binding clears only after physical hook removal/alias cleanup.

### Destroy reconciliation

- negative result leaves detached state active;
- non-negative matching result clears detached state;
- mismatched slot/context does not clear it;
- previous generation used in log remains the same value;
- physical source/hooked/phase/raw aliases reset only after accepted reconciliation.

### Scope

- no Present/Resize changes;
- no bind/rebind changes;
- no candidate-handoff changes;
- no monitor action reorder;
- no runtime-transition depth change;
- no generic REFramework behavior change.

---

# 21. Build / Validation Requirements

Run at minimum:

```powershell
cmake -S . -B build-2r3b -G "Visual Studio 17 2022" -A x64
cmake --build build-2r3b --config Release --target REFramework --parallel 4
```

Then:

```powershell
$env:PYTHONUTF8='1'
python dev/audit_direct_access_clang.py
```

Also run:

```text
git diff --check
```

Audit the final diff to prove that unrelated D3D11, Streamline, Present/Resize, bind/rebind, candidate-handoff, and renderer code did not change.

If no runtime smoke test is possible in the implementation environment, state that explicitly in the PR description. Do not claim DD2/MHW/XeFG runtime validation that was not performed.

---

# 22. Runtime Test Gate

Because the user is currently away from the test machine, runtime testing is not required before opening this structural PR.

When hardware testing resumes, 2R3B should be included in the next runtime-transition smoke gate:

### Native protection

- REF only, XeFG absent;
- REF + OptiScaler present but XeFG not selected.

### XeFG

- DD2 cold launch and normal exit;
- MHW cold launch and normal exit;
- repeated Alt+Tab;
- resolution/fullscreen transition;
- clean game exit / XeFG Destroy path;
- no stale detached state causing permanent recovery suppression;
- no generic rehook while Destroy result is negative/uncertain;
- no crash during reinit/destroy.

Do not block code-only progression on this test while the user is external, but preserve the test debt explicitly.

---

# 23. PR Description Requirements

The PR description should state clearly:

```text
2R3B runtime-detach semantic lifecycle extraction only.

Runtime identity matching, detached-state creation, semantic binding detach completion,
and matching Destroy-result reconciliation now live behind XeFGPresentationSession.

D3D12Hook still owns and executes renderer reset, physical hook removal,
raw alias clearing, and generic D3D12 physical state reset.

No hook-monitor recovery-policy reorder, Present/Resize change, bind/rebind change,
candidate-handoff change, COM ownership change, or Intel vendor-call locking change.
```

Include exact build/static validation performed.

---

# 24. Stop Conditions

Stop and do not broaden the PR if implementation appears to require any of the following:

- changing Intel Init/Destroy call timing;
- holding the lifecycle mutex across the Intel vendor call;
- changing candidate-handoff lock order;
- changing active swapchain ownership;
- redesigning `XeFGBinding`;
- touching Present/Resize policy;
- touching bind/rebind transaction ordering;
- modifying generic D3D12 watchdog timing;
- changing hook-monitor recovery classification;
- adding timers, sleeps, polling, or worker threads;
- introducing a generic FG/session/provider abstraction.

If one of these becomes necessary for compilation, document the reason rather than silently expanding scope.

---

# 25. Definition of Done

2R3B is complete when:

1. runtime exact/same-HWND match policy is owned by `XeFGPresentationSession`;
2. detached uncertainty creation is owned by the session;
3. monitor-state reset associated with detach is owned by the session;
4. semantic active-binding clear after physical detach is performed through the session;
5. Destroy-result matching/non-failure reconciliation is owned by the session;
6. negative Destroy still leaves detached uncertainty fail-closed;
7. mismatched Destroy cannot clear another runtime's detached state;
8. `D3D12Hook` still owns renderer reset, physical hook removal, raw alias cleanup, and physical D3D12 state reset;
9. `XeFGCompatibility` vendor-call lock boundary remains unchanged;
10. active swapchain remains borrowed;
11. queue/device ownership remains unchanged;
12. Present/Resize and bind/rebind paths are untouched;
13. Release build passes;
14. direct-access audit passes;
15. `git diff --check` passes;
16. PR targets `REFforXeFG`.

The intended architectural result is:

> `XeFGPresentationSession` decides and records what XeFG runtime lifecycle transition is semantically occurring; `D3D12Hook` performs only the physical D3D12 teardown/reset required to enact that transition.
