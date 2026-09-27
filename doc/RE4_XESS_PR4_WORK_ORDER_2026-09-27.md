# RE4 XeSS Production — PR 4 Work Order

Base branch: feature/re4-xess  
Production code baseline before PR 4 documentation: 4e185455e8b49ee496b20f79cfb152e33243c906  
Implementation must branch from the latest feature/re4-xess so this work order and architecture clarification are included.  
PR target: feature/re4-xess  
Suggested implementation branch: feature/re4-xess-pr4-output-handoff  
Date: 2026-09-27  
Scope: engine-visible display-resolution XeSS output TargetState, pre-Overlay handoff installation, downstream RE4 Overlay/UI/final-output reuse, next-frame restoration, and resize/device/failure lifecycle  
Do not merge without review.

Primary architecture:
- doc/RE4_XESS_PRODUCTION_ARCHITECTURE_2026-09-27.md

Research evidence:
- doc/RE4_XESS_BRIDGE_ARCHITECTURE_AND_RE_STATUS_2026-09-26.md

Previous production milestones:
- PR 1: runtime shell
- PR 2: temporal frame builder
- PR 3: detached real XeSS execution
- PR 3 merged as 4e185455e8b49ee496b20f79cfb152e33243c906

---

## 1. Objective

PR 4 makes the real XeSS result visible through RE4's existing downstream presentation path.

~~~text
render-resolution RE4 scene
    |
    | true pre-Overlay
    v
public XeSS execute
    |
    | output directly into an engine-visible
    | display-resolution HDR TargetState resource
    v
replace Overlay main TargetState
    |
    v
original RE4 Overlay draw
    |
    v
original later RE4 output/composite work
    |
    v
native final DrawInstanced(3,1,0,0)
    |
    v
native swapchain / Present
~~~

PR 4 must preserve RE4 Overlay/UI, the native final screen-out pass, active swapchain ownership, existing OptiScaler/XeFG presentation compatibility, and the standard public XeSS producer contract.

PR 4 must not copy XeSS output directly to a swapchain backbuffer.

---

## 2. Code-review findings

### 2.1 Overlay main is the first handoff anchor

Current verified semantic relationship:

~~~text
on_pre_overlay_layer_draw

Overlay main TargetState native resource
    ==
Scene PostMainTarget
    ==
Scene HDRTarget
~~~

Use this same semantic object as the output re-entry anchor.

### 2.2 Do not use PrepareOutput

Current RE4 research proves PrepareOutput / OutputTargetState owns or contains active swapchain backbuffers.

It is presentation state, not the scene-color TargetState.

Also, the current PrepareOutput setter manually AddRefs the incoming state and is not the same balanced ownership surface as Overlay main.

PR 4 must not modify PrepareOutput or OutputTargetState.

### 2.3 Overlay main already exposes balanced intrusive ownership

Current code exposes Overlay::get_main_target_state() as a reference to sdk::intrusive_ptr<TargetState>.

Use that reference directly.

Assignment through sdk::intrusive_ptr balances release/add_ref automatically.

Do not manually drain or normalize engine refcounts.

### 2.4 TargetState clone already deep-clones RTV/Texture

TargetState::clone(new_dimensions):

- copies the TargetState descriptor;
- clones each RTV;
- clones the RTV Texture at requested dimensions;
- updates rect for RTV0;
- creates a new engine TargetState.

Use this existing engine creation path.

Do not duplicate TargetState/RTV/Texture binary layouts inside RE4XeSS.

### 2.5 Do not restore at post-Overlay

Capture 9 brackets original Overlay draw:

~~~text
on_pre_overlay_layer_draw
    -> original Overlay draw
    -> on_overlay_layer_draw
~~~

But Capture 28 observed later HDR/final-output work after the post-Overlay boundary.

Restoring inside on_overlay_layer_draw can therefore remove the handoff too early.

Keep the handoff installed through the rest of the frame.

Restore it at the next true pre-Overlay callback, before collecting the new frame's semantic Color.

This also keeps engine-pointer mutation on the proven render/XeSS owner thread.

### 2.6 Capture 30b remains the acceptance witness

Verified native final sequence:

~~~text
Swapchain 0x00 -> 0x04
Color     0x04 -> 0xC0

DrawInstanced(3,1,0,0)

Swapchain 0x04 -> 0x00
Present
~~~

The native fullscreen draw must remain.

Do not replace this path.

---

## 3. New production component

Add:

~~~text
src/mods/re4_xess/RE4XeSSOutputHandoff.hpp
src/mods/re4_xess/RE4XeSSOutputHandoff.cpp
~~~

Responsibilities:

- create/recreate display-resolution handoff TargetState;
- validate its native D3D12 resource;
- remember original Overlay main TargetState;
- install handoff state after successful XeSS submission;
- restore previous original state at next pre-Overlay;
- track installation/generation invariants;
- quarantine/clear state on terminal generation changes;
- bounded debug logging.

It must not call XeSS APIs, submit command lists, modify swapchain buffers, modify PrepareOutput, know OptiScaler backends, or know XeFG internals.

---

## 4. Handoff target creation

At a valid true pre-Overlay frame, use the current original Overlay main state as the template.

### 4.1 Template validation

Require:

~~~text
originalState != null
originalState == current Overlay main state
originalState RTV count == 1

original native resource == frame.color
original native resource == Scene PostMainTarget
original native resource == Scene HDRTarget
~~~

Validate original D3D12 desc:

~~~text
Dimension   = TEXTURE2D
Width       = renderWidth
Height      = renderHeight
Format      = R11G11B10_FLOAT
SampleCount = 1
Flags include:
    ALLOW_RENDER_TARGET
    ALLOW_UNORDERED_ACCESS
~~~

Do not hand off from a different semantic target.

### 4.2 Clone to display resolution

Create an engine target equivalent to:

~~~cpp
auto handoff = originalState->clone({
    { displayWidth, displayHeight }
});
~~~

Hold the result in sdk::intrusive_ptr<TargetState>.

Do not retain only a raw TargetState pointer.

### 4.3 Validate cloned state

Require:

~~~text
handoff != null
handoff != originalState

handoff RTV count == 1
handoff native resource != null
handoff native resource != frame.color

resource width  == displayWidth
resource height == displayHeight
resource format == R11G11B10_FLOAT
sample count    == 1

resource flags include:
    ALLOW_RENDER_TARGET
    ALLOW_UNORDERED_ACCESS
~~~

Check typed UAV support using the same D3D12 capability policy already used by PR 3.

Validate the cloned TargetState rect is display-resolution.

If any invariant fails, do not submit XeSS for presentation.

### 4.4 Handoff generation signature

Recreate when any of these changes:

~~~text
D3D12 device identity
original/template TargetState identity
original Color resource identity
display width/height
Color format
~~~

Do not reuse an old-device TargetState.

---

## 5. Fresh-clone resource-state contract

TargetState::clone(new_dimensions) delegates texture creation to RE Engine.

The current wrapper proves object/resource identity and descriptors, but does not expose an API that queries the D3D12 resource's initial state.

Do not silently claim this is already reverse-engineered.

### 5.1 First implementation

For a freshly created, never-engine-submitted handoff texture:

~~~text
first bridge use expected before state:
    COMMON

after XeSS:
    PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE
    0xC0
~~~

For later frames of the same validated handoff generation:

~~~text
expected before state:
    0xC0
~~~

The 0xC0 downstream state is the verified current RE4 semantic Color contract.

### 5.2 Runtime validation gate

The fresh-clone COMMON assumption must be validated during PR 4 runtime testing.

A visual result with no obvious corruption is **not** sufficient evidence.

Required validation evidence:

1. run a dedicated validation session with the D3D12 debug layer enabled **before the RE4 D3D12 device is created**;
2. give the cloned native resource a stable debug name such as `RE4XeSS Handoff Output`;
3. observe the first handoff using exactly:

~~~text
COMMON -> UNORDERED_ACCESS -> 0xC0
~~~

4. observe at least 64 consecutive stable handoff frames using:

~~~text
0xC0 -> UNORDERED_ACCESS -> 0xC0
~~~

5. cover at least one mode transition/recreation cycle;
6. verify there are no D3D12 debug-layer messages attributable to the named handoff resource or bridge command lists for invalid StateBefore, resource-state mismatch, invalid barrier use, or lifetime/use-after-free behavior.

Do not add permanent production debug-layer enablement just to satisfy this test.

If no safe pre-device debug-layer activation path is available in the local validation setup, do **not** mark the COMMON/0xC0 gate PASS from visual behavior alone. Use the narrow handoff-resource state-provenance fallback instead and keep PR 4 unready until the state contract is evidenced.

If D3D12 validation, engine behavior, or the narrow state trace contradicts either:

~~~text
fresh cloned target is usable from COMMON
or
substituted target returns to 0xC0 before later XeSS use
~~~

stop.

Do not guess a different StateBefore and do not add broad D3D12 tracing.

Add the narrowest observation for only the cloned handoff resource, then update the handoff state contract from evidence.

---

## 6. Change RE4XeSSD3D12 from detached output to supplied output

PR 3 currently owns a detached output.

PR 4 normal path writes directly into the validated engine-visible handoff resource.

### 6.1 Output binding

Add a bridge-facing contract such as:

~~~cpp
struct OutputBinding {
    ID3D12Resource* resource{};
    D3D12_RESOURCE_STATES before_state{};
    D3D12_RESOURCE_STATES after_state{};
};
~~~

Extend submit to accept this binding.

### 6.2 Validate supplied output

Require:

~~~text
resource != null
display-resolution extent
R11G11B10_FLOAT
Texture2D
sample count 1
ALLOW_UNORDERED_ACCESS
ALLOW_RENDER_TARGET
typed UAV capability
~~~

### 6.3 Per-slot output writer lifetime pin

Add to each submitted slot:

~~~cpp
Microsoft::WRL::ComPtr<ID3D12Resource> output_pin;
~~~

Acquire before recording.

Keep until that slot's **PR 3 bridge fence** completion is proven.

This pin protects the XeSS/bridge command list that writes the output. It does **not** prove that later RE4 Overlay/final-output GPU command lists have finished reading the handoff resource.

Use the same Signal-failure quarantine rules as Color, Depth, and original Velocity.

### 6.4 Downstream-consumer lifetime is a separate fence domain

Current source review does not establish that RE Engine `RenderResource::release()` defers D3D12 resource destruction until GPU completion.

Therefore PR 4 must not release a handoff TargetState merely because the PR 3 bridge fence is idle.

Use a separate **downstream retirement fence** owned by `RE4XeSSOutputHandoff`.

The existing callback sequence provides the required semantic point:

~~~text
RE4 final engine command lists submitted
    |
Present / Present1 original call
    |
Mod::on_post_present()
~~~

For every frame in which an XeSS handoff was installed and reached the presentation path, `RE4XeSS::on_post_present()` must enqueue:

~~~cpp
activeDirectQueue->Signal(outputRetirementFence, retirementValue);
~~~

The queue/device must match the handoff generation.

Because the Signal is enqueued on the same active DIRECT queue **after** the frame's RE4 downstream command submissions, completion of that retirement value proves that prior GPU consumers of the handoff resource have retired.

No XeSS API may be called from `on_post_present()`.

If the existing presentation compatibility path suppresses mod post-present callbacks for a frame, no retirement evidence exists for that frame. Do not synthesize completion. Keep the affected generation alive/quarantined until a later valid marker for that generation or a confirmed device-removal terminal condition exists.

If the retirement `Signal` itself fails, quarantine the output generation.

### 6.5 Same-generation frame reuse does not require a CPU wait

Normal frame-to-frame reuse of the same handoff texture is ordered by the same DIRECT queue:

~~~text
frame N XeSS write
    ->
frame N RE4 downstream reads/final output
    ->
frame N+1 XeSS write
~~~

The frame N+1 bridge list is submitted later to the same queue, so queue ordering serializes the prior downstream reads before the next write.

Do not CPU-wait on the downstream retirement fence every frame.

The retirement fence exists for **lifetime/destruction/recreation proof**, not normal steady-state reuse.

### 6.6 Remove detached output from normal path

Generation resources still create the converted MV target.

They no longer need to create a separate detached XeSS output for the normal PR 4 path.

Do not keep a hidden direct-to-swapchain fallback.

---

## 7. Output barriers

First use:

~~~text
handoff output:
    COMMON -> UNORDERED_ACCESS

xessD3D12Execute

handoff output:
    UNORDERED_ACCESS -> 0xC0
~~~

Later use:

~~~text
0xC0 -> UNORDERED_ACCESS
XeSS
UNORDERED_ACCESS -> 0xC0
~~~

The UAV-to-SRV transition synchronizes XeSS writes for downstream RE4 reads.

Do not keep PR 3's output-stays-UAV barrier when a transition to 0xC0 is already recorded.

Original Velocity restoration remains unchanged.

Color/Depth input treatment remains unchanged.

### 7.1 State transaction rule

Do not commit the handoff resource's tracked state until ExecuteCommandLists has been issued and Signal succeeded.

If recording/Close/XeSS execute fails before submission, actual output state remains unchanged.

If Signal fails after submission, the output generation is quarantined and its state is unknown for reuse.

Do not install that output into Overlay.

---

## 8. Pre-Overlay transaction order

Refactor on_pre_overlay_layer_draw in this semantic order.

### Step A — restore previous-frame handoff first

Before service_owner_thread or semantic Color lookup:

~~~text
restore previous handoff if still installed on this Overlay layer
~~~

Restoration must run only on the established pre-Overlay owner thread.

### Step B — service runtime/bridge transitions

Run existing owner-thread service.

If Off, draining, faulted, or reconfiguring:

- do not install a new handoff;
- leave restored original state in place;
- clear same-frame caches as appropriate.

### Step C — build and validate semantic packet

Reuse PR 2/3 logic.

At this point Overlay main must again be the original HDR/PostMain state.

### Step D — prepare handoff target

Create/reuse the display-resolution clone.

Do not mutate Overlay main yet.

### Step E — submit XeSS into handoff resource

Use:

~~~text
output.resource    = handoff native resource
output.beforeState = tracked pre-XeSS state
output.afterState  = 0xC0
~~~

### Step F — install only after successful submission and Signal

Only when bridge result == Submitted:

~~~cpp
savedOriginal = layer->get_main_target_state();
layer->get_main_target_state() = handoffState;
~~~

Then record:

~~~text
installed = true
installedOverlay = layer
installedFrame = current frame
~~~

Commit temporal history exactly as PR 3 already does.

### Step G — failure paths

Busy:

- original state remains installed;
- no handoff install;
- invalidate history;
- next valid XeSS submission uses resetHistory=1.

Faulted/quarantined:

- original state remains installed;
- no new handoff;
- follow PR 3 bridge/runtime drain/quarantine behavior.

---

## 9. Restoration contract

The handoff intentionally remains installed after the pre-Overlay callback.

It must survive:

~~~text
original Overlay draw
on_overlay_layer_draw
later RE4 output/composite recording
final native screen-out pass
Present
~~~

Do not restore inside on_overlay_layer_draw.

### 9.1 Restore at next pre-Overlay

At the start of the next frame:

If:

~~~text
installed == true
current Overlay pointer == installedOverlay
current Overlay main state == handoffState
~~~

restore through intrusive_ptr assignment:

~~~cpp
layer->get_main_target_state() = savedOriginal;
~~~

Then clear installation state.

Keep the reusable handoff generation alive.

### 9.2 Mismatch handling

If the same Overlay object exists but current main state is neither the expected handoff nor the saved original:

- do not overwrite it;
- mark a handoff invariant failure;
- fail closed.

Another system changed engine state and must not be stomped.

### 9.3 Renderer/layer generation change

If Overlay identity changed after renderer/device recreation:

- do not dereference/write the saved old Overlay pointer;
- treat that installation as old-generation state;
- retire it through the device-generation lifecycle;
- create a fresh handoff from the new Overlay template.

---

## 10. OutputHandoff ownership

Suggested state:

~~~cpp
sdk::intrusive_ptr<TargetState> handoff_state;
sdk::intrusive_ptr<TargetState> saved_original_state;

Microsoft::WRL::ComPtr<ID3D12Fence> downstream_retirement_fence;
uint64_t next_retirement_value{1};
uint64_t last_signaled_retirement_value{};
uint64_t last_completed_retirement_value{};

Overlay* installed_overlay{};

uintptr_t template_state_identity{};
uintptr_t template_resource_identity{};
ID3D12Device* device_identity{};
ID3D12CommandQueue* queue_identity{};

uint32_t display_width{};
uint32_t display_height{};
DXGI_FORMAT format{};

D3D12_RESOURCE_STATES expected_pre_xess_state{ COMMON };

bool installed{};
bool presented_since_install{};
bool retirement_marker_required{};
bool quarantined{};
~~~

Exact member names may differ.

Do not retain raw engine state pointers without intrusive ownership when lifetime crosses callbacks.

The raw Overlay pointer is identity-only and must not be dereferenced after renderer generation changes.

---

## 11. Lifecycle coordination

### Mode Off

At next pre-Overlay owner callback:

1. restore installed handoff;
2. stop new temporal/XeSS submissions;
3. drain PR 3 bridge work;
4. require the latest downstream retirement marker for the last presented handoff frame;
5. release the old handoff TargetState generation **only after both**:
   - bridge writer fences are safe; and
   - downstream retirement fence completion is proven;
6. if downstream completion cannot be proven, keep the handoff generation quarantined instead of releasing it.

### Quality or display-size change

At next pre-Overlay:

1. restore old handoff;
2. stop old generation;
3. drain bridge slots;
4. require completion of the last downstream retirement marker for the old handoff generation;
5. release/recreate the old handoff only after both writer-side and downstream-consumer completion are proven;
6. if either proof is unavailable, quarantine the old handoff generation;
7. recreate runtime/bridge generation as existing PR 3 policy requires;
8. create new display-resolution handoff;
9. first later successful frame uses resetHistory=1.

### Device reset/removal

on_device_reset remains control-plane only.

It must not directly write Overlay main or destroy XeSS from an arbitrary thread.

For handoff lifetime:

- a bridge fence alone is insufficient;
- if a valid downstream retirement marker for the old generation completed, normal release is allowed after writer fences are also safe;
- if no such marker exists, do not infer downstream completion from Present count, callback return, timeout, or bridge idleness;
- confirmed device removal is a terminal condition for the old D3D12 generation;
- otherwise keep the old handoff TargetState/resource generation quarantined until downstream retirement can be proven.

On next valid owner callback/new renderer generation:

- never write through old Overlay pointer;
- retire/quarantine old bridge/handoff generation using the combined writer + downstream-consumer rules;
- rebuild from the new device and new Overlay template only without reusing old-generation resources.

### Signal-failure quarantine

If Signal fails after ExecuteCommandLists:

- do not install handoff;
- keep handoff TargetState/resource generation quarantined with bridge generation;
- do not release based on callback return, frame count, or timeout;
- retire only under PR 3 safe terminal conditions.

---

## 12. Post-Overlay and post-Present callbacks

It is acceptable to add on_overlay_layer_draw for observation only.

Under bounded Debug Log verify:

~~~text
same Overlay object
main state still == installed handoff state
handoff native resource identity
display extent
~~~

Do not restore or mutate from this callback.

`on_post_present()` has a different mandatory role: when the current frame presented an installed handoff and the active queue/device still match the handoff generation, enqueue the downstream retirement fence Signal described in section 6.4.

This callback must not call any public XeSS API and must not release the TargetState immediately. It only records retirement evidence for later owner-thread teardown/recreation.

---

## 13. UI/status

Suggested states:

~~~text
Off - native RE4 rendering

XeSS execute active - waiting for output handoff

XeSS output handoff active - RE4 presentation path

XeSS output handoff draining

XeSS output handoff unavailable: <reason>
~~~

Do not add another user-facing handoff switch.

An active upscaling mode implies production output handoff.

---

## 14. Logging

Use:

~~~text
[RE4XeSS][Output]
[RE4XeSS][Resize]
[RE4XeSS][Failure]
~~~

Useful bounded evidence:

~~~text
frame
original TargetState identity
original Color resource identity
handoff TargetState identity
handoff native resource identity
render extent
display extent
before state
after state
install result
next-frame restore result
post-Overlay still-installed observation
~~~

Log at most the first 32 successful handoffs per generation.

Always log invariant mismatches/failures.

---

## 15. Explicit current unknown

Current research proves original semantic Color state, downstream native final-output window, and the final fullscreen draw.

It does not prove the initial D3D12 state of a newly cloned, never-used engine Texture.

PR 4 may test the narrow COMMON-first contract in section 5.

If that fails, the next step is:

~~~text
narrow handoff-resource state provenance only
~~~

Not:

~~~text
direct swapchain copy
random StateBefore values
global D3D12 tracing
PrepareOutput replacement
descriptor patching without evidence
~~~

---

## 16. Narrow final-draw provenance fallback

First attempt the TargetState handoff without descriptor tracing.

If runtime shows all of:

~~~text
handoff target created
XeSS execute succeeds
handoff installed
handoff survives original Overlay draw
native final fullscreen draw still occurs
but final presented scene does not consume XeSS handoff
~~~

stop.

Do not merge a direct-copy workaround.

The follow-up diagnostic is limited to the already-proven final DrawInstanced(3,1,0,0) on the last engine DIRECT list before Present.

Trace only the SRV/descriptor/resource provenance needed to determine whether that draw still references old Color or another intermediate.

Any correction remains isolated inside RE4XeSSOutputHandoff.

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

Do not modify:

- REFramework_XeFG_PreRetireSwapchainV1;
- XeFG resize/binding lifecycle;
- hook monitor;
- swapchain identity handling;
- OptiScaler producer interception.

Do not add direct backbuffer copy, custom OptiScaler ABI, XeFG producer calls, PrepareOutput replacement, or final descriptor patch without evidence.

---

## 18. Expected changed-file set

Expected approximately:

~~~text
CMakeLists.txt

src/mods/re4_xess/RE4XeSSOutputHandoff.hpp
src/mods/re4_xess/RE4XeSSOutputHandoff.cpp

src/mods/re4_xess/RE4XeSSD3D12.hpp
src/mods/re4_xess/RE4XeSSD3D12.cpp

src/mods/re4_xess/RE4XeSS.hpp
src/mods/re4_xess/RE4XeSS.cpp
~~~

No XeSS ABI change should be needed.

No new external dependency should be needed.

No shared Renderer change should be needed unless a tiny ownership-safe helper is genuinely missing.

If shared Renderer code changes, explain why existing TargetState clone and Overlay intrusive_ptr access are insufficient.

---

## 19. Static acceptance checks

### No presentation bypass

Search new code for:

~~~text
GetBuffer
CopyResource
CopyTextureRegion
backbuffer
Present
~~~

No PR 4 normal path may write XeSS output directly to swapchain.

### PrepareOutput untouched

No RE4 XeSS call to PrepareOutput::set_output_state.

### Engine-visible output supplied to XeSS

Confirm:

~~~text
xess execute output
    ==
handoff TargetState native D3D12 resource
~~~

### Balanced state ownership

Handoff/original state crossing uses sdk::intrusive_ptr<TargetState>.

No refcount-draining loops.

### Slot output pin

Every submitted slot pins supplied output until the bridge writer fence completes.

### Downstream retirement proof

Static review must confirm:

~~~text
PR 3 bridge fence
    = XeSS/bridge writer completion only

PR 4 downstream retirement fence
    = RE4 post-handoff consumer completion
~~~

Handoff TargetState destruction/recreation requires both domains to be safe, unless confirmed device removal terminates the old generation.

`on_post_present()` must Signal the retirement fence on the same handoff-generation DIRECT queue and must not call XeSS APIs.

### Native final path unhooked

No final-draw replacement or swapchain hook added.

---

## 20. Build validation

~~~powershell
cmake -S . -B build-pr4 ^
  -G "Visual Studio 18 2026" ^
  -A x64 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DDEVELOPER_MODE=ON

cmake --build build-pr4 --config Release --target REFramework -- /m
~~~

Required:

~~~text
0 compile errors
0 link errors
git diff --check passes
~~~

PE verification:

- no static libxess.dll import;
- no new runtime dependency beyond current PR 3 set.

---

## 21. Manual RE4 runtime validation

Runtime validation is required because PR 4 changes visible presentation.

### Off baseline

Expected native render size, native Overlay main, no installed handoff, native final output.

### Quality first bring-up

Expected:

~~~text
scene render extent < display extent
display-resolution handoff TargetState created
handoff native resource R11G11B10_FLOAT
XeSS writes into handoff
handoff transitions to 0xC0
handoff installed into Overlay main
~~~

Visible result:

- scene is upscaled to display resolution;
- UI/Overlay remains visible;
- UI is not directly XeSS-upscaled with scene;
- no black frame;
- no stretched viewport;
- no swapchain resize.

### Next-frame restoration

Debug evidence:

~~~text
frame N:
    handoff installed

frame N post-Present:
    downstream retirement Signal queued

frame N+1 pre-Overlay:
    handoff restored to original
    original again matches PostMain/HDR
    new packet collected
    handoff reused after successful submit
~~~

Steady-state reuse must not wait for the retirement fence on CPU; same-queue ordering provides read-before-next-write ordering.

### Post-Overlay persistence

If observation callback exists:

~~~text
on_overlay_layer_draw:
    Overlay main still == handoff
~~~

Do not restore there.

### Native final screen-out remains

Expected:

- native final output path remains active;
- Present still rotates swapchain buffers normally;
- no REF direct-copy to swapchain;
- unique fullscreen draw remains structurally present.

### UI/menu coverage

Test gameplay HUD on/off, pause/menu, inventory, and map if practical.

Expected scene = XeSS output, UI downstream/native, no stale scene, no size mismatch.

### Mode transitions

Test Quality -> Balanced -> Native AA -> Off -> Quality.

For at least one transition, log and verify:

~~~text
last bridge writer fence completes
last downstream retirement fence completes
only then old handoff TargetState generation is released/recreated
~~~

Expected previous handoff restoration, safe dual-domain drain, target recreation where needed, history reset, and native output on Off.

If the downstream marker is missing or Signal fails, the old generation must remain quarantined rather than being released.

### Resize/fullscreen/Alt+Tab

Expected no old TargetState reuse across new device/display generation and correct new display extent.

### Load Save

First valid post-load XeSS submission remains resetHistory=1 and visible handoff resumes only after temporal load gate recovery.

### D3D12 debug-layer state validation

Run the dedicated debug-layer session defined in section 5.2.

PASS requires:

~~~text
fresh handoff:
    COMMON -> UAV -> 0xC0

64+ stable repeated frames:
    0xC0 -> UAV -> 0xC0

one recreation/mode transition:
    no handoff-related invalid StateBefore/state-mismatch/lifetime warning
~~~

The handoff native resource should carry the debug name `RE4XeSS Handoff Output` so relevant messages can be identified.

Unrelated existing game/debug-layer messages do not automatically fail the test; any message involving the named handoff resource, bridge command lists, or the documented transitions must be resolved.

### Long session

Check no refcount growth pattern, no per-frame TargetState churn, no ring starvation, no stale restoration, no downstream retirement backlog during normal operation, and no device removal.

---

## 22. Native XeSS and OptiScaler validation

If native XeSS is available, confirm the engine-visible output target is accepted as R11G11B10_FLOAT display-resolution UAV and downstream shader-readable resource.

With stock OptiScaler:

- same public XeSS call stream;
- same supplied output resource;
- no backend branch in REF;
- alternate SR backend can substitute through OptiScaler;
- RE4 output handoff remains unchanged.

PR 4 does not require XeFG acceptance yet.

---

## 23. Acceptance criteria

PR 4 is complete only when:

- RE4XeSSOutputHandoff exists as a separate engine re-entry component;
- handoff TargetState is cloned from semantic Overlay HDR/PostMain state;
- handoff state is display-resolution R11G11B10_FLOAT;
- XeSS writes directly into the handoff TargetState native resource;
- supplied output is slot-pinned through the bridge writer fence;
- a separate same-queue downstream retirement fence proves completion of RE4 post-handoff consumers before handoff-generation release/recreation;
- steady-state reuse relies on same-DIRECT-queue ordering and does not CPU-wait every frame;
- XeSS output transitions to 0xC0 for RE4 downstream use;
- Overlay main is replaced only after successful submission/Signal;
- original Overlay main is restored at next pre-Overlay before new Color collection;
- handoff remains installed across original Overlay draw;
- PrepareOutput is untouched;
- swapchain is not directly written by REF;
- native final RE4 screen-out remains active;
- UI/Overlay remains downstream of scene upscaling;
- mode/resize/device/load transitions restore or safely retire handoff generations;
- bridge-Signal and downstream-retirement-Signal failures quarantine the affected output generation;
- device removal is never confused with retirement completion;
- COMMON -> UAV -> 0xC0 and repeated 0xC0 -> UAV -> 0xC0 are validated with the D3D12 debug layer or the documented narrow provenance fallback;
- no foreign refcount-draining logic exists;
- existing XeFG compatibility is untouched;
- x64 Release build succeeds;
- visible RE4 runtime validation succeeds.

---

## 24. PR creation requirements

Open as Draft against feature/re4-xess.

PR description must state:

1. PR 4 of the RE4 XeSS production sequence;
2. PR 3 merge/base commit 4e185455e8b49ee496b20f79cfb152e33243c906;
3. detached output is replaced by an engine-visible Overlay-main TargetState output;
4. XeSS writes directly into that cloned display-resolution resource;
5. original Overlay main restores at next pre-Overlay, not post-Overlay;
6. PrepareOutput/swapchain remain untouched;
7. output resource is bridge-fence pinned for writer lifetime;
8. separate post-Present downstream retirement fence and teardown criteria;
9. fresh-clone COMMON / steady 0xC0 debug-layer validation result;
10. native final screen-out remains active;
11. UI/menu runtime results;
12. resize/Alt+Tab results actually performed;
13. stock OptiScaler results actually performed;
14. tests remaining for the user.

Do not merge automatically.

---

## 25. Stop conditions

Stop and report instead of widening PR 4 if:

- cloned TargetState cannot produce required display-resolution HDR resource;
- cloned output lacks typed UAV support;
- fresh clone is proven not to follow the documented first-use state contract;
- substituted handoff does not return to expected shader-readable state;
- Overlay main cannot be safely restored at next pre-Overlay;
- downstream RE4 changes Overlay main unexpectedly;
- visible UI is lost or demonstrably enters the XeSS scene input;
- TargetState handoff does not reach the proven final screen-output draw;
- correction appears to require direct backbuffer copy;
- correction appears to require PrepareOutput replacement;
- correction appears to require existing XeFG compatibility changes;
- downstream RE4 GPU consumption cannot be retired with a same-queue marker and no confirmed device-removal terminal condition exists;
- D3D12 debug-layer or narrow state-provenance evidence contradicts the COMMON/0xC0 contract.

If TargetState handoff does not reach the final draw, the next action is the narrow final-draw descriptor/SRV provenance gate.

Do not solve that contradiction inside PR 4 by guessing.
