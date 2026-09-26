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

Capture 9 ends generic Color/Overlay resource discovery. Capture 10 establishes the native-size baseline. The active diagnostic is now a **temporary render/display split test** and remains default-off.

It currently:

- exists only for RE4;
- uses the already verified `on_pre_overlay_layer_draw()` engine boundary;
- fixes Color semantically to Overlay main == Scene `PostMainTarget` == Scene `HDRTarget`;
- reads Depth directly from `DepthStencilTex`;
- reads Velocity directly from `VelocityTarget`;
- records Color/Depth/Velocity native-resource extents and formats in one frame-local sample;
- records whether those temporal input extents are aligned;
- temporarily overrides `via.SceneView.get_Size` to a fixed **1920x1080** only while the diagnostic is enabled;
- records both the original SceneView size and the overridden size with render-frame correlation;
- records the active D3D12 swapchain `DXGI_SWAP_CHAIN_DESC1` width/height/format/buffer count;
- records the existing D3D12Hook display-size and render-size hints for comparison, without treating them as authoritative engine render size;
- pairs a post-Overlay observation on the same frame and records the current working-target extent when available;
- preserves the original Overlay call by returning `true` from the pre callback;
- caps sampling to ten observations per reset.

The old primary-Scene MRT candidate scan, PostEffect candidate scan, and repeated Overlay boundary discovery are removed from the active probe because their semantic questions are already closed.

It does **not**:

- expose production XeSS quality presets or a production upscaling UI;
- change `ImageQualityRate`;
- change TAA or dynamic-resolution settings;
- submit D3D12 work;
- issue barriers;
- modify or read back pixels;
- alter jitter;
- call XeSS;
- modify OptiScaler;
- run in non-RE4 games.

The next runtime log should focus on:

~~~text
sizeSample=
color={width=...,height=...}
depth={width=...,height=...}
velocity={width=...,height=...}
temporalExtentsAligned=
engineView ... sameFrame=... originalViewSize=...x... overriddenViewSize=1920x1080
dxgi ... swapSize=...x... hookDisplay=...x... hookRenderHint=...x...
sizeSamplePost=...
current={width=...,height=...}
~~~

The decisive question is whether the fixed SceneView override alone causes Color/Depth/Velocity to move together to 1920x1080 while the DXGI output remains 2560x1440. This is a diagnostic-only causal test, not the final XeSS quality-selection implementation.

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

Need to prove:

- the exact RE4 projection matrices that must receive jitter;
- timing before Scene rendering;
- X/Y sign convention;
- normalization convention;
- relation to current render dimensions;
- phase progression;
- that the exact jitter sent to XeSS is the jitter applied to RE4 for that frame.

Historical pd-upscaler modified projection matrix `[2][0]` / `[2][1]`, but current RE4 1.5.9 behavior still needs current-build validation.

### Gate D — Motion-vector semantics

Velocity resource identity is strong, but XeSS also requires correct semantics.

Need to establish:

- pixel-space vs normalized-space convention;
- X/Y sign;
- scale;
- low-resolution vs display-resolution MV;
- whether projection jitter is already included.

Historical pd-upscaler used approximately:

~~~text
motionScaleX = renderWidth / 2
motionScaleY = -renderHeight / 2
~~~

Treat this as a hypothesis until current RE4 behavior is measured.

### Gate E — Depth convention

Depth resource identity is strong.

Still prove:

- reversed/inverted depth or normal depth;
- near/far mapping;
- the correct XeSS depth flag.

Do not inherit the historical flag without current-build evidence.

### Gate F — Camera parameters

Need current-build values for:

- near plane;
- far plane;
- vertical FOV;
- projection convention.

Historical formula:

~~~text
verticalFov = 2 * atan(1 / projection[1][1])
~~~

Use only after current matrix convention is verified.

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
ImageQualityRate         controlled and verified
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
| Jitter | **LOW / pending** | Historical method known, current-build proof pending |
| MV scale/sign/jitter semantics | **LOW / pending** | Identity proven, semantics pending |
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

### 18.3 Prove jitter and MV semantics together

Do not validate them independently if the engine couples them.

Log one authoritative per-frame record containing:

~~~text
frame
render size
jitter x/y
projection offsets
velocity resource
velocity scale x/y
camera matrices
~~~

### 18.4 Prove depth/camera/reset

Close the remaining XeSS metadata contract.

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

The major resource and size-control uncertainty is now substantially closed: HDR/PostMain color, Depth, Velocity, the pre-Overlay engine boundary, SceneView-driven internal render size, Overlay working surfaces, and presentation output are mapped. The remaining work is primarily jitter/MV/depth/camera temporal semantics, execution ordering, output integration, and lifecycle.

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

Capture 9 identifies `on_pre_overlay_layer_draw()` as the verified engine-level insertion boundary and the semantic Overlay-main / HDR/PostMain resource as the production Color anchor. Capture 10 establishes the native-resolution baseline. Capture 11 then proves that overriding `SceneView.get_Size` to 1920x1080 moves Color, Depth, and Velocity together while the 2560x1440 DXGI output remains unchanged. Render/display size control is therefore closed; the next active gate is jitter plus motion-vector semantics. The production goal remains to insert standard XeSS SR at the pre-Overlay HDR scene boundary, keep the game's own Overlay/UI path intact, and let unmodified upstream OptiScaler intercept the standard XeSS producer calls for alternate SR and XeFG.

Until the remaining gates are proven, the diagnostic stays passive and no production XeSS dispatch is enabled.
