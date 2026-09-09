# Work Order — 2R4A: Move XeFG Present Suppression / Render-Boundary Policy Behind `XeFGPresentationSession`

Date: 2026-09-09  
Repository: `onehoon/REFramework`  
PR base branch: `REFforXeFG`  
Planning baseline reviewed: `REFforXeFG` @ `d6ac54c100fe6e73045b64028fde5a1100bdd963`  
Architecture authority: `doc/2ndrefactor/REFramework_OPTISCALER_XEFG_SECOND_STAGE_REFACTOR_DESIGN_2026-09-09.md`  
Previous steps: conceptual 2R3 completed through 2R3A / 2R3B / 2R3C

---

## 1. Objective

Begin conceptual **2R4 — Move XeFG Present / Present1 Policy Into the Session** with a deliberately small first PR.

This PR is **2R4A**.

The goal is:

> Move XeFG-specific Present suppression, resize-hold suppression interpretation, suppressed-present counting ownership, and first-render-boundary decision state behind `XeFGPresentationSession`, while leaving actual Present / Present1 forwarding, recursion handling, renderer callbacks, generic alias updates, post-resize diagnostic emission, and post-Present execution inside `D3D12Hook` exactly where they are today.

This must be a **behavior-preserving refactor**.

Do not complete the whole conceptual 2R4 in one PR.

In particular, **do not move the current post-resize Present diagnostic/sample helper yet**. That helper has a subtle debug-dependent observable behavior described below and should be handled in a later 2R4B after the basic suppression policy is isolated.

---

## 2. Why 2R4 Is Split

Current `D3D12Hook::present_common()` still mixes:

```text
generic / physical Present mechanism
    tracked-instance validation
    Present-entry liveness update
    native raw-alias updates
    recursion handling
    renderer callback invocation
    original Present / Present1 forwarding
    device-removed logging
    post-render callback invocation

XeFG policy
    resize-hold active decision
    observe-only suppression decision
    suppressed-Present counting
    first-render-boundary diagnostic decision/state

XeFG resize diagnostics
    post-resize sample consumption
    debug-only Present-after-resize logging
    ordinal-1 pre/post renderer snapshots
```

The policy portion is ready to move behind `XeFGPresentationSession`.

The post-resize diagnostic helper is not as mechanically safe to move in the same PR because current code does this:

```cpp
const auto sample = m_xefg_session.resize_lifecycle().consume_post_resize_present_sample();
if (!sample.has_value() || !XeFGCompatibility::is_debug_log_enabled()) return 0;
...
return sample->ordinal;
```

Important current behavior:

```text
the resize sample is consumed even when debug logging is disabled,
but the caller receives ordinal 0 when debug logging is disabled.
```

Therefore moving the sample into a generic `PresentDecision` and returning the raw ordinal unconditionally would accidentally make the existing `present_pre_render_callback` / `present_post_render_callback` diagnostic snapshots run when debug logging is off.

That would be a behavior change.

For 2R4A, keep this helper and its call site mechanically unchanged.

---

## 3. Branch / PR Rules

Implementation branch should start from the current `REFforXeFG` tip **after this work-order commit is present**.

Suggested branch:

```text
refactor/xefg-2r4a-present-policy-session
```

Open the PR against:

```text
base: REFforXeFG
```

Do not target `master`.

Do not rebase onto upstream `praydog/REFramework` as part of this PR.

Keep this PR small enough to review `present_common()` line-by-line against the pre-change implementation.

---

## 4. Current Code Reviewed

At baseline `d6ac54c100fe6e73045b64028fde5a1100bdd963`, top-level `present_common()` reaches XeFG policy only **after**:

```text
framework/lifecycle mutex acquisition
tracked-instance validation
Present-entry counter/timestamp update
generic alias refresh
native-only command-queue discovery
nested Present direct-forward guard
```

Current policy block is conceptually:

```cpp
const auto xefg_resize_transition_hold = d3d12->is_xefg_resize_hold_active();
const auto suppress_render_callbacks = d3d12->should_suppress_xefg_render_callbacks();
const auto post_resize_ordinal = d3d12->is_xefg_source()
    ? d3d12->log_xefg_post_resize_present(swap_chain, kind, original_present)
    : 0;
const auto log_render_boundary = d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
    && !suppress_render_callbacks
    && d3d12->m_on_present
    && !d3d12->m_xefg_session.render_boundary_logged();
```

Then:

```text
if first render boundary -> diagnostic enter log

if resize hold active
    -> increment suppressed Present count
    -> log first three suppressed Presents

if not suppressed and m_on_present exists
    -> optional ordinal-1 pre-render snapshot
    -> m_on_present
    -> optional ordinal-1 post-render snapshot
    -> first-render-boundary returned log
    -> mark render boundary logged

original Present / Present1

if XeFG + DEVICE_REMOVED
    -> device removed diagnostic

if suppressed
    -> g_framework->note_present_activity()
else if m_on_post_present
    -> m_on_post_present
```

This exact ordering is the baseline.

---

## 5. Non-Negotiable Scope

This PR is only XeFG Present policy/state extraction.

Do not intentionally modify:

- D3D11;
- Streamline / DLSSG;
- FSRFG;
- native D3D12 discovery;
- native phase-1 transition behavior;
- native Present1 installation;
- `present()` phase-1 generic implementation;
- Present / Present1 original function lookup;
- Present / Present1 recursion handling;
- `g_present_depth` behavior;
- `m_ignore_next_present` behavior;
- generic `m_swap_chain` / `m_device` alias update behavior;
- native command-queue offset discovery;
- `m_on_present` callback ownership or invocation mechanism;
- `m_on_post_present` callback ownership or invocation mechanism;
- original Present / Present1 call ordering;
- device-removed handling;
- runtime detach / Destroy behavior from 2R3B;
- hook-monitor policy from 2R3C;
- candidate handoff;
- bind/rebind transaction;
- ResizeBuffers / ResizeBuffers1 / ResizeTarget behavior;
- MHW resize-hold activation rule;
- Intel runtime calls;
- physical hook ownership;
- COM ownership.

Do not introduce a generic frame-generation Present provider or universal Present abstraction.

---

## 6. Critical Present Ordering Contract

### Render-capable XeFG path

Preserve exactly:

```text
tracked top-level Present / Present1
-> update real Present liveness
-> evaluate XeFG policy
-> m_on_present
-> original Present / Present1
-> m_on_post_present
```

### Observe-only XeFG path

Preserve exactly:

```text
tracked top-level Present / Present1
-> update real Present liveness
-> suppress renderer callback
-> original Present / Present1
-> g_framework->note_present_activity()
-> no normal m_on_post_present
```

### Resize-hold XeFG path

Preserve exactly:

```text
tracked top-level Present / Present1
-> update real Present liveness
-> suppress renderer callback
-> increment suppressed Present counter once
-> original Present / Present1
-> g_framework->note_present_activity()
-> no normal m_on_post_present
```

### Native path

Preserve native behavior exactly.

The new session policy call must be dormant when `xefg_source == false`.

### Nested Present

Nested Present must continue to direct-forward the original call **before any XeFG Present policy evaluation or mutation occurs**.

Do not move session policy evaluation above the current `g_present_depth > 0` guard.

---

## 7. Add a Narrow XeFG Present Decision Type

Add an XeFG-specific type under `XeFGPresentationSession`.

Recommended conceptual shape:

```cpp
class XeFGPresentationSession {
public:
    struct PresentDecision {
        bool xefg_source{};
        bool resize_hold_active{};
        bool suppress_render_callbacks{};
        bool log_first_render_boundary{};
        uint64_t resize_event_id{};
        uint64_t hold_trigger_event_id{};
    };

    ...
};
```

Exact names may differ.

Do not put callback objects, `D3D12Hook*`, `std::function`, original Present pointers, COM objects, or physical hook objects in this result.

The result is semantic policy only.

---

## 8. Add One Pure Present Policy Evaluation Method

Prefer a session method conceptually similar to:

```cpp
PresentDecision evaluate_present_policy(
    bool xefg_source,
    bool render_callback_available) const noexcept;
```

Required semantics:

```cpp
PresentDecision result{};
result.xefg_source = xefg_source;

if (!xefg_source) {
    return result;
}

result.resize_hold_active = m_resize_lifecycle.suppress_renderer();
result.suppress_render_callbacks =
    m_binding.observe_only() || result.resize_hold_active;
result.log_first_render_boundary =
    !result.suppress_render_callbacks
    && render_callback_available
    && !m_render_boundary_logged;
result.resize_event_id = m_resize_lifecycle.event_id();
result.hold_trigger_event_id = m_resize_lifecycle.hold_trigger_event_id();
return result;
```

The important rule is:

> `xefg_source == false` must return immediately before any XeFG lifecycle state is mutated.

This keeps the inactive session structurally passive on the native path.

The evaluation method should be read-only / `const`.

Do not increment suppressed-Present state inside this pure classification method.

Reason: the current counter increment happens after post-resize sample/log handling, and preserving that relative order makes review easier.

---

## 9. Move Suppressed-Present Counting Behind the Session Without Reordering It

Current D3D12Hook wrapper ultimately calls:

```cpp
m_xefg_session.resize_lifecycle().note_suppressed_present();
```

Move semantic ownership behind the session with a narrow method, for example:

```cpp
uint32_t note_suppressed_present(const PresentDecision& decision) noexcept {
    if (!decision.xefg_source || !decision.resize_hold_active) {
        return 0;
    }

    return m_resize_lifecycle.note_suppressed_present();
}
```

Call it at the **same relative location** as today:

```text
policy evaluation
-> existing post-resize helper call
-> first-render-boundary enter decision/log
-> if resize hold active: increment suppressed count
-> renderer callback block
```

Do not increment the counter for observe-only suppression unless resize hold is also active.

Current behavior counts suppressed Presents only for the resize-hold branch.

---

## 10. Move First-Render-Boundary State Mutation Behind the Session

The session already owns:

```cpp
bool m_render_boundary_logged{};
```

2R4A should stop `D3D12Hook::present_common()` from manipulating that raw state via generic getter/setter decisions.

Add a narrow commit method, conceptually:

```cpp
void mark_render_boundary_logged(const PresentDecision& decision) noexcept {
    if (decision.xefg_source && decision.log_first_render_boundary) {
        m_render_boundary_logged = true;
    }
}
```

Call it only after `m_on_present(*d3d12)` returns, at the same location where current code sets the flag.

Do not mark it before the callback.

Do not mark it when the callback is suppressed.

Do not mark it when no `m_on_present` callback exists.

All existing reset sites for `m_render_boundary_logged` during bind/rebind/session reset remain unchanged in this PR.

---

## 11. `present_common()` Target Shape

Keep the surrounding control flow unchanged.

The policy portion should become conceptually similar to:

```cpp
const auto xefg_present = d3d12->m_xefg_session.evaluate_present_policy(
    d3d12->is_xefg_source(),
    static_cast<bool>(d3d12->m_on_present));

const auto post_resize_ordinal = d3d12->is_xefg_source()
    ? d3d12->log_xefg_post_resize_present(swap_chain, kind, original_present)
    : 0;

if (xefg_present.log_first_render_boundary) {
    // existing enter log
}

if (xefg_present.resize_hold_active) {
    const auto suppressed_present =
        d3d12->m_xefg_session.note_suppressed_present(xefg_present);

    if (suppressed_present <= 3) {
        // existing log, using xefg_present.hold_trigger_event_id
    }
}

if (!xefg_present.suppress_render_callbacks && d3d12->m_on_present) {
    if (post_resize_ordinal == 1 && g_framework != nullptr) {
        g_framework->log_d3d12_resize_snapshot(
            "present_pre_render_callback",
            xefg_present.resize_event_id);
    }

    d3d12->m_on_present(*d3d12);

    if (post_resize_ordinal == 1 && g_framework != nullptr) {
        g_framework->log_d3d12_resize_snapshot(
            "present_post_render_callback",
            xefg_present.resize_event_id);
    }

    if (xefg_present.log_first_render_boundary) {
        // existing returned log
        d3d12->m_xefg_session.mark_render_boundary_logged(xefg_present);
    }
}
```

Then preserve the existing original-call section and post-call section.

The final branch remains behaviorally equivalent:

```cpp
if (xefg_present.suppress_render_callbacks) {
    g_framework->note_present_activity();
} else if (d3d12->m_on_post_present) {
    d3d12->m_on_post_present(*d3d12);
}
```

The exact local variable names may differ.

---

## 12. Do Not Move Post-Resize Present Sampling in 2R4A

Keep this method in `D3D12Hook`:

```cpp
uint32_t D3D12Hook::log_xefg_post_resize_present(
    IDXGISwapChain3* swap_chain,
    const char* kind,
    void* original_fn)
```

Keep its implementation behaviorally unchanged.

Especially preserve:

```text
consume sample first
-> if no sample OR debug logging disabled: return 0
-> otherwise log sample
-> return sample ordinal
```

Do not make `PresentDecision` contain or consume `PostResizePresentSample` in this PR.

Do not change when `present_pre_render_callback` / `present_post_render_callback` snapshots occur.

A later 2R4B may move this diagnostic decision behind the session with explicit preservation of the debug-dependent ordinal semantics.

---

## 13. Do Not Move Present / Present1 Mechanism

Keep in `D3D12Hook`:

```text
Present[8] / Present1[22] original lookup
IDXGISwapChain1 -> IDXGISwapChain3 QueryInterface for Present1
lifecycle mutex acquisition
tracked swapchain validation
Present-entry timestamp/count update
m_inside_present state
native queue discovery
nested Present direct forwarding
g_present_depth increment/decrement
m_ignore_next_present
original call
HRESULT logging
DEVICE_REMOVED logging
m_on_present invocation
m_on_post_present invocation
g_framework->note_present_activity()
```

The session must not call framework callbacks.

The session must not call original Present.

The session must not know `kind`, `original_present`, or the Present1 parameter object.

---

## 14. Preserve `note_present_activity()` Semantics

Current suppressed path intentionally updates the generic hook monitor without running renderer/mod GPU callbacks:

```cpp
if (suppress_render_callbacks) {
    g_framework->note_present_activity();
}
```

Preserve this exactly.

Do not move `note_present_activity()` into `XeFGPresentationSession`.

The session decides suppression; `D3D12Hook` executes the generic monitor-liveness mechanism.

Do not call both `note_present_activity()` and `m_on_post_present` on the same top-level Present if current code would choose only one.

---

## 15. Preserve Device-Removed Diagnostics

Keep this condition and its relative location after original Present unchanged:

```cpp
if (d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
    && result == DXGI_ERROR_DEVICE_REMOVED) {
    ...
}
```

Do not move device removal policy into the session in 2R4A.

Do not broaden the HRESULT condition.

---

## 16. Existing Helper Cleanup — Conservative Only

After the new session Present policy API is in use, these D3D12Hook wrappers may become unused in the Present path:

```cpp
is_xefg_resize_hold_active()
should_suppress_xefg_render_callbacks()
note_xefg_suppressed_present()
```

Do not make helper cleanup a goal of this PR.

Preferred approach:

- leave them temporarily if removing them would enlarge the header diff;
- remove only helpers proven to have no remaining production references;
- do not combine cleanup with unrelated XeFG API reshaping.

2R7 is the final broad transitional-surface cleanup stage.

---

## 17. Native Path Protection

This PR must not cause stale XeFG session state to affect a native Present.

Required invariant:

```text
if current swapchain source != XeFGInternal:
    PresentDecision.suppress_render_callbacks == false
    PresentDecision.resize_hold_active == false
    PresentDecision.log_first_render_boundary == false
    no suppressed-Present counter mutation
    no render-boundary flag mutation
```

The native Present path must continue to reach:

```text
m_on_present
-> original Present
-> m_on_post_present
```

under the same existing conditions.

Do not let `m_binding.observe_only()` alone suppress anything when `xefg_source == false`.

---

## 18. COM / Ownership Invariants

This PR must not change any COM lifetime rule.

Preserve:

```text
active XeFG swapchain = borrowed raw pointer
active queue/device = strong ComPtr inside XeFGBinding
candidate swapchain = strong while candidate/pending
old swapchain keepalive = bounded local ComPtr only during destructive teardown/rebind
```

Forbidden:

```text
persistent active swapchain AddRef
manual AddRef balancing tricks
Release-until-refcount loops
refcount drains
COM probing of stale borrowed swapchain on timeout
```

Present policy evaluation should not perform COM calls.

---

## 19. Expected File Scope

Prefer changes concentrated in:

```text
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp
src/D3D12Hook.cpp
```

`src/D3D12Hook.hpp` may change only if required for conservative helper cleanup or a narrow bridge declaration.

Do not modify:

```text
src/REFramework.cpp
src/compatibility/xefg/XeFGCompatibility.cpp
src/compatibility/xefg/XeFGCompatibility.hpp
src/compatibility/xefg/XeFGCandidateHandoff.*
src/compatibility/xefg/XeFGDiscovery.*
src/compatibility/xefg/XeFGBinding.*
src/compatibility/xefg/XeFGResizeLifecycle.*
```

unless compilation proves a tiny include-only change necessary.

No build-system file change should be necessary because no new source file is introduced.

---

## 20. Static Review Checklist

Review the final diff line-by-line against baseline `d6ac54c100fe6e73045b64028fde5a1100bdd963`.

Verify all of the following:

### Placement / ordering

- policy evaluation still occurs only after the nested-Present early-forward branch;
- post-resize helper call remains in the same relative position before suppressed counter logging / renderer callback;
- first-render-boundary enter log remains before renderer callback;
- first-render-boundary state commit remains after renderer callback returns;
- original Present call remains after the renderer callback section;
- device-removed diagnostic remains after original Present;
- suppressed path still calls `note_present_activity()` after original Present;
- non-suppressed path still calls `m_on_post_present()` after original Present.

### Policy equivalence

For XeFG render-capable + no hold:

```text
suppress = false
resize_hold = false
m_on_present runs if installed
m_on_post_present runs if installed
```

For XeFG observe-only + no hold:

```text
suppress = true
resize_hold = false
suppressed-Present counter does NOT increment
m_on_present does not run
original Present still runs
note_present_activity runs
m_on_post_present does not run
```

For XeFG resize hold:

```text
suppress = true
resize_hold = true
suppressed-Present counter increments exactly once
original Present still runs
note_present_activity runs
renderer callbacks remain suppressed
```

For native source:

```text
session policy is no-op
no XeFG state mutation
native callback flow unchanged
```

### Diagnostics

- first-render-boundary logs retain current strings;
- suppressed-Present logs retain current strings and first-three limit;
- hold trigger event ID is unchanged;
- post-resize logging helper unchanged;
- debug-off post-resize ordinal behavior unchanged;
- device removed logs unchanged.

---

## 21. Build / Validation Requirements

Run at minimum:

```powershell
cmake -S . -B build-2r4a -G "Visual Studio 17 2022" -A x64
cmake --build build-2r4a --config Release --target REFramework --parallel 4
$env:PYTHONUTF8='1'; python dev/audit_direct_access_clang.py
git diff --check
```

Also perform a focused source-diff audit proving that:

```text
REFramework.cpp unchanged
XeFGCompatibility unchanged
2R3B detach/Destroy unchanged
2R3C monitor evaluation unchanged
bind/rebind unchanged
candidate handoff unchanged
Resize callbacks unchanged
post-resize helper semantics unchanged
Present/Present1 original-call and callback order unchanged
```

If runtime testing is unavailable, state that explicitly in the PR description.

Do not claim DD2/MHW/XeFG runtime validation unless actually performed.

Because this PR changes the decision plumbing inside `present_common()`, a later runtime gate is still required before the final second-stage refactor is considered complete.

---

## 22. Runtime Smoke — Deferred but Required Before Final Integration

When runtime access is available, validate at minimum:

### Native protection

1. REFramework only, no OptiScaler / no XeFG.
2. REFramework + OptiScaler with XeFG not selected.

Expected:

```text
normal REF overlay
normal renderer callbacks
no XeFG suppression
no stale XeFG session takeover
```

### DD2 XeFG control

- cold launch;
- overlay open/close;
- gameplay;
- Alt+Tab;
- clean exit.

### MHW XeFG lifecycle

- cold launch;
- overlay;
- gameplay/load transition;
- repeated Alt+Tab;
- resize/fullscreen transition;
- clean exit.

Look specifically for:

```text
missing overlay callback
unexpected renderer suppression
Present re-entry loop
stale monitor liveness
unexpected DEVICE_REMOVED
stale binding after transition
```

Runtime smoke is not mandatory for opening this small structural PR if the environment cannot run games, but it remains part of the overall integration gate.

---

## 23. Stop Conditions

Stop and report instead of expanding scope if implementation appears to require any of the following:

```text
changing original Present / Present1 call order
moving m_swapchain_hook ownership
moving framework callbacks into the session
changing g_present_depth recursion logic
changing m_ignore_next_present
changing post-resize sample debug semantics
changing Resize lifecycle implementation
changing MHW-specific resize policy
changing hook-monitor behavior
changing bind/rebind transaction
adding timers/sleeps/retries
adding COM ownership to the active swapchain
introducing generic FG abstractions
```

If one of these seems necessary, do not silently incorporate it into 2R4A.

---

## 24. PR Description Requirements

The PR body should clearly state:

```text
2R4A only: XeFG Present suppression/render-boundary policy extraction.

The session now decides:
- XeFG resize-hold state for Present;
- observe-only/hold renderer suppression;
- first-render-boundary eligibility;
- suppressed-Present counter mutation through a session API.

D3D12Hook still executes:
- recursion handling;
- renderer callbacks;
- original Present/Present1;
- post-Present callback / note_present_activity;
- post-resize diagnostic helper;
- device-removed handling.

No Present/Present1 ordering change is intended.
No post-resize diagnostic/sample move is included.
No runtime smoke result is claimed unless actually performed.
```

Include exact build/static validation performed.

---

## 25. Acceptance Criteria

2R4A is complete when all of the following are true:

- `XeFGPresentationSession` exposes one narrow Present policy decision;
- observe-only suppression decision is no longer reconstructed directly in `present_common()`;
- resize-hold suppression decision is no longer reconstructed directly in `present_common()`;
- first-render-boundary eligibility is decided by the session;
- render-boundary state mutation is committed through the session after renderer callback returns;
- resize-hold suppressed-Present counting is performed through the session at the same relative point;
- native Present remains unaffected when XeFG source is inactive;
- nested Present bypasses policy evaluation exactly as before;
- post-resize sample/log helper remains behaviorally unchanged;
- original Present / Present1 invocation order remains unchanged;
- `m_on_present`, `m_on_post_present`, and `note_present_activity()` branch behavior remains unchanged;
- no COM ownership change;
- no Resize policy change;
- no hook-monitor change;
- x64 Release build passes;
- direct-access audit passes;
- `git diff --check` passes.

---

## 26. Expected Follow-Up

After 2R4A is reviewed and merged, the next small step should be **2R4B**.

2R4B can address the remaining XeFG Present-side diagnostic/sample interpretation, especially:

```text
consume_post_resize_present_sample()
debug-dependent returned ordinal
ordinal-1 pre/post renderer snapshots
remaining Present-side direct resize-lifecycle reads
transitional helper cleanup
```

That later PR must explicitly preserve the current subtle rule:

```text
sample consumption occurs even with debug logging disabled,
but ordinal-driven extra diagnostics currently remain disabled when debug logging is disabled.
```

Do not pull 2R4B into this PR.

---

## Final Rule

The architectural boundary for this PR is:

> `XeFGPresentationSession` decides whether XeFG allows renderer work at this top-level Present boundary. `D3D12Hook` still performs all actual D3D12 Present/Present1 mechanics and REFramework callbacks in the exact existing order.

When XeFG is inactive, the session must be a no-op and REFramework must behave like normal REFramework.
