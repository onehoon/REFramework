# Work Order — 2R5A: Move XeFG Resize-Hold Lifecycle and MHW `ResizeTarget` Policy Behind `XeFGPresentationSession`

Date: 2026-09-09  
Repository: `onehoon/REFramework`  
PR base branch: `REFforXeFG`  
Planning baseline reviewed: `REFforXeFG` @ `bbd028a07b915e954b6a04a4f2af828d30179505`  
Architecture authority: `doc/2ndrefactor/REFramework_OPTISCALER_XEFG_SECOND_STAGE_REFACTOR_DESIGN_2026-09-09.md`  
Previous stage: conceptual 2R4 completed through 2R4A / 2R4B

---

## 1. Objective

Begin conceptual **2R5 — Move XeFG Resize Policy and MHW Rule Into the Session** with a deliberately small first PR.

This PR is **2R5A**.

The goal is:

> Move XeFG resize-hold arm / complete / clear policy and the MHW-only `ResizeTarget` hold activation rule behind `XeFGPresentationSession`, while leaving physical D3D12 resize callback execution, recursion handling, renderer reset callbacks, original DXGI calls, raw alias updates, and physical diagnostic emission inside `D3D12Hook` exactly where they are today.

This is a **behavior-preserving refactor**.

Do not complete the whole conceptual 2R5 in this PR.

A later **2R5B** should handle the remaining resize-event interpretation / `ResizeBuffers1` reset policy / diagnostic-surface cleanup after this high-risk hold lifecycle is isolated.

---

## 2. Why 2R5 Is Split

Current resize handling spans three physical callbacks with different semantics:

```text
ResizeBuffers
ResizeBuffers1
ResizeTarget
```

The current code also mixes several distinct responsibilities:

```text
physical D3D12 mechanism
    recursion handling
    original function lookup / forwarding
    renderer reset callback execution
    display/render dimension aliases
    physical swapchain / hook diagnostics

XeFG semantic lifecycle
    resize event state
    resize-hold active state
    suppressed-Present count
    hold arm / complete / clear transitions

XeFG game policy
    MHW-only ResizeTarget hold activation
```

Moving all of this at once would make it difficult to prove that the three resize paths retain their subtly different behavior.

2R5A therefore moves only the **hold lifecycle and MHW-specific activation policy**.

2R5B can then move the remaining event/reset interpretation once hold semantics have an explicit session API.

---

## 3. Branch / PR Rules

Create a fresh implementation branch from the current `REFforXeFG` tip **after this work-order commit is present**.

Suggested branch:

```text
refactor/xefg-2r5a-resize-hold-mhw-session
```

Open the implementation PR against:

```text
base: REFforXeFG
```

Do not target `master`.

Do not rebase onto upstream `praydog/REFramework` as part of this PR.

Keep the implementation concentrated in:

```text
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp
src/D3D12Hook.hpp
src/D3D12Hook.cpp
```

`XeFGResizeLifecycle.hpp/.cpp` should normally need no functional changes in 2R5A.

No CMake files should change unless source membership unexpectedly changes.

---

## 4. Current Code Reviewed

At baseline `bbd028a07b915e954b6a04a4f2af828d30179505`, `D3D12Hook.cpp` still directly owns the high-level hold interpretation.

### Current hold arm bridge

```cpp
void D3D12Hook::arm_xefg_resize_transition_hold(uint64_t event_id) {
    if (m_swapchain_source != SwapchainSource::XeFGInternal
        || m_xefg_session.binding().observe_only()
        || event_id == 0) {
        return;
    }

    m_xefg_session.resize_lifecycle().arm(event_id);

    // existing logs
}
```

### Current hold completion bridge

```cpp
void D3D12Hook::complete_xefg_resize_transition_hold(
    uint64_t completion_event_id,
    XefgResizeEventKind completion_kind,
    HRESULT result) {

    if (!m_xefg_session.resize_lifecycle().suppress_renderer()) {
        return;
    }

    if (FAILED(result)) {
        // log keep
        return;
    }

    // log complete
    m_xefg_session.resize_lifecycle().complete(
        completion_event_id,
        completion_kind,
        result);
}
```

### Current hold clear bridge

```cpp
void D3D12Hook::clear_xefg_resize_transition_hold(const char* reason) {
    if (!m_xefg_session.resize_lifecycle().suppress_renderer()) {
        return;
    }

    // log clear
    m_xefg_session.resize_lifecycle().clear();
}
```

### Current MHW-only activation rule

Inside `D3D12Hook::resize_target()`:

```cpp
if (event_id != 0
    && renderer_reset_performed
    && d3d12->is_xefg_render_capable()
    && sdk::GameIdentity::get().is_mhwilds()) {
    d3d12->arm_xefg_resize_transition_hold(event_id);
}
```

This MHW check is currently the only reason `D3D12Hook.cpp` needs `sdk/GameIdentity.hpp` for XeFG resize policy.

---

## 5. Critical Existing Lifecycle Semantics

These are non-negotiable.

### 5.1 MHW is the only current game that arms the ResizeTarget hold

Required behavior remains:

```text
MHW + tracked XeFG ResizeTarget + renderer reset performed + render-capable binding
    -> arm hold

DD2 / other games
    -> do not arm hold

observe-only XeFG
    -> do not arm hold

native D3D12
    -> do not arm XeFG hold
```

Do not generalize the rule to all RE Engine games.

Do not add an allowlist / provider abstraction / generic frame-generation policy.

### 5.2 Successful ResizeTarget does NOT complete the hold

Current intended sequence for MHW is:

```text
ResizeTarget
    -> renderer reset
    -> arm hold
    -> original ResizeTarget succeeds
    -> hold REMAINS active

later ResizeBuffers or ResizeBuffers1
    -> renderer reset / physical resize flow
    -> original call succeeds
    -> complete hold
```

Do not complete the hold after successful `ResizeTarget`.

The hold intentionally bridges the ResizeTarget -> ResizeBuffers/ResizeBuffers1 transition.

### 5.3 Failed ResizeTarget clears the just-armed hold

Current code performs:

```cpp
if (event_id != 0 && FAILED(result)) {
    d3d12->clear_xefg_resize_transition_hold("resize_target_failed");
}
```

Preserve this behavior exactly.

A failed MHW `ResizeTarget` must not leave the transition hold armed.

### 5.4 Failed ResizeBuffers / ResizeBuffers1 keeps an existing hold active

Current `complete_xefg_resize_transition_hold()` behavior is:

```text
no active hold
    -> no-op

active hold + FAILED(result)
    -> log keep
    -> hold remains active

active hold + non-failed result
    -> log complete
    -> clear hold via XeFGResizeLifecycle::complete()
```

Do not turn failed completion into clear/quarantine/retry.

### 5.5 Result semantics remain HRESULT semantics

For these DXGI resize callbacks, preserve the current `FAILED(result)` / non-failed interpretation.

Do not mix this with Intel XeFG integer result semantics from PR #37.

This PR must not alter Intel Init/Destroy result handling.

### 5.6 Renderer reset behavior is NOT being normalized in 2R5A

The current three resize paths are intentionally not identical.

In particular:

- `ResizeBuffers1` suppresses renderer reset in observe-only mode.
- `ResizeTarget` currently executes `m_on_resize_target` when present and only uses render-capable state to decide whether the **hold** may be armed afterward.
- generic `ResizeBuffers` retains its existing callback behavior.

Do not change renderer reset eligibility in this PR.

Do not "fix" `ResizeTarget` to match `ResizeBuffers1` observe-only behavior.

That would be a behavior change outside 2R5A.

---

## 6. Move the MHW Policy Location Into `XeFGPresentationSession`

The architecture requirement is explicit:

```text
current:
D3D12Hook.cpp -> sdk::GameIdentity::get().is_mhwilds()

target:
XeFGPresentationSession / XeFG-specific policy -> is_mhwilds()
```

After 2R5A, `D3D12Hook::resize_target()` must no longer directly call:

```cpp
sdk::GameIdentity::get().is_mhwilds()
```

The session should own the game-specific decision.

A narrow method may conceptually look like:

```cpp
ResizeHoldArmDecision evaluate_and_arm_resize_target_hold(
    bool xefg_source,
    uint64_t event_id,
    bool renderer_reset_performed) noexcept;
```

The exact name may differ.

The session method should internally evaluate:

```text
xefg_source
&& event_id != 0
&& renderer_reset_performed
&& binding is render-capable / not observe-only
&& sdk::GameIdentity::get().is_mhwilds()
```

If accepted, it may perform the semantic `m_resize_lifecycle.arm(event_id)` mutation and return enough pre/post state for `D3D12Hook` to preserve existing logs.

Do not pass a generic game-name string or game enum through `D3D12Hook` merely to keep the policy outside the session.

The point of this step is to remove the MHW-specific decision from generic D3D12 callback code.

---

## 7. Recommended Resize-Hold Decision Types

Use narrow XeFG-specific result types so `D3D12Hook` can emit the same diagnostics without reading/mutating session internals repeatedly.

The exact type names may differ, but a shape like this is appropriate:

```cpp
class XeFGPresentationSession {
public:
    struct ResizeHoldSnapshot {
        bool active{};
        uint64_t trigger_event_id{};
        uint32_t suppressed_present_count{};
        uint64_t binding_generation{};
    };

    struct ResizeHoldArmDecision {
        bool armed{};
        ResizeHoldSnapshot state{};
    };

    enum class ResizeHoldCompletionDisposition : uint8_t {
        NoActiveHold,
        KeepFailedCompletion,
        Completed,
    };

    struct ResizeHoldCompletionDecision {
        ResizeHoldCompletionDisposition disposition{
            ResizeHoldCompletionDisposition::NoActiveHold};
        ResizeHoldSnapshot previous{};
        uint64_t completion_event_id{};
        XeFGResizeLifecycle::EventKind completion_kind{
            XeFGResizeLifecycle::EventKind::None};
        HRESULT result{S_OK};
    };

    struct ResizeHoldClearDecision {
        bool cleared{};
        ResizeHoldSnapshot previous{};
        const char* reason{};
    };
};
```

This is an architectural recommendation, not a requirement to use these exact names.

Do not put physical swapchain/hook pointers, renderer callbacks, framework pointers, or COM ownership in these session result types.

---

## 8. Move Hold Arm Semantics Behind the Session

The session should own the final semantic arm operation.

Conceptually:

```cpp
XeFGPresentationSession::ResizeHoldArmDecision
XeFGPresentationSession::evaluate_and_arm_resize_target_hold(
    bool xefg_source,
    uint64_t event_id,
    bool renderer_reset_performed) noexcept {

    ResizeHoldArmDecision result{};

    const bool should_arm = xefg_source
        && event_id != 0
        && renderer_reset_performed
        && !m_binding.observe_only()
        && sdk::GameIdentity::get().is_mhwilds();

    if (!should_arm) {
        return result;
    }

    result.armed = m_resize_lifecycle.arm(event_id);
    result.state = resize_hold_snapshot();
    return result;
}
```

Important:

- native path must remain dormant;
- observe-only must not arm;
- event ID zero must not arm;
- MHW-only rule remains exact;
- no timer fallback;
- no retries;
- no COM probing.

If `XeFGResizeLifecycle::arm()` returns false, do not claim the hold armed.

---

## 9. Move Hold Completion Semantics Behind the Session

The semantic method should preserve the current three-way outcome:

```text
NoActiveHold
KeepFailedCompletion
Completed
```

Conceptually:

```cpp
ResizeHoldCompletionDecision complete_resize_hold(
    uint64_t completion_event_id,
    XeFGResizeLifecycle::EventKind completion_kind,
    HRESULT result) noexcept;
```

Required behavior:

```cpp
if (!m_resize_lifecycle.suppress_renderer()) {
    return NoActiveHold;
}

snapshot current trigger/count/generation;

if (FAILED(result)) {
    return KeepFailedCompletion;
}

m_resize_lifecycle.complete(completion_event_id, completion_kind, result);
return Completed;
```

Capture diagnostics needed by `D3D12Hook` **before** successful completion clears the hold state.

Do not change the completion call sites:

- successful/failing `ResizeBuffers` still reaches completion at the current location after original return;
- successful/failing `ResizeBuffers1` still reaches completion at the current location after original return;
- `ResizeTarget` success still does not call completion.

---

## 10. Move Hold Clear Semantics Behind the Session

Add a narrow session method conceptually similar to:

```cpp
ResizeHoldClearDecision clear_resize_hold(const char* reason) noexcept;
```

Required behavior:

```text
no active hold
    -> no mutation / cleared = false

active hold
    -> snapshot trigger/count/generation
    -> clear XeFGResizeLifecycle hold
    -> cleared = true
```

`D3D12Hook` should remain responsible for the existing textual log emission.

The session may carry the existing stable reason pointer through the result, but it must not format log strings.

Current reason string:

```text
resize_target_failed
```

must remain unchanged at the existing call site.

Runtime-transition clear reasons already used elsewhere must also remain unchanged if the same bridge is shared.

---

## 11. Keep Physical Logging in `D3D12Hook`

The session owns semantic decisions and state mutation.

`D3D12Hook` should continue to emit existing logs such as:

```text
[XeFG][ResizeHold] action = arm
[XeFG][ResizeHold] action = arm_debug
[XeFG][ResizeHold] action = keep, reason = completion_failed
[XeFG][ResizeHold] action = complete
[XeFG][ResizeHold] action = clear
```

Preserve the existing message text and values unless a value is now supplied from a decision snapshot instead of direct lifecycle reads.

Especially preserve:

```text
trigger_event_id
completion_event_id
completion_kind
HRESULT
suppressed_presents
binding generation
reason string
```

The debug-only arm log may continue to include the physical `m_swap_chain` pointer because that is a D3D12Hook-owned diagnostic value.

Do not move raw renderer aliases into the session just for logging.

---

## 12. `ResizeTarget` Target Shape

Keep physical callback structure unchanged.

After the existing renderer reset block:

```cpp
bool renderer_reset_performed = false;
if (d3d12->m_on_resize_target) {
    ... existing pre-reset diagnostics ...
    d3d12->m_on_resize_target(*d3d12);
    renderer_reset_performed = true;
    ... existing post-reset diagnostics ...
}
```

replace the direct MHW policy block with a thin bridge, conceptually:

```cpp
const auto arm = d3d12->m_xefg_session.evaluate_and_arm_resize_target_hold(
    d3d12->is_xefg_source(),
    event_id,
    renderer_reset_performed);

d3d12->log_xefg_resize_hold_arm(arm);
```

or retain the current `D3D12Hook::arm_xefg_resize_transition_hold(...)` name as a logging bridge if that produces a smaller diff.

What matters is:

> the MHW/game-specific decision and semantic `arm()` mutation are behind the session; the generic callback only supplies physical context and emits diagnostics.

Then preserve:

```text
original ResizeTarget call
-> failure logging
-> depth decrement
-> if event_id != 0 && FAILED(result): clear hold("resize_target_failed")
-> original_return diagnostic
-> return result
```

Do not reorder this sequence.

---

## 13. `ResizeBuffers` / `ResizeBuffers1` Target Shape

Do not redesign these callbacks in 2R5A.

Only replace the current direct completion-policy bridge with session delegation while preserving the exact call location.

Current effective order must remain:

### ResizeBuffers

```text
tracked top-level event begin
-> existing renderer reset callback
-> original ResizeBuffers
-> depth decrement
-> original_return diagnostic
-> semantic hold completion evaluation
-> return
```

### ResizeBuffers1

```text
tracked XeFG validation
-> nested direct forwarding guard
-> event begin
-> existing observe-only reset suppression
-> optional renderer reset
-> original ResizeBuffers1
-> depth decrement
-> result log
-> original_return diagnostic
-> semantic hold completion evaluation
-> return
```

Do not move the completion evaluation before the original call.

Do not change `FAILED(result)` behavior.

Do not change the `ResizeBuffers1` observe-only reset rule in 2R5A.

---

## 14. Remove the MHW Include From Generic D3D12 Code

After the policy move, check whether `src/D3D12Hook.cpp` still uses anything from:

```cpp
#include <sdk/GameIdentity.hpp>
```

If not, remove that include from `D3D12Hook.cpp`.

Add the dependency only where the XeFG-specific policy now lives, expected:

```cpp
src/compatibility/xefg/XeFGPresentationSession.cpp
```

Do not move unrelated game identity logic.

This cleanup is part of the architectural purpose of 2R5A.

---

## 15. Do Not Move Resize Event Begin / Reset Policy Yet

Defer these to 2R5B:

- `begin_xefg_resize_event(...)` cleanup;
- tracked/top-level resize-event interpretation cleanup;
- `ResizeBuffers1` `should_reset_renderer` semantic extraction;
- remaining resize lifecycle diagnostic-state API cleanup;
- possible cleanup of direct `resize_lifecycle()` reads used only by resize diagnostics;
- `get_xefg_last_resize_kind()` cleanup if justified.

2R5A should not become a broad resize rewrite.

---

## 16. Non-Negotiable Scope Boundaries

Do not intentionally modify:

- Present / Present1 policy completed in 2R4;
- post-resize Present sample behavior;
- hook-monitor policy;
- runtime detach / Destroy lifecycle;
- candidate handoff;
- pending-candidate lock order;
- initial bind;
- same-object rebind;
- changed-object rebind;
- XeFG discovery;
- Intel runtime calls;
- active swapchain ownership;
- queue/device COM ownership;
- physical `m_swapchain_hook` ownership;
- physical `m_present_hook` ownership;
- D3D11;
- Streamline / DLSSG;
- FSRFG;
- native D3D12 discovery;
- native Present1 behavior;
- generic watchdog behavior;
- anti-tamper/integrity behavior.

Do not add timers, sleeps, polling, delayed release, retry loops, refcount draining, or COM probing.

Do not introduce a generic frame-generation resize interface.

---

## 17. COM / Ownership Invariants

This PR should not need COM ownership changes at all.

Preserve:

```text
active XeFG swapchain = borrowed raw pointer
active queue/device    = strong ComPtr through XeFGBinding
physical VtableHook    = D3D12Hook-owned
```

Do not add a persistent `ComPtr<IDXGISwapChain3>` to the session.

Do not call `AddRef`/`Release` as part of resize policy movement.

Do not add forced refcount-drain behavior.

---

## 18. Locking / Threading Invariants

Resize callbacks currently execute while holding the existing REFramework hook-monitor/lifecycle mutex.

Do not change that in 2R5A.

Do not add a new mutex inside `XeFGPresentationSession` for these transitions.

Do not call Intel vendor functions from the new session resize methods.

Do not alter:

```text
hook-monitor/lifecycle mutex -> pending-candidate mutex
```

ordering elsewhere.

No worker thread, timer, or polling path is required.

---

## 19. Expected Diff

Expected files:

```text
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp
src/D3D12Hook.hpp
src/D3D12Hook.cpp
```

Expected structural change:

```text
D3D12Hook
    physical resize callback / renderer-reset mechanism
    physical diagnostic logging
          |
          v
XeFGPresentationSession
    MHW-only ResizeTarget hold policy
    semantic arm / complete / clear
    hold diagnostic snapshots
          |
          v
XeFGResizeLifecycle
    primitive state mutation
```

Unexpected / stop-and-report changes:

```text
REFramework.cpp
XeFGCompatibility.cpp/.hpp
XeFGCandidateHandoff
XeFGDiscovery
XeFGBinding ownership semantics
D3D11
Streamline
FSRFG
Intel loader/runtime code
CMake source lists
```

If implementation appears to require any of these, stop and report instead of expanding scope.

---

## 20. Static Acceptance Checklist

The PR is acceptable only if static review can show all of the following:

- [ ] MHW remains the only current game that arms the ResizeTarget hold.
- [ ] DD2 / other games still do not arm the hold.
- [ ] observe-only XeFG still does not arm the MHW hold.
- [ ] native D3D12 remains dormant with respect to XeFG hold policy.
- [ ] renderer reset callbacks remain in D3D12Hook.
- [ ] `ResizeTarget` success leaves an armed hold active.
- [ ] `ResizeTarget` failure clears an active hold with reason `resize_target_failed`.
- [ ] `ResizeBuffers` failure keeps an existing hold active.
- [ ] `ResizeBuffers1` failure keeps an existing hold active.
- [ ] successful `ResizeBuffers` completes the hold at the same location as before.
- [ ] successful `ResizeBuffers1` completes the hold at the same location as before.
- [ ] completion/clear logs preserve pre-mutation trigger/count/generation values.
- [ ] `D3D12Hook.cpp` no longer performs the MHW game-identity decision.
- [ ] `sdk/GameIdentity.hpp` is removed from D3D12Hook.cpp if no other use remains.
- [ ] `ResizeBuffers1` observe-only renderer-reset behavior is unchanged.
- [ ] no Present/Present1 changes.
- [ ] no bind/rebind/candidate changes.
- [ ] no COM ownership changes.

---

## 21. Required Validation

Run:

```text
cmake -S . -B build-2r5a -G "Visual Studio 17 2022" -A x64
cmake --build build-2r5a --config Release --target REFramework --parallel 4
python dev/audit_direct_access_clang.py
git diff --check
```

Expected:

- Release x64 build succeeds;
- `dinput8.dll` is produced;
- direct-access audit reports `Total violations: 0`;
- `git diff --check` is clean.

Also perform a forbidden-scope diff audit verifying that no unrelated Present, binding, monitor, runtime, Streamline, D3D11, or candidate code changed.

---

## 22. Runtime Gate

If runtime validation is available, preferred smoke sequence:

### A. Native REFramework / XeFG inactive

- launch RE Engine game;
- overlay opens;
- basic gameplay;
- resize / display-mode path if practical;
- clean exit;
- no unexpected XeFG hold log or recovery loop.

### B. DD2 + OptiScaler + Intel XeFG

- cold launch;
- XeFG active;
- REFramework overlay visible;
- Alt+Tab / display transition once or twice;
- no unexpected MHW-only hold arm;
- no crash / stale hook / recovery loop;
- clean exit.

### C. MHW + OptiScaler + Intel XeFG — preferred high-value gate

Because this PR moves the MHW-specific policy, MHW is the most valuable runtime test when available:

- cold launch;
- XeFG active;
- trigger a display/resize transition that reaches `ResizeTarget`;
- verify hold arm still occurs only after renderer reset;
- verify successful following `ResizeBuffers`/`ResizeBuffers1` completes it;
- verify no permanent suppressed-render state;
- clean exit.

Runtime smoke is valuable but absence of runtime access should be reported honestly rather than simulated.

---

## 23. Completion Report

PR description should explicitly state:

```text
- 2R5A only; conceptual 2R5 is not yet complete.
- MHW-only ResizeTarget hold policy moved behind XeFGPresentationSession.
- hold arm / complete / clear semantic mutations moved behind the session.
- physical renderer reset and DXGI callback execution remain in D3D12Hook.
- successful ResizeTarget still leaves the hold active until ResizeBuffers/ResizeBuffers1 completion.
- failed ResizeTarget still clears the hold.
- failed ResizeBuffers/ResizeBuffers1 still preserves the hold.
- ResizeBuffers1 observe-only reset semantics are unchanged.
- no Present, bind/rebind, candidate, runtime, monitor, or COM ownership changes.
- build / audit / diff-check results.
- runtime test status.
```

---

## 24. Final Rule

> **2R5A changes who owns the XeFG resize-hold decision, not when REFramework resets the renderer, not when DXGI resize calls execute, and not which games receive the hold. MHW remains the sole current ResizeTarget hold case.**

After 2R5A is merged and reviewed, proceed to **2R5B** for the remaining XeFG resize-event / reset interpretation and diagnostic-surface cleanup.