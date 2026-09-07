# Work Order: XeFG MHW Crash PR-A — Runtime Lifecycle Observability

Date: 2026-09-07  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `19c1a15b4d15d6cea91395bb3beb49523943d5a6` (`fix: bootstrap XeFG debug log before startup hooks`, PR #33 merged)

This work order is the first PR in the follow-up MHW/XeFG crash investigation after the R1-R11 refactor series.

The purpose of **PR-A is observability only**.

> Add enough exact XeFG runtime lifecycle identity and destroy/re-init diagnostics to determine whether REFramework is holding or tracking an old XeFG proxy swapchain/context across an OptiScaler/XeFG lifecycle transition.

PR-A must **not** change the current active-binding ownership semantics, must **not** convert the active XeFG swapchain `ComPtr` to a borrowed/raw pointer, and must **not** add timeout-driven recovery behavior. Those changes belong to later PRs only after this instrumentation confirms the lifecycle sequence.

---

# 1. Investigation Background

The current crash investigation is specifically about:

```text
Monster Hunter Wilds
+ REFramework fork
+ OptiScaler
+ Intel XeFG output
```

Observed runtime behavior from multiple tests:

1. XeFG initializes normally.
2. REFramework successfully binds to the XeFG internal/proxy swapchain.
3. Normal ResizeTarget / ResizeBuffers / ResizeBuffers1 events can complete successfully many times.
4. The game can run for a long period, including real gameplay.
5. Eventually REFramework stops receiving Present/Present1 entries from the bound XeFG swapchain.
6. Hook monitor repeatedly reports:

```text
[XeFG][HookMonitor] action = preserve_binding,
reason = present_timeout,
generation = 1
```

7. In the earlier captured Intel crash, a later lifecycle transition reached:

```text
OptiScaler
-> Intel XeFG driver path (igxess_fg.dll)
-> MHW D3D12Core.dll
-> D3D12CoreCreateLayeredDevice
-> D3D12.dll
-> C0000005 / NULL dereference
```

8. Removing OptiScaler's aggressive backbuffer `GetBuffer -> Release until refcount <= limit` blocks fixed the separate NVIDIA startup crash, but **did not eliminate the Intel long-session crash**.

Therefore the Intel issue must not be reduced to that OptiScaler resize-release block.

---

# 2. Current Source-Level Risk That PR-A Must Observe

Current `XeFGBinding` strongly owns:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain{};
Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue{};
Microsoft::WRL::ComPtr<ID3D12Device4> m_device{};
```

and keeps that state until explicit binding replacement or clear/unhook.

OptiScaler, meanwhile, has XeFG swapchain/context lifecycle logic that can:

- preserve an FG swapchain based on COM reference-count behavior;
- reuse a current FG swapchain for the same HWND;
- release/destroy a XeFG swapchain context;
- recreate a XeFG swapchain/context for the same game window.

The working hypothesis for this investigation is:

> REFramework may continue to strongly own and/or logically preserve an old XeFG proxy swapchain while OptiScaler/XeFG is attempting to release, destroy, preserve, reuse, or recreate that runtime context.

This is **not yet proven**.

PR-A exists to make the lifecycle observable before changing ownership semantics.

---

# 3. Recommended PR Identity

Suggested branch:

```text
investigate/xefg-mhw-lifecycle-observability
```

Suggested PR title:

```text
XeFG PR-A: add runtime lifecycle observability for MHW crash investigation
```

Suggested commit title:

```text
debug: trace XeFG runtime destroy and re-init lifecycle
```

---

# 4. Scope

PR-A has four required responsibilities.

## A1. Track XeFG runtime identity in the active binding

Extend the semantic metadata associated with the active XeFG binding so diagnostics can answer:

```text
Which runtime slot created this binding?
Which XeFG context created this binding?
Which HWND was associated with the init transaction?
Which binding generation was active when a destroy/re-init happened?
```

Minimum metadata:

```cpp
void* runtime_context{};     // borrowed identity only
HWND hwnd{};
size_t runtime_slot{};
```

Use a sentinel / optional representation for `runtime_slot` when no XeFG runtime is associated.

Important:

- `runtime_context` is an identity token only in PR-A.
- Do not dereference it outside the intercepted runtime call where its validity is known.
- Do not AddRef/Release it.
- Do not use it to trigger detach/recovery in this PR.

The metadata must be propagated from the exact `InitFromSwapChainDesc` dispatch path into the accepted `XeFGBindingCandidate`, then into the active `XeFGBinding`.

Suggested candidate extension:

```cpp
struct XeFGBindingCandidate {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> selected_queue{};
    Microsoft::WRL::ComPtr<ID3D12Device4> device{};

    void* runtime_context{};
    HWND hwnd{};
    size_t runtime_slot{kInvalidRuntimeSlot};

    XeFGQueueRelation relation{XeFGQueueRelation::InitQueueUnavailable};
    bool observe_only{true};
};
```

Do not change the existing strong ownership fields in PR-A.

## A2. Add exact-runtime `xefgSwapChainDestroy` interception

`XeFGRuntimeRegistry` currently registers exact runtime hooks for:

```text
xefgSwapChainD3D12InitFromSwapChainDesc
xefgSwapChainD3D12GetSwapChainPtr
```

Add optional interception for:

```text
xefgSwapChainDestroy
```

The hook must follow the existing exact-HMODULE, fixed-slot runtime-registry architecture.

Do not add a global `GetProcAddress` guess, process-wide export patch, or path-name heuristic.

Suggested registry shape:

```cpp
using DestroyFn = int32_t (WINAPI*)(void* context);

struct DestroyDispatchTarget {
    HMODULE module{};
    DestroyFn original{};
};
```

Each `RuntimeHook` should optionally contain:

```cpp
FARPROC destroy_export{};
std::unique_ptr<FunctionHook> destroy_hook{};
```

Add a fixed thunk table exactly like the existing InitDesc/GetSwapChain thunk arrays.

If the export is missing:

- runtime installation must still succeed;
- log `destroy = missing` in debug diagnostics;
- do not reject the runtime.

PR-A must observe Destroy but **must not prevent, retry, delay, detach, clear, unhook, or otherwise alter the Destroy call**.

Required call order:

```text
REF diagnostic pre-log
-> original xefgSwapChainDestroy(context)
-> REF diagnostic post-log with result
-> return original result unchanged
```

No lifecycle mutex may be held across the original Intel/XeFG Destroy call unless the current architecture already requires it for the same export path. Prefer observing without introducing a new lock inversion surface.

## A3. Log pre-init state before calling original `InitFromSwapChainDesc`

The current discovery path observes the init transaction and makes the binding decision **after** the original `InitFromSwapChainDesc` returns.

For this investigation, add a diagnostic snapshot immediately before the original runtime init call.

This is critical because the prior crash occurred inside the runtime/device creation path. If the process crashes during original Init, post-init candidate logs never execute.

Required debug log fields:

```text
slot
module
context
hwnd
active_binding_present
active_generation
active_context
active_hwnd
active_runtime_slot
active_swapchain
active_queue
active_device
active_observe_only
context_match
hwnd_match
runtime_slot_match
last_resize_event_id
last_resize_kind
last_present_age_ms
```

Suggested tag:

```text
[XeFG][RuntimeLifecycle] stage = pre_init
```

Example:

```cpp
spdlog::info(
    "[XeFG][RuntimeLifecycle] stage = pre_init, slot = {}, context = 0x{:x}, hwnd = 0x{:x}, "
    "active = {}, generation = {}, active_context = 0x{:x}, active_hwnd = 0x{:x}, "
    "active_runtime_slot = {}, active_swapchain = 0x{:x}, context_match = {}, hwnd_match = {}, slot_match = {}",
    slot,
    reinterpret_cast<uintptr_t>(context),
    reinterpret_cast<uintptr_t>(hwnd),
    ...);
```

This log must be gated by the existing persistent XeFG Debug Log setting added/fixed through PR #33.

Do not add a second debug option.

## A4. Log Destroy against the currently active REF binding

Before calling original Destroy, record whether the runtime context being destroyed matches the active REF XeFG binding metadata.

Required fields:

```text
stage = destroy_enter
slot
module
context
active_binding_present
active_generation
active_context
active_hwnd
active_runtime_slot
active_swapchain
active_queue
active_device
active_observe_only
context_match
runtime_slot_match
last_present_age_ms
last_resize_event_id
last_resize_kind
```

After original Destroy returns:

```text
stage = destroy_return
slot
context
result
active_generation
active_context
active_swapchain
context_match
```

Important:

> The post-Destroy log may only print cached pointer values/identity metadata. It must not dereference the destroyed runtime context or perform COM calls on a proxy object whose lifetime may have changed.

---

# 5. Required Data-Flow Changes

The minimum data flow should become:

```text
XeFGRuntimeRegistry
    slot + module
        |
        v
XeFGCompatibility::dispatch_init_desc(slot, context, hwnd, ...)
        |
        | preserve slot/context/HWND as observation metadata
        v
XeFGDiscovery::Observation
        |
        v
XeFGDiscovery::build_binding_candidate()
        |
        v
XeFGBindingCandidate
    + runtime_context
    + hwnd
    + runtime_slot
        |
        v
XeFGCandidateHandoff
        |
        v
D3D12Hook::bind_external_swapchain / replace_xefg_binding
        |
        v
XeFGBinding
    existing strong COM ownership unchanged
    + runtime identity metadata
```

Destroy flow:

```text
xefgSwapChainDestroy export
        |
        v
exact runtime-slot destroy thunk
        |
        v
XeFGCompatibility::dispatch_destroy(slot, context)
        |
        +--> diagnostic comparison with active XeFGBinding
        |
        v
original destroy(context)
        |
        v
result-only diagnostic
        |
        v
return result unchanged
```

---

# 6. Suggested `XeFGBinding` API Additions

Keep this narrow.

Possible additions:

```cpp
static constexpr size_t kInvalidRuntimeSlot = static_cast<size_t>(-1);

void* runtime_context() const noexcept;
HWND hwnd() const noexcept;
size_t runtime_slot() const noexcept;

bool runtime_identity_matches(
    size_t runtime_slot,
    void* runtime_context) const noexcept;
```

Update commit methods to receive metadata:

```cpp
void commit_initial(
    ComPtr<IDXGISwapChain3> swapchain,
    ComPtr<ID3D12CommandQueue> queue,
    ComPtr<ID3D12Device4> device,
    bool observe_only,
    size_t runtime_slot,
    void* runtime_context,
    HWND hwnd);
```

Equivalent changes may be made through a small metadata struct if cleaner:

```cpp
struct XeFGRuntimeIdentity {
    size_t slot{kInvalidRuntimeSlot};
    void* context{};
    HWND hwnd{};
};
```

Preferred rule:

> Keep runtime identity as simple non-owning metadata. Do not turn it into a general runtime lifecycle controller in PR-A.

`clear()` must clear this metadata together with existing binding state.

---

# 7. `XeFGDiscovery` Requirements

Current `Observation` already tracks:

```text
context
hwnd
init_queue
presentation_queue
factory
internal_swapchain
init_result
```

Add the runtime slot to the observation or inject it into the binding candidate immediately after candidate construction.

Preferred approach:

```cpp
struct Observation {
    size_t runtime_slot{kInvalidRuntimeSlot};
    void* context{};
    HWND hwnd{};
    ...
};
```

Pass `slot` into `observe_init()` so the complete runtime identity travels through one transaction object.

This avoids a hidden side channel between `XeFGCompatibility` and `XeFGDiscovery`.

---

# 8. Runtime Registry Requirements

Modify:

```text
src/compatibility/xefg/XeFGRuntimeRegistry.hpp
src/compatibility/xefg/XeFGRuntimeRegistry.cpp
```

Required changes:

1. Resolve `xefgSwapChainDestroy` from the exact module during installation.
2. Hook it when present.
3. Store its original function through `FunctionHook` exactly like existing runtime exports.
4. Add fixed destroy thunks for slots `0..7`.
5. Add `resolve_destroy(slot)`.
6. Preserve existing install behavior when Destroy is absent.
7. Extend runtime registry debug output to show:

```text
init_desc = present/missing
get_swapchain = present/missing
destroy = present/missing
```

Do not increase runtime capacity in this PR.

Do not refactor the fixed thunk architecture.

---

# 9. Logging Policy

PR #33 fixed early loading of the persistent XeFG Debug Log option.

PR-A must reuse that setting.

Normal log level:

- preserve existing warnings/errors;
- do not flood normal-user logs with lifecycle snapshots.

Debug Log enabled:

Required new tags:

```text
[XeFG][RuntimeLifecycle] stage = pre_init
[XeFG][RuntimeLifecycle] stage = init_return
[XeFG][RuntimeLifecycle] stage = destroy_enter
[XeFG][RuntimeLifecycle] stage = destroy_return
```

`init_return` should include the runtime result and, if available, the candidate/binding decision identity.

Do not log every Present.

Do not change the current first-N / identity-change Present logging policy.

---

# 10. Explicit Non-Goals

PR-A must NOT do any of the following:

- remove `ComPtr<IDXGISwapChain3>` from `XeFGBinding`;
- convert the XeFG proxy swapchain to borrowed ownership;
- add explicit detach before Destroy;
- add explicit detach before re-init;
- call `g_framework->on_reset()` from Destroy instrumentation;
- clear `XeFGBinding` because Destroy was observed;
- remove/reset `m_swapchain_hook` because Destroy was observed;
- retry failed XeFG Destroy;
- change OptiScaler behavior;
- alter MHW-only ResizeHold policy;
- alter ResizeBuffers / ResizeBuffers1 / ResizeTarget behavior;
- alter `should_preserve_active_binding_on_monitor_timeout()`;
- add sustained-timeout recovery;
- change the binding generation policy except where necessary to carry metadata through the existing commit calls;
- change strong queue/device ownership;
- perform broad R6/R7 cleanup;
- modify upstream/native non-XeFG paths.

These are later-PR concerns.

---

# 11. Tests

Add focused unit tests where the current test structure permits them.

Minimum coverage:

## Registry tests

- runtime with InitDesc + GetSwapChain + Destroy installs all available hooks;
- runtime missing Destroy still installs successfully;
- `resolve_destroy(slot)` resolves only the exact active runtime slot;
- invalid/inactive slots fail safely;
- multiple runtimes do not cross-dispatch Destroy.

## Binding metadata tests

- `commit_initial()` stores slot/context/HWND metadata;
- same-swapchain update refreshes runtime metadata from the accepted candidate;
- replacement updates metadata and generation consistently;
- `clear()` clears runtime metadata;
- runtime identity comparison distinguishes:
  - same slot + same context;
  - same slot + different context;
  - different slot + same raw context value;
  - null context.

## Dispatch tests

Where feasible with mock functions:

- Destroy dispatcher calls original exactly once;
- Destroy original result is returned unchanged;
- instrumentation does not mutate active binding;
- failed Destroy still leaves PR-A behavior unchanged;
- pre-init diagnostics are generated before invoking original InitDesc.

If the repository does not currently have a practical logging-test harness, do not introduce a large logger abstraction solely for PR-A. Test state/data flow directly and runtime-validate log ordering.

---

# 12. Runtime Validation

Build a Release test DLL from PR-A and reproduce with:

```text
Monster Hunter Wilds
Intel GPU
OptiScaler XeFG output
REFramework Debug Log = true
```

Use the OptiScaler build currently used for the Intel MHW reproduction. Do not require the old aggressive backbuffer-release block to be restored merely for this PR.

Before each crash reproduction:

```text
delete any old reframework_crash.dmp
```

This is mandatory so a stale dump is not mistaken for a new crash dump.

Capture:

```text
re2_framework_log.txt
OptiScaler.log
new reframework_crash.dmp if one is actually produced
```

The key evidence to look for is one of these sequences.

## Sequence A — Destroy while REF still holds matching binding

```text
RuntimeLifecycle pre_init/bound generation = N
...
RuntimeLifecycle destroy_enter
    context_match = true
    active_swapchain != null
    generation = N
...
destroy_return
```

This is strong evidence that REF is still bound/owning the proxy when XeFG destroy is attempted.

## Sequence B — re-init while old binding remains active

```text
RuntimeLifecycle pre_init
    active_binding_present = true
    active_generation = N
    context_match / hwnd_match / slot_match values recorded
-> original InitFromSwapChainDesc
```

This is the critical sequence for the prior crash because a crash inside original Init can now be correlated with the old active binding state even if no post-init log executes.

## Sequence C — present starvation without Destroy/re-init

```text
present_timeout
present_timeout
present_timeout
...
no RuntimeLifecycle destroy_enter
no RuntimeLifecycle pre_init
```

This would weaken the immediate Destroy/re-init hypothesis and point more strongly toward a different proxy/hook ownership transition.

---

# 13. Acceptance Criteria

PR-A is complete when all of the following are true:

1. `xefgSwapChainDestroy` is optionally intercepted per exact runtime module/slot.
2. Missing Destroy export does not reject a runtime.
3. Active XeFG binding exposes runtime slot/context/HWND diagnostic metadata.
4. Existing XeFG swapchain/queue/device strong ownership remains unchanged.
5. `pre_init` logs active binding identity **before** original `InitFromSwapChainDesc` executes.
6. Destroy logs compare the incoming context to active binding metadata.
7. Original Destroy is called exactly once and its result is returned unchanged.
8. No Destroy/re-init event triggers reset, unhook, detach, clear, rebind, or recovery behavior.
9. Existing ResizeHold semantics are unchanged.
10. Existing hook-monitor timeout behavior is unchanged.
11. Existing native D3D11/D3D12 paths remain unchanged.
12. Release build succeeds.
13. Relevant automated tests pass.
14. MHW Intel runtime validation produces usable lifecycle logs with Debug Log enabled.

---

# 14. Review Checklist

Before opening the PR, explicitly review the diff for accidental behavior changes.

The PR should be rejected/reworked if any of these appear:

```text
ComPtr swapchain ownership removed
Destroy causes binding clear
Destroy causes unhook
Destroy causes renderer reset
pre-init causes old binding detach
hook monitor timeout policy changed
resize lifecycle behavior changed
generic DXGI hooking behavior changed
```

The ideal PR-A diff should look like:

```text
runtime export interception + identity metadata + diagnostics + tests
```

and nothing more.

---

# 15. Follow-up Boundary

Do not implement these in PR-A, but leave the code structured so they can be implemented cleanly later.

## PR-B — ownership/lifecycle correction

Expected scope after PR-A evidence is reviewed:

- remove long-lived strong ownership of the XeFG proxy swapchain if confirmed unsafe;
- convert proxy swapchain tracking to borrowed semantics;
- explicit detach before matching Destroy;
- explicit detach before relevant re-init;
- remove vtable hook while the object is still valid;
- reset renderer before invalidating the borrowed binding;
- rebind only after successful new Init/candidate discovery.

Ownership conversion and lifecycle detach must be done together, not as separate intermediate behavior changes.

## PR-C — sustained timeout/stale-binding defense

Expected later scope:

- distinguish one-off Present starvation from sustained starvation;
- validate hook instance vs active binding identity;
- stop treating cached alias equality as sufficient proof of liveness;
- avoid infinite `preserve_binding` loops;
- do not blindly generic-rehook a XeFG proxy on first timeout.

---

# 16. Final Implementation Principle

PR-A is an evidence-gathering PR.

The current hypothesis is strong enough to instrument, but not strong enough to justify changing COM ownership before observing the exact runtime transition.

The required principle is:

> Observe the XeFG context/swapchain lifecycle at the exact runtime boundary first. Preserve current behavior. Use the resulting MHW logs to decide whether PR-B should change ownership and detach ordering.
