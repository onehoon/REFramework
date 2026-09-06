# XeFG Special K Removal — P3.3B MHW Alt+Enter Resize Transition Hold

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch for implementation: `master`  
Suggested implementation branch: `fix/xefg-p3-3b-altenter-resize-transition-hold`  
Suggested PR title: `P3.3B: hold XeFG renderer across ResizeTarget transition`

---

## 1. Purpose

P2 through P2.2 established the first working Special-K-free Intel XeFG render path. P3.1 added strong ownership and complete binding identity. P3.2 added safe atomic replacement for changed XeFG internal presentation bindings. P3.3A then added detailed resize-lifecycle diagnostics for the reproducible Monster Hunter Wilds Alt+Enter failure. PR #14 added exact-HMODULE multi-runtime XeFG discovery for OptiScaler layouts containing more than one `libxess_fg.dll`.

The target topology remains intentionally narrow:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (this fork)
Special K    = absent
FG Output    = Intel XeFG
Renderer     = D3D12
Primary game = Monster Hunter Wilds
```

P3.3A runtime evidence now shows a concrete REFramework-side lifecycle hazard during MHW Alt+Enter:

```text
tracked XeFG internal ResizeTarget
    -> REFramework on_reset()
    -> REFramework releases all three D3D12 backbuffer references
    -> ResizeTarget returns successfully
    -> an intermediate Present1 arrives almost immediately
    -> REFramework reinitializes its D3D12 renderer
    -> REFramework reacquires the same three internal presentation backbuffers
    -> outer OptiScaler / XeFG / Streamline ResizeBuffers is still in progress
    -> Intel XeFG reports outstanding backbuffer references
    -> outer ResizeBuffers returns E_PENDING (0x8000000A)
    -> game reports Fatal D3D error
```

P3.3B must prevent REFramework from reacquiring D3D12 backbuffer / RTV resources during that incomplete XeFG resize transition.

The narrow functional goal is:

```text
successful top-level tracked XeFG ResizeTarget
    -> existing REFramework reset releases renderer resources
    -> arm a XeFG resize-transition render hold
    -> forward any intermediate Present / Present1 calls normally
    -> DO NOT run REFramework render callbacks while the hold is active
    -> keep hook-monitor liveness updated while rendering is suppressed
    -> keep REFramework backbuffer references released
    -> wait for a subsequent successful tracked ResizeBuffers or ResizeBuffers1
    -> release the hold only after that buffer resize succeeds
    -> allow the next Present / Present1 to recreate REFramework rendering
```

This is a lifecycle fix, not a timing workaround.

Do **not** sleep, delay the game thread, poll, force COM reference counts, or use an arbitrary timeout to guess when the transition has finished.

---

## 2. Required Base and Parallel-PR Relationship

The work order is written against current `master`:

```text
7fec56436d3fab11cd3c6e118b9299e737eb08ac
docs: add P3.3R XeFG hook-monitor preservation work order
```

The last functional code commit underneath it is:

```text
b4ff1092a16ee85a3ce4f9a017395d797a4d233d
fix: support multiple XeFG runtime modules (#14)
```

Current `master` therefore contains the same functional D3D12/XeFG code as `b4ff1092...`, plus the P3.3R work-order document.

### 2.1 P3.3B may be implemented in parallel with P3.3R

P3.3R and P3.3B deliberately solve different problems:

```text
P3.3R
    -> hook-monitor recovery defect
    -> preserve an already-active XeFG binding when Present stops

P3.3B
    -> Alt+Enter resize-transition defect
    -> prevent REF renderer reacquisition between ResizeTarget and later buffer resize
```

P3.3B must not modify:

```text
REFramework::hook_monitor()
REFramework::hook_d3d12() recovery policy
P3.3R suppression logic
hook-monitor timeout values
```

If P3.3R merges before P3.3B implementation finishes:

1. rebase P3.3B onto updated `master`,
2. preserve P3.3R exactly,
3. resolve only mechanical overlap,
4. do not fold P3.3R behavior into the P3.3B commit.

If P3.3B merges first, P3.3R should rebase and remain independent.

### 2.2 Read before editing

Read the following before implementation:

```text
doc/REFramework_OptiScaler_XeFG_SpecialK_Removal_Analysis_Plan_2026-09-05.md
doc/REFramework_XeFG_P3_LIFECYCLE_ROBUSTNESS_PLAN_2026-09-06.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P2_2_RESIZEBUFFERS1_PRE_RESET.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P3_1_BINDING_OWNERSHIP_IDENTITY.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P3_2_ATOMIC_BINDING_REPLACEMENT.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P3_3A_MHW_ALTENTER_RESIZE_LIFECYCLE_DIAGNOSTICS.md
doc/work-order/XEFG_SPECIALK_REMOVAL_MULTI_MODULE_XEFG_RUNTIME_SUPPORT.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P3_3R_HOOK_MONITOR_BINDING_PRESERVATION.md
src/D3D12Hook.hpp
src/D3D12Hook.cpp
src/REFramework.cpp
src/REFramework.hpp
```

Do not redesign P2/P2.1/P2.2/P3.1/P3.2 while implementing this work order.

---

## 3. Runtime Evidence From P3.3A + Multi-Module Test

The key MHW test session is the Intel P3.3A + multi-module run captured on 2026-09-06.

Tested REFramework commit:

```text
cebe978ed82e85027d94bdea1fede6216cc14d0d
```

OptiScaler:

```text
0.9.5-pre4 (8dac650)
```

The test reproduced the Alt+Enter failure.

### 3.1 REFramework released its own renderer references correctly at ResizeTarget

Immediately before the ResizeTarget reset, P3.3A reported:

```text
backbuffer_ref_count = 3

backbuffer_0 = 0x238ed6470
backbuffer_1 = 0x238ed84f0
backbuffer_2 = 0x238ed89d0
```

The existing `REFramework::on_reset()` path then released the renderer state.

After deinitialization:

```text
backbuffer_ref_count = 0
```

This confirms that the existing reset path can release the REFramework D3D12 backbuffer references successfully.

P3.3B must reuse this existing reset behavior.

Do not add manual `Release()` loops to REFramework-owned D3D12 resources.

### 3.2 ResizeTarget itself returned successfully

The tracked internal XeFG `ResizeTarget` returned:

```text
S_OK
```

Therefore the critical problem is not a failure of the tracked `ResizeTarget` call itself.

### 3.3 An intermediate Present1 immediately caused REF renderer reacquisition

After the reset and successful ResizeTarget return, an intermediate Present1 arrived roughly tens of milliseconds later.

P3.3A logged the pre-render snapshot with:

```text
backbuffer_ref_count = 0
```

Then `REFramework::on_present_d3d12()` / renderer initialization ran and reacquired:

```text
0x238ed6470
0x238ed84f0
0x238ed89d0
```

These are the same three internal presentation backbuffers that had just been released.

The renderer then reached its normal initialized state again before the outer XeFG resize completed.

This is the lifecycle behavior P3.3B must suppress.

### 3.4 The outer ResizeBuffers was already active while REF reacquired the resources

OptiScaler logs show the relevant outer transition beginning around:

```text
13:44:41.314xxx
```

REFramework reacquired the internal presentation backbuffers around:

```text
13:44:41.315xxx
```

The outer resize did not return until around:

```text
13:44:41.904xxx
```

Therefore REFramework renderer reacquisition occurred while the outer resize transaction was still active.

### 3.5 The outer ResizeBuffers failed with outstanding references

OptiScaler / XeFG later reported:

```text
Back buffers have outstanding references
```

and:

```text
Result: 8000000A
```

which is:

```text
E_PENDING
```

The game crash report then reported a fatal D3D error with the same HRESULT.

### 3.6 REFramework did not observe the failing outer ResizeBuffers on its tracked internal swapchain

In the failing Alt+Enter interval, REFramework observed:

```text
ResizeTarget
Present1 after resize
```

but did not observe a matching tracked:

```text
ResizeBuffers
or
ResizeBuffers1
```

before the outer call failed.

This means the failing outer resize occurs on a different wrapper / proxy / lifecycle layer than the tracked internal presentation swapchain.

This distinction matters:

```text
outer OptiScaler / XeFG / Streamline swapchain lifecycle
    !=
tracked internal post-FG presentation swapchain used by REFramework rendering
```

P3.3B must not assume that the tracked internal swapchain receives the outer ResizeBuffers entry before failure.

### 3.7 Successful earlier resize provides the expected completion model

A previous successful resize sequence in the same general environment showed:

```text
outer resize begins
    -> tracked internal ResizeBuffers1 occurs
    -> REFramework reset runs before original ResizeBuffers1
    -> internal ResizeBuffers1 succeeds
    -> outer resize succeeds
    -> later Present recreates REFramework renderer
```

This gives P3.3B a strong lifecycle completion signal:

> A subsequent successful tracked internal `ResizeBuffers` or `ResizeBuffers1` is a valid point at which REFramework may release the temporary render hold.

The hold must not be released merely because a Present arrived.

---

## 4. Current Master Code Facts

P3.3B must be implemented around the existing code, not by replacing it.

### 4.1 Active XeFG internal binding uses instance hooks

Current active XeFG binding installs:

```text
Present[8]
Present1[22]
ResizeBuffers[13]
ResizeTarget[14]
ResizeBuffers1[39]
```

The active object is the validated internal / post-FG presentation swapchain captured during `xefgSwapChainD3D12InitFromSwapChainDesc`.

Do not move REFramework rendering to the public XeFG proxy.

### 4.2 `present_common()` already has a proven callback-suppression pattern

Current code computes:

```cpp
const auto suppress_render_callbacks =
    d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
    && d3d12->m_xefg_p21_observe_only;
```

When suppression is active, it does not execute REFramework renderer callbacks and instead calls:

```cpp
g_framework->note_present_activity();
```

This preserves hook-monitor liveness while still forwarding the original Present / Present1.

P3.3B should extend this existing pattern rather than inventing a second presentation path.

### 4.3 `ResizeTarget` already has top-level XeFG event identity

Current `resize_target()` determines:

```cpp
const auto is_xefg_internal =
    d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
    && swap_chain == d3d12->m_swapchain_hook->get_instance();

const auto is_top_level = g_resize_target_depth == 0;

const auto event_id = is_xefg_internal && is_top_level
    ? d3d12->begin_xefg_resize_event(XefgResizeEventKind::ResizeTarget)
    : 0;
```

This is the correct authority for starting the hold.

Do not introduce another swapchain identity heuristic.

### 4.4 `ResizeTarget` already resets REFramework before calling the original method

Current code performs:

```text
ResizeTarget entry
    -> P3.3A pre-reset diagnostics
    -> m_on_resize_target(...)
    -> REFramework::on_reset()
    -> P3.3A post-reset diagnostics
    -> original ResizeTarget
```

P3.3B must not add another reset at `ResizeTarget`.

It should only prevent renderer reacquisition after the existing reset.

### 4.5 `ResizeBuffers1` already uses the proven P2.2 pre-reset ordering

Current tracked XeFG internal `ResizeBuffers1` performs:

```text
enter
    -> optional existing renderer reset
    -> original ResizeBuffers1
    -> original_return
```

P3.3B must preserve this exact ordering.

A successful original return is a valid hold-completion boundary.

### 4.6 All relevant callbacks run under the same lifecycle mutex

`present_common()`, `resize_target()`, `resize_buffers()`, `resize_buffers1()`, binding replacement, and hook-monitor lifecycle work are serialized with:

```cpp
g_framework->get_hook_monitor_mutex()
```

The mutex is recursive in the current architecture.

P3.3B must reuse this serialization.

Do not add a new mutex for the hold state.

---

## 5. Root Cause Model for This PR

P3.3B is based on the following concrete model.

### Current failing model

```text
MHW Alt+Enter
    |
    v
tracked internal ResizeTarget
    |
    +--> REF on_reset()
    |      backbuffers = 0
    |
    +--> original ResizeTarget = S_OK
    |
    v
intermediate Present1
    |
    +--> REF present callback runs
    +--> renderer initializes
    +--> same internal backbuffers reacquired
    |
    v
outer XeFG ResizeBuffers still active
    |
    +--> Intel XeFG detects outstanding refs
    |
    v
E_PENDING
    |
    v
Fatal D3D error
```

### Desired P3.3B model

```text
MHW Alt+Enter
    |
    v
tracked internal ResizeTarget
    |
    +--> REF on_reset()
    |      backbuffers = 0
    |
    +--> arm resize-transition hold
    |
    +--> original ResizeTarget
    |
    v
intermediate Present / Present1
    |
    +--> original Present is still called
    +--> REF render callback is suppressed
    +--> REF backbuffers remain 0
    +--> hook monitor activity is refreshed
    |
    v
outer XeFG resize continues
    |
    v
tracked internal ResizeBuffers / ResizeBuffers1
    |
    +--> existing reset path remains valid
    +--> original buffer resize succeeds
    |
    +--> complete resize-transition hold
    |
    v
next Present / Present1
    |
    +--> REF renderer initializes normally
    +--> new/current backbuffers acquired only after resize completion
```

---

## 6. Core Design Decision — Scoped XeFG Resize-Transition Render Hold

P3.3B should add a small state machine owned by `D3D12Hook`.

The state is not a general fullscreen state machine.

It has only two semantic states:

```text
Idle
HeldAfterResizeTarget
```

### Enter hold

Enter only when all of the following are true:

```text
source == XeFGInternal
tracked instance swapchain
ResizeTarget is top-level
existing REFramework reset callback has run
render mode is not observe-only
```

Arm the hold before calling the original top-level ResizeTarget so that any reentrant Present inside the original call is also protected.

If the original ResizeTarget fails, clear the hold immediately.

If the original ResizeTarget succeeds, keep the hold active.

### While held

For tracked XeFG Present / Present1:

```text
forward original presentation call
skip m_on_present
skip m_on_post_present
call note_present_activity()
keep renderer resources released
```

This is intentionally the same callback-suppression semantics already used by P2.1 observe-only mode.

### Complete hold

Complete only when a later top-level tracked internal:

```text
ResizeBuffers
or
ResizeBuffers1
```

returns success.

Do not complete on:

```text
Present
Present1
ResizeTarget return
elapsed time
number of presented frames
window messages
Alt+Enter key state
fullscreen flag polling
```

### Abort / reset hold

Clear stale hold state when the D3D12/XeFG binding lifecycle itself is replaced or destroyed:

```text
successful XeFG binding replacement
new external XeFG binding commit
D3D12Hook::unhook()
```

This prevents a hold from leaking across a genuinely new binding generation.

---

## 7. Required Change A — Add Explicit Hold State to `D3D12Hook`

Expected file:

```text
src/D3D12Hook.hpp
```

Add narrowly-scoped state.

Recommended fields:

```cpp
bool m_xefg_resize_transition_hold{ false };
uint64_t m_xefg_resize_transition_hold_event_id{ 0 };
uint32_t m_xefg_resize_transition_suppressed_present_count{ 0 };
```

Recommended helpers:

```cpp
void arm_xefg_resize_transition_hold(uint64_t event_id);
void complete_xefg_resize_transition_hold(
    uint64_t completion_event_id,
    XefgResizeEventKind completion_kind,
    HRESULT result);
void clear_xefg_resize_transition_hold(const char* reason);
```

A small getter is acceptable for diagnostics/tests:

```cpp
bool is_xefg_resize_transition_hold_active() const noexcept {
    return m_xefg_resize_transition_hold;
}
```

Do not make these globals.

The hold belongs to the active `D3D12Hook` / binding lifecycle.

### 7.1 Do not use atomics for this state unless code inspection proves necessary

The relevant mutation points are already serialized under `hook_monitor_mutex`.

Avoid adding atomics that imply cross-thread lock-free semantics the surrounding code does not use.

---

## 8. Required Change B — Implement Bounded Hold Diagnostics

Expected file:

```text
src/D3D12Hook.cpp
```

Add machine-readable logging such as:

```text
[XeFG][ResizeHold] action = arm, trigger_event_id = 9, binding_generation = 1
[XeFG][ResizeHold] action = suppress_present, trigger_event_id = 9, suppressed_present = 1, kind = Present1
[XeFG][ResizeHold] action = complete, trigger_event_id = 9, completion_event_id = 10, completion_kind = ResizeBuffers1, result = 0x00000000, suppressed_presents = 2
[XeFG][ResizeHold] action = clear, reason = resize_target_failed
[XeFG][ResizeHold] action = clear, reason = binding_replaced
```

Do not log every held Present forever.

Recommended bounded policy:

```text
log suppress_present for first 3 held Presents only
always log arm
always log complete
always log explicit clear/abort
```

The total suppressed-present count should still be included in the completion log.

### 8.1 Example helper implementation

Illustrative only; adapt to current style:

```cpp
void D3D12Hook::arm_xefg_resize_transition_hold(uint64_t event_id) {
    if (m_swapchain_source != SwapchainSource::XeFGInternal
        || m_xefg_p21_observe_only
        || event_id == 0) {
        return;
    }

    m_xefg_resize_transition_hold = true;
    m_xefg_resize_transition_hold_event_id = event_id;
    m_xefg_resize_transition_suppressed_present_count = 0;

    spdlog::info(
        "[XeFG][ResizeHold] action = arm, trigger_event_id = {}, "
        "binding_generation = {}, swapchain = 0x{:x}",
        event_id,
        m_xefg_binding_generation,
        reinterpret_cast<uintptr_t>(m_swap_chain));
}
```

Recommended clear helper:

```cpp
void D3D12Hook::clear_xefg_resize_transition_hold(const char* reason) {
    if (!m_xefg_resize_transition_hold) {
        return;
    }

    spdlog::info(
        "[XeFG][ResizeHold] action = clear, reason = {}, "
        "trigger_event_id = {}, suppressed_presents = {}, generation = {}",
        reason != nullptr ? reason : "unknown",
        m_xefg_resize_transition_hold_event_id,
        m_xefg_resize_transition_suppressed_present_count,
        m_xefg_binding_generation);

    m_xefg_resize_transition_hold = false;
    m_xefg_resize_transition_hold_event_id = 0;
    m_xefg_resize_transition_suppressed_present_count = 0;
}
```

Recommended completion helper:

```cpp
void D3D12Hook::complete_xefg_resize_transition_hold(
    uint64_t completion_event_id,
    XefgResizeEventKind completion_kind,
    HRESULT result) {

    if (!m_xefg_resize_transition_hold) {
        return;
    }

    if (FAILED(result)) {
        spdlog::info(
            "[XeFG][ResizeHold] action = keep, reason = completion_failed, "
            "trigger_event_id = {}, completion_event_id = {}, completion_kind = {}, "
            "result = 0x{:08x}",
            m_xefg_resize_transition_hold_event_id,
            completion_event_id,
            resize_kind_name(completion_kind),
            static_cast<uint32_t>(result));
        return;
    }

    spdlog::info(
        "[XeFG][ResizeHold] action = complete, trigger_event_id = {}, "
        "completion_event_id = {}, completion_kind = {}, result = 0x{:08x}, "
        "suppressed_presents = {}, generation = {}",
        m_xefg_resize_transition_hold_event_id,
        completion_event_id,
        resize_kind_name(completion_kind),
        static_cast<uint32_t>(result),
        m_xefg_resize_transition_suppressed_present_count,
        m_xefg_binding_generation);

    m_xefg_resize_transition_hold = false;
    m_xefg_resize_transition_hold_event_id = 0;
    m_xefg_resize_transition_suppressed_present_count = 0;
}
```

If `resize_kind_name()` visibility makes the helper awkward, keep the naming local and simple.

Do not broaden this into a generic state-machine framework.

---

## 9. Required Change C — Arm the Hold After the Existing ResizeTarget Reset and Before Original ResizeTarget

Expected function:

```cpp
HRESULT WINAPI D3D12Hook::resize_target(...)
```

Current top-level tracked XeFG path already computes `event_id` and already invokes the reset callback.

Preserve all existing P3.3A logging.

### 9.1 Track whether the existing renderer reset path actually ran

Recommended shape:

```cpp
bool renderer_reset_performed = false;

if (d3d12->m_on_resize_target) {
    // existing P3.3A pre-reset logging
    d3d12->m_on_resize_target(*d3d12);
    renderer_reset_performed = true;
    // existing P3.3A post-reset logging
}
```

Do not call `on_reset()` a second time.

### 9.2 Arm only for top-level tracked XeFG render mode

After the existing reset block and before calling the original ResizeTarget:

```cpp
if (event_id != 0
    && renderer_reset_performed
    && !d3d12->m_xefg_p21_observe_only) {
    d3d12->arm_xefg_resize_transition_hold(event_id);
}
```

### 9.3 Why arm before the original call

The observed failing MHW session had the intermediate Present after ResizeTarget returned, but the implementation should also protect against a reentrant Present originating inside the original ResizeTarget call.

Because the lifecycle mutex is recursive, same-thread reentrancy is possible in principle.

Therefore the safe order is:

```text
existing REF reset complete
    -> hold armed
    -> original ResizeTarget invoked
```

### 9.4 Clear the hold if ResizeTarget itself fails

After original return:

```cpp
if (event_id != 0 && FAILED(result)) {
    d3d12->clear_xefg_resize_transition_hold("resize_target_failed");
}
```

A failed ResizeTarget is not evidence that the expected buffer-resize completion sequence will follow.

Do not leave a permanent hold solely because the initial trigger itself failed.

### 9.5 Do not release the hold on successful ResizeTarget return

This is critical.

The P3.3A failure happened specifically because successful ResizeTarget was followed by an intermediate Present before the outer buffer resize completed.

Therefore this is wrong:

```cpp
if (SUCCEEDED(result)) {
    clear_hold(); // WRONG
}
```

Successful ResizeTarget means:

```text
keep hold active
```

until a later successful tracked buffer resize completes.

---

## 10. Required Change D — Extend `present_common()` Suppression to Include the Resize Hold

Expected function:

```cpp
HRESULT D3D12Hook::present_common(...)
```

Current code:

```cpp
const auto suppress_render_callbacks =
    d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
    && d3d12->m_xefg_p21_observe_only;
```

Change the semantic condition to:

```cpp
const auto xefg_resize_transition_hold =
    d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
    && d3d12->m_xefg_resize_transition_hold;

const auto suppress_render_callbacks =
    d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
    && (d3d12->m_xefg_p21_observe_only || xefg_resize_transition_hold);
```

Do not create a separate Present function for the hold.

### 10.1 Original Present / Present1 must still run

While held:

```text
original_call() must still execute
```

P3.3B is preventing REFramework renderer resource reacquisition, not freezing the XeFG presentation pipeline.

Do not return `S_OK` without forwarding the call.

Do not use `ignore_next_present()`.

### 10.2 Skip both render callbacks while held

The existing suppression path skips:

```text
m_on_present
m_on_post_present
```

Keep that behavior.

This is deliberate because both callbacks may touch renderer / GPU / mod state that assumes valid backbuffers.

P3.3B should mirror observe-only suppression rather than attempting to split callbacks into guessed safe/unsafe subsets.

### 10.3 Keep hook-monitor liveness alive

At the end of the suppressed Present path, preserve the existing behavior:

```cpp
g_framework->note_present_activity();
```

This is important for both branches:

```text
without P3.3R merged
    -> held intermediate Presents must not make hook monitor think presentation is dead

with P3.3R merged
    -> the same liveness update remains correct and harmless
```

### 10.4 Add bounded held-Present diagnostics

Illustrative code:

```cpp
if (xefg_resize_transition_hold) {
    const auto suppressed =
        ++d3d12->m_xefg_resize_transition_suppressed_present_count;

    if (suppressed <= 3) {
        spdlog::info(
            "[XeFG][ResizeHold] action = suppress_present, trigger_event_id = {}, "
            "suppressed_present = {}, kind = {}, present_call = {}",
            d3d12->m_xefg_resize_transition_hold_event_id,
            suppressed,
            kind,
            present_call);
    }
}
```

Do not log every frame if the hold persists unexpectedly.

### 10.5 Preserve P3.3A post-resize diagnostics

Do not remove:

```text
log_xefg_post_resize_present(...)
present_pre_render_callback snapshot
present_post_render_callback snapshot
```

For a held Present, `present_pre_render_callback` / `present_post_render_callback` should naturally not appear because the renderer callback is intentionally suppressed.

The existing `present_after_resize` lifecycle log may still appear and is useful.

---

## 11. Required Change E — Complete the Hold Only After Successful Tracked `ResizeBuffers`

Expected function:

```cpp
HRESULT WINAPI D3D12Hook::resize_buffers(...)
```

Keep the current:

```text
P3.3A event creation
existing pre-reset callback
original call
original_return log
```

After the original top-level tracked XeFG ResizeBuffers returns and after the existing `original_return` log:

```cpp
if (event_id != 0) {
    d3d12->complete_xefg_resize_transition_hold(
        event_id,
        XefgResizeEventKind::ResizeBuffers,
        result);
}
```

The helper must leave the hold active on failure.

### 11.1 Why successful buffer resize is authoritative

At that point DXGI/XeFG has successfully completed the operation that requires old backbuffer references to be released.

Only then may REFramework safely permit the next Present to reacquire buffers.

### 11.2 Do not complete before calling the original ResizeBuffers

This is wrong:

```text
ResizeBuffers entry
    -> clear hold
    -> REF could reacquire on reentrant Present
    -> original ResizeBuffers
```

Required ordering:

```text
ResizeBuffers entry
    -> keep hold
    -> existing reset
    -> original ResizeBuffers
    -> if success: complete hold
```

---

## 12. Required Change F — Complete the Hold Only After Successful Tracked `ResizeBuffers1`

Expected function:

```cpp
HRESULT WINAPI D3D12Hook::resize_buffers1(...)
```

Preserve the P2.2 ordering exactly:

```text
ResizeBuffers1 entry
    -> pre-reset
    -> original ResizeBuffers1
    -> original_return
```

After `original_return` is known:

```cpp
d3d12->complete_xefg_resize_transition_hold(
    event_id,
    XefgResizeEventKind::ResizeBuffers1,
    result);
```

### 12.1 Do not change P2.2 reset policy

During a hold, REFramework renderer resources should already be released.

The existing `m_on_resize_buffers` reset may therefore be redundant in some transitions, but P3.3B must not optimize or coalesce it.

This PR is about preventing premature reacquisition.

Reset coalescing is explicitly out of scope.

### 12.2 Failed ResizeBuffers1 keeps the hold

If:

```text
FAILED(result)
```

then:

```text
hold remains active
```

Do not reacquire renderer resources after a failed buffer resize.

---

## 13. Required Change G — Clear Hold on Binding Replacement / Rebind / Unhook

A hold is meaningful only for the active binding generation that triggered it.

Do not let it leak into a new binding generation.

### 13.1 Same-object P3.2 rebind

In the successful same-object branch of:

```cpp
D3D12Hook::replace_xefg_binding(...)
```

clear stale hold after the new queue/device/mode state is committed.

Recommended:

```cpp
clear_xefg_resize_transition_hold("binding_replaced");
```

Place it before returning success.

### 13.2 Different-object P3.2 rebind

After the new swapchain / queue / device / hook is committed successfully:

```cpp
clear_xefg_resize_transition_hold("binding_replaced");
```

Do not clear before provisional new binding preparation succeeds.

If rebind fails and old binding remains active, keep the old hold state associated with that old binding.

### 13.3 External XeFG bind

When `bind_external_swapchain()` successfully commits a new XeFG binding, ensure stale hold state is reset.

Initial bind naturally starts with no hold.

If the same object has somehow carried state through a lifecycle path, the bind should still establish:

```text
hold = false
trigger_event_id = 0
suppressed_present_count = 0
```

### 13.4 Unhook

`D3D12Hook::unhook()` must clear the state.

This can be a direct reset without a diagnostic if logging during destructor/unhook would be noisy, or via the helper if consistent.

Required final invariant:

```text
unhooked D3D12Hook cannot retain active resize-transition hold state
```

---

## 14. Important Non-Goal — Do Not Add a Time-Based Fail-Open in P3.3B

Do not implement:

```cpp
if (hold_elapsed > 500ms) clear_hold();
```

or:

```cpp
if (hold_elapsed > 1s) clear_hold();
```

or:

```cpp
if (suppressed_present_count >= N) clear_hold();
```

The failing P3.3A outer ResizeBuffers itself took roughly 590 ms.

A 500 ms fallback would have been capable of reacquiring REF backbuffers while the failing outer resize was still executing.

More importantly, any fixed timeout would replace an identified lifecycle race with another timing race.

### 14.1 What to do if no completion resize is observed

If runtime testing shows:

```text
hold arms
intermediate Presents are suppressed
E_PENDING disappears
but no tracked ResizeBuffers/ResizeBuffers1 completion ever occurs
```

then classify the result as:

```text
partial success: crash prevention works, completion boundary incomplete
```

Do not add an arbitrary timeout in the same PR just to restore the overlay.

Capture the logs and design the next exact lifecycle boundary from evidence.

A later task may need to observe the outer/public XeFG resize boundary directly.

That is intentionally not part of the initial P3.3B implementation.

---

## 15. Important Non-Goal — Do Not Hook the Public XeFG Proxy in This PR

P3.3A shows that the failing outer ResizeBuffers is outside REFramework's tracked internal swapchain.

It is tempting to immediately add another vtable hook to the public `XefgInterpolationSwapChain` / OptiScaler-visible wrapper.

Do not do that in P3.3B.

Reasons:

1. P3.3B already has a minimally invasive fix candidate supported by runtime evidence.
2. The public proxy may participate in OptiScaler Detours / Streamline / Intel wrapper chains.
3. Adding a second lifecycle hook expands hook-order and restoration risk substantially.
4. P3.3B should first test whether keeping REF resources released allows the existing internal ResizeBuffers1 completion path to proceed naturally.

If the hold prevents E_PENDING but never receives a completion boundary, then a later PR can add lifecycle-only observation of the public proxy based on that evidence.

Do not render on the public proxy.

---

## 16. Important Non-Goal — Do Not Force COM Reference Counts

Never copy OptiScaler's emergency backbuffer-release loop into REFramework.

Do not do:

```cpp
while (ref_count > expected) {
    resource->Release();
}
```

REFramework must release only the references it owns through its existing renderer teardown.

P3.3A already proved that:

```text
REFramework reset -> backbuffer_ref_count = 0
```

for REFramework-owned renderer references.

The bug is reacquisition timing, not inability to release its own resources.

---

## 17. Important Non-Goal — Do Not Add Sleeps or GPU Waits

Do not add:

```text
Sleep(...)
WaitForSingleObject(...)
manual fence waits
busy loops
Present polling
fullscreen polling
```

P3.3B should not stall MHW or XeFG.

It should simply refrain from REFramework renderer work during the unsafe interval.

---

## 18. Important Non-Goal — Do Not Modify Hook Monitor Behavior

Do not modify:

```text
REFramework::hook_monitor()
5-second Present timeout
1-second last-chance timeout
hook_d3d12 recovery
P3.3R logic if already merged
```

P3.3R owns recovery semantics.

P3.3B should call the already-existing:

```cpp
note_present_activity()
```

while it intentionally suppresses render callbacks.

This prevents a legitimate held Present stream from being misclassified as dead presentation.

---

## 19. Important Non-Goal — Do Not Change P3.2 Binding Replacement

Do not redesign:

```text
BindingGate
binding identity
same-object rebind
new-object provisional VtableHook preparation
binding generation semantics
strong swapchain/queue/device ownership
```

P3.3B may only clear hold state after an already-successful binding commit.

The P3.2 transaction itself must remain structurally unchanged.

---

## 20. Important Non-Goal — Do Not Change Multi-Module Runtime Registry

Do not modify:

```text
kMaxXefgRuntimes
slot-specific InitDesc thunks
GetSwapChainPtr thunks
exact-HMODULE registry
module enumeration
loader notification handling
```

PR #14 is unrelated to the Alt+Enter resource-reacquisition race.

---

## 21. Detailed Example Patch Shape

This section is illustrative, not a blind patch.

Adapt it to current `master` after rebasing.

### 21.1 `D3D12Hook.hpp`

Conceptual addition:

```cpp
protected:
    ...

    void arm_xefg_resize_transition_hold(uint64_t event_id);
    void complete_xefg_resize_transition_hold(
        uint64_t completion_event_id,
        XefgResizeEventKind completion_kind,
        HRESULT result);
    void clear_xefg_resize_transition_hold(const char* reason);

    ...

    uint64_t m_xefg_resize_event_id{ 0 };
    XefgResizeEventKind m_xefg_last_resize_kind{ XefgResizeEventKind::None };
    std::chrono::steady_clock::time_point m_xefg_last_resize_event_time{};
    uint32_t m_xefg_post_resize_present_budget{ 0 };
    uint32_t m_xefg_post_resize_present_ordinal{ 0 };

    bool m_xefg_resize_transition_hold{ false };
    uint64_t m_xefg_resize_transition_hold_event_id{ 0 };
    uint32_t m_xefg_resize_transition_suppressed_present_count{ 0 };
```

Do not expose the fields publicly unless needed by diagnostics/tests.

### 21.2 helper implementations

Conceptual:

```cpp
void D3D12Hook::arm_xefg_resize_transition_hold(uint64_t event_id) {
    if (event_id == 0
        || m_swapchain_source != SwapchainSource::XeFGInternal
        || m_xefg_p21_observe_only) {
        return;
    }

    m_xefg_resize_transition_hold = true;
    m_xefg_resize_transition_hold_event_id = event_id;
    m_xefg_resize_transition_suppressed_present_count = 0;

    spdlog::info(
        "[XeFG][ResizeHold] action = arm, trigger_event_id = {}, "
        "generation = {}, swapchain = 0x{:x}",
        event_id,
        m_xefg_binding_generation,
        reinterpret_cast<uintptr_t>(m_swap_chain));
}
```

Conceptual completion:

```cpp
void D3D12Hook::complete_xefg_resize_transition_hold(
    uint64_t completion_event_id,
    XefgResizeEventKind completion_kind,
    HRESULT result) {

    if (!m_xefg_resize_transition_hold) {
        return;
    }

    if (FAILED(result)) {
        spdlog::info(
            "[XeFG][ResizeHold] action = keep, reason = buffer_resize_failed, "
            "trigger_event_id = {}, completion_event_id = {}, completion_kind = {}, "
            "result = 0x{:08x}",
            m_xefg_resize_transition_hold_event_id,
            completion_event_id,
            resize_kind_name(completion_kind),
            static_cast<uint32_t>(result));
        return;
    }

    const auto trigger_event_id = m_xefg_resize_transition_hold_event_id;
    const auto suppressed_presents = m_xefg_resize_transition_suppressed_present_count;

    m_xefg_resize_transition_hold = false;
    m_xefg_resize_transition_hold_event_id = 0;
    m_xefg_resize_transition_suppressed_present_count = 0;

    spdlog::info(
        "[XeFG][ResizeHold] action = complete, trigger_event_id = {}, "
        "completion_event_id = {}, completion_kind = {}, result = 0x{:08x}, "
        "suppressed_presents = {}, generation = {}",
        trigger_event_id,
        completion_event_id,
        resize_kind_name(completion_kind),
        static_cast<uint32_t>(result),
        suppressed_presents,
        m_xefg_binding_generation);
}
```

### 21.3 `resize_target()`

Conceptual integration:

```cpp
bool renderer_reset_performed = false;

if (d3d12->m_on_resize_target) {
    if (event_id != 0) {
        d3d12->log_xefg_resize_event(..., "pre_reset", ...);
        g_framework->log_d3d12_resize_snapshot(
            "resize_target_pre_reset", event_id);
    }

    d3d12->m_on_resize_target(*d3d12);
    renderer_reset_performed = true;

    if (event_id != 0) {
        d3d12->log_xefg_resize_event(..., "post_reset", ...);
        g_framework->log_d3d12_resize_snapshot(
            "resize_target_post_reset", event_id);
    }
}

if (event_id != 0
    && renderer_reset_performed
    && !d3d12->m_xefg_p21_observe_only) {
    d3d12->arm_xefg_resize_transition_hold(event_id);
}

++g_resize_target_depth;
const auto result = resize_target_fn(swap_chain, new_target_parameters);
--g_resize_target_depth;

if (event_id != 0 && FAILED(result)) {
    d3d12->clear_xefg_resize_transition_hold("resize_target_failed");
}
```

Keep the existing error and P3.3A `original_return` logging around this.

### 21.4 `present_common()`

Conceptual integration:

```cpp
const auto xefg_internal =
    d3d12->m_swapchain_source == SwapchainSource::XeFGInternal;

const auto resize_transition_hold =
    xefg_internal && d3d12->m_xefg_resize_transition_hold;

const auto suppress_render_callbacks =
    xefg_internal
    && (d3d12->m_xefg_p21_observe_only || resize_transition_hold);

if (resize_transition_hold) {
    const auto suppressed =
        ++d3d12->m_xefg_resize_transition_suppressed_present_count;

    if (suppressed <= 3) {
        spdlog::info(
            "[XeFG][ResizeHold] action = suppress_present, "
            "trigger_event_id = {}, suppressed_present = {}, kind = {}, "
            "present_call = {}",
            d3d12->m_xefg_resize_transition_hold_event_id,
            suppressed,
            kind,
            present_call);
    }
}
```

Then preserve the existing callback logic:

```cpp
if (!suppress_render_callbacks && d3d12->m_on_present) {
    ...
}

++g_present_depth;
const auto result = original_call();
--g_present_depth;

if (suppress_render_callbacks) {
    g_framework->note_present_activity();
} else if (d3d12->m_on_post_present) {
    d3d12->m_on_post_present(*d3d12);
}
```

Do not change original-call ordering.

### 21.5 `resize_buffers()` completion

Conceptual:

```cpp
const auto result = resize_buffers_fn(...);

...

if (event_id != 0) {
    d3d12->log_xefg_resize_event(
        event_id,
        XefgResizeEventKind::ResizeBuffers,
        "original_return",
        swap_chain,
        resize_buffers_original,
        result,
        true);

    d3d12->complete_xefg_resize_transition_hold(
        event_id,
        XefgResizeEventKind::ResizeBuffers,
        result);
}
```

### 21.6 `resize_buffers1()` completion

Conceptual:

```cpp
const auto result = original(...);

spdlog::info(
    "[XeFG][ResizeBuffers1] stage = original_return, result = 0x{:08x}",
    static_cast<uint32_t>(result));

d3d12->log_xefg_resize_event(
    event_id,
    XefgResizeEventKind::ResizeBuffers1,
    "original_return",
    swap_chain,
    resize_buffers1_original,
    result,
    true);

d3d12->complete_xefg_resize_transition_hold(
    event_id,
    XefgResizeEventKind::ResizeBuffers1,
    result);
```

### 21.7 binding lifecycle cleanup

Same-object successful rebind:

```cpp
...
++m_xefg_binding_generation;
clear_xefg_resize_transition_hold("binding_replaced");
return true;
```

Different-object successful rebind:

```cpp
...
++m_xefg_binding_generation;
clear_xefg_resize_transition_hold("binding_replaced");
return true;
```

Unhook:

```cpp
m_xefg_resize_transition_hold = false;
m_xefg_resize_transition_hold_event_id = 0;
m_xefg_resize_transition_suppressed_present_count = 0;
```

Avoid calling a logging helper after dependent hook state has already been destroyed.

---

## 22. Required Invariants

The implementation must preserve all of the following.

### Invariant 1 — REF owns no renderer backbuffers while hold is active

After the existing ResizeTarget reset and throughout held Presents:

```text
REFramework D3D12 backbuffer references should remain 0
```

P3.3A diagnostics should demonstrate this.

### Invariant 2 — Present pipeline remains live

Held Present / Present1 must still call the original function.

No fake success.

No skipped XeFG Present.

### Invariant 3 — Hook monitor sees intentional Present activity

Held Present / Present1 must continue to refresh REFramework present activity via the existing suppression path.

### Invariant 4 — Hold is XeFG-internal only

Native D3D12 behavior must remain unchanged.

FSRFG behavior must remain unchanged.

### Invariant 5 — Observe-only remains observe-only

P2.1 observe-only semantics remain unchanged.

Resize hold is an additional suppression reason, not a replacement for observe-only mode.

### Invariant 6 — A failed buffer resize never releases the hold

If the completion resize returns failure:

```text
hold remains active
```

### Invariant 7 — Successful tracked buffer resize releases the hold before the next Present

Completion occurs after original buffer resize success while holding the lifecycle mutex.

The next Present can then recreate renderer state safely.

### Invariant 8 — New binding generation cannot inherit stale hold

Successful binding commit or unhook clears the state.

### Invariant 9 — No arbitrary timing policy

No timeout or frame-count completion.

---

## 23. Concurrency / Reentrancy Requirements

Do not underestimate reentrancy in DXGI wrapper stacks.

### 23.1 Keep using `hook_monitor_mutex`

All hold-state transitions happen inside code already holding the lifecycle mutex.

Do not add another lock.

### 23.2 Arm before original ResizeTarget

This protects against same-thread reentrant Present inside the original ResizeTarget call.

### 23.3 Complete only after original buffer resize returns

This protects against reentrant Present while a buffer resize is still executing.

### 23.4 Nested resize calls

Existing depth guards remain authoritative:

```text
g_resize_target_depth
g_resize_buffers_depth
g_resize_buffers1_depth
```

Only top-level tracked events with nonzero P3.3A `event_id` should arm or complete the hold.

Do not let nested wrapper recursion independently toggle hold state.

---

## 24. Diagnostics Required for Runtime Validation

Keep P3.3A diagnostics enabled for this PR.

Required new P3.3B lines:

```text
[XeFG][ResizeHold] action = arm
[XeFG][ResizeHold] action = suppress_present
[XeFG][ResizeHold] action = complete
```

or on failure paths:

```text
[XeFG][ResizeHold] action = keep, reason = buffer_resize_failed
[XeFG][ResizeHold] action = clear, reason = ...
```

### 24.1 Expected successful MHW Alt+Enter log pattern

Desired:

```text
[XeFG][ResizeLifecycle] kind = ResizeTarget stage = pre_reset
[D3D12][ResizeState] ... backbuffer_ref_count = 3

Reset!

[D3D12][ResizeState] ... backbuffer_ref_count = 0
[XeFG][ResizeHold] action = arm

[XeFG][ResizeLifecycle] kind = Present1 stage = present_after_resize
[XeFG][ResizeHold] action = suppress_present

# No renderer_acquire_complete here.
# No new REF GetBuffer success here.

[XeFG][ResizeLifecycle] kind = ResizeBuffers1 stage = enter
...
[XeFG][ResizeBuffers1] stage = original_return, result = 0x00000000
[XeFG][ResizeHold] action = complete

next Present / Present1
    -> present_pre_render_callback
    -> renderer acquire
    -> present_post_render_callback
```

### 24.2 Forbidden pattern while hold is active

This must not happen:

```text
ResizeHold action = arm
    -> renderer_acquire_complete
    -> ResizeHold action = complete
```

Renderer reacquisition must occur only after completion.

---

## 25. Runtime Validation Matrix

### 25.1 MHW — baseline launch

Topology:

```text
OptiScaler dxgi.dll
REFramework dinput8.dll
Special K absent
XeFG active
D3D12
```

Verify before Alt+Enter:

```text
game starts
XeFG active
OptiScaler overlay visible
REFramework overlay visible
normal presentation stable
no hold active during ordinary gameplay
```

### 25.2 MHW — Alt+Enter primary acceptance test

Perform Alt+Enter using the same workflow that reproduced P3.3A failure.

Required observations:

```text
ResizeTarget triggers existing reset
ResizeHold arms
one or more intermediate Present/Present1 may be suppressed
no REF backbuffer reacquisition while hold active
outer resize does not return E_PENDING
tracked buffer resize completion appears if expected
ResizeHold completes only after successful tracked buffer resize
REF overlay returns on subsequent Present
XeFG remains active
OptiScaler overlay remains active
no fatal D3D error
```

### 25.3 Repeat Alt+Enter

Perform multiple transitions:

```text
windowed -> fullscreen/borderless direction
fullscreen/borderless -> windowed direction
repeat at least several times
```

Each cycle must show balanced:

```text
arm -> complete
```

No stale hold between cycles.

### 25.4 DD2 regression test

Intel Dragon's Dogma 2 was the first stable XeFG functional baseline.

Verify:

```text
launch
XeFG active
Opti overlay visible
REF overlay visible
normal gameplay
Alt+Tab
return to game
```

P3.3B must not regress DD2.

A previous P3.1 DD2 Alt+Tab run did not crash; preserve that behavior.

### 25.5 Native/non-XeFG smoke test if convenient

The code change is source-gated, but a quick native D3D12 smoke test is desirable when available.

Confirm no hold log appears when:

```text
SwapchainSource != XeFGInternal
```

---

## 26. Result Classification

Use the following classifications exactly during runtime review.

### PASS

```text
MHW Alt+Enter survives
no E_PENDING outer ResizeBuffers failure
hold arms
intermediate REF renderer reacquisition is absent
tracked successful ResizeBuffers/ResizeBuffers1 completes hold
REF overlay returns
XeFG remains functional
```

### PARTIAL PASS — crash fixed but completion boundary missing

```text
MHW Alt+Enter survives
E_PENDING disappears
REF backbuffer reacquisition is suppressed
but no successful tracked ResizeBuffers/ResizeBuffers1 appears
hold remains active
REF overlay does not return
```

This proves the crash-defense hypothesis but means completion needs another evidence-driven PR.

Do not hide this with a timeout.

### FAIL — original crash remains

```text
E_PENDING still occurs
```

Then inspect whether:

```text
hold armed before intermediate Present
renderer callback was actually suppressed
backbuffer_ref_count stayed 0
```

If all three are true and E_PENDING still occurs, REFramework is not the remaining outstanding owner and further investigation must focus on another component.

### FAIL — REF still reacquires while held

If logs show:

```text
ResizeHold active
renderer_acquire_complete
```

then P3.3B implementation is incorrect.

Fix the suppression path before investigating other owners.

---

## 27. Build / Verification Requirements

Follow the repository's normal build and test workflow plus the user's global Codex completion policy.

At minimum:

```text
build the affected REFramework target/configuration
no new compile warnings attributable to P3.3B
review final diff
confirm no hook-monitor changes
confirm no P3.2 transaction changes beyond hold cleanup after successful commit
confirm no multi-module runtime-registry changes
```

If the repository has no practical unit-test seam for live DXGI hook behavior, do not create fake low-value tests solely to satisfy a checkbox.

Prefer compile validation plus the explicit runtime acceptance matrix above.

If a small deterministic helper test is practical for hold-state transitions without mocking COM/DXGI internals excessively, it is acceptable.

Do not perform a large test-framework refactor in this PR.

---

## 28. Expected File / LOC Scope

Preferred implementation files:

```text
src/D3D12Hook.hpp
src/D3D12Hook.cpp
```

`src/REFramework.cpp` should normally require **no behavioral change** for P3.3B.

If implementation requires modifying `REFramework.cpp`, explain why in the PR description and ensure the change is not hook-monitor related.

Expected size:

```text
roughly 70-160 LOC
```

depending on diagnostics/helper factoring.

Keep the PR small.

Do not mix cleanup/refactoring unrelated to the hold.

---

## 29. Explicit Out of Scope

Do not include any of the following in P3.3B:

```text
P3.3R hook-monitor recovery changes
hook-monitor timeout changes
generic D3D12 recovery redesign
public XeFG proxy rendering
public XeFG proxy lifecycle hook
Streamline wrapper hook expansion
OptiScaler source changes
Special K compatibility/emulation
manual COM refcount forcing
manual backbuffer Release loops
sleep-based resize workaround
fixed timeout hold release
frame-count hold release
fullscreen polling
Alt+Enter key detection
window-mode policy
ResizeTarget suppression
skipping original Present
skipping original ResizeTarget
reset coalescing
P3.2 rebind redesign
P3.1 ownership redesign
PR #14 runtime registry changes
private Intel XeFG object offsets
reverse engineering private Intel layouts
FSRFG work
DLSS-G work
native/no-FG acceptance expansion
Resident Evil Requiem stutter investigation
NVIDIA/MHW release-storm side work
large generic graphics refactor
```

---

## 30. PR Description Requirements

The implementation PR description should state clearly:

### Problem

```text
During MHW Alt+Enter, REF releases its internal XeFG presentation backbuffers at ResizeTarget but immediately reacquires the same resources on an intermediate Present1 before the outer XeFG ResizeBuffers completes. The outer resize then fails with E_PENDING/outstanding references.
```

### Change

```text
Add a XeFG-internal resize-transition render hold after top-level ResizeTarget reset. While held, Present/Present1 are forwarded but REF render callbacks are suppressed. Release the hold only after a later successful tracked ResizeBuffers/ResizeBuffers1 or a binding lifecycle replacement.
```

### Non-goals

```text
No hook-monitor changes.
No timeout workaround.
No public-proxy hook.
No OptiScaler changes.
```

### Runtime validation

Include MHW Alt+Enter logs showing:

```text
arm
suppressed intermediate Present
successful completion resize
complete
overlay reinitialize
```

If runtime result is only partial, say so explicitly and keep the PR draft until the user reviews the evidence.

---

## 31. Implementation Checklist

Before coding:

- [ ] Rebase onto current `master`.
- [ ] Read P3.3A diagnostics work order.
- [ ] Read P3.3R work order and preserve separation.
- [ ] Inspect current `present_common()` suppression behavior.
- [ ] Inspect current `resize_target()` P3.3A event path.
- [ ] Inspect current `resize_buffers()` and `resize_buffers1()` return ordering.
- [ ] Inspect P3.2 rebind commit points.

Implementation:

- [ ] Add per-`D3D12Hook` resize-transition hold state.
- [ ] Add bounded arm/suppress/complete diagnostics.
- [ ] Arm after existing top-level XeFG ResizeTarget reset.
- [ ] Arm before original ResizeTarget call.
- [ ] Clear if original ResizeTarget fails.
- [ ] Extend `present_common()` suppression condition.
- [ ] Continue forwarding original Present/Present1.
- [ ] Continue `note_present_activity()` while suppressed.
- [ ] Do not run `m_on_present` while held.
- [ ] Do not run `m_on_post_present` while held.
- [ ] Complete only after successful top-level tracked ResizeBuffers.
- [ ] Complete only after successful tracked ResizeBuffers1.
- [ ] Keep hold on failed buffer resize.
- [ ] Clear stale hold after successful P3.2 binding replacement.
- [ ] Clear stale hold on new external XeFG binding.
- [ ] Clear hold on unhook.

Do not implement:

- [ ] No time-based fallback.
- [ ] No frame-count fallback.
- [ ] No sleeps.
- [ ] No forced COM releases.
- [ ] No public proxy hook.
- [ ] No hook-monitor modification.
- [ ] No OptiScaler modification.

Verification:

- [ ] Build succeeds.
- [ ] Final diff is focused.
- [ ] MHW normal launch works.
- [ ] MHW Alt+Enter reproducer tested.
- [ ] REF resources remain released during held Present.
- [ ] E_PENDING result checked.
- [ ] Hold completion behavior checked.
- [ ] REF overlay return checked.
- [ ] XeFG continuity checked.
- [ ] DD2 regression smoke tested.

---

## 32. Definition of Done

P3.3B is complete only when the code and evidence show one of the following clearly.

### Preferred complete success

```text
MHW Alt+Enter
    -> REF resets
    -> ResizeHold arms
    -> intermediate Present1 is forwarded without REF renderer reacquisition
    -> outer XeFG resize progresses
    -> tracked buffer resize succeeds
    -> ResizeHold completes
    -> next Present recreates REF renderer
    -> game continues
    -> XeFG continues
    -> both overlays remain usable
    -> no E_PENDING fatal error
```

### Evidence-backed partial result

If the crash disappears but the hold never receives a successful tracked completion resize:

```text
Do not invent a timeout.
Do not claim full completion.
Report partial success with logs.
```

That result would justify a later lifecycle-observation PR for the outer XeFG/public proxy boundary.

### Rejected result

The following is not acceptable:

```text
Alt+Enter appears to work because REF sleeps for N ms
Alt+Enter appears to work because Present is skipped
Alt+Enter appears to work because COM refs are force-released
Alt+Enter appears to work because renderer is permanently disabled after ResizeTarget
```

The fix must remain a narrow, lifecycle-driven REFramework resource-ownership correction.

---

## 33. Architectural Principle to Preserve

The broader Special-K-removal project is converging on this ownership model:

```text
XeFG owns presentation lifecycle
OptiScaler owns FG integration
REFramework observes validated XeFG lifecycle boundaries
REFramework renders only on the validated internal/post-FG presentation swapchain
REFramework releases its renderer resources before XeFG buffer mutation
REFramework does not reacquire those resources until the buffer mutation is actually complete
```

P3.3B should make the final line true for the MHW Alt+Enter path without introducing another compatibility layer or reproducing Special K behavior.
