# Work Order: XeFG Refactor R10 — Hook-Monitor Isolation and Final Upstream-Surface Cleanup

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `f23f96ea5e79792da485d0fefef87c5e36b830c1` (`Refactor R9: reduce XeFG Present and resize callback logic (#27)`)

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
- R9 / PR #27 — narrow XeFG semantic helpers in physical Present/resize callbacks

Related documents:

- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_ARCHITECTURE_2026-09-06.md`
- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_PR_SPLIT_PLAN_2026-09-06.md`
- `doc/REFramework_XeFG_P3_LIFECYCLE_ROBUSTNESS_PLAN_2026-09-06.md`
- `doc/work-order/XEFG_REFACTOR_R9_PRESENT_RESIZE_CALLBACK_CLEANUP.md`

This work order implements **R10 only** from the fine-grained XeFG refactor plan.

The fork remains **unreleased**. R10 is therefore the last architectural cleanup before the deliberately separate R11 logging/debug-UI pass. Fork-only bridge APIs that are no longer required should be removed now rather than retained for compatibility with an unreleased implementation.

Runtime behavior is still the contract. R10 must not redesign the working XeFG presentation/binding/resize behavior merely because this is the final structural PR.

---

# 1. Recommended PR Identity

Suggested branch:

```text
refactor/xefg-r10-hook-monitor-surface-cleanup
```

Suggested PR title:

```text
Refactor R10: isolate XeFG hook-monitor policy and final core surface
```

Suggested commit title:

```text
refactor: isolate XeFG hook-monitor policy
```

One primary responsibility:

> Finish the R1–R10 architecture by moving the remaining XeFG hook-monitor policy and public runtime-dispatch ownership out of upstream-sensitive `REFramework.cpp` / `D3D12Hook.hpp`, leaving only intentional physical D3D12 bridge points.

R10 has two closely related cleanup parts because they share the same final goal: **reduce XeFG knowledge in core/upstream-sensitive code**.

Do not start R11 logging policy or configuration UI in this PR.

---

# 2. Release / Development Policy

The remaining sequence is:

```text
R10 hook-monitor + final upstream-surface cleanup
R11 final logging levels + persistent Debug Logging UI
final runtime validation
release
```

Because the fork is unreleased, R10 may:

- remove obsolete public XeFG methods from `D3D12Hook.hpp` immediately after callers migrate;
- move XeFG API dispatch / InitDesc publication code into the existing compatibility façade without compatibility shims;
- move R9 callback-only helpers out of the public section;
- remove now-unnecessary `D3D12Hook.hpp` includes after forward declarations are sufficient;
- remove file-local XeFG dispatch state from `D3D12Hook.cpp` once moved.

Still forbidden:

- changing hook-monitor timeout values;
- changing last-chance timing;
- changing `try_lock()` / lifecycle-mutex behavior;
- changing generic/native D3D recovery;
- protecting an incomplete/stale XeFG binding from recovery;
- changing R7 bind/rebind transaction ordering;
- changing R8 resize lifecycle semantics;
- changing R9 Present/resize ordering;
- changing MHW-only hold policy;
- changing hook slots;
- changing queue-selection policy;
- changing public XeFG proxy authority;
- adding timeout/sleep/poll/retry recovery;
- changing log levels or removing diagnostics;
- adding Debug Logging UI;
- FSRFG/DLSSG/Streamline redesign;
- generic frame-generation abstractions;
- unrelated REFramework cleanup.

---

# 3. Current State After R9

R1–R9 successfully moved most XeFG implementation into `src/compatibility/xefg`, but two upstream-sensitive surfaces still know more than necessary.

## 3.1 `REFramework::hook_monitor()` still owns XeFG preservation policy

Current master contains logic equivalent to:

```cpp
const bool preserve_xefg_binding = !m_is_d3d11
    && d3d12 != nullptr
    && d3d12->has_active_xefg_instance_binding();

if (preserve_xefg_binding) {
    spdlog::info(
        "[XeFG][HookMonitor] action = preserve_binding, "
        "reason = present_timeout, generation = {}, "
        "swapchain = 0x{:x}, queue = 0x{:x}",
        d3d12->get_xefg_binding_generation(),
        reinterpret_cast<uintptr_t>(d3d12->get_swap_chain()),
        reinterpret_cast<uintptr_t>(d3d12->get_command_queue()));
    d3d12->log_hook_monitor_snapshot("xefg_rehook_suppressed");
} else {
    // generic D3D recovery
}
```

The behavior is correct, but the ownership is not final: the generic core directly knows the XeFG health predicate and XeFG binding diagnostics.

## 3.2 `D3D12Hook.hpp` still publicly exposes XeFG runtime dispatch

Current public surface still contains:

```cpp
using XefgInitFn = ...;
using XefgGetSwapchainFn = ...;

static int32_t xefg_init_desc_dispatch(...);
static int32_t xefg_get_swapchain_dispatch(...);

bool has_active_xefg_instance_binding() const noexcept;
```

The runtime registry currently includes `D3D12Hook.hpp` only so its stable per-slot thunks can call those public static dispatchers.

That makes the dependency look like:

```text
XeFGRuntimeRegistry
    -> D3D12Hook
        -> XeFGDiscovery / candidate publication
```

After R1–R9, the physical D3D12 hook no longer needs to be the owner of public XeFG API dispatch.

## 3.3 R9 callback-only helpers are public

R9 intentionally added narrow helpers first so callback behavior could be reviewed separately:

```cpp
is_xefg_source()
is_tracked_xefg_instance(...)
is_xefg_render_capable()
is_xefg_resize_hold_active()
should_suppress_xefg_render_callbacks()
note_xefg_suppressed_present()
begin_tracked_xefg_resize_event(...)
```

These are useful to `D3D12Hook` physical callbacks, but they do not need to be general public API if no external caller requires them.

R10 should perform that caller audit and reduce their visibility.

---

# 4. Target Architecture After R10

The intended dependency after this PR is:

```text
REFramework.cpp
    -> XeFGCompatibility façade
       only at valid integration points:
       - loader/module handoff
       - pending work processing
       - hook-monitor timeout decision

XeFGRuntimeRegistry
    -> stable exact-slot thunks
    -> narrow XeFGCompatibility dispatch entry

XeFGCompatibility
    -> XeFGRuntimeRegistry
    -> XeFGDiscovery
    -> XeFGCandidateHandoff
    -> narrow friend/bridge access to D3D12Hook when physical state is required

D3D12Hook
    -> native REFramework D3D12 mechanics
    -> physical XeFG instance Present/resize hook targets
    -> candidate application / physical bind transaction bridge
    -> XeFGBinding + XeFGResizeLifecycle semantic state used by physical callbacks
```

The important result is not zero references to XeFG in `D3D12Hook`.

The important result is:

> `D3D12Hook` no longer acts as the process-level XeFG API/runtime dispatch implementation, and `REFramework::hook_monitor()` no longer reconstructs XeFG health policy.

---

# 5. Expected File Scope

Expected modified files:

```text
MODIFY src/REFramework.cpp
MODIFY src/D3D12Hook.hpp
MODIFY src/D3D12Hook.cpp
MODIFY src/compatibility/xefg/XeFGCompatibility.hpp
MODIFY src/compatibility/xefg/XeFGCompatibility.cpp
MODIFY src/compatibility/xefg/XeFGRuntimeRegistry.cpp
```

Possible but only if compilation requires it:

```text
MODIFY src/compatibility/xefg/XeFGRuntimeRegistry.hpp
```

Normally unchanged:

```text
src/compatibility/xefg/XeFGBinding.*
src/compatibility/xefg/XeFGResizeLifecycle.*
src/compatibility/xefg/XeFGDiscovery.*
src/compatibility/xefg/XeFGCandidateHandoff.*
CMakeLists.txt
cmake.toml
```

No new source file is expected. Use the existing `XeFGCompatibility` façade.

---

# 6. Part A — Move Hook-Monitor Policy Behind `XeFGCompatibility`

## 6.1 Generic monitor timing must remain byte-for-byte equivalent in behavior

Do not change the outer monitor flow:

```text
m_do_not_hook_d3d_count gate
-> try_lock hook monitor mutex
-> process pending XeFG work at current point
-> determine renderer type
-> five-second present timeout
-> one-second last-chance window
-> preservation/recovery decision
-> reset monitor timestamps
```

Do not move the XeFG decision earlier than the current final recovery decision.

Do not skip `m_has_last_chance` behavior.

Do not change:

```cpp
std::chrono::seconds(5)
std::chrono::seconds(1)
```

R10 isolates the decision; it does not tune the monitor.

## 6.2 Recommended semantic façade API

Exact naming may differ. A clean form is conceptually:

```cpp
class D3D12Hook;

class XeFGCompatibility {
public:
    enum class HookMonitorAction : uint8_t {
        AllowGenericRecovery,
        PreserveActiveBinding,
    };

    static HookMonitorAction on_present_timeout(
        D3D12Hook& hook) noexcept;
};
```

Alternative acceptable form:

```cpp
static bool should_preserve_active_binding_on_monitor_timeout(
    const D3D12Hook& hook) noexcept;
```

If the pure-query variant is used, preserve the existing XeFG log content without forcing `REFramework.cpp` to reconstruct the health predicate from raw fields.

Preferred result for the core is approximately:

```cpp
const bool preserve_xefg_binding =
    !m_is_d3d11
    && d3d12 != nullptr
    && XeFGCompatibility::on_present_timeout(*d3d12)
        == XeFGCompatibility::HookMonitorAction::PreserveActiveBinding;

if (!preserve_xefg_binding) {
    spdlog::info("Sending rehook request for D3D");
    if (d3d12 != nullptr) {
        d3d12->log_hook_monitor_snapshot("rehook_request");
    }

    if (m_is_d3d11) {
        hook_d3d11();
    } else {
        if (d3d12 != nullptr) {
            spdlog::info(
                "[D3D12][HookLifecycle] action = hook, "
                "reason = hook_monitor_recovery");
        }
        hook_d3d12();
    }
}
```

The generic recovery branch should otherwise remain unchanged.

---

# 7. Hook-Monitor Preservation Predicate — Exact Contract

The current healthy-binding predicate is a compatibility contract.

It currently requires all of the following:

```text
D3D12 hook reports hooked
AND not phase 1
AND source == XeFGInternal
AND XeFGBinding active/complete enough for current semantics
AND physical swapchain VtableHook exists
AND binding-owned swapchain/queue/device aliases match D3D12Hook raw aliases
```

Equivalent current logic:

```cpp
return m_hooked
    && !m_is_phase_1
    && m_swapchain_source == SwapchainSource::XeFGInternal
    && m_xefg_binding.active()
    && m_swapchain_hook != nullptr
    && m_xefg_binding.aliases_match(
        m_swap_chain,
        m_command_queue,
        m_device);
```

R10 must preserve this exact truth table unless a stricter check is already proven equivalent by current code.

Do **not** weaken it to:

```cpp
XeFGCompatibility::is_module_loaded()
```

or:

```cpp
m_swapchain_source == SwapchainSource::XeFGInternal
```

or:

```cpp
m_xefg_binding.active()
```

alone.

A stale/partial binding must still be eligible for generic recovery.

---

# 8. Monitor Truth Table — Mandatory Static Audit

Review the final implementation against this table:

| State | Preserve timeout rehook? |
|---|---:|
| Native D3D12 | No |
| D3D11 active | No XeFG preservation decision; existing D3D11 recovery remains |
| XeFG module merely loaded | No |
| XeFG candidate pending but no active binding | No |
| `m_hooked == false` | No |
| phase 1 | No |
| XeFG source but missing `m_swapchain_hook` | No |
| XeFG binding inactive/incomplete | No |
| XeFG aliases mismatch raw D3D12 aliases | No |
| healthy XeFG render-capable binding | Yes |
| healthy XeFG observe-only binding | Yes |
| healthy XeFG binding while resize hold active | Yes |

Important:

> Resize hold is **not** a reason to invalidate an otherwise healthy binding.

Real Present activity normally maintains liveness during the hold, but if the generic timeout path is nevertheless reached, the healthy binding must still be protected.

---

# 9. Preserve Existing Hook-Monitor Logging Until R11

R10 is not the logging cleanup PR.

The existing preservation message contains:

```text
[XeFG][HookMonitor]
action = preserve_binding
reason = present_timeout
generation
swapchain
queue
```

and the existing D3D12 monitor snapshot:

```text
xefg_rehook_suppressed
```

Keep those diagnostics available with equivalent timing/content.

It is acceptable to **move** the existing XeFG preservation log into `XeFGCompatibility.cpp` because the policy moves there.

Do not:

- downgrade it to debug yet;
- remove generation/swapchain/queue data yet;
- add new high-frequency logs;
- perform broad message rewriting.

R11 will classify user-facing vs debug logs after the final call sites are stable.

---

# 10. Recommended Narrow Friend Bridge for Monitor Health

The façade needs to evaluate physical D3D12 hook state, but the core should not make all of that state public.

A narrow friend bridge is acceptable:

```cpp
class XeFGCompatibility;

class D3D12Hook {
public:
    friend class XeFGCandidateHandoff;
    friend class XeFGCompatibility;

    // ...
};
```

Then either:

1. `XeFGCompatibility` evaluates the exact private physical-health predicate directly; or
2. `D3D12Hook` keeps a non-public helper such as:

```cpp
bool has_healthy_xefg_physical_binding() const noexcept;
```

that only the façade/friend and internal code can use.

Do not keep `has_active_xefg_instance_binding()` public solely for `REFramework.cpp` after the façade caller is migrated.

Do not expose `m_xefg_binding`, `m_swapchain_hook`, or raw aliases publicly.

---

# 11. Part B — Move XeFG Runtime/API Dispatch Out of `D3D12Hook`

The runtime registry already owns exact-module slot allocation and stable thunk selection.

The physical D3D12 hook should no longer publicly own these process-level API dispatch entry points:

```cpp
D3D12Hook::xefg_init_desc_dispatch(...)
D3D12Hook::xefg_get_swapchain_dispatch(...)
D3D12Hook::xefg_init_desc_common(...)
D3D12Hook::publish_xefg_candidate(...)
```

Move their responsibility into the compatibility façade.

Suggested conceptual target:

```cpp
class XeFGCompatibility {
public:
    static int32_t dispatch_init_desc(
        size_t slot,
        void* context,
        HWND hwnd,
        const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
        ID3D12CommandQueue* queue,
        IDXGIFactory2* factory,
        const void* init_params);

    static int32_t dispatch_get_swapchain(
        size_t slot,
        void* context,
        REFIID riid,
        void** swapchain);
};
```

Exact names are not important.

The ownership is.

---

# 12. Runtime Registry Thunks After R10

Current thunks call `D3D12Hook`:

```cpp
return D3D12Hook::xefg_init_desc_dispatch(...);
```

After R10 they should call the compatibility-owned dispatch bridge:

```cpp
return XeFGCompatibility::dispatch_init_desc(
    Slot,
    context,
    hwnd,
    swap_chain_desc,
    fullscreen_desc,
    command_queue,
    factory,
    init_params);
```

and:

```cpp
return XeFGCompatibility::dispatch_get_swapchain(
    Slot,
    context,
    riid,
    swap_chain);
```

`XeFGRuntimeRegistry.cpp` should no longer need `D3D12Hook.hpp` merely for XeFG export dispatch.

Replace that dependency with the narrow compatibility façade include.

A small implementation-level callback from registry thunk to façade is acceptable here; the important architectural removal is that **runtime hook dispatch no longer enters the physical D3D12 hook class as its process-level API owner**.

---

# 13. InitDesc Dispatch Semantics Must Remain Exact

Moving the code must not change any ordering.

Required sequence remains:

```text
stable runtime slot thunk entered
-> resolve exact slot from XeFGRuntimeRegistry
-> obtain exact HMODULE + original InitFromSwapChainDesc
-> fail safely if slot/runtime not active
-> log existing RuntimeDispatch diagnostic
-> XeFGDiscovery::observe_init(...)
     - serialized outer InitDesc observation
     - temporary factory CreateSwapChainForHwnd[15] capture
     - original XeFG InitFromSwapChainDesc called exactly once
     - temporary capture removed after outer init returns
-> preserve existing InitDesc diagnostics
-> build validated binding candidate
-> if accepted, publish through XeFGCandidateHandoff
-> return original XeFG init result unchanged
```

Do not publish a candidate before the original XeFG init returns.

Do not call the original init twice.

Do not lose the exact runtime `HMODULE` associated with the slot.

Do not mutate API parameters.

---

# 14. Candidate Publication Semantics Must Remain Exact

Current publication does:

```cpp
XeFGBindingCandidateResult decision{};
{
    std::scoped_lock lock{g_xefg_state_mutex};
    decision = XeFGDiscovery::build_binding_candidate(observation);
    // existing accepted/rejected diagnostics
}

if (!decision.accepted()) {
    return;
}

XeFGCandidateHandoff::publish(
    std::move(*decision.candidate));
```

When moving this out of `D3D12Hook.cpp`:

- move the serialization mutex with the responsibility if it is used only by this dispatch/publication path;
- keep candidate validation before publication;
- keep rejected candidates non-mutating;
- keep publication through the existing R5 handoff;
- do not bypass `XeFGCandidateHandoff` by directly touching `g_d3d12_hook`;
- do not add retry/polling/background work.

Do not move R7 physical bind/rebind transaction into the façade in R10.

The façade publishes a candidate; `D3D12Hook` remains the physical binding target through the established handoff bridge.

---

# 15. `GetSwapChainPtr` Dispatch Semantics Must Remain Exact

The optional public proxy hook remains diagnostic/observational.

Required sequence:

```text
stable runtime slot thunk entered
-> resolve exact slot-specific original GetSwapChainPtr
-> if unavailable/inactive: current safe failure behavior
-> call original exactly once
-> return original result unchanged
-> on success with non-null proxy:
     compare/log against current internal presentation candidate
```

Do not turn `GetSwapChainPtr` into renderer authority.

Do not bind REFramework to the public XeFG interpolation proxy.

Do not make the optional hook required.

---

# 16. Remove Dispatch-Only State From `D3D12Hook.cpp`

After the move, audit whether the following are still needed in `D3D12Hook.cpp`:

```cpp
constexpr int32_t kXefgSuccess
std::mutex g_xefg_state_mutex
using XefgInitFn
using XefgGetSwapchainFn
#include "compatibility/xefg/XeFGRuntimeRegistry.hpp"
```

If they are now used only by compatibility dispatch, move/remove them from `D3D12Hook.cpp`.

Do not remove unrelated discovery diagnostics such as generic D3D12 dummy-swapchain logging merely because they are nearby.

---

# 17. `D3D12Hook.hpp` Final Surface Cleanup

After all R10 caller migrations, perform an explicit symbol-by-symbol audit.

## Remove from public surface if no longer externally required

Expected candidates:

```text
XefgInitFn
XefgGetSwapchainFn
xefg_init_desc_dispatch
xefg_get_swapchain_dispatch
has_active_xefg_instance_binding
```

## Move from public to protected/private if callback-only

Expected R9 candidates:

```text
is_xefg_source
is_tracked_xefg_instance
is_xefg_render_capable
is_xefg_resize_hold_active
should_suppress_xefg_render_callbacks
note_xefg_suppressed_present
begin_tracked_xefg_resize_event
```

These are physical callback implementation helpers, not general REFramework API.

## Keep public only where current external core/renderer diagnostics still require it

Examples may include, pending caller audit:

```text
get_xefg_last_resize_event_id
get_xefg_binding_generation
get_xefg_last_resize_kind
get_swapchain_source
log_hook_monitor_snapshot
```

Do not remove a getter that R11 still needs to compile existing diagnostics merely to make the header aesthetically smaller.

R10 is architectural cleanup, not a forced zero-XeFG public-header target.

---

# 18. Header Include Cleanup

Current `D3D12Hook.hpp` directly includes:

```cpp
#include "compatibility/xefg/XeFGDiscovery.hpp"
#include "compatibility/xefg/XeFGBinding.hpp"
#include "compatibility/xefg/XeFGResizeLifecycle.hpp"
```

After moving dispatch/publication declarations out, `XeFGDiscovery.hpp` may no longer be necessary in the header.

If the only remaining discovery type in a method signature is a reference to:

```cpp
XeFGBindingCandidate
```

prefer a forward declaration if valid:

```cpp
struct XeFGBindingCandidate;
```

Keep full includes for types stored by value as members:

```cpp
XeFGBinding m_xefg_binding;
XeFGResizeLifecycle m_xefg_resize_lifecycle;
```

Do not introduce fragile forward declarations for types whose complete definition is actually required.

---

# 19. Do Not Over-Clean the Physical Binding Bridge

The following remain legitimate `D3D12Hook` responsibilities after R10:

```text
apply_xefg_candidate(...)
bind_external_swapchain(...)
replace_xefg_binding(...)
physical VtableHook ownership
Present/Present1 callbacks
ResizeBuffers/ResizeTarget/ResizeBuffers1 callbacks
renderer reset ordering
raw D3D12 alias synchronization
MHW-only resize hold activation at callback boundary
```

Do not move these into `XeFGCompatibility` in R10.

That would collapse the carefully separated R7–R9 failure domains back into a large compatibility manager.

Conceptually after R10:

```text
XeFGCompatibility
    discovers / validates / publishes / advises monitor

D3D12Hook
    physically binds / hooks / presents / resizes
```

---

# 20. R7 Transaction Invariants Remain Frozen

R10 must not alter changed-object replacement ordering:

```text
validate candidate
-> acquire strong new ownership
-> prepare complete new VtableHook
-> preparation failure leaves old binding untouched
-> renderer reset
-> remove old hook while old COM ownership is alive
-> commit new XeFGBinding + aliases + hook
-> generation increment
```

Same-object update behavior also remains unchanged.

Do not use final-surface cleanup to combine or rewrite these functions.

---

# 21. R8/R9 Present and Resize Invariants Remain Frozen

R10 must not change:

```text
hold active
    -> REFramework render callback suppressed
    -> original Present / Present1 still forwarded
    -> real Present activity keeps monitor alive

successful ResizeBuffers / ResizeBuffers1
    -> hold completes

failed ResizeBuffers / ResizeBuffers1
    -> hold remains

failed ResizeTarget
    -> hold clears

MHW-only hold activation
    -> remains MHW-only
```

Do not move game policy into the compatibility façade as part of hook-monitor cleanup.

---

# 22. `process_pending_work()` Timing Is Frozen

Current hook monitor performs:

```cpp
if (d3d12 != nullptr) {
    XeFGCompatibility::process_pending_work();
}
```

before the renderer-type timeout decision.

Keep that relative placement unless compilation forces only a syntactic reshuffle with identical execution timing.

R10 must not turn pending work into:

- a background thread;
- periodic polling independent of the existing monitor;
- arbitrary sleeps;
- delayed candidate publication.

R2/R5 timing invariants remain valid.

---

# 23. Native / Generic Hook-Monitor Behavior Is Frozen

For native D3D12 without a healthy XeFG binding:

```text
present timeout
-> last chance
-> generic hook_d3d12 recovery
```

must remain exactly available.

For D3D11:

```text
present timeout
-> existing hook_d3d11 recovery
```

must remain unchanged.

R10 must not globally disable recovery merely because `libxess_fg.dll` was loaded once.

This is a merge-blocking invariant.

---

# 24. No FSRFG / DLSSG Behavior Changes

Do not alter:

```text
Streamline hooks
sl.dlss_g.dll handling
FSRFG behavior
native DXGI/D3D11 behavior
```

R10 remains XeFG-specific.

A generic `FrameGenerationHookMonitorPolicy` abstraction is explicitly out of scope.

---

# 25. Logging Is Still Deferred to R11

R10 may move an existing log together with the code that owns the policy.

R10 must not decide whether the log is suitable for end users.

After R10 merges, R11 will inspect the **actual final call sites** and implement:

```text
REFramework Configuration
    Debug Logging [ ]
```

with:

```text
default OFF
persistent through existing config system
concise support-critical normal logging
verbose investigation diagnostics behind debug mode
```

Do not add the setting in R10.

---

# 26. Suggested Implementation Sequence

A low-risk implementation order is:

## Step 1 — Add façade monitor API

Add `HookMonitorAction` or equivalent and implement the exact current healthy-binding predicate.

Do not change the core caller yet beyond compilation scaffolding.

## Step 2 — Migrate `REFramework::hook_monitor()`

Replace direct:

```cpp
d3d12->has_active_xefg_instance_binding()
```

with the façade decision.

Preserve generic recovery and timestamp-reset code exactly.

## Step 3 — Move RuntimeDispatch / InitDesc / public-proxy dispatch

Move the current code from `D3D12Hook.cpp` into `XeFGCompatibility.cpp` with minimal textual/ordering changes.

Do not rewrite while moving.

## Step 4 — Retarget fixed registry thunks

Change only the thunk destination from `D3D12Hook` to `XeFGCompatibility`.

Audit exact slot identity.

## Step 5 — Remove obsolete D3D12 declarations/definitions

Remove runtime dispatch typedefs/functions and dispatch-only file statics/includes.

## Step 6 — Reduce R9 helper visibility

Move callback-only helpers out of public scope after confirming no external callers.

## Step 7 — Header/include audit

Remove `XeFGDiscovery.hpp` from `D3D12Hook.hpp` if a forward declaration is sufficient.

## Step 8 — Final behavior diff audit

Review native D3D monitor, exact runtime dispatch, candidate publication, R7 transaction, R8/R9 callbacks independently.

This order keeps failures attributable.

---

# 27. Code Example — Compatibility-Owned Monitor Decision

One acceptable shape:

```cpp
// XeFGCompatibility.hpp
class D3D12Hook;

class XeFGCompatibility {
public:
    enum class HookMonitorAction : uint8_t {
        AllowGenericRecovery,
        PreserveActiveBinding,
    };

    static HookMonitorAction on_present_timeout(
        D3D12Hook& hook) noexcept;

    // existing loader/runtime APIs...
};
```

```cpp
// XeFGCompatibility.cpp
XeFGCompatibility::HookMonitorAction
XeFGCompatibility::on_present_timeout(D3D12Hook& hook) noexcept {
    const bool healthy =
        hook.m_hooked
        && !hook.m_is_phase_1
        && hook.m_swapchain_source == D3D12Hook::SwapchainSource::XeFGInternal
        && hook.m_xefg_binding.active()
        && hook.m_swapchain_hook != nullptr
        && hook.m_xefg_binding.aliases_match(
            hook.m_swap_chain,
            hook.m_command_queue,
            hook.m_device);

    if (!healthy) {
        return HookMonitorAction::AllowGenericRecovery;
    }

    spdlog::info(
        "[XeFG][HookMonitor] action = preserve_binding, "
        "reason = present_timeout, generation = {}, "
        "swapchain = 0x{:x}, queue = 0x{:x}",
        hook.m_xefg_binding.generation(),
        reinterpret_cast<uintptr_t>(hook.m_swap_chain),
        reinterpret_cast<uintptr_t>(hook.m_command_queue));

    hook.log_hook_monitor_snapshot("xefg_rehook_suppressed");
    return HookMonitorAction::PreserveActiveBinding;
}
```

This example assumes `XeFGCompatibility` is a friend of `D3D12Hook`.

A private D3D12 health helper is also acceptable if it keeps the same truth table.

---

# 28. Code Example — Core Hook Monitor After Isolation

Conceptual target:

```cpp
const bool preserve_xefg_binding =
    !m_is_d3d11
    && d3d12 != nullptr
    && XeFGCompatibility::on_present_timeout(*d3d12)
        == XeFGCompatibility::HookMonitorAction::PreserveActiveBinding;

if (!preserve_xefg_binding) {
    spdlog::info("Sending rehook request for D3D");

    if (d3d12 != nullptr) {
        d3d12->log_hook_monitor_snapshot("rehook_request");
    }

    if (m_is_d3d11) {
        hook_d3d11();
    } else {
        if (d3d12 != nullptr) {
            spdlog::info(
                "[D3D12][HookLifecycle] action = hook, "
                "reason = hook_monitor_recovery");
        }
        hook_d3d12();
    }
}

// Existing monitor timestamp reset stays exactly where it is now.
```

Do not move the timestamp reset inside either branch.

---

# 29. Code Example — Runtime Thunk Destination

Before:

```cpp
template <size_t Slot>
int32_t WINAPI xefg_init_desc_thunk(...) {
    return D3D12Hook::xefg_init_desc_dispatch(
        Slot, ...);
}
```

After:

```cpp
template <size_t Slot>
int32_t WINAPI xefg_init_desc_thunk(...) {
    return XeFGCompatibility::dispatch_init_desc(
        Slot, ...);
}
```

Same for `GetSwapChainPtr`.

The fixed 8-slot thunk table and exact-module registry behavior must remain unchanged.

---

# 30. Code Example — Dispatch-Owned Candidate Publication

Conceptual compatibility implementation:

```cpp
int32_t XeFGCompatibility::dispatch_init_desc(
    size_t slot,
    void* context,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    ID3D12CommandQueue* command_queue,
    IDXGIFactory2* factory,
    const void* init_params) {

    const auto target = XeFGRuntimeRegistry::instance().resolve_init(slot);
    if (!target) {
        // preserve existing failure log / result
        return -1;
    }

    auto observation_scope = XeFGDiscovery::observe_init(
        target->original,
        context,
        hwnd,
        swap_chain_desc,
        fullscreen_desc,
        command_queue,
        factory,
        init_params);

    const auto& observation = observation_scope.observation;

    XeFGBindingCandidateResult decision{};
    {
        std::scoped_lock lock{s_dispatch_state_mutex};
        decision = XeFGDiscovery::build_binding_candidate(observation);
        // preserve current accepted/rejected diagnostics
    }

    if (decision.accepted()) {
        XeFGCandidateHandoff::publish(
            std::move(*decision.candidate));
    }

    return observation.init_result;
}
```

Do not copy this mechanically if it changes current diagnostic ordering. Prefer moving the existing body with the smallest possible semantic diff.

---

# 31. Required Static Validation

Before opening the PR, explicitly audit:

## Hook monitor

```text
healthy XeFG -> preserve
every incomplete/stale case -> generic recovery remains
native D3D12 -> generic recovery unchanged
D3D11 -> recovery unchanged
monitor timing constants unchanged
last-chance flow unchanged
monitor mutex flow unchanged
```

## Runtime registry / dispatch

```text
8 stable thunks unchanged
slot N resolves original for slot N
exact HMODULE retained
InitDesc original called exactly once
GetSwapChainPtr original called exactly once
optional GetSwapChainPtr remains optional
candidate publication still post-init
rejected candidate does not mutate active binding
```

## D3D12 physical behavior

```text
R7 bind/rebind code unchanged
R8 lifecycle transitions unchanged
R9 Present/resize callback ordering unchanged
MHW-only condition unchanged
five XeFG hook slots unchanged
```

## Header surface

```text
no runtime dispatch API remains public on D3D12Hook
callback-only R9 helpers are non-public unless an actual external caller requires them
no duplicate obsolete typedefs/statics remain
no unnecessary XeFGDiscovery include in D3D12Hook.hpp if forward declaration suffices
```

---

# 32. Build / Diff Validation

Required:

```text
cmake --build build --config Release --target REFramework -- /m:1 /v:q
git diff --check
```

Also audit the checked-in build files, but R10 should not require new source registration because no new source file is expected.

Do not regenerate `CMakeLists.txt` broadly.

---

# 33. Runtime Gate After R10 — Final Architecture Wave

R10 completes the R1–R10 architectural refactor, so a runtime wave is strongly recommended before R11 changes logging.

## A. Native D3D12 without OptiScaler/XeFG

Verify:

```text
REFramework starts normally
overlay opens
Lua/plugins/mod initialization unaffected
Present/resize behavior normal
no XeFG preservation when inactive
generic hook-monitor recovery remains available
```

## B. Dragon's Dogma 2 + OptiScaler + Intel XeFG

Verify:

```text
XeFG initializes
OptiScaler overlay visible
REFramework overlay visible
validated internal presentation swapchain remains target
presentation queue remains selected
repeated Alt+Tab survives
no destructive hook-monitor rehook loop
binding generation remains sensible
```

## C. Monster Hunter Wilds if available

Exercise ResizeTarget / mode transition:

```text
renderer reset occurs
MHW-only hold arms
Present/Present1 continue forwarding while held
successful ResizeBuffers/ResizeBuffers1 completes hold
REFramework rendering returns afterward
```

## D. Multi-runtime / Pragmata path if available

Verify:

```text
multiple exact runtime slots remain independent
no launch regression
no wrong-original dispatch between runtime modules
```

Do not broaden R10 merely because one runtime environment is unavailable for local testing. Record what was actually tested.

---

# 34. Merge-Blocking Review Findings

Treat the following as blockers:

1. Healthy XeFG binding can again be torn down by generic present-timeout recovery.
2. XeFG module-loaded state alone suppresses generic recovery.
3. Incomplete/stale binding is preserved.
4. D3D11/native recovery timing or branching changes.
5. `m_has_last_chance`, monitor mutex, or timeout constants change.
6. Runtime slot N can dispatch to another module's original function.
7. InitDesc original can be called zero/two times on normal active-slot path.
8. Candidate is published before outer InitDesc returns.
9. Public `GetSwapChainPtr` becomes renderer authority.
10. R7 physical binding transaction is moved/rewritten.
11. R8 hold success/failure semantics change.
12. Present/Present1 forwarding or callback ordering changes.
13. MHW-only hold becomes generic.
14. FSRFG/DLSSG/Streamline behavior is changed.
15. New polling/sleep/retry/background monitor logic is added.
16. XeFG runtime dispatch remains publicly owned by `D3D12Hook` without a concrete unavoidable reason.
17. Callback-only R9 helpers remain public solely by accident after caller audit.
18. R11 logging/UI work is mixed into this PR.

---

# 35. Non-Blocking / Do Not Over-Review

Do not block R10 for:

- minor helper naming;
- whether the monitor result is a bool vs small enum;
- a narrow friend declaration to `XeFGCompatibility`;
- exact placement of a moved log if runtime timing is equivalent;
- one extra private helper that improves auditability;
- leaving a diagnostic getter public because current R11-bound logging still needs it;
- small formatting differences;
- speculative races that are impossible under the existing hook-monitor lifecycle mutex contract.

The review standard remains realistic/material defects, not theoretical cleanup perfection.

---

# 36. Expected End State

After R10 the architecture should read approximately:

```text
REFramework core
    loader event -> XeFGCompatibility
    pending work -> XeFGCompatibility
    monitor timeout -> XeFGCompatibility semantic decision

XeFGCompatibility
    runtime API dispatch coordination
    discovery/candidate publication coordination
    monitor preservation policy

XeFGRuntimeRegistry
    exact HMODULE + slot + FunctionHook + stable thunks

XeFGDiscovery
    InitDesc observation + factory capture + candidate validation

XeFGCandidateHandoff
    immediate/pending candidate delivery

XeFGBinding
    active strong COM ownership + identity/mode/generation

XeFGResizeLifecycle
    resize/hold semantic state

D3D12Hook
    native D3D12 mechanics
    physical XeFG swapchain VtableHook
    transactional physical binding application
    Present/Present1/Resize physical callbacks
```

At that point the architectural refactor is complete.

The next PR is **R11 only**:

```text
final logging classification
persistent Configuration -> Debug Logging checkbox
default OFF
normal support-critical logs retained
verbose P1-P3/R1-R10 diagnostics debug-gated
final release validation
```
