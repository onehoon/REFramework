# RE4 XeSS Production — PR 2 Work Order

Base branch: feature/re4-xess  
Expected base HEAD at authoring time: 9bcaee281df2ea5843778627f9c51110f9af2cf4  
PR target: feature/re4-xess  
Suggested implementation branch: feature/re4-xess-pr2-temporal-frame  
Date: 2026-09-27  
Scope: production temporal frame builder, SceneView input-resolution control, camera metadata, deterministic XeSS jitter integration, and RE4 history-reset state  
Do not merge without review.

Primary architecture:
- doc/RE4_XESS_PRODUCTION_ARCHITECTURE_2026-09-27.md

Research evidence:
- doc/RE4_XESS_BRIDGE_ARCHITECTURE_AND_RE_STATUS_2026-09-26.md

PR 1:
- merged as 9bcaee281df2ea5843778627f9c51110f9af2cf4
- provides RE4-only UI/config, dynamic libxess discovery, public XeSS context creation, and RE4 Renderer layout corrections

---

## 1. Objective

Implement the production temporal-input side of the RE4 XeSS bridge without executing XeSS yet.

PR 2 must establish this frame contract:

~~~text
selected RE4 XeSS quality mode
    |
    v
public xessGetOptimalInputResolution()
    |
    v
requested render extent
    |
    v
via.SceneView.get_Size override
    |
    +--> HDR/PostMain Color
    +--> DepthStencilTex
    +--> VelocityTarget
           all at requested render extent
    |
    v
RE4 deterministic projection jitter/history integration
    |
    v
true pre-Overlay frame packet
    Color + Depth + Velocity
    render/display sizes
    jitter
    motion scale
    near/far/FOV
    resetHistory
~~~

PR 2 does not:

- call xessD3D12Init;
- call xessD3D12Execute;
- create XeSS output textures;
- create or submit a bridge command list;
- perform D3D12 ResourceBarrier work;
- alter Overlay/output TargetState;
- alter the swapchain;
- alter existing XeFG compatibility code.

The only intentional render-path mutations in PR 2 are:

1. RE4 SceneView input-resolution override;
2. RE4 projection/history jitter integration already proven by Capture 13.

---

## 2. Frozen evidence to implement, not rediscover

Do not re-run broad reverse-engineering probes in production code.

The following facts are already closed by the research branch.

### 2.1 Render/display split

Capture 11 proved:

~~~text
SceneView returned size   = 1920x1080
Color                     = 1920x1080
Depth                     = 1920x1080
Velocity                  = 1920x1080

DXGI swapchain/display    = 2560x1440
~~~

Therefore:

~~~text
renderWidth/renderHeight
    = bridge-controlled SceneView size

displayWidth/displayHeight
    = active DXGI swapchain/output size
~~~

Do not use D3D12Hook::get_render_width()/get_render_height() as the authoritative render size.

Do not modify ImageQualityRate.

### 2.2 Semantic temporal resources

Use:

~~~text
Color
    Overlay main TargetState native D3D12 resource
    == Scene PostMainTarget
    == Scene HDRTarget
    observed format R11G11B10_FLOAT

Depth
    Scene::DepthStencilTex native D3D12 resource

Velocity
    Scene::VelocityTarget native D3D12 resource
    observed format R16G16B16A16_SNORM
~~~

Do not use primary Scene MRT0/1/2 as Color.

Do not use the transient post-Overlay working target as Color.

Do not use PrepareOutput/swapchain state as temporal Color.

### 2.3 Motion-vector producer semantics

Already proven:

~~~text
R = X
G = Y

jitter included in MV:
    false

motionScaleX =  renderWidth / 2
motionScaleY = -renderHeight / 2
~~~

PR 2 computes and stores these scales in the frame packet.

Do not convert the Velocity resource itself in this PR.

### 2.4 Depth/camera semantics

Already proven:

~~~text
depth convention:
    inverted/reversed

near:
    primary via.Camera NearClipPlane

far:
    primary via.Camera FarClipPlane

vertical FOV:
    2 * atan(1 / SceneInfo.projection[1][1])
~~~

Do not hardcode observed near/far/FOV values.

### 2.5 RE4 jitter integration

Capture 13 proved the RE4 matrix/history mechanics.

For pixel jitter Jx/Jy:

~~~text
matrixJitterX = +2 * Jx / renderWidth
matrixJitterY = -2 * Jy / renderHeight
~~~

For each of the six verified SceneInfo variants:

~~~text
main
depthDistortion
filter
jitterDisable
jitterDisablePost
zPrepass
~~~

the production path must preserve the proven construction:

1. read current unjittered projection/view;
2. construct old_view_projection_matrix from the previous unjittered projection/view after applying the current frame jitter to that previous projection;
3. save the current unjittered projection/view for next-frame history;
4. apply the current jitter to current projection;
5. recompute inverse_projection_matrix;
6. recompute view_projection_matrix;
7. recompute inverse_view_projection_matrix.

Do not modify the primary via.Camera projection result itself. It remains the unjittered reference.

### 2.6 Load Save reset window

Use the Capture 22 rule:

~~~text
normal gameplay
    |
    | SceneLoadZoneManager._Pause becomes true
    v
load/history invalid
    |
    | _Pause may become false
    | history remains invalid
    |
    | GameSituationManager.InhibitBit returns
    | to the remembered pre-load normal value
    v
first valid gameplay frame
    resetHistory = true
    resume temporal history
~~~

Do not hardcode the observed 0xB9 inhibit value.

---

## 3. Code-review findings from merged PR 1

### 3.1 RE4 isolation is already correct

RE4XeSS is created only inside:

~~~cpp
if (sdk::GameIdentity::get().is_re4()) {
    ...
}
~~~

Keep this.

Do not create a second global/singleton RE4 temporal controller outside the Mod.

### 3.2 PR 1 runtime already resolves xessGetOptimalInputResolution

RE4XeSSRuntime already dynamically resolves:

~~~text
xessGetOptimalInputResolution
~~~

Add a small public wrapper around the existing resolved function.

Do not change DLL discovery.

Do not change the two supported runtime locations.

Do not add an SDK/submodule/import library.

### 3.3 Existing callbacks already support the required integration

Mod exposes the required callback surfaces, including:

~~~text
on_view_get_size
on_camera_get_projection_matrix
on_scene_layer_update
on_pre_scene_layer_draw
on_pre_overlay_layer_draw
~~~

Use these production callbacks.

Do not add a new game-code detour for SceneView or Camera if the existing Mod callbacks can perform the work.

### 3.4 Existing Renderer SDK accessors already expose the resource path

Current shared Renderer accessors include:

~~~text
Scene::get_scene_info()
Scene::get_depth_distortion_scene_info()
Scene::get_filter_scene_info()
Scene::get_jitter_disable_scene_info()
Scene::get_jitter_disable_post_scene_info()
Scene::get_z_prepass_scene_info()

Scene::get_depth_stencil_d3d12()
Scene::get_motion_vectors_d3d12()
Scene::get_post_main_target_d3d12()
Scene::get_hdr_target_d3d12()

Overlay::get_main_target_state()
TargetState::get_native_resource_d3d12()
~~~

Prefer these semantic accessors.

Do not copy raw diagnostic offsets into RE4XeSS when an existing verified accessor already exists.

---

## 4. New production file

Add:

~~~text
src/mods/re4_xess/RE4XeSSFrame.hpp
~~~

The frame type should remain a plain normalized producer contract with no OptiScaler/XeFG types.

Target shape:

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

Exact naming can follow repository style.

Do not put:

~~~text
OptiScaler backend
XeFG state
swapchain backbuffer pointer
XeSS output pointer
command list
descriptor heap
~~~

into this frame object.

Those belong to later layers.

---

## 5. Extend RE4XeSSRuntime only for the public resolution query

Add a small result type, for example:

~~~cpp
struct InputResolutionQuery {
    xess_2d_t optimal{};
    xess_2d_t minimum{};
    xess_2d_t maximum{};
};
~~~

Expose a method equivalent to:

~~~cpp
std::optional<InputResolutionQuery> query_optimal_input_resolution(
    xess_2d_t output_resolution,
    xess_quality_settings_t quality,
    std::string& error) const;
~~~

Requirements:

- valid only in ContextReady;
- call the already resolved xessGetOptimalInputResolution;
- return optimal/min/max;
- reject zero output or zero returned optimal dimensions;
- preserve the exact public frontend result;
- apply the following exact min/max policy:

~~~text
all min/max dimensions are zero:
    range metadata is treated as unavailable;
    do not range-check optimal

all min/max dimensions are nonzero:
    range is meaningful;
    require min.x <= max.x and min.y <= max.y;
    require optimal.x in [min.x, max.x];
    require optimal.y in [min.y, max.y]

mixed zero/nonzero min/max dimensions:
    malformed response;
    fail temporal configuration
~~~

Do not silently repair or synthesize min/max values.
- do not replace it with hardcoded XeSS ratios;
- do not call xessD3D12Init;
- do not call xessD3D12Execute.

This is important for stock OptiScaler compatibility.

OptiScaler's XeSS frontend implements xessGetOptimalInputResolution and may apply its own configured quality ratio/rounding. REFramework must accept that frontend result rather than recomputing a backend-specific ratio.

---

## 6. Display-resolution source

For an active mode, derive output/display size from the active D3D12 swapchain.

Preferred:

~~~text
D3D12Hook::get_swap_chain()
    -> IDXGISwapChain3::GetDesc1()
    -> Width / Height
~~~

If the DXGI descriptor reports zero dimensions during a transient resize state, do not invent dimensions.

It is acceptable to wait until a valid nonzero display extent returns.

Do not use:

~~~text
D3D12Hook::get_render_width()
D3D12Hook::get_render_height()
~~~

as the scene render extent.

A display-size change is a temporal-generation change:

~~~text
display size changes
    -> query xessGetOptimalInputResolution again
    -> update requested render extent
    -> invalidate jitter/history
    -> first later valid frame resetHistory=true
~~~

PR 2 does not recreate an XeSS initialized context/output because those do not exist yet.

---

## 7. Temporal configuration state

Add a small coordinator-side state describing the current temporal generation.

It should track at least:

~~~text
requested UpscalingMode
public xess_quality_settings_t
display extent
queried optimal render extent
queried min/max input extent
configuration generation
temporal-ready boolean
last setup failure reason
~~~

Only enter temporal-ready when:

~~~text
RE4
requested mode != Off
RE4XeSSRuntime == ContextReady
valid D3D12 swapchain/display extent
valid xessGetOptimalInputResolution result
nonzero render extent
render extent <= display extent
matching aspect ratio within the explicit tolerance below
~~~

Aspect validation must use a deterministic cross-product test, not floating-point equality:

~~~text
crossError =
    abs(renderWidth * displayHeight - renderHeight * displayWidth)

relativeAspectError =
    crossError / (renderHeight * displayWidth)

require relativeAspectError <= 0.01
~~~

Use 64-bit arithmetic for the products.

The 1% tolerance intentionally allows ordinary integer/multiple rounding from a public XeSS/OptiScaler frontend while rejecting a clearly incompatible aspect ratio.

If the returned optimal extent exceeds 1% relative aspect error, fail temporal configuration and log the exact returned display/render extents and error. Do not silently rewrite the runtime-returned size.

When not temporal-ready:

- on_view_get_size must leave the native result untouched;
- no projection jitter is injected;
- no frame packet is published;
- native RE4 rendering continues.

Do not force the persisted mode to Off just because a transient setup condition is missing.

---

## 8. SceneView render-size override

Implement:

~~~text
on_view_get_size(REManagedObject* scene_view, float* result)
~~~

Requirements:

- RE4-only;
- active mode only;
- runtime ContextReady only;
- temporal configuration ready only;
- result must be non-null;
- requested render width/height must be nonzero.

When all conditions are true, overwrite the returned SceneView size with the queried optimal input resolution.

Do not alter the native result when Off, Faulted, waiting for device, waiting for swapchain, or resolution query failure.

Do not modify ImageQualityRate.

Do not call xessGetOptimalInputResolution from the SceneView callback itself.

The query belongs to coordinator configuration/update logic; the callback only consumes an already validated requested extent.

### 8.1 Native AA

Do not special-case Native AA with a hardcoded ratio.

Still call the public frontend query with XESS_QUALITY_SETTING_AA.

The expected normal result is render == display, but the public frontend remains authoritative.

### 8.2 Rate-limited debug evidence

With Debug Log enabled, log a configuration transition once:

~~~text
[RE4XeSS][Temporal] mode=Quality display=2560x1440 input=... min=... max=...
~~~

Do not log every SceneView get_Size call.

---

## 9. Jitter sequence

Use a deterministic Halton sequence with bases 2 and 3.

Do not use the old four-phase diagnostic square pattern in production.

### 9.1 Jitter range

Pixel jitter supplied to the future XeSS execute path must remain in:

~~~text
[-0.5, +0.5]
~~~

Use centered Halton points:

~~~text
Jx = halton(sampleIndex, 2) - 0.5
Jy = halton(sampleIndex, 3) - 0.5
~~~

Use sample indices beginning at 1.

### 9.2 Sequence length

Do not hardcode sequence length by quality preset.

Compute it from the actual queried render/display ratio.

XeSS guidance requires the repeated sequence to scale with the square of the upscale factor. Use:

~~~text
scaleX = displayWidth  / renderWidth
scaleY = displayHeight / renderHeight
scale  = max(scaleX, scaleY)

phaseCount = max(8, ceil(8 * scale * scale))
~~~

Examples:

~~~text
1.0x -> 8
2.0x -> 32
3.0x -> 72
~~~

This deliberately uses the runtime-returned resolution and therefore also follows OptiScaler frontend quality overrides.

Do not hardcode quality-to-phase-count tables.

### 9.3 Sequence lifecycle

Advance the jitter index once per valid primary-scene render frame, not once per callback invocation.

Reset the sequence to its first sample when:

- active mode changes;
- display extent changes;
- queried render extent changes;
- D3D12 device changes;
- runtime context is recreated;
- temporal history is invalidated by Load Save;
- resource identity discontinuity invalidates history;
- a frame-gap/history discontinuity is detected.

Store the selected pixel jitter in RE4XeSSFrame so PR 3 can pass exactly the same values to xessD3D12Execute.

---

## 10. Camera metadata capture

Implement/extend:

~~~text
on_camera_get_projection_matrix
~~~

Do not change the returned Camera projection.

Only process the primary camera:

~~~text
camera == sdk::get_primary_camera()
~~~

Capture for the current render frame:

~~~text
near plane
far plane
camera frame id
~~~

Use via.Camera:

~~~text
get_NearClipPlane
get_FarClipPlane
~~~

Validate:

~~~text
finite
near > 0
far > near
~~~

Vertical FOV comes from the primary SceneInfo projection:

~~~text
verticalFov = 2 * atan(1 / projection[1][1])
~~~

Validate finite and positive.

If camera metadata is missing for the same frame, do not publish a valid frame packet.

Do not hardcode 0.01 / 10000 / 45 degrees.

---

## 11. Primary Scene selection and jitter injection

Implement:

~~~text
on_scene_layer_update
~~~

Only process a Scene when:

~~~text
RE4
active mode
temporal-ready
layer != nullptr
layer->is_fully_rendered()
sdk::get_primary_camera() != nullptr
layer->get_camera() == sdk::get_primary_camera()
valid renderer frame id
~~~

### 11.1 Six SceneInfo variants

Build the same six-entry set used by the successful diagnostic:

~~~text
get_scene_info()
get_depth_distortion_scene_info()
get_filter_scene_info()
get_jitter_disable_scene_info()
get_jitter_disable_post_scene_info()
get_z_prepass_scene_info()
~~~

The primary/main SceneInfo must exist.

Other variants may be null; skip null variants.

### 11.2 Preserve unjittered history

Maintain per-variant history:

~~~text
previous unjittered projection
previous view
previous scene frame
history-valid flag
~~~

Never store the already-jittered current projection as the next frame's unjittered history.

### 11.3 Current-frame mutation

For each non-null SceneInfo, preserve the diagnostic's exact matrix convention:

~~~cpp
auto current_projection = info->projection_matrix;
auto current_view = info->view_matrix;

auto previous_projection =
    history_valid ? previous_unjittered_projection : current_projection;
auto previous_view =
    history_valid ? previous_view_matrix : current_view;

previous_projection[2][0] += matrix_jitter_x;
previous_projection[2][1] += matrix_jitter_y;
info->old_view_projection_matrix = previous_projection * previous_view;

// Save unjittered current state for the next frame before mutation.
previous_unjittered_projection = current_projection;
previous_view_matrix = current_view;

info->projection_matrix[2][0] += matrix_jitter_x;
info->projection_matrix[2][1] += matrix_jitter_y;
info->inverse_projection_matrix = glm::inverse(info->projection_matrix);
info->view_projection_matrix = info->projection_matrix * info->view_matrix;
info->inverse_view_projection_matrix = glm::inverse(info->view_projection_matrix);
~~~

### 11.4 Frame gaps

If:

~~~text
previousFrame + 1 != currentFrame
~~~

invalidate temporal history before constructing old_view_projection_matrix.

Do not bridge stale projection/view across a frame gap.

### 11.5 SceneInfo ownership

SceneInfo objects are engine-owned.

Use them within the current callback.

Do not AddRef/Release them or retain stale pointers for later writes after load/device lifecycle changes.

---

## 12. Production reset/history state

Add a reset state separate from runtime DLL/context state.

Track at least:

~~~text
history_invalid
load_transition_active
inhibit_departure_pending
pause_previous
remembered_normal_inhibit_valid
remembered_normal_inhibit
departure_inhibit
startup_mid_load
post_pause_rebaseline_candidate_valid
post_pause_rebaseline_candidate
post_pause_rebaseline_stable_count
first_valid_frame_reset_pending
~~~

The first valid temporal frame after initial activation must have reset_history=true.

### 12.1 Generic reset triggers

Invalidate history on:

- Off -> active;
- active mode change;
- display-size change;
- queried render-size change;
- D3D12 device/context recreation;
- frame gap;
- Color identity change;
- Depth identity change;
- Velocity identity change;
- missing/invalid temporal input after history was valid;
- Load Save transition.

### 12.2 Narrow Load Save singleton access

Do not retain the diagnostic's broad manager/field enumeration.

Resolve only:

~~~text
chainsaw.SceneLoadZoneManager
    static get_Instance
    field _Pause

chainsaw.GameSituationManager
    static get_Instance
    field InhibitBit
~~~

Use narrow reflection helpers and fail safely if either type/method/field is unavailable.

For InhibitBit, do not assume the observed 0xB9 value.

Read its integral field value according to the reflected field width, or otherwise use an ABI-safe narrow helper verified against the runtime type.

Do not enumerate every field every frame.

### 12.3 Load transition algorithm

Capture 22 proves that InhibitBit can leave its normal value **before** _Pause rises:

~~~text
normal InhibitBit
    -> different value

~77 frames later in the captured load:
    _Pause false -> true
~~~

Therefore the normal inhibit baseline must not be overwritten every frame while load_transition_active is still false.

Use this state machine.

#### Establishing the normal baseline

On the first valid observation with:

~~~text
_Pause == false
AND no load/departure state is active
~~~

capture current InhibitBit as remembered_normal_inhibit.

Once remembered_normal_inhibit is valid, do **not** replace it merely because a later frame reports another InhibitBit value.

#### Early inhibit departure

If:

~~~text
_Pause == false
AND remembered_normal_inhibit is valid
AND current InhibitBit != remembered_normal_inhibit
~~~

then:

- set inhibit_departure_pending=true;
- record departure_inhibit=current value for diagnostics;
- freeze remembered_normal_inhibit at the old normal value;
- invalidate projection/frame history immediately;
- reset the jitter sequence;
- keep waiting for _Pause confirmation or a return to the frozen baseline.

If current InhibitBit returns to remembered_normal_inhibit before _Pause ever rises:

- clear inhibit_departure_pending;
- keep history invalid until the next fully valid frame;
- that next valid frame gets reset_history=true.

Do not promote departure_inhibit to the new normal value merely because _Pause has not risen yet.

#### Pause confirmation

If _Pause becomes true at any point:

- enter load_transition_active=true;
- clear inhibit_departure_pending;
- keep the previously remembered normal inhibit value unchanged;
- invalidate all temporal history;
- reset jitter;
- do not publish an accumulating-history frame.

This also applies when _Pause is already true on the first observation. In that case:

~~~text
startup_mid_load=true
load_transition_active=true
history invalid
~~~

and no pre-load normal inhibit value is invented.

#### Normal recovery when a pre-load baseline exists

After _Pause becomes false, do not resume immediately.

When remembered_normal_inhibit is valid, recovery requires:

~~~text
_Pause == false
AND current InhibitBit == remembered_normal_inhibit
~~~

Only then:

- end load_transition_active;
- clear departure/startup fallback state;
- keep first_valid_frame_reset_pending=true;
- the next fully valid temporal frame gets reset_history=true;
- after that frame, normal temporal accumulation resumes.

#### Startup-mid-load fallback when no pre-load baseline exists

If RE4XeSS first observes the game with _Pause already true, there is no evidence-backed pre-load normal InhibitBit to compare against.

Do not hardcode 0xB9 and do not adopt the value seen immediately after _Pause falls.

For this fallback only:

1. wait until _Pause is false;
2. remember the first post-pause InhibitBit as a temporary candidate, but do not recover;
3. require a **subsequent InhibitBit transition** after _Pause is false;
4. treat the new value as a rebaseline candidate;
5. require that candidate to remain unchanged for at least **3 consecutive valid observations**;
6. adopt it as remembered_normal_inhibit;
7. set first_valid_frame_reset_pending=true and recover on the next fully valid temporal frame.

If no post-pause InhibitBit transition occurs, remain history-invalid rather than guessing.

This startup-mid-load fallback is a conservative implementation policy for a case not directly covered by Capture 22; the normal evidence-backed path remains frozen-baseline departure -> _Pause -> return to frozen baseline.

If either singleton/field observation is temporarily unavailable, stay conservative and keep history invalid.

---

## 13. Resource/frame-packet collection at true pre-Overlay

Implement:

~~~text
on_pre_overlay_layer_draw
~~~

This is the final PR 2 frame-builder boundary.

Do not submit GPU work.

### 13.1 Same-frame primary Scene cache

on_scene_layer_update may cache only the minimum identity needed to finish the same frame:

~~~text
current primary Scene pointer
scene render frame id
current jitter
camera metadata validity/value
~~~

At pre-Overlay, require the cached Scene frame to equal the current renderer frame.

If not, skip packet publication and invalidate temporal history.

### 13.2 Resolve resources

At true pre-Overlay:

Color:

~~~text
overlay->get_main_target_state()
    -> get_native_resource_d3d12()
~~~

Depth and Velocity:

~~~text
cached primary Scene
    -> get_depth_stencil_d3d12()
    -> get_motion_vectors_d3d12()
~~~

Optional identity cross-check under Debug Log:

~~~text
Color == scene->get_post_main_target_d3d12()
Color == scene->get_hdr_target_d3d12()
~~~

A mismatch is a production invariant failure for that frame.

Do not switch to another Color source automatically.

### 13.3 Validate extents

Read D3D12 resource descriptions.

Require all three temporal inputs to have:

~~~text
Width  == requested renderWidth
Height == requested renderHeight
~~~

Require Texture2D and sample count 1 unless later evidence requires otherwise.

Do not mutate resources.

### 13.4 Build RE4XeSSFrame

For a fully valid frame, populate:

~~~text
color/depth/velocity
render size
display size
pixel jitter
motionScaleX =  renderWidth / 2
motionScaleY = -renderHeight / 2
near/far/FOV
resetHistory
frame id
~~~

Construct at most one pointer-bearing RE4XeSSFrame per render frame.

The Color/Depth/Velocity pointers are **borrowed only for the current true pre-Overlay callback**.

Lifetime contract:

~~~text
on_pre_overlay_layer_draw begins
    -> resolve engine resources
    -> validate and construct RE4XeSSFrame
    -> future PR 3 may consume the packet synchronously here
on_pre_overlay_layer_draw returns
    -> resource pointers in that packet are no longer valid for dereference/use
~~~

PR 3 must execute/record its resource use synchronously before the same pre-Overlay callback returns.

Do not queue the pointer-bearing packet for another thread or a later callback.

Do not AddRef/Release the engine resources to extend packet lifetime.

For PR 2 debugging, persist only a metadata snapshot after the callback. If resource identity is useful in that snapshot, store addresses as uintptr_t/opaque identity values; do not expose them as reusable ID3D12Resource* handles.

PR 2 does not pass the transient packet to xessD3D12Execute yet.

---

## 14. Resource identity reset tracking

Track the last valid:

~~~text
Color ID3D12Resource*
Depth ID3D12Resource*
Velocity ID3D12Resource*
render extent
display extent
~~~

If any identity changes after a valid history generation:

~~~text
invalidate history
reset jitter sequence
next valid frame resetHistory=true
~~~

Do not AddRef/Release engine-owned resources merely for identity tracking.

Store borrowed pointer values only inside the active callback and, for cross-frame identity comparison, store their raw address values as opaque identities. Do not treat those saved identities as dereferenceable resource handles.

Clear them on:

- Off;
- device reset;
- runtime shutdown/rebootstrap;
- temporal configuration invalidation.

---

## 15. Off / failure behavior

Off must immediately return PR 2 to native behavior.

When requested mode becomes Off:

- PR 1 runtime shutdown behavior remains;
- temporal-ready=false;
- SceneView callback stops overriding;
- jitter callback stops mutating SceneInfo;
- cached frame/resource identities clear;
- projection history clears;
- jitter sequence resets;
- Load Save temporal state resets.

Do not attempt to write old cached SceneInfo pointers during shutdown.

The next engine callbacks naturally use native RE4 state.

If runtime is Faulted or temporal configuration cannot be established:

- no SceneView override;
- no jitter;
- no valid frame packet;
- native RE4 behavior.

---

## 16. UI/debug status update

Replace the PR 1-only status text once PR 2 temporal setup is active.

Suggested states:

~~~text
Off - native RE4 rendering

Context ready - waiting for display/input resolution

Temporal inputs active - PR2 validation only; XeSS execute not active

Temporal setup unavailable: <reason>
~~~

When Debug Log is enabled, event-driven logs should include:

~~~text
[RE4XeSS][Temporal] mode / display / optimal / min / max
[RE4XeSS][Temporal] jitter phase count
[RE4XeSS][Frame] first valid packet after generation change
[RE4XeSS][Reset] reason=<...>
[RE4XeSS][Reset] load pause entered
[RE4XeSS][Reset] load recovery completed
[RE4XeSS][Failure] temporal invariant failure
~~~

Do not emit full frame data every frame indefinitely.

A bounded bring-up trace for the first 16 or 32 valid frames after each temporal generation is acceptable behind Debug Log.

---

## 17. No-touch / protected code

No behavioral changes in:

~~~text
src/compatibility/xefg/**
existing XeFG logic in src/REFramework.cpp
existing XeFG logic in src/D3D12Hook.*
OptiScaler repository
~~~

Do not add:

- OptiScaler detection;
- backend-specific quality ratios;
- XeFG calls;
- swapchain replacement;
- D3D12 command-list hooks;
- ResourceBarrier hooks;
- descriptor tracing;
- output-handoff logic.

If one appears required, stop and report instead of expanding PR 2.

---

## 18. Expected changed-file set

Expected approximately:

~~~text
CMakeLists.txt                            # generated source list if new file requires refresh

src/mods/re4_xess/RE4XeSSFrame.hpp
src/mods/re4_xess/RE4XeSS.hpp
src/mods/re4_xess/RE4XeSS.cpp
src/mods/re4_xess/RE4XeSSRuntime.hpp
src/mods/re4_xess/RE4XeSSRuntime.cpp
~~~

Small RE4-only SDK helper additions are acceptable only if an existing semantic accessor is genuinely missing.

Do not port RE4TemporalProbe as a production dependency.

Do not modify shared Renderer offsets again unless a concrete missing production accessor requires an RE4-gated addition.

---

## 19. Build validation

Use the existing x64 Release build:

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

PR #59 showed that the repository Build PR workflow currently targets PRs whose base is master, so a feature/re4-xess-based PR may not receive that CI automatically.

Do not treat missing GitHub CI on this branch as proof of build failure.

Record the actual local build result in the PR description.

---

## 20. Static acceptance checks

Before opening the PR, confirm:

### RE4 isolation

- RE4XeSS still constructed only for RE4.
- New callbacks all fail closed outside RE4.

### No producer execute yet

No call to:

~~~text
xessD3D12Init
xessD3D12Execute
~~~

### No D3D12 mutation

No new:

~~~text
ResourceBarrier
ExecuteCommandLists
CreateCommandAllocator
CreateCommandList
CreateCommittedResource for XeSS output
~~~

### No output mutation

No writes to:

~~~text
Overlay TargetState
PrepareOutput TargetState
swapchain backbuffer
~~~

### No protected changes

~~~text
git diff --name-only feature/re4-xess...HEAD
~~~

must show no file under src/compatibility/xefg/ and no D3D12Hook.* change.

---

## 21. Manual RE4 runtime validation

Do not mark runtime items PASS unless actually tested.

### 21.1 Off baseline

Expected:

~~~text
Upscaling Mode = Off
native SceneView size
no bridge jitter
no temporal frame packet
native rendering unchanged
~~~

### 21.2 Quality bring-up

Use one controlled mode first, preferably Quality.

Expected:

~~~text
public xessGetOptimalInputResolution succeeds
display extent comes from active DXGI swapchain
SceneView returned extent becomes queried optimal input extent
Color/Depth/Velocity follow the same extent
swapchain display extent remains unchanged
frame packet becomes valid at true pre-Overlay
~~~

Because PR 2 still does not execute XeSS or hand off an upscaled result, visible image quality is not an acceptance criterion. A low-resolution/jittered native presentation during active PR 2 bring-up is expected as an intermediate development state.

### 21.3 Native AA

Expected:

~~~text
public AA query succeeds
render extent normally equals display extent
jitter/history still active
frame packet valid
~~~

Do not force 1:1 if the public frontend returns a different valid result; record the contradiction and stop for review.

### 21.4 Mode transitions

Test at least:

~~~text
Quality -> Balanced
Balanced -> Native AA
Native AA -> Off
Off -> Quality
~~~

Expected:

- resolution query updates;
- SceneView follows new query only when temporal-ready;
- jitter sequence resets;
- history invalidates;
- first valid new-generation packet has resetHistory=true;
- Off restores native callbacks.

### 21.5 Load Save

With an active mode and Debug Log:

Expected sequence for the Capture 22 ordering:

~~~text
InhibitBit leaves remembered normal value
    freeze old baseline
    history invalid

later:
_Pause false -> true
    confirm load transition
    keep frozen baseline

_Pause true -> false
    still invalid

InhibitBit returns to frozen pre-load baseline
    recovery armed

first valid gameplay frame
    resetHistory=true
~~~

Also test/inspect the startup-mid-load guard if practical: first observation with _Pause=true must enter history-invalid state without inventing a pre-load baseline.

Do not accept _Pause false alone as recovery.

### 21.6 Resource identity/lifecycle

Observe a normal load/scene transition.

Expected:

- resource identity changes do not crash;
- history resets;
- new valid resources are adopted without AddRef/Release loops;
- no stale SceneInfo writes.

### 21.7 Missing runtime or context fault

Expected:

- no SceneView override;
- no jitter;
- no valid frame packet;
- native RE4 behavior.

---

## 22. Acceptance criteria

PR 2 is complete only when:

- xessGetOptimalInputResolution is exposed through RE4XeSSRuntime.
- Render size comes from the public XeSS frontend query, not hardcoded ratios.
- Display size comes from active DXGI output/swapchain state.
- SceneView override drives Color/Depth/Velocity to the requested render extent.
- ImageQualityRate is untouched.
- Production jitter uses centered Halton(2,3), not the diagnostic four-phase square.
- Jitter phase count is derived from actual display/render ratio.
- The exact same pixel jitter is stored in the frame packet for future PR 3 execute.
- Six SceneInfo variants use the Capture 13 history construction.
- Primary Camera projection remains unmodified.
- Near/far/FOV are captured dynamically.
- Color/Depth/Velocity semantic identities match the research contract.
- Temporal resource extents are validated at true pre-Overlay.
- Motion scales are W/2 and -H/2.
- Load Save reset preserves the pre-departure InhibitBit baseline even when InhibitBit changes before _Pause, then waits for restoration after _Pause.
- Resource identity/size/context/mode discontinuities invalidate history.
- First valid frame of a new temporal generation has resetHistory=true.
- Off/fault paths stop render-size and jitter mutation.
- No xessD3D12Init/Execute occurs.
- No bridge GPU command list/resource/output work occurs.
- Existing XeFG/OptiScaler compatibility code is untouched.
- x64 Release build succeeds.

---

## 23. PR creation requirements

Open as a Draft PR against:

~~~text
feature/re4-xess
~~~

PR description must state:

1. this is PR 2 of the RE4 XeSS production sequence;
2. PR 1 base/merge commit;
3. render size is obtained through the public xessGetOptimalInputResolution frontend;
4. no backend-specific scaling ratio exists in REFramework;
5. SceneView + jitter are active only in RE4 and only when temporal-ready;
6. xessD3D12Init/Execute and output handoff are still absent;
7. existing XeFG compatibility is unchanged;
8. exact local build result;
9. runtime tests actually performed;
10. runtime tests left for the user.

Do not merge automatically.

---

## 24. Stop conditions

Stop and report instead of widening scope if:

- xessGetOptimalInputResolution fails against the intended OptiScaler-provided frontend;
- the runtime returns an invalid/contradictory optimal input range;
- SceneView override no longer causes Color/Depth/Velocity to follow together;
- a required temporal input no longer matches the semantic resource map;
- the six-SceneInfo jitter construction no longer survives to pre-Scene draw;
- Load Save field names/types differ from the proven current RE4 build;
- production reset requires a new broad manager scan;
- implementation appears to require ImageQualityRate;
- implementation appears to require D3D12 command-list/resource-state work before PR 3;
- implementation appears to require Overlay/swapchain output mutation before PR 4;
- any change to existing XeFG compatibility seems necessary.

These are architecture/evidence contradictions and require review before code scope changes.
