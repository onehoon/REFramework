# Work Order — 2R2: Move XeFG Session State Ownership into `XeFGPresentationSession`

Date: 2026-09-09  
Repository: `onehoon/REFramework`  
PR base branch: `REFforXeFG`  
Planning baseline: `REFforXeFG` @ `f289592b0b7c45f9c825cb15e4969678ec32be67`  
Architecture authority: `doc/2ndrefactor/REFramework_OPTISCALER_XEFG_SECOND_STAGE_REFACTOR_DESIGN_2026-09-09.md`  
Previous step: PR #38 / 2R1 merged

---

## 1. Objective

Implement the next deliberately small step of the second-stage OptiScaler + Intel XeFG refactor.

The goal of this PR is **state ownership only**:

> Make `XeFGPresentationSession` own the active XeFG semantic/lifecycle state that is currently stored directly on `D3D12Hook`, while keeping all current decisions and physical hook operations in `D3D12Hook`.

This PR must **not** move policy yet.

That means the existing `D3D12Hook` logic may continue to perform:

- binding consistency checks;
- runtime detach decisions;
- destroy-result reconciliation;
- hook-monitor decisions;
- Present/Present1 suppression decisions;
- resize-hold decisions;
- bind/rebind transactions;
- raw alias synchronization;
- physical `VtableHook` ownership/removal.

Only the location of the XeFG state changes.

This is intentionally narrower than the broader conceptual 2R2 in the architecture document. The user preference for this unreleased branch is to keep PRs small and independently reviewable, even if intermediate architecture is temporarily transitional.

---

## 2. Branch / PR Rules

Create a new implementation branch from the current `REFforXeFG` tip.

Suggested branch name:

```text
refactor/xefg-2r2-session-state
```

Open the PR against:

```text
base: REFforXeFG
```

Do **not** target `master`.

Do not rebase onto upstream `praydog/REFramework` in this PR.

---

## 3. Current State After 2R1

2R1 established:

```text
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp
```

and moved these type definitions out of `D3D12Hook.hpp`:

```text
XeFGMonitorBindingKey
XeFGHookMonitorState
XeFGDetachedState
```

However, the actual active state still lives directly on `D3D12Hook`:

```cpp
XeFGBinding m_xefg_binding{};
XeFGResizeLifecycle m_xefg_resize_lifecycle{};
XeFGDetachedState m_xefg_detached_state{};
XeFGHookMonitorState m_xefg_monitor_state{};
const char* m_last_xefg_monitor_action{};
...
bool m_xefg_p21_render_boundary_logged{ false };
```

`XeFGPresentationSession` is still an empty shell:

```cpp
class XeFGPresentationSession {
public:
    XeFGPresentationSession() = default;
};
```

This PR changes that ownership boundary.

---

## 4. Non-Negotiable Scope

This is XeFG-only work.

Do not intentionally modify:

- D3D11;
- DLSSG / Streamline behavior;
- FSRFG behavior;
- native D3D12 discovery;
- native Present behavior;
- native Present1 behavior;
- native ResizeBuffers / ResizeTarget behavior;
- REFramework renderer behavior;
- Lua / plugins / mods / input / VR / UI;
- anti-tamper/integrity behavior;
- OptiScaler code;
- Intel XeFG private object handling.

Do not introduce a generic frame-generation abstraction.

Forbidden examples remain:

```text
IFrameGenerationProvider
FrameGenerationSession
PresentationProvider
GenericSwapchainSession
multi-provider registry
```

The new owner remains explicitly `XeFGPresentationSession`.

---

# 5. Exact Ownership Move

Move ownership of exactly these six pieces of state from `D3D12Hook` into `XeFGPresentationSession`:

```text
1. XeFGBinding
2. XeFGResizeLifecycle
3. XeFGDetachedState
4. XeFGHookMonitorState
5. last XeFG monitor action
6. P2.1 first render-boundary diagnostic flag
```

Conceptually:

```cpp
class XeFGPresentationSession {
public:
    XeFGPresentationSession() = default;

    XeFGBinding& binding() noexcept { return m_binding; }
    const XeFGBinding& binding() const noexcept { return m_binding; }

    XeFGResizeLifecycle& resize_lifecycle() noexcept { return m_resize_lifecycle; }
    const XeFGResizeLifecycle& resize_lifecycle() const noexcept { return m_resize_lifecycle; }

    XeFGDetachedState& detached_state() noexcept { return m_detached_state; }
    const XeFGDetachedState& detached_state() const noexcept { return m_detached_state; }

    XeFGHookMonitorState& monitor_state() noexcept { return m_monitor_state; }
    const XeFGHookMonitorState& monitor_state() const noexcept { return m_monitor_state; }

    const char* last_monitor_action() const noexcept { return m_last_monitor_action; }
    void set_last_monitor_action(const char* action) noexcept { m_last_monitor_action = action; }

    bool render_boundary_logged() const noexcept { return m_render_boundary_logged; }
    void set_render_boundary_logged(bool value) noexcept { m_render_boundary_logged = value; }

private:
    XeFGBinding m_binding{};
    XeFGResizeLifecycle m_resize_lifecycle{};
    XeFGDetachedState m_detached_state{};
    XeFGHookMonitorState m_monitor_state{};
    const char* m_last_monitor_action{};
    bool m_render_boundary_logged{};
};
```

Exact naming may follow repository style.

The important requirement is ownership, not exact accessor spelling.

---

## 6. Include Requirements

`XeFGPresentationSession.hpp` currently includes `XeFGBinding.hpp`.

Because the session now owns resize state, also include:

```cpp
#include "XeFGResizeLifecycle.hpp"
```

Do not move `XeFGBinding` or `XeFGResizeLifecycle` implementation into the session source file.

They remain focused helper/state classes.

---

# 7. `D3D12Hook` Member Replacement

Remove these direct fields from `D3D12Hook`:

```cpp
XeFGBinding m_xefg_binding{};
XeFGResizeLifecycle m_xefg_resize_lifecycle{};
XeFGDetachedState m_xefg_detached_state{};
XeFGHookMonitorState m_xefg_monitor_state{};
const char* m_last_xefg_monitor_action{};
bool m_xefg_p21_render_boundary_logged{ false };
```

Replace them with one session object:

```cpp
XeFGPresentationSession m_xefg_session{};
```

Prefer a direct member object in this PR.

Do not introduce heap allocation or `std::unique_ptr<XeFGPresentationSession>` unless there is a demonstrated compile-time reason.

A direct member keeps construction/destruction deterministic and does not add a new allocation to the native D3D12 path.

Place the session member in approximately the same region where the old XeFG state fields lived so the relative destruction order versus `m_swapchain_hook`/`m_present_hook` is not needlessly changed.

---

# 8. Mechanical Reference Conversion Only

Update existing `D3D12Hook` code to access the same state through the session.

Examples:

### Binding

Before:

```cpp
m_xefg_binding.active()
m_xefg_binding.lifecycle_snapshot()
m_xefg_binding.observe_only()
m_xefg_binding.generation()
m_xefg_binding.commit_initial(...)
m_xefg_binding.commit_same_swapchain_update(...)
m_xefg_binding.commit_replacement(...)
m_xefg_binding.refresh_runtime_identity(...)
m_xefg_binding.clear()
```

After:

```cpp
m_xefg_session.binding().active()
m_xefg_session.binding().lifecycle_snapshot()
m_xefg_session.binding().observe_only()
m_xefg_session.binding().generation()
m_xefg_session.binding().commit_initial(...)
m_xefg_session.binding().commit_same_swapchain_update(...)
m_xefg_session.binding().commit_replacement(...)
m_xefg_session.binding().refresh_runtime_identity(...)
m_xefg_session.binding().clear()
```

### Resize lifecycle

Before:

```cpp
m_xefg_resize_lifecycle.begin(...)
m_xefg_resize_lifecycle.arm(...)
m_xefg_resize_lifecycle.complete(...)
m_xefg_resize_lifecycle.clear()
m_xefg_resize_lifecycle.suppress_renderer()
m_xefg_resize_lifecycle.note_suppressed_present()
m_xefg_resize_lifecycle.consume_post_resize_present_sample()
```

After:

```cpp
m_xefg_session.resize_lifecycle().begin(...)
m_xefg_session.resize_lifecycle().arm(...)
m_xefg_session.resize_lifecycle().complete(...)
m_xefg_session.resize_lifecycle().clear()
m_xefg_session.resize_lifecycle().suppress_renderer()
m_xefg_session.resize_lifecycle().note_suppressed_present()
m_xefg_session.resize_lifecycle().consume_post_resize_present_sample()
```

### Detached state

Before:

```cpp
m_xefg_detached_state.active
m_xefg_detached_state.previous_runtime
m_xefg_detached_state.previous_generation
m_xefg_detached_state = {...}
m_xefg_detached_state = {}
```

After, mechanically equivalent:

```cpp
m_xefg_session.detached_state().active
m_xefg_session.detached_state().previous_runtime
m_xefg_session.detached_state().previous_generation
m_xefg_session.detached_state() = {...}
m_xefg_session.detached_state() = {}
```

### Monitor state

Before:

```cpp
m_xefg_monitor_state.note_timeout(...)
m_xefg_monitor_state.clear()
m_xefg_monitor_state.consecutive_timeouts()
```

After:

```cpp
m_xefg_session.monitor_state().note_timeout(...)
m_xefg_session.monitor_state().clear()
m_xefg_session.monitor_state().consecutive_timeouts()
```

### Diagnostic flags

Before:

```cpp
m_last_xefg_monitor_action
m_xefg_p21_render_boundary_logged
```

After, through simple session getters/setters.

Do not redesign the decision logic while performing these replacements.

---

# 9. Existing Public / Protected `D3D12Hook` API Must Remain Stable

For this PR, keep existing external methods and semantics intact.

Examples that should continue to exist and return the same values:

```cpp
get_xefg_last_resize_event_id()
get_xefg_binding_generation()
get_xefg_last_resize_kind()
is_xefg_observe_only()
get_xefg_lifecycle_snapshot()
has_active_xefg_instance_binding()
has_consistent_active_xefg_binding()
has_xefg_monitor_state()
has_xefg_detached_state()
get_xefg_monitor_binding_key()
note_xefg_monitor_timeout()
get_xefg_timeout_count()
note_xefg_monitor_action()
clear_xefg_monitor_state()
is_xefg_render_capable()
is_xefg_resize_hold_active()
should_suppress_xefg_render_callbacks()
note_xefg_suppressed_present()
```

These methods may now read/write `m_xefg_session`, but **do not move their policy implementation into the session yet**.

That policy extraction belongs to later steps.

---

# 10. Do Not Introduce `PhysicalBindingView` Yet

The architecture document proposes a narrow physical consistency view such as:

```cpp
struct PhysicalBindingView {
    bool hook_object_active{};
    bool phase1{};
    IDXGISwapChain3* renderer_swapchain{};
    ID3D12CommandQueue* renderer_queue{};
    ID3D12Device4* renderer_device{};
    void* hook_target{};
};
```

Do **not** implement that in this PR unless needed solely for compilation.

Current `has_consistent_active_xefg_binding()` may continue to compare:

```text
session binding
vs
D3D12Hook raw aliases
vs
physical hook target
```

inside `D3D12Hook` exactly as before.

Reason: this PR is only the state-ownership move. The physical-view/API boundary can be introduced together with the policy extraction in the next step, where it provides actual value.

---

# 11. Critical Ownership Invariants

This PR touches the semantic owner, so COM lifetime rules are non-negotiable.

## 11.1 Active XeFG swapchain remains borrowed

`XeFGBinding` must remain unchanged:

```cpp
IDXGISwapChain3* m_swapchain{}; // borrowed
```

Do not convert it to:

```cpp
ComPtr<IDXGISwapChain3>
```

and do not add a persistent AddRef.

## 11.2 Queue/device remain strongly owned

Keep:

```cpp
ComPtr<ID3D12CommandQueue>
ComPtr<ID3D12Device4>
```

inside `XeFGBinding` exactly as today.

## 11.3 Candidate ownership remains unchanged

Do not change `XeFGBindingCandidate` ownership semantics.

## 11.4 Old swapchain bounded keepalive remains unchanged

The current local keepalive pattern around destructive hook removal must remain exactly scoped as today.

Do not move it into a persistent session field.

## 11.5 No manual COM draining

Forbidden:

```text
manual AddRef balancing tricks
Release-until-refcount loops
refcount drain loops
COM probing of stale borrowed objects
```

---

# 12. Bind / Rebind Must Remain Mechanically Identical

Do not rewrite:

```cpp
bind_external_swapchain(...)
replace_xefg_binding(...)
apply_xefg_candidate(...)
```

beyond replacing direct field access with session field access.

Preserve the current transactional replacement order:

```text
validate next candidate
-> take bounded strong next references
-> prepare all five XeFG hooks
-> if preparation fails, old active binding remains intact
-> renderer reset
-> remove old physical hook with bounded old keepalive
-> commit next semantic binding
-> synchronize renderer-facing raw aliases
-> install prepared physical hook
-> clear detached/monitor/resize state as current code does
```

Required XeFG hook methods remain:

```text
Present[8]
Present1[22]
ResizeBuffers[13]
ResizeTarget[14]
ResizeBuffers1[39]
```

Do not reorder them for cleanup purposes in this PR.

Same-object update semantics must also remain unchanged.

---

# 13. Runtime Detach / Destroy Must Remain Identical

Do not change the behavior of:

```cpp
detach_xefg_binding_for_runtime_transition(...)
note_xefg_destroy_result(...)
```

Only redirect their state access through `m_xefg_session`.

Preserve:

- exact-runtime matching;
- optional same-HWND matching;
- detached uncertainty creation;
- renderer reset ordering;
- bounded local old-swapchain keepalive;
- physical hook reset/removal;
- raw alias clearing only when matching the old binding;
- binding clear;
- negative Destroy result remaining fail-closed;
- matching non-negative Destroy clearing detached state;
- current `result >= 0` / `result < 0` XeFG result semantics.

Do not hold lifecycle locks across Intel vendor calls.

---

# 14. Present / Resize Policy Must Not Move Yet

Do not redesign or relocate Present/Present1 policy in this PR.

Current branches for:

```text
observe-only suppression
resize-hold suppression
suppressed Present counting
post-resize samples
first-render-boundary logging
note_present_activity()
renderer callbacks
```

must remain in the same current `D3D12Hook` control flow.

Likewise, resize policy remains in `D3D12Hook` for now, including the current MHW-specific hold rule.

Only the underlying state object is reached via the session.

---

# 15. Native REFramework Path Protection

When XeFG is inactive, the new session object must be passive.

This PR must not make session construction perform any of the following:

```text
module detection
hook installation
COM acquisition
Intel API calls
OptiScaler probing
thread creation
timer creation
logging
native D3D12 state mutation
```

Default construction should remain effectively state initialization only.

Do not make native D3D12 behavior depend on `m_xefg_session.active()` or similar new gating logic in this PR.

---

# 16. Source Files Expected to Change

Expected implementation diff should be small and concentrated:

```text
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp   (only if trivial accessor definitions are placed out-of-line)
src/D3D12Hook.hpp
src/D3D12Hook.cpp
```

Normally not expected:

```text
CMakeLists.txt
cmake.toml
```

The session files already exist and are already registered from 2R1.

If build metadata changes, explain why.

Do not modify unless required:

```text
src/compatibility/xefg/XeFGCompatibility.cpp
src/compatibility/xefg/XeFGCandidateHandoff.cpp
src/compatibility/xefg/XeFGDiscovery.cpp
src/compatibility/xefg/XeFGRuntimeRegistry.cpp
src/REFramework.cpp
```

If those files require behavioral edits, stop and report rather than expanding scope.

---

# 17. Required Mechanical Audit

After implementation, search the repository for all former direct owner fields:

```text
m_xefg_binding
m_xefg_resize_lifecycle
m_xefg_detached_state
m_xefg_monitor_state
m_last_xefg_monitor_action
m_xefg_p21_render_boundary_logged
```

Expected result:

```text
D3D12Hook direct member declarations -> none
session-owned equivalents          -> present in XeFGPresentationSession
D3D12Hook runtime logic             -> accesses session state
```

Do not leave duplicate copies of the same state on both classes.

The entire purpose of this PR is to eliminate duplicated ownership.

---

# 18. Validation

Because this PR changes actual state ownership, it has a higher bar than 2R1.

Run at minimum:

```text
cmake -S . -B <build-dir> -G "Visual Studio 17 2022" -A x64
cmake --build <build-dir> --config Release --target REFramework --parallel 4
python dev/audit_direct_access_clang.py
git diff --check
```

If project generation is needed for environmental reasons, use the repository's normal cmkr flow, but this PR should normally not require source-registration changes.

Required static results:

```text
Release x64 build: PASS
direct-access audit: 0 violations
git diff --check: PASS
```

---

# 19. Runtime Gate for 2R2

2R1 was allowed to merge without game testing because it moved only type definitions.

2R2 is the first PR that changes active state ownership, so after code review this becomes the first meaningful quick runtime gate.

Recommended minimal smoke before moving to the next stage:

## A. Native protection

```text
REFramework only
OptiScaler absent
XeFG absent
```

Check:

- game launches;
- REF overlay opens;
- basic gameplay works;
- clean exit;
- no repeated D3D rehook/reset loop.

## B. OptiScaler present, XeFG not selected

Check that presence of OptiScaler alone does not make the XeFG session take control.

## C. DD2 + OptiScaler + Intel XeFG

Quick smoke:

- cold launch;
- REF overlay visible;
- XeFG active;
- enter gameplay;
- Alt+Tab once or twice;
- clean exit;
- no stale binding / immediate crash / hook-monitor loop.

MHW full lifecycle stress is not mandatory for this PR unless a code-review concern specifically points to resize/detach behavior. The broader MHW gate belongs to the later resize/policy and bind/rebind steps.

Do not claim runtime tests in the PR unless actually performed.

---

# 20. PR Description Requirements

Open the PR against `REFforXeFG`.

Suggested title:

```text
XeFG 2R2: move presentation state ownership into session
```

Suggested summary:

```text
- make XeFGPresentationSession own binding, resize, detached, monitor, and diagnostic state
- replace D3D12Hook direct XeFG state fields with one session member
- keep existing D3D12Hook policy and physical hook behavior unchanged
- preserve current COM ownership and bind/rebind ordering
```

Explicit scope statement:

```text
2R2 state-ownership move only.
No runtime policy extraction, no physical hook ownership move, no Present/Resize redesign, and no candidate-handoff change.
```

Include exact build/static validation performed.

If runtime smoke is not performed before opening the PR, state that clearly rather than implying runtime validation.

---

# 21. Stop Conditions

Stop and report instead of expanding the PR if any of the following appears necessary:

- moving `m_swapchain_hook` or `m_present_hook` into the session;
- rewriting bind/rebind ordering;
- changing candidate handoff;
- changing runtime transition locks;
- changing XeFG result semantics;
- changing monitor thresholds;
- changing Present callback order;
- changing resize policy;
- moving the MHW-specific policy;
- modifying Streamline/DLSSG behavior;
- modifying D3D11 behavior;
- introducing a generic FG abstraction;
- adding a persistent `ComPtr<IDXGISwapChain3>` for the active binding;
- adding a worker thread/timer/polling mechanism.

Those belong to later steps or indicate the boundary should be reconsidered.

---

# 22. Review Checklist

A reviewer should be able to answer **yes** to all of the following:

- Is `XeFGPresentationSession` now the single owner of the six targeted XeFG state items?
- Were the old direct `D3D12Hook` copies removed rather than duplicated?
- Is `XeFGBinding` still storing the active swapchain as a borrowed pointer?
- Are queue/device ownership semantics unchanged?
- Are bounded local swapchain keepalives unchanged?
- Is physical `VtableHook` ownership still in `D3D12Hook`?
- Is candidate handoff unchanged?
- Is bind/rebind ordering unchanged?
- Are runtime detach/Destroy semantics unchanged?
- Are monitor thresholds unchanged?
- Is Present/Present1 ordering unchanged?
- Is resize behavior unchanged?
- Is the MHW-only policy unchanged and still in its current location for now?
- Is native D3D12 behavior unchanged when XeFG is inactive?
- Did the Release x64 build pass?
- Did the direct-access audit pass with zero violations?
- Did `git diff --check` pass?

If these conditions hold, the PR has achieved the intended small 2R2 step.

---

## Final Rule

> 2R2 changes where XeFG presentation state lives, not what that state means and not how D3D12Hook acts on it.

After this PR, `XeFGPresentationSession` should be the real semantic state container, while `D3D12Hook` remains the unchanged policy/mechanism executor. Policy extraction and the narrow physical consistency view come next.