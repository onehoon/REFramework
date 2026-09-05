# REFramework × OptiScaler × XeFG — Analysis and Implementation Plan for Removing Special K

Date: 2026-09-05

## 1. Objective

The primary goal is to make the following configuration work reliably **without Special K**:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (forked build)
Special K    = not installed
```

Required success criteria:

- OptiScaler functions correctly.
- Intel XeFG functions correctly.
- OptiScaler overlay is visible and functional.
- REFramework overlay is visible and functional.
- REFramework scripting and normal game integration continue to work.
- The setup survives resolution changes, fullscreen/window-mode transitions, Alt+Tab, and swapchain recreation.
- REFramework does not enter a recurring D3D12 unhook/rehook cycle after XeFG becomes active.
- Only after the Special K dependency is removed successfully should Resident Evil Requiem be used to validate whether the periodic stutter is also resolved.

Important priority:

> The first objective is not to solve Requiem stutter directly.  
> The first objective is to remove the requirement for Special K by making REFramework natively compatible with the XeFG presentation path used by OptiScaler.

---

## 2. Current Known Working and Failing Topologies

Several load topologies involving OptiScaler, REFramework, and Special K are known to work depending on the game and GPU:

```text
OptiScaler : dxgi.dll
REFramework: ReShade64.dll
Special K  : plugins/dxgi.dll
```

```text
OptiScaler : dxgi.dll
REFramework: dinput8.dll
Special K  : plugins/dxgi.dll
```

```text
OptiScaler : d3d12.dll
REFramework: ReShade64.dll
Special K  : dxgi.dll
```

```text
Special K  : dxgi.dll
OptiScaler : loaded by Special K
REFramework: ReShade64.dll loaded automatically by Special K
```

The exact working topology varies by game and GPU. Some combinations work, some fail to load, and some crash.

A key observation is that the filename used to load OptiScaler is not the fundamental cause. The failure appears whenever OptiScaler + REFramework are used with XeFG without the stabilizing Special K DXGI layer.

The preferred target configuration remains the simplest and most common one:

```text
OptiScaler  = dxgi.dll
REFramework = dinput8.dll
Special K   = absent
```

---

## 3. Confirmed Failure Mechanism Without Special K

In the DD2 no-SK test:

- REFramework loads normally.
- REFramework game hooks remain active.
- OptiScaler initializes XeFG successfully.
- OptiScaler's own overlay continues rendering.
- XeFG continues presenting frames successfully.
- REFramework never reaches its D3D12 renderer initialization path.
- REFramework repeatedly enters its hook-monitor recovery path.

The critical REFramework log pattern is:

```text
Hooked DirectX 12
...
Last chance encountered for hooking
Sending rehook request for D3D
Unhooking D3D12
Hooking D3D12
Reinitializing D3D12Hook via known pointers
...
```

At the same time, OptiScaler continues presenting frames successfully:

```text
FGHooks::hkFGPresent
XeFG_Dx12::Present
LocalPresent
MenuOverlayDx::Present
RenderImGui_DX12
LocalPresent Calling original present
Original present result: 0
```

This proves that the presentation path itself is alive while REFramework's D3D12 Present hook is starved.

---

## 4. Why REFramework Fails

REFramework currently discovers the real D3D12 swapchain in two phases.

### Phase 1

REFramework creates a dummy D3D12 swapchain and stores its vtable:

```cpp
s_swapchain_vtable = *(void***)target_swapchain;
```

It then globally hooks the dummy/native `Present` vtable entry:

```cpp
auto& present_fn = s_swapchain_vtable[8]; // Present

m_present_hook =
    std::make_unique<PointerHook>(
        &present_fn,
        &D3D12Hook::present);
```

The assumption is:

> The first real game Present will pass through the same native DXGI Present vtable entry.

### Phase 2

Once the first real Present reaches `D3D12Hook::present`, REFramework transitions to a per-instance swapchain hook:

```cpp
m_swapchain_hook =
    std::make_unique<VtableHook>(swap_chain);

m_swapchain_hook->hook_method(8,  (uintptr_t)&D3D12Hook::present);
m_swapchain_hook->hook_method(13, (uintptr_t)&D3D12Hook::resize_buffers);
m_swapchain_hook->hook_method(14, (uintptr_t)&D3D12Hook::resize_target);

m_is_phase_1 = false;
```

The failing XeFG topology never reaches this transition.

---

## 5. XeFG Breaks the Phase-1 Assumption

Intel XeFG uses a proxy swapchain.

OptiScaler initializes XeFG using:

```cpp
xefgSwapChainD3D12InitFromSwapChainDesc(...)
```

and retrieves the Intel proxy with:

```cpp
xefgSwapChainD3D12GetSwapChainPtr(...)
```

After initialization, the application is expected to use the XeFG proxy swapchain rather than the original native swapchain.

Therefore the real presentation path becomes conceptually:

```text
Game
  ↓
OptiScaler / XeFG proxy
  ↓
XeFG Present / Present1
  ↓
OptiScaler FG hooks
  ↓
OptiScaler LocalPresent
  ↓
underlying presentation swapchain
  ↓
screen
```

REFramework is still waiting on the dummy/native `Present` vtable path:

```text
REFramework dummy native swapchain
        ↓
vtable[8] Present hook
        X
        X actual XeFG presentation no longer reaches this entry
        X
```

This is why REFramework:

- never sets `m_inside_present`,
- never captures the actual game swapchain,
- never initializes D3D12 render targets,
- never initializes ImGui,
- never displays its overlay,
- eventually assumes hooking failed and starts its periodic recovery loop.

---

## 6. Present1 Is a First-Class Part of the Problem

Current REFramework D3D12 hooking treats `IDXGISwapChain::Present` as the primary presentation entry.

However, OptiScaler's current swapchain wrapper has explicit `Present1` handling:

```cpp
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present1(
    UINT SyncInterval,
    UINT Flags,
    const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    ...
    result = LocalPresent(
        _real1,
        SyncInterval,
        Flags,
        pPresentParameters,
        _device,
        _handle,
        _uwp);
}
```

The no-SK OptiScaler logs also show the XeFG path actively going through `hkFGPresent1`.

Therefore a correct REFramework XeFG implementation should not only add XeFG detection. It should make the newer swapchain entry points first-class hook targets.

At minimum:

| Vtable slot | Method | Current REF | XeFG fork target |
|---:|---|---|---|
| 8 | `Present` | Yes | Yes |
| 13 | `ResizeBuffers` | Yes | Yes |
| 14 | `ResizeTarget` | Yes | Yes |
| 22 | `Present1` | No | Yes |
| 39 | `ResizeBuffers1` | No | Yes |

The exact interface version and vtable indexing must be verified against the interface actually captured at runtime before final implementation.

---

## 7. What Special K Is Actually Providing

Special K is not fundamentally fixing XeFG itself.

The most consistent interpretation of the working topology is:

> Special K provides a stable DXGI/swapchain mediation layer that keeps the presentation path observable to REFramework even while XeFG is using or recreating its own proxy/internal swapchains.

This explains why multiple Special K load topologies can work even when REFramework is still loaded as its normal `dinput8.dll`.

Therefore:

- `ReShade64.dll` is not the actual technical fix.
- Special K loading REFramework as a ReShade-style plugin is only one workable load topology.
- OptiScaler being named `dxgi.dll` or `d3d12.dll` is not the fundamental issue.
- The relevant invariant is that Special K stabilizes or mediates the DXGI/swapchain chain.

The fork should therefore reproduce the required **swapchain awareness**, not reproduce Special K itself.

---

## 8. Recommended REFramework-Side Architecture

The cleanest long-term fix is to add explicit XeFG awareness to REFramework.

### 8.1 Treat XeFG as a supported frame-generation swapchain family

REFramework already contains special handling for frame-generation technologies such as Streamline/DLSS-G and FSR3-related swapchains.

XeFG should be handled similarly instead of relying on the generic phase-1 native Present discovery path.

A possible internal classification:

```cpp
enum class SwapchainSource {
    Native,
    Streamline,
    FSR3,
    XeFGInternal
};
```

This is preferable to scattering XeFG-specific conditionals across existing logic.

---

## 9. Do Not Rely on Private Intel Object Layouts

Avoid reverse-engineering a private offset inside the XeFG proxy object to locate its internal/native swapchain.

That approach would be fragile across:

- Intel XeFG versions,
- driver versions,
- OptiScaler updates,
- games,
- GPU generations.

Instead, use the public XeFG initialization path and observable DXGI creation path.

---

## 10. Recommended XeFG Discovery Strategy

The most promising design is to monitor the Intel XeFG initialization path.

Relevant API:

```cpp
xefgSwapChainD3D12InitFromSwapChainDesc(
    context,
    hwnd,
    desc,
    fullscreenDesc,
    ID3D12CommandQueue* queue,
    IDXGIFactory2* factory,
    params);
```

This call gives REFramework several valuable pieces of information directly:

- target HWND,
- swapchain descriptor,
- real D3D12 command queue,
- factory used by XeFG.

OptiScaler itself passes the real queue and factory into this call.

A recommended sequence is:

```text
libxess_fg.dll detected
        ↓
hook xefgSwapChainD3D12InitFromSwapChainDesc
        ↓
capture:
    HWND
    ID3D12CommandQueue*
    IDXGIFactory2*
        ↓
during XeFG initialization:
    observe CreateSwapChainForHwnd
        ↓
capture the presentation swapchain created by XeFG
        ↓
bind REFramework directly to that swapchain
```

This avoids guessing private Intel object offsets.

---

## 11. Prefer the Post-FG Presentation Swapchain Over the Public XeFG Proxy

A simple proof-of-concept could directly bind REFramework to the XeFG public proxy returned by:

```cpp
xefgSwapChainD3D12GetSwapChainPtr(...)
```

That may be useful for diagnostics because it should quickly prove that the current phase-1 discovery is the problem.

However, it may not be the ideal final rendering target.

REFramework already contains a relevant warning in its Streamline handling: rendering the menu on a frame-generation proxy swapchain can cause flicker or incorrect menu rendering.

The safer final target is the **post-frame-generation presentation swapchain** that ultimately owns the real output backbuffers.

The desired render order is:

```text
XeFG generates/interpolates frame
        ↓
REFramework renders overlay
        ↓
OptiScaler renders its own overlay
        ↓
native Present / Present1
```

This keeps both overlays outside the frame-generation interpolation input where possible.

---

## 12. Why the OptiScaler Wrapper Is a Useful Final Target

OptiScaler's wrapped swapchain is already positioned immediately before the underlying native presentation call.

Its `Present1` path is effectively:

```text
WrappedIDXGISwapChain4::Present1
        ↓
LocalPresent
        ↓
OptiScaler MenuOverlayDx::Present
        ↓
underlying Present1
```

If REFramework captures and instance-hooks that wrapper, the effective order can become:

```text
XeFG output
    ↓
REFramework Present/Present1 hook
    ↓
REFramework ImGui
    ↓
OptiScaler wrapped Present/Present1
    ↓
OptiScaler overlay
    ↓
native DXGI
```

This is desirable because:

- both overlays stay visible,
- REFramework does not need to know OptiScaler internals,
- the hook can still work when OptiScaler changes its proxy filename,
- the binding is to the actual active presentation object.

---

## 13. Command Queue Handling Should Be Improved for XeFG

Current REFramework D3D12 logic can discover the command queue by scanning a swapchain object and reusing a private object offset such as the observed `0x140`.

For XeFG support, this should not be necessary.

`xefgSwapChainD3D12InitFromSwapChainDesc` already receives the real `ID3D12CommandQueue*`.

For an XeFG-specific bind path, REFramework should use that explicit pointer:

```cpp
m_command_queue = queue_from_xefg_init;
```

Advantages:

- no private swapchain layout dependency,
- no repeated offset probing,
- more reliable across different wrapper types,
- easier to diagnose.

The existing native/legacy command-queue discovery path should remain untouched for non-XeFG cases.

---

## 14. Add an Explicit External Swapchain Binding Path

Rather than forcing XeFG through the existing phase-1 dummy Present mechanism, introduce a direct binding method.

Conceptually:

```cpp
bool D3D12Hook::bind_external_swapchain(
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* queue,
    SwapchainSource source);
```

Responsibilities:

1. Validate the swapchain.
2. Validate/get its D3D12 device.
3. Release the obsolete phase-1 Present hook.
4. Remove the previous instance vtable hook if necessary.
5. Set:
   - `m_swap_chain`
   - `m_command_queue`
   - `m_device`
   - source metadata
6. Install instance-level hooks on the active swapchain.
7. Set `m_is_phase_1 = false`.
8. Reset hook-monitor timing state.
9. Trigger safe renderer reinitialization when needed.

This keeps XeFG support explicit and minimizes impact on the existing native path.

---

## 15. Add Present1 Support Using a Shared Present Core

Add a proper `Present1` callback:

```cpp
static HRESULT WINAPI present1(
    IDXGISwapChain1* swap_chain,
    UINT sync_interval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* params);
```

Avoid duplicating all existing Present logic.

Refactor the common lifecycle into a shared internal function that handles:

- instance validation,
- `m_inside_present`,
- current swapchain update,
- device retrieval,
- command queue selection,
- `m_on_present`,
- REFramework D3D12 rendering,
- original Present/Present1 dispatch,
- `m_on_post_present`,
- recursion protection.

The public hook wrappers should mainly adapt argument differences.

---

## 16. ResizeBuffers1 Support Is Required

Supporting only `Present1` may restore the overlay initially but still leave the setup fragile after:

- resolution change,
- window/fullscreen transition,
- swapchain recreation,
- Alt+Tab-related resize,
- game-side graphics setting changes.

Current REFramework already resets its D3D12 renderer before the original `ResizeBuffers` call by invoking its registered resize callback.

That same lifecycle should be implemented for `ResizeBuffers1`.

Conceptual flow:

```text
ResizeBuffers1 entered
        ↓
REFramework on_reset()
        ↓
release REF RTV/backbuffer references
        ↓
call original ResizeBuffers1
        ↓
next Present / Present1
        ↓
recreate render targets
        ↓
REFramework initialized again
```

This is especially important because OptiScaler and XeFG also manage backbuffer ownership during resize.

---

## 17. Observe Both the Public XeFG Proxy and the Final Presentation Swapchain

A robust final design may use two related objects for different purposes.

### Public XeFG proxy

Use for lifecycle awareness:

- XeFG enabled/disabled state,
- proxy replacement,
- `ResizeBuffers`,
- `ResizeBuffers1`,
- swapchain recreation boundaries.

### Final presentation swapchain

Use for actual REFramework rendering:

- `Present`,
- `Present1`,
- current backbuffer index,
- render-target creation.

This separation is likely safer than forcing all behavior onto one object.

---

## 18. Logging Must Be Added Before Functional Changes

The first fork PR should primarily improve observability.

The current analysis is strong, but the next implementation should turn the remaining assumptions into runtime facts.

### 18.1 Module load logging

When these modules load:

```text
libxess_fg.dll
libxess.dll
dxgi.dll
d3d12.dll
```

log:

- module base,
- full path,
- load time,
- whether XeFG hooks are armed.

### 18.2 Phase-1 target logging

When the dummy swapchain is created, log:

```text
[D3D12][Discovery]
dummy swapchain          = 0x...
dummy vtable             = 0x...
Present slot[8]          = 0x...
Present owner module     = ...
Present1 slot[22]        = 0x...
Present1 owner module    = ...
```

Determine function ownership with a safe address-to-module helper.

### 18.3 Candidate swapchain logging

For relevant swapchain creation paths:

```text
[D3D12][SwapchainCandidate]
swapchain                = 0x...
factory                  = 0x...
device/queue             = 0x...
hwnd                     = 0x...
width/height             = ...
format                   = ...
buffer count             = ...
vtable                   = 0x...
Present[8]               = 0x...
Present owner            = ...
Present1[22]             = 0x...
Present1 owner           = ...
ResizeBuffers[13]        = 0x...
ResizeBuffers1[39]       = 0x...
```

Also classify whether the candidate was created before, during, or after XeFG init.

### 18.4 XeFG API logging

Observe/hook:

```text
xefgSwapChainD3D12InitFromSwapChain
xefgSwapChainD3D12InitFromSwapChainDesc
xefgSwapChainD3D12GetSwapChainPtr
```

Log:

```text
[XeFG]
context                  = 0x...
hwnd                     = 0x...
queue                    = 0x...
factory                  = 0x...
proxy swapchain          = 0x...
proxy vtable             = 0x...
Present owner            = ...
Present1 owner           = ...
```

### 18.5 Actual Present logging

For `Present` and `Present1`:

```text
[D3D12][Present]
kind                     = Present / Present1
swapchain                = 0x...
source                   = Native / Streamline / FSR3 / XeFG
vtable                   = 0x...
original target          = 0x...
target owner module      = ...
thread                   = ...
phase                    = phase1 / instance
```

Rate-limit after the first several frames.

Suggested policy:

- log the first 10 calls,
- then only state changes,
- allow full trace only through an explicit debug flag.

### 18.6 Swapchain transition logging

```text
[D3D12][SwapchainTransition]
old                      = 0x...
new                      = 0x...
reason                   = ...
old source               = ...
new source               = ...
```

Possible reasons:

```text
initial discovery
XeFG init
XeFG proxy replacement
ResizeBuffers
ResizeBuffers1
CreateSwapChainForHwnd
fullscreen transition
explicit rebind
```

### 18.7 Hook-monitor diagnostics

Expand generic messages with:

```text
[D3D12][HookMonitor]
renderer                 = D3D12
inside_present           = false
phase1                   = true
active_swapchain         = 0x...
swapchain_source         = ...
xefg_detected            = true
last Present age         = ...
last Present1 age        = ...
last swapchain transition age = ...
```

### 18.8 Resize lifecycle logging

For both resize paths:

```text
[D3D12][Resize]
kind                     = ResizeBuffers / ResizeBuffers1
swapchain                = 0x...
source                   = ...
buffer count             = ...
width/height             = ...
format                   = ...
flags                    = ...
result                   = ...
```

After successful recovery:

```text
[D3D12][ResizeRecovery]
swapchain                = 0x...
render targets recreated = true
REFramework initialized  = true
```

---

## 19. Development Phases

### P1 — Instrumentation Only

Goal:

- prove the exact XeFG swapchain topology,
- identify Present vs Present1 ownership,
- identify the final presentation object,
- capture the queue/factory supplied to XeFG.

Primary test:

```text
Dragon's Dogma 2
OptiScaler = dxgi.dll
REFramework = dinput8.dll
Special K = absent
XeFG = enabled
```

Expected result:

- the current failure may remain,
- but the log must identify exactly where the phase-1 hook is bypassed.

### P2 — XeFG Present Binding PoC

Goal:

- restore REFramework overlay without Special K.

Implement:

- XeFG detection,
- direct binding to the identified active presentation swapchain,
- Present1 support,
- direct use of the XeFG-supplied command queue where appropriate.

Initial success condition:

```text
Special K absent
XeFG active
OptiScaler overlay visible
REFramework overlay visible
REFramework scripting alive
```

### P3 — Lifecycle Stabilization

Implement:

- `ResizeBuffers1`,
- swapchain replacement detection,
- XeFG proxy lifecycle awareness,
- safe external swapchain rebinding,
- render-target reset/rebuild,
- hook-monitor XeFG awareness.

Completion condition:

- no recurring hook-monitor rehook,
- survives resolution changes,
- survives fullscreen/window transitions,
- survives Alt+Tab,
- survives game graphics-setting changes that recreate the swapchain.

This is the point at which Special K can be considered technically unnecessary.

### P4 — Requiem Stutter Validation

Only after P3 succeeds.

Test Resident Evil Requiem with:

```text
OptiScaler = dxgi.dll
REFramework fork = dinput8.dll
Special K = absent
XeFG = enabled
```

Measure:

- `Sending rehook request for D3D` count,
- `Unhooking D3D12` count,
- `Hooking D3D12` count,
- `IntegrityCheckBypass: Restored descriptor` count,
- frametime spikes,
- visible periodic stutter.

Compare against:

1. stock REFramework + OptiScaler, no SK,
2. stock REFramework + OptiScaler + Special K,
3. forked REFramework + OptiScaler, no SK.

---

## 20. Requiem Periodic Stutter Hypothesis

Independent measurements from the XeUnlock project observed recurring REFramework D3D12 hook recovery with XeFG in Resident Evil Requiem:

```text
Sending rehook request for D3D
→ Unhooking D3D12
→ Hooking D3D12
```

at approximately:

```text
11.006–11.010 seconds
```

The same measurements reported many:

```text
IntegrityCheckBypass: Restored descriptor
```

events without Special K and almost none with Special K.

REFramework's own RE9 integrity fallback warns that the `JobQueue::SubmitDescriptor` fallback may cause lag during integrity-check jobs.

A plausible chain is:

```text
XeFG changes/replaces the presentation swapchain
        ↓
REFramework loses the effective Present hook
        ↓
hook monitor eventually forces D3D12 rehook
        ↓
Requiem integrity / anti-tamper fallback activity increases
        ↓
large periodic hitch
        ↓
cycle repeats
```

This is a strong hypothesis for the Requiem-specific periodic stutter.

However:

> Do not design the fork around the stutter hypothesis.

First make XeFG presentation tracking correct.  
If the Requiem stutter disappears as a consequence, treat that as a validated secondary benefit.

---

## 21. Causes Already Weakened or Ruled Out

### Input / Insert-key failure

Ruled out. REFramework never initializes its D3D12 renderer in the failing case.

### General REFramework startup failure

Ruled out. Core/game hooks continue working.

### XeFG initialization failure

Ruled out. OptiScaler continues presenting successfully through XeFG.

### Command queue scan failure

Not supported. Both working and failing captures found the same observed command-queue offset.

### `Failed to get type info`

Not causal. It appears in both working and failing configurations.

### OptiScaler being named `dxgi.dll`

Ruled out as the fundamental cause.

### Later OptiScaler Special K-specific ResizeBuffers/Release workaround

Not the original overlay fix. The Special K + REF + XeFG combination already worked before that hardening existed.

---

## 22. Important Constraints

The fork must preserve normal REFramework behavior when XeFG is absent.

Do not regress:

- native D3D12 games,
- FSR3 / FSRFG behavior,
- DLSS-G / Streamline handling,
- D3D11,
- VR paths,
- normal Present-only games.

XeFG-specific behavior should activate only after positive XeFG detection.

Avoid:

- game-specific hardcoded addresses,
- Intel private object offsets,
- OptiScaler-specific private interfaces unless absolutely necessary,
- permanent global DXGI vtable replacement,
- unnecessary synchronization/state-machine complexity.

Prefer:

- public XeFG APIs,
- runtime module detection,
- instance-level swapchain hooks,
- explicit ownership/transition logging,
- existing REFramework renderer/reset machinery.

---

## 23. Expected Code Areas

Primary REFramework fork files:

```text
src/D3D12Hook.hpp
src/D3D12Hook.cpp
src/REFramework.cpp
src/REFramework.hpp
```

Likely new helper:

```text
src/XeFGHook.hpp
src/XeFGHook.cpp
```

Potential XeFG helper responsibilities:

- `libxess_fg.dll` detection,
- XeFG export resolution,
- init hook installation,
- queue/factory capture,
- proxy swapchain capture,
- presentation swapchain candidate tracking,
- callback into `D3D12Hook::bind_external_swapchain`.

Keep XeFG-specific code out of the generic D3D12 hook as much as practical.

---

## 24. Definition of Done for Special K Removal

The Special K removal work is complete when this configuration passes:

```text
Game         = Dragon's Dogma 2
OptiScaler   = dxgi.dll
REFramework  = forked dinput8.dll
Special K    = absent
FG Output    = XeFG
```

Required observations:

```text
XeFG works
OptiScaler overlay works
REFramework Insert menu works
REFramework scripting works
REFramework reaches D3D12 renderer initialization
Present and/or Present1 callbacks remain active
no recurring "Last chance encountered for hooking"
no recurring "Sending rehook request for D3D"
resolution changes recover correctly
fullscreen/window-mode changes recover correctly
Alt+Tab does not permanently break the overlay
swapchain recreation rebinds correctly
```

Only after all of the above are stable should Requiem be used as the next validation target.

---

## 25. Requiem Follow-up Validation

Once Special K has been removed successfully in DD2:

```text
OptiScaler   = dxgi.dll
REFramework  = forked dinput8.dll
Special K    = absent
XeFG         = enabled
```

Validate:

```text
REF overlay visible
Opti overlay visible
no recurring D3D rehook
no periodic loss of re.on_frame
no large recurring descriptor-restore burst
periodic ~11-second stutter absent or materially reduced
```

If periodic stutter remains despite a stable D3D hook, only then investigate independent XeFG frame-pacing, latency, queue, or OptiScaler issues.

---

## 26. Recommended Development Order

```text
P1 — Add detailed XeFG / DXGI / Present / Present1 diagnostics to REFramework
        ↓
Capture fresh DD2 no-SK logs
        ↓
Confirm exact active presentation swapchain and owner modules
        ↓
P2 — Add XeFG-aware external swapchain binding + Present1 support
        ↓
Restore REF overlay with no Special K
        ↓
P3 — Add ResizeBuffers1 + recreation/rebind lifecycle
        ↓
Eliminate recurring hook-monitor recovery
        ↓
Special K removal considered complete
        ↓
P4 — Test Resident Evil Requiem
        ↓
Verify whether the periodic stutter disappears as a consequence
```

---

## 27. Core Design Principle

The long-term fix should be framed as:

> **Native Intel XeFG swapchain support in REFramework**

not:

> an OptiScaler-specific Special K replacement hack.

OptiScaler is the current reproducer and deployment target, but the underlying problem is that REFramework's current D3D12 swapchain discovery model assumes a presentation topology that XeFG no longer guarantees.

The correct fix is to make REFramework aware of that topology and bind directly to the active XeFG presentation path.
