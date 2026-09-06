# Work Order: XeFG Refactor R2 — Loader / Probe Handoff Isolation

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `65f9b3ee81971c3e2aac6df49518fa2dd588365d` (`Refactor R1: extract XeFG runtime registry (#17)`)

Related documents:

- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_ARCHITECTURE_2026-09-06.md`
- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_PR_SPLIT_PLAN_2026-09-06.md`
- `doc/work-order/XEFG_REFACTOR_R1_RUNTIME_REGISTRY_EXTRACTION.md`

This work order implements **R2 only** from the fine-grained refactor plan.

---

# 1. Recommended PR Identity

Suggested branch:

```text
refactor/xefg-r2-loader-probe-handoff
```

Suggested PR title:

```text
Refactor R2: isolate XeFG loader and probe handoff
```

Suggested commit title:

```text
refactor: isolate XeFG loader and probe handoff
```

This PR must remain a **behavior-preserving ownership extraction**.

Do not combine R3 or later work into this PR.

---

# 2. Objective

Move XeFG-specific **module observation, pending-probe state, already-loaded module enumeration, and exact-module handoff into the R1 runtime registry** out of `D3D12Hook` and behind a narrow `XeFGCompatibility` façade.

After R2, `REFramework.cpp` should still own the generic Windows loader integration mechanics, but it must no longer call XeFG runtime-management methods on `D3D12Hook`.

The intended dependency after this PR is:

```text
REFramework.cpp
    |
    | generic loader hooks remain here
    | only narrow XeFG notifications cross the boundary
    v
XeFGCompatibility                 <-- NEW OWNER IN R2
    |- loader-notification pending state
    |- module-seen state
    |- first-seen diagnostic timestamp
    |- already-loaded module enumeration
    |- exact-module export diagnostics
    |- synchronous handoff to registry
    |
    v
XeFGRuntimeRegistry               <-- FROM R1
    |- exact HMODULE registry
    |- stable runtime slots
    |- export FunctionHook ownership
    |- original trampoline lookup
    |
    v
D3D12Hook                         <-- STILL OWNS R3+ WORK
    |- InitDesc dispatch/common path
    |- factory capture
    |- candidate validation/publication
    |- active bind/rebind
    |- Present/resize lifecycle
```

The key architectural result is:

> `REFramework.cpp` observes Windows loader events, `XeFGCompatibility` owns XeFG loader/probe policy, `XeFGRuntimeRegistry` owns per-runtime hooks, and `D3D12Hook` remains the presentation/discovery owner until later PRs.

---

# 3. Why R2 Exists

R1 successfully extracted the exact-HMODULE runtime registry, but the higher-level XeFG loader/probe policy is still embedded in `D3D12Hook`.

Current `D3D12Hook.hpp` still exposes high-level XeFG loader/probe APIs such as:

```cpp
static void install_xefg_api_hooks_if_available();
static void install_xefg_api_hooks_for_module(HMODULE module, std::wstring_view full_path);
static void mark_xefg_probe_pending() noexcept;
static void process_pending_xefg_probe();
static void notify_xefg_module_loaded(
    HMODULE module,
    std::wstring_view base_name,
    std::wstring_view full_path);
static bool is_xefg_module_loaded();
```

`REFramework.cpp` currently knows those methods directly in three places:

```text
hook_monitor()
    -> D3D12Hook::process_pending_xefg_probe()

LdrLoadDll hook after successful original call
    -> D3D12Hook::notify_xefg_module_loaded(...)

LdrRegisterDllNotification callback
    -> D3D12Hook::mark_xefg_probe_pending()
```

Those are valid integration points, but `D3D12Hook` should not be the owner of loader policy.

R2 creates the compatibility façade without touching the proven InitDesc/discovery/binding path.

---

# 4. Current Behavior That Must Be Preserved Exactly

This section is a contract, not background information.

## 4.1 Loader notification callback remains lightweight

The `LdrRegisterDllNotification` callback can run in loader-sensitive context.

Current XeFG behavior there is intentionally minimal:

```cpp
if (lower_base_dll_name == L"libxess_fg.dll") {
    D3D12Hook::mark_xefg_probe_pending();
}
```

`mark_xefg_probe_pending()` currently does only state/timestamp work:

```text
module_loaded = true
first_seen_ms = set once
probe_pending = true
```

It does **not** enumerate modules or install `FunctionHook`s in the notification callback.

R2 must preserve this property.

## 4.2 Successful LdrLoadDll handoff remains synchronous

The existing `LdrLoadDll` hook calls the original first.

After the original successfully returns a real `HMODULE` for `libxess_fg.dll`, REFramework immediately performs the exact-module handoff before returning to the caller.

Current semantic:

```text
caller
  -> hooked LdrLoadDll
      -> original LdrLoadDll
          -> module is loaded
      <- original returns success + exact HMODULE
      -> install XeFG API hooks for that exact HMODULE
  <- return to caller
```

This ordering exists so the caller cannot race ahead and resolve/invoke the newly loaded XeFG public API before REFramework installs the export detour.

**R2 must not turn this path into pending-only work.**

The new façade call from this path must remain synchronous and must reach `XeFGRuntimeRegistry::install_for_module()` before the hooked `LdrLoadDll` returns.

## 4.3 Pending probe processing remains on the existing hook-monitor path

Current `REFramework::hook_monitor()` performs:

```cpp
if (d3d12 != nullptr) {
    D3D12Hook::process_pending_xefg_probe();
}
```

R2 may replace the callee, but must preserve the location/cadence and surrounding hook-monitor locking semantics.

Do not introduce:

- a worker thread;
- a timer;
- polling loop;
- arbitrary sleep;
- asynchronous queue;
- detached task.

## 4.4 Already-loaded runtime enumeration remains multi-module

Current `install_xefg_api_hooks_if_available()` uses a Toolhelp module snapshot and enumerates **all** loaded modules named `libxess_fg.dll`.

For each matching exact module it calls the exact-module install path.

This behavior was introduced for multi-runtime cases and must survive R2.

Do not replace enumeration with:

```cpp
GetModuleHandleW(L"libxess_fg.dll")
```

or any other single-basename lookup.

## 4.5 R1 exact-HMODULE registry is authoritative

R1 created:

```cpp
XeFGRuntimeRegistry::instance().install_for_module(module, full_path);
```

R2 must route exact module installation to this API.

Do not duplicate runtime tables, thunk arrays, or `FunctionHook` ownership in `XeFGCompatibility`.

---

# 5. Scope

## 5.1 Required changes

Create:

```text
src/compatibility/xefg/XeFGCompatibility.hpp
src/compatibility/xefg/XeFGCompatibility.cpp
```

Move loader/probe ownership into this façade:

- XeFG module-seen atomic state;
- pending-probe atomic state;
- first-seen diagnostic timestamp state;
- `mark_xefg_probe_pending()` semantics;
- `process_pending_xefg_probe()` semantics;
- already-loaded `libxess_fg.dll` enumeration;
- exact module-loaded diagnostics;
- exact module handoff into `XeFGRuntimeRegistry`.

Update `REFramework.cpp` to call the façade instead of `D3D12Hook` for those XeFG loader/probe operations.

Remove the corresponding loader/probe APIs and static fields from `D3D12Hook.hpp` / `D3D12Hook.cpp` when no longer referenced.

## 5.2 Explicitly out of scope

Do **not** modify the semantics of:

- `D3D12Hook::xefg_init_desc_dispatch()`;
- `D3D12Hook::xefg_get_swapchain_dispatch()`;
- `D3D12Hook::xefg_init_desc_common()`;
- `D3D12Hook::create_xefg_swapchain()`;
- `D3D12Hook::publish_xefg_candidate()`;
- `D3D12Hook::consume_pending_xefg_binding()`;
- `D3D12Hook::bind_external_swapchain()`;
- `D3D12Hook::replace_xefg_binding()`;
- queue/device relation policy;
- Present/Present1 callbacks;
- ResizeBuffers/ResizeBuffers1/ResizeTarget behavior;
- resize-transition hold;
- hook-monitor preserve-binding decision;
- logging levels;
- Debug Logging UI;
- Streamline / DLSSG behavior;
- D3D11 behavior;
- generic loader infrastructure;
- runtime unload/reload architecture.

Do not move the InitDesc transaction into `XeFGCompatibility`; that is R3.

Do not move binding state into `XeFGCompatibility`; that is R5+.

---

# 6. Required Ownership Boundary

After R2, responsibilities should be divided as follows.

## 6.1 `REFramework.cpp`

Still owns generic Windows integration:

```text
LdrRegisterDllNotification registration
LdrLoadDll detour mechanics
name filtering for libxess_fg.dll
DLSSG / Streamline loader branch
current-game-path loader handling
hook_monitor() scheduling/cadence
```

But it should only call narrow XeFG façade entry points.

## 6.2 `XeFGCompatibility`

Owns XeFG-specific loader/probe policy:

```text
module observed
pending probe
first seen timestamp
already-loaded runtime enumeration
module export diagnostics
exact HMODULE runtime-registration handoff
```

It should know about `XeFGRuntimeRegistry`.

It should **not** know about renderer reset, active binding replacement, resize hold, or Present callbacks.

## 6.3 `XeFGRuntimeRegistry`

Remains unchanged in responsibility:

```text
exact HMODULE registry
stable slot/thunk identity
XeFG public export FunctionHook ownership
per-runtime original trampoline resolution
```

R2 should not redesign R1.

## 6.4 `D3D12Hook`

Still owns all presentation-side XeFG behavior for now:

```text
InitDesc common transaction
factory capture
candidate validation
pending binding
active binding
renderer callback integration
Present/resize hooks
```

The only R2-related direct dependency it may need is a read-only query for loader diagnostics, e.g. whether an XeFG module has been observed.

---

# 7. Recommended `XeFGCompatibility` API

A static façade is preferred for R2.

Reason:

- `mark_probe_pending()` can be called from loader-notification context;
- avoid first-use function-local singleton construction inside a loader-sensitive callback;
- all state is process-global by design;
- no object graph or ownership injection is needed yet.

Recommended header shape:

```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

#include <windows.h>

class XeFGCompatibility {
public:
    // Lightweight loader-notification path only.
    static void mark_probe_pending() noexcept;

    // Synchronous exact-HMODULE handoff. Safe only outside loader-notification
    // callback context, matching the current post-LdrLoadDll path.
    static void on_module_loaded(
        HMODULE module,
        std::wstring_view base_name,
        std::wstring_view full_path);

    // Consumes pending notification work on the existing hook-monitor path.
    static void process_pending_work();

    // Read-only diagnostic state used by D3D12Hook logging.
    static bool is_module_loaded() noexcept;

private:
    static void install_already_loaded_runtimes();

    static inline std::atomic<bool> s_module_loaded{false};
    static inline std::atomic<bool> s_probe_pending{false};
    static inline std::atomic<int64_t> s_first_seen_ms{-1};
};
```

Exact naming may differ, but keep the API narrow.

Do not add callback registration, event buses, provider interfaces, dependency injection, or generic frame-generation abstraction.

---

# 8. Implementation Details

## 8.1 Move pending/module state out of `D3D12Hook`

Current `D3D12Hook.hpp` contains state conceptually equivalent to:

```cpp
static inline std::atomic<bool> s_xefg_module_loaded{false};
static inline std::atomic<bool> s_xefg_probe_pending{false};
static inline std::atomic<int64_t> s_xefg_first_seen_ms{-1};
```

Move those into `XeFGCompatibility`.

Do not leave mirrored copies in both classes.

There must be one authoritative pending flag and one authoritative module-seen state.

## 8.2 Preserve `mark_probe_pending()` memory ordering

Current logic uses release/acquire semantics around the pending flag.

Preserve it unless there is a proven reason not to.

Example:

```cpp
void XeFGCompatibility::mark_probe_pending() noexcept {
    s_module_loaded.store(true, std::memory_order_release);

    const auto observed_ms = /* same diagnostic epoch semantics */;
    int64_t expected = -1;
    s_first_seen_ms.compare_exchange_strong(
        expected,
        observed_ms,
        std::memory_order_relaxed);

    s_probe_pending.store(true, std::memory_order_release);
}
```

The function must remain lightweight.

Forbidden inside `mark_probe_pending()`:

```text
CreateToolhelp32Snapshot
Module32First/Next
GetProcAddress loops
FunctionHook construction
registry installation
renderer calls
mutex waits on REFramework lifecycle
sleep/yield loops
```

## 8.3 Preserve exact post-LdrLoadDll synchronous installation

The current `REFramework.cpp` path should become conceptually:

```cpp
if (NT_SUCCESS(result)
    && module != nullptr
    && *module != nullptr
    && is_libxess_fg_dll(name)) {

    const auto xefg_module = static_cast<HMODULE>(*module);

    wchar_t path[MAX_PATH]{};
    const auto path_length = GetModuleFileNameW(
        xefg_module,
        path,
        ARRAYSIZE(path));

    XeFGCompatibility::on_module_loaded(
        xefg_module,
        L"libxess_fg.dll",
        std::wstring_view{path, path_length});
}
```

Do not change this to:

```cpp
XeFGCompatibility::mark_probe_pending();
```

only.

That would create a real timing regression because the caller could invoke XeFG before the later hook-monitor pass installs the export hook.

## 8.4 `on_module_loaded()` must call the R1 registry directly

Recommended flow:

```cpp
void XeFGCompatibility::on_module_loaded(
    HMODULE module,
    std::wstring_view base_name,
    std::wstring_view full_path) {

    if (module == nullptr) {
        return;
    }

    s_module_loaded.store(true, std::memory_order_relaxed);

    // Preserve first-seen timestamp behavior and current diagnostics.
    ...

    // Preserve current export visibility diagnostics.
    const auto init_from_swap_chain = GetProcAddress(
        module,
        "xefgSwapChainD3D12InitFromSwapChain");
    const auto init_from_swap_chain_desc = GetProcAddress(
        module,
        "xefgSwapChainD3D12InitFromSwapChainDesc");
    const auto get_swap_chain_ptr = GetProcAddress(
        module,
        "xefgSwapChainD3D12GetSwapChainPtr");

    ... log existing information ...

    // R1 is the sole runtime hook owner.
    XeFGRuntimeRegistry::instance().install_for_module(module, full_path);
}
```

Important:

- do not recreate `FunctionHook` here;
- do not resolve the module again by basename;
- do not allocate a runtime slot here;
- do not duplicate R1's duplicate-HMODULE logic;
- do not hold any compatibility mutex across registry installation unless one is actually required.

`XeFGRuntimeRegistry` already owns its own synchronization.

## 8.5 Move already-loaded module enumeration to the façade

Current `D3D12Hook::install_xefg_api_hooks_if_available()` enumerates modules using Toolhelp.

Move equivalent logic into:

```cpp
void XeFGCompatibility::install_already_loaded_runtimes();
```

Example:

```cpp
void XeFGCompatibility::install_already_loaded_runtimes() {
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        GetCurrentProcessId());

    if (snapshot == INVALID_HANDLE_VALUE) {
        spdlog::warn(
            "[XeFG][RuntimeRegistry] module enumeration failed, error = {}",
            GetLastError());
        return;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, L"libxess_fg.dll") == 0) {
                on_module_loaded(
                    entry.hModule,
                    std::wstring_view{entry.szModule},
                    std::wstring_view{entry.szExePath});
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
}
```

RAII cleanup is acceptable if it stays simple and does not expand scope.

Do not collapse this to one module.

## 8.6 Preserve pending consumption semantics

Recommended:

```cpp
void XeFGCompatibility::process_pending_work() {
    if (!s_probe_pending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    install_already_loaded_runtimes();
}
```

This must remain idempotent with repeated notifications.

Repeated enumeration is acceptable because R1 exact-HMODULE registration is idempotent.

## 8.7 Keep generic loader infrastructure in `REFramework.cpp`

Do not move these in R2:

```text
LDR_DLL_* structs / typedefs
LdrRegisterDllNotification setup
LdrLoadDll function hook object
is_libxess_fg_dll() name filter
setup_ldr_load_dll_hook()
ldr_notification_callback() itself
other DLL-loaded handling
```

R2 only changes which XeFG subsystem receives the handoff.

This keeps the diff small and avoids entangling XeFG refactor with generic loader code.

---

# 9. Required `REFramework.cpp` Changes

Add the façade include:

```cpp
#include "compatibility/xefg/XeFGCompatibility.hpp"
```

Then make only the narrow call-site substitutions.

## 9.1 Hook monitor

Before:

```cpp
if (d3d12 != nullptr) {
    D3D12Hook::process_pending_xefg_probe();
}
```

After:

```cpp
if (d3d12 != nullptr) {
    XeFGCompatibility::process_pending_work();
}
```

Do not move the call outside the current hook-monitor lifecycle context in this PR.

## 9.2 Post-LdrLoadDll exact-module handoff

Before:

```cpp
D3D12Hook::notify_xefg_module_loaded(
    xefg_module,
    L"libxess_fg.dll",
    std::wstring_view{path, path_length});
```

After:

```cpp
XeFGCompatibility::on_module_loaded(
    xefg_module,
    L"libxess_fg.dll",
    std::wstring_view{path, path_length});
```

Everything around it should remain unchanged unless needed for compilation.

## 9.3 Loader notification callback

Before:

```cpp
if (lower_base_dll_name == L"libxess_fg.dll") {
    D3D12Hook::mark_xefg_probe_pending();
}
```

After:

```cpp
if (lower_base_dll_name == L"libxess_fg.dll") {
    XeFGCompatibility::mark_probe_pending();
}
```

Do not call `on_module_loaded()` from this callback.

---

# 10. Required `D3D12Hook` Cleanup in R2

Once call sites are migrated, remove loader/probe implementation ownership from `D3D12Hook`.

Expected removable public APIs:

```cpp
static void install_xefg_api_hooks_if_available();
static void install_xefg_api_hooks_for_module(
    HMODULE module,
    std::wstring_view full_path);
static void mark_xefg_probe_pending() noexcept;
static void process_pending_xefg_probe();
static void notify_xefg_module_loaded(
    HMODULE module,
    std::wstring_view base_name,
    std::wstring_view full_path);
static bool is_xefg_module_loaded();
```

Expected removable static fields:

```cpp
s_xefg_module_loaded
s_xefg_probe_pending
s_xefg_first_seen_ms
```

Expected removable `D3D12Hook.cpp` implementation blocks:

```text
install_xefg_api_hooks_if_available()
install_xefg_api_hooks_for_module()
mark_xefg_probe_pending()
notify_xefg_module_loaded()
process_pending_xefg_probe()
```

Do **not** remove these R1/R3 bridge methods yet:

```cpp
D3D12Hook::xefg_init_desc_dispatch(...)
D3D12Hook::xefg_get_swapchain_dispatch(...)
D3D12Hook::xefg_init_desc_common(...)
```

The R1 thunk table currently still targets the D3D12 dispatch methods. That remains correct for R2.

---

# 11. D3D12 Hook-Monitor Diagnostic Query

`D3D12Hook::log_hook_monitor_snapshot()` currently logs whether an XeFG module has been observed.

Do not drop that diagnostic in R2 merely because state ownership moved.

Replace the old class-local query with the façade query.

Conceptually:

```cpp
spdlog::info(
    "[D3D12][HookMonitor] ... xefg_module_loaded = {} ...",
    ...,
    XeFGCompatibility::is_module_loaded(),
    ...);
```

This is a read-only dependency and is acceptable in R2.

Do not use this as an excuse to refactor the hook-monitor preservation decision. That is R10.

---

# 12. Header / Include Hygiene

Because source files are globbed under `src/**.cpp`, no CMake target source-list change should be required.

Expected include movement:

`XeFGCompatibility.cpp` likely needs:

```cpp
#include "XeFGCompatibility.hpp"

#include <chrono>
#include <tlhelp32.h>

#include <spdlog/spdlog.h>

#include "XeFGRuntimeRegistry.hpp"
#include "utility/String.hpp"
```

`REFramework.cpp` gains:

```cpp
#include "compatibility/xefg/XeFGCompatibility.hpp"
```

`D3D12Hook.cpp` may need:

```cpp
#include "compatibility/xefg/XeFGCompatibility.hpp"
```

for the read-only diagnostic query.

Remove `<tlhelp32.h>` from `D3D12Hook.cpp` only if it is no longer used anywhere else in that file after the move.

Do not perform unrelated include cleanup.

---

# 13. Loader-Lock Safety Rules

This section is release-blocking.

## 13.1 Notification callback path

Allowed:

```text
atomic stores/exchange/CAS
simple timestamp capture already equivalent to current behavior
```

Not allowed:

```text
Toolhelp snapshot enumeration
FunctionHook create/remove
runtime-registry installation
renderer reset
D3D12 hook lifecycle lock waits
filesystem scanning
thread creation
sleep/yield retry loops
```

## 13.2 Post-LdrLoadDll path

After the original `LdrLoadDll` has returned successfully, the current code deliberately performs the heavier exact-module hook installation synchronously.

Preserve that distinction.

The code comment describing this ordering should remain accurate after R2.

## 13.3 Avoid dynamic singleton initialization from notification context

Prefer static façade methods + statically initialized atomics over:

```cpp
XeFGCompatibility::instance()
```

if the first call could occur from `LdrRegisterDllNotification` callback context.

Do not introduce unnecessary first-use initialization guards under loader-sensitive execution.

---

# 14. Concurrency / State Rules

R2 should not introduce a new broad mutex.

The state machine is intentionally small:

```text
notification sees libxess_fg.dll
    -> module_loaded = true
    -> first_seen set once
    -> probe_pending = true

hook monitor later
    -> exchange probe_pending false
    -> enumerate all loaded libxess_fg.dll modules
    -> exact-HMODULE registry install (idempotent)

post-LdrLoadDll exact path
    -> module_loaded = true
    -> first_seen set once
    -> exact-HMODULE registry install immediately
```

The registry already serializes its own runtime table.

Do not wrap registry installation in another compatibility mutex without a concrete need.

Potential repeated execution is expected and safe:

```text
same HMODULE observed through post-LdrLoadDll
then later seen by pending enumeration
    -> R1 registry reports duplicate
    -> no second FunctionHook
```

That is correct behavior.

---

# 15. Migration Table

Use this table as the implementation checklist.

| Current symbol / responsibility | R2 destination | Notes |
|---|---|---|
| `D3D12Hook::mark_xefg_probe_pending()` | `XeFGCompatibility::mark_probe_pending()` | lightweight only |
| `D3D12Hook::process_pending_xefg_probe()` | `XeFGCompatibility::process_pending_work()` | same hook-monitor cadence |
| `D3D12Hook::notify_xefg_module_loaded()` | `XeFGCompatibility::on_module_loaded()` | synchronous exact-module path |
| `D3D12Hook::install_xefg_api_hooks_if_available()` | private `XeFGCompatibility::install_already_loaded_runtimes()` | preserve Toolhelp multi-module enumeration |
| `D3D12Hook::install_xefg_api_hooks_for_module()` | remove bridge; call `XeFGRuntimeRegistry::install_for_module()` from façade | R1 registry is owner |
| `s_xefg_module_loaded` | `XeFGCompatibility` | one authoritative copy |
| `s_xefg_probe_pending` | `XeFGCompatibility` | one authoritative copy |
| `s_xefg_first_seen_ms` | `XeFGCompatibility` | preserve first-seen semantics |
| first-seen diagnostic epoch | `XeFGCompatibility.cpp` | preserve relative timestamp semantics |
| `D3D12Hook::is_xefg_module_loaded()` | `XeFGCompatibility::is_module_loaded()` | only read-only diagnostics |
| R1 runtime slots/hooks | **no move** | remain in `XeFGRuntimeRegistry` |
| InitDesc dispatch/common | **no move** | R3+ |

---

# 16. Forbidden Changes

The following are out of scope even if they look convenient while editing nearby code.

## 16.1 No generic frame-generation façade

Do not create:

```text
FrameGenerationCompatibility
IFrameGenerationRuntime
FGProvider
FGRegistry
DLSSG/XeFG common loader abstraction
```

This project is intentionally XeFG-specific until another real compatibility problem proves a common abstraction is necessary.

## 16.2 No loader infrastructure rewrite

Do not replace:

```text
LdrRegisterDllNotification
LdrLoadDll hook
```

with a different loader monitoring strategy.

## 16.3 No registry redesign

Do not change R1:

```text
8-slot capacity
exact-HMODULE identity
thunk dispatch
required InitDesc / optional GetSwapChainPtr semantics
FunctionHook ownership
original trampoline resolution
```

unless a concrete correctness bug is discovered and documented separately.

## 16.4 No discovery/binding changes

Do not alter swapchain/queue selection, COM ownership, rebind, Present, resize, or hold state.

## 16.5 No logging cleanup yet

Keep existing loader/XeFG diagnostic information substantially equivalent.

The final logging PR will decide what becomes debug-only.

Do not use R2 to reduce INFO logs.

---

# 17. Expected Changed Files

Preferred R2 diff:

```text
src/REFramework.cpp
src/D3D12Hook.cpp
src/D3D12Hook.hpp
src/compatibility/xefg/XeFGCompatibility.cpp    (new)
src/compatibility/xefg/XeFGCompatibility.hpp    (new)
```

`XeFGRuntimeRegistry.*` should normally require **no semantic modification**.

If the registry must be changed, the PR description must explain exactly why R2 cannot be completed through the existing R1 public API.

No unrelated files should change.

---

# 18. Size Target

Planning target from the split plan:

```text
effective implementation change: ~80–160 LOC
GitHub additions + deletions:      ~140–280 LOC
```

Because this version of R2 moves the loader/probe state fully into the façade rather than leaving a forwarding wrapper in `D3D12Hook`, a modest overrun is acceptable if the semantic scope remains exactly one responsibility.

Reasonable soft ceiling:

```text
~220 effective LOC
```

Do not merge R3 work just because the PR is below the ceiling.

---

# 19. Required Static Review Before PR

Before opening the PR, explicitly audit the final diff for all of the following.

## 19.1 `REFramework.cpp`

Confirm:

```text
[ ] generic loader registration unchanged
[ ] LdrLoadDll original call ordering unchanged
[ ] exact HMODULE still taken from *module after successful original call
[ ] GetModuleFileNameW path acquisition unchanged or equivalent
[ ] synchronous façade call occurs before LdrLoadDll hook returns
[ ] notification callback only marks pending
[ ] Streamline loader branch unchanged
[ ] game-path loader logic unchanged
[ ] hook-monitor pending processing remains in same lifecycle location
```

## 19.2 `XeFGCompatibility`

Confirm:

```text
[ ] one authoritative module-loaded atomic
[ ] one authoritative pending atomic
[ ] first-seen timestamp set once
[ ] pending notification path has no heavy work
[ ] process_pending_work uses exchange(false)
[ ] Toolhelp enumeration finds all matching modules
[ ] exact HMODULE passed unchanged to registry
[ ] no basename re-resolution
[ ] no FunctionHook ownership duplicated
[ ] no renderer/binding dependency
```

## 19.3 `D3D12Hook`

Confirm:

```text
[ ] loader/probe APIs removed from public header
[ ] loader/probe static atomics removed
[ ] InitDesc/GetSwapChainPtr dispatch methods unchanged semantically
[ ] factory transaction untouched
[ ] candidate publication untouched
[ ] binding/rebind untouched
[ ] Present/resize untouched
[ ] hook-monitor snapshot still reports XeFG module state
```

## 19.4 `XeFGRuntimeRegistry`

Confirm:

```text
[ ] exact-HMODULE identity unchanged
[ ] 8 stable slots unchanged
[ ] duplicate handling unchanged
[ ] InitDesc required semantics unchanged
[ ] GetSwapChainPtr optional semantics unchanged
[ ] original trampoline resolution unchanged
```

---

# 20. Build / Validation Requirements

Minimum required before PR completion:

```text
cmake --build build --config Release --target REFramework
```

Also run:

```text
git diff --check
```

If project CI has the existing direct-struct-access audit, it must remain green.

No new warning should be introduced.

A build failure caused by unused/missing loader includes should be fixed narrowly; do not expand into broad include cleanup.

---

# 21. R1 + R2 Runtime Wave Gate

R2 completes the first runtime-refactor wave defined in the split plan.

After R2, perform a short real runtime smoke before beginning R3 when hardware/game access is available.

## 21.1 Dragon's Dogma 2 + OptiScaler + XeFG

Expected:

```text
[ ] game launches normally
[ ] OptiScaler XeFG initializes
[ ] REFramework overlay appears
[ ] OptiScaler overlay appears
[ ] no startup crash
[ ] no repeated destructive XeFG hook installation
[ ] exact runtime installation logs still appear
[ ] no regression in presentation binding
```

R2 itself does not need to change any render behavior, so any overlay regression is a strong signal that loader timing or module handoff changed incorrectly.

## 21.2 Multi-runtime case when available

For Pragmata or another known process with multiple `libxess_fg.dll` instances:

```text
[ ] all loaded matching modules are enumerated
[ ] each unique HMODULE is handed to the R1 registry
[ ] duplicate observation of the same HMODULE remains idempotent
[ ] runtime that actually executes InitDesc still reaches D3D12Hook dispatch
```

Do not claim this runtime validation if it was not actually run.

---

# 22. Failure Analysis Guide

If R2 introduces a regression, classify it before changing code.

## 22.1 XeFG never reaches InitDesc dispatch

Check first:

```text
Did post-LdrLoadDll call XeFGCompatibility::on_module_loaded synchronously?
Did on_module_loaded pass the exact HMODULE to XeFGRuntimeRegistry?
Did registry install succeed?
Was the matching slot thunk installed?
```

Do not change discovery/binding code to fix a missing loader handoff.

## 22.2 Pending notification is seen but runtime is not installed

Check:

```text
Did mark_probe_pending set pending=true?
Did hook_monitor still call process_pending_work at the previous location?
Did process_pending_work exchange pending=false and enumerate modules?
Did Toolhelp enumeration retain TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32?
```

## 22.3 Duplicate installation logs appear

Repeated observation is not itself a bug.

The actual invariant is:

```text
same exact HMODULE
    -> one registry entry
    -> one required InitDesc FunctionHook
```

R1 may log a duplicate observation while correctly doing no second installation.

## 22.4 Startup deadlock/crash appears

First inspect loader-context work.

Verify the notification callback did not start doing:

```text
module enumeration
FunctionHook creation
registry installation
lifecycle mutex acquisition
```

The notification callback must remain lightweight.

---

# 23. PR Review Blockers

Treat the following as blocking defects.

1. `REFramework.cpp` still directly calls XeFG loader/probe methods on `D3D12Hook` after the façade is introduced.
2. `mark_probe_pending()` performs heavy loader/module/hook work.
3. Post-`LdrLoadDll` exact-module installation becomes asynchronous or pending-only.
4. Exact HMODULE is discarded and the code reverts to basename-only `GetModuleHandleW` lookup.
5. Already-loaded enumeration only handles one `libxess_fg.dll`.
6. R1 runtime registry ownership is duplicated in the façade.
7. R1 slot/thunk/original-trampoline semantics change without necessity.
8. InitDesc discovery transaction is moved/modified in R2.
9. Binding/resize/Present behavior changes.
10. Streamline/DLSSG loader behavior changes.
11. Hook-monitor preserve-binding policy changes.
12. New polling/thread/timer/sleep behavior is introduced.

The following are not automatically blockers:

- minor naming differences from this document;
- a few extra move-only LOC;
- simple RAII replacement for Toolhelp handle cleanup;
- keeping a very small temporary compatibility alias when required for compilation, provided `REFramework.cpp` no longer uses it and the reason is documented.

---

# 24. Suggested Implementation Sequence

Implement in this order to keep intermediate states understandable.

## Step 1 — Add façade skeleton

Create:

```text
XeFGCompatibility.hpp
XeFGCompatibility.cpp
```

Add static state and declarations only.

Build if useful.

## Step 2 — Move module/probe state and methods

Move:

```text
mark pending
process pending
module-loaded diagnostics
Toolhelp enumeration
first-seen state
```

Route exact module install to the existing R1 registry.

Do not touch REFramework call sites yet if an intermediate build is easier.

## Step 3 — Change `REFramework.cpp` call sites

Replace exactly three XeFG call categories:

```text
hook monitor pending processing
post-LdrLoadDll exact-module handoff
loader notification pending mark
```

## Step 4 — Remove obsolete D3D12 loader/probe bridge

Remove dead declarations, definitions, state, and now-unused includes.

Keep InitDesc dispatch bridges intact.

## Step 5 — Restore hook-monitor diagnostic query

Make `D3D12Hook::log_hook_monitor_snapshot()` read module state from the façade.

## Step 6 — Final scope audit

Use the migration table and forbidden-change list above.

Do not continue into R3.

---

# 25. Example Final Control Flow

## 25.1 XeFG runtime loaded through hooked `LdrLoadDll`

```text
REFramework::ldr_load_dll_hook
    |
    | original LdrLoadDll succeeds
    | exact HMODULE returned
    v
XeFGCompatibility::on_module_loaded
    |
    |- mark module seen / timestamp
    |- log public export availability
    v
XeFGRuntimeRegistry::install_for_module(exact HMODULE)
    |
    |- exact-HMODULE dedupe
    |- stable slot
    |- module-specific FunctionHook
    v
return from LdrLoadDll hook
```

## 25.2 XeFG observed by loader notification

```text
LdrRegisterDllNotification callback
    |
    v
XeFGCompatibility::mark_probe_pending
    |
    |- atomics only
    v
return from loader notification callback

later: REFramework::hook_monitor
    |
    v
XeFGCompatibility::process_pending_work
    |
    v
Toolhelp enumerate every loaded libxess_fg.dll
    |
    v
XeFGCompatibility::on_module_loaded(each exact HMODULE)
    |
    v
XeFGRuntimeRegistry::install_for_module
```

## 25.3 XeFG public InitDesc executes

R2 does not change this path:

```text
R1 stable runtime thunk
    |
    v
D3D12Hook::xefg_init_desc_dispatch
    |
    v
D3D12Hook::xefg_init_desc_common
    |
    v
existing factory capture / validation / binding
```

This unchanged lower path is an important R2 acceptance condition.

---

# 26. PR Description Template

Suggested PR body:

```markdown
## Summary

- add `XeFGCompatibility` as the narrow XeFG loader/probe façade
- move XeFG pending/module-seen state and already-loaded runtime enumeration out of `D3D12Hook`
- route exact module installation directly to the R1 `XeFGRuntimeRegistry`
- replace `REFramework.cpp` direct XeFG runtime-management calls with façade calls
- preserve existing loader timing and presentation/discovery behavior

## Not in scope

- InitDesc/factory discovery extraction (R3)
- queue/candidate extraction (R4)
- pending binding / active binding changes (R5-R7)
- Present/resize lifecycle changes (R8-R9)
- hook-monitor policy cleanup (R10)
- logging cleanup / Debug Logging UI

## Critical invariants

- loader notification callback remains lightweight/pending-only
- successful post-LdrLoadDll exact-HMODULE installation remains synchronous
- already-loaded enumeration remains multi-module
- R1 exact-HMODULE registry remains authoritative
- D3D12 InitDesc/discovery/binding behavior is unchanged

## Validation

- Release build: PASS/FAIL
- `git diff --check`: PASS/FAIL
- runtime smoke: performed / not performed
```

---

# 27. Completion Criteria

R2 is complete only when all of the following are true:

```text
[ ] XeFGCompatibility.hpp/.cpp exist
[ ] REFramework.cpp no longer calls XeFG loader/probe management on D3D12Hook
[ ] notification callback remains lightweight
[ ] post-LdrLoadDll exact-HMODULE install remains synchronous
[ ] pending probe processing remains on existing hook-monitor path
[ ] all already-loaded libxess_fg.dll modules are enumerated
[ ] XeFGRuntimeRegistry is the only owner of runtime FunctionHooks
[ ] D3D12Hook loader/probe APIs/state are removed or reduced to no longer-owning code with documented necessity
[ ] InitDesc dispatch/common logic is behaviorally unchanged
[ ] discovery/candidate/binding/Present/resize code is unchanged
[ ] Streamline/DLSSG handling is unchanged
[ ] Release build passes
[ ] git diff --check passes
[ ] final diff contains no R3+ work
```

---

# 28. Final Reminder to the Implementing Agent

This is not a feature PR.

The working XeFG compatibility behavior already exists.

R2 succeeds by changing **who owns loader/probe coordination**, not by changing how XeFG is rendered.

The safest implementation is the smallest one that establishes this boundary:

```text
Windows loader mechanics
    -> REFramework.cpp

XeFG loader/probe policy
    -> XeFGCompatibility

exact runtime hook ownership
    -> XeFGRuntimeRegistry

presentation/discovery/binding
    -> D3D12Hook (for now)
```

If a proposed cleanup affects the XeFG InitDesc transaction, queue selection, swapchain binding, Present/resize behavior, or hook-monitor recovery policy, stop and defer it to the later PR that owns that responsibility.
