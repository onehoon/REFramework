# Work Order: XeFG Refactor R1 — Runtime Registry Extraction

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `001c372f147d8407c9cb02df0335461055830a0b`  
Latest behavior-changing source baseline: `0a5f7a3bfca4367abe3379cc2635d1a7bb894f53` (`fix: hold XeFG rendering across resize transitions (#16)`)

Related architecture documents:

- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_ARCHITECTURE_2026-09-06.md`
- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_PR_SPLIT_PLAN_2026-09-06.md`

This work order implements **R1 only** from the fine-grained refactor plan.

---

# 1. Recommended PR Identity

Suggested branch:

```text
refactor/xefg-r1-runtime-registry
```

Suggested PR title:

```text
Refactor R1: extract XeFG runtime registry
```

Suggested commit title:

```text
refactor: extract XeFG runtime registry
```

This PR is intentionally small and behavior-preserving.

Do **not** combine R2 or later work into this PR even if the adjacent changes appear easy.

---

# 2. Objective

Extract the **exact-HMODULE XeFG runtime registry, stable runtime slots/thunks, XeFG export resolution, and per-runtime `FunctionHook` ownership** from `D3D12Hook.cpp` into a dedicated XeFG compatibility component.

The current runtime behavior must remain unchanged.

The target architectural result after R1 is:

```text
REFramework.cpp
    |
    | existing calls unchanged in R1
    v
D3D12Hook XeFG loader/probe bridge
    |
    | install exact HMODULE
    v
XeFGRuntimeRegistry                <-- NEW OWNER IN R1
    |- exact HMODULE registry
    |- fixed runtime slots
    |- InitDesc/GetSwapChainPtr thunk tables
    |- export address resolution
    |- FunctionHook ownership
    |- runtime install state
    |- matching original trampoline lookup
    |
    | temporary dispatch bridge
    v
D3D12Hook::xefg_*_dispatch()
    |
    v
EXISTING discovery / candidate / binding / lifecycle code
```

R1 changes **ownership/location**, not XeFG presentation policy.

---

# 3. Product / Safety Context

This fork is still REFramework first.

The refactor exists only to isolate the fork-specific OptiScaler + Intel XeFG compatibility path and make future upstream maintenance safer.

The following are non-negotiable:

1. REFramework native behavior must not regress.
2. FSRFG and DLSSG/Streamline are not targets.
3. Existing OptiScaler + XeFG behavior from the current master must not be redesigned.
4. Exact `HMODULE` identity remains authoritative for multiple `libxess_fg.dll` modules.
5. No OptiScaler private class/symbol/offset dependency may be introduced.
6. No Intel private object-layout dependency may be introduced.
7. No generic frame-generation provider abstraction may be introduced.
8. Logging cleanup is deferred to the final logging PR.

If an implementation choice is architecturally prettier but requires changing a proven runtime contract, do not do it in R1.

---

# 4. Why R1 Exists

The current master already supports multiple loaded XeFG runtime modules correctly, but that support lives inside `D3D12Hook.cpp`.

Current `D3D12Hook.cpp` owns all of the following runtime-specific machinery:

```text
kMaxXefgRuntimes = 8
XefgRuntimeInstallState
XefgRuntimeHook
g_xefg_runtimes
find_xefg_runtime()
get_xefg_runtime_by_slot()
allocate_xefg_runtime_slot()
xefg_init_desc_thunk<Slot>()
xefg_get_swapchain_thunk<Slot>()
g_xefg_init_thunks
g_xefg_get_swapchain_thunks
GetProcAddress() for XeFG public exports
FunctionHook creation per runtime
per-runtime original trampoline ownership
```

These are not normal REFramework D3D12 renderer responsibilities.

They belong in the fork-specific XeFG compatibility island.

However, R1 must **not** also move the lower-level XeFG discovery transaction, candidate validation, active binding, resize state, Present policy, or hook monitor behavior.

---

# 5. Current Behavior That Must Be Preserved Exactly

## 5.1 Runtime capacity

Current capacity:

```cpp
constexpr size_t kMaxXefgRuntimes = 8;
```

Keep it at 8 in R1.

Do not make it dynamically sized merely for cleanup.

Do not add slot recycling or unload handling.

---

## 5.2 One exact HMODULE = one runtime registry entry

Current semantic:

```text
same HMODULE observed again
    -> duplicate
    -> no second FunctionHook
    -> existing runtime remains authoritative
```

Do not deduplicate by basename alone.

Do not deduplicate by path alone.

The loaded module handle is the current runtime identity.

---

## 5.3 Required and optional exports

Required export:

```text
xefgSwapChainD3D12InitFromSwapChainDesc
```

Optional export:

```text
xefgSwapChainD3D12GetSwapChainPtr
```

Required behavior:

```text
InitFromSwapChainDesc missing
    -> reject only that runtime entry

InitFromSwapChainDesc hook creation fails
    -> reject/reset only that runtime entry

GetSwapChainPtr missing
    -> InitDesc runtime remains valid

GetSwapChainPtr present but optional hook creation fails
    -> keep InitDesc runtime active
    -> do not reject the runtime
```

Do not promote `GetSwapChainPtr` to a required API.

---

## 5.4 Stable per-runtime thunk identity

Each registered runtime receives one stable slot.

Conceptually:

```text
runtime module A -> slot 0 -> thunk<0> -> original trampoline A
runtime module B -> slot 1 -> thunk<1> -> original trampoline B
```

The detour must never use a single process-global original pointer for multiple modules.

Do not infer the source runtime from:

- return address;
- last-loaded module;
- basename lookup;
- thread-local guesses;
- swapchain vtable owner;
- OptiScaler private state.

The runtime slot is explicit dispatch identity.

---

## 5.5 Runtime state

Current runtime install states are:

```text
Empty
Installing
Active
```

Preserve these semantics.

A slot must not be dispatchable as an active runtime before its required InitDesc hook is successfully created.

---

## 5.6 Do not hold the runtime registry lock across the original XeFG call

The dispatch path may lock briefly to:

- validate slot;
- validate runtime is active;
- obtain exact `HMODULE`;
- obtain the correct original trampoline.

Then release the registry lock.

Only after releasing the registry lock may the existing lower-level XeFG path invoke the original XeFG function.

Required conceptual flow:

```text
thunk<slot>
    -> lookup active runtime under registry mutex
    -> copy module + original pointer
    -> unlock registry mutex
    -> enter existing D3D12Hook XeFG common path
    -> call original XeFG function
```

This rule is important because the original XeFG init can synchronously trigger DXGI factory callbacks and existing transaction state.

---

# 6. Strict R1 Scope

## 6.1 Move into the new runtime registry

Move ownership of:

```text
kMaxXefgRuntimes
XefgRuntimeInstallState
XefgRuntimeHook
runtime table
duplicate lookup by exact HMODULE
lookup by slot
slot allocation
stable InitDesc thunk table
stable GetSwapChainPtr thunk table
XeFG export resolution
InitDesc FunctionHook ownership
GetSwapChainPtr FunctionHook ownership
install state transitions
matching original trampoline lookup
```

The new component should be located under:

```text
src/compatibility/xefg/
```

Preferred files:

```text
src/compatibility/xefg/XeFGRuntimeRegistry.hpp
src/compatibility/xefg/XeFGRuntimeRegistry.cpp
```

Do not create additional files unless there is a concrete compile/ownership reason.

Two files are preferred for R1.

---

## 6.2 Keep in `D3D12Hook.cpp` for R1

Keep all of the following where they are:

```text
install_xefg_api_hooks_if_available() module enumeration bridge
notify_xefg_module_loaded()
mark_xefg_probe_pending()
process_pending_xefg_probe()
XefgInitTransaction
g_xefg_init_transaction_mutex
g_xefg_transaction
g_xefg_factory_hook
create_xefg_swapchain()
xefg_init_desc_common()
GetSwapChainPtr post-call transaction/public-proxy observation
queue identity capture/classification
publish_xefg_candidate()
g_pending_xefg_binding
consume_pending_xefg_binding()
bind_external_swapchain()
replace_xefg_binding()
strong active XeFG COM ownership
binding generation
Present / Present1 handling
ResizeBuffers / ResizeTarget / ResizeBuffers1 handling
resize transition hold
hook-monitor health policy
all existing diagnostic policy
```

Some small wrapper functions in `D3D12Hook.cpp` may become one-line calls into `XeFGRuntimeRegistry`.

That is intentional.

---

## 6.3 Do not modify `REFramework.cpp` in R1

This is a hard scope boundary unless compilation makes a trivial include-only change absolutely unavoidable.

Current loader/probe call sites must remain behaviorally and structurally unchanged in R1.

R2 owns the loader/probe façade cleanup.

If the implementation starts moving:

```text
LdrLoadDll hook logic
LdrRegisterDllNotification logic
libxess_fg.dll filename recognition
hook-monitor calls
```

stop. That is R2 or later.

---

# 7. Expected Changed Files

Preferred R1 diff:

```text
ADD    src/compatibility/xefg/XeFGRuntimeRegistry.hpp
ADD    src/compatibility/xefg/XeFGRuntimeRegistry.cpp
MODIFY src/D3D12Hook.cpp
MODIFY src/D3D12Hook.hpp      only as needed for shared function signatures / bridge declarations
```

Unexpected files that should normally remain untouched:

```text
src/REFramework.cpp
src/REFramework.hpp
src/mods/*
shared/*
cmake.toml
CMakeLists.txt
```

The REFramework target already uses recursive `src/**.cpp` / `src/**.hpp` source globs through the cmkr configuration, so the new compatibility source files should not require build-list maintenance.

Do not edit generated `CMakeLists.txt` just to register these two new source files.

---

# 8. Recommended Runtime Registry API

Exact naming may vary slightly to fit repository style, but keep the API narrow.

A good target shape is:

```cpp
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "utility/FunctionHook.hpp"

class XeFGRuntimeRegistry {
public:
    using InitFn = int32_t (WINAPI*)(
        void* context,
        HWND hwnd,
        const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
        ID3D12CommandQueue* command_queue,
        IDXGIFactory2* factory,
        const void* init_params);

    using GetSwapchainFn = int32_t (WINAPI*)(
        void* context,
        REFIID riid,
        void** swap_chain);

    struct InitDispatchTarget {
        HMODULE module{};
        InitFn original{};
    };

    struct GetSwapchainDispatchTarget {
        HMODULE module{};
        GetSwapchainFn original{};
    };

    static XeFGRuntimeRegistry& instance();

    bool install_for_module(HMODULE module, std::wstring_view full_path);

    std::optional<InitDispatchTarget> resolve_init(size_t slot);
    std::optional<GetSwapchainDispatchTarget> resolve_get_swapchain(size_t slot);

private:
    static constexpr size_t kMaxRuntimes = 8;

    enum class InstallState : uint8_t {
        Empty,
        Installing,
        Active,
    };

    struct RuntimeHook {
        HMODULE module{};
        std::wstring path{};
        size_t slot{};
        FARPROC init_desc_export{};
        FARPROC get_swapchain_export{};
        std::unique_ptr<FunctionHook> init_desc_hook{};
        std::unique_ptr<FunctionHook> get_swapchain_hook{};
        InstallState state{InstallState::Empty};
    };

    RuntimeHook* find_by_module_locked(HMODULE module);
    RuntimeHook* find_by_slot_locked(size_t slot);
    std::optional<size_t> allocate_slot_locked() const;

    std::mutex m_mutex{};
    std::array<std::optional<RuntimeHook>, kMaxRuntimes> m_runtimes{};
};
```

This is an example, not mandatory line-for-line code.

Important properties are mandatory:

- registry owns its mutex;
- registry owns runtime entries;
- registry owns per-runtime `FunctionHook`s;
- caller receives a copied exact module/original dispatch target;
- callers never receive mutable runtime-entry pointers that outlive the registry lock;
- no active binding/render state appears in this class.

---

# 9. Stable Thunk Design

The fixed thunk arrays should move with the runtime registry implementation because they exist solely to provide stable runtime dispatch identity.

Example:

```cpp
namespace {

template <size_t Slot>
int32_t WINAPI xefg_init_desc_thunk(
    void* context,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    ID3D12CommandQueue* command_queue,
    IDXGIFactory2* factory,
    const void* init_params)
{
    return D3D12Hook::xefg_init_desc_dispatch(
        Slot,
        context,
        hwnd,
        swap_chain_desc,
        fullscreen_desc,
        command_queue,
        factory,
        init_params);
}

template <size_t Slot>
int32_t WINAPI xefg_get_swapchain_thunk(
    void* context,
    REFIID riid,
    void** swap_chain)
{
    return D3D12Hook::xefg_get_swapchain_dispatch(
        Slot,
        context,
        riid,
        swap_chain);
}

constexpr std::array<XeFGRuntimeRegistry::InitFn, 8> kInitThunks{
    &xefg_init_desc_thunk<0>,
    &xefg_init_desc_thunk<1>,
    &xefg_init_desc_thunk<2>,
    &xefg_init_desc_thunk<3>,
    &xefg_init_desc_thunk<4>,
    &xefg_init_desc_thunk<5>,
    &xefg_init_desc_thunk<6>,
    &xefg_init_desc_thunk<7>,
};

constexpr std::array<XeFGRuntimeRegistry::GetSwapchainFn, 8> kGetSwapchainThunks{
    &xefg_get_swapchain_thunk<0>,
    &xefg_get_swapchain_thunk<1>,
    &xefg_get_swapchain_thunk<2>,
    &xefg_get_swapchain_thunk<3>,
    &xefg_get_swapchain_thunk<4>,
    &xefg_get_swapchain_thunk<5>,
    &xefg_get_swapchain_thunk<6>,
    &xefg_get_swapchain_thunk<7>,
};

} // namespace
```

A temporary dependency from `XeFGRuntimeRegistry.cpp` to the existing public static `D3D12Hook::xefg_*_dispatch()` functions is acceptable in R1.

Do **not** invent a callback bus, `std::function` dispatch layer, provider interface, or generic compatibility event framework to avoid this temporary bridge.

Later PRs remove the temporary bridge as ownership boundaries improve.

---

# 10. Export Installation Example

The new registry should preserve the current installation semantics.

Illustrative implementation:

```cpp
bool XeFGRuntimeRegistry::install_for_module(
    HMODULE module,
    std::wstring_view full_path)
{
    if (module == nullptr) {
        return false;
    }

    std::scoped_lock lock{m_mutex};

    if (find_by_module_locked(module) != nullptr) {
        spdlog::info(
            "[XeFG][RuntimeRegistry] action = duplicate, module = 0x{:x}, path = {}",
            reinterpret_cast<uintptr_t>(module),
            utility::narrow(std::wstring{full_path}));
        return true;
    }

    const auto init_export = GetProcAddress(
        module,
        "xefgSwapChainD3D12InitFromSwapChainDesc");

    if (init_export == nullptr) {
        spdlog::warn(
            "[XeFG][RuntimeRegistry] action = rejected, module = 0x{:x}, path = {}, reason = init_desc_missing",
            reinterpret_cast<uintptr_t>(module),
            utility::narrow(std::wstring{full_path}));
        return false;
    }

    const auto slot = allocate_slot_locked();
    if (!slot) {
        spdlog::error(
            "[XeFG][RuntimeRegistry] action = rejected, module = 0x{:x}, reason = capacity_exceeded, capacity = {}",
            reinterpret_cast<uintptr_t>(module),
            kMaxRuntimes);
        return false;
    }

    auto& runtime = m_runtimes[*slot].emplace();
    runtime.module = module;
    runtime.path = std::wstring{full_path};
    runtime.slot = *slot;
    runtime.init_desc_export = init_export;
    runtime.get_swapchain_export = GetProcAddress(
        module,
        "xefgSwapChainD3D12GetSwapChainPtr");
    runtime.state = InstallState::Installing;

    runtime.init_desc_hook = std::make_unique<FunctionHook>(
        Address{reinterpret_cast<void*>(runtime.init_desc_export)},
        Address{reinterpret_cast<void*>(kInitThunks[*slot])});

    if (!runtime.init_desc_hook->create()) {
        spdlog::error(
            "[XeFG][RuntimeRegistry] action = rejected, slot = {}, module = 0x{:x}, reason = init_hook_failed",
            *slot,
            reinterpret_cast<uintptr_t>(module));
        m_runtimes[*slot].reset();
        return false;
    }

    if (runtime.get_swapchain_export != nullptr) {
        runtime.get_swapchain_hook = std::make_unique<FunctionHook>(
            Address{reinterpret_cast<void*>(runtime.get_swapchain_export)},
            Address{reinterpret_cast<void*>(kGetSwapchainThunks[*slot])});

        if (!runtime.get_swapchain_hook->create()) {
            spdlog::warn(
                "[XeFG][RuntimeRegistry] api = GetSwapChainPtr, action = optional_hook_failed, slot = {}, module = 0x{:x}",
                *slot,
                reinterpret_cast<uintptr_t>(module));
            runtime.get_swapchain_hook.reset();
        }
    }

    runtime.state = InstallState::Active;

    spdlog::info(
        "[XeFG][RuntimeRegistry] action = installed, slot = {}, module = 0x{:x}, path = {}, init_desc = 0x{:x}, get_swapchain = 0x{:x}",
        *slot,
        reinterpret_cast<uintptr_t>(module),
        utility::narrow(runtime.path),
        reinterpret_cast<uintptr_t>(runtime.init_desc_export),
        reinterpret_cast<uintptr_t>(runtime.get_swapchain_export));

    return true;
}
```

Do not "improve" the log wording/levels in this PR unless required by a compile issue.

The final logging PR will handle verbosity and Debug Logging policy.

---

# 11. Dispatch Resolution Example

Do not expose the registry's internal runtime record directly.

Return a small copied dispatch target instead.

Example:

```cpp
std::optional<XeFGRuntimeRegistry::InitDispatchTarget>
XeFGRuntimeRegistry::resolve_init(size_t slot)
{
    std::scoped_lock lock{m_mutex};

    auto* runtime = find_by_slot_locked(slot);
    if (runtime == nullptr
        || runtime->state != InstallState::Active
        || runtime->init_desc_hook == nullptr) {
        return std::nullopt;
    }

    return InitDispatchTarget{
        .module = runtime->module,
        .original = reinterpret_cast<InitFn>(
            runtime->init_desc_hook->get_original()),
    };
}
```

Equivalent GetSwapChainPtr lookup:

```cpp
std::optional<XeFGRuntimeRegistry::GetSwapchainDispatchTarget>
XeFGRuntimeRegistry::resolve_get_swapchain(size_t slot)
{
    std::scoped_lock lock{m_mutex};

    auto* runtime = find_by_slot_locked(slot);
    if (runtime == nullptr
        || runtime->state != InstallState::Active
        || runtime->get_swapchain_hook == nullptr) {
        return std::nullopt;
    }

    return GetSwapchainDispatchTarget{
        .module = runtime->module,
        .original = reinterpret_cast<GetSwapchainFn>(
            runtime->get_swapchain_hook->get_original()),
    };
}
```

Because R1 does not implement runtime unload/removal, the copied trampoline pointer remains tied to an owned active registry entry exactly as in current master.

Do not add unload support in this PR.

---

# 12. Required Temporary `D3D12Hook` Bridge

## 12.1 `install_xefg_api_hooks_for_module()`

Keep the existing D3D12Hook-facing API for R1 so callers do not change.

It may become a thin wrapper:

```cpp
void D3D12Hook::install_xefg_api_hooks_for_module(
    HMODULE module,
    std::wstring_view full_path)
{
    XeFGRuntimeRegistry::instance().install_for_module(module, full_path);
}
```

Do not change `REFramework.cpp` to call the registry directly yet.

That is R2.

---

## 12.2 `xefg_init_desc_dispatch()`

The existing `D3D12Hook` dispatch function should no longer inspect a global runtime array.

It should ask the registry for the exact target and then enter the existing common transaction body.

Example:

```cpp
int32_t D3D12Hook::xefg_init_desc_dispatch(
    size_t slot,
    void* context,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    ID3D12CommandQueue* command_queue,
    IDXGIFactory2* factory,
    const void* init_params)
{
    const auto target =
        XeFGRuntimeRegistry::instance().resolve_init(slot);

    if (!target) {
        spdlog::error(
            "[XeFG][RuntimeDispatch] api = InitFromSwapChainDesc, slot = {}, action = fail, reason = runtime_not_active",
            slot);
        return -1;
    }

    spdlog::info(
        "[XeFG][RuntimeDispatch] api = InitFromSwapChainDesc, slot = {}, module = 0x{:x}, context = 0x{:x}",
        slot,
        reinterpret_cast<uintptr_t>(target->module),
        reinterpret_cast<uintptr_t>(context));

    return xefg_init_desc_common(
        slot,
        target->module,
        target->original,
        context,
        hwnd,
        swap_chain_desc,
        fullscreen_desc,
        command_queue,
        factory,
        init_params);
}
```

The existing `xefg_init_desc_common()` implementation must remain behaviorally unchanged.

Do not move it in R1.

---

## 12.3 `xefg_get_swapchain_dispatch()`

Likewise, resolve only the correct original through the registry.

Then preserve the rest of the current D3D12Hook-owned behavior, including current post-call diagnostics/transaction observation.

Conceptually:

```cpp
const auto target =
    XeFGRuntimeRegistry::instance().resolve_get_swapchain(slot);

if (!target) {
    // preserve current failure behavior/log
    return -1;
}

const auto result = target->original(context, riid, swap_chain);

// KEEP current post-call observation/logging exactly where it is in R1.
// Do not move transaction/public-proxy policy into the registry.
```

The registry owns **which original function belongs to this runtime**, not what REFramework does with the returned swapchain.

---

# 13. Mutex / Concurrency Rules

The current implementation uses `g_xefg_state_mutex` for several unrelated XeFG responsibilities.

R1 may split runtime-registry synchronization from the remaining discovery/pending-binding synchronization because the registry now owns its own data.

Required result:

```text
XeFGRuntimeRegistry::m_mutex
    guards only runtime table / hook ownership / slot lookup

g_xefg_state_mutex (existing)
    remains with existing discovery / transaction-adjacent / pending state
```

Do not export the registry mutex to D3D12Hook.

Do not make D3D12Hook lock the registry mutex manually.

Do not hold both mutexes at the same time unless existing behavior makes it unavoidable. The preferred R1 design requires no nested registry/state lock.

Critically:

```text
resolve runtime target under registry mutex
-> copy target
-> release registry mutex
-> invoke existing lower-level path
```

Do not invoke original XeFG exports while the registry mutex is held.

---

# 14. Function-by-Function Migration Map

Use this map during implementation review.

| Current symbol / responsibility | R1 destination | Notes |
|---|---|---|
| `kMaxXefgRuntimes` | `XeFGRuntimeRegistry` | Keep value 8 |
| `XefgRuntimeInstallState` | `XeFGRuntimeRegistry` | Same states |
| `XefgRuntimeHook` | `XeFGRuntimeRegistry` | Registry owns `FunctionHook`s |
| `g_xefg_runtimes` | `XeFGRuntimeRegistry` | No equivalent global left in D3D12Hook.cpp |
| `find_xefg_runtime()` | registry private helper | exact HMODULE |
| `get_xefg_runtime_by_slot()` | registry private helper | slot lookup |
| `allocate_xefg_runtime_slot()` | registry private helper | no recycling |
| `xefg_init_desc_thunk<>()` | registry `.cpp` | still calls temporary D3D12Hook dispatch bridge |
| `xefg_get_swapchain_thunk<>()` | registry `.cpp` | same |
| `g_xefg_init_thunks` | registry `.cpp` | stable fixed table |
| `g_xefg_get_swapchain_thunks` | registry `.cpp` | stable fixed table |
| export `GetProcAddress` | registry | exact supplied HMODULE only |
| InitDesc `FunctionHook` | registry | required |
| GetSwapChainPtr `FunctionHook` | registry | optional |
| `install_xefg_api_hooks_for_module()` | thin D3D12Hook wrapper | caller compatibility until R2 |
| `install_xefg_api_hooks_if_available()` | KEEP in D3D12Hook | enumeration moves later |
| `notify_xefg_module_loaded()` | KEEP in D3D12Hook | R2 |
| `xefg_init_desc_dispatch()` | KEEP temporary bridge | lookup runtime via registry |
| `xefg_get_swapchain_dispatch()` | KEEP temporary bridge | lookup runtime via registry |
| `xefg_init_desc_common()` | KEEP unchanged | R3 scope |
| `create_xefg_swapchain()` | KEEP unchanged | R3 scope |
| queue validation | KEEP unchanged | R4 scope |
| pending candidate | KEEP unchanged | R5 scope |
| active binding / COM ownership | KEEP unchanged | R6/R7 scope |
| resize lifecycle | KEEP unchanged | R8 scope |
| Present policy | KEEP unchanged | R9 scope |
| hook monitor | KEEP unchanged | R10 scope |

---

# 15. Step-by-Step Implementation Order

Follow a mechanical sequence to minimize accidental behavior changes.

## Step 1 — Add registry header

Create:

```text
src/compatibility/xefg/XeFGRuntimeRegistry.hpp
```

Define only:

- XeFG public export function pointer types;
- copied dispatch target types;
- install/resolve API;
- private runtime record;
- private install state;
- private mutex/table helpers.

Do not reference renderer, swapchain binding generation, resize state, or REFramework UI.

---

## Step 2 — Add registry implementation

Create:

```text
src/compatibility/xefg/XeFGRuntimeRegistry.cpp
```

Move:

- runtime slot helpers;
- fixed thunks;
- fixed thunk arrays;
- export lookup;
- per-runtime FunctionHook creation;
- install state transition;
- dispatch-target resolution.

Keep existing log messages and levels as close to byte-for-byte equivalent as practical.

---

## Step 3 — Replace D3D12Hook runtime globals with registry calls

Remove from `D3D12Hook.cpp`:

- runtime table;
- runtime record;
- runtime install enum;
- runtime slot allocation/find helpers;
- thunk definitions;
- thunk arrays.

Do not remove the remaining XeFG transaction/pending state.

---

## Step 4 — Convert install function into a bridge

Keep the existing public/static D3D12Hook function used by current callers.

Its implementation should delegate to the registry.

Do not modify loader call sites.

---

## Step 5 — Convert runtime dispatch to registry lookup

For InitDesc and optional GetSwapChainPtr:

```text
slot
-> registry resolve
-> exact module + original trampoline copy
-> existing D3D12Hook lower path
```

Do not move the common InitDesc transaction body.

Do not move GetSwapChainPtr observation logic.

---

## Step 6 — Compile before any cleanup

Build immediately after the mechanical extraction.

Fix compile errors only.

Do not use the extraction as an excuse to rename unrelated XeFG fields or reformat large parts of `D3D12Hook.cpp`.

---

## Step 7 — Perform a scope audit

Before finalizing, verify R2-R10 logic was not accidentally pulled in.

Use the acceptance audit below.

---

# 16. Explicitly Forbidden Changes

R1 must **not** do any of the following.

## Architecture scope creep

- no `XeFGCompatibility` façade wiring from `REFramework.cpp` yet;
- no generic `IFrameGenerationProvider`;
- no generic runtime/plugin provider registry;
- no event bus;
- no background worker;
- no polling loop;
- no timer-based runtime discovery;
- no runtime unload/reload architecture;
- no dynamic slot growth;
- no slot reuse design.

## XeFG behavior changes

- do not change queue selection;
- do not change candidate validation;
- do not change internal presentation swapchain authority;
- do not bind public `GetSwapChainPtr` proxy;
- do not change observe-only policy;
- do not change initial bind behavior;
- do not change rebind behavior;
- do not change binding generation semantics;
- do not change Present/Present1 hook slots;
- do not change resize hook slots;
- do not change ResizeTarget hold behavior;
- do not change hook-monitor preservation.

## Other technology changes

- no FSRFG changes;
- no DLSSG/Streamline changes;
- no D3D11 changes;
- no Special K work;
- no OptiScaler private hook;
- no hooking-library replacement.

## Logging changes

- do not demote current INFO diagnostics to DEBUG yet;
- do not add the UI Debug Logging checkbox yet;
- do not redesign log categories;
- do not remove existing support logs.

Logging is intentionally the final refactor PR.

---

# 17. LOC / Diff Budget

Planning target:

```text
effective implementation change: ~180–280 LOC
soft ceiling:                    ~350 LOC
GitHub additions+deletions:      ~350–600 LOC expected because of movement
```

Do not optimize for a numeric diff at the cost of clean ownership.

However, if R1 starts exceeding roughly 350 effective LOC or requires significant edits outside the expected four files, stop and reassess whether later-PR responsibilities are leaking into R1.

Large delete+add counts caused by moving existing code are acceptable.

Large amounts of **new policy logic** are not.

---

# 18. Build / Static Validation

At minimum run the repository's normal Release build path used by the current project.

Expected configuration from repository documentation:

```text
cmake -B build
cmake --build build --config Release
```

Use the project's established build environment/commands if the checkout already has a configured build directory.

Required checks:

```text
Release build succeeds
git diff --check succeeds
no unresolved symbols
no duplicate XeFG thunk definitions
no duplicate runtime table remains
new source files are compiled through existing src/** glob
```

Also inspect the final diff for accidental generated-file changes.

Do not commit build output.

---

# 19. Required Source Audit After Implementation

The final implementation should satisfy all of these.

## Registry ownership audit

Search for the runtime table / runtime hook ownership.

Expected:

```text
XefgRuntimeHook / equivalent runtime record
    -> only in XeFGRuntimeRegistry

8-slot runtime array
    -> only in XeFGRuntimeRegistry

per-runtime init_desc_hook / get_swapchain_hook
    -> only in XeFGRuntimeRegistry

fixed thunk arrays
    -> only in XeFGRuntimeRegistry.cpp
```

## D3D12Hook audit

Expected to remain in D3D12Hook after R1:

```text
xefg_init_desc_common
create_xefg_swapchain
queue validation
candidate publication
pending binding
active binding
resize lifecycle
Present policy
```

If those moved, R1 scope was exceeded.

## REFramework core audit

Expected:

```text
src/REFramework.cpp
    -> no functional R1 changes
```

## Other FG audit

Expected:

```text
Streamline/DLSSG code
    -> unchanged

FSRFG behavior
    -> unchanged
```

---

# 20. Runtime Validation Policy for R1

R1 is the first half of the R1+R2 runtime wave.

Mandatory R1 merge evidence is primarily:

```text
Release build
static/source ownership audit
diff review
```

If a suitable XeFG test machine/game is immediately available, a launch smoke is useful but not required to expand this PR.

The **mandatory game-level wave gate** is after R2.

After R1+R2, validate at least:

```text
DD2 + OptiScaler + XeFG
    game launches
    XeFG initializes
    OptiScaler overlay visible
    REFramework overlay visible
    no repeated runtime-hook install loop
    no startup crash
```

And where available:

```text
known multi-runtime / Pragmata scenario
    exact-HMODULE runtime handling remains intact
```

Do not add game-specific code to make a smoke test pass.

---

# 21. Acceptance Criteria

R1 is complete only when all of the following are true.

## Architecture

- [ ] `XeFGRuntimeRegistry.hpp/.cpp` exists under `src/compatibility/xefg/`.
- [ ] Runtime table ownership is removed from `D3D12Hook.cpp`.
- [ ] Per-runtime `FunctionHook` ownership is removed from `D3D12Hook.cpp`.
- [ ] Fixed slot thunk arrays are owned by the runtime registry implementation.
- [ ] Exact `HMODULE` remains the runtime identity.
- [ ] Runtime capacity remains 8.
- [ ] No unload/reload architecture is added.

## Dispatch correctness

- [ ] InitDesc thunk slot resolves the matching runtime's original trampoline.
- [ ] GetSwapChainPtr thunk slot resolves the matching runtime's original trampoline.
- [ ] Registry mutex is released before original XeFG functions are invoked.
- [ ] Duplicate exact HMODULE remains idempotent.
- [ ] Missing required InitDesc rejects only that runtime.
- [ ] Optional GetSwapChainPtr failure does not disable InitDesc support.

## Behavior preservation

- [ ] `xefg_init_desc_common()` behavior is not redesigned.
- [ ] factory capture is not redesigned.
- [ ] queue validation is not changed.
- [ ] candidate publication is not changed.
- [ ] active binding/rebind is not changed.
- [ ] Present/resize behavior is not changed.
- [ ] hook monitor is not changed.
- [ ] FSRFG/DLSSG/D3D11 are not changed.

## Upstream-sensitive scope

- [ ] `REFramework.cpp` has no functional R1 changes.
- [ ] No unrelated REFramework cleanup is included.
- [ ] No generated CMake file change is included solely for new source registration.

## Quality

- [ ] Release build succeeds.
- [ ] `git diff --check` succeeds.
- [ ] No new warnings attributable to R1.
- [ ] Final diff is reviewed for accidental scope creep.

---

# 22. PR Description Requirements

The PR description should explicitly state that this is a **behavior-preserving extraction**.

Recommended summary:

```markdown
## Summary

- extract the exact-HMODULE XeFG runtime registry from `D3D12Hook.cpp`
- move stable 8-slot runtime thunks and per-runtime export `FunctionHook` ownership into `XeFGRuntimeRegistry`
- preserve the current D3D12Hook loader/probe/discovery/binding path through temporary bridge methods
- no intended runtime behavior change

## Not in scope

- loader/probe façade cleanup (R2)
- InitDesc/factory discovery extraction (R3)
- queue/candidate extraction (R4)
- binding/rebind changes (R5-R7)
- resize/Present/hook-monitor cleanup (R8-R10)
- logging cleanup / Debug Logging UI

## Validation

- Release build: PASS
- `git diff --check`: PASS
- source ownership audit: PASS
```

Do not claim runtime validation unless it was actually performed.

---

# 23. Agent Final Report Requirements

When implementation is finished, report:

1. exact base commit used;
2. branch name;
3. files added/modified;
4. effective scope summary;
5. Git diff stats;
6. build command and result;
7. `git diff --check` result;
8. whether runtime smoke was performed;
9. confirmation that `REFramework.cpp` was not functionally changed;
10. confirmation that R2-R10 work was not included;
11. any unavoidable deviation from this work order and why.

If implementation reveals that the registry cannot be extracted without changing discovery/binding behavior, do **not** silently expand scope. Keep the safest compilable R1 subset and report the blocking coupling for review.

---

# 24. Definition of Done

R1 is done when the code has one clear answer to this question:

> Who owns the loaded XeFG runtime modules, their exact-HMODULE identity, stable dispatch slots, exported XeFG function hooks, and matching original trampolines?

After R1 the answer must be:

```text
XeFGRuntimeRegistry
```

And the answer to these questions must still be:

```text
Who owns XeFG init/factory discovery transaction?
    -> existing D3D12Hook path (until R3)

Who owns candidate validation?
    -> existing D3D12Hook path (until R4)

Who owns active XeFG binding/rebind?
    -> existing D3D12Hook path (until R6/R7)

Who owns resize/Present physical callbacks?
    -> D3D12Hook

Who owns hook-monitor policy?
    -> current existing path (until R10)
```

That narrow ownership transfer is the entire purpose of this PR.
