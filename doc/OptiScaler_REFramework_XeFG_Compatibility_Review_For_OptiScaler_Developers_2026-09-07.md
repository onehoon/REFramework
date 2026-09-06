# OptiScaler × REFramework × XeFG Compatibility Review

Date: 2026-09-07  
Audience: OptiScaler developers / REFramework compatibility work  
Scope: D3D12 XeFG swapchain, resize, resource ownership, hook lifecycle, and Special K-related compatibility behavior

> This document intentionally excludes the older REFramework PDUpscaler / OptiScaler overlay-menu compatibility issue. That is a separate problem and is not part of this review.

---

## 1. Purpose

We have been working on a REFramework fork whose goal is to make the following topology work reliably without Special K:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll
XeFG         = active FG output
Special K    = not installed
```

The original failure mode was that REFramework could not reliably follow the XeFG presentation path. That part has now been addressed on the REFramework side by adding explicit XeFG-aware D3D12 swapchain discovery and binding.

After that work, a second and separate compatibility problem became visible in Monster Hunter Wilds: a resize/resource-lifecycle failure involving repeated backbuffer `Release()` calls from OptiScaler.

This review separates the responsibilities of both projects and proposes a cleaner compatibility contract so neither project needs to compensate for the other through aggressive COM-reference manipulation or Special K-specific behavior.

---

## 2. Current Tested Status

### 2.1 REFramework fork

The current fork can successfully use XeFG without Special K in tested RE Engine titles.

The fork no longer depends only on REFramework's old generic D3D12 phase-1 assumption that a real game `Present` will eventually pass through the same native DXGI vtable entry captured from a dummy swapchain.

Instead, the XeFG compatibility path now:

- detects `libxess_fg.dll`;
- observes XeFG runtime initialization;
- captures the XeFG-related command queue/factory context;
- discovers the active XeFG presentation swapchain;
- binds directly to the active swapchain;
- hooks `Present`;
- hooks `Present1`;
- hooks `ResizeBuffers`;
- hooks `ResizeTarget`;
- hooks `ResizeBuffers1`;
- supports active XeFG swapchain replacement/rebinding;
- preserves a healthy XeFG instance binding instead of entering the old D3D12 unhook/rehook loop merely because the generic Present monitor times out.

This part is working and should be treated as the REFramework-side solution to the proxy-swapchain problem.

---

## 3. Newly Confirmed Monster Hunter Wilds Resize Finding

A separate problem was observed on Monster Hunter Wilds.

### 3.1 Failing behavior before the test

With the normal OptiScaler code path, resize activity could produce a pattern where OptiScaler repeatedly attempted to release a backbuffer until its returned reference count dropped to a configured threshold.

The observed behavior included:

- `GetBuffer()` succeeding;
- `Release()` returning an abnormal very large value in the billions in the failing sequence;
- repeated OptiScaler backbuffer-release logging;
- effectively unbounded release activity;
- hang/crash behavior.

This did not look like the original REFramework XeFG presentation-discovery problem. XeFG presentation itself was active, and the failure occurred in the resize/resource cleanup path.

### 3.2 Test requested by OptiScaler developer

The OptiScaler developer asked us to disable the relevant backbuffer-release block in **both**:

- `FGHooks::hkResizeBuffers`
- `FGHooks::hkResizeBuffers1`

from `OptiScaler/hooks/FG_Hooks.cpp`.

### 3.3 Test result

We tested a locally modified OptiScaler build with those two blocks disabled.

Result:

- the infinite/repeating release log disappeared;
- Monster Hunter Wilds no longer reproduced the previous release-loop behavior;
- the tested XeFG + REFramework configuration operated normally in that scenario.

This is important because it converts the earlier ownership theory from a hypothesis into a directly demonstrated compatibility relationship:

> The Monster Hunter Wilds infinite-release failure is tied to the aggressive XeFG backbuffer-release logic in the two resize paths.

This does **not** prove that every resize issue in every game is caused by that logic, but for the tested MHW failure it is now a concrete and reproducible result.

---

## 4. The Relevant OptiScaler Pattern

The current XeFG resize path contains logic conceptually equivalent to:

```cpp
ID3D12Resource* backBuffer = nullptr;
auto result = swapchain->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

if (result == S_OK)
{
    auto refCount = backBuffer->Release();

    while (refCount > XEFG_RESOURCE_REF_LIMIT)
    {
        refCount = backBuffer->Release();
    }
}
```

with:

```cpp
#define XEFG_RESOURCE_REF_LIMIT 1
```

The intent is understandable: `ResizeBuffers` requires all references to the swapchain backbuffers to be released, and real games/mods/overlays may otherwise cause resize to fail.

The compatibility problem is that the returned COM reference count is process-wide object ownership information, not an ownership token belonging only to OptiScaler.

---

## 5. Why Draining COM References Is Unsafe in a Multi-Overlay Process

`IDXGISwapChain::GetBuffer()` gives the caller one COM reference.

If the object already has references held by other components, the returned count after releasing OptiScaler's `GetBuffer()` reference can legitimately remain greater than one.

Example:

```text
Game / engine                  : 1 reference
REFramework renderer           : 1 reference
Another wrapper / overlay      : 1 reference
OptiScaler GetBuffer()         : +1 reference
--------------------------------------------
Total                          : 4 references
```

After OptiScaler releases the reference obtained from `GetBuffer()`:

```text
Release() -> returned count 3
```

That does **not** mean OptiScaler owns three references that it is allowed to release.

If OptiScaler then continues:

```cpp
while (refCount > 1)
    backBuffer->Release();
```

it can release references owned by:

- REFramework;
- another overlay;
- another swapchain wrapper;
- the game engine;
- an FG implementation;
- another injected component.

The resulting behavior depends on what those owners do next and can include:

- use-after-release;
- invalid render-target state;
- stale resource pointers;
- broken resize reconstruction;
- corrupted COM lifetime accounting;
- crashes;
- hangs;
- nonsensical returned reference-count values once the object lifetime is already compromised.

The MHW test result strongly supports this ownership interpretation for the observed failure.

---

## 6. Recommended Ownership Rule

A simple ownership rule would make OptiScaler and REFramework much easier to compose:

> Each component should release only the COM references it explicitly owns.

For a `GetBuffer()` probe, that means:

```cpp
ID3D12Resource* backBuffer = nullptr;
if (SUCCEEDED(swapchain->GetBuffer(i, IID_PPV_ARGS(&backBuffer))))
{
    // Optional inspection only.
    backBuffer->Release(); // release exactly the reference returned to this caller
}
```

If OptiScaler itself owns additional references through:

- its menu renderer;
- HudFix;
- resource tracking;
- a wrapped swapchain;
- XeFG state;
- an internal RTV/backbuffer cache;

then those should be released through their actual owner/reset path rather than by repeatedly calling `Release()` on a backbuffer pointer until a global count happens to reach a target.

Likewise, REFramework should release REFramework-owned references before forwarding resize.

---

## 7. What REFramework Now Does During XeFG Resize

The REFramework fork hooks all relevant active swapchain resize entry points:

```text
ResizeBuffers
ResizeTarget
ResizeBuffers1
```

The intended ordering is:

```text
Resize enters REFramework hook
        ↓
REFramework renderer reset callback
        ↓
REFramework releases its own backbuffer / RTV / renderer references
        ↓
original resize path continues into OptiScaler / XeFG / DXGI
        ↓
resize completes
        ↓
next Present / Present1
        ↓
REFramework recreates its renderer state
```

This is the compatibility contract we think is preferable.

REFramework should make itself resize-safe. OptiScaler should make itself resize-safe. Neither side should force the other side's references to zero.

---

## 8. Special K Is Important Evidence Here

OptiScaler currently contains explicit Special K-aware swapchain handling using `IID_IUnwrappedDXGISwapChain`.

In relevant resize paths, when an SK swapchain is detected, OptiScaler can skip the normal backbuffer-release behavior.

Conceptually:

```text
Special K swapchain detected
        ↓
skip aggressive main-swapchain backbuffer release
```

while the non-SK path can enter the manual backbuffer-release logic.

This provides an important explanation for earlier observations where:

```text
OptiScaler + REFramework + XeFG + Special K
```

appeared more stable than:

```text
OptiScaler + REFramework + XeFG
```

The difference is not necessarily only that Special K provides a stable DXGI mediation layer.

There is also an explicit behavioral difference inside OptiScaler: Special K can cause OptiScaler to avoid the aggressive backbuffer-release path.

That means the compatibility target should **not** be to make our REFramework fork pretend to be Special K.

We do not recommend:

- emulating `IID_IUnwrappedDXGISwapChain` in REFramework;
- presenting REFramework objects as Special K objects;
- relying on Special K private behavior as a compatibility API.

That would hide the ownership problem rather than solve it.

---

## 9. Recommended OptiScaler Changes

### 9.1 Highest priority: remove or redesign refcount-drain loops

We recommend reviewing every path where OptiScaler does this pattern:

```cpp
refCount = object->Release();
while (refCount > N)
    refCount = object->Release();
```

for swapchain/backbuffer resources.

The two MHW-tested resize blocks should be the first candidates because disabling them already fixed the reproduced failure.

Recommended direction:

- release only OptiScaler-owned references;
- let resize fail naturally if a foreign component still holds a backbuffer;
- log the failure and owner-facing diagnostics if needed;
- do not solve foreign ownership by decrementing the object's COM count manually.

### 9.2 Audit the wrapped swapchain path as well

There are similar manual backbuffer-reference adjustment patterns in `wrapped_swapchain.cpp`.

These should be reviewed under the same rule.

If the wrapped swapchain owns a reference, release that exact owned reference.

If another component owns a reference, it should remain untouched.

### 9.3 Keep Special K-specific behavior only where it is genuinely an SK API compatibility requirement

Some SK-specific handling may still be necessary, for example if a particular SK wrapper requires calling `ResizeBuffers` instead of `ResizeBuffers1`.

That should be separated from resource-ownership cleanup.

In other words:

```text
SK method/API compatibility       -> may remain SK-specific
foreign backbuffer ref draining   -> should not be required for SK or non-SK
```

This separation will make the code easier to reason about and test.

---

## 10. Recommended REFramework Fork Direction

For our fork, we do **not** recommend adding defensive code that tries to survive another component repeatedly releasing REFramework-owned resources.

The current XeFG architecture should remain responsible for:

- XeFG runtime discovery;
- active swapchain discovery;
- direct binding to the presentation object;
- command-queue association;
- `Present` support;
- `Present1` support;
- `ResizeBuffers` support;
- `ResizeTarget` support;
- `ResizeBuffers1` support;
- safe renderer reset before resize;
- swapchain replacement/rebinding;
- preserving a healthy XeFG binding from false hook-monitor recovery.

It should **not** add:

- Special K GUID emulation;
- fake SK identity;
- arbitrary `AddRef()` padding to survive foreign releases;
- refcount correction loops;
- OptiScaler-specific COM lifetime hacks.

Those approaches would make the integration fragile and version-dependent.

---

## 11. One REFramework Item to Re-evaluate After the OptiScaler Fix

The REFramework fork currently has a Monster Hunter Wilds-specific XeFG resize-transition hold.

This was introduced while investigating lifecycle instability around MHW resize transitions.

Now that disabling the OptiScaler aggressive release blocks removes the observed infinite-release failure, the MHW-specific hold should be retested under the corrected OptiScaler behavior.

Possible outcomes:

### Case A — still required

If MHW still needs the transition hold even after OptiScaler stops draining foreign references, then it is likely a genuine MHW/XeFG presentation-lifecycle quirk and should remain.

### Case B — no longer required

If normal behavior remains stable without the hold, then the hold may have been compensating for the old resource-lifetime interaction and could potentially be removed.

We recommend retesting before changing it rather than removing it immediately.

---

## 12. Upstream REFramework Assessment

Upstream REFramework still has a different problem from the MHW refcount issue.

Its generic D3D12 hook flow is fundamentally based on discovering native DXGI behavior using a dummy D3D12 swapchain and then expecting the real presentation path to eventually reach the corresponding Present hook.

That assumption becomes unreliable with proxy/frame-generation swapchains.

Upstream already has special handling for Streamline/DLSS-G-related swapchain behavior, which demonstrates that frame-generation technologies sometimes require an explicit lifecycle path.

For XeFG, upstream would benefit from the following generic improvements before or together with an XeFG adapter.

### 12.1 `Present1` as a first-class presentation entry point

Modern wrapped/proxy swapchains may use `Present1` rather than only `Present`.

Recommended:

```text
Present
Present1
    ↓
shared present core
```

### 12.2 `ResizeBuffers1` as a first-class resize entry point

Supporting only `ResizeBuffers` is not enough for modern swapchain wrappers.

Recommended:

```text
ResizeBuffers
ResizeBuffers1
ResizeTarget
    ↓
shared resize/reset lifecycle
```

### 12.3 Direct binding to an externally discovered active swapchain

A generic API similar to:

```cpp
bind_external_swapchain(
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* queue,
    SwapchainSource source);
```

allows REFramework to support proxy/wrapped swapchains without forcing them through the dummy-native-Present discovery path.

### 12.4 Avoid destroying a healthy proxy binding merely because the old Present monitor times out

Once REFramework has positively identified and instance-hooked the active XeFG/proxy swapchain, a timeout in the old generic discovery model should not automatically trigger full D3D12 unhook/rehook recovery.

### 12.5 XeFG-specific adapter on top of the generic mechanism

Once the generic modern-swapchain infrastructure exists, XeFG support can be implemented as a narrow adapter that:

- detects the runtime;
- observes XeFG initialization;
- captures the queue/factory context;
- identifies the active presentation swapchain;
- hands it to the generic external-binding path.

This is preferable to adding OptiScaler-specific branches to upstream REFramework.

---

## 13. Suggested Division of Responsibility

| Responsibility | Preferred owner |
|---|---|
| Detect XeFG runtime | REFramework XeFG adapter |
| Discover XeFG/proxy presentation swapchain | REFramework |
| Bind D3D12 renderer to active presentation swapchain | REFramework |
| Handle Present / Present1 | REFramework |
| Release REFramework renderer refs before resize | REFramework |
| Handle ResizeBuffers / ResizeBuffers1 / ResizeTarget | REFramework |
| Release OptiScaler-owned renderer/menu/HudFix/resource refs | OptiScaler |
| Manage XeFG implementation-owned state | OptiScaler / XeFG runtime |
| Drain references owned by another component | Nobody |
| Emulate Special K private swapchain identity | Nobody |
| Special K-specific method compatibility | OptiScaler, only when SK is actually present |

---

## 14. Suggested OptiScaler Validation Matrix

After removing/redesigning the aggressive backbuffer-release logic, we suggest testing at least:

### Core RE Engine / XeFG cases

```text
Dragon's Dogma 2
Monster Hunter Wilds
Resident Evil Requiem / demo if available
```

with:

```text
OptiScaler = dxgi.dll
REFramework fork = dinput8.dll
Special K = absent
XeFG = enabled
```

### Lifecycle tests

For each title:

1. launch game;
2. load into gameplay;
3. open/close menus that change presentation state;
4. Alt+Tab repeatedly;
5. change resolution;
6. switch borderless/window mode where supported;
7. trigger fullscreen-state changes where supported;
8. change graphics settings that recreate the swapchain;
9. run for an extended gameplay period;
10. exit normally.

### Required observations

- no infinite `Release()` log;
- no D3D12 hook-monitor unhook/rehook loop;
- no resize hang;
- no resize crash;
- no stale swapchain binding;
- REFramework renderer reconstructs after resize;
- XeFG continues presenting;
- OptiScaler continues operating after swapchain recreation.

---

## 15. Diagnostic Logging Recommendation

For future cross-project debugging, ownership-oriented logging is more useful than raw refcount draining.

Recommended OptiScaler resize diagnostics:

```text
[XeFG][Resize] swapchain = ...
[XeFG][Resize] kind = ResizeBuffers / ResizeBuffers1
[XeFG][Resize] releasing Opti-owned menu resources
[XeFG][Resize] releasing Opti-owned FG resources
[XeFG][Resize] calling original resize
[XeFG][Resize] result = ...
```

Recommended REFramework diagnostics:

```text
[XeFG][Resize] binding_generation = ...
[XeFG][Resize] REFramework renderer reset begin
[XeFG][Resize] REFramework renderer reset complete
[XeFG][Resize] original resize result = ...
[XeFG][Present] renderer recreated
```

This makes it possible to verify that each component released its own resources without trying to infer ownership from a process-wide COM count.

---

## 16. What We Do Not Recommend

We explicitly do not recommend the following approaches:

### 16.1 Keep releasing until refcount reaches 1

This is the main behavior that the MHW test has now implicated directly.

### 16.2 Make REFramework artificially hold extra references

Adding extra `AddRef()` calls so it can survive foreign `Release()` calls would only turn the bug into a reference-count arms race.

### 16.3 Make REFramework emulate Special K

Special K-specific interfaces are an implementation detail, not a stable cross-project compatibility contract.

### 16.4 Add an OptiScaler-name check inside REFramework core D3D12 hooks

The correct abstraction is support for modern/proxy swapchains, with XeFG as a technology-specific adapter. The core should not depend on a particular mod DLL name.

### 16.5 Treat every remaining Capcom quirk as an REFramework problem

OptiScaler has separate Capcom-specific behavior related to DLSS availability, DXGI spoofing, compute-signature restore, mode correction, and other game-specific concerns.

Those should not be removed merely because REFramework's XeFG swapchain handling has improved.

This review is specifically about swapchain/resource lifetime compatibility.

---

## 17. Recommended Next Actions

### OptiScaler

1. Keep the MHW-confirmed `ResizeBuffers` / `ResizeBuffers1` aggressive backbuffer-release blocks disabled for further testing.
2. Replace them with explicit cleanup of OptiScaler-owned resources only.
3. Audit equivalent refcount-drain loops in `wrapped_swapchain.cpp` and related XeFG/FG resize paths.
4. Separate Special K API/method compatibility from backbuffer ownership workarounds.
5. Test the non-SK REFramework + XeFG topology across several RE Engine titles.

### REFramework fork

1. Keep the current XeFG discovery/binding architecture.
2. Do not emulate SK or compensate for foreign COM releases.
3. Verify that all REFramework-owned backbuffer resources are released before forwarding resize.
4. Retest the MHW-specific resize-transition hold with the corrected OptiScaler behavior.
5. Remove that MHW-specific hold only if testing shows it is no longer necessary.

### Upstream REFramework

1. Add generic `Present1` support.
2. Add generic `ResizeBuffers1` support.
3. Refactor shared present/resize lifecycle code.
4. Add a generic active/external swapchain binding mechanism.
5. Improve hook-monitor behavior for positively identified proxy swapchains.
6. Add XeFG support as an adapter on top of that generic mechanism.

---

## 18. Bottom Line

There are two independent compatibility layers here.

### Layer 1 — REFramework XeFG awareness

This was the original no-Special-K problem.

Our REFramework fork now explicitly follows the XeFG presentation lifecycle instead of depending on the native dummy-Present assumption.

That architecture appears to be the correct long-term REFramework-side fix.

### Layer 2 — resize/resource ownership

The Monster Hunter Wilds failure exposed a separate issue in OptiScaler's XeFG resize cleanup.

The key new evidence is:

> After disabling the aggressive backbuffer-release blocks in both `hkResizeBuffers` and `hkResizeBuffers1`, as suggested by the OptiScaler developer, the infinite release logging disappeared and the tested MHW configuration behaved normally.

Because that test directly changes the failing behavior, we recommend treating those refcount-drain blocks as the highest-priority OptiScaler compatibility debt in the current REFramework + XeFG integration.

The clean long-term contract is straightforward:

```text
REFramework releases REFramework-owned resources.
OptiScaler releases OptiScaler-owned resources.
XeFG manages XeFG-owned resources.
No component drains another component's COM references.
```

That should provide a more stable foundation than relying on Special K-specific detection or process-wide refcount manipulation.