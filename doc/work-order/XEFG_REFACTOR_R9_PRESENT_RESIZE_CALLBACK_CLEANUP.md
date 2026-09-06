# Work Order: XeFG Refactor R9 — Reduce XeFG Logic Inside Present / Resize Callbacks

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `9e64726d3cefd1e95298cf01ec0bd1d94d6ab81f` (`Refactor R8: extract XeFG resize lifecycle state (#26)`)

Relevant merged refactor baseline:

- R1 / PR #17 — exact-HMODULE XeFG runtime registry extraction
- R2 / PR #18 — loader/probe handoff isolation
- PR #19 — checked-in `CMakeLists.txt` XeFG source registration fix
- PR #20 — MHW-only XeFG `ResizeTarget` transition hold
- R3 / PR #21 — InitDesc observation + temporary factory capture extraction
- R4 / PR #22 — queue/device/HWND validation + strongly-owned candidate construction
- R5 / PR #23 — pending candidate handoff extraction
- R6 / PR #24 — active XeFG strong ownership/generation/mode/identity in `XeFGBinding`
- R7 / PR #25 — transactional initial bind + changed-object rebind
- R8 / PR #26 — resize event/hold/post-resize state in `XeFGResizeLifecycle`

Related documents:

- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_ARCHITECTURE_2026-09-06.md`
- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_PR_SPLIT_PLAN_2026-09-06.md`
- `doc/REFramework_XeFG_P3_LIFECYCLE_ROBUSTNESS_PLAN_2026-09-06.md`
- `doc/work-order/XEFG_REFACTOR_R8_RESIZE_LIFECYCLE_STATE.md`

This work order implements **R9 only** from the fine-grained XeFG refactor plan.

The fork is still **unreleased**. Internal fork-only helper names and temporary wrappers may therefore be cleaned up freely in this PR if doing so makes callback behavior easier to audit. The runtime contract remains fixed. Logging cleanup remains deferred until R11, immediately before final release validation.

---

# 1. Recommended PR Identity

Suggested branch:

```text
refactor/xefg-r9-present-resize-callbacks
```

Suggested PR title:

```text
Refactor R9: reduce XeFG logic in Present and resize callbacks
```

Suggested commit title:

```text
refactor: isolate XeFG callback decisions
```

One primary responsibility:

> Keep the physical D3D12/DXGI callback functions in `D3D12Hook`, but replace repeated low-level XeFG field/state branching inside those callbacks with narrow semantic queries and lifecycle notifications.

This PR is **not** a behavioral redesign.

The target is easier auditing:

```text
physical callback
    -> identify tracked XeFG instance
    -> ask narrow XeFG semantic decision
    -> preserve existing renderer/original-call ordering
    -> notify lifecycle transition
```

Do not start R10 hook-monitor isolation or R11 logging cleanup.

---

# 2. Release / Development Policy

The repository remains unreleased. Planned sequence after this PR:

```text
R9 callback cleanup
R10 hook-monitor / final upstream surface isolation
R11 logging levels + persistent Debug Logging UI
final runtime validation
release
```

Because this is unreleased, R9 may:

- add small private/protected `D3D12Hook` semantic helpers;
- rename/remove fork-only callback helpers that become redundant;
- collapse duplicated source + tracked-instance tests into one helper;
- remove direct callback access to `XeFGResizeLifecycle` where a semantic wrapper makes the behavior clearer;
- remove direct callback access to `XeFGBinding::observe_only()` where a render-capability query is clearer.

Still forbidden:

- changing the MHW-only policy;
- changing any hook slot;
- changing initial bind/rebind transaction ordering;
- changing strong COM ownership;
- changing original Present/Present1 call order;
- changing original ResizeTarget/ResizeBuffers/ResizeBuffers1 call order;
- changing nested Present/resize behavior;
- changing native D3D12 behavior;
- changing hook-monitor timeout/recovery behavior;
- adding new timeout/sleep/retry logic;
- changing log levels or removing diagnostics;
- generic frame-generation provider abstractions;
- FSRFG/DLSSG/Streamline redesign.

---

# 3. Current State After R8

R8 successfully moved semantic resize state into:

```text
XeFGResizeLifecycle
    event id
    last kind/time
    transition hold
    hold trigger event id
    suppressed-present count
    post-resize sample budget/ordinal
```

The remaining problem is callback readability.

`present_common()` still directly combines:

```cpp
m_swapchain_source == SwapchainSource::XeFGInternal
m_xefg_binding.observe_only()
m_xefg_resize_lifecycle.suppress_renderer()
m_xefg_resize_lifecycle.note_suppressed_present()
m_xefg_resize_lifecycle.hold_trigger_event_id()
m_xefg_resize_lifecycle.event_id()
```

and each resize callback independently reconstructs some form of:

```cpp
m_swapchain_source == SwapchainSource::XeFGInternal
&& swap_chain == m_swapchain_hook->get_instance()
```

This is now the right time to reduce those callback details because the semantic state objects already exist.

Do **not** move the physical callback functions out of `D3D12Hook`.

---

# 4. R9 Design Principle

R9 should distinguish three layers:

```text
XeFGBinding
    = active binding ownership + mode + identity

XeFGResizeLifecycle
    = resize/hold semantic state transitions

D3D12Hook physical callbacks
    = hook instance validation
    = renderer callback ordering
    = original DXGI method forwarding
    = game-specific MHW policy
    = logging context
```

The callbacks should ask semantic questions instead of reconstructing state ownership details repeatedly.

Good:

```cpp
const bool tracked_xefg = d3d12->is_tracked_xefg_instance(swap_chain);
const bool suppress_renderer = d3d12->should_suppress_xefg_render_callbacks();
```

Avoid:

```cpp
const bool suppress_renderer =
    d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
    && (d3d12->m_xefg_binding.observe_only()
        || d3d12->m_xefg_resize_lifecycle.suppress_renderer());
```

The latter reproduces internal policy in every callback.

---

# 5. Preferred File Scope

Expected modified files:

```text
MODIFY src/D3D12Hook.hpp
MODIFY src/D3D12Hook.cpp
```

Optional only if genuinely useful:

```text
MODIFY src/compatibility/xefg/XeFGResizeLifecycle.hpp
MODIFY src/compatibility/xefg/XeFGResizeLifecycle.cpp
```

Normally unchanged:

```text
CMakeLists.txt
cmake.toml
src/compatibility/xefg/XeFGBinding.*
src/compatibility/xefg/XeFGCandidateHandoff.*
src/compatibility/xefg/XeFGDiscovery.*
src/compatibility/xefg/XeFGCompatibility.*
src/compatibility/xefg/XeFGRuntimeRegistry.*
src/REFramework.cpp
src/REFramework.hpp
```

Preferred: **no new source file in R9**.

There is no need for `XeFGCallbackManager`, `XeFGPresentProvider`, or a generic event bus.

---

# 6. Recommended Narrow Helpers

Exact names may differ, but the recommended semantic surface is approximately:

```cpp
bool is_xefg_source() const noexcept;

bool is_tracked_xefg_instance(
    IDXGISwapChain3* swapchain) const noexcept;

bool is_xefg_render_capable() const noexcept;

bool is_xefg_resize_hold_active() const noexcept;

bool should_suppress_xefg_render_callbacks() const noexcept;

uint64_t begin_tracked_xefg_resize_event(
    IDXGISwapChain3* swapchain,
    XefgResizeEventKind kind,
    bool top_level);

uint32_t note_xefg_suppressed_present() noexcept;
uint64_t get_xefg_resize_hold_trigger_event_id() const noexcept;
```

Do not add all of these mechanically if a smaller set is sufficient.

A good R9 implementation usually needs only 4–6 narrow helpers.

The important thing is that callback code no longer duplicates low-level binding/lifecycle conditions.

---

# 7. `is_xefg_source()` Semantics

Recommended:

```cpp
bool D3D12Hook::is_xefg_source() const noexcept {
    return m_swapchain_source == SwapchainSource::XeFGInternal;
}
```

This is deliberately weaker than `has_active_xefg_instance_binding()`.

Do not silently replace every source check with the stronger health predicate.

Why:

- callback behavior already has separate physical-hook/tracked-instance checks;
- changing source checks to require full binding health can alter failure/fallback behavior;
- R10 will address hook-monitor health semantics separately.

R9 is a callback readability refactor, not a health-policy rewrite.

---

# 8. `is_tracked_xefg_instance()` Semantics

This helper is the most valuable R9 reduction.

Recommended:

```cpp
bool D3D12Hook::is_tracked_xefg_instance(
    IDXGISwapChain3* swapchain) const noexcept {
    return swapchain != nullptr
        && m_swapchain_source == SwapchainSource::XeFGInternal
        && m_swapchain_hook != nullptr
        && swapchain == m_swapchain_hook->get_instance();
}
```

Important: preserve current callback semantics.

Do **not** add new requirements such as:

```cpp
m_xefg_binding.active()
!m_is_phase_1
m_xefg_binding.aliases_match(...)
```

unless the current callback already requires those conditions at that point.

Those additional requirements may be valid for hook-monitor health, but that is R10.

For R9, this helper should replace the exact repeated physical tracked-instance condition that exists today.

---

# 9. Render-Capability Query

Recommended:

```cpp
bool D3D12Hook::is_xefg_render_capable() const noexcept {
    return m_swapchain_source == SwapchainSource::XeFGInternal
        && !m_xefg_binding.observe_only();
}
```

This is useful for policies such as:

```cpp
renderer_reset_performed
&& is_xefg_render_capable()
&& sdk::GameIdentity::get().is_mhwilds()
```

Do not move `GameIdentity` into `XeFGBinding` or `XeFGResizeLifecycle`.

---

# 10. Present Suppression Query

Current policy is:

```text
XeFG source + observe-only
    -> suppress REFramework renderer callbacks

XeFG source + transition hold
    -> suppress REFramework renderer callbacks

native source
    -> never suppressed by XeFG compatibility
```

Recommended helper:

```cpp
bool D3D12Hook::should_suppress_xefg_render_callbacks() const noexcept {
    return m_swapchain_source == SwapchainSource::XeFGInternal
        && (m_xefg_binding.observe_only()
            || m_xefg_resize_lifecycle.suppress_renderer());
}
```

Do not add any new reason to suppress callbacks.

In particular, do not suppress because:

- XeFG module is merely loaded;
- candidate discovery is pending;
- binding generation changed;
- Present is old/stale by timeout;
- another FG provider is detected.

---

# 11. Transition-Hold Query Must Remain Distinct From General Suppression

`observe_only` and resize hold both suppress REFramework rendering, but only the resize hold has:

- hold trigger event id;
- suppressed-present diagnostic count;
- MHW-specific lifecycle meaning.

Therefore preserve a distinct helper/query:

```cpp
bool D3D12Hook::is_xefg_resize_hold_active() const noexcept {
    return m_swapchain_source == SwapchainSource::XeFGInternal
        && m_xefg_resize_lifecycle.suppress_renderer();
}
```

Do not infer hold activity from `should_suppress_xefg_render_callbacks()`.

Otherwise observe-only mode could incorrectly increment hold diagnostics.

---

# 12. `present_common()` Target Shape

R9 should make the relevant portion conceptually look like:

```cpp
const bool xefg_hold_active = d3d12->is_xefg_resize_hold_active();
const bool suppress_render_callbacks =
    d3d12->should_suppress_xefg_render_callbacks();

const auto post_resize_ordinal = d3d12->is_xefg_source()
    ? d3d12->log_xefg_post_resize_present(
        swap_chain, kind, original_present)
    : 0;

if (xefg_hold_active) {
    const auto suppressed_present =
        d3d12->note_xefg_suppressed_present();

    // Existing first-three diagnostic behavior remains unchanged.
}

if (!suppress_render_callbacks && d3d12->m_on_present) {
    // Existing pre/post resize diagnostics remain in the same order.
    d3d12->m_on_present(*d3d12);
}

++g_present_depth;
const HRESULT result = original_call();
--g_present_depth;

if (suppress_render_callbacks) {
    g_framework->note_present_activity();
} else if (d3d12->m_on_post_present) {
    d3d12->m_on_post_present(*d3d12);
}
```

The exact implementation can differ.

The ordering may not.

---

# 13. Present Ordering — Merge-Blocking Contract

The current order must remain:

```text
tracked instance validation
-> nested Present early-forward path if applicable
-> decide XeFG render suppression
-> optional REFramework pre-render callback
-> call original Present / Present1
-> if renderer suppressed:
       note real Present activity to hook monitor
   else:
       normal post-present callback
```

Most important:

> **Suppression must never skip the original XeFG Present/Present1.**

Forbidden:

```cpp
if (suppress_render_callbacks) {
    return S_OK; // WRONG
}
```

Forbidden:

```cpp
if (suppress_render_callbacks) {
    g_framework->note_present_activity();
    return original_call(); // changes post-call/error/device-removed ordering
}
```

Keep the current common path.

---

# 14. Nested Present Behavior Is Frozen

Current nested Present behavior:

```cpp
if (g_present_depth > 0) {
    ++g_present_depth;
    const auto result = original_call();
    --g_present_depth;
    d3d12->m_inside_present = false;
    return result;
}
```

Do not route nested Present through the new semantic suppression helpers if that changes behavior.

Do not run renderer/mod callbacks in nested Present.

Do not call `note_present_activity()` as a new side effect for nested Present.

Do not alter `g_present_depth` semantics.

---

# 15. `Present` / `Present1` Entry Routing Is Frozen

Do not change the physical method selection:

```text
Present[8]
Present1[22]
```

Do not merge `present()` and `present1()` into a new hook abstraction.

Do not change the current `QueryInterface(IDXGISwapChain3)` fallback behavior in `present1()`.

Do not alter original function retrieval from `m_swapchain_hook`.

This PR only reduces XeFG policy/state branching once the physical callback has already been entered.

---

# 16. Begin-Resize Helper

Current `ResizeBuffers` and `ResizeTarget` independently compute:

```cpp
const auto is_xefg_internal =
    m_swapchain_source == SwapchainSource::XeFGInternal
    && swap_chain == m_swapchain_hook->get_instance();

const auto is_top_level = depth == 0;

const auto event_id = is_xefg_internal && is_top_level
    ? begin_xefg_resize_event(kind)
    : 0;
```

Recommended helper:

```cpp
uint64_t D3D12Hook::begin_tracked_xefg_resize_event(
    IDXGISwapChain3* swapchain,
    XefgResizeEventKind kind,
    bool top_level) {
    if (!top_level || !is_tracked_xefg_instance(swapchain)) {
        return 0;
    }

    return m_xefg_resize_lifecycle.begin(kind);
}
```

This helper must not:

- perform renderer reset;
- arm hold;
- log;
- inspect game identity;
- change nested depth;
- call original DXGI methods.

It only translates callback context into a lifecycle begin notification.

---

# 17. `ResizeBuffers` Behavior Is Frozen

Important current distinction:

`ResizeBuffers` currently runs `m_on_resize_buffers` whenever that callback exists.

It does **not** apply the same observe-only gate used by the XeFG-specific `ResizeBuffers1` path.

Do not “normalize” the two callbacks in R9.

Preserve:

```text
ResizeBuffers
    top-level tracked XeFG -> begin lifecycle event
    existing renderer callback behavior unchanged
    original ResizeBuffers called
    lifecycle completion notified after original returns
```

The completion notification must still receive the actual HRESULT.

Failed completion must still keep a hold active.

---

# 18. `ResizeBuffers1` Behavior Is Frozen

Current tracked-instance gate:

```text
not tracked XeFG instance
    -> immediately call original ResizeBuffers1
```

Preserve this exactly through `is_tracked_xefg_instance()`.

Nested behavior also remains:

```text
nested ResizeBuffers1
    -> original call only
    -> no new lifecycle event
    -> no renderer reset
```

Top-level tracked behavior:

```text
begin event
-> diagnostics
-> renderer reset only when render-capable and callback exists
-> original ResizeBuffers1
-> lifecycle completion with actual HRESULT
```

Recommended semantic reset condition:

```cpp
const bool should_reset_renderer =
    d3d12->is_xefg_render_capable()
    && static_cast<bool>(d3d12->m_on_resize_buffers);
```

Do not broaden reset to observe-only mode.

---

# 19. `ResizeTarget` Behavior Is Frozen

Top-level tracked XeFG behavior remains:

```text
begin ResizeTarget event
-> existing dimensions/logging
-> existing renderer ResizeTarget callback
-> remember whether renderer reset actually ran
-> if:
     event tracked
     renderer reset ran
     XeFG is render-capable
     game is MHW
   then arm hold
-> call original ResizeTarget
-> if original failed, clear hold
-> log original return
```

Recommended gate after R9:

```cpp
if (event_id != 0
    && renderer_reset_performed
    && d3d12->is_xefg_render_capable()
    && sdk::GameIdentity::get().is_mhwilds()) {
    d3d12->arm_xefg_resize_transition_hold(event_id);
}
```

Do not move `is_mhwilds()` into the lifecycle object.

Do not broaden MHW-only hold to all games.

---

# 20. Failed `ResizeTarget` Must Still Clear Hold

Preserve:

```cpp
if (event_id != 0 && FAILED(result)) {
    d3d12->clear_xefg_resize_transition_hold(
        "resize_target_failed");
}
```

Do not move this to a timeout or later Present.

Do not leave the hold active because `ResizeBuffers` may never follow after a failed `ResizeTarget`.

---

# 21. Successful/Failed Resize Completion Semantics Are Frozen

For `ResizeBuffers` and `ResizeBuffers1`:

```text
actual HRESULT success
    -> complete transition hold if active

actual HRESULT failure
    -> keep hold active
```

The callback may call the wrapper unconditionally when `event_id != 0`; the lifecycle object determines whether a hold exists and whether success permits completion.

Do not pre-filter success in a way that loses current logging semantics unless the resulting behavior/log content is provably identical.

---

# 22. Post-Resize Diagnostic Sampling Is Not Logging Cleanup

R8 moved the 3-sample budget into `XeFGResizeLifecycle`.

R9 may simplify the callback access, but must preserve:

```text
after each top-level tracked XeFG resize begin:
    next 3 XeFG Present/Present1 entries may consume post-resize samples
    ordinal = 1, 2, 3
```

Do not reduce/remove those diagnostics in R9.

Do not change them to debug level yet.

That is R11.

---

# 23. First Post-Resize Renderer Snapshot Ordering Is Frozen

Current behavior on post-resize ordinal 1 when rendering is not suppressed:

```text
present_pre_render_callback snapshot
-> m_on_present
-> present_post_render_callback snapshot
```

Preserve exactly.

Do not move these snapshots after original Present.

Do not emit them when renderer callbacks are suppressed if they were not emitted before.

---

# 24. Hook-Monitor Liveness Is R10, But Current Behavior Must Be Preserved

Current behavior during XeFG suppression:

```cpp
if (suppress_render_callbacks) {
    g_framework->note_present_activity();
} else if (m_on_post_present) {
    m_on_post_present(*d3d12);
}
```

R9 may make the reason for this clearer, but may not change it.

Do not:

- change hook-monitor timeout values;
- change recovery predicates;
- add a new liveness timestamp;
- remove `note_present_activity()`;
- call `note_present_activity()` before original Present;
- call both `note_present_activity()` and normal post-present callbacks when suppression is active.

R10 will isolate the monitor policy itself.

---

# 25. Device-Removed Diagnostic Path Is Frozen

Current XeFG-specific diagnostic:

```cpp
if (is_xefg_source()
    && result == DXGI_ERROR_DEVICE_REMOVED) {
    // log device removed reason
}
```

R9 may replace the raw source comparison with `is_xefg_source()`.

Do not change error handling, reset behavior, or recovery behavior here.

Logging level/content cleanup is R11.

---

# 26. Avoid Moving Logging Into Semantic State Objects

`XeFGResizeLifecycle` should remain free of D3D12/REFramework logging context.

Do not pass into it:

- swapchain pointer for logging;
- command queue;
- device;
- hook instance;
- module owner strings;
- binding generation;
- `spdlog` logger.

R9 can keep existing logging wrappers in `D3D12Hook`.

This prevents R11 from having to untangle logs from the lifecycle state machine.

---

# 27. Recommended Callback Helper Surface Example

A compact R9 header result might look like:

```cpp
bool is_xefg_source() const noexcept;
bool is_tracked_xefg_instance(IDXGISwapChain3* swapchain) const noexcept;
bool is_xefg_render_capable() const noexcept;
bool is_xefg_resize_hold_active() const noexcept;
bool should_suppress_xefg_render_callbacks() const noexcept;

uint64_t begin_tracked_xefg_resize_event(
    IDXGISwapChain3* swapchain,
    XefgResizeEventKind kind,
    bool top_level);

uint32_t note_xefg_suppressed_present() noexcept;
```

Possible inline implementations:

```cpp
bool D3D12Hook::is_xefg_source() const noexcept {
    return m_swapchain_source == SwapchainSource::XeFGInternal;
}

bool D3D12Hook::is_tracked_xefg_instance(
    IDXGISwapChain3* swapchain) const noexcept {
    return swapchain != nullptr
        && is_xefg_source()
        && m_swapchain_hook != nullptr
        && swapchain == m_swapchain_hook->get_instance();
}

bool D3D12Hook::is_xefg_render_capable() const noexcept {
    return is_xefg_source() && !m_xefg_binding.observe_only();
}

bool D3D12Hook::is_xefg_resize_hold_active() const noexcept {
    return is_xefg_source()
        && m_xefg_resize_lifecycle.suppress_renderer();
}

bool D3D12Hook::should_suppress_xefg_render_callbacks() const noexcept {
    return is_xefg_source()
        && (m_xefg_binding.observe_only()
            || m_xefg_resize_lifecycle.suppress_renderer());
}
```

This is an example, not a requirement to use exactly these names.

---

# 28. Do Not Over-Abstract

Reject designs that introduce:

```text
IFrameGenerationCallbackPolicy
IFrameGenerationPresentContext
FrameGenerationEventBus
DXGICallbackProvider
GenericResizeLifecycle
```

The scope remains specifically OptiScaler + XeFG compatibility.

Likewise, do not move native callback behavior into compatibility components merely for symmetry.

The desired result is fewer XeFG details in upstream-sensitive callback bodies, not a new framework.

---

# 29. Native D3D12 Behavior Must Remain Byte-for-Byte Equivalent in Ordering

Audit the native path carefully.

When XeFG is inactive, R9 must not change:

- phase-1 Present behavior;
- native tracked swapchain behavior;
- command queue recovery/scanning;
- Proton handling;
- Streamline handling;
- `m_on_present` timing;
- original Present timing;
- `m_on_post_present` timing;
- ResizeBuffers nested workaround;
- ResizeTarget nested workaround;
- display/render dimension updates.

A semantic helper should return false for native source and leave the existing path untouched.

---

# 30. Physical Hook Ownership Is Frozen

Do not change:

```text
D3D12Hook::present
D3D12Hook::present1
D3D12Hook::resize_buffers
D3D12Hook::resize_buffers1
D3D12Hook::resize_target
```

as hook destinations.

Do not move `VtableHook` ownership.

Do not change the five XeFG instance slots:

```text
Present[8]
ResizeBuffers[13]
ResizeTarget[14]
Present1[22]
ResizeBuffers1[39]
```

---

# 31. R7 Binding Transaction Must Not Be Reworked

R9 may call existing clear helpers from binding paths, but do not redesign:

- `apply_xefg_candidate()`;
- `bind_external_swapchain()` transaction;
- `replace_xefg_binding()` transaction;
- same-swapchain generation semantics;
- changed-swapchain hook preparation ordering.

If a helper name must be adjusted because callback semantics now use it, keep the mutation sequence unchanged.

---

# 32. R8 Lifecycle State Machine Must Not Be Reworked

Do not use R9 as an excuse to redesign:

```cpp
XeFGResizeLifecycle::begin()
XeFGResizeLifecycle::arm()
XeFGResizeLifecycle::complete()
XeFGResizeLifecycle::clear()
XeFGResizeLifecycle::consume_post_resize_present_sample()
```

Small accessor additions are acceptable.

Changing transition semantics belongs outside R9 and requires separate justification.

---

# 33. Logging Policy Is Frozen Until R11

The code currently contains extensive XeFG and D3D12 diagnostics.

R9 must **not** decide which messages are too verbose for end users.

Do not:

- convert `info` to `debug`;
- delete callstack logging;
- reduce diagnostic count;
- add the Debug Logging UI;
- change persistence settings.

R11 will handle all of that together so the release has one coherent logging policy.

---

# 34. Expected Diff Shape

Preferred changed files:

```text
src/D3D12Hook.hpp
src/D3D12Hook.cpp
```

Optional lifecycle accessor cleanup:

```text
src/compatibility/xefg/XeFGResizeLifecycle.hpp
src/compatibility/xefg/XeFGResizeLifecycle.cpp
```

Unexpected and review-sensitive changes:

```text
src/REFramework.cpp
src/REFramework.hpp
XeFGBinding.*
XeFGDiscovery.*
XeFGCandidateHandoff.*
XeFGRuntimeRegistry.*
CMakeLists.txt
cmake.toml
```

If those appear, explain why in the PR body.

---

# 35. Estimated Size

Target:

```text
effective implementation: ~120–220 LOC
GitHub add+delete:          ~220–420 LOC
```

Because the fork is unreleased, deleting redundant wrappers is encouraged.

Do not merge R10 simply because R9 ends up small.

---

# 36. Static Scenario Matrix

Before opening the PR, reason through all of these sequences.

## Scenario A — Native Present

```text
source = Native
hold = false
observe-only irrelevant
```

Expected:

```text
XeFG helpers return false
normal renderer callback runs
original Present runs
normal post-present callback runs
```

## Scenario B — XeFG render-capable, no hold

```text
source = XeFGInternal
observe_only = false
hold = false
```

Expected:

```text
render callbacks not suppressed
m_on_present runs
original Present runs
m_on_post_present runs
```

## Scenario C — XeFG observe-only

```text
source = XeFGInternal
observe_only = true
hold = false
```

Expected:

```text
REFramework renderer callbacks suppressed
original Present still runs
note_present_activity runs after original
normal post-present callback does not run
```

## Scenario D — MHW transition hold

```text
source = XeFGInternal
observe_only = false
hold = true
```

Expected:

```text
renderer callbacks suppressed
suppressed-present count increments
original Present/Present1 still runs
note_present_activity after original
```

## Scenario E — ResizeTarget in non-MHW XeFG game

Expected:

```text
resize event may be tracked
normal renderer reset behavior unchanged
hold NOT armed solely because XeFG is active
original ResizeTarget forwarded
```

## Scenario F — MHW tracked ResizeTarget

Expected:

```text
event begins
renderer ResizeTarget callback runs
if reset ran and render-capable -> hold armed
original ResizeTarget runs
failure -> hold clear
success -> hold remains until successful ResizeBuffers/1
```

## Scenario G — successful ResizeBuffers after hold

Expected:

```text
new ResizeBuffers event begins
prior hold trigger id remains original ResizeTarget event id
original ResizeBuffers succeeds
hold completes
```

## Scenario H — failed ResizeBuffers after hold

Expected:

```text
original ResizeBuffers fails
hold remains active
trigger id remains unchanged
```

## Scenario I — ResizeBuffers1 observe-only

Expected:

```text
tracked event begins
renderer reset is NOT invoked by current ResizeBuffers1-specific policy
original ResizeBuffers1 runs
completion semantics unchanged
```

## Scenario J — nested ResizeBuffers1

Expected:

```text
no new lifecycle event
no renderer reset
original called once for that nested invocation
```

## Scenario K — nested Present

Expected:

```text
no renderer callback
no new XeFG suppression side effects
original forwarded through current nested path
```

---

# 37. Merge-Blocking Review Findings

Request changes if any of these occur:

1. XeFG hold can prevent original Present/Present1 from being called.
2. `note_present_activity()` moves before original Present or disappears during suppression.
3. native Present callback ordering changes.
4. nested Present behavior changes.
5. nested ResizeBuffers/ResizeTarget/ResizeBuffers1 behavior changes.
6. MHW-only hold is broadened to other games.
7. observe-only `ResizeBuffers1` starts resetting the renderer.
8. failed ResizeBuffers/ResizeBuffers1 clears the hold.
9. failed ResizeTarget no longer clears the hold.
10. tracked-instance helper adds new health requirements that change current callback reachability.
11. hook slots or physical hook ownership change.
12. R7 bind/rebind transaction is redesigned.
13. R8 lifecycle transition semantics are redesigned.
14. hook-monitor timeout/recovery policy changes.
15. logging levels/diagnostic removal are mixed into R9.
16. generic FG/provider/event-bus infrastructure is introduced.
17. native D3D12 / Streamline / Proton behavior changes without a required reason.

---

# 38. Non-Blocking Findings

Normally do not block for:

- minor helper naming preference;
- whether a tiny helper is inline in the header or in `.cpp`;
- one harmless extra semantic accessor;
- comments that could be shorter;
- small formatting differences;
- retaining one thin compatibility wrapper until R10 if behavior is unchanged.

Do not over-review theoretical races unrelated to the changed callback semantics.

---

# 39. Mandatory Build / Static Validation

Run from latest master-based branch:

```text
cmake -S . -B build
cmake --build build --config Release --target REFramework
git diff --check
```

Also audit:

```text
no new source file unless clearly justified
no CMake change expected
no log-level changes
no GameIdentity policy expansion
no hook-slot changes
no R10 monitor-policy changes
```

Search the diff for direct callback state access.

A good R9 result should noticeably reduce occurrences inside physical callbacks of:

```text
m_swapchain_source == SwapchainSource::XeFGInternal
m_xefg_binding.observe_only()
m_xefg_resize_lifecycle.suppress_renderer()
m_xefg_resize_lifecycle.hold_trigger_event_id()
```

Some occurrences outside callbacks/inside logging wrappers are acceptable.

Do not chase zero occurrences if doing so creates artificial abstraction.

---

# 40. Mandatory Runtime Wave After R9

R9 is a planned runtime-wave boundary.

Primary test:

```text
Dragon's Dogma 2
+ REFramework R9 build
+ OptiScaler
+ Intel XeFG output
```

Verify:

```text
game launches
XeFG initializes
REFramework overlay visible
OptiScaler overlay visible
no hook-monitor rehook loop
no repeated binding churn
repeated Alt+Tab works
Present continues during suppression paths
```

MHW test when available:

```text
Monster Hunter Wilds
+ OptiScaler XeFG
```

Exercise:

```text
Alt+Enter / display-mode transition / known ResizeTarget path
```

Verify log sequence remains conceptually:

```text
ResizeTarget event
-> renderer reset
-> MHW hold arm
-> original ResizeTarget return
-> suppressed Present(s), but original Present forwarded
-> successful ResizeBuffers or ResizeBuffers1
-> hold complete
-> normal renderer callbacks resume
```

If practical, also exercise:

```text
windowed <-> borderless transition
resolution change
repeated Alt+Tab
```

Stop and fix R9 before proceeding to R10 if this wave regresses.

---

# 41. PR Description Checklist

PR body should explicitly state:

```text
- R9 only: callback semantic cleanup
- physical Present/Resize hooks remain in D3D12Hook
- native Present/Resize ordering unchanged
- original Present/Present1 always forwarded during XeFG suppression
- MHW-only hold policy unchanged
- failed completion semantics unchanged
- hook-monitor behavior unchanged
- logging levels unchanged; final logging cleanup deferred to R11
```

Include build commands and runtime test results actually performed.

Do not claim MHW runtime validation if it was not run.

---

# 42. Stop Condition

R9 is complete when the architecture reads approximately:

```text
XeFGBinding
    -> active ownership / identity / mode

XeFGResizeLifecycle
    -> resize event / hold state

D3D12Hook callbacks
    -> narrow semantic queries
    -> physical original-call forwarding
    -> renderer callback ordering
    -> MHW-only activation policy
```

At that point stop.

Do not begin R10 in the same PR.

R10 will then isolate:

```text
healthy XeFG binding preservation
hook-monitor recovery interaction
final upstream-sensitive D3D12Hook surface cleanup
```

R11 follows with final release-oriented logging/UI cleanup.
