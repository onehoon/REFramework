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

REFramework does not distribute the XeSS runtime. Resolve the directory containing the loaded REFramework DLL, prefer <REF>\libxess.dll, then <REF>\OptiScaler\libxess.dll, and load the selected DLL by exact path. A missing first candidate is expected and must continue to the second candidate; only unexpected inspection errors or both candidates being absent are failures. Do not search arbitrary PATH/current-working-directory locations or mutate process-wide DLL search state.

XeSS-SR is not thread-safe.

The first PR4 runtime capture disproved the earlier assumption that the true RE4 pre-Overlay callback has a stable CPU thread identity: the callback moved from Windows thread 1304 to 28692 within the same active session.

Therefore the production XeSS owner is a dedicated RE4XeSS worker thread, not an RE Engine callback thread.

Context creation, version query, optimal-input query, initialization, velocity-scale setup, execute, and context destruction all run only on that dedicated worker thread.

RE4 callbacks may move between engine threads and only provide control state or synchronous work requests. They must not call public XeSS functions directly.

The semantic true pre-Overlay boundary remains the ordering point: the callback synchronously asks the worker to service/record/submit XeSS CPU work, receives the result, then installs the engine OutputHandoff on the callback thread. The synchronous dispatch waits for CPU recording/submission only; it does not wait for GPU fence completion.

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

RE4XeSSD3D12 is serialized by the same dedicated RE4XeSS worker that owns RE4XeSSRuntime. Alternating RE4 callback threads must not mutate bridge allocator/list/fence state directly.

The research campaign proves that an REFramework-owned DIRECT list can be ordered at the true pre-Overlay semantic boundary. The corrective worker design preserves that ordering by synchronously recording/submitting the bridge list before the callback installs the handoff and lets RE4 downstream work continue.

The implementation must rely on the semantic callback boundary, not on a hardcoded queue ordinal or callback thread ID.

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
- do **not** bind OutputHandoff restore/install to a fixed Windows callback thread ID;
- serialize OutputHandoff mutation through the single-entry semantic pre-Overlay coordinator instead;
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

The post-Present callback is not assumed to run on the dedicated XeSS worker or on any stable pre-Overlay callback thread. It pins and synchronizes access to its generation's queue/fence, then only Signals and publishes marker evidence. It performs no XeSS API call, Overlay/TargetState access, TargetState release, or fence wait.

OutputHandoff TargetState install/restore/retirement decisions remain on the semantic RE4 callback side. This callback side is serialized by a non-reentrant pre-Overlay coordinator gate, not by a fixed callback thread ID. A handoff installed on one valid pre-Overlay callback thread may be restored on a different valid pre-Overlay callback thread.

Bridge writer-idle/quarantine information comes from the worker result/snapshot; the callback side does not mutate RE4XeSSD3D12 directly.

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
WaitingForWorker
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

Control/device generations are independently monotonic. Every worker control/submit result is tagged with the exact `control_generation` and `device_reset_generation` it accepted. Before callback-side handoff installation or temporal-history commit, the coordinator reloads the current generations and rejects stale results.

If a stale result is detected **after** the worker already submitted GPU work:

~~~text
do not install Overlay handoff
do not commit temporal history
retain output through bridge writer completion
request output generation retirement
resume only from the newer generation
~~~

No downstream retirement marker is required for that stale output unless it was actually installed into RE4 and therefore acquired downstream readers.

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

### Current PR66 runtime gate — TargetState OutputHandoff

The production LoadAccessor blocker is resolved.

Runtime now proves the exact TDB71 access path:

~~~text
chainsaw.SceneLoadZoneManager
    get_Instance
    _Pause
    System.Boolean storage width = 1

chainsaw.GameSituationManager
    get_Instance
    <InhibitBit>k__BackingField
    System.UInt64 storage width = 8
~~~

The Capture 22 load-window semantics remain frozen and are now observed in production behavior: startup during Pause does not invent a baseline, Pause release alone does not resume history, and the current normal InhibitBit value is re-adopted dynamically after stable post-pause observations. The observed 0xB9 value must never be hardcoded.

The current blocker has moved to RE4XeSSOutputHandoff:

~~~text
first valid temporal packet                 proven
public XeSS runtime discovery/init           proven
stock OptiScaler XeSS frontend interception  proven through init
create_render_target_view                    proven
create_texture                               proven
live Overlay TargetState layout              proven
live Overlay TargetState slot +0x90          proven
live TargetState vtable                       proven for exact RE4 image
create_target_state                          unresolved
distinct single-RTV handoff TargetState      unavailable
xessD3D12Execute                             not reached
~~~

For the exact validated RE4 1.5.9.0 image, the live Overlay TargetState vtable is anchored at RVA 0x7B1C148. Current PR66 static xref discovery is diagnostic only and is not allowed to become a production factory resolver without runtime ABI/ownership proof.

The latest diagnostic run proves the live vtable exactly matches imageBase + 0x7B1C148, but the full executable-section decoder stops early and reports discoveryComplete=false. The current implementation incorrectly combines static scan completeness with live-anchor trust, preventing the bounded TargetState probe from arming.

The immediate diagnostic correction is:

~~~text
live-anchor trust:
    exact RE4 image identity
    + validated Overlay slot
    + readable live TargetState
    + exact live vtable match

static xref scan completeness:
    separate diagnostic evidence
    not a prerequisite for the bounded read-only probe
~~~

Production create_target_state remains fail-closed until an actual constructor/factory ABI and ownership contract are proven.

The public call stream remains the same and is serialized on the single dedicated RE4XeSS worker thread:

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

First-runtime corrective work order:

~~~text
doc/RE4_XESS_PR4_RUNTIME_BLOCKER_FIX_WORK_ORDER_2026-09-27.md
~~~

Next runtime blocker work order after PR65 worker/runtime success:

~~~text
doc/RE4_XESS_LOAD_STATE_ACCESSOR_DIAGNOSTIC_WORK_ORDER_2026-09-27.md
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
| AD-21 | Every public XeSS API call is serialized on one dedicated RE4XeSS worker thread. The true pre-Overlay callback remains the semantic ordering boundary but its CPU thread identity is explicitly not assumed stable. |
| AD-22 | The RE4XeSSFrame remains callback-scoped CPU data, while submitted Color/Depth/original-Velocity resources are retained by slot-local COM pins until fence completion is proven. |
| AD-23 | `GetCompletedValue()==UINT64_MAX` is device removal, never completion; unprovable post-submit generations are quarantined rather than reused or freed speculatively. |
| AD-24 | The PR3 bridge fence proves only REF/XeSS writer completion. Engine-visible handoff TargetState release/recreation additionally requires a same-DIRECT-queue downstream-retirement fence signaled after Present, unless confirmed device removal terminally ends the old generation. |
| AD-25 | Normal handoff texture reuse relies on same-queue GPU ordering and does not CPU-wait every frame; teardown/recreation uses the downstream retirement proof. |
| AD-26 | Fresh cloned handoff state COMMON -> UAV -> 0xC0 and steady 0xC0 -> UAV -> 0xC0 are runtime validation gates that require D3D12 debug-layer evidence or a narrow handoff-resource state-provenance capture. |
| AD-27 | PR4 first runtime evidence showed the true pre-Overlay callback move from thread 1304 to 28692; callback-thread migration is normal and must not quarantine XeSS. |
| AD-28 | XeSS runtime discovery tries <REF>\libxess.dll then <REF>\OptiScaler\libxess.dll; expected absence of the first candidate continues to the second instead of faulting. |
| AD-29 | RE4XeSSRuntime and RE4XeSSD3D12 share the dedicated worker CPU owner. OutputHandoff remains callback-side; synchronous worker dispatch preserves pre-Overlay ordering without a per-frame GPU wait. |
| AD-30 | RE4XeSSOutputHandoff is owned by the semantic pre-Overlay coordinator, not a fixed callback thread ID. Install on callback thread A and restore on callback thread B is valid when coordinator serialization and Overlay/generation invariants hold. |
| AD-31 | The true pre-Overlay coordinator is non-reentrant. Concurrent callback overlap fails closed without a second worker dispatch or callback-side temporal/handoff mutation, and marks the active pass stale before install/history commit. |
| AD-32 | Worker control/submit results are tagged with control/device-reset generations. Callback-side install/history commit requires a post-return generation recheck; stale-after-submit work is retired by writer lifetime rules without becoming visible. |
| AD-33 | RE4 LoadAccessor uses SceneLoadZoneManager._Pause plus GameSituationManager.<InhibitBit>k__BackingField with managed primitive storage widths; the normal InhibitBit value is learned dynamically and must never be hardcoded. |
| AD-34 | For the exact validated RE4 1.5.9.0 image, the live Overlay TargetState vtable RVA 0x7B1C148 is a diagnostic type anchor only; it is not itself a production creator/factory ABI. |
| AD-35 | Static TargetState-vtable xref scan completeness is diagnostic evidence and must not by itself veto a live anchor already validated by exact image identity, Overlay slot identity, readable layout, and exact vtable match. |
| AD-36 | RE4 create_target_state remains fail-closed until constructor/factory ABI, descriptor semantics, return object layout, and ownership/refcount contract are runtime-proven. Rejected candidates 0x44C7A27, 0x47212A6-as-fixed-factory, 0x78F42D0-as-usable-TargetState, and 0x4597A0-as-Overlay-writer must not be productionized. |
| AD-37 | DLSS5-Feeder externally corroborates that synthetic standard upscaler API calls can be intercepted by stock OptiScaler, but its post-process/backbuffer-copy architecture does not replace RE4's required pre-Overlay TargetState handoff. Public XeSS remains the RE4 producer contract. |
| AD-38 | Exact decoded TargetState-vtable xrefs at RE4 RVAs 0x47C3E0A and 0x47D21AF are reverse-engineering leads only. They must be classified to a concrete constructor/initializer dataflow and runtime-proven before any production creator/factory resolver is allowed. |
| AD-39 | Incomplete RUNTIME_FUNCTION xref coverage does not invalidate already decoded exact positive xrefs and does not veto a runtime live-anchor proven by exact image identity and vtable equality. Decoder failures are diagnostic coverage metadata and should be rate-limited in logs. |
| AD-40 | RE4 RVA 0x47C3E00 is destructor/deallocation-side evidence for the exact validated image and must not be used as a TargetState creator/factory path. |
| AD-41 | RE4 RVA 0x47D2180 is the strongest current TargetState creator candidate because it allocates 0xA8 bytes, invokes an initializer-like routine, installs the exact live TargetState vtable, and returns the object. It remains diagnostic-only until ABI, ownership, and live-object identity are runtime-proven. |
| AD-42 | The next creator proof is an early read-only observation of real engine calls to 0x47D2180 plus raw-pointer correlation against the later live Overlay main TargetState. No diagnostic probe may invoke the candidate function itself or retain engine objects through AddRef/Release. |
| AD-43 | Runtime observation promotes RE4 RVA 0x47D2180 from diagnostic candidate to the proven exact-image TargetState allocation/construction wrapper. It allocates 0xA8 bytes, consumes TargetState::Desc* through RDX, initializes the new object, installs the exact TargetState vtable, returns the allocation, and produces valid TargetState layouts with refCount=1. |
| AD-44 | Raw pointer identity between an early object created by 0x47D2180 and a later live Overlay TargetState is not required for production promotion. The prior correlation requirement is superseded because the wrapper has been observed creating multiple valid fresh TargetState instances, while the 16-entry probe exhausted before the live Overlay anchor existed. |
| AD-45 | shared/sdk/Renderer.cpp may resolve RE4 create_target_state to RVA 0x47D2180 only after exact image/function validation. The existing TargetState* (*)(void*, TargetState::Desc*) ABI and fn(nullptr, desc) call shape are compatible with the proven RE4 body. Any validation mismatch must return nullptr; non-RE4 resolver behavior remains unchanged. |
| AD-46 | RE4 TargetState objects produced by 0x47D2180 point Desc::rtvs at internal storage at object+0x68. Clone-side temporary RTV-array cleanup is a separate ownership task and must not be changed in the same commit that first enables the production resolver. |
| AD-47 | The validated RE4 create_target_state resolver at RVA 0x47D2180 is runtime-proven in the production path: TargetState cloning, display-resolution output creation, worker submission, Overlay installation, post-Overlay survival, and downstream retirement marking all succeed. TargetState discovery is no longer a production blocker. |
| AD-48 | A same-generation OutputHandoff MissingMarker condition is a transient callback-ordering state, not a producer/context failure. While the real post-Present marker is pending, restore native Overlay state and skip new XeSS submit/install; preserve the worker/context/control generation and reset temporal history for the next accepted frame. Never synthesize the downstream marker early. |
| AD-49 | An exception escaping the public xessD3D12Execute path leaves external runtime and command-recording state uncertain. Preserve fail-closed quarantine and do not continue/submit the interrupted command list. Add bounded diagnostics at the exact public API boundary before considering recovery. |
| AD-50 | The completed TargetState vtable/xref/creator probes are no longer part of normal production startup. Keep the exact-image resolver validation, but place heavy historical discovery probes behind explicit diagnostic opt-in or remove them from the normal RE4 XeSS path. |
| AD-51 | Execute-boundary api-enter/api-return diagnostics prove the current terminal failure escapes directly from public xessD3D12Execute before REFramework records restore barriers, closes, submits, or signals the command list. Those post-call D3D12 stages are not the source of the captured terminal exception. |
| AD-52 | At OptiScaler 44cfee4d, the final paired log line "XeSSFeatureDx12::EvaluateInternal Executing!!" is immediately before XeSSProxy::D3D12Execute. Until exception address/module evidence exists, treat the fault as inside the intercepted/native XeSS backend boundary without assigning ownership to OptiScaler, Intel XeSS, or the driver. |
| AD-53 | For the next failure capture, use only a narrow observation mechanism around m_functions.d3d12_execute that records the native exception code/address/module/RVA and continues exception search. Do not add process-wide VEH, worker-wide SEH translation, recovery, or retry. |
| AD-54 | CreateRenderTargetViewProbe is no longer required in the normal producer path. Its factory/format/handoff questions are already proven; remove it from normal execution or gate it behind explicit diagnostic opt-in while preserving the actual RE4 RTV resolver. |
| AD-55 | The 16:10 capture identifies a native 0xC0000005 near-null write to address 0xC at game-directory OptiScaler dxgi.dll+0x21256f. This identifies the instruction module, not the underlying pointer origin or correct fix. Symbolize the exact OptiScaler binary before changing SR integration behavior. |
| AD-56 | The completed narrow SEH observation retains EXCEPTION_CONTINUE_SEARCH and terminal worker/bridge/output/runtime quarantine. Debugging may obtain first-chance call stacks with a debugger or optionally copy bounded GPRs as POD in that same filter; do not add broad handlers or same-process retry. |
| AD-57 | The production runtime no longer arms the completed CreateRenderTargetViewProbe in its normal RE4 XeSS path. Preserve the actual validated RTV factory resolver and OutputHandoff while locating the fault. |
| AD-58 | A same-Overlay third-TargetState identity encountered during OutputHandoff restore is a terminal ownership uncertainty, not permission to overwrite the observed state with the saved original. Preserve old output generation pins and downstream lifetime constraints; first capture pointer identities and marker state. |
| AD-59 | Quality-change ownership diagnosis must precede any recovery policy. Mode selection increments the control generation but does not establish which component wrote Overlay main TargetState. Capture bounded transition/first-failure evidence and suppress duplicate terminal errors without lifting quarantine. |
| AD-60 | A repeatable native write AV at game-directory OptiScaler dxgi.dll+0x21256f, with first-chance RAX=0 and write target=0xc, requires disassembly of that exact binary and pointer-producer provenance before any semantic fix. Registers alone do not prove the instruction addressing mode or owning component. |
| AD-61 | Treat 128 ordinary Execute api-enter/api-return logs as the deliberate MAX_EXECUTE_API_LOGS budget, not the total submission count; terminal submission=205 and 205 paired OptiScaler XeSS intercepts are separate evidence. Keep the narrow SEH filter and fail-closed quarantine intact. |
| AD-62 | Post-fault quality requests do not validate healthy in-flight quality transition. Verify the same-Overlay third-TargetState mode-change issue separately from the exact-binary native OptiScaler AV. |

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


---

## 21. PR66 TargetState handoff checkpoint — 2026-09-28

This checkpoint is the current production handoff state and should be read together with the full evidence record in RE4_XESS_BRIDGE_ARCHITECTURE_AND_RE_STATUS_2026-09-26.md.

### Proven and frozen

~~~text
LoadAccessor                               ✅
Capture 22 load semantics                 ✅
first valid resetHistory packet           ✅
dedicated XeSS worker/runtime ownership   ✅
public XeSS -> stock OptiScaler init      ✅
RTV factory                               ✅
create_texture                             ✅
live Overlay TargetState layout           ✅
Overlay main TargetState slot +0x90       ✅
live TargetState vtable RVA 0x7B1C148     ✅ exact validated RE4 image only
~~~

### Rejected/retired diagnostics

~~~text
legacy generic create_target_state path   rejected for RE4
0x44C7A27                                 rejected as TargetState factory
0x47212A6                                 polymorphic vcall site, not fixed factory
0x78F42D0 return                          rejected as usable live TargetState
0x4597A0                                  retired as Overlay TargetState writer
~~~

### Current production blocker

~~~text
actual TargetState creator/factory ABI    unknown
distinct single-RTV handoff state         unavailable
OutputHandoff install                     blocked
Worker XeSS submit                        blocked before submit
OptiScaler xessD3D12Execute               0 calls
~~~

### Current diagnostic blocker

Latest runtime proves:

~~~text
expected TargetState vtable = live TargetState vtable
match = true
~~~

but current static xref discovery stops on undecodable executable ranges:

~~~text
xrefCount = 0
complete = false
~~~

and the current code incorrectly derives:

~~~text
trusted = match && discoveryComplete
~~~

so the bounded TargetState probe does not arm.

PR66 follow-up comment 5863592995 defines the next correction:

~~~text
decouple live-anchor trust from full static-xref scan completeness
repair xref discovery independently
keep all production handoff paths fail-closed
~~~

The xref scanner should prefer per-RUNTIME_FUNCTION decode or candidate-driven exact RIP-relative validation. Do not blind-resynchronize bytes merely to force full coverage.

### Next acceptance

The next runtime capture must first show:

~~~text
[TargetStateVtableProbe] live-anchor ... match=true ... trusted=true
[TargetStateProbe] armed ...
~~~

Then continue only the smallest diagnostic needed to identify the actual TargetState allocation/constructor path.

Do not:

- merge PR66 yet;
- bypass the handoff with a direct swapchain copy;
- start XeFG validation;
- reintroduce retired writer/factory candidates;
- convert a static xref directly into a production call;
- weaken RE4-only image/callsite validation.

### XeSS teardown note

The current xessDestroyContext result -8 is XESS_RESULT_ERROR_INVALID_CONTEXT. In the current stock OptiScaler path this is expected before the first Execute/CreateDLSSContext creates its internal context entry. Because the current capture never reaches xessD3D12Execute, this is a downstream teardown symptom, not the primary blocker.

### DLSS5-Feeder comparison

DLSS5-Feeder demonstrates that a synthetic public upscaler producer can feed stock OptiScaler successfully. It does so at a post-process/ReShade-style boundary and can copy the result to the finished backbuffer, so it does not need an RE Engine TargetState.

That does not alter this architecture.

RE4 still requires:

~~~text
low-resolution SceneView
    -> pre-Overlay semantic temporal inputs
    -> public XeSS
    -> display-resolution output
    -> engine-visible TargetState
    -> native Overlay/UI/final-output path
~~~

Public XeSS remains the standard producer contract.


---

## 22. PR66 latest TargetState xref checkpoint — 2026-09-28 13:47 build

This section supersedes the diagnostic-blocker state recorded in Section 21.

Latest local runtime:

~~~text
re2_framework_log(20260928-045315).txt
commit header: a72333a1a9944c156bf947e43cfab9410877867c
branch: feature/re4-xess-load-state-accessor-diagnostic
~~~

### 22.1 Previous live-anchor trust blocker is closed

The runtime now reports:

~~~text
liveVtable=expectedVtable
imageIdentityValid=true
match=true
discoveryComplete=false
trusted=true
~~~

and the bounded TargetStateProbe arms successfully.

Therefore static xref scan completeness remains separate from live-anchor trust.

### 22.2 Vtable discovery now has two positive leads

The RUNTIME_FUNCTION-based scan reports:

~~~text
functions=508868
scannedFunctions=507568
failedFunctions=1300
xrefCount=2
complete=false
~~~

Exact decoded references:

~~~text
0x47C3E0A:
    LEA rax, [rel imageBase+0x7B1C148]

0x47D21AF:
    LEA rax, [rel imageBase+0x7B1C148]
~~~

The scan is not exhaustive, but these two references are positive decoded evidence.

Neither RVA is a production factory by itself.

### 22.3 Old dynamic provider probe is complete

The existing site-1 probe again reaches re4+0x78F42D0 and reproduces:

~~~text
numRtv=1
rtv0=null
overlayVtableMatch=false
targetStateLike=false
~~~

Do not continue spending captures on the same provider-return path.

### 22.4 Next diagnostic boundary

PR66 work-order comment:

~~~text
5863746550
~~~

Next step:

~~~text
for 0x47C3E0A and 0x47D21AF only
    -> record containing RUNTIME_FUNCTION Begin/End
    -> decode bounded local instruction context
    -> trace the LEA-loaded vtable address
    -> determine whether the site is constructor, initializer,
       destructor, overload, or type-query helper
    -> choose one strongest candidate
    -> only then add a bounded read-only dynamic probe
~~~

Do not dynamically hook both candidates before static context classification.

Do not call either candidate directly.

### 22.5 Remote/local implementation handoff note

The runtime includes the RUNTIME_FUNCTION scanner and decoupled trusted=true behavior, but the currently visible remote source at the stamped implementation commit does not fully reflect those local changes.

Before continuing, preserve and push the exact local implementation that produced this runtime so the PR source and runtime evidence converge.

### 22.6 Production blocker remains unchanged

~~~text
actual TargetState creator/factory ABI    unknown
distinct single-RTV handoff TargetState   unavailable
OutputHandoff install                     blocked
Worker XeSS submit                        blocked before Execute
OptiScaler xessD3D12Execute               0 calls
~~~

No architecture change is justified.

Keep:

~~~text
public XeSS producer
    -> stock OptiScaler interception
    -> selected SR backend
    -> RE4 engine-visible TargetState handoff
    -> native Overlay/UI/final output
~~~

Do not direct-copy XeSS output to the swapchain.

Keep PR66 Draft and unmerged.


---

## 23. PR66 TargetState creator checkpoint — 2026-09-28 14:05 build

This checkpoint supersedes the unresolved two-xref classification state in Section 22.

Latest runtime:

~~~text
re2_framework_log(20260928-050834).txt
commit header: 42c3b66fd5163c331ecb1247e42b25cfc13e9d40
branch: feature/re4-xess-load-state-accessor-diagnostic
~~~

Current remote source contains the bounded context diagnostic in:

~~~text
06a50b2572a19ba0f4ff977babd2a786273a8bfe
Log bounded TargetState vtable xref context
~~~

### 23.1 Scanner and trust-gate diagnostics are no longer blockers

The scanner now:

~~~text
uses exception-directory RUNTIME_FUNCTION ranges
retains the two exact positive vtable xrefs
rate-limits individual decode failures to 12
reports complete=false separately from positive xrefs
~~~

The live Overlay anchor remains:

~~~text
imageIdentityValid=true
liveVtable == expectedVtable
match=true
trusted=true
~~~

No further work is required on this trust gate unless new runtime evidence contradicts it.

### 23.2 0x47C3E00 is not a creator path

The 0x47C3E00 function:

~~~text
writes the TargetState vtable onto an existing RCX object
preserves EDX as flag bits
performs destructor-side work
conditionally performs a deallocation-like call
returns the existing object pointer
~~~

Treat it as deleting-destructor / ownership-side evidence only.

Do not use it for create_target_state.

### 23.3 0x47D2180 is the sole current creator candidate

The 0x47D2180 function:

~~~text
requests 0xA8 bytes
receives a new object pointer
invokes re4+0x446E790 on that object
writes the exact TargetState vtable to [RBX]
returns the same object in RAX
~~~

This is the strongest candidate yet for the engine TargetState allocation/construction path.

However, production integration still requires proof of:

~~~text
real engine caller(s)
input argument semantics
constructed object layout
initial refcount / ownership transfer
relationship to the live Overlay TargetState instance
safe descriptor/RTV population contract
~~~

Therefore:

~~~text
0x47D2180 = diagnostic creator candidate
0x47D2180 != production create_target_state yet
~~~

### 23.4 Next implementation boundary

PR66 comment:

~~~text
5863904529
~~~

Add one early, bounded, read-only creator probe for 0x47D2180 only.

Arm before plugin/renderer initialization at the existing post-integrity bootstrap point.

Observe real engine calls and retain only plain diagnostic values.

Do not AddRef, Release, call, modify, or retain engine objects.

The strongest acceptance condition is:

~~~text
createdObjectFrom0x47D2180 == laterLiveOverlayMainTargetState
~~~

Also collect exact direct callers of 0x47D2180 from the already-bounded RUNTIME_FUNCTION decode so argument setup can be reconstructed.

The old 0x47212A6 provider-return diagnostic should be disabled for the next capture because it is exhausted.

### 23.5 Production architecture is unchanged

The desired path remains:

~~~text
RE4 low-resolution scene
    -> true pre-Overlay semantic inputs
    -> public XeSS D3D12 producer
    -> stock OptiScaler interception
    -> display-resolution SR output
    -> engine-visible TargetState handoff
    -> native RE4 Overlay/UI/final presentation
~~~

Current blocker:

~~~text
production-safe TargetState creator ABI   not yet proven
distinct handoff TargetState              unavailable
OutputHandoff                             blocked
XeSS Execute                              not reached
~~~

Do not bypass the handoff with a swapchain copy.

Do not start XeFG validation.

Keep PR66 Draft and unmerged.


---

## 24. PR66 TargetState factory promotion checkpoint — 2026-09-28 14:34 build

This section supersedes the diagnostic-only status in Section 23.

Latest runtime:

~~~text
re2_framework_log(20260928-054035).txt
stamped commit: f99742e77f8b3df4db81bec639110183b967296e
branch: feature/re4-xess-load-state-accessor-diagnostic
~~~

Current remote implementation:

~~~text
27fa92f75b1b4da985749bcd3a2b729b75cf0d9f
Add bounded RE4 TargetState creator probe
~~~

### 24.1 0x47D2180 is now proven as the generic RE4 TargetState creator

Runtime observed real engine calls producing multiple fresh TargetState objects.

Single-RTV examples have:

~~~text
vtable       exact TargetState vtable
refCount     1
numRtv       1
valid RTV    yes
valid Texture yes
~~~

The same wrapper also creates multi-RTV states with the same vtable and valid resource graph.

Therefore:

~~~text
0x47D2180 = proven RE4 TargetState allocation/construction wrapper
~~~

for the exact validated image.

This is no longer only a reverse-engineering lead.

### 24.2 Production ABI is established

The existing SDK factory type is:

~~~cpp
TargetState* (*)(void*, TargetState::Desc*)
~~~

and the call site already uses:

~~~cpp
fn(nullptr, desc)
~~~

The exact RE4 wrapper:

~~~text
preserves incoming RDX as Desc*
does not consume incoming RCX
allocates 0xA8 bytes
passes RCX=new object / RDX=Desc* to re4+0x446E790
installs the TargetState vtable
returns the new object
~~~

Therefore the existing SDK ABI is compatible.

The next change is no longer another creator probe.

It is the exact-image RE4 resolver in shared/sdk/Renderer.cpp.

### 24.3 Previous Overlay pointer-correlation requirement is retired as a gate

The diagnostic stored only the first 16 creator observations.

Those 16 entries were exhausted during initialization and the hooks disarmed before the first live Overlay TargetState was available.

Later matchedObservation=false results therefore did not observe the relevant creation interval.

More importantly, a generic factory does not need to be proven by showing that one arbitrary early-created instance later becomes the Overlay main state.

The direct creation evidence is stronger and sufficient for promotion.

Do not increase the capture budget merely to chase pointer identity.

### 24.4 Exact-image resolver requirements

PR66 implementation comment:

~~~text
5864220846
~~~

For RE4 only, shared/sdk/Renderer.cpp::create_target_state() may resolve:

~~~text
RVA                    0x47D2180
RUNTIME_FUNCTION       0x47D2180..0x47D21D2
TargetState vtable RVA 0x7B1C148
image size             0x0E405000
PE checksum            0x0DEE3479
~~~

The implementation must validate the proven function bytes and relative call targets before returning the function pointer.

Fail closed:

~~~text
validation mismatch -> nullptr
~~~

Keep non-RE4 behavior unchanged.

Do not make shared/sdk depend on the RE4XeSS mod.

### 24.5 Ownership remains a separate safety gate

Captured objects consistently show:

~~~text
TargetState::Desc::rtvs == object + 0x68
~~~

inside the 0xA8 allocation.

This proves the final object does not retain the caller's RTV-array pointer as its own array location.

The current TargetState::clone() success path does not release its temporary cloned_desc.rtvs allocation.

That may need an RE4-specific cleanup once AddRef/copy ownership is proven.

Do not combine that cleanup with the first factory-resolver commit.

First validate factory resolution and OutputHandoff with the existing conservative ownership behavior.

### 24.6 Next runtime acceptance

The next capture should prove:

~~~text
RE4 create_target_state resolver validates
TargetState::clone returns a distinct valid state
single cloned RTV remains valid
OutputHandoff prepare succeeds
OutputHandoff install succeeds
worker reaches public XeSS Execute
stock OptiScaler observes xessD3D12Execute
post-present retirement remains safe
~~~

If clone succeeds and a later stage fails, that newly exposed stage becomes the next blocker.

Do not start XeFG validation before this SR/output-handoff path is stable.

Keep PR66 Draft and unmerged.


---

## 25. PR66 live production-path checkpoint — 2026-09-28 14:57 build

This section supersedes the pre-execution production state in Section 24.

Paired runtime:

~~~text
REFramework:
    re2_framework_log(20260928-055958).txt
    commit 088fdbbedff9bf59bcf56618e725fefc1a07a5da
    branch feature/re4-xess-load-state-accessor-diagnostic

OptiScaler:
    OptiScaler(1).log
    v10.0.0-dev
    commit 44cfee4d
~~~

Current remote PR66 head at analysis time:

~~~text
1d9bb7774d9c05c33bce45842d1e8cf225638c4d
~~~

PR66 stabilization work order:

~~~text
5864421040
~~~

### 25.1 The production bridge contract is now demonstrated

The exact RE4 resolver validates:

~~~text
create_target_state -> re4+0x47D2180
~~~

and runtime successfully performs:

~~~text
render-resolution semantic TargetState
    -> display-resolution TargetState clone
    -> worker public XeSS Execute
    -> stock OptiScaler interception/evaluation
    -> output resource returns shader-readable
    -> cloned TargetState installed into Overlay main
    -> native Overlay consumes it
    -> post-Present retirement marker proves downstream completion
~~~

This is the first capture that reaches the intended production architecture end-to-end.

The required architecture remains unchanged:

~~~text
RE4 low-resolution scene
    -> true pre-Overlay semantic inputs
    -> public XeSS D3D12
    -> stock OptiScaler interception
    -> selected SR implementation
    -> display-resolution engine-visible TargetState
    -> native Overlay/UI/final output
~~~

No swapchain-copy bypass is justified.

No private OptiScaler ABI is needed.

### 25.2 Production TargetState creation is no longer a blocker

The capture proves:

~~~text
validated exact-image resolver       PASS
TargetState::clone                   PASS
distinct display-resolution resource PASS
single-RTV output state              PASS
typed UAV-capable output             PASS
Overlay installation                PASS
post-Overlay state identity          PASS
~~~

The TargetState factory research/probe phase is complete for this exact RE4 image.

Do not require live-pointer correlation or vtable-xref rescanning for production use.

The exact-image validation in shared/sdk/Renderer.cpp remains the fail-closed gate.

### 25.3 OutputHandoff marker ordering must tolerate the real callback schedule

The live game can invoke the next pre-Overlay callback before the previous frame's post-Present callback has signaled the downstream retirement marker.

Therefore the lifecycle must distinguish:

~~~text
lifetime proof pending
~~~

from:

~~~text
lifetime proof failed
~~~

Frozen rule:

~~~text
MissingMarker for the same generation:
    restore native Overlay state
    do not reuse/submit/install the handoff
    do not wait
    do not teardown the XeSS runtime
    do not advance/recreate the control generation
    invalidate temporal history
    resume after the real post-Present marker settles
~~~

The next accepted producer frame must use:

~~~text
resetHistory=true
~~~

because a temporal producer frame was skipped.

The post-Present marker remains mandatory and must not be moved earlier.

### 25.4 Current primary blocker is sustained execute stability

After repeated successful frames, the worker observes an unknown exception.

The paired OptiScaler timestamp places the last visible external stage at:

~~~text
hk_xessD3D12Execute
    -> XeSSFeatureDx12::EvaluateInternal
        -> all required resources present
        -> Executing!!
~~~

followed immediately by:

~~~text
RE4XeSS worker terminated an operation after an unhandled exception
~~~

This establishes the next investigation boundary, but not the root cause.

Current production policy:

~~~text
exception escaping public XeSS call
    -> command recording state uncertain
    -> bridge generation uncertain
    -> output generation uncertain
    -> quarantine all affected state
    -> fail closed
    -> no automatic same-context retry
~~~

Do not interpret this capture as proving an OptiScaler backend defect, XeSS library defect, or REFramework D3D12 defect until the exact call boundary is instrumented.

### 25.5 Required execute-boundary diagnostics

Instrument only the smallest boundary around:

~~~cpp
RE4XeSSRuntime::execute()
    -> m_functions.d3d12_execute(...)
~~~

Required bounded observations:

~~~text
api-enter
    frame id
    bridge slot
    submit sequence
    worker thread
    control generation
    device-reset generation
    command-list identity
    Color / Depth / converted-MV / Output identities
    input/output extents
    resetHistory

api-return
    XeSS result

api-threw
    std::exception message when available
    otherwise unknown-exception classification
    safe exception/SEH code only if the existing exception model exposes it
~~~

Do not add a broad process-wide SEH translator.

Do not close/submit the interrupted command list after a thrown public call.

The existing outer worker quarantine remains the final safety net.

### 25.6 Completed reverse-engineering probes should leave the normal path

The runtime no longer requires:

~~~text
508k-function TargetState vtable discovery scan
early creator hooks
old provider-return hooks
~~~

for factory resolution.

Production should retain:

~~~text
exact executable identity
exact create_target_state function-range validation
exact byte/call-target/vtable validation
~~~

and remove or explicitly opt-in the historical discovery probes.

This reduces startup cost and runtime perturbation without weakening the production resolver.

### 25.7 Deferred ownership cleanup remains separate

The temporary clone-side Desc::rtvs allocation remains a possible cleanup target because the constructed RE4 TargetState points its final RTV list at object+0x68.

However, memory ownership cleanup is not the current stability blocker.

Do not mix it with:

~~~text
MissingMarker severity correction
execute-boundary exception diagnostics
~~~

Prove those first.

### 25.8 Acceptance before XeFG work resumes

Before advancing to XeFG validation, require a sustained SR run that demonstrates:

~~~text
create_target_state resolver remains stable
TargetState/output generation is reused safely
normal delayed markers do not recreate the XeSS context
skipped marker-wait frames resume with resetHistory=true
public XeSS/OptiScaler execution continues without terminal quarantine
post-Present retirement remains correct
mode changes drain/recreate cleanly
~~~

Until then:

~~~text
SR producer architecture     proven
SR sustained stability       not yet proven
XeFG production validation   deferred
~~~

Keep PR66 Draft and unmerged.


---

## 26. PR66 exact execute-boundary checkpoint — 2026-09-28 15:17 build

This checkpoint supersedes the broader exception boundary in Section 25.

Paired runtime:

~~~text
REFramework:
    re2_framework_log(20260928-062039).txt
    stamped commit 23550b91e80c4361e733788a8610ac74c4fbfa9a
    branch feature/re4-xess-load-state-accessor-diagnostic

OptiScaler:
    OptiScaler(2).log
    v10.0.0-dev
    commit 44cfee4d
~~~

Current remote PR66 head at analysis time:

~~~text
5cd612303ee9e9efe778f4cf47913fde66aa7ff9
Handle delayed XeSS retirement and execute faults
~~~

Follow-up work order:

~~~text
5864651645
~~~

### 26.1 Delayed-marker handling is no longer the current blocker

Observed delayed markers now settle without the prior context-unavailable cascade.

The runtime shows:

~~~text
restored previous handoff before marker
real post-Present marker arrives
same control generation continues
next Execute succeeds
handoff resumes
~~~

No output-handoff-unavailable or producer-context-unavailable reset appears in this capture.

The explicit WaitingForPostPresentMarker branch remains required for a longer delay, although this particular run settled the marker before prepare had to return that status.

Production rule remains:

~~~text
never reuse before downstream proof
never synthesize the marker
never wait for it on CPU/GPU
do not recreate the producer merely because the same-generation marker is late
~~~

### 26.2 Public execute failure location is now proven

The added API boundary diagnostic records normal executions as:

~~~text
api-enter
api-return result=0
Execute result=SUCCESS
~~~

At the terminal failure:

~~~text
frame=6216
submission=201
slot=0
input=853x480
output=2560x1440
resetHistory=false
api-enter
api-exception kind=unknown
~~~

with no api-return.

Therefore:

~~~text
exception location:
    inside m_functions.d3d12_execute(...)

excluded after-call stages:
    velocity restore barrier
    output finish barrier
    command-list close
    ExecuteCommandLists
    queue Signal
~~~

The bridge/output/runtime quarantine remains mandatory because the interrupted external call may have partially modified command-list or backend state.

### 26.3 OptiScaler source correlation narrows the external call chain

The paired OptiScaler log reaches:

~~~text
hk_xessD3D12Execute
NVSDK_NGX_D3D12_EvaluateFeature
XeSSFeatureDx12::EvaluateInternal
all required resources present
AutoExposure enabled
Executing!!
~~~

At OptiScaler 44cfee4d, Executing!! is emitted immediately before:

~~~cpp
xessResult = XeSSProxy::D3D12Execute()(_xessContext, InCommandList, &params);
~~~

There is no normal xessD3D12Execute error-result log afterward.

Current evidence therefore supports only:

~~~text
exception escaped during the intercepted/native XeSS backend call
~~~

It does not yet identify the owning binary.

Do not change producer resource states, output lifetime, or worker ownership based only on this evidence.

### 26.4 Failure recurrence is approximately time/count stable across quality modes

Earlier capture:

~~~text
Ultra Quality
1969x1107
first Execute 14:59:22.858
failure       14:59:24.915
~2.06 seconds
~~~

Current capture:

~~~text
Ultra Performance
853x480
first Execute 15:20:04.148
failure       15:20:06.266
~2.12 seconds
submission 201
~~~

This strongly suggests a repeatable time/submission boundary rather than a single-quality input-size failure.

Treat it as evidence for diagnostics, not as a workaround condition.

### 26.5 Next implementation boundary is exception ownership, not rendering behavior

The next patch should add a narrow Windows/MSVC observation filter only around the external function-pointer invocation.

Collect:

~~~text
exception code
ExceptionAddress
ContextRecord RIP
module name/path
module RVA
ExceptionFlags
NumberParameters
~~~

For access violation also collect:

~~~text
read/write/execute kind
faulting virtual address
~~~

The observation filter must:

~~~text
copy POD only
avoid logging/allocating inside the filter
return EXCEPTION_CONTINUE_SEARCH
preserve the current outer catch/quarantine path
~~~

Do not install process-wide handlers.

Do not translate the exception into a recoverable result.

Do not retry or recreate the backend automatically.

### 26.6 Remove the remaining RTV diagnostic hook from production execution

The completed TargetState diagnostics are gone from normal startup.

CreateRenderTargetViewProbe remains active, but is no longer required for the production contract.

Remove or opt-in gate:

~~~cpp
CreateRenderTargetViewProbe::instance().ensure(...)
~~~

and its unused normal lifecycle plumbing.

Preserve:

~~~text
exact RE4 create_render_target_view resolver
TargetState clone behavior
OutputHandoff validation
~~~

This removes a diagnostic hook before the next native exception capture.

### 26.7 Acceptance before any recovery or XeFG work

The next run should attempt to pass:

~~~text
submission 201
submission 250
submission 300
~~~

without changing the producer architecture.

If the exception recurs, the capture must identify:

~~~text
exception code
faulting instruction
owning module
module RVA
AV access kind/address when applicable
~~~

Only then decide whether the next code change belongs to:

~~~text
REFramework
OptiScaler
native Intel XeSS
driver-facing integration
~~~

Current status:

~~~text
SR architecture                 proven
delayed-marker context churn    no longer reproducing
execute exception call boundary proven
exception ownership             unknown
sustained SR stability          blocked
XeFG validation                 deferred
~~~

Keep PR66 Draft and unmerged.


---

## 27. PR66 OptiScaler-proxy native fault checkpoint — 2026-09-28 16:10

This section supersedes Section 26's statement that the faulting binary is
unidentified. Previous call-boundary and retirement evidence remains valid.

Paired logs:

~~~text
REFramework: re2_framework_log(20260928-071122).txt
    reported stamp fa05355903a1b9e9e16b5e45c5c28feb0512a96b
    branch feature/re4-xess-load-state-accessor-diagnostic
    build time 2026-09-28 15:37

OptiScaler: OptiScaler(3).log
    v10.0.0-dev / 44cfee4d / build 20260926_090919
    OptiScaler working as game-directory dxgi.dll
~~~

PR66 HEAD at checkpoint:

~~~text
75c93f2374254e7b91d59df73a78d6ad41ded9d1
Capture native XeSS execute exceptions
~~~

The log records the new native-exception diagnostics present in the current
PR HEAD even though the stamped SHA is older. The logged SHA alone must not be
treated as proof of the exact built source.

PR66 action comment:

~~~text
5865230623
~~~

### 27.1 Exception location is now concrete

The terminal failure:

~~~text
frame=8343
submission=200
slot=7
controlGeneration=1
resetGeneration=0
input=853x480
output=2560x1440
code=0xc0000005
ExceptionAddress=0x7ffda8e0256f
Rip=0x7ffda8e0256f
module=E:\SteamLibrary\steamapps\common\RESIDENT EVIL 4  BIOHAZARD RE4\dxgi.dll
moduleRVA=0x21256f
ExceptionFlags=0x0
NumberParameters=2
accessKind=write
targetAddress=0xc
~~~

is a Windows access violation writing to a near-null address.

This identifies the faulting instruction as belonging to OptiScaler's
game-directory DXGI proxy, not the system DXGI or native libxess.dll image.

A faulting instruction inside that module is **not**, by itself, proof that
OptiScaler created the invalid pointer. The source, caller chain, and object
state must be identified before making a defect/fix attribution.

### 27.2 Preserve the public interception contract

The external path remains:

~~~text
REFramework
    -> public xessD3D12Execute
        -> stock OptiScaler hk_xessD3D12Execute
            -> NVSDK_NGX_D3D12_EvaluateFeature
                -> XeSSFeatureDx12::EvaluateInternal
                    -> XeSSProxy::D3D12Execute / callbacks
~~~

The OptiScaler trace reaches Evaluating/Executing!! immediately before the
captured AV, but only the faulting instruction's module is proven.

Do not assume that the native Intel XeSS library itself faulted.

Do not introduce private OptiScaler exports, a custom XeSS ABI, direct XeFG
frontend calls, or a swapchain-copy shortcut.

### 27.3 This is a recurring, not yet explained, ~2.1-second boundary

~~~text
Capture 1: first success -> failure ~2.06s (Ultra Quality, 1969x1107)
Capture 2: first call -> failure ~2.12s (Ultra Performance, 853x480), submit 201
Capture 3: first call -> failure ~2.11s (Ultra Performance, 853x480), submit 200
~~~

Treat the similarity as a valuable test condition.

Neither exactly 200 submissions nor a literal timer or frame-count switch has
been proven causal.

### 27.4 The PR66 fix is no longer blind REFramework execute-path modification

Current requirements:

~~~text
1. symbolize/disassemble the exact OptiScaler 44cfee4d dxgi.dll
   at RVA 0x21256f;
2. identify faulting function/instruction and caller chain;
3. identify register/pointer that produces write to 0xC;
4. determine whether the invalid state originates inside the proxy,
   comes from XeSS callback/interception, or was passed from the producer;
5. only then make the smallest targeted source change in the correct owner;
6. run a sustained >300 XeSS-submit validation.
~~~

A first-chance debugger dump taken before outer C++ unwinding is preferable to
broad additional runtime hooks.

If debugger/symbol access is unavailable, an optional bounded diagnostic may
copy the integer registers in ContextRecord as POD through the existing
narrow SEH observation filter and log later in the existing catch.

No process-wide VEH, generic exception translator, stack unwind inside the SEH
filter, false-success return, unsafe retry, or automatic context recreation.

### 27.5 Already-correct paths must remain unchanged

~~~text
validated RE4 TargetState creator                 PASS
distinct display-resolution TargetState clone    PASS
post-Overlay handoff                              PASS
same-generation delayed-marker settlement        OBSERVED
CreateRenderTargetViewProbe normal-path removal  VERIFIED
public XeSS normal result=0 for early submissions OBSERVED
exception fail-closed quarantine                 PASS
~~~

The current log contains no old RTV, TargetState creator, or vtable-probe
capture records. The runtime still shows delayed-marker settlement and
continues using controlGeneration=1/resetGeneration=0 until the terminal AV.

Mode changes after the fault do not silently reactivate the uncertain
generation.

Keep intact:

~~~text
RE4-only isolation
public XeSS-only frontend
worker API ownership and resource pins
pre-overlay non-reentrant gate
generation checks
output retirement proofs
fail-closed native rendering fallback
existing XeFG/D3D12Hook subsystem
~~~

Defer:

~~~text
clone temporary RTV-array ownership cleanup
new resource-state or barrier changes
automatic exception recovery
XeFG production validation
~~~

### 27.6 Acceptance checkpoint

The next implementation decision depends on **exact function and pointer
provenance at OptiScaler dxgi.dll+0x21256f**, not on additional speculative
changes to REFramework.

Production status:

~~~text
Architecture implemented          YES
First end-to-end SR execution      PROVEN
Delayed marker context churn      NO LONGER REPRODUCES
Faulting module                   OptiScaler game-directory dxgi.dll
Exception                         0xC0000005 WRITE to 0xC
Faulting RVA                      0x21256f
Underlying pointer origin         UNKNOWN
Sustained SR stability            BLOCKED
XeFG production validation        DEFERRED
~~~

Keep PR66 Draft, Open, and unmerged.


---

## 28. PR66 quality-mode TargetState identity checkpoint — 2026-09-28 16:24

This checkpoint changes the next investigation priority, not the previous OptiScaler native AV evidence in Section 27.

Paired logs: REFramework re2_framework_log(20260928-072521).txt and OptiScaler(4).log (44cfee4d). REFramework is stamped fa05355903a1b9e9e16b5e45c5c28feb0512a96b, build 15:37. Remote PR66 HEAD at review: 425bcb7419619674a8d2fac405d155d1d89c1728. Exact stamp/source-tree identity remains unproven.

### 28.1 Quality-change restoration is a new blocker

The public XeSS API returned result=0 on all 118 observed submissions. Last successful frame: 7374 at 16:24:53.544 (Quality, input 1706x960, output 2560x1440).

At 16:24:53.552 the mode changed Quality -> Ultra Quality; at 16:24:53.599 OutputHandoff::restore() first reported a main TargetState that was neither its installed handoff nor saved original. No additional XeSS Execute followed. The same error was emitted 2,192 times through 16:25:07.881.

No native-exception or api-exception was observed in this capture. The prior OptiScaler dxgi.dll+0x21256f 0xC0000005 write-to-0xC remains independently unresolved; this run did not reach its prior ~200-submission window.

### 28.2 Source ordering and safety

In on_pre_overlay_layer_draw(), restore() executes before new mode-generation retirement and worker-control service. The unexpected TargetState branch is a same-layer identity mismatch; an Overlay layer generation change is already handled separately.

request_mode() changes the requested mode and increments the control generation; no code evidence here identifies the writer of the third TargetState.

Do not overwrite the third pointer with the saved state, clear installed bookkeeping, release old output pins, or invent post-Present completion. Preserve fail-closed quarantine until writer identity and downstream use are proven.

### 28.3 Immediate PR66 task

PR comment: 5865417657.

Capture only the first unexpected identity with: callback phase/thread/frame, requested mode, control/device generation, installed frame, current/installed Overlay layer pointers, observed/handoff/saved/template state pointers, installed/markerPending/retirementRequested/hardQuarantined flags and existing marker values.

Bracket last successful install, requested mode generation change, and first subsequent pre-Overlay restore. Avoid dereferencing an unknown pointer. Make repeated quarantine reporting idempotent while preserving failure semantics. Decide if the third state is a genuine engine replacement, stale saved baseline, or another writer only after seeing evidence.

### 28.4 Production gates

~~~text
Validated RE4 TargetState creator / normal OutputHandoff  proven
Public XeSS -> stock OptiScaler                         118 success returns
Delayed same-generation marker settlement              observed
Quality-change TargetState identity                     blocked
Third pointer / writer / safe retirement                unknown
Separate ~200-submit OptiScaler native AV               unresolved
Sustained SR stability / XeFG validation                deferred
~~~

Keep PR66 Draft/Open/unmerged. No speculative Intel SDK ABI, D3D12 barrier, or direct-swapchain changes are justified by this capture.


---

## 29. Exact-image native exception regression — 16:49 paired capture

The REFramework log re2_framework_log(20260928-075010).txt and
OptiScaler(5).log (44cfee4d / 20260926_090919) reproduce the previously
observed OptiScaler game-directory dxgi.dll write access violation at the
**same module-relative instruction RVA 0x21256f**.

Relevant PR66 follow-up comment: **5865784530**.
Review HEAD when posted: 0be6550ddc865dafc09fa3bbb0295a5c530203ed.
REFramework local runtime reports build 11:26 and hash
a9bd0b59cefde15317a774602b14a67548c2145e;
do not substitute remote HEAD for exact built sources.

### 29.1 Reproduced signature

~~~text
public XeSS first entered   16:49:29.124, submission 1, frame 7300
native exception            16:49:31.223, submission 205, frame 7505
input/output                1969x1107 -> 2560x1440
control/reset generation    1 / 0
exception code              0xc0000005
access                      WRITE 0xc
faulting module             <RE4 game directory>\dxgi.dll (OptiScaler proxy)
moduleRVA                   0x21256f
RIP                         0x7ffda6a4256f
first-chance RAX            0x0
RDX                         0x1f6f8945890
RCX                         0x5cf6782388b50000
R8                          0x0
R9                          0x35094ce0
R10                         0xec359125e7c0067
R11                         0x6919c17fb0
RSP                         0x6919c18200
RBP                         0x6919c18300
~~~

The 2.10-second interval and approximate 205 submissions are
repeatability conditions, not evidence of a literal timer/frame trigger.

**A null RAX and a write-to-0xc do not by themselves identify the
faulting assembly instruction.** Disassemble the exact binary to
determine its effective address operand and containing source function.

The REFramework normal-call telemetry uses MAX_EXECUTE_API_LOGS=128:
128 api-enter and 128 api-return result=0 are printed, then ordinary
per-call logging stops. The exceptional submission is still numbered
205. The paired OptiScaler log contains 205 hk_xessD3D12Execute and
202 XeSSFeatureDx12::EvaluateInternal Executing!! messages.
Do not report missing logs 129–204 as missing calls; do not report
205 completed successful GPU evaluations.

### 29.2 Separate blockers remain separate

Normal Overlay identities observed earlier in the same run:

~~~text
Overlay layer        0x1f4e5250800
handoff TargetState  0x1f4e4b453a0
saved original       0x1f4ccc98210
~~~

Delayed-marker settlements are observed at frames 7303, 7381 and 7460.

The new capture does **not** reproduce the Section 28 third-state
restore failure or the previous log flood. However, it does not
validate the healthy-generation Quality -> Ultra Quality transition:
Off -> Ultra Quality is the sole mode change before the AV;
all subsequent mode changes take place after terminal worker failure.

REF observes and logs the AV through its already implemented narrow
EXCEPTION_CONTINUE_SEARCH filter, then terminates the worker operation,
quarantines uncertain output lifetime and rejects automatic restart.
Post-fault snapshots report bridgeWriterUncertain=true and
retirementRequested=true. Preserve those fail-closed invariants.

### 29.3 Engineering decision gates

1. **Exact OptiScaler binary identification.** Inspect/hash the
   game-directory dxgi.dll used during this run (not the system copy).
   Record SHA-256, PE timestamp, image base/size, and exact build match.
2. **Instruction and pointer provenance.** Disassemble around
   0x21256f, recover function/callers from a matching PDB/map if
   available and capture first-chance stack/context before C++
   unwinding. Identify who produced the pointer and whether the
   issue originates within OptiScaler XeSS/NGX integration, a
   driver/interception callback, or a REF-provided argument.
3. **Smallest owner-correct patch.** Do not change REF GPU barriers,
   TargetState ownership, public XeSS ABI, or D3D12Hook/FG lifecycle
   merely because OptiScaler is the faulting instruction module.
4. **Independent validation.** Reproduce/test an active healthy
   Quality -> Ultra Quality switch for Section 28, and separately
   verify >300 continuous successful public XeSS submissions on the
   exact OptiScaler build after a justified native AV fix.

MenuCommon::Init re-entry around 16:49:31.204 precedes the exception
but is only a temporal observation, not a proven cause.

Status:

~~~text
TargetState clone / basic handoff                   VERIFIED
public XeSS -> OptiScaler SR                       VERIFIED up to failure
native exception module/RVA                        PROVEN
faulting register snapshot                         PROVEN
instruction / source function / pointer origin     UNKNOWN
healthy-generation quality transition              NOT TESTED IN THIS RUN
sustained SR (>300)                                 BLOCKED
XeFG production                                     DEFERRED
~~~

Do not add process-wide VEH, broad exception translation,
fake XeSS success, automatic context restart, or forced Overlay state
repair without independent ownership proof.

Keep PR66 Draft / Open / unmerged.
