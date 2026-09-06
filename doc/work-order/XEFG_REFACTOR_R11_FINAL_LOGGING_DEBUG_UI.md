# R11 Work Order — Final XeFG Logging Cleanup and `Debug Log` UI

## 1. Purpose

R1–R10 are complete and the current OptiScaler + Intel XeFG compatibility path has now been runtime-tested across four RE Engine games without a material compatibility regression:

- Dragon's Dogma 2
- PRAGMATA
- Resident Evil Requiem (`re9`)
- Monster Hunter Wilds

The current runtime evidence is good enough to move to the final planned logging pass.

R11 is **not a functional XeFG refactor**. It is the final user-facing logging cleanup before release validation.

The goals are:

1. keep enough default logging that a normal user's `re2_framework_log.txt` is still useful when they report "REFramework / OptiScaler / XeFG does not work";
2. stop shipping the large diagnostic firehose accumulated during P3 and R1–R10 development;
3. add a persistent `Debug Log` checkbox to the existing REFramework `Configuration` UI;
4. preserve all current log-file retention behavior exactly as it is today;
5. make no XeFG detection, binding, Present, resize, hook-monitor, Lua, overlay, or renderer behavior change.

This is the final logging/configuration PR in the XeFG compatibility sequence.

---

## 2. Planning base

Plan against current `master`:

```text
5a6d61846284fcac5a7b302190a6d0252a372135
ux: auto-hide startup menu after 3 seconds (#29)
```

R10 remains the functional compatibility baseline:

```text
acf542e3b1099cff1b8b2bcf9b5a8b00d2888c23
Refactor R10: isolate XeFG hook-monitor policy and final core surface (#28)
```

Do not implement from an older R10 checkout. PR #29 changed `REFrameworkConfig` startup-menu behavior and must remain intact.

Before editing, rebase/refresh against the latest `master` and preserve any newer unrelated changes.

---

## 3. Runtime evidence driving this cleanup

The logging policy in this work order is based on real V09 runs rather than only on source inspection.

### DD2

Observed healthy behavior:

- initial XeFG binding generation 1;
- no rebind churn;
- `ResizeBuffers1` 17/17 successful;
- Lua smoke callback progressed from 1 to 15000;
- no `DEVICE_REMOVED`, `ACCESS_DENIED`, or `INVALID_CALL` regression.

Observed detailed resize lifecycle volume:

```text
~48.3 [XeFG][ResizeLifecycle] messages/minute
```

### PRAGMATA

Observed healthy behavior:

- stable generation 1 binding;
- `ResizeBuffers1` 9/9 successful;
- Lua callback progressed to 13800;
- R10 healthy-binding preservation was actually exercised:

```text
last_chance
-> [XeFG][HookMonitor] action = preserve_binding
-> xefg_rehook_suppressed
-> no destructive D3D12 rehook
```

Observed detailed resize lifecycle volume:

```text
~28.8 messages/minute
```

### Resident Evil Requiem

Observed healthy behavior:

- generation 1 stable;
- repeated 1920x1080 <-> 1920x1200 transitions;
- 8 successful `ResizeTarget` calls;
- 13 successful `ResizeBuffers1` calls;
- Lua remained alive after the final successful resize.

Observed detailed resize lifecycle volume:

```text
~57.2 messages/minute
```

### Monster Hunter Wilds

Manual runtime validation included:

- repeated Alt+Enter window/fullscreen transitions;
- several Alt+Tab transitions to Windows Explorer and the Steam client;
- no crash or notable recovery failure.

The MHW-specific resize hold was heavily exercised:

- 16 `ResizeTarget` hold arms;
- 23 successful `ResizeBuffers1` calls;
- a long transition suppressed 462 REFramework renderer callbacks for about 4.85 seconds and then completed normally when `ResizeBuffers1` returned `S_OK`;
- R10 hook-monitor preservation was also exercised without destructive rehook.

Observed detailed log volume:

```text
[XeFG][ResizeLifecycle] ~77.9 messages/minute
[XeFG][ResizeHold]      ~15.1 messages/minute
```

### Conclusion from the runtime evidence

The detailed lifecycle diagnostics were useful during development, but they are too verbose for normal users.

The default log still needs concise state-transition and failure evidence. A support report should normally allow us to tell whether:

- XeFG runtime registration happened;
- the internal presentation binding became active;
- the binding was render-capable or observe-only;
- a rebind happened or failed;
- a MHW hold started/completed/failed;
- hook-monitor preserved the active binding or performed recovery;
- a Present/resize/device failure occurred.

Everything below that level should normally require `Debug Log`.

---

## 4. Hard requirements

### 4.1 UI

Use the existing REFramework `Configuration` collapsing section in:

```text
src/mods/REFrameworkConfig.cpp
```

Add exactly one checkbox labeled:

```text
Debug Log
```

Requirements:

- use the existing `ModToggle` pattern;
- default is `false` / unchecked;
- persist through the existing REFramework configuration file;
- changing the checkbox takes effect immediately for subsequent log messages;
- no tooltip;
- no help marker;
- no explanatory text;
- no restart-required text;
- do not add another section or submenu.

Conceptually:

```cpp
changed |= m_debug_log->draw("Debug Log");
```

The exact implementation must also update the runtime debug-log policy immediately when the value changes.

### 4.2 Persistence

The current REFramework config already uses:

```text
re2_fw_config.txt
```

and `REFrameworkConfig` stores its values through `m_options`, `on_config_load()`, and `on_config_save()`.

Add the new toggle through that existing mechanism.

Suggested member shape:

```cpp
ModToggle::Ptr m_debug_log{
    ModToggle::create(generate_name("DebugLog"), false)
};
```

Add it to `m_options` so no custom configuration parser is introduced.

A clean internal key such as:

```text
REFrameworkConfig_DebugLog
```

is expected from the existing `generate_name()` convention.

The fork is unreleased. No legacy alias/migration key is required.

### 4.3 Retention must not change

The current logger is created as:

```cpp
spdlog::basic_logger_mt(
    "REFramework",
    get_persistent_dir("re2_framework_log.txt").string(),
    true)
```

The final `true` is the current truncate behavior for the basic file logger.

R11 must **not** change any of the following:

- filename `re2_framework_log.txt`;
- persistent-directory resolution/fallback;
- sink type;
- append/truncate behavior;
- file rotation behavior;
- number of log files;
- cleanup/retention lifetime;
- startup/shutdown flush policy.

Do not create a separate debug log file.

`Debug Log` only changes which diagnostic messages are emitted into the existing log.

---

## 5. Do not use global spdlog level as the UI switch

Do **not** implement the checkbox by globally changing the REFramework/spdlog level to `debug` or `trace`.

Reason:

- this PR only controls the verbose diagnostics introduced for the XeFG compatibility work;
- globally enabling `spdlog::debug` could expose unrelated upstream debug logging and create a different firehose;
- globally raising/lowering the logger level could also accidentally hide normal upstream REFramework messages.

The checkbox should control a narrow fork-specific diagnostic policy.

A minimal implementation is to keep the runtime flag behind the existing XeFG compatibility facade, which is already included by `D3D12Hook.cpp`:

```cpp
class XeFGCompatibility {
public:
    static void set_debug_log_enabled(bool enabled) noexcept;
    static bool is_debug_log_enabled() noexcept;

private:
    static std::atomic<bool> s_debug_log_enabled;
};
```

Example:

```cpp
void XeFGCompatibility::set_debug_log_enabled(bool enabled) noexcept {
    s_debug_log_enabled.store(enabled, std::memory_order_relaxed);
}

bool XeFGCompatibility::is_debug_log_enabled() noexcept {
    return s_debug_log_enabled.load(std::memory_order_relaxed);
}
```

`REFrameworkConfig` should publish its loaded/current toggle value to this flag.

This avoids making low-level XeFG discovery/binding code depend directly on ImGui or `REFrameworkConfig`.

If a comparably small and cleaner central helper already exists by implementation time, it may be used, but do not introduce a general logging framework for this PR.

---

## 6. Required default-vs-debug logging policy

The important distinction is:

```text
DEFAULT OFF
    concise lifecycle + actionable failures

DEBUG ON
    default log
    + detailed identity/pointer/vtable/discovery/Present/resize snapshots
```

Debug ON must be additive. It must not replace or suppress the default support-critical lines.

### 6.1 Always keep warnings and errors

Do not hide meaningful `warn` / `error` events behind `Debug Log`.

Examples that must remain visible with Debug Log OFF:

- runtime registry enumeration failure;
- required InitDesc export missing;
- runtime registry capacity exceeded;
- XeFG API hook installation failure;
- candidate rejection that prevents rendering;
- external bind failure;
- initial physical hook preparation/creation failure;
- failed rebind preparation;
- failed Present/Present1;
- `DXGI_ERROR_DEVICE_REMOVED` and device-removed reason;
- failed `ResizeBuffers`, `ResizeBuffers1`, or `ResizeTarget`;
- failed MHW hold completion;
- destructive hook-monitor recovery.

Do not downgrade existing warning/error severity merely to reduce normal log volume.

### 6.2 Keep a concise runtime-registration line

Current verbose examples include:

```text
[XeFG][Module]
[XeFG][Exports]
[XeFG][RuntimeRegistry]
```

Default OFF should retain one concise successful runtime registration line per newly registered runtime, for example:

```text
[XeFG][RuntimeRegistry] action = installed, slot = 0, path = ...\libxess_fg.dll
```

The following details should be Debug-only:

- HMODULE/base address;
- export addresses;
- full export-presence dump;
- duplicate rediscovery detail;
- `first_seen_ms` diagnostics;
- loader handoff pointer values.

A duplicate exact-HMODULE registration is routine and should normally be Debug-only.

Registration rejection/failure remains normal warn/error.

### 6.3 Keep concise binding state

A default user log must tell us whether the XeFG presentation binding actually became usable.

Keep a concise accepted-candidate line with semantic information, not raw addresses. For example:

```text
[XeFG][Bind] accepted = true, reason = init_success, mode = render, queue_relation = distinct_same_device
```

For fallback:

```text
[XeFG][Bind] accepted = true, reason = init_success_observe_only, mode = observe_only, queue_relation = ...
```

A candidate rejection remains a warning with its reason.

After physical binding succeeds, keep a concise commit line. Existing `[D3D12][ExternalBind]` may be retained but should not require pointer dumps in normal mode. For example:

```text
[D3D12][ExternalBind] source = xefg_internal, generation = 1, mode = render
```

Debug ON may additionally print swapchain/queue/device/original-function addresses.

### 6.4 Keep rebind lifecycle, but make it concise by default

Rebinds are rare and support-critical.

With Debug Log OFF, preserve enough to reconstruct:

```text
begin
-> success / failure
-> resulting generation
```

Normal lines should include only fields such as:

```text
stage
reason
generation
same-object vs replacement
observe/render mode if relevant
```

Do not print old/new swapchain and queue addresses by default.

The existing full `log_xefg_rebind()` address dump belongs behind Debug Log.

Intermediate transaction details such as:

```text
new_hook_prepared
old_renderer_reset
old_hook_removed
```

may be Debug-only as long as default mode still has a concise begin and final success/failure result.

Failures should remain warning-level/support-visible.

### 6.5 Keep MHW ResizeHold semantic transitions; gate suppression spam

MHW hold events are useful when a user reports that the overlay disappears after a mode transition.

Keep concise normal lines for:

```text
action = arm
action = complete
action = keep / completion_failed
action = clear when the reason is meaningful
```

Useful normal fields:

```text
trigger_event_id
completion_event_id
completion_kind
result
suppressed_presents
generation
reason
```

Do not include swapchain pointers in the default `arm` line.

The repeated first-three suppression samples:

```text
[XeFG][ResizeHold] action = suppress_present
```

are Debug-only.

Routine cleanup-only `clear` messages caused by normal unhook/bind replacement may be Debug-only if they add no support value. Failure-related clear/keep events must remain visible.

### 6.6 Keep hook-monitor outcome; gate full snapshots

R10 preservation is support-critical and was exercised successfully in PRAGMATA and MHW.

Keep a concise default event:

```text
[XeFG][HookMonitor] action = preserve_binding, reason = present_timeout, generation = N
```

Do not include swapchain/queue pointers by default.

Keep destructive generic recovery lines visible, including the existing lifecycle reason:

```text
[D3D12][HookLifecycle] action = hook, reason = hook_monitor_recovery
```

The large `D3D12Hook::log_hook_monitor_snapshot()` state dump is Debug-only:

```text
[D3D12][HookMonitor] event = ...
    is_hooked
    is_phase_1
    inside_present
    active_swapchain
    active_device
    active_command_queue
    present_entry_count
    last_present_entry_age_ms
    ...
```

The existing generic `Last chance encountered for hooking` behavior does not need functional modification in this PR.

### 6.7 Move full InitDesc/discovery/queue identity details to Debug

The following are development diagnostics and should be Debug-only on success:

```text
[XeFG][RuntimeDispatch] successful dispatch detail
[XeFG][InitDesc] full descriptor/context dump
[XeFG][InternalSwapchain]
[XeFG][QueueIdentity]
[XeFG][P2.1Probe]
[XeFG][PublicProxy]
[D3D12][Discovery]
[D3D12][SwapchainCandidate]
[D3D12][HookInstall]
[D3D12][PhaseTransition]
```

This includes:

- context addresses;
- factory addresses;
- HWND values used only for diagnosis;
- swapchain pointers;
- queue COM identities;
- device identities;
- vtable slot addresses and module owners;
- public-proxy comparison;
- dummy-device/discovery pointer snapshots introduced for XeFG diagnosis.

A `RuntimeDispatch` failure (`runtime_not_active`) remains an error with Debug Log OFF.

### 6.8 Move Present-entry diagnostics to Debug

The current first-10/change-triggered:

```text
[D3D12][PresentEntry]
```

messages are Debug-only.

They contain pointer/vtable/thread/owner information and are not necessary for the default support path now that the compatibility flow has proven stable.

Actual Present/Present1 failures remain always visible.

### 6.9 Move full ResizeLifecycle diagnostics to Debug

All stage-by-stage:

```text
[XeFG][ResizeLifecycle]
```

messages are Debug-only.

This includes:

- `enter`;
- dimensions/flags detail;
- `pre_reset`;
- `post_reset`;
- `original_return` success detail;
- `present_after_resize` sample 1/2/3;
- pointers, identities, generation, queue/device, function owner dumps.

The observed 28–78 messages/minute across the four validation games is the main reason for this decision.

### 6.10 Move successful ResizeBuffers1 stage tracing to Debug

The current successful-stage tracing:

```text
[XeFG][ResizeBuffers1] stage = enter
[XeFG][ResizeBuffers1] stage = pre_reset_begin
[XeFG][ResizeBuffers1] stage = pre_reset_end
[XeFG][ResizeBuffers1] stage = original_return
```

is Debug-only.

However, the current `ResizeBuffers1` path does not have the same simple error line that `ResizeBuffers` and `ResizeTarget` already have.

Before hiding the successful `original_return` detail, add/retain an unconditional error path such as:

```cpp
if (FAILED(result)) {
    spdlog::error(
        "[XeFG][ResizeBuffers1] failed, result = 0x{:08x}",
        static_cast<uint32_t>(result));
}
```

This is a logging-only change. Do not alter return values, hold completion behavior, reset ordering, or nested-call behavior.

### 6.11 Gate renderer/resize snapshots, not renderer failures

Detailed D3D12 renderer-acquire/resize snapshots in `REFramework.cpp`, including backbuffer/resource/reference-count and pre/post-reset state used during P3 diagnosis, are Debug-only.

Examples include calls through:

```cpp
REFramework::log_d3d12_resize_snapshot(...)
```

and detailed successful renderer-acquire diagnostics tied to the XeFG resize event.

Do not hide actual renderer initialization/acquisition failures that a user support log needs.

---

## 7. Suggested implementation shape

### 7.1 Config state

`src/mods/REFrameworkConfig.hpp`

```cpp
private:
    ModToggle::Ptr m_debug_log{
        ModToggle::create(generate_name("DebugLog"), false)
    };
```

Add to `m_options`.

Expose a simple value accessor only if useful for tests or config synchronization:

```cpp
bool is_debug_log_enabled() const {
    return m_debug_log->value();
}
```

Do not expose ImGui details outside this class.

### 7.2 Load synchronization

After the normal `m_options` config load:

```cpp
XeFGCompatibility::set_debug_log_enabled(m_debug_log->value());
```

Default before configuration load remains false.

This is acceptable: default/support-critical messages are deliberately still emitted before the config is loaded, while deep early pointer/export diagnostics may remain unavailable until a persisted Debug Log setting is loaded.

Do not complicate startup ordering just to capture every pre-config diagnostic line.

### 7.3 UI synchronization

Conceptually:

```cpp
if (m_debug_log->draw("Debug Log")) {
    XeFGCompatibility::set_debug_log_enabled(m_debug_log->value());
    changed = true;
}
```

Then use the existing:

```cpp
if (changed) {
    g_framework->request_save_config();
}
```

No special save path is needed.

### 7.4 Gating at call sites

Do not convert verbose messages to `spdlog::debug()` and depend on logger level.

Prefer explicit gating:

```cpp
if (XeFGCompatibility::is_debug_log_enabled()) {
    spdlog::info("[XeFG][QueueIdentity] ...");
}
```

For expensive diagnostic construction, guard the entire calculation, not only the final `spdlog::info()` call.

Examples:

- vtable snapshot creation;
- module-owner resolution;
- stack/resource snapshot formatting;
- queue identity logging payloads where capture is only diagnostic.

Do **not** skip data capture that is required for actual validation/binding behavior. Only skip work that exists solely to build a log message.

---

## 8. Files expected to change

Expected core set:

```text
src/mods/REFrameworkConfig.hpp
src/mods/REFrameworkConfig.cpp
src/compatibility/xefg/XeFGCompatibility.hpp
src/compatibility/xefg/XeFGCompatibility.cpp
src/compatibility/xefg/XeFGRuntimeRegistry.cpp
src/compatibility/xefg/XeFGDiscovery.cpp
src/D3D12Hook.cpp
src/REFramework.cpp
```

Not every file is mandatory if inspection shows it has no verbose call to gate.

Do not edit `XeFGResizeLifecycle` just to move logging policy into it; it currently owns state, not logging presentation.

Avoid changes outside this set unless required to build/test the narrow logging feature.

---

## 9. Functional invariants — must remain unchanged

R11 must preserve all R1–R10 runtime behavior.

### XeFG discovery/binding

Do not change:

- exact-HMODULE runtime registration semantics;
- maximum runtime slots;
- stable dispatch thunks;
- required vs optional XeFG exports;
- serialized InitDesc transaction;
- temporary `IDXGIFactory2::CreateSwapChainForHwnd[15]` capture;
- original XeFG InitDesc call count/order;
- internal presentation swapchain authority;
- presentation queue validation;
- `distinct_same_device` render-capable policy;
- observe-only fallback policy;
- candidate publication/handoff ordering.

### Active binding

Do not change:

- strong COM ownership;
- binding identity `(swapchain, queue, mode)`;
- generation semantics;
- R7 transactional initial bind/rebind ordering;
- failed new-hook preparation preserving the current good binding.

### Present/resize

Do not change:

- Present slot 8;
- ResizeBuffers slot 13;
- ResizeTarget slot 14;
- Present1 slot 22;
- ResizeBuffers1 slot 39;
- original Present/Present1 forwarding;
- nested Present behavior;
- nested resize behavior;
- renderer callback ordering;
- post-present activity accounting.

### MHW hold

Do not change the exact MHW-only activation policy:

```cpp
event_id != 0
&& renderer_reset_performed
&& d3d12->is_xefg_render_capable()
&& sdk::GameIdentity::get().is_mhwilds()
```

Do not add timeouts/fallbacks.

Do not change:

- failed ResizeTarget clears stale hold;
- failed ResizeBuffers/ResizeBuffers1 keeps hold;
- successful ResizeBuffers/ResizeBuffers1 completes hold;
- original Present continues while REF renderer callbacks are suppressed.

### Hook monitor

Do not change:

- 5-second Present timeout;
- 1-second last-chance window;
- healthy XeFG binding predicate;
- R10 preservation decision;
- generic native recovery path.

The healthy-binding contract remains:

```cpp
m_hooked
&& !m_is_phase_1
&& m_swapchain_source == SwapchainSource::XeFGInternal
&& m_xefg_binding.active()
&& m_swapchain_hook != nullptr
&& m_xefg_binding.aliases_match(
    m_swap_chain,
    m_command_queue,
    m_device)
```

---

## 10. Upstream-surface rule

This is not a general REFramework logging cleanup.

Do not opportunistically rewrite or remove old upstream logs merely because they look verbose.

In particular, do not broaden this PR into cleanup of all:

- loader notification logging;
- generic D3D11 logging;
- generic D3D12 initialization logging;
- mod/plugin/Lua logging;
- unrelated callstack logging;
- startup diagnostics.

The target is the diagnostic surface added or materially expanded for the OptiScaler/XeFG compatibility work, plus the minimal config/UI plumbing required to control it.

If a generic-looking log is not clearly part of the XeFG compatibility diagnostic additions, leave it unchanged unless it is required to satisfy this work order.

---

## 11. Build and static validation

At minimum:

1. build the same configurations used by current CI/workflow;
2. run `git diff --check`;
3. ensure no new warnings are introduced by unused diagnostic-only helpers/variables when Debug Log gating is compiled in;
4. verify no logging guard changes control flow or lock lifetime;
5. verify `Debug Log` default false when the config key is absent.

Pay special attention to code where diagnostic payload construction currently acquires COM interfaces or resolves module ownership. Gating must not remove an operation that the runtime path actually depends on.

---

## 12. Runtime validation after implementation

### 12.1 Persistence/UI

Fresh config / no `DebugLog` key:

```text
Configuration
[ ] Debug Log
```

Expected:

- unchecked;
- no explanation text;
- no tooltip/help text;
- remains off after restart unless user enables it.

Enable it:

```text
Configuration
[x] Debug Log
```

Expected:

- verbose diagnostic messages begin for subsequent events without restart;
- state persists after restart.

Disable it again:

- verbose diagnostic messages stop for subsequent events;
- normal support-critical messages continue;
- state persists after restart.

### 12.2 Debug Log OFF — minimum support evidence

For OptiScaler + XeFG, the default log must still allow a reviewer to identify at least:

```text
runtime registration outcome
binding accepted/rejected
active binding commit/generation
render vs observe-only state
rebind begin/result if a rebind occurs
MHW hold arm/result if exercised
hook-monitor preserve/recovery if exercised
Present/resize/device failure if one occurs
```

A user should not normally have to reproduce the problem immediately with Debug Log enabled just for us to learn whether XeFG registration or binding failed.

### 12.3 Debug Log ON

Trigger startup and at least one resize/mode transition.

Verify that detailed diagnostics return, including the relevant current markers:

```text
[XeFG][Module]
[XeFG][Exports]
[XeFG][RuntimeDispatch]
[XeFG][InitDesc]
[XeFG][InternalSwapchain]
[XeFG][QueueIdentity]
[XeFG][P2.1Probe]
[XeFG][PublicProxy]
[D3D12][Discovery]
[D3D12][SwapchainCandidate]
[D3D12][PresentEntry]
[XeFG][ResizeLifecycle]
[XeFG][ResizeBuffers1]
[D3D12][HookMonitor] detailed snapshots
D3D12 resize/renderer snapshots introduced for XeFG diagnosis
```

Debug ON does not need to reproduce byte-for-byte every historical log line, but it must retain equivalent deep diagnostic power.

### 12.4 Four-game smoke wave

Run a short final smoke test on the already-proven matrix:

#### DD2

- OptiScaler + XeFG;
- REFramework overlay visible;
- Lua autorun works;
- simple resize/focus transition;
- no rebind/recovery churn.

#### PRAGMATA

- OptiScaler + XeFG;
- verify runtime slot registration remains sane;
- if hook-monitor preservation naturally occurs, confirm no destructive rehook.

#### Resident Evil Requiem

- repeat at least one window/resolution transition;
- renderer recovers;
- Lua callback continues.

#### Monster Hunter Wilds

- Alt+Enter mode transition;
- MHW ResizeHold completes normally;
- Alt+Tab away/back once;
- no crash;
- overlay and Lua remain usable after return.

Run the matrix primarily with Debug Log OFF.

One representative game should also be tested with Debug Log ON to prove the deep diagnostics are still available.

### 12.5 Native/non-XeFG smoke

Run at least one normal native D3D12 REFramework scenario without XeFG.

Verify:

- overlay remains functional;
- Lua/plugins remain functional;
- generic hook monitor remains functional;
- no XeFG debug toggle changes native renderer semantics;
- normal upstream logging is not accidentally hidden by a global log-level change.

---

## 13. Retention validation

Before/after R11, verify the logger initialization still uses the same `re2_framework_log.txt` path and the same `basic_logger_mt(..., true)` behavior.

No new files such as the following are allowed:

```text
re2_framework_debug_log.txt
xefg_debug.log
re2_framework_log.1.txt
```

unless the user explicitly changes the retention policy in a future task.

---

## 14. Non-goals

Do not:

- alter XeFG feature detection;
- alter OptiScaler detection;
- add FSRFG/DLSSG abstractions;
- hook OptiScaler private classes/functions;
- change DXGI hook slots;
- change COM ownership;
- change queue selection;
- change binding identity/generation;
- change MHW-only hold scope;
- add hold timeouts;
- change hook-monitor timing;
- change renderer reset order;
- change Lua behavior;
- change startup menu auto-hide behavior from PR #29;
- redesign the Configuration UI;
- add explanatory UI copy for `Debug Log`;
- add a second log file;
- change log retention/rotation/truncation;
- perform a repository-wide upstream logging cleanup.

---

## 15. Expected PR size

This should remain one R11 PR.

Target roughly:

```text
~150–300 effective LOC
~350 LOC soft ceiling
```

Do not split the config flag from the logging classification into separate PRs; the checkbox is not meaningful without the gated logs and the gated logs need the persisted control.

Do not exceed the soft ceiling merely to introduce an abstraction layer. Prefer straightforward guards and a narrow central flag.

---

## 16. Acceptance criteria

R11 is complete only when all of the following are true:

- [ ] existing `Configuration` section contains exactly one new `Debug Log` checkbox;
- [ ] no explanation/help UI was added;
- [ ] default is OFF;
- [ ] value persists in the existing `re2_fw_config.txt` mechanism;
- [ ] changing the value affects subsequent diagnostic logging without restart;
- [ ] global spdlog level is not used as the feature switch;
- [ ] default log retains concise runtime registration and binding outcome;
- [ ] default log retains rebind outcome if exercised;
- [ ] default log retains meaningful MHW ResizeHold transitions;
- [ ] default log retains hook-monitor preservation/recovery outcomes;
- [ ] default log retains Present/resize/device failures;
- [ ] successful `ResizeBuffers1` trace spam is hidden with Debug Log OFF;
- [ ] failed `ResizeBuffers1` remains explicitly visible with Debug Log OFF;
- [ ] `[XeFG][ResizeLifecycle]` stage-by-stage logging is hidden with Debug Log OFF;
- [ ] detailed PresentEntry/discovery/vtable/queue/pointer snapshots are hidden with Debug Log OFF;
- [ ] detailed diagnostics return with Debug Log ON;
- [ ] warnings/errors are not accidentally gated;
- [ ] no XeFG behavior changed;
- [ ] no native D3D11/D3D12 behavior changed;
- [ ] no Lua/plugin/overlay behavior changed;
- [ ] MHW-only hold policy is unchanged;
- [ ] R10 healthy-binding hook-monitor preservation is unchanged;
- [ ] `re2_framework_log.txt` filename/path/sink/truncate/retention behavior is unchanged;
- [ ] `git diff --check` passes;
- [ ] build/CI passes;
- [ ] final runtime smoke passes with Debug Log OFF;
- [ ] one representative Debug Log ON run confirms deep diagnostics are still available.

---

## 17. Final implementation report

The implementing agent should report:

1. files changed;
2. exact config key used for `Debug Log`;
3. default normal-log markers retained;
4. debug-only markers gated;
5. any existing log message whose severity changed and why;
6. confirmation that retention/logger construction was untouched;
7. build/static validation commands and results;
8. runtime validation performed;
9. any marker intentionally left unchanged because it is upstream/general rather than XeFG-specific.
