# Work Order P1 — XeFG / D3D12 Diagnostic Instrumentation

Date: 2026-09-05
Repository: `onehoon/REFramework`
Target branch: `master`
Baseline master commit: `9f1c1d65155a29b8aaf88e21be8e2a104db6b5f4`
Source baseline: upstream-equivalent code at `b6baf6b406efc65e077b99cb4d9ad25b0a0a9095`
Parent analysis: `doc/REFramework_OptiScaler_XeFG_SpecialK_Removal_Analysis_Plan_2026-09-05.md`

## 1. Purpose

This is the first implementation work order for removing the Special K requirement from the REFramework + OptiScaler + Intel XeFG stack.

The target runtime topology is:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (this fork)
Special K    = absent
FG Output    = XeFG
```

The immediate goal of P1 is **not** to fix the overlay yet.

P1 must add enough low-risk diagnostic instrumentation to REFramework to prove, from one DD2 no-Special-K run, exactly which D3D12/DXGI swapchain and Present path REFramework is patching, which swapchain(s) are created while XeFG is active, and why the existing phase-1 Present hook is not reached.

The output of P1 must make the P2 implementation decision evidence-based rather than speculative.

---

## 2. Primary Objective

Add targeted, rate-limited diagnostics that answer all of the following questions:

1. What object and vtable does REFramework use for its dummy phase-1 D3D12 swapchain?
2. Which module owns the function currently found at the dummy swapchain's `Present` slot?
3. Which module owns the equivalent `Present1`, `ResizeBuffers`, and `ResizeBuffers1` entries?
4. Which swapchain objects are returned from `CreateSwapChainForHwnd` before and after XeFG is loaded/initialized?
5. What are the vtable addresses and owner modules for those candidate swapchains?
6. Does `D3D12Hook::present()` ever execute after XeFG becomes active in the failing no-SK topology?
7. If it executes, which swapchain instance called it and was REFramework still in phase 1 or already instance-bound?
8. When the hook monitor emits `Last chance encountered for hooking` / `Sending rehook request for D3D`, what is the exact D3D12 hook state at that moment?
9. Is `libxess_fg.dll` loaded before, during, or after the relevant swapchain creation events?
10. Can the logs distinguish a normal successful native/FSRFG discovery path from the failing XeFG path?

---

## 3. Non-Goals

Do **not** implement the actual XeFG compatibility fix in P1.

Specifically, P1 must not:

- add a `Present1` hook,
- add a `ResizeBuffers1` hook,
- add an external swapchain binding API,
- replace REFramework's current phase-1/phase-2 discovery model,
- bind directly to an XeFG proxy or internal presentation swapchain,
- modify OptiScaler,
- add Special K compatibility code,
- reproduce Special K behavior,
- add Requiem-specific logic,
- change integrity-check behavior,
- change frame pacing or latency behavior,
- alter render-target lifetime beyond existing behavior,
- introduce Intel private-object offset scanning,
- vendor Intel XeFG headers or libraries just for this logging pass.

The failing XeFG overlay behavior is allowed to remain unchanged after P1.

---

## 4. Current Master Code Findings

The work must be implemented against the current fork master, not against an older REFramework snapshot.

### 4.1 Current D3D12 phase-1 discovery

`D3D12Hook::hook()` creates a dummy D3D12 device, command queue, factory, and swapchain, then stores:

```cpp
s_swapchain_vtable = *(void***)target_swapchain;
s_factory_vtable = *(void***)factory;
```

`D3D12Hook::hook_impl()` currently resets the existing hooks and installs a pointer hook only on the dummy/native swapchain `Present` entry:

```cpp
m_present_hook.reset();
m_swapchain_hook.reset();

m_is_phase_1 = true;

auto& present_fn = s_swapchain_vtable[8]; // Present
m_present_hook = std::make_unique<PointerHook>(&present_fn, &D3D12Hook::present);
```

It also installs the existing global factory hook on slot 15 (`CreateSwapChainForHwnd`).

### 4.2 Current phase-2 instance binding

The first accepted call into `D3D12Hook::present()` transitions REFramework to an instance-level `VtableHook`:

```cpp
m_swapchain_hook = std::make_unique<VtableHook>(swap_chain);
m_swapchain_hook->hook_method(8,  (uintptr_t)&D3D12Hook::present);
m_swapchain_hook->hook_method(13, (uintptr_t)&D3D12Hook::resize_buffers);
m_swapchain_hook->hook_method(14, (uintptr_t)&D3D12Hook::resize_target);
m_is_phase_1 = false;
```

There is currently no `Present1` or `ResizeBuffers1` hook path in `D3D12Hook`.

### 4.3 Current frame-generation special handling

`D3D12Hook::create_swapchain()` already has special handling for known swapchain wrappers.

For a Streamline `interposer::DXGISwapChain`, REFramework attempts to locate the internal swapchain and intentionally switches away from the Streamline proxy because hooking the proxy causes the REFramework menu to flicker/render incorrectly.

It also recognizes `FrameInterpolationSwapChain` and marks frame-generation usage.

There is no equivalent Intel XeFG recognition on current master.

### 4.4 Current hook monitor

`REFramework.cpp` currently logs:

```text
Last chance encountered for hooking
Sending rehook request for D3D
```

before requesting a D3D rehook when the renderer is expected to be active but the hook has not entered Present.

These messages currently do not contain enough state to diagnose why the hook was lost or never reached.

### 4.5 Existing DLL-load notification path

`REFramework.cpp` already registers an `LdrRegisterDllNotification` callback.

It currently logs DLL loads and explicitly recognizes `sl.dlss_g.dll`, calling `D3D12Hook::hook_streamline()` when Streamline DLSS-G is detected.

P1 should extend this existing mechanism for **XeFG diagnostics only** rather than inventing a second module watcher.

---

## 5. Files Expected in Scope

Primary files:

```text
src/D3D12Hook.hpp
src/D3D12Hook.cpp
src/REFramework.cpp
```

A small reusable utility file may be touched only if it is clearly cleaner than keeping a local helper in `D3D12Hook.cpp`.

Do not perform unrelated refactors.

Do not reorganize the graphics-hook architecture in P1.

---

## 6. Required Instrumentation

### 6.1 Safe address-to-module resolver

Add a small helper capable of taking an arbitrary executable/function address and returning a best-effort module identity.

Required output where available:

```text
module base
module filename
full module path (optional if already easy to obtain)
```

Recommended Win32 mechanisms include either:

```text
GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, ...)
GetModuleFileNameW(...)
```

or a `VirtualQuery`-based equivalent.

Requirements:

- must be safe when passed null,
- must fail gracefully for non-module/JIT/unknown addresses,
- must not change module lifetime/refcounts,
- must not throw out of the logging path,
- must not dereference arbitrary unvalidated memory,
- diagnostic failure must never affect game behavior.

Suggested output format:

```text
0x00007FF... [dxgi.dll]
0x00007FF... [dxgi.dll | C:\Windows\System32\dxgi.dll]
0x00007FF... [unknown]
```

Keep this helper lightweight. P1 does not need a general-purpose module-inspection subsystem.

---

### 6.2 Dummy swapchain / phase-1 discovery snapshot

Immediately after the dummy swapchain and vtable pointers are known in `D3D12Hook::hook()`, emit one structured diagnostic block.

Log at least:

```text
[D3D12][Discovery]
dummy_swapchain          = 0x...
dummy_vtable             = 0x...
factory                   = 0x...
factory_vtable            = 0x...
Present[8]                = 0x...
Present[8].owner          = ...
ResizeBuffers[13]         = 0x...
ResizeBuffers[13].owner   = ...
ResizeTarget[14]          = 0x...
ResizeTarget[14].owner    = ...
Present1[22]              = 0x...
Present1[22].owner        = ...
ResizeBuffers1[39]        = 0x...
ResizeBuffers1[39].owner  = ...
CreateSwapChainForHwnd[15]= 0x...
CreateSC.owner            = ...
command_queue             = 0x...
command_queue_offset      = 0x...
```

Notes:

- `Present1[22]` and `ResizeBuffers1[39]` are **diagnostic reads only** in P1.
- Before reading these slots, confirm the dummy object was successfully queried to a sufficiently new DXGI swapchain interface (`IDXGISwapChain1/3/4` as appropriate) rather than blindly assuming a vtable length.
- If the interface cannot be validated, log `unavailable` rather than reading beyond an interface contract.
- Do not hook slots 22 or 39 in P1.

The purpose is to establish which module owns the native dummy targets that phase 1 currently patches.

---

### 6.3 Existing phase-1 hook installation snapshot

When `hook_impl()` installs `m_present_hook`, log one concise line containing:

```text
[D3D12][HookInstall]
phase                    = phase1
slot                     = Present[8]
target                   = 0x...
target_owner             = ...
destination              = D3D12Hook::present
```

When the code transitions to the per-instance `VtableHook`, log:

```text
[D3D12][HookInstall]
phase                    = instance
swapchain                = 0x...
vtable                   = 0x...
Present[8].original      = 0x...
Present[8].owner         = ...
ResizeBuffers[13]        = 0x...
ResizeTarget[14]         = 0x...
```

This must be emitted once per bind/rebind, not once per frame.

---

### 6.4 `CreateSwapChainForHwnd` candidate snapshot

Extend the existing `D3D12Hook::create_swapchain()` diagnostics.

For every successful returned swapchain that is relevant to the game window, capture a concise candidate snapshot before any existing Streamline/internal-swapchain substitution changes the returned pointer.

Log at least:

```text
[D3D12][SwapchainCandidate]
sequence                 = N
swapchain                = 0x...
vtable                   = 0x...
factory                   = 0x...
device_or_queue_arg       = 0x...
hwnd                      = 0x...
width                     = ...
height                    = ...
format                    = ...
buffer_count              = ...
swap_effect               = ...
flags                     = ...
type_name                 = ... / unknown
Present[8]                = 0x...
Present[8].owner          = ...
ResizeBuffers[13]         = 0x...
ResizeBuffers[13].owner   = ...
Present1[22]              = 0x... / unavailable
Present1[22].owner        = ... / unavailable
ResizeBuffers1[39]        = 0x... / unavailable
ResizeBuffers1[39].owner  = ... / unavailable
xefg_module_loaded        = true/false
```

Also log the result of existing wrapper classification:

```text
classification = native
classification = streamline_interposer
classification = frame_interpolation_swapchain
classification = unknown_wrapper
```

Do not introduce XeFG object-name heuristics unless the runtime type name itself is directly available from the existing RTTI helper.

Do not add pattern scans for Intel objects in P1.

---

### 6.5 XeFG module-load detection

Extend the existing `ldr_notification_callback` in `REFramework.cpp`.

Recognize at minimum:

```text
libxess_fg.dll
```

Optionally recognize `libxess.dll` as contextual information, but the key signal is `libxess_fg.dll`.

When `libxess_fg.dll` loads, log:

```text
[XeFG][Module]
name                     = libxess_fg.dll
base                     = 0x...
full_path                = ...
```

Maintain a small thread-safe/process-lifetime diagnostic state that can answer:

```text
Is XeFG module currently known to be loaded?
When was it first observed relative to REFramework startup?
```

For P1, this state is for logging/classification only.

Do **not** install hooks into XeFG exports in this work order.

However, it is useful to passively call `GetProcAddress` and log whether the following exports are present, without calling them or detouring them:

```text
xefgSwapChainD3D12InitFromSwapChain
xefgSwapChainD3D12InitFromSwapChainDesc
xefgSwapChainD3D12GetSwapChainPtr
```

Example:

```text
[XeFG][Exports]
InitFromSwapChain        = 0x... / missing
InitFromSwapChainDesc    = 0x... / missing
GetSwapChainPtr          = 0x... / missing
```

This will help P2 choose the cleanest interception point without making P1 behaviorally invasive.

---

### 6.6 Existing `D3D12Hook::present()` entry diagnostics

Instrument the existing `D3D12Hook::present()` callback only.

The diagnostic must allow us to prove whether the callback is reached under no-SK XeFG.

For the first few accepted calls, log:

```text
[D3D12][PresentEntry]
call                     = N
phase                    = phase1 / instance
swapchain                = 0x...
vtable                   = 0x...
hwnd                      = 0x...
tracked_swapchain        = 0x...
original_present         = 0x...
original_owner           = ...
thread_id                = ...
xefg_module_loaded       = true/false
```

Rate limiting is mandatory.

Recommended policy:

- log the first 8–10 calls,
- thereafter log only when one of these changes:
  - swapchain pointer,
  - phase,
  - original Present target,
  - function owner module,
  - XeFG-loaded state.

Do not produce one log block every rendered frame.

A separate monotonically increasing Present-entry counter may be retained for hook-monitor diagnostics.

---

### 6.7 Phase transition diagnostics

Immediately before phase 1 transitions to the instance hook, emit:

```text
[D3D12][PhaseTransition]
phase1 -> instance
swapchain                = 0x...
vtable                   = 0x...
xefg_module_loaded       = true/false
```

If the no-SK XeFG run never prints this line, that is an important expected P1 result.

Do not otherwise change the transition logic.

---

### 6.8 Hook-monitor state snapshot

Enhance the existing hook-monitor logging in `REFramework.cpp`.

When printing:

```text
Last chance encountered for hooking
```

and again when printing:

```text
Sending rehook request for D3D
```

also print a structured D3D12 diagnostic snapshot.

The hook monitor needs read-only accessors from `D3D12Hook` where necessary.

At minimum include:

```text
[D3D12][HookMonitor]
event                    = last_chance / rehook_request
is_hooked                = true/false
is_phase_1               = true/false
inside_present           = true/false
active_swapchain         = 0x...
active_device            = 0x...
active_command_queue     = 0x...
present_entry_count      = ...
xefg_module_loaded       = true/false
```

If straightforward without spreading timing state across unrelated classes, also include:

```text
last_present_entry_age_ms = ...
```

Do not add complex synchronization solely to obtain this timestamp.

The diagnostic accessor path must not mutate hook state.

---

### 6.9 Unhook / rehook reason visibility

The current logs already print `Unhooking D3D12` and `Hooking D3D12`.

For P1, add enough context to distinguish:

```text
initial hook
hook-monitor recovery
swapchain reset/recreate path
other existing path
```

Do this with a minimal reason enum/tag or call-site diagnostic if practical.

Do not redesign hook ownership.

If introducing a reason enum would unnecessarily spread through the code, it is acceptable to place the reason only on the hook-monitor request path and retain the existing generic logs elsewhere.

---

## 7. Logging Quality Requirements

P1 is a diagnostic change, but it must still be safe to run in real games.

### Required properties

- deterministic prefixes (`[D3D12][...]`, `[XeFG][...]`),
- pointer values formatted consistently,
- module ownership included where useful,
- no per-frame unbounded spam,
- no stacktrace on every Present,
- no expensive module lookup every frame after the first stable state,
- no exceptions escaping diagnostics,
- no crashes if a swapchain query or RTTI lookup fails,
- no COM reference leaks from diagnostic `QueryInterface` calls,
- no permanent module reference increments from owner resolution,
- no global lock added to the hot Present path unless already held by existing logic.

### Log level

Use the existing REFramework logging conventions.

The high-value event lines required for this investigation should be visible in the normal framework log used for the DD2 test. Extremely verbose stable-state details may use debug level where appropriate, but do not make the required evidence dependent on a logging configuration the current test environment does not normally enable.

---

## 8. No Behavior Change Requirement

P1 must preserve current behavior.

The following existing semantics must remain unchanged:

- phase-1 still hooks `Present[8]` only,
- first valid Present still performs the same phase-2 transition,
- current Streamline special handling stays intact,
- `FrameInterpolationSwapChain` handling stays intact,
- current resize callbacks stay intact,
- current command queue discovery stays intact,
- current hook-monitor thresholds stay unchanged,
- D3D11 behavior is untouched,
- VR behavior is untouched.

Do not attempt to make the REFramework menu appear under XeFG in this PR.

If the menu happens to change behavior because instrumentation changes timing, treat that as a warning and investigate; do not accept timing-dependent accidental success as completion.

---

## 9. Validation Matrix

### Test A — Required failing reproducer

```text
Game         = Dragon's Dogma 2
OptiScaler   = dxgi.dll
REFramework  = forked dinput8.dll
Special K    = absent
FG Output    = XeFG
```

Expected functional state for P1:

- game launches,
- OptiScaler functions,
- XeFG functions,
- OptiScaler overlay continues to work,
- REFramework core/game hooks remain alive,
- REFramework overlay may remain unavailable,
- no new crash/hang introduced.

Required captured evidence:

1. `[XeFG][Module]` entry for `libxess_fg.dll`.
2. XeFG export-presence snapshot.
3. dummy phase-1 vtable/owner snapshot.
4. all relevant `CreateSwapChainForHwnd` candidate snapshots around XeFG activation.
5. clear evidence whether `D3D12Hook::present()` is entered at all.
6. if not entered, `present_entry_count = 0` in the hook-monitor snapshot.
7. hook-monitor state printed at `last_chance` and `rehook_request`.
8. no unbounded frame-by-frame logging.

### Test B — Native/no-FG control

Use the same game and REFramework build with XeFG disabled / normal non-FG presentation where practical.

Expected evidence:

```text
PresentEntry occurs
phase1 -> instance transition occurs
active swapchain becomes non-null
hook monitor does not repeatedly request recovery
```

This provides the control sample needed to compare the successful discovery topology to XeFG.

### Test C — Existing supported FG control (preferred if readily available)

If DD2 can be tested with the previously working FSRFG path, capture the same diagnostic sequence.

This is preferred because the original problem statement already establishes that REFramework overlay works with FSRFG while failing with XeFG.

Do not block P1 completion if this control is not practical on the test machine, provided Test A and Test B are complete.

---

## 10. Required P1 Evidence Summary

At the end of the implementation/testing work, the developer must provide a short evidence summary answering these exact questions from the generated log:

### Q1
Which module owns the dummy swapchain `Present[8]` target that REFramework patches during phase 1?

### Q2
Which swapchain candidate appears to be used by XeFG/OptiScaler for the active presentation path?

### Q3
What modules own `Present[8]` and `Present1[22]` on that candidate?

### Q4
Does current `D3D12Hook::present()` execute after XeFG becomes active with no Special K?

### Q5
Does REFramework ever reach `phase1 -> instance` in that failing run?

### Q6
At the exact hook-monitor recovery point, what are:

```text
is_phase_1
inside_present
active_swapchain
present_entry_count
xefg_module_loaded
```

### Q7
How does the successful native/FSRFG control differ from the XeFG failure?

P2 must not begin until these questions can be answered from logs rather than inference.

---

## 11. Build and Regression Requirements

Before marking P1 complete:

1. Build the same targets/configurations used by the repository's normal Windows development workflow.
2. No new compiler errors.
3. No new warnings introduced by the diagnostic code where reasonably enforceable.
4. Run existing automated tests available for the touched codebase/workflow.
5. Confirm D3D11 code path was not modified.
6. Confirm existing Streamline/DLSS-G handling still compiles.
7. Confirm no new third-party runtime dependency was added.
8. Confirm no Intel XeFG redistributable/header dependency was added merely for diagnostics.

If full local build/test execution is not possible, report exactly what was and was not run. Do not claim verification that did not occur.

---

## 12. Implementation Constraints

- Keep the diff focused on P1 diagnostics.
- Prefer local/private helpers over premature public architecture.
- Do not build P2 APIs speculatively unless a tiny read-only accessor is required by hook-monitor logging.
- Any new public getter in `D3D12Hook` must be read-only and minimal.
- Use atomics for counters/booleans only where needed; do not introduce a new global mutex just for diagnostics.
- Respect the existing `g_framework->get_hook_monitor_mutex()` ownership in the Present/hook paths; do not create reverse lock ordering.
- Avoid calling heavy Win32/module-resolution work on every frame.
- Cache stable owner-module strings/addresses where practical.
- Maintain current COM ownership semantics.

---

## 13. Suggested Minimal Internal Diagnostic State

This is guidance, not a required exact API.

A small amount of state is sufficient:

```text
atomic<uint64_t> present_entry_count
atomic<bool> xefg_module_loaded
first XeFG module timestamp / relative startup time
last observed swapchain pointer
last logged Present target
last logged phase
```

If `last_present_entry_age_ms` is implemented, a steady-clock timestamp can be updated on Present entry.

Do not turn P1 into a generalized telemetry framework.

---

## 14. Deliverables

P1 is complete only when all of these exist:

1. Source changes implementing the required diagnostics.
2. Successful build verification or an explicit statement of unavailable build tooling.
3. A DD2 no-SK XeFG runtime log using the P1 build.
4. A native/no-FG or FSRFG control log using the same P1 build.
5. A concise evidence summary answering Q1–Q7 above.
6. No functional XeFG fix mixed into the P1 diff.

The logs themselves do not need to be committed to the repository unless specifically requested later.

---

## 15. Completion Gate for P2

Do not proceed directly from implementation to a guessed XeFG hook.

P2 may begin only after the P1 logs establish the actual runtime topology.

The expected current hypothesis is:

```text
REFramework phase-1 dummy/native Present[8]
        ↓
not reached by active XeFG presentation path

XeFG / OptiScaler active swapchain
        ↓
Present and/or Present1 path
        ↓
OptiScaler LocalPresent
        ↓
real output
```

P1 must either confirm this hypothesis or replace it with a better one backed by captured addresses, owner modules, object identities, and phase-transition evidence.

The next work order will then design the smallest correct XeFG-aware binding strategy from those facts.

---

## 16. Scope Discipline

The success criterion of this work order is **diagnostic certainty**, not feature completion.

Do not optimize for making the menu visible by accident.

Do not add Special K.

Do not modify OptiScaler.

Do not test Requiem as the primary P1 target.

Dragon's Dogma 2 is the primary reproducer because the current working/no-SK logs already establish the baseline behavior and isolate the swapchain compatibility problem without introducing Requiem's integrity/anti-tamper behavior as an additional variable.

After Special K removal is technically complete in later phases, Resident Evil Requiem will be used as the secondary validation target for the periodic stutter hypothesis.