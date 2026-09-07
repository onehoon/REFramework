# Work Order — XeFG `Debug Log` Early Bootstrap Fix

## 1. Purpose

Fix the current ordering bug where the persisted REFramework `Debug Log` option is applied **after** the important XeFG startup / runtime discovery / binding sequence has already happened.

This is a **small configuration/bootstrap fix only**.

Do not refactor XeFG binding, resize handling, hook-monitor policy, renderer lifecycle, or the general REFramework configuration system as part of this task.

---

## 2. Planning Base

Plan and implement against the latest `master`.

Baseline observed when this work order was written:

```text
b4e2e5ba09cb72aa2b3fe6b749807047f96536a9
Add RE4 OptiScaler XeFG architecture handoff
```

Refresh against the latest `master` before editing and preserve any newer unrelated work.

Relevant existing R11 document:

```text
doc/work-order/XEFG_REFACTOR_R11_FINAL_LOGGING_DEBUG_UI.md
```

R11 introduced the persistent `Debug Log` UI option and routed detailed XeFG diagnostics through:

```cpp
XeFGCompatibility::is_debug_log_enabled()
```

The logging policy itself is not being redesigned here.

---

## 3. Confirmed Problem

The current XeFG debug flag starts disabled:

```cpp
std::atomic<bool> XeFGCompatibility::s_debug_log_enabled{false};
```

The saved UI/config value is propagated in `REFrameworkConfig::on_config_load()`:

```cpp
XeFGCompatibility::set_debug_log_enabled(m_debug_log->value());
```

and is also updated when the user changes the `Debug Log` checkbox in the UI.

That live UI behavior is correct.

The bug is startup ordering.

Real Monster Hunter Wilds runtime evidence showed this sequence:

```text
20:38:50  REFramework process startup
20:38:50  XeFG runtime registry installed
20:39:18  XeFG binding accepted
20:39:28  REFrameworkConfig::on_config_load()
```

Therefore, even when the persisted option is already `true`, `XeFGCompatibility::s_debug_log_enabled` remains `false` during the first XeFG initialization and binding transaction.

As a result, startup-only diagnostics are lost, including some or all of:

```text
[XeFG][Module]
[XeFG][Exports]
[XeFG][LoaderHandoff]
[XeFG][RuntimeDispatch]
[XeFG][InitDesc]
[XeFG][InternalSwapchain]
[XeFG][QueueIdentity]
[XeFG][P2.1Probe]
[D3D12][Discovery]
[D3D12][HookInstall]
[D3D12][PhaseTransition]
```

Later debug diagnostics do appear after `REFrameworkConfig::on_config_load()` applies the saved value. This confirms that the checkbox and the runtime setter work; the defect is specifically that the persisted value is not available early enough.

---

## 4. Required Behavior

When the saved REFramework configuration contains:

```text
REFrameworkConfig_DebugLog=true
```

then `XeFGCompatibility::is_debug_log_enabled()` must return `true` **before any XeFG runtime discovery / loader handoff / API-hook installation / init dispatch / binding diagnostics can occur**.

When the persisted option is absent or false, startup behavior must remain equivalent to today:

```text
Debug Log = false
```

Detailed startup diagnostics must remain suppressed.

The existing UI checkbox must continue to work at runtime and must continue to persist normally.

---

## 5. Implementation Constraints

### 5.1 Do not globally move REFramework mod configuration initialization earlier

Do **not** solve this by moving the entire `REFrameworkConfig::on_config_load()` flow, all mod config loading, or general mod initialization earlier in process startup.

That would unnecessarily widen the behavioral surface of this fix.

Only the persisted `Debug Log` value needs an early bootstrap path.

### 5.2 Keep one runtime source of truth

The runtime source of truth remains:

```cpp
XeFGCompatibility::set_debug_log_enabled(bool)
XeFGCompatibility::is_debug_log_enabled()
```

Do not introduce a second permanent debug flag inside `D3D12Hook`, `REFramework`, the loader hook, or another subsystem.

### 5.3 Preserve normal config ownership

`REFrameworkConfig` remains the owner of the user-facing option:

```cpp
ModToggle::Ptr m_debug_log{
    ModToggle::create(generate_name("DebugLog"), false)
};
```

Do not rename the persisted key.

Do not create a second user-facing setting.

Do not change the default from `false`.

### 5.4 Early read must be read-only

The bootstrap path should read only the one persisted debug value required for early diagnostics.

It must not save/rewrite the config file, migrate unrelated settings, or trigger general configuration callbacks.

### 5.5 Failure must be safe

If the config cannot be found, cannot be parsed, or the key is absent, the early bootstrap must simply leave XeFG debug logging disabled.

Do not make REFramework startup fail because diagnostic configuration could not be read.

---

## 6. Recommended Implementation Shape

Inspect the current REFramework config-file loading utilities and reuse the existing parser / key semantics where possible.

Preferred architecture:

```text
REFramework startup
    |
    +-- early diagnostic bootstrap
    |      |
    |      +-- read persisted REFrameworkConfig_DebugLog only
    |      +-- XeFGCompatibility::set_debug_log_enabled(saved_value)
    |
    +-- install / process XeFG loader and runtime hooks
    |      |
    |      +-- startup debug diagnostics now obey saved value
    |
    +-- normal REFramework/mod initialization
           |
           +-- REFrameworkConfig::on_config_load()
           +-- set_debug_log_enabled(m_debug_log->value()) again
```

The later normal setter is intentional and should remain. It provides the final normal synchronization with `REFrameworkConfig` and keeps runtime UI changes authoritative.

A small dedicated helper is acceptable, for example conceptually:

```cpp
void bootstrap_xefg_debug_log_from_config() noexcept;
```

The exact name/location should follow the current source architecture after inspection.

Do not hard-code a separate config-file path or duplicate parsing logic if an existing low-level config reader can safely be reused before mod initialization.

If the normal high-level config object cannot safely be constructed at that point, implement the narrowest possible early read using the same config location/key conventions.

---

## 7. Ordering Requirement

The early bootstrap must happen before code paths that can produce debug-gated XeFG startup diagnostics.

At minimum verify ordering relative to:

- already-loaded XeFG runtime discovery;
- `LdrLoadDll` handoff / XeFG module load handling;
- `XeFGRuntimeRegistry::install_for_module()`;
- `XeFGCompatibility::dispatch_init_desc()`;
- `XeFGDiscovery` internal swapchain / queue identity capture;
- initial external XeFG D3D12 binding;
- initial D3D12 discovery / hook-install diagnostics.

Do not merely call the bootstrap somewhere before `REFrameworkConfig::on_config_load()`; that is not sufficient if XeFG initialization can still happen first.

---

## 8. Do Not Change

Do not modify the functional behavior of:

- XeFG runtime detection;
- XeFG runtime registry slot handling;
- `InitFromSwapChainDesc` interception;
- internal swapchain discovery;
- queue validation / queue relation policy;
- external D3D12 binding;
- Present / Present1 behavior;
- MHW resize transition hold;
- ResizeBuffers / ResizeBuffers1 / ResizeTarget behavior;
- hook-monitor timeout preservation;
- renderer reset / reacquire logic;
- OptiScaler compatibility policy;
- Lua smoke behavior;
- log rotation / retention policy.

This PR is configuration ordering only.

---

## 9. Logging Expectations After Fix

### Debug Log enabled before launch

A fresh launch with saved `Debug Log=true` should contain early detailed records such as the applicable subset of:

```text
[XeFG][Module] ...
[XeFG][Exports] ...
[XeFG][RuntimeRegistry] ... init_desc=... get_swapchain=...
[XeFG][RuntimeDispatch] api = InitFromSwapChainDesc ...
[XeFG][InitDesc] ...
[XeFG][InternalSwapchain] ...
[XeFG][QueueIdentity] ...
[XeFG][P2.1Probe] ...
[D3D12][Discovery] ...
[D3D12][HookInstall] ...
```

Exact lines depend on the runtime path taken; do not add artificial log messages merely to satisfy this list.

The key requirement is that any existing debug-gated diagnostic reached before normal config initialization is now allowed to log when the persisted option was already enabled.

### Debug Log disabled before launch

A launch with saved `Debug Log=false` should retain the R11 normal-user logging policy.

The detailed startup diagnostics above must remain suppressed, while essential default records such as successful runtime installation/binding summaries continue as currently designed.

---

## 10. Important Note About `arm_debug`

Do not use this line as the sole validation marker:

```text
[XeFG][ResizeHold] action = arm_debug
```

Current code only emits it when `arm_xefg_resize_transition_hold()` is reached, which in MHW is associated with the qualifying `ResizeTarget` hold path.

A session can have debug logging correctly enabled and still produce no `arm_debug` line if no qualifying `ResizeTarget` occurs after the option is active.

Use startup diagnostics such as `[XeFG][Module]`, `[XeFG][RuntimeDispatch]`, `[XeFG][QueueIdentity]`, or equivalent debug-gated initialization records to validate the ordering fix.

---

## 11. Tests

Add focused automated coverage where practical.

Minimum expectations:

1. persisted debug value `true` is propagated to `XeFGCompatibility` by the early bootstrap;
2. persisted debug value `false` leaves debug logging disabled;
3. missing key defaults safely to false;
4. malformed/unavailable config does not break startup and defaults safely to false;
5. later normal `REFrameworkConfig::on_config_load()` still synchronizes the runtime flag;
6. UI toggle behavior remains unchanged.

Avoid tests that depend on real OptiScaler or a physical XeFG-capable GPU for the basic configuration behavior.

If the current architecture makes direct unit testing difficult, isolate the config-value extraction enough that its semantics can be tested without initializing D3D12.

---

## 12. Manual Validation

Use Monster Hunter Wilds + OptiScaler + XeFG because the issue was observed there and its XeFG initialization happens before normal mod config loading.

### Case A — saved Debug Log ON

Before launch, ensure the persisted option is already enabled.

Expected:

- startup completes normally;
- detailed XeFG startup diagnostics appear **before** the first `REFrameworkConfig::on_config_load()` line;
- initial XeFG binding succeeds as before;
- no change in swapchain source, queue relation, or renderer mode;
- later resize diagnostics continue to appear normally.

A useful ordering proof should resemble:

```text
[XeFG][Module] ...
[XeFG][RuntimeDispatch] ...
[XeFG][QueueIdentity] ...
[XeFG][Bind] accepted = true ...
...
REFrameworkConfig::on_config_load()
```

### Case B — saved Debug Log OFF

Expected:

- startup completes normally;
- detailed startup diagnostics remain suppressed;
- normal R11 default summaries remain available;
- no functional XeFG difference from Case A.

### Case C — runtime toggle

Start with Debug Log OFF, then enable it from the Configuration UI.

Expected:

- future debug-gated diagnostics become visible immediately;
- setting persists;
- on the next process launch, startup diagnostics are visible from the early XeFG phase.

---

## 13. Acceptance Criteria

The task is complete when all of the following are true:

- [ ] persisted `REFrameworkConfig_DebugLog=true` is applied before XeFG startup discovery/binding diagnostics;
- [ ] startup debug records can appear before `REFrameworkConfig::on_config_load()`;
- [ ] persisted false/missing config keeps the current default quiet logging behavior;
- [ ] UI checkbox behavior and persistence remain unchanged;
- [ ] no second permanent debug state is introduced;
- [ ] no general mod/config initialization ordering is changed;
- [ ] no XeFG functional behavior changes;
- [ ] automated tests covering the narrow bootstrap semantics pass;
- [ ] existing relevant REFramework tests pass;
- [ ] Debug/Release build validation passes according to repository policy;
- [ ] manual MHW validation confirms early logs with saved Debug Log ON and no regression with it OFF.

---

## 14. PR Scope / Review Guidance

Keep this as one small PR.

The expected functional diff should be limited to:

- a narrow early read/bootstrap of the existing `Debug Log` setting;
- invocation at the correct pre-XeFG-init point;
- focused tests;
- any minimal comments needed to explain why normal config loading is too late for startup XeFG diagnostics.

Do not bundle unrelated cleanup.

During review, the most important question is not whether the code can read the config early, but whether it does so **early enough to precede every practical XeFG initialization path** while preserving the normal REFramework configuration lifecycle.