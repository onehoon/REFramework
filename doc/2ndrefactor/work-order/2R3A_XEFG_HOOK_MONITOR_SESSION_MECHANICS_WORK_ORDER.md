# Work Order — 2R3A: Move XeFG Hook-Monitor State Mechanics Behind `XeFGPresentationSession`

Date: 2026-09-09  
Repository: `onehoon/REFramework`  
PR base branch: `REFforXeFG`  
Planning baseline reviewed: `REFforXeFG` @ `41f1e2082d5e20f12721cf55e2de5794f661e6eb`  
Architecture authority: `doc/2ndrefactor/REFramework_OPTISCALER_XEFG_SECOND_STAGE_REFACTOR_DESIGN_2026-09-09.md`  
Previous steps: PR #38 / 2R1 merged, PR #39 / 2R2 merged

---

## 1. Objective

Implement the next deliberately small step of the second-stage OptiScaler + Intel XeFG refactor.

This PR is **2R3A**, a split of the broader conceptual 2R3 from the architecture document.

The purpose is:

> Move XeFG hook-monitor state mechanics and active-binding consistency interpretation behind `XeFGPresentationSession`, while preserving the current high-level recovery classification in `XeFGCompatibility` and preserving all runtime detach / Destroy behavior in `D3D12Hook` for a later PR.

This PR is intentionally narrower than the full conceptual 2R3 because the integration branch is unreleased and the preferred review strategy is small, independently reversible PRs.

This is still a behavior-preserving refactor.

---

## 2. Why 2R3 Is Split

After 2R2, `XeFGPresentationSession` already owns:

```text
XeFGBinding
XeFGResizeLifecycle
XeFGDetachedState
XeFGHookMonitorState
last monitor action
P2.1 render-boundary diagnostic state
```

However, the monitor-related interpretation still lives mainly in `D3D12Hook`:

```cpp
has_consistent_active_xefg_binding()
has_xefg_monitor_state()
has_xefg_detached_state()
get_xefg_monitor_binding_key()
note_xefg_monitor_timeout()
get_xefg_timeout_count()
note_xefg_monitor_action()
clear_xefg_monitor_state()
```

and `XeFGCompatibility::evaluate_hook_monitor_timeout()` still consumes those helpers to classify:

```text
AllowGenericRecovery
PreserveGrace
SuppressRuntimeTransition
SuppressDetachedUncertain
QuarantineSustainedTimeout
QuarantineInconsistentState
```

The original 2R3 also includes runtime-detach and Destroy reconciliation. Doing monitor encapsulation and runtime detach in one PR would touch two independent failure domains:

```text
A. watchdog / recovery classification
B. Intel runtime transition / object destruction lifecycle
```

Keep them separate.

For 2R3A:

```text
move monitor/session interpretation
keep high-level action classification where it is
keep detach/Destroy mechanics where they are
```

A later 2R3B should handle runtime transition / detached lifecycle policy.

---

## 3. Branch / PR Rules

Create a fresh implementation branch from the **current tip of `REFforXeFG` after this work-order commit is present**.

Suggested branch name:

```text
refactor/xefg-2r3a-monitor-policy
```

Open the PR against:

```text
base: REFforXeFG
```

Do not target `master`.

Do not rebase onto upstream `praydog/REFramework` as part of this PR.

---

## 4. Current Code Boundary Reviewed

Current `XeFGPresentationSession` owns the state but still exposes raw mutable state accessors:

```cpp
XeFGBinding& binding() noexcept;
XeFGResizeLifecycle& resize_lifecycle() noexcept;
XeFGDetachedState& detached_state() noexcept;
XeFGHookMonitorState& monitor_state() noexcept;
const char* last_monitor_action() const noexcept;
void set_last_monitor_action(const char* action) noexcept;
bool render_boundary_logged() const noexcept;
void set_render_boundary_logged(bool value) noexcept;
```

Current `D3D12Hook` then reconstructs monitor semantics from these state objects plus physical D3D12 state.

For example, current active-binding consistency is equivalent to:

```cpp
return m_hooked
    && !m_is_phase_1
    && m_swapchain_source == SwapchainSource::XeFGInternal
    && m_xefg_session.binding().active()
    && m_swapchain_hook != nullptr
    && m_xefg_session.binding().aliases_match(m_swap_chain, m_command_queue, m_device)
    && m_swapchain_hook->get_instance().ptr() == m_xefg_session.binding().swapchain();
```

Current monitor-state presence is equivalent to:

```cpp
return m_xefg_session.detached_state().active
    || m_xefg_session.binding().active()
    || m_swapchain_source == SwapchainSource::XeFGInternal;
```

Current timeout state still lives in the session-owned `XeFGHookMonitorState`, but `D3D12Hook` directly drives it:

```cpp
return m_xefg_session.monitor_state().note_timeout(
    get_xefg_monitor_binding_key(),
    m_present_entry_count.load(std::memory_order_relaxed),
    get_last_present_age_ms());
```

2R3A should make the session itself responsible for these monitor-specific interpretations.

---

## 5. Non-Negotiable Scope

This PR is XeFG-specific and hook-monitor-specific.

Do not intentionally modify:

- D3D11;
- DLSSG / Streamline;
- FSRFG;
- native D3D12 discovery;
- native Present / Present1 behavior;
- ResizeBuffers / ResizeTarget / ResizeBuffers1 behavior;
- renderer callback order;
- candidate handoff;
- initial XeFG bind transaction;
- XeFG rebind transaction;
- COM ownership;
- Intel XeFG runtime hooks;
- loader behavior;
- anti-tamper/integrity behavior;
- MHW-specific resize-hold policy;
- `REFramework.cpp` hook-monitor timing;
- runtime transition depth semantics.

Do not introduce a generic frame-generation abstraction.

Do not move `m_swapchain_hook` or `m_present_hook` ownership.

Do not move runtime detach / Destroy handling in this PR.

---

# 6. Add a Narrow Physical Binding View

The session needs enough information to validate its semantic binding against the physical `D3D12Hook` state without gaining direct access to the entire hook object.

Add a small XeFG-specific view to `XeFGPresentationSession`.

Recommended shape:

```cpp
class XeFGPresentationSession {
public:
    struct PhysicalBindingView {
        bool hook_active{};
        bool phase1{};
        bool xefg_source{};
        bool swapchain_hook_present{};
        IDXGISwapChain3* renderer_swapchain{};
        ID3D12CommandQueue* renderer_queue{};
        ID3D12Device4* renderer_device{};
        void* hook_target{};
    };

    ...
};
```

Exact member names may differ, but preserve every current consistency input.

### Why both `swapchain_hook_present` and `hook_target`

Current logic separately requires:

```cpp
m_swapchain_hook != nullptr
```

and then compares:

```cpp
m_swapchain_hook->get_instance().ptr() == binding.swapchain()
```

Do not collapse these into one assumption unless semantic equivalence is proved.

The safest representation is:

```text
physical hook object exists
AND
physical hook target matches the semantic swapchain
```

This avoids accidentally treating a null hook target as equivalent to a missing hook object.

### Do not include generic implementation objects

The view must not expose:

```text
VtableHook*
PointerHook*
REFramework*
callbacks
mutexes
Streamline state
```

Only plain state required for XeFG consistency interpretation belongs in the view.

---

# 7. Add Session-Level Monitor APIs

Move monitor-specific interpretation behind `XeFGPresentationSession`.

Recommended API shape:

```cpp
class XeFGPresentationSession {
public:
    bool has_monitor_state(bool xefg_source) const noexcept;
    bool detached_uncertain() const noexcept;

    bool consistent_with(const PhysicalBindingView& physical) const noexcept;

    XeFGMonitorBindingKey monitor_binding_key(void* hook_target) const noexcept;

    XeFGHookMonitorState::TimeoutClass note_monitor_timeout(
        void* hook_target,
        uint64_t present_entry_count,
        int64_t present_age_ms) noexcept;

    uint32_t monitor_timeout_count() const noexcept;

    bool note_monitor_action(const char* action) noexcept;
    void clear_monitor_state() noexcept;

    ...
};
```

The exact API can be adjusted if a smaller equivalent interface is cleaner.

The required responsibility boundary is more important than exact names.

---

## 8. Required Semantics for `consistent_with()`

The new session method must preserve the current predicate exactly.

Conceptually:

```cpp
bool XeFGPresentationSession::consistent_with(
    const PhysicalBindingView& physical) const noexcept {
    return physical.hook_active
        && !physical.phase1
        && physical.xefg_source
        && m_binding.active()
        && physical.swapchain_hook_present
        && m_binding.aliases_match(
            physical.renderer_swapchain,
            physical.renderer_queue,
            physical.renderer_device)
        && physical.hook_target == m_binding.swapchain();
}
```

Do not weaken this check.

Do not make a binding count as consistent merely because:

```text
semantic binding is active
or
swapchain pointer matches
or
hook target matches
```

All existing conditions remain required.

This predicate is part of the fail-closed protection against stale or partially detached XeFG state.

---

# 9. Required Semantics for Monitor-State Presence

Current behavior must remain:

```text
monitor state exists if ANY is true:

1. detached uncertainty is active
2. semantic XeFG binding is active
3. D3D12Hook source is XeFGInternal
```

A session method may therefore take the external source state as a bool:

```cpp
bool XeFGPresentationSession::has_monitor_state(bool xefg_source) const noexcept {
    return m_detached_state.active
        || m_binding.active()
        || xefg_source;
}
```

Do not make the session inspect or own `D3D12Hook::SwapchainSource` directly.

The session should remain independent of the generic D3D12 enum.

---

# 10. Required Monitor Key Semantics

Current key fields are contractual for this refactor:

```cpp
struct XeFGMonitorBindingKey {
    uint64_t generation{};
    size_t runtime_slot{};
    void* runtime_context{};
    IDXGISwapChain3* swapchain{};
    void* hook_target{};
};
```

The session should build the key from:

```text
binding generation
binding runtime slot
binding runtime context
binding swapchain
physical hook target supplied by D3D12Hook
```

Do not add/remove fields in this PR.

Do not switch to HWND matching for monitor identity.

Do not change equality semantics.

---

# 11. Timeout Semantics Must Remain Bit-for-Bit Equivalent

`XeFGHookMonitorState::note_timeout()` already has the correct behavior and should not be rewritten unnecessarily.

Current semantics:

```text
if state is uninitialized
OR monitor key changed
OR Present entry count advanced
    -> replace key
    -> update last Present count
    -> consecutive_timeouts = 1
    -> initialized = true
    -> Grace

otherwise
    -> increment consecutive_timeouts

Sustained only when BOTH:
    consecutive_timeouts >= 3
    present_age_ms >= 20000
```

Do not change:

```cpp
kSustainedTimeoutThreshold = 3
kMinimumSustainedPresentAgeMs = 20000
```

Do not alter the first timeout count from `1` to `0`.

Do not change `>=` comparisons.

Do not introduce wall-clock timing or sleeps.

Do not move the generic watchdog cadence.

---

# 12. Preserve Monitor Action Dedup Semantics Exactly

Current action dedup is pointer comparison on `const char*`:

```cpp
if (m_xefg_session.last_monitor_action() == action) {
    return false;
}
```

This PR must preserve that behavior.

A suitable session implementation is:

```cpp
bool XeFGPresentationSession::note_monitor_action(const char* action) noexcept {
    if (m_last_monitor_action == action) {
        return false;
    }

    m_last_monitor_action = action;
    return true;
}
```

Do **not** silently change this to:

```cpp
strcmp(...)
std::string
std::string_view value comparison
```

Changing the comparison model would be a behavior change unrelated to this refactor.

Likewise, `clear_monitor_state()` must preserve current reset semantics:

```cpp
m_monitor_state.clear();
m_last_monitor_action = nullptr;
```

It must **not** clear:

```text
detached state
binding
resize state
render-boundary state
```

---

# 13. D3D12Hook Should Build the Physical View

Keep the actual physical D3D12 state in `D3D12Hook`.

Add one narrow helper if useful:

```cpp
XeFGPresentationSession::PhysicalBindingView
D3D12Hook::get_xefg_physical_binding_view() const noexcept {
    return {
        .hook_active = m_hooked,
        .phase1 = m_is_phase_1,
        .xefg_source = m_swapchain_source == SwapchainSource::XeFGInternal,
        .swapchain_hook_present = m_swapchain_hook != nullptr,
        .renderer_swapchain = m_swap_chain,
        .renderer_queue = m_command_queue,
        .renderer_device = m_device,
        .hook_target = m_swapchain_hook != nullptr
            ? m_swapchain_hook->get_instance().ptr()
            : nullptr,
    };
}
```

Exact aggregate syntax may follow repository/compiler style.

The helper should be a pure snapshot builder.

It must not:

```text
AddRef objects
QueryInterface objects
call Intel APIs
install/remove hooks
change aliases
reset renderer state
```

---

# 14. Keep D3D12Hook Wrapper Surface Temporarily

To keep this PR small and avoid changing `XeFGCompatibility` in the same diff, preserve the existing `D3D12Hook` monitor wrapper methods for now.

They should become thin delegations.

Examples:

### Before

```cpp
bool D3D12Hook::has_consistent_active_xefg_binding() const noexcept {
    return m_hooked
        && !m_is_phase_1
        && m_swapchain_source == SwapchainSource::XeFGInternal
        && m_xefg_session.binding().active()
        && m_swapchain_hook != nullptr
        && m_xefg_session.binding().aliases_match(m_swap_chain, m_command_queue, m_device)
        && m_swapchain_hook->get_instance().ptr() == m_xefg_session.binding().swapchain();
}
```

### Target

```cpp
bool D3D12Hook::has_consistent_active_xefg_binding() const noexcept {
    return m_xefg_session.consistent_with(get_xefg_physical_binding_view());
}
```

### Monitor state

```cpp
bool D3D12Hook::has_xefg_monitor_state() const noexcept {
    return m_xefg_session.has_monitor_state(is_xefg_source());
}
```

### Detached read

```cpp
bool D3D12Hook::has_xefg_detached_state() const noexcept {
    return m_xefg_session.detached_uncertain();
}
```

### Monitor key

```cpp
XeFGMonitorBindingKey D3D12Hook::get_xefg_monitor_binding_key() const noexcept {
    return m_xefg_session.monitor_binding_key(
        m_swapchain_hook != nullptr ? m_swapchain_hook->get_instance().ptr() : nullptr);
}
```

### Timeout

```cpp
XeFGHookMonitorState::TimeoutClass
D3D12Hook::note_xefg_monitor_timeout() noexcept {
    return m_xefg_session.note_monitor_timeout(
        m_swapchain_hook != nullptr ? m_swapchain_hook->get_instance().ptr() : nullptr,
        m_present_entry_count.load(std::memory_order_relaxed),
        get_last_present_age_ms());
}
```

### Count

```cpp
uint32_t D3D12Hook::get_xefg_timeout_count() const noexcept {
    return m_xefg_session.monitor_timeout_count();
}
```

### Action dedup / clear

```cpp
bool D3D12Hook::note_xefg_monitor_action(const char* action) noexcept {
    return m_xefg_session.note_monitor_action(action);
}

void D3D12Hook::clear_xefg_monitor_state() noexcept {
    m_xefg_session.clear_monitor_state();
}
```

These wrappers are transitional and may be collapsed in a later PR.

Do not force `XeFGCompatibility` to consume the session directly in 2R3A.

---

# 15. `XeFGCompatibility::evaluate_hook_monitor_timeout()` Must Remain Logically Unchanged

Do not move the high-level action classification in this PR.

Current sequence must remain equivalent to:

```text
if runtime transition active
    -> SuppressRuntimeTransition
else if no XeFG monitor state
    -> AllowGenericRecovery
else if detached uncertain
    -> SuppressDetachedUncertain
else if inconsistent active binding
    -> QuarantineInconsistentState
else if timeout classification is Sustained
    -> QuarantineSustainedTimeout
else
    -> PreserveGrace
```

Do not reorder these conditions.

This ordering matters.

For example:

```text
runtime transition suppression must win before normal state evaluation
and
detached uncertainty must win before consistency/timeout classification
```

Keep the current reason strings and log action names unchanged.

Ideally `XeFGCompatibility.cpp` has **zero behavioral diff** in 2R3A.

If an include or signature-only adjustment is required, keep it minimal.

---

# 16. Runtime Detach / Destroy Is Explicitly Deferred

Do not move or redesign these in 2R3A:

```cpp
D3D12Hook::detach_xefg_binding_for_runtime_transition(...)
D3D12Hook::note_xefg_destroy_result(...)
```

Do not move:

- exact runtime matching;
- same-HWND fallback matching;
- detached-state assignment;
- Destroy success reconciliation;
- raw alias clearing;
- renderer reset;
- physical hook reset;
- borrowed swapchain keepalive;
- result semantics.

Those will be handled in the next substep, 2R3B.

It is acceptable that those methods continue to use:

```cpp
m_xefg_session.detached_state()
m_xefg_session.binding()
```

directly for one more PR.

Do not broaden 2R3A just to eliminate every raw accessor.

---

# 17. Do Not Touch Present / Resize Policy

2R3A is not 2R4 or 2R5.

Do not move:

```text
observe-only suppression
resize-hold suppression
suppressed Present counting
post-resize Present sampling
first-render-boundary logging
MHW ResizeTarget hold rule
```

`present_common()` and resize callbacks should be unchanged except for incidental compilation fixes if absolutely required.

If implementation starts editing Present/Resize decision flow, stop and reduce scope.

---

# 18. COM / Hook Ownership Invariants

This PR must not alter any COM ownership or physical hook ownership.

Required state remains:

```text
active XeFG swapchain
    -> borrowed raw pointer in XeFGBinding

active queue/device
    -> strong ComPtr in XeFGBinding

candidate swapchain
    -> strong ComPtr while pending/applying

old active swapchain during destructive hook removal
    -> bounded local ComPtr keepalive only

physical swapchain hook
    -> owned by D3D12Hook
```

Do not add:

```text
persistent ComPtr<IDXGISwapChain3> in XeFGPresentationSession
manual AddRef / Release
Release-until-count loops
refcount drain logic
COM probing from monitor timeout logic
```

Monitor evaluation must remain pointer/state inspection only.

---

# 19. Inactive XeFG Path Must Stay Passive

When XeFG is inactive:

```text
session default state is empty
no module probing occurs from session methods
no COM call occurs
no hook installation occurs
no renderer callback occurs
no state mutation occurs merely because D3D12Hook exists
```

`consistent_with()` and monitor-state getters must be pure state checks.

Do not make `XeFGPresentationSession` constructor perform work.

---

# 20. Expected Files

Expected primary touched files:

```text
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp
src/D3D12Hook.hpp
src/D3D12Hook.cpp
```

Prefer no changes to:

```text
src/compatibility/xefg/XeFGCompatibility.cpp
src/compatibility/xefg/XeFGCompatibility.hpp
src/REFramework.cpp
src/compatibility/xefg/XeFGCandidateHandoff.cpp
src/compatibility/xefg/XeFGDiscovery.cpp
src/compatibility/xefg/XeFGBinding.cpp
src/compatibility/xefg/XeFGResizeLifecycle.cpp
```

No new source file is required, so generated build metadata should normally remain unchanged.

If `CMakeLists.txt` changes without a source-registration reason, investigate before committing it.

---

# 21. Raw Accessor Cleanup Allowed in This PR

Because monitor mechanics become encapsulated, it is appropriate to remove session accessors that are no longer needed for monitor state.

Candidates to remove after all callers are migrated:

```cpp
XeFGHookMonitorState& monitor_state() noexcept;
const XeFGHookMonitorState& monitor_state() const noexcept;
const char* last_monitor_action() const noexcept;
void set_last_monitor_action(const char* action) noexcept;
```

Do not remove general accessors still required by later transitional code, such as:

```cpp
binding()
resize_lifecycle()
detached_state()
```

unless static search proves they are no longer used and removing them does not broaden the PR.

The goal is gradual encapsulation, not an all-at-once interface cleanup.

---

# 22. Static Equivalence Audit

Before opening the PR, compare the old and new logic manually.

Required checks:

### Consistency predicate

Every current boolean condition must still exist exactly once:

```text
m_hooked
!m_is_phase_1
XeFGInternal source
binding active
swapchain hook object present
binding aliases match renderer aliases
physical hook target matches binding swapchain
```

### Monitor presence

Must still be:

```text
detached || binding active || XeFGInternal source
```

### Monitor key

Must still contain exactly:

```text
generation
runtime slot
runtime context
binding swapchain
physical hook target
```

### Timeout

Must still use:

```text
present entry count
last Present age
threshold 3
minimum age 20000 ms
```

### Action dedup

Must still use `const char*` pointer identity.

### Clear

Must clear only monitor timeout state + last action.

---

# 23. Validation

A full game/runtime smoke is intentionally deferred because the user is currently away from the test machine and wants to continue code work.

Do not claim runtime validation.

Run at minimum:

```text
cmake -S . -B <build-dir> -G "Visual Studio 17 2022" -A x64
cmake --build <build-dir> --config Release --target REFramework --parallel 4
python dev/audit_direct_access_clang.py
git diff --check
```

If the repository uses `PYTHONUTF8=1` for the audit in the current environment, preserve that invocation.

Required results:

```text
Release x64 build: PASS
direct-access audit: 0 violations
git diff --check: PASS
```

Also perform a source-level diff audit proving:

```text
XeFGCompatibility monitor action order unchanged
runtime detach / Destroy code unchanged
Present / Resize code unchanged
bind / rebind code unchanged
COM ownership unchanged
```

---

# 24. Deferred Runtime Gate

Because runtime testing is currently unavailable, do not block this small structural PR solely on DD2/MHW testing if static/build review is clean.

However, keep a cumulative runtime gate before the second-stage refactor is considered validated.

At the next available test window, the accumulated 2R2 + 2R3A/2R3B state-policy changes should receive at least:

```text
A. REF only / native D3D12 smoke
B. REF + OptiScaler with XeFG not selected
C. DD2 + OptiScaler + Intel XeFG
D. MHW + OptiScaler + Intel XeFG lifecycle / Alt+Tab / resize smoke
```

Do not treat lack of immediate runtime access as evidence that behavior changed or is correct; it is simply deferred evidence.

---

# 25. Suggested PR Description

Title:

```text
XeFG 2R3A: move hook-monitor mechanics into presentation session
```

Summary:

```text
- add a narrow XeFG physical binding view for session consistency checks
- move monitor-state presence, binding consistency, monitor-key construction, timeout sampling, and action dedup behind XeFGPresentationSession
- keep D3D12Hook monitor helpers as thin delegates for now
- leave XeFGCompatibility recovery classification and runtime detach/Destroy behavior unchanged
```

Scope statement:

```text
2R3A monitor-mechanics extraction only.
No recovery-policy reorder, no runtime-detach move, no Present/Resize changes, no bind/rebind changes, and no COM or physical-hook ownership changes.
```

Validation section must list exact commands/results.

Runtime evidence section must state clearly if no game test was performed.

---

# 26. Stop Conditions

Stop and report instead of broadening the PR if any of the following appears necessary:

- changing `XeFGCompatibility::evaluate_hook_monitor_timeout()` action ordering;
- changing watchdog timing;
- moving runtime detach / Destroy handling;
- changing `REFramework.cpp` monitor cadence;
- changing Present or resize control flow;
- changing bind/rebind order;
- changing COM ownership;
- moving physical hook ownership;
- querying COM objects from timeout logic;
- adding a worker thread, timer, sleep, polling loop, or delayed recovery;
- introducing a generic frame-generation abstraction;
- touching Streamline/DLSSG or FSRFG behavior.

If one of those is genuinely required, leave the PR incomplete and report why.

---

# 27. Review Priorities

Review this PR in this order:

1. **Predicate equivalence** — no consistency condition lost.
2. **Timeout equivalence** — no threshold or counter semantic changed.
3. **Recovery ordering protection** — `XeFGCompatibility` decision order untouched.
4. **State isolation** — monitor internals now owned/interpreted by the session.
5. **Native path isolation** — session is passive when XeFG is inactive.
6. **No lifecycle spillover** — detach/Destroy and Present/Resize remain for later PRs.

Do not block the PR for purely stylistic alternatives if the implementation preserves these invariants and keeps the diff small.

---

# 28. Definition of Done for 2R3A

2R3A is complete when all are true:

1. `XeFGPresentationSession` owns and interprets hook-monitor state mechanics.
2. Active XeFG consistency is evaluated by the session against a narrow physical view.
3. Monitor-state presence is evaluated by the session.
4. Monitor binding-key construction is behind the session.
5. Timeout sampling/counter access is behind the session.
6. Monitor action dedup/reset is behind the session.
7. `D3D12Hook` retains only thin monitor delegation wrappers.
8. `XeFGCompatibility::evaluate_hook_monitor_timeout()` preserves current classification order and semantics.
9. Runtime detach and Destroy reconciliation remain unchanged.
10. Present/Resize and bind/rebind behavior remain unchanged.
11. Active swapchain remains borrowed.
12. Queue/device ownership remains strong in `XeFGBinding`.
13. Physical hook ownership remains in `D3D12Hook`.
14. Release x64 build passes.
15. direct-access audit reports zero violations.
16. `git diff --check` passes.
17. No runtime validation is claimed unless actually performed.

The next planned substep after this PR should be **2R3B: runtime-transition / detached-lifecycle policy extraction**, still keeping renderer reset and physical hook destruction in `D3D12Hook`.
