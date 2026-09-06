# Work Order: XeFG Refactor R8 — Resize Lifecycle State Extraction

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `da103193cafe466db2fd2cf3e93329a9bb0307a8` (`Refactor R7: transactional XeFG initial bind and rebind (#25)`)

Relevant merged refactor baseline:

- R1 / PR #17: `65f9b3ee81971c3e2aac6df49518fa2dd588365d` — exact-HMODULE XeFG runtime registry extraction
- R2 / PR #18: `aa3a53e516b882a77d399c929efa0ef29d1426b0` — XeFG loader/probe handoff isolation
- PR #19: `3f83b8af0184f931daec44dc45257f7fc46966a4` — tracked `CMakeLists.txt` XeFG source registration
- PR #20: `74042e1686f62a54a50540e1a113a3ae648778c1` — MHW-only XeFG `ResizeTarget` transition hold
- R3 / PR #21: `cfb6efe5102f330c9ddf6fdadb7e9af984ce0678` — InitDesc observation + temporary factory capture extraction
- R4 / PR #22: `1eab453505fe3c75bb7ed45715ce7a1d2c6cc35f` — queue/device/HWND validation + strongly-owned candidate construction
- R5 / PR #23: `d470bee7eb6f369bbef8983982902520e0c0572c` — pending candidate handoff extraction
- R6 / PR #24: `be062b529df67c79571d84d896d994efc8125bf3` — active XeFG ownership/generation/mode/identity centralized in `XeFGBinding`
- R7 / PR #25: `da103193cafe466db2fd2cf3e93329a9bb0307a8` — transactional initial bind + changed-object rebind preparation

Related documents:

- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_ARCHITECTURE_2026-09-06.md`
- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_PR_SPLIT_PLAN_2026-09-06.md`
- `doc/REFramework_XeFG_P3_LIFECYCLE_ROBUSTNESS_PLAN_2026-09-06.md`
- `doc/work-order/XEFG_REFACTOR_R7_INITIAL_BIND_TRANSACTIONAL_REBIND.md`

This work order implements **R8 only** from the fine-grained XeFG refactor plan.

The fork is still **unreleased**. Therefore R8 should prefer a clean internal cut: once all call sites use the new lifecycle object, remove the old fork-only `m_xefg_resize_*` fields and helper state rather than keeping duplicate compatibility aliases.

That freedom applies only to internal structure. The working runtime behavior is the compatibility contract.

---

# 1. Recommended PR Identity

Suggested branch:

```text
refactor/xefg-r8-resize-lifecycle
```

Suggested PR title:

```text
Refactor R8: extract XeFG resize lifecycle state
```

Suggested commit title:

```text
refactor: extract XeFG resize lifecycle state
```

One primary responsibility:

> Move XeFG-specific resize event, transition-hold, suppressed-present, and post-resize diagnostic state out of scattered `D3D12Hook` members into one `XeFGResizeLifecycle` semantic state object while keeping all physical DXGI callbacks and current game/render policy in `D3D12Hook`.

Do not start R9 callback-policy cleanup in this PR.

---

# 2. Release / Development Policy

The repository remains unreleased. Planned sequence remains:

```text
R8 resize lifecycle state extraction
R9 Present/resize callback cleanup
R10 hook-monitor/upstream-surface cleanup
R11 final logging + persistent Debug Logging UI
final runtime validation
release
```

Because the code is unreleased, R8 may:

- introduce `XeFGResizeLifecycle.hpp/.cpp` now;
- remove the old nested resize enum from `D3D12Hook` if all fork-only call sites migrate in the same PR;
- remove all old resize lifecycle member fields after migration;
- move post-resize diagnostic budget/ordinal/timestamp into the lifecycle object now rather than carrying temporary duplicate state into R9/R11;
- use narrow value-returning transition results to preserve logging outside the semantic object.

Still forbidden:

- changing MHW-only activation policy;
- changing which physical callbacks are hooked;
- changing renderer reset ordering;
- changing original DXGI forwarding order;
- changing success/failure semantics of hold completion;
- adding timeout/sleep/Present-count based recovery;
- changing hook-monitor behavior;
- changing log levels or doing broad logging cleanup;
- generic frame-generation abstractions;
- FSRFG/DLSSG/Streamline behavior changes;
- unrelated D3D12 refactors.

---

# 3. Current State After R7

The active binding transaction is now isolated enough that the remaining XeFG resize state is visibly independent.

Current `D3D12Hook.hpp` still contains:

```cpp
enum class XefgResizeEventKind : uint8_t {
    None,
    ResizeTarget,
    ResizeBuffers,
    ResizeBuffers1,
};

uint64_t m_xefg_resize_event_id{0};
bool m_xefg_resize_transition_hold{false};
uint64_t m_xefg_resize_transition_hold_event_id{0};
uint32_t m_xefg_resize_transition_suppressed_present_count{0};
XefgResizeEventKind m_xefg_last_resize_kind{XefgResizeEventKind::None};
std::chrono::steady_clock::time_point m_xefg_last_resize_event_time{};
uint32_t m_xefg_post_resize_present_budget{0};
uint32_t m_xefg_post_resize_present_ordinal{0};
```

and lifecycle mutators:

```cpp
uint64_t begin_xefg_resize_event(XefgResizeEventKind kind);
void arm_xefg_resize_transition_hold(uint64_t event_id);
void complete_xefg_resize_transition_hold(
    uint64_t completion_event_id,
    XefgResizeEventKind completion_kind,
    HRESULT result);
void clear_xefg_resize_transition_hold(const char* reason);
```

`present_common()` also directly reads/mutates several of those fields:

```cpp
m_xefg_resize_transition_hold
m_xefg_resize_transition_hold_event_id
m_xefg_resize_transition_suppressed_present_count
m_xefg_post_resize_present_budget
m_xefg_post_resize_present_ordinal
m_xefg_resize_event_id
```

R8 consolidates the state but does **not** redesign callback behavior.

---

# 4. Current Proven Runtime Contract

The current lifecycle behavior exists because ordinary renderer behavior around XeFG `ResizeTarget` caused instability in the proven MHW path.

The contract to preserve is:

```text
tracked XeFG ResizeTarget
    -> normal REFramework ResizeTarget renderer reset runs
    -> only if:
         event is tracked
         renderer reset actually ran
         active XeFG mode is render-capable (not observe-only)
         current game is Monster Hunter Wilds
       then arm transition hold
    -> forward original ResizeTarget

while hold active
    -> Present / Present1 MUST still call the original XeFG presentation function
    -> REFramework renderer / mod render callback is suppressed
    -> present activity still keeps hook monitor alive

tracked ResizeBuffers / ResizeBuffers1
    -> forward existing renderer reset behavior
    -> forward original DXGI call
    -> if original call SUCCEEDS, complete hold
    -> if original call FAILS, keep hold active

failed tracked ResizeTarget
    -> clear stale hold

same-object XeFG rebind
changed-object XeFG rebind
initial external bind
unhook
    -> clear stale transition hold
```

Important current policy detail:

> **The hold is armed only for Monster Hunter Wilds.**

PR #20 intentionally made this game-specific:

```cpp
if (event_id != 0
    && renderer_reset_performed
    && !d3d12->m_xefg_binding.observe_only()
    && sdk::GameIdentity::get().is_mhwilds()) {
    d3d12->arm_xefg_resize_transition_hold(event_id);
}
```

R8 must not broaden this to DD2, Pragmata, all XeFG games, or all frame-generation paths.

---

# 5. Why R8 Is Separate From R9

R8 answers:

> What semantic XeFG resize state exists, and how does that state transition?

R9 answers:

> How should physical Present/Resize callbacks consume those semantic decisions with less XeFG-specific branching?

Do not merge the two failure domains.

R8 failure examples:

- event id increments differently;
- failed ResizeBuffers incorrectly clears the hold;
- stale hold survives a rebind/unhook;
- suppressed-present count is not reset on arm/complete/clear;
- post-resize diagnostic budget changes from three samples;
- last resize timestamp/kind changes timing semantics.

R9 failure examples:

- Present original-call forwarding changes;
- renderer callback suppression branch moves to the wrong side of the original Present;
- nested resize behavior changes;
- native callback path becomes coupled to XeFG.

R8 should make R9 easier without performing R9.

---

# 6. Strict R8 Scope

## 6.1 Move into `XeFGResizeLifecycle`

Move semantic ownership of:

- monotonically increasing resize event id;
- last resize kind;
- last resize timestamp;
- transition hold active flag;
- hold trigger event id;
- suppressed-present count while hold is active;
- post-resize diagnostic budget;
- post-resize present ordinal;
- begin-event transition;
- arm-hold transition;
- successful/failed completion semantics;
- clear/reset semantics;
- note-suppressed-present transition;
- post-resize diagnostic sample consumption;
- narrow state queries/getters.

## 6.2 Keep in `D3D12Hook`

Keep physical ownership/policy:

- `resize_target()` callback;
- `resize_buffers()` callback;
- `resize_buffers1()` callback;
- `present()` / `present1()` / `present_common()` callbacks;
- retrieval/call of original DXGI functions;
- nested-call depth counters;
- renderer callback invocation;
- renderer reset ordering;
- `g_framework->note_present_activity()`;
- MHW-only `GameIdentity` condition;
- `XeFGBinding::observe_only()` policy;
- swapchain-source / tracked-instance checks;
- all VtableHook ownership;
- binding generation;
- current lifecycle log formatting and log levels.

## 6.3 Explicitly do not move

Do **not** put into `XeFGResizeLifecycle`:

- `D3D12Hook*`;
- `IDXGISwapChain*` ownership;
- `ID3D12CommandQueue*`;
- `ID3D12Device*`;
- `VtableHook`;
- `g_framework`;
- `sdk::GameIdentity`;
- renderer callbacks;
- spdlog dependency if avoidable;
- physical callback function pointers;
- binding generation state.

The lifecycle object should be a small semantic state machine, not another hook manager.

---

# 7. Recommended New Files

Add:

```text
src/compatibility/xefg/XeFGResizeLifecycle.hpp
src/compatibility/xefg/XeFGResizeLifecycle.cpp
```

Modify:

```text
src/D3D12Hook.hpp
src/D3D12Hook.cpp
CMakeLists.txt
```

Normally unchanged:

```text
src/compatibility/xefg/XeFGBinding.*
src/compatibility/xefg/XeFGCandidateHandoff.*
src/compatibility/xefg/XeFGDiscovery.*
src/compatibility/xefg/XeFGCompatibility.*
src/compatibility/xefg/XeFGRuntimeRegistry.*
src/REFramework.cpp
cmake.toml
```

As established in PR #19, the checked-in `CMakeLists.txt` needs explicit registration:

```cmake
"src/compatibility/xefg/XeFGResizeLifecycle.cpp"
"src/compatibility/xefg/XeFGResizeLifecycle.hpp"
```

Do not broadly regenerate `CMakeLists.txt`.

Do not modify `cmake.toml`; its recursive source declaration is already adequate.

---

# 8. Recommended Lifecycle Type

A clean R8 target is:

```cpp
#pragma once

#include <chrono>
#include <cstdint>

#include <Windows.h>

class XeFGResizeLifecycle {
public:
    enum class EventKind : uint8_t {
        None,
        ResizeTarget,
        ResizeBuffers,
        ResizeBuffers1,
    };

    enum class CompletionAction : uint8_t {
        NoHold,
        KeepHold,
        Completed,
    };

    struct CompletionResult {
        CompletionAction action{CompletionAction::NoHold};
        uint64_t trigger_event_id{};
        uint64_t completion_event_id{};
        EventKind completion_kind{EventKind::None};
        uint32_t suppressed_presents{};
    };

    struct ClearResult {
        bool cleared{};
        uint64_t trigger_event_id{};
        uint32_t suppressed_presents{};
    };

    struct PostResizePresentSample {
        bool valid{};
        uint64_t event_id{};
        uint32_t ordinal{};
        int64_t elapsed_ms{};
    };

    uint64_t begin(EventKind kind) noexcept;

    bool arm(uint64_t trigger_event_id) noexcept;

    CompletionResult complete(
        uint64_t completion_event_id,
        EventKind completion_kind,
        HRESULT result) noexcept;

    ClearResult clear() noexcept;

    bool hold_active() const noexcept;
    bool suppress_renderer() const noexcept;
    uint64_t hold_event_id() const noexcept;
    uint32_t suppressed_present_count() const noexcept;

    uint32_t note_suppressed_present() noexcept;

    bool has_post_resize_present_sample() const noexcept;
    PostResizePresentSample consume_post_resize_present_sample() noexcept;

    uint64_t last_event_id() const noexcept;
    EventKind last_kind() const noexcept;
    std::chrono::steady_clock::time_point last_event_time() const noexcept;

private:
    uint64_t m_event_id{};
    EventKind m_last_kind{EventKind::None};
    std::chrono::steady_clock::time_point m_last_event_time{};

    bool m_hold_active{};
    uint64_t m_hold_event_id{};
    uint32_t m_suppressed_present_count{};

    uint32_t m_post_resize_present_budget{};
    uint32_t m_post_resize_present_ordinal{};
};
```

Exact naming can differ.

The important shape is:

```text
XeFGResizeLifecycle
    = semantic state + deterministic transitions

D3D12Hook
    = physical hooks + renderer policy + logging context
```

---

# 9. Avoid Coupling Lifecycle State to Game Policy

Do not write APIs such as:

```cpp
lifecycle.arm_if_mhwilds(...);
lifecycle.on_resize_target(GameIdentity::get(), binding, ...);
```

That makes the compatibility state object responsible for game policy and renderer policy.

Keep the current policy at the physical callback boundary:

```cpp
if (event_id != 0
    && renderer_reset_performed
    && !d3d12->m_xefg_binding.observe_only()
    && sdk::GameIdentity::get().is_mhwilds()) {
    d3d12->arm_xefg_resize_transition_hold(event_id);
}
```

The thin `D3D12Hook` wrapper may then delegate only the state mutation:

```cpp
const auto armed = m_xefg_resize_lifecycle.arm(event_id);
```

R8 is state extraction, not policy migration.

---

# 10. Begin-Event Semantics Must Remain Exact

Current behavior:

```cpp
m_xefg_resize_event_id++;
m_xefg_last_resize_kind = kind;
m_xefg_last_resize_event_time = std::chrono::steady_clock::now();
m_xefg_post_resize_present_budget = 3;
m_xefg_post_resize_present_ordinal = 0;
return m_xefg_resize_event_id;
```

R8 must preserve exactly:

```cpp
uint64_t XeFGResizeLifecycle::begin(EventKind kind) noexcept {
    ++m_event_id;
    m_last_kind = kind;
    m_last_event_time = std::chrono::steady_clock::now();
    m_post_resize_present_budget = 3;
    m_post_resize_present_ordinal = 0;
    return m_event_id;
}
```

Do not reset the hold merely because a new resize event begins.

That would change the current state machine.

A `ResizeBuffers/ResizeBuffers1` event is specifically allowed to begin while a prior `ResizeTarget` hold remains active and then complete that hold after the original call succeeds.

---

# 11. Arm Semantics Must Remain Exact

Current state mutation:

```cpp
m_xefg_resize_transition_hold = true;
m_xefg_resize_transition_hold_event_id = event_id;
m_xefg_resize_transition_suppressed_present_count = 0;
```

The existing wrapper also rejects:

```text
non-XeFG source
observe-only mode
event_id == 0
```

In R8, the source/mode policy may remain in the `D3D12Hook` wrapper so the semantic object stays narrow.

Recommended:

```cpp
void D3D12Hook::arm_xefg_resize_transition_hold(uint64_t event_id) {
    if (m_swapchain_source != SwapchainSource::XeFGInternal
        || m_xefg_binding.observe_only()
        || event_id == 0) {
        return;
    }

    if (!m_xefg_resize_lifecycle.arm(event_id)) {
        return;
    }

    spdlog::info(/* preserve current log */);
}
```

and:

```cpp
bool XeFGResizeLifecycle::arm(uint64_t event_id) noexcept {
    if (event_id == 0) {
        return false;
    }

    m_hold_active = true;
    m_hold_event_id = event_id;
    m_suppressed_present_count = 0;
    return true;
}
```

Do not introduce a timeout here.

Do not require the trigger event to equal `last_event_id()` as a new validation rule; current code does not impose that behavior.

---

# 12. Completion Semantics Must Remain Exact

Current behavior is intentionally asymmetric:

```text
no hold active
    -> no-op

hold active + FAILED ResizeBuffers/ResizeBuffers1
    -> KEEP hold active

hold active + successful ResizeBuffers/ResizeBuffers1
    -> clear hold
```

Do not “clean up” failed completion by clearing the hold.

Recommended implementation:

```cpp
XeFGResizeLifecycle::CompletionResult XeFGResizeLifecycle::complete(
    uint64_t completion_event_id,
    EventKind completion_kind,
    HRESULT result) noexcept {
    CompletionResult out{};
    out.completion_event_id = completion_event_id;
    out.completion_kind = completion_kind;

    if (!m_hold_active) {
        return out;
    }

    out.trigger_event_id = m_hold_event_id;
    out.suppressed_presents = m_suppressed_present_count;

    if (FAILED(result)) {
        out.action = CompletionAction::KeepHold;
        return out;
    }

    out.action = CompletionAction::Completed;
    m_hold_active = false;
    m_hold_event_id = 0;
    m_suppressed_present_count = 0;
    return out;
}
```

The D3D12Hook wrapper can preserve the existing logs based on `CompletionAction`.

Important:

- do not clear post-resize diagnostic budget here unless current behavior already does;
- do not change the last event id/kind here;
- do not use `completion_event_id > trigger_event_id` as a new gate;
- do not add completion kind filtering beyond current call-site policy.

The physical callbacks already determine that completion is invoked only from tracked XeFG `ResizeBuffers` / `ResizeBuffers1` paths.

---

# 13. Clear Semantics Must Remain Exact

Current `clear_xefg_resize_transition_hold(reason)`:

```text
if no hold
    -> return

if hold
    -> log trigger event + suppressed count
    -> clear hold flag
    -> clear trigger event id
    -> zero suppressed-present count
```

The semantic object should not need the textual `reason`.

Recommended:

```cpp
XeFGResizeLifecycle::ClearResult XeFGResizeLifecycle::clear() noexcept {
    if (!m_hold_active) {
        return {};
    }

    ClearResult out{
        .cleared = true,
        .trigger_event_id = m_hold_event_id,
        .suppressed_presents = m_suppressed_present_count,
    };

    m_hold_active = false;
    m_hold_event_id = 0;
    m_suppressed_present_count = 0;
    return out;
}
```

Then preserve reason/logging outside:

```cpp
void D3D12Hook::clear_xefg_resize_transition_hold(const char* reason) {
    const auto cleared = m_xefg_resize_lifecycle.clear();
    if (!cleared.cleared) {
        return;
    }

    spdlog::info(
        "[XeFG][ResizeHold] action = clear, reason = {}, ...",
        reason != nullptr ? reason : "unknown",
        cleared.trigger_event_id,
        cleared.suppressed_presents,
        m_xefg_binding.generation());
}
```

Existing clear call sites must remain:

```text
same-object binding update        -> "binding_replaced"
changed-object binding replacement -> "binding_replaced"
initial external bind             -> "external_bind"
unhook                             -> "unhook"
failed ResizeTarget               -> "resize_target_failed"
```

Do not remove any of these in R8.

---

# 14. Present Suppression Semantics

Current code derives:

```cpp
const auto xefg_resize_transition_hold =
    d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
    && d3d12->m_xefg_resize_transition_hold;

const auto suppress_render_callbacks =
    d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
    && (d3d12->m_xefg_binding.observe_only()
        || xefg_resize_transition_hold);
```

R8 should do the mechanical semantic-state substitution only:

```cpp
const auto xefg_resize_transition_hold =
    d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
    && d3d12->m_xefg_resize_lifecycle.suppress_renderer();

const auto suppress_render_callbacks =
    d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
    && (d3d12->m_xefg_binding.observe_only()
        || xefg_resize_transition_hold);
```

Do not simplify this further in R8.

In particular, do not move the full `suppress_render_callbacks` policy into `XeFGResizeLifecycle`, because observe-only belongs to `XeFGBinding` and callback policy cleanup belongs to R9.

Recommended lifecycle query:

```cpp
bool XeFGResizeLifecycle::suppress_renderer() const noexcept {
    return m_hold_active;
}
```

---

# 15. Suppressed Present Count

Current behavior increments the count only while the resize hold is active and logs the first three suppressed presents.

Preserve:

```cpp
if (xefg_resize_transition_hold) {
    const auto suppressed_present =
        d3d12->m_xefg_resize_lifecycle.note_suppressed_present();

    if (suppressed_present <= 3) {
        spdlog::info(/* existing log */);
    }
}
```

Recommended semantic method:

```cpp
uint32_t XeFGResizeLifecycle::note_suppressed_present() noexcept {
    if (!m_hold_active) {
        return 0;
    }
    return ++m_suppressed_present_count;
}
```

Do not use the count as a recovery trigger.

The count is diagnostic only.

Forbidden:

```text
clear hold after N presents
clear hold after 3 presents
clear hold after X milliseconds
```

---

# 16. Post-Resize Diagnostic State

Current `begin_xefg_resize_event()` gives every tracked event a three-Present diagnostic budget:

```cpp
m_xefg_post_resize_present_budget = 3;
m_xefg_post_resize_present_ordinal = 0;
```

and `log_xefg_post_resize_present()` consumes one slot per eligible Present and computes elapsed time from `m_xefg_last_resize_event_time`.

Because the fork is unreleased, R8 should preferably move these fields too so no resize lifecycle state remains split across `D3D12Hook` and `XeFGResizeLifecycle`.

Recommended semantic sample method:

```cpp
XeFGResizeLifecycle::PostResizePresentSample
XeFGResizeLifecycle::consume_post_resize_present_sample() noexcept {
    if (m_post_resize_present_budget == 0) {
        return {};
    }

    --m_post_resize_present_budget;
    const auto ordinal = ++m_post_resize_present_ordinal;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_last_event_time).count();

    return {
        .valid = true,
        .event_id = m_event_id,
        .ordinal = ordinal,
        .elapsed_ms = elapsed,
    };
}
```

Then `D3D12Hook::log_xefg_post_resize_present()` remains responsible for formatting/logging swapchain, hook, binding, queue, device, and original-function metadata.

This is preferred over moving spdlog and D3D12 object knowledge into the lifecycle class.

Do not change the budget from `3` in R8.

R11 may later decide whether these diagnostics belong behind Debug Logging.

---

# 17. Last Resize Kind and Public Diagnostic Accessors

Current public diagnostic APIs:

```cpp
uint64_t get_xefg_last_resize_event_id() const;
const char* get_xefg_last_resize_kind() const;
```

Preserve their externally observable behavior.

Recommended:

```cpp
uint64_t get_xefg_last_resize_event_id() const {
    return m_xefg_resize_lifecycle.last_event_id();
}

const char* get_xefg_last_resize_kind() const {
    return resize_kind_name(m_xefg_resize_lifecycle.last_kind());
}
```

If `resize_kind_name()` remains in `D3D12Hook.cpp`, update it to accept the new enum type:

```cpp
const char* resize_kind_name(XeFGResizeLifecycle::EventKind kind) {
    switch (kind) {
    case XeFGResizeLifecycle::EventKind::ResizeTarget:
        return "ResizeTarget";
    case XeFGResizeLifecycle::EventKind::ResizeBuffers:
        return "ResizeBuffers";
    case XeFGResizeLifecycle::EventKind::ResizeBuffers1:
        return "ResizeBuffers1";
    default:
        return "None";
    }
}
```

Do not change the existing string values in R8.

---

# 18. Thin D3D12Hook Wrappers Are Acceptable in R8

The existing methods:

```cpp
begin_xefg_resize_event(...)
arm_xefg_resize_transition_hold(...)
complete_xefg_resize_transition_hold(...)
clear_xefg_resize_transition_hold(...)
```

may remain as thin wrappers for this PR.

That is actually preferred if it keeps the callback diff mechanical and preserves logging exactly.

Example:

```cpp
uint64_t D3D12Hook::begin_xefg_resize_event(
    XeFGResizeLifecycle::EventKind kind) {
    return m_xefg_resize_lifecycle.begin(kind);
}
```

R9 can later decide whether callbacks should call the lifecycle object directly or use narrower semantic notifications.

Do not remove wrappers merely to make R8 look “cleaner” if doing so causes large callback churn.

---

# 19. ResizeTarget Callback — Required Mechanical Migration

Keep the physical flow unchanged.

Current conceptual sequence:

```text
identify tracked XeFG top-level ResizeTarget
-> begin event
-> update render dimensions
-> log enter
-> nested-call compatibility path unchanged
-> run on_resize_target callback
-> mark renderer_reset_performed
-> MHW + render-capable mode => arm hold
-> call original ResizeTarget
-> if failed => clear hold("resize_target_failed")
-> log original_return
-> return original result
```

Only the event-kind type/state backing should change.

Recommended call-site conversion:

```cpp
using ResizeKind = XeFGResizeLifecycle::EventKind;

const auto event_id = is_xefg_internal && is_top_level
    ? d3d12->begin_xefg_resize_event(ResizeKind::ResizeTarget)
    : 0;
```

The MHW gate must stay exactly where it currently is relative to renderer reset and original `ResizeTarget`:

```cpp
if (event_id != 0
    && renderer_reset_performed
    && !d3d12->m_xefg_binding.observe_only()
    && sdk::GameIdentity::get().is_mhwilds()) {
    d3d12->arm_xefg_resize_transition_hold(event_id);
}
```

Do not arm after original `ResizeTarget` in R8.

Do not broaden the condition.

---

# 20. ResizeBuffers Callback — Required Mechanical Migration

Preserve:

```text
tracked XeFG + top-level only => begin event
nested behavior unchanged
renderer reset callback ordering unchanged
original ResizeBuffers called exactly as before
completion called only after original returns
FAILED completion keeps an active hold
```

Recommended event type conversion:

```cpp
const auto event_id = is_xefg_internal && is_top_level
    ? d3d12->begin_xefg_resize_event(
          XeFGResizeLifecycle::EventKind::ResizeBuffers)
    : 0;
```

After original return:

```cpp
if (event_id != 0) {
    d3d12->log_xefg_resize_event(
        event_id,
        XeFGResizeLifecycle::EventKind::ResizeBuffers,
        "original_return",
        swap_chain,
        resize_buffers_original,
        result,
        true);

    d3d12->complete_xefg_resize_transition_hold(
        event_id,
        XeFGResizeLifecycle::EventKind::ResizeBuffers,
        result);
}
```

Do not reinterpret failure as a stale-hold condition.

---

# 21. ResizeBuffers1 Callback — Required Mechanical Migration

Keep current special behavior:

- only tracked XeFG instance takes the special path;
- nested call directly forwards original;
- renderer reset occurs only when `!observe_only && m_on_resize_buffers`;
- original `ResizeBuffers1` is always forwarded;
- completion occurs after original return;
- failure keeps hold active.

Only replace direct lifecycle state/enum access.

Do not add `ResizeBuffers1` behavior to native swapchains.

---

# 22. Present / Present1 — R8 Boundary

R8 may mechanically replace direct state reads with lifecycle queries, but do not restructure the callback.

Allowed:

```cpp
m_xefg_resize_lifecycle.suppress_renderer()
m_xefg_resize_lifecycle.hold_event_id()
m_xefg_resize_lifecycle.note_suppressed_present()
m_xefg_resize_lifecycle.consume_post_resize_present_sample()
```

Not allowed in R8:

- moving the original Present call before/after callbacks;
- changing recursive Present behavior;
- changing observe-only suppression policy;
- changing `note_present_activity()` behavior;
- changing when `m_on_post_present` runs;
- changing device-removed diagnostics;
- general Present callback cleanup.

Those are R9/R11 concerns.

---

# 23. Binding / Unhook Integration

R7 still clears stale resize hold in these paths:

```text
same-object binding update
changed-object replacement
initial external XeFG bind
unhook
```

Keep those calls.

R8 only changes their backing state.

Example:

```cpp
clear_xefg_resize_transition_hold("binding_replaced");
```

should continue to work, with the wrapper internally calling:

```cpp
m_xefg_resize_lifecycle.clear();
```

Do not move binding ownership into the resize lifecycle object.

Do not clear `XeFGBinding` from lifecycle methods.

---

# 24. Native / Non-XeFG Isolation

R8 must not cause lifecycle hold state to suppress native REFramework rendering.

The current outer gate must remain:

```cpp
m_swapchain_source == SwapchainSource::XeFGInternal
```

before hold state contributes to Present suppression.

Even if stale lifecycle state somehow exists, native source must behave as native.

This is a merge gate.

Do not write:

```cpp
const auto suppress_render_callbacks =
    m_xefg_resize_lifecycle.suppress_renderer();
```

without the XeFG source gate.

Correct mechanical form remains conceptually:

```cpp
const auto xefg_resize_transition_hold =
    m_swapchain_source == SwapchainSource::XeFGInternal
    && m_xefg_resize_lifecycle.suppress_renderer();
```

---

# 25. No Timeout / Heuristic Recovery

R8 must **not** add any of the following:

```text
hold timeout
wall-clock timeout
Present-count timeout
frame-count timeout
sleep/yield recovery
background worker
polling thread
“auto recover after N suppressed presents”
```

The current completion model is event-driven:

```text
successful tracked ResizeBuffers/ResizeBuffers1
    -> complete

failed ResizeTarget
    -> clear

rebind/unhook
    -> clear
```

Keep it that way.

The `last_resize_event_time` is diagnostic timing only.

---

# 26. Logging Policy for R8

Do not reduce logging in this PR.

The project will do user-facing logging cleanup only after R10, in R11.

Therefore:

- preserve existing `[XeFG][ResizeLifecycle]` lines;
- preserve `[XeFG][ResizeHold]` lines;
- preserve current generation/swapchain/queue/device context;
- preserve current `present_after_resize` first-three-sample diagnostics;
- preserve current callstack/debug-heavy logs for now even if they look excessive;
- do not change `info` -> `debug` yet.

If extracting state would otherwise require moving logging into `XeFGResizeLifecycle`, prefer value-returning transition results and keep spdlog in `D3D12Hook.cpp`.

R11 will decide which lines are support-critical vs Debug Logging.

---

# 27. Recommended Header Migration

After R8, `D3D12Hook.hpp` should no longer contain the old resize lifecycle fields.

Add:

```cpp
#include "compatibility/xefg/XeFGResizeLifecycle.hpp"
```

Replace:

```cpp
uint64_t m_xefg_resize_event_id{0};
bool m_xefg_resize_transition_hold{false};
uint64_t m_xefg_resize_transition_hold_event_id{0};
uint32_t m_xefg_resize_transition_suppressed_present_count{0};
XefgResizeEventKind m_xefg_last_resize_kind{XefgResizeEventKind::None};
std::chrono::steady_clock::time_point m_xefg_last_resize_event_time{};
uint32_t m_xefg_post_resize_present_budget{0};
uint32_t m_xefg_post_resize_present_ordinal{0};
```

with:

```cpp
XeFGResizeLifecycle m_xefg_resize_lifecycle{};
```

If the nested `XefgResizeEventKind` enum has no non-R8 consumer after migration, remove it entirely.

Do not keep both enum types as aliases merely for compatibility; the fork is unreleased.

---

# 28. Old-State Zero-Reference Audit

Before marking R8 complete, search the source tree for all removed names.

Expected zero references after migration:

```text
m_xefg_resize_event_id
m_xefg_resize_transition_hold
m_xefg_resize_transition_hold_event_id
m_xefg_resize_transition_suppressed_present_count
m_xefg_last_resize_kind
m_xefg_last_resize_event_time
m_xefg_post_resize_present_budget
m_xefg_post_resize_present_ordinal
D3D12Hook::XefgResizeEventKind
```

Exceptions are not expected if the clean-cut recommendation is followed.

Do not leave duplicate state “temporarily” for R9 unless a concrete compile/API constraint forces it; if that happens, document the reason in the PR body.

---

# 29. State Transition Matrix

Use this matrix during review.

## A. Begin ResizeTarget

Before:

```text
last event id = N
post-resize budget arbitrary
```

After `begin(ResizeTarget)`:

```text
last event id = N + 1
last kind = ResizeTarget
last event time = now
post-resize budget = 3
post-resize ordinal = 0
hold state unchanged
```

## B. Arm Hold

Before:

```text
hold = false
```

After eligible arm:

```text
hold = true
trigger event id = event_id
suppressed presents = 0
```

## C. Present During Hold

After each suppressed renderer callback:

```text
original Present still forwarded
suppressed-present count += 1
hold remains true
```

## D. Failed ResizeBuffers Completion

```text
HRESULT failed
hold remains true
trigger event id unchanged
suppressed count unchanged
```

## E. Successful ResizeBuffers Completion

```text
hold = false
trigger event id = 0
suppressed presents = 0
```

## F. Failed ResizeTarget

```text
clear("resize_target_failed")
hold = false
trigger event id = 0
suppressed presents = 0
```

## G. Rebind / External Bind / Unhook

```text
clear stale hold
```

## H. Native Source

```text
lifecycle state must not suppress native renderer callbacks
```

---

# 30. Static Review Scenarios

Review the implementation against at least these scenarios.

### Scenario 1 — No hold, ordinary tracked ResizeBuffers

```text
begin event
renderer reset behavior unchanged
original forwarded
complete() sees no hold
no hold mutation
```

### Scenario 2 — MHW ResizeTarget arms hold

```text
tracked XeFG
render-capable mode
renderer reset actually runs
MHW identity true
arm event N
original ResizeTarget succeeds
hold remains active
```

Note: successful ResizeTarget itself does **not** complete the hold.

### Scenario 3 — Present while hold active

```text
renderer callback suppressed
original Present/Present1 forwarded
note_present_activity still runs
suppressed-present diagnostic count increments
```

### Scenario 4 — ResizeBuffers succeeds after hold

```text
new ResizeBuffers event begins
hold remains active during call
original returns success
completion clears hold
```

### Scenario 5 — ResizeBuffers1 fails after hold

```text
original returns failure
completion result = KeepHold
hold remains active
```

### Scenario 6 — ResizeTarget fails

```text
hold may have been armed before original call
original fails
clear("resize_target_failed")
hold cleared
```

### Scenario 7 — Observe-only XeFG

```text
ResizeTarget renderer hold not armed
Present suppression remains observe-only behavior from XeFGBinding
```

### Scenario 8 — Non-MHW XeFG

```text
ResizeTarget hold not armed
```

### Scenario 9 — Binding replaced while hold active

```text
existing R7 clear("binding_replaced") call clears hold
new binding generation proceeds
```

### Scenario 10 — Unhook while hold active

```text
clear("unhook") before hook/binding teardown
```

### Scenario 11 — New resize event while hold active

```text
begin() updates event id/kind/time and diagnostic budget
hold trigger event id remains the original ResizeTarget event
successful ResizeBuffers/1 can complete it
```

This distinction is important. Do not overwrite the hold trigger id merely because a completion event begins.

---

# 31. Failure / Regression Checks

Blocking regressions include:

1. hold cleared by failed ResizeBuffers/ResizeBuffers1;
2. hold no longer cleared by failed ResizeTarget;
3. MHW-only gate broadened or removed;
4. native Present suppressed by lifecycle state;
5. original Present/Present1 not forwarded during hold;
6. original ResizeBuffers/1/Target forwarding order changed;
7. renderer reset ordering changed;
8. begin() clears or re-arms hold unexpectedly;
9. hold trigger event id overwritten by completion event;
10. stale hold survives rebind/unhook;
11. post-resize budget changes from 3 without explicit reason;
12. timeout/Present-count recovery added;
13. lifecycle object starts owning D3D12 COM/hook objects;
14. R8 changes log levels or performs broad cleanup;
15. FSRFG/DLSSG/native behavior changes.

---

# 32. Suggested Implementation Order

A low-risk sequence:

### Step 1 — Add `XeFGResizeLifecycle` type

Implement only semantic state and deterministic methods.

No D3D12Hook changes yet except source registration if desired.

### Step 2 — Move event kind + begin state

Replace nested enum and `begin_xefg_resize_event` backing state.

Compile.

### Step 3 — Move hold arm/complete/clear state

Keep D3D12Hook wrappers/logging.

Compile.

### Step 4 — Move Present suppressed-count state

Mechanically change direct member access to lifecycle queries.

Compile.

### Step 5 — Move post-resize diagnostic state

Use a returned sample object; preserve log formatting.

Compile.

### Step 6 — Remove old fields/enum

Run zero-reference audit.

### Step 7 — Audit binding/unhook clear call sites

Ensure R7 paths still clear stale hold.

### Step 8 — Validate callback diff

Confirm no physical callback order changed beyond field-to-method substitutions.

---

# 33. Expected Diff Shape

Expected files:

```text
ADD    src/compatibility/xefg/XeFGResizeLifecycle.hpp
ADD    src/compatibility/xefg/XeFGResizeLifecycle.cpp
MODIFY src/D3D12Hook.hpp
MODIFY src/D3D12Hook.cpp
MODIFY CMakeLists.txt
```

Preferred effective implementation size:

```text
~150–280 LOC
```

GitHub add+delete may be larger because state + helper logic moves.

Do not split the lifecycle object into multiple PRs merely to satisfy LOC if doing so leaves duplicate state.

One semantic state cut is preferable.

---

# 34. Build / Static Validation

Required before PR completion:

```text
git diff --check
cmake -S . -B build
cmake --build build --config Release --target REFramework
```

Use the repository's established Windows/MSBuild invocation as necessary, for example:

```text
cmake --build build --config Release --target REFramework -- /m:4
```

Also perform source audits:

```text
old resize lifecycle fields: zero references
old nested resize enum: zero references if removed
new lifecycle files: registered in tracked CMakeLists.txt
cmake.toml: unchanged
GameIdentity MHW gate: unchanged in behavior
Present original-call position: unchanged
Resize original-call positions: unchanged
```

---

# 35. Runtime Validation Policy

The fine-grained split plan does not require a full runtime wave after every low-risk ownership extraction; the next formal lifecycle wave is after R9.

However, because R8 touches state consumed by Present and Resize callbacks, a short smoke is strongly recommended if convenient.

Preferred smoke:

```text
Dragon's Dogma 2 + OptiScaler + XeFG
    launch
    REFramework overlay visible
    OptiScaler overlay visible
    Alt+Tab smoke
    no rebind loop
    no crash
```

For the actual MHW-only hold behavior, Monster Hunter Wilds is the authoritative runtime case if available:

```text
MHW + OptiScaler + XeFG
    trigger known ResizeTarget lifecycle if reproducible
    hold arms only after renderer reset
    Present continues
    renderer callback suppressed while hold active
    successful ResizeBuffers/1 completes hold
```

Do not claim runtime validation unless actually performed.

A successful Release build is not equivalent to this runtime smoke.

---

# 36. PR Body Expectations

The PR body should explicitly state:

```text
- R8 only: XeFG resize lifecycle semantic state extraction
- physical DXGI callbacks remain in D3D12Hook
- MHW-only hold policy unchanged
- original Present/Resize forwarding unchanged
- failed completion still keeps hold active
- rebind/unhook stale-hold clearing preserved
- no timeout/heuristic recovery added
- logging levels/cleanup deferred to R11
```

Validation section should report only commands/tests actually run.

---

# 37. Review Classification Guidance

Use the project review philosophy.

## Blocking

Realistic behavior regressions such as:

- wrong hold completion on failure;
- native suppression;
- changed original-call order;
- MHW gate lost;
- stale hold lifetime regression;
- event id/trigger id semantics changed;
- physical hook ownership moved into lifecycle object;
- callback crash/null-deref introduced.

## Non-blocking

- small naming issue;
- minor comment mismatch;
- helper placement that does not alter behavior;
- harmless extra accessor that can be cleaned later.

## Theoretical only

Do not block on:

- pathological integer wraparound of `uint64_t` resize event id;
- impossible multi-billion-year steady-clock edge cases;
- speculative thread races not reachable under the existing hook-monitor mutex contract;
- style preference between `enum class` nesting choices.

R8 review should focus on realistic lifecycle behavior.

---

# 38. Merge Gate Checklist

Do not merge R8 unless all are true:

```text
[ ] latest master base is used
[ ] XeFGResizeLifecycle owns all intended resize semantic state
[ ] old duplicate lifecycle fields are removed
[ ] physical DXGI callbacks remain D3D12Hook-owned
[ ] MHW-only hold arm policy is unchanged
[ ] observe-only policy is unchanged
[ ] Present/Present1 originals still forward during hold
[ ] renderer callback suppression behavior is unchanged
[ ] failed ResizeBuffers/1 completion keeps hold
[ ] successful ResizeBuffers/1 completion clears hold
[ ] failed ResizeTarget clears hold
[ ] rebind/external-bind/unhook clear stale hold
[ ] no timeout/Present-count recovery exists
[ ] native source cannot be suppressed by XeFG lifecycle state
[ ] post-resize diagnostic budget remains 3 if moved
[ ] tracked CMakeLists.txt registers new files
[ ] cmake.toml is untouched
[ ] Release build passes
[ ] git diff --check passes
[ ] no unrelated cleanup
```

---

# 39. Non-Goals

R8 is not the place to:

- redesign DXGI resize handling generally;
- remove nested recursion compatibility logic;
- optimize callstack logging;
- move renderer callbacks into the compatibility layer;
- make MHW hold global;
- add DD2-specific resize behavior;
- add new XeFG recovery heuristics;
- change the hook monitor;
- change OptiScaler/XeFG discovery;
- change queue selection;
- change active binding ownership;
- clean final user logs;
- add Debug Logging UI.

---

# 40. Completion Definition

R8 is complete when this statement is true:

> `D3D12Hook` still physically handles exactly the same Present/Resize DXGI calls in exactly the same behavioral order, but all XeFG resize event/hold/post-resize state is owned by one dedicated `XeFGResizeLifecycle` object with deterministic begin/arm/complete/clear/suppressed-present transitions.

The expected architecture after R8 is:

```text
XeFGRuntimeRegistry          [R1]
XeFGCompatibility            [R2]
XeFGDiscovery observation    [R3]
XeFG validated candidate     [R4]
XeFGCandidateHandoff         [R5]
XeFGBinding                  [R6]
D3D12Hook bind transaction   [R7]
XeFGResizeLifecycle          [R8]

D3D12Hook
    still owns:
      physical Present/Present1 callbacks
      physical ResizeTarget/ResizeBuffers/ResizeBuffers1 callbacks
      renderer callback invocation
      MHW-specific arm policy
      logging context
```

That prepares the codebase for R9, where the callbacks themselves can be reduced to narrow semantic queries/notifications without changing the lifecycle state model again.
