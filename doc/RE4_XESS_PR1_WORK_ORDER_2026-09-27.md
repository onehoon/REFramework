# RE4 XeSS Production — PR 1 Work Order

Base branch: feature/re4-xess  
PR target: feature/re4-xess  
Suggested implementation branch: feature/re4-xess-pr1-runtime-shell  
Date: 2026-09-27  
Scope: RE4-only production shell, UI/config contract, public XeSS runtime loading, and context lifetime scaffolding  
Do not merge without review.

Primary architecture:
- doc/RE4_XESS_PRODUCTION_ARCHITECTURE_2026-09-27.md

Research evidence:
- doc/RE4_XESS_BRIDGE_ARCHITECTURE_AND_RE_STATUS_2026-09-26.md

---

## 1. Objective

Implement the smallest production RE4 XeSS shell that proves these boundaries in code:

~~~text
RE4 only
    -> RE4XeSS Mod exists
    -> RE4-only Upscaling Mode UI/config exists
    -> libxess.dll is discovered in one of the two supported REFramework-relative locations
    -> the selected DLL is loaded from its exact path
    -> required public XeSS exports are resolved dynamically
    -> a D3D12 XeSS context can be created/destroyed against RE4's existing D3D12 device

all non-RE4 titles
    -> no RE4XeSS object
    -> no RE4XeSS UI
    -> no libxess.dll load initiated by RE4XeSS
    -> no RE4-specific runtime behavior
~~~

PR 1 is not an upscaling implementation yet.

It must not change SceneView size, projection jitter, temporal resources, command submission, output resources, Overlay state, swapchain content, or XeFG behavior.

---

## 2. Code-review findings

### 2.1 RE4-only registration already has a correct pattern

src/Mods.cpp already registers game-specific Mods through sdk::GameIdentity.

Use:

~~~cpp
if (sdk::GameIdentity::get().is_re4()) {
    m_mods.emplace_back(std::make_unique<RE4XeSS>());
}
~~~

Do not construct RE4XeSS for all games and merely return from callbacks. Registration itself must be RE4-only.

Keep defense-in-depth is_re4() checks inside callbacks that can change runtime state.

### 2.2 No D3D12Hook changes are required

Current REFramework already exposes:

~~~text
g_framework->get_renderer_type()
g_framework->get_d3d12_hook()
D3D12Hook::get_device()
D3D12Hook::get_command_queue()
~~~

PR 1 needs only the existing D3D12 device getter.

Do not modify:
- src/D3D12Hook.cpp
- src/D3D12Hook.hpp

### 2.3 Existing XeFG compatibility is protected

Treat these as no-touch for this PR:

~~~text
src/compatibility/xefg/**
existing XeFG logic in REFramework.cpp
existing XeFG logic in D3D12Hook.*
REFramework_XeFG_PreRetireSwapchainV1
~~~

RE4XeSS must not include XeFG compatibility headers or depend on XeFG internal state.

### 2.4 Debug Log needs only a neutral getter

REFrameworkConfig already owns REFrameworkConfig_DebugLog.

Do not restructure its existing XeFG forwarding.

Add only a read-only accessor such as:

~~~cpp
bool is_debug_log_enabled() const noexcept {
    return m_debug_log->value();
}
~~~

RE4XeSS should read the global setting through REFrameworkConfig, not through XeFGCompatibility.

### 2.5 Port exactly two verified RE4 Renderer corrections

feature/re4-xess still lacks the runtime-verified RE4 fixes from diagnostic PR #58.

Port only these shared/sdk/Renderer.hpp changes:

~~~text
RE4 Texture descriptor:
    RenderResource::get_runtime_size() + 0x18

RE4 D3D12 resource container:
    0xB8
~~~

Expected gating:

~~~cpp
if (v >= 73 || gi.is_sf6() || gi.is_re4()) {
    return RenderResource::get_runtime_size() + 0x18;
}

if (v >= 71) {
    if (gi.is_sf6() || gi.is_re4()) return 0xB8;
    if (gi.is_mhrise()) return 0x98;
    return 0xA0;
}
~~~

Do not generalize either change to all TDB 71 games.

Do not port RE4TemporalProbe or any diagnostic tracing code.

### 2.6 cmake.toml is editable; CMakeLists.txt is generated

cmake.toml uses globs, but the checked-in generated CMakeLists.txt explicitly enumerates sources.

PR 1 does not need an XeSS-specific cmake.toml dependency/include-path change. Add the new source files, then refresh the generated CMakeLists.txt source enumeration through cmkr. Commit CMakeLists.txt only if generation changes it.

---

## 3. XeSS ABI and runtime discovery for PR 1

REFramework does not own or distribute the Intel XeSS SDK/runtime package for this feature.

For the target deployment, OptiScaler installation provides the runtime DLL set.

Do not add:

~~~text
dependencies/xess
XeSS git submodule
Intel XeSS SDK package
Intel XeSS include directory
libxess.lib
Intel XeSS runtime DLL copies
XeFG SDK files
~~~

### 3.1 Compile-time ABI surface

Add one small internal ABI declaration header:

~~~text
src/mods/re4_xess/RE4XeSSApi.hpp
~~~

Keep only the public XeSS ABI declarations required by the RE4 producer.

PR 1 needs at least the declarations required for:

~~~text
xess_context_handle_t
xess_result_t
xess_version_t
xess_2d_t
xess_quality_settings_t

xessGetVersion
xessGetOptimalInputResolution
xessDestroyContext
xessSetVelocityScale
xessD3D12CreateContext
xessD3D12Init
xessD3D12Execute
~~~

Preserve the public ABI exactly: calling convention, enum values, field widths, and packing.

Public quality values used by the RE4 UI contract:

~~~text
ULTRA_PERFORMANCE = 100
PERFORMANCE       = 101
BALANCED          = 102
QUALITY           = 103
ULTRA_QUALITY     = 104
ULTRA_QUALITY_PLUS= 105
AA                = 106
~~~

Do not copy unrelated SDK samples or implementation code.

### 3.2 Supported runtime locations

Search exactly these locations, relative to the directory containing the loaded REFramework DLL:

~~~text
1. <REFramework directory>\libxess.dll
2. <REFramework directory>\OptiScaler\libxess.dll
~~~

Priority is top-level first.

If both exist, use the top-level DLL.

If neither exists, fail closed and keep native RE4 rendering.

Do not fall back to arbitrary PATH/current-working-directory discovery.

### 3.3 Resolve the REFramework directory from the loaded module

Base the search on:

~~~cpp
REFramework::get_reframework_module()
~~~

Use the repository's existing module-path helper where practical.

Do not derive the base directory from REFramework::get_persistent_dir(), the current working directory, or the game executable path by assumption.

### 3.4 Exact-path loading and adjacent dependencies

After selecting a candidate, load that exact full path.

Preferred behavior:

~~~text
LoadLibraryExW(
    selected_full_path,
    nullptr,
    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)
~~~

This allows DLL dependencies located next to the selected libxess.dll to resolve when the OptiScaler DLL set is inside the OptiScaler subfolder.

Do not use SetDllDirectory as a process-wide shortcut.

Do not mutate global DLL search state.

### 3.5 OptiScaler transparency

RE4XeSS does not identify whether the selected libxess.dll is an Intel runtime-facing DLL or an OptiScaler-provided XeSS frontend/proxy.

The module only finds the DLL, loads it, resolves public XeSS exports, and calls the public XeSS ABI.

No OptiScaler detection or private API is added.

### 3.6 No XeSS packaging

PR 1 must not copy or package XeSS DLLs.

Runtime-file deployment is outside REFramework's responsibility for this target architecture.

---

## 4. New production files

Create only:

~~~text
src/mods/re4_xess/RE4XeSS.hpp
src/mods/re4_xess/RE4XeSS.cpp
src/mods/re4_xess/RE4XeSSApi.hpp
src/mods/re4_xess/RE4XeSSRuntime.hpp
src/mods/re4_xess/RE4XeSSRuntime.cpp
~~~

Do not create empty placeholders for later:
- RE4XeSSFrame
- RE4XeSSD3D12
- RE4XeSSOutputHandoff

Those belong to later PRs.

---

## 5. RE4XeSS Mod contract

### 5.1 Mod identity and callbacks

get_name() returns RE4XeSS.

Implement only:
- on_initialize()
- on_initialize_d3d_thread()
- on_frame()
- on_draw_ui()
- on_device_reset()
- on_config_load()
- on_config_save()
- destructor or explicit shutdown path

Do not add Scene, Overlay, PostEffect, or Camera callbacks in PR 1.

### 5.2 Registration

In src/Mods.cpp:
- include mods/re4_xess/RE4XeSS.hpp
- register only for sdk::GameIdentity::get().is_re4()
- keep it in the normal non-BAREBONES Mod block
- do not reorder unrelated Mods

### 5.3 Upscaling Mode UI

Display only for RE4:

~~~text
RE4 XeSS

Upscaling Mode:
  Off
  Native AA
  Ultra Quality Plus
  Ultra Quality
  Quality
  Balanced
  Performance
  Ultra Performance
~~~

Do not add a separate Enabled checkbox. Off is disabled.

Persist key:

~~~text
RE4XeSS_UpscalingMode
~~~

Use stable config tokens, not UI indices:

~~~text
off
native_aa
ultra_quality_plus
ultra_quality
quality
balanced
performance
ultra_performance
~~~

Unknown persisted values fail closed to off and log a warning.

Define an internal UpscalingMode enum and explicit helpers:
- config token <-> UpscalingMode
- UpscalingMode -> display label
- UpscalingMode -> optional xess_quality_settings_t

Mapping:

~~~text
NativeAA          -> XESS_QUALITY_SETTING_AA
UltraQualityPlus  -> XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS
UltraQuality      -> XESS_QUALITY_SETTING_ULTRA_QUALITY
Quality           -> XESS_QUALITY_SETTING_QUALITY
Balanced          -> XESS_QUALITY_SETTING_BALANCED
Performance       -> XESS_QUALITY_SETTING_PERFORMANCE
UltraPerformance  -> XESS_QUALITY_SETTING_ULTRA_PERFORMANCE
Off               -> no XeSS quality value
~~~

Do not add DLSS, FSR, XeFG, or OptiScaler-backend choices to REFramework UI.

### 5.4 PR 1 meaning of an active mode

PR 1 does not upscale.

For this PR only:

~~~text
Off
    -> XeSS runtime/context shut down

any active XeSS mode
    -> request runtime/context bootstrap
    -> no SceneView change
    -> no jitter
    -> no xessD3D12Init
    -> no xessD3D12Execute
    -> no visible rendering change
~~~

The UI must not claim that upscaling is active. Show a clear runtime status such as:

~~~text
Context ready — PR1 bootstrap only; SR execute not active
~~~

Do not perform D3D12/XeSS lifecycle operations directly from ImGui drawing code. UI updates requested mode and marks a pending transition; on_frame reconciles it.

---

## 6. RE4XeSSRuntime design

RE4XeSSRuntime owns only:
- HMODULE for libxess.dll
- dynamically resolved public XeSS function table
- xess_context_handle_t
- small runtime state
- failure reason

No RE Engine logic. No UI logic. No OptiScaler logic. No XeFG logic.

Suggested states:

~~~text
Unloaded
ModuleReady
ContextReady
Faulted
~~~

Make the class non-copyable and RAII-safe.

### 6.1 Runtime discovery and DLL loading

Resolve the directory containing the loaded REFramework DLL, then probe:

~~~text
<REF directory>\libxess.dll
<REF directory>\OptiScaler\libxess.dll
~~~

Load the first existing candidate by exact full path.

Prefer LoadLibraryExW with DLL-load-directory semantics so dependencies adjacent to the selected DLL can resolve without changing process-wide search state.

Do not:

- use bare LoadLibraryW(L"libxess.dll") as the only lookup mechanism;
- search arbitrary PATH entries after the two supported locations fail;
- use an Intel SDK/dependencies directory;
- use SetDllDirectory globally;
- copy a DLL from code;
- call OptiScaler private APIs.

When Debug Log is enabled, log the REFramework directory, both candidates, the selected path, and the load result.

### 6.2 Required public exports

Use RE4XeSSApi.hpp as the minimal public ABI declaration surface. Keep its declarations ABI-compatible with the public XeSS interface and use those types for function pointers.

Resolve at least:

~~~text
xessGetVersion
xessGetOptimalInputResolution
xessDestroyContext
xessSetVelocityScale
xessD3D12CreateContext
xessD3D12Init
xessD3D12Execute
~~~

Optional capabilities may include:
- xessD3D12BuildPipelines
- xessSetLoggingCallback
- xessIsOptimalDriver
- xessGetProperties

Do not call xessD3D12Init or xessD3D12Execute in PR 1.

Missing required exports fail bootstrap before context creation.

### 6.3 Context creation eligibility

Create a context only when all are true:

~~~text
game == RE4
requested mode != Off
g_framework exists
renderer == D3D12
D3D12Hook exists
D3D12Hook::get_device() != nullptr
required exports resolved
~~~

Use the existing RE4 D3D12 device:

~~~text
xessD3D12CreateContext(existingDevice, &context)
~~~

Never create another D3D12 device.

Do not call xessGetOptimalInputResolution for rendering decisions in PR 1.

### 6.4 Teardown

Shutdown order:

~~~text
if context exists:
    xessDestroyContext(context)

clear context

if module reference is owned:
    FreeLibrary(module)

clear function table
state = Unloaded
~~~

PR 1 submits no XeSS GPU work, so no bridge GPU fence is needed yet.

### 6.5 Failure and retry

Do not retry a missing/broken runtime every frame.

Faulted stays faulted until a meaningful trigger:
- Off -> active
- explicit active-mode change after shutdown
- device reset/recreation
- re-enable after Off

Keep one useful failure reason for UI/logging.

---

## 7. Coordinator lifecycle

### Startup, Off

Default is Off.

With Off:
- do not search for or load libxess.dll
- do not create context
- do not mutate rendering

### Startup, persisted active mode

Mods::on_initialize_d3d_thread already loads config before each Mod D3D initializer.

If an active mode is persisted:
1. verify RE4
2. verify D3D12
3. obtain current device
4. load runtime
5. resolve exports
6. create context
7. become ContextReady

No rendering changes follow.

### Runtime mode change

On any mode change:
1. mark transition pending in UI/config layer
2. on_frame shuts down old context/module
3. if new mode is Off, remain Unloaded
4. otherwise bootstrap a new context

This intentionally mirrors the future reconfiguration boundary.

### Device reset

on_device_reset():
- shut down context/module
- clear cached device identity
- preserve requested mode
- mark bootstrap retry pending if mode is active

Re-bootstrap only after a valid D3D12 device returns.

Do not call any XeFG lifecycle function.

---

## 8. Logging

Use:
- [RE4XeSS][Config]
- [RE4XeSS][Runtime]
- [RE4XeSS][Failure]

Event-level logs may report:
- mode transition
- runtime load success/failure
- missing required export
- context create result
- context destroy result
- device-reset shutdown

Detailed fields are gated by:

~~~cpp
REFrameworkConfig::get()->is_debug_log_enabled()
~~~

Debug-only fields may include:
- resolved libxess.dll path
- XeSS runtime version
- D3D12 device pointer identity
- required/optional export availability
- selected quality enum
- runtime state transitions

Do not emit a line every frame in steady ContextReady state.

Do not reuse the [XeFG] prefix.

---

## 9. Protected/no-touch list

No behavioral changes in:

~~~text
src/compatibility/xefg/**
src/D3D12Hook.cpp
src/D3D12Hook.hpp
existing XeFG sections of src/REFramework.cpp
OptiScaler repository
~~~

Do not:
- change REFramework_XeFG_PreRetireSwapchainV1
- change hook monitor
- change swapchain binding
- change ResizeBuffers/ResizeTarget
- change XeFG loader handoff
- add RE4 branches inside XeFG compatibility
- add OptiScaler detection

If any appears necessary, stop and report instead of widening PR 1.

---

## 10. Expected changed-file set

Expected approximately:

~~~text
CMakeLists.txt                            # cmkr-generated source-list refresh, if needed

shared/sdk/Renderer.hpp

src/Mods.cpp
src/mods/REFrameworkConfig.hpp

src/mods/re4_xess/RE4XeSS.hpp
src/mods/re4_xess/RE4XeSS.cpp
src/mods/re4_xess/RE4XeSSApi.hpp
src/mods/re4_xess/RE4XeSSRuntime.hpp
src/mods/re4_xess/RE4XeSSRuntime.cpp
~~~

No XeSS SDK/submodule/dependency package should be added.

cmake.toml does not need an XeSS-specific include path or link dependency.

Because checked-in CMakeLists.txt is generated and enumerates files explicitly, refresh its source list through cmkr if needed after adding the new source files.

REFrameworkConfig.cpp should not need restructuring.

Do not port:
- RE4TemporalProbe.*
- RE4TemporalProbeSupport.hpp
- diagnostic command-list hooks
- recording-function hooks
- capture scenario UI
- diagnostic tests wholesale

---

## 11. Build-system work

Do not add a XeSS package, submodule, include directory, import library, or runtime-copy step.

The existing cmake.toml source glob already covers src/** files.

After creating the new source files, refresh the generated CMakeLists.txt source enumeration through cmkr.

Preferred:

~~~text
cmkr gen
~~~

or use the repository's existing cmkr/CMake regeneration flow.

If configure does not regenerate because cmake.toml itself did not change, force a local cmkr regeneration rather than adding a fake semantic dependency.

Commit CMakeLists.txt only if it actually changes.

There must be no XeSS library in target_link_libraries.

---

## 12. Build validation

Use the repository PR-build shape:

~~~powershell
cmake -S . -B build ^
  -G "Visual Studio 18 2026" ^
  -A x64 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DDEVELOPER_MODE=ON

cmake --build build --config Release --target REFramework
~~~

Required:
- 0 compile errors
- 0 link errors

There must be no unresolved direct xess import.

If practical, inspect dinput8.dll imports and verify libxess.dll is not a static import dependency. The only XeSS runtime load must come from the explicit two-location RE4XeSS loader.

---

## 13. Static acceptance checks

Before opening the PR:

### Protected paths

Run:

~~~text
git diff --name-only feature/re4-xess...HEAD
~~~

There must be no changed file under src/compatibility/xefg/ and no D3D12Hook.* change.

### Isolation

Confirm no unconditional RE4XeSS global/singleton constructor loads XeSS before Mods decides the game.

### No FG API

New production source must not contain runtime use of:
- libxess_fg
- xefgSwapChain
- XeFGCompatibility
- FGInput

### No backend selection

No runtime branches for:
- DLSS
- FSR
- OptiScaler backend
- NVIDIA
- AMD
- Intel GPU vendor

The frontend is public XeSS only.

### No render mutation

PR 1 must not modify:
- SceneView size
- projection matrices
- Color/Depth/Velocity
- Overlay TargetState
- swapchain buffers
- D3D12 resource barriers
- command-list submission

---

## 14. Manual runtime checklist

Do not claim tests that were not actually run.

### RE4 default Off

Expected:
- RE4 XeSS menu visible
- Upscaling Mode = Off
- no RE4XeSS-initiated libxess load
- no context
- rendering unchanged

### RE4 active mode with native libxess

Use Quality for the first smoke test.

Expected:
- normal libxess.dll resolution
- exports resolve
- existing RE4 D3D12 device used
- xessD3D12CreateContext succeeds
- UI says bootstrap/context ready only
- no visible render-size/image change

Switch back to Off:
- xessDestroyContext
- release module reference
- rendering unchanged

### RE4 active mode with libxess unavailable

Expected:
- fail closed
- useful one-time error
- no crash
- no per-frame retry spam
- native rendering unchanged

### Device reset

With active requested mode:
- context/module shuts down
- requested mode remains
- re-bootstrap only after valid D3D12 returns

### Non-RE4 regression

If a non-RE4 D3D12 game is available:
- no RE4 XeSS menu
- no RE4XeSS initialization log
- no libxess load initiated by this feature
- existing XeFG/OptiScaler compatibility unchanged

If not tested, state that explicitly.

---

## 15. Acceptance criteria

PR 1 is complete only when:

- RE4XeSS is constructed only for RE4.
- The full RE4 Upscaling Mode selector exists and defaults to Off.
- Off causes zero XeSS bootstrap.
- Active modes bootstrap runtime/context without changing rendering.
- No XeSS SDK/submodule/runtime package is added to REFramework.
- RE4XeSSApi.hpp contains only the minimal public XeSS ABI needed by the producer.
- No XeSS import library is linked.
- libxess.dll discovery is limited to <REF>\libxess.dll, then <REF>\OptiScaler\libxess.dll.
- The selected DLL is loaded by exact path without process-wide DLL search mutation.
- Required public exports are dynamic.
- Existing RE4 D3D12 device is used for context creation.
- Context/module teardown is RAII-safe.
- Device reset tears down safely.
- Failure is fail-closed and does not spam retries.
- RE4 Renderer offsets are ported exactly and remain RE4-only.
- Debug Log is read through a neutral REFrameworkConfig getter.
- Existing XeFG compatibility behavior is untouched.
- No SceneView/jitter/input/command/output/swapchain mutation exists.
- Release x64 build succeeds.

---

## 16. PR creation requirements

Open a Draft PR against feature/re4-xess.

PR description must state:
1. this is PR 1 of the RE4 XeSS production sequence;
2. scope is RE4-only;
3. there is no XeSS execute/render modification yet;
4. dynamic public XeSS loading preserves stock OptiScaler interception;
5. existing XeFG compatibility code is unchanged;
6. the two supported libxess.dll runtime locations;
7. runtime tests actually performed;
8. runtime tests remaining for the user.

Do not merge automatically.

---

## 17. Stop conditions

Stop and report instead of widening scope if:

- context creation seems to require changes inside existing XeFG compatibility;
- neither supported REFramework-relative libxess.dll location can provide a usable public XeSS frontend;
- exact-path loading from the OptiScaler subdirectory cannot resolve required adjacent DLL dependencies without unsafe global search-path mutation;
- current RE4 D3D12 device is unsuitable for XeSS context creation;
- the minimal public XeSS ABI declarations required by the producer conflict with the runtime-exported ABI;
- another game would require a shared Renderer behavior change;
- implementation requires SceneView/jitter/output mutation before PR 2;
- build integration would require bundling an Intel runtime DLL.

These are architecture questions and require review before implementation scope changes.
