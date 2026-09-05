# Work Order P2 — XeFG Direct Presentation Binding + Present1

Date: 2026-09-05  
Repository: `onehoon/REFramework`  
Target branch: `master`  
Baseline REFramework master commit: `fac740d5c94a271cb5a86cb3b80489bc03f2d774`  
Observed OptiScaler `release/0.9` HEAD: `8dac650cbf90c85ca1d46747a66fc98118be75b8` (`0.9.5-pre4`)  
Parent analysis: `doc/REFramework_OptiScaler_XeFG_SpecialK_Removal_Analysis_Plan_2026-09-05.md`  
Parent work orders:

- `doc/work-order/XEFG_SPECIALK_REMOVAL_P1_DIAGNOSTIC_INSTRUMENTATION.md`
- `doc/work-order/XEFG_SPECIALK_REMOVAL_P1_1_EVIDENCE_COMPLETION.md`

---

## 1. Purpose

P1 and P1.1 have established the failure mechanism strongly enough to begin the first functional XeFG compatibility implementation.

The target topology is intentionally narrow:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (this fork)
Special K    = absent
FG Output    = XeFG
Renderer     = D3D12
```

P2 must make REFramework bind directly to the presentation swapchain created inside the XeFG initialization path instead of waiting for the generic native-DXGI phase-1 `Present[8]` discovery path.

P2 must also add first-class `Present1[22]` handling because the active XeFG path is confirmed to use `Present1` while the existing REFramework `Present[8]` callback remains completely starved.

The primary P2 outcome is:

```text
XeFG active
  + OptiScaler overlay active
  + REFramework overlay active
  + REFramework scripts / re.on_frame live
  + no recurring hook-monitor rehook loop
  + no Special K
```

This is the first functional Special-K-removal PR.

---

## 2. Scope Override — XeFG Only

This work order supersedes the remaining control-case language in P1/P1.1.

Do **not** gate P2 on:

- native/no-FG behavior,
- FSRFG behavior,
- DLSS-G behavior,
- preserving FSRFG compatibility,
- adding generic frame-generation abstractions that are not required for XeFG.

The project acceptance target for this phase is only:

```text
REFramework + OptiScaler + XeFG, without Special K
```

Existing non-XeFG code should not be intentionally broken, but no additional validation or implementation effort is required for those paths in P2.

---

## 3. P1/P1.1 Evidence That P2 Must Treat as Established

### 3.1 REFramework never reaches the existing Present callback

Intel DD2 and Intel MHW P1.1 both report the same hook-monitor state:

```text
is_hooked              = true
is_phase_1             = true
inside_present         = false
active_swapchain       = 0x0
active_device          = 0x0
active_command_queue   = 0x0
present_entry_count    = 0
xefg_module_loaded     = true
last_present_entry_age_ms = -1
```

This means:

```text
phase-1 native Present[8]
    -> zero entries
    -> zero phase transition
    -> zero renderer initialization
```

The repeated hook-monitor recovery is a consequence of that starvation, not the root cause.

### 3.2 The active XeFG path is healthy

In both Intel games, OptiScaler continuously presents successfully through the XeFG path:

```text
FGHooks::FGPresent
XeFG_Dx12::Present
FGHooks::hkFGPresent1
LocalPresent
MenuOverlayDx::Present
RenderImGui_DX12
LocalPresent Calling original present
LocalPresent Original present result: 0
```

Therefore P2 must attach REFramework to the active XeFG presentation path rather than trying to repair the generic native `Present[8]` discovery loop.

### 3.3 `libxess_fg.dll` and `igxess_fg.dll` have different roles

For the tested Intel topology:

```text
libxess_fg.dll
    = XeFG public runtime/API DLL used by OptiScaler

igxess_fg.dll
    = Intel driver-side XeFG implementation DLL
```

The public integration point is `libxess_fg.dll`.

Do not make behavior depend on:

```text
Present owner == libxess_fg.dll
```

or:

```text
Present owner == igxess_fg.dll
```

Runtime vtable ownership is diagnostic information only.

### 3.4 The public XeFG proxy is not the desired final render target

P1/P1.1 later observed:

```text
type_name = struct XefgInterpolationSwapChain
```

with Intel-driver-owned `Present`, `Present1`, `ResizeBuffers`, and `ResizeBuffers1` methods.

That object is the public XeFG proxy used by the application.

P2 should **not** bind REFramework rendering directly to this public interpolation proxy unless used temporarily for diagnostics.

### 3.5 OptiScaler already exposes the correct post-FG presentation object during XeFG initialization

The current `release/0.9` OptiScaler implementation calls:

```cpp
xefgSwapChainD3D12InitFromSwapChainDesc(
    context,
    hwnd,
    &swapChainDesc,
    &fullscreenDesc,
    realQueue,
    factory,
    &params);
```

and then:

```cpp
xefgSwapChainD3D12GetSwapChainPtr(...)
```

to obtain the public XeFG proxy.

During `InitFromSwapChainDesc`, XeFG calls the supplied factory's `CreateSwapChainForHwnd`.

The Intel DD2 and MHW OptiScaler P1.1 logs show that this call returns an OptiScaler `WrappedIDXGISwapChain4` object which is then immediately queried by `igxess_fg.dll`.

Observed DD2 sequence:

```text
XeFG_Dx12::CreateSwapchain1
...
DxgiFactoryHooks::CreateSwapChainForHwnd ... SkipWrapping: true
Created new swapchain: <native>
WrappedIDXGISwapChain4 created, real: <native>
Created new WrappedIDXGISwapChain4: <wrapper>
WrappedIDXGISwapChain4::QueryInterface Caller: igxess_fg.dll
```

MHW shows the same pattern.

This factory-returned object is the P2 target because it sits on the post-FG presentation chain before OptiScaler's final `LocalPresent` / overlay / native present path.

Do **not** hardcode the C++ RTTI name `WrappedIDXGISwapChain4` as an identification requirement. Capture it from the public XeFG initialization transaction.

---

## 4. Current REFramework Code Constraints

At baseline `fac740d5...`:

### 4.1 Generic D3D12 discovery

`D3D12Hook::hook()` creates a dummy D3D12 device/factory/queue/swapchain, discovers the command-queue offset, stores:

```cpp
s_swapchain_vtable
s_factory_vtable
s_command_queue_offset
```

and then calls `hook_impl()`.

### 4.2 Phase-1 hook

`hook_impl()` currently installs only:

```cpp
Present[8]
```

through the dummy/native swapchain vtable.

### 4.3 Existing phase transition

The first `D3D12Hook::present()` call removes the global Present hook and installs an instance `VtableHook` with:

```text
Present[8]
ResizeBuffers[13]
ResizeTarget[14]
```

then sets:

```cpp
m_is_phase_1 = false;
```

### 4.4 Current queue selection is incompatible with an explicit XeFG binding

Every normal `present()` currently overwrites `m_command_queue` using the discovered private swapchain offset:

```cpp
m_command_queue =
    *(ID3D12CommandQueue**)((uintptr_t)swap_chain + s_command_queue_offset);
```

For XeFG direct binding this must **not** happen.

The queue passed to `xefgSwapChainD3D12InitFromSwapChainDesc` is the exact queue XeFG uses and must remain authoritative for the XeFG-bound path.

### 4.5 REFramework D3D12 rendering itself is reusable

`REFramework::on_frame_d3d12()` already consumes:

```text
D3D12Hook::get_swap_chain()
D3D12Hook::get_command_queue()
D3D12Hook::get_device()
```

and already performs the backbuffer transition:

```text
PRESENT -> RENDER_TARGET
ImGui draw
RENDER_TARGET -> PRESENT
```

P2 should make the existing renderer receive the correct swapchain and queue. Do not build a separate XeFG renderer.

---

## 5. Required P2 Architecture

The desired high-level flow is:

```text
REFramework startup
    ↓
libxess_fg.dll already loaded or later detected
    ↓
install public XeFG API hooks
    ↓
xefgSwapChainD3D12InitFromSwapChainDesc(...)
    ↓
temporarily instance-hook supplied IDXGIFactory2
    ↓
intercept the CreateSwapChainForHwnd performed inside XeFG init
    ↓
capture returned post-FG presentation swapchain
    ↓
call original XeFG initialization
    ↓
remove temporary factory hook
    ↓
store exact ID3D12CommandQueue* passed to XeFG
    ↓
bind D3D12Hook directly to captured swapchain
    ↓
instance hook Present[8] + Present1[22]
    ↓
existing REFramework D3D12 renderer executes
    ↓
call original Opti/underlying Present or Present1
```

Expected effective order:

```text
XeFG output
   ↓
REFramework internal-swapchain Present/Present1 hook
   ↓
REFramework ImGui / scripts
   ↓
OptiScaler WrappedIDXGISwapChain4 Present/Present1
   ↓
OptiScaler LocalPresent
   ↓
OptiScaler overlay
   ↓
native presentation
```

---

## 6. Public XeFG API Hook Installation

### 6.1 Hook the runtime API module, not the driver implementation

Resolve exports dynamically from:

```text
libxess_fg.dll
```

Required functional hook:

```text
xefgSwapChainD3D12InitFromSwapChainDesc
```

Recommended diagnostic hook in the same PR:

```text
xefgSwapChainD3D12GetSwapChainPtr
```

The latter is useful to record the public proxy identity and prove that the internal presentation target is a different object.

Do not hook `igxess_fg.dll` exports.

### 6.2 Install early enough to catch the initial XeFG initialization

P1.1 showed that the initial internal XeFG swapchain is created **before** the existing `D3D12Hook::hook()` / dummy-swapchain path begins.

In the tested topology, OptiScaler loads `libxess_fg.dll` before REFramework starts its D3D12 hook, leaving enough time for REFramework startup code to install an inline hook on the export before XeFG initialization.

Add an explicit early bootstrap such as:

```cpp
D3D12Hook::install_xefg_api_hooks_if_available();
```

from a normal REFramework initialization context, outside loader lock.

Requirements:

- first check `GetModuleHandleW(L"libxess_fg.dll")`,
- install only once,
- use the existing `FunctionHook` infrastructure,
- keep `LdrRegisterDllNotification` callback lightweight,
- do not create the functional hook directly inside the loader notification callback,
- retain the existing deferred detection path for a later-loaded module and let a safe normal execution point install the hook.

Do not introduce a permanent polling loop.

### 6.3 Use the exact public ABI

The Intel public API prototype for `xefgSwapChainD3D12InitFromSwapChainDesc` is conceptually:

```text
context
HWND
DXGI_SWAP_CHAIN_DESC1*
DXGI_SWAP_CHAIN_FULLSCREEN_DESC*
ID3D12CommandQueue*
IDXGIFactory2*
init params*
```

and `xefgSwapChainD3D12GetSwapChainPtr` is conceptually:

```text
context
REFIID
void**
```

Use the exact ABI from Intel's public `xefg_swapchain_d3d12.h` when declaring the local function types.

Do not guess argument order.

Do not add a static link dependency on `libxess_fg.lib`.

A small local opaque declaration is preferred over importing the entire XeFG SDK solely for two dynamically resolved exports, provided the ABI is verified exactly.

---

## 7. Temporary Factory Capture During `InitFromSwapChainDesc`

### 7.1 Why this hook exists

The supplied `IDXGIFactory2*` is the factory XeFG uses to create the underlying presentation swapchain.

P2 must observe the `CreateSwapChainForHwnd` call that occurs **inside** the public XeFG initialization transaction.

This provides a stable causal relationship:

```text
this swapchain was created by this XeFG context
using this HWND
using this exact command queue
```

No private Intel offsets are needed.

### 7.2 Use a temporary per-instance factory hook

During the `InitFromSwapChainDesc` detour:

1. validate `pDxgiFactory`, `pCmdQueue`, `hWnd`, and descriptor,
2. install a temporary `VtableHook` on the supplied factory instance,
3. hook `CreateSwapChainForHwnd[15]`,
4. call the original XeFG `InitFromSwapChainDesc`,
5. capture the successfully returned swapchain from the factory detour,
6. remove the temporary factory hook before returning to OptiScaler,
7. publish the captured swapchain + exact queue as the XeFG internal binding candidate.

Use RAII / `ScopeGuard` so the temporary hook is always removed.

Do not leave this factory-instance hook installed after XeFG initialization returns.

### 7.3 Preserve the current hook chain

The temporary factory hook must call whatever implementation was already installed in slot 15 before P2 replaced that instance slot.

Do not bypass OptiScaler's factory hook chain.

Do not call the raw system `CreateSwapChainForHwnd` directly.

The returned object observed by P1.1 is expected to be the OptiScaler presentation wrapper, and bypassing the existing chain would defeat the purpose of P2.

### 7.4 Candidate validation

Accept a candidate only when:

- original `CreateSwapChainForHwnd` returned success,
- output pointer is non-null,
- candidate supports at least `IDXGISwapChain3`,
- HWND matches the active XeFG init transaction,
- `GetDevice` succeeds with a D3D12 device,
- the supplied command queue is non-null.

Where practical, compare the queue device and swapchain device COM identity for diagnostics.

Do not require:

- a specific RTTI name,
- a specific module owner,
- a specific raw vtable address,
- Intel private object layout.

### 7.5 No ownership abuse

Do not retain extra COM references to the captured swapchain/backbuffers as a lifetime strategy.

A transient `QueryInterface`/`ComPtr` for validation is fine, but P2 must not permanently `AddRef` the captured presentation swapchain merely to keep it alive.

The actual XeFG/Opti chain owns the object.

This is especially important because the separate NVIDIA/MHW investigation already demonstrates that swapchain/backbuffer lifetime assumptions can be fragile.

---

## 8. Track Public Proxy Identity Separately

If `xefgSwapChainD3D12GetSwapChainPtr` is hooked, record:

```text
XeFG context
public proxy pointer
public proxy vtable
Present owner
Present1 owner
```

Log whether:

```text
public_proxy == internal_presentation_candidate
```

The expected Intel result is `false`.

Use this only for diagnostics and context correlation.

**Do not bind REFramework rendering to the public `XefgInterpolationSwapChain` in P2.**

---

## 9. Add an Explicit XeFG External Binding Path

Add an explicit method similar to:

```cpp
bool D3D12Hook::bind_external_swapchain(
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* command_queue,
    SwapchainSource source);
```

A minimal source enum is sufficient for P2:

```cpp
enum class SwapchainSource {
    Native,
    XeFGInternal,
};
```

Do not expand this enum into FSRFG/Streamline work unless required by compilation.

### 9.1 Binding responsibilities

For `XeFGInternal`, `bind_external_swapchain()` must:

1. validate swapchain and queue,
2. obtain/validate the D3D12 device from the swapchain,
3. remove the obsolete phase-1 `m_present_hook`,
4. remove any previous instance `m_swapchain_hook`,
5. set the active swapchain,
6. set the exact XeFG command queue,
7. set the active D3D12 device,
8. mark source = `XeFGInternal`,
9. install the instance vtable hooks required by P2,
10. set `m_is_phase_1 = false`,
11. emit a structured bind log,
12. leave the object ready for the existing `m_on_present` renderer callbacks.

### 9.2 Required instance hooks in P2

Install:

```text
Present[8]
Present1[22]
ResizeBuffers[13]
ResizeTarget[14]
```

`Present1[22]` is new and mandatory.

`ResizeBuffers[13]` and `ResizeTarget[14]` should remain consistent with the existing instance-binding behavior.

`ResizeBuffers1[39]` is **not** part of P2; it belongs to P3 lifecycle hardening.

### 9.3 Idempotence

If the same XeFG internal swapchain + same queue is published more than once, the bind should be a no-op or clean idempotent refresh.

Do not repeatedly tear down/recreate the same instance hook on identical input.

If a **different** internal swapchain identity appears after a successful P2 binding, log it explicitly as a P3 rebind/recreation event rather than inventing a full lifecycle state machine in this PR.

---

## 10. Support Both Timing Orders

P2 must handle both:

### Case A — XeFG internal swapchain is captured before `D3D12Hook` exists

This is the observed Intel DD2/MHW startup order.

Store a non-owning pending binding record containing at least:

```text
XeFG context
internal swapchain pointer
command queue pointer
HWND
swapchain descriptor snapshot
```

When `REFramework::hook_d3d12()` creates/configures the `D3D12Hook` object, consume the pending XeFG binding before relying on the dummy phase-1 path.

Preferred flow:

```text
create D3D12Hook
register callbacks
pending XeFG internal binding exists
    -> bind_external_swapchain(...)
    -> mark D3D12 valid
    -> skip generic dummy phase-1 hook for this activation
```

### Case B — normal D3D12Hook exists first, XeFG initializes later

After the XeFG init detour returns and the candidate is validated:

```text
safe renderer reset if required
    ↓
bind_external_swapchain(...)
```

Do not hold the framework hook-monitor mutex while calling the original XeFG initialization function.

Avoid lock inversion with the existing `create_swapchain` and Streamline paths.

Use a dedicated XeFG-state mutex for capture bookkeeping if needed.

---

## 11. Add First-Class `Present1[22]`

Add an exact `Present1` hook wrapper:

```cpp
static HRESULT WINAPI present1(
    IDXGISwapChain1* swap_chain,
    UINT sync_interval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* params);
```

Use the actual interface type required by the vtable slot and cast only after interface validation.

### 11.1 Do not duplicate the entire Present implementation

Refactor the common instance-present lifecycle so `Present` and `Present1` share:

- active-instance validation,
- present-entry accounting,
- `m_last_present_entry_time`,
- structured diagnostics,
- `m_inside_present`,
- active swapchain update,
- device retrieval,
- command-queue handling,
- recursion protection,
- `m_on_present`,
- original call dispatch,
- `m_on_post_present`,
- result logging.

The wrapper-specific code should only adapt:

```text
Present arguments + original Present call
```

versus:

```text
Present1 arguments + DXGI_PRESENT_PARAMETERS + original Present1 call
```

Do not maintain two diverging copies of the current `present()` logic.

### 11.2 Diagnostics must identify which entry point fired

Extend the P1 structured entry log with a method field, for example:

```text
[D3D12][PresentEntry]
kind = Present1
source = xefg_internal
phase = instance
swapchain = ...
original_owner = ...
```

The first successful Intel run is expected to show `Present1` entries on the XeFG-bound internal presentation swapchain.

---

## 12. XeFG Command Queue Is Authoritative

For `SwapchainSource::XeFGInternal`:

```cpp
m_command_queue = command_queue_from_xefg_init;
```

must remain authoritative.

Do **not** overwrite it from:

```cpp
swap_chain + s_command_queue_offset
```

inside the shared Present path.

Retain the existing offset-based queue discovery only for the legacy/native path.

Recommended shape:

```cpp
if (m_swapchain_source != SwapchainSource::XeFGInternal) {
    // existing command queue offset logic
}
```

Do not scan the XeFG internal wrapper for a queue offset when the exact public-API queue is already available.

---

## 13. Renderer Reset Rules for P2

P2 is not the full resize/recreation phase.

For the initial successful XeFG bind:

- if REFramework D3D12 rendering has not initialized yet, do not perform unnecessary reset work,
- if replacing an already-active generic/native binding, call the existing `REFramework::on_reset()` before discarding old renderer resources,
- then bind the XeFG internal swapchain.

Do not add `ResizeBuffers1` lifecycle support in P2.

Do not add swapchain-destroy/recreate state machines in P2.

Those are P3.

---

## 14. Hook Monitor Behavior

Do not simply disable the hook monitor when XeFG is detected.

The desired outcome is that successful `Present`/`Present1` callbacks naturally keep normal renderer activity alive, so the recurring recovery loop stops because the hook actually works.

After a successful XeFG direct bind, the runtime should no longer show a repeating pattern of:

```text
Last chance encountered for hooking
Sending rehook request for D3D
```

while the game is actively presenting.

If the monitor still fires while `Present1` entries are flowing, investigate the existing timestamp/update path before adding any XeFG-specific suppression.

Do not hide a broken binding by bypassing monitor recovery.

---

## 15. Required Structured Logs

Keep diagnostics concise enough for normal test logs.

### 15.1 API hook install

```text
[XeFG][ApiHook] module = libxess_fg.dll
[XeFG][ApiHook] InitFromSwapChainDesc = ...
[XeFG][ApiHook] GetSwapChainPtr = ...
```

### 15.2 Init transaction

```text
[XeFG][InitDesc]
context = ...
hwnd = ...
queue = ...
factory = ...
width = ...
height = ...
format = ...
buffer_count = ...
flags = ...
```

### 15.3 Internal presentation capture

```text
[XeFG][InternalSwapchain]
context = ...
swapchain = ...
vtable = ...
Present[8] owner = ...
Present1[22] owner = ...
queue = ...
```

### 15.4 Public proxy capture

```text
[XeFG][PublicProxy]
context = ...
swapchain = ...
internal_same = false
```

### 15.5 Direct bind

```text
[D3D12][ExternalBind]
source = xefg_internal
swapchain = ...
queue = ...
device = ...
Present[8].original = ...
Present1[22].original = ...
```

### 15.6 Runtime Present

Existing `[D3D12][PresentEntry]` logging should be extended to identify:

```text
kind = Present | Present1
source = native | xefg_internal
```

Do not log every frame indefinitely. Preserve the existing rate-limited/identity-change behavior.

---

## 16. Files Expected in Scope

Primary files:

```text
src/D3D12Hook.hpp
src/D3D12Hook.cpp
src/REFramework.cpp
```

Only touch `REFramework.hpp` if a small public/private helper declaration is genuinely required.

Avoid adding a new XeFG subsystem file unless doing so materially reduces complexity. The existing P1 XeFG detection and D3D12 hook state already live in `D3D12Hook`.

Do not modify OptiScaler.

Do not vendor the full Intel XeFG SDK for P2.

Do not modify unrelated game-specific code.

---

## 17. Non-Goals — Explicitly Out of P2

P2 must not attempt to solve:

- `ResizeBuffers1[39]` lifecycle handling,
- full resolution-change hardening,
- fullscreen/borderless transition hardening,
- Alt+Tab recreation hardening,
- XeFG context destroy/recreate handling,
- multiple simultaneous XeFG contexts,
- complex swapchain generation/state machines,
- NVIDIA MHW OptiScaler backbuffer `Release()` storm,
- Resident Evil Requiem periodic stutter,
- integrity-check pacing problems,
- native/no-FG control support,
- FSRFG compatibility,
- DLSS-G compatibility work,
- Special K emulation,
- Intel private offsets,
- OptiScaler private interfaces,
- RTTI-name-based functional decisions.

If a different internal swapchain appears at runtime and requires lifecycle work, log it clearly and defer the full rebind behavior to P3 unless the minimal change is required to make the initial P2 validation possible.

---

## 18. Implementation Order

Implement in this order so failures remain diagnosable.

### Step 1 — Early public API hook bootstrap

- resolve `libxess_fg.dll`,
- install `InitFromSwapChainDesc` hook,
- optionally install `GetSwapChainPtr` diagnostic hook,
- emit API-hook logs.

### Step 2 — Temporary factory capture

- instance-hook factory slot 15 only during XeFG initialization,
- capture returned presentation swapchain,
- remove temporary hook,
- log candidate.

### Step 3 — Pending XeFG binding record

- support capture-before-D3D12Hook timing,
- retain only non-owning pointers plus descriptor/context metadata,
- provide safe consume/bind path.

### Step 4 — Direct external bind

- add source metadata,
- bind exact queue,
- install instance Present + Present1 + existing resize hooks,
- skip phase-1 when a valid pending XeFG binding already exists.

### Step 5 — Shared Present / Present1 core

- refactor common logic,
- keep recursion safety,
- preserve existing callback ordering,
- do not overwrite the XeFG queue from private offsets.

### Step 6 — Runtime validation

Do not add P3 lifecycle features until the P2 acceptance test is evaluated.

---

## 19. Required Validation — Intel XeFG Only

### Test A — Dragon's Dogma 2 / Intel

Required topology:

```text
OptiScaler = dxgi.dll
REF fork   = dinput8.dll
Special K  = absent
FGOutput   = XeFG
GPU        = Intel
```

Required observations:

1. `libxess_fg.dll` API hooks install before XeFG initialization.
2. `InitFromSwapChainDesc` is intercepted.
3. the factory-created internal presentation swapchain is captured.
4. the exact XeFG command queue is recorded.
5. public XeFG proxy identity is different from the internal target, if GetSwapChainPtr diagnostics are enabled.
6. `D3D12Hook` binds directly with `source = xefg_internal`.
7. `Present1` and/or `Present` entries occur on the internal bound object.
8. REFramework reaches D3D12 renderer initialization.
9. REFramework Insert/menu is visible.
10. OptiScaler overlay remains visible and functional.
11. a simple REFramework script callback / `re.on_frame` remains live.
12. XeFG remains enabled and visually functional.
13. no Special K DLL is present.
14. the recurring ~11-second D3D12 recovery cycle disappears during active presentation.

DD2 is the mandatory P2 gate.

### Test B — Monster Hunter Wilds / Intel

Use the same topology.

P1.1 shows MHW has more aggressive Streamline/DXGI proxy churn and repeated same-output swapchain reuse.

P2 requirements for MHW:

- REFramework overlay must appear at least on the initial XeFG binding,
- OptiScaler overlay and XeFG must remain functional,
- no pathological OptiScaler backbuffer-release loop may be introduced,
- identical repeated publication of the same internal swapchain must not cause repeated hook teardown/reinstall.

Full survival of a genuinely **new** internal swapchain identity belongs to P3.

If MHW changes internal swapchain identity and P2 loses the overlay only after that event, capture the log and report it as the exact P3 rebind trigger rather than expanding P2 into a full lifecycle rewrite.

---

## 20. Acceptance Criteria

P2 is complete when the DD2 Intel XeFG run demonstrates all of the following:

```text
Special K absent
OptiScaler = dxgi.dll
REFramework = dinput8.dll
XeFG active

public XeFG InitFromSwapChainDesc intercepted
internal presentation swapchain captured
exact XeFG command queue captured
REF direct-binds internal swapchain
Present1 hook receives frames
REF D3D12 renderer initializes
REF overlay visible
Opti overlay visible
REF scripts live
no recurring hook-monitor rehook cycle during active presentation
```

MHW Intel must also receive a smoke test and should at minimum prove that the same initial direct-binding architecture works there.

Do not require native/no-FG or FSRFG tests to merge P2.

---

## 21. Failure Reporting Requirements

If P2 does not reach the overlay, report the first failed boundary explicitly.

Use one of these categories:

```text
A. XeFG API hook installed too late / Init not intercepted
B. Init intercepted but factory CreateSwapChainForHwnd not observed
C. internal swapchain captured but validation failed
D. internal swapchain captured but D3D12Hook did not consume binding
E. direct bind succeeded but Present/Present1 never entered
F. Present1 entered but command queue/device invalid
G. renderer initialized but overlay not visible
H. overlay visible but hook monitor still rehooks
I. new swapchain identity appeared and requires P3 lifecycle work
```

Do not hide a failure by adding broad fallback paths in the same PR.

---

## 22. PR Size / Change Discipline

Prefer one focused P2 PR.

Target approximately **500 functional LOC or less** if practical.

Keep the implementation compact by:

- reusing existing `FunctionHook`, `VtableHook`, `ScopeGuard`, and diagnostic helpers,
- reusing the existing D3D12 renderer,
- using a minimal XeFG state record,
- avoiding a generic FG abstraction layer,
- deferring `ResizeBuffers1` and full recreation to P3.

If implementation pressure clearly pushes P2 far beyond this scope, do not silently broaden the PR. Keep the direct-bind + Present1 core together and defer lifecycle hardening.

---

## 23. Review Checklist

Before marking the PR ready:

- [ ] Based on REFramework master `fac740d5c94a271cb5a86cb3b80489bc03f2d774` or rebased newer master.
- [ ] Uses OptiScaler `release/0.9` behavior as the runtime reference, not OptiScaler master.
- [ ] No Special K dependency or API added.
- [ ] `libxess_fg.dll` is the public API hook point.
- [ ] No functional dependency on `igxess_fg.dll` ownership.
- [ ] Exact XeFG command queue comes from `InitFromSwapChainDesc`.
- [ ] Temporary factory hook is removed after init.
- [ ] Existing Opti/DXGI factory chain is preserved.
- [ ] Internal presentation swapchain, not public XeFG proxy, is the render target.
- [ ] No persistent COM `AddRef` is used as a lifetime workaround.
- [ ] Direct bind skips private queue-offset lookup for XeFG.
- [ ] `Present1[22]` is implemented.
- [ ] Present and Present1 share common lifecycle code.
- [ ] Existing recursion protection is preserved.
- [ ] Existing renderer callback order is preserved.
- [ ] Repeated same-object binding is idempotent.
- [ ] No `ResizeBuffers1` implementation slipped into P2.
- [ ] No native/no-FG or FSRFG acceptance gate added.
- [ ] DD2 Intel runtime test completed.
- [ ] MHW Intel smoke test completed.
- [ ] REF overlay + Opti overlay + XeFG verified together.
- [ ] `re.on_frame`/script activity verified.
- [ ] recurring D3D12 rehook loop absent during active DD2 XeFG presentation.

---

## 24. Expected P2 End-State Before P3

After P2, the architecture should conceptually be:

```text
Game / Streamline input path
        ↓
XeFG public proxy
        ↓
Intel XeFG interpolation
        ↓
Opti/XeFG internal presentation swapchain
        ↓
REFramework instance Present / Present1 hook
        ↓
REFramework renderer + scripts
        ↓
OptiScaler wrapped Present / Present1
        ↓
OptiScaler overlay / LocalPresent
        ↓
native DXGI presentation
```

P3 will then harden this working path for:

```text
ResizeBuffers1
real internal-swapchain identity changes
rebind / recreation
fullscreen / borderless
resolution changes
Alt+Tab
context destruction / recreation
```

Do not start P3 work until P2 proves the basic Special-K-free XeFG presentation binding on Intel DD2.
