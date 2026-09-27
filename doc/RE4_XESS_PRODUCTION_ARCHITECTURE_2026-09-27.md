# RE4 XeSS Production Architecture — Final Target Design

**Repository:** onehoon/REFramework  
**Production branch:** feature/re4-xess  
**Target game:** Resident Evil 4 (2023), Direct3D 12  
**Validated game build:** RE4 1.5.9.0, Steam AppID 2050650, BuildID 22377325  
**Design date:** 2026-09-27  
**Status:** Final target architecture for production implementation

---

## 1. Purpose

This document defines the production architecture for adding a native-looking XeSS D3D12 producer path to Resident Evil 4 through this custom REFramework fork.

The reverse-engineering phase is complete through Capture 30b. The implementation phase begins from feature/re4-xess, which was created from the current XeFG-compatible master lineage.

The production feature must make RE4 behave, from the outside, like a normal game that natively integrates public XeSS D3D12:

~~~text
RE4
  -> REFramework RE4-only XeSS producer
  -> public XeSS D3D12 API
  -> libxess.dll
      -> native Intel XeSS when used directly
      -> or stock OptiScaler interception
          -> selected SR backend
          -> FGInput=Upscaler
          -> XeFG
~~~

REFramework must not introduce a private REFramework-to-OptiScaler API, must not create an RE4-specific OptiScaler fork, and must not add its own XeFG frontend.

---

## 2. Frozen product requirements

The following requirements are architectural invariants.

### 2.1 RE4-only runtime scope

The feature exists only for Resident Evil 4.

The production Mod must be constructed only when:

~~~cpp
sdk::GameIdentity::get().is_re4()
~~~

is true.

Important callbacks must also fail closed if the running title is not RE4.

For every other RE Engine title:

- no RE4 XeSS object is constructed;
- no XeSS DLL is loaded by the RE4 module;
- no XeSS context is created;
- no RE4-specific D3D12 resource is allocated;
- no SceneView size is modified;
- no projection jitter is injected;
- no RE4 resource pointer is inspected or changed by this subsystem;
- no extra per-frame RE4 XeSS logging is emitted.

Compiling the code into the universal REFramework DLL is acceptable. Runtime behavior outside RE4 must remain unchanged.

### 2.2 Existing XeFG compatibility code is a protected subsystem

The existing custom REFramework XeFG / OptiScaler compatibility work is not part of the RE4 XeSS implementation surface.

The following area is treated as protected / no-touch for this feature:

~~~text
src/compatibility/xefg/**
existing XeFG lifecycle logic in D3D12Hook
existing XeFG loader handoff logic
REFramework_XeFG_PreRetireSwapchainV1
existing XeFG resize / binding / runtime-transition behavior
~~~

If a separate XeFG defect is discovered later, it must be handled as a separate compatibility change, not mixed into RE4 XeSS implementation PRs.

The RE4 XeSS module may coexist with this compatibility layer but must not depend on its internal types or state.

### 2.3 Stock OptiScaler remains unmodified

The production contract is the public XeSS D3D12 API.

REFramework must never require:

- an OptiScaler-specific export;
- a private ABI;
- a custom OptiScaler build;
- an OptiScaler patch for RE4;
- direct access to OptiScaler internal state;
- knowledge of which SR backend OptiScaler selected.

The same RE4 producer must work in both cases:

~~~text
RE4 -> public XeSS -> native libxess.dll
RE4 -> public XeSS -> OptiScaler XeSS frontend -> alternate SR backend
~~~

### 2.4 RE4 Frame Generation scope is only Upscaler -> XeFG

RE4 has no native FG frontend.

Therefore the RE4 implementation does not emulate DLSSG, FSR FG, or any other game FG API.

For OptiScaler Frame Generation, the supported architecture is only:

~~~text
FGInput=Upscaler
    -> OptiScaler consumes the intercepted upscaler frame data
    -> XeFG
~~~

The RE4 module does not call libxess_fg.dll, does not create an XeFG context, and does not register XeFG resources.

### 2.5 Upscaler backend substitution must continue to work

RE4 always produces the public XeSS frontend contract.

OptiScaler remains free to substitute another supported SR backend behind that frontend.

REFramework must not branch on:

~~~text
native XeSS
DLSS
FSR
other OptiScaler SR backend
~~~

and must not calculate different RE4 inputs for those backends.

Quality selection and input resolution are expressed through the XeSS producer contract. Backend mapping remains OptiScaler responsibility.

### 2.6 Existing REFramework Debug Log setting is reused

The RE4 XeSS module may emit detailed diagnostics when the existing REFramework Debug Log option is enabled.

Do not add a second global debug switch just for RE4 XeSS.

The RE4 module should consume a neutral read-only debug-log accessor from REFrameworkConfig; it must not query XeFGCompatibility::is_debug_log_enabled(), because that would create an unnecessary dependency between the new RE4 module and the protected XeFG compatibility subsystem.

---

## 3. Branch and source ownership

### 3.1 Branch roles

~~~text
master
    current production baseline
    existing XeFG / OptiScaler compatibility remains here

refactor/re4-temporal-diagnostic
    research/archive branch
    Capture 1-30b evidence
    not the production implementation base

feature/re4-xess
    production implementation branch
    starts from current master lineage
~~~

Do not merge the diagnostic Mod wholesale into feature/re4-xess.

Only production-relevant, verified facts and minimal RE4-specific SDK corrections should be ported.

### 3.2 Production source layout

Target source layout:

~~~text
src/mods/re4_xess/
    RE4XeSS.hpp
    RE4XeSS.cpp

    RE4XeSSFrame.hpp

    RE4XeSSRuntime.hpp
    RE4XeSSRuntime.cpp

    RE4XeSSD3D12.hpp
    RE4XeSSD3D12.cpp

    RE4XeSSOutputHandoff.hpp
    RE4XeSSOutputHandoff.cpp
~~~

The layout may begin with fewer translation units and split as responsibilities grow, but these ownership boundaries should remain.

### 3.3 Allowed shared changes

Shared REFramework changes must be minimal and either behavior-neutral or explicitly RE4-gated.

Expected shared changes:

~~~text
src/Mods.cpp
    RE4-only registration

shared/sdk/Renderer.hpp / Renderer.cpp
    only RE4-specific, runtime-verified layout/accessor corrections

src/mods/REFrameworkConfig.hpp
    neutral read-only Debug Log accessor if needed

build files
    compile the new production sources
~~~

The RE4-specific Texture layout corrections proven by the diagnostic branch are expected to be ported:

~~~text
RE4 Texture desc offset:
    RenderResource::get_runtime_size() + 0x18

RE4 D3D12 resource-container offset:
    0xB8
~~~

They must remain explicitly RE4-gated and must not change other TDB 71 titles.

---

## 4. Target runtime topology

~~~text
                    Resident Evil 4 D3D12
                             |
                  Scene / PostEffect pipeline
                             |
                render-resolution HDR scene
                             |
                +------------+------------+
                |            |            |
              Color        Depth       Velocity
                |            |            |
                +------------+------------+
                             |
                    true pre-Overlay
                             |
                  RE4XeSS frame builder
                             |
             REFramework-owned DIRECT cmd list
                             |
                  public xessD3D12Execute
                             |
                display-resolution HDR output
                             |
                  RE4XeSSOutputHandoff
                             |
               RE4 Overlay / UI / output path
                             |
               native final screen-out pass
               DrawInstanced(3,1,0,0)
                             |
                       DXGI swapchain
                             |
                 existing presentation stack
                             |
            stock OptiScaler / XeFG when enabled
~~~

The architectural goal is:

> upscale the 3D/HDR scene before RE4's UI/final presentation work, then keep RE4's own presentation path responsible for the final swapchain image.

Direct XeSS-output-to-swapchain copying is not the target architecture.

---

## 5. Production components

### 5.1 RE4XeSS — RE4 coordinator

RE4XeSS is the only Mod-facing production object.

Responsibilities:

- RE4-only construction and callback gating;
- configuration and enable/disable state;
- SceneView render-size control;
- projection jitter injection;
- temporal input collection;
- reset/history state machine;
- coordination of XeSS runtime, D3D12 executor, and output handoff;
- device/reset lifecycle;
- fail-closed rollback to normal RE4 rendering.

It must not contain OptiScaler-specific logic.

It must not contain XeFG-specific logic.

### 5.2 RE4XeSSFrame — normalized frame contract

All RE4 engine semantics are normalized before the XeSS runtime sees them.

Target frame packet:

~~~cpp
struct RE4XeSSFrame {
    ID3D12Resource* color{};
    ID3D12Resource* depth{};
    ID3D12Resource* velocity{};

    uint32_t render_width{};
    uint32_t render_height{};
    uint32_t display_width{};
    uint32_t display_height{};

    float jitter_x_pixels{};
    float jitter_y_pixels{};

    float motion_scale_x{};
    float motion_scale_y{};

    float near_plane{};
    float far_plane{};
    float vertical_fov{};

    bool reset_history{};
    uint64_t frame_id{};
};
~~~

Exact type naming can change.

No OptiScaler type belongs in this structure.

### 5.3 RE4XeSSRuntime — public XeSS frontend

This component owns only the public XeSS producer API contract.

Responsibilities:

- discovery of libxess.dll only at <REF>\libxess.dll or <REF>\OptiScaler\libxess.dll, followed by exact-path loading;
- resolution of required public XeSS exports;
- context creation;
- quality/input-resolution query;
- D3D12 initialization;
- velocity scale configuration;
- execute dispatch;
- context destruction;
- **single-thread ownership of every public XeSS API call**;
- API result logging;
- runtime capability/version reporting under Debug Log.

REFramework does not distribute the XeSS runtime. Resolve the directory containing the loaded REFramework DLL, prefer <REF>\libxess.dll, then <REF>\OptiScaler\libxess.dll, and load the selected DLL by exact path. Do not search arbitrary PATH/current-working-directory locations or mutate process-wide DLL search state.

XeSS-SR is not thread-safe. The production owner thread is the stable thread executing the true RE4 `on_pre_overlay_layer_draw()` callback. Context creation, version query, optimal-input query, initialization, velocity-scale setup, execute, and context destruction all run only on that recorded owner thread.

Non-owner callbacks may request state transitions but must not call public XeSS functions. If an owner-thread callback is not available for an otherwise necessary teardown, fail closed and quarantine the old XeSS context/module rather than making a cross-thread XeSS call.

No libxess_fg.dll API is loaded here.

### 5.4 RE4XeSSD3D12 — command submission and bridge-owned GPU lifetime

Responsibilities:

- own the REFramework command allocator/list ring;
- use the active RE4 DIRECT queue;
- own the fence used for allocator/list reuse **and fence-scoped lifetime proof**;
- own temporary per-slot COM pins for submitted RE4 Color/Depth/original-Velocity resources;
- own XeSS-specific output/conversion resources that are not engine objects;
- perform required legacy ResourceBarrier transitions;
- reset/recreate resources when display size or device identity changes;
- never modify or append to an engine-owned command list.

The research campaign proves that an REFramework-owned DIRECT list can be submitted from the true pre-Overlay callback safely.

The implementation must rely on the semantic callback boundary, not on a hardcoded queue ordinal.

### 5.5 RE4XeSSOutputHandoff — engine re-entry boundary

This is intentionally a separate component because it is the highest-risk part of production integration.

Its architectural contract is fixed:

1. take a valid display-resolution HDR XeSS result;
2. expose it to RE4's own downstream Overlay/UI/output pipeline;
3. keep RE4's final screen-out/presentation path active;
4. never use a direct copy to the swapchain as the normal path;
5. never drain foreign COM references;
6. restore original engine state cleanly on disable, failure, resize, or device reset.

The preferred first implementation is an engine-visible display-resolution target-state handoff:

- derive/clone an RE4 engine TargetState from the semantic Overlay/HDR target;
- size that handoff target to the display resolution;
- use the native D3D12 resource behind that handoff target as the XeSS output when its format/capabilities are valid;
- at the proven pre-Overlay boundary, redirect only Overlay::get_main_target_state() to that display-resolution handoff state;
- do not modify PrepareOutput / OutputTargetState for the first handoff because current RE4 evidence identifies it as swapchain presentation state;
- keep the handoff installed across the original Overlay draw and later output/composite recording;
- restore the saved original Overlay main TargetState at the **next** true pre-Overlay callback, before resolving the next frame's semantic Color;
- perform install/restore through sdk::intrusive_ptr assignment so refcount changes remain balanced;
- preserve the original engine state/pointers and restore or safely retire them on disable, failure, resize, or device-generation change.

This architecture keeps RE4 responsible for its own UI and final presentation instead of replacing the final backbuffer.

#### 5.5.1 Output-format adaptation

The public XeSS SR contract requires the output texture to use the same color format and color space as the input Color texture.

For the validated RE4 build:

~~~text
input Color:
    DXGI_FORMAT_R11G11B10_FLOAT

XeSS-native output:
    DXGI_FORMAT_R11G11B10_FLOAT
    display resolution
    UNORDERED_ACCESS
~~~

Do not silently choose a different XeSS output format.

If PR 4 needs a different engine-visible handoff format, any conversion happens **after** XeSS:

~~~text
XeSS output
    same format/color space as input Color
            |
      optional post-XeSS conversion
            |
engine-visible HDR handoff target
~~~

Any such conversion is bridge-owned and happens on the same ordered REFramework command submission path.

#### 5.5.2 Narrow validation gate

Capture 30b proves the native final output window and one unique fullscreen-triangle style draw:

~~~text
Swapchain 0x00 -> 0x04
Color     0x04 -> 0xC0
DrawInstanced(3,1,0,0)
Swapchain 0x04 -> 0x00
Present
~~~

It does not prove descriptor binding for that draw.

Therefore the first real output-handoff implementation must verify that the substituted engine-visible handoff target flows through the downstream final path.

If it does not, do not fall back to copying the XeSS result directly to the swapchain.

Instead, add the narrowest possible descriptor/SRV provenance probe around that already-proven single draw and adjust only RE4XeSSOutputHandoff.

The rest of the producer architecture must remain unchanged.

---

## 6. Verified RE4 temporal input contract

The production implementation uses the following runtime-verified semantics.

### 6.1 Color

~~~text
semantic source:
    Overlay main native resource
    == Scene PostMainTarget
    == Scene HDRTarget

format observed:
    R11G11B10_FLOAT

true pre-Overlay state:
    0xC0
    PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE
~~~

Do not use transient RenderContext::get_render_target() as the production Color anchor.

### 6.2 Depth

~~~text
semantic source:
    Scene::DepthStencilTex

true pre-Overlay state:
    0xE0
    DEPTH_READ
    | PIXEL_SHADER_RESOURCE
    | NON_PIXEL_SHADER_RESOURCE

depth convention:
    inverted / reversed
~~~

XeSS initialization must include the inverted-depth producer flag.

### 6.3 Motion vectors

~~~text
semantic source:
    Scene::VelocityTarget

current RE4 native format:
    DXGI_FORMAT_R16G16B16A16_SNORM

channels:
    R = X
    G = Y

jittered MV:
    false

true pre-Overlay state:
    0x04
    RENDER_TARGET

motion scale:
    X =  renderWidth / 2
    Y = -renderHeight / 2
~~~

The public XeSS low-resolution motion-vector contract uses `DXGI_FORMAT_R16G16_FLOAT`. Therefore the semantic source remains `Scene::VelocityTarget`, but the production XeSS execute path converts RE4 R/G normalized values into a bridge-owned `R16G16_FLOAT` texture before dispatch.

The conversion does not apply pixel scaling. `xessSetVelocityScale(renderWidth / 2, -renderHeight / 2)` remains the public producer conversion from normalized RE4 velocity values to pixel motion.

The original engine VelocityTarget is transitioned from 0x04 for the bridge conversion and restored to 0x04 before returning control to RE4.

### 6.4 Camera metadata

~~~text
near plane:
    primary via.Camera NearClipPlane

far plane:
    primary via.Camera FarClipPlane

vertical FOV:
    2 * atan(1 / SceneInfo.projection[1][1])
~~~

Do not hardcode the observed static ~45 degree state.

### 6.5 Render and display sizes

~~~text
render size:
    bridge-controlled SceneView size
    must match Color / Depth / Velocity extents

display size:
    active DXGI swapchain/output size
~~~

Do not use D3D12Hook render-size hints as the authoritative scene-size source.

Do not modify ImageQualityRate unless a later production contradiction proves it necessary.

---

## 7. XeSS producer contract

### 7.1 Public API only

The runtime uses normal XeSS D3D12 entry points.

Expected public calls include the equivalent of:

~~~text
xessD3D12CreateContext
xessGetOptimalInputResolution
xessD3D12Init
xessSetVelocityScale
xessD3D12Execute
xessDestroyContext
~~~

Optional public version/properties/logging calls may be used for diagnostics.

REFramework does not integrate or redistribute the XeSS SDK package for this feature. Keep only the minimal public XeSS ABI declarations required by the producer inside the RE4 XeSS subsystem, and resolve the runtime functions dynamically from the selected libxess.dll.

Do not duplicate private OptiScaler headers or internal types.

### 7.2 Input resolution policy

REFramework should request the optimal XeSS input resolution from the public XeSS frontend for the selected XeSS quality mode and current display size.

Do not hardcode DLSS, FSR, or OptiScaler backend ratios in REFramework.

This matters for OptiScaler compatibility because stock OptiScaler already implements the XeSS frontend resolution query and can apply its own configured override/mapping behind the same public contract.

Flow:

~~~text
display size
    -> xessGetOptimalInputResolution(...)
    -> render size
    -> SceneView size override
    -> actual Color/Depth/Velocity extents
~~~

Before executing XeSS, the actual resource extents must match the requested render size.

### 7.3 Upscaling-mode and XeSS quality policy

REFramework must expose a **RE4-only Upscaling Mode** selector in the REFramework UI.

The UI contract is:

~~~text
RE4 XeSS
  Upscaling Mode
    Off
    Native AA
    Ultra Quality Plus
    Ultra Quality
    Quality
    Balanced
    Performance
    Ultra Performance
~~~

`Off` is a local REFramework state, not an XeSS quality enum.

The active modes map directly to the public XeSS producer quality settings:

| REFramework UI | XeSS producer setting |
|---|---|
| Native AA | `XESS_QUALITY_SETTING_AA` |
| Ultra Quality Plus | `XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS` |
| Ultra Quality | `XESS_QUALITY_SETTING_ULTRA_QUALITY` |
| Quality | `XESS_QUALITY_SETTING_QUALITY` |
| Balanced | `XESS_QUALITY_SETTING_BALANCED` |
| Performance | `XESS_QUALITY_SETTING_PERFORMANCE` |
| Ultra Performance | `XESS_QUALITY_SETTING_ULTRA_PERFORMANCE` |

The RE4 UI must **not** display or select the actual OptiScaler SR backend. It selects only the XeSS frontend quality preset presented by RE4.

Therefore the same selection remains valid whether the public XeSS calls reach:

~~~text
native Intel XeSS
or
stock OptiScaler -> XeSS / DLSS / FSR / another supported SR backend
~~~

REFramework must not translate these modes into hardcoded DLSS/FSR scaling ratios.

For every non-Off mode:

~~~text
selected XeSS quality
    -> xessGetOptimalInputResolution(display size, quality)
    -> returned render size
    -> SceneView size
    -> Color / Depth / Velocity extents
~~~

`Native AA` is the 1:1 temporal-AA mode:

~~~text
render size  = display size
XeSS execute = active
~~~

A change between any active modes is a temporal/lifecycle event, not a simple UI-only change. The implementation must:

1. record the new XeSS quality setting;
2. query the new optimal input resolution;
3. update SceneView render size;
4. recreate or reinitialize XeSS/output resources if the active runtime requires it;
5. invalidate temporal history;
6. submit the first valid frame with `resetHistory = true`.

Changing to `Off` must stop XeSS execution and restore the complete native RE4 path:

- native SceneView sizing;
- no XeSS projection jitter injection;
- no XeSS output handoff;
- no RE4 XeSS command submission;
- temporal history invalidated for any later re-enable.

The UI itself must be registered/rendered only for RE4.

During early bring-up, implementation PRs may validate one preset at a time, but the final configuration contract is the full selector above.

No REF UI should offer DLSS/FSR backend selection, XeFG backend selection, or FG interpolation controls. Those belong to OptiScaler.

### 7.4 Motion scale

Use the verified RE4 producer conversion:

~~~text
xess velocity scale X =  renderWidth / 2
xess velocity scale Y = -renderHeight / 2
~~~

This is expressed through the public XeSS producer API.

The RE4 module does not special-case the downstream OptiScaler backend.

### 7.5 Jitter

RE4 projection injection mechanics are already proven.

The production jitter generator must:

- be deterministic;
- follow the XeSS producer sampling policy selected by the integrated SDK/sample guidance;
- pass the same pixel-space jitter values to XeSS execute metadata;
- convert them into RE4 projection offsets using the proven RE4 mapping;
- maintain previous/current projection history consistently;
- never copy fixed values from external MOD screenshots.

Verified RE4 matrix mapping:

~~~text
projection[2][0] += +2 * jitterX / renderWidth
projection[2][1] += -2 * jitterY / renderHeight
~~~

### 7.6 Init flags and producer semantics

Frozen producer semantics:

~~~text
inverted depth:
    true

jittered motion vectors:
    false

motion-vector resolution:
    render resolution, not display resolution

input scene color:
    HDR scene color, not an LDR swapchain image
~~~

Exposure handling must use a documented public XeSS path. No unverified RE4 exposure texture should be added merely to imitate another implementation.

---

## 8. Per-frame execution timeline

Target steady-state frame flow:

~~~text
[frame start]
    |
    | restore any temporary engine handoff from previous frame
    | process pending recreation/reset
    v
RE4 SceneView size query
    |
    | return selected render resolution
    v
RE4 camera/projection queries
    |
    | inject current XeSS jitter
    | maintain projection history
    v
RE4 scene / post processing
    |
    | produces render-resolution:
    |   Color
    |   Depth
    |   Velocity
    v
on_pre_overlay_layer_draw()
    |
    | validate RE4 semantic resource invariants
    | build RE4XeSSFrame
    | set resetHistory if required
    |
    | bridge-owned DIRECT list:
    |   transition original Velocity 0x04 -> readable
    |   convert RE4 SNORM RG -> bridge R16G16_FLOAT MV
    |   prepare XeSS output
    |   xessD3D12Execute
    |   optional post-XeSS output conversion
    |   prepare engine-visible handoff state
    |   restore original Velocity -> 0x04
    |
    | execute bridge list on active DIRECT queue
    | install downstream output handoff
    v
original RE4 Overlay / UI path
    |
    v
RE4 final output path
    |
    | proven unique final pass:
    | DrawInstanced(3,1,0,0)
    v
swapchain Present
    |
    v
existing REFramework / OptiScaler / XeFG presentation behavior
~~~

No CPU fence wait is performed every frame.

GPU ordering comes from submitting the bridge list to the same DIRECT queue at the proven semantic boundary.

The bridge fence exists for allocator/list reuse and lifecycle safety only.

---

## 9. Command-list and resource-state ownership

### 9.1 Engine resources are borrowed semantically, but submitted GPU inputs are fence-pinned

The bridge borrows:

~~~text
Color
Depth
Velocity
active D3D12 device
active DIRECT queue
engine semantic TargetState references needed for handoff
~~~

It does not take semantic ownership and must never aggressively drain COM references.

The pointer-bearing `RE4XeSSFrame` is callback-scoped CPU data and must not be queued to another thread.

However, D3D12 XeSS Execute only records commands; actual GPU execution happens after submission. Therefore a submitted command slot must hold balanced strong COM references to the engine Color, Depth, and original Velocity resource until that slot's fence completion is proven.

The ownership rule is:

~~~text
CPU packet:
    valid only during true pre-Overlay callback

submitted GPU inputs:
    slot-local ComPtr pin
    acquire before recording/submission
    retain through fence completion
    release only on proven safe slot reuse

cross-frame diagnostics:
    opaque address values only
~~~

This bounded AddRef/Release pair is a lifetime pin, not ownership transfer. No refcount-draining or foreign-resource retirement logic is permitted.

### 9.2 Bridge resources are owned

The bridge owns:

~~~text
XeSS context
bridge command allocators
bridge command lists
bridge writer fence
bridge R16G16_FLOAT motion-vector conversion resource
motion-vector conversion descriptors/root signature/PSO
XeSS-native output
post-XeSS output conversion resource if required
cloned/bridge-owned engine handoff TargetState where used
handoff downstream-retirement fence
temporary descriptors required by bridge-owned passes
~~~

The two fence domains have different meanings:

~~~text
bridge writer fence
    proves completion of REF/XeSS commands that write/read bridge inputs/output

downstream retirement fence
    is signaled on the same active DIRECT queue after Present
    and proves completion ordering for RE4 command submissions that consumed
    the installed handoff output later in the frame
~~~

Bridge writer completion alone is never sufficient evidence to destroy a handoff TargetState that RE4 may still be consuming.

All bridge-owned resources must be destroyed or recreated on the correct lifecycle transitions.

### 9.3 Allocator/list ring

Use an REFramework-owned ring large enough to avoid normal CPU stalls.

The diagnostic campaign proved safe nonblocking reuse with an eight-slot ring.

The production implementation may retain eight slots unless a later simplification is demonstrated safe.

Each slot contains at least:

~~~text
ID3D12CommandAllocator
ID3D12GraphicsCommandList
slot-local descriptor heap
ComPtr<Color>
ComPtr<Depth>
ComPtr<original Velocity>
ComPtr<supplied XeSS output>
last writer-fence value
~~~

Before resetting a slot, verify its previous fence has completed. Only then release the old engine-resource pins and rewrite/reset the slot.

`ID3D12Fence::GetCompletedValue() == UINT64_MAX` is a device-removal sentinel and must never be interpreted as normal completion.

Do not spin or block the render thread indefinitely.

### 9.4 Handoff downstream retirement

The engine-visible output TargetState remains owned by RE4XeSSOutputHandoff across the frame after installation.

Normal frame-to-frame reuse is safe without a CPU wait only while all relevant work remains ordered on the same active DIRECT queue:

~~~text
frame N XeSS write
    -> frame N RE4 Overlay/final-output reads
    -> frame N+1 XeSS write
~~~

Same-generation reuse is permitted only after the prior installed frame's post-Present marker has been queued. If the next pre-Overlay restoration occurs while that marker is still missing, do not submit or reinstall the same handoff generation. Wait nonblockingly for a later valid same-generation settlement marker, then resume reuse on a subsequent pre-Overlay callback. This prevents a late marker for frame N from being mistaken as evidence for a newly installed frame N+1 reader.

For destruction/recreation, stronger proof is required.

When an installed handoff reaches the presentation path, the existing Mod::on_post_present() callback signals a dedicated retirement fence on the same handoff-generation DIRECT queue.

Completion of that fence value proves queue retirement of the RE4 downstream consumers submitted before Present.

Therefore a handoff generation may be destroyed/recreated only when:

~~~text
writer-side bridge work is safe
AND
the latest downstream retirement marker is complete
~~~

unless confirmed device removal terminally ends the old D3D12 generation.

If the post-Present callback is suppressed or no retirement marker exists, do not infer completion from time/frame count/bridge idleness. Keep an explicit missing-marker state and quarantine the old generation. After next-pre-Overlay restoration, a later valid Present may append a settlement marker to the pinned same-generation queue even if that later frame did not install the handoff; restoration ensures no new readers are added. If a valid marker is queued but incomplete, report nonblocking Draining and poll again on a later owner callback.

If the retirement Signal fails or the callback observes a different active device/queue, hard-quarantine the old handoff generation. Do not signal a replacement queue or assume it covers the previous consumers. Confirmed device removal is the terminal exception.

The post-Present callback is not assumed to run on the true pre-Overlay owner thread. It pins and synchronizes access to its generation's queue/fence, then only Signals and publishes marker evidence. It performs no XeSS API call, Overlay/TargetState access, TargetState release, or fence wait. The true pre-Overlay owner thread alone polls retirement and releases/recreates the engine TargetState after both writer and downstream proofs are safe.

No public XeSS API is called from on_post_present().

### 9.5 Resource-state rules

At the true pre-Overlay boundary:

~~~text
Color    = 0xC0
Depth    = 0xE0
Velocity = 0x04
~~~

Color and Depth are already shader-readable for the verified path.

Original RE4 Velocity requires a bridge transition for the conversion pass and must be restored to 0x04.

The converted bridge-owned `R16G16_FLOAT` motion-vector resource is transitioned from UAV write to NON_PIXEL_SHADER_RESOURCE before XeSS consumes it.

Output resources follow their own bridge-owned state tracker.

Do not add redundant barriers to engine resources merely for diagnostics.

---

## 10. Reset and lifecycle state machine

Target coordinator state:

~~~text
Disabled
    |
    v
WaitingForD3D12
    |
    v
WaitingForOwnerThread
    |
    v
WaitingForValidScene
    |
    v
Ready
    |
    v
Active
    |
    +--> RecreatePending
    |       |
    |       v
    |      Ready
    |
    +--> Faulted
    |       |
    |       +--> retry only on explicit re-enable or valid device recreation
    |
    +--> Quarantined
            |
            +--> no slot reuse / no guessed completion
            +--> exit only after safe terminal condition
~~~

### 10.1 History reset conditions

The first valid frame after any of the following uses XeSS history reset:

- first enable;
- re-enable after Off;
- Upscaling Mode / XeSS quality change;
- first valid dispatch;
- render-size change;
- display-size change that recreates output;
- Color identity change;
- Depth identity change;
- Velocity identity change;
- XeSS context recreation;
- D3D12 device recreation;
- relevant swapchain recreation;
- RE4 Load Save recovery;
- recovery from a missing/invalid temporal input.

### 10.2 RE4 Load Save rule

Capture 22 shows that GameSituationManager.InhibitBit can leave its normal value **before** SceneLoadZoneManager._Pause rises. The captured ordering was:

~~~text
normal InhibitBit
    -> departure value

later:
_Pause false -> true

later:
_Pause true -> false

later:
InhibitBit returns to the original pre-load normal value
~~~

Therefore production code must freeze the remembered normal inhibit baseline when the first departure is observed and must not overwrite that baseline merely because _Pause has not risen yet.

Evidence-backed normal path:

~~~text
stable gameplay
    remember normal InhibitBit

current InhibitBit != remembered normal
    freeze remembered normal value
    arm history-invalid / pending-load state

_Pause becomes true
    confirm load-transition state
    keep frozen normal value

_Pause becomes false
    do not immediately resume

current InhibitBit == frozen remembered normal
    recovery is armed

first fully valid gameplay frame
    resetHistory = true
    then resume accumulation
~~~

If InhibitBit returns to the frozen baseline before _Pause ever rises, clear the pending-load suspicion but still reset temporal history on the next fully valid frame.

If the first observation occurs while _Pause is already true, enter history-invalid load state immediately and do not invent a pre-load baseline. A conservative implementation may rebaseline only after _Pause is false and a subsequent InhibitBit transition establishes a new stable value; it must never hardcode the observed 0xB9 value.

Do not hardcode the inhibit-bit value.

Do not use camera translation alone as a Load Save reset heuristic.

### 10.3 Device / swapchain lifecycle

On D3D12 device reset or incompatible swapchain/display-size recreation:

1. stop scheduling new XeSS dispatches immediately;
2. restore any temporary RE4 output handoff when the old Overlay object is still valid;
3. mark the old execution generation draining or terminal;
4. poll the bridge writer fence without per-frame blocking;
5. release slot-local engine input/output pins only after normal writer-fence completion is proven;
6. require completion of the latest downstream-retirement fence before releasing an engine-visible handoff TargetState;
7. if the downstream marker is missing/unprovable, quarantine the old handoff generation instead of releasing it;
8. destroy bridge-owned output/conversion resources only after the applicable writer/consumer lifetime proofs are safe;
9. destroy the XeSS context only on the recorded XeSS owner thread;
10. reset allocator/list/fence state;
11. return to WaitingForD3D12 / RecreatePending;
12. reinitialize only when the new D3D12 state is valid.

Fence rule:

~~~text
completed = bridgeFence->GetCompletedValue()

completed == UINT64_MAX
    -> DeviceRemoved
    -> never treat as completed >= target
    -> never reuse old slots

otherwise
    -> normal completion comparison is allowed
~~~

A Signal failure after `ExecuteCommandLists` is special: the list may have been accepted while writer completion proof was lost. That execution generation must keep its slot pins, command objects, bridge-owned resources, and XeSS context quarantined until either normal completion becomes provable or an explicit device-removal/reset terminal condition disposes the old device generation.

A downstream-retirement Signal failure is independently terminal for handoff lifetime: keep the handoff TargetState/resource generation quarantined until a valid same-generation retirement proof or confirmed device removal exists.

If neither writer nor downstream-consumer completion can be proven as required, keep that generation quarantined for the rest of the process rather than guessing completion.

Non-owner callbacks such as device-reset notification may only request teardown. They must not call public XeSS APIs directly.

The RE4 module must not call or alter XeFG lifecycle internals.

---

## 11. Fail-closed behavior

A production failure must prefer native RE4 rendering over a partially active bridge.

Examples that force a safe skip or session fault:

~~~text
not RE4
not D3D12
missing public XeSS runtime
required XeSS export missing
XeSS context creation failure
XeSS init failure
invalid Color / Depth / Velocity
resource extent mismatch
invalid active DIRECT queue
output handoff invariant mismatch
device identity mismatch
bridge allocator/list reuse not safe
~~~

If a failure occurs after the bridge changed SceneView size or installed a temporary handoff:

- restore native engine state;
- stop injecting jitter;
- stop submitting XeSS work;
- mark temporal history invalid;
- log the reason;
- leave the game rendering through its normal path.

Never keep low-resolution SceneView active while XeSS execution is unavailable.

---

## 12. OptiScaler compatibility contract

The RE4 producer does not detect whether libxess.dll is Intel's runtime or OptiScaler's XeSS proxy path.

That is intentional.

The public call stream remains the same and is serialized on the single pre-Overlay XeSS owner thread:

~~~text
CreateContext
GetOptimalInputResolution
Init
SetVelocityScale
Execute every valid frame
DestroyContext
~~~

Stock OptiScaler can then:

- intercept the XeSS producer;
- capture Color / Depth / Velocity / Output / jitter / reset / sizes;
- route the request to another SR backend;
- keep REFramework unaware of the selected backend.

The RE4 implementation must not call backend-specific code after OptiScaler is detected.

No "OptiScaler mode" exists inside RE4XeSS.

---

## 13. XeFG compatibility contract

RE4 has no native FG producer.

Therefore the only supported FG validation path is:

~~~text
RE4 public XeSS producer
    -> stock OptiScaler
    -> selected SR backend
    -> FGInput=Upscaler
    -> XeFG
~~~

The existing REFramework XeFG compatibility subsystem remains responsible for:

- libxess_fg.dll discovery/handoff;
- XeFG swapchain proxy ownership;
- presentation queue/swapchain binding;
- resize lifecycle;
- runtime transition handling;
- hook-monitor compatibility;
- public proxy retirement coordination.

The new RE4 module must not duplicate any of those responsibilities.

### 13.1 Validation order

XeFG is not tested until the same producer is already correct in these stages:

~~~text
1. native RE4 without RE4XeSS enabled
2. RE4 + native XeSS SR
3. RE4 + stock OptiScaler + alternate SR backend
4. RE4 + stock OptiScaler + FGInput=Upscaler + XeFG
~~~

If stage 3 works and stage 4 fails, do not immediately change the RE4 XeSS producer contract.

First isolate whether the failure belongs to the existing presentation/XeFG compatibility layer.

---

## 14. Debug logging

The existing REFrameworkConfig_DebugLog switch controls detailed RE4 XeSS diagnostics.

Recommended log namespaces:

~~~text
[RE4XeSS][Init]
[RE4XeSS][Runtime]
[RE4XeSS][Frame]
[RE4XeSS][Execute]
[RE4XeSS][Output]
[RE4XeSS][Reset]
[RE4XeSS][Resize]
[RE4XeSS][Failure]
~~~

Normal info/warn/error logs should be event-driven.

Debug Log may additionally include frame-level state.

Prefer logging state transitions instead of dumping every frame indefinitely.

Important debug fields:

~~~text
enabled state
XeSS runtime version/path
device / queue identity
quality mode
render resolution
display resolution
Color / Depth / Velocity identities
input resource states
jitter
motion scale
resetHistory + reason
XeSS execute result
output identity/format/state
handoff install/restore
context generation
device/swapchain generation
~~~

Do not reuse the [XeFG] prefix for RE4 XeSS logs.

---

## 15. Configuration and REFramework UI policy

The production UI is **RE4-only** and intentionally exposes one primary control:

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

A separate global `Enabled` checkbox is not required. `Off` is the disabled state.

Recommended persisted configuration model:

~~~text
RE4XeSS_UpscalingMode
~~~

The stored value should map to a stable REFramework enum/string and then to the public XeSS quality enum at runtime. Do not persist an OptiScaler backend name in REFramework.

The UI must not be rendered for non-RE4 games.

### 15.1 Mode behavior

~~~text
Off
    native RE4 rendering
    native SceneView size
    no XeSS jitter
    no XeSS execute
    no XeSS output handoff

Native AA
    render size = display size
    XeSS temporal AA active

all other active modes
    query render size through xessGetOptimalInputResolution
    SceneView uses the returned render size
    XeSS outputs at display resolution
~~~

Changing the mode at runtime must be treated as a controlled reconfiguration:

~~~text
mode changed
    -> mark current XeSS generation invalid
    -> restore any temporary output handoff
    -> query new input size if active
    -> recreate/reinit resources as required
    -> reset temporal history
    -> resume on first fully valid frame
~~~

### 15.2 OptiScaler ownership boundary

Do not add any of the following to REFramework UI:

- OptiScaler SR backend selector;
- DLSS/FSR-specific quality labels;
- XeFG backend selector;
- FG interpolation count;
- OptiScaler-specific toggles.

The REFramework selector describes only the public XeSS frontend contract. If OptiScaler intercepts it, backend substitution remains completely owned by OptiScaler.

### 15.3 Debug logging

Detailed UI/mode transition logs use the existing global REFramework Debug Log setting.

Recommended events:

~~~text
[RE4XeSS][Config] oldMode=... newMode=...
[RE4XeSS][Config] quality=... display=... requestedInput=...
[RE4XeSS][Reset] reason=quality_change
[RE4XeSS][Reset] reason=enable
[RE4XeSS][Reset] reason=disable
~~~

Do not create a second RE4-only debug toggle.

---

## 16. Implementation phases and PR boundaries

### PR 1 — production shell + public XeSS runtime lifecycle

Implementation work order:

~~~text
doc/RE4_XESS_PR1_WORK_ORDER_2026-09-27.md
~~~

Scope:

- create RE4-only Mod;
- RE4-only registration in Mods.cpp;
- port only required RE4 SDK layout fixes;
- add neutral Debug Log accessor;
- add public XeSS runtime loader/export table;
- context create/destroy scaffolding;
- no SceneView modification;
- no jitter;
- no execute;
- no output mutation.

Acceptance:

~~~text
RE4:
    module can initialize public XeSS runtime when enabled

non-RE4:
    no RE4XeSS construction/runtime activity

protected XeFG compatibility code:
    zero behavioral changes
~~~

### PR 2 — production temporal frame builder

Implementation work order:

~~~text
doc/RE4_XESS_PR2_WORK_ORDER_2026-09-27.md
~~~

Scope:

- semantic Color / Depth / Velocity accessors;
- SceneView render-size control;
- camera metadata;
- jitter injection/history;
- reset state machine;
- no visible output handoff yet.

Acceptance:

~~~text
frame packet matches research contract
actual resource extents match requested input size
native engine state restores cleanly on disable/failure
~~~

### PR 3 — real XeSS execute to detached output

Implementation work order:

~~~text
doc/RE4_XESS_PR3_WORK_ORDER_2026-09-27.md
~~~

Scope:

- allocator/list/fence ring;
- input barriers;
- RE4 SNORM -> XeSS R16G16_FLOAT motion-vector conversion;
- public XeSS init/execute;
- same-format bridge-owned detached output;
- original Velocity restoration;
- output not yet consumed by RE4 presentation.

Acceptance:

~~~text
native XeSS execute succeeds repeatedly
no GPU validation/state errors
no allocator reuse hazard
no engine input state corruption
~~~

### PR 4 — RE4 output handoff

Implementation work order:

~~~text
doc/RE4_XESS_PR4_WORK_ORDER_2026-09-27.md
~~~

Scope:

- engine-visible display-resolution handoff target;
- optional output conversion;
- pre-Overlay handoff installation;
- downstream RE4 Overlay/UI/output path;
- handoff restoration and lifecycle.

Acceptance:

~~~text
visible XeSS SR output
RE4 UI preserved
no direct swapchain copy architecture
final native screen-out path remains active
bridge writer lifetime and downstream RE4 consumer lifetime are both proven before handoff release
fresh COMMON -> UAV -> 0xC0 and steady 0xC0 -> UAV -> 0xC0 validated by D3D12 debug-layer evidence or narrow provenance
resize / enable-disable safe
~~~

If the target-state handoff does not reach the proven final pass, add only the narrow final-draw descriptor provenance required to correct this component.

### PR 5 — stock OptiScaler SR substitution

Scope:

- no producer architecture change;
- validate stock OptiScaler interception;
- test at least one non-XeSS SR backend through the XeSS frontend;
- verify quality/input-resolution behavior;
- verify reset and resize.

Acceptance:

~~~text
same RE4 producer code
OptiScaler successfully substitutes SR backend
no private REF/OptiScaler ABI
~~~

### PR 6 — Upscaler -> XeFG validation

Scope:

- FGInput=Upscaler;
- existing REFramework XeFG compatibility unchanged unless a separately proven defect exists;
- generated-frame runtime;
- resize/fullscreen/Alt+Tab;
- context recreation;
- long-session lifecycle.

Acceptance:

~~~text
SR remains correct
XeFG generated frames stable
no swapchain lifecycle regression
no impact on other games
~~~

### PR 7 — production polish

Scope:

- validate all exposed XeSS Upscaling Mode entries;
- UI/config polish;
- diagnostic rate limiting;
- cleanup of temporary implementation-only instrumentation;
- final non-RE4 regression pass.

---

## 17. Regression and acceptance matrix

Every production milestone must preserve these baselines.

### Non-RE4 regression

At minimum, verify representative D3D12 titles already supported by the custom fork still follow their normal path.

Expected invariant:

~~~text
RE4XeSS object not constructed
no libxess.dll load initiated by RE4XeSS
no RE4 SceneView/jitter/resource mutations
existing XeFG compatibility behavior unchanged
~~~

### RE4 native path

~~~text
RE4XeSS disabled
    game behavior matches baseline custom REFramework
~~~

### RE4 native XeSS

~~~text
RE4XeSS enabled
OptiScaler absent
    public XeSS runtime works
    render/display split works
    UI remains correct
~~~

### RE4 OptiScaler SR

~~~text
same RE4XeSS producer
stock OptiScaler installed
alternate SR backend selected
    output correct
    no REF backend-specific code
~~~

### RE4 XeFG

~~~text
same RE4XeSS producer
stock OptiScaler
FGInput=Upscaler
XeFG
    generated frames stable
    resize/fullscreen/Alt+Tab stable
~~~

---

## 18. Explicitly rejected designs

### 18.1 Do not use pd-upscaler as the production base

Historical pd-upscaler is an oracle only.

Do not merge/cherry-pick its broad branch history into production.

Do not add PDPerfPlugin.dll.

Its historical direct upscaled-output-to-backbuffer copy path is specifically not the desired architecture because the current project requires RE4's own Overlay/UI/presentation path to remain downstream of scene upscaling.

### 18.2 Do not create a separate bridge DLL/ASI

There is no production ATSBridge binary.

The bridge is a logical RE4 subsystem inside REFramework.

### 18.3 Do not implement a custom OptiScaler ABI

No REF-to-OptiScaler private export or protocol.

### 18.4 Do not implement an FG frontend in REFramework

RE4 uses only OptiScaler FGInput=Upscaler for the XeFG target scenario.

### 18.5 Do not directly replace the swapchain image with XeSS output

Capture 28-30b show a native non-copy final presentation stage.

The target architecture preserves that native downstream path.

### 18.6 Do not generalize RE4 offsets to other games

Every RE4-specific layout fix remains explicitly RE4-gated.

### 18.7 Do not reuse diagnostic capture machinery as production code

The diagnostic branch proved the contract.

Production code should implement the minimum stable contract, not carry broad hook/tracing infrastructure forward.

---

## 19. Frozen architectural decisions

| ID | Decision |
|---|---|
| AD-01 | Production implementation lives on the current custom REFramework master lineage, not pd-upscaler. |
| AD-02 | Runtime construction and behavior are RE4-only. |
| AD-03 | Existing XeFG compatibility code is a protected subsystem. |
| AD-04 | Public XeSS D3D12 is the only game-facing upscaler producer contract. |
| AD-05 | Stock OptiScaler must be able to intercept the same XeSS producer and substitute SR backends. |
| AD-06 | RE4 Frame Generation support is only OptiScaler FGInput=Upscaler -> XeFG. |
| AD-07 | REFramework never selects the OptiScaler SR backend. |
| AD-08 | Scene input is semantic HDR/PostMain Color + DepthStencilTex + VelocityTarget; RE4 Velocity RG is converted from R16G16B16A16_SNORM to bridge-owned R16G16_FLOAT before public XeSS execute. |
| AD-09 | Scene render size is controlled through SceneView; display size stays DXGI-owned. |
| AD-10 | XeSS work uses an REFramework-owned DIRECT list at true pre-Overlay. |
| AD-11 | Engine-owned command lists are never modified. |
| AD-12 | Original RE4 Velocity is transitioned for the bridge MV conversion, converted RG16F is consumed by XeSS, and original Velocity is restored to the verified RE4 state. |
| AD-13 | XeSS output must re-enter RE4 before its native UI/final presentation path. |
| AD-14 | Direct normal-path copy of XeSS output to the swapchain is rejected. |
| AD-15 | Output handoff uncertainty is isolated inside RE4XeSSOutputHandoff. |
| AD-16 | Existing global Debug Log controls detailed RE4 XeSS diagnostics. |
| AD-17 | Failure is fail-closed to native RE4 rendering. |
| AD-18 | RE4 UI exposes one Upscaling Mode selector: Off plus the public XeSS Native AA / UQ+ / UQ / Quality / Balanced / Performance / Ultra Performance presets. |
| AD-19 | Preset-to-render-size mapping is obtained through the public XeSS frontend query; REFramework does not hardcode backend-specific SR ratios. |
| AD-20 | Broad reverse-engineering tracing is finished; add new probes only for concrete implementation contradictions. |
| AD-21 | Every public XeSS API call is serialized on the stable true pre-Overlay owner thread; non-owner callbacks only request state transitions. |
| AD-22 | The RE4XeSSFrame remains callback-scoped CPU data, while submitted Color/Depth/original-Velocity resources are retained by slot-local COM pins until fence completion is proven. |
| AD-23 | `GetCompletedValue()==UINT64_MAX` is device removal, never completion; unprovable post-submit generations are quarantined rather than reused or freed speculatively. |
| AD-24 | The PR3 bridge fence proves only REF/XeSS writer completion. Engine-visible handoff TargetState release/recreation additionally requires a same-DIRECT-queue downstream-retirement fence signaled after Present, unless confirmed device removal terminally ends the old generation. |
| AD-25 | Normal handoff texture reuse relies on same-queue GPU ordering and does not CPU-wait every frame; teardown/recreation uses the downstream retirement proof. |
| AD-26 | Fresh cloned handoff state COMMON -> UAV -> 0xC0 and steady 0xC0 -> UAV -> 0xC0 are runtime validation gates that require D3D12 debug-layer evidence or a narrow handoff-resource state-provenance capture. |

---

## 20. Source of truth

Reverse-engineering evidence and capture history:

~~~text
doc/RE4_XESS_BRIDGE_ARCHITECTURE_AND_RE_STATUS_2026-09-26.md
~~~

That document is the source of truth for what was proven.

This production architecture document is the source of truth for what should now be built.

If implementation behavior contradicts the research evidence:

1. stop and record the exact contradiction;
2. do not silently weaken RE4-only isolation or bypass the public XeSS contract;
3. add the smallest targeted diagnostic necessary;
4. update both the evidence record and this architecture document before changing a frozen architectural decision.
