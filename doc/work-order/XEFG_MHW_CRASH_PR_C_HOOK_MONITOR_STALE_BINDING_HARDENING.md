# Work Order: XeFG MHW Crash PR-C — Hook-Monitor and Stale-Binding Hardening

Date: 2026-09-08  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `d4ba20ed0849dfdc4824e6900a069c635c1f8927` (`XeFG PR-B: borrow proxy swapchain and detach before runtime transitions`, PR #35 squash-merged)

This is the final planned REFramework-side PR in the focused MHW / Intel XeFG lifecycle investigation.

PR-A added exact runtime lifecycle observability.  
PR-B removed long-lived REF ownership of the XeFG proxy and added deterministic detach before matching XeFG Destroy/re-init.  
PR-C must harden the remaining hook-monitor behavior around temporary runtime transitions, uncertain detached state, and sustained Present starvation.

The primary objective is:

> Never let the generic D3D12 hook-monitor recovery path race an XeFG runtime transition or blindly tear down / rehook an uncertain borrowed-proxy relationship, while still distinguishing a harmless isolated timeout from a sustained stale-binding suspicion.

This PR is intentionally conservative. It must prefer a temporary loss of the REFramework overlay over unsafe COM access or speculative rehooking of a proxy whose lifetime is uncertain.

---

# 1. Investigation Background

Target configuration:

```text
Monster Hunter Wilds
+ onehoon/REFramework
+ OptiScaler
+ Intel XeFG output
```

The Intel long-session failure pattern previously observed was:

```text
XeFG init succeeds
-> REF binds generation 1
-> normal resize/reset activity can survive
-> Present/Present1 eventually stops reaching the tracked XeFG binding
-> repeated present_timeout preservation of the same generation
-> later XeFG/D3D12 lifecycle transition
-> crash inside Intel / D3D12 layered-device recreation path
```

A single timeout is not sufficient evidence of a stale binding. Earlier baseline runs showed an isolated timeout followed by normal Present recovery.

Therefore PR-C must not react aggressively to the first timeout.

The NVIDIA MHW startup problem remains a separate OptiScaler backbuffer-release issue and is outside this PR.

---

# 2. Current Master After PR-B

Current master now has the desired ownership model:

```cpp
class XeFGBinding {
    IDXGISwapChain3* m_swapchain{}; // borrowed
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue{};
    Microsoft::WRL::ComPtr<ID3D12Device4> m_device{};
    uint64_t m_generation{};
    bool m_observe_only{};
    RuntimeIdentity m_runtime{};
};
```

PR-B also added:

```cpp
D3D12Hook::detach_xefg_binding_for_runtime_transition(...)
```

and calls it before the original XeFG `InitFromSwapChainDesc` / `Destroy` functions.

The current lifecycle order is conceptually:

```text
hook-monitor lifecycle mutex acquired
-> matching pending candidate dropped
-> matching active XeFG binding detached
-> lifecycle mutex released
-> original Intel Init/Destroy executes
-> candidate is built/published after successful Init
```

That is the correct ownership boundary, but it exposes the remaining PR-C monitor problem described below.

---

# 3. Current Source-Level Problems PR-C Must Close

## 3.1 Active XeFG timeout preservation is currently unconditional and unbounded

Current code:

```cpp
bool XeFGCompatibility::should_preserve_active_binding_on_monitor_timeout(D3D12Hook& hook) noexcept {
    if (!hook.has_active_xefg_instance_binding()) {
        return false;
    }

    spdlog::info(
        "[XeFG][HookMonitor] action = preserve_binding, reason = present_timeout, generation = {}",
        hook.get_xefg_binding_generation());
    hook.log_hook_monitor_snapshot("xefg_rehook_suppressed");
    return true;
}
```

`REFramework::hook_monitor()` then skips generic recovery whenever that function returns true.

There is no distinction between:

```text
one harmless timeout
three consecutive timeouts with zero new Present entries
one minute of the same generation receiving no Present at all
```

This explains the repeated same-generation preservation pattern observed in MHW logs.

## 3.2 The active-binding predicate does not verify the physical VtableHook target identity

Current predicate checks:

```cpp
m_hooked
!m_is_phase_1
m_swapchain_source == XeFGInternal
m_xefg_binding.active()
m_swapchain_hook != nullptr
m_xefg_binding.aliases_match(m_swap_chain, m_command_queue, m_device)
```

But it does not explicitly verify:

```text
m_swapchain_hook target instance == active XeFGBinding swapchain
```

The hook object already exposes its stored target instance through the same mechanism used by existing diagnostics:

```cpp
m_swapchain_hook->get_instance().ptr()
```

PR-C should include this pointer-identity relation in the structural monitor classification.

Do not dereference the borrowed swapchain merely to perform this comparison.

## 3.3 PR-B detach creates a monitor-visible gap while original Intel code runs

This is the most important post-PR-B finding.

`prepare_for_xefg_runtime_transition()` correctly takes the hook-monitor mutex, detaches REF state, then releases that mutex before the original vendor call.

That is required to avoid holding REF locks across Intel code.

However, after the detach and before candidate publication, the current hook state is intentionally:

```text
XeFG source relationship exists conceptually
active XeFGBinding = false
m_swapchain_hook = null
raw XeFG aliases = null
```

During this period `should_preserve_active_binding_on_monitor_timeout()` returns false.

If `hook_monitor()` runs while the original Intel Init/Destroy call is still executing, it is therefore allowed to enter:

```cpp
hook_d3d12();
```

That can insert generic D3D12 recovery into the exact vendor lifecycle window that PR-B deliberately tried to keep clean.

PR-C must add an explicit **runtime-transition gate** so this cannot happen.

## 3.4 PR-B fail-closed detach is not currently represented to the monitor

PR-B intentionally says:

```text
if vendor re-init fails after REF detached:
    do not restore the old hook
    do not reuse the old borrowed proxy
    remain detached and wait for a future validated lifecycle/candidate

if vendor Destroy fails after REF detached:
    same fail-closed rule
```

The current hook monitor does not know this state exists.

After the vendor call returns, an inactive binding currently looks the same as an ordinary missing D3D12 hook, so generic recovery may later run.

PR-C must preserve PR-B's fail-closed contract explicitly.

---

# 4. Required PR-C Invariants

These are merge-blocking.

## Invariant C1 — no generic rehook during an XeFG runtime transition

From immediately before PR-B detaches matching REF state until the intercepted vendor Init/Destroy transaction has completed, hook-monitor recovery must be suppressed.

Do not accomplish this by holding `m_hook_monitor_mutex` across Intel code.

Use a lightweight transition state visible to the monitor.

## Invariant C2 — uncertain detached state remains fail-closed

If REF detached an active XeFG binding and:

```text
Init fails
Init succeeds but no validated candidate is committed
Destroy fails
```

generic D3D12 recovery must remain suppressed.

Do not reconstruct or probe the old borrowed proxy.

## Invariant C3 — successful validated lifecycle can clear the fail-closed gate

The detached/uncertain monitor gate may be cleared only by a state transition that makes recovery safe, for example:

```text
a validated XeFG candidate is successfully committed
OR
matching XeFG Destroy returns success and the old proxy/context is therefore gone
OR
an explicit full D3D12 unhook/shutdown path intentionally clears all state
```

Do not clear it merely because a timer expired.

## Invariant C4 — one timeout never triggers aggressive recovery

An isolated timeout must continue to suppress generic rehook for a structurally consistent active XeFG binding.

## Invariant C5 — sustained timeout is classified, not used as permission for unsafe COM work

After repeated timeouts with no new Present entries on the same binding identity, mark the binding as **sustained timeout / stale suspected / quarantined**.

But timeout alone must NOT cause any of the following:

```text
AddRef on the borrowed proxy
QueryInterface on the borrowed proxy
GetDevice / GetHwnd on the borrowed proxy
Release on the borrowed proxy
VtableHook removal from an object whose lifetime is uncertain
generic hook_d3d12() recovery
forced Release loops
```

The safe action is to quarantine and wait for either:

```text
Present recovery
or
an exact XeFG runtime lifecycle callback handled by PR-B
```

## Invariant C6 — structural inconsistency must not be treated as a healthy active binding

If the XeFG semantic binding, raw aliases, and VtableHook target do not agree, report an inconsistent/quarantined state.

Do not log it as normal `preserve_binding` health.

## Invariant C7 — native / non-XeFG recovery behavior must remain unchanged

Do not rewrite the generic D3D11/D3D12 monitor policy.

PR-C should only intercept monitor decisions while XeFG-specific state is relevant.

---

# 5. Recommended PR Identity

Suggested branch:

```text
fix/xefg-pr-c-hook-monitor-hardening
```

Suggested PR title:

```text
XeFG PR-C: harden hook monitor across runtime transitions and stale bindings
```

Suggested commit title:

```text
fix: harden XeFG hook monitor lifecycle recovery
```

---

# 6. Expected Files in Scope

Primary:

```text
src/REFramework.cpp
src/D3D12Hook.hpp
src/D3D12Hook.cpp
src/compatibility/xefg/XeFGCompatibility.hpp
src/compatibility/xefg/XeFGCompatibility.cpp
```

Recommended if a small testable state machine is extracted:

```text
src/compatibility/xefg/XeFGHookMonitorState.hpp
src/compatibility/xefg/XeFGHookMonitorState.cpp
CMakeLists.txt
```

Potentially touched only to clear monitor state at a successful candidate commit:

```text
src/compatibility/xefg/XeFGCandidateHandoff.*
```

Normally unchanged:

```text
XeFGBinding ownership model
XeFGDiscovery capture/queue policy
XeFGRuntimeRegistry hook installation
XeFGResizeLifecycle / MHW ResizeHold
Present/Present1 rendering logic
ResizeBuffers / ResizeBuffers1 / ResizeTarget behavior
Streamline/DLSS-G support
configuration UI
OptiScaler source
```

Do not add unrelated refactors.

---

# 7. Change C1 — Replace the Boolean Timeout Decision with an Explicit XeFG Monitor Decision

The current boolean API is too weak because `false` means both:

```text
XeFG is irrelevant -> generic recovery is allowed
```

and potentially:

```text
XeFG is in an uncertain detached/transition state -> generic recovery is NOT allowed
```

Use an explicit decision type.

Suggested shape:

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

Possible API:

```cpp
static XeFGMonitorAction evaluate_hook_monitor_timeout(D3D12Hook& hook) noexcept;
```

`REFramework::hook_monitor()` should call generic `hook_d3d12()` only for:

```cpp
XeFGMonitorAction::AllowGenericRecovery
```

All other XeFG actions suppress generic rehook for that monitor cycle.

Keep the native path unchanged.

Example integration:

```cpp
const auto xefg_action = !m_is_d3d11 && d3d12 != nullptr
    ? XeFGCompatibility::evaluate_hook_monitor_timeout(*d3d12)
    : XeFGMonitorAction::AllowGenericRecovery;

if (xefg_action == XeFGMonitorAction::AllowGenericRecovery) {
    spdlog::info("Sending rehook request for D3D");
    ... existing generic recovery ...
}
```

Exact enum names are flexible. The semantic distinction is mandatory.

---

# 8. Change C2 — Add an Explicit Runtime-Transition Gate

Add a lightweight XeFG runtime-transition counter/state in `XeFGCompatibility`.

A counter is safer than a single bool because multiple intercepted calls may theoretically nest/re-enter.

Suggested state:

```cpp
static std::atomic<uint32_t> s_runtime_transition_depth;
```

Suggested RAII helper:

```cpp
class RuntimeTransitionScope {
public:
    RuntimeTransitionScope() noexcept {
        XeFGCompatibility::begin_runtime_transition();
    }

    ~RuntimeTransitionScope() {
        XeFGCompatibility::end_runtime_transition();
    }

    RuntimeTransitionScope(const RuntimeTransitionScope&) = delete;
    RuntimeTransitionScope& operator=(const RuntimeTransitionScope&) = delete;
};
```

Required Init scope:

```text
transition gate ON
-> pre-init diagnostics
-> PR-B pending/active detach
-> original Intel Init
-> build candidate
-> publish / commit candidate if valid
-> init-return diagnostics
-> transition gate OFF
```

Required Destroy scope:

```text
transition gate ON
-> destroy-enter diagnostics
-> PR-B pending/active detach
-> original Intel Destroy
-> record result / update fail-closed state
-> destroy-return diagnostics
-> transition gate OFF
```

The hook-monitor mutex must still be released during the original Intel call.

The transition gate is only a monitor-visible state bit/counter; it is not a lock around Intel code.

Hook monitor behavior while depth > 0:

```text
SuppressRuntimeTransition
```

No generic rehook.

Do not call COM methods from the monitor merely because this state is active.

---

# 9. Change C3 — Represent PR-B's Fail-Closed Detached State

Add explicit state to `D3D12Hook` showing that an active XeFG relationship was detached for a runtime transition but a safe replacement/recovery point has not yet been established.

Suggested minimal shape:

```cpp
struct XeFGDetachedState {
    bool active{};
    XeFGBinding::RuntimeIdentity previous_runtime{};
    uint64_t previous_generation{};
    const char* reason{};
};
```

Do not store the old swapchain pointer here for future use.

The old borrowed pointer must not become a recovery token.

When `detach_xefg_binding_for_runtime_transition()` actually detaches an active binding:

```cpp
m_xefg_detached_state = {
    .active = true,
    .previous_runtime = snapshot.runtime,
    .previous_generation = snapshot.generation,
    .reason = reason,
};
```

Then clear the physical binding exactly as PR-B already does.

### Clear conditions

Clear `m_xefg_detached_state.active` when a validated candidate is successfully committed.

For example, after successful initial/replacement binding:

```cpp
m_xefg_detached_state = {};
```

Also clear it after a matching `xefgSwapChainDestroy()` returns success.

A narrow helper is preferable:

```cpp
void D3D12Hook::note_xefg_destroy_result(
    size_t runtime_slot,
    void* context,
    int32_t result) noexcept;
```

Only clear when:

```text
result == XeFG success
AND detached state's previous runtime slot/context matches the destroyed context
```

On Destroy failure, keep the detached state fail-closed.

On Init failure or rejected candidate, keep it fail-closed.

### Important

Do not time out this state into generic recovery.

A failed vendor lifecycle transaction leaves object lifetime uncertain. Time is not proof of safety.

---

# 10. Change C4 — Strengthen Active XeFG Structural Identity

Refine the current active-binding monitor predicate.

At minimum, a structurally consistent active XeFG binding should require:

```text
D3D12Hook is hooked
not phase 1
source == XeFGInternal
XeFGBinding active
swapchain hook exists
raw aliases match binding swapchain/queue/device
swapchain hook stored target instance == binding swapchain
```

Suggested form:

```cpp
bool D3D12Hook::has_consistent_active_xefg_binding() const noexcept {
    if (!m_hooked
        || m_is_phase_1
        || m_swapchain_source != SwapchainSource::XeFGInternal
        || !m_xefg_binding.active()
        || m_swapchain_hook == nullptr
        || !m_xefg_binding.aliases_match(m_swap_chain, m_command_queue, m_device)) {
        return false;
    }

    return m_swapchain_hook->get_instance().ptr()
        == m_xefg_binding.swapchain();
}
```

This comparison must remain pointer-only.

Do not call `QueryInterface`, `GetDevice`, `GetHwnd`, or `AddRef` from this check.

For diagnostics, expose enough information to distinguish:

```text
semantic binding active
hook missing
hook-target mismatch
alias mismatch
phase/source mismatch
```

If there is XeFG-specific partial/inconsistent state, classify it as `QuarantineInconsistentState` rather than a healthy preserve.

Do not automatically remove the hook solely because of the inconsistency.

---

# 11. Change C5 — Add a Bounded Sustained-Timeout State Machine

Do not count every monitor call globally.

Timeout state must be keyed to the current binding identity and actual Present progress.

Recommended key:

```text
binding generation
runtime slot
runtime context
swapchain pointer
VtableHook target pointer
```

Recommended progress signal:

```text
D3D12Hook::m_present_entry_count
```

The current D3D12 Present/Present1 paths already increment this count and update `m_last_present_entry_time` at entry.

A small dedicated helper is recommended:

```cpp
class XeFGHookMonitorState {
public:
    enum class TimeoutClass : uint8_t {
        Grace,
        Sustained,
    };

    TimeoutClass note_timeout(
        const BindingKey& key,
        uint64_t present_entry_count,
        int64_t present_age_ms);

    void clear() noexcept;
};
```

Suggested state:

```cpp
BindingKey m_key{};
uint64_t m_last_present_entry_count{};
uint32_t m_consecutive_timeouts{};
bool m_sustained_logged{};
```

### Reset rule

Start a new timeout sequence whenever either:

```text
binding key changes
OR
present_entry_count changed since the prior timeout sample
```

That means an isolated timeout followed by any real Present progress does not accumulate toward quarantine.

### Conservative threshold

Use a fixed internal threshold, not a user-facing setting.

Recommended:

```cpp
constexpr uint32_t kSustainedTimeoutThreshold = 3;
constexpr int64_t kMinimumSustainedPresentAgeMs = 20000;
```

Classify as sustained only when both are true:

```text
same binding identity
same Present entry count across 3 timeout evaluations
last Present age >= 20 seconds
```

The current hook-monitor cadence makes this intentionally conservative. Exact values may be adjusted slightly if source-level timing shows a better fit, but do not use one timeout as stale evidence.

---

# 12. Change C6 — Quarantine Sustained Timeout Instead of Generic Rehook

For a structurally consistent active XeFG binding:

### First / second timeout

```text
action = preserve_grace
```

Keep the current behavior of suppressing generic rehook.

### Sustained timeout

```text
action = quarantine_sustained_timeout
```

Still suppress generic rehook.

The important behavioral change is semantic and diagnostic:

```text
not "this binding is healthy forever"
but "Present has been absent long enough to suspect stale state; do not touch the borrowed proxy without lifecycle proof"
```

Do not detach the binding solely because of timeout.

Why:

- PR-B intentionally made the active proxy borrowed.
- A Present timeout gives no guarantee that the raw pointer is still a valid COM object.
- Constructing a temporary `ComPtr` would call AddRef on an uncertain object.
- Removing the VtableHook may write through an uncertain object.
- Generic rehook can run the normal unhook path and create the same uncertainty.

Therefore the safe terminal monitor state is quarantine/wait, not speculative recovery.

Recovery paths remain:

```text
Present resumes -> future monitor sampling observes Present progress and resets timeout sequence
validated XeFG Init candidate -> normal PR-B bind clears detached/quarantine state
matching runtime Destroy/re-init -> PR-B lifecycle logic performs deterministic detach while object lifetime is known
```

---

# 13. Change C7 — Preserve Generic Recovery Only When XeFG State Is Actually Safe/Irrelevant

`AllowGenericRecovery` should be returned only when none of the XeFG safety gates apply.

Examples:

```text
no XeFG-specific active/partial binding relationship
AND no runtime transition in progress
AND no fail-closed detached state
```

A successful matching Destroy may explicitly make generic recovery safe because the old XeFG context/proxy has been destroyed and REF no longer owns/hooks it.

That is materially different from:

```text
Destroy failed
Init failed after detach
candidate rejected after detach
sustained Present timeout with active borrowed proxy
```

Those cases remain suppressed/quarantined.

---

# 14. Hook-Monitor Integration Requirements

Current `REFramework::hook_monitor()` contains generic logic shared with non-XeFG paths. Keep modifications narrow.

Conceptual target:

```cpp
const auto xefg_action =
    (!m_is_d3d11 && d3d12 != nullptr)
        ? XeFGCompatibility::evaluate_hook_monitor_timeout(*d3d12)
        : XeFGMonitorAction::AllowGenericRecovery;

switch (xefg_action) {
case XeFGMonitorAction::AllowGenericRecovery:
    // existing behavior unchanged
    spdlog::info("Sending rehook request for D3D");
    hook_d3d12();
    break;

case XeFGMonitorAction::PreserveGrace:
case XeFGMonitorAction::SuppressRuntimeTransition:
case XeFGMonitorAction::SuppressDetachedUncertain:
case XeFGMonitorAction::QuarantineSustainedTimeout:
case XeFGMonitorAction::QuarantineInconsistentState:
    // Explicitly do not invoke generic recovery.
    break;
}
```

Do not fork or duplicate the rest of `hook_monitor()`.

Keep the existing monitor timer-reset behavior unless a very small change is needed to avoid obvious repeated log spam.

---

# 15. Logging Policy

Reuse the existing persistent XeFG Debug Log setting.

Do not add a new config option.

Normal-user logs should remain bounded.

Recommended state-transition logs:

```text
[XeFG][HookMonitor] action = preserve_grace, reason = present_timeout, generation = 1, timeout_count = 1
[XeFG][HookMonitor] action = quarantine, reason = sustained_present_timeout, generation = 1, timeout_count = 3, present_age_ms = ...
[XeFG][HookMonitor] action = suppress_rehook, reason = runtime_transition
[XeFG][HookMonitor] action = suppress_rehook, reason = detached_uncertain
[XeFG][HookMonitor] action = quarantine, reason = binding_identity_inconsistent
[XeFG][HookMonitor] action = allow_generic_recovery, reason = xefg_state_safe
```

Do not emit the same sustained/quarantine message every monitor cycle.

Log state transitions once where practical.

When Debug Log is enabled, include:

```text
binding generation
runtime slot/context/HWND
semantic swapchain
raw alias swapchain
VtableHook target instance
present entry count
last Present age
consecutive timeout count
runtime transition depth
detached fail-closed state
selected monitor action
```

Do not dereference the borrowed swapchain for diagnostics.

---

# 16. Locking / Reentrancy Rules

These rules are merge-blocking.

## 16.1 Do not hold hook-monitor mutex across Intel code

PR-B's rule remains unchanged.

## 16.2 Runtime transition state must not require a mutex in the monitor

Use atomic transition depth or equivalent lightweight state.

## 16.3 Do not perform COM Release while holding a new monitor-state mutex

PR #35 already fixed the analogous pending-candidate problem.

If a new helper needs synchronization, keep it POD/state-only and never let destruction of COM-owned objects happen inside that lock.

## 16.4 Respect existing lock order

Current important order is:

```text
hook-monitor lifecycle mutex
    -> pending-candidate mutex
```

Do not introduce an inverse acquisition path.

## 16.5 Prefer monitor state owned by `D3D12Hook`

Timeout sequence state belongs to the hook instance and should normally be accessed while the existing hook-monitor lifecycle mutex is held.

Do not create a second global synchronization domain unless required.

---

# 17. Important Safety Rule: No Timeout-Driven COM Probe

This deserves a separate explicit rule.

The following would be a blocking implementation defect:

```cpp
// DO NOT DO THIS on timeout-only evidence.
Microsoft::WRL::ComPtr<IDXGISwapChain3> keepalive = m_xefg_binding.swapchain();
keepalive->GetHwnd(...);
keepalive->GetDevice(...);
```

The raw pointer is borrowed.

Timeout means its lifetime is uncertain.

Only lifecycle paths where the vendor itself is entering a known Init/Destroy operation may use PR-B's bounded keepalive to safely remove the hook before the vendor transition proceeds.

Hook-monitor timeout is not such a lifecycle proof.

---

# 18. Tests

If the repository's current test structure permits a focused helper test, extract the timeout classifier into a small pure state object.

Minimum state-machine coverage:

## Timeout classification

1. First timeout on active key -> `Grace`.
2. Second same-key timeout with unchanged Present count -> still `Grace`.
3. Third same-key timeout with unchanged Present count and sufficient Present age -> `Sustained`.
4. Present count change resets the sequence.
5. Binding generation change resets the sequence.
6. Runtime context/slot change resets the sequence.
7. Hook target identity change resets/reclassifies the sequence.

## Transition gate

1. transition depth 0 -> normal evaluation.
2. depth > 0 -> generic rehook suppressed.
3. nested begin/end pairs do not clear the gate early.

## Detached fail-closed state

1. detach sets uncertain state.
2. successful validated candidate commit clears it.
3. failed Init leaves it set.
4. failed Destroy leaves it set.
5. successful matching Destroy clears it.
6. unrelated Destroy must not clear it.

## Structural identity

1. semantic/raw/hook target all match -> consistent.
2. hook instance mismatch -> inconsistent quarantine.
3. alias mismatch -> inconsistent quarantine.
4. missing hook with active semantic binding -> inconsistent quarantine.

If there is no suitable test harness, keep the classifier small and deterministic and document source-level validation in the PR body. Do not create a large new test framework for this PR.

---

# 19. Mandatory Build / Static Validation

Before opening the PR:

```text
Release x64 build PASS
git diff --check PASS
```

Also verify by source search:

```text
no timeout-driven AddRef/ComPtr construction from active borrowed swapchain
no timeout-driven QueryInterface/GetDevice/GetHwnd
no timeout-driven VtableHook reset
no new forced Release loop
no changes to MHW ResizeHold thresholds/policy
no changes to native/non-XeFG Present/resize semantics
```

PR body must state exactly what was and was not runtime-tested.

---

# 20. Runtime Validation Matrix

PR-C should be validated with the PR-B master baseline plus the final PR-C build.

Minimum targets:

```text
Monster Hunter Wilds + Intel XeFG
Dragon's Dogma 2 + XeFG
one non-MHW RE Engine title if available
```

Recommended scenarios:

```text
normal gameplay 30-60 min
Alt+Tab repeatedly
window/fullscreen or borderless transitions
resolution changes
menu/loading transitions
FG toggle / runtime recreation when supported
idle/background long enough to exceed one hook-monitor timeout
```

### Required log checks

Healthy isolated timeout:

```text
preserve_grace
-> Present entries resume
-> no accumulated sustained timeout state
-> no generic D3D12 rehook
```

Sustained starvation:

```text
same binding key
same present entry count
multiple timeout samples
-> quarantine_sustained_timeout
-> no generic hook_d3d12 recovery
-> no COM probe/release caused by monitor
```

Runtime re-init:

```text
runtime transition gate active
-> old binding detached by PR-B
-> no hook-monitor generic recovery during original Init
-> validated candidate committed
-> fail-closed detached state cleared
-> Present resumes on fresh binding
```

Destroy success:

```text
runtime transition gate active
-> PR-B detach
-> Destroy returns success
-> matching uncertain-detached state cleared
-> generic recovery allowed later if no XeFG replacement appears
```

Destroy failure:

```text
runtime transition gate active
-> PR-B detach
-> Destroy fails
-> detached uncertainty retained
-> generic recovery suppressed
```

---

# 21. MHW Intel Crash Validation

The original Intel crash signature to watch for remains:

```text
MHW / Streamline / OptiScaler / Intel XeFG
-> Intel igxess_fg.dll
-> MHW D3D12Core
-> Windows D3D12
-> D3D12CoreCreateLayeredDevice
-> NULL dereference / C0000005
```

PR-C must not claim to fix that crash solely because the source-level monitor behavior is safer.

The practical validation question is:

> After PR-A + PR-B + PR-C, do sustained Present starvation and later runtime transitions occur without REF issuing unsafe generic rehook activity or retaining stale lifecycle state?

If the crash still occurs with clean REF lifecycle logs, that becomes evidence to move the remaining investigation primarily to OptiScaler / Intel XeFG rather than adding more speculative REF recovery logic.

---

# 22. Explicit Non-Goals

PR-C must NOT:

- restore strong long-lived ownership of the XeFG proxy;
- change queue/device ownership;
- change PR-B detach ordering;
- retry failed XeFG Destroy;
- retry failed XeFG Init;
- create a new swapchain itself;
- call `xefgSwapChainD3D12GetSwapChainPtr` as a timeout recovery probe;
- automatically AddRef/Release a suspected stale proxy;
- automatically tear down the VtableHook solely from timeout evidence;
- automatically call generic `hook_d3d12()` for an uncertain XeFG state;
- modify OptiScaler code;
- change the NVIDIA MHW Resize/Resize1 workaround;
- change MHW ResizeHold behavior;
- refactor Streamline/DLSS-G;
- add user-facing timeout settings;
- perform broad logging cleanup;
- perform general REFramework refactoring.

This is the final narrow XeFG lifecycle-monitor hardening PR, not a new architecture phase.

---

# 23. Suggested Implementation Sequence

Implement in this order:

```text
1. Add explicit XeFG monitor action enum.
2. Add atomic runtime-transition depth / RAII scope.
3. Gate hook_monitor generic recovery while transition depth > 0.
4. Add D3D12Hook fail-closed detached state.
5. Set detached state from PR-B runtime detach.
6. Clear detached state on validated candidate commit.
7. Clear matching detached state on successful Destroy only.
8. Strengthen semantic/raw/VtableHook identity classification.
9. Add pure sustained-timeout state machine.
10. Integrate grace vs quarantine decisions.
11. Add bounded diagnostics.
12. Run build/static validation.
13. Run runtime matrix where hardware is available.
```

After each step, preserve this invariant:

> The monitor may observe XeFG uncertainty, but it must never convert that uncertainty into speculative COM access or generic hook recovery.

---

# 24. Acceptance Criteria

PR-C is complete only when all of the following are true:

1. Hook monitor cannot call generic D3D12 recovery while an intercepted XeFG Init/Destroy transaction is active.
2. Failed Init/Destroy after PR-B detach leaves the hook in an explicit fail-closed state rather than silently becoming generic-rehook eligible.
3. Successful matching Destroy can make generic recovery safe again.
4. Successful validated XeFG candidate commit clears detached uncertainty.
5. Active XeFG structural health checks include the physical VtableHook target identity.
6. One timeout remains a grace/preserve event.
7. Sustained same-generation/no-Present starvation becomes a distinct quarantine state.
8. Sustained timeout does not perform COM probes, COM ownership changes, hook removal, or generic rehook.
9. Present progress or a fresh validated binding resets timeout accumulation.
10. Native/non-XeFG hook-monitor behavior is unchanged.
11. Normal logging is bounded; Debug Log provides sufficient identity/state evidence.
12. Release x64 build passes.
13. `git diff --check` passes.
14. PR body does not claim the Intel MHW crash is fixed without runtime evidence.

---

# 25. Expected End State

After PR-C the REFramework-side XeFG lifecycle should be:

```text
validated XeFG Init
-> active borrowed proxy binding
-> Present/Present1 normally observed

isolated Present timeout
-> grace preserve
-> no generic rehook

sustained no-Present timeout on same identity
-> stale suspicion / quarantine
-> no unsafe COM access
-> no generic rehook
-> wait for Present recovery or exact runtime lifecycle event

matching re-init
-> runtime-transition gate ON
-> PR-B deterministic detach
-> original Intel Init with REF locks released
-> validated candidate commit
-> detached uncertainty cleared
-> gate OFF
-> fresh active binding

matching Destroy
-> runtime-transition gate ON
-> PR-B deterministic detach
-> original Intel Destroy
    success -> detached uncertainty may be cleared safely
    failure -> remain fail-closed
-> gate OFF
```

This completes the planned REFramework-side sequence:

```text
PR-A: observe exact runtime lifecycle
PR-B: remove proxy ownership conflict and detach deterministically
PR-C: prevent monitor recovery from violating that lifecycle and classify sustained stale state safely
```

After PR-C, further MHW Intel crash work should be evidence-driven from runtime logs/dumps rather than adding additional speculative REFramework recovery behavior.