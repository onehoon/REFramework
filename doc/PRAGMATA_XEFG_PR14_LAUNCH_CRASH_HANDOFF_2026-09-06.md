# Pragmata XeFG / PR14 Launch-Crash Handoff

Date: 2026-09-06

Repository: `onehoon/REFramework`

Scope: **Pragmata only.**

This document intentionally does **not** cover MHW Alt+Enter/P3.3A, Onimusha/OptiScaler root-signature behavior, intermittent DLSS availability, or the final Requiem stutter comparison.

---

## 1. Executive summary

Pragmata currently exposes **two separate XeFG compatibility problems** depending on REFramework revision/topology:

1. **P3.2-era build:** the game can launch and OptiScaler can present, but REFramework may hook the wrong `libxess_fg.dll` instance and never perform XeFG `ExternalBind`, so the REFramework overlay does not appear.
2. **PR #14 multi-runtime build:** Pragmata can terminate during startup **before D3D12/Present/XeFG InitDesc dispatch**, including in a root-only configuration where only one XeFG runtime is registered.

The second result is important because it means:

> The current launch crash cannot be explained only by “two `libxess_fg.dll` modules were registered at the same time.”

A dual-runtime layout may still influence which XeFG runtime becomes active, but **multiple registered modules are not a proven necessary condition for the launch crash**.

The strongest current hypothesis is narrower:

> Pragmata may be sensitive to REFramework inline-patching the **XeFG runtime that OptiScaler actually uses**, regardless of whether there is one runtime or multiple runtimes in the process.

This is not yet proven. The next test must distinguish a PR14 registry/thunk regression from a more general “active XeFG runtime export hook” problem.

---

## 2. Critical provenance correction

### All Pragmata logs discussed in this investigation are AMD tests

Do **not** infer GPU vendor from folder names.

The user explicitly clarified that **all current Pragmata test sessions are AMD**. Some historical local log folder names contain `Intel`, for example:

```text
C:\GoogleDrive\ref-xefg\Intel\P3.3-multi\pragmata-launchcrash
```

That directory name is stale/misclassified and must **not** be treated as hardware provenance.

Known Pragmata test GPU from the earlier P3.2 session:

```text
AMD Radeon RX 6700 XT
```

Therefore this handoff must not describe the launch crash as an Intel result.

---

## 3. Relevant REFramework revisions

### P3.2-era Pragmata session

Observed REF log commit:

```text
7ea40d2785f76eda945885797b8b43dc5fd99925
```

This session successfully reached D3D12 and created the game/XeFG swapchain path, but REFramework did not bind the actual OptiScaler XeFG presentation path.

### PR #14 multi-runtime implementation

PR:

```text
https://github.com/onehoon/REFramework/pull/14
```

Title:

```text
fix: support multiple XeFG runtime modules
```

PR head:

```text
d437518eef542cc6a4639b2d7cbdef38e9f040c4
```

Merged commit:

```text
b4ff1092a16ee85a3ce4f9a017395d797a4d233d
```

Hardware test builds in the crash logs identify:

```text
cebe978ed82e85027d94bdea1fede6216cc14d0d
```

This is the PR14 test tree/build used for the current launch-crash reproduction.

---

## 4. Why PR #14 existed

The original P2/P3 XeFG API interception assumed a single public XeFG runtime per process:

```cpp
std::unique_ptr<FunctionHook> g_xefg_init_hook{};
std::unique_ptr<FunctionHook> g_xefg_get_swapchain_hook{};
```

and selected it with basename-only lookup:

```cpp
GetModuleHandleW(L"libxess_fg.dll")
```

Pragmata demonstrated that more than one `libxess_fg.dll` HMODULE can coexist.

In the earlier AMD P3.2 + OptiScaler subfolder session, REFramework hooked a root/`_storage_` module while OptiScaler actually used a different module under its own directory.

Approximate topology:

```text
Pragmata process
|
+-- libxess_fg.dll A
|   displayed by REF module mapping as:
|   ...\PRAGMATA\_storage_\libxess_fg.dll
|   base in the earlier P3.2 log: 0x7FFE791F0000
|   REF hooked this module
|
+-- libxess_fg.dll B
    ...\PRAGMATA\OptiScaler\libxess_fg.dll
    base in the earlier P3.2 log: 0x7FFE6E810000
    OptiScaler actually used this runtime
```

The public `XefgInterpolationSwapChain` vtable in that P3.2 run belonged to the `OptiScaler\libxess_fg.dll` module, not the module REFramework had hooked.

Result:

```text
InitDesc seen by REF       = 0 for the active Opti runtime
ExternalBind               = 0
PresentEntry through REF   = not established
OptiScaler Present path    = alive
REFramework overlay        = absent
```

PR #14 therefore changed XeFG API discovery from a single basename-selected module into an exact-HMODULE runtime registry with per-slot hook thunks.

---

## 5. PR #14 implementation shape

PR #14 added:

```cpp
constexpr size_t kMaxXefgRuntimes = 8;

struct XefgRuntimeHook {
    HMODULE module{};
    std::wstring path{};
    size_t slot{};
    FARPROC init_desc_export{};
    FARPROC get_swapchain_export{};
    std::unique_ptr<FunctionHook> init_desc_hook{};
    std::unique_ptr<FunctionHook> get_swapchain_hook{};
    XefgRuntimeInstallState state{ XefgRuntimeInstallState::Empty };
};

std::array<std::optional<XefgRuntimeHook>, kMaxXefgRuntimes> g_xefg_runtimes{};
```

and slot-specific thunks such as:

```cpp
template <size_t Slot>
int32_t WINAPI xefg_init_desc_thunk(...) {
    return D3D12Hook::xefg_init_desc_dispatch(Slot, ...);
}
```

The intent was correct:

- enumerate already-loaded `libxess_fg.dll` modules,
- preserve exact `HMODULE`,
- register later loads using exact loader-returned module handles,
- keep a separate original trampoline per runtime,
- serialize the existing shared InitDesc/factory-capture transaction.

The registry/discovery part is observed to work.

The problem is that Pragmata now exposes a launch failure before any registered runtime actually reaches `RuntimeDispatch`.

---

## 6. Evidence set A — AMD P3.2, OptiScaler subfolder, game launches

Local evidence directory:

```text
C:\GoogleDrive\ref-xefg\AMD\P3.2\pragmata
```

OptiScaler version in that session:

```text
10.0.0-dev (15558603)
```

OptiScaler selected:

```text
DllPath = ...\PRAGMATA\OptiScaler
FGInput = DLSSG
FGOutput = XeFG
Dx12Upscaler = XeSS
```

Important timeline:

```text
OptiScaler loads root XeFG module / observes one already in memory
OptiScaler separately loads ...\OptiScaler\libxess_fg.dll
REF starts
REF hooks only the root/_storage_ XeFG module
D3D12 loads
Render frame 1
REF hooks D3D12 successfully
XeFG public swapchain later appears
its vtable owner = ...\OptiScaler\libxess_fg.dll
classification = unclassified
```

The game therefore **launched successfully** and reached D3D12.

Representative REF sequence:

```text
D3D12 loaded
Render frame: 1
Hooking D3D12
Hooked DirectX 12
```

Later public XeFG proxy candidate:

```text
type_name = struct XefgInterpolationSwapChain
Present[8] owner = ...\OptiScaler\libxess_fg.dll
Present1[22] owner = ...\OptiScaler\libxess_fg.dll
ResizeBuffers1[39] owner = ...\OptiScaler\libxess_fg.dll
classification = unclassified
```

### P3.2 result

```text
Game launch              PASS
D3D12 initialization     PASS
OptiScaler presentation  PASS
REF active XeFG bind     FAIL
REF overlay              FAIL
```

This is the original compatibility problem PR #14 attempted to solve.

---

## 7. Evidence set B — AMD PR14, dual/subfolder layout, launch crash

Historical local evidence directory name:

```text
C:\GoogleDrive\ref-xefg\Intel\P3.3-multi\pragmata-launchcrash
```

Again: **despite the folder name, this Pragmata result is AMD.**

In this session PR14 detected and registered two XeFG runtimes, approximately:

```text
slot 0
...\PRAGMATA\_storage_\libxess_fg.dll

slot 1
...\PRAGMATA\OptiScaler\libxess_fg.dll
```

Both export hook installations reported success.

After registration, however, the REF log remained at:

```text
Waiting for D3D12...
Waiting for D3D12...
...
```

and the process terminated before normal D3D12 presentation startup.

Observed counts:

```text
Hooking D3D12           = 0
Present / Present1      = 0
ExternalBind            = 0
ResizeBuffers1          = 0
Reset                   = 0
renderer initialization = 0
Opti LocalPresent       = 0
XeFG swapchain created  = 0
RuntimeDispatch         = 0
```

### Important interpretation

Because `RuntimeDispatch` is zero, this log does **not** show:

```text
wrong slot selected
-> wrong original trampoline called
-> crash inside detour dispatch
```

The crash/termination occurs earlier than that.

It is therefore reasonable to suspect the act/timing of export patch installation itself, but that is still a hypothesis.

---

## 8. Evidence set C — AMD PR14, root-only layout, still launch crashes

Local evidence directory:

```text
C:\GoogleDrive\ref-xefg\AMD\P3.3-multi\prag
```

OptiScaler version:

```text
0.9.5-pre3 (50b2364b)
```

OptiScaler reports:

```text
Setting DllPath to ...\PRAGMATA
```

and uses the root `libxess_fg.dll`.

REF sees only **one registered XeFG runtime**:

```text
[XeFG][Module]
base = 0x7ffe62cc0000
full_path = ...\PRAGMATA\_storage_\libxess_fg.dll

[XeFG][RuntimeRegistry]
action = installed
slot = 0
module = 0x7ffe62cc0000
init_desc = 0x7ffe62ccc450
get_swapchain = 0x7ffe62cc86a0
```

Immediately afterward:

```text
[REFramework] Waiting for D3D12...
...
```

No D3D12 startup follows before the log ends.

Again:

```text
RuntimeDispatch = 0
InitDesc         = 0
ExternalBind     = 0
Present          = 0
```

### Why this test matters

This root-only reproduction **falsifies the simple hypothesis**:

> “Pragmata crashes only because PR14 registers/hooks two XeFG runtimes at once.”

There is only one registered runtime in this test, yet the launch failure remains.

Therefore multi-module count alone is not sufficient to explain the failure.

---

## 9. Important `_storage_` clarification

REFramework's module bookkeeping/copy mechanism can report a game-root module as being associated with an `_storage_` path.

Do not automatically interpret:

```text
...\PRAGMATA\libxess_fg.dll
```

and:

```text
...\PRAGMATA\_storage_\libxess_fg.dll
```

as two simultaneously active HMODULEs merely because both path strings appear in logs.

For runtime multiplicity, use **module base / HMODULE identity**, not path spelling alone.

In the root-only PR14 crash session, the relevant XeFG runtime is one HMODULE:

```text
0x7ffe62cc0000
```

and only slot 0 is installed.

---

## 10. What is currently proven

### Proven

1. Pragmata current test evidence is AMD.
2. P3.2 can launch Pragmata and reach D3D12 in at least the tested OptiScaler-subfolder topology.
3. P3.2 can miss the actual OptiScaler XeFG runtime and therefore fail to `ExternalBind`.
4. PR14 successfully discovers/registers exact XeFG HMODULEs.
5. PR14 dual-runtime/subfolder Pragmata test fails before D3D12 presentation startup.
6. PR14 root-only/single-runtime Pragmata test also fails before D3D12 presentation startup.
7. Both PR14 launch-failure sessions have zero observed `RuntimeDispatch`.
8. Therefore “two runtime dispatches race and call the wrong original trampoline” is **not supported by current runtime evidence**.
9. Therefore “two registered XeFG runtimes are required to trigger the crash” is **false for the root-only reproduction**.

---

## 11. What is NOT proven

Do not state any of the following as established fact:

- “AMD GPUs cannot use the PR14 XeFG hook.”
- “Pragmata crashes because two `libxess_fg.dll` files exist.”
- “The PR14 template thunk has a bad calling convention.”
- “The per-slot original trampoline is wrong.”
- “GetSwapChainPtr is definitely the crashing hook.”
- “InitFromSwapChainDesc is definitely the crashing hook.”
- “Pragmata performs a XeFG DLL integrity check.”
- “REFramework's `_storage_` copy itself causes the crash.”
- “OptiScaler 10 introduced the crash.”

There is currently no crash dump/exception address proving any of these.

---

## 12. Friend's cross-game observation

The AMD tester reports that behavior varies by game.

Their impression is that failures may be more likely in layouts where both:

```text
game root\libxess_fg.dll
```

and:

```text
OptiScaler\libxess_fg.dll
```

exist.

Treat this as **useful field observation, not yet a proven rule**.

It is consistent with the original P3.2 wrong-module selection problem, because a dual layout makes it possible for REF to hook one runtime while OptiScaler uses another.

However, the Pragmata **root-only PR14 crash** proves that dual-file coexistence is not a required condition for the current launch failure.

---

## 13. Current leading hypotheses

Rank these as hypotheses, not conclusions.

### H1 — Active XeFG runtime export patching is unsafe in Pragmata

This currently fits both layouts best.

P3.2 subfolder case:

```text
REF hooks root/_storage_ runtime A
OptiScaler actually uses runtime B
-> launch survives
-> REF misses active XeFG InitDesc
-> overlay absent
```

PR14 dual case:

```text
REF hooks A and B
B is the active Opti runtime
-> launch fails before RuntimeDispatch
```

PR14 root-only case:

```text
only runtime A exists
A is also the active Opti runtime
REF hooks A
-> launch fails before RuntimeDispatch
```

This correlation is strong enough to test directly.

It is **not yet proof** that FunctionHook patching the active runtime is the root cause.

### H2 — PR14 registry/thunk refactor introduced a single-runtime regression

Also plausible.

Even slot 0/single-runtime now goes through:

```text
runtime registry
-> slot-specific thunk destination
-> per-runtime FunctionHook ownership
```

instead of the original singleton hook object.

The root-only PR14 failure could therefore be a generic PR14 regression unrelated to whether the runtime is active.

The most important next A/B test exists specifically to distinguish H1 from H2.

### H3 — GetSwapChainPtr hook installation is the trigger

PR14 installs both:

```text
InitFromSwapChainDesc
GetSwapChainPtr
```

before the crash boundary.

There is no dispatch evidence for either, so an InitDesc-only diagnostic build is useful.

### H4 — Pragmata-specific startup/integrity interaction

Pragmata has substantial REFramework integrity bypass/scanning activity during startup.

It is possible that changing code bytes in the active XeFG runtime creates a Pragmata-specific interaction before the API is called.

This remains speculative until another AMD game with the same active-runtime hook is compared or an exception/dump identifies the failing code.

---

## 14. Highest-value next tests

Do **not** jump directly into a broad architecture rewrite.

### Test 1 — P3.2 root-only Pragmata

This is the highest-value missing A/B.

Use the same root-only OptiScaler layout that crashes on PR14, but replace only REFramework with the known P3.2-era build.

Goal:

> Determine whether the old singleton `FunctionHook` also crashes when it hooks the XeFG runtime that OptiScaler is actually using.

Interpretation:

#### If P3.2 root-only also crashes

Then PR14's registry/thunk conversion is probably **not** the fundamental problem.

The stronger model becomes:

```text
REF inline-hooks active XeFG runtime
-> Pragmata launch failure
```

P3.2 subfolder merely survived because it accidentally hooked the inactive/wrong XeFG module.

#### If P3.2 root-only launches but PR14 root-only crashes

Then there is a genuine **PR14 single-runtime regression**.

Focus next on:

- thunk destination form,
- FunctionHook lifetime/ownership,
- install ordering,
- registry publication state,
- additional GetSwapChainPtr hook behavior,
- startup enumeration timing differences.

### Test 2 — PR14 registry-only build

Keep:

- exact-HMODULE enumeration,
- runtime registry,
- slot/path/module diagnostics.

Do **not** call `FunctionHook::create()` for either XeFG export.

Expected purpose:

```text
registry-only launch PASS
```

would prove that discovery/registry bookkeeping itself is harmless and move the failure boundary to export patch installation.

### Test 3 — PR14 InitDesc-only build

Install only:

```text
xefgSwapChainD3D12InitFromSwapChainDesc
```

Do not hook:

```text
xefgSwapChainD3D12GetSwapChainPtr
```

Interpretation:

```text
InitDesc-only PASS
both hooks FAIL
```

would isolate `GetSwapChainPtr` hook installation as the likely trigger.

If InitDesc-only still fails before dispatch, then active InitDesc export patching itself becomes the stronger candidate.

### Test 4 — another AMD Capcom/RE Engine game on PR14

Use a game already known to launch under P3.2.

Record exact XeFG topology:

- root XeFG exists?
- OptiScaler subfolder XeFG exists?
- number of HMODULEs,
- exact registered slots,
- active public proxy owner,
- launch success/failure.

This separates a Pragmata-specific interaction from a broader active-runtime-hook problem.

---

## 15. Recommended diagnostic build switches

For the next implementation/test PR, keep changes deliberately small.

A compile-time diagnostic mode is sufficient, for example conceptually:

```cpp
enum class XefgApiHookMode {
    RegistryOnly,
    InitDescOnly,
    InitDescAndGetSwapChain,
};
```

Do not ship user-facing configuration for this yet.

The goal is reproducible A/B binaries, not permanent configurability.

Recommended logs before each hook creation:

```text
[XeFG][HookInstall]
slot
module
path
api
export address
destination thunk
mode
```

and immediately after:

```text
[XeFG][HookInstall]
action = success/fail
```

Keep current `RuntimeDispatch` logging.

Do not add per-frame logging.

---

## 16. Do not change these areas while isolating Pragmata launch crash

Until the launch boundary is identified, do not mix in:

- P3.2 binding transaction redesign,
- presentation queue policy changes,
- Present/Present1 changes,
- ResizeBuffers1 behavior,
- P3.3 MHW resize diagnostics/fixes,
- hook-monitor timeout/recovery changes,
- public `XefgInterpolationSwapChain` direct binding,
- GPU-vendor-specific branching,
- OptiScaler path-name hardcoding,
- Intel private XeFG offsets,
- Streamline/DLSS changes.

The current failure is before those presentation lifecycle paths are exercised.

---

## 17. Current status matrix

| Scenario | GPU | XeFG topology | REF behavior | Result |
|---|---|---|---|---|
| Pragmata P3.2 + OptiScaler subfolder | AMD | root/_storage + OptiScaler runtime | hooks wrong/root runtime; active Opti runtime missed | **Launch PASS, REF overlay FAIL** |
| Pragmata PR14 + OptiScaler subfolder | AMD | two runtime HMODULEs registered | both runtime hooks installed | **Launch FAIL before D3D12/RuntimeDispatch** |
| Pragmata PR14 + root-only | AMD | one runtime HMODULE / slot 0 | InitDesc + GetSwapChainPtr hooks installed | **Launch FAIL before D3D12/RuntimeDispatch** |
| Pragmata P3.2 + root-only | AMD | one active runtime expected | **not tested yet** | **MISSING / highest-value A/B** |
| Pragmata PR14 registry-only | AMD | one or more discovered, no export patch | **not tested yet** | **MISSING** |
| Pragmata PR14 InitDesc-only | AMD | exact runtime, only InitDesc patched | **not tested yet** | **MISSING** |

---

## 18. Working conclusion for handoff

Do not hand this off as “multi-module support crashes when two XeFG DLLs are present.”

The more accurate current statement is:

> Pragmata originally exposed a real multi-HMODULE discovery problem: P3.2 could hook a root/`_storage_` XeFG runtime while OptiScaler used a different XeFG runtime, causing `ExternalBind`/overlay failure. PR #14 fixed exact-HMODULE discovery and per-runtime registration, but hardware validation then exposed a new/previously-hidden launch failure. The launch failure occurs even in a root-only configuration with a single registered runtime and before any XeFG RuntimeDispatch/D3D12 Present path executes. Therefore dual-module count alone is not the root cause. The leading possibilities are (a) Pragmata cannot tolerate REFramework inline-hooking the XeFG runtime OptiScaler actually uses, or (b) PR14 introduced a single-runtime hook regression. The next decisive test is P3.2 root-only versus PR14 root-only, followed by registry-only and InitDesc-only PR14 diagnostic builds.

Until those A/B results exist, do not redesign the presentation lifecycle or add AMD-specific behavior.
