# REFramework × OptiScaler XeFG Compatibility Refactor Architecture

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Planning baseline: `master` @ `0a5f7a3bfca4367abe3379cc2635d1a7bb894f53`  
Baseline commit: `fix: hold XeFG rendering across resize transitions (#16)`

---

## 1. Purpose

This document defines the architecture and implementation boundaries for the next refactor of the REFramework fork.

This is **not** a general REFramework refactor.

The fork exists first and foremost as REFramework, and REFramework's original purpose, features, game integration, scripting, plugin/mod behavior, renderer behavior, input behavior, overlay behavior, VR paths, and normal D3D behavior must not be degraded for architectural cleanliness.

The refactor has one narrow product goal:

> Isolate and harden the fork-specific compatibility code required for reliable coexistence between REFramework and OptiScaler when OptiScaler uses Intel XeFG output on D3D12, while minimizing long-term conflict with both REFramework upstream and OptiScaler changes.

The target runtime topology remains:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (this fork)
Special K    = absent
FG Output    = Intel XeFG
Renderer     = D3D12
```

The compatibility target is specifically **OptiScaler + XeFG**.

FSRFG and DLSSG are not refactor targets. They already expose the REFramework overlay in the intended use cases and are not currently required by this fork's product scope. Do not create generic FG architecture merely because it might be reusable later.

---

## 2. Non-Negotiable Refactor Conditions

### 2.1 REFramework functionality preservation is the highest priority

The refactor must preserve REFramework's existing functionality and behavior.

No change is justified merely because existing upstream code appears old, awkward, duplicated, or stylistically inconsistent.

A change belongs in this refactor only when it directly contributes to at least one of the following:

1. OptiScaler XeFG compatibility isolation;
2. OptiScaler XeFG runtime stability;
3. reduced conflict when following REFramework upstream;
4. reduced coupling to OptiScaler/XeFG implementation details;
5. easier diagnosis and maintenance of the XeFG compatibility path.

If a change does not satisfy one of those conditions, it is out of scope.

### 2.2 XeFG only

Do not redesign FSRFG, DLSSG, Streamline, D3D11, generic DXGI, or generic frame-generation handling.

Do not introduce an `IFrameGenerationProvider`, generic FG provider registry, generic multi-provider state machine, or equivalent architecture in this refactor.

If a future non-XeFG compatibility issue is reproduced, common abstractions can be extracted later from proven implementations.

### 2.3 Default behavior must remain upstream-like when XeFG compatibility is inactive

The intended control-flow rule is:

```text
No relevant OptiScaler/XeFG presentation path
    -> normal REFramework behavior

Validated OptiScaler/XeFG path detected
    -> fork XeFG compatibility path may take ownership of the required presentation binding

XeFG compatibility discovery/validation fails
    -> do not destabilize the game or OptiScaler
    -> do not invent fallback mutation of XeFG internals
    -> keep REFramework's safest available existing path
    -> log a concise failure reason
```

The XeFG subsystem must be additive and conditional, not a replacement for normal REFramework rendering.

### 2.4 Upstream flexibility is an architectural requirement

The refactor must deliberately reduce the amount of XeFG-specific code embedded in high-conflict upstream files.

The goal is not zero changes to upstream-owned files. That would force awkward indirection and can be worse than a few clear integration points.

The goal is:

> Keep fork-specific XeFG implementation in a compatibility island and keep the integration surface with upstream REFramework small, explicit, and easy to rebase.

### 2.5 OptiScaler private implementation details are not a contract

Do not depend on:

- private C++ class layouts in OptiScaler;
- private offsets inside OptiScaler wrapper objects;
- Intel XeFG private object offsets;
- OptiScaler internal function addresses or mangled private symbols;
- a particular OptiScaler proxy filename beyond the public XeFG runtime module that is actually required;
- a hard-coded OptiScaler version;
- GPU-vendor branching that is unrelated to observed XeFG behavior.

Continue to rely on observable/public boundaries already proven by P1-P3:

- `libxess_fg.dll` module loading;
- exported XeFG public functions;
- `xefgSwapChainD3D12InitFromSwapChainDesc`;
- observable `IDXGIFactory2::CreateSwapChainForHwnd` activity during the XeFG init transaction;
- standard COM identity and D3D12 interfaces;
- actual presentation-creation queue;
- actual internal presentation swapchain;
- standard DXGI Present/resize methods.

---

## 3. Current Master Baseline That Must Be Preserved

The latest planning baseline is:

```text
0a5f7a3bfca4367abe3379cc2635d1a7bb894f53
fix: hold XeFG rendering across resize transitions (#16)
```

The current implementation has accumulated several fixes that are now compatibility contracts. The refactor must preserve their semantics before any later cleanup or logging reduction.

### 3.1 Multiple XeFG runtime modules

The fork no longer assumes one process-global `libxess_fg.dll` instance.

Current master keeps an exact-HMODULE runtime registry with fixed dispatch thunks and stores, per runtime:

```text
HMODULE
path
slot
InitFromSwapChainDesc export
GetSwapChainPtr export
FunctionHook for InitFromSwapChainDesc
optional FunctionHook for GetSwapChainPtr
install state
```

This was introduced because real games can load more than one XeFG runtime module or otherwise expose module-loading patterns where a single global export hook is insufficient.

**Refactor invariant:** exact-module dispatch must survive extraction. Do not regress to `GetModuleHandleW(L"libxess_fg.dll")` as the authoritative single-runtime model.

### 3.2 Serialized XeFG init transaction

The current `xefgSwapChainD3D12InitFromSwapChainDesc` path serializes the XeFG init transaction.

During the transaction it records at least:

```text
XeFG context
HWND
outer/init command queue
factory
presentation queue captured from factory creation
candidate internal swapchain
factory-create success
outer XeFG init result
```

A temporary instance hook on `IDXGIFactory2::CreateSwapChainForHwnd[15]` observes the internal presentation swapchain created during the XeFG initialization window.

**Refactor invariant:** keep the transaction bounded to the outer XeFG init call and remove the temporary factory hook after the original XeFG init returns.

### 3.3 Presentation queue authority

The proven OptiScaler + XeFG path may use an outer XeFG init queue and a different presentation-creation queue on the same D3D12 device.

The compatibility implementation validates queue/device identity and, for the known working relation:

```text
init queue          = DIRECT
presentation queue  = DIRECT
queue relationship  = distinct same device
selected queue      = presentation queue
```

The presentation queue is the authority for REFramework rendering on this XeFG path.

**Refactor invariant:** never collapse this back into "use the queue passed to XeFG init" without evidence.

### 3.4 Internal presentation swapchain, not public interpolation proxy

The current design renders REFramework through the validated XeFG internal presentation swapchain captured from the DXGI factory path.

`xefgSwapChainD3D12GetSwapChainPtr` remains useful for observation/diagnosis, but the public XeFG proxy is not the final REFramework overlay rendering target.

**Refactor invariant:** do not move REFramework rendering to the public XeFG interpolation proxy merely to simplify code.

### 3.5 Present and Present1

The active XeFG instance binding hooks both:

```text
Present[8]
Present1[22]
```

Both converge on shared REFramework Present lifecycle handling.

This is necessary because the OptiScaler/XeFG path can use `Present1` as a first-class presentation entry point.

**Refactor invariant:** `Present1` remains a first-class supported entry point.

### 3.6 Resize coverage

The active XeFG binding hooks:

```text
ResizeBuffers[13]
ResizeTarget[14]
ResizeBuffers1[39]
```

`ResizeBuffers1` pre-reset behavior is already proven necessary to release REFramework backbuffer/RTV ownership before OptiScaler/XeFG completes the resize.

**Refactor invariant:** all three resize paths retain the current reset ordering and recursion behavior.

### 3.7 Strong COM ownership

Current master strongly owns the active XeFG binding using `ComPtr` for:

```text
IDXGISwapChain3
ID3D12CommandQueue
ID3D12Device4
```

This is not cosmetic. The current `VtableHook` implementation does not own the target COM object, and hook removal accesses the target object to restore its vtable.

**Refactor invariant:** the old XeFG swapchain must remain strongly referenced until the old vtable hook has been fully removed/restored.

Required ordering:

```text
strong old swapchain reference alive
    -> old VtableHook still installed
    -> renderer reset if required
    -> old VtableHook removed
    -> only then old COM ownership may be released
```

### 3.8 Transactional binding replacement

Current master supports active XeFG binding replacement.

For a changed swapchain, the intended semantic is:

```text
validate candidate
    -> create/prepare the new instance hook first
    -> if preparation fails, leave old binding unchanged
    -> reset old REFramework renderer
    -> remove old hook while old object is still alive
    -> commit new strong ownership / queue / device / mode / hook
    -> increment binding generation
```

For a same-swapchain binding update, queue/device/mode can be updated in place after the renderer reset.

**Refactor invariant:** no failed new-hook preparation may destroy a valid old binding.

### 3.9 Hook-monitor preservation

Current master prevents the generic REFramework hook monitor from destructively tearing down a complete active XeFG instance binding merely because the normal Present timing heuristic expires.

The preservation predicate currently requires a complete, internally consistent XeFG binding.

**Refactor invariant:** a healthy active XeFG binding must not be destroyed by generic timeout recovery.

This protection must remain narrow. Do not disable generic D3D recovery globally.

### 3.10 Resize transition hold

PR #16 added a narrow XeFG render hold for the mode-change sequence observed around `ResizeTarget`.

The current semantic is:

```text
tracked XeFG ResizeTarget
    -> REFramework renderer reset
    -> arm XeFG resize-transition hold
    -> forward original ResizeTarget

while hold active:
    Present/Present1 still forwarded
    REFramework render callbacks suppressed
    hook-monitor liveness maintained from real Present activity

successful tracked ResizeBuffers or ResizeBuffers1
    -> complete hold
    -> subsequent Present can rebuild/render normally

failed ResizeTarget
    -> clear hold

binding replacement / external bind / unhook
    -> clear stale hold state
```

**Refactor invariant:** do not replace this with arbitrary sleep, Present-count timeout, GPU wait, or speculative timer recovery as part of the refactor.

---

## 4. Current Structural Problem

The current implementation works, but the fork-specific XeFG compatibility code is distributed across upstream-sensitive areas.

### 4.1 `D3D12Hook.cpp` has become the XeFG implementation container

Current `D3D12Hook.cpp` contains, in addition to normal REFramework D3D12 logic:

- XeFG runtime registry;
- fixed runtime thunk dispatch;
- XeFG API hook installation;
- init transaction serialization;
- temporary factory hook management;
- internal swapchain capture;
- queue identity validation;
- candidate publication;
- pending binding storage;
- active XeFG binding identity;
- strong XeFG COM ownership;
- initial external bind;
- atomic replacement;
- binding generation;
- XeFG Present suppression state;
- resize event state;
- resize transition hold;
- detailed XeFG diagnostics.

This makes future upstream `D3D12Hook` changes harder to import and makes XeFG maintenance require editing the same file that owns native REFramework D3D12 behavior.

### 4.2 `D3D12Hook.hpp` exposes many XeFG-specific details

Current public/protected state includes direct XeFG concepts such as:

```text
SwapchainSource::XeFGInternal
install_xefg_api_hooks_if_available
install_xefg_api_hooks_for_module
xefg_init_desc_dispatch
xefg_get_swapchain_dispatch
mark_xefg_probe_pending
process_pending_xefg_probe
notify_xefg_module_loaded
XeFG resize event APIs
XeFG binding generation
XeFG observe-only state
XeFG strong COM ownership fields
XeFG transition-hold fields
```

Some bridge surface will remain necessary, but the implementation detail should not remain spread through the core class declaration.

### 4.3 `REFramework.cpp` knows too much about XeFG

The core framework currently contains XeFG-specific behavior in two important places:

1. loader handling / `LdrLoadDll` handoff and module notification;
2. hook-monitor recovery decisions.

Those are valid integration points, but the core should not own XeFG runtime policy.

The final design should make these call into a narrow compatibility façade instead.

### 4.4 Diagnostics are mixed with runtime policy

Detailed address/vtable/queue/backbuffer logging was necessary during P1-P3 investigation.

It should remain available for debugging, but logging detail should not define or complicate the compatibility architecture.

Logging cleanup is deliberately deferred to the final logging PR after the runtime architecture is stable.

---

## 5. Target Architecture

### 5.1 Design summary

Create a narrow XeFG compatibility island under REFramework source, for example:

```text
src/
  compatibility/
    xefg/
      XeFGCompatibility.hpp
      XeFGCompatibility.cpp
      XeFGRuntimeRegistry.hpp
      XeFGRuntimeRegistry.cpp
      XeFGDiscovery.hpp
      XeFGDiscovery.cpp
      XeFGBinding.hpp
      XeFGBinding.cpp
```

Exact filenames may change during implementation if a smaller split is cleaner, but responsibilities must remain separated as described below.

Do **not** create generic frame-generation infrastructure in this refactor.

### 5.2 Dependency direction

Desired dependency direction:

```text
REFramework core / D3D12Hook
        |
        | narrow explicit calls
        v
XeFGCompatibility façade
        |
        +--> XeFGRuntimeRegistry
        +--> XeFGDiscovery
        +--> XeFGBinding
```

The reverse dependency must be tightly controlled.

The compatibility subsystem may need a small bridge back to `D3D12Hook` for physical instance hook targets and renderer reset/liveness operations, but it must not become a second copy of REFramework renderer logic.

### 5.3 Compatibility façade

`XeFGCompatibility` should be the only XeFG-facing API that high-level REFramework core needs to know.

Conceptual responsibilities:

```cpp
class XeFGCompatibility {
public:
    static XeFGCompatibility& instance();

    void on_loader_notification_seen();
    void on_module_loaded(HMODULE module, std::wstring_view path);
    void process_pending_work();

    // Candidate/binding coordination with D3D12Hook.
    bool has_pending_binding() const;
    std::optional<XeFGBindingCandidate> consume_pending_binding();

    // Hook-monitor policy.
    bool should_preserve_active_binding(const D3D12Hook& hook) const;
};
```

This is conceptual, not a required exact API.

The important architectural property is that `REFramework.cpp` should not need to know about runtime slots, XeFG export hook objects, queue relation enums, init transaction internals, or resize state.

### 5.4 XeFGRuntimeRegistry

This component owns module/export hook lifetime and exact-module dispatch.

Responsibilities:

- enumerate already-loaded `libxess_fg.dll` modules when appropriate;
- accept exact `HMODULE` notifications;
- deduplicate exact modules;
- allocate a runtime dispatch slot;
- resolve required/optional exports;
- install the export hooks;
- keep each hook's original function associated with the correct `HMODULE`;
- reject capacity overflow without corrupting existing runtimes;
- provide stable dispatch to the discovery layer;
- retain current support for multiple runtime modules.

Must **not** own:

- REFramework renderer reset;
- D3D12 swapchain instance hook lifecycle;
- hook-monitor policy;
- generic DXGI discovery;
- OptiScaler private state.

### 5.5 XeFGDiscovery

This component owns the bounded `InitFromSwapChainDesc` observation transaction.

Responsibilities:

1. serialize the current XeFG init observation transaction;
2. capture:
   - context,
   - HWND,
   - init queue,
   - factory;
3. install the temporary instance hook on factory `CreateSwapChainForHwnd[15]`;
4. observe the candidate internal swapchain and actual presentation-creation queue;
5. remove the temporary factory hook when the outer original XeFG init returns;
6. validate candidate interfaces and HWND;
7. validate queue/device COM identities;
8. classify the queue relation;
9. select render mode / observe-only mode according to the already-proven policy;
10. publish an immutable `XeFGBindingCandidate` to the binding coordinator.

Suggested candidate shape:

```cpp
struct XeFGBindingCandidate {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> presentation_queue;
    Microsoft::WRL::ComPtr<ID3D12Device4> device;
    HWND hwnd{};
    XeFGQueueRelation relation{};
    bool observe_only{true};
};
```

The exact device ownership location can be decided during implementation, but candidate validation must be complete before active binding mutation begins.

`GetSwapChainPtr` observation may remain in this layer as diagnostic/lifecycle evidence, but it must not silently become the render-target authority.

### 5.6 XeFGBinding

This component owns the state and invariants of the **active XeFG compatibility binding**.

Responsibilities:

- strong COM ownership of active XeFG swapchain, selected queue, and device;
- binding identity;
- binding generation;
- render/observe-only mode;
- binding health predicate;
- safe initial bind coordination;
- safe same-object update;
- safe changed-object replacement coordination;
- resize transition hold state;
- resize event identity/state required by the compatibility logic;
- clear/reset of XeFG-specific state on rebind/unhook.

Suggested binding identity:

```text
swapchain COM object
selected presentation queue
D3D12 device identity
render/observe mode
binding generation
```

The physical `VtableHook` can remain owned by `D3D12Hook` if that produces the lowest-risk upstream integration, but the ownership ordering must be explicit and controlled by XeFG binding operations.

Do not move the `VtableHook` merely for architectural purity if doing so requires broad exposure of `D3D12Hook` internals.

A safe compromise is acceptable:

```text
XeFGBinding owns semantic state + strong COM lifetime
D3D12Hook owns physical hook callbacks / VtableHook
XeFGBinding replacement protocol tells D3D12Hook exactly when old hook removal is safe
```

This refactor prioritizes proven behavior and upstream mergeability over maximal object-oriented separation.

### 5.7 D3D12Hook after refactor

`D3D12Hook` must remain the physical owner of normal REFramework D3D12 hook entry points and renderer callbacks.

It should continue to own the generic mechanics that are actually D3D12Hook responsibilities:

```text
native phase-1 discovery
normal Present hook callback
Present1 callback
ResizeBuffers callback
ResizeTarget callback
ResizeBuffers1 callback where required by the active binding
REFramework on_present / on_post_present callback dispatch
native command-queue discovery
normal D3D12 renderer lifecycle integration
```

XeFG-specific decisions inside those callbacks should be reduced to narrow policy queries/notifications, for example conceptually:

```cpp
const auto xefg_policy = xefg_compat.on_present_enter(*this, swap_chain);

if (!xefg_policy.suppress_renderer) {
    run_reframework_present_callback();
}

const auto result = original_present();

xefg_compat.on_present_exit(*this, result);
```

The exact API can be simpler. Avoid building a generic event bus.

### 5.8 REFramework.cpp after refactor

`REFramework.cpp` should retain only unavoidable integration hooks.

Loader path:

```text
loader notification says libxess_fg.dll was observed
    -> mark compatibility work pending

LdrLoadDll returns successfully for exact libxess_fg.dll
    -> pass exact HMODULE/path to XeFGCompatibility
```

The core file should not manage runtime slots or export FunctionHook objects.

Hook monitor:

```text
generic monitor timeout
    -> ask D3D12Hook / XeFGCompatibility whether a complete active XeFG binding must be preserved
    -> preserve only when the narrow health predicate is true
    -> otherwise execute existing upstream recovery behavior
```

No global change to monitor timing is part of this refactor.

---

## 6. Required Runtime Invariants After Refactor

The following invariants are release-blocking for PRs 1-3.

### 6.1 Native REFramework behavior remains the default

When no valid XeFG compatibility binding exists:

- native D3D12 behavior remains unchanged;
- D3D11 behavior remains unchanged;
- REFramework overlay behavior remains unchanged;
- scripts/plugins/mods remain unchanged;
- hook monitor retains its existing generic behavior;
- no XeFG-specific state may suppress normal renderer callbacks.

### 6.2 XeFG discovery never changes the original XeFG call contract

Export hooks must call the correct original function for the exact hooked runtime module.

Do not alter XeFG parameters merely to make REFramework easier to hook.

Return the original result unless the existing implementation already requires a precisely documented behavior.

### 6.3 Candidate validation occurs before mutation

A candidate must not replace the active binding until required interfaces and device/queue relationship are validated.

Invalid candidates are rejected without destroying a working active binding.

### 6.4 Strong ownership outlives hook removal

For an old XeFG binding:

```text
old ComPtr ownership
    must remain valid while
old VtableHook exists or is being removed
```

No refactor may reverse that order.

### 6.5 Changed-object replacement is prepare-before-destroy

Required sequence:

```text
candidate validated
    -> new hook preparation attempted
    -> preparation success confirmed
    -> old renderer reset
    -> old hook removed
    -> old semantic binding retired
    -> new ownership and hook committed
    -> generation incremented
```

If preparation fails:

```text
old binding + old hook + old renderer state remain authoritative
```

### 6.6 Same-object queue/mode changes are real binding changes

Do not define binding identity as swapchain pointer only.

At minimum, changes in selected queue or observe/render mode must trigger the current safe update path.

### 6.7 Present forwarding continues during resize hold

The transition hold is a **REFramework renderer hold**, not a DXGI Present block.

During the hold:

- original Present/Present1 is still called;
- REFramework renderer/mod callbacks that depend on released backbuffers are suppressed;
- actual Present activity may keep the hook monitor alive;
- no synthetic frame or fake Present is introduced.

### 6.8 Successful resize completion is authoritative

The hold completes only on successful tracked `ResizeBuffers` or `ResizeBuffers1` according to the current behavior.

Do not clear the hold merely because an arbitrary amount of time elapsed.

### 6.9 Healthy XeFG binding blocks destructive generic recovery only

The hook monitor preservation predicate must remain strict.

A partially constructed, stale, or inconsistent binding must not be treated as healthy solely because XeFG was once detected.

### 6.10 OptiScaler failure must not become a REFramework crash amplifier

The compatibility layer must fail conservatively.

If an XeFG runtime hook cannot be installed, candidate validation fails, or a new instance hook cannot be prepared:

- do not corrupt existing REFramework native state;
- do not mutate OptiScaler private state;
- do not delete/patch unknown hooks;
- do not tear down a still-valid old XeFG binding unless required by explicit lifecycle state;
- leave a concise actionable log event.

---

## 7. PR 1 — Extract XeFG Runtime and Discovery Infrastructure

### 7.1 Goal

Move XeFG runtime-module handling and init-time discovery out of `D3D12Hook.cpp` without intentionally changing runtime behavior.

This PR is primarily an ownership/location refactor.

It must not redesign active binding/rebind/resize semantics.

### 7.2 Primary extraction targets

Move the following categories out of `D3D12Hook.cpp`:

#### Runtime registry state

```text
kMaxXefgRuntimes
XefgRuntimeInstallState
XefgRuntimeHook
g_xefg_runtimes
runtime slot allocation
runtime lookup by HMODULE
runtime lookup by slot
fixed InitDesc/GetSwapChain thunks
```

#### Runtime hook installation

```text
install_xefg_api_hooks_if_available
install_xefg_api_hooks_for_module
exact HMODULE export resolution
InitFromSwapChainDesc hook creation
optional GetSwapChainPtr hook creation
```

#### Init transaction state

```text
XefgInitTransaction
g_xefg_init_transaction_mutex
g_xefg_transaction
g_xefg_factory_hook
```

#### Discovery helpers

```text
XeFG queue identity capture/classification
XeFG candidate validation
CreateSwapChainForHwnd temporary detour path
GetSwapChainPtr observation
pending candidate publication storage
```

Diagnostic-only generic helpers may stay temporarily if moving them would unnecessarily mix PR1 with logging work.

### 7.3 Recommended files

Minimum preferred layout:

```text
src/compatibility/xefg/XeFGRuntimeRegistry.hpp
src/compatibility/xefg/XeFGRuntimeRegistry.cpp
src/compatibility/xefg/XeFGDiscovery.hpp
src/compatibility/xefg/XeFGDiscovery.cpp
src/compatibility/xefg/XeFGCompatibility.hpp
src/compatibility/xefg/XeFGCompatibility.cpp
```

If this is too many tiny files for the actual implementation, runtime registry and discovery may initially share one `.cpp`, but keep their state/roles logically separated.

### 7.4 D3D12Hook integration after PR1

`D3D12Hook` should no longer own runtime registry internals.

It may temporarily retain the active binding methods:

```text
consume pending XeFG candidate
bind_external_swapchain
replace_xefg_binding
XeFG resize lifecycle
```

That is intentional. PR1 should not combine discovery extraction with active-binding ownership changes.

### 7.5 REFramework loader integration after PR1

The loader handoff should call the compatibility façade rather than a D3D12Hook static XeFG API.

Conceptually:

```cpp
XeFGCompatibility::instance().on_module_loaded(module, full_path);
```

The lightweight loader-notification callback can mark pending work through the same façade.

Important:

- keep heavy hook installation outside unsafe loader-lock context;
- preserve the existing post-`LdrLoadDll` exact-module handoff behavior;
- do not add worker threads solely for architectural separation.

### 7.6 PR1 forbidden changes

Do not change:

- current active binding selection policy;
- selected presentation queue policy;
- VtableHook target slots;
- `Present`/`Present1` behavior;
- resize reset behavior;
- rebind ordering;
- hook-monitor policy;
- resize hold behavior;
- logging levels;
- FSRFG/DLSSG/Streamline behavior.

### 7.7 PR1 acceptance criteria

Build/static:

```text
Release build succeeds
direct-struct-access audit succeeds if still applicable
git diff --check succeeds
no new dependency on Intel private SDK headers
no OptiScaler source dependency is introduced
```

Runtime behavior preservation, targeted:

```text
DD2 + OptiScaler XeFG:
    game launches
    XeFG active
    OptiScaler overlay visible
    REFramework overlay visible
    no recurring destructive rehook loop

Pragmata or other known multi-runtime case:
    launch does not regress
    exact-HMODULE runtime handling remains intact
```

Native REFramework smoke:

```text
one known D3D12 REFramework game without OptiScaler/XeFG
    REFramework overlay opens
    normal scripting/plugin initialization still occurs
```

PR1 is not accepted if architecture extraction requires functional changes to make tests pass.

---

## 8. PR 2 — Extract XeFG Active Binding and Lifecycle State

### 8.1 Goal

Move XeFG-specific active binding identity, strong ownership, generation, replacement policy, and resize-transition state into a dedicated XeFG binding component while preserving the current physical D3D12 callback behavior.

This is the highest-risk refactor PR and should remain behavior-preserving.

### 8.2 State to move out of the main D3D12Hook declaration where practical

Candidate XeFG-owned state includes:

```text
strong bound swapchain ComPtr
strong selected queue ComPtr
strong device ComPtr
binding generation
observe-only/render mode
P2.1 render-boundary diagnostic state
resize event id
last XeFG resize kind
last resize time
post-resize diagnostic budget/ordinal
resize-transition-hold active flag
hold trigger event id
suppressed-present count
```

Diagnostic-only counters can remain until PR4 if moving them would complicate this PR. Semantic binding/lifecycle state should move now.

### 8.3 XeFGBinding responsibility

Recommended conceptual API:

```cpp
class XeFGBinding {
public:
    bool active() const noexcept;
    bool healthy_for_monitor_preservation(const D3D12Hook& hook) const noexcept;

    XeFGBindingChange classify(const XeFGBindingCandidate& candidate) const;

    // Strong ownership accessors used by D3D12Hook only where needed.
    IDXGISwapChain3* swapchain() const noexcept;
    ID3D12CommandQueue* queue() const noexcept;
    ID3D12Device4* device() const noexcept;

    uint64_t generation() const noexcept;
    bool observe_only() const noexcept;

    void arm_resize_hold(...);
    void complete_resize_hold(...);
    void clear_resize_hold(...);
    bool suppress_renderer_during_present() const noexcept;
};
```

This is a design example, not mandatory exact code.

### 8.4 Physical VtableHook ownership decision

Do not move `m_swapchain_hook` automatically.

The lower-risk architecture is likely:

```text
D3D12Hook:
    owns physical instance VtableHook
    owns static hook callback functions

XeFGBinding:
    owns active XeFG COM lifetime and semantic state
    owns generation/mode/transition policy
```

This minimizes changes to upstream callback mechanics.

The replacement operation must still be coordinated transactionally.

### 8.5 Required replacement protocol

For a new swapchain:

```text
1. Validate candidate fully.
2. Acquire strong candidate ownership.
3. Obtain candidate device.
4. Prepare a new VtableHook against the candidate.
5. Verify all required slots can be hooked:
       Present[8]
       Present1[22]
       ResizeBuffers[13]
       ResizeTarget[14]
       ResizeBuffers1[39]
6. If any preparation step fails:
       destroy only the uncommitted new hook/candidate state
       keep the old active binding untouched
       return failure
7. Reset the old REFramework renderer.
8. Remove the old active VtableHook while old XeFG strong ownership still exists.
9. Commit new XeFG strong ownership.
10. Commit new physical hook.
11. Update D3D12Hook alias pointers/getters if still required by upstream renderer code.
12. Clear stale resize hold state.
13. Increment generation.
```

For same swapchain but changed queue/mode:

```text
1. Validate new queue/device/mode.
2. Reset REFramework renderer once.
3. Keep existing instance VtableHook.
4. Replace strong queue/device ownership.
5. Update render/observe mode.
6. Clear stale transition state.
7. Increment generation.
```

### 8.6 D3D12Hook pointer policy

Upstream REFramework currently expects raw pointers such as:

```text
m_swap_chain
m_command_queue
m_device
```

Do not broadly rewrite upstream renderer code to consume a new binding object.

During this refactor it is acceptable for those raw pointers to remain as aliases to strongly owned XeFG objects while XeFG is active.

Required invariant:

```text
when source == XeFGInternal:
    m_swap_chain == XeFGBinding strong swapchain .Get()
    m_command_queue == XeFGBinding strong queue .Get()
    m_device == XeFGBinding strong device .Get()
```

The strong owner is the compatibility binding; raw fields remain interoperability aliases for existing REFramework code.

### 8.7 Present policy after PR2

`present_common()` should ask the XeFG binding/lifecycle object for semantic decisions rather than directly combining multiple XeFG fields.

Keep the physical flow:

```text
validate tracked instance
record real Present activity
update basic D3D12Hook state
handle recursion
ask XeFG binding whether renderer callbacks are suppressed
run REFramework renderer callback when allowed
call original Present/Present1
preserve monitor liveness from real Present during suppression
run post-present callback when allowed
```

Do not rewrite normal Present behavior for native D3D12.

### 8.8 Resize lifecycle after PR2

The XeFG binding object should own the transition state, but D3D12Hook continues to own the physical callbacks.

For tracked XeFG `ResizeTarget`:

```text
D3D12Hook callback
    -> invoke normal REFramework reset callback
    -> notify XeFGBinding that reset completed and hold may arm
    -> call original ResizeTarget
    -> notify result
```

For tracked XeFG `ResizeBuffers` / `ResizeBuffers1`:

```text
D3D12Hook callback
    -> perform current pre-reset behavior
    -> call original resize
    -> notify XeFGBinding of completion result
```

The compatibility object must not independently call DXGI resize methods.

### 8.9 PR2 forbidden changes

Do not:

- change the rendering target to the public XeFG proxy;
- change the selected queue policy;
- replace VtableHook with a new hooking library;
- hook additional OptiScaler private functions;
- hook `Release` only to simplify lifetime tracking;
- add timeouts/sleeps to resize hold;
- redesign generic D3D12 renderer initialization;
- move all normal D3D12 fields into a new generic binding abstraction;
- change FSRFG/DLSSG behavior;
- clean logging levels yet.

### 8.10 PR2 acceptance criteria

Static/build:

```text
Release build succeeds
audit succeeds
git diff --check succeeds
no direct loss of strong COM ownership ordering
```

Target runtime matrix:

```text
DD2:
    initial XeFG bind works
    both overlays visible
    repeated Alt+Tab works
    resolution/window changes do not lose REFramework overlay permanently

MHW:
    Alt+Enter path exercises ResizeTarget transition hold
    Present continues during hold
    renderer resumes only after valid resize completion
    no recurring hook-monitor destruction

Known re-init/rebind case:
    binding generation changes only on accepted replacement/update
    failed candidate does not destroy old binding
```

Native smoke:

```text
one normal D3D12 REFramework run without XeFG
    overlay and normal integration unchanged
```

---

## 9. PR 3 — Minimize Upstream Touch Points and Finalize Compatibility Boundaries

### 9.1 Goal

After runtime/discovery and binding/lifecycle state have been extracted, reduce direct XeFG knowledge in upstream-sensitive REFramework files to a small set of intentional bridge points.

PR3 is where the refactor becomes valuable for long-term upstream maintenance.

### 9.2 `REFramework.cpp` target state

The core framework should retain only these XeFG-related integration roles:

#### Loader observation bridge

The existing loader notification and `LdrLoadDll` hook remain useful upstream-level mechanisms.

But XeFG-specific runtime policy should become conceptually:

```cpp
if (loaded module is libxess_fg.dll) {
    XeFGCompatibility::instance().on_module_loaded(...);
}
```

The exact filename check may remain in a tiny helper near the loader bridge or move into the compatibility façade if that produces a cleaner diff.

Core must not own:

- runtime slot arrays;
- XeFG export hooks;
- XeFG transaction state;
- queue validation policy.

#### Hook-monitor bridge

Current explicit XeFG health check should become a narrow semantic query.

Preferred form is conceptually either:

```cpp
if (d3d12->should_preserve_binding_on_monitor_timeout())
```

or:

```cpp
if (XeFGCompatibility::instance().should_preserve_active_binding(*d3d12))
```

Choose the version that produces the smallest and clearest upstream conflict surface.

The rest of the upstream monitor code stays unchanged.

### 9.3 `D3D12Hook.hpp` target state

Reduce public/protected XeFG-specific declarations.

Prefer one or a few bridge members rather than exposing the whole runtime system.

Possible target shape:

```cpp
class D3D12Hook {
public:
    bool should_preserve_binding_on_monitor_timeout() const noexcept;

private:
    std::unique_ptr<XeFGBinding> m_xefg_binding;
};
```

Exact ownership can differ, but avoid keeping runtime-registry APIs as D3D12Hook static methods after PR1.

### 9.4 `D3D12Hook.cpp` target state

XeFG-specific code remaining in this file should be limited to places where it directly interacts with the physical D3D12 hook callback.

Examples of acceptable remaining integration:

```text
consume validated pending candidate at hook initialization boundary
initial/replacement physical VtableHook preparation
small Present policy query
small resize lifecycle notification
small unhook notification
```

Large runtime registry or discovery algorithms should not remain.

### 9.5 No requirement for zero core diffs

Do not contort the code to remove every XeFG line from upstream files.

A few explicit, understandable bridge calls are preferable to:

- global callbacks;
- opaque function-pointer registries;
- hidden singleton side effects;
- generic event buses;
- dynamic plugin-style architecture inside REFramework.

The architecture should be easy for a maintainer to understand during an upstream rebase.

### 9.6 Upstream merge strategy after PR3

The intended future update process becomes:

```text
1. Fetch/merge or rebase against REFramework upstream.
2. Resolve upstream changes in normal REFramework files first.
3. Re-apply/check the small XeFG bridge points.
4. Most fork-specific implementation remains untouched under src/compatibility/xefg/.
5. Build.
6. Run native REFramework smoke.
7. Run OptiScaler XeFG target matrix.
```

If an upstream change rewrites D3D12 Present/resize mechanics, only the physical bridge points and their assumptions should need review; runtime registry/discovery logic should normally remain unchanged.

### 9.7 OptiScaler update strategy after PR3

When OptiScaler changes:

First verify observable contracts rather than patching to internal implementation names.

Check in this order:

```text
1. Is libxess_fg.dll still loaded?
2. Is InitFromSwapChainDesc still used?
3. Does the init transaction still expose factory + init queue?
4. Is the actual internal presentation swapchain still created through observable DXGI factory activity?
5. Is the presentation queue relation unchanged?
6. Are Present/Present1 and resize entry points still standard DXGI methods on the captured object?
7. Did only hook ordering/ownership change?
```

Only if these observable boundaries disappear should a new discovery mechanism be investigated.

Do not preemptively couple to OptiScaler private wrapper classes.

### 9.8 PR3 final regression requirement

Because PR3 touches the highest-level integration boundaries, final validation must include more than only XeFG.

Minimum:

```text
Native D3D11 REFramework smoke
Native D3D12 REFramework smoke
REFramework overlay open/close and input
basic Lua/plugin/mod initialization smoke
OptiScaler + XeFG DD2
OptiScaler + XeFG MHW transition path
known multi-runtime launch case
```

This is not an expansion of the product scope to D3D11/other FG. It is a regression check because REFramework functionality preservation is the highest-level requirement.

FSRFG/DLSSG do not require new architecture or dedicated acceptance work unless PR3 unexpectedly changes a shared path they rely on.

---

## 10. PR 4 — Logging Cleanup and Debug Logging Toggle (Deferred Detail)

PR4 is intentionally documented only at a high level now.

The exact logging cleanup should be designed **after PR1-PR3 are merged and the final code layout is known**.

Required product behavior:

```text
REFramework overlay
    -> Configuration
    -> Debug Logging checkbox
    -> default OFF
    -> persisted through existing REFramework config system
```

Default/OFF logging should still retain enough information for normal user support, such as:

- XeFG compatibility detected/activated;
- binding accepted/replaced;
- concise selected presentation path;
- meaningful failure HRESULT/reason;
- hook installation failure;
- compatibility fallback/failure;
- fatal renderer/device errors.

Verbose development diagnostics should move behind Debug Logging, including detailed address/vtable/COM identity/backbuffer/queue snapshots and repetitive lifecycle traces.

Do not remove diagnostic capability; gate it.

Do not redesign REFramework's entire logging subsystem merely to implement this toggle.

---

## 11. Explicit Non-Goals

The following are not part of PR1-PR4 unless a concrete regression directly caused by the refactor requires a minimal fix:

```text
General REFramework modernization
D3D11 refactor
Generic D3D12 architecture rewrite
FSRFG compatibility redesign
DLSSG compatibility redesign
Streamline redesign
Generic frame-generation provider interface
Special K compatibility work
REFramework renderer rewrite
ImGui backend rewrite
message-hook redesign
input-hook redesign
Lua/plugin/mod architecture refactor
VR architecture refactor
new GPU vendor abstraction
OptiScaler source changes
Intel XeFG private object reverse engineering
performance optimization unrelated to XeFG compatibility
style-only cleanup
large naming cleanup
warning cleanup unrelated to touched code
```

---

## 12. Review Rules for This Refactor

Each PR review should answer these questions in order.

### 12.1 Does it preserve REFramework behavior?

Any realistic regression in normal REFramework behavior is blocking.

### 12.2 Does it preserve the proven XeFG compatibility contracts?

Blocking examples:

- loss of `Present1` coverage;
- loss of `ResizeBuffers1` reset;
- use of init queue instead of validated presentation queue;
- weak COM lifetime before hook removal;
- destroy-old-before-prepare-new replacement;
- hook-monitor destroying a healthy XeFG binding;
- resize hold suppressing original Present;
- rendering on the public interpolation proxy.

### 12.3 Does it reduce or at least not increase upstream coupling?

A refactor PR should not move XeFG code from one upstream hotspot to another.

### 12.4 Does it introduce speculative generic complexity?

Generic FG frameworks, provider registries, state machines, retries, or synchronization added without a demonstrated need are non-goals and should normally be rejected.

### 12.5 Is the failure path conservative?

A compatibility failure should degrade REFramework overlay compatibility, not destabilize the game or OptiScaler.

---

## 13. Suggested Final Source Ownership Map

Target ownership after PR3:

| Concern | Owner |
|---|---|
| Native D3D12 phase-1 discovery | `D3D12Hook` |
| Native D3D12 command queue discovery | `D3D12Hook` |
| Present/Present1 physical callbacks | `D3D12Hook` |
| Resize physical callbacks | `D3D12Hook` |
| REFramework renderer callbacks | `D3D12Hook` / `REFramework` existing path |
| Detect exact XeFG runtime module | `XeFGCompatibility` / `XeFGRuntimeRegistry` |
| XeFG export hooks | `XeFGRuntimeRegistry` |
| Exact-HMODULE dispatch | `XeFGRuntimeRegistry` |
| XeFG init transaction | `XeFGDiscovery` |
| Temporary factory hook | `XeFGDiscovery` |
| Internal swapchain capture | `XeFGDiscovery` |
| Presentation queue capture/validation | `XeFGDiscovery` |
| Candidate publication | `XeFGCompatibility` |
| Strong active XeFG COM ownership | `XeFGBinding` |
| XeFG binding identity/generation | `XeFGBinding` |
| XeFG replacement policy | `XeFGBinding` + narrow D3D12Hook physical-hook bridge |
| XeFG resize hold semantic state | `XeFGBinding` |
| Generic hook-monitor timing | `REFramework` existing path |
| "preserve healthy XeFG binding" decision | narrow `XeFGCompatibility`/`D3D12Hook` query |
| Debug logging policy | PR4, existing config UI + XeFG log gate |

---

## 14. Expected Result

After PR1-PR3, the fork should have the following properties:

```text
REFramework remains REFramework.

Normal REFramework paths remain upstream-like.

OptiScaler XeFG support exists as a narrow compatibility island.

D3D12Hook still owns D3D12 physical hook mechanics,
but no longer owns the entire XeFG runtime implementation.

REFramework.cpp knows only small XeFG bridge points,
not XeFG runtime policy.

Upstream updates mostly conflict at those small bridge points.

OptiScaler updates are handled by re-validating observable XeFG/DXGI contracts,
not by chasing private wrapper layouts.

The proven Special-K-free XeFG presentation path remains unchanged.
```

The key architectural principle is:

> **Isolate the compatibility implementation, not REFramework itself.**

And the key scope rule is:

> **If a change does not directly improve OptiScaler XeFG compatibility, preserve REFramework behavior, or reduce future upstream/OptiScaler maintenance cost, it does not belong in this refactor.**
