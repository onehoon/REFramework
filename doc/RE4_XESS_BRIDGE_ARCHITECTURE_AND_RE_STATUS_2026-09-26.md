# RE4 XeSS Bridge — Architecture, Reverse-Engineering Status, and Implementation Plan

**Repository:** `onehoon/REFramework`  
**Target:** Resident Evil 4 (2023), Direct3D 12  
**Game build under investigation:** RE4 `1.5.9.0`, Steam AppID `2050650`, BuildID `22377325`  
**Date:** 2026-09-26  
**Last research update:** 2026-09-28  
**Status:** Reverse-engineering phase complete through Capture 30b; production implementation moves to `feature/re4-xess`

---

## 1. Purpose and source-of-truth policy

This document is the canonical architecture and reverse-engineering record for the Resident Evil 4 XeSS integration in this REFramework fork.

The earlier ATSBridge repository was useful for architecture exploration, D3D12 tracing, pd-upscaler/PDPerf analysis, and work-order planning. The production direction is now intentionally simpler:

- there is **no separate production ATSBridge DLL/ASI**;
- the complete RE4 bridge is a logical subsystem compiled into this custom REFramework fork;
- REFramework owns the RE4-specific engine integration, temporal-resource discovery, render-size/jitter/reset control, and XeSS producer calls;
- the game-facing producer contract is **standard XeSS D3D12 only**;
- **official upstream OptiScaler is used unmodified** and receives/intercepts the normal XeSS calls through its existing public behavior;
- no private OptiScaler ABI, private export, direct REF↔OptiScaler bridge protocol, or custom OptiScaler fork is part of the production design;
- historical `pd-upscaler` / PDPerf code is an optional reverse-engineering oracle only, not a runtime dependency.

From this point forward, production code and current documentation live in this REFramework fork. ATSBridge should be treated as historical evidence/prototype material.

### 1.1 Research-to-production branch handoff

The reverse-engineering campaign is complete enough to begin the first production XeSS implementation.

Branch roles are now:

~~~text
master
    = current custom REFramework baseline
      including the existing XeFG / swapchain compatibility work

refactor/re4-temporal-diagnostic
    = research/archive branch
      Capture 1-30b diagnostics and evidence
      do not use as the production code base

feature/re4-xess
    = production implementation branch
      created from current master
      implement the RE4-only XeSS producer here
~~~

At the 2026-09-27 handoff, `feature/re4-xess` and `master` both point to `56eac9066489e61c5caf4198f868a1d8a05f50f8`.

Do **not** merge the diagnostic probe wholesale into the production branch. Port only the minimal engine/SDK facts and reusable callback/accessor support required by the production subsystem.

Historical upstream `pd-upscaler` remains a source of implementation ideas and reverse-engineering context only. Do not merge or cherry-pick the branch as the production base, and do not introduce `PDPerfPlugin.dll` as a runtime dependency.

---

## 2. Non-negotiable architecture

### 2.1 Final runtime topology

~~~text
Resident Evil 4
    │
    │ D3D12 renderer
    ▼
Custom REFramework fork
    │
    ├─ RE4-only lifecycle / engine integration
    ├─ render-size control
    ├─ jitter injection
    ├─ proven scene-color selection
    ├─ depth / motion-vector selection
    ├─ camera / reset metadata
    ├─ D3D12 state + execution-point control
    ├─ canonical temporal-frame data
    └─ standard XeSS D3D12 producer
              │
              │ xessD3D12Execute / public XeSS API
              ▼
          libxess.dll
              │
              │ intercepted through normal upstream behavior
              ▼
      OptiScaler upstream master
              │
              ├─ selected SR backend
              └─ FGInput = Upscaler
                        │
                        ▼
                       XeFG
~~~

The term "bridge" in this project means **the RE4-specific subsystem inside REFramework**. It does not mean a separately shipped bridge binary.

### 2.2 RE4-only isolation invariant

The RE4 temporal/XeSS subsystem must never become a generic REFramework feature accidentally.

Required rules:

- registration/construction only when `sdk::GameIdentity::get().is_re4()` is true;
- every callback additionally fails closed for non-RE4 titles;
- no RE4 bridge-specific D3D12 hooks, XeSS contexts, renderer mutations, render-size changes, jitter changes, resource tracking, or reset handling run in another game;
- RE4-specific SDK layout fixes remain behind explicit `is_re4()` branches unless independently proven generic;
- never infer "all TDB 71 games use this layout" from an RE4 observation;
- a build containing the RE4 bridge must behave like the normal custom REFramework build for every non-RE4 title.

### 2.3 OptiScaler boundary

Production REFramework must not:

- call OptiScaler-private symbols;
- depend on a custom OptiScaler export;
- require a modified OptiScaler branch;
- synthesize an NGX/DLSS producer frontend;
- create an FSR producer frontend;
- create an independent duplicate OptiScaler instance.

REFramework behaves like a native XeSS D3D12 game.

---

## 3. Why HUDless scene color matters

XeFG can handle UI through its own interpolation/composition paths, so HUDless input is **not a fundamental XeFG requirement**.

It is, however, important for **the spatial/temporal upscaling stage before FG**.

The historical pd-upscaler path could feed a final/presentation-oriented color resource to the upscaler, which meant HUD/UI could itself be temporally upscaled. That is undesirable because text, icons, reticles, and menu elements can become softened or reconstructed before FG is even involved.

The desired RE4 flow is:

~~~text
3D scene / lighting / post processing
          │
          ▼
HUDless HDR scene color
          │
          ▼
XeSS SR
          │
          ▼
RE4 UI / Overlay composition
          │
          ▼
final presentation image
          │
          ▼
OptiScaler FGInput=Upscaler / XeFG
~~~

Therefore the current reverse-engineering work treats the **pre-UI HDR scene-color boundary** as a production gate for XeSS SR.

XeFG HUDless/UI inputs remain a later optional quality path. They are not the reason this boundary is being identified.

---

## 4. Installed RE4 build truth

Current binary investigated:

~~~text
Path:
E:\SteamLibrary\steamapps\common\RESIDENT EVIL 4  BIOHAZARD RE4\re4.exe

File/product version:
1.5.9.0

Steam AppID:
2050650

Steam BuildID:
22377325

Size:
233,689,576 bytes

SHA-256:
A1082B154105FAC7CA22668FFC1C99A8BFC9F1B945439276C556EDEA7597A5B7

Architecture:
x64

Image base:
0x140000000

SizeOfImage:
0x0E405000

Entry RVA:
0x0E3CB310

Timestamp:
0x69A8F652

TDB version observed at runtime:
71
~~~

Embedded PDB information:

~~~text
runtime_il2cpp.pdb
GUID {107F892E-F947-4554-BB2A-4FE064191D82}
age 1
D:\RELauncher\engines\0\bin\Master\Steam_x64\runtime_il2cpp.pdb
~~~

No matching local PDB is assumed.

Three whole-image Ghidra headless attempts exhausted memory during x86 constant propagation/task processing. Full-image Ghidra analysis is therefore **not a project gate**. Current work uses runtime-guided, engine-semantic, narrowly scoped reverse engineering instead.

---

## 5. Historical pd-upscaler evidence

Pinned reference:

~~~text
praydog/REFramework
branch: pd-upscaler
commit: a24c3459fd5aef7d463eebfb6dcdc715ea989c06
~~~

Relevant files include:

- `src/mods/TemporalUpscaler.cpp`
- `dependencies/pd-perfmod/include/PDPerfPlugin.h`
- `shared/sdk/Renderer.cpp`
- `shared/sdk/Renderer.hpp`

The historical D3D12 path demonstrates real RE Engine temporal integration concepts:

- depth from `Scene::DepthStencilTex`;
- motion vectors from `Scene::VelocityTarget`;
- active camera near/far;
- vertical FOV derived from projection matrix;
- jitter injected into projection `[2][0]` and `[2][1]`;
- render-size control through RE Engine view/config logic;
- TAA disabled unless explicitly allowed;
- `ImageQualityRate=1.0` used to keep temporal inputs aligned;
- evaluation on a render-lifecycle path before final presentation;
- upscaled output copied toward the backbuffer.

Historical pd-upscaler also treated `PrepareOutput::get_output_state()` RTV0/native resource as color. That behavior is useful evidence of an older engine layout, but **must not be assumed correct for current RE4 1.5.9.0**.

Current runtime captures prove that today's analogous PrepareOutput output object is part of the presentation/backbuffer path.

Historical source is therefore a **concept oracle**, not current-build proof.

---

## 6. RE4 1.5.9 SDK compatibility findings

Two current RE4 layout fixes have been validated by runtime capture and remain RE4-specific.

### 6.1 Texture descriptor

RE4 1.5.9 matches the newer/SF6-style descriptor placement:

~~~cpp
if (v >= 73 || gi.is_sf6() || gi.is_re4()) {
    return RenderResource::get_runtime_size() + 0x18;
}
~~~

### 6.2 D3D12 resource container

RE4 1.5.9 resolves the Texture D3D12 resource container through the RE4-specific `0xB8` path / runtime RTTI validation.

These fixes must remain explicitly RE4-gated. They are not evidence that every TDB 71 title has the same layout.

---

## 7. Current semantic resource map

This is the most important current reverse-engineering result.

| Semantic | Current evidence | Status |
|---|---|---|
| Depth | `Scene::DepthStencilTex` → stable native D3D12 resource, 2560×1440, format 19, depth flag | **Strongly verified** |
| Motion vectors | `Scene::VelocityTarget` → stable native D3D12 resource; exact pointer identity with Scene MRT3 | **Strongly verified** |
| Scene MRT0/1/2 | Stable 4-RTV primary Scene TargetState; MRT3 is Velocity; 0/1/2 look G-buffer/material-like | **Verified as scene MRT/G-buffer evidence, not Color** |
| HDR scene color | PostEffect single RTV, 2560×1440, `R11G11B10_FLOAT`, RT+UAV | **Semantically verified** |
| `PostMainTarget` | Exact native-resource identity with the PostEffect color | **Verified** |
| `HDRTarget` | Exact native-resource identity with the PostEffect color | **Verified** |
| Overlay current target | Separate 1920×1080 `B8G8R8A8_UNORM` render target | **Strongly verified** |
| Overlay main target | Native resource resolves to the same HDR/PostMain resource | **Verified** |
| PrepareOutput output object | Current RE4 output object owns/contains the active swapchain backbuffers | **Verified presentation path** |
| Swapchain backbuffers | Exact pointer identity found inside current OutputTargetState | **Verified** |
| Pre-Overlay engine boundary | `on_pre_overlay_layer_draw()` sees Overlay main == `PostMainTarget` == `HDRTarget` in 50/50 paired samples across Static, Camera pan, HUD/menu off, and HUD/menu on | **Verified** |
| Complete pixel-level HUDlessness | No independent pixel/content proof that no earlier UI pass touched HDR/PostMain | **Not independently proven; narrow provenance only if later required** |

---

## 8. Runtime capture history

### Capture 1 — initial current-build resource discovery

Established:

- correct RE4 process/build path;
- stable primary Scene selection;
- stable depth candidate;
- stable motion-vector candidate;
- initial output/color chain observations;
- descriptor alignment mismatch that led to the RE4 `+0x18` correction.

### Capture 2 — corrected Texture layout

Validated the RE4-specific Texture layout correction.

Stable observations included:

- Depth: 2560×1440, format 19;
- Velocity: 2560×1440, format 13;
- PrepareOutput/output objects stable;
- old assumed output Texture path still null at both Scene and EndRendering.

This rejected the simple "same object, just wrong timing" explanation.

### Capture 3 — bounded RTV-tail layout diagnostic

The only meaningful runtime object in the inspected RTV tail was:

~~~text
RTV + 0x98 -> via.render.OutputTargetState
~~~

No current RE4 TextureDX12 object appeared at the old expected tail positions.

This showed that the old shared interpretation of that field as a generic TargetState/Texture path was wrong for this current RE4 output RTV.

### Capture 4 — presentation ownership proven

Current RE4 `OutputTargetState` was correlated directly with the active D3D12 swapchain.

All three actual `IDXGISwapChain::GetBuffer()` resources appeared inside the object at:

~~~text
+0x100
+0x108
+0x110
~~~

This exactly matches the historical `OutputTargetStateDX12` structural shape.

Conclusion:

> Current RE4 PrepareOutput output is presentation/backbuffer state, not the pre-upscale temporal scene color.

The project stopped spending captures on PrepareOutput as the temporal Color source.

### Capture 5 — primary Scene RenderContext MRT

The existing `RenderContext::get_render_target()` accessor was validated against current RE4.

Primary Scene produced a stable four-RTV TargetState:

~~~text
RTV0  format 24  R10G10B10A2_UNORM
RTV1  format 29  R8G8B8A8_UNORM_SRGB
RTV2  format 24  R10G10B10A2_UNORM
RTV3  format 13  R16G16B16A16_SNORM
~~~

RTV3 native resource exactly equals the independent `VelocityTarget` native resource.

Conclusion:

- the RenderContext accessor is valid and semantically useful;
- the four-RTV set is a real Scene MRT/G-buffer pass;
- RTV0/1/2 must **not** be promoted to temporal Color merely because they are stable render targets.

### Capture 6 — PostEffect scene-color candidate

The existing PostEffect layer callback exposed exactly one stable RTV:

~~~text
Format:
DXGI_FORMAT_R11G11B10_FLOAT

Captured size:
2560×1440

Flags:
ALLOW_RENDER_TARGET | ALLOW_UNORDERED_ACCESS
~~~

The resource:

- did not alias Depth;
- did not alias Velocity;
- did not alias Scene MRT0/1/2/3;
- was stable across Static, Camera pan, Character motion, and HUD/menu-on scenarios;
- correlated to the same render-frame IDs as the corresponding Scene samples.

This became the strongest Color candidate.

### Capture 7 — Overlay and PostEffect separation

HUD/menu-on capture proved that Overlay and PostEffect are distinct sibling surfaces under the same Scene.

Overlay current target:

~~~text
1920×1080
format 87 / B8G8R8A8_UNORM
flags 0x1
~~~

PostEffect target:

~~~text
2560×1440
format 26 / R11G11B10_FLOAT
flags 0x5
~~~

Observed callback order for matching render-frame IDs:

~~~text
Overlay -> PostEffect -> Scene
~~~

This invalidated the earlier simplistic model "PostEffect finishes and Overlay later draws directly onto that same current target."

### Capture 8 — semantic HDR/PostMain identity

The first fresh EndRendering provenance sample established:

~~~text
postEffect    == scenePostMain
postEffect    == sceneHDR
overlayMain   == scenePostMain / sceneHDR
~~~

In the captured run:

~~~text
postEffect / PostMain / HDR / Overlay main native resource
= 0x27f45fa0c10

Overlay current native resource
= 0x28064f70530
~~~

Therefore:

> The stable PostEffect `R11G11B10_FLOAT` resource is the current RE4 `PostMainTarget` / `HDRTarget` scene-color resource.

It is no longer merely a format/dimension-based candidate.

The same capture also revealed a diagnostic flaw: EndRendering samples 2–10 reused the frame-25801 anchors while the renderer advanced through later frames. Only the first correlation was fresh evidence. The EndRendering provenance sampler was therefore removed.

### Capture 9 — pre-Overlay engine boundary verified

The paired Overlay pre/post probe was run against PR #58 test merge commit:

~~~text
5b417fcc5740f897d9a0b649bfa6da53c11b8440
~~~

Fifty frame-local pairs were collected:

~~~text
Static screen   10/10
Camera pan      10/10
HUD/menu off    10/10
HUD/menu on     20/20
~~~

Every pre-Overlay sample reported:

~~~text
mainResource == PostMainTarget == HDRTarget
mainMatchesPostMain = true
mainMatchesHDR      = true
~~~

The Overlay main TargetState/resource also remained stable across the original Overlay draw:

~~~text
mainTargetStable   = true   50/50
mainResourceStable = true   50/50
~~~

This verifies the **engine-level pre-Overlay insertion boundary**: before the original `Overlay::draw()` executes, RE4 already exposes the same semantically identified HDR/PostMain scene-color resource through `Overlay::get_main_target_state()`.

The RenderContext current target must **not** be used as the production anchor. It is transient and state-dependent:

- `currentTargetStable=false` in 50/50 paired samples;
- before Overlay, the native current resource was null/unresolved in 39/50 samples and equal to HDR/PostMain in only 11/50;
- after Overlay, the current resource could resolve either to HDR/PostMain or to another menu/UI working resource depending on UI state;
- a second HUD/menu-on session remained on a distinct post-Overlay resource for 10/10 samples while Overlay main still remained the stable HDR/PostMain resource.

Therefore the production Color-selection rule should be based on the **semantic Overlay main / Scene HDR/PostMain relationship**, not on `RenderContext::get_render_target()` at Overlay time.

Capture 9 also confirms that the old EndRendering stale-anchor issue is gone: all 50 pre/post observations were paired on the same sampled frame.

Current conclusion:

> `on_pre_overlay_layer_draw()` is the verified engine-level insertion boundary for RE4 XeSS SR, with `Overlay::get_main_target_state()` / Scene `HDRTarget` / `PostMainTarget` as the stable color anchor.

This proves ordering and semantic resource identity. It does **not** independently prove pixel content (for example, whether an unrelated earlier UI pass could have touched the same resource). A content-sensitive or command-list provenance probe should be added only if later implementation behavior gives a concrete reason to doubt the engine-level boundary.

### Capture 10 — native render/display-size baseline verified

The render/display-size correlation probe was run against PR #58 test merge commit:

~~~text
1ab01c015b8d429353685ad474f6809e7193fb0b
~~~

Fifty complete size snapshots were collected:

~~~text
Static screen      10/10
Camera pan         10/10
Character motion   10/10
HUD/menu on        20/20
~~~

Every sample preserved the production Color invariant and temporal-input extent alignment:

~~~text
Color / HDR / PostMain = 2560x1440
Depth                  = 2560x1440
Velocity               = 2560x1440

colorInvariant          = true       50/50
temporalExtentsAligned  = true       50/50
~~~

The passive `via.SceneView.get_Size` callback correlated to the exact same render-frame ID in every sample:

~~~text
SceneView.get_Size = 2560x1440
sameFrame          = true       50/50
~~~

The active native DXGI output path was also 2560x1440 for all samples:

~~~text
swapchain DXGI desc = 2560x1440
swap format         = 28 / R8G8B8A8_UNORM
buffer count        = 3
D3D12Hook display   = 2560x1440
D3D12Hook render hint = 2560x1440
~~~

This establishes a clean **native-resolution baseline**:

~~~text
SceneView.get_Size
    ==
HDR/PostMain Color extent
    ==
Depth extent
    ==
Velocity extent
    ==
native swapchain/display extent
    ==
2560x1440
~~~

The key limitation is that render and display sizes are identical in this baseline. Capture 10 therefore proves frame correlation and native-size consistency, but it does **not yet prove** that `SceneView.get_Size` is the authoritative internal render-size control when render resolution diverges from display resolution.

Overlay/UI behavior remains independently useful. In the HUD/menu-on captures, the post-Overlay current working target was a distinct 1920x1080 `B8G8R8A8_UNORM` surface in 18/20 samples while Overlay main/HDR/PostMain remained 2560x1440. This confirms that UI working-target resolution can differ from both the scene temporal-input extent and the final native output extent.

The D3D12Hook `renderWidth/renderHeight` values are not treated as authoritative engine render-size evidence: they are existing hook-side hints populated through DXGI resize/target events. Production render-size logic should prefer the engine `SceneView.get_Size` relationship plus actual Color/Depth/Velocity resource extents.

Next decisive validation:

1. keep display/swapchain at 2560x1440;
2. use a controlled RE4 configuration that lowers internal rendering resolution without the probe itself mutating render size;
3. capture the same size signals;
4. determine whether `SceneView.get_Size` and Color/Depth/Velocity move together while DXGI output remains 2560x1440.

Expected proof shape:

~~~text
SceneView.get_Size      = lower internal extent
Color / Depth / Velocity = same lower internal extent
swapchain/display       = 2560x1440
Overlay/UI working size = independently observed
~~~

If this relationship is observed, `SceneView.get_Size` can be promoted to the engine render-size source for the future XeSS bridge. If only the resources shrink while SceneView remains at display size, a different engine render-size source must be identified.

### Capture 11 — SceneView controls internal temporal render extent

The fixed 1920x1080 SceneView split test was run against PR #58 test merge commit:

~~~text
a8fda4ec80fbc8367c356ac87d974e2033f7e47d
~~~

Fifty complete frame-local samples were collected:

~~~text
Static screen      10/10
Camera pan         10/10
Character motion   10/10
HUD/menu on        20/20
~~~

The game-reported native SceneView size remained 2560x1440, while the diagnostic overrode the returned value to 1920x1080. In every sample, all three temporal inputs followed the override together:

~~~text
original SceneView       = 2560x1440
overridden SceneView     = 1920x1080

HDR/PostMain Color       = 1920x1080
Depth                    = 1920x1080
Velocity                 = 1920x1080

colorInvariant           = true   50/50
temporalExtentsAligned   = true   50/50
same-frame correlation   = true   50/50
~~~

The presentation path did **not** follow the SceneView override:

~~~text
DXGI swapchain           = 2560x1440   50/50
D3D12Hook display        = 2560x1440
D3D12Hook render hint    = 2560x1440
~~~

This proves a causal relationship rather than simple native-size correlation:

> `via.SceneView.get_Size` controls the RE4 internal temporal render extent used by HDR/PostMain Color, Depth, and Velocity, while the DXGI swapchain/display extent remains independent.

The D3D12Hook render hint remaining 2560x1440 while the real temporal resources became 1920x1080 also confirms that it is **not** the authoritative scene render-size source.

No `ImageQualityRate`, TAA, jitter, XeSS, swapchain-resize, or resource-state changes were required. Therefore current evidence gives no reason to make `ImageQualityRate` part of the production render-size control path.

For the canonical temporal contract:

~~~text
renderWidth / renderHeight
    = bridge-controlled SceneView size
    = actual Color / Depth / Velocity extent

displayWidth / displayHeight
    = active DXGI swapchain/output extent
~~~

Gate B is considered **closed** for architecture and implementation planning. Production XeSS quality selection can later translate a XeSS quality setting into the requested SceneView input extent without exposing or depending on RE Engine `ImageQualityRate`.

### Capture 12 — native projection jitter baseline is zero

The consecutive-frame projection probe was run against PR #58 test merge commit:

~~~text
320dafd33336a411e32fe96ac089f2b78de80835
~~~

A total of 160 complete temporal samples were captured:

~~~text
Static screen      32
Camera pan         32
Character motion   32
HUD/menu on        64
~~~

Across all 160 samples:

~~~text
SceneInfo projection[2][0] = 0.0
SceneInfo projection[2][1] = 0.0
frameDelta p20             = 0.0
frameDelta p21             = 0.0
~~~

This remained true during active camera motion. Camera pan changed `old_view_projection_matrix` substantially frame-to-frame, while projection `[2][0]/[2][1]` remained zero. Therefore camera/view motion is cleanly separable from projection jitter in the current AA/upscaler-off baseline.

The primary Camera projection and primary Scene projection were correlated on the exact same render frame in all samples:

~~~text
cameraSameFrame       = true   160/160
cameraMatchesScene    = true   160/160
Scene p20 - Camera p20 = 0     160/160
Scene p21 - Camera p21 = 0     160/160
~~~

All six SceneInfo variants historically modified by pd-upscaler also had identical zero X/Y projection offsets in all samples:

~~~text
main
depthDistortion
filter
jitterDisable
jitterDisablePost
zPrepass
~~~

This establishes the current-build baseline required before bridge-controlled jitter is introduced.

A separate depth-projection relationship was also observed consistently in 160/160 samples:

~~~text
Camera:
    p22 ~= -1.000000954
    p32 ~= -0.010000009

SceneInfo:
    p22 ~= +0.000000954
    p32 ~= +0.010000009
~~~

The X/Y projection terms match exactly, while the depth terms are transformed by the engine. This is useful evidence for Gate E, but is not by itself sufficient to mark depth as inverted without further depth-specific validation.

VelocityTarget remained stable at:

~~~text
2560x1440
DXGI_FORMAT_R16G16B16A16_SNORM
flags = RT | UAV
~~~

Current conclusion:

> In the controlled AA/upscaler-off configuration, RE4 contributes no native projection jitter. Bridge-controlled temporal upscaling must therefore generate, apply, track, and report its own jitter sequence.

The next diagnostic stage may safely introduce a known deterministic jitter pattern because the zero-jitter baseline is now proven.

### Capture 13 — deterministic jitter injection and history handling verified

The deterministic four-phase jitter test was run against PR #58 test merge commit:

~~~text
7a18f001d762dfefeda379d47d5cc348623761ad
~~~

A total of **320 jittered frames** were captured. The four diagnostic phases occurred exactly 80 times each:

~~~text
phase 0: (+0.5, +0.5) px   80/80
phase 1: (-0.5, +0.5) px   80/80
phase 2: (-0.5, -0.5) px   80/80
phase 3: (+0.5, -0.5) px   80/80
~~~

At 2560x1440, all 320 frames produced the exact expected historical REFramework projection convention:

~~~text
(+0.5,+0.5) -> (+0.000390625, -0.000694444)
(-0.5,+0.5) -> (-0.000390625, -0.000694444)
(-0.5,-0.5) -> (-0.000390625, +0.000694444)
(+0.5,-0.5) -> (+0.000390625, +0.000694444)
~~~

Across six SceneInfo variants and 320 frames, **1920/1920 mutations** matched the requested values:

~~~text
before projection          = native zero-jitter baseline   1920/1920
historyProjection          = same current jitter           1920/1920
current projection after   = same current jitter           1920/1920
null SceneInfo variants                                      0/1920
~~~

The six verified variants were:

~~~text
main
depthDistortion
filter
jitterDisable
jitterDisablePost
zPrepass
~~~

The injected values also survived from Scene update to the actual pre-Scene draw boundary:

~~~text
jitterDrawCheck allVariantsMatch=true   320/320
~~~

Primary Camera projection remained the unjittered reference on every sampled frame:

~~~text
cameraSameFrame = true   320/320
Camera p20/p21  = 0,0    320/320
~~~

VelocityTarget remained stable at 2560x1440 / R16G16B16A16_SNORM / flags 0x5.

This verifies the current-build RE4 injection path and the historical pd-upscaler history treatment:

> Apply the current frame's jitter to the current projection and also to the previous unjittered projection used to rebuild `old_view_projection_matrix`.

That construction is intended to render the current frame at the jittered sample position without introducing the frame-to-frame jitter delta into engine-generated motion vectors. Capture 13 proves the projection/history construction itself; it does **not** yet prove the contents of VelocityTarget.

Gate C is therefore **closed for RE4 projection injection mechanics**. Production XeSS can later replace the diagnostic four-phase sequence with the XeSS-requested jitter sequence while retaining the verified RE4 matrix/history integration.

### Capture 14 — sparse VelocityTarget readback validates jitter exclusion

The first sparse MV-content readback was run from a local build whose runtime log identified commit:

~~~text
e1cb42c9ca5f7885f8bb2282ec443749f5bce16e
~~~

This happened to match the PR #58 head after the CI compile fix, but future locally built captures do not require exact remote hash identity when the probe signature and behavior match the documented diagnostic.

Readback plumbing completed cleanly:

~~~text
mvSnapshotQueued   8/8
mvReadbackBegin    8/8
mvReadback         72/72   (8 frames x 9 texels)
probe errors       0
fail-closed events 0
~~~

The existing jitter path also remained intact:

~~~text
jitterDrawCheck allVariantsMatch=true   224/224
~~~

The eight static-screen frames covered two complete four-phase jitter cycles. The first cycle contained small transient R/G motion at some sample points, but the same phase pattern did **not** repeat in the second cycle.

For the eight mostly-static grid points excluding the visibly moving lower-left point, the second cycle measured:

~~~text
R raw: min -1, max 0, median 0
G raw: min  0, max 6, median 1
~~~

At 2560x1440, if frame-to-frame projection-jitter delta were present directly in normalized XY motion, the injected four-phase sequence would produce an approximately repeating raw-SNORM signature of:

~~~text
phase 0 -> 1: X ~= -0.00078125  -> raw ~= -26
phase 1 -> 2: Y ~= +0.00138889  -> raw ~= +46
phase 2 -> 3: X ~= +0.00078125  -> raw ~= +26
phase 3 -> 0: Y ~= -0.00138889  -> raw ~= -46
~~~

No such globally repeated +/-26 / +/-46 pattern appeared. The second cycle instead converged to near-zero R/G on static points.

One point at approximately 640x1080 showed real scene motion:

~~~text
R: -5, -112, -9, -3, -7, -4, -5, -4
G: -107, -349, -191, 69, 23, -13, -19, -23
~~~

but those values also did not repeat with the four-phase jitter pattern, so they are scene/object motion rather than a global jitter signature.

The other channels behaved differently:

- B varied spatially and temporally over a much larger positive range;
- A remained constant at raw 32658 / approximately 0.99667 for all 72 texels.

Combined with the historical pd-upscaler behavior for TDB > 67, which passed the original `R16G16B16A16_SNORM` VelocityTarget directly and used `renderWidth/2, -renderHeight/2` motion scales, Capture 14 makes R/G the strong current-build XY candidates. Exact channel mapping, sign, and scale still require directional motion evidence.

Current conclusion:

> The verified RE4 history construction removes the injected projection-jitter delta from sampled static VelocityTarget XY candidates. Motion-vector jitter inclusion is therefore closed as **excluded** for the tested current-build path.

The next diagnostic should use controlled opposite-direction camera pans to establish channel mapping and sign first, while logging the historical pixel-scale interpretation as a candidate rather than assuming it is already proven.

### Capture 15 — horizontal MV channel and sign proven

Capture 15 was produced from a **local working-tree build**. The log's embedded REFramework commit hash remained:

~~~text
e1cb42c9ca5f7885f8bb2282ec443749f5bce16e
~~~

but that hash is **not used as the source identity for this capture** because the runtime clearly contains later uncommitted/local probe behavior:

~~~text
Camera pan right
Camera pan left
samples 5-20 sparse readback
historicalPixelCandidate={x=...,y=...}
~~~

For local diagnostic builds, the probe signature and emitted fields are therefore the authoritative compatibility check when the embedded git hash is stale.

The capture contained:

~~~text
Camera pan right runs   8
Camera pan left runs    7
readback frames         240
readback texels         2160 = 240 x 9
mvSnapshotQueued        240/240
mvReadbackBegin         240/240
probe errors            0
fail-closed events      0
jitterDrawCheck         480/480
~~~

Some directional resets contained effectively no camera motion. Even with those no-motion runs included, samples after the first readback frame showed a clear horizontal polarity:

~~~text
Camera pan right:
    120 analyzed frames
    median frame R = +742
    R sign: +92 / -3 / zero 25
    median frame G = -2

Camera pan left:
    105 analyzed frames
    median frame R = -970
    R sign: +1 / -92 / zero 12
    median frame G = 0
~~~

Removing the three effectively no-motion runs makes the directional evidence stronger:

~~~text
active Camera pan right:
    6 runs / 90 frames
    median R = +1103.5
    R sign = positive 89/90
    median |R| = 1103.5
    median |G| = 7.5
    median |R| / |G| ~= 177x

active Camera pan left:
    6 runs / 90 frames
    median R = -1242.5
    R sign = negative 88/90
    median |R| = 1242.5
    median |G| = 24.5
    median |R| / |G| ~= 41x
~~~

Therefore current-build evidence now establishes:

> **VelocityTarget R is the horizontal/X motion channel.**

Observed horizontal polarity is:

~~~text
camera rotates right -> R > 0
camera rotates left  -> R < 0
~~~

The orthogonal G channel is much smaller during broad horizontal camera motion, which strongly supports the corresponding historical model `G = vertical/Y`, but that mapping and Y polarity still require direct up/down camera motion.

The historical X scale candidate:

~~~text
candidatePixelX = R_snorm * renderWidth / 2
~~~

produced plausible frame-median values that tracked operator pan speed:

~~~text
active right median candidate X ~= +43.1 px/frame
active left  median candidate X ~= -48.5 px/frame
~~~

Individual runs varied with pan speed, including much faster runs. This is good consistency evidence for the historical `W/2` candidate, but it is **not independent scale proof** because Capture 15 did not separately measure actual image-space feature displacement.

Current conclusion:

- R -> X: **HIGH / PROVEN**;
- horizontal polarity: **HIGH / PROVEN**;
- G -> Y: **HIGH / strong hypothesis**;
- vertical polarity: pending direct up/down evidence;
- `W/2` and `-H/2` absolute scale: strong historical/current candidate, still pending an independent witness.


### Capture 16 — vertical MV channel and sign proven

Capture 16 came from another **local working-tree build**. The embedded REFramework commit hash again remained:

~~~text
e1cb42c9ca5f7885f8bb2282ec443749f5bce16e
~~~

As with Capture 15, that hash is not treated as the exact source identity. The runtime probe signature is authoritative: the log contains the current Camera pan up / Camera pan down scenarios, samples 5-20 sparse readback, and historicalPixelCandidate fields expected from the vertical-MV diagnostic.

The capture completed without probe/readback failure:

~~~text
directional runs          8
readback frames           128
readback texels           1152 = 128 x 9
mvSnapshotQueued          128/128
mvReadbackBegin           128/128
jitterDrawCheck           256/256 true
probe errors              0
fail-closed events        0
~~~

Several selected-down runs contained effectively no camera motion. Those zero-motion intervals were retained as useful baseline evidence but excluded from directional sign statistics.

For clearly active vertical motion (median |G| >= 100 at the frame level), the polarity was exact:

~~~text
active Camera pan up:
    42 frames
    G positive 42/42
    median G = +1086.5
    median |G| / |R| ~= 272x

active Camera pan down:
    19 frames
    G negative 19/19
    median G = -1139
    median |G| / |R| ~= 36x
~~~

R remained comparatively small during clean vertical motion. Combined with Capture 15, current-build evidence therefore establishes the complete XY channel mapping and observed camera-motion polarity:

~~~text
R = X
G = Y

camera pan right -> R > 0
camera pan left  -> R < 0
camera pan up    -> G > 0
camera pan down  -> G < 0
~~~

The historical Y conversion candidate:

~~~text
candidatePixelY = G_snorm * -renderHeight / 2
~~~

also produced the expected screen-space orientation:

~~~text
active up   -> candidate Y negative
active down -> candidate Y positive
~~~

with representative active-frame medians of approximately:

~~~text
up   ~= -23.9 px/frame
down ~= +25.0 px/frame
~~~

This is strong sign/orientation consistency for the historical -H/2 convention, but it is **not independent absolute-scale proof** because the candidate itself already contains that scale assumption.

Current conclusion:

- R -> X: **HIGH / PROVEN**;
- G -> Y: **HIGH / PROVEN**;
- horizontal polarity: **HIGH / PROVEN**;
- vertical polarity: **HIGH / PROVEN**;
- projection-jitter delta in MV: **excluded / PROVEN for the tested path**;
- W/2 and -H/2 absolute scale: **strong candidate, still pending independent proof**.

The next diagnostic must compare the historical pixel candidate against a witness that does not use those scale factors. The selected witness is a **rotation-only screen reprojection** computed from consecutive unjittered current/previous camera matrices at the same sparse sample coordinates.


---

## 9. Current HUD/UI boundary model

Current evidence supports this model:

~~~text
Primary Scene / G-buffer
        │
        ├─ DepthStencilTex
        ├─ VelocityTarget
        └─ scene MRTs
              │
              ▼
HDRTarget / PostMainTarget
R11G11B10_FLOAT
              │
              │ Overlay::get_main_target_state()
              │ also resolves here
              │
Overlay layer │
  current RenderContext target
  = separate B8G8R8A8 1920×1080 surface
              │
              ▼
PrepareOutput / later output composition
              │
              ▼
OutputTargetState
              │
              ▼
swapchain backbuffer
~~~

Important hook semantics:

~~~text
on_pre_overlay_layer_draw()
        │
        ▼
original Overlay::draw()
        │
        ▼
on_overlay_layer_draw()
~~~

Therefore the current diagnostic is deliberately bracketing the **original Overlay draw**.

Capture 9 closes the engine-level boundary question:

> Before the original `Overlay::draw()`, `Overlay::get_main_target_state()` already resolves to the same native resource as Scene `HDRTarget` and `PostMainTarget`.

This relationship held in 50/50 paired samples across static, camera-motion, HUD-off, and HUD/menu-on scenarios. The Overlay main target remained stable across the original draw, while the RenderContext current target was transient and sometimes changed to another UI/menu working surface.

Production implication:

- use the semantic Overlay main / Scene HDR/PostMain resource as the Color anchor;
- treat `on_pre_overlay_layer_draw()` as the verified engine-level XeSS SR insertion boundary;
- do **not** use Overlay's RenderContext current target as the production anchor.

Pixel identity alone cannot prove that no unrelated earlier UI pass ever touched HDR/PostMain. That is now a secondary content-level question, not an unresolved resource/boundary-discovery question. Escalate to narrow command-list/content provenance only if later XeSS integration behavior makes it necessary.

---

## 10. Current diagnostic PR behavior

Captures 12-16 now prove:

- native zero projection jitter;
- RE4 projection/history jitter injection;
- sparse VelocityTarget content readback;
- exclusion of bridge projection-jitter delta from sampled static MV;
- R as horizontal/X motion;
- G as vertical/Y motion;
- right/left horizontal polarity;
- up/down vertical polarity.

The active diagnostic now advances to **independent absolute MV scale validation**.

### Directional scenarios

~~~text
Static screen
Camera pan right
Camera pan left
Camera pan up
Camera pan down
Character motion
HUD/menu on
HUD/menu off
~~~

Sparse MV readback remains enabled only for the four directional camera scenarios.

For each directional run:

- samples 1-4 are warm-up only;
- samples 5-20 are read back;
- 16 consecutive readback frames;
- same 3x3 interior grid = 9 texels/frame;
- original game VelocityTarget receives no diagnostic D3D12 barrier;
- disposable engine Texture clone + Present-side sparse readback are retained;
- raw/SNORM RGBA and historical pixel-scale candidates are still logged.

Historical candidates remain:

~~~text
candidatePixelX = R_snorm * renderWidth / 2
candidatePixelY = G_snorm * -renderHeight / 2
~~~

### Independent rotation-only reprojection witness

For the same samples 5-20, the diagnostic now also derives a screen-space displacement from consecutive **unjittered** current/previous primary SceneInfo camera matrices.

The witness:

1. converts each sparse sample coordinate to NDC;
2. unprojects it through the current unjittered projection to obtain the current view ray;
3. transforms only the ray direction through current inverse-view and previous view, using w=0 so camera translation is intentionally excluded;
4. projects the direction with the previous unjittered projection;
5. computes:

~~~text
rotationOnlyReprojection
    = previousScreenPosition - currentScreenPosition
~~~

6. logs the residual:

~~~text
candidateMinusReprojection
    = historicalPixelCandidate - rotationOnlyReprojection
~~~

This is independent of the historical W/2,-H/2 conversion because the expected pixel displacement is derived from camera matrices and render dimensions directly, not from VelocityTarget values.

It is intentionally a **rotation-only** witness. Use clean stationary-scene camera pans so camera translation, moving geometry, parallax, and foreground/object motion do not become scale evidence.

### Capture 17 — rotation-only witness runtime result

Capture 17 used repeated directional runs rather than assuming every reset began with uninterrupted camera motion.

Observed capture shape:

~~~text
Right runs: 4
Left runs:  5
Up runs:    2
Down runs:  2

Total directional runs:       13
jitterFrame:                  416
jitterDrawCheck:              416 / 416 allVariantsMatch=true
MV readback frames:           208
MV sparse texels:             1,872
rotationOnlyReprojectionValid 1,872 / 1,872
probe readback/errors:        0
~~~

Important operator/test-harness note:

> The direction key was already being held when **Reset directional capture** was clicked in the REFramework UI. The click temporarily steals/interrupts camera input, so the first few samples of a run can be zero or low motion until gameplay input resumes. Those initial samples are not failed directional runs and must not be used as scale evidence.

The repeated runs were intentional so each direction would contain a resumed, sustained-motion interval after the UI interaction. Analysis must therefore use the actual MV/reprojection activity and frame IDs, not assume samples 5-20 are uniformly active from their first frame.

Capture 17 provides **strong positive support** for the historical absolute-scale candidates:

~~~text
motionScaleX =  renderWidth / 2
motionScaleY = -renderHeight / 2
~~~

During clean sustained-motion portions, multiple sparse points/runs approach 1:1 agreement between the historical pixel candidate and matrix-derived rotation-only screen displacement, including effective scales near the expected 1280 and -720 values at 2560x1440.

However, agreement is not spatially/run-wise uniform across every active sample. This does **not** reject W/2,-H/2. The current witness attaches each MV frame only to the immediately available current/previous camera-matrix pair, so exact temporal correspondence between the engine VelocityTarget and the camera matrix pair remains an unresolved variable. Because the reset click also creates a sharp stop/restart transition, this timing question should be isolated before adding depth-dependent reprojection.

Gate-D interpretation after Capture 17:

~~~text
R = X                                      PROVEN
G = Y                                      PROVEN
camera right/left polarity                 PROVEN
camera up/down polarity                    PROVEN
jitteredMotionVectors = false              PROVEN for tested path

motionScaleX =  renderWidth / 2             STRONGLY SUPPORTED, not closed
motionScaleY = -renderHeight / 2            STRONGLY SUPPORTED, not closed
MV <-> camera-matrix temporal alignment     ACTIVE
~~~

### Capture 18 — MV scale closed; fixed temporal offset rejected as the primary residual source

Capture 18 exercised the adjacent-frame reprojection witness across 26 repeated directional runs:

~~~text
Camera pan right: 11 runs
Camera pan left:   7 runs
Camera pan up:     4 runs
Camera pan down:   4 runs

Total runs:        26
jitterFrame:       814
jitterDrawCheck:   814 / 814 allVariantsMatch=true
MV readback:       410 frames
MV sparse texels:  3,690
rotation witness:  7,092 / 7,092 valid
probe MV errors:   0
~~~

One Right run was interactively reset at sample 14; the next run began immediately. This does not affect the usable sustained-motion evidence.

The Capture 17 procedure clarification still applies: clicking the REFramework reset control can briefly interrupt held gameplay camera input. Initial zero/low-motion frames are therefore excluded from scale judgment rather than treated as failed directional runs.

The temporal-alignment comparison tested one MV frame N against adjacent camera-matrix pairs:

~~~text
prior pair:      currentFrame = N - 1
same-frame pair: currentFrame = N
next pair:       previousFrame = N  (currentFrame = N + 1)
~~~

Result:

- no single prior/same/next alignment explains all active-motion residuals;
- extending the comparison beyond +/-1 frame likewise does not produce one global fixed lag;
- therefore the remaining disagreement is not primarily a constant MV-vs-camera frame offset.

More importantly, clean rotation-dominated regions repeatedly converge to approximately 1:1 agreement with the historical pixel conversion on both axes and both signs.

Representative horizontal evidence:

~~~text
Right run 5  best dominant-axis ratio ~= 0.994
Right run 9  same-frame ratio         ~= 0.945
Left run 13 prior-pair ratio          ~= 0.951
Left run 16 same-frame ratio          ~= 0.957
~~~

In especially clean spatial runs, the full 3x3 grid clusters near 1.0. Left run 16 produced approximately:

~~~text
0.945  0.964  0.976
0.954  0.968  0.980
0.945  0.954  0.964
~~~

Representative vertical samples also repeatedly converge near 1.0, including Up/Down samples around 0.96-1.03 and a Down sequence around 1.018 / 0.995 / 0.985.

Conversely, some runs show large spatial variation within the same frame and same temporal alignment. A global scale error or fixed temporal offset cannot produce that screen-position-dependent pattern.

The residual pattern is therefore consistent with components intentionally omitted by the rotation-only witness, such as third-person camera translation/orbit, depth-dependent parallax, foreground/player motion, and independently moving geometry. Proving those components individually is no longer necessary to determine the XeSS motion-vector scale.

Capture 18 closes the historical scale:

~~~text
motionScaleX =  renderWidth / 2
motionScaleY = -renderHeight / 2
~~~

Gate D is now CLOSED. Sparse Depth + full world-position reprojection is no longer required as an MV-scale gate.

### OptiScaler DepthInverted behavior — source verification

Upstream OptiScaler master was inspected at source snapshot:

~~~text
optiscaler/OptiScaler
commit: 44cfee4d436857742a9bf71bbe81396ec9989715
~~~

For the XeSS D3D12 input path, OptiScaler/inputs/XeSS_Dx12.cpp checks the producer-provided XeSS initialization flags:

~~~text
XESS_INIT_FLAG_INVERTED_DEPTH
    -> NVSDK_NGX_DLSS_Feature_Flags_DepthInverted
~~~

OptiScaler/upscalers/IFeature.cpp then uses that incoming feature flag unless the user explicitly overrides DepthInverted in OptiScaler config. When FGInput=Upscaler, the resulting upscaler DepthInverted state is copied into FGXeFGDepthInverted, so the producer's depth convention propagates into XeFG automatically.

This means:

> OptiScaler auto is not depth-texture content detection. For a standard XeSS producer, auto means the depth convention is inherited from the producer's XESS_INIT_FLAG_INVERTED_DEPTH path, with optional user override.

The XeFG config also has its own default, but that does not remove the producer responsibility for standard XeSS SR. REFramework must still initialize XeSS with the correct current-build depth convention.

Intel XeSS SR likewise defines normal depth as the default and requires XESS_INIT_FLAG_INVERTED_DEPTH when larger depth values represent nearer geometry.

### Capture 19 — Depth convention and camera projection metadata closed

Capture 19 was collected from Google Drive log `19_re2_framework_log.txt`. The embedded local git hash is not used as branch identity; the runtime `depthProjection` signature is authoritative.

The log contains four complete Static-screen runs:

~~~text
Run 1: frames 5674-5705   32 samples
Run 2: frames 6079-6110   32 samples
Run 3: frames 6260-6291   32 samples
Run 4: frames 6469-6500   32 samples

Total depthProjection samples: 128
jitterDrawCheck:              128 / 128 allVariantsMatch=true
probe missing/fail errors:    0
~~~

All 128 depth samples reported:

~~~text
cameraSameFrame = true
clipValid       = true
near            = 0.010000000
far             = 10000.000000000
sceneDepthInference = inverted
~~~

Camera projection was bit-stable at the logged precision:

~~~text
p22 = -1.000000954
p23 = -1.000000000
p32 = -0.010000009
p33 =  0.000000000
~~~

For near=0.01 and far=10000, the expected normal D3D coefficients are:

~~~text
p22 = -1.000000954
p32 = -0.010000010
~~~

The measured Camera-vs-normal aggregate error was:

~~~text
0.000000001   (128 / 128)
~~~

The Camera-vs-inverted error was approximately 1.020001888, so the primary Camera matrix is decisively the normal-depth form.

SceneInfo projection was also stable:

~~~text
p22 = +0.000000954
p23 = -1.000000000
p32 = +0.010000009
p33 =  0.000000000
p11 =  2.411319017
~~~

The expected inverted/reversed D3D coefficients are:

~~~text
p22 = +0.000001000
p32 = +0.010000010
~~~

The measured SceneInfo-vs-inverted aggregate error was:

~~~text
0.000000047   (128 / 128)
~~~

while SceneInfo-vs-normal error was approximately 1.020001888.

Therefore the current RE4 SceneInfo/DepthStencilTex render path uses **inverted/reversed depth**. The standard XeSS producer must initialize with:

~~~text
XESS_INIT_FLAG_INVERTED_DEPTH
~~~

This is producer-owned information. Upstream OptiScaler `auto` inherits that producer flag for the XeSS path and propagates it to XeFG when `FGInput=Upscaler`; it does not infer the convention from depth-buffer contents.

No GPU Depth texture readback is required for this gate.

Camera metadata is also closed for the tested build:

~~~text
near  = 0.01
far   = 10000.0

verticalFovRadians = 0.786246836
verticalFovDegrees ~= 45.048625
~~~

The vertical FOV is derived from the SceneInfo projection:

~~~text
verticalFov = 2 * atan(1 / projection[1][1])
~~~

Production code should derive current values per frame rather than hardcode the observed 45-degree state, so aiming/cutscene/FOV changes remain correct.

Gate E is **CLOSED**. Gate F is **CLOSED** for the producer contract: near/far are available from primary `via.Camera`, the SceneInfo projection convention is proven, and vertical FOV can be derived per frame from `p11`.

### Capture 20 — Reset/history transition evidence

Capture 20 was collected from Google Drive log `20_re2_framework_log.txt`.

User actions for the main run were explicitly:

- ordinary character movement;
- ordinary camera rotation;
- one **Load Save** operation;
- no weapon aiming was available yet;
- no deliberate cutscene/camera-cut test was performed.

The log contains two reset-witness runs because the diagnostic was reset once:

~~~text
Run 1: 108 frames
Run 2: 4096 frames, frame 11286 -> 15381
Total resetWitness lines: 4204
~~~

The second run is the production-relevant long capture.

Across all 4096 frames of the main run:

~~~text
frameGap          = 0
sceneChanged      = 0
sceneInfoChanged  = 0
cameraChanged     = 0
depthChanged      = 0
velocityChanged   = 0
colorChanged      = 0
renderSizeChanged = 0

Scene pointer unique count     = 1
SceneInfo pointer unique count = 1
Camera pointer unique count    = 1
Depth pointer unique count     = 1
Velocity pointer unique count  = 1
Color pointer unique count     = 1

render size = 2560x1440 throughout
~~~

Therefore **Load Save can reuse the same RE4 Scene, SceneInfo, Camera, Depth, Velocity, and Color objects without a render-frame gap or size change**.

This closes one important negative result:

> Resource/scene/camera identity changes are useful hard-reset conditions when they occur, but they are not sufficient to detect all RE4 temporal-history discontinuities.

Normal-play pose deltas and the Load Save sequence separate strongly in this capture.

Largest ordinary-play observations before the Load Save discontinuity included:

~~~text
translationDelta ~= 0.989906847 with rotationDeltaDegrees = 0
ordinary camera rotation max observed ~= 1.564230 deg/frame
~~~

The approximately 0.99-unit translation occurred during ordinary movement/camera activity. It must **not** be reinterpreted as aiming and demonstrates that a translation-only threshold near 1.0 would be unsafe.

During the single Load Save sequence, two large same-resource pose discontinuities were observed:

~~~text
sample 3931 / frame 15216
translationDelta     = 6.555595875
rotationDeltaDegrees = 31.378658

sample 3998 / frame 15283
translationDelta     = 2.412585497
rotationDeltaDegrees = 169.999954
~~~

At both events every logged identity/size/frame-continuity flag remained false:

~~~text
frameGap=false
sceneChanged=false
sceneInfoChanged=false
cameraChanged=false
depthChanged=false
velocityChanged=false
colorChanged=false
renderSizeChanged=false
~~~

Because the user did not perform a deliberate cutscene or weapon-aim transition, both large jumps are treated as belonging to the **Load Save process**. Their exact internal sub-phase is not assigned from this log alone.

Capture 20 therefore establishes:

- first valid producer frame remains an unconditional reset;
- render-size/resource/device/context changes remain unconditional hard resets;
- RE4 Load Save may preserve all relevant object/resource identities;
- camera-pose discontinuity exists even when all resource identities are stable;
- translation alone is not a safe discriminator from this evidence;
- rotation shows strong separation in this run (ordinary max ~=1.56 deg/frame vs Load Save 31.38/170 deg), but no production threshold is accepted yet.

### Capture 21 — engine old-view-projection result

Capture 21 was collected in `21_re2_framework_log.txt`.

The user clarified the exact sequence:

1. gameplay was already loaded;
2. the diagnostic was enabled/reset;
3. **Load Save was triggered almost immediately**;
4. after the load completed, ordinary movement and camera rotation were performed until the 4096-frame budget filled.

The log contains two witness runs:

~~~text
Run 1: 281 frames, frame 8133 -> 8413
Run 2: 4096 frames, frame 8414 -> 12509
Total historyWitness lines: 4377
~~~

For every valid consecutive-frame comparison:

~~~text
referenceValid samples       = 4375 / 4375
oldVsPrevious.maxAbs         = 0.0 in 4375 / 4375
oldVsPrevious.sumAbs         = 0.0 in 4375 / 4375
closerToCurrent              = 0

relation counts:
closerToPrevious = 2785
equal            = 1590
unavailable      = 2   // first frame of each run
~~~

Therefore:

> `SceneInfo::old_view_projection_matrix` is an exact copy of the previous frame's current view-projection matrix on the tested RE4 path. It does **not** expose a Load Save history-reset decision.

The user's timing clarification also fixes the event attribution.

The main run starts at approximately 19:47:25.360. Load Save was triggered almost immediately. The large pose discontinuities appear roughly 10-12 seconds into that run, before the later ordinary-movement section:

~~~text
frame 9953
translationDelta     = 0.213761196
rotationDeltaDegrees = 7.246003
oldVsPrevious.maxAbs = 0.0

frame 10020
translationDelta     = 2.401242018
rotationDeltaDegrees = 169.999954
oldVsPrevious.maxAbs = 0.0

frame 10153
translationDelta     = 2.412172794
rotationDeltaDegrees = 169.999954
oldVsPrevious.maxAbs = 0.0
~~~

Later ordinary movement/camera rotation reaches:

~~~text
frame 11600
translationDelta     = 0.060526457
rotationDeltaDegrees = 2.031050

frame 11601
translationDelta     = 1.343110323
rotationDeltaDegrees = 1.934922
~~~

So the Capture 21 Load Save attribution is:

~~~text
capture/reset
    -> Load Save issued almost immediately
    -> ~10-12 s later: 7.25 / 170 / 170 degree same-resource pose discontinuities
    -> later: ordinary movement/camera rotation
    -> 4096-frame budget exhausted
~~~

This strengthens two conclusions:

- translation alone is unsuitable as a reset discriminator;
- rotation discontinuity remains a viable fallback candidate, but should remain secondary to a deterministic RE4 load/game-state signal if one exists.

### Capture 22 — RE4 load/fade/game-state result

Capture 22 was collected in `22_re2_framework_log.txt`.

User sequence:

- stable gameplay baseline;
- **Load Save #1**;
- ordinary gameplay;
- **Load Save #2**;
- ordinary gameplay;
- ESC/menu path followed by game exit.

The probe remained observe-only and recorded reflected integral/enum state from six candidate singletons.

Main witness run:

~~~text
sample 1 -> 7043
frame 7691 -> 14733
managerCount = 6 throughout
fieldCount   = 15 throughout
frameGap     = 0
singleton object identity changes = 0
~~~

The two Load Save operations produced the same high-value state sequence.

#### Repeated SaveDataManager process sequence

Load Save #1:

~~~text
sample 661+
share.SaveDataManager.CurrentProcess

1 -> 2 -> 4 -> 5 -> 0 -> 2 -> 3 -> 0 -> 1
~~~

Load Save #2:

~~~text
sample 3292+
share.SaveDataManager.CurrentProcess

1 -> 2 -> 4 -> 5 -> 0 -> 2 -> 3 -> 0 -> 1
~~~

This is a strong load-operation witness, but the production reset state machine should not depend on undocumented enum-number semantics alone.

#### Repeated SceneLoadZone pause window

Load Save #1:

~~~text
sample 763 / frame 8453
SceneLoadZoneManager._Pause: 0 -> 1

sample 1071 / frame 8761
SceneLoadZoneManager._Pause: 1 -> 0
camera:
    translationDelta     = 2.401242018
    rotationDeltaDegrees = 169.999954
~~~

Load Save #2:

~~~text
sample 3388 / frame 11078
SceneLoadZoneManager._Pause: 0 -> 1

sample 3664 / frame 11354
SceneLoadZoneManager._Pause: 1 -> 0
camera:
    translationDelta     = 2.413129807
    rotationDeltaDegrees = 169.999954
~~~

The rising edge occurs before the destructive camera-history transition in both loads.

However, `_Pause: 1 -> 0` is **not** sufficient as the end of the invalid-history window. Additional large camera discontinuities occur after that edge.

#### GameSituation inhibit window covers the full load transition

Load Save #1:

~~~text
sample 686  / frame 8376
GameSituationManager.InhibitBit: 0xB9 -> 0

sample 1071 / frame 8761
_Pause: 1 -> 0 + ~170 degree jump

later:
another ~170 degree camera jump

sample 1419 / frame 9109
GameSituationManager.InhibitBit: 0 -> 0xB9
~~~

Load Save #2:

~~~text
sample 3316 / frame 11006
GameSituationManager.InhibitBit: 0xB9 -> 0

sample 3664 / frame 11354
_Pause: 1 -> 0 + ~170 degree jump

later:
another ~170 degree camera jump

sample 3904 / frame 11594
GameSituationManager.InhibitBit: 0 -> 0xB9
~~~

Therefore the observed RE4 load window is better modeled as:

~~~text
normal gameplay
    |
    | SceneLoadZoneManager._Pause becomes true
    v
history invalid / load transition active
    |
    | _Pause may become false before camera history is stable
    | keep history invalid
    |
    | GameSituationManager.InhibitBit returns to the remembered
    | pre-load normal value
    v
first stable gameplay frame
    -> XeSS reset = true
    -> resume normal temporal accumulation
~~~

The pre-load `InhibitBit` value should be remembered dynamically. Do **not** hardcode `0xB9` as a universal semantic constant from one game location/state.

`SaveDataManager.CurrentProcess` can be retained as corroborating evidence or an early load-operation hint, but the production load window does not need to decode the raw enum sequence if `_Pause` + dynamic inhibit restoration are sufficient.

#### Game-exit negative control

The final ESC/menu/exit path is distinguishable from the two Load Save operations.

Observed exit-side changes include:

~~~text
SaveDataManager._IsExcludeGameSaveInQuit: 0 -> 1 -> 0
MainModeManager.CurrPhase: 6 -> 7 -> 5 ...
GameSituationManager.InhibitBit: 0xB9 -> 0
~~~

and shortly afterward a separate large camera/world discontinuity:

~~~text
translationDelta ~= 212.4
rotationDeltaDegrees ~= 15.5
~~~

But the exit path does **not** show:

~~~text
SceneLoadZoneManager._Pause: 0 -> 1
SaveDataManager.CurrentProcess load sequence
~~~

This is a useful negative control: the selected load-state witness does not simply fire on every menu/quit transition.

#### Low-value candidate rejected

`SceneActivateMediator.EntryProcIntervalTimer` generated approximately 6330 of the 6409 field-change records and behaves as a near-continuous timer. It is not a useful reset signal.

### Gate G decision

Capture 22 provides a deterministic RE4 Load Save history-invalid window without relying on camera-angle thresholds.

For current production readiness, Gate G is closed with:

- unconditional reset conditions already listed below;
- RE4 Load Save invalidation armed by the observed SceneLoadZone pause transition;
- history kept invalid after pause release until the remembered pre-load GameSituation inhibit state is restored;
- first valid post-load gameplay frame submitted with `reset=true`;
- camera discontinuity retained only as optional future hardening for non-load cuts/teleports.

A dedicated cutscene-only capture can be added later if a real non-load cut exposes a contradiction. It no longer blocks proceeding to D3D12 execution-order work.

### Capture 23 — D3D12 execution ordering result

> **Timing correction:** Captures 23-27 were recorded from `on_overlay_layer_draw()` after the original Overlay draw, despite their historical `pre-Overlay` log labels. The queue/interface/barrier/lifetime evidence below remains valid, but any statement that depends specifically on the true pre-Overlay callback was superseded by the timing-audit correction after Capture 27. Capture 29 now closes true pre-Overlay submission ordering; the input-state tuple remains for Capture 29b because the legacy resource-state probe did not observe the real barrier path.


Capture 23 was collected in `23_re2_framework_log.txt`.

The diagnostic was reset/re-run four times, producing four complete 64-frame windows:

~~~text
Run 1: frame 8816 -> 8879
Run 2: frame 9155 -> 9218
Run 3: frame 9438 -> 9501
Run 4: frame 9634 -> 9697

total pre-Overlay samples = 256
~~~

All four runs produced the same queue/submission topology.

#### Active queue proven

One active command queue was observed throughout:

~~~text
queue = 0x26362a27760
queueType = 0 / D3D12_COMMAND_LIST_TYPE_DIRECT
~~~

The queue address is capture-local evidence only and must never be hardcoded.

Across all 256 pre-Overlay windows:

~~~text
ExecuteCommandLists calls after pre-Overlay and before Present = exactly 5 / frame
total ExecuteCommandLists records                             = 1280
numLists per ExecuteCommandLists                              = 1 in 1280/1280
submitted command-list type                                   = DIRECT in 1280/1280
submitsSinceBoundary at Present                               = 5 in 256/256
~~~

All queue submissions and all Presents were observed on thread `25576`.

The pre-Overlay callbacks themselves were distributed across nine worker threads, proving that the engine-level insertion callback is recorded from worker-side render activity while final queue submission is serialized on a different thread.

#### Temporal resource identity remains stable

Across 256/256 boundaries:

~~~text
currentResource
    == Overlay main resource
    == Scene PostMain/HDR Color
    == 0x262b2eec420

Depth    = 0x264b63bb1d0
Velocity = 0x264b63bd810
~~~

Each of these resource addresses remained unique/stable across the entire Capture 23 session.

Again, the absolute addresses are capture-local evidence only.

#### Command-list pool is deterministic

Exactly ten DIRECT command-list objects were submitted.

Each appeared exactly 128 times across the 1280 submissions.

The capture shows two five-list pools:

~~~text
Pool A
A1 0x26447ae66f0
A2 0x26447ae7080
A3 0x26449bac310
A4 0x26446464640
A5 0x26449baf2e0

Pool B
B1 0x262b2f91750
B2 0x264b5e681a0
B3 0x264b5ce70c0
B4 0x264b5ce8d70
B5 0x264b5cf9930
~~~

The exact per-frame submit order was deterministic for all 256 frames:

~~~text
frame % 4 == 0
A5 -> A4 -> A3 -> A2 -> A1

frame % 4 == 1
B5 -> B4 -> B3 -> B2 -> B1

frame % 4 == 2
A1 -> A2 -> A3 -> A4 -> A5

frame % 4 == 3
B1 -> B2 -> B3 -> B4 -> B5
~~~

This pattern is useful as a provenance witness, not as a production rule. Production code must discover the active command-list objects dynamically and must not depend on these addresses or on a hardcoded modulo-4 sequence.

### Gate H decision after Capture 23

Capture 23 closes the first two Gate H sub-gates:

- **DIRECT queue provenance: CLOSED**;
- **submitted command-list pool/order provenance: CLOSED**.

Still open:

- which command list/generation is actively being recorded when the verified pre-Overlay callback executes;
- explicit Color/Depth/Velocity resource states at that point;
- exact target-resource transitions before/after the insertion point;
- output UAV state and restoration strategy;
- eventual XeSS submission/fence integration.

A new standalone command list should still **not** be introduced yet.

### Capture 24 — submitted-base instance hook result

Capture 24 was collected in `24_re2_framework_log.txt`.

The probe completed four 64-frame windows again:

~~~text
resourceBoundary = 256
resourcePresent  = 256
resourceSubmit   = 1280
~~~

The already-proven post-boundary topology remained intact:

~~~text
post-pre-Overlay submits before Present = 5 / frame
submitted post-boundary list count      = 10 unique
each post-boundary list                 = 128 executions
Color == HDR                            = 256 / 256
Depth / Velocity identity               = stable
~~~

However, the intended per-recording state reconstruction did **not** succeed:

~~~text
resourceListHook = 22
resourceReset    = 20
resourceClose    = 0
resourceBarrier  = 0

pre-Overlay activeList != null = 0 / 256

post-boundary resourceSubmit generation:
    generation 0 = 10
    generation 1 = 1270
~~~

The ten repeated post-boundary lists were still submitted 128 times each, but after their initial observed generation they never produced another hooked Reset or any hooked Close.

That cannot be interpreted as "RE4 does not reset/close/re-record command lists." The same DIRECT list objects are being reused repeatedly, so a real recording lifecycle must exist somewhere in the underlying D3D12 path.

The correct conclusion is narrower:

> An instance-local Vtable hook installed on the pointer received through `ExecuteCommandLists(ID3D12CommandList*)` is not sufficient to observe RE4's actual repeated graphics-command-list recording path.

Possible explanations that remain open:

- the submitted base-interface pointer and the interface pointer used by RE4 while recording are different COM interface views of the same object;
- a derived `ID3D12GraphicsCommandListN` interface is used for recording;
- legacy `ResourceBarrier[26]` is not the only barrier path and RE4 may use `ID3D12GraphicsCommandList7::Barrier` enhanced barriers;
- more than one of the above may apply.

Do **not** select one explanation without another witness.

#### Whole-frame submission topology discovered

Capture 24 also extends Capture 23 beyond the pre-Overlay→Present window.

The `resourceSubmit` counter runs continuously, while each `resourceBoundary` records the counter value at pre-Overlay.

After warm-up, consecutive pre-Overlay boundaries show exactly seven DIRECT submissions per render frame.

Runs 2-4 are exact:

~~~text
63 / 63 frame intervals in each run:
    total DIRECT submits per frame = 7
~~~

Run 1 is almost identical but still contains hook-discovery warm-up:

~~~text
61 intervals = 7 submits
 2 intervals = 8 submits
~~~

Combined with Capture 23's proven five submissions after pre-Overlay, the stable frame topology is:

~~~text
2 DIRECT submissions
        |
        v
verified pre-Overlay boundary
        |
        v
5 DIRECT submissions
        |
        v
Present
~~~

The resource-state diagnostic dynamically encountered **22 DIRECT list pointers** in total, while only the known ten-list pool appears in the five post-boundary submissions.

Therefore the two pre-boundary submissions must be included in later command-list provenance work. The Color/Depth/Velocity-producing or state-transitioning list cannot be assumed to belong to the ten post-boundary objects.

### Gate H decision after Capture 24

Capture 24 does **not** close Color/Depth/Velocity states.

It does close two negative findings:

- submitted-base instance-vtable hooks are insufficient for recording-lifecycle reconstruction;
- tracing only the five post-boundary submissions is insufficient for whole-frame provenance.

Current Gate H state:

~~~text
DIRECT queue provenance                    CLOSED
post-boundary five-list provenance          CLOSED
stable whole-frame 2 + boundary + 5 shape  PROVEN after warm-up

exact recording interface                   OPEN
pre-Overlay active recording list           OPEN
legacy ResourceBarrier path                 NOT OBSERVED
enhanced Barrier path                       UNKNOWN
Color/Depth/Velocity states                 OPEN
~~~

### Capture 25 — COM interface and barrier-path provenance result

Capture 25 was collected in `25_re2_framework_log.txt`.

Four complete 64-frame windows were captured:

~~~text
Run 1: frame 14951 -> 15014
Run 2: frame 15162 -> 15225
Run 3: frame 15316 -> 15379
Run 4: frame 15476 -> 15539

interfaceBoundary = 256
~~~

#### Public GraphicsCommandList interface alias hypothesis rejected

The probe emitted:

~~~text
interfaceTopology = 81
interfaceMethods  = 81
~~~

Across all 81 topology records:

- `IUnknown` QueryInterface succeeded;
- `ID3D12GraphicsCommandList` through `ID3D12GraphicsCommandList7` all succeeded;
- `maxGraphicsVersion = 7` in 81/81 records;
- every GCL0..GCL7 public interface pointer exactly equaled the submitted base pointer;
- GCL0 and GCL7 used the same public vtable;
- canonical `IUnknown` identity was a distinct pointer.

The 81 topology records reduce to:

~~~text
unique submitted/base pointers = 21
unique canonical IUnknown      = 21
base <-> IUnknown mapping      = 1:1
~~~

Therefore:

> The Capture 24 recording blind spot cannot be explained by RE4 simply using a different public GCL1..GCL7 COM interface pointer for the same submitted object.

The public GraphicsCommandList interface views all alias the same pointer/vtable in this runtime.

#### Shared implementation addresses proven

Every `interfaceMethods` record reported the same implementation addresses within this process for:

~~~text
Close
Reset
legacy ResourceBarrier
GCL7::Barrier
~~~

This proves that all observed submitted DIRECT lists share common D3D12Core implementation functions for these methods.

The absolute addresses are process-local diagnostics and must never be hardcoded.

`ID3D12GraphicsCommandList7` support is also proven.

That means the enhanced-barrier API is available on the active RE4 command lists, but Capture 25 does **not** prove that RE4 actually calls `GCL7::Barrier`.

Support and runtime usage remain separate questions.

#### Whole-frame seven-submit topology confirmed explicitly

Capture 25 explicitly records queue submissions across the entire frame instead of inferring them only from boundary counters.

For stable frames:

~~~text
ordinal 1 -> before pre-Overlay
ordinal 2 -> before pre-Overlay

pre-Overlay boundary

ordinal 3 -> after boundary
ordinal 4 -> after boundary
ordinal 5 -> after boundary
ordinal 6 -> after boundary
ordinal 7 -> after boundary
~~~

Runs 2-4 reproduce seven submissions on all 63 post-first-frame intervals.

Run 1 contains one isolated eight-submit frame during discovery/warm-up; the remaining stable frame intervals follow the seven-submit pattern.

Therefore the stable whole-frame topology from Capture 24 is independently confirmed.

#### Pre-boundary and post-boundary pools are distinct

Within a stable run, pre-boundary submissions use a ten-list set:

- one fixed list appears every frame;
- nine additional list objects rotate through the second pre-boundary slot.

The five post-boundary submissions continue to use the ten-list/two-pool system proven by Capture 23.

The important conclusion is not the exact pool algorithm. It is:

> Both sides of the pre-Overlay boundary contain real DIRECT command-list work and must remain in the provenance model.

No capture-local pointer or ordinal cycle is a production contract.

#### Capture 25 diagnostic bug

The `interfacePresent` log continued after sample 64 because the sample/frame boundary fields were not cleared after Present.

This produced stale repeated `interfacePresent sample=64` lines after the actual 64-frame capture was already complete.

The queue submission capture itself stopped correctly, so this log-spam bug does not change the Capture 25 topology result.

The bug is fixed before Capture 26 by clearing the interface boundary sample/frame after every Present.

### Gate H decision after Capture 25

Capture 25 closes:

- public GCL0..GCL7 interface topology;
- GCL7 capability;
- shared implementation provenance for Close/Reset/legacy ResourceBarrier/GCL7 Barrier;
- explicit whole-frame seven-submit ordering.

Still open:

- whether repeated recording actually reaches the shared Reset/Close implementation hooks;
- whether RE4 uses legacy ResourceBarrier, enhanced Barrier, or both;
- the exact list/generation active at pre-Overlay;
- explicit Color/Depth/Velocity state transitions.

The next probe should therefore hook the **shared implementation functions**, not clone another submitted-object vtable.

### Capture 26 — shared recording-function and resource-state result

Capture 26 was collected in `26_re2_framework_log.txt`.

The raw log is approximately 18 MB because the shared recording hooks continued to emit detailed Reset/Close/barrier diagnostics after the 64-sample capture window had closed. That logging issue is diagnostic-only and is fixed before Capture 27.

Two complete 64-frame Capture 26 runs are present:

~~~text
Run 1: frame 15374 -> 15437
Run 2: frame 15587 -> 15650
~~~

Within each actual 64-frame window:

~~~text
recordingBoundary       = 64
recordingPresent        = 64
recordingReset          = 427
recordingClose          = 426
recordingLegacyBarrier  = 1426
recordingEnhancedBarrier= 0
~~~

The two runs reproduce the same counts.

Across the entire oversized log, including post-capture diagnostic spam:

~~~text
recordingReset           = 15747
recordingClose           = 15746
recordingLegacyBarrier   = 45260
recordingEnhancedBarrier = 0
~~~

This provides strong runtime evidence that the observed RE4 D3D12 recording path uses the **legacy `ID3D12GraphicsCommandList::ResourceBarrier` API**, not `ID3D12GraphicsCommandList7::Barrier`, despite GCL7 support being available.

The conclusion is limited to the observed active RE4 DIRECT rendering path. GCL7 capability remains real; it is simply not used here.

#### Shared Reset/Close path proven

The shared implementation-level hook sees repeated successful Reset/Close activity for the same submitted DIRECT-list population.

Therefore Capture 24's missing repeated lifecycle was a limitation of the submitted-object VtableHook approach, not an absence of real command-list recording.

All captured `recordingClose` results are successful.

The real recording work is observed on the D3D12 recording/submission thread, while the engine-level pre-Overlay callback is reached on other worker threads.

Accordingly, the pre-Overlay callback does not expose an engine-owned graphics command list that can safely be adopted directly as the XeSS recording list.

#### Pre-Overlay target states closed

The first few samples in each run are hook-discovery warm-up.

From sample 4 through sample 64 in both runs — **122/122 stable pre-Overlay boundaries** — the latest observed legacy transition state is identical:

~~~text
Color subresource 0
    state = 0xC0
          = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
          | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE

Depth subresource 0
    state = 0xE0
          = D3D12_RESOURCE_STATE_DEPTH_READ
          | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
          | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE

Depth subresource 1
    state = 0xE0
          = D3D12_RESOURCE_STATE_DEPTH_READ
          | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
          | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE

Velocity subresource 0
    state = 0x04
          = D3D12_RESOURCE_STATE_RENDER_TARGET
~~~

Therefore the verified pre-Overlay XeSS input boundary is:

~~~text
Color     = shader-readable
Depth     = depth-read + shader-readable
Velocity  = render target
~~~

The future XeSS execution path must not assume Velocity is already shader-readable.

At minimum, before XeSS consumes Velocity:

~~~text
Velocity
RENDER_TARGET
    -> shader-readable state required by the XeSS input binding
~~~

and before handing control back to RE4:

~~~text
Velocity
shader-readable
    -> RENDER_TARGET
~~~

must be restored unless the final XeSS API/state contract proves a different legal state combination.

Color and Depth are already in read-compatible states at the verified insertion boundary.

#### Game restoration cycle proven

The post-boundary engine lists repeatedly transition the same resources and return them to the same next-frame boundary state.

Representative recurring behavior includes:

~~~text
Depth
0xE0 -> DEPTH_WRITE -> 0xE0

Velocity
RENDER_TARGET -> 0xC0 -> UAV -> 0xC0
...
0xC0 -> RENDER_TARGET

Color
0xC0 <-> RENDER_TARGET
RENDER_TARGET -> COPY_SOURCE -> 0xC0
...
final -> 0xC0
~~~

Thus the RE4 render graph expects the next frame to begin from the restored state set:

~~~text
Color     = 0xC0
Depth     = 0xE0
Velocity  = 0x04
~~~

This restoration contract is now evidence-backed and should be preserved by the future bridge.

#### Pre-Overlay engine list ownership result

`recordingBoundary.activeList` remains null at all 128 Capture 26 boundaries.

This is consistent with the observed threading split:

- the shared D3D12 Reset/Close/barrier recording path runs on the D3D12 recording/submission thread;
- pre-Overlay callbacks occur on separate engine worker threads.

Therefore:

> The production bridge should own its XeSS command allocator/list rather than attempt to append XeSS commands to an engine-owned list inferred from the pre-Overlay callback thread.

This does **not** yet prove where the bridge-owned list should be submitted relative to the engine lists. That ordering is the next Gate H sub-gate.

#### Capture 26 logging fix

The 18 MB log was caused by shared-hook detail continuing after the capture window closed.

Before Capture 27, detailed shared Reset/Close/legacy/enhanced barrier logging is gated by `m_recording_capture_open`.

The implementation hooks may remain installed for diagnostic reuse, but they fast-pass to the original function without detailed capture logging after the 64-sample window.

### Gate H decision after Capture 26

Capture 26 closes:

- real repeated shared Reset/Close recording path;
- legacy versus enhanced barrier usage for the observed RE4 path;
- pre-Overlay Color state;
- pre-Overlay Depth state;
- pre-Overlay Velocity state;
- RE4's expected restoration state for those inputs;
- the conclusion that the bridge should not rely on an engine-owned list from the pre-Overlay callback thread.

Still open:

- exact safe ordering/lifetime for a REFramework-owned DIRECT command list;
- output UAV state and final XeSS output integration;
- resize/device-reset lifecycle for bridge-owned execution resources.

### Capture 27 — REFramework-owned empty DIRECT-list ordering closed

Capture 27 was collected in `27_re2_framework_log.txt`.

The log contains two complete 64-frame runs:

~~~text
Run 1: frame 16173 -> 16236
Run 2: frame 16583 -> 16646
~~~

Across the two runs:

~~~text
bridgeOrderBoundary        = 128
bridgeOrderIssue           = 128
bridgeOrderBoundaryResult  = 128
bridgeOrderPresent         = 128
bridgeOrderSubmit          = 1020
bridgeOrderList            = 1020
bridgeOrderSkip            = 0
~~~

Every `bridgeOrderBoundaryResult` reports a successful bridge submission and zero skipped frames.

No bridge-order allocator Reset, command-list Reset, Close, queue Signal, device-removal, or rendering-failure record is present.

#### Stable whole-frame ordering proven

The first sample of each run begins observation at pre-Overlay, so it sees the bridge-owned submission followed by the five already-known post-boundary engine submissions:

~~~text
sample 1:
    ordinal 1 = REF-owned empty list
    ordinal 2..6 = engine
~~~

From sample 2 through sample 64 in both runs — **126/126 stable frames** — the complete queue topology is:

~~~text
ordinal 1 = engine
ordinal 2 = engine

pre-Overlay boundary

ordinal 3 = REF-owned empty list

ordinal 4 = engine
ordinal 5 = engine
ordinal 6 = engine
ordinal 7 = engine
ordinal 8 = engine

Present
~~~

Exactly one submitted list is classified as probe-owned in every stable frame, always at ordinal 3.

Therefore the REFramework-owned list is deterministically inserted after the two pre-Overlay engine submissions and before the five existing post-boundary engine submissions.

The original engine topology is preserved around the injected empty list.

#### Fence-safe allocator/list reuse proven

Capture 27 uses eight REFramework-owned DIRECT allocator/list slots.

Observed slot order is the intended ring:

~~~text
0,1,2,3,4,5,6,7,0,1,2,3,...
~~~

Fence signal values progress continuously:

~~~text
Run 1: 1  -> 64
Run 2: 65 -> 128
~~~

The second manual `Reset capture` resets capture counters but intentionally retains the already-created bridge execution resources and their fence timeline.

For all **120 actual slot-reuse events**:

~~~text
completedBefore >= previousFence
completedBefore - previousFence = 7
~~~

No allocator/list slot is reset while its prior GPU work is still in flight.

At Present, the bridge fence has reached the just-issued signal value in **128/128 frames**.

The eight-slot design therefore has substantial observed headroom in this stable RE4 path and does not require a CPU/GPU wait.

#### Gate H decision at the time of Capture 27 — superseded by timing audit

Capture 27 originally appeared to close Gate H because its runtime label reported `stage=preOverlay`.

The later source-level audit proves that this scenario was actually executing from `on_overlay_layer_draw()`, after the original Overlay draw. Therefore the following Capture 27 facts remain valid:

- REFramework owns the allocator/list ring;
- the same active RE4 DIRECT queue accepts the bridge-owned list;
- eight-slot reuse is fence-gated and nonblocking;
- surrounding engine work remains stable around the **post-Overlay** insertion;
- no engine-owned recording list needs to be adopted or extended.

What is superseded is the timing-specific claim that this ordinal and the Capture 26 state tuple were already proven at `on_pre_overlay_layer_draw()`.

Gate H was reopened only for the narrow true pre-Overlay state/order correction. Capture 29 now closes ordering; Capture 29b remains only for the true pre-Overlay input-state tuple.

### Capture 28 — post-Overlay output copy provenance result

Capture 28 was collected in `28_re2_framework_log.txt`.

The log contains two complete 64-frame runs:

~~~text
Run 1: frame 9597 -> 9660
Run 2: frame 9905 -> 9968

outputCopyBoundary = 128
outputCopyPresent  = 128
outputCopy events  = 124
~~~

The first two samples of each run are shared-hook/list discovery warm-up.

Capture 28 was opened from `on_overlay_layer_draw()`, i.e. **after** the original RE4 Overlay draw. The runtime record still printed `stage=preOverlay` because that diagnostic string was inherited from the earlier insertion-boundary probes. That label is stale only; it does not change the recorded callback timing. The probe now prints `stage=postOverlay` for post-Overlay output scenarios, while Capture 29/29b true-pre scenarios print `stage=preOverlay`.

From sample 3 through sample 64 in both runs — **124/124 stable samples** — exactly one tracked copy occurs per frame.

Every tracked copy is:

~~~text
type = CopyResource

src
    = Scene::PostMainTarget
    = Scene::HDRTarget
    = Overlay main native resource

dst
    = one stable intermediate HDR resource

src/dst shape
    width  = 2560
    height = 1440
    format = 26 / DXGI_FORMAT_R11G11B10_FLOAT
    flags  = 0x5 / ALLOW_RENDER_TARGET | ALLOW_UNORDERED_ACCESS
~~~

No tracked `CopyTextureRegion` edge is observed.

The absolute resource addresses are process-local capture evidence and must not be hardcoded.

#### Copy is recorded by the final engine list

The command-list pointer that records the tracked `CopyResource` is later submitted as the **last engine DIRECT list of the frame**.

Across the 124 stable copy samples:

~~~text
122 copies -> Execute ordinal 7 in normal seven-submit frames
  2 copies -> Execute ordinal 8 in the two observed eight-submit frames
~~~

Therefore the semantic conclusion is stable even when an extra engine submission appears:

> The HDR/PostMain copy belongs to the final engine submission before Present.

Do not treat the absolute ordinal as a permanent ABI. The stable semantic witness is "final engine list before Present."

#### Copy graph does not reach the swapchain

Capture 28 seeds the graph with HDR/PostMain Color, then propagates tracking whenever a copy touches a tracked resource.

After the first Color -> intermediate edge:

- the intermediate remains tracked;
- no later `CopyResource` edge uses it as source or destination;
- no tracked `CopyTextureRegion` edge appears;
- none of the three active swapchain-buffer identities enters the copy-connected set;
- `reachedSwapchain=false` in **128/128 Present summaries**.

Therefore:

> The current RE4 HDR/PostMain -> presentation path is not a pure copy chain.

The proven current-build topology is now:

~~~text
HDR/PostMain / Overlay-main Color
R11G11B10_FLOAT
        |
        | CopyResource
        v
same-format HDR intermediate
R11G11B10_FLOAT
        |
        | non-copy presentation/composite path
        v
OutputTargetState / swapchain path
~~~

The "non-copy" edge is proven only by exclusion of the traced copy APIs. Its exact mechanism is still open.

A shader/full-screen composite is a plausible next hypothesis, but Capture 28 alone does **not** prove SRV binding, RTV binding, tone mapping, or a particular draw call.

### Gate I decision after Capture 28

Capture 28 closes:

- the first post-Overlay output copy edge;
- its source semantic identity;
- the repeated destination resource identity/shape;
- the fact that the copy belongs to the final engine submission;
- direct or chained copy-to-swapchain as the presentation mechanism.

Still open:

- how the HDR intermediate is consumed after the copy;
- where an active swapchain buffer becomes a render target;
- whether a draw occurs while the HDR intermediate is shader-readable and a swapchain buffer is render-target writable;
- whether that draw is the final HDR/tone-map/composite boundary;
- where a display-resolution XeSS output should replace or feed the current pipeline;
- output-resource lifetime and resize handling.

### Timing audit correction — Captures 23-27 were post-Overlay, not true pre-Overlay

A source-level audit after Capture 28 found an important callback-timing mismatch.

Capture 9 was collected while `RE4TemporalProbe` implemented both `on_pre_overlay_layer_draw()` and `on_overlay_layer_draw()`, so its paired pre/post Overlay semantic result remains valid.

During the later MV-readback work, the probe moved its Overlay-side diagnostic callback to `on_overlay_layer_draw()` so the Velocity snapshot was taken after the original Overlay draw. Gate H scenarios added afterward reused that callback, but their log strings and documentation continued to call the boundary `pre-Overlay`.

Therefore Captures 23-27 remain valid for the facts that do not depend on which side of the Overlay draw the callback occurred:

- one active DIRECT queue and the observed whole-frame submission population;
- public GCL0-GCL7 interface topology;
- shared Reset/Close/ResourceBarrier implementation provenance;
- actual use of legacy `ResourceBarrier` rather than enhanced `Barrier` in the observed path;
- post-Overlay Color/Depth/Velocity state transitions;
- bridge-owned allocator/list/fence reuse safety;
- successful insertion of a REF-owned list at the **post-Overlay** callback.

They do **not** prove the resource states or REF-owned-list ordinal at the production `on_pre_overlay_layer_draw()` insertion point.

This does not invalidate Capture 28. Capture 28 is explicitly a **post-Overlay output-path** capture and its Color -> HDR-intermediate copy result remains valid.

Gate H is therefore reopened only for a narrow timing correction: true pre-Overlay resource states and bridge-owned submission order.

### Capture 29 — true pre-Overlay ordering closed; state witness incomplete

Capture 29 was collected in `29_re2_framework_log.txt`.

The log contains two complete 64-frame runs for each requested scenario:

~~~text
D3D12 resource states
    Run 1: frame 13956 -> 14019
    Run 2: frame 14190 -> 14253

D3D12 bridge ordering
    Run 1: frame 16115 -> 16178
    Run 2: frame 16304 -> 16367
~~~

Both scenarios are now confirmed to execute from the restored true `on_pre_overlay_layer_draw()` callback. All 128 boundaries per scenario explicitly report:

~~~text
stage=preOverlay
~~~

#### True pre-Overlay REF-owned submission ordering proven

Across the two bridge-order runs:

~~~text
bridgeOrderBoundary       = 128
bridgeOrderIssue          = 128
bridgeOrderBoundaryResult = 128
bridgeOrderPresent        = 128
bridgeOrderSubmit         = 1020
bridgeOrderList           = 1020
bridgeOrderSkip           = 0
~~~

The first sample of each run begins observation at the boundary and therefore sees only the REF-owned list plus the five later engine submissions.

From sample 2 through sample 64 in both runs — **126/126 stable whole frames** — the true pre-Overlay topology is:

~~~text
ordinal 1 = engine DIRECT
ordinal 2 = engine DIRECT

true on_pre_overlay_layer_draw()

ordinal 3 = REFramework-owned DIRECT list

ordinal 4 = engine DIRECT
ordinal 5 = engine DIRECT
ordinal 6 = engine DIRECT
ordinal 7 = engine DIRECT
ordinal 8 = engine DIRECT

Present
~~~

Exactly one list is probe-owned in every stable frame, always at ordinal 3.

The allocator/list lifetime result also reproduces cleanly:

~~~text
submitted                    = 128 / 128
skipped                      = 0
actual eight-slot reuses     = 120
completedBefore >= old fence = 120 / 120
completedBefore-oldFence     = 7 in 120 / 120 reuses
Present fence complete       = 128 / 128
~~~

Therefore the timing-specific ordering question reopened after Capture 27 is now closed:

> The production RE4 XeSS insertion point can submit an REFramework-owned DIRECT command list from the true pre-Overlay callback after the first two engine DIRECT submissions and before the remaining five observed engine submissions.

The semantic relationship is the production contract. Do not hardcode an absolute ordinal without revalidating if the engine submission topology changes.

#### Resource-state scenario did not observe the real barrier path

The two `D3D12 resource states` runs also reached 128/128 true pre-Overlay boundaries and 128/128 Presents:

~~~text
resourceBoundary = 128
resourcePresent  = 128
resourceSubmit   = 640
resourceReset    = 20
resourceClose    = 0
resourceBarrier  = 0
~~~

This does **not** show that Color/Depth/Velocity have different states at true pre-Overlay. It shows that this diagnostic path did not observe the recording implementation that emits the relevant barriers.

The reason is now source-confirmed: `D3D12 resource states` still relies on the older per-submitted-object vtable hook. Capture 26 already proved that repeated RE4 recording is visible through the shared implementation-level Reset/Close/ResourceBarrier hooks instead.

Therefore Capture 29 does not invalidate the previously observed post-Overlay `0xC0 / 0xE0 / 0x04` tuple; it simply leaves the true pre-Overlay tuple unmeasured.

### Capture 29b objective and result — true pre-Overlay state tuple proven

Reuse the already-proven `D3D12 recording functions` scenario rather than adding another hook family.

For Capture 29b the scenario is moved from `on_overlay_layer_draw()` to the restored true `on_pre_overlay_layer_draw()` callback. Its shared implementation hooks remain unchanged.

This deliberately preserves the Capture 26 mechanism that observed:

- repeated real Reset/Close activity;
- legacy `ResourceBarrier` calls;
- Color/Depth/Velocity transitions;
- no enhanced-barrier activity in the observed RE4 DIRECT path.

Capture 29b procedure:

1. enter stable gameplay;
2. select `D3D12 recording functions`;
3. click `Reset capture`;
4. allow all **64 samples** to complete;
5. click `Reset capture` again;
6. allow another **64 samples** to complete;
7. do not Load Save, open menus, resize, Alt+Tab, or enable OptiScaler/XeFG validation.

Expected boundary label:

~~~text
recordingBoundary ... stage=preOverlay
~~~

Decision target:

- reconstruct the latest Color state at each true pre-Overlay boundary;
- reconstruct Depth subresources 0/1 at each true pre-Overlay boundary;
- reconstruct Velocity subresource 0 at each true pre-Overlay boundary;
- determine whether the stable tuple is still:

~~~text
Color     = 0xC0
Depth     = 0xE0
Velocity  = 0x04
~~~

No bridge-order rerun is required.

If Capture 29b reproduces that tuple consistently, Gate H is fully closed again. The already-prepared final HDR composite diagnostic then proceeds as Capture 30.

Capture 29b has now completed two full 64-sample runs:

~~~text
Run 1: frame 8214 -> 8277
Run 2: frame 8450 -> 8513

recordingBoundary = 128
recordingPresent  = 128
~~~

All 128 boundaries are `stage=preOverlay`. After shared-hook warm-up, sample 3-64 in both runs gives **124/124 stable frames** with:

~~~text
Color               = 0xC0
Depth subresource 0 = 0xE0
Depth subresource 1 = 0xE0
Velocity            = 0x04
~~~

The complete observed Color/Depth/Velocity transition cycle is stable across those frames and restores the same tuple at the actual true pre-Overlay boundary.

Therefore Gate H is now **CLOSED**. For production XeSS insertion, Color is already shader-readable at `0xC0`, Depth is depth-read + shader-readable at `0xE0`, while Velocity arrives as `0x04` (`RENDER_TARGET`) and must be transitioned for XeSS consumption and restored to `0x04` before returning control to RE4.

No further Gate H rerun is required.

### Capture 30 objective — final HDR composite/output witness (initial configuration)

New scenario:

~~~text
D3D12 final output composite
~~~

Capture 30 remains fully observe-only.

It reuses the Capture 28 `Color -> HDR intermediate` discovery edge, then follows only the resources relevant to the unresolved presentation boundary:

~~~text
Color
HDR intermediate
active swapchain buffers
~~~

The shared legacy `ResourceBarrier` hook records transitions for only those resources.

The shared command-list hook also observes:

~~~text
DrawInstanced
DrawIndexedInstanced
~~~

but emits detailed draw records only when both of these state conditions are simultaneously known:

~~~text
HDR intermediate
    -> shader-readable state

an active swapchain buffer
    -> D3D12_RESOURCE_STATE_RENDER_TARGET
~~~

This deliberately avoids broad draw-call tracing.

#### Cross-frame swapchain state

Swapchain state is tracked continuously once the active buffers are known.

Capture 30 preserves swapchain state across Present -> next-frame post-Overlay observation intervals so a swapchain transition recorded earlier in the frame is not lost before the next output-composite boundary.

Per-frame intermediate state is **not** carried forward blindly. When the current frame's `Color -> intermediate` copy is observed, the previous intermediate-state witness is invalidated. A later current-frame barrier must establish its shader-readable state before a draw can qualify as a candidate.

This prevents a previous frame's state from creating a false candidate draw.

#### Primary records

~~~text
finalCompositeBoundary
finalCompositeCopy
finalCompositeBarrier
finalCompositeDraw
finalCompositeSubmit
finalCompositeSubmitList
finalCompositePresent
~~~

A candidate draw records:

- draw API and arguments;
- command-list identity;
- sample/frame/thread;
- discovered HDR-intermediate identity and current state;
- active swapchain-buffer identity and current state.

At Present the summary records:

- whole-frame submit count;
- final-composite events since boundary;
- total observed draws since boundary;
- candidate draws since boundary;
- tracked-resource count;
- swapchain-buffer count;
- intermediate identity and final observed state.

#### Evidence limitation

A candidate draw proves **ordering/state concurrence**:

~~~text
HDR intermediate is shader-readable
AND
swapchain buffer is render-target writable
AND
a graphics draw is recorded on the observed RE4 DIRECT path
~~~

It does **not by itself prove descriptor binding** of that intermediate as an SRV.

If Capture 30 yields a stable unique candidate window, that is enough to identify the final output-composite stage for the next integration decision. Add descriptor-level provenance only if multiple candidate draws remain ambiguous.

Capture 30 procedure:

1. enter stable gameplay;
2. select `D3D12 final output composite`;
3. click `Reset capture`;
4. keep gameplay/camera mostly still;
5. allow all **64 samples** to complete;
6. do not Load Save, open menus, resize, Alt+Tab, or enable OptiScaler/XeFG validation.

Decision target:

- prove the HDR intermediate's post-copy shader-readable transition;
- prove the swapchain render-target transition;
- identify the command list / queue ordinal containing the state-overlap draw window;
- determine whether one stable final-composite draw stage exists before Present;
- use that stage to decide how a future display-resolution XeSS result should enter RE4's own presentation/UI pipeline.

Do not replace an engine target, modify a descriptor, or dispatch XeSS in Capture 30.

### Capture 30 result — final engine list and state window narrowed

Two complete 64-sample runs were collected:

~~~text
Run 1: frame 6741 -> 6804
Run 2: frame 7020 -> 7083

finalCompositeBoundary = 128
finalCompositeCopy     = 124
finalCompositePresent  = 128
~~~

Samples 3-64 in both runs give **124/124 stable frames**. The same event pattern repeats:

~~~text
earlier engine list:
    Color 0xC0 -> 0x04

final engine DIRECT list:
    Color 0x04 -> 0xC0
    Color 0xC0 -> 0x04
    Color 0x04 -> 0x08
    Color 0x08 -> 0x04
    Color 0x04 -> 0x800
    CopyResource: Color -> stable same-format HDR copy destination
    Color 0x800 -> 0xC0
    Color 0xC0 -> 0x04
    Color 0x04 -> 0xC0
    Color 0xC0 -> 0x04
    Swapchain 0x00 -> 0x04
    Color     0x04 -> 0xC0
    [narrow final-output draw window]
    Swapchain 0x04 -> 0x00
~~~

The Capture 28 copy edge is therefore reproduced in all 124 stable frames. However, after discovering that destination in the current frame, the probe records no explicit legacy transition that establishes it as shader-readable before Present:

~~~text
intermediateState at Present = 0x0
candidateDraws using the intermediate criterion = 0
~~~

This does **not** prove that the copy destination is unused. It means only that Capture 30 does not support the earlier assumption that this destination itself is the directly observed shader-readable final screen-output source.

The late Color/swapchain events are on the last engine DIRECT submission before Present:

~~~text
ordinal 7 = 122/124 stable frames
ordinal 8 =   2/124 stable frames
~~~

The two ordinal-8 cases are the rare eight-submit topology; the semantic position is still the final engine DIRECT submission before Present.

Most importantly, Capture 30 proves a repeatable narrow state-overlap window:

~~~text
active swapchain buffer = 0x04  (RENDER_TARGET)
Color                   = 0xC0  (shader-readable)
~~~

followed by `Swapchain 0x04 -> 0x00` and Present.

### Capture 30b objective — exact Color -> swapchain draw witness

The initial Capture 30 draw filter required the discovered copy destination to be shader-readable, which is why it emitted zero candidate draws. Reuse the same `D3D12 final output composite` scenario, but key the narrow candidate on:

~~~text
Color is shader-readable
AND
an active swapchain buffer is RENDER_TARGET
~~~

The detailed record keeps the draw API/arguments, command-list identity, sample/frame/thread, Color identity/state, copy-destination identity/state for context, and active swapchain identity/state.

Capture 30b procedure:

1. enter stable gameplay;
2. select `D3D12 final output composite`;
3. click `Reset capture`;
4. keep gameplay/camera mostly still;
5. allow all **64 samples** to complete;
6. exit and upload the log.

One clean 64-sample run is sufficient initially.

If one stable unique fullscreen-style draw pattern repeats between the Color/swapchain overlap and `Swapchain 0x04 -> 0x00`, use that stage as the Gate I final-output handoff. A state-overlap candidate still does not by itself prove descriptor binding; add narrow descriptor/SRV provenance only if multiple candidates remain ambiguous.

### Capture 30b result — exact final screen-output draw proven

Capture 30b completed two 64-sample runs:

~~~text
Run 1: frame 10775 -> 10838
Run 2: frame 11033 -> 11096
~~~

The first two samples of each run are hook/discovery warm-up. Samples 3-64 in both runs give **124/124 stable frames**.

Every stable frame contains exactly one candidate draw in the proven Color/swapchain overlap window:

~~~text
type          = DrawInstanced
vertices      = 3
instances     = 1
startVertex   = 0
startInstance = 0

Color state        = 0xC0
Swapchain state    = 0x04
Copy-destination state = 0x0
~~~

This is a stable fullscreen-triangle style draw.

The complete late-frame ordering is:

~~~text
Swapchain 0x00 -> 0x04
Color     0x04 -> 0xC0

DrawInstanced(3, 1, 0, 0)

Swapchain 0x04 -> 0x00
Present
~~~

Per-frame summaries report:

~~~text
eventsSinceBoundary        = 15
candidateDrawsSinceBoundary= 1
~~~

for all 124 stable frames.

The candidate command list is always the final engine DIRECT submission before Present:

~~~text
ordinal 7 = 122/124 stable frames
ordinal 8 =   2/124 stable frames
~~~

The two ordinal-8 cases are the previously observed rare eight-submit topology. The production contract is the semantic position — **last engine DIRECT submission before Present** — not the absolute ordinal.

The candidate also follows the active swapchain buffer rather than one fixed address, so the result is consistent with normal multi-buffer presentation.

This closes the native final-output discovery question. Descriptor-level provenance is not required before implementation because the state-based window produces one unique fullscreen-style candidate per stable frame. Add descriptor/SRV tracing later only if production integration contradicts this result.

The Capture 28/30 copy destination remains a real repeated same-format copy target, but its post-copy role is still intentionally unspecified. It must not be treated as the proven final screen-output SRV.

The probe still does **not**:

- dispatch XeSS;
- override SceneView/render size;
- touch ImageQualityRate;
- resize the swapchain;
- modify OptiScaler;
- run in non-RE4 games;
- dump full-frame MV content;
- add a diagnostic barrier to the original VelocityTarget.

### huutaiii MOD screenshot anaysis

A screenshot from the separate **huutaiii RE4 upscaler MOD** was supplied as an external reference. The MOD is not REFramework-based, and no source-code or runtime trace from that MOD has been inspected here.

Treat this subsection as **cross-check evidence only**. The labels visible in the screenshot are useful for comparing pipeline shape, but they do not independently prove that huutaiii's named resources are the exact same native RE4 resources identified by this project.

#### Directly visible screenshot facts

The screenshot reports:

~~~text
Swap chain format
    DXGI_FORMAT_R8G8B8A8_UNORM

HDR
    Off

Render scale
    1

FSRInColor
    1920x1080
    DXGI_FORMAT_R11G11B10_FLOAT

FSRInMV
    1920x1080
    DXGI_FORMAT_R16G16_FLOAT

FSRInDepth
    1920x1080
    DXGI_FORMAT_R32_FLOAT

FSR jitter
    -0.87500, +0.77778

FSRUpscaledColor
    1920x1080
    DXGI_FORMAT_R16G16B16A16_FLOAT

FSRSharpenedColor
    1920x1080
    DXGI_FORMAT_R11G11B10_FLOAT

ToneMapOut
    1920x1080
    DXGI_FORMAT_R11G11B10_FLOAT

ScreenOutPassInput
    1920x1080
    DXGI_FORMAT_R11G11B10_FLOAT

ScreenOutPassOutput
    1920x1080
    DXGI_FORMAT_R8G8B8A8_UNORM
~~~

The screenshot also exposes view, inverse-view, and projection matrices together with the FSR jitter value.

The visible `FSR accumulate` line reports both extents as `uvec2(1920, 1080)`. Combined with `Render scale 1`, this screenshot is a **1:1 input/output-resolution observation**. It does not by itself prove render/display size separation during actual upscaling.

#### Correspondence with this project's RE4 evidence

The screenshot is notably consistent with several independently established RE4 facts:

~~~text
huutaiii screenshot                  This project

FSRInColor R11G11B10_FLOAT    <->   HDR/PostMain Color R11G11B10_FLOAT
FSRInDepth                     <->   Scene::DepthStencilTex
FSRInMV                        <->   Scene::VelocityTarget
FSR jitter                     <->   projection-jitter producer path
R11G11B10 post-upscale stages  <->   HDR/post-processing path
R8G8B8A8 ScreenOut output      <->   SDR swapchain/output family
~~~

The correspondence is strongest at the **pipeline-shape level**, not at raw pointer/name identity.

This project has stronger controlled evidence for render/display separation than the supplied screenshot:

~~~text
SceneView.get_Size = 1920x1080
Color              = 1920x1080
Depth              = 1920x1080
Velocity           = 1920x1080

DXGI swapchain/display
                   = 2560x1440
~~~

Therefore the huutaiii screenshot is consistent with the input grouping already selected for the XeSS producer, but it does not replace the Capture 10/11 size-control evidence.

#### Output-chain implication for Capture 30

The most useful new external clue is the visible post-upscaler chain:

~~~text
FSRUpscaledColor
R16G16B16A16_FLOAT
        ↓
FSRSharpenedColor
R11G11B10_FLOAT
        ↓
SDR tone map
        ↓
ToneMapOut
R11G11B10_FLOAT
        ↓
ScreenOutPass
        ↓
ScreenOutPassOutput
R8G8B8A8_UNORM
        ↓
swapchain
R8G8B8A8_UNORM
~~~

This is consistent with the unresolved edge left by Capture 28.

Capture 28 already proves:

~~~text
HDR/PostMain/Overlay-main Color
R11G11B10_FLOAT
        ↓ CopyResource
stable same-format HDR intermediate
R11G11B10_FLOAT
        ↓
no further tracked copy edge to swapchain
~~~

The huutaiii screenshot therefore provides a plausible external interpretation of the missing **non-copy** portion:

~~~text
HDR intermediate
        ↓
tone-map / final composite stage
        ↓
screen-output pass
        ↓
R8G8B8A8_UNORM swapchain
~~~

This is **not proof** that this project's Capture 28 intermediate is exactly huutaiii's `ToneMapOut`, `FSRSharpenedColor`, or `ScreenOutPassInput`. Those labels come from another implementation and may refer to MOD-owned or differently intercepted resources.

For Capture 30, use these names only as interpretive hints when classifying the observed candidate draw window. The evidence target remains unchanged:

- current-frame HDR intermediate becomes shader-readable;
- an active swapchain buffer becomes `RENDER_TARGET`;
- a graphics draw occurs while both conditions are true;
- the list/ordinal containing that draw is identified;
- descriptor-level provenance is added only if multiple candidate draws remain ambiguous.

Do **not** modify the Capture 30 probe or production design solely to reproduce huutaiii's resource names or formats.

#### Matrix and jitter note

The screenshot's matrix/jitter display is useful corroboration that another RE4 temporal-upscaler implementation also tracks camera transforms plus a pixel-space jitter term.

However, its matrix layout, handedness, frame-history convention, and jitter-sequence generator are not documented by the screenshot. Therefore:

- do not copy the displayed matrix values;
- do not copy the specific jitter sample `-0.87500, +0.77778`;
- keep this project's independently verified RE4 projection-jitter conversion and MV/camera semantics as the production contract.

The screenshot is best treated as **independent convergence on the same broad RE4 temporal/post-processing pipeline**.

Capture 30 adds an important correction to the earlier screenshot interpretation: the Capture 28 copy destination is reproduced, but no current-frame legacy transition proves it shader-readable before Present. Therefore do **not** identify that destination directly with huutaiii's `ToneMapOut`, `FSRSharpenedColor`, or `ScreenOutPassInput`.

Capture 30b now identifies the exact native draw in that window: one `DrawInstanced(3,1,0,0)` fullscreen-triangle style pass per stable frame while Color is shader-readable and the active swapchain is `RENDER_TARGET`. This is structurally compatible with a final `ScreenOutPass`-style stage. The huutaiii resource names remain external analogies and are not adopted as native RE4 identities.

---

## 11. Canonical RE4 temporal-frame contract

Before calling XeSS, RE4-specific engine discovery should be normalized into one internal frame description.

A possible logical contract is:

~~~cpp
struct RE4TemporalFrameInputs
{
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motionVectors = nullptr;
    ID3D12Resource* output = nullptr;

    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;

    float jitterX = 0.0f;
    float jitterY = 0.0f;

    float motionScaleX = 1.0f;
    float motionScaleY = 1.0f;

    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float verticalFov = 0.0f;

    bool depthInverted = false;
    bool jitteredMotionVectors = false;
    bool lowResolutionMotionVectors = false;
    bool reset = false;

    uint64_t frameId = 0;
};
~~~

The exact type names can change. The important rule is that RE4 engine semantics are normalized **before** the XeSS-facing code.

No OptiScaler-internal type belongs in this contract.

---

## 12. Research gates and production validation gates

The reverse-engineering gates needed to start implementation are now closed through the native final-output discovery stage.

The remaining work is no longer broad RE4 pipeline discovery. It is production implementation plus runtime validation of the actual XeSS/OptiScaler/XeFG path.

### Gate A — HUDless Color boundary

Current status:

- HDR/PostMain resource: **verified**;
- separate Overlay working/current surfaces: **verified**;
- `on_pre_overlay_layer_draw()` engine-level insertion boundary: **verified in capture 9**;
- Overlay main == Scene `PostMainTarget` == Scene `HDRTarget`: **50/50 pre-Overlay samples**;
- Overlay main TargetState/resource stability across original Overlay draw: **50/50**;
- complete pixel-level proof that no unrelated earlier UI pass touched HDR/PostMain: not independently established.

For production planning, the Color resource and engine insertion boundary are now considered closed enough to proceed to the next temporal gates. Do not spend additional captures on generic Color/Overlay target discovery.

If later XeSS integration shows actual UI contamination or another contradiction, add the narrowest possible content-sensitive or command-list provenance probe around this exact boundary. Do not return to broad whole-frame tracing.

### Gate B — Render size vs display size

**Status: CLOSED by capture 11.**

Verified causal relationship:

~~~text
SceneView.get_Size override
        ↓
HDR/PostMain Color extent
Depth extent
Velocity extent

DXGI swapchain/display
        remains independent
~~~

Controlled split result:

~~~text
native/original SceneView = 2560x1440
bridge test SceneView     = 1920x1080
Color / Depth / Velocity  = 1920x1080   50/50
DXGI swapchain/display    = 2560x1440   50/50
~~~

Production rules:

- internal render width/height come from the bridge-controlled SceneView size and must match actual Color/Depth/Velocity extents;
- display width/height come from the active DXGI output/swapchain;
- D3D12Hook `renderWidth/renderHeight` is not an authoritative scene-size source;
- `ImageQualityRate` is not required by current evidence and should remain untouched unless a concrete future problem proves otherwise;
- production XeSS preset handling may later obtain the desired XeSS input resolution and apply it through the SceneView path.

### Gate C — Jitter

**Status: CLOSED for RE4 projection injection mechanics.**

Capture 12 proves the native AA/upscaler-off projection-jitter baseline is zero. Capture 13 proves:

- pixel-to-matrix conversion `(+2/W, -2/H)`;
- deterministic phase application to all six SceneInfo variants;
- primary Camera remains an unjittered reference;
- the previous unjittered projection can be rebuilt with the same **current** jitter;
- current/inverse/view-projection matrices remain coherent through pre-Scene draw;
- the engine does not overwrite the injected offsets before Scene draw.

The diagnostic four-phase sequence is only a validation pattern. Production jitter phase generation remains owned by the eventual XeSS integration, but the RE4-side application and history mechanics are now proven.

### Gate D — Motion-vector semantics

**Status: CLOSED by captures 14-18.**

Verified production model:

~~~text
Velocity resource = Scene::VelocityTarget     PROVEN
R = X                                         PROVEN
G = Y                                         PROVEN
camera right -> R positive                    PROVEN
camera left  -> R negative                    PROVEN
camera up    -> G positive                    PROVEN
camera down  -> G negative                    PROVEN
jitteredMotionVectors = false                 PROVEN for tested path

motionScaleX =  renderWidth / 2                PROVEN
motionScaleY = -renderHeight / 2               PROVEN
~~~

Evidence chain:

- Capture 14 proves bridge projection-jitter delta is excluded from sampled static VelocityTarget.
- Capture 15 closes R=X and horizontal polarity.
- Capture 16 closes G=Y and vertical polarity.
- Capture 17 introduces an independent rotation-only screen reprojection witness and strongly supports W/2,-H/2.
- Capture 18 tests adjacent and wider temporal alignments, rejects one fixed MV/camera lag as the primary residual source, and shows repeated near-1:1 agreement on clean rotation-dominated pixels across both axes and both signs.
- Spatially varying residuals within the same frame cannot be explained by a global scale error and are consistent with translation/parallax/foreground/object motion omitted by the rotation-only witness.

No further Depth-assisted reprojection work is required to determine the XeSS MV scale.

### Gate E — Depth convention

**Status: CLOSED by Capture 19.**

Current RE4 SceneInfo/DepthStencilTex uses inverted/reversed D3D depth.

Production XeSS rule:

~~~text
XESS_INIT_FLAG_INVERTED_DEPTH = set
~~~

Evidence:

- primary Camera near/far = 0.01 / 10000.0;
- Camera Z projection matches the normal-depth formula with aggregate error ~= 1e-9;
- SceneInfo Z projection matches the inverted-depth formula with aggregate error ~= 4.7e-8;
- the result is identical across 128/128 samples in four complete runs;
- `sceneDepthInference=inverted` in 128/128 samples.

OptiScaler does not replace this producer responsibility. Its XeSS `auto` path inherits the producer init flag and can then propagate the state to XeFG.

### Gate F — Camera parameters

**Status: CLOSED by Capture 19 for the producer contract.**

Current-build validated sources:

~~~text
cameraNear = via.Camera.get_NearClipPlane()
cameraFar  = via.Camera.get_FarClipPlane()

verticalFov = 2 * atan(1 / SceneInfo.projection[1][1])
~~~

Observed static-state values:

~~~text
near  = 0.01
far   = 10000.0
verticalFov = 0.786246836 rad ~= 45.048625 deg
~~~

Do not hardcode the observed FOV. Derive current metadata per frame so aiming, cutscene, and other projection changes propagate naturally.

### Gate G — Reset/history invalidation

**Status: CLOSED for current RE4 XeSS producer readiness by Captures 20-22.**

Unconditional production reset conditions:

- first valid XeSS frame;
- bridge enable/disable;
- invalid or missing required temporal input;
- render-size change;
- required-resource identity change;
- XeSS context recreation;
- D3D12 device/swapchain recreation.

Captures 20-21 prove that Load Save cannot be inferred from resource identity, frame gaps, or `old_view_projection_matrix`.

Capture 22 supplies the missing explicit RE4-side load window:

~~~text
SceneLoadZoneManager._Pause false -> true
    => arm history-invalid load state

_Pause true -> false
    => do NOT resume history yet

GameSituationManager.InhibitBit returns to the remembered
pre-load normal value
    => first valid gameplay frame uses reset=true
    => resume temporal accumulation
~~~

Production rules:

- snapshot the normal `InhibitBit` value before the load window;
- do not hardcode the observed `0xB9` value;
- keep history invalid across the complete pause/inhibit transition;
- treat `SaveDataManager.CurrentProcess` only as optional corroboration unless its enum semantics are explicitly decoded later;
- do not use translation-only reset heuristics;
- do not freeze a camera-rotation threshold for Load Save.

The Capture 22 quit path changes MainMode/quit/inhibit state but does not raise `SceneLoadZoneManager._Pause`, providing a useful negative control.

Non-load cutscene/teleport hardening may later use another explicit engine signal or a conservative camera-discontinuity fallback. That follow-up is not a blocker for Gate H.

### Gate H — D3D12 execution point and resource states

**Status: CLOSED by Captures 29 and 29b.**

The source-level timing audit still changes the interpretation of Captures 23-27:

- Capture 9 remains the verified semantic `on_pre_overlay_layer_draw()` boundary;
- Captures 23-27 were actually opened from `on_overlay_layer_draw()`, after the original Overlay draw;
- their queue/interface/barrier-API/fence-lifetime findings remain valid;
- their resource-state tuple is post-Overlay evidence.

Capture 29 now adds true pre-Overlay execution-order proof:

- active RE4 queue remains DIRECT;
- the REF-owned empty DIRECT list submits successfully in 128/128 frames;
- sample 2-64 in both runs produce 126/126 complete eight-submit frames;
- true pre-Overlay lies after engine ordinals 1-2;
- the REF-owned list is ordinal 3 in 126/126 stable frames;
- engine ordinals 4-8 follow before Present;
- all 120 observed eight-slot reuses are fence-safe and nonblocking;
- no bridge submission is skipped.

Capture 29b closes the remaining timing-specific item. The shared implementation-level recording hooks, opened from the true pre callback, reproduce the production tuple in 124/124 stable frames:

~~~text
Color    = 0xC0
Depth    = 0xE0
Velocity = 0x04
~~~

This is now a true pre-Overlay production fact rather than a post-Overlay inference.

No engine-owned command list should be modified.

### Gate I — XeSS output integration

**Status (updated 2026-09-30): native discovery is closed; the production XeSS output handoff is implemented and runtime-exercised, but acceptance is incomplete.** Earlier text in this gate describing implementation as wholly pending is superseded by PR66 runtime sections 47–48 below.

Capture 28 proves:

- HDR/PostMain/Overlay-main Color is copied once per stable frame with `CopyResource`;
- source and destination are both 2560×1440 `R11G11B10_FLOAT`, RT+UAV;
- the copy is recorded on the final engine list before Present;
- the destination is a stable HDR intermediate;
- no tracked CopyResource/CopyTextureRegion path continues from that intermediate to the swapchain;
- `reachedSwapchain=false` in 128/128 Present summaries.

Therefore direct copy replacement at the swapchain is not an evidence-backed integration strategy.

Capture 30b closes the remaining native draw-discovery questions:

- exactly one fullscreen-triangle style `DrawInstanced(3,1,0,0)` occurs per stable frame in the Color `0xC0` + swapchain `0x04` overlap;
- the pattern repeats in 124/124 stable frames across two runs;
- the candidate is always on the last engine DIRECT submission before Present;
- the candidate follows the active swapchain buffer;
- the Capture 28/30 copy destination remains at observed state `0x0` in this window and is not the proven final screen-output source.

The latest PR66 capture has eight OutputHandoff install-complete events and public XeSS Execute activity through OptiScaler. This proves that the output resource and engine TargetState handoff reach the live path; an install event alone does not prove the final on-screen pixels or UI composition are correct.

What remains under Gate I is:

- prove the installed output is consumed by the intended downstream GPU submission and map that submission to the eligible Present/queue and its retirement marker;
- complete D3D12 state/lifetime validation for the handoff output and prove safe retirement;
- establish sustained temporal output without the current marker-pending reset churn;
- visually verify final output and preserved UI/HUD;
- validate resize, fullscreen/Alt+Tab, swapchain recreation, and device reset.

Do not add descriptor-heap tracing preemptively. Reopen descriptor/SRV provenance only if the production implementation produces an ambiguity or contradiction.

Historical pd-upscaler remains a concept oracle only. Current RE4 1.5.9 runtime evidence determines the actual integration point.

The preferred ownership rule remains:

- REFramework allocates and releases only bridge-owned XeSS resources;
- engine resources remain borrowed;
- swapchain resources remain owned by the presentation path;
- no foreign COM reference draining or padding is permitted.

### Gate J — OptiScaler / XeFG validation

After standard XeSS works correctly in RE4:

1. verify official upstream OptiScaler intercepts the producer calls without REF-specific changes;
2. validate alternate SR backends through the XeSS frontend;
3. validate `FGInput=Upscaler`;
4. validate XeFG lifecycle with the existing custom REFramework XeFG presentation compatibility work;
5. only then consider optional XeFG HUDless/UI-composition quality improvements.

---

## 13. Recommended implementation structure inside REFramework

Avoid placing production logic in the temporary diagnostic Mod.

A clean end state can use an isolated RE4 namespace/subsystem, for example:

~~~text
src/mods/re4_xess/
    RE4XeSSBridge.hpp
    RE4XeSSBridge.cpp

    RE4TemporalInputs.hpp
    RE4TemporalInputs.cpp

    RE4RenderSize.hpp
    RE4RenderSize.cpp

    RE4Jitter.hpp
    RE4Jitter.cpp

    RE4XeSSContext.hpp
    RE4XeSSContext.cpp
~~~

Exact file names are not mandatory.

Logical responsibilities:

### RE4 temporal provider

Owns:

- Scene selection;
- HDR/PostMain color;
- Depth;
- Velocity;
- camera metadata;
- render/display dimensions;
- reset conditions.

### RE4 render-control layer

Owns:

- render-size override;
- TAA policy;
- image-quality scaling policy;
- jitter injection/restoration.

### XeSS producer

Owns:

- public XeSS context lifecycle;
- XeSS init;
- execution params;
- output resource handling;
- feature recreation/reset.

### Integration/lifecycle coordinator

Owns:

- enable/disable;
- device/reset/resize;
- valid-frame gating;
- interaction with existing D3D12Hook lifecycle.

Do not create a second independent D3D12 presentation-hook owner.

---

## 14. Resource and COM ownership rules

This fork already has substantial XeFG/proxy-swapchain lifecycle work. The RE4 XeSS subsystem must preserve the same ownership discipline.

Rules:

- REFramework releases REFramework-owned references;
- OptiScaler releases OptiScaler-owned references;
- XeFG manages XeFG-owned state;
- a `GetBuffer()` probe releases exactly the reference obtained by that call;
- never drain a COM object until a process-wide refcount reaches a target;
- do not add `AddRef()` padding merely to survive another module's invalid releases;
- destructive resize/reset paths must release only local ownership before forwarding/recreating.

This rule is relevant to both the existing XeFG presentation work and the future RE4 XeSS output resource lifecycle.

---

## 15. Controlled validation configuration

During temporal-input validation, prefer a controlled configuration that minimizes hidden engine temporal behavior.

Recommended baseline when the bridge begins controlling the temporal path:

~~~text
RE4 built-in FSR2        OFF
RE Engine TAA            disabled / NONE where required
Dynamic Resolution       OFF
ImageQualityRate         untouched unless later evidence requires it
Render size              explicitly known
Jitter                   bridge-controlled and logged
~~~

The exact final policy may change after current-build behavior is measured.

The important rule is:

> Do not validate XeSS inputs while an unknown game TAA/dynamic-resolution path silently changes color, depth, MV, or jitter semantics.

---

## 16. Evidence standards

A resource role is not accepted from format/dimensions alone.

Prefer multiple independent witnesses:

### Color

- reflected engine semantic name;
- stable native identity;
- correct position in the render lifecycle;
- expected producer/consumer relationship;
- controlled HUD/menu behavior.

### Depth

- reflected `DepthStencilTex`;
- depth-compatible format/flags;
- stable frame-local relationship;
- later XeSS depth convention validation.

### Motion vectors

- reflected `VelocityTarget`;
- exact identity with the Scene velocity MRT;
- correct dimensions;
- measured scale/sign/jitter convention.

### UI boundary

- engine layer relationship;
- pre/post callback ordering;
- resource identity;
- targeted D3D12 provenance only if semantic evidence remains ambiguous.

Use broad D3D12 tracing only as a last resort. The old ATSBridge trace produced useful topology but saturated rapidly and did not provide semantic role proof.

---

## 17. Current confidence table

| Area | Confidence | Notes |
|---|---|---|
| RE4-only isolation | **HIGH** | Registration/callback fail-closed policy established |
| RE4 Texture +0x18 desc layout | **HIGH** | Runtime validated |
| RE4 D3D12 container path | **HIGH** | Runtime validated |
| Depth identity | **HIGH** | Reflected engine property + stable native resource |
| Velocity identity | **HIGH** | Reflected engine property + exact Scene MRT identity |
| Scene MRT classification | **HIGH** | Real 4-RTV geometry/G-buffer pass |
| HDR/PostMain color identity | **HIGH** | Semantic resource identity proven in capture 8 |
| Overlay separate current surface | **HIGH** | Stable 1920×1080 B8G8R8A8 resource |
| Overlay main → HDR/PostMain | **HIGH** | Exact native identity |
| PrepareOutput presentation ownership | **HIGH** | Exact swapchain buffer identity |
| Pre-Overlay engine boundary | **HIGH** | Capture 9: Overlay main == HDR/PostMain before original Overlay draw in 50/50 paired samples |
| Complete pixel-level HUDlessness | **MEDIUM / deferred** | No contradiction observed; content-sensitive proof only if later integration requires it |
| Native render/display baseline | **HIGH** | Capture 10: SceneView + Color/Depth/Velocity + native DXGI output all 2560x1440 in 50/50 samples |
| SceneView render-size control | **HIGH / PROVEN** | Capture 11: 1920x1080 SceneView override moved Color/Depth/Velocity together in 50/50 samples while DXGI stayed 2560x1440 |
| Render/display split semantics | **HIGH / PROVEN** | Internal temporal extent and presentation extent are independently controllable |
| Jitter | **HIGH / PROVEN** | Capture 12 zero-jitter baseline + Capture 13 six-variant injection/history mechanics |
| MV channel/sign/jitter semantics | **HIGH / PROVEN** | Captures 14-16: jitter excluded; R=X, G=Y; both axis polarities proven |
| MV absolute scale | **HIGH / PROVEN** | Capture 18 closes W/2,-H/2 absolute scale |
| Depth inversion | **HIGH / PROVEN** | Capture 19: SceneInfo/DepthStencilTex is inverted/reversed depth in 128/128 samples |
| Near/far/FOV | **HIGH / PROVEN** | Capture 19: primary Camera near/far + SceneInfo-derived vertical FOV |
| Reset/history rules | **HIGH / PROVEN** | Capture 22: repeated _Pause + dynamic InhibitBit load window; two loads + quit negative control |
| DIRECT queue/list provenance | **HIGH / PROVEN** | Capture 23: 256/256 frames, five DIRECT single-list submits, deterministic ten-list pool |
| Whole-frame DIRECT submit topology | **HIGH / PROVEN** | Capture 24: stable 7/frame after warm-up = 2 before pre-Overlay + 5 after |
| Public GCL0-GCL7 topology | **HIGH / PROVEN** | Capture 25: all public GraphicsCommandList interfaces alias submitted pointer; GCL7 supported |
| Shared D3D12 method provenance | **HIGH / PROVEN** | Capture 25: common Close/Reset/legacy ResourceBarrier/GCL7 Barrier implementations |
| Actual barrier API usage | **HIGH / PROVEN** | Capture 26: repeated legacy ResourceBarrier use; zero enhanced Barrier calls |
| True pre-Overlay input states | **HIGH / PROVEN** | Capture 29b: 124/124 stable true-pre frames reproduce Color=0xC0, Depth=0xE0, Velocity=0x04 |
| RE4 input-state restoration | **HIGH / PROVEN** | Capture 29b reproduces the stable transition cycle and restores the true-pre tuple in 124/124 stable frames |
| Bridge-owned command-list ordering | **HIGH / true pre proven** | Capture 29 proves 126/126 stable true-pre frames with engine 1-2 -> REF ordinal 3 -> engine 4-8 -> Present, plus fence-safe reuse |
| Bridge allocator/list fence lifetime | **HIGH / PROVEN** | Capture 27: all 120 slot reuses occur only after prior fence completion; 128/128 Present fences complete |
| Output Color→HDR intermediate copy | **HIGH / PROVEN** | Capture 28: 124/124 post-warm-up samples use one same-format CopyResource edge |
| Copy-chain reachability to swapchain | **HIGH / REJECTED** | Capture 28: reachedSwapchain=false in 128/128 Present summaries; no second tracked copy edge |
| Final HDR composite/output stage | **HIGH / PROVEN** | Capture 30b: 124/124 stable frames contain exactly one DrawInstanced(3,1,0,0) in the Color=0xC0 + swapchain=0x04 window on the final engine DIRECT list |
| Standard XeSS → upstream OptiScaler | **DESIGN LOCKED, runtime pending** | No custom OptiScaler ABI permitted |
| XeFG through `FGInput=Upscaler` | **pending after SR** | Existing presentation compatibility work remains relevant |

---

## 18. Near-term work order

Research tracing is complete. Production work now runs on `feature/re4-xess`.

Implementation rules:

- base on the current XeFG-compatible `master`, not `pd-upscaler`;
- keep RE4 logic isolated under a dedicated production subsystem such as `src/mods/re4_xess/`;
- port proven semantic accessors/state rules, not the diagnostic capture machinery;
- use the official public XeSS D3D12 API directly;
- first validate Intel XeSS itself with OptiScaler absent;
- then validate stock upstream OptiScaler interception;
- then validate `FGInput=Upscaler` and XeFG lifecycle;
- keep PR #58 / `refactor/re4-temporal-diagnostic` as research history rather than the production base.

Work in this order.

### 18.1 Lock Color as production input

Capture 9 completes the generic Color/boundary discovery phase.

Production rule:

~~~text
Color anchor
    = Overlay::get_main_target_state() native resource
    = Scene::PostMainTarget
    = Scene::HDRTarget

Insertion boundary
    = on_pre_overlay_layer_draw()
~~~

Next implementation work should:

- encode this semantic accessor/selection rule;
- reject `RenderContext::get_render_target()` as the Overlay Color anchor because it is transient;
- remove exploratory Color candidate scanning from eventual production code;
- fail closed if the expected HDR/PostMain/Overlay-main invariant does not hold;
- keep the existing diagnostic available only while later gates are being validated.

### 18.2 Render-size path — complete

Capture 11 proves the production relationship:

~~~text
render size  = bridge-controlled SceneView size
display size = active DXGI swapchain/output size
~~~

Color, Depth, and Velocity must continue to match the selected render size. Do not add `ImageQualityRate` control unless later runtime evidence requires it.

### 18.3 Jitter and motion-vector semantics — complete

Captures 12-18 close:

- native jitter baseline;
- deterministic RE4 jitter injection;
- history treatment;
- sparse MV readback;
- exclusion of jitter delta from MV;
- R = X / G = Y;
- both axis polarities;
- motionScaleX = renderWidth / 2;
- motionScaleY = -renderHeight / 2.

The rotation-only witness is no longer a production dependency. It served as an independent proof tool.

### 18.4 Depth convention + camera metadata — complete

Capture 19 closes:

- reversed/inverted SceneInfo depth;
- `XESS_INIT_FLAG_INVERTED_DEPTH`;
- primary Camera near/far;
- SceneInfo projection structure;
- per-frame vertical-FOV derivation.

No GPU Depth readback is required for this gate.

### 18.5 Reset/history invalidation — complete for current producer readiness

Captures 20-22 establish:

- Load Save may preserve Scene/Camera/resource identity and render-frame continuity;
- `old_view_projection_matrix` does not reset on Load Save;
- translation-only heuristics are unsafe;
- two Load Saves repeat the same `SceneLoadZoneManager._Pause` transition;
- history must remain invalid after pause release until the remembered pre-load GameSituation inhibit state returns;
- the final quit path does not raise `SceneLoadZoneManager._Pause`.

Use the explicit RE4 load window rather than a camera threshold for Load Save.

### 18.6 Prove command-list/state insertion — CLOSED

Captures 23-27 established the D3D12 queue/list/barrier machinery and bridge-owned lifetime model, but the later source audit showed that their Overlay-side boundary was post-Overlay.

Capture 29 now closes the timing-specific submission-order question at the restored true pre callback:

~~~text
engine submit 1
engine submit 2
    -> true pre-Overlay
REF-owned DIRECT submit 3
engine submit 4
engine submit 5
engine submit 6
engine submit 7
engine submit 8
    -> Present
~~~

Keep these now-proven facts:

- one active DIRECT queue;
- REF-owned true-pre insertion after the first two engine submissions;
- bridge-owned allocator/list ring;
- fence-safe, nonblocking reuse.

The old `D3D12 resource states` scenario produced zero target barriers in Capture 29, so do not infer a different state tuple from that run.

Capture 29b has completed two 64-sample runs from true pre-Overlay while retaining the shared implementation hooks proven by Capture 26.

The stable result is:

- Color = `0xC0`;
- Depth subresources 0/1 = `0xE0`;
- Velocity subresource 0 = `0x04`.

The full transition cycle restores that tuple in 124/124 stable frames. Gate H is closed; do not rerun bridge ordering or state capture.

### 18.6.1 Prove output integration path

Capture 28 closes the copy-only hypothesis:

- stable Color -> one same-format HDR intermediate via `CopyResource`;
- 124/124 post-warm-up copy samples;
- the copy's command list is the final engine submission before Present;
- no subsequent tracked CopyResource/CopyTextureRegion edge;
- no copy-connected swapchain buffer in 128/128 Present summaries.

Therefore do not integrate XeSS by assuming an HDR -> swapchain copy chain.

Capture 30 has completed and narrows the final output path:

- the current-frame Color -> same-format copy destination is rediscovered in 124/124 stable frames;
- no current-frame legacy barrier establishes that copy destination as shader-readable before Present;
- active swapchain transitions `0x00 -> 0x04`;
- while the swapchain is RT, Color transitions `0x04 -> 0xC0`;
- the swapchain then transitions `0x04 -> 0x00`;
- this late sequence is on the final engine DIRECT submission before Present.

Capture 30b has completed the native output-discovery phase:

- 124/124 stable frames contain exactly one candidate;
- candidate = `DrawInstanced(3,1,0,0)`;
- Color = `0xC0`;
- active swapchain = `0x04`;
- copy destination state = `0x0`;
- candidate list = final engine DIRECT submission before Present.

Do not add more broad D3D12 tracing at this point.

The next work belongs on `feature/re4-xess`: implement the first RE4-only public XeSS D3D12 producer using the proven temporal inputs, command-list ordering, state contract, reset logic, and output-stage evidence. Add another diagnostic only in response to a concrete production contradiction.

### 18.7 Add public XeSS producer

Call the official XeSS D3D12 API directly.

First milestone:

> Correct RE4 XeSS SR using Intel XeSS itself, with HUD/UI preserved outside the upscaled scene path.

### 18.8 Validate upstream OptiScaler

With the same producer code:

- load stock upstream OptiScaler;
- verify interception;
- verify backend substitution;
- validate frame data and output.

No REF-specific OptiScaler patch is allowed to become a requirement.

### 18.9 Validate XeFG

After SR is stable:

- `FGInput=Upscaler`;
- generated frames;
- resize/fullscreen/Alt+Tab;
- long-session lifecycle;
- UI interpolation quality.

Optional XeFG UI-composition enhancements come afterward.

---

## 19. Explicit non-goals

Do not expand this work into:

- a generic temporal-upscaler framework for every RE Engine game;
- D3D11 support;
- VR/multi-eye support;
- a standalone ATSBridge runtime DLL;
- a custom OptiScaler ABI;
- a custom OptiScaler fork requirement;
- NGX/DLSS producer emulation;
- FSR producer emulation;
- broad command-list tracing without a specific unanswered question;
- aggressive foreign COM-ref cleanup;
- automatic propagation of RE4 layout fixes to other titles.

These can be revisited only with separate evidence and requirements.

---

## 20. Definition of implementation readiness

The RE4 bridge is ready to begin production XeSS dispatch only when all of the following are known without guessing:

- exact HUDless scene-color resource and safe insertion boundary;
- exact Depth resource and depth convention;
- exact Motion Vector resource and scale/sign/jitter semantics;
- render size and display size;
- jitter sequence and injection timing;
- near/far/FOV;
- reset/history-invalid rules;
- output ownership;
- command list;
- queue ordering;
- required resource states/barriers;
- resize/device lifecycle.

The reverse-engineering project is now **at the implementation-start gate**.

This does not mean production validation is finished. It means the remaining unknowns are best resolved by the real XeSS implementation rather than by broader passive tracing.

The major resource, timing, state, and native output-stage uncertainty is now closed strongly enough to start production. HDR/PostMain Color, Depth, Velocity, the true pre-Overlay boundary, SceneView-driven internal render size, Overlay working surfaces, presentation ownership, jitter, MV semantics, inverted depth, camera metadata, reset/history invalidation, DIRECT queue/list topology, resource states, and bridge-owned list/fence lifetime are all mapped. Capture 29/29b close the true-pre execution/state contract. Capture 28 rejects a copy-only swapchain path. Capture 30 narrows the final output window, and Capture 30b closes the native final-draw discovery with one stable fullscreen-triangle style draw in 124/124 stable frames on the last engine DIRECT submission before Present.

The next evidence milestone is not another generic capture. It is a real standard XeSS D3D12 dispatch on `feature/re4-xess`, followed by controlled validation of the actual output handoff, lifecycle, upstream OptiScaler interception, and XeFG.

---

## 21. Current bottom line

The RE4 temporal pipeline is no longer an unknown black box.

Current evidence supports:

~~~text
Scene MRT / G-buffer
    ├─ DepthStencilTex
    └─ VelocityTarget (also Scene MRT3)
             │
             ▼
HDRTarget / PostMainTarget
R11G11B10_FLOAT
             │
             ├─ Overlay main target points here
             └─ Overlay also has a separate B8G8R8A8 UI-like current surface
             │
             ├─ CopyResource (Capture 28/30)
             ▼
same-format HDR copy destination
R11G11B10_FLOAT
             │
             └─ post-copy role not yet proven

HDR/PostMain Color
R11G11B10_FLOAT
             │
             ├─ final list: Color -> 0xC0
             │   while active swapchain -> 0x04
             │
             ├─ final fullscreen draw (Capture 30b PROVEN)
             │   DrawInstanced(3,1,0,0)
             ▼
swapchain backbuffers
             └─ 0x04 -> 0x00 -> Present
~~~

Capture 9 identifies `on_pre_overlay_layer_draw()` as the verified engine-level insertion boundary and the semantic Overlay-main / HDR/PostMain resource as the production Color anchor. Captures 10-18 close render/display control, jitter injection/history, and MV semantics. Capture 19 closes inverted depth plus near/far/FOV/projection metadata, and Capture 22 closes the current Load Save reset/history gate. Captures 23-27 prove the DIRECT queue/list population, public GCL topology, shared recording implementations, actual legacy ResourceBarrier usage, post-Overlay state behavior, and safe REF-owned allocator/list/fence lifetime. The source-level timing audit corrected their original pre/post interpretation. Capture 29 proves the true-pre REF-owned ordinal-3 insertion, and Capture 29b closes Gate H completely with Color=0xC0, Depth=0xE0, Velocity=0x04 at the actual pre-Overlay boundary in 124/124 stable frames. Capture 28 proves a repeated HDR/PostMain -> same-format CopyResource edge and rejects a copy-connected route to the swapchain. Capture 30 proves the final engine list and the Color-readable + swapchain-RT window. Capture 30b then closes the native final-output discovery: 124/124 stable frames contain exactly one `DrawInstanced(3,1,0,0)` fullscreen-triangle style pass in that window, always on the last engine DIRECT submission before Present.

The reverse-engineering phase is therefore complete for the first production milestone. Implementation now moves to `feature/re4-xess`, based on the existing XeFG-compatible `master`. The production goal remains to insert standard XeSS SR at the pre-Overlay HDR scene boundary, preserve RE4's own Overlay/UI path, and let unmodified upstream OptiScaler intercept the standard XeSS producer calls for alternate SR and XeFG.

The diagnostic branch remains passive and should now be treated as an evidence/archive branch. New production XeSS dispatch belongs only on `feature/re4-xess`.


---

## 21. Production architecture handoff

The reverse-engineering record above remains the source of truth for what was proven.

The final production target architecture for implementation on feature/re4-xess is defined in:

~~~text
doc/RE4_XESS_PRODUCTION_ARCHITECTURE_2026-09-27.md
~~~

That document is the source of truth for what should now be built. If implementation contradicts a proven runtime fact, update the evidence record first instead of silently changing a frozen production assumption.

---

## 22. PR4 first production runtime validation — callback thread assumption corrected

Runtime evidence was collected after PR4 merge using:

~~~text
GoogleDrive/ETS2ATS/RE4/PR4 Log/re2_framework_log.txt
GoogleDrive/ETS2ATS/RE4/PR4 Log/OptiScaler.log
~~~

REFramework runtime log build identity:

~~~text
Commit hash: 6484ec35ac043d441c857c929552ad7a0246bf39
Build date: 27.09.2026
Game: re4
~~~

OptiScaler runtime identity:

~~~text
OptiScaler v10.0.0-dev
commit/log identity: 44cfee4d
~~~

### 22.1 OptiScaler deployment was valid

OptiScaler successfully loaded:

~~~text
<RE4>\OptiScaler\libxess.dll
<RE4>\OptiScaler\libxess_dx11.dll
<RE4>\OptiScaler\libxess_fg.dll
~~~

Therefore the tested deployment layout itself was valid.

### 22.2 REFramework runtime discovery stopped incorrectly on candidate 1

REFramework attempted the supported order:

~~~text
1. <RE4>\libxess.dll
2. <RE4>\OptiScaler\libxess.dll
~~~

Candidate 1 did not exist.

The current implementation converted that expected miss into:

~~~text
Could not inspect runtime candidate:
The system cannot find the file specified.
~~~

and aborted before candidate 2.

This is an implementation bug, not a deployment contradiction.

The production rule is now:

~~~text
candidate 1 missing
    -> continue

candidate 2 exists
    -> select exact path
    -> LoadLibraryExW exact path
~~~

Unexpected file-inspection errors remain fatal.

### 22.3 The true pre-Overlay semantic boundary is stable, but its CPU thread identity is not

The first active callback logged:

~~~text
owner thread established: 1304
pre-Overlay callback owner verified:
    ownerThread=1304 callbackThread=1304
~~~

Approximately 5 ms later the same semantic callback arrived on:

~~~text
thread 28692
~~~

and the implementation quarantined itself.

This disproves only the earlier CPU-thread-affinity assumption.

It does **not** invalidate:

- the true pre-Overlay semantic boundary;
- its resource identities;
- its queue ordering;
- Color/Depth/Velocity state evidence;
- the output-handoff timing evidence.

Production correction:

~~~text
true pre-Overlay callback
    = semantic ordering boundary
    != XeSS API owner thread
~~~

All public XeSS calls move to one dedicated RE4XeSS worker thread.

The varying pre-Overlay callback synchronously dispatches CPU record/submit work to that worker, then continues engine-side handoff only after the worker returns.

### 22.4 No XeSS producer execution was reached in this capture

Neither REFramework nor OptiScaler logged producer calls corresponding to:

~~~text
xessD3D12CreateContext
xessGetOptimalInputResolution
xessD3D12Init
xessSetVelocityScale
xessD3D12Execute
~~~

Therefore this capture does not validate or invalidate:

- RG16F motion-vector conversion;
- bridge command-list execution;
- PR4 TargetState output handoff;
- COMMON/UAV/0xC0 output-state contract;
- visible SR quality;
- OptiScaler backend substitution;
- XeFG.

The next production retest must first clear runtime discovery and dedicated-worker ownership, then resume validation from context creation onward.

### 22.5 Corrective implementation work order

~~~text
doc/RE4_XESS_PR4_RUNTIME_BLOCKER_FIX_WORK_ORDER_2026-09-27.md
~~~

This correction supersedes the earlier assumption that the pre-Overlay callback thread itself can own the XeSS API.

---

## 23. PR4-Log after PR65 — standard XeSS producer path reached OptiScaler; Load Save accessor is the next blocker

Runtime evidence:

~~~text
GoogleDrive/ETS2ATS/RE4/PR4-Log/re2_framework_log.txt
GoogleDrive/ETS2ATS/RE4/PR4-Log/OptiScaler.log
~~~

### 23.1 Dedicated worker and runtime discovery correction succeeded

Observed:

~~~text
[RE4XeSS][Worker] started thread=29452

candidate 1:
    <RE4>\libxess.dll
    missing -> continued

candidate 2:
    <RE4>\OptiScaler\libxess.dll
    selected

LoadLibraryExW exact path = success
~~~

The true pre-Overlay callback moved across multiple RE Engine threads while the dedicated worker remained stable.

No callback-thread migration quarantine occurred.

### 23.2 Stock OptiScaler is already loaded and intercepting the public producer API

OptiScaler logged:

~~~text
OptiScaler v10.0.0-dev loaded
working as dxgi.dll
XeSSProxy::InitXeSS LoadResult: true
~~~

and intercepted:

~~~text
hk_xessGetVersion
hk_xessD3D12CreateContext
hk_xessGetOptimalInputResolution
hk_xessD3D12Init
hk_xessSetVelocityScale
~~~

For the first Ultra Quality configuration both sides agreed on:

~~~text
display       = 2560x1440
optimal input = 1969x1107
velocityScale = (984.5, -553.5)
~~~

Therefore the standard:

~~~text
REFramework
    -> public XeSS API
    -> stock OptiScaler XeSS frontend
~~~

producer path is now runtime-proven through initialization.

The early OptiScaler warning:

~~~text
Config::CheckUpscalerFiles libxess.dll not found!
~~~

does not represent the final load result in this capture; OptiScaler subsequently loaded `OptiScaler\libxess.dll` successfully.

### 23.3 Actual XeSS dispatch was still never reached

Observed counts:

~~~text
OptiScaler hk_xessGetVersion          = 18
OptiScaler hk_xessD3D12CreateContext  = 18
OptiScaler hk_xessD3D12Init           = 18
OptiScaler hk_xessSetVelocityScale    = 18

RE4XeSS Worker submit                 = 0
RE4XeSS first resetHistory packet     = 0
RE4XeSS Output handoff                = 0
OptiScaler hk_xessD3D12Execute        = 0
~~~

The repeated context recreation corresponds to user-driven quality-mode changes during the test.

### 23.4 Production Load Save accessor failed continuously

Observed:

~~~text
load-state-observation-unavailable = 18,743
load-history-invalid               = 18,726
~~~

No valid production load snapshot was recorded.

Current production accessor attempts:

~~~text
chainsaw.SceneLoadZoneManager
    get_Instance
    _Pause

chainsaw.GameSituationManager
    get_Instance
    InhibitBit
~~~

The startup type registry log proves both manager types exist, but the production accessor currently collapses type/method/field/context/instance/raw-field failures into one `std::nullopt`.

This prevents identifying the exact contradiction.

### 23.5 Capture 22 semantics remain valid

Do not discard the proven load window:

~~~text
_Pause false -> true
    => history invalid

_Pause true -> false
    => still invalid

InhibitBit returns to remembered normal baseline
    => first resumed frame resetHistory = true
~~~

The current blocker is accessor implementation, not the state-machine model.

### 23.6 Next work order

~~~text
doc/RE4_XESS_LOAD_STATE_ACCESSOR_DIAGNOSTIC_WORK_ORDER_2026-09-27.md
~~~

The next runtime milestone is:

~~~text
valid LoadAccessor snapshot
    -> first temporal frame packet
    -> Worker submit
    -> OptiScaler hk_xessD3D12Execute
    -> PR4 OutputHandoff validation resumes
~~~




---

## 24. PR66 handoff checkpoint — LoadAccessor resolved, TargetState creation remains the production blocker

This section supersedes the older statement in Section 23 that the Load Save accessor is the next blocker.

Active PR:

~~~text
PR #66
branch: feature/re4-xess-load-state-accessor-diagnostic
state: Draft / open / unmerged
~~~

Latest runtime capture used for this checkpoint:

~~~text
re2_framework_log(10).txt
log header commit: 88d6714eda4869661335de89ea86298c5fe29512
branch: feature/re4-xess-load-state-accessor-diagnostic
build date/time: 2026-09-28 13:30
game: re4
~~~

The runtime functionality in this local build includes the TargetState-vtable discovery work represented by the current PR66 development line. Local build stamping may therefore lag the exact remote head; use the runtime behavior as the evidence and Git history as the implementation record.

### 24.1 LoadAccessor is now runtime-proven

The production accessor reaches:

~~~text
[RE4XeSS][LoadAccessor] stage=Valid
~~~

with the concrete TDB71 schema:

~~~text
pause manager:
    chainsaw.SceneLoadZoneManager
    get_Instance
    _Pause
    System.Boolean
    managed storage width = 1

situation manager:
    chainsaw.GameSituationManager
    get_Instance
    <InhibitBit>k__BackingField
    System.UInt64
    managed storage width = 8
~~~

Important implementation rule:

> REFramework metadata type size is not the managed primitive storage width.

The runtime must continue deriving Boolean/integral/enum widths from managed primitive identity instead of using the TDB metadata size directly.

### 24.2 Capture 22 load semantics survived production validation

The latest runtime begins while the game is already in a pause/load state:

~~~text
_Pause = true
InhibitBit = 0x0
~~~

No normal gameplay baseline is invented.

Later:

~~~text
_Pause true -> false
    -> keep temporal history blocked

InhibitBit 0x0 -> 0xB9
    -> begin stable post-pause rebaseline

three stable observations
    -> adopt current normal baseline dynamically
    -> first valid packet uses resetHistory=true
~~~

Observed production log:

~~~text
load pause released; keeping history blocked until a post-pause InhibitBit transition and stable rebaseline
post-pause InhibitBit transition 0x0->0xb9; beginning stable rebaseline observations
adopted post-pause InhibitBit baseline=0xb9 after three stable observations
first valid resetHistory packet
~~~

0xB9 is evidence from this run only.

Do not hardcode it.

The frozen semantics remain:

- remember the stable gameplay InhibitBit value dynamically;
- if InhibitBit departs before Pause rises, freeze the remembered normal value and arm load suspicion;
- Pause=true confirms the load window;
- Pause=false alone does not resume temporal history;
- resume only after InhibitBit returns/rebaselines to a stable normal value;
- the first valid gameplay packet after the load window sets resetHistory=true;
- startup while Pause=true never invents a baseline;
- no camera-threshold fallback.

The LoadAccessor is no longer the current blocker.

---

## 25. PR66 TargetState / OutputHandoff reverse-engineering status

The current production blocker is the engine-visible TargetState required by RE4XeSSOutputHandoff.

The following parts are runtime-proven:

~~~text
first valid temporal packet                 ✅
RE4 semantic Color/Depth/Velocity packet    ✅
public XeSS runtime discovery/init           ✅
stock OptiScaler public XeSS interception    ✅ through init/velocity scale
create_render_target_view                    ✅
create_texture                               ✅
live Overlay main TargetState layout         ✅
live Overlay main TargetState slot (+0x90)   ✅
live TargetState vtable anchor               ✅
create_target_state                          ❌ unresolved
distinct single-RTV handoff TargetState      ❌
Worker XeSS submit                           ⏸ blocked before submit
OptiScaler hk_xessD3D12Execute               0 calls
OutputHandoff install                        ⏸
downstream retirement validation             ⏸
~~~

### 25.1 RE4 RTV resolver is closed

Generic signatures may fail on RE4 1.5.9.0, but the RE4-only validated callsite fallback succeeds.

Stable evidence:

~~~text
callsite RVA: 0x447AF5A
resolved target: re4+0x4470470
runtime log:
    Found create_render_target_view via RVA 0x447AF5A
~~~

create_texture also resolves.

Do not reopen the RTV resolver unless new evidence contradicts it.

### 25.2 create_target_state remains intentionally fail-closed

The generic legacy CircularDOF_SceneMipTexture / call-order candidate is not accepted for RE4.

Current behavior remains correct:

~~~text
Searching for create_target_state
[Renderer][RE4] TargetState factory is unresolved; refusing the legacy call-order candidate
TargetState::clone did not produce a distinct single-RTV handoff target
~~~

Do not convert any diagnostic RVA into a production call until the ABI, ownership, descriptor semantics, and returned object are proven.

### 25.3 Rejected TargetState candidates — do not repeat these experiments

#### Site 2 / re4+0x44C7A27

Paired returns were scalar-like values (0x1EB, 0x5A0) rather than TargetState pointers.

Status:

~~~text
re4+0x44C7A27 -> rejected as TargetState factory
~~~

#### Site 1 / re4+0x47212A6

This is a polymorphic vtable + 0x40 dispatch site, not a fixed factory.

Observed dynamic callees from the same callsite included:

~~~text
re4+0x44742F0
re4+0x447A540
re4+0x78F42D0
re4+0x44AF030
~~~

Status:

~~~text
re4+0x47212A6 -> rejected as fixed TargetState factory
~~~

#### re4+0x78F42D0 return

The return is readable and superficially follows the same coarse 0x40-byte layout:

~~~text
valid image vtable
refCount = -2
numRtv = 1
display-sized rect = 2560x1440
~~~

but decisive validation fails:

~~~text
candidate vtable != live Overlay TargetState vtable
candidate rect = display extent, not current Overlay target extent
rtvs[0] = nullptr
no valid RTV format/dimension/texture
targetStateLike = false
~~~

Vtable mismatch alone would not reject a subclass, but numRtv=1 with a null RTV entry means this is not the complete usable TargetState required by OutputHandoff.

Status:

~~~text
re4+0x78F42D0 return -> rejected as usable live TargetState
~~~

It may remain a rendering-state/container clue, but must not be wired into create_target_state().

### 25.4 re4+0x4597A0 writer candidate is retired

Static analysis showed a plausible intrusive-pointer style assignment pattern around:

~~~text
source owner + 0x60
AddRef incoming
receiver + 0x90 assignment
Release previous
~~~

The initial provenance probe was accidentally coupled to the short TargetStateProbe and only lived for two frames. That lifecycle bug was fixed and a later run observed the candidate for the full 1800-frame bounded window with zero writes.

A final timing test then moved the hook to the core REFramework startup path after integrity bootstrap and before the first render frame.

Required ordering was achieved:

~~~text
TargetState provenance early arm
    < Render frame: 1
~~~

Even with the hook proven active before rendering, the bounded run recorded:

~~~text
writes = 0
earlyCaptured = 0
earlyCorrelated = 0
~~~

Status:

~~~text
re4+0x4597A0 -> retired as Overlay main TargetState writer candidate
~~~

Do not move this hook earlier again.

Do not add it back as an active writer candidate.

---

## 26. Proven live TargetState vtable anchor and latest discovery blocker

The strongest current TargetState reverse-engineering anchor is the real live Overlay main TargetState object.

For the exact validated RE4 1.5.9.0 image:

~~~text
image base in latest run  = 0x7ff631bd0000
TargetState vtable RVA    = 0x7B1C148
expected vtable           = 0x7ff6396ec148
live TargetState vtable   = 0x7ff6396ec148
match                     = true
~~~

The fixed RVA is diagnostic evidence for this exact image identity only.

Do not generalize it to other RE Engine games or unvalidated RE4 builds.

### 26.1 Latest TargetStateVtableProbe result

The new bounded discovery pass starts very early:

~~~text
[TargetStateVtableProbe] bootstrap-begin
point=REFramework-constructor-after-integrity
before-plugin-init=true

[TargetStateVtableProbe] discovery-begin
imageSize=0xe405000
checksum=0xdee3479
vtableRva=0x7b1c148
~~~

The current full executable-section decoder does not complete:

~~~text
decode stopped:
    sectionIndex=0
    sectionRva=0x1000
    offset=0xb73860

decode stopped:
    sectionIndex=7
    sectionRva=0xe3cb000
    offset=0x0

discovery-summary:
    xrefCount=0
    truncated=false
    complete=false
~~~

This is an incomplete scan, not evidence that no vtable xrefs exist.

The current log line:

~~~text
bootstrap-result discovered=true
~~~

is misleading because the scan was attempted but not completed and discovered zero xrefs.

### 26.2 Live anchor is correct but incorrectly marked untrusted

Later the real Overlay TargetState proves the anchor:

~~~text
live-anchor:
    liveVtable         = expectedVtable
    match              = true
    discoveryComplete  = false
    trusted            = false
~~~

Current code couples:

~~~text
live anchor trust
    = exact live vtable match
      AND full static xref scan completed
~~~

That coupling is now the immediate diagnostic blocker.

Because trusted=false, the bounded TargetState probe refuses to arm:

~~~text
[TargetStateProbe] not armed: live TargetState vtable anchor is untrusted
~~~

The production handoff then reaches the already-known failure:

~~~text
create_render_target_view ✅
create_texture            ✅
create_target_state       ❌
TargetState::clone        ❌
~~~

No xessD3D12Execute is reached.

### 26.3 Required correction already issued on PR66

PR66 comment:

~~~text
5863592995
~~~

Required change:

1. keep exact RE4 image identity validation;
2. keep the validated Overlay slot;
3. keep readable live TargetState structural validation;
4. require exact liveVtable == imageBase + 0x7B1C148;
5. keep existing callsite byte validation before installing any dynamic hook;
6. do not require full-image xref scan completion merely to trust the read-only live anchor diagnostic.

The static xref scanner must be repaired independently.

Preferred bounded approaches:

~~~text
A. decode per proven x64 RUNTIME_FUNCTION range from the PE exception directory
   so one data island/bad range does not terminate the rest of .text;

or

B. candidate-driven RIP-relative reference search
   -> fully decode/validate only candidate instruction starts
   -> accept only references resolving exactly to imageBase + 0x7B1C148
~~~

Do not use blind byte resynchronization solely to force complete=true.

Discovery logging must distinguish:

~~~text
attempted
complete
xrefCount
truncated
~~~

rather than reporting discovered=true for an incomplete zero-xref pass.

### 26.4 Next acceptance point

The next runtime capture should first prove:

~~~text
TargetStateVtableProbe live-anchor:
    match=true
    trusted=true

TargetStateProbe:
    armed
~~~

Static xref discovery may continue to report incomplete coverage while that diagnostic correction is being validated; it must not silently become production TargetState resolution.

The primary reverse-engineering goal remains:

> Find the actual TargetState allocation/constructor/factory path from the proven live TargetState type, then prove its ABI and ownership contract.

Do not productionize 0x78F42D0, 0x4597A0, or any newly found xref from static evidence alone.

---

## 27. XeSS / OptiScaler teardown note from the latest capture

The latest log reports:

~~~text
xessDestroyContext returned XeSS result -8
~~~

XeSS defines -8 as:

~~~text
XESS_RESULT_ERROR_INVALID_CONTEXT
~~~

In current stock OptiScaler, hk_xessDestroyContext() returns INVALID_CONTEXT when its internal context map has no entry.

That map is populated on the Execute/CreateDLSSContext path.

This capture never reaches the first xessD3D12Execute, because OutputHandoff fails earlier.

Therefore for the current evidence:

~~~text
xessDestroyContext -8
    = downstream teardown symptom
    != current primary blocker
~~~

Do not widen PR66 into an XeSS teardown redesign unless this becomes independently reproducible after Execute is actually reached.

---

## 28. External corroboration — DLSS5-Feeder does not change the RE4 architecture

Repository reviewed:

~~~text
https://github.com/jlrouzies-fr/DLSS5-Feeder
~~~

That project demonstrates a closely related integration pattern:

~~~text
synthetic standard NGX/DLSS producer calls
    -> stock OptiScaler interception
    -> OptiScaler-selected SR backend
~~~

This supports the general producer/interceptor design already used here.

It does not solve the current RE4 OutputHandoff problem.

DLSS5-Feeder operates from a ReShade/post-process style boundary and can write the processed result back to the completed frame/backbuffer. It therefore does not need to manufacture an RE Engine TargetState or re-enter RE4 before Overlay/UI.

The RE4 production target is different:

~~~text
low-resolution RE4 SceneView
    -> pre-Overlay HDR/PostMain Color + true Depth + true Velocity + true Jitter
    -> public XeSS producer
    -> display-resolution SR output
    -> engine-visible TargetState handoff
    -> native RE4 Overlay/UI/final output
    -> Present
~~~

Therefore:

- keep public XeSS as the RE4 producer contract;
- do not switch to a synthetic NGX producer merely because DLSS5-Feeder works;
- do not adopt its final-backbuffer copy as the normal RE4 output architecture;
- the current blocker remains RE Engine TargetState creation/handoff.

---

## 29. Handoff state — 2026-09-28

A new engineer/session can continue from this exact state.

### Frozen proven facts

~~~text
RE4-only isolation                         proven
true pre-Overlay semantic boundary         proven
Color = HDR/PostMain                       proven
Depth = Scene::DepthStencilTex             proven, inverted
Velocity = Scene::VelocityTarget           proven
MV conversion/sign/scale                   proven
SceneView render-size control              proven
jitter injection/history                   proven
near/far/FOV                               proven
LoadAccessor                               proven Valid
Capture 22 load-state semantics            proven in production behavior
dedicated XeSS worker ownership            proven
public XeSS -> stock OptiScaler init path  proven
RTV factory                                proven
create_texture                             proven
live Overlay TargetState layout            proven
Overlay main TargetState slot +0x90        proven
live TargetState vtable RVA 0x7B1C148      proven for exact RE4 image
~~~

### Rejected/retired leads

~~~text
legacy generic create_target_state candidate   rejected for RE4
0x44C7A27                                     rejected as TargetState factory
0x47212A6                                     polymorphic dispatch, not fixed factory
0x78F42D0 return                              rejected as usable TargetState
0x4597A0                                      retired as Overlay TargetState writer
direct normal-path XeSS output -> swapchain   rejected architecture
~~~

### Current blocker

~~~text
actual RE4 TargetState creator/factory ABI    unknown
    -> no distinct single-RTV handoff TargetState
    -> no OutputHandoff install
    -> no worker XeSS submit
    -> no hk_xessD3D12Execute
~~~

The immediately preceding diagnostic blocker is narrower:

~~~text
live TargetState vtable anchor matches exactly
but
static xref scan incomplete
    -> current code marks anchor untrusted
    -> TargetStateProbe does not arm
~~~

### Next action

Implement the narrow PR66 correction from comment 5863592995:

~~~text
decouple live-anchor trust from full static-xref scan completeness
repair xref discovery independently
keep all production paths fail-closed
~~~

Then collect the next runtime log.

Do not merge PR66 yet.

Do not start XeFG validation yet.

Do not add another broad D3D12 trace.

Do not bypass OutputHandoff by copying XeSS output directly to the swapchain.

The next meaningful milestone is a proven TargetState construction path and a distinct engine-visible single-RTV handoff state.


---

## 30. PR66 latest runtime — trust gate fixed and two TargetState-vtable xrefs discovered

Latest runtime evidence:

~~~text
file: re2_framework_log(20260928-045315).txt
commit header: a72333a1a9944c156bf947e43cfab9410877867c
branch: feature/re4-xess-load-state-accessor-diagnostic
build date/time: 2026-09-28 13:47
~~~

Important handoff note:

> The local runtime contains diagnostic changes that are not yet fully represented by the current remote source at the same stamped commit.

The runtime clearly uses:

~~~text
exception-directory / RUNTIME_FUNCTION coverage
live-anchor trust decoupled from discoveryComplete
~~~

while the remote source at the stamped commit still reflects the older executable-section scanner / stricter trust gate.

Before the next diagnostic change, preserve and push the exact local implementation that produced this runtime.

### 30.1 Previous trust-gate blocker is resolved

Runtime now proves:

~~~text
[TargetStateVtableProbe] live-anchor
    liveVtable=0x7ff6396ec148
    expectedVtable=0x7ff6396ec148
    imageIdentityValid=true
    match=true
    discoveryComplete=false
    trusted=true
~~~

The bounded TargetState probe subsequently arms:

~~~text
[TargetStateProbe] armed
    frame=5285
    isolatedSite=1
    siteName=RTV owner vcall+0x40 A
~~~

Therefore:

~~~text
live TargetState exact match     ✅
live anchor trusted              ✅
TargetStateProbe arm             ✅
full xref scan complete          ❌ not required for trust
~~~

Do not restore the old rule:

~~~text
trusted = match && discoveryComplete
~~~

### 30.2 RUNTIME_FUNCTION discovery found two exact vtable xrefs

The new scanner reports:

~~~text
coverage=exception-directory-runtime-functions
functions=508868
scannedFunctions=507568
failedFunctions=1300
scannedBytes=0x74be117
xrefCount=2
truncated=false
complete=false
~~~

The two positive xrefs are:

~~~text
xref 0
    RVA = 0x47C3E0A
    instruction = LEA rax, [rel imageBase+0x7B1C148]
    bytes = 48 8D 05 37 83 35 03

xref 1
    RVA = 0x47D21AF
    instruction = LEA rax, [rel imageBase+0x7B1C148]
    bytes = 48 8D 05 92 9F 34 03
~~~

These are exact decoded RIP-relative references to the proven TargetState vtable.

complete=false means discovery is not exhaustive.

It does not invalidate the two positive xrefs already found.

The two RVAs are now the strongest static leads for the actual TargetState constructor/initializer/destructor/type-use path.

They are not production factory RVAs yet.

### 30.3 Old site-1/provider-return path is exhausted

The old isolated site-1 probe ran again.

The same polymorphic callees appeared, including:

~~~text
re4+0x44742F0
re4+0x447A540
re4+0x78F42D0
re4+0x44AF030
~~~

The re4+0x78F42D0 return again validated as unusable:

~~~text
numRtv=1
rect=(0,0,2560,1440)
overlayVtableMatch=false
rtv0=0
rtv0Valid=false
targetStateLike=false
~~~

This reconfirms the prior rejection.

Do not spend the next runtime capture on the same site-1/provider-return probe.

### 30.4 Production blocker is unchanged

The latest runtime still reaches:

~~~text
create_render_target_view  ✅
create_texture             ✅
create_target_state        ❌ unresolved
distinct TargetState       ❌
OutputHandoff install      ❌
Worker submit              0
hk_xessD3D12Execute        0
~~~

Repeated mode changes recreate the public XeSS frontend successfully, but every first valid temporal packet still fails at TargetState cloning/handoff.

The repeated:

~~~text
xessDestroyContext result -8
~~~

remains the known downstream INVALID_CONTEXT teardown symptom before first Execute, not the primary blocker.

### 30.5 Next work order

PR66 follow-up comment:

~~~text
5863746550
~~~

The next commit should classify only the two discovered vtable xrefs.

For each xref, capture its containing RUNTIME_FUNCTION:

~~~text
xrefRva
functionIndex
functionBeginRva
functionEndRva
xrefOffsetWithinFunction
~~~

and a strictly bounded decoded instruction window around it.

The immediate questions are:

~~~text
where does the LEA-loaded vtable address flow?
is it stored to object + 0?
which register is the candidate object?
is there a nearby allocation/base-constructor call?
is there a release/deallocation path?
does the function return the object?
are the two xrefs constructor/destructor pairs, overloads, or type-query helpers?
~~~

Do not dynamically hook both functions yet.

First classify the static contexts, then select the strongest single constructor/initializer candidate for a later bounded read-only dynamic probe.

### 30.6 Diagnostic logging cleanup

The RUNTIME_FUNCTION scan is structurally better because decode failure in one function does not terminate the scan.

However, the latest run emits approximately 1300 individual decode-stopped warnings.

Keep the final counters, but rate-limit individual failure logs to a small fixed number.

Do not change scan semantics merely to suppress warnings.

---

## 31. Handoff state after latest runtime

### Frozen proven facts

~~~text
LoadAccessor valid                                      proven
Capture 22 production load behavior                     proven
TargetState vtable RVA 0x7B1C148                       proven for exact RE4 image
live-anchor match/trust                                 proven
RUNTIME_FUNCTION-based bounded scan                     operational
TargetState vtable xref 0x47C3E0A                      proven exact reference
TargetState vtable xref 0x47D21AF                      proven exact reference
TargetStateProbe can arm                                proven
~~~

### Rejected / retired

~~~text
legacy generic create_target_state                     rejected
0x44C7A27                                              rejected factory
0x47212A6                                              polymorphic dispatch only
0x78F42D0 return                                       rejected usable TargetState
0x4597A0                                               retired Overlay writer
repeating old site-1 provider probe                    exhausted
~~~

### Current task

~~~text
classify 0x47C3E0A and 0x47D21AF containing functions
    -> identify actual vtable store/object flow
    -> choose strongest constructor/initializer candidate
    -> only then add one bounded dynamic probe
~~~

### Current production blocker

~~~text
actual TargetState creator/factory ABI unknown
    -> no distinct single-RTV handoff state
    -> OutputHandoff unavailable
    -> no XeSS Execute
~~~

Keep PR66 Draft and unmerged.


---

## 32. PR66 14:05 runtime — TargetState xrefs classified

Latest runtime evidence:

~~~text
file: re2_framework_log(20260928-050834).txt
commit header: 42c3b66fd5163c331ecb1247e42b25cfc13e9d40
branch: feature/re4-xess-load-state-accessor-diagnostic
build date/time: 2026-09-28 14:05
~~~

The current remote PR66 source subsequently advanced to:

~~~text
06a50b2572a19ba0f4ff977babd2a786273a8bfe
Log bounded TargetState vtable xref context
~~~

and now contains the bounded xref-context scanner represented by this capture.

### 32.1 RUNTIME_FUNCTION scanner acceptance is complete

The scanner still reports incomplete exhaustive coverage:

~~~text
functions=508868
scannedFunctions=507568
failedFunctions=1300
xrefCount=2
complete=false
~~~

but individual decoder failures are now rate-limited correctly:

~~~text
decodeFailureDetailsLogged=12
decodeFailureDetailsSuppressed=true
~~~

The two exact positive vtable xrefs remain valid.

The live anchor also remains correctly trusted independently of full scan completeness:

~~~text
liveVtable=0x7ff6396ec148
expectedVtable=0x7ff6396ec148
imageIdentityValid=true
match=true
discoveryComplete=false
trusted=true
~~~

### 32.2 Candidate 0 is destructor/deallocation-side evidence

Candidate 0:

~~~text
functionBeginRva = 0x47C3E00
functionEndRva   = 0x47C3E58
xrefRva          = 0x47C3E0A
~~~

Observed flow:

~~~asm
LEA  RAX, TargetState_vtable
MOV  EDI, EDX
MOV  [RCX], RAX
MOV  RBX, RCX
CALL re4+0x446EE30
TEST DIL, 1
...
MOV  RCX, RBX
TEST DIL, 4
...
CALL re4+0x3ACC670
MOV  RAX, RBX
~~~

This is strongly consistent with a deleting-destructor/deallocation thunk:

~~~text
existing object arrives in RCX
EDX is preserved as destruction flags
TargetState vtable is restored onto the existing object
destructor-side work is called
flags gate a deallocation-like call
the original object pointer is returned
~~~

Status:

~~~text
re4+0x47C3E00 -> retire as TargetState creator/factory lead
~~~

This RVA remains useful as destructor/ownership evidence only.

Do not dynamically probe it in the next capture.

### 32.3 Candidate 1 is the strongest TargetState creator/factory lead

Candidate 1:

~~~text
functionBeginRva = 0x47D2180
functionEndRva   = 0x47D21D2
xrefRva          = 0x47D21AF
~~~

Observed flow:

~~~asm
MOV  EDX, 0xA8
CALL re4+0x3AB27F0
MOV  RBX, RAX
TEST RAX, RAX
JZ   failure

MOV  RDX, RDI
MOV  RCX, RAX
CALL re4+0x446E790

LEA  RAX, TargetState_vtable
MOV  [RBX], RAX
MOV  RAX, RBX
RET
~~~

This is materially stronger than every previous TargetState candidate.

The function:

~~~text
requests a fixed 0xA8-byte allocation
retains the allocated object in RBX
passes the new object to an initializer/constructor-like call
installs the exact proven live TargetState vtable into [RBX]
returns the same object pointer in RAX
~~~

Current classification:

~~~text
re4+0x47D2180 -> allocation + initialization wrapper / TargetState creator candidate
~~~

This is not yet a production create_target_state resolver.

The following remain unproven:

~~~text
input ABI
meaning of the argument forwarded through RDI/RDX
allocator semantics
initial refcount / ownership transfer
whether created objects are the same TargetState type instance used by Overlay
whether descriptor/RTV fields are ready immediately after construction
safe lifetime contract for OutputHandoff
~~~

### 32.4 Old site-1 probe is now fully exhausted

The old isolated 0x47212A6 path ran again and reproduced the known re4+0x78F42D0 result:

~~~text
numRtv=1
rtv0=null
overlayVtableMatch=false
targetStateLike=false
~~~

No new information was produced.

Do not run the old provider-return probe in the next capture.

### 32.5 Next diagnostic — early 0x47D2180 creator correlation

PR66 work-order comment:

~~~text
5863904529
~~~

The next diagnostic must target only re4+0x47D2180.

Arm it at the early REFramework bootstrap point:

~~~text
REFramework-constructor-after-integrity
before-plugin-init=true
~~~

Do not wait for the first valid XeSS frame.

The TargetState used by Overlay may be constructed during renderer initialization.

The probe remains read-only and bounded.

Required real-call observations:

~~~text
caller RVA / callsite
effective input registers
allocation result
post-initializer object
post-vtable-store object
returned object
TargetState structural snapshot
RTV/texture identity where readable
~~~

The most important acceptance is raw pointer correlation:

~~~text
observed object created by re4+0x47D2180
        ==
later live Overlay main TargetState
~~~

If true, this directly proves that the candidate path creates the live engine TargetState instance class used by Overlay.

If false, preserve the result and compare structure/callers without weakening the criterion.

Also enumerate bounded direct CALL-rel32 references resolving exactly to re4+0x47D2180 so caller-side argument setup can be reconstructed.

### 32.6 Production state remains fail-closed

The latest capture still reaches:

~~~text
create_render_target_view  ✅
create_texture             ✅
create_target_state        ❌ unresolved
TargetState::clone         ❌
OutputHandoff              ❌
xessD3D12Execute           0 calls
~~~

The repeated xessDestroyContext=-8 remains the known downstream symptom before first Execute.

Do not wire re4+0x47D2180 into production until object identity, ABI, descriptor semantics, and ownership/refcount behavior are runtime-proven.

---

## 33. Handoff state after xref classification

### Proven / strongest evidence

~~~text
TargetState vtable RVA 0x7B1C148             proven exact-image anchor
0x47C3E0A vtable xref                        proven
0x47D21AF vtable xref                        proven
0x47C3E00 function                           destructor/deallocation-side
0x47D2180 function                           strongest creator/factory lead
0x47D2180 allocation size                    0xA8
0x47D2180 vtable write                       [RBX] = TargetState vtable
0x47D2180 return                             RAX = RBX
~~~

### Current task

~~~text
early read-only observe re4+0x47D2180
    -> identify real callers and input ABI
    -> snapshot constructed object
    -> correlate created object pointer with live Overlay TargetState
    -> prove ownership/refcount semantics
~~~

### Current production blocker

~~~text
actual production-safe TargetState creation ABI not yet proven
    -> distinct single-RTV handoff state unavailable
    -> OutputHandoff unavailable
    -> no XeSS Execute
~~~

Keep PR66 Draft and unmerged.


---

## 34. PR66 14:34 runtime — RE4 TargetState creator proven

Latest runtime evidence:

~~~text
file: re2_framework_log(20260928-054035).txt
stamped commit: f99742e77f8b3df4db81bec639110183b967296e
branch: feature/re4-xess-load-state-accessor-diagnostic
build date/time: 2026-09-28 14:34
~~~

The current remote PR66 implementation containing the bounded creator probe is:

~~~text
27fa92f75b1b4da985749bcd3a2b729b75cf0d9f
Add bounded RE4 TargetState creator probe
~~~

The log clearly contains that creator-probe behavior.

Treat the stamped runtime hash as local-build provenance rather than assuming it exactly identifies the pushed source tree.

### 34.1 0x47D2180 executes as a real engine TargetState creator

The early read-only probe successfully observed real engine calls to:

~~~text
re4+0x47D2180
~~~

The wrapper behavior is now runtime-proven:

~~~text
allocation size          0xA8
allocation result        non-null
post-initializer object  same allocation
post-vtable object       same allocation
returned object          same allocation
vtable                   exact TargetState vtable
initial refCount         1
descriptor               readable
RTV entries              valid
Texture backing          valid
~~~

Two single-RTV examples:

~~~text
id=1
    object=0x22feeb96700
    vtable=0x7ff6396ec148
    refCount=1
    numRtv=1
    rect=3840x2160
    rtv0 valid
    format=29
    dimension=4
    targetStateLike=true

id=2
    object=0x22feeb967c0
    vtable=0x7ff6396ec148
    refCount=1
    numRtv=1
    rect=1920x1088
    rtv0 valid
    format=29
    dimension=4
    targetStateLike=true
~~~

The same wrapper also creates valid multi-RTV states:

~~~text
numRtv=6
rect sequence includes:
    256x256
    128x128
    64x64
    32x32
    16x16
vtable=exact TargetState vtable
refCount=1
valid RTV / Texture objects
~~~

This upgrades the classification from:

~~~text
TargetState creator candidate
~~~

to:

~~~text
proven RE4 TargetState allocation/construction wrapper
~~~

for the exact validated RE4 image.

### 34.2 Existing SDK ABI matches the proven wrapper

The existing SDK abstraction in shared/sdk/Renderer.cpp is already:

~~~cpp
TargetState* (*)(void*, TargetState::Desc*)
~~~

and invokes the resolved function as:

~~~cpp
fn(nullptr, desc)
~~~

The exact RE4 body is compatible with this contract.

Proven local dataflow:

~~~asm
MOV RDI, RDX          ; preserve incoming Desc*
MOV ECX, 1
MOV EDX, 0xA8
CALL re4+0x3AB27F0   ; allocation

MOV RBX, RAX
TEST RAX, RAX
JZ failure

MOV RDX, RDI          ; original Desc*
MOV RCX, RAX          ; new TargetState
CALL re4+0x446E790   ; initializer

LEA RAX, TargetState_vtable
MOV [RBX], RAX
MOV RAX, RBX
RET
~~~

The incoming RCX is not consumed before being overwritten for the allocator call.

The incoming RDX is the descriptor pointer used by the initializer.

Therefore, for this exact image:

~~~text
existing SDK function-pointer type    compatible
existing fn(nullptr, desc) shape      compatible
0x47D2180                              production resolver candidate accepted
~~~

### 34.3 Previous live-pointer correlation requirement is superseded

The creator probe was configured with:

~~~text
observationLimit=16
frameLimit=1800
~~~

All 16 observations were consumed during early initialization.

It then disarmed:

~~~text
reason=observation budget reached
observations=16
frames=1
~~~

Only afterward did the first live Overlay state become available.

The later:

~~~text
matchedObservation=false
structuralMatches=0
~~~

therefore does not reject 0x47D2180.

More importantly, raw identity between an arbitrary earlier-created TargetState and the later live Overlay TargetState is not required to prove a generic TargetState creation wrapper.

The wrapper itself has already been observed creating fresh objects with:

~~~text
exact TargetState vtable
correct TargetState layout
refCount=1
valid RTVs
valid Texture backing
multiple descriptor shapes
~~~

Do not spend another capture merely increasing the creator observation budget to chase Overlay object identity.

### 34.4 Direct caller scan result is provenance-only

Static direct CALL-rel32 enumeration reports:

~~~text
targetRva=0x47D2180
callerCount=0
~~~

while runtime calls arrive with caller return RVAs including:

~~~text
0x4197775
0x4523F2E
~~~

This is consistent with indirect dispatch.

That caller-dispatch detail is not required before using the exact validated function RVA through the already-established SDK ABI.

Return to caller reconstruction only if the production resolver behaves unexpectedly.

### 34.5 Important RE4 RTV-array ownership observation

Every captured TargetState uses an RTV array pointer inside the allocated 0xA8 object.

Examples:

~~~text
id=1
    object = 0x22feeb96700
    rtvs   = 0x22feeb96768
    delta  = +0x68

id=2
    object = 0x22feeb967c0
    rtvs   = 0x22feeb96828
    delta  = +0x68

id=3
    object = 0x22fee9b6010
    rtvs   = 0x22fee9b6078
    delta  = +0x68
~~~

Thus the constructed RE4 object does not retain Desc::rtvs as its final array pointer.

It materializes/points to internal RTV storage at object+0x68.

This raises a likely later cleanup issue in TargetState::clone():

~~~text
temporary cloned_desc.rtvs array
    -> passed to creator
    -> creator builds internal object storage
    -> current clone success path does not release temporary array
~~~

Do not change this ownership behavior in the same commit as the resolver.

First prove the production resolver and OutputHandoff.

Then separately verify AddRef/copy semantics and safely release the temporary caller-owned array if warranted.

### 34.6 Next implementation — promote exact RE4 resolver

PR66 implementation work order:

~~~text
5864220846
~~~

Modify only the RE4 path of:

~~~text
shared/sdk/Renderer.cpp::create_target_state()
~~~

For the exact RE4 image, resolve:

~~~text
creator RVA             0x47D2180
function range          0x47D2180..0x47D21D2
TargetState vtable RVA  0x7B1C148
image size              0x0E405000
PE checksum             0x0DEE3479
~~~

Validate the already-proven instruction anchors and relative CALL targets.

On any mismatch:

~~~text
return nullptr
~~~

For non-RE4 games, preserve the existing generic resolver unchanged.

Do not introduce a dependency from shared/sdk/Renderer.cpp to mods/re4_xess.

### 34.7 Next runtime acceptance

The next capture should validate production flow:

~~~text
[Renderer][RE4] Found create_target_state via validated RVA 0x47D2180
TargetState::clone returns distinct state
new state has one valid RTV
OutputHandoff prepare succeeds
OutputHandoff install succeeds
worker submit succeeds
hk_xessD3D12Execute first call is reached
post-present retirement remains correct
~~~

If a later stage fails, stop at that newly exposed blocker.

Do not return to broad TargetState provenance diagnostics unless the validated resolver itself fails.

---

## 35. Handoff state after creator proof

### Proven

~~~text
TargetState vtable RVA 0x7B1C148                  proven exact-image anchor
0x47C3E00                                        destructor/deallocation-side
0x47D2180 allocation size                       0xA8
0x47D2180 descriptor input                      RDX
0x47D2180 initializer object                    RCX=new allocation
0x47D2180 final vtable                          exact TargetState vtable
0x47D2180 returned object                       allocation
0x47D2180 initial refCount                      1
0x47D2180 output resources                      valid
0x47D2180 generic TargetState creator wrapper   proven
existing SDK factory ABI                        compatible
~~~

### Current task

~~~text
wire exact-image RE4 create_target_state resolver
    -> keep fail-closed validation
    -> leave non-RE4 path unchanged
    -> validate TargetState::clone
    -> advance to OutputHandoff
~~~

### Deferred ownership follow-up

~~~text
RE4 constructed TargetState rtvs = object + 0x68
temporary clone-side Desc::rtvs ownership cleanup requires separate proof
~~~

### Current production blocker

~~~text
resolver not yet wired
    -> TargetState::clone still returns no distinct state
    -> OutputHandoff unavailable
    -> no XeSS Execute
~~~

Keep PR66 Draft and unmerged.


---

## 36. PR66 14:57 runtime — production XeSS/OptiScaler path reached

Latest paired runtime evidence:

~~~text
REFramework:
    file: re2_framework_log(20260928-055958).txt
    commit: 088fdbbedff9bf59bcf56618e725fefc1a07a5da
    branch: feature/re4-xess-load-state-accessor-diagnostic
    build date/time: 2026-09-28 14:57

OptiScaler:
    file: OptiScaler(1).log
    version: 10.0.0-dev
    commit: 44cfee4d
~~~

The current remote PR66 implementation subsequently advanced to:

~~~text
1d9bb7774d9c05c33bce45842d1e8cf225638c4d
~~~

and contains the exact-image RE4 TargetState resolver exercised by this capture.

### 36.1 Exact RE4 create_target_state resolver is runtime-proven

The runtime reports:

~~~text
Searching for create_target_state
[Renderer][RE4] Found create_target_state via validated RVA 0x47d2180
~~~

The first handoff immediately proceeds to:

~~~text
cloned display handoff TargetState
worker submit
bridge initialization
xessD3D12Execute result=SUCCESS
Overlay handoff install
post-Overlay handoff confirmation
downstream retirement marker
~~~

Concrete first accepted frame:

~~~text
frame=6423
input=1969x1107
output=2560x1440
resetHistory=true
beforeState=COMMON
afterState=0xC0
result=SUCCESS
~~~

The installed handoff is a display-resolution engine-visible TargetState and remains installed through the post-Overlay observation.

This closes the previous TargetState production blocker.

Current status:

~~~text
create_render_target_view                  proven
create_texture                             proven
create_target_state RVA 0x47D2180          proven production resolver
TargetState::clone                         proven
display-resolution cloned output           proven
worker submit                              proven
public xessD3D12Execute                    proven
Overlay install                            proven
post-Overlay survival                      proven
downstream post-Present marker             proven
~~~

### 36.2 Stock OptiScaler public-XeSS interception is runtime-proven

The paired OptiScaler log shows the REFramework producer entering the stock public XeSS interception path:

~~~text
hk_xessD3D12CreateContext
hk_xessGetOptimalInputResolution
    output=2560x1440
    optimal input=1969x1107
hk_xessD3D12Init
hk_xessSetVelocityScale
    x=984.5
    y=-553.5
hk_xessD3D12Execute
~~~

During evaluation, OptiScaler reports all required temporal inputs:

~~~text
Color exist
MotionVectors exist
Output exist
Depth exist
Executing!!
~~~

Therefore the intended high-level bridge contract is now demonstrated in a real run:

~~~text
RE4
    -> custom REFramework public XeSS producer
    -> stock OptiScaler libxess interception
    -> OptiScaler upscaler feature evaluation
    -> engine-visible display-resolution TargetState
    -> native Overlay path
~~~

No private REF <-> OptiScaler ABI was required.

### 36.3 New timing issue — next pre-Overlay can precede the previous post-Present marker

The first newly exposed handoff issue is callback ordering, not TargetState creation.

Frame 6426:

~~~text
14:59:23.040 Execute frame=6426 result=SUCCESS
14:59:23.040 installed handoff
14:59:23.040 post-Overlay observation confirms handoff
14:59:23.049 restored handoff without its post-Present marker
14:59:23.049 Reset reason=output-handoff-unavailable
14:59:23.062 downstream retirement marker queued value=4
14:59:23.062 delayed-marker settlement queued
14:59:23.071 Reset reason=producer-context-unavailable
~~~

Frame 6429 reproduces the same ordering.

Interpretation:

~~~text
next pre-Overlay callback
    can arrive before
post-Present callback for the previous installed handoff
~~~

The current OutputHandoff safety behavior itself is correct:

~~~text
restore original Overlay state
do not reuse the handoff while marker is pending
wait for the real post-Present path to signal the downstream fence
~~~

The problem is severity propagation.

The same-generation MissingMarker state currently becomes owner/context unavailability and forces producer reconfiguration even though the marker arrives only milliseconds later.

### 36.4 MissingMarker must become a transient skipped-frame state

PR66 follow-up work order:

~~~text
5864421040
~~~

Required behavior:

~~~text
same generation + post-Present marker pending
    -> restore original Overlay state
    -> no new XeSS submit/install
    -> no CPU/GPU wait
    -> preserve worker/XeSS context
    -> preserve control generation
    -> invalidate temporal history only
    -> next accepted frame uses resetHistory=true
~~~

Do not signal the retirement marker early.

The marker must remain on the real post-Present path.

Do not weaken the current rule that prevents reuse while the previous downstream consumer lifetime is unproven.

Only change the caller's interpretation of:

~~~text
RetirementStatus::MissingMarker
~~~

from producer-unavailable/fault severity to transient frame skip.

### 36.5 Sustained execution exposes a new public-XeSS exception

After the path runs successfully for many frames, the worker reports:

~~~text
14:59:24.915
[RE4XeSS][Worker] RE4XeSS worker terminated an operation after an unhandled exception
[RE4XeSS][Failure] output handoff quarantined: Unknown exception in the RE4XeSS worker
[RE4XeSS][Reset] reason=xess-execute-fault
~~~

The paired OptiScaler log immediately before that point reports:

~~~text
14:59:24.914708 hk_xessD3D12Execute
14:59:24.914778 XeSSFeatureDx12::EvaluateInternal
14:59:24.914784 Input Resolution: 1969x1107
14:59:24.914788 Color exist
14:59:24.914794 MotionVectors exist
14:59:24.914799 Output exist
14:59:24.914803 Depth exist
14:59:24.914824 Executing!!
~~~

The temporal correlation strongly narrows the failure to:

~~~text
public xessD3D12Execute interception/evaluation
or immediately beneath that call
~~~

It does not yet prove which OptiScaler/XeSS backend component throws.

No backend-specific root cause should be claimed from this capture alone.

### 36.6 Current exception handling is safe but too coarse

Current worker architecture catches the exception outside the complete request:

~~~text
process_request()
    -> process_submit()
        -> RE4XeSSD3D12::submit()
            -> record_and_submit()
                -> RE4XeSSRuntime::execute()
                    -> m_functions.d3d12_execute(...)
~~~

An unknown exception therefore reaches the outer worker catch.

The outer catch safely:

~~~text
marks terminal fault
quarantines bridge
quarantines runtime
quarantines output handoff
does not attempt unsafe recovery
~~~

Keep that fail-closed behavior.

The next diagnostic must move the observation boundary closer to the exact public API call.

Add bounded:

~~~text
api-enter
api-return
api-threw-std-exception
api-threw-unknown
~~~

metadata around:

~~~cpp
m_functions.d3d12_execute(...)
~~~

including:

~~~text
frame
slot
submission sequence
worker thread
control generation
device-reset generation
command list
Color
Depth
converted MV
Output
input/output extents
resetHistory
~~~

If the public call throws, do not continue recording/close/submit the current command list as if the API completed.

Keep the generation quarantined.

Do not add same-process automatic retry in this diagnostic step.

### 36.7 Retire completed TargetState discovery probes from the normal path

The exact production resolver no longer depends on:

~~~text
full RUNTIME_FUNCTION TargetState-vtable xref scan
early TargetStateCreatorProbe hooks
old provider-return probes
~~~

The latest startup still scans approximately:

~~~text
508868 RUNTIME_FUNCTION entries
~~~

and arms creator hooks despite the factory already being proven and productionized.

Remove these heavy diagnostics from the normal RE4 XeSS startup path or require an explicit diagnostic/debug opt-in.

Keep the exact-image resolver validation in shared/sdk/Renderer.cpp.

This is cleanup/isolation, not a change to the proven factory contract.

### 36.8 Clone temporary RTV ownership remains deferred

The successful handoff run demonstrates that the constructed cloned TargetState and its native resource survive repeated use.

Earlier evidence still shows:

~~~text
TargetState::Desc::rtvs == object + 0x68
~~~

for RE4 constructed states.

The temporary clone-side Desc::rtvs allocation may therefore be releasable after construction, but do not combine that ownership change with the current marker/exception stabilization.

Validate ownership separately after the live producer path is stable.

---

## 37. Handoff state after first end-to-end producer execution

### Proven end-to-end

~~~text
semantic Color / Depth / Velocity inputs             proven
public XeSS context/init                             proven
validated RE4 TargetState creator                    proven
display-resolution cloned TargetState                proven
worker-side XeSS command recording                   proven
stock OptiScaler hk_xessD3D12Execute                 proven
OptiScaler sees Color/MV/Depth/Output                 proven
XeSS Execute success                                 proven repeatedly
engine-visible Overlay install                       proven
post-Overlay handoff survival                        proven
downstream retirement marker                         proven
~~~

### Current stabilization tasks

~~~text
1. treat same-generation MissingMarker as transient skip
   without XeSS context/bridge reconfiguration

2. localize the synchronous exception at/under
   public xessD3D12Execute while preserving quarantine safety

3. retire completed TargetState discovery probes
   from normal startup
~~~

### Current production blocker

~~~text
TargetState creation is no longer the blocker.

Current blocker:
    sustained public XeSS execution eventually throws an unknown exception
    -> worker terminal quarantine
    -> producer stops
~~~

### Not yet ready

~~~text
long-duration SR stability                 not proven
mode-change stability after live Execute   not proven
XeFG validation                            not started for this production path
clone temporary-array ownership cleanup    deferred
~~~

Do not start XeFG validation until the SR/output-handoff path survives sustained execution without terminal quarantine.

Keep PR66 Draft and unmerged.


---

## 38. PR66 15:17 runtime — public XeSS exception boundary proven

Latest paired capture:

~~~text
REFramework:
    file: re2_framework_log(20260928-062039).txt
    stamped commit: 23550b91e80c4361e733788a8610ac74c4fbfa9a
    branch: feature/re4-xess-load-state-accessor-diagnostic
    build time: 2026-09-28 15:17

OptiScaler:
    file: OptiScaler(2).log
    version: 10.0.0-dev
    commit: 44cfee4d
~~~

Current remote PR66 head at analysis time:

~~~text
5cd612303ee9e9efe778f4cf47913fde66aa7ff9
Handle delayed XeSS retirement and execute faults
~~~

PR66 follow-up work order:

~~~text
5864651645
~~~

### 38.1 Previous delayed-marker context churn is no longer reproduced

The run repeatedly reaches:

~~~text
restored handoff without its post-Present marker
queued delayed-marker settlement
same control generation continues
next XeSS submit succeeds
resumed handoff after marker settlement
~~~

Example:

~~~text
frame 6040:
    marker missing at restore
    delayed marker value=25 settles

frame 6041:
    controlGeneration=1
    resetGeneration=0
    same output generation
    api-return result=0
    Execute result=SUCCESS
    handoff resumes
~~~

The capture contains no:

~~~text
output-handoff-unavailable
producer-context-unavailable
~~~

reset cascade.

This is the intended operational result.

The explicit WaitingForPostPresentMarker branch is still retained as the fail-safe for a longer scheduling delay.

In this capture, marker settlement occurred between restore and prepare, so that explicit wait branch was not itself exercised.

Do not make further lifecycle changes from this capture.

### 38.2 Execute-boundary diagnostics now prove the exception is inside the public call

The first producer submission:

~~~text
frame=6016
slot=0
submission=1
input=853x480
output=2560x1440
resetHistory=true
api-enter
api-return result=0
Execute result=SUCCESS
~~~

The run continues successfully through repeated submissions.

The terminal failure is:

~~~text
frame=6216
slot=0
submission=201
workerThread=18040
controlGeneration=1
resetGeneration=0
commandList=0x1b2109107b0
color=0x1b1724915a0
depth=0x1b172493250
convertedMV=0x1b21090fd10
originalMV=0x1b172496220
output=0x1b16bb21650
input=853x480
outputExtent=2560x1440
resetHistory=false
api-exception kind=unknown
~~~

Critically:

~~~text
api-enter exists
api-return does not exist
api-exception follows immediately
~~~

Therefore the exception escapes directly from:

~~~cpp
m_functions.d3d12_execute(m_context, command_list, &params)
~~~

and is not caused by REFramework work after the call.

The following stages are excluded for this failure:

~~~text
restore-velocity ResourceBarrier
output-finish ResourceBarrier
GraphicsCommandList::Close
ExecuteCommandLists
CommandQueue::Signal
~~~

### 38.3 Paired OptiScaler log narrows the failure to its native XeSS backend call

At the same failure timestamp, OptiScaler reports:

~~~text
15:20:06.266202 hk_xessD3D12Execute
15:20:06.266234 NVSDK_NGX_D3D12_EvaluateFeature
15:20:06.266272 XeSSFeatureDx12::EvaluateInternal
15:20:06.266278 Input Resolution: 853x480
15:20:06.266283 Color exist
15:20:06.266290 MotionVectors exist
15:20:06.266295 Output exist
15:20:06.266299 Depth exist
15:20:06.266304 AutoExposure enabled
15:20:06.266321 Executing!!
~~~

No normal OptiScaler XeSS error-result log follows.

At OptiScaler commit 44cfee4d, this log occurs immediately before:

~~~cpp
xessResult = XeSSProxy::D3D12Execute()(_xessContext, InCommandList, &params);
~~~

Therefore the current strongest boundary is:

~~~text
REFramework public xessD3D12Execute call
    -> OptiScaler hk_xessD3D12Execute
        -> NVSDK_NGX_D3D12_EvaluateFeature
            -> XeSSFeatureDx12::EvaluateInternal
                -> XeSSProxy::D3D12Execute
                    -> exception escapes before normal return
~~~

This still does not identify which binary owns the faulting instruction.

Possible owners remain:

~~~text
OptiScaler glue
native Intel XeSS
graphics driver
another dependency reached by native XeSS
~~~

Do not assign root cause until the exception address/module is captured.

### 38.4 Cross-run timing suggests a repeatable ~2-second / ~200-submit failure

Previous capture:

~~~text
mode: Ultra Quality
input: 1969x1107
first successful Execute: 14:59:22.858
unknown exception:        14:59:24.915
elapsed:                  ~2.06 s
~~~

Current capture:

~~~text
mode: Ultra Performance
input: 853x480
first Execute api-enter:  15:20:04.148
unknown exception:        15:20:06.266
elapsed:                  ~2.12 s
failing submission:       201
~~~

This is a strong reproducibility clue.

It lowers the likelihood that the failure is specific to one XeSS quality mode or one input resolution.

Do not add a frame-count or time-based workaround.

### 38.5 Next diagnostic target — native exception identity

The current kind=unknown classification is no longer sufficient.

The next PR66 diagnostic must capture, without swallowing or translating the exception:

~~~text
exception code
ExceptionAddress
ContextRecord RIP
faulting module
module RVA
ExceptionFlags
NumberParameters
~~~

For access violation:

~~~text
ExceptionInformation[0] = read/write/execute
ExceptionInformation[1] = target virtual address
~~~

Implementation boundary:

~~~text
m_functions.d3d12_execute(...)
~~~

Use a narrow Windows/MSVC-only observation filter.

Requirements:

~~~text
no process-wide VEH
no worker-wide _set_se_translator
no recovery
no retry
no logging/allocation from inside the filter
copy POD only
return EXCEPTION_CONTINUE_SEARCH
log captured information from the existing catch path
~~~

The existing outer worker quarantine remains the final safety net.

### 38.6 Remaining CreateRenderTargetViewProbe should leave normal production execution

The TargetState/xref/creator diagnostics are absent from this capture and are no longer normal-path dependencies.

However, CreateRenderTargetViewProbe still arms and produces captures.

Its production questions are already closed:

~~~text
RE4 RTV factory             proven
R11G11B10_FLOAT RTV         proven
TargetState clone           proven
display handoff output      proven
repeated live use           proven
~~~

The normal pre-Overlay call to:

~~~cpp
CreateRenderTargetViewProbe::instance().ensure(...)
~~~

is diagnostic-only and its result is ignored.

Remove it from the normal path or require an explicit diagnostic opt-in.

Do not modify the actual create_render_target_view resolver.

### 38.7 Current safety behavior remains correct

If the public call throws:

~~~text
do not continue command recording
do not close the interrupted list
do not submit it
do not signal its fence
quarantine bridge
quarantine XeSS runtime
quarantine output handoff
do not wait
do not retry same context
~~~

Current PR66 behavior already follows this policy.

---

## 39. Handoff state after exact execute-boundary localization

### Proven

~~~text
TargetState factory/resolver                         proven
TargetState clone / OutputHandoff                    proven
stock OptiScaler public-XeSS interception            proven
repeated public XeSS success                         proven
same-generation delayed marker recovery              operationally proven
exception occurs inside public xessD3D12Execute      proven
OptiScaler reaches native XeSS backend call          proven
post-call REFramework D3D12 stages not at fault      proven for terminal exception
~~~

### Current single diagnostic objective

~~~text
capture native exception code + faulting address/module/RVA
~~~

### Current production blocker

~~~text
sustained public XeSS execution
    -> approximately 2.1 seconds / approximately 200 submits
    -> exception escapes native backend path
    -> terminal quarantine
~~~

### Deferred

~~~text
automatic recovery after exception       deferred
clone temporary RTV-array cleanup        deferred
XeFG production validation               deferred
~~~

Do not begin XeFG validation until sustained SR survives beyond the reproducible failure window.

Keep PR66 Draft and unmerged.


---

## 40. PR66 native exception identity — 2026-09-28 16:10 paired capture

This section **supersedes the faulting-module-unknown checkpoint in Sections 38–39**.
The earlier observations remain valid historical evidence, but the 16:10 capture
establishes the faulting instruction's module and the native exception identity.

Paired logs:

~~~text
REFramework:
    re2_framework_log(20260928-071122).txt
    reported source stamp: fa05355903a1b9e9e16b5e45c5c28feb0512a96b
    branch: feature/re4-xess-load-state-accessor-diagnostic
    build time: 2026-09-28 15:37

OptiScaler:
    OptiScaler(3).log
    v10.0.0-dev, commit 44cfee4d
    binary build: 20260926_090919
~~~

PR66 HEAD at review:

~~~text
75c93f2374254e7b91d59df73a78d6ad41ded9d1
Capture native XeSS execute exceptions
~~~

The runtime has the narrow native SEH diagnostics introduced in that HEAD, while
the file's reported source stamp predates it. Record the discrepancy; do not
treat the logged stamp alone as proof of the executable's precise source commit.

PR66 next-step work order:

~~~text
5865230623
~~~

### 40.1 Fault is an access violation at a game-directory OptiScaler-proxy instruction

Exact REFramework failure:

~~~text
16:10:50.150
[RE4XeSS][Execute] native-exception
frame=8343
workerThread=12256
controlGeneration=1
resetGeneration=0
slot=7
submission=200
commandList=0x1e3b9503840
color=0x1e2ef06c8a0
depth=0x1e2ef06a260
convertedMV=0x1e3914110f0
originalMV=0x1e2ef071520
output=0x1e2d95a2b90
input=853x480
outputExtent=2560x1440
resetHistory=false
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

Immediate aftermath:

~~~text
api-exception kind=unknown
worker unhandled exception
output handoff quarantined
reason=xess-execute-fault
reason=producer-faulted
reason=worker-control-fault
~~~

This is a Windows access violation attempting to write address 0xC, i.e.
a near-null write.

The reported module is the **game-directory** dxgi.dll, not
C:\Windows\System32\dxgi.dll and not libxess.dll.

The paired OptiScaler log independently records:

~~~text
CheckWorkingMode OptiScaler working as dxgi.dll, system dll loaded
~~~

Thus the faulting **instruction** is in the OptiScaler DXGI proxy image.

This does **not** yet prove where the bad pointer originated or which source
component owns the underlying defect. The instruction may run in response to
an invalid argument, a stale object, or an internally generated invalid state.

The next action must map the *exact locally used* dxgi.dll image at RVA 0x21256f
to its containing function, instruction operand, and callers.

### 40.2 Paired OptiScaler trace stops at the intercepted XeSS evaluation boundary

Last visible paired OptiScaler trace:

~~~text
16:10:50.141073 MenuHdrCheck Output HDR: false, UI Mode: LinearHDR
16:10:50.141073+ hk_xessD3D12Execute
16:10:50.141115 NVSDK_NGX_D3D12_EvaluateFeature
16:10:50.141162 XeSSFeatureDx12::EvaluateInternal
16:10:50.141171 Input Resolution: 853x480
16:10:50.141176 Color exist
16:10:50.141185 MotionVectors exist
16:10:50.141191 Output exist
16:10:50.141198 Depth exist
16:10:50.141204 AutoExposure enabled
16:10:50.141225 Executing!!
16:10:50.150    REFramework native exception, dxgi.dll+0x21256f
~~~

At OptiScaler commit 44cfee4d, the Executing!! log immediately precedes:

~~~cpp
xessResult = XeSSProxy::D3D12Execute()(_xessContext, InCommandList, &params);
~~~

However, the last printed stage is **not** a faulting-instruction stack trace.
Do not conclude from it that Intel's native XeSS DLL contains the fault.

The faulting RIP is in the game-directory OptiScaler dxgi.dll. A debugger or
exact-binary symbol/disassembly mapping is required to learn how control
returned/re-entered that module.

An OptiInput/Menu reentry also appears at 16:10:50.128, immediately preceding
the last visible XeSS evaluation, but temporal proximity alone does not
establish causality; do not modify overlay or input hooks from this observation.

### 40.3 Failure timing recurs across three captures, but is not a fixed-count proof

~~~text
Capture 1 / 14:59 / Ultra Quality / 1969x1107:
    first successful Execute: 14:59:22.858
    terminal exception:        14:59:24.915
    elapsed:                   ~2.06 s
    submission:                not instrumented

Capture 2 / 15:20 / Ultra Performance / 853x480:
    first api-enter:           15:20:04.148
    terminal exception:        15:20:06.266
    elapsed:                   ~2.12 s
    failing submission:        201

Capture 3 / 16:10 / Ultra Performance / 853x480:
    first api-enter:           16:10:48.041
    terminal exception:        16:10:50.150
    elapsed:                   ~2.11 s
    failing submission:        200
~~~

This is a strong timing/submission reproducibility clue, not proof of
a literal 200-frame trigger or of a specific backend implementation bug.

Do not introduce frame-count workarounds or change XeSS temporal logic based on
the timing alone.

### 40.4 Cleanup and lifecycle checks in the new run

The commit associated with the runtime removes the normal-path call to:

~~~cpp
CreateRenderTargetViewProbe::instance().ensure(...)
~~~

The REFramework log contains no:

~~~text
[RE4XeSS][RTVProbe]
[RE4XeSS][TargetStateCreatorProbe]
[RE4XeSS][TargetStateVtableProbe]
~~~

This meets the immediate goal of removing historical diagnostic hook activity
from the normal producer execution path; it is not a standalone performance
claim.

Delayed post-Present markers are still recorded, including:

~~~text
frame 8143 -> delayed settlement value 1
frame 8216 -> delayed settlement value 73
frame 8290 -> delayed settlement value 147
~~~

The capture has no observed output-handoff-unavailable or
producer-context-unavailable cascade before the terminal native exception.

The producer continues on the same control/device-reset generation until
the exception.

The fault then correctly triggers terminal quarantine. Subsequent quality
changes do not silently recreate the uncertain generation.

### 40.5 Required next investigation: exact-image symbolization / original exception context

Investigate the exact file used during capture:

~~~text
E:\SteamLibrary\steamapps\common\RESIDENT EVIL 4  BIOHAZARD RE4\dxgi.dll
OptiScaler 44cfee4d / build 20260926_090919
RVA 0x21256f
~~~

Resolve with the matching PDB/build map, or disassemble that exact binary:

~~~text
containing function/source line if symbols available
faulting instruction and operand
register/pointer producing the write to 0xC
call-site chain, including whether control passed through a XeSS callback
object lifetime at the failure
~~~

A debugger first-chance exception context/stack before C++ unwinding is the
preferred additional artifact.

If symbols and debugger are unavailable, an optional tightly bounded change
may copy the key integer registers from ContextRecord to an existing POD
observation inside the *already installed narrow SEH filter*; log only after
entering the outer C++ catch. No broad exception handler is justified.

Do not use an RVA from another OptiScaler build to label this one.

Do not make speculative changes to:

~~~text
Color/Depth/Velocity resource barriers
TargetState construction
OutputHandoff retirement
XeSS worker thread ownership
OptiScaler selection/routing
XeFG or D3D12Hook lifecycle
~~~

Do not add a private REF–OptiScaler interface.

The faulting *module* is now attributed to the OptiScaler proxy, but
the *root cause and correct ownership of a source change* remain unproven.
If symbolization points to an OptiScaler function, investigate the smallest
appropriate upstream/fork fix separately, retaining unmodified stock
OptiScaler as the production integration target.

---

## 41. Current RE4 XeSS production handoff after first native AV attribution

~~~text
TargetState factory / cloning            PROVEN
public XeSS / stock OptiScaler routing   PROVEN
engine-visible OutputHandoff             PROVEN
delayed-marker same-generation recovery  OBSERVED
historical RTV diagnostic removal        VERIFIED IN LOG
native exception identity                0xC0000005 write to 0xC
faulting instruction's module            game-directory OptiScaler dxgi.dll
faulting instruction RVA                 0x21256f
faulting source function / pointer origin UNKNOWN
sustained XeSS SR stability              BLOCKED
XeFG validation                          DEFERRED
~~~

Next mandatory acceptance:

~~~text
map exact dxgi.dll+0x21256f instruction/callers
establish bad-pointer provenance before any behavioral fix
retain terminal fail-closed quarantine
validate a justified change with >300 successful XeSS submissions
~~~

Keep PR66 Draft, Open, and unmerged.


---

## 42. PR66 quality-transition OutputHandoff checkpoint — 2026-09-28 16:24

This supersedes the immediate next-task ordering in Section 41, **not** its historical OptiScaler native AV evidence.

Paired capture: REFramework re2_framework_log(20260928-072521).txt (reported build 15:37 / stamped fa05355903a1b9e9e16b5e45c5c28feb0512a96b) and OptiScaler(4).log (44cfee4d). Current PR66 code at review: 425bcb7419619674a8d2fac405d155d1d89c1728. The logged source stamp does not reliably identify all local changes.

Evidence:

~~~text
16:24:53.544  last successful public XeSS API return, frame 7374, submission 118
16:24:53.552  requested mode Quality -> Ultra Quality, control generation change
16:24:53.599  OutputHandoff restore quarantines: unexpected Overlay TargetState
16:24:53.632  mode-change reset follows
~~~

118 api-enter and 118 api-return result=0; no native-exception or api-exception. The same OutputHandoff quarantine line then repeats 2,192 times until 16:25:07.881. OptiScaler stops receiving new XeSS Execute requests at the last accepted frame. Earlier delayed post-Present markers still settle.

Source predicate: RE4XeSSOutputHandoff::restore() sees the same Overlay layer but a main TargetState that is neither the installed handoff nor its saved original. The actual third pointer and owner were not logged. request_mode() changes mode/control generation but does not itself assign Overlay main state. Restore precedes retirement/control dispatch, so this failure prevents ordinary mode-change service.

PR66 work-order comment **5865417657**: add first-failure pointer/Overlay identity, saved/handoff/template state, installed frame, control/device generation and marker-retirement state; bracket the mode request and next pre-Overlay restore with bounded telemetry; suppress repeat logging while keeping quarantine intact. Do not replace the unknown third state, clear installed bookkeeping, release retained output, or synthesize downstream lifetime completion.

This is a separate **quality-change OutputHandoff ownership blocker**, observed at 118 successful submits before the independently unresolved approximately 200-submission OptiScaler dxgi.dll+0x21256f native write AV.

### Current handoff

~~~text
TargetState creator/clone and public XeSS interception     proven
Quality -> Ultra Quality transition                        blocked on third TargetState
Mode-change failure spam                                   2,192 repeats
Third pointer/write ownership                              unknown
OptiScaler native AV                                       not encountered in this run; still unresolved
XeFG validation                                            deferred
~~~

Next: classify the third pointer from one bounded capture while preserving fail-closed ownership. After quality-transition stability, separately retest sustained >300-submission SR and investigate the exact OptiScaler binary AV. Keep PR66 Draft/Open/unmerged.


---

## 43. 16:49 capture — repeated OptiScaler dxgi native AV with first-chance GPRs

This checkpoint follows Section 42. The previous 16:24 quality-transition identity
failure and this native OptiScaler-proxy AV are independent unresolved tracks.
The new capture reaches the native AV *before* attempting a live in-flight
quality-mode change, so it cannot validate the Section 42 fix.

### 43.1 Exact paired inputs

~~~text
REFramework: re2_framework_log(20260928-075010).txt
  log stamp:  a9bd0b59cefde15317a774602b14a67548c2145e
  branch:     feature/re4-xess-load-state-accessor-diagnostic
  build time: 2026-09-28 11:26
OptiScaler: OptiScaler(5).log
  v10.0.0-dev 44cfee4d, built 20260926_090919
  proxy mode: game-directory dxgi.dll
PR66 remote HEAD when reviewed:
  0be6550ddc865dafc09fa3bbb0295a5c530203ed
PR66 follow-up comment:
  5865784530
~~~

Do not equate the logged local hash stamp to the current remote PR66 tree.

### 43.2 Native fault at submission 205, same module/RVA as earlier

~~~text
first public Execute call     16:49:29.124  frame=7300  submission=1
last reported native fault    16:49:31.223  frame=7505  submission=205
workerThread                  16860
controlGeneration             1
resetGeneration               0
slot                          4
input                         1969x1107
output                        2560x1440
resetHistory                  false

ExceptionCode                 0xC0000005
ExceptionAddress / RIP        0x7ffda6a4256f
module                        <game directory>\dxgi.dll (OptiScaler proxy)
moduleRVA                     0x21256f
accessKind                    write
targetAddress                 0xc
GPRValid                      true

RAX                           0x0
RCX                           0x5cf6782388b50000
RDX                           0x1f6f8945890
R8                            0x0
R9                            0x35094ce0
R10                           0xec359125e7c0067
R11                           0x6919c17fb0
RSP                           0x6919c18200
RBP                           0x6919c18300
~~~

The exact OptiScaler proxy module RVA, exception code and near-null
write target match the prior Section 40 native exception attribution.
This is a reproducible instruction-location signature.

RAX=0 is *additional context*, not proof of the instruction operand
or object provenance. Only disassembly of the exact captured dxgi.dll
can establish whether the failing store is [rax+0xc], another register,
or an indirect calculation.

First Execute to fault is approximately 2.10 seconds. Similar timing
across captures is not proof of a 200-submit gate, timer or periodic
cleanup path.

### 43.3 The 128-line normal Execute budget is not the submit count

~~~text
REFramework logged api-enter                        128
REFramework logged api-return result=0              128
REFramework last normal verbose submission          128
REFramework terminal exceptional submission         205
OptiScaler hk_xessD3D12Execute                      205
OptiScaler XeSSFeatureDx12::EvaluateInternal
  Executing!!                                       202
~~~

The source has MAX_EXECUTE_API_LOGS=128; it intentionally omits later
ordinary api-enter/api-return entries. The terminal submission=205
is therefore consistent with the OptiScaler 205 intercepts. Do not
misread the absent 129–204 verbose entries as failed calls, and do
not claim 205 confirmed successful backend executions.

OptiScaler last Execute/Executing trace is at 16:49:31.221, directly
before REFramework records the native exception at 16:49:31.223.
No successful public XeSS result is logged for submission 205.

### 43.4 Other working paths and fail-closed behavior

The observed early installed Overlay identities agree:

~~~text
Overlay layer             0x1f4e5250800
installed handoff state   0x1f4e4b453a0
saved original state      0x1f4ccc98210
~~~

Delayed downstream-marker settlements occurred for restored frames
7303, 7381, and 7460 (values 4, 81 and 160, respectively).

This capture shows no Section 42 unexpected third-TargetState error,
but **does not exercise healthy-generation Quality -> Ultra Quality**:
it begins Off -> Ultra Quality and later changes quality only *after*
the native worker exception.

Following the AV:

~~~text
16:49:31.223 api-exception kind=unknown
16:49:31.224 worker exception termination
16:49:31.224 output handoff quarantined once
16:49:31.233 producer-faulted reset
16:49:33.876 Ultra Quality -> Balanced, controlGeneration=2
16:49:35.643 Balanced -> Ultra Quality Plus, controlGeneration=3
16:49:36.960 Ultra Quality Plus -> Performance, controlGeneration=4
~~~

Subsequent marker snapshots keep the output generation retained with
retirementRequested=true and bridgeWriterUncertain=true; the worker
is not automatically restarted. Avoid claiming the old output has
retired based solely on markerPending=false or installed=false.

### 43.5 Required next proof and minimal engineering gate

PR66 comment 5865784530 requires:

1. Identify/hash the **actual game-directory** OptiScaler dxgi.dll
   for the 44cfee4d / 20260926_090919 build.
2. Disassemble the exact faulting instruction around RVA 0x21256f,
   with matching symbols or build map when available.
3. Capture a first-chance stack if available, and trace the producing
   pointer through OptiScaler, XeSS->NGX translation and incoming
   REF resource/callback ownership. Do not assign fault causality
   from module identity alone.
4. Only then apply the smallest justified patch in the actual
   owner, if any. The supported contract remains stock OptiScaler.
5. Separately verify a live healthy-generation Quality ->
   Ultra Quality mode change for the Section 42 third-state case,
   then sustained >300-submission XeSS SR for this native AV.

The OptiScaler log also records MenuCommon::Init re-entry around
16:49:31.204, before the failure; this is **temporal correlation
only**, not proven causation.

Preserve the current narrow EXCEPTION_CONTINUE_SEARCH observer,
quarantined GPU-resource lifetimes, public XeSS frontend, no
direct swapchain output, and unchanged XeFG/D3D12Hook integration.
Do not add unproven GPU barriers or retries.

---

## 44. Handoff after the 16:49 capture

~~~text
Engine TargetState creator and normal Overlay handoff    PROVEN
Public XeSS -> stock OptiScaler                          PROVEN through prior returns
Last logged normal public Execute                        128
Last known attempted public Execute                      205 (native AV)
GPRs of exact faulting context                           CAPTURED
Exact OptiScaler game-directory fault RVA                0x21256f
Underlying pointer producer / faulting instruction       NOT YET IDENTIFIED
Section 42 healthy mode-change identity regression       NOT TESTED HERE
Sustained XeSS SR beyond ~200 submissions                BLOCKED
XeFG production validation                               DEFERRED
~~~

PR66 remains Draft / Open / unmerged.


---

## 45. Exact OptiScaler DXGI binary disassembly — internal global slot confirmed

**Supersedes only the “faulting instruction / immediate pointer source UNKNOWN” wording in Sections 43–44.** The earlier 16:49 capture, pending OutputHandoff quality-switch investigation, and sustained-SR blocker remain valid.

Evidence was posted to [PR66 issue comment 5865873929](https://github.com/onehoon/REFramework/pull/66#issuecomment-5865873929) following exact-binary disassembly of the game-directory OptiScaler proxy from the 16:49 paired capture.

### 45.1 Pinned binary identity

~~~text
File: <RE4 game directory>\dxgi.dll
SHA-256:
  60E6FB52F924C1C47ED7ECE4B19B747112FA09C2492F323E558E30957196C2B8

OptiScaler log:
  v10.0.0-dev
  commit 44cfee4d
  build 20260926_090919

PE32+ x64
Preferred image base: 0x180000000
SizeOfImage:          0x18AB000
File size:            25,743,872 bytes
Debug-directory timestamp: 2026-09-26 09:12:09Z
~~~

The local analysis did not find a matching PDB alongside the DLL. The local OptiScaler checkout also did not contain the log-reported 44cfee4d revision. **The upstream GitHub commit 44cfee4d436857742a9bf71bbe81396ec9989715 exists**, but its source alone does not prove a newly built PDB matches the captured optimized DLL. Match the PDB GUID/Age (or reproduce the crash using the *new* PDB-matched DLL) before assigning source lines to the original RVA.

### 45.2 Faulting instruction and pointer provenance: proven

Windows exception from the 16:49 capture:

~~~text
exception code: 0xC0000005
faulting module RVA: 0x21256F
first-chance RAX: 0x0
access: WRITE
effective address: 0xC
~~~

The exact file's .pdata places RVA 0x21256F within:

~~~text
RUNTIME_FUNCTION: [0x2124C0, 0x212949)
~~~

Disassembly of the **captured file**, not an assumed rebuilt binary:

~~~asm
; RVA 0x212568, bytes 48 8B 05 49 FC 63 01:
mov rax, qword ptr [rip+0x163fc49] ; module global at dxgi.dll+0x18521B8

; RVA 0x21256F, bytes 83 48 0C 10:
or dword ptr [rax+0xC], 0x10
~~~

With RAX=0, the faulting operand resolves to WRITE 0xC exactly. This eliminates uncertainty over the **faulting instruction and immediate pointer source**.

The pointer was loaded independently from OptiScaler's module-global slot at RVA 0x18521B8, **not directly from incoming RCX/RDX/R8**. However, this observation does **not** establish what owns or initializes that slot, or whether its source state originally came from a REF-provided object/callback.

### 45.3 Static caller and slot writer evidence

~~~text
immediate direct caller:
  CALL at dxgi.dll+0x1D1945 -> dxgi.dll+0x2124C0

caller RCX source:
  separate module-global slot dxgi.dll+0x1845FA8

faulting target pointer source:
  module-global slot dxgi.dll+0x18521B8

observed static stores to the target global:
  dxgi.dll+0x20CF06
  dxgi.dll+0x20CF17
  dxgi.dll+0x20D6C1
~~~

These are concrete static program relationships. They do not explain why the target slot is null at submission 205; no valid C++ symbol/ownership label has yet been assigned.

Recommended next evidence:

1. Match the exact captured binary to its original PDB/build map; if absent, fetch upstream source at commit 44cfee4d, build ReleaseDebug and reproduce with the **new matched binary+PDB** (do not symbolicate the old RVA using the new PDB without a match).
2. In a first-chance debugger session, inspect the target global at +0x18521B8, its stores and the caller at +0x1D1945, and capture the pre-unwind stack and relevant object lifetime.
3. Determine whether the slot was never initialized, cleared by a teardown/transition, or points to an external object that was invalidated. Do not label any of these alternatives as established yet.
4. Make only the smallest owner-correct source fix once a responsible source symbol/lifecycle path is proven. Keep stock-OptiScaler public XeSS compatibility as the target.

### 45.4 Scope of next REFramework work

The PR66 code should **not** add a null check, bypass, recovery retry, fabricated XeSS success, D3D12 resource-state change, or output-lifetime relaxation merely to mask this exact OptiScaler-internal dereference.

No REFramework source change, test DLL, code commit, or push was made by the exact-binary analysis step. PR66 remained Draft/Open; head at that checkpoint was 2ae09f78c70a18719d854de2e0ea61fe1c6f09c3.

Maintain these as **two independent pending gates**:

~~~text
A. sustained public XeSS SR:
   OptiScaler dxgi.dll+0x21256F
   0xC0000005 WRITE 0xC
   internal global +0x18521B8 is null
   owner/source lifecycle still UNKNOWN

B. live healthy-generation Quality -> Ultra Quality:
   same-Overlay unexpected third TargetState
   origin and safe retirement still UNKNOWN

XeFG production validation: DEFERRED
~~~


---

## 46. PR66 paired 00/01 transition instrumentation — 2026-09-28

Follow-up source: [PR66 comment 5866997374](https://github.com/onehoon/REFramework/pull/66#issuecomment-5866997374), paired RE4 captures `ETS2ATS/RE4/ref opti/00` and `01`. Logged build stamps (`REF a9bd0b59`, OptiScaler fork `d7f64081`, `20260928_175724`) are runtime metadata, not verified binary hashes.

`00` remained stable in Balanced: the capture reports 2,656 OptiScaler evaluations and 128 initial successful REF public XeSS Execute returns, with no OutputHandoff mismatch/quarantine/native Execute AV. In `01`, Balanced -> Ultra Quality Plus was requested at 18:03:21.206; the next pre-Overlay restore at 18:03:21.257 observed an unexpected third TargetState on the same Overlay and latched hard quarantine. The last confirmed post-Overlay handoff was frame 8795; the reported cached retirement completion was zero while the last signal was 750. These observations do not identify who wrote the third pointer.

Static audit at PR66 source head `239565e91eb9cd217a290606eede202870356e1b` found the Overlay getter returns a reference to its intrusive TargetState slot (`shared/sdk/Renderer.hpp:468-470`). The two direct REF writes in RE4 XeSS are limited to `RE4XeSSOutputHandoff::restore()` and `install()`; remaining getter uses in the feature path inspect/observe the state. `request_mode()` does not write it. This does not establish whether RE Engine or another module performs the third write. The new bounded transition event includes the exact slot address/offset; a debugger data-write breakpoint and call stack are still required if callback-boundary events do not expose the writer.

PR66 now adds only bounded evidence collection: monotonic transition event sequence/timestamp and mode/device/frame/thread/identity/ownership flags; first-mismatch cached versus actual nonblocking fence completion; and cumulative XeSS success checkpoints at 256, 512, 1024, 2048 and subsequent powers of two, preserving the detailed first 128 calls and bounded per-generation transition detail. It never waits on the GPU or changes handoff ownership, quarantine, markers, or resource pins.

~~~text
Balanced steady-state (>300 REF XeSS success returns)       NOT YET VALIDATED
Balanced -> Ultra Quality Plus writer attribution           UNKNOWN
TargetState owner-correct fix                                NOT APPLIED (evidence insufficient)
Quarantine safety                                            PRESERVED
Off/On and Load Save after transition                        NOT YET VALIDATED
PR66                                                        Draft / Open / unmerged
~~~

The local test bundle `artifacts/pr66-output-transition-239565e9/` was built RelWithDebInfo/x64 from base HEAD `239565e91eb9cd217a290606eede202870356e1b` plus uncommitted instrumentation:

~~~text
dinput8.dll SHA-256  489D8C82F112150919CC4C37BA09B35C09A742F1B40E0BAE27D26A547716C959
dinput8.pdb SHA-256  F4B6F0EAA1B6D340B6D20107522905D7FD1598767CB81A28AD5C079CDC525FBA
PDB GUID / Age      {8FC16AE4-E113-459E-A432-DB6E89E9B617} / 2
Symbol check        PASS (private symbols, line records, globals, type info)
~~~

The REF embedded stamp identifies only its base commit; use the DLL hash and matching PDB identity as the exact build identity. Next evidence must capture the first TargetState write if callback provenance remains ambiguous. Do not infer success from OptiScaler `Upscaling done` counts, write back/adopt the third object, clear quarantine, synthesize a post-Present marker, or release retained resources early. Keep the OptiScaler native AV investigation independent. The >300-success run, mode transition, Off/On and Load Save sequences are still unvalidated.

## 47. PR66 Phase A lifetime trace — source status, runtime gate pending

PR66 follow-up work order: [comment 5884381727](https://github.com/onehoon/REFramework/pull/66#issuecomment-5884381727). This change implements bounded, read-only Phase A tracing only. It is enabled only for RE4 + D3D12 while REFramework Debug Log is enabled. The trace keeps a fixed 4,096-event ring, emits at most 32 compact dumps, checkpoints every 512 Present callbacks or 1,024 successful submits, and includes anomaly-triggered skip dumps.

Captured identities originate at their owning boundaries and are carried forward: Scene/pre-Overlay/post-Overlay ordinals and overlap epoch; trace, submit, bridge-slot, writer-fence, output-generation and monotonic install IDs; Overlay/TargetState/output identities; Present/Present1 ordinal, swapchain/source, caller/return TIDs, active queue identity/type, original-call timing/result and callback suppression; and downstream marker Signal value plus cached and actual nonblocking fence values. `UINT64_MAX` is recorded as invalid completion/device-removal evidence, never as completion. Successful XeSS submit accounting separates API return, command-list submission, Signal success, and writer-fence completion. `PostOverlayObservation`, Present callback adjacency, timestamps, and a successful Signal explicitly remain **not GPU-reader proof**.

### Gate A status

~~~text
Source instrumentation/build: implemented; x64 Release build succeeded from base `3291ca246b8eb96f7475183293daea64479269b5` plus this Phase-A source diff; PE machine `0x8664`; `dinput8.dll` SHA-256 `AF5F6977CBEE2B19E22CC7ABB5351C62DAECDAA112149745FA364B61F8F8622C`.
CTest: no tests are registered in this build tree.
Matching PDB: not produced by this Release build; the adjacent PDB is older and must not be used to symbolize this DLL.
Runtime capture with the new trace: pending user RE4 + OptiScaler test.
Actual RE4 downstream consumer command submissions identified: NOT YET PROVEN.
Installed output -> consumer submissions -> eligible Present/queue -> its marker: NOT YET PROVEN.
OutputHandoff reuse policy / quarantine / marker semantics / history invalidation: unchanged.
Output TargetState ring: NOT IMPLEMENTED; blocked until Gate A passes.
PR66: Draft / Open / unmerged.
~~~

The trace deliberately does not install a process-wide `ExecuteCommandLists` hook. Current callback-boundary observations cannot identify which engine command submission actually reads a given output; adding such a hook without a narrower validated RE4 boundary would exceed the evidence and scope. Use the new runtime trace to establish callback/Present/fence order first. If the consumer-to-Present relation remains ambiguous, investigate a bounded RE4-specific read-only submission boundary on this PR; do not infer FIFO use from timestamps and do not change output reuse rules until that relation and queue ordering are proven.

### PR66 quality-transition TargetState chain update (2026-09-30)

PR66 adds bounded, allocation-free history for completed verified RE4 Overlay writer transactions so a `handoff -> A -> B` sequence is reconciled as one causal chain rather than requiring the last transaction alone to clear the original handoff. It requires contiguous observed store sequence, exact Overlay/slot and control/device/mode identity, same writer thread, validated clear/replacement sites, current terminal pointer, and a stable snapshot tied to the live store sequence. The chain is consumed once; overflow, missing/interleaved stores, stale evidence, or any mismatch still quarantines. The engine-installed TargetState is never overwritten and existing writer/downstream fence, retirement, and quarantine behavior is unchanged.

Standalone deterministic writer-chain tests and a local x64 Release build pass. Quality-change runtime validation that XeSS Execute resumes after the chained replacement remains pending; PR66 is still Draft/Open/unmerged. Output-ring and marker-pending behavior are not changed by this update.

## 48. PR66 latest paired runtime status — 2026-09-30 21:04–21:05 KST

This section supersedes the pending-runtime statements in sections 46–47 and the earlier writer-chain checkpoint above where those statements describe current status. It does not rewrite their historical evidence.

### Capture identity and transition result

The tested PR #66 source HEAD was `a5e36fca710042fb366a4833e14c3a9c0c15eb87`, matching the local branch and remote PR HEAD at review. The paired logs are:

~~~text
build-load-accessor/runtime-test-20260930/re2_framework_log.txt.run1
build-load-accessor/runtime-test-20260930/OptiScaler.log.run1
~~~

The PR is Draft/Open/unmerged. Its Build PR check completed successfully in run `36711929572`.

The user changed modes through Off -> Performance -> Balanced -> Ultra Quality Plus -> Performance -> Ultra Quality Plus -> Native AA -> Ultra Performance -> Ultra Quality. REF logged eight `OutputTransition phase=install-complete` records. It accepted two verified `handoff -> A -> B` writer chains, at control generations 2 and 6. Each acceptance matched the same live Overlay and `+0x90` slot, contiguous writer store sequence, expected generation/mode and terminal pointer; REF left the engine's terminal TargetState in the slot and retired only the displaced handoff through existing fence gates. This confirms the chain reconciliation on two observed transitions only; it does not imply every quality transition required or exercised a multi-transaction chain.

OptiScaler logged 1,120 `hk_xessD3D12Execute` entries. REF independently reached `cumulativeSuccess=1024 cumulativeFailure=0` at its Execute checkpoint. These are API invocation/checkpoint totals, not proof of the same number of uninterrupted displayed frames.

### Temporal and OutputHandoff result

The run recorded 921 `output-handoff-marker-pending` history resets and 16 `pre-overlay-temporal-gate-invalid` resets. The marker-pending path continues to skip submissions and invalidate temporal history as designed; these counts represent an outstanding production continuity issue, not a logging-only issue. A sustained low-reset session is not accepted. Controlled Off/On, Load Save, resize/fullscreen, and long-session tests also remain pending.

### Phase A Gate A remains blocked

The latest run exercised the bounded LifetimeTrace, but it still did not establish the causal chain required before changing output reuse policy:

~~~text
installed output
    -> actual downstream consumer command submission / queue
    -> eligible Present on that queue
    -> marker Signal and fence completion for that output
~~~

Evidence and diagnostic gap:

- The OutputTransition `post-present-marker-attempt` records still report `frameKnown=false`; there is no explicit frame/install token joining the callback to the installed output.
- The emitted LifetimeTrace event samples contain `present`, `post-present-callback`, `pre-overlay`, `post-overlay-observation`, and `skip` kinds. No `submit`, `output-install`, or `marker` kind is present in the emitted event records.
- LifetimeTrace checkpoint summary counters for installed/unmarked outputs, successful submit classes, marker skips/completions, and mapping ambiguities remain zero, while the separate OutputTransition stream records eight completed installs and the regular reset logs record marker-pending skips.
- Consequently the current capture proves neither downstream consumer submission identity nor which Present/marker retires a particular installed output. Callback adjacency, timestamps, successful XeSS API calls, and a queue Signal are not substitutes for that proof.

This is both a still-unmet correlation gate and a diagnostic coverage discrepancy to resolve in read-only instrumentation. The narrow next step is to make the per-install Submit/OutputInstall/Marker events and summary classification visible and carry an explicit validated install/frame token through the eligible Present and marker path; if that still cannot identify the real consumer submission, investigate a bounded RE4-specific read-only submission boundary. Do not add a process-wide `ExecuteCommandLists` hook without a validated boundary.

### Implementation-order decision

~~~text
Writer-chain reconciliation       runtime-observed twice; broader matrix pending
Phase A exact consumer/marker map  NOT PROVEN; instrumentation correlation gap remains
Phase B bounded output ring        NOT IMPLEMENTED; do not start before Gate A
Phase C temporal continuity        NOT ACCEPTED; reset churn remains high
PR66                              Draft / Open / unmerged
XeFG                              OFF and out of scope for this SR capture
~~~

## 49. PR66 phase-aware lifetime capture — active gameplay reached, Present ownership still unproven

This section supersedes the Phase-A diagnostic-gap conclusion in §48 for the new paired capture only. It does not alter the historical 21:04–21:05 capture or authorize an OutputHandoff lifetime change.

### Capture identity

~~~text
RE4 run                    2026-09-30 22:09:29–22:11:51 KST
REF embedded base          a5e36fca710042fb366a4833e14c3a9c0c15eb87
REF test DLL SHA-256        3C5CA779B421A6B970A3BD552A354F69BC73908AC0716D1531315A053C91956E
OptiScaler                  v0.9.5-pre4 / a556a639 / 20260929_135800
Paired logs                 build-load-accessor/runtime-test-phase-aware-20260930-2211/
PR66                        Draft / Open / unmerged
~~~

OptiScaler recorded 817 `hk_xessD3D12Execute` calls and 817 `Upscaling done: true` records. The REF log contains active-phase Submit, OutputInstall, OutputRestore, PostPresentCallback, Marker, Skip and downstream-fence-complete events, with IDs and queue/fence data. Thus the previous “no active dump was emitted” diagnosis is no longer current for this run: the phase-aware capture reached active XeSS gameplay and emitted bounded windows. The earlier 32-dump exhaustion was genuine for the older capture, not evidence that its in-memory recorder had stopped.

At the 22:11:32.577 mode-transition window, active counters were 429 successful submits/installs, 269 successful submissions with `resetHistory`, 160 continuous submissions, 269 skips (264 marker-pending and 5 temporal-gate), and 428 queued markers. These are the counters at that checkpoint, not whole-session frame totals. Across the paired log, the separate visible reset records reached 559 marker-pending and 12 temporal-gate events. The marker-pending path therefore still causes real skipped submissions/history invalidation; this is not just diagnostic noise.

### Observed frame-to-marker sequence

The bounded capture includes an active `frame=31068`, `trace=30677`, `install_id=428` submit/install, followed by a PostPresent callback and queued downstream marker carrying the same `install_id=428`, `present=31066`, and downstream fence value 141. The subsequent `frame=31069` boundary observes fence 141 complete and restores the output before another submit. This is an **inferred candidate** association only: the event explicitly says `marker-queued-different-present-ordinal`, no downstream consumer command submission was observed, and the lifetime trace's active mapping counters remain `proven=0`. A timestamp, callback adjacency, shared install ID, or successful queue Signal does not prove that Present 31066 read output 428.

The first active output also shows the continuity stall directly: after the initial successful install at frame 30370, frame 30371 reaches pre-Overlay while the marker is pending and is skipped; the marker is queued later. The active dump budget preserved the first install/execute, first marker-pending, first queued marker, periodic, and mode-transition windows. Ten window records are present; nine dump windows emitted event lines, one later request was suppressed by the class budget. The capture contains quality/Off transitions while active, but it does not satisfy sustained temporal-continuity acceptance.

### Diagnostic follow-up and gate

The captured build's active window summary did not print the already-maintained `installed_unmarked_high_water` (nor current unmarked/marked-incomplete counts). The follow-up source now includes those three values in each bounded window and the independent one-shot lifecycle summary. This formatting addition has not yet been runtime-captured. The paired REF log also ends without a `[LifetimeTrace] lifecycle-summary` line, so runtime emission of the shutdown summary remains unverified; do not interpret the missing line as zero counters.

The follow-up source and this status update pass `git diff --check`; the standalone lifetime-trace tests and existing writer-chain tests both pass. A new Release x64 `dinput8.dll` is built at `build-load-accessor/bin/REFramework/dinput8.dll`, PE machine `0x8664`, SHA-256 `837D34B8A7025332F6E77F5C2A1D6BAD03EF8C9C6143725F58B8537C35CBC5E0`. It is installed in the RE4 directory after confirming no `re4.exe` was running; the prior phase-aware DLL was preserved as `dinput8.dll.phase-aware-pre-summary-fields-20260930.bak`. Runtime validation of the added counters and normal-exit summary is still pending. Per the test-before-push workflow, this source change has not been committed or pushed yet.

~~~text
Phase-aware active capture         OBSERVED
OutputInstall / Restore / Marker   OBSERVED
Frame N -> N+1 marker-pending     OBSERVED; submission skipped and history invalidated
Active output high-water printed  PENDING NEXT RUNTIME CAPTURE
Present -> actual downstream read  NOT PROVEN
Output -> reader -> Present -> marker -> GPU-complete chain NOT PROVEN
Bounded output ring               NOT IMPLEMENTED; blocked on ownership proof and overlap evidence
Temporal continuity               NOT ACCEPTED; marker-pending churn remains
XeFG                              OFF and out of scope
~~~

Do not accept a ring design from the candidate mapping above. Next runtime evidence must include the new high-water field and lifecycle summary from a normal game exit (if that shutdown path invokes the Mod destructor), while preserving explicit `unknown / inferred-candidate / proven` labels. If consumer identity is still unknown, continue with a narrowly bounded, read-only RE4 submission-boundary investigation; do not add a process-wide command-list hook or change reuse, marker, fence, quarantine, or history-reset policy.

Do not relax marker-pending behavior, suppress the associated history reset, accept an unverified TargetState, clear quarantine, or release retained output early to improve counters. Output-ring sizing and implementation remain downstream of Gate A evidence. PR66 must stay Draft until the writer-transition and independent lifetime/temporal safety gates pass.

## 51. PR66 paired active capture — 2026-09-30 22:53–22:54 KST

The paired logs are preserved in `build-load-accessor/runtime-test-phase-aware-20260930-2253/`. The run used the delivered PR66 test DLL, and the REF log independently confirms the exact file hash:

~~~text
REF PR/base stamp         a5e36fca710042fb366a4833e14c3a9c0c15eb87
REF DLL SHA-256           8C7C37FC58D7D36BB5AB6A409D53F373BCC6E2392543698DE7EB24F5E6CF4CA1
OptiScaler                v0.9.5-pre4 / a556a639 / 20260929_135800
OptiScaler XeSS Execute   819; Upscaling done: true 819
REF log SHA-256           D5368DFBEA6FA935FD584E68273E763677430A2DE41AD6C9289B810ACA50FE52
OptiScaler log SHA-256    2BE7B9C743A7E4D2AA809922ADD33577E3EF3B9CD91A018745F4D28F8803C37D
PR66                      Draft / Open / unmerged
~~~

The runtime identity line exactly matches the delivered DLL SHA. The capture contains active `Submit`, `OutputInstall`, `OutputRestore`, `PostPresentCallback`, `Marker`, `Skip`, and downstream-fence completion events, so phase-aware instrumentation is working in active gameplay. At the 22:54:11.121 checkpoint, the active counters were 530 successful submits/installs, 320 reset-history submits, 210 continuous submits, 318 skips (308 marker-pending and 10 temporal-gate), 529 queued markers, `installedUnmarked=1`, unmarked-output high-water 1, `markedGpuIncomplete=0`, and mapping counts unknown=530 / inferred-candidate=1,058 / proven=0. At the preceding 22:54:05.492 checkpoint, `markedGpuIncomplete=1`. These are checkpoint values, not whole-run final totals. Across the visible REF log, there are 538 `output-handoff-marker-pending` resets and 20 `pre-overlay-temporal-gate-invalid` resets; both are real submission/history disruptions, not log-only counters.

The bounded frame sequence at the end of the active trace demonstrates the exact marker-late ordering:

~~~text
frame 12393 / trace 11889 / submit 90 -> OutputInstall install 529; writer fence 90
frame 12394 / trace 11890 enters pre-Overlay
install 529 restores with reason=restore-marker-pending; frame 12394 skips with output-handoff-marker-pending
PostPresent callback observes install 529 and Present ordinal 12392
marker queues downstream fence 90, labeled inferred-candidate / different-present-ordinal
frame 12395 enters pre-Overlay
then the trace observes actual downstream GetCompletedValue=90
~~~

The candidate association is explicit: current bookkeeping associates Present 12392 and fence 90 with `install_id=529`. That does **not** prove Present 12392 submitted or consumed a GPU read of output 529. Producer submit/install and the observed Present/marker carry the same Direct queue identity, and the actual fence completion is distinct from the cached value; however, no downstream reader command submission/resource-use edge is recorded. Therefore the marker can currently certify only completion of work ordered before its Signal on that queue, not that a particular output was read. Gate A remains blocked and no ring/reuse/lifetime change is authorized.

Two first-divergence records were emitted as pending and each resolved to an accepted verified two-transaction engine writer chain (control generations 2 and 8). OptiScaler execute calls continued after each resolution: 622 were logged after the first acceptance and 289 after the second, for 819 total. The run logged no `OutputHandoffMismatch` or `hardQuarantined=true`; this validates the observed chain scenarios only, not every transition. No writer ownership or quarantine rule was relaxed.

The log contains bounded checkpoint summaries but no `[LifetimeTrace] lifecycle-summary`; it also has no `REFramework shutting down...` record, so the `RE4XeSS` destructor summary was not observed. Do not interpret its absence as zero counts. The trace ring had recorded 9,319 active events at the last snapshot and overwritten 5,223; its retained sample is intentionally bounded. The full capture ends with configuration save/cleanup lines, but the available logs do not establish the process-exit mechanism.

~~~text
Active trace event coverage             PASS
Runtime REF DLL identity                PASS (embedded SHA matches delivered DLL)
Verified writer-chain wording/resolution PASS (2 pending -> accepted/resolved)
Execute resumes after observed chains   PASS (OptiScaler calls continue)
Marker-pending N -> N+1 skip/reset       CONFIRMED
Output -> actual downstream reader      NOT PROVEN
Eligible Present -> exact reader marker NOT PROVEN
Whole-run final summary/high-water      NOT OBSERVED
Output ring / lifetime policy change    NOT AUTHORIZED
Temporal continuity                    NOT ACCEPTED
~~~

This capture closes the active-event and writer-chain wording runtime checks, but not the consumer ownership proof. Continue with a narrow, read-only downstream submission/resource-use correlation; do not add a process-wide `ExecuteCommandLists` hook, infer ownership from queue identity or callback adjacency, or change marker, fence, reuse, quarantine, or history-reset behavior. Keep PR66 Draft/Open.

## 50. PR66 next paired active capture — Gate A ordering reproduced, ownership still unknown

Capture files preserved under `build-load-accessor/runtime-test-phase-aware-20260930-2225/`.

~~~text
Run window                 2026-09-30 22:25:24–22:26:25 KST (log timestamps)
REF embedded base stamp    a5e36fca710042fb366a4833e14c3a9c0c15eb87
REF DLL identified SHA-256 837D34B8A7025332F6E77F5C2A1D6BAD03EF8C9C6143725F58B8537C35CBC5E0
OptiScaler                 v0.9.5-pre4 / a556a639 / 20260929_135800
OptiScaler Execute         986; Upscaling done: true 986
PR66                       Draft / Open / unmerged
~~~

The hash above identifies the delivered test artifact correlated with this run; the REF runtime log itself contains the embedded base commit/build stamp, not a self-computed DLL SHA. The next delivery remains separately identified by its artifact SHA before runtime use.

### Active counters and actual frame ordering

The new phase-aware output now contains the previously missing high-water fields. At the last emitted mode-transition snapshot (`22:25:59.855`), LifetimeTrace reports 422 active successful submits/installs, 249 `resetHistory` submits, 173 continuous submits, 248 skips (245 marker-pending and 3 temporal-gate), 422 queued markers, `installedUnmarkedHighWater=1`, `mappingUnknown=422`, `mappingCandidate=844`, and `mappingProven=0`. These are snapshot counters at that timestamp, not a whole-run terminal summary. The full visible REF log contains 577 marker-pending resets and 22 temporal-gate resets; no `[LifetimeTrace] lifecycle-summary` was emitted, so the final session totals/high-water cannot be asserted.

The earliest active sequence establishes the skip ordering without changing policy: `frame=9408 / trace=9005 / install=1` submits and installs; the next `frame=9409 / trace=9006` reaches pre-Overlay, finds the previous output's marker pending, restores only under the existing gate, and skips the submission. Later, PostPresent callback `present=9406` queues a marker for `install=1`, downstream fence 1. A later sample shows the same pattern around frames 10078–10079; actual `GetCompletedValue` is logged separately from the cached completion value. This confirms a real marker-pending skip/reset, but not that Present 9406 consumed output 1: install-time Present identity is unknown and the marker says `different-present-ordinal`.

At the last active snapshot, unmarked-output high-water is one. That is not sufficient to size or authorize a ring because the output-to-reader/Present association remains candidate-only and the final summary is absent. The fixed event store retained 4,096 of 6,559 active events at the last snapshot (2,463 active events overwritten); nine of ten bounded dump windows emitted event lines and one request was suppressed. Active submit/install/marker/skip events are now present, so the former pre-active dump starvation is resolved for this capture.

### Quality transitions and writer-provenance log correction

The run logged eight mode requests and seven new XeSS context/init sequences, including a gameplay Off (`mode token 0`) followed by re-enable. Four quality transitions were accepted as verified `chainLength=2` engine writer replacements. However, each was preceded by an error-level `HandoffProvenance first-divergence; writer=unknown` line, followed by the successful chain acceptance. This is diagnostic severity/causal-reporting noise, not evidence that the accepted transitions failed; the raw pre-acceptance pointer/sample evidence must remain available.

The next narrow diagnostic change will report the initial divergence as pending validation, then emit a sequence-linked resolved record only when the same Overlay, slot, generations, and terminal pointer match an accepted verified writer chain. Unverified third-object restore failures must continue to emit errors and quarantine. No TargetState ownership, writer acceptance, retirement, marker, fence, quarantine, or history-reset policy changes are allowed.

### Gate decision

~~~text
Phase-aware active capture and bounded windows    PASS
Frame N -> N+1 marker-pending skip/reset          OBSERVED
Separate cached vs actual fence completion        OBSERVED
High-water at emitted checkpoint                 1 (not proven whole-run maximum)
Verified quality writer chains                   4 accepted; diagnostic wording follow-up needed
Actual downstream reader -> eligible Present     NOT PROVEN
Output ring                                       NOT AUTHORIZED / NOT IMPLEMENTED
Final independent lifecycle summary              NOT OBSERVED in this process log
Temporal continuity                              NOT ACCEPTED; 577 marker resets / 22 gate resets
~~~

The post-capture source changes only the pending/resolved provenance wording and sequence link, and adds a one-shot SHA-256 log of the REF module file when RE4 debug logging is enabled. A Release x64 DLL passed local lifetime-trace/writer-chain tests and PE identity validation (`0x8664`), then was installed with the previous DLL backed up after confirming no `re4.exe` was running:

~~~text
New test DLL SHA-256  8C7C37FC58D7D36BB5AB6A409D53F373BCC6E2392543698DE7EB24F5E6CF4CA1
Previous DLL SHA-256  837D34B8A7025332F6E77F5C2A1D6BAD03EF8C9C6143725F58B8537C35CBC5E0
Backup                 dinput8.dll.pre-provenance-resolution-sha-log-20260930.bak
Runtime validation     PENDING; do not commit/push before reviewing paired logs
~~~

Keep PR66 Draft and do not change output lifetime rules until the actual reader/queue/Present ownership edge is proven.
