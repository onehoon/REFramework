# XeFG Special K Removal — P3.3R Hook-Monitor Binding Preservation

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch for implementation: `master`  
Suggested implementation branch: `fix/xefg-p3-3r-hook-monitor-binding-preservation`  
Suggested PR title: `P3.3R: preserve active XeFG binding on hook-monitor timeout`

## 1. Purpose

P2 through P2.2 established the first working Special-K-free Intel XeFG render path. P3.1 added strong ownership and complete XeFG binding identity. P3.2 added serialized replacement of changed XeFG internal bindings. P3.3A added resize-lifecycle diagnostics. PR #14 then added exact-HMODULE multi-runtime XeFG API discovery for OptiScaler configurations that load more than one `libxess_fg.dll`.

The target topology remains:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (this fork)
Special K    = absent
FG Output    = Intel XeFG
Renderer     = D3D12
```

This work order addresses one narrow recovery defect:

> When REFramework already owns a valid, active `XeFGInternal` presentation binding and Present activity stops long enough to trigger the generic D3D hook monitor, the monitor currently destroys that valid XeFG binding by calling the generic `hook_d3d12()` recovery path. The newly created `D3D12Hook` then falls back to native phase-1 DXGI discovery, which is known not to receive the real XeFG presentation path. The result is a self-sustaining ~11-second unhook/rehook loop.

P3.3R must prevent that destructive recovery when the active XeFG instance binding is still structurally intact.

The intended behavior is:

```text
active XeFGInternal instance binding
    + no Present for hook-monitor timeout
    -> keep the existing D3D12Hook object
    -> keep the existing XeFG VtableHook
    -> keep owned swapchain / queue / device
    -> do NOT call generic hook_d3d12()
    -> re-arm only the monitor recovery window
    -> allow Present/Present1 to resume through the existing hook if presentation resumes
    -> allow P3.2 to replace the binding if a later validated XeFG InitDesc publishes a changed binding
```

This is intentionally a **small recovery hotfix**, not the MHW Alt+Enter crash fix.

P3.3R must not attempt to solve the `ResizeBuffers -> E_PENDING` crash. That work remains a separate P3.3B lifecycle task.

---

## 2. Required Base

Implement against current `master`:

```text
b4ff1092a16ee85a3ce4f9a017395d797a4d233d
fix: support multiple XeFG runtime modules (#14)
```

This base already includes:

```text
P2       direct XeFG presentation binding + Present1
P2.1     presentation-queue identity and render queue selection
P2.2     ResizeBuffers1 pre-reset
P3.1     strong binding ownership and identity
P3.2     atomic changed-binding replacement
P3.3A    MHW resize lifecycle diagnostics
PR #14   multiple same-basename XeFG runtime module support
```

If `master` advances before implementation:

1. rebase onto the then-current `master`,
2. inspect any changes to `REFramework::hook_monitor()`, `REFramework::hook_d3d12()`, `D3D12Hook::unhook()`, and XeFG binding ownership,
3. preserve the semantics described below rather than blindly applying the example diff.

Read before editing:

```text
doc/REFramework_OptiScaler_XeFG_SpecialK_Removal_Analysis_Plan_2026-09-05.md
doc/REFramework_XeFG_P3_LIFECYCLE_ROBUSTNESS_PLAN_2026-09-06.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P3_1_BINDING_OWNERSHIP_IDENTITY.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P3_2_ATOMIC_BINDING_REPLACEMENT.md
doc/work-order/XEFG_SPECIALK_REMOVAL_P3_3A_MHW_ALTENTER_RESIZE_LIFECYCLE_DIAGNOSTICS.md
doc/work-order/XEFG_SPECIALK_REMOVAL_MULTI_MODULE_XEFG_RUNTIME_SUPPORT.md
src/D3D12Hook.hpp
src/D3D12Hook.cpp
src/REFramework.cpp
src/REFramework.hpp
```

Do not modify the P3.2 replacement transaction or PR #14 runtime registry as part of this PR.

---

## 3. Runtime Evidence — Rehook Is a Downstream Recovery Defect

The 2026-09-06 Monster Hunter Wilds P3.3A + multi-module test session used:

```text
REF commit:
cebe978ed82e85027d94bdea1fede6216cc14d0d

OptiScaler:
0.9.5-pre4 (8dac650)
```

The session reproduced the MHW Alt+Enter `E_PENDING` failure and then remained alive long enough for the REF hook monitor to repeatedly recover.

### 3.1 Important classification correction

The periodic rehook loop did **not** begin during healthy presentation.

The first D3D12 hook occurred at:

```text
13:41:45.896  Hooking D3D12
```

No hook-monitor recovery occurred for the following healthy presentation period.

The Alt+Enter failure boundary occurred around:

```text
13:44:41.xxx  ResizeTarget / Present1 transition
13:44:41.904  OptiScaler ResizeBuffers -> E_PENDING
```

Only after presentation stopped did the first monitor event occur:

```text
13:44:46.555  Last chance encountered for hooking
13:44:47.556  Sending rehook request for D3D
```

Therefore, for this session:

```text
Alt+Enter E_PENDING
    -> presentation stops
    -> hook monitor timeout
    -> destructive generic recovery
    -> persistent phase-1 rehook loop
```

P3.3R fixes the final two steps only.

It does **not** fix the initial `E_PENDING`.

### 3.2 The first recovery request still has the valid XeFG binding

Immediately before the first destructive recovery:

```text
13:44:46.555
[D3D12][HookMonitor]
event                = last_chance
is_hooked            = true
is_phase_1           = false
inside_present       = false
active_swapchain     = 0x74b2cc0
active_device        = 0x598d18e0
active_command_queue = 0x89697e90
present_entry_count  = 19203
xefg_module_loaded   = true
last_present_age_ms  = 5246
```

One second later:

```text
13:44:47.556
[D3D12][HookMonitor]
event                = rehook_request
is_hooked            = true
is_phase_1           = false
active_swapchain     = 0x74b2cc0
active_device        = 0x598d18e0
active_command_queue = 0x89697e90
present_entry_count  = 19203
last_present_age_ms  = 6247
```

This is the most important evidence for this PR:

> The monitor is not recovering an already-lost XeFG binding at the first timeout. It is destroying a binding that is still active, owned, hooked, and in instance mode.

### 3.3 Generic recovery immediately destroys the useful state

The very next lines are:

```text
13:44:47.556
[D3D12][HookLifecycle] action = hook, reason = hook_monitor_recovery

13:44:47.558
Unhooking D3D12

13:44:47.558
Hooking D3D12
```

The replacement hook then performs the generic dummy-device / dummy-swapchain discovery path:

```text
Creating dummy device
Creating dummy DXGI factory
Creating dummy command queue
Creating dummy swapchain
...
[D3D12][Discovery] Present[8] owner = C:\WINDOWS\system32\dxgi.dll
...
Initializing hooks
[D3D12][HookInstall] phase = phase1, slot = Present[8]
```

The next monitor snapshot confirms the result:

```text
13:44:57.889
[D3D12][HookMonitor]
event                = last_chance
is_hooked            = true
is_phase_1           = true
active_swapchain     = 0x0
active_device        = 0x0
active_command_queue = 0x0
present_entry_count  = 0
xefg_module_loaded   = true
last_present_age_ms  = -1
```

The original external XeFG binding has been discarded.

### 3.4 Why the new hook cannot rediscover the same XeFG binding

The XeFG binding handoff is intentionally one-shot:

```text
XeFG InitFromSwapChainDesc
    -> capture internal presentation swapchain
    -> publish PendingXefgBinding
    -> D3D12Hook::hook()
    -> consume_pending_xefg_binding()
    -> bind_external_swapchain()
    -> pending binding cleared
```

`consume_pending_xefg_binding()` removes the pending binding when it is consumed.

If the generic monitor later destroys the active `D3D12Hook` object without a new XeFG InitDesc transaction, the replacement hook has no pending external binding to consume.

It therefore falls back to the generic native discovery path.

P1 already established that native phase-1 `Present[8]` discovery is not authoritative for the active XeFG presentation topology.

P3.3R must not attempt to invent another swapchain-discovery mechanism. It must simply stop destroying the valid binding.

### 3.5 The observed ~11-second cadence is generated by current monitor timing

After the first destructive recovery, the session records:

```text
Hooking D3D12   = 17
Unhooking D3D12 = 16
Last chance     = 16
Rehook request  = 16
```

Measured rehook-request intervals:

```text
minimum = 11.013 s
median  = 11.017 s
maximum = 11.334 s
```

This matches the current monitor timing:

```text
present timeout      = 5 s
last-chance delay    = 1 s
post-rehook re-arm   = m_last_present_time = now + 5 s
next timeout requires another 5 s beyond that future timestamp
```

Conceptually:

```text
5 s future offset
+ 5 s timeout
+ 1 s last chance
≈ 11 s
```

Do not change these timeout constants in P3.3R. The cadence is evidence that the recovery branch is repeatedly re-entering; the fix is to stop destructive XeFG recovery, not tune the timer.

---

## 4. Current Code Behavior That Must Change

Current `REFramework::hook_monitor()` eventually reaches the generic recovery branch:

```cpp
if (!m_has_last_chance && now - m_last_chance_time > std::chrono::seconds(1)) {
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

    m_last_present_time = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    m_last_message_time = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    m_last_chance_time = std::chrono::steady_clock::now()
        + std::chrono::seconds(1);
    m_has_last_chance = true;
}
```

For generic native D3D12 this behavior must remain unchanged.

For an active XeFG internal instance binding, it is destructive.

`REFramework::hook_d3d12()` currently recreates the hook object:

```cpp
m_d3d12_hook.reset();
m_d3d12_hook = std::make_unique<D3D12Hook>();

m_d3d12_hook->on_present(...);
m_d3d12_hook->on_post_present(...);
m_d3d12_hook->on_resize_buffers(...);
m_d3d12_hook->on_resize_target(...);

if (m_d3d12_hook->hook()) {
    ...
}
```

Destroying the old `D3D12Hook` also removes the existing XeFG instance VtableHook and releases its strong active binding ownership in the established P3.1 order.

That teardown is correct when a teardown is actually required.

The defect is calling this generic teardown/reinitialize path merely because no Present arrived for the monitor timeout.

---

## 5. Core Design Decision

P3.3R must be **caller-local**, not a global rewrite of `hook_d3d12()`.

Only the evidence-backed caller is changed:

```text
REFramework::hook_monitor()
    -> generic recovery decision
```

Do **not** add a broad early-return to `REFramework::hook_d3d12()` in this PR.

Reason:

- `hook_d3d12()` has other startup/fallback semantics.
- A global semantic change could mask a legitimate explicit reinitialization request.
- The runtime evidence only proves that the hook monitor must not destroy a structurally active XeFG instance binding due to Present timeout.
- Keeping the guard at the monitor decision point produces the smallest, most auditable diff.

If future evidence shows another caller destructively re-enters `hook_d3d12()` while a valid XeFG binding exists, handle that in a separate targeted change.

---

## 6. Scope

### Explicitly in scope

```text
identify whether current D3D12Hook still owns a complete active XeFG instance binding
suppress hook-monitor generic D3D12 rehook only for that state
keep existing D3D12Hook object alive
keep XeFG VtableHook alive
keep strong XeFG swapchain / queue / device ownership alive
preserve P3.2 binding generation and replacement path
preserve PR #14 multi-runtime registry
use explicit structured diagnostics for the suppressed recovery
re-arm the existing monitor timing window without pretending a Present occurred
keep native D3D11/D3D12 recovery behavior unchanged
```

### Explicitly out of scope

```text
MHW Alt+Enter E_PENDING fix
ResizeTarget transition hold
delayed renderer recreation
ResizeBuffers / ResizeBuffers1 behavior changes
new wrapper/proxy ResizeBuffers hooks
backbuffer reference policy
reset coalescing
long-minimize policy beyond preserving the active binding
changing 5-second / 1-second hook-monitor timing
new generic swapchain rediscovery
recovering a XeFG session that has already fallen back to phase 1
sticky process-global "XeFG was once active" state
new thread or timer
new mutex
changing P3.2 BindingGate/Rebind behavior
changing pending-binding ownership
changing multi-module XeFG runtime registry
XeFG module unload support
FSRFG
DLSS-G
Special K compatibility/emulation
OptiScaler changes
Resident Evil Requiem stutter work
```

---

## 7. Required Change A — Add a Narrow Active-Binding Predicate

Add one public const query to `D3D12Hook`.

Suggested name:

```cpp
bool has_active_xefg_instance_binding() const noexcept;
```

The name may vary slightly, but the semantics must be narrow and explicit.

The predicate is not "XeFG DLL is loaded."

It means:

> This `D3D12Hook` still represents the established P3.x XeFG internal instance binding and owns the objects required to preserve that binding.

Recommended conditions:

```text
m_hooked == true
m_is_phase_1 == false
m_swapchain_source == SwapchainSource::XeFGInternal
m_xefg_binding_generation != 0
m_swapchain_hook != nullptr
m_xefg_bound_swapchain != nullptr
m_xefg_bound_queue != nullptr
m_xefg_bound_device != nullptr
m_swap_chain == m_xefg_bound_swapchain.Get()
m_command_queue == m_xefg_bound_queue.Get()
m_device == m_xefg_bound_device.Get()
```

Example:

```cpp
bool has_active_xefg_instance_binding() const noexcept {
    return m_hooked
        && !m_is_phase_1
        && m_swapchain_source == SwapchainSource::XeFGInternal
        && m_xefg_binding_generation != 0
        && m_swapchain_hook != nullptr
        && m_xefg_bound_swapchain != nullptr
        && m_xefg_bound_queue != nullptr
        && m_xefg_bound_device != nullptr
        && m_swap_chain == m_xefg_bound_swapchain.Get()
        && m_command_queue == m_xefg_bound_queue.Get()
        && m_device == m_xefg_bound_device.Get();
}
```

An inline definition in `D3D12Hook.hpp` is acceptable.

Do not acquire another mutex inside this helper.

`REFramework::hook_monitor()` already owns `m_hook_monitor_mutex` while making this decision, and the P3.2 binding replacement path uses that same lifecycle lock for active-binding mutation.

Add a comment if useful:

```cpp
// Caller must hold REFramework's D3D hook lifecycle mutex when a stable
// active-binding snapshot is required.
```

### Why the predicate must be stricter than `xefg_module_loaded`

The following is insufficient:

```cpp
D3D12Hook::is_xefg_module_loaded()
```

A XeFG module may remain loaded while:

```text
no binding has ever been established
the hook is still phase 1
the active swapchain was already cleared
the active queue/device aliases are absent
```

Suppressing generic recovery merely because `libxess_fg.dll` exists would break legitimate initialization/fallback behavior.

### Why the predicate should include strong-owner/raw-alias agreement

P3.1 deliberately introduced strong XeFG ownership:

```cpp
m_xefg_bound_swapchain
m_xefg_bound_queue
m_xefg_bound_device
```

while the renderer continues to use raw aliases:

```cpp
m_swap_chain
m_command_queue
m_device
```

For monitor preservation, require those views to agree.

If they do not agree, do not classify the state as a healthy preservable XeFG binding.

P3.3R does not attempt to repair an inconsistent binding.

---

## 8. Required Change B — Suppress Only the Destructive Monitor Recovery

Modify the one-second post-last-chance recovery block in `REFramework::hook_monitor()`.

Before printing:

```text
Sending rehook request for D3D
```

determine whether the current D3D12 hook has a preservable active XeFG instance binding.

Conceptual structure:

```cpp
if (!m_has_last_chance
    && now - m_last_chance_time > std::chrono::seconds(1)) {

    const bool preserve_xefg_binding =
        !m_is_d3d11
        && d3d12 != nullptr
        && d3d12->has_active_xefg_instance_binding();

    if (preserve_xefg_binding) {
        // New P3.3R path.
    } else {
        // Existing generic recovery path, unchanged.
    }

    // Existing monitor re-arm logic remains shared.
}
```

### XeFG preserve path

When `preserve_xefg_binding == true`:

1. do **not** call `hook_d3d12()`,
2. do **not** call `D3D12Hook::unhook()`,
3. do **not** reset `m_d3d12_hook`,
4. do **not** clear active swapchain/queue/device,
5. do **not** rebuild native phase-1 hooks,
6. do **not** run dummy swapchain discovery,
7. do **not** reset the renderer merely for the monitor timeout,
8. log the suppression,
9. re-arm the existing monitor timing window.

Recommended log:

```cpp
spdlog::info(
    "[XeFG][HookMonitor] action = preserve_binding, "
    "reason = present_timeout, generation = {}, "
    "swapchain = 0x{:x}, queue = 0x{:x}",
    d3d12->get_xefg_binding_generation(),
    reinterpret_cast<uintptr_t>(d3d12->get_swap_chain()),
    reinterpret_cast<uintptr_t>(d3d12->get_command_queue()));

d3d12->log_hook_monitor_snapshot("xefg_rehook_suppressed");
```

The exact wording can vary, but it must be machine-readable enough to distinguish:

```text
actual generic rehook
vs
XeFG recovery suppression
```

### Do not emit a false "Sending rehook request" message

Current logging prints:

```text
Sending rehook request for D3D
```

before the actual `hook_d3d12()` call.

After P3.3R, that text must only be printed when a real generic rehook will occur.

For the preserve path, emit only the new XeFG preservation log.

This is important for runtime counting and future regression analysis.

---

## 9. Required Change C — Preserve Existing Generic Recovery Exactly

When the predicate is false, retain the existing logic.

Conceptually:

```cpp
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

Do not change behavior for:

```text
D3D11
native D3D12
D3D12 hook object missing
XeFG module loaded but no active external binding
phase-1 XeFG discovery before an external binding is established
structurally incomplete/inconsistent XeFG state
```

This is the primary regression-safety boundary for the PR.

---

## 10. Required Change D — Re-Arm the Monitor Without Faking Present Activity

After either:

```text
actual generic rehook
or
XeFG preserve-binding suppression
```

reuse the current recovery-window re-arm behavior:

```cpp
m_last_present_time =
    std::chrono::steady_clock::now() + std::chrono::seconds(5);

m_last_message_time =
    std::chrono::steady_clock::now() + std::chrono::seconds(5);

m_last_chance_time =
    std::chrono::steady_clock::now() + std::chrono::seconds(1);

m_has_last_chance = true;
```

Prefer sharing this existing block after the branch rather than duplicating it.

### Do not call `note_present_activity()` from the monitor

P3.3R must not do this:

```cpp
g_framework->note_present_activity();
```

The monitor has not observed a Present.

Calling the Present-liveness helper from the monitor would make diagnostics claim presentation activity that did not occur.

Keep:

```text
D3D12Hook::m_last_present_entry_time
present_entry_count
last_present_entry_age_ms
```

truthful.

The monitor deadline may be re-armed; the actual Present-entry metrics must remain untouched.

### Do not reset `present_entry_count`

Preservation means retaining the existing instance hook and its diagnostic history.

In the original failure:

```text
before first destructive recovery:
present_entry_count = 19203

after generic rehook:
present_entry_count = 0
```

With P3.3R, the first timeout must not reset this counter.

---

## 11. Required Change E — Do Not `return` Early From `hook_monitor()`

The D3D recovery block is not the only responsibility of `REFramework::hook_monitor()`.

Later logic also monitors the Windows message hook.

Therefore, do not implement the XeFG preservation branch as:

```cpp
if (preserve_xefg_binding) {
    ...
    return;
}
```

unless code review proves that all later monitor responsibilities are intentionally skipped.

Preferred structure:

```cpp
if (preserve_xefg_binding) {
    // log only
} else {
    // existing D3D rehook
}

// shared D3D timer re-arm

// continue into the existing message-hook monitor logic
```

This avoids fixing D3D recovery by accidentally disabling another watchdog.

---

## 12. Interaction With P3.2 Binding Replacement

P3.3R must preserve P3.2 authority.

P3.2 owns:

```text
new validated XeFG InitDesc candidate
    -> BindingGate
    -> compare identity
    -> identical: unchanged
    -> changed: atomic rebind
```

P3.3R owns:

```text
no Present for generic hook-monitor timeout
    + old active XeFG binding still structurally intact
    -> do not destroy it
```

These are complementary.

### Important case — presentation stalls and XeFG later re-initializes

Desired behavior:

```text
presentation stops
    -> monitor preserves old binding

later XeFG InitDesc publishes new candidate
    -> P3.2 compares binding
    -> if changed, P3.2 replaces old binding atomically
    -> next Present/Present1 uses new binding
```

Do not make the monitor clear old ownership in anticipation of a future rebind.

P3.2 already knows how to safely remove the old VtableHook while the old swapchain remains strongly owned.

### Important case — same binding resumes

Desired behavior:

```text
presentation temporarily stops
    -> monitor preserves old binding
    -> no new InitDesc
    -> Present1 resumes on same instance
    -> existing hook receives it
    -> normal renderer/mod callbacks continue
```

This is particularly important for future minimize/restore validation.

---

## 13. Interaction With P3.3A / Future P3.3B

P3.3A proved the MHW resize failure has a separate lifecycle issue.

The runtime evidence shows:

```text
ResizeTarget
    -> REF releases its three internal presentation backbuffers
    -> intermediate Present1
    -> REF reacquires the same three backbuffers
    -> outer OptiScaler/Streamline ResizeBuffers is still in progress
    -> XeFG reports outstanding backbuffer references
    -> E_PENDING
```

P3.3R must not change any part of that sequence.

Specifically, do not add:

```text
resize-transition hold
Present callback suppression
delayed GetBuffer
new outer swapchain resize hook
extra on_reset()
```

The point of keeping P3.3R independent is to make its runtime result interpretable.

Expected after P3.3R in the same failing Alt+Enter case:

```text
the E_PENDING may still occur
the game may still enter fatal D3D failure
BUT
the first hook-monitor timeout must not destroy the still-active XeFG binding
and the ~11-second Unhooking/Hooking D3D12 loop must not begin
```

A crash remaining after this PR is **not** evidence that P3.3R failed.

---

## 14. Suggested Code Shape

The following is a design example, not a blind patch. Adapt names to current code if `master` changes.

### `src/D3D12Hook.hpp`

```cpp
public:
    bool has_active_xefg_instance_binding() const noexcept {
        return m_hooked
            && !m_is_phase_1
            && m_swapchain_source == SwapchainSource::XeFGInternal
            && m_xefg_binding_generation != 0
            && m_swapchain_hook != nullptr
            && m_xefg_bound_swapchain != nullptr
            && m_xefg_bound_queue != nullptr
            && m_xefg_bound_device != nullptr
            && m_swap_chain == m_xefg_bound_swapchain.Get()
            && m_command_queue == m_xefg_bound_queue.Get()
            && m_device == m_xefg_bound_device.Get();
    }
```

If preferred, put the implementation in `.cpp`, but do not add new state just to implement the query.

### `src/REFramework.cpp`

Current conceptual block:

```cpp
if (!m_has_last_chance
    && now - m_last_chance_time > std::chrono::seconds(1)) {

    spdlog::info("Sending rehook request for D3D");
    ...

    if (m_is_d3d11) {
        hook_d3d11();
    } else {
        hook_d3d12();
    }

    // re-arm
}
```

Target conceptual block:

```cpp
if (!m_has_last_chance
    && now - m_last_chance_time > std::chrono::seconds(1)) {

    const bool preserve_xefg_binding =
        !m_is_d3d11
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

    // Keep the existing shared recovery-window re-arm behavior.
    m_last_present_time =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    m_last_message_time =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    m_last_chance_time =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    m_has_last_chance = true;
}
```

### Expected healthy preserved state

Before timeout:

```text
is_hooked            = true
is_phase_1           = false
active_swapchain     != 0
active_device        != 0
active_command_queue != 0
binding_generation   >= 1
present_entry_count  = N
```

After preservation event, if no Present has occurred yet:

```text
is_hooked            = true
is_phase_1           = false
active_swapchain     = same pointer
active_device        = same pointer
active_command_queue = same pointer
binding_generation   = same generation
present_entry_count  = same N
```

The only expected change is the monitor deadline.

---

## 15. Failure Behavior

P3.3R is deliberately conservative.

### If the active XeFG predicate is true

Preserve.

Do not attempt a generic rehook.

### If the predicate is false

Use the existing generic recovery exactly as before.

Do not guess why the predicate failed.

Examples:

```text
m_swap_chain == nullptr
m_swapchain_hook == nullptr
m_xefg_binding_generation == 0
phase 1 is already active
raw aliases do not match strong owners
source is Native
```

All remain on existing behavior.

### Do not add automatic repair of inconsistent fields

Do not write code such as:

```cpp
if (m_swap_chain == nullptr && m_xefg_bound_swapchain) {
    m_swap_chain = m_xefg_bound_swapchain.Get();
}
```

That would create a new recovery policy without runtime evidence.

P3.3R only prevents destruction of a binding that is already coherent.

---

## 16. No New Sticky State

Do not add:

```cpp
bool m_xefg_was_ever_bound;
static bool s_xefg_binding_was_established;
```

for this PR.

The original failing session reaches the first timeout while the binding is still active and coherent, so no historical state is required to fix the observed destructive transition.

A sticky "XeFG was once active" flag would raise additional questions:

```text
module unload
true device loss
intentional unhook
shutdown
new native swapchain
failed rebind
```

Those questions are unnecessary for this small PR.

---

## 17. No New Hook-Integrity Probe

Do not broaden this PR into vtable-integrity verification.

The current evidence shows:

```text
m_swapchain_hook still present
active binding still owned
phase 1 false
present count previously advancing
then presentation stops after E_PENDING
```

P3.3R does not need to prove that every vtable slot still contains REFramework's detour before deciding not to destroy it.

If future long-minimize testing proves an external component actually overwrites the XeFG instance vtable while the binding remains owned, that is a separate defect and should receive a targeted integrity/reinstall design.

---

## 18. Files Expected to Change

Prefer exactly:

```text
src/D3D12Hook.hpp
src/REFramework.cpp
```

`src/D3D12Hook.cpp` should not be needed if the predicate is inline.

Expected implementation size:

```text
~20–60 LOC
```

A slightly larger diff is acceptable for clear comments/log formatting, but this PR should not become a refactor.

If implementation requires more than roughly 100 LOC, stop and reassess whether unrelated lifecycle logic is being pulled into the patch.

---

## 19. Static Verification

Codex must perform at least:

```text
git diff --check
```

Review the final diff manually for these invariants:

### Required invariant 1

No P3.2 binding replacement semantics changed.

Search final diff for accidental modifications around:

```text
replace_xefg_binding
BindingGate
m_xefg_binding_generation
m_xefg_bound_swapchain
m_xefg_bound_queue
m_xefg_bound_device
```

The new predicate may read those fields; it must not alter replacement behavior.

### Required invariant 2

No resize behavior changed.

No functional changes to:

```text
resize_target
resize_buffers
resize_buffers1
present_common renderer suppression
REFramework::on_reset()
deinit_d3d12()
```

### Required invariant 3

No hook-monitor timeout changed.

Keep:

```text
5 seconds
1 second
existing post-recovery re-arm
```

### Required invariant 4

Generic recovery remains present.

There must still be a reachable existing path that calls:

```text
hook_d3d11()
hook_d3d12()
```

when no preservable active XeFG binding exists.

### Required invariant 5

No false rehook log on preservation.

`Sending rehook request for D3D` must not be emitted in the suppression branch.

---

## 20. Build Verification

Build the normal Windows Release target using the repository's established build procedure.

At minimum report:

```text
configure result
Release build result
output dinput8.dll path
git diff --check result
```

Do not claim game runtime validation from Codex unless the implementation environment actually launches the games on supported hardware.

If build warnings already exist on unchanged `master`, distinguish pre-existing warnings from new warnings.

P3.3R should introduce no new warning.

---

## 21. Runtime Validation Matrix

Hardware/game validation is expected to be performed manually after the PR build.

### Test A — Normal XeFG gameplay baseline

Primary:

```text
Monster Hunter Wilds
Intel GPU
OptiScaler XeFG
REFramework dinput8
Special K absent
```

Run normal gameplay/menu presentation for several minutes without Alt+Enter.

Expected:

```text
XeFG works
OptiScaler overlay works
REFramework overlay works
Present/Present1 continues
no unexpected preserve-binding log during continuous presentation
no generic rehook
```

This confirms the new branch is dormant while Present activity is healthy.

### Test B — Reproduce the known Alt+Enter E_PENDING session

This test intentionally does **not** require the crash to be fixed.

Trigger the same MHW Alt+Enter transition.

If the existing `E_PENDING` still occurs, inspect the hook-monitor period.

Required P3.3R result at the first timeout:

```text
Last chance encountered for hooking

[XeFG][HookMonitor]
action = preserve_binding
reason = present_timeout

[D3D12][HookMonitor]
event = xefg_rehook_suppressed
is_phase_1 = false
active_swapchain != 0
active_device != 0
active_command_queue != 0
present_entry_count remains the prior nonzero value
```

Must **not** occur for that timeout:

```text
Sending rehook request for D3D
[D3D12][HookLifecycle] action = hook, reason = hook_monitor_recovery
Unhooking D3D12
Hooking D3D12
Creating dummy device
Creating dummy swapchain
[D3D12][HookInstall] phase = phase1
```

If the game later exits because of the existing fatal `E_PENDING`, that is outside P3.3R acceptance.

### Test C — Long minimize / restore

This is important because prior P3.3A evidence did not exercise a true long-minimize recovery.

With XeFG active:

1. reach stable gameplay,
2. minimize or Alt+Tab away long enough to exceed the D3D monitor threshold,
3. remain away for at least 15 seconds if the game actually stops presenting,
4. restore the game.

Two valid subcases exist.

#### C1 — game continues presenting while minimized

No monitor timeout may occur.

Expected:

```text
no preserve log required
no generic rehook
restore works
REF overlay still works
```

#### C2 — game stops presenting while minimized

Expected:

```text
monitor may emit Last chance
monitor may emit preserve_binding
NO destructive D3D rehook
active XeFG binding remains instance-mode and nonzero
on restore, Present/Present1 resumes through same binding
REF renderer/overlay resumes
```

This is the most important success case for proving that preservation is better than destructive generic recovery.

### Test D — XeFG re-init after a preserved timeout, if naturally reproducible

If the game/OptiScaler emits a new validated XeFG InitDesc after preservation:

Expected:

```text
P3.2 BindingGate remains authoritative
identical candidate -> unchanged
changed candidate   -> Rebind transaction
```

P3.3R must not block or bypass P3.2.

Do not invent a synthetic re-init path solely for this PR if it is not easy to reproduce.

---

## 22. Runtime Acceptance Criteria

P3.3R is accepted when all of the following are true.

### Functional

```text
active XeFG binding is not destroyed merely because Present times out
native D3D recovery path still exists
P3.2 changed-binding replacement remains intact
```

### Diagnostic

At a suppressed XeFG timeout:

```text
new preserve log is emitted
is_phase_1 remains false
active_swapchain remains nonzero
active_device remains nonzero
active_command_queue remains nonzero
binding generation remains unchanged
present_entry_count is not reset by the monitor
```

### Regression

No new:

```text
device removed
access denied
ResizeBuffers1 invalid call
hook recursion
vtable restoration failure
startup XeFG bind failure
multi-module runtime regression
```

### Known non-goal

This PR does **not** need to eliminate:

```text
MHW Alt+Enter E_PENDING
Fatal D3D error caused by that E_PENDING
```

Do not hold this PR open because the known crash remains.

---

## 23. Explicit Rejection Cases

The following implementations are not acceptable.

### Rejection A — Disable D3D hook monitor globally when XeFG DLL is loaded

Bad:

```cpp
if (D3D12Hook::is_xefg_module_loaded()) {
    return;
}
```

Why:

```text
module-loaded != active binding
breaks initialization/fallback
can suppress legitimate recovery before binding exists
```

### Rejection B — Change timeout from 5 seconds to a larger value

Bad:

```cpp
if (now - m_last_present_time > 30s)
```

Why:

```text
does not fix destructive recovery
only delays it
changes native behavior
```

### Rejection C — Re-publish old owned binding into global pending state

Bad conceptual behavior:

```text
monitor timeout
-> copy m_xefg_bound_* into g_pending_xefg_binding
-> destroy hook
-> construct new hook
-> consume copied pending
```

Why:

```text
unnecessary teardown
creates new cross-object lifetime/race surface
duplicates P3.2 authority
the valid old instance hook already exists
```

### Rejection D — Call `on_reset()` on every monitor timeout

Why:

```text
Present timeout is not proof renderer resources are invalid
can alter the separate MHW resize lifecycle
not required to preserve the hook
```

### Rejection E — Force `m_is_phase_1 = false` after generic recovery

Why:

```text
does not restore the lost instance hook
creates false state
```

### Rejection F — Add a global `hook_d3d12()` early-return in this PR

Why:

```text
broader semantics than evidence requires
could affect startup/fallback/explicit reinitialization callers
```

### Rejection G — Treat `last_present_entry_age_ms` as fresh after suppression

Do not overwrite actual entry-time diagnostics.

---

## 24. Expected Log Comparison

### Before P3.3R

```text
13:44:46.555 Last chance encountered for hooking
13:44:46.555 [HookMonitor] is_phase_1=false, active_swapchain=0x74b2cc0, present_entry_count=19203

13:44:47.556 Sending rehook request for D3D
13:44:47.556 [HookLifecycle] action=hook, reason=hook_monitor_recovery
13:44:47.558 Unhooking D3D12
13:44:47.558 Hooking D3D12
...
13:44:47.598 [HookInstall] phase=phase1

13:44:57.889 Last chance encountered for hooking
13:44:57.889 [HookMonitor] is_phase_1=true, active_swapchain=0x0, present_entry_count=0
...
(repeats ~11 s)
```

### Target after P3.3R

```text
Last chance encountered for hooking
[HookMonitor] is_phase_1=false, active_swapchain=<same>, present_entry_count=<same N>

[XeFG][HookMonitor]
action=preserve_binding
reason=present_timeout
generation=<same generation>
swapchain=<same>
queue=<same>

[D3D12][HookMonitor]
event=xefg_rehook_suppressed
is_phase_1=false
active_swapchain=<same>
active_device=<same>
active_command_queue=<same>
present_entry_count=<same N>
```

No:

```text
Unhooking D3D12
Hooking D3D12
phase1 reinstall
```

If presentation remains stopped, another preservation diagnostic may occur after the monitor is re-armed. Repeated **logs** are acceptable; repeated destructive rehook is not.

---

## 25. PR Boundaries and Review Checklist

Before opening the PR, confirm:

- [ ] Base is current `master`.
- [ ] Diff is limited to the monitor-preservation change.
- [ ] `D3D12Hook` has a narrow active-XeFG-instance predicate.
- [ ] Predicate requires instance mode, active hook, generation, strong owners, and alias agreement.
- [ ] `hook_monitor()` suppresses only generic D3D12 recovery for that predicate.
- [ ] `Sending rehook request for D3D` is not printed on suppression.
- [ ] Existing monitor re-arm timing is reused.
- [ ] `note_present_activity()` is not called from the suppression branch.
- [ ] No early `return` skips the message-hook monitor logic.
- [ ] Native D3D11/D3D12 recovery is unchanged.
- [ ] P3.2 rebind code is unchanged.
- [ ] P3.3A resize behavior is unchanged.
- [ ] PR #14 runtime registry is unchanged.
- [ ] No new sticky state.
- [ ] No new mutex/thread/timer.
- [ ] `git diff --check` passes.
- [ ] Release build passes.
- [ ] No new compiler warnings.
- [ ] PR description explicitly states that MHW Alt+Enter `E_PENDING` remains a separate issue.

---

## 26. Suggested PR Description

Suggested concise PR body:

```markdown
## Purpose

Prevent REFramework's generic D3D hook monitor from destroying an already-active XeFG internal presentation binding after a Present timeout.

## Runtime evidence

In the MHW P3.3A + multi-module failure session, the first hook-monitor recovery occurs only after the Alt+Enter `E_PENDING` stops presentation.

Immediately before the first recovery request the XeFG binding is still active:

- `is_phase_1 = false`
- `active_swapchain != 0`
- `active_device != 0`
- `active_command_queue != 0`
- `present_entry_count = 19203`

The generic recovery then destroys that binding, rebuilds native phase-1 `Present[8]`, and subsequent snapshots show:

- `is_phase_1 = true`
- `active_swapchain = 0`
- `present_entry_count = 0`

This repeats at ~11-second intervals.

## Change

- Detect a coherent active `XeFGInternal` instance binding.
- On hook-monitor Present timeout, preserve that binding instead of calling generic `hook_d3d12()`.
- Keep native D3D recovery unchanged when no preservable XeFG binding exists.
- Keep existing monitor timing and diagnostics.
- Do not change resize or Alt+Enter behavior.

## Non-goal

This PR does not fix the separate MHW Alt+Enter `ResizeBuffers -> E_PENDING` failure.
```

---

## 27. Final Implementation Principle

The P3.x XeFG path already has two authoritative lifecycle mechanisms:

```text
normal same binding:
    keep using the existing instance hook

changed validated binding:
    P3.2 atomic replacement
```

The generic native D3D hook monitor must not introduce a third mechanism that destroys an intact external XeFG binding and attempts to rediscover it through a phase-1 path already proven to be presentation-starved.

For P3.3R, the correct recovery action is deliberately simple:

```text
if active XeFG instance binding is still coherent:
    preserve it
else:
    keep existing generic recovery
```

Nothing else belongs in this PR.
