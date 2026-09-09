# REFramework × OptiScaler Intel XeFG — Second-Stage Refactor Design

Date: 2026-09-09  
Repository: `onehoon/REFramework`  
Planning baseline: `master` @ `32f202c8e6944bf7f1eda8f2610ebef58c69b11f`  
Baseline commit: `XeFG F-03: honor non-negative result semantics (#37)`  
Upstream comparison baseline: `praydog/REFramework` `master` @ `b6baf6b406efc65e077b99cb4d9ad25b0a0a9095`

---

## 1. Decision

A second-stage refactor is justified, but only under a much narrower rule than a general REFramework refactor:

> Refactor only the fork-specific compatibility path required for REFramework to coexist with OptiScaler when OptiScaler uses Intel XeFG on D3D12.

The refactor is **not** intended to improve, redesign, modernize, or generalize REFramework itself.

The highest-priority requirement is stronger than architectural cleanliness:

> **REFramework's original/native functionality must not regress.**

This requirement governs every PR in this plan.

The target topology remains:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (this fork)
Special K    = absent
FG Output    = Intel XeFG
Renderer     = D3D12
```

The second-stage work is primarily structural hardening. The first refactor and later lifecycle fixes already solved many concrete compatibility problems. The remaining value comes from reducing state duplication, keeping XeFG policy out of generic D3D12 code, and making future XeFG fixes less likely to destabilize normal REFramework behavior.

---

## 2. Relationship to the First Refactor

The existing documents under `doc/refactor/` describe the first XeFG compatibility refactor and remain useful historical references:

- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_ARCHITECTURE_2026-09-06.md`
- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_PR_SPLIT_PLAN_2026-09-06.md`

That work was implemented across R1-R11 and later followed by lifecycle hardening in PRs #34-#37.

This document is the new planning authority for a second-stage refactor on the current master.

### Important semantic change since the first design

The old design documents contain statements describing the active XeFG swapchain as strongly owned by `XeFGBinding`.

That is no longer the current contract.

PR #35 intentionally changed the active XeFG swapchain to a **borrowed pointer** while preserving strong ownership for queue/device and using only bounded local keepalives when REF must safely remove a vtable hook.

Current master is authoritative:

```cpp
IDXGISwapChain3* m_swapchain{}; // borrowed; hook lifetime is bounded by the caller
Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue{};
Microsoft::WRL::ComPtr<ID3D12Device4> m_device{};
```

Therefore this second-stage refactor must preserve the following ownership model:

```text
pending/candidate swapchain
    -> strong ComPtr while candidate is pending / being validated

active XeFG binding swapchain
    -> borrowed pointer

active selected queue/device
    -> strong ComPtr

old active swapchain during destructive hook removal
    -> bounded local ComPtr keepalive only
```

Do not restore permanent active `ComPtr<IDXGISwapChain3>` ownership merely because the older refactor document used that model.

---

## 3. Non-Negotiable Scope

### 3.1 XeFG only

This refactor must not redesign or intentionally change:

- DLSSG / Streamline;
- FSRFG;
- D3D11;
- generic frame-generation handling;
- generic DXGI architecture;
- REFramework renderer architecture;
- Lua / scripting;
- plugins / mods;
- input;
- VR;
- game integration unrelated to XeFG;
- the anti-tamper/integrity functionality of REFramework;
- OptiScaler private internals.

Do not introduce:

- `IFrameGenerationProvider`;
- a generic FG provider registry;
- a generic multi-provider state machine;
- a new generic presentation framework;
- a replacement hooking library.

### 3.2 Native REFramework behavior is a protected compatibility surface

When no validated OptiScaler/XeFG presentation binding is active, control flow must remain effectively the existing REFramework path.

Desired rule:

```text
XeFG inactive / not validated
    -> existing native REFramework path

validated OptiScaler + XeFG binding active
    -> narrow XeFG compatibility path may intervene

XeFG discovery/bind/lifecycle failure
    -> fail closed for XeFG-specific work
    -> do not corrupt generic REFramework D3D12 state
    -> do not invent fallback ownership/release behavior
```

The second-stage refactor is successful only if it makes XeFG changes easier to contain without changing how normal REFramework works.

### 3.3 Behavior-preserving PR series

The required PR series in this document is a **behavior-preserving refactor**.

Each PR must preserve the current master semantics before proceeding to the next PR.

If a desired cleanup would intentionally change runtime behavior, it does not belong in the required series. It must be proposed separately after the second-stage refactor is complete and validated.

---

## 4. Current Master — Code Review Findings

Current master already has a good compatibility island under:

```text
src/compatibility/xefg/
    XeFGBinding.cpp/.hpp
    XeFGCandidateHandoff.cpp/.hpp
    XeFGCompatibility.cpp/.hpp
    XeFGDiscovery.cpp/.hpp
    XeFGResizeLifecycle.cpp/.hpp
    XeFGResult.hpp
    XeFGRuntimeRegistry.cpp/.hpp
```

The problem is no longer that all XeFG code lives in `D3D12Hook.cpp`.

The remaining problem is that **the active presentation session and policy are still split between the compatibility island and `D3D12Hook`**.

### 4.1 Current state is represented in several places

For an active XeFG binding, current correctness depends on agreement between:

```text
1. semantic XeFG state
   XeFGBinding

2. renderer-facing raw aliases
   D3D12Hook::m_swap_chain
   D3D12Hook::m_command_queue
   D3D12Hook::m_device

3. physical hook target
   D3D12Hook::m_swapchain_hook->get_instance()

4. lifecycle side-state
   XeFGResizeLifecycle
   XeFGDetachedState
   XeFGHookMonitorState
   m_last_xefg_monitor_action
```

The current consistency predicate demonstrates the coupling directly:

```cpp
return m_hooked
    && !m_is_phase_1
    && m_swapchain_source == SwapchainSource::XeFGInternal
    && m_xefg_binding.active()
    && m_swapchain_hook != nullptr
    && m_xefg_binding.aliases_match(m_swap_chain, m_command_queue, m_device)
    && m_swapchain_hook->get_instance().ptr() == m_xefg_binding.swapchain();
```

This predicate is correct and useful, but it also shows why further structural hardening has value: multiple independent fields must stay synchronized across bind, replacement, resize, re-init, destroy, monitor timeout, and unhook.

### 4.2 `D3D12Hook.hpp` still exposes too much XeFG state

Current `D3D12Hook.hpp` directly contains or exposes:

- `XeFGMonitorBindingKey`;
- `XeFGHookMonitorState`;
- `SwapchainSource::XeFGInternal`;
- active `XeFGBinding`;
- active `XeFGResizeLifecycle`;
- detached-state structure;
- monitor state and last action;
- runtime detach/destroy methods;
- candidate apply method;
- binding replacement method;
- alias synchronization method;
- resize hold methods;
- numerous XeFG semantic predicates;
- XeFG diagnostics and lifecycle getters.

This is still a large fork-specific surface in one of the most upstream-sensitive REFramework headers.

### 4.3 `D3D12Hook.cpp` still contains the active XeFG session state machine

Current core code still performs:

- active candidate classification routing;
- initial XeFG direct bind;
- transactional rebind;
- semantic binding commits;
- detached-state creation/clear;
- hook-monitor sample state;
- Present/Present1 render suppression decisions;
- resize-hold policy;
- MHW-specific resize policy;
- XeFG lifecycle logging state;
- synchronization of semantic binding to generic raw aliases.

The first refactor moved discovery/runtime responsibilities out correctly, but the active presentation session still lives inside the generic D3D12 hook object.

### 4.4 MHW-specific XeFG policy still exists in generic D3D12 hook code

Current `resize_target()` contains a narrow MHW rule:

```cpp
if (event_id != 0
    && renderer_reset_performed
    && d3d12->is_xefg_render_capable()
    && sdk::GameIdentity::get().is_mhwilds()) {
    d3d12->arm_xefg_resize_transition_hold(event_id);
}
```

The policy itself is intentionally narrow and should remain unchanged.

Its location is the problem.

A generic low-level D3D12 hook should not need to know that one specific game requires an XeFG resize hold. That is compatibility policy and belongs under `compatibility/xefg`.

### 4.5 Hook-monitor policy is isolated conceptually but still depends on D3D12 internals

`XeFGCompatibility::evaluate_hook_monitor_timeout(D3D12Hook&)` already owns the decision policy, but it reaches deeply into D3D12Hook via friend access to inspect:

- detached uncertainty;
- active binding consistency;
- monitor key;
- timeout sampler;
- physical hook target;
- presentation progress.

The direction is mostly correct, but the state it evaluates should be owned by one XeFG presentation-session object rather than by the generic D3D12 hook.

### 4.6 Candidate/discovery layers are already reasonably clean

The following components should not be redesigned without a concrete defect:

- `XeFGRuntimeRegistry`;
- `XeFGDiscovery`;
- `XeFGResult`.

They already implement the intended boundaries:

- exact-HMODULE runtime registration;
- bounded InitDesc/factory observation;
- public XeFG result semantics;
- queue/device validation;
- internal presentation swapchain candidate construction.

The second refactor should build on them rather than replace them.

---

## 5. Upstream Comparison and Native-Path Protection

At the comparison baseline, upstream `praydog/REFramework` `D3D12Hook.hpp` contains the normal D3D12 state and Streamline handling but no XeFG subsystem.

Upstream's public/protected surface is materially smaller than the fork's current `D3D12Hook.hpp`.

This second-stage refactor should move the fork closer to the following principle:

> The visible structural delta in `D3D12Hook.hpp/.cpp` should be a small XeFG bridge, not an XeFG lifecycle implementation.

This does **not** mean blindly restoring upstream code or deleting current fork behavior.

Current fork behavior is the baseline for the behavior-preserving series.

### Important native Present1 observation

Current fork's native phase-1-to-instance transition installs `Present1[22]` in addition to the older native hook set, while the inspected upstream `D3D12Hook.hpp` does not expose a `Present1` hook.

Because the required second-stage series is behavior-preserving, **do not remove that native Present1 hook during this refactor**.

After the refactor and runtime validation, a separate audit may determine whether that is unnecessary XeFG spillover into the native path. Any removal would be a separate behavior-change PR, not part of this plan.

---

## 6. Target Architecture

Introduce one new active-session object:

```text
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp
```

Conceptually:

```text
REFramework core
    |
    | minimal integration calls
    v
D3D12Hook
    |  owns generic/native physical D3D12 hook mechanism
    |  owns renderer-facing raw aliases
    |  executes physical Present/Resize forwarding
    |
    +---- one narrow XeFG bridge ---->
                                   XeFGPresentationSession
                                      |
                                      +-- XeFGBinding
                                      +-- XeFGResizeLifecycle
                                      +-- detached lifecycle state
                                      +-- hook-monitor state
                                      +-- Present suppression policy
                                      +-- MHW XeFG resize policy
                                      +-- lifecycle snapshots/diagnostics state

XeFGCompatibility
    |
    +-- XeFGRuntimeRegistry
    +-- XeFGDiscovery
    +-- XeFGCandidateHandoff
    +-- coordinates runtime transition with XeFGPresentationSession
```

### 6.1 Why `XeFGPresentationSession`

One active XeFG presentation path has one coherent lifecycle:

```text
candidate
 -> active binding
 -> present/resize activity
 -> possible rebind
 -> runtime detach
 -> destroy/re-init result
 -> active again or quarantined/idle
```

That lifecycle should be represented by one object.

The name `XeFGPresentationSession` is intentional:

- it is XeFG-specific;
- it is presentation/lifecycle-specific;
- it is not a generic FG provider;
- it does not imply ownership of the Intel context itself;
- it does not imply ownership of generic REFramework rendering.

### 6.2 D3D12Hook remains the mechanism owner

To minimize native REFramework regression risk, **do not move generic D3D12 physical hook ownership out of `D3D12Hook` in this second-stage series**.

`D3D12Hook` continues to own:

- generic `m_present_hook`;
- generic/active `m_swapchain_hook`;
- callback addresses;
- original function lookup;
- forwarding to original Present/Present1/Resize methods;
- `m_on_present` / `m_on_post_present` / resize callback invocation;
- existing renderer-facing raw aliases;
- native command-queue discovery;
- Streamline behavior.

This is deliberately conservative.

Moving the physical vtable-hook object into the compatibility subsystem would produce stronger theoretical isolation, but it would also alter the most timing-sensitive ownership boundary. That is not justified while the primary requirement is zero regression of REFramework's existing behavior.

### 6.3 Session becomes semantic authority for XeFG

While an XeFG session is active:

```text
XeFGPresentationSession::binding()
    = semantic authority

D3D12Hook::m_swap_chain
D3D12Hook::m_command_queue
D3D12Hook::m_device
    = renderer-facing mirrors only

D3D12Hook::m_swapchain_hook target
    = physical hook mechanism only
```

Required invariant:

```text
if XeFGPresentationSession is active:
    raw renderer aliases must match session binding
    physical hook target must match session binding swapchain
```

The consistency check belongs to the session API, not to scattered D3D12Hook conditionals.

When no XeFG session is active, the session must not become an authority over native D3D12 state.

---

## 7. Proposed `XeFGPresentationSession` Responsibilities

The exact API may be adjusted during implementation, but responsibility boundaries are mandatory.

### 7.1 State owned by the session

Move these concepts out of `D3D12Hook`:

```text
XeFGBinding
XeFGResizeLifecycle
XeFGDetachedState
XeFGMonitorBindingKey
XeFGHookMonitorState
last XeFG monitor action/reason
XeFG P2.1 render-boundary diagnostic flag
```

Recommended conceptual state:

```cpp
class XeFGPresentationSession {
public:
    struct DetachedState {
        bool active{};
        XeFGBinding::RuntimeIdentity previous_runtime{};
        uint64_t previous_generation{};
        const char* reason{};
    };

    struct PhysicalBindingView {
        bool hook_object_active{};
        bool phase1{};
        IDXGISwapChain3* renderer_swapchain{};
        ID3D12CommandQueue* renderer_queue{};
        ID3D12Device4* renderer_device{};
        void* hook_target{};
    };

    // semantic state / snapshots
    bool active() const noexcept;
    bool observe_only() const noexcept;
    bool render_capable() const noexcept;
    bool has_monitor_state() const noexcept;
    bool detached_uncertain() const noexcept;
    bool consistent_with(const PhysicalBindingView&) const noexcept;
    XeFGBinding::RuntimeLifecycleSnapshot lifecycle_snapshot() const noexcept;

private:
    XeFGBinding m_binding{};
    XeFGResizeLifecycle m_resize{};
    DetachedState m_detached{};
    XeFGHookMonitorState m_monitor{};
    const char* m_last_monitor_action{};
    bool m_render_boundary_logged{};
};
```

This is an architectural sketch, not a requirement to use these exact names.

### 7.2 What the session must not own

Do not move these into the session:

- generic `D3D12Hook::m_swapchain_hook`;
- generic `D3D12Hook::m_present_hook`;
- native swapchain discovery;
- native queue offset scanning;
- Streamline hooks;
- REFramework ImGui renderer objects;
- `m_on_present` callback ownership;
- game scripting/mod/plugin execution;
- Intel XeFG context ownership;
- OptiScaler objects.

---

## 8. Exact File / Class Movement Plan

### 8.1 `src/D3D12Hook.hpp`

#### Keep

- normal D3D12 public getters;
- normal Present/Resize callback registration;
- native D3D12 state;
- `m_present_hook`;
- `m_swapchain_hook`;
- Streamline structure;
- native hook implementation declarations;
- minimal XeFG bridge declarations required by the active session.

#### Move out

- `XeFGMonitorBindingKey`;
- `XeFGHookMonitorState`;
- `XeFGDetachedState`;
- direct `XeFGBinding m_xefg_binding` field;
- direct `XeFGResizeLifecycle m_xefg_resize_lifecycle` field;
- direct detached/monitor/last-action state fields;
- XeFG policy helpers whose only purpose is to interpret session state;
- MHW/XeFG policy surface;
- monitor classifier details.

#### Target end-state

`D3D12Hook.hpp` should ideally need only a forward declaration plus a narrow session member or bridge:

```cpp
class XeFGPresentationSession;

class D3D12Hook {
    ...
private:
    std::unique_ptr<XeFGPresentationSession> m_xefg_session{};
};
```

A small number of bridge/friend methods is acceptable if required to preserve current physical-hook mechanics.

Do not force pimpl/general D3D12 refactoring merely to remove every XeFG name from this header.

### 8.2 `src/D3D12Hook.cpp`

#### Keep as mechanism

- native `hook()` / `hook_impl()` behavior;
- native phase-1 Present path;
- existing native/Streamline queue discovery;
- physical `VtableHook` creation/removal;
- physical original-function lookup;
- renderer callback execution;
- actual original Present/Present1/Resize invocation;
- generic raw alias fields and normal native updates.

#### Move to the session

- `XeFGHookMonitorState::note_timeout` / `clear`;
- active XeFG consistency policy;
- detached lifecycle state policy;
- monitor key construction;
- monitor timeout sampling;
- XeFG render suppression decision;
- resize hold decision/state interpretation;
- MHW-specific hold decision;
- session-specific diagnostic state.

#### Retain only thin calls

Desired callback shape after extraction:

```cpp
if (xefg_session != nullptr && xefg_session->tracks(swap_chain)) {
    const auto decision = xefg_session->on_present_enter(...physical view...);
    // D3D12Hook executes the same current callback/original ordering.
}
```

The extraction must not rewrite the generic native Present body into a new universal abstraction.

### 8.3 `src/compatibility/xefg/XeFGPresentationSession.hpp/.cpp` — new

Own:

- active XeFG semantic binding state;
- resize lifecycle state;
- detached uncertainty state;
- monitor timeout state;
- monitor identity key;
- present suppression decisions;
- resize event/hold decisions;
- MHW-only hold policy;
- lifecycle snapshot helpers;
- bounded session diagnostics state.

Do not own the generic renderer or generic hook objects.

### 8.4 `XeFGBinding.hpp/.cpp`

Keep as a focused value/state object.

The session should own an instance of it.

Do not merge `XeFGBinding` into a large monolithic session implementation unless doing so clearly reduces complexity without altering tested behavior.

Preserve:

```text
swapchain = borrowed
queue = strong ComPtr
device = strong ComPtr
generation semantics unchanged
runtime identity semantics unchanged
```

### 8.5 `XeFGResizeLifecycle.hpp/.cpp`

Keep as a focused helper.

Move only its ownership from `D3D12Hook` to `XeFGPresentationSession`.

Preserve exactly:

- event-id behavior;
- event kind behavior;
- hold arm behavior;
- successful completion rule;
- failure retention rule;
- suppressed-present count;
- post-resize sampling budget.

### 8.6 `XeFGCandidateHandoff.hpp/.cpp`

Current lifecycle-sensitive mutex behavior is correct and must be preserved.

Second-stage goal:

- stop treating `D3D12Hook` as the semantic XeFG binding owner;
- route candidate application through the live session/compatibility bridge;
- preserve capture-before-hook behavior;
- preserve pending candidate behavior;
- preserve release-outside-pending-mutex behavior.

Do not introduce polling, a worker thread, arbitrary delay, or a queue of stale candidates.

### 8.7 `XeFGCompatibility.hpp/.cpp`

Keep as high-level XeFG runtime façade.

Change its relationship with active presentation state:

```text
current:
XeFGCompatibility -> friend access into many D3D12Hook XeFG fields

target:
XeFGCompatibility -> XeFGPresentationSession public/internal API
                   -> minimal D3D12 physical bridge only when needed
```

Keep:

- module detection;
- runtime registry coordination;
- runtime-transition depth;
- Init/GetSwapchain/Destroy dispatch;
- result semantics;
- debug logging façade.

Move detailed monitor state evaluation to the session, while compatibility continues to own/log the high-level recovery action.

### 8.8 `XeFGDiscovery.hpp/.cpp`

No redesign planned.

Only update include/API references if required by the new session/handoff boundary.

### 8.9 `XeFGRuntimeRegistry.hpp/.cpp`

No behavioral change planned.

### 8.10 `XeFGResult.hpp`

No behavioral change planned.

### 8.11 `src/REFramework.cpp`

Keep loader infrastructure and generic hook-monitor cadence unchanged.

Allow only narrow call-surface cleanup.

For example, instead of core code needing to reason about detailed XeFG states, compatibility may expose a narrow recovery decision.

Conceptual target:

```cpp
const auto allow_recovery =
    XeFGCompatibility::allow_generic_d3d12_recovery(d3d12.get());
```

Detailed reasons such as grace, transition, detached uncertainty, sustained timeout, and inconsistency remain inside XeFG compatibility diagnostics.

Do not change:

- 5-second / last-chance generic watchdog semantics;
- D3D11 behavior;
- message-hook watchdog;
- Streamline loader handling;
- generic `hook_d3d12()` behavior.

### 8.12 Build files

`cmake.toml` is authoritative and already uses source globs for the REFramework target, but the checked-in generated `CMakeLists.txt` contains explicit generated source lists.

When adding `XeFGPresentationSession.cpp/.hpp`:

1. add the source files;
2. run the repository's cmkr generation flow (`cmkr gen` / existing project command);
3. commit the regenerated `CMakeLists.txt`;
4. do not treat manual edits to generated `CMakeLists.txt` as the source of truth.

This avoids repeating the earlier source-registration CI issue.

---

## 9. Core Lifecycle Invariants That Must Survive Every PR

### 9.1 Candidate ownership

`XeFGBindingCandidate` may strongly own the candidate swapchain while it is pending or being applied.

Do not weaken this prematurely.

### 9.2 Active swapchain ownership

The active session stores the swapchain as borrowed.

Do not introduce permanent AddRef ownership in the active binding.

### 9.3 Hook-removal keepalive

Before removing/restoring a vtable hook whose target is the borrowed XeFG swapchain:

```text
capture bounded local ComPtr keepalive
    -> renderer reset if current behavior requires it
    -> remove old VtableHook
    -> clear aliases / semantic binding
    -> release local keepalive at scope end
```

Do not call Intel vendor code while keeping stale REF hooks on an object being destroyed/reinitialized.

### 9.4 Transactional replacement

Changed-object replacement must preserve the current order:

```text
validate next candidate
-> acquire bounded strong next references needed for preparation
-> prepare all five XeFG hook methods
-> if preparation fails: old binding remains intact
-> reset old renderer
-> remove old physical hook while bounded old keepalive exists
-> commit semantic next binding
-> synchronize renderer-facing aliases
-> install/commit prepared physical hook
-> clear transition/monitor state as current behavior requires
```

Required XeFG slots remain:

```text
Present[8]
Present1[22]
ResizeBuffers[13]
ResizeTarget[14]
ResizeBuffers1[39]
```

### 9.5 Same-object update

Same swapchain with changed queue/mode must preserve the existing same-object update behavior:

- no unnecessary physical hook replacement;
- one renderer reset as currently required;
- update queue/device/mode/runtime identity;
- increment generation using current semantics.

### 9.6 Runtime transition lock rule

Current safe lock direction must remain:

```text
hook-monitor/lifecycle mutex
    -> pending-candidate mutex
```

Release REF lifecycle locks before calling Intel Init/Destroy vendor code.

Never hold the hook-monitor mutex across the vendor call simply to make session ownership look cleaner.

### 9.7 Candidate destruction and COM re-entry

When dropping a pending candidate during runtime transition:

```text
move candidate ownership out while pending mutex is held
-> clear pending state
-> unlock pending mutex
-> release candidate COM references
```

This protects against a COM `Release()` causing re-entry into XeFG/REF code while the pending mutex is still held.

### 9.8 XeFG result semantics

PR #37 semantics are part of the contract:

```text
result >= 0 -> non-failure / success-or-warning path
result < 0  -> failure
```

Do not regress to exact-zero success checks.

### 9.9 Failure policy

A negative Init/Destroy result or an inconsistent active session must remain fail-closed.

Forbidden recovery shortcuts:

- force-Release loops;
- repeated COM refcount draining;
- timeout-triggered COM probing of a borrowed stale swapchain;
- restoring a stale hook after failed Destroy;
- binding the public interpolation proxy just because internal discovery failed;
- forced generic D3D rehook while detached state is uncertain.

---

## 10. Present / Present1 Contract

The second refactor may move the **decision** into the session, but it must not change execution order.

### Render-capable XeFG path

Preserve:

```text
tracked XeFG Present / Present1
-> validate physical/session identity
-> update presentation liveness
-> m_on_present
-> original Present / Present1
-> m_on_post_present
```

### Observe-only or resize-hold path

Preserve:

```text
tracked XeFG Present / Present1
-> validate identity
-> update real Present liveness
-> suppress renderer/mod GPU callbacks
-> original Present / Present1 still forwarded
-> REFramework note_present_activity()
-> no normal post-render callback
```

### Nested Present

Preserve the current recursion handling and direct original forwarding behavior.

Do not add timers, sleeps, or delayed rendering.

### Suggested decision object

A narrow result can make the mechanism/policy separation explicit:

```cpp
struct XeFGPresentDecision {
    bool tracked{};
    bool suppress_renderer_callbacks{};
    bool maintain_monitor_liveness{};
    bool resize_hold_active{};
    uint32_t post_resize_ordinal{};
    bool log_first_render_boundary{};
};
```

This is preferable to D3D12Hook asking six separate XeFG helper predicates during each callback.

---

## 11. Resize Contract

The session should own XeFG interpretation of resize events while D3D12Hook continues to execute the physical callback and existing renderer reset.

### `ResizeBuffers`

Preserve:

- top-level tracking semantics;
- existing renderer reset order;
- original call forwarding;
- hold completion only on current valid successful completion behavior.

### `ResizeBuffers1`

Remain XeFG-specific in the compatibility path.

Preserve:

- tracked XeFG instance validation;
- nested forwarding behavior;
- observe-only reset suppression;
- pre-original renderer reset in render-capable mode;
- all original arguments unchanged;
- hold completion semantics.

### `ResizeTarget`

Preserve current MHW-only activation rule.

Move only the policy location:

```text
current:
D3D12Hook.cpp -> sdk::GameIdentity::get().is_mhwilds()

target:
XeFGPresentationSession / XeFG-specific policy -> is_mhwilds()
```

The low-level D3D12 callback should ask the XeFG session whether a hold should be armed after the existing renderer reset.

Do not generalize the hold to all games.

Do not add a timer fallback.

---

## 12. Hook-Monitor Contract

The generic REFramework monitor must remain generic.

The XeFG session owns enough state to answer whether generic recovery is safe.

Internal XeFG actions may remain:

```text
AllowGenericRecovery
PreserveGrace
SuppressRuntimeTransition
SuppressDetachedUncertain
QuarantineSustainedTimeout
QuarantineInconsistentState
```

But `REFramework.cpp` should not need direct access to XeFG binding internals.

### Current timeout semantics to preserve

- key changes / new Present progress reset the timeout sequence to grace;
- sustained classification requires the current established threshold;
- sustained/inconsistent/detached/runtime-transition states suppress destructive generic recovery;
- native/non-XeFG path continues to allow normal generic recovery.

Do not change thresholds during the refactor.

If threshold tuning is ever desired, make it a separate behavior-change PR backed by runtime evidence.

---

# 13. Required PR Sequence

The second-stage refactor should be split into **7 PRs**.

The count is intentionally larger than a coarse 4-5 PR refactor because the product requirement is not merely code cleanliness; it is preservation of REFramework behavior. Each PR should have one primary failure domain and should be independently revertible.

---

## 2R1 — Introduce `XeFGPresentationSession` Shell and Move Session-Only Types

### Goal

Create the new session boundary without changing active binding behavior.

### Add

```text
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp
```

### Move first

- `XeFGMonitorBindingKey`;
- `XeFGHookMonitorState` implementation;
- `XeFGDetachedState` type;
- session-local monitor action/reason storage type if practical.

### Do not move yet

- active `XeFGBinding` ownership;
- active `XeFGResizeLifecycle` ownership;
- physical hook transaction;
- Present/Resize policy;
- candidate handoff.

### Bridge strategy

Temporary delegation from D3D12Hook is acceptable.

The purpose of 2R1 is to establish the target type and prove build/integration with no semantic change.

### Files expected

- new session `.hpp/.cpp`;
- `D3D12Hook.hpp/.cpp` minimal type movement;
- generated build files if required.

### Acceptance

- Release x64 build;
- `git diff --check`;
- `python dev/audit_direct_access_clang.py` passes;
- no native Present/Resize logic changes;
- no runtime thresholds changed;
- no ownership changes.

---

## 2R2 — Move Active XeFG Semantic State Into the Session

### Goal

Make `XeFGPresentationSession` the semantic state owner while leaving D3D12Hook as physical hook/renderer mechanism.

### Move ownership

From `D3D12Hook` into session:

- `XeFGBinding`;
- `XeFGResizeLifecycle`;
- detached state;
- monitor state;
- last monitor action;
- P2.1 render-boundary diagnostic flag.

### Keep in D3D12Hook

- `m_swap_chain`;
- `m_command_queue`;
- `m_device`;
- `m_swapchain_hook`;
- `m_present_hook`.

These raw aliases remain necessary to existing REFramework rendering.

### Add a physical-view check

The session should receive a narrow view of the physical/generic state when it needs to validate consistency.

Do not give the session arbitrary access to every D3D12Hook field.

### Critical invariant

When session is inactive, no session method should mutate native aliases.

### Acceptance

- same initial/rebind/detach generation values as before;
- same borrowed-swapchain ownership;
- same queue/device strong ownership;
- same raw alias values while active;
- same physical-target consistency check;
- native path unchanged.

---

## 2R3 — Move Runtime-Detach and Hook-Monitor State Policy Behind the Session

### Goal

Stop `XeFGCompatibility` from depending on many D3D12Hook XeFG internals.

### Move/delegate

- runtime identity match/evaluation;
- detached uncertainty creation and clearing;
- destroy-result reconciliation;
- monitor key construction;
- timeout sampling;
- consistency classification;
- monitor action deduplication.

### Keep in D3D12Hook

The actual destructive mechanism required for detach:

```text
renderer reset
physical hook reset/removal
raw alias clearing
```

The session decides/records what lifecycle transition is occurring; D3D12Hook executes the physical operation using the same ordering.

### `REFramework.cpp`

Reduce the hook-monitor interface to a narrow allow/suppress result if practical in this PR.

Do not alter monitor timing.

### Acceptance

- runtime-transition gate behavior identical;
- negative Destroy leaves detached uncertainty;
- matching non-negative Destroy clears the detached state exactly as current master;
- sustained timeout still quarantines;
- inconsistent identity still quarantines;
- native timeout recovery unchanged.

---

## 2R4 — Move XeFG Present / Present1 Policy Into the Session

### Goal

Make D3D12Hook perform callbacks/forwarding while the session decides XeFG-specific suppression/lifecycle behavior.

### Move decisions

- observe-only suppression;
- resize-hold suppression;
- suppressed-present counting;
- post-resize present sample decision;
- first-render-boundary diagnostic state;
- monitor-liveness decision for suppressed rendering.

### Keep mechanism

- actual original function lookup;
- recursion guard;
- `m_on_present` invocation;
- original Present/Present1 invocation;
- `m_on_post_present` invocation;
- generic renderer-facing alias assignment required by current flow.

### Critical requirement

The before/after call sequence must be identical to current master.

No universal Present abstraction should be introduced.

### Acceptance

Static review must demonstrate that for every existing XeFG mode the same branches invoke:

- renderer callback or suppression;
- original Present;
- post callback or `note_present_activity()`.

---

## 2R5 — Move XeFG Resize Policy and MHW Rule Into the Session

### Goal

Remove XeFG game-specific resize policy from the generic D3D12 callback implementation.

### Move

- begin/complete/clear XeFG resize event interpretation;
- resize-hold arm/complete decision;
- MHW-only `ResizeTarget` hold policy;
- session-specific resize lifecycle diagnostics state.

### Keep

- generic `resize_buffers()` mechanics;
- generic `resize_target()` mechanics;
- XeFG `resize_buffers1()` physical callback/forwarding;
- actual renderer reset callback execution.

### Expected source cleanup

`D3D12Hook.cpp` should no longer need `sdk/GameIdentity.hpp` solely for the MHW XeFG hold rule.

### Acceptance

- MHW remains the only game where current hold is armed;
- DD2/other games retain current no-hold policy;
- failure/success semantics unchanged;
- no timer/sleep fallback.

---

## 2R6 — Consolidate XeFG Candidate Apply / Bind / Rebind Transaction Around Session Authority

### Goal

Remove duplicate semantic mutation paths while leaving physical hook ownership in D3D12Hook.

### Current problem

Initial bind and active replacement contain overlapping work:

- validate device;
- prepare five hooks;
- reset previous renderer;
- remove old hooks;
- commit semantic binding;
- mirror aliases;
- update session state.

This increases the chance that one future XeFG fix changes only one path.

### Target

The session should classify semantic candidate changes:

```text
NoActiveBinding
Identical
SameSwapchainUpdate
ChangedSwapchainReplacement
Reject
```

D3D12Hook then executes one physical transaction according to that plan and calls the session's explicit commit method only after the physical preconditions are satisfied.

### Important boundary

Do **not** move `m_swapchain_hook` ownership out of D3D12Hook in this PR.

### Candidate handoff

Update `XeFGCandidateHandoff` so semantic application enters through the session/compatibility bridge rather than relying on friend access to D3D12Hook state.

Preserve pending mutex and lifecycle mutex ordering exactly.

### Acceptance

- all five hook methods prepared before destructive replacement;
- failed preparation leaves old binding/hook/renderer unchanged;
- same-object update keeps existing hook;
- changed-object replacement keeps bounded old keepalive through hook removal;
- active swapchain remains borrowed after commit;
- no forced Release behavior.

---

## 2R7 — Collapse Temporary XeFG Surface in `D3D12Hook` and Final Non-Regression Audit

### Goal

Remove transitional wrappers and make the intended boundary visible in the final source layout.

### Remove where no longer needed

- direct XeFG state fields in D3D12Hook;
- monitor state types from D3D12Hook header;
- detached-state type from D3D12Hook header;
- friend access that is no longer necessary;
- duplicated helper predicates replaced by one session decision;
- duplicated semantic alias synchronization entry points;
- dead/unreachable XeFG-only branches in generic helper code **only if static control-flow review proves they are unreachable on current master**.

### Keep intentionally

- minimal XeFG bridge/session pointer;
- physical hook methods used by XeFG;
- current native behavior;
- current native Present1 behavior, even if later considered for a separate audit;
- current diagnostics needed to debug lifecycle failures.

### End-state audit

Compare final `D3D12Hook.hpp/.cpp` against upstream conceptually:

```text
upstream/native responsibilities
+ small explicit XeFG bridge
```

not:

```text
upstream/native responsibilities
+ embedded XeFG state machine
```

### Acceptance

- Release x64 build;
- `git diff --check`;
- direct-access audit clean;
- no change to D3D11/Streamline/FSRFG code paths;
- full runtime matrix completed before declaring the second-stage refactor finished.

---

# 14. PR Merge Discipline

Do not stack all seven PRs and merge them without runtime gates.

Recommended gates:

```text
2R1 + 2R2
    -> build/static gate + quick native REF smoke + DD2 XeFG smoke

2R3
    -> runtime-transition / monitor smoke

2R4 + 2R5
    -> DD2 + MHW lifecycle smoke

2R6
    -> mandatory full XeFG lifecycle matrix before merge

2R7
    -> final native + XeFG regression matrix
```

The most dangerous PR is 2R6 because it touches active bind/rebind ordering. Keep it isolated and easy to revert.

If a PR requires simultaneous changes to unrelated native D3D12 architecture to compile, stop and redesign the boundary rather than broadening scope.

---

## 15. Static / Build Validation for Every PR

Minimum required checks:

```text
1. cmkr generation when source files/build metadata change
2. x64 Release REFramework build
3. git diff --check
4. python dev/audit_direct_access_clang.py
5. inspect final diff for unrelated D3D11/Streamline/renderer changes
```

Additional source assertions should be used where useful:

- no new active `ComPtr<IDXGISwapChain3>` in `XeFGBinding` / session;
- no forced `Release()` loops;
- no new public XeFG proxy render binding;
- no new Intel private-layout offsets;
- no new OptiScaler private symbols;
- no change from non-negative XeFG result semantics;
- no timer/sleep based lifecycle recovery.

---

# 16. Runtime Validation Matrix

## 16.1 Native REFramework protection matrix

The native matrix is mandatory because preserving REFramework is the highest priority.

### A. REFramework only

```text
OptiScaler absent
XeFG runtime absent
```

Validate:

- game launch;
- REFramework overlay opens/closes;
- scripting/mod/plugin behavior used by the test title still works;
- resize/fullscreen/Alt+Tab do not regress;
- no XeFG session becomes active;
- generic D3D monitor behavior remains normal.

### B. REFramework + OptiScaler, XeFG not selected

Use a non-XeFG output where available.

Purpose:

> Prove that merely having OptiScaler installed does not cause the XeFG session to take over normal REFramework behavior.

No FSRFG/DLSSG refactor is permitted to make this pass; the expectation is that those paths remain untouched.

## 16.2 XeFG matrix

### DD2 — control/positive title

DD2 has already provided useful positive evidence, including Alt+Tab stability in prior testing.

Validate:

- cold launch;
- both overlays;
- repeated Insert/menu interaction;
- repeated Alt+Tab;
- resolution changes where available;
- fullscreen/window changes where available;
- normal gameplay/load transitions;
- clean exit.

### Monster Hunter Wilds — lifecycle stress title

MHW is the primary resize/lifecycle stress case because it carries the narrow XeFG `ResizeTarget` hold policy and has produced prior D3D/XeFG lifecycle failures.

Validate:

- cold launch;
- overlay rendering;
- Alt+Enter/fullscreen transitions;
- resolution changes;
- repeated Alt+Tab;
- resize/recreation sequences;
- extended gameplay/load transitions;
- clean exit;
- no stale hook after re-init/destroy.

### Third RE Engine XeFG title when available

Use one additional title only as a cross-game check. Do not add game-specific policy merely because the third title produces different logs unless an actual reproducible defect exists.

---

# 17. Runtime Acceptance Criteria

The second-stage refactor is complete only when all of the following remain true.

### Native / XeFG-off

- REFramework overlay remains functional;
- original game integration remains functional;
- scripting/plugins/mod behavior is not broken by the refactor;
- native D3D12 recovery behavior is unchanged;
- no XeFG session owns or mutates native state when inactive.

### XeFG-on

- OptiScaler initializes XeFG;
- REFramework overlay remains visible and functional;
- OptiScaler overlay remains functional;
- no recurring D3D12 unhook/rehook loop;
- no semantic binding/raw alias/physical hook identity divergence;
- no renderer callback during observe-only state;
- no renderer callback during active resize hold;
- no renderer callback during detached/quarantined uncertain state;
- Present/Present1 continues to be forwarded while rendering is suppressed;
- `ResizeBuffers1` maintains its required pre-reset behavior;
- MHW-only hold remains MHW-only;
- negative vendor result remains fail-closed;
- non-negative vendor result semantics remain correct;
- no COM refcount draining / force-release loop is introduced.

---

## 18. Diagnostic Requirements During the Refactor

Do not aggressively remove lifecycle logs while state ownership is moving.

The refactor should retain enough debug information to identify:

```text
runtime slot
runtime context
HWND
binding generation
semantic swapchain
raw renderer swapchain
physical hook target
selected queue
device
observe/render mode
detached state
resize event / hold state
present entry count / age
monitor action
```

The final user-facing default log policy can remain the current debug-gated arrangement.

Logging cleanup is not a goal of the second-stage refactor unless a moved log must follow its state owner.

---

## 19. Forbidden Refactor Shortcuts

Do not use any of the following to simplify the design:

1. Move all D3D12 Present handling into a generic provider abstraction.
2. Route native Present through `XeFGPresentationSession` even when XeFG is inactive.
3. Put FSRFG/DLSSG into the new session architecture.
4. Move Streamline into `compatibility/xefg`.
5. Change normal REFramework command-queue scanning because XeFG has an explicit queue.
6. Replace native raw aliases with a new global renderer context object.
7. Change hook-monitor timing while moving hook-monitor state.
8. Make the active XeFG swapchain permanently strong-owned again.
9. Add Release hooks or force-refcount cleanup.
10. Hold REF lifecycle locks across Intel vendor calls.
11. Bind to private Intel/OptiScaler layouts.
12. Combine MHW policy generalization with the structural move.
13. Remove native Present1 behavior during the behavior-preserving series.
14. Refactor unrelated awkward upstream code because the same file is already being edited.

---

# 20. Post-Refactor Optional Audit — Not Part of the Required Series

After 2R1-2R7 are complete and validated, one separate audit may be useful:

## Native Present1 spillover audit

Current fork installs `Present1[22]` in the native instance path, while the inspected upstream D3D12Hook surface does not expose Present1.

This may be historical spillover from XeFG work.

However, removing it changes current fork behavior, so it is intentionally excluded from the required second-stage refactor.

Only consider a separate PR if all of the following are true:

- upstream comparison still confirms the native path does not require it;
- XeFG has its own explicit Present1 path after the refactor;
- native runtime smoke passes both before and after the change;
- no current fork feature depends on native Present1.

Treat this as behavior restoration/audit, not refactor cleanup.

---

## 21. Expected Value and Risk

This work should not be sold as a guaranteed large immediate crash-rate reduction.

Many of the major concrete crash/lifecycle hazards have already been addressed in both forks:

- transactional XeFG binding replacement;
- `ResizeBuffers1` reset handling;
- MHW resize hold;
- borrowed active proxy semantics;
- detach before Intel re-init/destroy;
- hook-monitor quarantine;
- corrected XeFG result semantics;
- OptiScaler-side removal of aggressive COM ownership draining.

The second-stage refactor instead targets the remaining architectural failure mode:

> future code changes causing semantic binding, raw renderer aliases, physical hook target, resize state, and monitor state to diverge.

Engineering expectation, not telemetry:

```text
steady normal gameplay:
    small direct incremental benefit

lifecycle-heavy cases:
    meaningful reduction in remaining REF-origin state-split risk

future maintenance/regression prevention:
    high value
```

The refactor is worthwhile only while it remains narrow. If it expands into a generic REFramework renderer/presentation redesign, its regression risk becomes greater than its expected value.

---

# 22. Definition of Done

The second-stage refactor is done when:

1. `XeFGPresentationSession` is the single semantic owner of active XeFG presentation lifecycle state.
2. `D3D12Hook` remains the generic/native physical D3D12 mechanism owner.
3. `D3D12Hook.hpp` no longer exposes the current collection of XeFG binding/resize/detached/monitor state objects.
4. XeFG Present/resize policy is outside generic D3D12 logic except for thin physical bridge calls.
5. MHW-specific XeFG resize policy is under `compatibility/xefg`.
6. `XeFGCompatibility` evaluates session state without broad friend access to D3D12Hook internals.
7. Candidate handoff no longer treats D3D12Hook as the semantic XeFG state owner.
8. Active swapchain ownership remains borrowed, with bounded keepalive only during safe hook removal.
9. All current Intel XeFG lifecycle/result/monitor/resize semantics remain unchanged.
10. XeFG-off/native REFramework runtime validation passes.
11. OptiScaler-present-but-XeFG-off validation passes.
12. DD2 XeFG validation passes.
13. MHW XeFG lifecycle validation passes.
14. No FSRFG/DLSSG/D3D11/general renderer refactor was introduced.

Final architectural rule:

> **When XeFG is inactive, REFramework should behave like REFramework. When a validated OptiScaler + Intel XeFG presentation session is active, only the XeFG compatibility island should add the extra lifecycle behavior required to coexist safely.**
