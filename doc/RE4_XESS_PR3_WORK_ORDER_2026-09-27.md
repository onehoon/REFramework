# RE4 XeSS Production — PR 3 Work Order

Base branch: `feature/re4-xess`  
Production code baseline before PR 3 documentation: `1b59aa05a63586ec62137a5ab48f03ebbb7bf730`  
Implementation must branch from the latest `feature/re4-xess` so this work order and architecture corrections are included.  
PR target: `feature/re4-xess`  
Suggested implementation branch: `feature/re4-xess-pr3-detached-execute`  
Date: 2026-09-27  
Scope: real public XeSS D3D12 initialization/execution on an REFramework-owned command path, bridge-owned motion-vector conversion, detached display-resolution output, and GPU lifetime/fence management  
Do not merge without review.

Primary architecture:

- `doc/RE4_XESS_PRODUCTION_ARCHITECTURE_2026-09-27.md`

Research evidence:

- `doc/RE4_XESS_BRIDGE_ARCHITECTURE_AND_RE_STATUS_2026-09-26.md`

Previous production milestones:

- PR 1 merged as `9bcaee281df2ea5843778627f9c51110f9af2cf4`
- PR 2 merged as `1b59aa05a63586ec62137a5ab48f03ebbb7bf730`

External API/code references checked while authoring this work order:

~~~text
Intel XeSS SDK v3.0.2
commit 8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0

OptiScaler master
commit 81602c9aee972589bf9a3ed68a4fbb90f9365328
~~~

The external references are used only to verify the public producer ABI/contract. REFramework still does not add an XeSS SDK package or OptiScaler private dependency.

---

## 1. Objective

PR 3 is the first production milestone that records and submits real XeSS GPU work.

Target flow:

~~~text
RE4 render-resolution scene
    |
    | true pre-Overlay callback
    v
RE4XeSSFrame
    Color      = RE4 HDR/PostMain R11G11B10_FLOAT
    Depth      = RE4 DepthStencilTex
    Velocity   = RE4 VelocityTarget R16G16B16A16_SNORM
    |
    v
RE4XeSSD3D12-owned DIRECT list
    |
    | original Velocity 0x04 -> NON_PIXEL_SHADER_RESOURCE
    | convert Velocity RG SNORM -> bridge R16G16_FLOAT
    | bridge MV UAV -> NON_PIXEL_SHADER_RESOURCE
    |
    | public xessD3D12Execute
    |   Color
    |   Depth
    |   converted Velocity
    |   detached display-resolution output
    |
    | restore original Velocity -> 0x04
    | serialize detached output UAV access
    v
active RE4 DIRECT queue
    |
    v
fence signal
    |
    v
original RE4 Overlay/UI/output continues unchanged
~~~

PR 3 output is deliberately **detached**.

Do not:

- install the XeSS output into an RE4 TargetState;
- replace HDR/PostMain;
- copy the XeSS output to the swapchain;
- modify final screen-out descriptors;
- invoke XeFG APIs;
- detect/select an OptiScaler backend.

PR 4 owns engine output handoff.

---

## 2. Code-review findings that constrain PR 3

### 2.1 PR 2 already builds the correct semantic frame at true pre-Overlay

Current `RE4XeSS::on_pre_overlay_layer_draw()` already validates and constructs:

~~~text
Color
Depth
Velocity
render/display extents
jitter
motion scale
near/far/FOV
resetHistory
frame id
~~~

Keep this semantic boundary.

PR 3 must consume the pointer-bearing `RE4XeSSFrame` **synchronously inside the same callback**.

Do not retain or queue engine resource pointers after the callback returns.

### 2.2 PR 2 currently commits temporal history before any XeSS execute exists

The current PR 2 tail does:

~~~text
construct packet
persist metadata snapshot
clear first_valid_frame_reset_pending
clear history_invalid
clear frame state
return
~~~

PR 3 must convert this into a transaction.

The reset/history state is committed only after the bridge command list has been successfully submitted and fence tracking has been established.

Required shape:

~~~text
build packet
    |
    v
try detached XeSS submission
    |
    +-- Submitted
    |      persist metadata snapshot
    |      consume resetHistory
    |      history becomes valid
    |
    +-- Busy / skipped
    |      do not consume resetHistory
    |      invalidate history for the next valid execute
    |
    +-- Fault
           do not consume resetHistory
           stop further execution for this generation
           drain previously submitted bridge work before runtime teardown
~~~

This avoids telling XeSS that history is continuous when a jittered RE4 frame was never actually submitted to the XeSS producer.

### 2.3 Existing D3D12Hook access is sufficient

Current code already exposes:

~~~text
D3D12Hook::get_device()
D3D12Hook::get_command_queue()
D3D12Hook::get_swap_chain()
~~~

Do not modify `D3D12Hook.*`.

Validate that the selected queue is a DIRECT queue.

### 2.4 Do not reuse the VR CommandContext helper

`src/mods/vr/d3d12/CommandContext.*` is a VR-specific single-context abstraction that:

- owns its own blocking wait behavior;
- logs under `[VR]`;
- is not an eight-slot nonblocking ring;
- is not designed around the RE4 pre-Overlay semantic boundary.

Create an RE4 XeSS-specific D3D12 owner instead.

### 2.5 Important motion-vector format adaptation

Current RE4 evidence:

~~~text
Scene::VelocityTarget
DXGI_FORMAT_R16G16B16A16_SNORM
R = X
G = Y
normalized values
pixel scales:
    X = value * renderWidth  / 2
    Y = value * -renderHeight / 2
~~~

Intel XeSS v3.0.2 public guidance defines the low-resolution motion-vector input as:

~~~text
DXGI_FORMAT_R16G16_FLOAT
~~~

The old pd-upscaler path passing RE4's SNORM resource directly is not proof that native XeSS accepts that format.

Therefore PR 3 must not pass the original RE4 `R16G16B16A16_SNORM` resource directly to `xessD3D12Execute`.

Add a bridge-owned conversion target:

~~~text
DXGI_FORMAT_R16G16_FLOAT
render resolution
ALLOW_UNORDERED_ACCESS
~~~

Convert only R/G and preserve their normalized numeric values.

Then retain the already-proven public velocity scale:

~~~text
xessSetVelocityScale(
    renderWidth / 2,
    -renderHeight / 2)
~~~

### 2.6 XeSS output format must match input Color

Intel XeSS public guidance requires the output texture to use the same color format/color space as the input.

Current RE4 Color is:

~~~text
DXGI_FORMAT_R11G11B10_FLOAT
scene-linear HDR
~~~

Therefore PR 3 detached XeSS output is:

~~~text
DXGI_FORMAT_R11G11B10_FLOAT
display resolution
ALLOW_UNORDERED_ACCESS
initial/steady state = UNORDERED_ACCESS
~~~

Do not choose FP16 merely because the Intel sample uses FP16.

If the device cannot create/use a typed UAV for the current Color format, fail the execution generation and report the contradiction. Do not silently change XeSS output format.

Any later post-XeSS format conversion belongs to PR 4's output handoff.

---

## 3. New production component

Add:

~~~text
src/mods/re4_xess/RE4XeSSD3D12.hpp
src/mods/re4_xess/RE4XeSSD3D12.cpp
~~~

This component owns:

- bridge command allocators/lists;
- bridge fence and fence event;
- per-slot conversion descriptor heaps;
- motion-vector conversion root signature and PSO;
- bridge `R16G16_FLOAT` velocity resource;
- detached XeSS output resource;
- bridge resource-state tracking;
- nonblocking slot acquisition;
- queue submission/fence values.

It does not own:

- RE4 Color;
- RE4 Depth;
- RE4 VelocityTarget;
- RE4 device/queue;
- engine TargetStates;
- swapchain buffers.

Do not AddRef/Release engine resources for lifetime extension.

Normal COM references held by bridge-owned device resources and objects are expected.

---

## 4. Extend the minimal public XeSS ABI

Update:

~~~text
src/mods/re4_xess/RE4XeSSApi.hpp
~~~

Add only the public declarations needed for real init/execute.

### 4.1 Required public types

Add:

~~~text
xess_coord_t
xess_init_flags_t
xess_d3d12_init_params_t
xess_d3d12_execute_params_t
~~~

Use the public XeSS v3.0.2 ABI layout with 8-byte packing.

Required init flags:

~~~cpp
XESS_INIT_FLAG_NONE = 0,
XESS_INIT_FLAG_HIGH_RES_MV = 1 << 0,
XESS_INIT_FLAG_INVERTED_DEPTH = 1 << 1,
XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE = 1 << 2,
XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK = 1 << 3,
XESS_INIT_FLAG_USE_NDC_VELOCITY = 1 << 4,
XESS_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP = 1 << 5,
XESS_INIT_FLAG_LDR_INPUT_COLOR = 1 << 6,
XESS_INIT_FLAG_JITTERED_MV = 1 << 7,
XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE = 1 << 8,
~~~

### 4.2 ABI static checks

For x64 / pack(8), add compile-time checks equivalent to:

~~~cpp
static_assert(sizeof(xess_d3d12_init_params_t) == 64);
static_assert(sizeof(xess_d3d12_execute_params_t) == 136);

static_assert(offsetof(xess_d3d12_execute_params_t, pOutputTexture) == 40);
static_assert(offsetof(xess_d3d12_execute_params_t, jitterOffsetX) == 48);
static_assert(offsetof(xess_d3d12_execute_params_t, inputWidth) == 64);
static_assert(offsetof(xess_d3d12_execute_params_t, inputColorBase) == 72);
static_assert(offsetof(xess_d3d12_execute_params_t, pDescriptorHeap) == 120);
static_assert(offsetof(xess_d3d12_execute_params_t, descriptorHeapOffset) == 128);
~~~

If the official public ABI source used during implementation disagrees with these values, stop and verify before changing the constants.

Do not include private OptiScaler types.

---

## 5. RE4 XeSS init contract

Extend `RE4XeSSRuntime` with a small public-producer initialization wrapper.

Suggested API:

~~~cpp
struct InitSignature {
    xess_2d_t output_resolution{};
    xess_quality_settings_t quality{};
    uint32_t init_flags{};
    float velocity_scale_x{};
    float velocity_scale_y{};
};

bool initialize_sr(const InitSignature& signature, std::string& error);
bool execute(
    ID3D12GraphicsCommandList* command_list,
    const xess_d3d12_execute_params_t& params,
    std::string& error);
bool sr_initialized() const noexcept;
void clear_sr_initialized_state() noexcept;
~~~

Exact names may differ.

### 5.1 Init parameters

Use:

~~~text
outputResolution = current display resolution
qualitySetting   = selected public XeSS quality

initFlags =
    XESS_INIT_FLAG_INVERTED_DEPTH

creationNodeMask = 0
visibleNodeMask  = 0

pTempBufferHeap  = nullptr
bufferHeapOffset = 0

pTempTextureHeap = nullptr
textureHeapOffset= 0

pPipelineLibrary = nullptr
~~~

Do not set:

~~~text
HIGH_RES_MV
JITTERED_MV
USE_NDC_VELOCITY
LDR_INPUT_COLOR
EXPOSURE_SCALE_TEXTURE
RESPONSIVE_PIXEL_MASK
EXTERNAL_DESCRIPTOR_HEAP
ENABLE_AUTOEXPOSURE
~~~

PR 3 uses:

- low-resolution motion vectors;
- non-jittered motion vectors;
- normalized velocity plus `xessSetVelocityScale`;
- HDR input color;
- neutral scalar exposure.

### 5.2 Velocity scale

After successful `xessD3D12Init`, call:

~~~text
xessSetVelocityScale(
    renderWidth / 2,
    -renderHeight / 2)
~~~

Failure of either init or velocity-scale setup prevents execution readiness.

### 5.3 Exposure policy for PR 3

Execute with:

~~~text
exposureScale = 1.0f
pExposureScaleTexture = nullptr
~~~

Do not enable autoexposure in PR 3.

The detached-output milestone validates producer execution/state/lifetime, not final visual exposure quality.

If PR 4 visible output proves exposure is wrong, handle exposure through a documented public XeSS path as a separate evidence-driven change.

### 5.4 Runtime failure ownership

Do not let `RE4XeSSRuntime::execute()` automatically destroy the context on an execute error.

Previous XeSS bridge lists may still be in flight.

Execution errors are reported to the coordinator, which:

1. stops scheduling new bridge work;
2. drains previously submitted bridge fence values;
3. only then destroys/recreates the runtime context.

---

## 6. Execution generation and reinitialization

Add an execution signature separate from PR 2's temporal configuration.

At minimum:

~~~text
D3D12 device identity
DIRECT queue identity
quality
display width/height
render width/height
XeSS init flags
~~~

Color format is validated at the actual pre-Overlay frame and controls detached-output resource creation.

### 6.1 Ready conditions

XeSS execution may become ready only when:

~~~text
RE4
requested mode != Off
PR2 temporal-ready
runtime context ready
valid DIRECT queue
bridge D3D12 common objects ready
xessD3D12Init successful for current signature
xessSetVelocityScale successful
~~~

### 6.2 Signature changes

Mode changes already recreate the PR 1 runtime context.

For display/render size or other execution-signature changes:

~~~text
stop scheduling bridge executes
    |
poll bridge fence until all submitted work is complete
    |
recreate/reconfigure bridge resources as needed
    |
xessD3D12Init with the new signature
    |
xessSetVelocityScale
    |
reset temporal/XeSS history
    |
resume
~~~

Do not call `xessD3D12Init` while earlier XeSS command lists are still pending.

Do not block every frame while draining.

Polling `GetCompletedValue()` from the normal coordinator path is preferred.

---

## 7. Eight-slot command allocator/list ring

Use eight slots, matching the diagnostic evidence.

Suggested slot:

~~~cpp
struct CommandSlot {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> velocity_descriptors;
    uint64_t last_fence_value{};
};
~~~

One bridge-wide fence is sufficient.

Bridge-wide state:

~~~text
ID3D12Fence
next fence value
optional fence event for explicit lifecycle waits
next slot search index
last submitted fence value
~~~

### 7.1 Slot acquisition

Before resetting a slot:

~~~text
slot.lastFence == 0
OR
bridgeFence.GetCompletedValue() >= slot.lastFence
~~~

If the preferred slot is busy, scan the other seven slots.

If all eight are busy:

- do not wait in the pre-Overlay callback;
- return `Busy`;
- do not execute XeSS for that frame;
- leave engine resources untouched;
- invalidate XeSS temporal history so the next submitted frame uses `resetHistory=true`.

### 7.2 Command-list creation

Create DIRECT command allocators/lists from the current RE4 D3D12 device.

Close newly-created command lists once so later per-frame use follows:

~~~text
allocator->Reset()
list->Reset(allocator, nullptr)
record
list->Close()
queue->ExecuteCommandLists()
queue->Signal()
~~~

Never append to an engine command list.

---

## 8. Motion-vector conversion pass

### 8.1 Why conversion is mandatory

Current RE4 VelocityTarget:

~~~text
R16G16B16A16_SNORM
~~~

Public XeSS low-res MV contract:

~~~text
R16G16_FLOAT
~~~

The conversion preserves the numeric R/G values.

It does not multiply by render dimensions inside the shader.

Pixel scaling stays in the public XeSS velocity-scale API.

### 8.2 Bridge velocity resource

Create:

~~~text
Dimension   = TEXTURE2D
Width       = renderWidth
Height      = renderHeight
MipLevels   = 1
ArraySize   = 1
Format      = DXGI_FORMAT_R16G16_FLOAT
SampleCount = 1
Flags       = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
Heap        = DEFAULT
~~~

Track its actual submitted state.

Preferred steady state after a successful bridge submission:

~~~text
D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
~~~

### 8.3 Conversion shader

Use a minimal bridge-owned compute shader.

Equivalent HLSL:

~~~hlsl
Texture2D<float4> InputVelocity : register(t0);
RWTexture2D<float2> OutputVelocity : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint width;
    uint height;
    OutputVelocity.GetDimensions(width, height);

    if (tid.x >= width || tid.y >= height) {
        return;
    }

    OutputVelocity[tid.xy] = InputVelocity.Load(int3(tid.xy, 0)).rg;
}
~~~

The typed SNORM SRV converts R/G to normalized float values automatically.

The FP16 output stores those normalized values for XeSS.

### 8.4 Shader compilation

The project already delay-loads `D3DCOMPILER_47.dll`.

For this small private conversion pass, runtime compilation is acceptable.

Use:

~~~text
D3DCompile
target = cs_5_1
entry  = main
~~~

Add `d3dcompiler` to the REFramework link libraries in `cmake.toml` if the symbol is not already provided transitively, then regenerate `CMakeLists.txt`.

Do not add DXC or another external package for this PR.

Compile the shader once per device/bridge setup, not every frame.

A shader compile failure is a bridge initialization failure and must fail closed.

### 8.5 Descriptor ownership

Each ring slot owns a small shader-visible CBV/SRV/UAV heap with two descriptors:

~~~text
descriptor 0 = current RE4 VelocityTarget SRV
    DXGI_FORMAT_R16G16B16A16_SNORM

descriptor 1 = bridge velocity UAV
    DXGI_FORMAT_R16G16_FLOAT
~~~

The slot is fence-free before descriptors are rewritten, so CPU descriptor writes cannot race with an older GPU use of the same slot heap.

### 8.6 Root signature / PSO

Use a minimal compute root signature with one descriptor table containing:

~~~text
1 SRV t0
1 UAV u0
~~~

No engine descriptor heap is borrowed.

No engine root signature/PSO is modified.

---

## 9. Detached XeSS output

Create one bridge-owned detached output resource for the current execution generation.

Derive its format from the validated Color resource.

For the current supported RE4 build, require:

~~~text
Color Format = DXGI_FORMAT_R11G11B10_FLOAT
~~~

Output desc:

~~~text
Dimension   = TEXTURE2D
Width       = displayWidth
Height      = displayHeight
MipLevels   = 1
ArraySize   = 1
Format      = Color.Format
SampleCount = 1
Flags       = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
Heap        = DEFAULT
Initial state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS
~~~

Before creation, verify D3D12 format support sufficient for a typed UAV, including:

~~~text
D3D12_FORMAT_SUPPORT1_TEXTURE2D
D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW
D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE
~~~

If the current Color format or UAV capability does not meet the public XeSS contract, stop execution and log the exact format/support bits.

Do not create a different-format XeSS output as a workaround.

### 9.1 Repeated writes

The detached output remains in UAV state.

Record a UAV barrier for the detached output after a successful `xessD3D12Execute` recording so repeated frame writes are ordered on the same queue.

PR 3 does not transition the output for engine presentation.

---

## 10. Exact resource-state sequence

At true pre-Overlay, research proves:

~~~text
Color    = 0xC0
Depth    = 0xE0
Velocity = 0x04
~~~

### 10.1 Color

Color already includes:

~~~text
NON_PIXEL_SHADER_RESOURCE
PIXEL_SHADER_RESOURCE
~~~

Do not transition it merely to remove the PIXEL_SHADER_RESOURCE bit.

Pass it directly as `pColorTexture`.

### 10.2 Depth

Depth already includes:

~~~text
DEPTH_READ
NON_PIXEL_SHADER_RESOURCE
PIXEL_SHADER_RESOURCE
~~~

Do not transition it.

Pass it directly as `pDepthTexture`.

### 10.3 Original RE4 VelocityTarget

Record:

~~~text
D3D12_RESOURCE_STATE_RENDER_TARGET
    ->
D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
~~~

Run the bridge conversion.

Then, after the XeSS execute has been recorded, restore:

~~~text
D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    ->
D3D12_RESOURCE_STATE_RENDER_TARGET
~~~

The original RE4 VelocityTarget must be back in `0x04` before the bridge list completes and before downstream RE4 work executes.

### 10.4 Bridge velocity

For first use, transition from its actual initial state to UAV if necessary.

Per successful frame:

~~~text
bridge MV current steady state
    -> UAV

compute conversion

UAV
    -> NON_PIXEL_SHADER_RESOURCE

XeSS reads bridge MV

remain NON_PIXEL_SHADER_RESOURCE
~~~

The UAV->NPSR transition provides synchronization for conversion writes before XeSS reads.

### 10.5 Do not mutate engine states on a discarded list

If XeSS recording fails before queue submission:

- do not submit the list;
- actual engine resource states remain unchanged;
- do not update bridge state trackers as though the discarded list executed.

Only commit state-tracker changes after a successful queue submission.

---

## 11. Public XeSS execute parameters

Populate:

~~~cpp
xess_d3d12_execute_params_t params{};

params.pColorTexture = frame.color;
params.pVelocityTexture = converted_velocity;
params.pDepthTexture = frame.depth;
params.pExposureScaleTexture = nullptr;
params.pResponsivePixelMaskTexture = nullptr;
params.pOutputTexture = detached_output;

params.jitterOffsetX = frame.jitter_x_pixels;
params.jitterOffsetY = frame.jitter_y_pixels;
params.exposureScale = 1.0f;
params.resetHistory = frame.reset_history ? 1u : 0u;

params.inputWidth = frame.render_width;
params.inputHeight = frame.render_height;

params.inputColorBase = {0, 0};
params.inputMotionVectorBase = {0, 0};
params.inputDepthBase = {0, 0};
params.inputResponsiveMaskBase = {0, 0};
params.reserved0 = {0, 0};
params.outputColorBase = {0, 0};

params.pDescriptorHeap = nullptr;
params.descriptorHeapOffset = 0;
~~~

Do not apply another jitter scale.

The PR 2 jitter values are already pixel-space values in the public XeSS range.

Do not pass the original RE4 VelocityTarget as `pVelocityTexture`.

---

## 12. Pre-Overlay submission transaction

Refactor the end of `on_pre_overlay_layer_draw()`.

Suggested bridge result:

~~~cpp
enum class SubmitResult {
    Submitted,
    Busy,
    Faulted,
};
~~~

Flow:

~~~text
validate packet
    |
    v
bridge.submit(frame, runtime)
    |
    +-- Submitted
    |      update opaque resource identities
    |      store FrameSnapshot
    |      clear first_valid_frame_reset_pending
    |      clear history_invalid
    |      clear last reset reason
    |
    +-- Busy
    |      invalidate_history("xess-command-ring-busy")
    |      keep reset pending
    |
    +-- Faulted
           invalidate_history("xess-execute-fault")
           mark execution generation faulted
           stop scheduling
           begin drain-before-teardown
~~~

Always clear same-frame cached Scene/Camera state before returning from the callback.

### 12.1 What counts as Submitted

Only return `Submitted` after:

1. a free slot was acquired;
2. allocator/list reset succeeded;
3. all required barriers/conversion commands were recorded;
4. `xessD3D12Execute` returned success;
5. original Velocity restore was recorded;
6. command list Close succeeded;
7. `ExecuteCommandLists` was issued;
8. queue `Signal` succeeded;
9. the slot's last fence value was stored.

If `xessD3D12Execute` returns an error, do not submit the partially recorded list.

### 12.2 Signal failure

`ExecuteCommandLists` is void but queue `Signal` returns an HRESULT.

If Signal fails after ExecuteCommandLists:

- treat the execution generation as terminally faulted;
- do not reuse allocators/resources whose completion can no longer be proven;
- log the HRESULT and `GetDeviceRemovedReason()`;
- wait for device/reset lifecycle to establish a new safe generation.

Do not immediately Release resources that may still be referenced by the submitted GPU work.

---

## 13. Execution fault and drain behavior

Add coordinator state such as:

~~~text
execution_ready
execution_reconfigure_pending
execution_faulted
execution_failure_reason
transition_waiting_for_bridge_idle
~~~

### 13.1 Normal reconfiguration

For mode/quality/display/render changes:

- stop new submissions immediately;
- poll the bridge fence;
- once idle, reconfigure/reinitialize;
- reset history;
- resume.

No per-frame CPU fence wait.

### 13.2 Mode Off

Off must stop new SceneView/jitter/execute activity immediately through the existing temporal reset.

If bridge work remains in flight:

- do not destroy XeSS context yet;
- do not Release bridge resources yet;
- drain asynchronously;
- once idle, destroy runtime context and bridge resources.

### 13.3 Device reset

On device reset:

- stop submissions;
- mark execution/temporal state invalid;
- do not submit against the old queue/device after the callback;
- release runtime/bridge GPU resources only when safe completion is established.

If the device is removed, record `GetDeviceRemovedReason()` and move to a recreate/wait state rather than attempting more XeSS work.

Do not call any XeFG lifecycle API.

---

## 14. D3D12 bridge initialization checks

Before creating the ring:

- device != nullptr;
- queue != nullptr;
- queue `GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT`;
- device identity matches the current PR 1 runtime device generation.

Give all bridge-owned D3D12 objects useful names, for example:

~~~text
RE4XeSS Fence
RE4XeSS CommandAllocator 0..7
RE4XeSS CommandList 0..7
RE4XeSS VelocityDescriptors 0..7
RE4XeSS ConvertedVelocity
RE4XeSS DetachedOutput
RE4XeSS VelocityConvertRootSignature
RE4XeSS VelocityConvertPSO
~~~

---

## 15. Logging

Add event-driven logs:

~~~text
[RE4XeSS][Init]
[RE4XeSS][Execute]
[RE4XeSS][Resize]
[RE4XeSS][Failure]
~~~

Useful initialization evidence:

~~~text
device
queue
queue type
display size
render size
quality
init flags
velocity scale
converted MV format
output format
ring size
~~~

Bounded execute evidence under Debug Log, e.g. first 32 successful submissions per execution generation:

~~~text
frame
slot
fence value
resetHistory
jitter
input/output size
Color/Depth/original MV identities
converted MV identity
detached output identity
xessD3D12Execute result
~~~

Do not log every successful frame forever.

For ring-busy events, rate-limit warnings.

---

## 16. UI status

Update the RE4 XeSS status text to distinguish:

~~~text
Off - native RE4 rendering

Temporal inputs active - waiting for XeSS execution initialization

XeSS execute active - detached output only; not presented yet

XeSS execute draining for reconfiguration

XeSS execute unavailable: <reason>
~~~

Do not claim visible upscaling in PR 3.

The detached output is not yet wired into RE4 presentation.

---

## 17. Protected/no-touch paths

No behavioral changes in:

~~~text
src/compatibility/xefg/**
src/D3D12Hook.cpp
src/D3D12Hook.hpp
existing XeFG logic in src/REFramework.cpp
OptiScaler repository
~~~

Do not add:

- XeFG API calls;
- OptiScaler detection;
- DLSS/FSR backend logic;
- swapchain interception;
- engine command-list mutation;
- final-output descriptor tracing;
- engine TargetState replacement;
- backbuffer copy.

If any of these appears necessary, stop and report.

---

## 18. Expected changed-file set

Expected approximately:

~~~text
cmake.toml                              # d3dcompiler link if required
CMakeLists.txt                          # regenerated

src/mods/re4_xess/RE4XeSSApi.hpp
src/mods/re4_xess/RE4XeSSRuntime.hpp
src/mods/re4_xess/RE4XeSSRuntime.cpp

src/mods/re4_xess/RE4XeSSD3D12.hpp
src/mods/re4_xess/RE4XeSSD3D12.cpp

src/mods/re4_xess/RE4XeSS.hpp
src/mods/re4_xess/RE4XeSS.cpp
~~~

`RE4XeSSFrame.hpp` should not require semantic changes unless a small producer field is genuinely missing.

Do not port diagnostic hooks into production.

---

## 19. Build-system work

If `D3DCompile` requires an explicit link entry, add:

~~~toml
"d3dcompiler"
~~~

to the REFramework link libraries in `cmake.toml`.

The target already has:

~~~text
/DELAYLOAD:D3DCOMPILER_47.dll
~~~

Keep that delayed-load behavior.

Regenerate checked-in `CMakeLists.txt` through cmkr after adding new source files/build dependencies.

Do not add:

- Intel XeSS SDK package;
- DXC package;
- shader build subproject;
- OptiScaler headers/libraries.

---

## 20. Static acceptance checks

Before opening the PR:

### 20.1 Protected paths

~~~text
git diff --name-only feature/re4-xess...HEAD
~~~

must contain no:

~~~text
src/compatibility/xefg/**
src/D3D12Hook.*
~~~

### 20.2 Real execute exists

Confirm PR 3 contains calls to:

~~~text
xessD3D12Init
xessSetVelocityScale
xessD3D12Execute
~~~

through the dynamically resolved public function table.

### 20.3 No direct engine MV pass-through

Confirm:

~~~text
params.pVelocityTexture != frame.velocity
~~~

for the current RE4 `R16G16B16A16_SNORM` path.

### 20.4 No presentation handoff

Confirm there is no:

~~~text
TargetState replacement
swapchain GetBuffer copy target
CopyResource(detachedOutput, backbuffer)
CopyResource(backbuffer, detachedOutput)
final-screen descriptor patch
~~~

### 20.5 No imported libxess

The built REFramework DLL must still have no static `libxess.dll` import.

---

## 21. Local build validation

Use x64 Release:

~~~powershell
cmake -S . -B build ^
  -G "Visual Studio 18 2026" ^
  -A x64 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DDEVELOPER_MODE=ON

cmake --build build --config Release --target REFramework
~~~

Required:

~~~text
0 compile errors
0 link errors
~~~

Also run:

~~~text
git diff --check
~~~

If practical, inspect the PE imports:

- `libxess.dll` must not be a static import;
- `D3DCOMPILER_47.dll` may appear as a delay-loaded import.

---

## 22. Manual RE4 runtime validation sequence

Do not mark tests PASS unless actually run.

Start with Debug Log enabled.

### 22.1 Off baseline

Expected:

~~~text
no bridge D3D12 ring
no XeSS init/execute
native rendering
~~~

### 22.2 First active bring-up — Quality

Use Quality first.

Expected initialization:

~~~text
public input resolution query succeeds
DIRECT queue validated
8-slot ring created
conversion PSO compiled
converted MV target created as R16G16_FLOAT
xessD3D12Init succeeds
xessSetVelocityScale succeeds
detached output created as R11G11B10_FLOAT at display size
~~~

Expected frame execution:

~~~text
original Velocity 0x04 -> NPSR
velocity conversion recorded
converted MV -> NPSR
xessD3D12Execute = SUCCESS
original Velocity restored to 0x04
bridge list submitted
fence progresses
~~~

No visible upscaled presentation is expected.

### 22.3 Repeated execution

Run stable gameplay long enough to establish:

- many successful execute calls;
- fence values continue advancing;
- allocator/list slots are reused only after completion;
- no frame-by-frame CPU wait;
- no ring corruption;
- no device removed error;
- no repeated execute failure.

### 22.4 ResetHistory

Exercise:

- Off -> Quality;
- Quality -> Balanced;
- resize/display change;
- Load Save.

For the first successful submission after each temporal discontinuity, confirm:

~~~text
resetHistory = 1
~~~

and subsequent continuous submissions use:

~~~text
resetHistory = 0
~~~

If a bridge frame is skipped because the ring is busy/faulted, the next successful submit must reset history.

### 22.5 Load Save

Confirm the PR 2 quarantine still works and no XeSS execute is submitted while:

~~~text
pre-Pause inhibit departure pending
Pause active
startup/untrusted-baseline rebaseline pending
~~~

First valid post-recovery XeSS submission must carry `resetHistory=1`.

### 22.6 Resource identity changes

On scene/load transitions:

- original engine resources remain borrowed;
- no Release loop;
- descriptor slot rewrites occur only on fence-free slots;
- output/conversion owned resources remain valid or recreate safely;
- first valid execute after discontinuity resets history.

### 22.7 Mode Off with work in flight

Switch active mode -> Off.

Expected:

~~~text
new submissions stop immediately
SceneView/jitter path returns native
bridge drains submitted work
runtime context/resources destroy only after safe completion
~~~

### 22.8 Runtime without OptiScaler, if native libxess is available

This is optional if the local deployment only has the OptiScaler-provided frontend.

If tested, confirm the public native XeSS path accepts:

~~~text
Color  = R11G11B10_FLOAT
Depth  = current RE4 depth resource
MV     = converted R16G16_FLOAT
Output = R11G11B10_FLOAT UAV
~~~

If native XeSS rejects one of these despite the documented public contract, stop and preserve the exact result/log.

### 22.9 Stock OptiScaler frontend

With the normal OptiScaler-provided `libxess.dll`:

Expected:

- Create/Init/SetVelocityScale/Execute public call stream is visible to OptiScaler;
- REFramework does not branch on selected backend;
- selected backend can consume the same producer packet;
- detached output remains unpresented.

Do not enable XeFG validation yet as an acceptance requirement for PR 3.

---

## 23. Acceptance criteria

PR 3 is complete only when:

- complete minimal XeSS init/execute ABI declarations are correct and statically checked;
- current runtime context can be initialized with inverted-depth / low-res / non-jittered-MV semantics;
- velocity scale is set to `W/2, -H/2`;
- RE4 `R16G16B16A16_SNORM` MV is converted to bridge-owned `R16G16_FLOAT`;
- original engine VelocityTarget is restored to `0x04`;
- Color and Depth are not redundantly transitioned;
- detached output matches Color format and display extent;
- output stays outside RE4 presentation;
- eight-slot allocator/list ring is nonblocking;
- slot reuse is fence-proven;
- no engine-owned command list is modified;
- queue submission uses the active RE4 DIRECT queue;
- repeated output UAV writes are synchronized;
- resetHistory is consumed only after successful submission;
- skipped/failed submissions force reset on the next valid execute;
- runtime/context teardown never races known in-flight bridge work;
- mode/display/render/device changes enter a safe drain/reconfigure path;
- no OptiScaler private API exists;
- no XeFG compatibility behavior changes;
- no static libxess import exists;
- x64 Release build succeeds.

---

## 24. PR creation requirements

Open as a Draft PR against:

~~~text
feature/re4-xess
~~~

PR description must state:

1. this is PR 3 of the RE4 XeSS production sequence;
2. PR 2 base/merge commit `1b59aa05a63586ec62137a5ab48f03ebbb7bf730`;
3. real XeSS Init/SetVelocityScale/Execute is now active;
4. RE4 SNORM Velocity is converted to public-XeSS-compatible RG16F;
5. detached XeSS output is not presented;
6. original Velocity is restored before RE4 downstream work;
7. ring/fence ownership and no-per-frame-wait behavior;
8. existing XeFG compatibility remains untouched;
9. exact local build result;
10. runtime tests actually performed;
11. runtime tests left for the user.

Do not merge automatically.

---

## 25. Stop conditions

Stop and report instead of widening PR 3 if:

- current RE4 Color is not `R11G11B10_FLOAT`;
- current RE4 Velocity is not `R16G16B16A16_SNORM`;
- the device cannot provide the required UAV capability for the same-format XeSS output;
- converted `R16G16_FLOAT` MV is rejected by the public frontend;
- native XeSS rejects the documented current RE4 depth resource format;
- `xessD3D12Init` or `xessSetVelocityScale` requires backend-specific behavior;
- OptiScaler interception requires a private/non-public producer call;
- executing on the own DIRECT list breaks the proven pre-Overlay ordering;
- original Velocity cannot be restored to `0x04` safely;
- repeated XeSS submission requires modifying an engine command list;
- implementation appears to require an output TargetState/swapchain change before PR 4;
- implementation appears to require changes in existing XeFG compatibility.

These are architecture/evidence contradictions and require review before scope expands.
