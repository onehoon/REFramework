# RE4 XeSS — Native Fallback, View Ownership, and Per-Frame Render Admission Architecture

**Date:** 2026-09-28  
**Status:** Proposed architecture / implementation roadmap; no runtime behavior changes in this document  
**Repository:** `onehoon/REFramework`  
**Source baseline reviewed:** PR #66, `feature/re4-xess-load-state-accessor-diagnostic` at `a341c99e997fc21cc680507ca0bee3ade6740bb6`  
**Target:** Resident Evil 4 (2023), RE4 1.5.9.0, Direct3D 12  
**Test configuration:** REFramework = `dinput8.dll`; OptiScaler = game-directory `dxgi.dll`; Special K absent; **XeFG OFF** for this investigation  
**Relationship to PR #66:** This is a MULTI-PR follow-up architecture. PR #66 remains the draft OutputHandoff/TargetState provenance and XeSS production investigation. Do **not** implement this entire proposal in PR #66.

> **Primary requirement:** Whenever the RE4 XeSS producer is not supposed to process a view/frame, leave that view on its **unmodified native RE4 rendering path**. Never intentionally render a view at a XeSS-reduced internal resolution while already knowing that the corresponding XeSS output cannot be submitted/installed.
>
> “Native” means the size returned by the original `via.SceneView.get_Size` for that view under the current game settings. It does **not** necessarily mean the DXGI swapchain size, forcibly setting `ImageQualityRate` to 100%, or changing user graphics settings. The original getter remains the authority.

---

## 1. Scope and status of evidence

### 1.1 Observed user-facing problem

During initial loading and other screens where XeSS is not visibly operating, the rendered scene sometimes appears to retain the reduced XeSS input resolution, producing severely degraded image quality. The specific newest capture that crashed during startup does **not** independently establish the rendered extent on the last frame: no successful XeSS Execute or installed output handoff was recorded in that capture. Treat the reported poor image quality as user observation and the reachable source paths below as separate evidence. Collect correlated size/Execute/output records before attributing any specific screen to a single path.

### 1.2 Verified implementation facts, not hypotheses

The following are directly supported by the reviewed PR #66 source:

1. `Hooks::view_get_size_hook_internal()` first invokes the original `via.SceneView.get_Size`, then lets mods mutate the returned float pair. Leaving the pair alone preserves the engine's original result.
2. `RE4XeSS::on_view_get_size()` ignores its `scene_view` argument and uses `is_temporal_active()` plus `m_load_observation_valid` to change that result to `m_input_resolution.optimal`.
3. `is_temporal_active()` checks configured/worker context readiness, generations and `m_temporal_ready`; it does **not** check the current Pause/Inhibit load-window state, per-view identity, output-marker eligibility, or an actually installed XeSS result for the current frame.
4. `on_scene_layer_update()` separately checks load state, primary camera and render frame, and injects projection jitter only after those checks.
5. `on_pre_overlay_layer_draw()` performs the decisive checks and `prepare()` / `submit_sync()` / `install()` **after** scene rendering. It can skip execution after the scene was rendered at a reduced extent.
6. `RE4XeSSOutputHandoff::prepare()` uses `m_marker_pending || m_missing_marker`, generation signature, retirement/quarantine conditions, and live resource validation. Its current public `Snapshot` does not expose all pre-admission facts.
7. `RE4XeSSWorker::process_submit()` lazily creates the D3D12 bridge on the first valid submit when `!m_bridge->ready()`. The initial worker service that initializes the XeSS runtime explicitly returns Waiting; it does not submit the first frame.
8. A selected XeSS mode is not proof of an executed frame: worker or handoff may be waiting, draining, quarantined, or faulted.
9. `REFramework::run_imgui_frame(false)` calls `on_frame()` during the BeginRendering application entry, but a user mode request can subsequently occur while drawing the UI. Ordering of *every* native `get_Size` call relative to BeginRendering has **not** been established for every RE4 screen.
10. `RE4XeSSOutputHandoff::restore()` deliberately quarantines unexpected third TargetState identities rather than overwriting their owners. This safety behavior must remain unchanged.

### 1.3 Known architectural requirements

The canonical production architecture already states:

- `doc/RE4_XESS_PRODUCTION_ARCHITECTURE_2026-09-27.md`, section 11: “Never keep low-resolution SceneView active while XeSS execution is unavailable.”
- Native Off preserves the original SceneView size, disables XeSS jitter/submit/handoff, and resets history for subsequent re-enable.
- Actual RE4 Color/Depth/Velocity extents follow SceneView size; DXGI output size remains independent (documented Capture 11).
- OutputHandoff must not release a generation until actual downstream lifetime proof is satisfied.
- No direct output-to-swapchain copy is permitted as a substitute for the pre-Overlay RE Engine handoff.

This proposal makes that already documented Native fallback rule operational without changing the producer/interceptor contract.

### 1.4 Source reference map

All links below are pinned to the reviewed source revision unless explicitly labeled as general documentation:

| Concern | Source |
|---|---|
| Original SceneView getter and mod callback ordering | [src/mods/Hooks.cpp, `view_get_size_hook_internal`](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/src/mods/Hooks.cpp#L1003-L1023) |
| BeginRendering, ImGui, `on_frame` sequencing | [src/mods/Hooks.cpp](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/src/mods/Hooks.cpp#L979-L996), [src/REFramework.cpp](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/src/REFramework.cpp#L963-L992) |
| Temporal readiness and worker snapshot | [src/mods/re4_xess/RE4XeSS.cpp](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/src/mods/re4_xess/RE4XeSS.cpp#L3953-L4066) |
| Load state, update order | [src/mods/re4_xess/RE4XeSS.cpp](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/src/mods/re4_xess/RE4XeSS.cpp#L3734-L3761), [load rules](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/src/mods/re4_xess/RE4XeSS.cpp#L4310-L4493) |
| Size hook, camera and scene jitter | [src/mods/re4_xess/RE4XeSS.cpp](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/src/mods/re4_xess/RE4XeSS.cpp#L4495-L4666) |
| Restore/service/submit/install sequencing | [src/mods/re4_xess/RE4XeSS.cpp](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/src/mods/re4_xess/RE4XeSS.cpp#L4668-L5151) |
| Handoff readiness and marker ownership | [src/mods/re4_xess/RE4XeSSOutputHandoff.cpp](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/src/mods/re4_xess/RE4XeSSOutputHandoff.cpp#L591-L875), [header](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/src/mods/re4_xess/RE4XeSSOutputHandoff.hpp#L16-L101) |
| First-submit bridge initialization and readiness definitions | [src/mods/re4_xess/RE4XeSSWorker.cpp](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/src/mods/re4_xess/RE4XeSSWorker.cpp#L574-L666), [snapshot](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/src/mods/re4_xess/RE4XeSSWorker.cpp#L692-L729) |
| Potential output-view discovery, not verified as gameplay binding | [shared/sdk/Renderer.cpp, `get_output_layer`](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/shared/sdk/Renderer.cpp#L1130-L1160), [`Output::get_scene_view`](https://github.com/onehoon/REFramework/blob/a341c99e997fc21cc680507ca0bee3ade6740bb6/shared/sdk/Renderer.cpp#L2079-L2121) |

---

## 2. Failure mechanism and unavoidable boundary

The current effective sequence can be:

~~~text
BeginRendering / on_frame
    -> update_load_state
    -> update_temporal_configuration
       XeSS context / input extent available
    -> is_temporal_active may become true

via.SceneView.get_Size
    -> original RE4 size
    -> override to XeSS input size
    -> RE4 allocates/renders lower-resolution scene resources

camera + scene updates
    -> may or may not inject jitter depending on later gates

pre-Overlay
    -> restore preceding handoff
    -> service worker
    -> check load state, valid scene, camera, resource identity
    -> check OutputHandoff generation / post-Present marker
    -> prepare / execute / install if all conditions succeed
    -> OTHERWISE skip output, even though scene was already low resolution
~~~

**Defect class A — known-before-rendering skip:** A load state, rejected scene, stale producer generation, currently pending marker or terminal quarantine was already detectable before `get_Size`. A low-resolution override in this case is avoidable and must be prevented.

**Defect class B — newly discovered after rendering:** A resource mismatch, unexpected third TargetState, bridge submission failure, native exception or concurrent lifecycle transition is first discovered in pre-Overlay or later. A late failure cannot be turned into a full-resolution rendering of the **same** frame without re-rendering the scene or some separately proven alternative. The scope of this roadmap is to make the *next* frame Native, preserve all ownership contracts, and produce bounded evidence for the unavoidable affected frame. Do not claim that a status bit can retroactively change rendered GPU resources.

**Defect class C — wrong-view override:** `on_view_get_size` ignores the `scene_view` argument. The common hook is not proven to be called only for the primary gameplay view. Secondary, menu, loading, cutscene and other views must retain the original getter result unless their participation is actually proven and intentionally supported. Do not infer view identity from timing or size alone.

### 2.1 Contrast: intended behavior

~~~text
frame entry:
  default Native (original get_Size)
  observe load / producer / previous handoff / approved view

if a specific view and frame are pre-admitted:
  allow XeSS input size for THAT view
  use the SAME admission for camera/scene temporal mutation
  perform existing pre-Overlay late validations and submit

otherwise:
  leave original SceneView size untouched
  leave projection/jitter untouched
  do not submit a XeSS frame for that view
  retain the configured quality preference; do not rewrite it to Off

post-Present:
  preserve real retirement marker/fence semantics
  make updated handoff availability observable to the NEXT frame
~~~

---

## 3. Non-negotiable invariants

1. Native path never overwrites the original `get_Size` result. There is no forced swapchain-size fallback and no `ImageQualityRate` adjustment.
2. All changes are RE4/D3D12-only; other RE Engine titles and DX11 paths retain their original hooks and behavior.
3. An active XeSS preset is a requested *quality*, not proof that this particular view/frame can execute.
4. Only an explicitly identified, approved SceneView may be modified. Unknown/missing view identity means Native.
5. The size override and projection/SceneInfo jitter follow a consistent **render-frame admission**. No Native scene may receive XeSS-only jitter.
6. A new XeSS frame is admitted only when all *currently knowable* preconditions are satisfied. All post-render checks remain mandatory.
7. Never require `bridge.ready()` for the **first** candidate frame if the bridge is initialized lazily from `process_submit()`. Do not build a first-frame deadlock.
8. A transient blocked frame invalidates temporal history; the next accepted packet carries `resetHistory=true`. Do not destroy an otherwise healthy XeSS runtime solely because the marker was late.
9. Never synthesize post-Present markers, CPU/GPU wait for retirement, release pinned resources without proof, or force-clear quarantine.
10. Never overwrite an unknown third TargetState with a stale saved original. Preserve current fail-closed OutputHandoff ownership handling.
11. Do not broaden this work into XeFG, OptiScaler backend selection, OptiScaler reset implementation, NGX substitution or swapchain-copy fallback.
12. Disable new diagnostics by default; never reuse the retired writer hook or make new provenance hooks a prerequisite for Native fallback.
13. A missing or stale render-frame token must fail Native, not reuse the last admitted XeSS policy.
14. A device/mode/generation change invalidates future low-resolution admission immediately; previously submitted generations still obey existing lifetime rules.
15. Keep the saved XeSS mode unchanged during automatic Native fallback. The user need not toggle Off to recover from normal load/transition states.

---

## 4. Proposed boundaries and data contracts

The architecture deliberately adds a small admission layer to existing components rather than a second renderer, process-global mode switch, or separate GPU synchronization machine.

### 4.1 Configuration/readiness (existing)

Existing owners remain:

- `RE4XeSSWorker` owns the XeSS runtime and bridge, device/queue and producer configuration.
- `RE4XeSS` owns requested mode, control/device reset generations, LoadAccessor semantics and frame-local rendering decisions.
- `RE4XeSSOutputHandoff` owns its TargetState, marker, downstream fence, retirement and quarantine.
- `Hooks.cpp` only exposes the original SceneView callback; do not change its universal callback contract.

`m_temporal_ready` retains its meaning as temporal configuration readiness. Do **not** make it synonymous with successful output for the current frame.

### 4.2 New per-frame Render Admission (proposed)

Conceptual, NOT copy/paste implementation:

~~~cpp
enum class RenderPath : uint8_t {
    Native,
    XeSSCandidate
};

enum class NativeReason : uint8_t {
    None,
    ModeOff,
    UnrecognizedView,
    FrameTokenUnknown,
    LoadSnapshotInvalid,
    LoadingOrInhibitTransition,
    ProducerNotReady,
    ProducerFaultedOrDraining,
    GenerationStale,
    DisplayOrInputInvalid,
    HandoffMarkerPending,
    HandoffRetiring,
    HandoffQuarantined,
    DeviceUnavailable
};

struct FrameRenderPlan {
    RenderPath path{RenderPath::Native};
    NativeReason reason{NativeReason::FrameTokenUnknown};

    uint64_t render_frame{};
    uint64_t control_generation{};
    uint64_t device_reset_generation{};
    uint64_t admission_epoch{};

    uintptr_t approved_scene_view{}; // identity only, not an owned pointer
    uint32_t input_width{};
    uint32_t input_height{};

    // Report size-override evidence separately; do not assume a getter ran.
};
~~~

The exact representation may be smaller. No broad standalone state machine is needed. The plan is **immutable per publication** and is valid only for the exact frame/generation/approved-view identity. There must not be an unqualified process-wide `allow_low_resolution` bool.

Do not store a raw SceneView pointer as an owned reference, dereference it asynchronously, or treat its numerical value as permanently valid across scene changes. Cache the identity only for the validated frame or bounded view generation.

### 4.3 New read-only handoff admission (proposed)

The current handoff `Snapshot` does not include `m_marker_pending`, `m_missing_marker`, `m_retirement_requested`, `m_hard_quarantined`, `m_bridge_writer_uncertain` or `m_device_removed`.

Expose a tightly scoped POD snapshot or equivalent lock-protected query, e.g.:

~~~cpp
struct HandoffAdmissionSnapshot {
    bool has_generation{};
    bool installed{};
    bool marker_pending{};
    bool missing_marker{};
    bool retirement_requested{};
    bool hard_quarantined{};
    bool bridge_writer_uncertain{};
    bool device_removed{};
    bool identity_mismatch{};
    uint64_t generation{};
};
~~~

**No mutation** in the query: no `restore()`, `prepare()`, `poll_retirement()`, AddRef/Release, fence signaling or calls through unvalidated Overlay pointers. Reads must follow existing synchronization ownership; use existing `m_retirement_mutex` for the relevant non-atomic fields or a correctly published, coherent POD snapshot, not concurrent unprotected reads. Assess cost before using a lock-based query from a high-frequency native `get_Size` hook; preferably take the snapshot once at a verified frame boundary, then consume a cheap published plan in the hot hook.

An existing handoff `installed=true` alone is **not** a terminal failure. A valid installed previous frame can be restored at the next pre-Overlay. The exact marker/retirement state and generation determine whether a *new* frame may be accepted. A marker pending for the **previous** generation cannot be assumed to have settled because a later presentation callback might run.

A request to retire is not an instruction to destroy a generation in the admission function. Existing lifecycle code remains authoritative.

### 4.4 Actual view binding (to be proved before strict gating)

Existing API exploration candidates:

- `sdk::renderer::get_output_layer()`;
- `sdk::renderer::layer::Output::get_scene_view()`;
- `sdk::renderer::layer::Scene::get_camera()` / `get_view_id()`;
- the `scene_view` parameter of `on_view_get_size()`;
- render-frame ID from `sdk::renderer::get_renderer()->get_render_frame()`.

**Unproven:** The Output-layer SceneView is necessarily the same object that determines the primary pre-Overlay HDR/PostMain Color/Depth/Velocity extent for all supported gameplay states. The SDK `Output::get_scene_view()` performs dynamic offset discovery and has null/fallback assumptions. Do not invoke it on each size hook or assume that pointer equality alone separates menus/loading from gameplay.

A first, passive runtime pass must correlate getter callers/view identity, primary rendered Scene layer, camera, frame ID, original returned size, actual scene resource extents and XeSS output handoff. Only after this correlation may code select an approved view binding. A view reused across menu/gameplay also needs an independently observed state gate. If the relationship cannot be established, explicitly leave the specific view Native until evidence exists.

---

## 5. Stage-by-stage rendering contract

### 5.1 Frame-boundary admission

At a *verified* pre-rendering boundary:

1. Start with a Native plan. Never carry prior approval implicitly.
2. Process existing mode/device reset observations and LoadAccessor state using their existing semantics.
3. Obtain a coherent producer snapshot, handoff admission snapshot, and render frame ID without changing owner state.
4. Reject if requested mode is Off, snapshot is invalid, actual Pause is active, Inhibit departure is suspected, load/rebaseline is active, or producer is faulted/draining/unavailable.
5. Check current mode, quality, device/reset/control generation, valid display/input extents, and exact producer configuration.
6. Reject a known unsettled marker, retirement or quarantine that would prevent the next new output generation. Do not require a previously created bridge for the first candidate frame.
7. Apply the validated view binding to the plan. If no binding is known yet, keep Native.
8. Publish a frame/generation-qualified `XeSSCandidate` plan; it is a provisional admission, not a promise that `xessD3D12Execute` will succeed.

**Important sequencing caveat:** `on_frame()` currently runs through BeginRendering/ImGui. Source alone does not prove that *all* get_Size calls occur after this hook on every startup/menu/load view. Prove the actual ordering with frame-tagged passive logs. If the getter runs before a plan is published, leave original size unchanged and treat that view/frame as Native. Do not “fix” uncertain order with unverified thread assumptions.

**UI caveat:** `request_mode()` can advance `m_control_generation` while the UI is being drawn after `on_frame()`. A plan for the old generation must not authorize size overrides in the new generation. Prefer a generation/epoch check at consumption, and delay adopting new quality into a full new-frame plan if necessary. Do not silently mix two quality ratios in one frame.

### 5.2 SceneView size hook

After the original `get_Size` returned:

~~~cpp
void RE4XeSS::on_view_get_size(
    REManagedObject* view, float* original_result)
{
    // PSEUDOCODE: details depend on proven frame/view ordering.
    // Native is the default; the original result is authoritative.
    if (!original_result || !view)
        return;

    const auto plan = read_current_admission();
    if (!plan.is_current() ||
        plan.path != RenderPath::XeSSCandidate ||
        plan.approved_scene_view != reinterpret_cast<uintptr_t>(view))
        return;

    // Validate original values, plan extents, frame and generations.
    // Then change only this approved view's return value.
    original_result[0] = static_cast<float>(plan.input_width);
    original_result[1] = static_cast<float>(plan.input_height);

    // Record a bounded, frame-qualified override observation.
}
~~~

Do not conflate a getter invocation with actual render-resource allocation. Capture the original and effective size *by this call* and later correlate with real Color/Depth/Velocity extents.

### 5.3 Camera and Scene temporal mutation

`on_camera_get_projection_matrix()` and `on_scene_layer_update()` must consume the **same frame/generation policy**.

- Native: no XeSS-only camera/projection/SceneInfo jitter, no XeSS Scene history advancement, and no XeSS submit.
- XeSS candidate: only the already validated primary scene and camera, with the current policy token.
- Continue to require matching current primary camera/frame and valid near/far/FOV/resource identities.
- Where possible, require evidence that the approved SceneView was actually overridden for this frame before applying the downstream SceneInfo mutations. Do not require that evidence earlier than the verified real callback order; current camera metadata collection may happen before the size getter.
- Never add jitter to a view that remained at its original size solely because the global XeSS mode is non-Off.
- Preserve the exact existing MV conversion, inverse-matrix and history semantics after successful admission.

A single global mutable “size override was seen” bit is not sufficient if multiple views, nested callbacks, or threads can occur. Use frame/view/generation correlation; make ordering assumptions only after capture.

### 5.4 Pre-Overlay execution

Keep the existing order of safety-critical operations:

~~~text
pre-Overlay entry
    -> existing terminal mismatch check
    -> restore prior handoff if the verified same Overlay allows it
    -> existing request_retirement / worker service / retirement polling
    -> reconcile current frame admission and actual worker state
    -> native path: no new Submit/Install, reset temporal history
    -> candidate: validate scene + camera + actual resources
    -> OutputHandoff prepare
    -> worker Submit/Execute
    -> OutputHandoff install
    -> post-Overlay observation
    -> genuine post-Present retirement marker
~~~

Do not return at the top of pre-Overlay merely because the new plan says Native: previous installed handoff state and retirement must still be handled safely. Do not reorder `restore()` versus ownership checks to “make fallback work”; the independent third-TargetState provenance issue is still unresolved.

If the scene was already rendered at reduced resolution but a newly discovered late failure prevents Execute or Install, log the actual stage and make the *next* valid frame Native. This document does not invent a same-frame full-resolution redraw. Existing fail-closed quarantine remains authoritative.

### 5.5 Post-Present and next frame

Maintain existing genuine fence and marker ordering. Publish only observed marker/retirement state; a marker value does not itself prove the Overlay TargetState was unchanged. For a transient missing same-generation marker, skip new low-resolution candidate frames without tearing down an otherwise healthy runtime. The first accepted frame after an interruption uses `resetHistory=true`.

---

## 6. Precise availability policy

| Screen/condition | Pre-rendering result | Explanation |
|---|---|---|
| RE4 Off or non-RE4 game | Native | No RE4 XeSS changes to getter |
| Startup before proven frame/view binding | Native | Original getter unchanged |
| XeSS context not ready / quality not queried | Native | No reduced render extent |
| LoadAccessor missing or unsupported | Native | Fail closed, no guessed gameplay baseline |
| `_Pause=true`, startup-mid-load, Inhibit departure/rebaseline | Native | Load-state facts known pre-rendering |
| Menu, non-primary view, or unrecognized view | Native | Do not infer target from resolution alone |
| Valid gameplay frame, first bridge submit not yet initialized | *Candidate only when all other preconditions are proven* | Lazy bridge creation must remain possible |
| Valid gameplay frame with working handoff generation | XeSS candidate | Still subject to post-render resource and owner checks |
| Known pending post-Present marker blocking new output | Native for new work | Do not manufacture marker completion |
| Generation transition / known retirement drain | Native | Re-admit only after safe transition |
| Known terminal handoff mismatch/quarantine | Native for future size decisions; preserve pinned unknown owner | Native sizing is not permission to overwrite TargetState |
| Late unexpected XeSS Execute/resource/ownership error after low-res scene | Current frame may remain affected; next frame Native | No same-frame redraw in scope |
| Native AA mode with successful XeSS processing | Same-size XeSS candidate | Input=display is temporal AA, **not** Off |

A menu and gameplay might share the same SceneView object or change the scene tree; the table is a desired policy, not evidence that current code can distinguish every screen today. PRs must prove classification before activating a new gate.

---

## 7. The first-frame bootstrap trap

**Do not gate size override solely on `ProducerSnapshot::execution_ready`.**

Observed source relationship:

~~~text
worker service:
  xess context / query / init -> owner configuration
  first service pass: Waiting (no frame submitted)

current first accepted scene:
  low-resolution input resources exist
  pre-Overlay worker.process_submit()
    -> if (!bridge.ready())
         bridge.initialize(...)
    -> bridge.submit(...)
    -> xessD3D12Execute
~~~

If a first candidate requires `bridge.ready()` and the bridge becomes ready only inside `process_submit()`, bootstrap can never happen.

Two permitted options, to decide with evidence:

**Option A — preferred minimal change:** Treat a validated XeSS context, matching current input/output extent, healthy worker and available handoff generation as *candidate* readiness. Permit the first candidate to reach the existing lazy bridge initialization; preserve all post-render checks. A rare first-frame initialization failure can still affect that frame, but the next frame must be Native and diagnostics must identify it.

**Option B — only if tests demand it:** Separate bridge-independent resources/initialization into a proven pre-rendering readiness stage without requiring source Color/Depth/Velocity that does not yet exist. This is a materially larger lifecycle change; do not undertake it speculatively.

No circular “execute_ready before resources; resources only after execute_ready” dependency is acceptable.

---

## 8. State freshness, synchronization and lifetime

- `m_requested_mode`, `m_control_generation` and `m_device_reset_generation` are atomics; producer snapshots are protected by `m_producer_snapshot_mutex`. Do not combine independently sampled fields into an apparently consistent frame token without an appropriate snapshot/recheck.
- Handoff's non-atomic marker/retirement fields are protected by `m_retirement_mutex`. Do not read them directly from the SceneView hook.
- Treat the admission as invalid if frame ID, control generation, device reset generation or an explicit render admission epoch changes before use.
- Do not propagate a plan across resize, device loss, mode change, first/last render-frame discontinuity or validated view replacement.
- Do not add locking to the universal hooks for this RE4-specific feature.
- Avoid long locks, heap formatting, reflection, dynamic scene-tree searches or repeated COM queries in `on_view_get_size`. A cheap immutable policy token or precomputed POD snapshot is preferred.
- The initial admission may be provisional. Post-render evidence can **revoke future** admission but cannot undo the current GPU rendering. Explicitly distinguish these in code/logging.
- A false Native decision costs a frame of XeSS quality/performance; a false XeSS admission can produce an incorrectly scaled frame. Prefer Native for unknown states and gather evidence if that causes excessive fallback.
- Existing OutputHandoff generation, fence and resource ownership remain the source of truth. The admission layer does not own, retain or retire TargetStates.
- Do not claim safety from an “atomic bool enabled” alone. Any necessary synchronization should be justified by observed callback threading and current ownership rules, not hypothetical exotic interleavings.

---

## 9. Minimal telemetry and evidentiary requirements

New instrumentation must be passive, bounded, and disabled by default outside intentional diagnostics. The previous PR66 `HandoffProvenance` opt-in caused a reported startup crash in one test environment; the new fallback must not depend on it, reuse the retired writer RVA, or introduce another unvalidated executable hook.

Suggested normalized fields:

~~~text
[RE4XeSS][RenderAdmission]
  sequence / timestamp / tid
  frameKnown / frameId
  requestedMode / controlGeneration / resetGeneration
  renderPath=Native|XeSSCandidate
  reason=<enum>
  sceneViewId / approvedSceneViewId
  originalViewSize / effectiveViewSize (when getter observed)
  producerContextReady / bridgeExecutionReady / producerDraining / producerFaulted
  handoffGeneration / markerPending / retirementRequested / quarantined
  loadValid / pause / inhibitDeparture / loadTransition
  sizeOverrideObserved
  sceneTemporalMutationObserved
  executeSubmitted / executeResult / handoffInstalled (when known)
~~~

A log entry must never claim `executeSubmitted=true` merely because the frame was admitted. Later stages should emit corresponding events with the same frame/generation token.

Capture windows (bounded by events, not wall-clock sampling alone):

1. A baseline native-only scene and original get_Size calls before XeSS ready.
2. First admitted XeSS frame, with real input and display extents.
3. Menu/loading transition including view identity and LoadAccessor transitions.
4. Last successful OutputHandoff install → post-Overlay → post-Present marker → next pre-Overlay.
5. Quality change and generation transition; include any third-State mismatch.
6. First failure/quarantine and next frame's effective SceneView size.

Avoid logging every `get_Size` invocation, repeated pointer dereference or verbose broad D3D12 tracing. Retain first-N and transition/failure-only records, periodic aggregate counters, and a one-shot shutdown summary. Do not depend on crash-time destructor execution for essential evidence; flush bounded critical errors when needed.

### 9.1 Required proof before enabling view-specific override

A correlation record must show all of:

~~~text
renderFrame
sceneView pointer of original get_Size call
original vs effective getter extent
actual Color / Depth / Velocity extents for the same frame
selected primary Scene layer / camera identity
pre-Overlay semantic color identity
matching output handoff when present
menu / loading / gameplay classification source
~~~

If the SDK output-view lookup is unsuitable for RE4 1.5.9.0, document that negative result and find the actual view relation from passive source/runtime evidence. Do not cast unchecked engine pointers from a guessed layout.

---

## 10. Regression matrix and release gates

Perform changes with XeFG OFF. The existing stock OptiScaler public XeSS interception contract remains the target; the production build may also be tested with the currently used OptiScaler fork, but document binary hash/build and distinguish any fork-only behavior.

| Case | Required result |
|---|---|
| Mode Off from fresh process | Exact original `get_Size`, no jitter, no XeSS submit |
| Startup/initial loading before runtime init | Native extent, even when XeSS mode is selected |
| Valid runtime but Pause=true / startup-mid-load | Native getter, no injected jitter/Execute |
| InhibitBit departure before Pause | Native from the first *observed* departure, history invalidated |
| Load release before stable Inhibit rebaseline | Remain Native |
| Normal gameplay after stable rebaseline | Candidate resumes, first accepted packet `resetHistory=true` |
| Menu, secondary and unknown views | Original getter unchanged |
| First bootstrapped XeSS frame | Bridge initializes successfully without `execution_ready` deadlock |
| Steady XeSS gameplay | OutputHandoff installed, real input/output extents correct |
| Missing same-generation post-Present marker | Native for blocked new work, no fabricated marker or teardown |
| Balanced → Ultra Quality Plus or other quality change | Old generation retired by proof, no known-bad low-res-only interval if pre-detectable |
| Off → On and On → Off | Requested setting respected, native path restored, history reset on re-enable |
| Resize/fullscreen/Alt+Tab/device reset | Do not reuse stale frame/view/producer plan |
| Late resource validation failure | Fail closed, one bounded diagnostic; next frame Native |
| Third TargetState mismatch | Do not overwrite unknown owner, keep quarantine; next admission Native where engine can safely render |
| OptiScaler native execute AV | Preserve existing exception/quarantine handling; no speculative REF workaround |
| Non-RE4 title and DX11 | No new rendering changes |

Where practical, verify **at least 300 consecutive actual public `xessD3D12Execute` success returns** in healthy steady gameplay, not merely OptiScaler's `Upscaling done: true` count. Capture the boundary from native-only → first candidate → sustained output, then actual active-generation quality change. Delayed-marker stability and repeated load/unload transitions are separate required checks.

**Explicit limitation:** Tests must not label an unmeasured late-failure frame “full resolution.” The assertion is stronger for already-known pre-rendering skip conditions; for a post-render failure the acceptance is safe next-frame fallback with no illegal TargetState mutation.

---

## 11. Multi-PR implementation roadmap

Do not squash these phases into one giant PR. Each PR must have a narrowly stated behavioral surface, replayable captures, independent review and a rollback point. Use the then-current verified source hash for each phase; the links in section 1 are the original design baseline, not a promise that PR66's final source will be identical.

### PR A — Passive baseline and callback-order / view-ownership proof

**Scope:** diagnostics + documentation only. No rendering behavior changes.

- Capture `get_Size` original result, view identity, render-frame correlation and actual scene-resource extents.
- Correlate primary Scene layer/camera, Output SceneView, pre-Overlay and post-Present identities with checked, low-rate observations.
- Establish whether the correct admission boundary executes before relevant `get_Size` on startup, loading, menu and gameplay.
- Identify which views are being overridden today; prove or reject proposed SDK binding.
- Capture pre-rendering versus late failures and the relevant handoff marker state without new writer hooks.

**Acceptance:** a documented timeline and an explicit matrix of “proven primary view,” “secondary/unknown,” and any unverified lifecycle ordering. Diagnostic OFF must behave identically to the original build; diagnostic ON must not recreate the reported startup crash. If an anchor cannot be validated, report `unknown` and keep the probe off.

### PR B — Load-state Native fallback / synchronized temporal gates

**Depends on:** callback-order facts from PR A relevant to the load path.

- Move/reuse existing *observed* load-window conditions into pre-render admission.
- Ensure invalid LoadAccessor, observed Pause, Inhibit departure, active load and post-pause rebaseline prevent size override.
- Gate SceneInfo jitter/history and Execute by the same frame policy.
- Preserve Capture 22 InhibitBit semantics; do not guess a static normal value or change pause detection.
- Re-enable XeSS only after existing stable rebaseline and set `resetHistory=true`.

**Acceptance:** startup-mid-load, pre-pause Inhibit departure, Pause release and Load Save return preserve original SceneView size when XeSS is known ineligible. The original Off behavior and normal gameplay are unchanged.

### PR C — Primary SceneView ownership and non-target-view isolation

**Depends on:** verified PR A view-binding evidence.

- Implement RE4-specific approved-view identity and render-frame qualification.
- Keep unrecognized/secondary/menu/loading views Native.
- Avoid dynamic SDK offset discovery or unsafe pointer dereference in the hot `get_Size` hook.
- Invalidate binding on observed scene/view replacement or incompatible generation.
- Couple camera/scene temporal modifications to the approved view/frame.

**Acceptance:** the proven gameplay view alone receives XeSS reduced input size; all tested non-target views retain their original getter results. No blanket rule based on matching dimensions or camera names.

### PR D — OutputHandoff pre-admission and initial bridge bootstrap

**Depends on:** verified PR B/C frame/view policy.

- Add read-only handoff pre-admission snapshot with proper synchronization.
- Integrate marker pending, generation retirement and terminal quarantine into the *pre-rendering* decision.
- Avoid per-getter expensive lock/reflection work by publishing a bounded coherent frame token.
- Preserve the first-valid-frame lazy bridge init path; do NOT require `execution_ready` before first submit.
- Keep all pre-Overlay resource checks, actual `prepare()` / `submit_sync()` / `install()`, and genuine post-Present marker handling unchanged.

**Acceptance:** blocked new frames do not intentionally render at XeSS-only input extent; first XeSS frame still initializes and runs; >300 successful submissions remain possible; no fence/TargetState ownership regression.

### PR E — Lifecycle epoch consistency and late-failure handling

**Depends on:** PR B/C/D.

- Make mode requests, reset generation, view changes, frame discontinuities and producer snapshots revoke stale admission before future size overrides.
- Handle the BeginRendering/ImGui mid-frame mode-request ordering observed in PR A without mixing quality ratios within one render frame.
- Preserve prior-generation cleanup at pre-Overlay even when current frame is Native.
- On a late rejection or native Execute fault, invalidate future admission/history and log precise stage; **do not pretend to redraw the current frame**.
- Do not make auto-recovery of unknown TargetState ownership part of this PR.

**Acceptance:** quality transitions, Off/On, resize, Alt+Tab, device-reset and late fault all have coherent admission/output histories; no stale candidate leaks across generations.

### PR F — End-to-end regression, diagnostics reduction and canonical handoff

**Depends on:** PR B–E and separate PR66 OutputHandoff safety status.

- Run the complete matrix in section 10 with explicit verified binary identities.
- Validate performance/no excessive Native thrash under healthy playback.
- Keep only bounded operational diagnostics; remove obsolete experimental logging.
- Update `RE4_XESS_PRODUCTION_ARCHITECTURE_2026-09-27.md` and `RE4_XESS_BRIDGE_ARCHITECTURE_AND_RE_STATUS_2026-09-26.md` with the implemented guarantees, remaining limitations and references to runtime captures.
- Record resolved cases and separate unresolved incidents without conflation.

**Acceptance:** native screens and successful XeSS screens have measured correct extents, genuinely successful XeSS Execute and lifecycle signals, with no known regression to ownership, replay or performance.

### PR boundaries summary

| PR | Primary change type | Must NOT absorb |
|---|---|---|
| A | Passive discovery | Native behavior fix; retired writer hooks |
| B | Load-aware admission | Unproven view layout or new GPU lifecycle |
| C | Per-view isolation | Speculative marker/retirement redesign |
| D | Handoff readiness preflight | Quality transition ownership rewrite |
| E | Frame/generation consistency | Same-frame GPU rerender architecture |
| F | Validation / documentation cleanup | New XeFG or OptiScaler features |

These are proposed logical PR slices, **not** already-created PR numbers or a mandate to implement all phases when earlier evidence shows a smaller sufficient design. Combine only tightly coupled details within a phase; do not combine all phases for convenience.

---

## 12. Independence from existing open investigations

**OutputHandoff third TargetState writer:** PR #66 currently tracks an unexpected object at the same Overlay+0x90 slot following an active quality change. This plan must **not** infer its writer, restore a saved pointer over an unknown owner, or clear hard quarantine. Native rendering policy controls future `get_Size` decisions, not foreign TargetState ownership.

**OptiScaler native XeSS Execute access violation:** An older OptiScaler proxy build had an internally null global-pointer write in `dxgi.dll` at a precisely observed RVA. This is a separate owner/binary problem, not evidence that SceneView size caused the fault. Do not patch REF rendering to mask it.

**HandoffProvenance startup crash:** A recent opt-in debug run reportedly crashed before a first XeSS Execute and reported writer instrumentation not armed. Keep its crash investigation separate; it is not proof that writer instrumentation captured a third-State write, nor should the new policy depend on this diagnostic.

**XeFG:** Keep OFF throughout the Native fallback investigation. Once XeSS SR and output lifetime are stable, return to XeFG on its own dedicated validation track.

---

## 13. Explicitly rejected shortcuts

- `if (producer.execution_ready) override size`: deadlocks or permanently stalls lazy first-submit initialization.
- `if (mode != Off) override every SceneView`: repeats the current scope problem.
- `if (Pause is false) override size`: ignores pre-pause Inhibit departure, baseline/rebaseline and unknown views.
- `if (get_output_layer()->get_scene_view() == scene_view) trust gameplay` without runtime proof: one pointer can span multiple modes, and current accessor is not a cheap validated hot-path lookup.
- “Fix” native fallback by forcing `get_Size` to the swapchain size: overrides legitimate RE4 native/user resolution behavior.
- Change `ImageQualityRate` or a global scale setting: not required by the established RE4 SceneView control path.
- AddRef or restore arbitrary third TargetState, eagerly retire pins, invent downstream markers, or perform CPU/GPU waits.
- Move all worker controls into the SceneView getter, perform D3D12 resource allocation from the getter, or introduce process-wide exception/guard hooks.
- Assert same-frame full native recovery when failure is discovered only after low-resolution scene rendering.
- Bundle Native fallback, provenance crash remediation, native OptiScaler AV and XeFG into one PR.

---

## 14. Handoff instructions for a new code agent

1. Read this architecture alongside the two canonical RE4 XeSS documents and current PR #66 comments.
2. Fetch the **actual current PR66 HEAD** before editing. Source hashes embedded in old logs may be build-base stamps rather than exact local source identities.
3. Start with PR A as passive evidence gathering, or explain with concrete existing logs why its relevant evidence is already sufficient. Do not jump straight to an assumed SceneView-pointer match.
4. For each PR, state the precise pre-rendering invariant improved and the paths unchanged. Provide file/line/ABI proof for any new RE Engine lookup.
5. Reproduce with **XeFG OFF**, stock OptiScaler XeSS frontend where applicable, and only the intended diagnostic flags.
6. Deliver a fresh-process Native→XeSS→Native→XeSS timeline with actual getter/texture extents, actual public Execute results and OutputHandoff lifecycle.
7. Leave PR #66 **Draft / unmerged** until its own independent safety gates are satisfied. This roadmap does not certify PR66's third-State provenance, writer ownership, or native-AV tracks.

**End state:** RE4 stays visibly native wherever XeSS is known to be ineligible, limits the SceneView override to the proven intended view, and only engages reduced scene rendering when the current frame is reasonably expected to reach its real pre-Overlay XeSS output handoff. Any remaining post-render failure is explicit, bounded, safe for future frames, and never “fixed” by violating RE Engine TargetState ownership.

## 15. PR66 bounded exception — known LoadAccessor Native admission (2026-10-01)

The user requested a **narrow incremental fix on PR66** for the observed load-window regression, rather than implementation of PRs A–F together. The September 30/October 1 paired capture includes `scene-view-size` frame 1037 (original 2560x1440, overridden 1969x1107) during an already-established startup Pause/load window, with repeated `load-history-invalid` pre-Overlay skips and no successful XeSS install until frame 3764. That observation warrants a safe known-state correction without claiming complete per-view/per-frame ownership proof.

**Incremental implementation (PR66; not the full architecture):**

- Add one small, side-effect-free `RE4XeSSLoadEligibility` predicate, using only existing `update_load_state()` facts: observation valid, Pause history observed and not currently paused, frozen normal InhibitBit baseline valid, no pre-Pause departure, no load transition and no startup rebaseline pending.
- Consume that same predicate in `on_view_get_size()` *before* calling the existing size-decision helper, in `on_camera_get_projection_matrix()` and in the existing scene/pre-Overlay load checks. A known load window returns the original `SceneView.get_Size` result; no forced swapchain extent or `ImageQualityRate` change. A healthy normal gameplay frame remains eligible for the existing lazy first XeSS submit.
- **Deliberately do not add the load predicate to `is_temporal_active()`**: the pre-Overlay `!is_temporal_active()` branch requests handoff retirement; making load windows appear to be an inactive producer there could disrupt an otherwise healthy previous generation. The existing pre-Overlay restore, worker service, retirement polling and genuine marker/fence contract remain unchanged.
- Retain the existing `load-history-invalid` skip/invalidation and next accepted packet's `resetHistory=true`. Log a bounded `native-load-window` getter reason when the temporal configuration itself is ready but load eligibility is false.
- Deterministic tests cover observed Pause, unknown Pause/baseline, missing LoadAccessor, Inhibit departure, transition/rebaseline and normal recovery; the original getter result is preserved rather than substituted with the display extent.

**Explicit limits:** this fixes only load conditions **already observed before the size hook**. The entire frame/view ownership, all callback ordering on every RE4 screen, secondary-view isolation, marker pre-admission, and newly discovered post-render failures remain PR A/C/D/E/F responsibilities. This change does **not** assert full Native fallback for an unobserved status change occurring after `get_Size` or for a marker that becomes unavailable after rendering. It does not establish final UI pixels or the downstream GPU consumer. Runtime Native→XeSS→Native load transitions are still required for acceptance. Keep PR66 Draft/unmerged.
