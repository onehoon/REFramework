# REFramework × OptiScaler × XeFG — P3 Lifecycle Robustness Plan

Date: 2026-09-06

## 1. Purpose

P2 through P2.2 established a working Special-K-free D3D12 presentation path for the target topology:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (this fork)
Special K    = absent
FG Output    = Intel XeFG
Renderer     = D3D12
```

P2.2 is now a functional baseline on Intel + Dragon's Dogma 2:

- the game starts successfully,
- XeFG remains active,
- the OptiScaler overlay works,
- the REFramework overlay works,
- the earlier `DXGI_ERROR_DEVICE_REMOVED / DXGI_ERROR_ACCESS_DENIED` failure is gone,
- the later `ResizeBuffers1 -> DXGI_ERROR_INVALID_CALL` failure is gone,
- repeated observed `ResizeBuffers1` calls return `S_OK`,
- Present/Present1 continues afterwards,
- the recurring REFramework hook-monitor recovery loop is absent in the successful run.

P3 is therefore **not another discovery phase for basic rendering**.

P3 exists to make the now-working XeFG binding survive the complete presentation lifecycle:

```text
initial XeFG init
    -> normal Present/Present1
    -> ResizeTarget / ResizeBuffers / ResizeBuffers1
    -> resolution / window-mode changes
    -> Alt+Tab / minimize / restore
    -> XeFG re-initialization
    -> internal swapchain or presentation-queue replacement
    -> hook-monitor interaction
    -> game shutdown / module teardown
```

The guiding rule is:

> Preserve the proven P2.2 render path and close lifecycle gaps one small PR at a time.

Do not redesign the working presentation path merely to make the architecture look cleaner.

---

## 2. Required Base

Repository:

```text
onehoon/REFramework
```

P3 planning baseline:

```text
master
517cf67533babedbd89131b65da5e41241589437
feat: handle XeFG ResizeBuffers1 resets (#10)
```

If `master` advances, every later work order must rebase on current `master` and preserve all previously merged XeFG behavior.

Relevant pinned dependency reviewed for lifecycle semantics:

```text
cursey/kananlib
8c27b656734355db0f2893581fd62e838fa130ad
```

Its `VtableHook` stores the target object address but does **not** call COM `AddRef` on the target. `VtableHook::~VtableHook()` calls `remove()`, which attempts to read/write the target object's vtable pointer to restore the original vtable.

That fact matters directly for XeFG swapchain replacement: REFramework must ensure a hooked COM object is still alive until its vtable hook has been removed.

---

## 3. Scope Boundary

P3 acceptance is intentionally narrow.

### In scope

```text
REFramework + OptiScaler + Intel XeFG + D3D12 + no Special K
XeFG internal presentation swapchain lifecycle
actual XeFG presentation queue lifecycle
Present / Present1 continuity
ResizeBuffers / ResizeBuffers1 / ResizeTarget continuity
internal swapchain replacement
presentation queue replacement
fullscreen/window-mode transitions
resolution changes
Alt+Tab / minimize / restore
hook-monitor interaction during those transitions
safe REFramework renderer reset/reinitialize
safe hook removal/rebind ordering
end-of-session / relevant XeFG module lifecycle if runtime evidence reaches it
final REFramework scripting smoke validation
```

### Explicitly out of scope

```text
native/no-FG acceptance work
FSRFG
Special K compatibility
rewriting OptiScaler
rendering the REFramework overlay on the public XeFG interpolation proxy
private Intel object-layout offsets
NVIDIA Monster Hunter Wilds release-storm investigation
Resident Evil Requiem stutter investigation before P3 acceptance is complete
large generic D3D12 refactors unrelated to the XeFG path
```

Existing non-XeFG behavior should not be intentionally broken, but P3 design must not be constrained by adding native/no-FG test requirements.

---

## 4. Confirmed P2.2 Baseline

The P2.2 Intel DD2 run confirms the binding selected by P2.1 remains the correct render path.

Observed queue relationship:

```text
init queue              = direct, priority 0
presentation queue      = direct, priority 100
queue relation          = distinct_same_device
selected render queue   = presentation queue
probe mode              = presentation_queue_render
```

The critical P2.2 resize sequence succeeds:

```text
[XeFG][ResizeBuffers1] stage = enter
[XeFG][ResizeBuffers1] stage = pre_reset_begin
Reset!
[XeFG][ResizeBuffers1] stage = pre_reset_end
[XeFG][ResizeBuffers1] stage = original_return, result = 0x00000000
```

The same 1280x720 `ResizeBuffers1` path succeeds more than once in the run.

After successful resize:

```text
REFramework reinitializes D3D12
Present / Present1 continues
OptiScaler/XeFG presentation continues
REFramework overlay remains available
```

The successful P2.2 run does not show the previous leading failures:

```text
DXGI_ERROR_DEVICE_REMOVED
DXGI_ERROR_ACCESS_DENIED
DXGI_ERROR_INVALID_CALL
Present1 failed
recurring Last chance -> rehook loop
```

This is the baseline that every P3 PR must preserve.

---

## 5. Current Lifecycle Map in `master`

### 5.1 XeFG module discovery

REFramework observes `libxess_fg.dll` through loader notification and the `LdrLoadDll` handoff.

After `LdrLoadDll` returns, it installs hooks for the public XeFG exports, including:

```text
xefgSwapChainD3D12InitFromSwapChainDesc
xefgSwapChainD3D12GetSwapChainPtr
```

This avoids performing the full API-hook installation from the loader-notification callback.

### 5.2 XeFG initialization transaction

`xefgSwapChainD3D12InitFromSwapChainDesc` currently creates one process-global transaction containing:

```text
context
hwnd
init_queue
factory
presentation_queue
candidate internal swapchain
factory-create success
outer XeFG init result
```

During the outer XeFG init call, REFramework temporarily instance-hooks the supplied factory's:

```text
IDXGIFactory2::CreateSwapChainForHwnd[15]
```

The temporary detour captures:

```text
candidate internal swapchain
actual ID3D12CommandQueue passed as the factory's D3D12 pDevice argument
```

The second value is the actual XeFG presentation-creation queue established by P2.1.

### 5.3 Candidate validation and publish

After the outer XeFG init returns successfully, `publish_xefg_candidate()` validates:

```text
IDXGISwapChain3 availability
HWND match
candidate D3D12 device
init queue D3D12 device
presentation queue D3D12 device
canonical COM identities
presentation queue type == DIRECT
candidate / queue device identity
```

For the confirmed Intel path:

```text
relation = distinct_same_device
```

so the presentation queue is selected and renderer callbacks remain enabled.

### 5.4 Active external binding

`bind_external_swapchain()` currently stores:

```text
m_swap_chain
m_command_queue
m_device
m_swapchain_source
m_xefg_p21_observe_only
```

and installs an instance `VtableHook` on the active XeFG internal swapchain:

```text
Present[8]
ResizeBuffers[13]
ResizeTarget[14]
Present1[22]
ResizeBuffers1[39]   // P2.2
```

The current active object pointers are raw COM pointers.

### 5.5 Present lifecycle

`Present` and `Present1` converge on `present_common()`.

For the active XeFG internal binding it:

```text
acquires hook_monitor_mutex
validates active hook/swapchain
updates Present entry count/time
marks inside_present
runs REFramework renderer callback when render mode is enabled
calls original Present/Present1
logs device removal if returned
runs post-present callback / liveness update
clears inside_present
```

The renderer uses `D3D12Hook::get_command_queue()`, which on the confirmed Intel path is the actual presentation queue.

### 5.6 Resize lifecycle

Current pre-original reset coverage is:

```text
ResizeBuffers[13]  -> existing on_reset() path
ResizeTarget[14]   -> existing on_reset() path
ResizeBuffers1[39] -> P2.2 on_reset() path for XeFGInternal
```

P2.2 proved that releasing REFramework backbuffer/RTV references before `ResizeBuffers1` is required.

The next Present/Present1 recreates the D3D12 renderer.

### 5.7 Hook monitor

The existing monitor is generic:

```text
~5 seconds without recognized present activity -> Last chance
~1 second later -> D3D rehook request
```

P2.1/P2.2 Present accounting now keeps this stable during normal XeFG rendering.

There is not yet a XeFG-specific transition/minimize policy.

### 5.8 Shutdown / module lifecycle

`D3D12Hook::~D3D12Hook()` calls `unhook()`.

The loader notification code defines an unloaded notification reason, but current XeFG handling only acts on module load. The XeFG module-loaded flag and global XeFG API `FunctionHook` objects do not currently implement an unload/reload state machine.

This is a later lifecycle concern, not a reason to destabilize the working P2.2 path immediately.

---

## 6. Lifecycle Gaps Found by Code Review

### Gap A — full XeFG internal swapchain replacement is deliberately rejected

Current `publish_xefg_candidate()` contains the explicit P2 guard:

```cpp
if (hook->is_hooked()
    && hook->get_swap_chain() != nullptr
    && hook->get_swapchain_source() == SwapchainSource::XeFGInternal
    && hook->get_swap_chain() != pending.swapchain) {
    spdlog::warn(
        "[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = p3_rebind_deferred",
        reinterpret_cast<uintptr_t>(pending.swapchain));
    return;
}
```

This was correct for P2 because silently replacing a proven binding was intentionally deferred.

It is now the primary P3 gap.

If XeFG creates a different internal presentation swapchain after a mode change/re-init, REFramework remains attached to the old object.

**Priority: highest.**

### Gap B — active XeFG COM objects are not strongly owned by `D3D12Hook`

Current active fields are raw pointers:

```cpp
ID3D12Device4* m_device;
IDXGISwapChain3* m_swap_chain;
ID3D12CommandQueue* m_command_queue;
```

The temporary and pending XeFG structures also currently publish raw pointers.

The pinned `kananlib::VtableHook` does not own the target object. Its destructor attempts to restore the old vtable by accessing the original target address.

Therefore a robust replacement sequence needs explicit XeFG-side COM ownership at least for the lifetime of the installed hook.

Required invariant:

```text
old XeFG swapchain remains strongly referenced
        until
old VtableHook has been removed/restored
        then
old strong reference may be released
```

**Priority: highest and prerequisite for enabling replacement.**

### Gap C — binding identity is incomplete

Current idempotence check is approximately:

```cpp
source == source
swapchain == swapchain
command_queue == command_queue
hook exists
hooked == true
```

It does not include the XeFG observe/render mode.

Also, the current P3 deferral only catches a changed swapchain pointer.

A same-swapchain / changed-presentation-queue re-init can therefore bypass the P3 deferral and reach `bind_external_swapchain()` without an explicit renderer reset.

A same-swapchain / same-queue / changed observe-mode case can be incorrectly treated as already bound.

Required identity for XeFG must include at minimum:

```text
swapchain pointer
selected command queue pointer
source
observe/render mode
```

Device identity should continue to be validated before publish and logged for diagnostics.

**Priority: highest and prerequisite for enabling replacement.**

### Gap D — rebind is not transactional yet

`bind_external_swapchain()` currently removes the previous instance hook before constructing/committing the new active binding.

That is acceptable for the initial P2 path, but a replacement path should define explicit failure semantics.

Desired P3 invariant:

```text
validated candidate
    -> preserve old binding until replacement is ready
    -> release old REFramework renderer resources
    -> remove old hook while old object is still alive
    -> commit new object/queue/mode
    -> install/activate new hook
```

If a new hook cannot be installed, the code must not leave ambiguous active pointers.

**Priority: P3.2 after ownership/identity foundation.**

### Gap E — repeated resize events can trigger repeated reset callbacks

P2.2 logs show sequences where `ResizeTarget` resets REFramework and a following `ResizeBuffers1` resets it again before the renderer is rebuilt.

This is currently non-fatal, and P2.2 remains successful.

However `REFramework::on_reset()` also reaches mod/device-reset callbacks, so repeated reset signals are not completely free.

Do not change this in P3.1/P3.2 unless runtime evidence shows it is harmful.

**Priority: later / evidence-driven.**

### Gap F — module unload/reload is not modeled

`LDR_DLL_NOTIFICATION_REASON_UNLOADED` is defined but not used for XeFG lifecycle handling.

Current global state such as:

```text
s_xefg_module_loaded
g_xefg_init_hook
g_xefg_get_swapchain_hook
g_xefg_transaction
g_pending_xefg_binding
```

is load-oriented and effectively process-lifetime state.

A real unload/reload could leave stale module/hook state.

Cleanup must not be performed unsafely under loader lock.

**Priority: later; implement only with a safe outside-loader-lock handoff.**

### Gap G — the init transaction has no generation model

`g_xefg_transaction` is one global transaction object.

The proven games appear to serialize the relevant init sequence, but repeated or overlapping initialization is not explicitly generation-tagged.

For later lifecycle diagnostics it is useful to distinguish:

```text
XeFG init generation 1
XeFG init generation 2
...
```

This becomes especially useful when correlating public proxy replacement, internal swapchain replacement, and queue changes.

**Priority: medium. Add only where it reduces ambiguity; do not build a large state machine preemptively.**

### Gap H — hook monitor is not transition/minimize aware

Normal XeFG Present/Present1 now keeps the monitor alive, but a long minimize/restore or recreation transition may legitimately stop presentation long enough to cross the generic 5+1 second recovery threshold.

No P2.2 evidence currently proves this is a problem.

Do not change monitor timing globally without a reproducing log.

**Priority: evidence-driven after Alt+Tab/minimize testing.**

### Gap I — public XeFG proxy is diagnostic-only

`xefgSwapChainD3D12GetSwapChainPtr` currently logs the public proxy but does not track it as a lifecycle generation.

The original architecture plan suggested using the public proxy for lifecycle awareness while rendering only to the internal presentation swapchain.

That remains a useful option if later logs show a lifecycle boundary that is not visible through `InitFromSwapChainDesc` / internal factory creation.

Do not render REFramework on the public proxy as part of P3.

**Priority: conditional.**

---

## 7. P3 Design Invariants

Every P3 PR must preserve these invariants.

### 7.1 Rendering target

```text
REFramework renders only through the validated XeFG internal presentation swapchain.
```

Do not move rendering to the public XeFG interpolation proxy.

### 7.2 Queue authority

For the confirmed Intel relation:

```text
relation = distinct_same_device
```

REFramework must continue rendering through the actual presentation-creation queue, not the outer XeFG init queue.

### 7.3 Renderer reset ordering

Whenever an active render binding is being replaced:

```text
release REFramework renderer/backbuffer resources first
then detach/replace the active presentation binding
```

### 7.4 Hook/object lifetime ordering

For a hooked XeFG COM object:

```text
strong COM reference remains alive
    while VtableHook is installed
strong COM reference remains alive
    while VtableHook is being removed
release COM reference only after hook removal
```

### 7.5 Lifecycle mutex

All active binding replacement must remain serialized through:

```cpp
g_framework->get_hook_monitor_mutex()
```

This is the same mutex used by Present/Present1 and hook-monitor replacement.

Do not add a second independent active-binding lock that can deadlock with it.

### 7.6 Exact original calls

Present/resize detours must continue to forward the original call and arguments unchanged unless a specific work order says otherwise.

### 7.7 No hardware claims from Codex

Codex performs:

```text
code changes
Release build
static audit
PR/artifact creation
```

Codex does not launch DD2/MHW and does not decide runtime success.

The user performs real-hardware testing and supplies logs. ChatGPT analyzes those logs and decides the next work order.

---

## 8. PR Breakdown

The PRs below are deliberately small. Later work orders must be generated only after the previous PR's code review and, where relevant, user runtime logs.

### P3.1 — XeFG Binding Ownership and Identity Foundation

**Goal:** make the current binding representation safe enough to support a later replacement, without enabling replacement yet.

Required changes:

```text
PendingXefgBinding holds strong COM references
D3D12Hook strongly owns the active XeFG swapchain/queue/device while hooked
old hook is removed before old COM ownership is released
binding identity includes swapchain + queue + source + observe/render mode
exactly identical binding is a no-op
ANY changed active XeFG binding is explicitly deferred and classified
```

Important behavior:

```text
P3.1 does NOT enable XeFG swapchain replacement yet.
```

Instead of only deferring `swapchain_changed`, it must also defer:

```text
queue_changed
mode_changed
multiple_fields_changed
```

This closes the unsafe same-swapchain/new-queue hole before P3.2.

Expected size:

```text
~100-220 LOC
```

Primary files:

```text
src/D3D12Hook.cpp
src/D3D12Hook.hpp
```

The first work order is:

```text
doc/work-order/XEFG_SPECIALK_REMOVAL_P3_1_BINDING_OWNERSHIP_IDENTITY.md
```

### P3.2 — Atomic XeFG Binding Replacement

**Goal:** replace the P2/P3.1 defer gate with a safe rebind transaction.

Expected behavior:

```text
new validated XeFG binding arrives
    -> compare binding identity
    -> identical: no-op
    -> changed: classify reason
    -> reset active REFramework renderer if old binding rendered
    -> safely detach old hook while old object is strongly owned
    -> bind new swapchain/queue/mode
    -> increment/log binding generation
    -> next Present/Present1 recreates renderer
```

Special case to consider:

```text
same swapchain + changed queue/mode
```

Do not blindly construct a second `VtableHook` on the same already-hooked object, because that would copy a vtable that already contains REFramework detours and risks recursion.

P3.2 should either:

- update queue/mode without rebuilding the same object's vtable hook, or
- explicitly remove the old same-object hook before installing the replacement.

The final implementation must be chosen after P3.1 lands and current code is re-read.

Failure behavior must be explicit; do not silently leave half-committed state.

Expected size:

```text
~120-250 LOC
```

### P3.3 — Resize / Fullscreen Transition Normalization

**Goal:** use P3.2 runtime logs to determine whether multiple reset callbacks or transition ordering need hardening.

Known current observation:

```text
ResizeTarget -> Reset!
followed by
ResizeBuffers1 -> Reset!
```

This is currently successful, so P3.3 must not invent a redesign without evidence.

Potential work only if justified:

```text
transition generation logging
coalescing duplicate renderer reset work
ensuring one mod/device reset semantic per actual transition
preserving pre-ResizeBuffers1 backbuffer release
successful renderer recreation after each completed transition
```

Expected size:

```text
prefer <250 LOC
```

If P3.2 logs prove the existing reset behavior is already harmless and stable across required tests, P3.3 may become diagnostics-only or be skipped.

### P3.4 — Alt+Tab / Minimize / Hook-Monitor Robustness

**Goal:** prevent legitimate presentation pauses from being misclassified as a broken D3D12 hook, but only if runtime logs reproduce the problem.

User tests should intentionally include:

```text
Alt+Tab away and back
minimize for >6 seconds and restore
window/fullscreen transitions
multiple repeated cycles
```

Only if logs show `Last chance -> rehook` during a healthy XeFG binding should P3.4 add a narrowly-scoped transition/minimize policy.

Do not globally increase the hook-monitor timeout as a blind fix.

Prefer state-based evidence over longer arbitrary timers.

Expected size:

```text
prefer <200 LOC
```

### P3.5 — XeFG Module / Context / Teardown Lifecycle and Final Diagnostics

**Goal:** close any remaining process-lifecycle state that real testing proves reachable.

Review targets:

```text
repeated XeFG init contexts/generations
public proxy generation correlation
libxess_fg.dll unload/reload
stale global FunctionHook state
pending transaction/binding cleanup
D3D12Hook teardown ownership release
```

Any module-unload cleanup must respect loader-lock safety. Do not destroy/patch function hooks directly from an unsafe loader callback.

If the tested games never unload/reload XeFG and no stale state is observed, keep this PR minimal.

Expected size:

```text
prefer <250 LOC
```

---

## 9. Runtime Validation Cadence

The implementation agent must not run the hardware tests.

After each behavior-changing P3 PR:

```text
Codex builds artifact
        ->
user tests on real Intel hardware
        ->
user provides REFramework + OptiScaler logs
        ->
ChatGPT correlates lifecycle
        ->
next work order is written only from current evidence
```

### Minimum DD2 scenarios

After P3.2 or any later lifecycle behavior change, user testing should include:

```text
1. fresh game launch with XeFG enabled
2. verify REFramework overlay
3. verify OptiScaler overlay
4. play/render for a short period
5. change resolution at least twice
6. switch supported fullscreen/window/borderless mode where practical
7. Alt+Tab out and back several times
8. minimize long enough to cross the hook monitor's existing ~6 second recovery window, then restore
9. repeat a graphics-setting change known to cause ResizeTarget/ResizeBuffers1
10. exit the game normally
```

### MHW sequence

Monster Hunter Wilds should be tested after DD2 proves the corresponding P3 step.

MHW should be treated as a second lifecycle workload, not mixed with the separate NVIDIA release-storm investigation.

---

## 10. Required Diagnostic Signals for P3

Keep diagnostics bounded and state-change-oriented.

### Binding identity / defer

P3.1 should produce a machine-readable log such as:

```text
[XeFG][BindingGate]
action = unchanged | defer
reason = identical | swapchain_changed | queue_changed | mode_changed | multiple_fields_changed
old_swapchain = ...
new_swapchain = ...
old_queue = ...
new_queue = ...
old_observe_only = ...
new_observe_only = ...
```

### Rebind

P3.2 should later produce:

```text
[XeFG][Rebind]
stage = begin | old_renderer_reset | old_hook_removed | new_binding_committed | failed
reason = ...
generation = ...
old_swapchain = ...
new_swapchain = ...
old_queue = ...
new_queue = ...
```

Do not print these every Present frame.

### Existing logs to preserve

```text
[XeFG][InitDesc]
[XeFG][InternalSwapchain]
[XeFG][QueueIdentity]
[XeFG][Bind]
[XeFG][P2.1Probe]
[D3D12][ExternalBind]
[D3D12][PresentEntry]
[XeFG][ResizeBuffers1]
[D3D12][HookMonitor]
```

P3 should not remove the evidence that made P2/P2.1/P2.2 diagnosable until the entire Special-K-removal track is complete.

---

## 11. Final P3 Acceptance Criteria

P3 is complete only when user hardware validation supports all of the following for the target XeFG topology:

```text
DD2 launches normally
MHW launches normally
XeFG remains active
OptiScaler overlay works
REFramework overlay works
REFramework D3D12 renderer initializes and reinitializes after transitions
actual presentation queue remains authoritative
ResizeBuffers succeeds
ResizeBuffers1 succeeds
ResizeTarget path does not permanently break rendering
resolution changes recover
fullscreen/window-mode transitions recover
Alt+Tab recovers
minimize/restore does not permanently lose the overlay
validated internal swapchain replacement rebinds correctly when it occurs
validated presentation queue replacement rebinds correctly when it occurs
no stale old swapchain hook remains active
no recurring hook-monitor recovery loop during a healthy session
no DXGI_ERROR_DEVICE_REMOVED regression
no DXGI_ERROR_ACCESS_DENIED regression
no DXGI_ERROR_INVALID_CALL regression caused by REFramework-held backbuffers
normal game exit does not expose a XeFG hook teardown crash
```

Final REFramework integration smoke test must also include existing scripting functionality, for example a minimal Lua callback using:

```lua
re.on_frame(function()
    -- increment/log a bounded counter for validation
end)
```

The purpose is not to add a new feature. It is to prove the normal REFramework scripting/frame callback path still operates with XeFG after lifecycle hardening.

Only after P3 is accepted should Resident Evil Requiem be used for the secondary periodic-stutter validation track.

---

## 12. PR Discipline

P3 intentionally favors more PRs over large PRs.

Rules:

```text
one lifecycle responsibility per PR
prefer <250 LOC of functional change
avoid >300 LOC unless the exact task cannot reasonably be split
no unrelated cleanup/refactor
no native/no-FG test expansion
no P3.N+1 behavior hidden inside P3.N
build/static validation by Codex
runtime validation by user
logs analyzed before the next behavior-changing work order
```

A smaller PR that leaves a later gap explicitly deferred is preferred over a large PR that is difficult to review and difficult to isolate on hardware.
