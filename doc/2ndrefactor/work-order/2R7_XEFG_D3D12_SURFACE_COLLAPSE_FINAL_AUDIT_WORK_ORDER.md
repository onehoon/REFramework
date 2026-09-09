# Work Order — 2R7: Collapse Transitional XeFG Surface in `D3D12Hook` and Prepare the Final Audit

Date: 2026-09-09  
Repository: `onehoon/REFramework`  
PR base branch: `REFforXeFG`  
Planning baseline reviewed: `REFforXeFG` @ `3b879a6d19b8375a1496b9d76422881be4b164e0`  
Architecture authority: `doc/2ndrefactor/REFramework_OPTISCALER_XEFG_SECOND_STAGE_REFACTOR_DESIGN_2026-09-09.md`  
Previous stage: conceptual 2R6 merged through PR #47

---

## 1. Objective

Implement conceptual **2R7 — Collapse Temporary XeFG Surface in `D3D12Hook` and Finalize the Intended Ownership Boundary**.

This is the **last implementation PR** in the second-stage refactor series.

The goal is not to add another XeFG behavior layer. The goal is to remove transitional APIs that only existed while 2R1-2R6 were moving ownership into `XeFGPresentationSession`, tighten the final `D3D12Hook <-> XeFGPresentationSession` boundary, and leave the codebase in a state suitable for a separate whole-tree final review.

The desired end-state is:

```text
D3D12Hook
    = generic/native D3D12 physical hook mechanism
    + small explicit XeFG physical bridge

XeFGPresentationSession
    = XeFG semantic presentation/lifecycle authority

XeFGCompatibility
    = high-level runtime/discovery/recovery façade
```

Do **not** turn 2R7 into another refactor project.

---

## 2. Current Baseline Reviewed

Baseline:

```text
REFforXeFG @ 3b879a6d19b8375a1496b9d76422881be4b164e0
```

2R6 has already consolidated:

```text
live candidate
pending candidate
explicit XeFG external bind
    -> one XeFG candidate transaction
```

and keeps physical ownership in `D3D12Hook`.

The current code is functionally in good shape, but `D3D12Hook.hpp` still exposes several transitional wrappers from earlier second-stage PRs.

Current examples include:

```cpp
bool has_active_xefg_instance_binding() const noexcept;
bool has_consistent_active_xefg_binding() const noexcept;
bool has_xefg_monitor_state() const noexcept;
bool has_xefg_detached_state() const noexcept;
XeFGMonitorBindingKey get_xefg_monitor_binding_key() const noexcept;
XeFGHookMonitorState::TimeoutClass note_xefg_monitor_timeout() noexcept;
uint32_t get_xefg_timeout_count() const noexcept;
bool note_xefg_monitor_action(const char* action) noexcept;
void clear_xefg_monitor_state() noexcept;
```

After 2R3C, the real monitor policy already runs through:

```cpp
XeFGPresentationSession::MonitorEvaluation
evaluate_xefg_monitor_timeout(bool runtime_transition_active) noexcept;
```

so the lower-level monitor wrappers above are transitional surface unless a current call site proves otherwise.

---

## 3. Primary 2R7 Principle

Use this rule for every deletion:

> Remove only a XeFG wrapper/helper whose semantic responsibility has already moved elsewhere and whose current call sites are either zero or can be replaced by an already-existing narrower session/compatibility bridge without changing ordering.

Do not delete a helper merely because its name looks redundant.

Before removing any method, perform a repository-wide call-site search on the current branch.

---

## 4. Mandatory Cleanup Candidate A — Old Monitor Wrapper Layer

The following `D3D12Hook` methods are expected to be removable after call-site verification:

```cpp
has_active_xefg_instance_binding()
has_consistent_active_xefg_binding()
has_xefg_monitor_state()
has_xefg_detached_state()
get_xefg_monitor_binding_key()
note_xefg_monitor_timeout()
get_xefg_timeout_count()
note_xefg_monitor_action()
```

`clear_xefg_monitor_state()` is also transitional if its remaining use is only an internal `D3D12Hook` cleanup path.

If so, replace that wrapper with the direct session operation at the exact same point:

```cpp
// old
clear_xefg_monitor_state();

// target, if this is the only remaining use
m_xefg_session.clear_monitor_state();
```

Do not change when the monitor state is cleared.

### Why these wrappers should disappear

2R3C already established one authoritative policy path:

```text
D3D12 physical view + present progress
    -> XeFGPresentationSession::evaluate_monitor_timeout()
    -> MonitorEvaluation
    -> XeFGCompatibility maps/logs high-level action
```

Keeping the old individual sampling/key/action wrappers makes it look as if two monitor APIs still exist.

---

## 5. Mandatory Cleanup Candidate B — Remove Broad `XeFGCompatibility` Friend Access

Current header still has:

```cpp
friend class XeFGCompatibility;
```

At the reviewed baseline, `XeFGCompatibility` primarily needs one narrow monitor bridge:

```cpp
hook.evaluate_xefg_monitor_timeout(...)
```

Do not retain broad friend access merely for that one operation.

Preferred final shape:

```cpp
class D3D12Hook {
public:
    XeFGPresentationSession::MonitorEvaluation
    evaluate_xefg_monitor_timeout(bool runtime_transition_active) noexcept;

    ...
};
```

Then remove:

```cpp
friend class XeFGCompatibility;
```

The exact placement/name may differ, but the access boundary should be explicit and narrow.

Do **not** expose raw physical members publicly as a replacement for the friend.

Forbidden replacement:

```cpp
IDXGISwapChain3* get_xefg_hook_target();
ID3D12CommandQueue* get_internal_queue_for_xefg_monitor();
...
```

The whole point of 2R3C was to avoid that surface.

---

## 6. Session Accessor Tightening

`XeFGPresentationSession` still exposes several direct state accessors introduced during the staged migration:

```cpp
XeFGBinding& binding() noexcept;
const XeFGBinding& binding() const noexcept;

XeFGResizeLifecycle& resize_lifecycle() noexcept;
const XeFGResizeLifecycle& resize_lifecycle() const noexcept;

XeFGDetachedState& detached_state() noexcept;
const XeFGDetachedState& detached_state() const noexcept;

bool render_boundary_logged() const noexcept;
void set_render_boundary_logged(bool value) noexcept;
```

2R7 should audit these carefully.

### 6.1 Remove zero-use accessors

If repository-wide search confirms no current consumer:

```text
resize_lifecycle() mutable/const
render_boundary_logged() getter
```

remove them.

Do not keep unused public session internals “just in case”.

### 6.2 Do not force risky encapsulation rewrites

The non-const `binding()` / `detached_state()` accessors are less clean, but do not replace them with a large new lifecycle abstraction solely for aesthetics.

Use this rule:

```text
trivial exact-order replacement available
    -> tighten accessor

requires moving physical hook/reset ordering
    -> leave for final audit, do not broaden 2R7
```

A small explicit session method is acceptable if it preserves current ordering exactly.

Example for the unhook cleanup state only:

```cpp
void XeFGPresentationSession::clear_transient_state_for_unhook() noexcept {
    m_detached_state = {};
    clear_monitor_state();
}
```

Then:

```cpp
clear_xefg_resize_transition_hold("unhook");
m_xefg_session.clear_transient_state_for_unhook();
```

This is acceptable only if it preserves the exact current timing.

Do **not** move `m_binding.clear()` before physical hook removal.

---

## 7. Preserve the Physical/Semantic Destruction Order

Current `D3D12Hook::unhook()` ordering is important.

The essential sequence must remain:

```text
lifecycle mutex held
-> invalidate global handoff target
-> clear resize hold
-> clear detached/monitor transient state
-> determine whether hook/binding work remains
-> capture bounded old swapchain keepalive if active
-> reset physical hooks
-> clear renderer-facing aliases if they point at XeFG binding
-> clear semantic binding
-> mark hook inactive / phase1
```

Do not collapse this into a one-line session reset that clears the borrowed binding before physical hook removal.

Forbidden:

```cpp
m_xefg_session.reset_all(); // if this clears binding before VtableHook reset
m_swapchain_hook.reset();
```

The active XeFG swapchain remains borrowed; the bounded local keepalive during physical hook removal is still required.

---

## 8. Keep `sync_xefg_binding_aliases()` Unless a Truly Equivalent Narrow Replacement Exists

Current alias synchronization is physical bridge code:

```cpp
void D3D12Hook::sync_xefg_binding_aliases() noexcept {
    m_swap_chain = m_xefg_session.binding().swapchain();
    m_command_queue = m_xefg_session.binding().queue();
    m_device = m_xefg_session.binding().device();
}
```

This is not automatically dead merely because 2R6 centralized semantic commit.

The renderer-facing raw aliases still belong to `D3D12Hook`.

Two acceptable end-states are:

```text
A. keep one sync_xefg_binding_aliases() physical bridge
```

or

```text
B. replace it with one snapshot-based physical publication helper
```

Example B:

```cpp
void D3D12Hook::publish_xefg_binding_aliases(
    const XeFGBinding::RuntimeLifecycleSnapshot& binding) noexcept {
    m_swap_chain = binding.swapchain;
    m_command_queue = binding.queue;
    m_device = binding.device;
}
```

Do not duplicate alias assignments across initial/same-object/replacement branches.

One physical publication point is preferable.

---

## 9. Keep the 2R6 Transaction Mechanism Intact

2R7 must not redesign the 2R6 transaction.

Keep:

```cpp
XeFGHookPreparation prepare_xefg_instance_hook(...);
bool apply_xefg_binding_request(...);
bool apply_xefg_candidate(...);
```

unless a purely naming-level cleanup is clearly warranted.

The following semantics are frozen:

```text
Identical
    -> no GetDevice
    -> no reset
    -> no hook replacement
    -> no generation increment

SameSwapchainUpdate
    -> GetDevice
    -> renderer reset
    -> existing VtableHook retained
    -> generation +1

ChangedSwapchainReplacement
    -> GetDevice
    -> prepare all 5 hooks
    -> bounded old keepalive
    -> renderer reset
    -> old hooks removed
    -> semantic commit
    -> aliases published
    -> prepared hook published

NoActiveBinding
    -> GetDevice
    -> prepare all 5 hooks before destructive work
    -> commit_initial generation 1
```

Mandatory hook slots remain:

```text
Present[8]
Present1[22]
ResizeBuffers[13]
ResizeTarget[14]
ResizeBuffers1[39]
```

---

## 10. Keep Resize Physical Bridges

The following helpers may remain because they are physical/logging bridges, not duplicated semantic state machines:

```cpp
begin_tracked_xefg_resize_event(...)
arm_xefg_resize_transition_hold(...)
complete_xefg_resize_transition_hold(...)
clear_xefg_resize_transition_hold(...)
log_xefg_resize_event(...)
log_xefg_post_resize_present(...)
```

Do not remove them merely to reduce XeFG names in `D3D12Hook.hpp`.

Their current role is acceptable:

```text
session decides semantic state/policy
D3D12Hook emits physical diagnostic / executes callback ordering
```

### MHW rule

Do not move or alter the MHW rule again.

It is already behind:

```cpp
m_xefg_session.evaluate_and_arm_resize_target_hold(...)
```

and `D3D12Hook.cpp` no longer owns the game-specific policy.

---

## 11. Keep Present / Present1 Execution Exactly Intact

Do not change the tested 2R4 execution order.

Preserve:

```text
tracked top-level XeFG Present/Present1
-> liveness timestamp/count
-> physical alias refresh
-> nested guard
-> session present decision
-> session post-resize sample decision
-> optional diagnostics
-> optional renderer callback
-> original Present/Present1
-> device-removed diagnostic
-> note_present_activity OR post callback
```

Do not move session policy above the nested guard.

Do not merge native Present and XeFG Present into a new universal abstraction.

Do not remove the current native Present1 hook in 2R7.

---

## 12. Dead Branch Cleanup in `bind_external_swapchain()`

2R6 already introduced the early XeFG route:

```cpp
if (source == SwapchainSource::XeFGInternal) {
    return apply_xefg_binding_request(...);
}
```

Therefore code below that early return is non-XeFG external binding code.

It is acceptable to remove a remaining XeFG-only conditional below the early return only when static review proves it is unreachable.

Example principle:

```cpp
// After early XeFG return, do not keep conditionals that can only be true for XeFGInternal.
```

But do not refactor the native/non-XeFG external binding implementation beyond directly eliminating unreachable XeFG duplication.

No generic D3D12 cleanup project in 2R7.

---

## 13. Diagnostic Getter Collapse

`XeFGCompatibility::active_binding_snapshot()` currently reaches `D3D12Hook` through several small getters:

```cpp
get_xefg_lifecycle_snapshot()
get_xefg_last_resize_event_id()
get_xefg_last_resize_kind()
get_last_present_age_ms()
```

This is not a correctness problem, so cleanup is optional.

If consolidation clearly reduces the public header surface without changing any values, one narrow diagnostic snapshot is acceptable.

Conceptual example:

```cpp
struct XeFGRuntimeDiagnosticSnapshot {
    XeFGBinding::RuntimeLifecycleSnapshot binding{};
    uint64_t last_resize_event_id{};
    XeFGResizeLifecycle::EventKind last_resize_kind{};
    int64_t last_present_age_ms{-1};
};

XeFGRuntimeDiagnosticSnapshot get_xefg_runtime_diagnostic_snapshot() const noexcept;
```

However, do not make this mandatory if it creates more code than it removes.

2R7 should favor smaller risk over theoretical API perfection.

---

## 14. Files Expected

Primary expected changes:

```text
src/D3D12Hook.hpp
src/D3D12Hook.cpp
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp
src/compatibility/xefg/XeFGCompatibility.cpp
```

Possible but not expected unless a compile-only cleanup requires it:

```text
src/compatibility/xefg/XeFGCompatibility.hpp
```

Normally unchanged:

```text
src/compatibility/xefg/XeFGCandidateHandoff.cpp/.hpp
src/compatibility/xefg/XeFGBinding.cpp/.hpp
src/compatibility/xefg/XeFGResizeLifecycle.cpp/.hpp
src/compatibility/xefg/XeFGDiscovery.cpp/.hpp
src/compatibility/xefg/XeFGRuntimeRegistry.cpp/.hpp
src/REFramework.cpp
```

No CMake changes are expected because no source files should be added or removed.

---

## 15. Explicitly Forbidden Changes

Do not:

- change candidate discovery or queue selection;
- change pending-candidate mutex behavior;
- change lifecycle mutex ordering;
- change Intel Init/Destroy dispatch;
- change XeFG result semantics;
- change active swapchain ownership;
- add persistent `ComPtr<IDXGISwapChain3>` ownership;
- add COM refcount probing/draining;
- change generation semantics;
- change MHW resize-hold behavior;
- change Present/Present1 callback order;
- change ResizeTarget/ResizeBuffers/ResizeBuffers1 order;
- change monitor thresholds/timing/classification;
- change native Present1 behavior;
- alter Streamline/DLSSG/FSRFG behavior;
- refactor D3D11;
- add a generic FG provider abstraction;
- remove diagnostics that were useful in the four-game XeFG validation unless they are exact duplicates.

---

## 16. Required Static Audit Before Coding

Before modifying code, produce a call-site list for every candidate removal.

At minimum audit:

```text
has_active_xefg_instance_binding
has_consistent_active_xefg_binding
has_xefg_monitor_state
has_xefg_detached_state
get_xefg_monitor_binding_key
note_xefg_monitor_timeout
get_xefg_timeout_count
note_xefg_monitor_action
clear_xefg_monitor_state
resize_lifecycle()
render_boundary_logged()
set_render_boundary_logged()
friend class XeFGCompatibility
```

For each item classify:

```text
zero-use -> remove
single trivial forwarding use -> inline/remove wrapper
still semantic/physical boundary -> keep
```

Do not infer zero-use from header appearance alone.

---

## 17. Required Post-Change Audit

After implementation, prove the following by source review:

```text
1. XeFGCompatibility no longer has broad friend access to D3D12Hook unless a concrete unavoidable need is documented.
2. Old per-field/per-action monitor wrappers no longer form a second monitor API.
3. XeFGPresentationSession remains the only owner of binding/resize/detached/monitor semantic state.
4. D3D12Hook still owns VtableHook/PointerHook and physical renderer callbacks.
5. No semantic binding is cleared before old physical hook removal.
6. Bounded old-swapchain keepalive remains in detach/unhook/replacement paths.
7. Candidate transaction order from 2R6 is byte-for-byte equivalent in control flow except for wrapper removal.
8. Pending and live candidate paths still converge on apply_xefg_candidate().
9. MHW-only hold policy remains inside XeFGPresentationSession.
10. Present/Present1 nested guard remains before XeFG session mutation.
11. ResizeBuffers1 observe-only reset suppression remains unchanged.
12. Hook-monitor thresholds remain 3 samples / 20,000 ms sustained age semantics.
13. Negative XeFG results remain failure; non-negative remain non-failure.
14. Native Present1 remains untouched.
15. No D3D11/Streamline/FSRFG source changes.
```

---

## 18. Build / Static Validation

Run:

```text
cmake -S . -B build-2r7 -G "Visual Studio 17 2022" -A x64
cmake --build build-2r7 --config Release --target REFramework --parallel 4
python dev/audit_direct_access_clang.py
git diff --check
```

Also run targeted text/source audits proving removed wrappers have no remaining declaration/definition/call-site remnants.

Recommended examples:

```text
rg "has_active_xefg_instance_binding|has_consistent_active_xefg_binding|has_xefg_monitor_state|get_xefg_monitor_binding_key|note_xefg_monitor_timeout|get_xefg_timeout_count|note_xefg_monitor_action" src

rg "friend class XeFGCompatibility" src/D3D12Hook.hpp
```

Expected result for removed symbols: no matches.

---

## 19. Runtime Acceptance for 2R7

Project runtime scope is specifically:

```text
REFramework + OptiScaler + Intel XeFG + D3D12
```

Do **not** make these separate runtime scenarios a 2R7 merge requirement:

```text
REF-only native D3D12
REF + OptiScaler with XeFG not selected
```

They are not the product target of this fork-specific compatibility work.

Native behavior is protected here by narrow scope, code review, and static control-flow comparison.

For XeFG runtime confidence, use the existing practical game matrix when needed:

```text
RE9
DD2
MHW
Pragmata
```

2R7 is cleanup-only, so do not invent synthetic rebind behavior solely to create coverage.

The whole second-stage refactor will receive a separate final whole-tree review after 2R7 is merged.

---

## 20. Final Source Shape Target

A good final `D3D12Hook.hpp` should read conceptually like:

```cpp
class D3D12Hook {
public:
    bool hook();
    bool unhook();

    bool apply_xefg_candidate(const XeFGBindingCandidate& candidate);
    bool detach_xefg_binding_for_runtime_transition(...);
    void note_xefg_destroy_result(...);
    XeFGPresentationSession::MonitorEvaluation
        evaluate_xefg_monitor_timeout(bool runtime_transition_active) noexcept;

    // normal D3D12 public surface...

private/protected:
    XeFGPresentationSession::PhysicalBindingView
        get_xefg_physical_binding_view() const noexcept;

    XeFGHookPreparation prepare_xefg_instance_hook(...);
    bool apply_xefg_binding_request(...);

    // physical resize/present logging bridges...

    XeFGPresentationSession m_xefg_session{};
    std::unique_ptr<PointerHook> m_present_hook{};
    std::unique_ptr<VtableHook> m_swapchain_hook{};
};
```

Not literally this exact access layout, but the visible architecture should be:

```text
one session
one monitor decision bridge
one candidate transaction bridge
physical hook mechanics
```

not a second XeFG state machine in `D3D12Hook`.

---

## 21. Suggested PR

Branch:

```text
refactor/xefg-2r7-surface-collapse
```

PR title:

```text
XeFG 2R7: collapse transitional D3D12 surface
```

Base:

```text
REFforXeFG
```

---

## 22. Stop Conditions

Stop and report instead of broadening the PR if cleanup appears to require:

- moving `m_swapchain_hook` into the session;
- changing hook ownership timing;
- changing runtime detach ordering;
- introducing a new transaction abstraction;
- changing generic/native D3D12 control flow;
- changing monitor policy;
- changing resize/present behavior;
- changing candidate discovery;
- creating a general frame-generation framework;
- deleting diagnostics whose usefulness cannot be proven redundant.

If a wrapper cannot be removed without changing behavior, **keep it** and document why in the PR body.

2R7 is successful when the final boundary is clearer **without changing the tested behavior**.

---

## 23. Completion Definition

2R7 is complete when:

```text
- transitional zero-use XeFG wrappers are gone;
- broad friend access is removed or explicitly justified;
- session public surface is tightened where low-risk and provably safe;
- D3D12Hook still owns physical D3D12 mechanics only;
- XeFGPresentationSession remains semantic authority;
- 2R3-2R6 behavior and ordering are unchanged;
- Release build/static audits pass;
- PR is reviewed and merged into REFforXeFG;
```

After that merge, do **not** immediately declare the entire second-stage refactor finished.

The next step is a separate **whole-tree final review / acceptance audit** of the completed `REFforXeFG` branch.
