# Work Order — 2R4B: Move XeFG Post-Resize Present Sample / Diagnostic Decision Behind `XeFGPresentationSession`

Date: 2026-09-09  
Repository: `onehoon/REFramework`  
PR base branch: `REFforXeFG`  
Planning baseline reviewed: `REFforXeFG` @ `40a6a891f2cd30b88f0b605cddfd501dcc4b572b`  
Architecture authority: `doc/2ndrefactor/REFramework_OPTISCALER_XEFG_SECOND_STAGE_REFACTOR_DESIGN_2026-09-09.md`  
Previous step: 2R4A merged via PR #43

---

## 1. Objective

Complete the remaining small part of conceptual **2R4 — Move XeFG Present / Present1 Policy Into the Session**.

2R4A already moved the main XeFG Present policy behind `XeFGPresentationSession`:

- observe-only renderer suppression;
- resize-hold renderer suppression;
- resize-hold suppressed-Present counting;
- first-render-boundary eligibility;
- first-render-boundary state commit;
- XeFG Present resize-event / hold-trigger metadata used by the callback path.

One XeFG-specific stateful Present concern still remains in `D3D12Hook.cpp`:

```cpp
D3D12Hook::log_xefg_post_resize_present(...)
```

That helper currently does two different jobs:

```text
semantic/session work
    consume one post-resize Present sample from XeFGResizeLifecycle
    advance the 3-Present diagnostic budget / ordinal
    determine whether the sample becomes visible to downstream diagnostics

physical/diagnostic mechanism
    log thread / swapchain / physical-hook / queue / device information
    return an ordinal used by D3D12Hook to decide pre/post renderer snapshots
```

The goal of 2R4B is:

> Move ownership of post-resize Present sample consumption and diagnostic visibility decisions behind `XeFGPresentationSession`, while leaving actual logging, physical D3D12 values, framework snapshot calls, renderer callbacks, and original Present / Present1 execution inside `D3D12Hook`.

This is a **behavior-preserving refactor**.

After 2R4B, conceptual 2R4 should be considered complete. The next architectural stage is 2R5 (XeFG resize policy and the MHW-specific rule).

---

## 2. Current Code Reviewed

At baseline `40a6a891f2cd30b88f0b605cddfd501dcc4b572b`, top-level `D3D12Hook::present_common()` performs:

```cpp
const auto xefg_present = d3d12->m_xefg_session.evaluate_present_policy(
    d3d12->is_xefg_source(),
    static_cast<bool>(d3d12->m_on_present));

const auto post_resize_ordinal = d3d12->is_xefg_source()
    ? d3d12->log_xefg_post_resize_present(swap_chain, kind, original_present)
    : 0;
```

Then the existing ordering is:

```text
Present policy evaluation
-> consume/log post-resize Present sample
-> first-render-boundary enter log
-> resize-hold suppressed-Present count/log
-> optional pre-render resize snapshot
-> m_on_present
-> optional post-render resize snapshot
-> first-render-boundary returned log / state commit
-> original Present / Present1
-> XeFG DEVICE_REMOVED diagnostic
-> suppressed path: note_present_activity()
   otherwise: m_on_post_present
```

The current helper is:

```cpp
uint32_t D3D12Hook::log_xefg_post_resize_present(
    IDXGISwapChain3* swap_chain,
    const char* kind,
    void* original_fn) {

    const auto sample =
        m_xefg_session.resize_lifecycle().consume_post_resize_present_sample();

    if (!sample.has_value() || !XeFGCompatibility::is_debug_log_enabled()) {
        return 0;
    }

    // existing [XeFG][ResizeLifecycle] present_after_resize log

    return sample->ordinal;
}
```

This exact behavior is the baseline.

---

## 3. Critical Existing Semantics — Do Not Lose These

### 3.1 Sample consumption is independent of debug logging

This is the most important 2R4B invariant.

Current behavior is:

```text
consume_post_resize_present_sample()
    happens first

then:
    debug OFF -> return 0
    debug ON  -> emit log and return raw sample ordinal
```

Therefore:

> A post-resize sample is consumed even when XeFG debug logging is disabled.

Do **not** change the implementation into:

```cpp
if (!XeFGCompatibility::is_debug_log_enabled()) {
    return {};
}

consume_post_resize_present_sample();
```

That would incorrectly preserve the sample budget while debug logging is off and would change future ordinals.

### 3.2 Debug-off downstream ordinal remains effectively zero

Current caller only runs renderer-boundary resize snapshots when:

```cpp
post_resize_ordinal == 1
```

Because the old helper returns `0` whenever debug logging is off, these calls currently do **not** execute with debug logging disabled:

```text
present_pre_render_callback
present_post_render_callback
```

2R4B must preserve that behavior exactly.

Do not expose the raw consumed ordinal to the caller when diagnostics are disabled in a way that makes those snapshots run.

### 3.3 Three-sample budget remains unchanged

`XeFGResizeLifecycle::begin()` currently resets:

```text
m_post_resize_present_budget = 3
m_post_resize_present_ordinal = 0
```

`consume_post_resize_present_sample()` then consumes up to three Present samples.

Do not modify:

- budget size;
- ordinal progression;
- elapsed-time calculation;
- event ID source;
- resize-kind source.

### 3.4 Sample consumption occurs before resize-hold suppressed-Present counting

Current top-level order is:

```text
post-resize sample consume/log
-> first-render-boundary enter decision/log
-> resize-hold note_suppressed_present()
```

Preserve this order.

Do not fold sample consumption into `evaluate_present_policy()` because 2R4A intentionally kept that method pure and because doing so would make review of state mutation ordering harder.

### 3.5 Nested Present must not consume samples

The existing `g_present_depth > 0` direct-forward path returns before the XeFG policy block.

Keep post-resize sample consumption below that guard.

Nested Present / Present1 must not decrement the post-resize sample budget.

### 3.6 Native Present must not consume XeFG samples

When `xefg_source == false`, the session must remain dormant.

No native D3D12 Present may consume `XeFGResizeLifecycle` post-resize budget.

---

## 4. Non-Negotiable Scope

This PR is only the remaining XeFG post-resize Present sample / diagnostic decision extraction.

Do not intentionally modify:

- D3D11;
- Streamline / DLSSG;
- FSRFG;
- native D3D12 discovery;
- native phase-1 behavior;
- native Present1 installation;
- Present / Present1 original lookup;
- Present / Present1 recursion handling;
- `g_present_depth` behavior;
- `m_ignore_next_present` behavior;
- generic raw alias updates;
- native command-queue discovery;
- `m_on_present` ownership or invocation;
- `m_on_post_present` ownership or invocation;
- original Present / Present1 ordering;
- `g_framework->note_present_activity()` behavior;
- DEVICE_REMOVED handling;
- 2R3 hook-monitor policy;
- 2R3 runtime detach / Destroy lifecycle;
- candidate handoff;
- bind / rebind transactions;
- COM ownership;
- physical hook ownership;
- ResizeBuffers / ResizeBuffers1 / ResizeTarget behavior;
- MHW-specific resize-hold activation rule;
- XeFG resize hold arm / complete / clear policy;
- Intel runtime calls;
- loader behavior;
- anti-tamper / integrity behavior.

Do not begin 2R5 in this PR.

Do not move `sdk::GameIdentity` policy.

Do not introduce a generic frame-generation Present abstraction.

---

## 5. Keep `PresentDecision` Pure

The 2R4A method must remain a read-only classification step:

```cpp
PresentDecision evaluate_present_policy(
    bool xefg_source,
    bool render_callback_available) const noexcept;
```

Do not make it consume post-resize samples.

Required flow remains:

```text
nested guard passed
-> evaluate_present_policy()       // pure
-> consume post-resize sample      // state mutation in 2R4B method
-> diagnostic emission
-> suppressed-Present counting
-> callbacks / original Present
```

This separation is intentional.

---

## 6. Add a Narrow Post-Resize Present Decision Type

Add an XeFG-specific result type under `XeFGPresentationSession`.

Recommended shape:

```cpp
struct PostResizePresentDecision {
    std::optional<XeFGResizeLifecycle::PostResizePresentSample> sample{};
    bool emit_present_after_resize_log{};
    bool capture_renderer_snapshots{};
};
```

Exact names may differ.

The result should express all semantic information that `D3D12Hook` needs without making `D3D12Hook` directly consume `XeFGResizeLifecycle` state.

Do not put any of the following into the result:

- `D3D12Hook*`;
- `IDXGISwapChain3` ownership;
- `ComPtr`;
- callback objects;
- original Present pointers;
- thread IDs;
- queue / device pointers;
- vtable hook objects;
- formatted log strings.

The existing `XeFGResizeLifecycle::PostResizePresentSample` is already the correct semantic sample payload and should be reused rather than duplicated.

---

## 7. Add One Session Method That Consumes the Sample

Preferred conceptual API:

```cpp
PostResizePresentDecision consume_post_resize_present(
    const PresentDecision& present,
    bool diagnostics_enabled) noexcept;
```

Required semantics:

```cpp
PostResizePresentDecision result{};

if (!present.xefg_source) {
    return result;
}

result.sample = m_resize_lifecycle.consume_post_resize_present_sample();

if (!result.sample.has_value()) {
    return result;
}

// IMPORTANT: sample is already consumed at this point.
if (!diagnostics_enabled) {
    return result;
}

result.emit_present_after_resize_log = true;
result.capture_renderer_snapshots = result.sample->ordinal == 1;
return result;
```

The exact implementation may differ, but the observable semantics above are mandatory.

Important:

```text
diagnostics_enabled == false
    MUST NOT prevent sample consumption
```

The session does not call `XeFGCompatibility::is_debug_log_enabled()` itself.

`D3D12Hook` supplies a plain boolean so `XeFGPresentationSession` remains independent of the compatibility façade.

---

## 8. Preserve the Existing Diagnostic Visibility Contract

The session decision must produce this effective table:

| XeFG source | Sample available | Debug diagnostics | Present-after-resize log | Renderer pre/post snapshots |
|---|---|---|---|---|
| no | irrelevant | either | no | no |
| yes | no | either | no | no |
| yes | yes | off | no | no |
| yes | ordinal 1 | on | yes | yes, if renderer callback actually executes |
| yes | ordinal 2/3 | on | yes | no |

The `capture_renderer_snapshots` decision only means the sample ordinal/debug policy allows snapshots.

Actual snapshot execution must still remain inside the existing render-callback block:

```cpp
if (!xefg_present.suppress_render_callbacks && d3d12->m_on_present) {
    ...
}
```

Therefore observe-only / resize-hold suppression still prevents renderer snapshots even if the consumed sample is ordinal 1.

No session method should call `g_framework->log_d3d12_resize_snapshot()`.

---

## 9. Keep Physical Diagnostic Emission in `D3D12Hook`

`XeFGPresentationSession` should decide **whether** a post-resize diagnostic is visible and provide the semantic sample.

`D3D12Hook` should continue to emit the log because the log includes physical/generic information owned by `D3D12Hook`:

```text
GetCurrentThreadId()
current Present swapchain
renderer-facing tracked swapchain
physical VtableHook instance
active semantic binding swapchain/generation
raw command queue
device
original Present function
module/function owner description
```

Do not move those physical values into the session.

Refactor the helper so it no longer consumes lifecycle state.

Recommended shape:

```cpp
void D3D12Hook::log_xefg_post_resize_present(
    const XeFGPresentationSession::PostResizePresentDecision& decision,
    IDXGISwapChain3* swap_chain,
    const char* kind,
    void* original_fn) const {

    if (!decision.emit_present_after_resize_log
        || !decision.sample.has_value()) {
        return;
    }

    const auto& sample = *decision.sample;

    spdlog::info(
        "[XeFG][ResizeLifecycle] event_id = {}, kind = {}, "
        "stage = present_after_resize, present_ordinal = {}, ...",
        sample.event_id,
        kind,
        sample.ordinal,
        ...);
}
```

Exact signature may differ.

The helper may become `const` because it should be logging-only after this refactor, but making it `const` is optional.

Do not leave a second hidden call to:

```cpp
m_xefg_session.resize_lifecycle().consume_post_resize_present_sample()
```

inside `D3D12Hook`.

---

## 10. `present_common()` Target Shape

The top-level policy area should become conceptually similar to:

```cpp
const auto xefg_present = d3d12->m_xefg_session.evaluate_present_policy(
    d3d12->is_xefg_source(),
    static_cast<bool>(d3d12->m_on_present));

const auto post_resize =
    d3d12->m_xefg_session.consume_post_resize_present(
        xefg_present,
        XeFGCompatibility::is_debug_log_enabled());

d3d12->log_xefg_post_resize_present(
    post_resize,
    swap_chain,
    kind,
    original_present);
```

Then preserve the existing relative order:

```text
post-resize log emission
-> first-render-boundary enter log
-> resize-hold suppressed-Present count/log
-> renderer callback block
```

Inside the existing renderer callback block:

```cpp
if (!xefg_present.suppress_render_callbacks && d3d12->m_on_present) {
    if (post_resize.capture_renderer_snapshots && g_framework != nullptr) {
        g_framework->log_d3d12_resize_snapshot(
            "present_pre_render_callback",
            xefg_present.resize_event_id);
    }

    d3d12->m_on_present(*d3d12);

    if (post_resize.capture_renderer_snapshots && g_framework != nullptr) {
        g_framework->log_d3d12_resize_snapshot(
            "present_post_render_callback",
            xefg_present.resize_event_id);
    }

    ... existing first-render-boundary commit ...
}
```

Then preserve the original Present / Present1 section and post-Present section unchanged.

---

## 11. Preserve the Existing Event-ID Distinction

Current code has two closely related event-ID values:

### `present_after_resize` log

The log uses:

```cpp
sample->event_id
```

from `XeFGResizeLifecycle::PostResizePresentSample`.

### Renderer pre/post snapshots

After 2R4A, the callback block uses:

```cpp
xefg_present.resize_event_id
```

captured by `evaluate_present_policy()` before the post-resize helper runs.

Do not casually merge these two values in 2R4B.

Keep:

```text
present_after_resize log -> decision.sample->event_id
renderer pre/post snapshot -> xefg_present.resize_event_id
```

They are expected to normally represent the same resize generation at this point, but preserving the current data source makes the refactor mechanically behavior-preserving and avoids introducing a new callback-time semantic assumption.

---

## 12. `XeFGResizeLifecycle` Should Not Need Functional Changes

The existing class already exposes the required primitive:

```cpp
std::optional<PostResizePresentSample>
consume_post_resize_present_sample();
```

Its behavior is correct for this PR.

Do not intentionally modify:

```cpp
begin()
arm()
complete()
clear()
consume_post_resize_present_sample()
```

Do not change:

```text
m_post_resize_present_budget
m_post_resize_present_ordinal
m_last_event_time
m_event_id
m_last_kind
```

If `XeFGResizeLifecycle.cpp/.hpp` changes for anything beyond an unavoidable type-access compile fix, stop and explain why rather than broadening 2R4B silently.

---

## 13. Expected File Changes

Expected implementation files:

```text
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp
src/D3D12Hook.hpp
src/D3D12Hook.cpp
```

Normally unchanged:

```text
src/compatibility/xefg/XeFGResizeLifecycle.hpp
src/compatibility/xefg/XeFGResizeLifecycle.cpp
src/compatibility/xefg/XeFGCompatibility.hpp
src/compatibility/xefg/XeFGCompatibility.cpp
src/compatibility/xefg/XeFGBinding.hpp
src/compatibility/xefg/XeFGBinding.cpp
src/compatibility/xefg/XeFGCandidateHandoff.cpp
src/compatibility/xefg/XeFGDiscovery.cpp
src/REFramework.cpp
CMakeLists.txt
cmake.toml
```

No new source files are required, so generated build metadata should not change.

---

## 14. Do Not Perform Unrelated Helper Cleanup

There are still transitional XeFG helper wrappers in `D3D12Hook.hpp` from earlier steps.

Do not use 2R4B as a general cleanup PR.

In particular, avoid removing unrelated monitor / resize / bind wrappers merely because they currently look unused.

Broad XeFG surface cleanup belongs to the final consolidation stage (2R7) after Present, Resize, and candidate transaction work is complete.

For 2R4B, only change the `log_xefg_post_resize_present` signature/surface required by this extraction.

---

## 15. Static Behavior Audit — Required

Review `present_common()` line-by-line against baseline `40a6a891f2cd30b88f0b605cddfd501dcc4b572b`.

For a top-level tracked XeFG Present / Present1, prove the following order remains:

```text
Present entry/liveness update
-> generic aliases/native-only queue logic
-> nested guard
-> evaluate_present_policy()
-> consume one post-resize sample if budget exists
-> emit present_after_resize diagnostic only under existing debug condition
-> first-render-boundary enter log
-> resize-hold suppressed-Present count
-> optional renderer pre snapshot
-> m_on_present
-> optional renderer post snapshot
-> first-render-boundary state commit
-> original Present / Present1
-> DEVICE_REMOVED diagnostic
-> note_present_activity() OR m_on_post_present
```

Also prove:

```text
native path -> zero XeFG sample mutation
nested Present -> zero XeFG sample mutation
debug off -> sample still consumed
debug off -> no pre/post renderer snapshots
sample ordinal 2/3 -> no pre/post renderer snapshots
observe-only -> no renderer snapshot/callback
resize hold -> no renderer snapshot/callback, suppressed counter still increments once
```

---

## 16. Direct-Access Audit

After the PR, search for:

```text
consume_post_resize_present_sample
```

Expected production ownership:

```text
XeFGResizeLifecycle.hpp/.cpp
XeFGPresentationSession.cpp
```

`D3D12Hook.cpp` should no longer directly call:

```cpp
m_xefg_session.resize_lifecycle().consume_post_resize_present_sample()
```

The physical log helper may read session binding/physical D3D12 aliases for diagnostics, but it should not own sample consumption.

---

## 17. Build / Validation Requirements

Run at minimum:

```powershell
cmake -S . -B build-2r4b -G "Visual Studio 17 2022" -A x64
cmake --build build-2r4b --config Release --target REFramework --parallel 4
$env:PYTHONUTF8='1'; python dev/audit_direct_access_clang.py
git diff --check
```

Expected:

```text
Release x64 build: PASS
direct-access audit: Total violations: 0
git diff --check: PASS
```

Also run a source-scope audit confirming there are no changes to:

```text
REFramework.cpp
XeFGCompatibility runtime-transition code
candidate handoff
bind/rebind transactions
XeFG resize callback behavior
MHW resize policy
D3D11 / Streamline / FSRFG
```

If no game/XeFG runtime environment is available, do not claim runtime validation.

---

## 18. Runtime Gate if Available

2R4B should not intentionally alter rendering behavior, but because it runs in the top-level Present path, a small smoke gate is useful when a suitable machine is available.

### A. Native REF only

```text
OptiScaler absent
XeFG absent
```

Check:

- launch;
- REF overlay;
- gameplay;
- clean exit;
- no Present regression.

### B. OptiScaler present, XeFG not selected

Check:

- native/other FG presentation remains unchanged;
- XeFG session remains dormant.

### C. DD2 + OptiScaler + Intel XeFG

Check:

- cold launch;
- REF overlay visible;
- XeFG active;
- gameplay;
- Alt+Tab 1–2 times;
- clean exit;
- no missing renderer callback;
- no repeated monitor recovery loop.

Debug-on validation, if convenient, should show the same first three post-resize Present samples/ordinals as before.

Runtime smoke is not a substitute for the static ordering audit.

---

## 19. Suggested Branch / PR

Suggested implementation branch:

```text
refactor/xefg-2r4b-post-resize-present-session
```

Base:

```text
REFforXeFG
```

Suggested PR title:

```text
XeFG 2R4B: move post-resize Present diagnostics into session
```

Suggested summary:

```text
- move post-resize Present sample consumption behind XeFGPresentationSession
- encode debug-visible Present-after-resize and renderer-snapshot decisions in a narrow session result
- keep physical logging and framework snapshot execution in D3D12Hook
- preserve debug-off sample consumption, 3-sample budget, callback ordering, and native dormancy
```

Explicit scope statement:

> 2R4B completes Present-policy extraction only. No Resize policy move, no MHW rule move, no bind/rebind change, no hook-monitor change, no runtime lifecycle change, and no physical hook ownership change.

---

## 20. Stop Conditions

Stop and report instead of expanding scope if implementation appears to require any of the following:

- changing `XeFGResizeLifecycle::begin/arm/complete/clear` semantics;
- changing the 3-sample post-resize budget;
- changing debug-off sample consumption;
- moving renderer callbacks into the session;
- moving `g_framework` calls into the session;
- moving physical swapchain/queue/device data into the session;
- changing Present / Present1 recursion behavior;
- changing original Present ordering;
- modifying ResizeBuffers / ResizeTarget / ResizeBuffers1;
- moving the MHW-specific resize policy;
- changing candidate handoff or lock ordering;
- changing bind/rebind transaction ordering;
- changing COM ownership;
- changing hook-monitor recovery policy;
- adding timers, sleeps, polling, or worker threads;
- introducing a generic FG abstraction.

---

## 21. Completion Rule

2R4B is complete when:

> `XeFGPresentationSession` owns the stateful decision to consume and expose post-resize Present samples, while `D3D12Hook` only performs the existing physical diagnostics/callback mechanism according to that decision.

The key behavioral invariant is:

```text
debug OFF:
    sample budget still advances
    no present_after_resize log
    no ordinal-1 renderer snapshots

debug ON:
    same three samples / ordinals as before
    same present_after_resize log
    ordinal 1 only may trigger existing renderer pre/post snapshots
```

No other Present, Resize, lifecycle, ownership, or recovery behavior should change.

After this PR, proceed to **2R5 — move XeFG resize policy and the MHW-specific hold rule behind `XeFGPresentationSession`**.
