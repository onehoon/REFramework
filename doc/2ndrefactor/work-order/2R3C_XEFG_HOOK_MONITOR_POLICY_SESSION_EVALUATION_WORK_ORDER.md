# Work Order — 2R3C: Move XeFG Hook-Monitor Recovery Classification Behind `XeFGPresentationSession`

Date: 2026-09-09  
Repository: `onehoon/REFramework`  
PR base branch: `REFforXeFG`  
Planning baseline reviewed: `REFforXeFG` @ `c8a28ed1eeac2d5791973e82dd3d9cea411b719c`  
Architecture authority: `doc/2ndrefactor/REFramework_OPTISCALER_XEFG_SECOND_STAGE_REFACTOR_DESIGN_2026-09-09.md`  
Previous steps: 2R1, 2R2, 2R3A, and 2R3B merged

---

## 1. Objective

Implement the final small sub-step of the conceptual **2R3** stage.

2R3A already moved XeFG monitor state mechanics behind `XeFGPresentationSession`.
2R3B moved runtime-detach semantic lifecycle and Destroy reconciliation behind the session.

One important part of the original 2R3 architecture goal still remains in `XeFGCompatibility.cpp`:

```cpp
XeFGCompatibility::evaluate_hook_monitor_timeout(D3D12Hook& hook)
```

currently reconstructs the XeFG recovery classification by calling several separate `D3D12Hook` XeFG helpers.

The goal of 2R3C is:

> Make `XeFGPresentationSession` perform one coherent XeFG hook-monitor recovery evaluation from a narrow physical/liveness input, while keeping the existing public `XeFGMonitorAction` contract and generic REFramework watchdog behavior unchanged.

After this PR, `XeFGCompatibility::evaluate_hook_monitor_timeout()` should become a thin compatibility/mapping/logging façade rather than the place that understands detached state, binding consistency, timeout sequencing, and XeFG monitor precedence.

This is a **behavior-preserving refactor**.

Do not start 2R4 Present/Present1 policy extraction in this PR.

---

## 2. Why 2R3C Exists

The second-stage design describes 2R3 as:

```text
Move Runtime-Detach and Hook-Monitor State Policy Behind the Session
```

The current branch has already completed most of that responsibility split:

### Completed in 2R3A

- monitor-state presence;
- detached uncertainty read;
- physical/semantic binding consistency interpretation;
- monitor key construction;
- timeout sampling/counting;
- monitor-action deduplication;
- monitor-state clear.

### Completed in 2R3B

- exact runtime match;
- optional same-HWND match;
- detached uncertainty creation;
- semantic detach completion;
- matching non-negative Destroy reconciliation;
- detached uncertainty clearing.

### Still remaining in `XeFGCompatibility.cpp`

The following precedence policy is still assembled outside the session:

```text
runtime transition active
-> suppress runtime-transition recovery

no XeFG monitor state
-> allow generic recovery

detached uncertain
-> suppress generic recovery

inconsistent active binding
-> quarantine

sustained timeout
-> quarantine

otherwise
-> preserve grace
```

That leaves the compatibility façade depending on several D3D12Hook XeFG internals even though the session already owns the state required to make the decision.

2R3C closes that remaining gap before moving to Present policy in 2R4.

---

## 3. Branch / PR Rules

Create a fresh implementation branch from the current `REFforXeFG` tip **after this work-order commit is present**.

Suggested branch:

```text
refactor/xefg-2r3c-monitor-policy-session
```

Open the PR against:

```text
base: REFforXeFG
```

Do not target `master`.

Do not rebase onto upstream `praydog/REFramework` as part of this PR.

Keep this PR small. The expected implementation should remain concentrated in:

```text
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp
src/D3D12Hook.hpp
src/D3D12Hook.cpp
src/compatibility/xefg/XeFGCompatibility.cpp
```

`XeFGCompatibility.hpp` should ideally remain unchanged because the existing external `XeFGMonitorAction` and `evaluate_hook_monitor_timeout(D3D12Hook&)` contract should remain stable.

`REFramework.cpp` should remain unchanged.

---

## 4. Current Code Reviewed

At baseline `c8a28ed1eeac2d5791973e82dd3d9cea411b719c`, the current classifier is:

```cpp
XeFGMonitorAction XeFGCompatibility::evaluate_hook_monitor_timeout(D3D12Hook& hook) noexcept {
    XeFGMonitorAction action = XeFGMonitorAction::AllowGenericRecovery;
    const char* reason = "xefg_state_safe";

    if (is_runtime_transition_active()) {
        action = XeFGMonitorAction::SuppressRuntimeTransition;
        reason = "runtime_transition";
    } else if (!hook.has_xefg_monitor_state()) {
        action = XeFGMonitorAction::AllowGenericRecovery;
    } else if (hook.has_xefg_detached_state()) {
        action = XeFGMonitorAction::SuppressDetachedUncertain;
        reason = "detached_uncertain";
    } else if (!hook.has_consistent_active_xefg_binding()) {
        action = XeFGMonitorAction::QuarantineInconsistentState;
        reason = "binding_identity_inconsistent";
    } else if (hook.note_xefg_monitor_timeout() == XeFGHookMonitorState::TimeoutClass::Sustained) {
        action = XeFGMonitorAction::QuarantineSustainedTimeout;
        reason = "sustained_present_timeout";
    } else {
        action = XeFGMonitorAction::PreserveGrace;
        reason = "present_timeout";
    }

    if (hook.note_xefg_monitor_action(reason) && is_debug_log_enabled()) {
        ...
    }

    return action;
}
```

This ordering and all result meanings are the exact behavioral baseline.

Current `XeFGPresentationSession` already provides the individual building blocks:

```cpp
has_monitor_state(...)
detached_uncertain()
consistent_with(...)
monitor_binding_key(...)
note_monitor_timeout(...)
monitor_timeout_count()
note_monitor_action(...)
clear_monitor_state()
```

Current `D3D12Hook` already knows how to build:

```cpp
XeFGPresentationSession::PhysicalBindingView
```

from the generic/physical D3D12 state.

Therefore this PR should mostly **compose existing session operations into one session-level evaluation**, not redesign them.

---

## 5. Non-Negotiable Scope

This PR is only the final hook-monitor policy consolidation for conceptual 2R3.

Do not intentionally modify:

- D3D11;
- Streamline / DLSSG;
- FSRFG;
- native D3D12 discovery;
- generic D3D12 watchdog cadence;
- `REFramework.cpp` 5-second / last-chance behavior;
- runtime-transition depth implementation;
- runtime detach / Destroy implementation from 2R3B;
- candidate handoff;
- pending-candidate lock ordering;
- XeFG discovery;
- initial binding;
- same-object rebind;
- changed-object rebind;
- COM ownership;
- Present / Present1 callback behavior;
- ResizeBuffers / ResizeBuffers1 / ResizeTarget behavior;
- MHW-specific resize hold;
- physical `m_swapchain_hook` ownership;
- physical `m_present_hook` ownership;
- Intel runtime calls;
- loader hooks;
- anti-tamper/integrity behavior.

Do not introduce a generic frame-generation monitor abstraction.

This remains explicitly XeFG-specific.

---

## 6. Preserve the Existing Public Recovery Contract

Keep the existing compatibility-facing enum unchanged:

```cpp
enum class XeFGMonitorAction : uint8_t {
    AllowGenericRecovery,
    PreserveGrace,
    SuppressRuntimeTransition,
    SuppressDetachedUncertain,
    QuarantineSustainedTimeout,
    QuarantineInconsistentState,
};
```

Keep the existing call surface used by the generic REFramework monitor:

```cpp
static XeFGMonitorAction evaluate_hook_monitor_timeout(D3D12Hook& hook) noexcept;
```

Do **not** require `REFramework.cpp` to know about a new session-specific enum or state structure.

Reason:

- the generic monitor already has a sufficiently narrow compatibility contract;
- the remaining architectural problem is inside the compatibility evaluation implementation;
- changing the outer contract adds no value in this small PR.

---

# 7. Add a Session-Level Monitor Evaluation Result

Add an XeFG-specific internal result type to `XeFGPresentationSession`.

Recommended conceptual shape:

```cpp
class XeFGPresentationSession {
public:
    enum class MonitorDisposition : uint8_t {
        AllowGenericRecovery,
        PreserveGrace,
        SuppressRuntimeTransition,
        SuppressDetachedUncertain,
        QuarantineSustainedTimeout,
        QuarantineInconsistentState,
    };

    struct MonitorEvaluation {
        MonitorDisposition disposition{MonitorDisposition::AllowGenericRecovery};
        const char* reason{"xefg_state_safe"};

        XeFGBinding::RuntimeLifecycleSnapshot binding{};
        XeFGMonitorBindingKey key{};

        uint64_t present_entry_count{};
        int64_t present_age_ms{-1};
        uint32_t timeout_count{};
        bool detached_uncertain{};
        bool action_changed{};
    };

    ...
};
```

Exact names may differ.

The result should contain enough already-evaluated diagnostic information that `XeFGCompatibility.cpp` does not need to reconstruct XeFG state by calling many separate D3D12Hook helper functions afterward.

Do not put generic D3D12 physical hook objects or COM ownership into this result.

A raw `hook_target` should continue to be represented through the existing monitor key / physical view only.

---

# 8. Add One Coherent Session Evaluation Method

Add a session method conceptually similar to:

```cpp
MonitorEvaluation evaluate_monitor_timeout(
    const PhysicalBindingView& physical,
    bool runtime_transition_active,
    uint64_t present_entry_count,
    int64_t present_age_ms) noexcept;
```

The session receives only the narrow information needed to decide XeFG recovery policy:

```text
semantic XeFG state owned by the session
+
PhysicalBindingView supplied by D3D12Hook
+
runtime-transition-active boolean supplied by compatibility/D3D12 bridge
+
Present liveness values supplied by D3D12Hook
```

The session must not access `D3D12Hook` directly.

Do not add a `D3D12Hook*` back-reference to the session.

---

# 9. Recovery Classification Ordering Must Be Bit-for-Bit Equivalent in Meaning

Implement the current precedence **exactly**.

Required order:

```text
1. runtime transition active
2. no XeFG monitor state
3. detached uncertainty
4. inconsistent active binding
5. sustained timeout
6. grace timeout
```

Conceptually:

```cpp
MonitorEvaluation result{};
result.present_entry_count = present_entry_count;
result.present_age_ms = present_age_ms;

if (runtime_transition_active) {
    result.disposition = MonitorDisposition::SuppressRuntimeTransition;
    result.reason = "runtime_transition";
} else if (!has_monitor_state(physical.xefg_source)) {
    result.disposition = MonitorDisposition::AllowGenericRecovery;
    result.reason = "xefg_state_safe";
} else if (detached_uncertain()) {
    result.disposition = MonitorDisposition::SuppressDetachedUncertain;
    result.reason = "detached_uncertain";
} else if (!consistent_with(physical)) {
    result.disposition = MonitorDisposition::QuarantineInconsistentState;
    result.reason = "binding_identity_inconsistent";
} else if (note_monitor_timeout(
               physical.hook_target,
               present_entry_count,
               present_age_ms)
           == XeFGHookMonitorState::TimeoutClass::Sustained) {
    result.disposition = MonitorDisposition::QuarantineSustainedTimeout;
    result.reason = "sustained_present_timeout";
} else {
    result.disposition = MonitorDisposition::PreserveGrace;
    result.reason = "present_timeout";
}
```

Do not reorder detached-vs-consistency checks.

Do not sample/increment timeout state when:

- runtime transition is active;
- there is no XeFG monitor state;
- detached uncertainty is active;
- the active physical/semantic identity is inconsistent.

That matches current behavior because `note_xefg_monitor_timeout()` is currently reached only after all preceding branches pass.

---

# 10. Preserve All Existing Reason Strings

Use the exact current reason strings:

```text
xefg_state_safe
runtime_transition
detached_uncertain
binding_identity_inconsistent
sustained_present_timeout
present_timeout
```

Do not rename them for style cleanup.

These strings are part of existing diagnostics and monitor-action dedup behavior.

The session currently deduplicates monitor actions via the reason pointer passed to `note_monitor_action()`.

Keep equivalent stable behavior.

A straightforward implementation is for `evaluate_monitor_timeout()` to select one of the above string literals and then call:

```cpp
result.action_changed = note_monitor_action(result.reason);
```

on every evaluation, just as the old compatibility function always executed `hook.note_xefg_monitor_action(reason)` before testing whether debug logging was enabled.

Do not move action-state mutation behind a debug-log conditional.

---

# 11. Preserve Timeout Semantics Exactly

Do not modify `XeFGHookMonitorState::note_timeout()` thresholds or state transitions.

Current contract remains:

```text
new monitor key
OR Present entry count changed
    -> consecutive timeout count = 1
    -> Grace

same key + no Present progress
    -> increment consecutive timeout count

Sustained only when:
    consecutive_timeouts >= 3
    AND present_age_ms >= 20000
```

Do not change:

```cpp
kSustainedTimeoutThreshold = 3
kMinimumSustainedPresentAgeMs = 20000
```

Do not add elapsed timers, sleeps, retries, COM probes, or extra Present sampling.

---

# 12. Capture Diagnostics in the Session Result

After classification, populate the result with the same semantic diagnostic values currently logged by `XeFGCompatibility.cpp`.

At minimum:

```cpp
result.binding = m_binding.lifecycle_snapshot();
result.key = monitor_binding_key(physical.hook_target);
result.timeout_count = monitor_timeout_count();
result.detached_uncertain = detached_uncertain();
```

The ordering should preserve current observable values.

Recommended sequence:

```text
perform classification
-> update monitor-action dedup state
-> snapshot binding/key/count/detached state for logging
-> return evaluation
```

Do not mutate binding or detached state during monitor evaluation.

Only monitor timeout/action tracking may mutate, exactly as current behavior already does.

---

# 13. Add One Thin D3D12Hook Bridge

`D3D12Hook` should continue to own the generic physical data needed by the session.

Add one narrow bridge, conceptually:

```cpp
XeFGPresentationSession::MonitorEvaluation
D3D12Hook::evaluate_xefg_monitor_timeout(bool runtime_transition_active) noexcept {
    return m_xefg_session.evaluate_monitor_timeout(
        get_xefg_physical_binding_view(),
        runtime_transition_active,
        m_present_entry_count.load(std::memory_order_relaxed),
        get_last_present_age_ms());
}
```

The bridge must not contain recovery policy.

It only gathers physical/liveness inputs and delegates to the session.

Do not pass the entire `D3D12Hook` object into the session.

---

# 14. Simplify `XeFGCompatibility::evaluate_hook_monitor_timeout()`

The compatibility function should become approximately:

```cpp
XeFGMonitorAction XeFGCompatibility::evaluate_hook_monitor_timeout(D3D12Hook& hook) noexcept {
    const auto evaluation =
        hook.evaluate_xefg_monitor_timeout(is_runtime_transition_active());

    const auto action = map_monitor_disposition(evaluation.disposition);

    if (evaluation.action_changed && is_debug_log_enabled()) {
        const auto& binding = evaluation.binding;

        spdlog::info(
            "[XeFG][HookMonitor] action = {}, reason = {}, ...",
            monitor_action_name(action),
            evaluation.reason,
            ...);

        hook.log_hook_monitor_snapshot("xefg_monitor_decision");
    }

    return action;
}
```

Exact implementation may differ, but the compatibility function should no longer perform this detailed state branching itself:

```cpp
hook.has_xefg_monitor_state()
hook.has_xefg_detached_state()
hook.has_consistent_active_xefg_binding()
hook.note_xefg_monitor_timeout()
hook.note_xefg_monitor_action(...)
```

Those decisions belong behind the session after this PR.

---

# 15. Keep the Existing `XeFGMonitorAction` Mapping Explicit

Because the external enum remains in `XeFGCompatibility.hpp`, use a small explicit mapping helper in `XeFGCompatibility.cpp`.

For example:

```cpp
XeFGMonitorAction monitor_action_from_disposition(
    XeFGPresentationSession::MonitorDisposition disposition) noexcept {
    switch (disposition) {
    case XeFGPresentationSession::MonitorDisposition::AllowGenericRecovery:
        return XeFGMonitorAction::AllowGenericRecovery;
    case XeFGPresentationSession::MonitorDisposition::PreserveGrace:
        return XeFGMonitorAction::PreserveGrace;
    case XeFGPresentationSession::MonitorDisposition::SuppressRuntimeTransition:
        return XeFGMonitorAction::SuppressRuntimeTransition;
    case XeFGPresentationSession::MonitorDisposition::SuppressDetachedUncertain:
        return XeFGMonitorAction::SuppressDetachedUncertain;
    case XeFGPresentationSession::MonitorDisposition::QuarantineSustainedTimeout:
        return XeFGMonitorAction::QuarantineSustainedTimeout;
    case XeFGPresentationSession::MonitorDisposition::QuarantineInconsistentState:
        return XeFGMonitorAction::QuarantineInconsistentState;
    }

    return XeFGMonitorAction::AllowGenericRecovery;
}
```

Do not use ordinal/static-cast coupling between the two enums.

An explicit switch is intentionally safer for this compatibility boundary.

---

# 16. Preserve Hook-Monitor Logging Exactly in Meaning

Keep the current log category and fields:

```text
[XeFG][HookMonitor]
action
reason
generation
runtime_slot
context
swapchain
hook_target
present_entry_count
present_age_ms
timeout_count
transition_depth
detached_uncertain
```

Values should come from the returned evaluation where practical.

`transition_depth` may continue to come directly from:

```cpp
s_runtime_transition_depth.load(std::memory_order_acquire)
```

because exact depth is compatibility/runtime-global state, not session state.

Keep:

```cpp
hook.log_hook_monitor_snapshot("xefg_monitor_decision");
```

for now.

Moving or redesigning the physical D3D12 diagnostic snapshot is not required for this PR.

Do not change the debug logging cadence:

```text
log only when the monitor reason/action changed
AND XeFG debug logging is enabled
```

while still updating action-dedup state even when debug logging is disabled.

---

# 17. D3D12Hook Helper Cleanup: Conservative Only

After the new one-call monitor bridge is in place, some existing helper wrappers may become unused by production code:

```cpp
has_xefg_monitor_state()
has_xefg_detached_state()
get_xefg_monitor_binding_key()
note_xefg_monitor_timeout()
get_xefg_timeout_count()
note_xefg_monitor_action()
```

Before deleting any of them, run repository-wide reference searches on the implementation branch.

Example:

```powershell
rg -n "has_xefg_monitor_state|has_xefg_detached_state|get_xefg_monitor_binding_key|note_xefg_monitor_timeout|get_xefg_timeout_count|note_xefg_monitor_action" src
```

Rules:

- remove a wrapper only if it is genuinely unused after the new evaluator is wired;
- do not remove `clear_xefg_monitor_state()` merely because the monitor classifier no longer calls it — bind/rebind/unhook/lifecycle code may still use it;
- do not remove `has_consistent_active_xefg_binding()` if other lifecycle/handoff code still uses it;
- do not turn this PR into the final 2R7 surface-cleanup pass.

A few transitional wrappers remaining is acceptable.

The priority is correct responsibility movement, not maximum deletion count.

---

# 18. `REFramework.cpp` Must Remain Unchanged

The generic hook monitor should continue to do what it does today:

```text
generic Present watchdog
-> last-chance wait/check
-> ask XeFGCompatibility for recovery action
-> only AllowGenericRecovery performs generic D3D12 recovery
-> XeFG grace/transition/detached/quarantine states suppress generic recovery
```

Do not change:

- 5-second watchdog semantics;
- 1-second last-chance behavior;
- generic `hook_d3d12()` call timing;
- D3D11 monitoring;
- message-hook monitoring.

This PR should not require any `REFramework.cpp` edit.

If implementation unexpectedly requires one, stop and reduce/reconsider the scope before proceeding.

---

# 19. Runtime Transition Semantics Must Remain Unchanged

2R3B already established the runtime-detach split.

Do not touch:

```cpp
detach_xefg_binding_for_runtime_transition(...)
note_xefg_destroy_result(...)
```

Do not change:

- exact runtime matching;
- optional same-HWND fallback;
- detached-state creation;
- bounded old-swapchain keepalive;
- renderer reset ordering;
- physical hook removal;
- raw alias clearing;
- matching non-negative Destroy reconciliation;
- negative Destroy fail-closed behavior.

The monitor classifier observes detached/runtime-transition state; it must not become the owner of runtime teardown.

---

# 20. COM / Ownership Invariants

This PR must not alter any COM ownership semantics.

Keep:

```text
candidate swapchain -> strong while pending/applying
active XeFG swapchain -> borrowed raw pointer
active queue/device -> strong ComPtr
old active swapchain during destructive hook removal -> bounded local ComPtr keepalive
```

Forbidden:

```text
persistent active-swapchain AddRef
Release-until-refcount loops
manual refcount draining
timeout-triggered COM probing
public XeFG proxy fallback binding
```

A hook-monitor timeout must remain a classification event only.

It must never probe or retain a possibly stale borrowed XeFG swapchain.

---

# 21. Expected Diff Shape

Expected primary edits:

### `XeFGPresentationSession.hpp`

Add:

```text
MonitorDisposition
MonitorEvaluation
evaluate_monitor_timeout(...)
```

### `XeFGPresentationSession.cpp`

Implement the exact current recovery precedence using existing session helpers.

### `D3D12Hook.hpp/.cpp`

Add one thin input-gathering/delegation method.

Optionally remove monitor-only wrappers proven unused after repository-wide search.

### `XeFGCompatibility.cpp`

Replace detailed branching with:

```text
one D3D12/session evaluation call
-> explicit disposition-to-XeFGMonitorAction mapping
-> existing debug log
-> return action
```

Expected unchanged files include:

```text
REFramework.cpp
XeFGCompatibility.hpp
XeFGBinding.hpp/.cpp
XeFGResizeLifecycle.hpp/.cpp
XeFGDiscovery.hpp/.cpp
XeFGCandidateHandoff.hpp/.cpp
XeFGRuntimeRegistry.hpp/.cpp
```

No build-file regeneration should be required because no new source files are being added.

---

# 22. Validation Requirements

Run at minimum:

```powershell
cmake -S . -B build-2r3c -G "Visual Studio 17 2022" -A x64
cmake --build build-2r3c --config Release --target REFramework --parallel 4
$env:PYTHONUTF8='1'; python dev/audit_direct_access_clang.py
git diff --check
```

Also perform source-scope checks:

```powershell
git diff --name-only REFforXeFG...HEAD
rg -n "runtime_transition|xefg_state_safe|detached_uncertain|binding_identity_inconsistent|sustained_present_timeout|present_timeout" src/compatibility/xefg src/D3D12Hook.*
```

Review the final diff and verify:

- all six recovery outcomes still exist;
- precedence is unchanged;
- reason strings are unchanged;
- timeout thresholds are unchanged;
- timeout sampling occurs only in the same branch as before;
- monitor-action dedup state updates even with debug logging disabled;
- `REFramework.cpp` is unchanged;
- runtime detach/Destroy code is unchanged;
- Present/Resize code is unchanged;
- bind/rebind/candidate code is unchanged;
- no COM ownership changes exist.

Because the user is currently working remotely and this integration branch is unreleased, a game runtime smoke test is **not required to open this small structural PR**.

If no runtime test is performed, state that explicitly in the PR description and do not imply DD2/MHW/XeFG runtime validation.

---

# 23. Review Checklist

Before opening the PR, verify each item:

- [ ] PR base is `REFforXeFG`.
- [ ] Branch started from the current tip containing 2R3B and this work order.
- [ ] Session owns the detailed XeFG recovery precedence after the change.
- [ ] `XeFGCompatibility::evaluate_hook_monitor_timeout()` no longer directly chains multiple XeFG state predicates.
- [ ] Public `XeFGMonitorAction` contract is unchanged.
- [ ] `REFramework.cpp` is unchanged.
- [ ] Runtime transition remains highest precedence.
- [ ] No-monitor state still allows generic recovery.
- [ ] Detached uncertainty still suppresses generic recovery before consistency/timeout evaluation.
- [ ] Inconsistent identity still quarantines before timeout sampling.
- [ ] Sustained timeout still requires the established monitor threshold.
- [ ] Grace still returns `PreserveGrace`.
- [ ] Reason strings are unchanged.
- [ ] Action dedup still runs regardless of debug-log enablement.
- [ ] Existing debug log fields remain available.
- [ ] Exact runtime transition depth logging remains available.
- [ ] 2R3B detach/Destroy code is untouched.
- [ ] Present/Present1 code is untouched.
- [ ] Resize code and MHW rule are untouched.
- [ ] Bind/rebind/candidate handoff are untouched.
- [ ] Active swapchain remains borrowed.
- [ ] Queue/device remain strongly owned.
- [ ] No forced Release/refcount drain code added.
- [ ] Release build passes.
- [ ] direct-access audit passes.
- [ ] `git diff --check` passes.

---

# 24. Stop Conditions

Stop and reduce/reconsider the PR if implementation starts requiring any of the following:

```text
REFramework.cpp watchdog rewrite
new timer/sleep/retry logic
runtime detach rewrite
Destroy semantic changes
Present/Present1 callback order changes
Resize behavior changes
MHW policy changes
candidate handoff changes
physical hook ownership movement
active swapchain strong ownership
Intel runtime calls from the session
generic frame-generation abstraction
```

Those are outside 2R3C.

---

# 25. PR Description Requirements

The PR description should clearly state:

```text
2R3C completes the conceptual 2R3 monitor-policy extraction.

Detailed XeFG recovery precedence is now evaluated by XeFGPresentationSession using a narrow physical/liveness view.
XeFGCompatibility keeps the existing public XeFGMonitorAction contract and maps the session result to that contract.
The generic REFramework watchdog and REFramework.cpp are unchanged.

No runtime-detach, Destroy, Present/Resize, bind/rebind, candidate-handoff, COM-ownership, or native D3D12 behavior change is intended.
```

Include exact build/static validation performed.

If runtime smoke was not performed, say so explicitly.

---

## Final Rule

The purpose of 2R3C is not to redesign recovery.

It is to make the current recovery policy live with the XeFG semantic state that already owns the information required to make that policy decision.

After this PR, the conceptual 2R3 stage should be considered structurally complete, and the next implementation stage can move to **2R4 — XeFG Present / Present1 policy extraction**.
