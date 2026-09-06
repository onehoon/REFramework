# Work Order: XeFG Refactor R3 — InitDesc Transaction and Factory Capture Extraction

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `74042e1686f62a54a50540e1a113a3ae648778c1` (`P3.3B.1: scope XeFG ResizeTarget hold to MHW`, merged as PR #20)

Relevant merged refactor baseline:

- R1 / PR #17: `65f9b3ee81971c3e2aac6df49518fa2dd588365d` — runtime registry extraction
- R2 / PR #18: `aa3a53e516b882a77d399c929efa0ef29d1426b0` — loader / probe handoff isolation
- PR #19: `3f83b8af0184f931daec44dc45257f7fc46966a4` — register compatibility sources in checked-in `CMakeLists.txt`
- PR #20: `74042e1686f62a54a50540e1a113a3ae648778c1` — scope XeFG ResizeTarget transition hold to Monster Hunter Wilds only

Related design documents:

- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_ARCHITECTURE_2026-09-06.md`
- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_PR_SPLIT_PLAN_2026-09-06.md`
- `doc/work-order/XEFG_REFACTOR_R1_RUNTIME_REGISTRY_EXTRACTION.md`
- `doc/work-order/XEFG_REFACTOR_R2_LOADER_PROBE_HANDOFF.md`

This work order implements **R3 only** from the fine-grained refactor plan.

---

# 1. Recommended PR Identity

Suggested branch:

```text
refactor/xefg-r3-init-transaction-factory-capture
```

Suggested PR title:

```text
Refactor R3: extract XeFG InitDesc transaction and factory capture
```

Suggested commit title:

```text
refactor: extract XeFG InitDesc discovery transaction
```

This PR is a **behavior-preserving ownership extraction**.

Do not begin R4 queue validation / candidate construction in this PR.

---

# 2. Primary Objective

Move the bounded XeFG `xefgSwapChainD3D12InitFromSwapChainDesc` observation transaction and its temporary `IDXGIFactory2::CreateSwapChainForHwnd[15]` instance hook out of `D3D12Hook.cpp` into a dedicated XeFG discovery component.

The new component should own only the mechanics required to answer:

> During this exact XeFG InitDesc call, what presentation swapchain and queue did XeFG create through the supplied DXGI factory?

It must **not** decide whether that observation is a valid REFramework binding.

After R3, the desired ownership boundary is:

```text
XeFGRuntimeRegistry (R1)
    |
    | exact runtime slot + exact original InitDesc trampoline
    v
D3D12Hook::xefg_init_desc_dispatch()
    |
    | starts one bounded observation
    v
XeFGDiscovery                         <-- NEW OWNER IN R3
    |- serialize outer InitDesc observation transaction
    |- store current InitDesc call inputs
    |- install temporary factory VtableHook
    |- intercept CreateSwapChainForHwnd[15]
    |- call the original DXGI factory method
    |- capture returned internal presentation swapchain
    |- capture actual presentation command queue
    |- call original XeFG InitDesc exactly once
    |- remove temporary factory hook after InitDesc returns
    |- return immutable/raw observation result
    |
    v
D3D12Hook                             <-- STILL OWNS R4+ POLICY
    |- InitDesc runtime-slot/module diagnostics
    |- queue/device validation
    |- HWND/interface validation
    |- render vs observe-only decision
    |- PendingXefgBinding publication
    |- active bind/rebind
    |- Present/resize lifecycle
```

The one-primary-responsibility rule for this PR is:

> **Extract discovery observation mechanics. Do not extract discovery acceptance policy.**

---

# 3. Why This Extraction Is Needed

R1 and R2 have already removed two unrelated responsibilities from `D3D12Hook`:

```text
runtime registry / stable thunks    -> XeFGRuntimeRegistry
loader / pending probe handoff      -> XeFGCompatibility
```

However, `D3D12Hook.cpp` still owns a second independent XeFG subsystem: the temporary factory-capture transaction used while calling XeFG InitDesc.

Current master still contains transaction-specific state conceptually equivalent to:

```cpp
struct XefgInitTransaction {
    void* context{};
    HWND hwnd{};
    ID3D12CommandQueue* init_queue{};
    ID3D12CommandQueue* presentation_queue{};
    IDXGIFactory2* factory{};
    IDXGISwapChain1* candidate{};
    bool factory_create_succeeded{false};
    int32_t init_result{-1};
};

std::recursive_mutex g_xefg_init_transaction_mutex{};
XefgInitTransaction g_xefg_transaction{};
std::unique_ptr<VtableHook> g_xefg_factory_hook{};
```

and the following responsibilities remain mixed into `D3D12Hook`:

```text
xefg_init_desc_common()
    -> serialize transaction
    -> seed context/HWND/init queue/factory
    -> create temporary factory instance hook
    -> hook CreateSwapChainForHwnd[15]
    -> call original XeFG InitDesc
    -> record result
    -> remove temporary factory hook
    -> trigger candidate validation/publication

create_xefg_swapchain()
    -> call original factory CreateSwapChainForHwnd
    -> capture returned internal swapchain
    -> capture presentation queue passed as factory "device"
```

This is discovery mechanics, not generic D3D12 presentation-hook ownership.

R3 should isolate those mechanics now, before R4 extracts the queue/device/acceptance policy.

---

# 4. Proven Behavior That Must Remain Unchanged

The currently working XeFG presentation discovery path is:

```text
public XeFG InitFromSwapChainDesc
    -> exact runtime thunk from R1 registry
    -> D3D12Hook dispatch
    -> bounded temporary hook of supplied IDXGIFactory2 instance
    -> intercept CreateSwapChainForHwnd[15]
    -> capture XeFG internal post-FG presentation swapchain
    -> capture actual queue used to create that presentation swapchain
    -> call original XeFG InitDesc exactly once
    -> remove temporary factory hook
    -> validate candidate
    -> select render or observe-only policy
    -> publish/bind
```

The critical fact remains:

> REFramework renders against the XeFG **internal presentation swapchain** and actual **presentation command queue**, not the public XeFG interpolation proxy.

R3 must not change that fact or reinterpret it.

---

# 5. Strict Scope

## 5.1 Move into the new discovery component

Move ownership of the following transaction mechanics:

- `XefgInitTransaction`-equivalent active transaction state;
- `g_xefg_init_transaction_mutex`-equivalent serialization;
- `g_xefg_factory_hook` ownership;
- initialization/reset of one transaction;
- temporary `VtableHook` creation for the **supplied `IDXGIFactory2` instance**;
- factory vtable slot **15** interception;
- `CreateSwapChainForHwnd` original-function lookup and forwarding;
- capture of:
  - XeFG context;
  - target HWND;
  - InitDesc command queue;
  - supplied factory;
  - actual command queue passed to `CreateSwapChainForHwnd`;
  - factory-returned internal swapchain;
  - whether factory creation succeeded;
  - original XeFG InitDesc result;
- teardown of the temporary factory hook after original InitDesc returns;
- a narrow diagnostic accessor if required by the existing `GetSwapChainPtr` diagnostic comparison.

## 5.2 Keep in `D3D12Hook` in R3

Do **not** move:

- runtime slot dispatch;
- exact runtime `HMODULE` diagnostic context;
- R1 registry original trampoline lookup;
- `XefgQueueRelation`;
- `QueueIdentitySnapshot`;
- queue COM identity capture;
- queue device identity capture;
- queue type validation;
- candidate device validation;
- candidate HWND validation;
- `IDXGISwapChain3` interface validation;
- presentation queue selection policy;
- render vs observe-only decision;
- `PendingXefgBinding`;
- `publish_xefg_candidate` binding policy;
- pending candidate handoff;
- active strong COM ownership;
- external binding/rebinding;
- Present/Present1 behavior;
- ResizeBuffers / ResizeTarget / ResizeBuffers1 behavior;
- hook-monitor preservation policy;
- logging cleanup.

Those are later R-series tasks.

---

# 6. Explicit Non-Goals

Do not use R3 as an opportunity to perform any of the following:

- R4 queue/candidate refactor;
- R5 pending candidate handoff extraction;
- R6 active binding-state extraction;
- R7 rebind transaction rewrite;
- R8 resize-state extraction;
- R9 Present/resize callback cleanup;
- R10 hook-monitor cleanup;
- Debug Logging UI work;
- log-level reduction;
- generic frame-generation abstractions;
- FSRFG changes;
- DLSSG/Streamline changes;
- generic DXGI factory-hook redesign;
- generic `IFrameGenerationProvider` or provider registry;
- Intel API/private-offset hooks;
- OptiScaler private class/symbol dependencies;
- hooking-library replacement;
- arbitrary timeout/sleep/retry logic;
- module unload/reload architecture;
- COM lifetime redesign of the active binding.

In particular, do **not** turn this component into a generic swapchain discovery service.

It is specifically the proven OptiScaler + XeFG InitDesc discovery path.

---

# 7. Latest-Master Constraints Added by PR #19 and PR #20

## 7.1 PR #19 build-source lesson

`cmake.toml` declares recursive source globs:

```toml
[target.REFramework]
sources = ["src/**.cpp", "src/**.c"]
headers = ["src/**.hpp", "src/**.h"]
```

However, CI currently consumes the checked-in generated `CMakeLists.txt`, whose explicit source list had to be patched in PR #19 for the R1/R2 compatibility files.

Therefore if R3 adds:

```text
src/compatibility/xefg/XeFGDiscovery.cpp
src/compatibility/xefg/XeFGDiscovery.hpp
```

then this PR must also ensure those files are present in the checked-in `CMakeLists.txt` source list.

Preferred approach for R3:

```text
- do not broadly regenerate CMakeLists.txt;
- add only the two new XeFGDiscovery entries next to the existing XeFG compatibility files;
- do not modify cmake.toml, because its existing glob already describes the desired source set;
- keep the CMake diff surgical.
```

Example:

```cmake
"src/compatibility/xefg/XeFGCompatibility.cpp"
"src/compatibility/xefg/XeFGCompatibility.hpp"
"src/compatibility/xefg/XeFGDiscovery.cpp"
"src/compatibility/xefg/XeFGDiscovery.hpp"
"src/compatibility/xefg/XeFGRuntimeRegistry.cpp"
"src/compatibility/xefg/XeFGRuntimeRegistry.hpp"
```

A successful local build is not sufficient if the new `.cpp` is absent from the checked-in CMake target.

## 7.2 PR #20 resize policy is frozen for R3

Latest master intentionally arms the XeFG ResizeTarget transition hold only when:

```cpp
event_id != 0
&& renderer_reset_performed
&& !m_xefg_p21_observe_only
&& sdk::GameIdentity::get().is_mhwilds()
```

R3 must not alter:

- the `is_mhwilds()` condition;
- resize event ordering;
- renderer reset ordering;
- hold arm/clear semantics;
- Present suppression semantics.

If R3 touches that region for any reason, treat it as scope leakage and revert it.

---

# 8. Recommended New Component

Recommended files:

```text
src/compatibility/xefg/XeFGDiscovery.hpp
src/compatibility/xefg/XeFGDiscovery.cpp
```

Preferred responsibility statement:

```cpp
// Observes one XeFG InitFromSwapChainDesc call and captures the swapchain/queue
// created through the supplied DXGI factory. It does not decide whether that
// observation is valid for REFramework rendering.
```

Do not place active binding ownership here.

---

# 9. Recommended Observation Type

Use a narrow result type that describes only what happened during the InitDesc transaction.

Conceptual example:

```cpp
class XeFGDiscovery {
public:
    using InitFn = int32_t (WINAPI*)(
        void* context,
        HWND hwnd,
        const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
        ID3D12CommandQueue* command_queue,
        IDXGIFactory2* factory,
        const void* init_params);

    struct Observation {
        void* context{};
        HWND hwnd{};
        ID3D12CommandQueue* init_queue{};
        ID3D12CommandQueue* presentation_queue{};
        IDXGIFactory2* factory{};
        IDXGISwapChain1* internal_swapchain{};
        bool factory_create_succeeded{};
        int32_t init_result{-1};
    };

    static Observation observe_init(
        InitFn original,
        void* context,
        HWND hwnd,
        const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
        ID3D12CommandQueue* command_queue,
        IDXGIFactory2* factory,
        const void* init_params);

    static IDXGISwapChain1* current_internal_swapchain_for_diagnostics() noexcept;
};
```

Exact naming is flexible.

The important semantic boundary is not.

---

# 10. Raw Observation vs Strong Ownership

R3 should **not** convert the transaction output into the final strong binding candidate.

Current discovery transaction observes raw objects supplied/returned by the XeFG call. R4 will decide whether to construct a validated immutable candidate with strong COM ownership.

Therefore, in R3:

```text
Observation
    = raw facts from one bounded InitDesc call

Validated candidate
    = NOT R3
```

Do not prematurely add:

```cpp
ComPtr<IDXGISwapChain3> validated_swapchain;
ComPtr<ID3D12CommandQueue> validated_queue;
ComPtr<ID3D12Device4> validated_device;
```

to the discovery observation solely because those types will exist later.

That belongs to R4 candidate construction.

Likewise, do not add a `Release` hook for any observed object.

---

# 11. Required InitDesc Transaction Sequence

The transaction sequence must remain structurally equivalent to current master.

Required order:

```text
1. acquire the existing transaction-serialization contract
2. initialize fresh observation state
3. clear any stale temporary factory hook
4. if factory != nullptr:
       create VtableHook for this factory instance
       hook CreateSwapChainForHwnd slot 15
       if hook preparation fails:
           clear temporary hook
           continue anyway
5. call original XeFG InitFromSwapChainDesc exactly once
6. record original result
7. remove temporary factory hook
8. return completed observation
9. D3D12Hook performs current validation/publication policy
```

The original XeFG API call is authoritative and must not be skipped merely because the temporary factory hook failed.

Failure to observe the internal swapchain should result in the existing later `no_candidate` / rejection behavior, not an altered XeFG API result.

---

# 12. Factory Hook Must Stay Bounded to the Outer InitDesc Call

The factory hook is intentionally temporary.

Required invariant:

```text
before original InitDesc
    temporary factory instance hook may exist

while original InitDesc executes
    CreateSwapChainForHwnd[15] is observed

after original InitDesc returns
    temporary factory hook is removed
```

Do not:

- leave the factory hook installed for future calls;
- convert it into a process-global DXGI factory detour;
- hook all factories;
- hook `CreateSwapChain` or unrelated factory methods;
- add delayed cleanup timers;
- use the public XeFG proxy as a substitute when factory capture fails.

The bounded hook is one of the key reasons the current approach has low interference with OptiScaler and native REFramework behavior.

---

# 13. CreateSwapChainForHwnd Capture Semantics

The physical callback should move into `XeFGDiscovery`.

Conceptual signature:

```cpp
static HRESULT WINAPI create_swapchain_for_hwnd(
    IDXGIFactory2* factory,
    IUnknown* device,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    IDXGIOutput* restrict_to_output,
    IDXGISwapChain1** swap_chain);
```

Required behavior:

```cpp
const auto original = /* temporary factory hook slot 15 original */;
const auto result = original(
    factory,
    device,
    hwnd,
    desc,
    fullscreen_desc,
    restrict_to_output,
    swap_chain);

if (SUCCEEDED(result) && swap_chain != nullptr && *swap_chain != nullptr) {
    // "device" is expected to be the queue used for DXGI swapchain creation.
    // Query it as ID3D12CommandQueue only for observation.
    // Preserve current behavior if this query fails.

    observation.presentation_queue = ...;
    observation.internal_swapchain = *swap_chain;
    observation.factory_create_succeeded = true;
}

return result;
```

Critical rules:

- call the original factory function exactly once;
- do not mutate its arguments;
- return its HRESULT unchanged;
- only record a candidate after the original factory call succeeds and returns a non-null swapchain;
- do not decide render/observe mode here;
- do not compare device identity here;
- do not reject a non-DIRECT queue here;
- do not call REFramework renderer code here.

---

# 14. Preserve Presentation Queue Meaning

The queue captured in the factory callback is semantically important.

Current proven architecture distinguishes:

```text
InitDesc command_queue
    = queue supplied to XeFG public initialization

presentation_queue
    = queue actually passed to DXGI CreateSwapChainForHwnd
      for the internal post-FG presentation swapchain
```

Those can be two different DIRECT queues on the same D3D12 device.

R3 must preserve both values independently in the observation.

Do not normalize them to one pointer.

Do not decide in R3 which one REFramework will render through.

That decision remains R4 policy.

---

# 15. Transaction Serialization Requirements

The current code uses a `std::recursive_mutex` to serialize the full outer InitDesc transaction.

Preserve this contract in R3.

Recommended shape:

```cpp
std::recursive_mutex XeFGDiscovery::s_transaction_mutex{};
```

and:

```cpp
XeFGDiscovery::Observation XeFGDiscovery::observe_init(...) {
    std::unique_lock transaction_lock{s_transaction_mutex};
    ...
    const auto result = original(...);
    ...
    return completed;
}
```

Do not narrow the lock to only setup/teardown without evidence that concurrent InitDesc transactions are safe.

Do not introduce a worker thread.

Do not add sleeps/retries.

## Important callback-lock warning

Do **not** add a new non-recursive mutex acquisition in the `CreateSwapChainForHwnd` callback that can block behind the outer InitDesc transaction.

The original XeFG call invokes the factory path during the bounded transaction. A new lock relationship around that callback can create a real deadlock if the original call waits for the factory operation to return.

Keep the synchronization topology equivalent to current behavior.

Do not attempt a generalized concurrency redesign in R3.

---

# 16. Recommended Internal State Shape

A possible implementation:

```cpp
class XeFGDiscovery {
public:
    struct Observation { ... };

    static Observation observe_init(...);
    static IDXGISwapChain1* current_internal_swapchain_for_diagnostics() noexcept;

private:
    struct ActiveTransaction {
        Observation observation{};
    };

    static HRESULT WINAPI create_swapchain_for_hwnd(...);

    static std::recursive_mutex s_transaction_mutex;
    static ActiveTransaction s_active;
    static std::unique_ptr<VtableHook> s_factory_hook;
    static std::atomic<IDXGISwapChain1*> s_diagnostic_candidate;
};
```

The atomic diagnostic pointer is optional; it is one straightforward way to keep the existing `GetSwapChainPtr` comparison non-blocking without exposing the transaction object.

If another equally narrow mechanism preserves the current diagnostic behavior without introducing lock inversion, it is acceptable.

Do not expose the full mutable `ActiveTransaction` outside the component.

---

# 17. D3D12Hook Integration

## 17.1 Keep runtime dispatch in D3D12Hook for R3

R1 registry thunks still target:

```cpp
D3D12Hook::xefg_init_desc_dispatch(...)
D3D12Hook::xefg_get_swapchain_dispatch(...)
```

Do not move those stable bridge entry points in R3.

R3 should only change the common InitDesc implementation behind the dispatch.

Current conceptual shape:

```cpp
int32_t D3D12Hook::xefg_init_desc_common(...) {
    // transaction + factory hook + original call
    ...
    publish_xefg_candidate();
    return result;
}
```

Required direction:

```cpp
int32_t D3D12Hook::xefg_init_desc_common(...) {
    const auto observation = XeFGDiscovery::observe_init(
        original,
        context,
        hwnd,
        swap_chain_desc,
        fullscreen_desc,
        command_queue,
        factory,
        init_params);

    spdlog::info(
        "[XeFG][InitDesc] slot = {}, module = 0x{:x}, context = 0x{:x}, result = {}",
        slot,
        reinterpret_cast<uintptr_t>(module),
        reinterpret_cast<uintptr_t>(context),
        observation.init_result);

    publish_xefg_candidate(observation);
    return observation.init_result;
}
```

The exact code can differ, but `D3D12Hook` should no longer own the temporary factory-hook transaction.

## 17.2 Change candidate publication to accept the observation

Today `publish_xefg_candidate()` reads global transaction state.

R3 should stop that hidden dependency.

Recommended signature change:

```cpp
static void publish_xefg_candidate(
    const XeFGDiscovery::Observation& observation);
```

Then inside the function:

```cpp
const auto& transaction = observation;
```

and preserve the current validation/policy body as mechanically as possible.

This signature change is **not R4**.

R3 is only making the existing policy consume an explicit observation instead of a hidden global transaction.

Do not rewrite the policy while making this change.

---

# 18. Mechanical Field Mapping for `publish_xefg_candidate`

Use a one-to-one mapping from current transaction fields to the new observation fields.

Recommended mapping:

| Current field | R3 observation field | Policy change? |
|---|---|---|
| `transaction.context` | `observation.context` | No |
| `transaction.hwnd` | `observation.hwnd` | No |
| `transaction.init_queue` | `observation.init_queue` | No |
| `transaction.presentation_queue` | `observation.presentation_queue` | No |
| `transaction.factory` | `observation.factory` | No |
| `transaction.candidate` | `observation.internal_swapchain` | No |
| `transaction.factory_create_succeeded` | same semantic field | No |
| `transaction.init_result` | `observation.init_result` | No |

Every existing rejection branch should remain semantically unchanged.

Examples that must still remain in the policy layer:

```text
init_failed
no_candidate
queue_device_unavailable
no_idxgi_swapchain3
hwnd_mismatch
candidate_device_unavailable
device_mismatch
presentation_queue_unavailable
presentation_queue_device_mismatch
presentation_queue_not_direct
```

Do not move these reasons into `XeFGDiscovery` except for the purely factual fact that the factory call itself did or did not produce a swapchain.

---

# 19. Preserve Existing State Mutex Semantics Around Policy

`g_xefg_state_mutex` currently protects more than the InitDesc transaction. It is also involved with pending-binding state and other XeFG policy state.

R3 must not delete or repurpose it wholesale.

After the transaction moves out, `publish_xefg_candidate(observation)` may continue using `g_xefg_state_mutex` around the same policy sections as current master.

Do not use R3 to redesign that mutex.

The only synchronization ownership intentionally moving in R3 is the **dedicated InitDesc transaction serialization / temporary factory-hook state**.

---

# 20. GetSwapChainPtr Diagnostic Compatibility

Current `xefg_get_swapchain_dispatch()` logs whether the public XeFG swapchain pointer equals the internal factory-captured candidate:

```text
[XeFG][PublicProxy] ... internal_same = ...
```

This is diagnostic only.

It must remain diagnostic only after R3.

Allowed direction:

```cpp
const auto internal_candidate =
    XeFGDiscovery::current_internal_swapchain_for_diagnostics();

spdlog::info(
    "[XeFG][PublicProxy] context = 0x{:x}, swapchain = 0x{:x}, internal_same = {}",
    ...,
    *swap_chain == internal_candidate);
```

Do not:

- turn `GetSwapChainPtr` into the discovery source;
- bind its returned proxy;
- use proxy vtable ownership as render authority;
- reject the real factory-captured internal swapchain because proxy comparison differs.

The public proxy remains a diagnostic/supporting signal only.

---

# 21. Logging Policy for R3

Logging cleanup is explicitly deferred to the final logging PR.

Therefore R3 should preserve existing log meaning and approximate ordering.

Important existing diagnostics to retain:

```text
[XeFG][RuntimeDispatch] InitFromSwapChainDesc
[XeFG][InitDesc]
[XeFG][InternalSwapchain]
[XeFG][PublicProxy]
[XeFG][QueueIdentity]
[XeFG][Bind]
[XeFG][P2.1Probe]
```

Moving one of the transaction-local log statements into `XeFGDiscovery.cpp` is acceptable if necessary to keep the code coherent.

Do not:

- change large numbers of info logs to debug;
- rename log families unnecessarily;
- remove support-critical values;
- add new repetitive per-frame logging.

Final log reduction and Debug Logging UI remain later work.

---

# 22. No API Parameter Mutation

R3 must preserve every argument passed to both:

```text
original XeFG InitFromSwapChainDesc
original IDXGIFactory2::CreateSwapChainForHwnd
```

Do not rewrite:

- HWND;
- swapchain descriptor;
- fullscreen descriptor;
- queue pointer;
- factory pointer;
- init params;
- DXGI output restriction;
- swapchain flags.

This PR is an observer extraction, not an API interception policy change.

---

# 23. Failure Behavior Must Remain Non-Destructive

Required behavior by failure point:

## Factory pointer is null

```text
no factory hook
-> original XeFG InitDesc still called
-> observation contains no factory-created candidate
-> existing validation rejects candidate
```

## VtableHook construction fails

```text
clear temporary hook state
-> original XeFG InitDesc still called
-> existing validation decides based on resulting observation
```

## Hooking slot 15 fails

```text
temporary hook not considered active
-> original XeFG InitDesc still called
-> no invented fallback
```

## Original factory CreateSwapChainForHwnd fails

```text
return original HRESULT unchanged
-> do not mark factory_create_succeeded
-> do not fabricate internal swapchain
```

## Queue QueryInterface fails inside factory callback

```text
internal swapchain may still be observed
presentation_queue remains unavailable
-> R4-current policy decides observe/reject behavior exactly as today
```

## Original XeFG InitDesc fails

```text
record exact result
-> remove temporary factory hook
-> existing policy rejects with init_failed
-> return exact original result
```

No discovery failure may crash the game or OptiScaler merely because REFramework could not obtain a renderable candidate.

---

# 24. Files Expected to Change

Expected primary files:

```text
src/compatibility/xefg/XeFGDiscovery.hpp          NEW
src/compatibility/xefg/XeFGDiscovery.cpp          NEW
src/D3D12Hook.cpp                                 MODIFY
src/D3D12Hook.hpp                                 MODIFY
CMakeLists.txt                                    MODIFY, source registration only
```

Normally **do not modify**:

```text
src/REFramework.cpp
src/compatibility/xefg/XeFGCompatibility.cpp
src/compatibility/xefg/XeFGCompatibility.hpp
src/compatibility/xefg/XeFGRuntimeRegistry.cpp
src/compatibility/xefg/XeFGRuntimeRegistry.hpp
cmake.toml
src/mods/...
shared/...
```

If implementation appears to require changes outside the expected set, stop and determine whether the work has crossed into R4+.

---

# 25. D3D12Hook.hpp Cleanup Expected in R3

The R3 extraction should remove the temporary factory callback declaration from `D3D12Hook` if that callback has moved into `XeFGDiscovery`.

For example, remove:

```cpp
static HRESULT WINAPI create_xefg_swapchain(...);
```

Keep:

```cpp
static int32_t xefg_init_desc_dispatch(...);
static int32_t xefg_get_swapchain_dispatch(...);
static int32_t xefg_init_desc_common(...);
static void publish_xefg_candidate(...);
```

with only the minimal signature adjustment required to pass the discovery observation.

Do not use R3 to clean the rest of `D3D12Hook.hpp` yet.

The larger public-surface cleanup belongs to R10.

---

# 26. Suggested Header Example

A concrete direction, not mandatory exact syntax:

```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "utility/VtableHook.hpp"

class XeFGDiscovery {
public:
    using InitFn = int32_t (WINAPI*)(
        void*, HWND,
        const DXGI_SWAP_CHAIN_DESC1*,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
        ID3D12CommandQueue*,
        IDXGIFactory2*,
        const void*);

    struct Observation {
        void* context{};
        HWND hwnd{};
        ID3D12CommandQueue* init_queue{};
        ID3D12CommandQueue* presentation_queue{};
        IDXGIFactory2* factory{};
        IDXGISwapChain1* internal_swapchain{};
        bool factory_create_succeeded{false};
        int32_t init_result{-1};
    };

    static Observation observe_init(
        InitFn original,
        void* context,
        HWND hwnd,
        const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
        ID3D12CommandQueue* command_queue,
        IDXGIFactory2* factory,
        const void* init_params);

    static IDXGISwapChain1* current_internal_swapchain_for_diagnostics() noexcept;

private:
    static HRESULT WINAPI create_swapchain_for_hwnd(
        IDXGIFactory2* factory,
        IUnknown* device,
        HWND hwnd,
        const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
        IDXGIOutput* restrict_to_output,
        IDXGISwapChain1** swap_chain);

    static std::recursive_mutex s_transaction_mutex;
    static Observation s_active;
    static std::unique_ptr<VtableHook> s_factory_hook;
    static std::atomic<IDXGISwapChain1*> s_diagnostic_candidate;
};
```

Avoid additional abstractions unless they materially simplify this exact transaction.

---

# 27. Suggested Implementation Skeleton

Conceptual only:

```cpp
XeFGDiscovery::Observation XeFGDiscovery::observe_init(
    InitFn original,
    void* context,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    ID3D12CommandQueue* command_queue,
    IDXGIFactory2* factory,
    const void* init_params)
{
    std::unique_lock transaction_lock{s_transaction_mutex};

    s_active = {};
    s_active.context = context;
    s_active.hwnd = hwnd;
    s_active.init_queue = command_queue;
    s_active.factory = factory;
    s_active.init_result = -1;
    s_diagnostic_candidate.store(nullptr, std::memory_order_relaxed);

    s_factory_hook.reset();

    if (factory != nullptr) {
        try {
            auto hook = std::make_unique<VtableHook>(Address{factory});
            if (hook->hook_method(
                    15,
                    Address{reinterpret_cast<void*>(&XeFGDiscovery::create_swapchain_for_hwnd)})) {
                s_factory_hook = std::move(hook);
            }
        } catch (...) {
            s_factory_hook.reset();
        }
    }

    const auto result = original(
        context,
        hwnd,
        swap_chain_desc,
        fullscreen_desc,
        command_queue,
        factory,
        init_params);

    s_active.init_result = result;
    s_factory_hook.reset();

    return s_active;
}
```

Notes:

- preserve the current actual logging around this sequence;
- do not treat this skeleton as authorization to alter exception policy or result codes;
- do not call candidate validation from inside `XeFGDiscovery`.

---

# 28. Suggested Factory Callback Skeleton

```cpp
HRESULT WINAPI XeFGDiscovery::create_swapchain_for_hwnd(
    IDXGIFactory2* factory,
    IUnknown* device,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    IDXGIOutput* restrict_to_output,
    IDXGISwapChain1** swap_chain)
{
    if (s_factory_hook == nullptr) {
        return E_FAIL;
    }

    using CreateFn = HRESULT (WINAPI*)(
        IDXGIFactory2*, IUnknown*, HWND,
        const DXGI_SWAP_CHAIN_DESC1*,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
        IDXGIOutput*, IDXGISwapChain1**);

    const auto original = s_factory_hook->get_method<CreateFn>(15);
    const auto result = original(
        factory,
        device,
        hwnd,
        desc,
        fullscreen_desc,
        restrict_to_output,
        swap_chain);

    if (SUCCEEDED(result) && swap_chain != nullptr && *swap_chain != nullptr) {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        if (device != nullptr && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
            s_active.presentation_queue = queue.Get();
        }

        s_active.internal_swapchain = *swap_chain;
        s_active.factory_create_succeeded = true;
        s_diagnostic_candidate.store(*swap_chain, std::memory_order_relaxed);
    }

    return result;
}
```

Again, do not add queue validation here.

---

# 29. Candidate Publication Example

Mechanical transition only:

Before:

```cpp
void D3D12Hook::publish_xefg_candidate() {
    ...
    std::scoped_lock lock{g_xefg_state_mutex};
    const auto& transaction = g_xefg_transaction;
    ...
}
```

After:

```cpp
void D3D12Hook::publish_xefg_candidate(
    const XeFGDiscovery::Observation& observation)
{
    ...
    std::scoped_lock lock{g_xefg_state_mutex};
    const auto& transaction = observation;
    ... // keep existing validation/policy logic unchanged
}
```

If the field is renamed from `candidate` to `internal_swapchain`, perform only the mechanical member-name substitution.

Do not reorganize the branch structure in this PR.

---

# 30. Forbidden Simplifications

The following would be incorrect for R3:

## Incorrect: use public GetSwapChainPtr instead of factory capture

```cpp
// WRONG
GetSwapChainPtr(...)
-> bind returned proxy
```

## Incorrect: use InitDesc queue as presentation queue unconditionally

```cpp
// WRONG
observation.presentation_queue = command_queue;
```

## Incorrect: persistent global factory hook

```cpp
// WRONG
install factory vtable hook once and leave it installed
```

## Incorrect: validate queue/device in callback

```cpp
// WRONG FOR R3
if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return E_FAIL;
```

## Incorrect: renderer work inside discovery

```cpp
// WRONG
if (candidate)
    g_framework->on_reset();
```

## Incorrect: move active binding ownership now

```cpp
// WRONG FOR R3
ComPtr<IDXGISwapChain3> active_xefg_swapchain;
```

## Incorrect: generic abstraction

```cpp
// WRONG
class IFrameGenerationSwapchainDiscoveryProvider { ... };
```

---

# 31. Preserve Native REFramework Behavior

When XeFG is absent:

```text
XeFGDiscovery::observe_init()
    is never entered
```

Therefore native D3D12 behavior must remain unchanged.

Audit the PR specifically to ensure no changes to:

- native dummy swapchain discovery;
- native D3D12 Present flow;
- D3D11 path;
- Streamline/DLSSG path;
- renderer callbacks;
- Lua/plugin/mod behavior;
- VR/input/UI code.

A native D3D12 diff outside include/declaration plumbing is a scope warning.

---

# 32. Interaction with R1 Runtime Registry

R1 must remain authoritative for exact runtime dispatch.

Required flow:

```text
XeFGRuntimeRegistry::resolve_init(slot)
    -> exact original trampoline for the exact HMODULE
    -> D3D12Hook::xefg_init_desc_common(... original ...)
    -> XeFGDiscovery::observe_init(original, ...)
```

Do not move `FunctionHook` ownership into `XeFGDiscovery`.

Do not make `XeFGDiscovery` search for `libxess_fg.dll` exports.

Do not make it know about runtime slot allocation.

---

# 33. Interaction with R2 XeFGCompatibility

R2 must remain unchanged.

`XeFGCompatibility` owns:

- module-seen state;
- pending probe state;
- module enumeration;
- synchronous exact-module handoff to registry.

R3 must not call `XeFGCompatibility` from the factory callback or transaction.

No new loader dependency should be introduced.

---

# 34. Static Review Checklist

A reviewer should be able to answer **yes** to all of these before merge.

## Scope / ownership

- [ ] `XeFGDiscovery` owns only InitDesc/factory-capture mechanics.
- [ ] Runtime registry ownership remains in `XeFGRuntimeRegistry`.
- [ ] Loader/probe ownership remains in `XeFGCompatibility`.
- [ ] Queue/candidate acceptance policy remains in `D3D12Hook`.
- [ ] Binding/resize/Present behavior is unchanged.

## Transaction correctness

- [ ] Original XeFG InitDesc is called exactly once.
- [ ] Original InitDesc arguments are unchanged.
- [ ] Outer InitDesc observation remains serialized.
- [ ] Temporary factory hook is installed before original InitDesc when possible.
- [ ] Factory slot 15 is still `CreateSwapChainForHwnd`.
- [ ] Temporary factory hook is removed after original InitDesc returns.
- [ ] Hook setup failure does not skip original InitDesc.

## Factory callback correctness

- [ ] Original factory method is called exactly once.
- [ ] Original factory arguments are unchanged.
- [ ] Original HRESULT is returned unchanged.
- [ ] Candidate is recorded only after successful factory call with non-null swapchain.
- [ ] Actual presentation queue is independently observed.
- [ ] Init queue and presentation queue are not collapsed.

## Policy preservation

- [ ] Public GetSwapChainPtr remains diagnostic only.
- [ ] Queue relation classification has not moved or changed.
- [ ] Candidate HWND/device/interface validation has not moved or changed.
- [ ] Render vs observe-only policy has not moved or changed.
- [ ] Existing pending-binding and rebind code is unchanged except mechanical observation plumbing.

## Latest-master protection

- [ ] PR #20 MHW-only resize hold condition is unchanged.
- [ ] No resize lifecycle code was modified.
- [ ] No hook-monitor policy change is included.

## Build registration

- [ ] New `.cpp/.hpp` are present in checked-in `CMakeLists.txt`.
- [ ] No broad generated-CMake churn is present.

---

# 35. Build / Validation Requirements

Required before PR is marked ready:

```text
1. git diff --check
2. Release configure/build
3. REFramework target builds and links the new XeFGDiscovery.cpp
4. inspect generated/checked-in source list to confirm the file is compiled
5. source ownership audit
6. no unrelated changes
```

Recommended commands consistent with current project workflow:

```text
cmake -B build
cmake --build build --config Release --target REFramework
```

The PR description must state the actual build result.

Do not write `PASS` if the command was not run.

---

# 36. Runtime Validation Position

R3 itself is **not** the scheduled discovery-wave runtime gate.

The fine-grained plan places the next full discovery runtime gate after R4, because R3 only moves observation mechanics and R4 then moves the acceptance/candidate layer.

Therefore:

```text
R3
    -> build/static validation required
    -> runtime smoke optional but valuable

R4
    -> discovery wave runtime gate required
```

If a quick runtime smoke is available for R3, useful checks are:

```text
DD2 + OptiScaler + XeFG
    game launches
    XeFG InitDesc is intercepted
    [XeFG][InternalSwapchain] still appears
    both overlays remain available
    no repeated runtime installation loop
```

But absence of an R3-only runtime smoke is not itself a merge blocker if code review and CI are clean.

---

# 37. Expected Effective Size

Planning target:

```text
effective implementation change: ~180–280 LOC
soft ceiling:                    ~350 LOC
GitHub add+delete:               ~350–600 LOC
```

Because code is moving from `D3D12Hook.cpp` into two new files, GitHub changed-line count may look larger than semantic change.

Do not combine R4 merely to make the extraction look more complete.

---

# 38. PR Description Template

Recommended PR body:

```markdown
## Summary

- extract the serialized XeFG InitFromSwapChainDesc observation transaction into `XeFGDiscovery`
- move the temporary IDXGIFactory2::CreateSwapChainForHwnd[15] instance hook and internal swapchain/presentation-queue capture out of `D3D12Hook`
- pass the completed observation back to the existing D3D12Hook candidate-validation/publication policy
- preserve the existing R1 runtime registry, R2 loader handoff, binding, Present, and resize behavior

## Not in scope

- queue/device acceptance policy (R4)
- pending candidate handoff (R5)
- active binding ownership/rebind protocol (R6-R7)
- resize/Present lifecycle changes (R8-R9)
- hook-monitor cleanup (R10)
- logging cleanup / Debug Logging UI

## Critical invariants

- original XeFG InitDesc is called exactly once
- temporary factory hook is bounded to the outer InitDesc transaction
- factory CreateSwapChainForHwnd[15] original is forwarded exactly once
- internal presentation swapchain and actual presentation queue remain the observed facts
- public XeFG GetSwapChainPtr remains diagnostic only
- PR #20 MHW-only ResizeTarget hold policy is unchanged

## Validation

- Release build: PASS/FAIL
- git diff --check: PASS/FAIL
- checked-in CMake source registration: PASS/FAIL
- runtime smoke: performed / not performed
```

---

# 39. Blocking Review Findings

Treat the following as blocking if present:

- original XeFG InitDesc can be skipped or called more than once;
- original InitDesc parameters are changed;
- temporary factory hook survives the bounded transaction unintentionally;
- factory callback no longer forwards the exact original slot-15 function;
- factory original can be called more than once;
- presentation queue is replaced by/init-normalized to the InitDesc queue;
- public XeFG proxy becomes candidate/render authority;
- candidate validation moves into discovery with changed behavior;
- queue/device acceptance policy changes;
- pending/active binding logic is materially rewritten;
- MHW-only resize hold condition changes;
- a new mutex arrangement can realistically deadlock the factory callback during original InitDesc;
- new source file is not actually part of the REFramework CMake target;
- R4+ work is mixed into this PR.

Do not block solely on naming/style preferences or theoretical pathologies without a realistic failure mode.

---

# 40. Non-Blocking / Follow-Up Findings

Normally non-blocking for R3:

- naming preferences for `Observation` fields;
- whether transaction-local logs live in `D3D12Hook.cpp` or `XeFGDiscovery.cpp`;
- minor const/noexcept refinements;
- future strong candidate ownership design;
- future module unload handling;
- future log-level cleanup;
- future generic test seams that would expand the PR materially.

Do not inflate R3 for speculative robustness.

---

# 41. Stop Condition

Once the following is true, **stop the PR**:

```text
XeFG InitDesc transaction state is no longer owned by D3D12Hook
AND
CreateSwapChainForHwnd[15] temporary hook/capture is owned by XeFGDiscovery
AND
D3D12Hook receives an explicit completed observation
AND
existing queue/candidate/binding policy consumes it unchanged
AND
build/source registration is correct
```

Do not continue into queue validation extraction.

That is R4.

---

# 42. Final Architecture After R3

Expected dependency after merge:

```text
REFramework.cpp
    |
    v
XeFGCompatibility
    |
    v
XeFGRuntimeRegistry
    |
    | exact runtime original dispatch
    v
D3D12Hook::xefg_init_desc_dispatch
    |
    v
XeFGDiscovery
    |
    | Observation
    v
D3D12Hook candidate validation/publication
    |
    v
existing pending/active binding + renderer path
```

This is the desired intermediate state.

R4 will then take the next single responsibility:

```text
Observation
    -> queue/device/interface/HWND validation
    -> relation classification
    -> immutable validated binding candidate
```

Do not implement that R4 arrow in this PR.
