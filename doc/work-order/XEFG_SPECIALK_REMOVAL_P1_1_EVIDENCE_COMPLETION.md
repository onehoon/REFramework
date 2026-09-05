# Work Order P1.1 — XeFG Runtime Evidence Completion

Date: 2026-09-05
Repository: `onehoon/REFramework`
Target branch: `master`
Baseline master commit: `3682c8d926d7e63a097e2f40b0f80afa5cebea91`
Parent work order: `doc/work-order/XEFG_SPECIALK_REMOVAL_P1_DIAGNOSTIC_INSTRUMENTATION.md`
Parent analysis: `doc/REFramework_OptiScaler_XeFG_SpecialK_Removal_Analysis_Plan_2026-09-05.md`

## 1. Purpose

P1 runtime validation on Intel + Dragon's Dogma 2 produced strong evidence for the expected XeFG failure mechanism, but it also exposed one diagnostic blind spot in the P1 implementation.

This work order is a **small diagnostic-completion PR** between P1 and P2.

Do **not** begin the actual XeFG compatibility implementation in this PR.

The goals are:

1. make the hook-monitor state snapshot appear in the exact failing pre-Present state,
2. preserve the already-proven P1 behavior and evidence,
3. document the Intel XeFG DLL roles correctly,
4. collect the final no-FG / FSRFG control evidence needed for Q7,
5. close Q6 and Q7 before the P2 architecture is implemented.

The target runtime topology remains:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (fork)
Special K    = absent
FG Output    = XeFG
GPU          = Intel
Game         = Dragon's Dogma 2
```

---

## 2. Important Intel XeFG DLL Role Correction

The implementation and all future analysis must distinguish the following two DLLs.

### `libxess_fg.dll`

For the tested OptiScaler topology, this is the **XeFG runtime/API DLL used by OptiScaler**.

It exposes the public XeFG swapchain APIs relevant to our future integration, including:

```text
xefgSwapChainD3D12InitFromSwapChain
xefgSwapChainD3D12InitFromSwapChainDesc
xefgSwapChainD3D12GetSwapChainPtr
```

This is the module currently tracked by the P1 deferred XeFG probe.

### `igxess_fg.dll`

On the tested Intel system, this is the **Intel graphics-driver XeFG implementation DLL** loaded from the Intel DriverStore.

The DD2 P1 runtime log showed that the actual `XefgInterpolationSwapChain` vtable entries were owned by `igxess_fg.dll`, including:

```text
Present[8]
Present1[22]
ResizeBuffers[13]
ResizeTarget[14]
ResizeBuffers1[39]
```

Do not describe `igxess_fg.dll` as the OptiScaler-bundled/runtime API DLL.

Do not describe `libxess_fg.dll` as the Intel driver implementation DLL in this tested topology.

### Architectural consequence

Do **not** make future XeFG compatibility depend on a hardcoded assumption such as:

```text
Present owner == libxess_fg.dll
```

or:

```text
Present owner == igxess_fg.dll
```

The public XeFG API surface is the preferred stable integration point. Actual swapchain implementation ownership may vary by GPU, driver, XeFG runtime version, or topology.

For P1.1, this distinction is primarily documentation/diagnostic context. Do not add behavioral logic based on either DLL filename.

---

## 3. P1 Intel DD2 Runtime Evidence Already Established

Tested OptiScaler build:

```text
OptiScaler v0.9.5-pre3
commit/build id: 94b3f414
release/0.9 line
```

Tested REFramework P1 build reported:

```text
Commit hash: e27674612dc4a11f6b8703a29fb84262591e33e5
```

No Special K was present.

### 3.1 XeFG is alive and presenting

OptiScaler successfully created XeFG and continuously presented frames through its XeFG path.

Observed sequence included:

```text
XeFG_Dx12::CreateSwapchain1 XeFG swapchain created
FGHooks::HookFGSwapchain Hooking FG SwapChain present
...
FGHooks::FGPresent
XeFG_Dx12::Present
FGHooks::hkFGPresent1
LocalPresent
MenuOverlayDx::Present
RenderImGui_DX12
LocalPresent Calling original present
LocalPresent Original present result: 0
```

Therefore the failing REFramework overlay is **not** caused by XeFG presentation stopping.

### 3.2 XeFG creation precedes REFramework phase-1 hook installation

The important startup ordering was:

```text
21:13:23.620  igxess_fg.dll loaded
21:13:23.704  XeFG-related CreateSwapChainForHwnd begins
21:13:23.728  native/internal swapchain created
21:13:23.728  Opti WrappedIDXGISwapChain4 created
21:13:24.032  XeFG swapchain created
21:13:24.032  Opti hooks XeFG Present / Present1

21:13:24.469  REFramework begins D3D12 hook setup
21:13:24.487  REFramework creates dummy swapchain
21:13:24.487  REFramework installs phase-1 Present[8] hook
```

This means the current global `CreateSwapChainForHwnd` instrumentation can miss the **initial** XeFG/internal presentation swapchain creation because REFramework's D3D12 hook is installed later.

Do not attempt to solve this ordering problem in P1.1.

### 3.3 REFramework phase-1 target is native Windows DXGI

The dummy swapchain snapshot showed:

```text
Present[8] owner         = C:\Windows\System32\dxgi.dll
Present1[22] owner       = C:\Windows\System32\dxgi.dll
ResizeBuffers[13] owner  = C:\Windows\System32\dxgi.dll
ResizeBuffers1[39] owner = C:\Windows\System32\dxgi.dll
```

The installed phase-1 hook was:

```text
[D3D12][HookInstall]
phase = phase1
slot = Present[8]
target_owner = C:\Windows\System32\dxgi.dll
```

### 3.4 `D3D12Hook::present()` is never reached

In the failing no-SK XeFG run:

```text
[D3D12][PresentEntry]
```

appeared **zero times**.

Likewise:

```text
[D3D12][PhaseTransition]
```

appeared **zero times**.

There was no D3D12 renderer / ImGui initialization sequence.

This establishes:

```text
REF phase-1 native Present[8]
    -> no callback
    -> no phase1 -> instance transition
    -> no REF D3D12 overlay initialization
```

### 3.5 Later XeFG candidate confirms driver-owned proxy methods

During a later resize/recreation event, P1 captured:

```text
type_name = struct XefgInterpolationSwapChain

Present[8]         owner = igxess_fg.dll
Present1[22]       owner = igxess_fg.dll
ResizeBuffers[13]  owner = igxess_fg.dll
ResizeTarget[14]   owner = igxess_fg.dll
ResizeBuffers1[39] owner = igxess_fg.dll
```

The method addresses matched the addresses OptiScaler had already logged when hooking the XeFG swapchain.

This is strong cross-log evidence that the candidate really is the active Intel XeFG proxy object.

### 3.6 Present1 is confirmed active

OptiScaler repeatedly logged the XeFG `Present1` path while REFramework's existing `Present[8]` callback remained at zero entries.

This confirms that `Present1` support is a real P2 requirement, not merely a static-code hypothesis.

### 3.7 Intel DD2 did not reproduce the NVIDIA/MHW release storm

Intel DD2 performed many XeFG resize operations without the pathological sequence seen in the separate NVIDIA/MHW test:

```text
Releasing backbuffer ... RefCount 3xxxxxxxxx
```

Do not mix the NVIDIA/MHW resize/refcount crash investigation into P1.1.

That is a separate OptiScaler/GPU/game-specific investigation track.

---

## 4. Remaining P1 Evidence Gap

The parent P1 work order requires hook-monitor snapshots at both:

```text
Last chance encountered for hooking
```

and:

```text
Sending rehook request for D3D
```

The failing Intel XeFG runtime produced the recovery messages repeatedly, approximately on the existing recovery cadence, but produced **no**:

```text
[D3D12][HookMonitor]
```

lines.

This prevents Q6 from being closed directly from the log.

---

## 5. Root Cause of the Missing Q6 Diagnostic

Current `REFramework::hook_monitor()` gates the D3D12 diagnostic with:

```cpp
if (renderer_type == REFramework::RendererType::D3D12 && d3d12 != nullptr) {
    d3d12->log_hook_monitor_snapshot("last_chance");
}
```

and similarly for `rehook_request`.

However, `REFramework.hpp` initializes:

```cpp
RendererType m_renderer_type{RendererType::D3D11};
```

The XeFG failure occurs **before `D3D12Hook::present()` is ever entered**.

Therefore the diagnostic is incorrectly dependent on renderer state that the failing hook path never gets a chance to establish.

This is a diagnostic bug, not evidence that the game is actually running D3D11.

The same gate also suppresses the P1 diagnostic lifecycle reason:

```text
[D3D12][HookLifecycle] action = hook, reason = hook_monitor_recovery
```

in the exact failure state we need to observe.

---

## 6. Required Code Change — Decouple Recovery Diagnostics from `renderer_type`

### 6.1 Last-chance snapshot

When `Last chance encountered for hooking` is emitted, log the D3D12 snapshot whenever the D3D12 hook object exists.

Do not require `renderer_type == D3D12` for this diagnostic.

Recommended minimal shape:

```cpp
spdlog::info("Last chance encountered for hooking");

if (d3d12 != nullptr) {
    d3d12->log_hook_monitor_snapshot("last_chance");
}
```

This is diagnostics only.

A D3D11 title may therefore occasionally print a dormant D3D12-hook snapshot while initial renderer detection is unresolved. That is acceptable for P1 diagnostics and is preferable to suppressing the exact pre-Present D3D12 failure state under investigation.

Do **not** alter renderer selection merely to avoid this diagnostic line.

### 6.2 Rehook-request snapshot

At `Sending rehook request for D3D`, emit the D3D12 snapshot before the recovery action whenever `d3d12 != nullptr`.

Recommended shape:

```cpp
spdlog::info("Sending rehook request for D3D");

if (d3d12 != nullptr) {
    d3d12->log_hook_monitor_snapshot("rehook_request");
}
```

### 6.3 Recovery reason must match the recovery path actually chosen

Do not use the stale `renderer_type` value to decide whether to log the D3D12 recovery reason.

The existing recovery action already uses:

```cpp
if (m_is_d3d11) {
    hook_d3d11();
} else {
    hook_d3d12();
}
```

Keep that behavior unchanged.

When the code is about to take the `hook_d3d12()` branch, emit:

```text
[D3D12][HookLifecycle] action = hook, reason = hook_monitor_recovery
```

Example structure:

```cpp
if (m_is_d3d11) {
    hook_d3d11();
} else {
    if (d3d12 != nullptr) {
        spdlog::info(
            "[D3D12][HookLifecycle] action = hook, reason = hook_monitor_recovery");
    }

    hook_d3d12();
}
```

It is also acceptable to emit the reason immediately before the branch using an equivalent condition, as long as the log accurately describes the recovery action that is actually taken.

### 6.4 Do not modify the recovery algorithm

P1.1 must **not** change:

- the 5-second / last-chance timing,
- the extra recovery delay,
- `m_has_last_chance`,
- `m_last_present_time`,
- `m_last_chance_time`,
- `m_is_d3d11` / `m_is_d3d12` semantics,
- `RendererType`,
- D3D11/D3D12 selection,
- actual hook/unhook order.

Only make the diagnostic output observe the state that already exists.

---

## 7. Required Q6 Output

The next Intel DD2 no-SK XeFG run must show, for both recovery events, lines equivalent to:

```text
Last chance encountered for hooking
[D3D12][HookMonitor] event = last_chance, ...

Sending rehook request for D3D
[D3D12][HookMonitor] event = rehook_request, ...
[D3D12][HookLifecycle] action = hook, reason = hook_monitor_recovery
```

For the currently observed failure, the expected values are approximately:

```text
is_hooked              = true
is_phase_1             = true
inside_present         = false
active_swapchain       = 0x0
active_device          = 0x0
active_command_queue   = 0x0
present_entry_count    = 0
xefg_module_loaded     = true
last_present_entry_age_ms = -1
```

These are **expected observations, not values to hardcode**.

The runtime log is authoritative.

If any field differs, report the actual value and investigate before P2.

---

## 8. XeFG Module Diagnostics — Preserve Correct Roles

P1.1 does not require new XeFG hooks.

The existing deferred probe for:

```text
libxess_fg.dll
```

should remain the public-runtime/API probe.

Do not change it to `igxess_fg.dll`.

Do not probe the public `xefgSwapChainD3D12*` exports from `igxess_fg.dll` as a replacement for `libxess_fg.dll`.

If any diagnostic wording is touched while implementing P1.1, use role-accurate labels such as:

```text
libxess_fg.dll : OptiScaler-used XeFG runtime/API module
igxess_fg.dll  : Intel driver-side XeFG implementation module (Intel test)
```

Optional: if adding a concise structured log for `igxess_fg.dll` materially improves clarity, it must remain diagnostic-only and must not perform heavy loader work inside the DLL notification callback. This is optional and should not expand the PR unnecessarily because the candidate owner logging already captures the DriverStore path.

---

## 9. Files Expected in Scope

Required:

```text
src/REFramework.cpp
```

Only if necessary for a very small diagnostic accessor/label improvement:

```text
src/D3D12Hook.hpp
src/D3D12Hook.cpp
```

Do not touch renderer code, Present implementation, resize handling, or XeFG API hooking in this PR.

Expected functional code delta should be very small, ideally well under 100 LOC.

---

## 10. Non-Goals

P1.1 must not:

- hook `Present1`,
- hook `ResizeBuffers1`,
- hook any `xefgSwapChainD3D12*` export,
- bind to `XefgInterpolationSwapChain`,
- bind to OptiScaler's wrapped/internal swapchain,
- capture the XeFG command queue,
- add a new swapchain state machine,
- change current Present/resize behavior,
- alter the hook-monitor timing or recovery policy,
- suppress the repeated rehook loop,
- modify OptiScaler,
- add Special K behavior,
- add Intel driver offsets or pattern scans,
- hardcode `igxess_fg.dll` as the universal XeFG detection mechanism,
- add NVIDIA/MHW resize/refcount workarounds,
- add Requiem-specific fixes.

P2 remains the first functional XeFG compatibility phase.

---

## 11. Build / Static Verification

Run the normal fork PR build path.

At minimum:

1. configure CMake using the repository's existing CI-equivalent command,
2. build `REFramework`,
3. confirm `dinput8.dll` is produced,
4. run the existing direct-struct-field audit if still present in CI,
5. inspect the final diff for unrelated changes.

There is no requirement to change CI in P1.1.

---

## 12. Runtime Validation Matrix

Runtime testing must use the same P1.1 REFramework binary for each comparison.

### Test A — Intel DD2, no-SK XeFG failure reproduction

Required topology:

```text
GPU          = Intel
Game         = Dragon's Dogma 2
OptiScaler   = dxgi.dll, release/0.9-line build
REFramework  = dinput8.dll, P1.1 build
Special K    = absent
FG Output    = XeFG
```

Required evidence:

- `libxess_fg.dll` runtime/API module is observed,
- Intel driver `igxess_fg.dll` is observed in the runtime/candidate ownership path,
- Opti XeFG Present/Present1 continues to work,
- `[D3D12][PresentEntry]` remains zero if the failure is unchanged,
- no phase transition if the failure is unchanged,
- `last_chance` snapshot appears,
- `rehook_request` snapshot appears,
- `hook_monitor_recovery` lifecycle reason appears before D3D12 recovery,
- no behavior change from P1.

### Test B — Intel DD2 native/no-FG control

Preferred mandatory control:

```text
same game / GPU / REFramework P1.1 build
Special K absent
frame generation disabled
```

Keep other settings as close to Test A as practical.

Required evidence:

- REFramework reaches `D3D12Hook::present()`,
- `[D3D12][PresentEntry]` appears,
- `[D3D12][PhaseTransition] phase1 -> instance` appears,
- D3D12 renderer/ImGui initialization succeeds,
- REFramework overlay works,
- recurring hook-monitor recovery does not continue after successful binding.

This is the primary Q7 control.

### Test C — FSRFG control, if practical

Recommended but not required if Test B already provides a clean successful control.

Use the same game/GPU/P1.1 build with FSRFG instead of XeFG.

Capture:

- candidate type/classification,
- Present entry path,
- phase transition,
- instance hook installation,
- hook-monitor behavior.

This is useful because it compares XeFG against another FG topology rather than only against no-FG.

---

## 13. Required P1 Evidence Summary After Runtime Validation

Produce a concise table answering the original P1 Q1-Q7.

### Q1 — dummy phase-1 Present owner

Already established on Intel DD2 XeFG:

```text
C:\Windows\System32\dxgi.dll
```

### Q2 — active XeFG/Opti candidate

Already strongly established:

```text
XefgInterpolationSwapChain
```

with Opti's internal wrapped/native presentation path downstream.

### Q3 — XeFG candidate Present owners

On the tested Intel system:

```text
Present[8]   -> igxess_fg.dll
Present1[22] -> igxess_fg.dll
```

Treat this as Intel runtime evidence, not a universal filename contract.

### Q4 — current `D3D12Hook::present()` after XeFG activation

Already established:

```text
No / present_entry_count = 0
```

### Q5 — phase1 -> instance

Already established:

```text
No
```

### Q6 — hook-monitor state

Must be filled from the P1.1 runtime snapshots.

Do not infer or manually substitute expected values.

### Q7 — control difference

Must be filled from Test B and optionally Test C.

At minimum explain:

```text
successful control:
  phase-1 Present callback is reached
  -> phase1 -> instance transition occurs
  -> renderer/ImGui initializes
  -> hook monitor stops recovering

XeFG failure:
  phase-1 native Present callback is never reached
  -> no instance binding
  -> no REF D3D12 renderer initialization
  -> repeated recovery
```

Use the actual control log to name the candidate/owner path rather than assuming it in advance.

---

## 14. P2 Gate

Do not start P2 until:

- Q6 is captured directly from the P1.1 log,
- Q7 has a successful control log,
- Q1-Q7 can all be answered from runtime evidence.

Once this gate is satisfied, P2 may implement the first functional XeFG path.

Current evidence already strongly supports the following P2 direction, but **P1.1 must not implement it**:

```text
1. use public libxess_fg.dll XeFG API interception as the stable integration point,
2. capture the exact swapchain/queue/factory information around XeFG initialization,
3. support Present1 as a first-class presentation callback,
4. avoid relying on the dummy native DXGI Present[8] discovery path after XeFG becomes active,
5. avoid hardcoding the Intel driver implementation DLL name,
6. preserve native D3D12 / D3D11 / Streamline / FSRFG behavior.
```

The P2 work order should be written only after P1.1 runtime evidence is reviewed.

---

## 15. Completion Criteria

P1.1 is complete when all of the following are true:

- hook-monitor snapshots are no longer suppressed by the pre-Present renderer-type state,
- `last_chance` and `rehook_request` both show D3D12 state when a D3D12 hook object exists,
- `hook_monitor_recovery` accurately labels the actual D3D12 recovery branch,
- renderer selection and recovery behavior are unchanged,
- no Present1/ResizeBuffers1/XeFG functional hook is added,
- build succeeds,
- Intel DD2 no-SK XeFG reproduces the same behavior with Q6 now visible,
- native/no-FG control succeeds and supplies Q7,
- optional FSRFG control is captured if practical,
- the final Q1-Q7 evidence summary is complete,
- `libxess_fg.dll` and `igxess_fg.dll` roles are described correctly.

---

## 16. Suggested PR Scope

One small PR is preferred.

Suggested branch:

```text
diag/xefg-p1-evidence-completion
```

Suggested PR title:

```text
diag: complete XeFG P1 runtime evidence
```

Keep the PR diagnostic-only and materially below the user's normal ~500 LOC PR preference.
