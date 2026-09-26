# RE4 XeSS Bridge — Architecture, Reverse-Engineering Status, and Implementation Plan

**Repository:** `onehoon/REFramework`  
**Target:** Resident Evil 4 (2023), Direct3D 12  
**Game build under investigation:** RE4 `1.5.9.0`, Steam AppID `2050650`, BuildID `22377325`  
**Date:** 2026-09-26  
**Status:** Active source of truth for the RE4 XeSS integration in this fork

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

### Capture 19 objective — close Depth convention and camera projection metadata

Capture 12 already observed a highly suggestive exact relationship:

~~~text
Camera projection:
    p22 ~= -1.000000954
    p32 ~= -0.010000009

SceneInfo projection:
    p22 ~= +0.000000954
    p32 ~= +0.010000009
~~~

This resembles the same near/far pair encoded once as normal D3D depth and once as reversed/inverted D3D depth.

Capture 19 narrows this to a direct coefficient proof rather than adding a GPU Depth readback.

The probe now reads the primary via.Camera near/far clip planes through reflected engine methods and, on the same render frame, logs:

~~~text
camera near / far
Camera projection p22 / p23 / p32 / p33
SceneInfo projection p22 / p23 / p32 / p33 / p11
expected normal-depth p22 / p32 from near/far
expected inverted-depth p22 / p32 from near/far
Camera error vs normal / inverted
SceneInfo error vs normal / inverted
sceneDepthInference
verticalFovRadians = 2 * atan(1 / SceneInfo projection[1][1])
~~~

The expected right-handed D3D 0..1 depth terms are:

~~~text
normal:
    p22 = far / (near - far)
    p32 = far * near / (near - far)

inverted:
    p22 = near / (far - near)
    p32 = far * near / (far - near)
~~~

Capture 19 procedure can be minimal:

1. select Static screen;
2. enable/reset the diagnostic;
3. collect one complete 32-sample run;
4. confirm cameraSameFrame=true and clipValid=true;
5. confirm Camera projection matches the normal formula;
6. confirm SceneInfo projection matches the inverted formula;
7. confirm perspective structure p23/p33 is stable;
8. confirm derived vertical FOV is finite/stable.

If this relationship holds consistently, close Gate E and the projection portion of Gate F without a Depth texture readback. A sparse depth-content readback remains fallback-only if the projection/clip-plane evidence contradicts itself.

The probe still does **not**:

- dispatch XeSS;
- override SceneView/render size;
- touch ImageQualityRate;
- resize the swapchain;
- modify OptiScaler;
- run in non-RE4 games;
- dump full-frame MV content;
- add a diagnostic barrier to the original VelocityTarget.

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

## 12. Remaining production gates

No production XeSS dispatch should be enabled until these gates are closed.

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

**Status: ACTIVE — Capture 19 prepared.**

Depth resource identity is already strong. The remaining requirement is to prove whether current RE4 SceneInfo/DepthStencilTex uses normal or reversed depth so the standard XeSS producer can set the correct initialization flag.

Important OptiScaler boundary:

- OptiScaler does not inspect depth content and automatically infer normal vs reversed depth for the XeSS producer path;
- its XeSS hook reads XESS_INIT_FLAG_INVERTED_DEPTH supplied by the producer and maps that to its internal DepthInverted state;
- user config can override it;
- with FGInput=Upscaler, the upscaler state is propagated into XeFG automatically.

Therefore REFramework remains responsible for setting the correct XeSS init flag.

Capture 19 uses current-build camera near/far plus Camera/SceneInfo projection coefficients to distinguish the normal and inverted D3D formulas directly. Capture 12 already shows a strong normal-Camera / inverted-SceneInfo signature; the new witness makes the near/far relationship explicit.

If Capture 19 consistently shows:

~~~text
Camera    ~= normal-depth coefficients
SceneInfo ~= inverted-depth coefficients
~~~

then production XeSS must set:

~~~text
XESS_INIT_FLAG_INVERTED_DEPTH
~~~

A GPU Depth texture content readback is fallback-only if these coefficient relationships fail or become ambiguous.

### Gate F — Camera parameters

**Status: ACTIVE — partially coupled to Capture 19.**

Capture 19 also records:

- current primary-camera near plane;
- current primary-camera far plane;
- stable perspective matrix structure;
- vertical FOV derived from the unjittered SceneInfo projection:

~~~text
verticalFov = 2 * atan(1 / projection[1][1])
~~~

This is enough to validate the camera metadata that OptiScaler/XeFG may consume when FGInput=Upscaler.

Note that native XeSS SR itself primarily needs the correct depth convention/init flag and temporal inputs; near/far/FOV are especially relevant to the downstream FG path and should still be normalized into the canonical temporal-frame contract.

### Gate G — Reset/history invalidation

Define when XeSS temporal history resets, including at least:

- first valid frame;
- resolution/render-size change;
- swapchain/device recreation;
- scene/load discontinuity;
- camera cut/teleport if the game exposes a usable signal;
- bridge enable/disable;
- invalid or missing required input.

Never blindly keep history across a destructive transition.

### Gate H — D3D12 execution point and resource states

The final producer needs a real `ID3D12GraphicsCommandList*`.

Prove:

- command list belongs to the correct DIRECT queue;
- exact queue ordering relative to RE4 scene work and Overlay/UI work;
- input resource states when XeSS executes;
- output UAV state;
- required transitions and restoration;
- submission/fence ordering;
- resource lifetime through execution and resize.

The existing engine callbacks should be exhausted first. Add narrow command-list provenance only when necessary.

### Gate I — XeSS output integration

Decide and prove:

- who allocates the display-resolution XeSS output resource;
- where it is inserted back into the RE4 output path;
- how the original UI composition remains intact;
- resource lifetime and resize behavior.

The preferred design is for REFramework to own bridge resources it allocates and to release only those references.

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
| MV absolute scale | **MEDIUM / active** | Historical W/2,-H/2 candidates; independent rotation-only reprojection witness is next |
| Depth inversion | **LOW / pending** | Resource proven, convention pending |
| Near/far/FOV | **LOW / pending** | Historical path known, current proof pending |
| Reset/history rules | **LOW / pending** | Must be designed/tested |
| Safe XeSS command list / barriers | **LOW / pending** | No production execution yet |
| Standard XeSS → upstream OptiScaler | **DESIGN LOCKED, runtime pending** | No custom OptiScaler ABI permitted |
| XeFG through `FGInput=Upscaler` | **pending after SR** | Existing presentation compatibility work remains relevant |

---

## 18. Near-term work order

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

### 18.4 Close depth convention + camera metadata

Capture 19 is now the next runtime test.

Use primary via.Camera near/far values and current SceneInfo projection coefficients to prove the D3D depth mapping and derive vertical FOV. Do not add a Depth texture readback unless coefficient evidence is ambiguous.

The standard XeSS producer must set XESS_INIT_FLAG_INVERTED_DEPTH itself when required. OptiScaler auto inherits the producer flag; it is not a substitute for producer-side depth-convention knowledge.

### 18.5 Prove command-list/state insertion

Only now add the narrow D3D12 execution machinery required for XeSS.

### 18.6 Add public XeSS producer

Call the official XeSS D3D12 API directly.

First milestone:

> Correct RE4 XeSS SR using Intel XeSS itself, with HUD/UI preserved outside the upscaled scene path.

### 18.7 Validate upstream OptiScaler

With the same producer code:

- load stock upstream OptiScaler;
- verify interception;
- verify backend substitution;
- validate frame data and output.

No REF-specific OptiScaler patch is allowed to become a requirement.

### 18.8 Validate XeFG

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

The current project is **not yet at this gate**.

The major resource and size-control uncertainty is now substantially closed: HDR/PostMain color, Depth, Velocity, the pre-Overlay engine boundary, SceneView-driven internal render size, Overlay working surfaces, and presentation output are mapped. MV channel mapping, both axis polarities, jitter exclusion, jitter injection mechanics, and absolute W/2,-H/2 scale are now closed through Capture 18. The remaining work is primarily depth/camera semantics, reset/history rules, execution ordering, output integration, and lifecycle.

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
             ▼
PrepareOutput / output composition
             │
             ▼
OutputTargetState
             │
             ▼
swapchain backbuffers
~~~

Capture 9 identifies on_pre_overlay_layer_draw() as the verified engine-level insertion boundary and the semantic Overlay-main / HDR/PostMain resource as the production Color anchor. Capture 10 establishes the native-resolution baseline. Capture 11 then proves that overriding SceneView.get_Size to 1920x1080 moves Color, Depth, and Velocity together while the 2560x1440 DXGI output remains unchanged. Captures 12-16 close render/display control, jitter injection/history, MV jitter exclusion, R=X, G=Y, and both axis polarities. Capture 17 adds the independent rotation-only witness, and Capture 18 closes the absolute MV scale at W/2,-H/2 while rejecting one fixed MV/camera frame offset as the primary source of spatial residuals. Initial zero/low-motion samples immediately after Reset remain a known UI-click input-interruption artifact, not failed directional evidence. Gate D is now closed. The next active gate is Depth/camera projection semantics: Capture 19 will use reflected primary-camera near/far plus Camera/SceneInfo projection coefficients to determine the required XeSS inverted-depth flag and validate vertical FOV without adding a GPU Depth readback unless needed. The production goal remains to insert standard XeSS SR at the pre-Overlay HDR scene boundary, keep the game's own Overlay/UI path intact, and let unmodified upstream OptiScaler intercept the standard XeSS producer calls for alternate SR and XeFG.

Until the remaining gates are proven, the diagnostic stays passive and no production XeSS dispatch is enabled.
