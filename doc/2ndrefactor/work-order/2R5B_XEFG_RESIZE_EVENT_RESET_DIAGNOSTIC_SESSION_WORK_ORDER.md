# Work Order — 2R5B: Move Remaining XeFG Resize Event / Reset Interpretation Behind `XeFGPresentationSession`

Date: 2026-09-09  
Repository: `onehoon/REFramework`  
PR base branch: `REFforXeFG`  
Planning baseline reviewed: `REFforXeFG` @ `687ce14a78b73beccdc19d591b8f528ec9d02105`  
Architecture authority: `doc/2ndrefactor/REFramework_OPTISCALER_XEFG_SECOND_STAGE_REFACTOR_DESIGN_2026-09-09.md`  
Previous step: 2R5A merged via PR #45

---

## 1. Objective

Complete conceptual **2R5 — Move XeFG Resize Policy and MHW Rule Into the Session**.

2R5A already moved:

- MHW-only `ResizeTarget` hold activation policy;
- resize-hold arm mutation;
- resize-hold completion semantics;
- resize-hold clear semantics;
- MHW `GameIdentity` dependency out of `D3D12Hook.cpp`.

One set of XeFG resize responsibilities remains spread through `D3D12Hook`:

```text
XeFG resize-event begin eligibility
tracked/top-level interpretation
ResizeBuffers1 observe-only renderer-reset eligibility
session-specific resize lifecycle diagnostic state reads
```

The goal of 2R5B is:

> Move the remaining XeFG resize-event and XeFG-specific reset-policy interpretation behind `XeFGPresentationSession`, while leaving physical DXGI resize forwarding, recursion handling, renderer reset callback execution, raw width/height aliases, physical swapchain/hook inspection, framework snapshot calls, and log emission in `D3D12Hook`.

This must remain a **behavior-preserving refactor**.

After 2R5B, conceptual 2R5 is complete. Do not begin 2R6 candidate/bind/rebind consolidation in this PR.

---

## 2. Current Code Reviewed

Baseline: `687ce14a78b73beccdc19d591b8f528ec9d02105`.

### Current event begin bridge

`D3D12Hook` still directly begins the session lifecycle:

```cpp
uint64_t D3D12Hook::begin_xefg_resize_event(XefgResizeEventKind kind) {
    return m_xefg_session.resize_lifecycle().begin(kind);
}

uint64_t D3D12Hook::begin_tracked_xefg_resize_event(
    IDXGISwapChain3* swapchain,
    XefgResizeEventKind kind,
    bool top_level) {

    if (!top_level || !is_tracked_xefg_instance(swapchain)) {
        return 0;
    }

    return begin_xefg_resize_event(kind);
}
```

### Current `ResizeBuffers1` reset policy

`ResizeBuffers1` still directly interprets binding mode:

```cpp
const auto should_reset_renderer =
    !d3d12->m_xefg_session.binding().observe_only()
    && static_cast<bool>(d3d12->m_on_resize_buffers);
```

This is XeFG policy and should move behind the session.

### Current resize diagnostic reads

`D3D12Hook::log_xefg_resize_event()` still directly reads semantic session state several times:

```cpp
m_xefg_session.binding().swapchain()
m_xefg_session.binding().generation()
m_xefg_session.binding().observe_only()
```

`get_xefg_last_resize_kind()` also directly reads:

```cpp
m_xefg_session.resize_lifecycle().last_kind()
```

The physical log itself correctly remains in `D3D12Hook` because it also includes:

```text
thread ID
current physical Present/resize swapchain
IUnknown identity QI
VtableHook instance
raw renderer-facing swapchain/queue/device aliases
original function address/module owner
```

2R5B should move only the semantic snapshot source, not the physical diagnostic mechanism.

---

## 3. Critical Existing Resize Semantics

All of these are non-negotiable.

### 3.1 `ResizeBuffers`

Current XeFG event begin eligibility is:

```text
top-level call
AND tracked XeFG instance
    -> begin XeFG ResizeBuffers event

nested call OR native/untracked call
    -> no XeFG event
```

But the physical renderer reset callback remains generic:

```cpp
if (d3d12->m_on_resize_buffers) {
    d3d12->m_on_resize_buffers(*d3d12);
}
```

Do **not** make generic `ResizeBuffers` renderer reset depend on XeFG observe-only state.

### 3.2 `ResizeBuffers1`

Current order is:

```text
validate d3d12 / hook / swapchain
-> lookup original ResizeBuffers1
-> if not tracked XeFG instance: direct-forward original
-> if nested: direct-forward original
-> begin XeFG ResizeBuffers1 event
-> update display dimensions
-> render reset only if NOT observe-only and callback exists
-> original ResizeBuffers1
-> result logging
-> complete existing resize hold using 2R5A session API
```

Preserve this exact structure.

Observe-only XeFG must continue to skip `m_on_resize_buffers` in `ResizeBuffers1`.

### 3.3 `ResizeTarget`

Current event begin eligibility is:

```text
top-level call
AND tracked XeFG instance
    -> begin XeFG ResizeTarget event
```

But renderer reset remains:

```cpp
if (d3d12->m_on_resize_target) {
    d3d12->m_on_resize_target(*d3d12);
}
```

This currently runs independently of observe-only mode.

Do **not** normalize `ResizeTarget` to the `ResizeBuffers1` observe-only rule.

2R5A already moved only the MHW hold decision after this reset. Keep that behavior intact.

### 3.4 Nested calls must not begin XeFG resize events

Do not move event mutation above the existing recursion/depth guards in a way that causes nested resize callbacks to consume event IDs or reset post-resize sample state.

### 3.5 Native D3D12 remains dormant

Native/non-XeFG `ResizeBuffers` and `ResizeTarget` must not mutate `XeFGResizeLifecycle`.

A non-tracked `ResizeBuffers1` must continue to direct-forward before any XeFG lifecycle mutation.

### 3.6 `XeFGResizeLifecycle::begin()` behavior is unchanged

Do not alter:

```text
m_event_id increment
m_last_kind assignment
m_last_event_time update
post-resize sample budget reset to 3
post-resize ordinal reset to 0
```

2R5B moves who decides to call `begin()`. It does not change `begin()` itself.

---

## 4. Non-Negotiable Scope

Do not intentionally modify:

- Present / Present1 behavior from 2R4;
- post-resize Present sample semantics from 2R4B;
- MHW hold arm policy from 2R5A;
- resize-hold arm / complete / clear semantics from 2R5A;
- candidate handoff;
- initial bind / rebind transaction;
- runtime detach / Destroy behavior;
- hook-monitor classifier/timing;
- physical `m_swapchain_hook` / `m_present_hook` ownership;
- COM ownership;
- D3D11;
- Streamline / DLSSG;
- FSRFG;
- native D3D12 queue discovery;
- native phase-1 behavior;
- Intel XeFG runtime calls;
- anti-tamper/integrity behavior;
- MHW game selection policy;
- resize call arguments or HRESULT interpretation.

Do not introduce a generic frame-generation resize abstraction.

---

## 5. Move XeFG Resize-Event Begin Interpretation Behind the Session

Keep physical tracking in `D3D12Hook`.

The session must not inspect `VtableHook`, `m_swapchain_hook`, raw renderer aliases, or COM identity to decide whether a swapchain is physically tracked.

A narrow session API is recommended:

```cpp
struct ResizeEventDecision {
    bool active{};
    uint64_t event_id{};
    XeFGResizeLifecycle::EventKind kind{XeFGResizeLifecycle::EventKind::None};
};

ResizeEventDecision begin_resize_event(
    bool xefg_source,
    bool tracked_instance,
    bool top_level,
    XeFGResizeLifecycle::EventKind kind) noexcept;
```

Required semantics:

```cpp
ResizeEventDecision result{};
result.kind = kind;

if (!xefg_source || !tracked_instance || !top_level) {
    return result;
}

result.event_id = m_resize_lifecycle.begin(kind);
result.active = result.event_id != 0;
return result;
```

Exact names may differ.

The important boundary is:

```text
D3D12Hook
    determines physical tracked-instance and recursion/top-level status

XeFGPresentationSession
    decides whether semantic XeFG resize lifecycle begins
    performs the begin() mutation
```

### Recommended D3D12 bridge

`begin_tracked_xefg_resize_event()` may remain temporarily as a very thin bridge:

```cpp
uint64_t D3D12Hook::begin_tracked_xefg_resize_event(
    IDXGISwapChain3* swapchain,
    XefgResizeEventKind kind,
    bool top_level) {

    const auto decision = m_xefg_session.begin_resize_event(
        is_xefg_source(),
        is_tracked_xefg_instance(swapchain),
        top_level,
        kind);

    return decision.event_id;
}
```

If this shape is used, the old direct helper:

```cpp
begin_xefg_resize_event(...)
```

should normally become unnecessary and may be removed.

### `ResizeBuffers1`

After the existing non-tracked and nested early-return guards, use the same semantic begin path rather than calling `resize_lifecycle().begin()` directly.

Do not move begin above either early-return guard.

---

## 6. Move Only `ResizeBuffers1` Reset Eligibility Behind the Session

Do not create one universal resize-reset rule.

The only XeFG-specific reset rule to move in 2R5B is the current `ResizeBuffers1` rule:

```cpp
!m_binding.observe_only() && callback_available
```

Use a narrow method such as:

```cpp
bool should_reset_renderer_for_resize_buffers1(
    bool resize_callback_available) const noexcept {

    return !m_binding.observe_only() && resize_callback_available;
}
```

or a small XeFG-specific decision type.

Required behavior table:

| Mode | callback exists | `ResizeBuffers1` renderer reset |
|---|---:|---:|
| render-capable XeFG | yes | yes |
| render-capable XeFG | no | no |
| observe-only XeFG | yes | no |
| observe-only XeFG | no | no |

Do not apply this decision to generic `ResizeBuffers` or `ResizeTarget`.

Do not move actual callback invocation into the session.

`D3D12Hook` still executes:

```cpp
d3d12->m_on_resize_buffers(*d3d12);
```

at the same location.

---

## 7. Add a Narrow Resize Diagnostic Snapshot

The session should provide semantic resize/binding information in one read-only snapshot so physical log helpers no longer repeatedly interpret session internals.

Recommended shape:

```cpp
struct ResizeDiagnosticSnapshot {
    IDXGISwapChain3* binding_swapchain{}; // borrowed semantic alias only
    uint64_t binding_generation{};
    bool observe_only{};
    XeFGResizeLifecycle::EventKind last_kind{XeFGResizeLifecycle::EventKind::None};
};

ResizeDiagnosticSnapshot resize_diagnostic_snapshot() const noexcept;
```

This is a borrowed/read-only diagnostic view. It must not AddRef the swapchain.

Do not place in this snapshot:

- raw renderer queue/device aliases from `D3D12Hook`;
- `VtableHook*`;
- callback objects;
- framework pointers;
- original function pointers;
- thread IDs;
- formatted strings.

### Use in `log_xefg_resize_event()`

Replace repeated reads such as:

```cpp
m_xefg_session.binding().swapchain()
m_xefg_session.binding().generation()
m_xefg_session.binding().observe_only()
```

with one session snapshot.

Preserve the exact log message text and all physical values.

The following must remain physically sourced from `D3D12Hook`:

```text
swap_chain
IUnknown identity QI
m_swap_chain
m_swapchain_hook instance
m_command_queue
m_device
original_fn / owner
thread ID
```

### `get_xefg_last_resize_kind()`

It may delegate to a session getter or the diagnostic snapshot rather than reading `resize_lifecycle()` directly.

Do not remove this externally used diagnostic getter unless all call sites are explicitly proven absent.

### `log_xefg_post_resize_present()`

It is acceptable to reuse the same semantic resize diagnostic snapshot for binding swapchain/generation there if this reduces direct session reads without changing the log.

Do not change the 2R4B sample decision or event-ID source.

---

## 8. Preserve Exact Call-Site Ordering

### `ResizeBuffers`

Keep:

```text
mutex
-> physical original lookup
-> determine top-level/tracked XeFG event eligibility
-> semantic begin if eligible
-> enter diagnostics
-> display width/height aliases
-> existing nested handling
-> generic m_on_resize_buffers reset
-> original ResizeBuffers
-> original-return diagnostics
-> 2R5A hold completion
```

Do not reorder generic reset relative to original call.

### `ResizeBuffers1`

Keep:

```text
mutex
-> validate hook/swapchain
-> original lookup
-> non-tracked direct-forward
-> nested direct-forward
-> semantic event begin
-> enter diagnostics
-> display width/height aliases
-> session decides reset eligibility
-> if allowed: existing pre-reset diagnostics / callback / post-reset diagnostics
-> original ResizeBuffers1
-> result diagnostics
-> 2R5A hold completion
```

### `ResizeTarget`

Keep:

```text
mutex
-> original lookup
-> determine top-level/tracked event eligibility
-> semantic begin if eligible
-> render dimension aliases
-> enter diagnostics
-> existing nested handling
-> generic m_on_resize_target reset
-> 2R5A MHW hold arm bridge
-> original ResizeTarget
-> failed-result 2R5A hold clear
-> original-return diagnostics
```

Do not move the MHW hold arm before renderer reset.

---

## 9. Stale Transitional Helper Cleanup

After the implementation is complete, check whether these D3D12Hook helpers still have any call sites:

```cpp
is_xefg_render_capable()
is_xefg_resize_hold_active()
should_suppress_xefg_render_callbacks()
note_xefg_suppressed_present()
begin_xefg_resize_event()
```

Several became transitional after 2R4/2R5A.

If a helper has **zero call sites** on the PR head, it may be removed in 2R5B.

Do not remove helpers merely because they look redundant if they are still referenced by diagnostics/tests/other translation units.

Keep:

```cpp
is_xefg_source()
is_tracked_xefg_instance(...)
```

because they are physical D3D12 bridge concepts and remain appropriate.

Do not perform unrelated D3D12Hook cleanup.

---

## 10. Files Expected

Primary:

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
src/compatibility/xefg/XeFGCompatibility.cpp
src/compatibility/xefg/XeFGCandidateHandoff.cpp
src/REFramework.cpp
```

No CMake changes are expected because no new source files are required.

If implementation appears to require changes outside this expected set, stop and reassess before broadening scope.

---

## 11. Explicitly Forbidden Changes

Do not:

- change resize recursion counters/depth behavior;
- move session mutation before existing nested direct-forward guards;
- make native ResizeBuffers/ResizeTarget obey XeFG reset policy;
- suppress `ResizeTarget` reset in observe-only mode;
- enable `ResizeBuffers1` reset in observe-only mode;
- change MHW-only hold eligibility;
- complete a hold after successful ResizeTarget;
- clear hold after failed ResizeBuffers/ResizeBuffers1;
- alter post-resize sample budget or ordinal semantics;
- change HRESULT success/failure interpretation;
- add timers, sleeps, retries, polling, or fallback workers;
- add COM QueryInterface calls inside session policy methods;
- move physical hook ownership into the session;
- introduce a generic FG/provider abstraction;
- touch bind/rebind transactions planned for 2R6.

---

## 12. Validation

Run:

```text
cmake -S . -B build-2r5b -G "Visual Studio 17 2022" -A x64
cmake --build build-2r5b --config Release --target REFramework --parallel 4
python dev/audit_direct_access_clang.py
git diff --check
```

Required source-level audit:

```text
1. ResizeBuffers nested/native behavior unchanged.
2. ResizeBuffers1 non-tracked and nested direct-forward guards remain before session begin mutation.
3. ResizeBuffers1 observe-only still suppresses renderer reset.
4. ResizeTarget renderer reset behavior unchanged, including observe-only behavior.
5. MHW hold arm remains after ResizeTarget renderer reset and before original call.
6. ResizeTarget failure still clears with reason "resize_target_failed".
7. ResizeBuffers/ResizeBuffers1 hold completion call sites remain after original return.
8. No Present/Present1 diff except unavoidable compile-only declaration cleanup.
9. No bind/rebind/candidate/runtime-monitor behavior change.
10. No COM ownership change.
```

Runtime game validation may remain deferred in this environment. Do not claim it was performed if it was not.

Cumulative runtime gate later still includes:

```text
A. REF only / native D3D12
B. REF + OptiScaler, XeFG not selected
C. DD2 + OptiScaler + Intel XeFG
D. MHW + OptiScaler + Intel XeFG resize / Alt+Tab / lifecycle
```

---

## 13. Suggested PR

Branch:

```text
refactor/xefg-2r5b-resize-event-policy-session
```

PR title:

```text
XeFG 2R5B: move remaining resize policy into presentation session
```

Base:

```text
REFforXeFG
```

---

## 14. Stop Conditions

Stop and report instead of broadening the PR if implementation appears to require:

- changing generic ResizeBuffers/ResizeTarget callback policy;
- changing MHW hold behavior;
- modifying `XeFGResizeLifecycle::begin/arm/complete/clear` semantics;
- moving renderer callbacks into the session;
- changing Present/Present1 sequencing;
- touching candidate bind/rebind logic;
- adding COM ownership or persistent swapchain references;
- changing hook-monitor timing;
- touching Streamline/DLSSG/FSRFG;
- adding asynchronous recovery.

---

## 15. Definition of Done

2R5B is complete when:

- XeFG resize-event begin eligibility/mutation is behind `XeFGPresentationSession`;
- `ResizeBuffers1` observe-only reset eligibility is behind `XeFGPresentationSession`;
- D3D12Hook continues to own tracked-instance determination and physical resize execution;
- physical resize logs remain in D3D12Hook but use a narrow session semantic snapshot where appropriate;
- generic ResizeBuffers and ResizeTarget reset behavior is unchanged;
- MHW-only hold behavior from 2R5A is unchanged;
- nested/non-tracked resize paths do not mutate XeFG resize lifecycle;
- no Present, bind/rebind, monitor, runtime lifecycle, COM, or physical-hook behavior changes occur;
- Release build, direct-access audit, and diff-check pass.

After this PR, conceptual **2R5 is complete** and the next stage is **2R6 — consolidate candidate apply / bind / rebind transaction around session authority**.
