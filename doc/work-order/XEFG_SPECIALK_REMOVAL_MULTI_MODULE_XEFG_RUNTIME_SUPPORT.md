# Work Order: Multi-Module XeFG Runtime Support for OptiScaler 10.x

## Status

Implementation work order for the next compatibility PR after P3.3A diagnostics.

This work is **independent of the MHW Alt+Enter lifecycle investigation**. MHW remains an Intel-only runtime validation track. This work addresses a separate, already-observed loader/discovery failure exposed by OptiScaler 10.x packaging and multi-module XeFG runtime topology.

Recommended PR identity:

```text
PR #14
Multi-module XeFG runtime discovery and API hook dispatch
```

Suggested branch:

```text
fix/xefg-multi-module-runtime-hooks
```

Target base: current `master` after PR #13 squash merge.

---

# 1. Objective

Replace REFramework's current **single-`libxess_fg.dll` module assumption** with a safe **per-HMODULE XeFG runtime registry** so that multiple copies of `libxess_fg.dll` can coexist in the same process and the XeFG runtime that actually executes `xefgSwapChainD3D12InitFromSwapChainDesc` is always intercepted.

The target outcome is:

```text
OptiScaler 0.9.x layout
  one XeFG runtime
  -> existing working behavior remains unchanged

OptiScaler 10.x layout
  two or more libxess_fg.dll modules may coexist
  -> REFramework hooks every relevant XeFG public runtime instance
  -> the runtime actually used by OptiScaler reaches the existing InitDesc capture path
  -> internal post-FG presentation swapchain is captured
  -> existing P2/P2.1/P2.2/P3.1/P3.2 validation and binding logic runs unchanged
  -> REF overlay becomes available
```

This is **not AMD-specific code**. AMD Pragmata is the first clear reproducer, but the architectural issue is generic:

> A process can contain multiple loaded modules with the same basename `libxess_fg.dll`, and REFramework must treat HMODULE identity, not basename identity, as authoritative.

---

# 2. Why This Work Is Needed

## 2.1 OptiScaler 0.9.x vs 10.x loading-layout change

The relevant upstream branches are:

```text
OptiScaler release/0.9
head observed during analysis:
8dac650cbf90c85ca1d46747a66fc98118be75b8

OptiScaler master / 10.x
head observed during analysis:
da70e61e1542a0b99adcb24168ff941e42109567
```

The XeFG API path itself is not the important difference. Both branches still use the XeFG public API and D3D12 `InitFromSwapChainDesc` path.

The important difference is the default main runtime directory.

### release/0.9

Default `MainDllPath`:

```cpp
if (!Config::Instance()->MainDllPath.has_value())
{
    Config::Instance()->MainDllPath.set_volatile_value(L".");
}
```

The 0.9 INI also documents:

```text
[Libraries]
; Main folder for OptiScaler to check dll files below
; Default is .\
OptiDllPath=auto
```

This naturally keeps XeFG runtime lookup near the game executable/root layout.

### master / OptiScaler 10.x

Default `MainDllPath`:

```cpp
if (!Config::Instance()->MainDllPath.has_value())
{
    Config::Instance()->MainDllPath.set_volatile_value(L"OptiScaler");
}
```

The master INI documents:

```text
[Libraries]
; Main folder for OptiScaler to check dll files below
; Default is .\OptiScaler
OptiDllPath=auto
```

This means OptiScaler 10.x can load its own XeFG runtime from:

```text
<Game>\OptiScaler\libxess_fg.dll
```

while another `libxess_fg.dll` with the same basename may already exist elsewhere in the process.

This is the key compatibility difference this work order must account for.

---

# 3. Runtime Evidence: AMD Pragmata / OptiScaler 10.x

The AMD Pragmata session exposed two different XeFG runtime modules in the same process.

Observed topology:

```text
Module A
  path approximately:
  <game>\_storage_\libxess_fg.dll
  base: 0x7FFE791F0000

Module B
  path approximately:
  <game>\OptiScaler\libxess_fg.dll
  base: 0x7FFE6E810000
```

REFramework hooked Module A's XeFG exports:

```text
[XeFG][ApiHook] InitFromSwapChainDesc = 0x7ffe791fa380
[XeFG][ApiHook] GetSwapChainPtr        = 0x7ffe791f7050
```

But the public `XefgInterpolationSwapChain` actually used by the running OptiScaler path had vtable ownership under Module B:

```text
Present[8]        owner = <game>\OptiScaler\libxess_fg.dll
ResizeBuffers[13] owner = <game>\OptiScaler\libxess_fg.dll
ResizeTarget[14]  owner = <game>\OptiScaler\libxess_fg.dll
Present1[22]      owner = <game>\OptiScaler\libxess_fg.dll
ResizeBuffers1[39] owner = <game>\OptiScaler\libxess_fg.dll
```

Result:

```text
XeFG context/swapchain creation: succeeds in OptiScaler
OptiScaler Present path: continues
REF XeFG InitDesc interception: 0
REF InternalSwapchain capture: 0
REF ExternalBind: 0
REF overlay: unavailable
```

This is not evidence that the public proxy should be bound directly. It is evidence that REFramework intercepted the wrong XeFG runtime module.

---

# 4. Current REFramework Failure Mechanism

Current code maintains only one pair of XeFG API hooks:

```cpp
std::unique_ptr<FunctionHook> g_xefg_init_hook{};
std::unique_ptr<FunctionHook> g_xefg_get_swapchain_hook{};
```

Current installation also relies on basename lookup:

```cpp
const auto module = GetModuleHandleW(L"libxess_fg.dll");
if (module == nullptr) {
    return;
}
```

Then:

```cpp
const auto init_export =
    GetProcAddress(module, "xefgSwapChainD3D12InitFromSwapChainDesc");

if (init_export != nullptr && g_xefg_init_hook == nullptr) {
    ...
}
```

This has two structural failures.

## 4.1 Basename lookup loses exact module identity

`GetModuleHandleW(L"libxess_fg.dll")` is not a valid identity mechanism when multiple modules with the same basename are loaded from different full paths.

The code already receives the exact HMODULE in the loader handoff:

```cpp
const auto xefg_module = static_cast<HMODULE>(*module);
D3D12Hook::notify_xefg_module_loaded(
    xefg_module,
    L"libxess_fg.dll",
    std::wstring_view{path, path_length});
```

But `notify_xefg_module_loaded()` currently calls a hook installer that performs another basename-only lookup. The exact HMODULE is effectively discarded for installation purposes.

That must be removed.

## 4.2 A single global FunctionHook cannot represent multiple runtime copies

Even if every loaded module were enumerated, this structure is still insufficient:

```cpp
g_xefg_init_hook
g_xefg_get_swapchain_hook
```

Each loaded module has its own export address and therefore its own original trampoline.

The detour callback must know which runtime instance it came from in order to call the correct original function.

---

# 5. Non-Goals

Do **not** mix this work with MHW fullscreen lifecycle work.

Do not implement any of the following in this PR:

- MHW Alt+Enter fix
- resize/reset coalescing
- hook-monitor timeout changes
- P3.2 rebind behavior changes
- presentation queue policy changes
- candidate validation changes
- public `XefgInterpolationSwapChain` rendering
- RTTI-based XeFG classification as a binding authority
- module-owner filename as a binding authority
- Intel/AMD/NVIDIA GPU-specific branches
- hardcoded `OptiScaler` directory matching
- Special K compatibility work
- Requiem stutter work
- generic D3D12 refactor

Also do not expand this PR into a complete module-unload architecture unless required by static correctness. Runtime unload/reload handling is a separate lifecycle problem and should remain a follow-up unless implementation safety absolutely requires a minimal guard.

---

# 6. Preserve Existing Proven P2/P3 Architecture

The existing architecture below is still correct and must remain the core presentation-binding path:

```text
XeFG public InitFromSwapChainDesc
  -> temporarily hook supplied IDXGIFactory2::CreateSwapChainForHwnd[15]
  -> capture factory-returned internal post-FG presentation swapchain
  -> call original XeFG init
  -> remove temporary factory hook
  -> validate candidate
  -> validate presentation queue/device identity
  -> publish PendingXefgBinding
  -> bind/rebind existing D3D12Hook
  -> Present[8] + Present1[22]
  -> ResizeBuffers[13] + ResizeTarget[14] + ResizeBuffers1[39]
```

The multi-module work belongs **above** this path.

Desired layering:

```text
NEW
XeFG runtime registry / module-specific API hook dispatch
       |
       v
EXISTING
xefg_init_from_swapchain_desc common transaction logic
       |
       v
EXISTING
factory candidate capture
       |
       v
EXISTING
publish_xefg_candidate()
       |
       v
EXISTING
P3.1/P3.2 ownership + atomic binding
```

Do not rewrite a proven lower layer to solve an upper-layer module-selection bug.

---

# 7. Required Design

## 7.1 Introduce a per-runtime registry

Replace the single-module hook globals with a small registry keyed by exact HMODULE identity.

Suggested shape:

```cpp
struct XefgRuntimeHook {
    HMODULE module{};
    std::wstring path{};

    void* init_desc_export{};
    void* get_swapchain_export{};

    std::unique_ptr<FunctionHook> init_desc_hook{};
    std::unique_ptr<FunctionHook> get_swapchain_hook{};

    size_t dispatch_slot{};
};
```

Possible container:

```cpp
constexpr size_t kMaxXefgRuntimes = 8;

std::array<std::optional<XefgRuntimeHook>, kMaxXefgRuntimes>
    g_xefg_runtimes{};
```

A vector is acceptable, but a fixed slot table is preferred because the detour target needs a stable runtime dispatch identity.

Recommended invariant:

```text
one HMODULE == one runtime registry entry == one stable dispatch slot
```

Registration must be idempotent.

If the same HMODULE is observed multiple times:

```text
same exact module pointer
-> do not create another FunctionHook
-> log duplicate observation at debug/info level as appropriate
-> return existing slot
```

Do not deduplicate by full path alone. HMODULE identity is authoritative for the current loaded instance.

---

## 7.2 Remove basename-only hook installation

The install API should accept the exact module explicitly.

Bad current shape:

```cpp
void install_xefg_api_hooks_if_available() {
    const auto module = GetModuleHandleW(L"libxess_fg.dll");
    ...
}
```

Required direction:

```cpp
bool install_xefg_api_hooks_for_module(
    HMODULE module,
    std::wstring_view full_path);
```

Example:

```cpp
bool D3D12Hook::install_xefg_api_hooks_for_module(
    HMODULE module,
    std::wstring_view full_path)
{
    if (module == nullptr) {
        return false;
    }

    std::scoped_lock lock{g_xefg_state_mutex};

    if (auto* existing = find_xefg_runtime(module)) {
        return true;
    }

    const auto init_desc = GetProcAddress(
        module,
        "xefgSwapChainD3D12InitFromSwapChainDesc");

    const auto get_swapchain = GetProcAddress(
        module,
        "xefgSwapChainD3D12GetSwapChainPtr");

    if (init_desc == nullptr) {
        spdlog::warn(
            "[XeFG][RuntimeRegistry] module = 0x{:x}, path = {}, action = reject, reason = init_desc_missing",
            reinterpret_cast<uintptr_t>(module),
            utility::narrow(std::wstring{full_path}));
        return false;
    }

    // Allocate stable slot, create module-specific hooks,
    // then commit registry entry only after required hook creation succeeds.
    ...
}
```

No code in the installation path should resolve the module again by basename once an HMODULE has already been supplied.

---

# 8. Required Per-Runtime Detour Dispatch

## 8.1 Why one callback + one global original pointer is unsafe

Current callback behavior conceptually does:

```cpp
const auto original =
    reinterpret_cast<XefgInitFn>(g_xefg_init_hook->get_original());
```

With two different runtime modules:

```text
Module A InitDesc export -> trampoline A
Module B InitDesc export -> trampoline B
```

A callback must call the matching trampoline.

Therefore simply storing multiple FunctionHooks while keeping one destination callback is not enough unless the callback can recover the source runtime identity reliably.

Do not infer runtime identity from:

- return-address filename heuristics
- thread-local guesses without explicit setup
- current public swapchain vtable owner
- last-loaded module
- basename lookup

Use explicit static dispatch identity.

---

## 8.2 Recommended template dispatch slots

Use compile-time thunks that encode the runtime slot in the target function itself.

Suggested pattern:

```cpp
using XefgInitDescFn = int32_t (WINAPI*)(
    void* context,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    ID3D12CommandQueue* command_queue,
    IDXGIFactory2* factory,
    const void* init_params);

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
    return xefg_init_desc_dispatch(
        Slot,
        context,
        hwnd,
        swap_chain_desc,
        fullscreen_desc,
        command_queue,
        factory,
        init_params);
}
```

Provide a fixed thunk table:

```cpp
using XefgInitDescDetour = XefgInitDescFn;

constexpr std::array<XefgInitDescDetour, kMaxXefgRuntimes>
    kXefgInitDescThunks{
        &xefg_init_desc_thunk<0>,
        &xefg_init_desc_thunk<1>,
        &xefg_init_desc_thunk<2>,
        &xefg_init_desc_thunk<3>,
        &xefg_init_desc_thunk<4>,
        &xefg_init_desc_thunk<5>,
        &xefg_init_desc_thunk<6>,
        &xefg_init_desc_thunk<7>,
    };
```

Then dispatch through the exact runtime slot:

```cpp
int32_t xefg_init_desc_dispatch(
    size_t slot,
    void* context,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    ID3D12CommandQueue* command_queue,
    IDXGIFactory2* factory,
    const void* init_params)
{
    XefgInitDescFn original{};

    {
        std::scoped_lock lock{g_xefg_state_mutex};

        auto* runtime = get_xefg_runtime_by_slot(slot);
        if (runtime == nullptr || runtime->init_desc_hook == nullptr) {
            spdlog::critical(
                "[XeFG][RuntimeDispatch] slot = {}, action = fail, reason = runtime_missing",
                slot);
            return /* safe XeFG error value if available */;
        }

        original = runtime->init_desc_hook->get_original<XefgInitDescFn>();
    }

    return xefg_init_desc_common(
        slot,
        original,
        context,
        hwnd,
        swap_chain_desc,
        fullscreen_desc,
        command_queue,
        factory,
        init_params);
}
```

Important: do **not** hold `g_xefg_state_mutex` while invoking the original XeFG function. The original function will cause factory callbacks and needs access to state managed by the common transaction path.

The exact function decomposition can differ, but runtime identity and matching original trampoline must remain explicit.

---

# 9. GetSwapChainPtr Must Also Be Per-Runtime

Apply the same dispatch model to:

```text
xefgSwapChainD3D12GetSwapChainPtr
```

Suggested pattern:

```cpp
using XefgGetSwapchainFn = int32_t (WINAPI*)(
    void* context,
    REFIID riid,
    void** swap_chain);

template <size_t Slot>
int32_t WINAPI xefg_get_swapchain_thunk(
    void* context,
    REFIID riid,
    void** swap_chain)
{
    return xefg_get_swapchain_dispatch(
        Slot,
        context,
        riid,
        swap_chain);
}
```

Do not leave `GetSwapChainPtr` on a single global hook if InitDesc is converted to multi-runtime.

Although `GetSwapChainPtr` is currently diagnostic/supporting rather than the primary candidate source, mismatched original dispatch would still be unsafe.

---

# 10. Preserve a Common InitDesc Transaction Path

Do not clone P2 logic once per runtime.

After resolving the module-specific original function, route all runtimes through one common body.

Recommended decomposition:

```cpp
int32_t xefg_init_desc_common(
    size_t runtime_slot,
    XefgInitDescFn original,
    void* context,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    ID3D12CommandQueue* command_queue,
    IDXGIFactory2* factory,
    const void* init_params);
```

The body should remain semantically equivalent to the existing `xefg_init_from_swapchain_desc()`:

1. construct `XefgInitTransaction`
2. install temporary factory instance hook
3. call the supplied exact original function
4. record result
5. remove temporary factory hook
6. publish candidate through existing `publish_xefg_candidate()`

Add runtime slot/module identity only for logging and correct dispatch. Do not modify candidate semantics.

---

# 11. Serialize InitDesc Transactions

Current transaction state is process-global:

```cpp
XefgInitTransaction g_xefg_transaction{};
std::unique_ptr<VtableHook> g_xefg_factory_hook{};
```

With multiple runtimes, theoretically two modules could call InitDesc concurrently on different threads. The current singleton transaction cannot safely represent two simultaneous factory captures.

Do **not** redesign the entire transaction model in this PR unless necessary.

Use a narrow serialization mutex around the full logical InitDesc transaction.

Suggested addition:

```cpp
std::recursive_mutex g_xefg_init_transaction_mutex{};
```

or a normal mutex if reentrancy analysis proves a recursive mutex unnecessary.

Recommended scope:

```cpp
int32_t xefg_init_desc_common(...)
{
    std::unique_lock transaction_lock{
        g_xefg_init_transaction_mutex};

    // prepare g_xefg_transaction
    // install factory hook
    // call exact original XeFG InitDesc
    // collect result/candidate
    // remove factory hook
    // publish candidate
}
```

Be careful about lock ordering.

Recommended rule:

```text
g_xefg_init_transaction_mutex
    outer logical serialization

g_xefg_state_mutex
    short state/registry access only

framework hook-monitor mutex
    existing publish/binding lifecycle only
```

Do not hold `g_xefg_state_mutex` across the original XeFG InitDesc call.

Document the final lock ordering in code comments if more than one mutex can be held at the same time.

---

# 12. Startup Enumeration Is Mandatory

Fixing only `LdrLoadDll` is insufficient.

In the Pragmata session, more than one XeFG runtime may already be resident by the time REFramework initialization reaches the hook setup path.

Therefore startup must enumerate all currently loaded modules.

Required behavior:

```text
REFramework D3D12/XeFG hook bootstrap
  -> enumerate process modules
  -> for every loaded module:
       obtain full path
       basename compare case-insensitively with libxess_fg.dll
       register exact HMODULE
  -> install per-runtime hooks
```

Use a normal Windows module enumeration mechanism appropriate for the existing project dependencies, for example:

- `K32EnumProcessModules` / `EnumProcessModules`
- Toolhelp snapshot
- PEB/utility module enumeration only if there is already a safe project helper

Prefer the smallest dependency footprint already consistent with this repository.

Pseudo-code:

```cpp
void D3D12Hook::discover_loaded_xefg_runtimes()
{
    for (HMODULE module : enumerate_process_modules()) {
        const auto path = get_module_path(module);
        if (!path) {
            continue;
        }

        if (_wcsicmp(
                std::filesystem::path(*path)
                    .filename().c_str(),
                L"libxess_fg.dll") != 0) {
            continue;
        }

        notify_xefg_module_loaded(
            module,
            L"libxess_fg.dll",
            *path);
    }
}
```

Do not use `GetModuleHandleW(L"libxess_fg.dll")` inside this discovery pass.

---

# 13. Future Loads Must Use the Exact Returned HMODULE

The existing `LdrLoadDll` handoff is already the right architectural interception point because it receives the exact returned module handle after successful loader completion.

Preserve that principle.

Existing form:

```cpp
if (NT_SUCCESS(result)
    && module != nullptr
    && *module != nullptr
    && is_libxess_fg_dll(name))
{
    const auto xefg_module = static_cast<HMODULE>(*module);
    ...
    D3D12Hook::notify_xefg_module_loaded(
        xefg_module,
        L"libxess_fg.dll",
        full_path);
}
```

Required change is downstream:

```text
notify exact HMODULE
  -> directly register/hook this exact HMODULE
  -> never call basename GetModuleHandleW again
```

Repeated load notifications for an already registered HMODULE must be harmless.

---

# 14. Pending-Probe Path Must Stop Collapsing Back to One Module

Current pending-probe logic eventually does another basename lookup:

```cpp
const auto module = GetModuleHandleW(L"libxess_fg.dll");
```

Remove that single-instance behavior.

Acceptable options:

### Option A — make pending probe trigger a full enumeration

```cpp
void D3D12Hook::process_pending_xefg_probe()
{
    if (!s_xefg_probe_pending.exchange(false, ...)) {
        return;
    }

    discover_loaded_xefg_runtimes();
}
```

This is simple and robust.

### Option B — queue exact module handles

A small pending-HMODULE queue/set can be used if desired, but this is more code and is not required for this PR because the loader handoff already performs exact immediate registration.

Prefer Option A unless measured overhead or lifecycle constraints justify something more complex.

---

# 15. Registry Installation Must Be Transactional

Do not publish a half-installed runtime entry.

Recommended sequence:

```text
1. check module not already registered
2. obtain free dispatch slot
3. resolve exports from exact HMODULE
4. prepare FunctionHook for InitDesc
5. create InitDesc hook
6. prepare/create GetSwapChainPtr hook if export exists
7. only then commit runtime entry to registry
```

If InitDesc hook creation fails:

```text
-> destroy provisional hooks
-> release slot
-> leave registry unchanged
-> log failure
```

If GetSwapChainPtr is considered optional, document that explicitly and keep InitDesc as the minimum required hook.

Do not leave a runtime registered with an unusable InitDesc hook.

---

# 16. Capacity Handling

Use a bounded runtime table to keep dispatch simple.

Recommended:

```cpp
constexpr size_t kMaxXefgRuntimes = 8;
```

Eight simultaneous XeFG runtimes is already far beyond the currently observed topology.

If capacity is exhausted:

```text
- do not overwrite another slot
- do not crash
- do not hook with the wrong thunk
- log an explicit warning/error
```

Example:

```text
[XeFG][RuntimeRegistry]
module = 0x...
path = ...
action = reject
reason = capacity_exceeded
capacity = 8
```

---

# 17. Required Diagnostics

Add enough logs to prove that the intended runtime was hooked without turning the normal Present path into noisy logging.

## 17.1 Runtime discovery/registration

Required fields:

```text
[XeFG][RuntimeRegistry]
action = discovered | installed | duplicate | rejected
slot = N
module = 0x...
path = ...
init_desc = 0x...
get_swapchain = 0x...
reason = ...
```

Example expected Opti 10.x Pragmata output:

```text
[XeFG][RuntimeRegistry] action = installed, slot = 0,
  module = 0x7ffe791f0000,
  path = D:\...\_storage_\libxess_fg.dll,
  init_desc = 0x7ffe791fa380,
  get_swapchain = 0x7ffe791f7050

[XeFG][RuntimeRegistry] action = installed, slot = 1,
  module = 0x7ffe6e810000,
  path = D:\...\OptiScaler\libxess_fg.dll,
  init_desc = 0x...,
  get_swapchain = 0x...
```

## 17.2 Runtime dispatch

At InitDesc entry, log the slot/module/path at least once per initialization event:

```text
[XeFG][RuntimeDispatch]
api = InitFromSwapChainDesc
slot = 1
module = 0x7ffe6e810000
context = 0x...
hwnd = 0x...
```

The existing `[XeFG][InitDesc]` log can be extended with `runtime_slot` and `runtime_module` rather than adding redundant lines if that keeps the code smaller.

## 17.3 Existing logs must remain

Do not remove existing high-value logs:

```text
[XeFG][InitDesc]
[XeFG][InternalSwapchain]
[XeFG][QueueIdentity]
[XeFG][Bind]
[XeFG][BindingGate]
[XeFG][Rebind]
[D3D12][ExternalBind]
```

These are needed for cross-version acceptance.

---

# 18. Do Not Bind the Public XeFG Proxy

The AMD log shows a public `XefgInterpolationSwapChain` owned by `OptiScaler\libxess_fg.dll`.

Do **not** solve the issue by changing classification to treat that object as the final render target.

The project invariant remains:

> REF renders on the validated internal post-FG presentation swapchain captured from the factory call during XeFG initialization.

Therefore:

```text
public XefgInterpolationSwapChain
  -> diagnostics only
  -> not sufficient for ExternalBind

factory-created internal presentation swapchain
  -> validated
  -> authoritative bind target
```

Module-owner paths may explain topology but must not become binding policy.

---

# 19. Do Not Hardcode OptiScaler 10.x Paths

Avoid code like:

```cpp
if (path.find(L"\\OptiScaler\\libxess_fg.dll") != npos) {
    // prefer this module
}
```

That would fix one packaging layout but preserve the wrong architecture.

The correct rule is:

```text
Every loaded libxess_fg.dll HMODULE with the expected public export
is a candidate runtime instance.

Hook each runtime instance independently.

Whichever runtime actually calls InitFromSwapChainDesc naturally enters
its own correct detour and then feeds the existing candidate-validation path.
```

No path preference is needed.

---

# 20. Do Not Add GPU-Specific Logic

No code such as:

```cpp
if (vendor == AMD) {
    ...
}
```

should be added for this fix.

The bug is caused by runtime-module topology, not GPU vendor identity.

Validation should include AMD because that is the current reproducer, but implementation must remain vendor-neutral.

---

# 21. Interaction with Existing Loader Notification

Current REFramework has both:

- `LdrRegisterDllNotification`
- `LdrLoadDll` hook/handoff

Keep the loader notification callback lightweight.

Do not install heavy FunctionHooks while executing under loader-notification constraints if existing architecture intentionally defers that work.

Recommended behavior:

```text
LdrRegisterDllNotification
  -> mark probe pending / diagnostics only

LdrLoadDll post-original handoff
  -> exact returned HMODULE
  -> safe registration/hook installation

hook monitor / normal execution pending probe
  -> enumerate all loaded XeFG runtimes as catch-up
```

This preserves the original safety motivation while fixing identity loss.

---

# 22. Suggested Code Decomposition

The following is a recommendation, not a mandatory exact API, but the final implementation should be comparably explicit.

```cpp
namespace {

constexpr size_t kMaxXefgRuntimes = 8;

struct XefgRuntimeHook {
    HMODULE module{};
    std::wstring path{};
    size_t slot{};
    void* init_desc_export{};
    void* get_swapchain_export{};
    std::unique_ptr<FunctionHook> init_desc_hook{};
    std::unique_ptr<FunctionHook> get_swapchain_hook{};
};

std::array<std::optional<XefgRuntimeHook>, kMaxXefgRuntimes>
    g_xefg_runtimes{};

std::mutex g_xefg_runtime_registry_mutex{};
std::mutex g_xefg_init_transaction_mutex{};

XefgRuntimeHook* find_xefg_runtime(HMODULE module);
XefgRuntimeHook* get_xefg_runtime_by_slot(size_t slot);
std::optional<size_t> allocate_xefg_runtime_slot();

bool install_xefg_runtime_hooks(
    HMODULE module,
    std::wstring_view path);

void discover_loaded_xefg_runtimes();

int32_t xefg_init_desc_dispatch(...);
int32_t xefg_init_desc_common(...);
int32_t xefg_get_swapchain_dispatch(...);

} // namespace
```

If possible, reuse `g_xefg_state_mutex` rather than adding a second registry mutex, but only if lock scope remains short and lock ordering stays obvious.

Avoid unnecessary class-wide public API changes.

---

# 23. Example Registration Implementation

Illustrative only:

```cpp
bool install_xefg_runtime_hooks(
    HMODULE module,
    std::wstring_view path)
{
    if (module == nullptr) {
        return false;
    }

    std::scoped_lock lock{g_xefg_state_mutex};

    if (find_xefg_runtime(module) != nullptr) {
        spdlog::info(
            "[XeFG][RuntimeRegistry] action = duplicate, module = 0x{:x}, path = {}",
            reinterpret_cast<uintptr_t>(module),
            utility::narrow(std::wstring{path}));
        return true;
    }

    const auto slot = allocate_xefg_runtime_slot();
    if (!slot) {
        spdlog::error(
            "[XeFG][RuntimeRegistry] action = rejected, module = 0x{:x}, reason = capacity_exceeded",
            reinterpret_cast<uintptr_t>(module));
        return false;
    }

    auto* init_desc = GetProcAddress(
        module,
        "xefgSwapChainD3D12InitFromSwapChainDesc");

    if (init_desc == nullptr) {
        spdlog::warn(
            "[XeFG][RuntimeRegistry] action = rejected, module = 0x{:x}, reason = init_desc_missing",
            reinterpret_cast<uintptr_t>(module));
        return false;
    }

    XefgRuntimeHook candidate{};
    candidate.module = module;
    candidate.path = std::wstring{path};
    candidate.slot = *slot;
    candidate.init_desc_export = init_desc;
    candidate.get_swapchain_export = GetProcAddress(
        module,
        "xefgSwapChainD3D12GetSwapChainPtr");

    candidate.init_desc_hook = std::make_unique<FunctionHook>(
        Address{reinterpret_cast<void*>(candidate.init_desc_export)},
        Address{reinterpret_cast<void*>(kXefgInitDescThunks[*slot])});

    if (!candidate.init_desc_hook->create()) {
        spdlog::error(
            "[XeFG][RuntimeRegistry] action = rejected, slot = {}, module = 0x{:x}, reason = init_hook_failed",
            *slot,
            reinterpret_cast<uintptr_t>(module));
        return false;
    }

    if (candidate.get_swapchain_export != nullptr) {
        candidate.get_swapchain_hook = std::make_unique<FunctionHook>(
            Address{reinterpret_cast<void*>(candidate.get_swapchain_export)},
            Address{reinterpret_cast<void*>(kXefgGetSwapchainThunks[*slot])});

        if (!candidate.get_swapchain_hook->create()) {
            // Either fail transactionally or explicitly treat this hook as optional.
            // Pick one policy and document it.
        }
    }

    g_xefg_runtimes[*slot] = std::move(candidate);

    spdlog::info(
        "[XeFG][RuntimeRegistry] action = installed, slot = {}, module = 0x{:x}, path = {}",
        *slot,
        reinterpret_cast<uintptr_t>(module),
        utility::narrow(std::wstring{path}));

    return true;
}
```

Important: adapt the exact `FunctionHook` constructor signature to the existing project style. This sample shows intent, not copy-paste authority.

---

# 24. Safer Registry Publication Pattern

Because the thunk can theoretically execute as soon as `FunctionHook::create()` succeeds, the implementation must ensure dispatch data exists before the hooked function can be called.

Do not create a race where:

```text
hook active
-> XeFG thread enters thunk
-> slot lookup returns empty because registry entry has not been published yet
```

Two acceptable patterns:

### Pattern A — publish stable slot state before enabling hook

Store a registry entry with a clear installation state:

```cpp
enum class XefgRuntimeInstallState {
    Empty,
    Installing,
    Active,
};
```

Then let dispatch reject or wait safely if state is not Active.

### Pattern B — slot object always exists, FunctionHook pointer becomes valid atomically under lock

Use fixed registry storage and install fields under the registry mutex before detour creation, then mark active only after creation succeeds.

Do not use a movable container whose reallocation can invalidate dispatch references.

This race is more important than syntactic simplicity.

---

# 25. Runtime Object Lifetime

Do not unload any XeFG module from REFramework.

The registry stores non-owning HMODULE identity only.

This PR should not call `FreeLibrary` on runtime modules.

FunctionHook objects must remain alive for as long as their corresponding runtime module is considered active.

If module unload is observed later, that requires a dedicated teardown design because leaving a hook object targeting unmapped code would be unsafe. For this PR:

```text
- document unload/reload as not yet supported
- do not invent unsafe teardown
- keep current behavior no worse than baseline
```

If implementation can cheaply detect an unload notification and mark diagnostics without touching unmapped target memory, that is acceptable, but do not expand scope into full unload support.

---

# 26. Static Acceptance Criteria

The PR is ready for hardware testing only if all are true.

## Build/static

- project configures successfully
- Release build succeeds
- existing direct-struct-field audit passes
- `dinput8.dll` artifact is produced
- no new warnings attributable to the change

## Single-module assumption removal

Search the XeFG hook bootstrap path and verify:

- no hook installation depends on `GetModuleHandleW(L"libxess_fg.dll")`
- `notify_xefg_module_loaded(HMODULE module, ...)` uses the exact supplied module
- pending-probe path does not collapse to a single basename module

A basename check is still fine when identifying whether a module's filename is `libxess_fg.dll`; it is not fine for selecting the runtime instance to hook.

## Registry

- exact same HMODULE registration is idempotent
- different HMODULEs with same basename get different slots
- each InitDesc hook has the matching slot-specific detour
- each runtime's callback resolves its own original trampoline
- no container reallocation invalidates runtime state used by callbacks

## Existing binding invariants

- `publish_xefg_candidate()` semantics unchanged unless a very small plumbing change is required
- presentation queue selection unchanged
- device identity validation unchanged
- P3.2 rebind semantics unchanged
- public proxy is not newly accepted as render target

---

# 27. Runtime Acceptance Matrix

## 27.1 Intel + OptiScaler release/0.9 — regression check

Expected topology:

```text
runtime count = 1
```

Required:

```text
RuntimeRegistry installed exactly one active XeFG runtime
InitDesc seen
InternalSwapchain seen
ExternalBind seen
REF overlay visible
Opti overlay visible
XeFG works
no new periodic rehook caused by registry change
```

This verifies that multi-module support does not regress the already working 0.9 topology.

## 27.2 AMD + OptiScaler 10.x Pragmata — primary fix validation

Expected topology based on captured session:

```text
runtime count >= 2
```

Required logs should show both runtime modules registered, including the `OptiScaler\libxess_fg.dll` instance.

Critical success sequence:

```text
[XeFG][RuntimeRegistry] action = installed, slot = 0 ...
[XeFG][RuntimeRegistry] action = installed, slot = 1 ...OptiScaler\libxess_fg.dll

[XeFG][RuntimeDispatch] api = InitFromSwapChainDesc, slot = <Opti runtime slot>
[XeFG][InitDesc] ...
[XeFG][InternalSwapchain] ...
[XeFG][QueueIdentity] ...
[XeFG][Bind] accepted = true ...
[D3D12][ExternalBind] source = xefg_internal ...
```

User-visible acceptance:

```text
REF Insert overlay appears
OptiScaler overlay remains functional
XeFG/Present path remains functional
```

Do not require long-term FG-active behavior from a menu-only session unless the tester actually enters gameplay.

## 27.3 Another Capcom game + OptiScaler 10.x

Because the same missing-overlay symptom was reported in another Capcom game, test one second game if available.

The purpose is to prove this is a generic packaging/runtime-topology fix rather than a Pragmata-specific special case.

---

# 28. Negative / Failure-Mode Tests

Static/unit-style validation where practical:

### Duplicate registration

```text
register(module A)
register(module A)
-> one slot only
-> no second FunctionHook
```

### Two modules same basename

```text
register(module A, ...\_storage_\libxess_fg.dll)
register(module B, ...\OptiScaler\libxess_fg.dll)
-> two distinct slots
```

### Missing InitDesc export

```text
module matches basename but export absent
-> reject safely
-> no partial active entry
```

### Capacity exhausted

```text
more than kMaxXefgRuntimes
-> explicit reject log
-> no overwrite/out-of-bounds dispatch
```

### GetSwapChainPtr export absent

Choose and document policy:

```text
InitDesc required
GetSwapChainPtr optional diagnostic hook
```

is acceptable if existing logic does not require GetSwapChainPtr for binding.

---

# 29. Logging Acceptance Examples

A good multi-runtime startup should be understandable from logs without source inspection.

Example:

```text
[XeFG][RuntimeRegistry] action = discovered, module = 0x7ffe791f0000,
  path = D:\Game\_storage_\libxess_fg.dll
[XeFG][RuntimeRegistry] action = installed, slot = 0,
  module = 0x7ffe791f0000,
  init_desc = 0x7ffe791fa380,
  get_swapchain = 0x7ffe791f7050

[XeFG][RuntimeRegistry] action = discovered, module = 0x7ffe6e810000,
  path = D:\Game\OptiScaler\libxess_fg.dll
[XeFG][RuntimeRegistry] action = installed, slot = 1,
  module = 0x7ffe6e810000,
  init_desc = 0x7ffe6e81....,
  get_swapchain = 0x7ffe6e81....

[XeFG][RuntimeDispatch] api = InitFromSwapChainDesc,
  slot = 1,
  module = 0x7ffe6e810000,
  context = 0x...

[XeFG][InternalSwapchain] ...
[XeFG][Bind] accepted = true ...
[D3D12][ExternalBind] source = xefg_internal ...
```

This should make it impossible to repeat the current ambiguity where logs only prove that some `libxess_fg.dll` was hooked.

---

# 30. PR Size and Scope Guidance

Keep this PR focused.

Recommended functional size:

```text
~200–350 LOC
```

A modest amount above that is acceptable if the template thunk table and diagnostics account for the difference, but avoid combining unrelated lifecycle fixes.

Expected files:

```text
src/D3D12Hook.cpp
src/D3D12Hook.hpp   only if required for declarations
src/REFramework.cpp only if startup enumeration/loader handoff plumbing requires it
```

Do not modify unrelated renderer files.

---

# 31. Suggested Implementation Order

Implement in this order to reduce regression risk.

## Step 1 — isolate current common callback body

Refactor existing `xefg_init_from_swapchain_desc()` into a common function that accepts an explicit original function pointer and runtime identity.

No behavior change yet.

## Step 2 — add fixed runtime registry

Add:

```text
HMODULE
full path
slot
export addresses
per-module hook objects
installation state
```

## Step 3 — add slot-specific InitDesc thunks

Verify each slot can retrieve the correct original trampoline.

## Step 4 — convert GetSwapChainPtr to the same dispatch model

Do not leave mixed single/multi architecture.

## Step 5 — change exact-HMODULE registration

`notify_xefg_module_loaded()` must directly install the supplied module.

## Step 6 — add startup enumeration

Catch runtimes already loaded before REFramework's LdrLoadDll hook became active.

## Step 7 — convert pending probe to full enumeration

Remove the remaining basename single-instance selection.

## Step 8 — add transaction serialization

Protect the global factory-capture transaction against concurrent InitDesc calls.

## Step 9 — verify no P2/P3 semantic changes

Diff-review specifically around:

```text
publish_xefg_candidate
bind_external_swapchain
replace_xefg_binding
present/present1
resize handlers
```

These should remain unchanged except for necessary signatures/log metadata.

---

# 32. Review Checklist for PR #14

When the PR is opened, review the following carefully.

### Blocking if present

- callback for module B can call module A's original trampoline
- exact HMODULE is discarded and basename lookup remains authoritative
- registry slot can become invalid due to vector reallocation
- hook becomes live before dispatch slot state exists
- `FunctionHook` is destroyed while target module remains hooked
- runtime registry stores duplicate entries for the same HMODULE
- concurrent InitDesc can overwrite shared transaction/factory hook
- public XeFG proxy is newly bound directly
- existing P3.2 rebind logic is modified without necessity
- loader-lock unsafe hook installation is introduced

### Non-blocking / follow-up unless proven reachable

- complete module unload/reload teardown
- more than eight simultaneous XeFG runtimes
- path cosmetic normalization
- additional diagnostic verbosity tuning

---

# 33. Expected Root-Cause Closure

Before this PR:

```text
OptiScaler 10.x
  loads/uses <game>\OptiScaler\libxess_fg.dll

REFramework
  sees another libxess_fg.dll first
  GetModuleHandleW("libxess_fg.dll") chooses one basename match
  global hook slot becomes occupied
  actual Opti XeFG runtime remains unhooked

Actual Opti InitFromSwapChainDesc
  bypasses REF

No factory capture
No internal swapchain candidate
No ExternalBind
No REF overlay
```

After this PR:

```text
REFramework startup
  enumerates every loaded libxess_fg.dll HMODULE

Module A
  registered slot 0
  own hooks / own originals

Module B
  registered slot 1
  own hooks / own originals

Opti calls Module B InitFromSwapChainDesc
  -> slot 1 detour
  -> Module B original trampoline
  -> existing temporary factory capture
  -> internal presentation swapchain
  -> existing validation
  -> ExternalBind
  -> REF renderer/overlay
```

That is the intended technical closure for the OptiScaler 10.x multi-module compatibility issue.

---

# 34. Final Constraints

The implementation must preserve these project rules:

1. **Render only the validated internal post-FG presentation swapchain.**
2. **Presentation queue remains authoritative for the proven distinct-same-device case.**
3. **Do not use Intel private offsets or private XeFG object layout.**
4. **Do not classify/bind by module owner filename.**
5. **Do not add OptiScaler-specific path hardcoding.**
6. **Do not add GPU-vendor-specific behavior.**
7. **Strong COM ownership and P3.2 atomic rebind semantics remain unchanged.**
8. **MHW Alt+Enter lifecycle investigation remains a separate P3.3 track.**
9. **Special K remains absent from the target topology.**
10. **Hardware runtime validation is performed by the user; Codex should implement, build, statically verify, and report exact evidence.**

---

# 35. Codex Deliverable

Implement the above as one focused PR.

At completion, report:

```text
- exact files changed
- final functional LOC estimate
- registry capacity
- how exact HMODULE identity is preserved
- how per-runtime original trampoline dispatch is guaranteed
- how startup-loaded modules are enumerated
- how future LdrLoadDll modules are registered
- how duplicate registration is prevented
- how InitDesc transaction concurrency is serialized
- whether GetSwapChainPtr is required or optional
- confirmation that P2/P3 binding semantics were not changed
- configure/build/audit results
- generated dinput8.dll artifact status
- any remaining runtime-only acceptance items
```

Do not claim AMD/Opti10 runtime success from build/static evidence alone. The user will perform the actual hardware/game validation.
