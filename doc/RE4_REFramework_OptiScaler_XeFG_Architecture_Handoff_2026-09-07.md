# RE4 × REFramework × OptiScaler × XeFG Architecture Handoff

**Date:** 2026-09-07  
**Scope:** Resident Evil 4 Remake (2023), D3D12, `onehoon/REFramework`, modified OptiScaler, XeFG 2x first  
**Status:** Architecture / implementation-order handoff. This is intentionally not a detailed PR breakdown.

---

## 1. Purpose

This document defines the recommended architecture and implementation order for using REFramework as the RE Engine temporal-data producer and OptiScaler as the temporal-upscaling / frame-generation backend for Resident Evil 4 Remake, with XeFG as the first frame-generation target.

The goal is not to make REFramework own XeFG, emulate Special K, create a fake NGX game frontend, or duplicate OptiScaler's existing FG system.

The intended topology is:

```text
Resident Evil 4 Remake (D3D12)
        |
        v
onehoon/REFramework
  RE Engine temporal integration
  - render resolution control
  - jitter injection
  - color capture
  - depth capture
  - motion-vector capture
  - camera data
  - reset/lifecycle signals
        |
        v
versioned REF <-> OptiScaler direct ABI
        |
        v
OptiScaler
  - selected temporal upscaler backend
  - existing upscaler-fed FG input path
  - XeFG backend
  - XeFG proxy swapchain / presentation lifecycle
        |
        v
XeFG D3D12 proxy swapchain
        |
        v
Present
```

The primary architectural rule is:

> **REFramework produces game/engine data. OptiScaler owns upscaling, FG and the XeFG swapchain.**

---

## 2. Why RE4 is the first target

RE4 is the preferred first XeFG target for this direct integration because it removes the largest uncertainty present in D3D11/ReShade-based solutions.

RE4 is already D3D12, while XeFG is also D3D12. Therefore there is no D3D11-to-D3D12 interop swapchain layer between the game and XeFG.

REFramework's historical `pd-upscaler` / `TemporalUpscaler` implementation already demonstrates that RE4 can provide the temporal data required by an external temporal upscaler. That path is therefore a proven source of engine integration knowledge.

The first PoC should reuse the RE4-specific temporal integration concepts from that implementation, but it should not reproduce the legacy PDPerfPlugin architecture as the new permanent boundary.

RE4 is also a better first target than Granblue Fantasy: Relink + Luma for XeFG because Relink requires all of the following at once:

```text
D3D11 game
+ ReShade
+ Luma Addon
+ OptiScaler NGX interception
+ D3D11 -> D3D12 resource interop
+ Dx11wDx12SC presentation wrapper
+ XeFG D3D12 swapchain
```

RE4 avoids that entire interop/wrapper stack.

---

## 3. Existing components that should be reused

### 3.1 REFramework side

The relevant historical source is the `TemporalUpscaler` implementation from the upstream `pd-upscaler` work.

The valuable part is **not** PDPerfPlugin itself. The valuable part is the RE Engine integration that produces the data required by a temporal upscaler.

For RE4, the useful responsibilities include:

- selecting / identifying the relevant RE Engine scene/output stages;
- controlling internal render size;
- disabling or bypassing the engine TAA path where required;
- forcing image-quality scaling semantics needed to keep color/depth/MV aligned;
- injecting jitter into the actual RE Engine projection matrices;
- extracting scene color;
- extracting depth;
- extracting motion vectors;
- obtaining near plane, far plane and vertical FOV;
- computing or preserving correct MV scale;
- generating reset/history-invalid signals;
- creating and managing a display-resolution output resource.

For the first PoC, only the RE4 + D3D12 + non-VR path should be brought forward.

Do **not** bring unrelated complexity into the first implementation:

- D3D11;
- VR / stereo / multi-eye;
- DMC5 motion-vector conversion;
- other RE Engine games;
- PDPerfPlugin backend selection;
- legacy generic PDUpscaler compatibility logic;
- unrelated overlay behavior.

### 3.2 Existing onehoon/REFramework XeFG compatibility work

The current fork's XeFG compatibility subsystem must remain the presentation-side observer/binder for REFramework itself.

Existing work to preserve includes:

- XeFG runtime detection;
- XeFG swapchain observation/capture;
- direct active swapchain instance binding;
- `Present` and `Present1` handling;
- `ResizeBuffers`, `ResizeTarget` and `ResizeBuffers1` handling;
- transactional rebind semantics;
- reset-before-destructive-transition behavior;
- preservation of a positively identified healthy active XeFG binding;
- avoidance of generic timeout-driven hook churn once a healthy XeFG instance is known.

This subsystem does **not** become the creator or owner of the XeFG swapchain. It only makes REFramework coexist with OptiScaler's XeFG presentation topology safely.

### 3.3 OptiScaler side

OptiScaler already has most of the frame-generation plumbing needed after a temporal-upscaler input has been accepted.

The important existing architecture is conceptually:

```text
upscaler input
    |
    v
upscaler feature evaluate
    |
    +--> frame metadata / depth / motion vectors
              |
              v
        existing FG input path
              |
              v
          IFGFeature_Dx12
              |
              v
             XeFG
```

The direct REFramework integration should become **another source of upscaler-frame input**, not a separate XeFG implementation.

The implementation should reuse OptiScaler's existing:

- D3D12 feature provider / selected upscaler backend;
- per-frame upscaler parameter semantics;
- `FGInput::Upscaler`-style frame metadata flow where practical;
- `IFGFeature_Dx12` abstraction;
- XeFG backend;
- XeFG swapchain creation and Present path;
- existing frame time, jitter, MV scale, camera and reset setters.

---

## 4. Responsibility boundaries

The architecture should be designed around explicit ownership and lifecycle boundaries.

### REFramework owns

- RE Engine hooks;
- game-specific render-resolution logic;
- RE Engine jitter injection;
- RE Engine temporal resource discovery/capture;
- any REFramework-owned intermediate/output D3D12 resources;
- interpretation of RE4 camera data;
- RE4-specific history reset causes;
- notification to OptiScaler that a bridge session/frame is being reset or destroyed.

### OptiScaler owns

- direct bridge session implementation;
- selected upscaler backend instance;
- translation of the direct ABI frame description into OptiScaler's internal feature parameters;
- temporal upscaler evaluation;
- FG input staging;
- XeFG context;
- XeFG resources owned or retained by OptiScaler;
- XeFG proxy swapchain;
- XeFG Present / frame-generation lifecycle.

### XeFG owns

- XeFG internal context objects;
- XeFG-owned swapchain/runtime state;
- internal interpolation resources not explicitly owned by OptiScaler.

### Non-negotiable COM rule

```text
REFramework releases REFramework-owned references.
OptiScaler releases OptiScaler-owned references.
XeFG manages XeFG-owned references.
No component drains another component's COM references.
```

Every cross-DLL D3D12 pointer should be treated as **borrowed** unless the ABI explicitly states otherwise.

If OptiScaler must retain a borrowed pointer after the ABI call returns, it must:

1. take exactly one explicit `AddRef()` for its own retained ownership;
2. later perform exactly one matching `Release()`.

Never use global COM refcount values as a cleanup target.

Never use:

```cpp
while (refCount > 1)
    object->Release();
```

or any equivalent refcount normalization/drain loop.

---

## 5. Recommended direct ABI

The DLL boundary should be a small, versioned C ABI.

Do not pass C++ classes, `std::` types, `ComPtr`, STL containers, or implementation-specific OptiScaler/REFramework classes across the boundary.

The discovery model should be export-based rather than filename-based.

REFramework should enumerate loaded modules and probe for an exported entry point such as:

```cpp
extern "C" __declspec(dllexport)
const OS_REF_Interface*
OptiScaler_REF_GetInterface(uint32_t requestedVersion);
```

This avoids assuming that OptiScaler is installed as `dxgi.dll`. OptiScaler may be loaded through another supported proxy name.

A first-version interface can conceptually provide:

```cpp
struct OS_REF_Interface {
    uint32_t abiVersion;
    uint32_t structSize;

    OS_REF_Result (*CreateSession)(
        const OS_REF_SessionDesc*,
        OS_REF_Session**);

    OS_REF_Result (*SubmitFrame)(
        OS_REF_Session*,
        const OS_REF_D3D12_FrameV1*);

    void (*ResetSession)(
        OS_REF_Session*,
        uint32_t reason);

    void (*DestroySession)(
        OS_REF_Session*);
};
```

The exact names are not important yet. The important properties are:

- versioned;
- fixed-width integer types;
- explicit struct sizes;
- forward-compatible tail extension;
- no C++ ABI dependency;
- explicit resource ownership semantics;
- explicit command-list / synchronization contract.

---

## 6. Session-level contract

A direct session should represent one logical temporal-upscaler integration instance for the current RE4 renderer/swapchain generation.

A session description should carry enough immutable or rarely-changing information to initialize the selected OptiScaler backend, for example:

- D3D12 device;
- command queue if required by the selected architecture;
- initial render width/height;
- display width/height;
- quality / scaling mode;
- HDR flag;
- inverted-depth flag;
- jittered-MV flag;
- display-resolution-MV vs low-resolution-MV flag;
- auto-exposure flag where relevant.

For the first PoC, configuration may be intentionally constrained:

- one fixed supported quality mode;
- one render/display resolution relationship;
- one RE4-specific flag configuration.

Do not attempt to solve generalized quality-mode negotiation before basic rendering and XeFG lifecycle are proven.

---

## 7. Per-frame contract

The per-frame description should carry the temporal data REFramework already knows.

Conceptually:

```cpp
struct OS_REF_D3D12_FrameV1 {
    uint32_t structSize;
    uint64_t frameId;

    ID3D12GraphicsCommandList* commandList;

    ID3D12Resource* color;
    ID3D12Resource* depth;
    ID3D12Resource* motionVectors;
    ID3D12Resource* output;

    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t displayWidth;
    uint32_t displayHeight;

    float jitterX;
    float jitterY;

    float mvScaleX;
    float mvScaleY;

    float nearPlane;
    float farPlane;
    float verticalFov;
    float aspectRatio;

    double frameTimeDelta;

    uint32_t flags;
    uint32_t reset;
};
```

The first ABI should also explicitly describe resource states if the chosen call model requires OptiScaler to perform barriers.

For example:

- color incoming state;
- depth incoming state;
- motion-vector incoming state;
- output incoming state;
- expected state after return if different.

The state contract is more important than the exact field names.

---

## 8. Output-resource ownership

For the first PoC, the preferred model is:

> **REFramework creates and owns the output resource. OptiScaler only writes into it during frame evaluation.**

This is simpler than having OptiScaler allocate a D3D12 resource and return ownership across the DLL boundary.

The flow becomes:

```text
REF-owned display-resolution output texture
        |
        | borrowed pointer
        v
OptiScaler evaluates selected upscaler into it
        |
        v
control returns to REFramework
        |
        v
REFramework inserts/copies that result into the RE Engine output path
```

This gives a clean lifetime rule and avoids cross-module allocation ownership during the first implementation.

---

## 9. RE4 render integration flow

The first REFramework implementation should preserve the proven temporal logic rather than trying to infer everything from the final swapchain.

The high-level frame flow should be:

```text
RE Engine frame starts
    |
    v
REFramework determines render/display dimensions
    |
    v
REFramework generates current temporal jitter
    |
    v
same jitter is injected into RE Engine projection
    |
    v
RE Engine renders low-resolution scene
    |
    v
REFramework captures:
    - source color
    - depth
    - motion vectors
    - camera values
    - reset state
    |
    v
REFramework calls OptiScaler direct SubmitFrame
    |
    v
OptiScaler temporal upscaler evaluates into REF-owned output
    |
    v
OptiScaler stages the same frame metadata/resources for FG
    |
    v
REFramework feeds/copies upscaled result into RE Engine output
    |
    v
normal game presentation continues
    |
    v
OptiScaler XeFG presentation path generates/presents interpolated frame(s)
```

The jitter sent to OptiScaler must be the exact same jitter that REFramework applied to the RE Engine projection for that rendered frame.

---

## 10. Temporal-upscaler and FG relationship

A central design goal is to avoid two independent frame descriptions.

The direct bridge should submit one authoritative temporal frame description.

OptiScaler should use it for both:

1. temporal upscaling;
2. the existing upscaler-fed FG path.

Conceptually:

```text
              REFramework frame submission
                       |
                       v
              common OptiScaler frame data
                  /             \
                 /               \
                v                 v
       temporal upscaler      FG input staging
                                     |
                                     v
                                  XeFG
```

If implementation cleanup is necessary inside OptiScaler, the desirable long-term shape is a shared internal submission layer, for example:

```text
           FGFrameSubmissionDx12
                    ^
          /---------+----------\
         /                      \
 native upscaler input      REF bridge input
```

However, this internal refactor should not be made larger than necessary for the first PoC. Reusing existing internal `NVNGX_Parameters` as an OptiScaler-only implementation detail is acceptable.

The important rule is that NGX must not become the public ABI between REFramework and OptiScaler.

---

## 11. XeFG inputs for the first PoC

The first XeFG PoC does not need full HUD separation.

The first required frame-generation resources should be limited to the minimum necessary to prove interpolation:

```text
Depth                required
Motion vectors       required
Jitter               required metadata
MV scale             required metadata
Camera values        required metadata
Frame delta          required metadata
Reset                required lifecycle signal
HudlessColor         deferred
UIColor              deferred
Distortion           deferred
```

The existing OptiScaler XeFG backend already supports optional HUDless/UI resources, so those can be added later after basic FG is stable.

Therefore the expected first result may contain UI interpolation artifacts. That is acceptable for the architecture PoC.

Success means **real generated frames and lifecycle stability**, not perfect HUD composition.

---

## 12. Command-list and synchronization contract

This area must be designed explicitly before implementation begins because it is a likely source of GPU hangs if left implicit.

The first implementation should answer all of the following in code/comments and ABI documentation:

1. Who owns the command list passed to `SubmitFrame`?
2. Is OptiScaler allowed to record into the caller's command list?
3. Are color/depth/MV resources guaranteed to remain valid until the call returns only, or beyond return?
4. What are their incoming resource states?
5. What state does OptiScaler leave them in?
6. What is the output resource's incoming/outgoing state?
7. Does OptiScaler submit GPU work itself, or does the caller execute the command list?
8. If OptiScaler retains resources until a later FG Present, what explicit fence/lifetime guarantee protects them?

The simplest first contract is preferred:

- borrowed resources are valid for the duration of the submission call;
- REFramework owns the command-list lifetime;
- OptiScaler records only the work explicitly permitted by the ABI;
- the output resource is caller-owned;
- any resource OptiScaler needs beyond the call is copied/retained under explicit OptiScaler ownership rather than silently holding a borrowed pointer.

Where possible, reuse OptiScaler's existing per-frame copy/cache mechanisms used by upscaler-fed FG instead of extending borrowed lifetimes.

---

## 13. Reset and resize lifecycle

Reset handling must connect the new bridge to the existing fork lifecycle without creating a second competing swapchain manager.

The desired sequence on a destructive transition is:

```text
RE4 / XeFG resize, fullscreen transition, swapchain recreation, etc.
        |
        v
onehoon REFramework detects transition
        |
        v
REFramework releases only REFramework-owned render/output resources
        |
        v
REFramework notifies OptiScaler bridge session: ResetSession(reason)
        |
        v
OptiScaler resets only OptiScaler-owned upscaler/FG bridge state
        |
        v
OptiScaler's own XeFG lifecycle recreates/rebinds its XeFG swapchain as required
        |
        v
REFramework's existing XeFG compatibility layer observes and binds the new active proxy instance
```

The bridge reset callback must **not** directly make REFramework create, release or recreate XeFG swapchains.

The existing REFramework transactional swapchain rebind model should remain intact.

---

## 14. Current MHW ownership finding applies here too

The MHW investigation produced an important general rule for this project.

OptiScaler currently/previously contained aggressive backbuffer/swapchain COM-release loops in multiple FG lifecycle paths. Disabling the aggressive `ResizeBuffers` and `ResizeBuffers1` backbuffer-release blocks eliminated the reproduced MHW infinite-release behavior and restored normal behavior in the tested configuration.

Before trusting RE4 direct XeFG results, the new integration must be tested against the same ownership rule:

> OptiScaler must release only references it owns.

The direct bridge must never motivate new refcount-padding, refcount-draining, or force-release workarounds.

Any remaining equivalent aggressive drain logic in related FG/swapchain paths should be audited as part of XeFG stability work, especially where the new RE4 topology can reach those paths during resize/recreation.

---

## 15. Implementation order

This section defines the recommended **technical sequence**, not a detailed PR plan.

### Stage A - Establish the RE4 temporal provider without OptiScaler

First make the current `onehoon/REFramework` capable of producing the RE4 temporal data independently of PDPerfPlugin.

Target capabilities:

- RE4-only activation gate;
- D3D12-only;
- non-VR only;
- capture color/depth/MV;
- obtain correct render/display dimensions;
- generate and inject jitter;
- derive MV scale;
- obtain near/far/FOV;
- determine reset/history-invalid state;
- allocate/reallocate the REF-owned display-resolution output texture;
- log frame metadata at debug level.

At this stage do not call any external upscaler.

Exit criterion:

> The provider produces stable, internally consistent frame data through gameplay, Alt+Tab, resize/fullscreen transitions and normal shutdown.

### Stage B - Add ABI discovery and session negotiation

Add the narrow compatibility layer under a dedicated namespace/module, for example:

```text
src/compatibility/optiscaler/
    OptiScalerBridge.hpp
    OptiScalerBridge.cpp
```

Responsibilities:

- enumerate loaded modules;
- locate `OptiScaler_REF_GetInterface` by export;
- negotiate ABI version;
- create/destroy one RE4 bridge session;
- expose reset notification;
- fail closed if OptiScaler is missing or incompatible;
- never hardcode a proxy DLL filename.

At this stage, session creation and no-op frame submission can be used to validate lifetime order.

Exit criterion:

> RE4 can start, create a bridge session, survive transition/reset events, destroy the session and exit cleanly even before real upscaling is connected.

### Stage C - Direct temporal upscaling, FG disabled

Implement the OptiScaler side of the ABI.

The preferred first implementation is a thin adapter over existing OptiScaler internals:

```text
REF C ABI frame
    |
    v
OptiScaler direct session
    |
    v
OptiScaler-private parameter translation
    |
    v
existing FeatureProvider_Dx12 / IFeature_Dx12
```

It is acceptable for OptiScaler to build internal `NVNGX_Parameters` from the ABI frame because that remains an internal implementation detail.

First prove:

- bridge session initialization;
- selected temporal upscaler initialization;
- evaluate into REF-owned output;
- REFramework places the result into the RE Engine output pipeline;
- no PDPerfPlugin is present or used;
- resize/recreate output resource correctly;
- clean shutdown.

FG must remain disabled in this stage.

Exit criterion:

> RE4 renders correctly using the direct REF -> OptiScaler temporal-upscaler path without PDPerfPlugin.

### Stage D - Feed the same frame into OptiScaler's existing FG input layer

Once direct SR is stable, connect the already-submitted depth/MV/jitter/camera/reset data to the existing upscaler-fed FG path.

Do not create a separate REFramework-specific XeFG data model unless the existing common layer proves insufficient.

Target internal flow:

```text
REF SubmitFrame
     |
     +--> temporal upscaler evaluate
     |
     +--> common FG frame staging
               |
               v
         IFGFeature_Dx12
```

Exit criterion:

> With FG output still disabled, OptiScaler reports a valid staged FG frame every real game frame, with valid Depth and Velocity resources and coherent metadata.

### Stage E - Enable XeFG 2x

Enable only:

```text
FG input: upscaler/direct-bridge-derived frame
FG output: XeFG
Interpolation: 1 generated frame (2x output)
```

Do not begin with 3x/4x.

Do not begin with UI composition.

Verify that OptiScaler alone owns XeFG context and proxy swapchain creation.

REFramework should only observe/bind the resulting active XeFG swapchain through the compatibility logic already present in the fork.

Exit criterion:

> RE4 produces real XeFG-generated frames while the base frame is rendered through the direct REF -> OptiScaler temporal-upscaling path.

### Stage F - Lifecycle hardening

After basic XeFG works, specifically test:

- repeated Alt+Tab;
- window focus transitions;
- borderless/fullscreen transitions supported by the game;
- `ResizeBuffers`;
- `ResizeBuffers1` if reached by the XeFG proxy;
- repeated FG off/on;
- temporal-upscaler backend reinitialization if supported;
- resolution changes;
- game menu/gameplay transitions;
- RE4 cutscene transitions;
- shutdown;
- relaunch.

At this stage, trace failures by ownership domain:

```text
REFramework engine-data problem?
OptiScaler bridge/session problem?
OptiScaler FG resource-staging problem?
XeFG swapchain lifecycle problem?
REFramework XeFG proxy binding problem?
```

Do not respond to lifecycle failures by introducing cross-component release hacks.

### Stage G - HUD separation and quality work

Only after stable XeFG 2x:

- identify whether the existing RE Engine layer hooks can provide a useful HudlessColor;
- identify UI layer timing/resources;
- feed optional `HudlessColor` / `UIColor` through OptiScaler's existing FG resource abstraction;
- evaluate XeFG UI composition quality;
- then consider higher interpolation counts.

This is intentionally outside the first architecture proof.

---

## 16. Suggested REFramework source organization

Do not simply dump the historical full `TemporalUpscaler` implementation into current master.

Prefer separating engine integration from backend transport.

A conceptual structure is:

```text
src/mods/
    RE4TemporalProvider.hpp
    RE4TemporalProvider.cpp

src/compatibility/optiscaler/
    OptiScalerBridge.hpp
    OptiScalerBridge.cpp
    OptiScalerBridgeABI.h
```

The provider owns RE4-specific engine knowledge.

The bridge owns only external OptiScaler communication.

This is preferable to a class that simultaneously knows:

- RE Engine internals;
- PDPerfPlugin;
- OptiScaler ABI;
- XeFG;
- swapchain hooks.

Keep those layers independent.

---

## 17. Suggested OptiScaler source organization

The exact directory name can follow OptiScaler conventions, but conceptually keep the direct bridge thin.

For example:

```text
OptiScaler/bridge/
    REFBridge.h
    REFBridge_Dx12.cpp
```

Responsibilities should be limited to:

- export ABI;
- version negotiation;
- session creation/destruction;
- translating session/frame descriptions into existing OptiScaler internals;
- dispatching reset;
- returning clear result/error codes.

Do not embed RE4-specific engine behavior inside OptiScaler.

OptiScaler should never need to know that the data source is RE4 beyond optional diagnostics.

Long term the direct bridge should be generic enough that another engine-data producer could use the same contract.

---

## 18. Logging requirements

The first implementation needs enough logging to identify which layer failed without overwhelming normal users.

Important debug events include:

### REFramework

- bridge module/export found;
- ABI version negotiated;
- session created/destroyed;
- render/display resolution;
- temporal resource pointer/descriptor changes;
- jitter and reset transitions;
- output resource recreation;
- bridge submit success/failure;
- reset reason;
- active XeFG proxy binding generation change.

### OptiScaler

- REF bridge interface request/version;
- session create/destroy;
- selected upscaler backend;
- accepted render/display resolution;
- per-frame resource validity summary at trace/debug level;
- FG input validity transitions;
- XeFG context/swapchain create/destroy;
- reset reason;
- ownership-related resource retention/release events where useful.

Normal per-frame pointer dumps should remain trace/debug only.

No repeated unbounded Release logging should ever be accepted as normal lifecycle behavior.

---

## 19. Initial validation matrix

The first useful matrix is deliberately small.

### Base architecture

```text
Game: Resident Evil 4 Remake
API: D3D12
REFramework: onehoon fork
OptiScaler: modified bridge build
PDPerfPlugin: absent
Special K: absent
```

### Pass 1 - Provider only

- FG off;
- external upscaler bridge off;
- validate temporal capture and lifecycle.

### Pass 2 - Direct upscaling

- bridge on;
- FG off;
- one selected temporal upscaler backend;
- validate image quality and transitions.

### Pass 3 - XeFG

- same bridge;
- `FGInput` based on submitted upscaler frame;
- `FGOutput=XeFG`;
- 2x only;
- HUD separation disabled/deferred.

### Minimum transition tests

- fresh launch;
- load save / enter gameplay;
- menu -> gameplay;
- repeated Alt+Tab;
- resize or supported display-mode transition;
- FG off -> on;
- FG on -> off -> on;
- return to title/menu;
- normal exit;
- second launch.

---

## 20. First-PoC success criteria

The architecture should not be declared successful just because a generated frame appears once.

The first PoC is successful when all of the following are true:

1. REFramework captures valid RE4 color/depth/MV/jitter/camera data.
2. Direct ABI discovery and version negotiation succeed.
3. The bridge creates one well-defined session.
4. OptiScaler performs temporal upscaling without PDPerfPlugin.
5. REFramework receives/uses the direct upscaler result correctly.
6. The same authoritative frame data feeds OptiScaler's FG input layer.
7. XeFG 2x produces interpolated frames.
8. REFramework remains functional with the XeFG proxy swapchain.
9. Alt+Tab is stable.
10. resize/swapchain recreation is stable.
11. FG off/on is stable.
12. no global COM refcount drain is used.
13. no recurring/unbounded Release loop occurs.
14. no recurring REFramework D3D12 generic rehook loop occurs for an already healthy XeFG binding.
15. normal shutdown does not crash or hang.
16. second launch works without stale state.

UI interpolation artifacts are acceptable at this stage.

---

## 21. Explicit non-goals for the first implementation

Do not expand the first RE4 architecture effort into:

- D3D11 support;
- Granblue Fantasy Relink/Luma support;
- other RE Engine games;
- DMC5;
- VR;
- DLSS-G output;
- FSR FG output;
- XeFG 3x/4x;
- perfect HUD/UI composition;
- generic public SDK design;
- removal of every OptiScaler quirk;
- Special K emulation;
- fake Special K swapchain identity;
- NGX frontend emulation as the public bridge;
- PDPerfPlugin adapter mode;
- unrelated overlay/PDUpscaler menu behavior.

Those can be evaluated only after the direct RE4 D3D12/XeFG path is stable.

---

## 22. Decisions that should remain open until implementation evidence exists

A few details should deliberately not be over-designed now.

### Render-resolution / quality negotiation

For the first PoC, a fixed known-good scaling mode is acceptable. Generalized backend-driven recommended resolution can come later.

### Jitter sequence ownership

REFramework must ultimately inject the actual game jitter. Whether the first implementation computes the sequence locally or requests recommended jitter parameters from OptiScaler can remain an implementation choice, provided the exact applied jitter is submitted with the frame.

### Internal OptiScaler refactor depth

If the existing upscaler input/FG functions can be reused through a thin adapter, prefer that for the first PoC. Extract a generalized common submission layer only when duplication or lifecycle ambiguity becomes real.

### MHW-specific REFramework XeFG resize hold

Do not remove the existing MHW-specific resize-transition hold as part of this RE4 work. Retest it separately after OptiScaler ownership fixes are proven across the original MHW reproducer.

---

## 23. Architectural anti-patterns to avoid

Do not solve integration problems using any of the following:

```text
REF AddRef padding to survive OptiScaler cleanup
OptiScaler Release-until-refcount-threshold loops
fake Special K GUID / identity
REF-created XeFG context or XeFG swapchain
two independent jitter sequences
holding borrowed resources across frames without explicit ownership
hardcoded assumption that OptiScaler is dxgi.dll
copying the whole historical TemporalUpscaler backend architecture unchanged
making RE4-specific logic part of OptiScaler core
```

If one of these seems necessary, the ownership or lifecycle design is probably wrong and should be revisited.

---

## 24. Recommended next-session starting point

The next implementation/design session should start from **Stage A**, not from XeFG itself.

The first concrete question should be:

> What is the smallest clean subset of the historical RE4 `TemporalUpscaler` engine integration that must be brought into current `onehoon/REFramework` to produce a standalone D3D12 temporal-frame description without PDPerfPlugin?

From there, inspect current onehoon master against the historical `pd-upscaler` implementation and classify code into:

```text
KEEP
- RE4 engine hooks
- temporal resource capture
- jitter
- render-size control
- camera/reset data

REWRITE / ISOLATE
- output texture ownership
- provider lifecycle
- backend-facing frame descriptor

DROP FOR FIRST POC
- PDPerfPlugin
- VR
- DMC5
- other RE games
- legacy backend/UI complexity
```

Only after that provider is stable should the OptiScaler direct ABI be implemented.

---

## 25. Bottom line

The recommended RE4 architecture is not:

```text
REFramework -> XeFG
```

and not:

```text
REFramework -> fake NGX game -> OptiScaler
```

It is:

```text
REFramework
  = RE Engine temporal integration / data producer

        |
        | versioned direct frame contract
        v

OptiScaler
  = temporal upscaler + FG coordinator + XeFG owner

        |
        v

XeFG proxy swapchain
```

RE4 is the best first target because this architecture stays entirely in D3D12 and lets the project reuse the current onehoon/REFramework XeFG proxy lifecycle work instead of introducing D3D11/ReShade/swapchain interop as a second major problem.

The implementation should proceed in the order:

```text
RE4 temporal provider
    -> bridge/session lifecycle
    -> direct temporal upscaling
    -> common FG input staging
    -> XeFG 2x
    -> resize/Alt+Tab/FG-toggle hardening
    -> optional HUD/UI separation
```

That order keeps each failure domain observable and prevents the XeFG presentation layer from hiding earlier engine-data or ownership bugs.
