# REFramework × OptiScaler XeFG Refactor — Fine-Grained PR Split Plan

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Execution baseline code: `0a5f7a3bfca4367abe3379cc2635d1a7bb894f53` (`fix: hold XeFG rendering across resize transitions (#16)`)  
Architecture document: `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_ARCHITECTURE_2026-09-06.md`

---

## 1. Purpose of this split plan

This document refines the implementation sequence from the architecture document into much smaller pull requests.

It **supersedes the coarse PR 1 / PR 2 / PR 3 execution split** in the architecture document. The architecture, invariants, scope, and non-goals in that document remain authoritative; only the implementation granularity is replaced by this plan.

The reason for the finer split is practical:

- reduce agent implementation load per PR;
- reduce review load per PR;
- make regressions attributable to one semantic change;
- make revert/bisect easy;
- avoid combining discovery, binding, lifecycle, and upstream-isolation risks;
- preserve working REFramework behavior while moving XeFG code out of upstream-sensitive files.

The guiding rule is:

> One PR should have one primary ownership change and one obvious failure domain.

Do not combine adjacent PRs merely because the total diff looks small.

---

## 2. Scope remains unchanged

This remains an **OptiScaler + Intel XeFG + D3D12 compatibility refactor**.

Do not expand the work into:

- generic frame-generation provider infrastructure;
- FSRFG changes;
- DLSSG/Streamline changes;
- D3D11 refactoring;
- generic DXGI redesign;
- REFramework renderer redesign;
- Lua/plugin/mod cleanup;
- input/UI/VR cleanup;
- hooking-library replacement;
- OptiScaler private implementation hooks;
- unrelated modernization.

REFramework's original functionality is the highest-priority compatibility requirement.

---

## 3. Current code seams used for the split

Current master places several independent responsibilities in `D3D12Hook.cpp` / `D3D12Hook.hpp`:

1. XeFG module/runtime registry;
2. fixed runtime thunk dispatch;
3. XeFG export hook installation;
4. loader/probe handoff;
5. serialized `InitFromSwapChainDesc` transaction;
6. temporary `CreateSwapChainForHwnd[15]` capture;
7. queue/device identity classification;
8. binding candidate construction/publication;
9. pending candidate handoff into a live `D3D12Hook`;
10. active XeFG strong COM ownership;
11. initial external instance binding;
12. transactional active binding replacement;
13. binding generation/mode identity;
14. resize lifecycle state;
15. `ResizeTarget` transition hold;
16. Present/Present1 render suppression policy;
17. hook-monitor preservation policy;
18. extensive diagnostics.

These responsibilities are separable enough that they should not be moved in three large steps.

---

## 4. LOC policy

LOC numbers below are planning estimates, not merge gates.

Two numbers matter:

- **effective implementation LOC**: code whose ownership/API/logic is materially being changed;
- **GitHub diff LOC**: additions + deletions, which can be roughly doubled by file moves.

Target per PR:

```text
preferred effective change: 100–300 LOC
soft ceiling:               ~350 LOC
exception:                  only when a single indivisible protocol requires more
```

A move-only PR may show 400–700 GitHub changed lines while still being a small semantic change.

Do not artificially split one safety-critical transaction in the middle only to hit an LOC number.

---

# 5. Detailed PR sequence

## R1 — Extract XeFG Runtime Registry

### Goal

Move the exact-HMODULE XeFG runtime registry and fixed slot/thunk ownership out of `D3D12Hook.cpp` without changing discovery or binding behavior.

### Move in this PR

- `kMaxXefgRuntimes`;
- runtime install-state enum;
- per-runtime record containing:
  - `HMODULE`;
  - path;
  - slot;
  - export addresses;
  - `FunctionHook` objects;
- runtime slot allocation;
- exact-HMODULE duplicate lookup;
- runtime lookup by slot;
- stable fixed InitDesc/GetSwapChainPtr thunk arrays;
- required `InitFromSwapChainDesc` export resolution;
- optional `GetSwapChainPtr` export resolution;
- export hook creation and per-module original function ownership.

### Keep out of this PR

- init transaction logic;
- factory hook/capture;
- queue validation;
- candidate publication;
- active binding state;
- resize/present logic;
- hook monitor;
- logging cleanup.

### Temporary bridge allowed

The extracted registry may temporarily dispatch into existing `D3D12Hook` XeFG functions. That bridge is intentionally temporary and removed later.

Do not create a generic callback/event framework solely to avoid that temporary bridge.

### Critical invariants

- exact `HMODULE` determines the correct original export;
- 8-slot current capacity remains unchanged;
- duplicate exact module does not create a second hook;
- missing required InitDesc export rejects only that runtime;
- optional GetSwapChainPtr hook failure does not invalidate InitDesc support;
- no single-global-module fallback is introduced.

### Estimated size

```text
effective:        ~180–280 LOC
GitHub add+delete: ~350–600 LOC
```

### Validation

- Release build;
- existing static checks;
- `git diff --check`;
- verify runtime registry has no renderer/binding ownership;
- verify D3D12 native path diff is limited to bridge calls/includes.

---

## R2 — Isolate XeFG Loader / Probe Handoff

### Goal

Remove direct XeFG runtime-management calls from `REFramework.cpp` while preserving the existing loader timing behavior.

### Move/change in this PR

Create the narrow compatibility façade entry points needed for:

- loader notification observed;
- exact XeFG module loaded after successful `LdrLoadDll`;
- pending compatibility work processing;
- already-loaded runtime enumeration handoff.

Change `REFramework.cpp` from calls such as direct `D3D12Hook::notify_xefg_module_loaded()` / `mark_xefg_probe_pending()` / `process_pending_xefg_probe()` to the compatibility façade.

### Keep in `REFramework.cpp`

The generic loader infrastructure itself may remain there for now:

- `LdrRegisterDllNotification` mechanism;
- `LdrLoadDll` interception mechanics;
- other non-XeFG loader handling such as Streamline and game-path logic.

The key goal is to remove XeFG **policy/ownership**, not to relocate all loader code.

### Critical invariant

The successful post-`LdrLoadDll` exact-module handoff must still happen before the caller can proceed to resolve/invoke the newly loaded XeFG API, matching the current working ordering.

Do not defer this to an arbitrary background worker.

### Estimated size

```text
effective:        ~80–160 LOC
GitHub add+delete: ~140–280 LOC
```

### Validation

- Release build;
- no change to DLSSG/Streamline loader branch;
- no heavy work added to unsafe loader-notification context;
- existing exact XeFG module handoff preserved.

### Runtime gate after R2

A short DD2 + XeFG launch smoke is recommended after R1+R2 as the first wave gate.

Expected:

- game launches;
- XeFG initializes;
- both overlays still appear;
- no repeated runtime-hook installation;
- no startup crash.

---

## R3 — Extract XeFG Init Transaction and Factory Capture

### Goal

Move only the bounded `InitFromSwapChainDesc` observation transaction and temporary DXGI factory instance hook out of `D3D12Hook.cpp`.

### Move in this PR

- init transaction structure/state;
- recursive transaction mutex;
- transaction begin/reset;
- temporary `IDXGIFactory2` instance `VtableHook` ownership;
- `CreateSwapChainForHwnd[15]` capture callback;
- captured:
  - context;
  - HWND;
  - init queue;
  - factory;
  - presentation queue passed to factory creation;
  - internal swapchain candidate;
  - create success;
  - outer InitDesc result;
- transaction cleanup after original InitDesc returns.

### Keep out of this PR

- queue identity policy;
- candidate accept/reject policy;
- selected presentation queue decision;
- pending-binding handoff;
- active binding/rebind;
- resize lifecycle.

### Design requirement

The output should be an observation/result object, conceptually:

```cpp
struct XeFGDiscoveryObservation {
    void* context{};
    HWND hwnd{};
    ID3D12CommandQueue* init_queue{};
    ID3D12CommandQueue* presentation_queue{};
    IDXGISwapChain1* candidate{};
    bool factory_create_succeeded{};
    int32_t init_result{};
};
```

Exact type naming is implementation-defined.

### Critical invariants

- transaction remains serialized;
- factory hook exists only for the bounded outer InitDesc transaction;
- original XeFG InitDesc is called exactly once;
- temporary factory hook is removed after original InitDesc returns;
- no active binding mutation happens inside this extraction component.

### Estimated size

```text
effective:        ~180–280 LOC
GitHub add+delete: ~350–600 LOC
```

### Validation

- Release build;
- original export call/return contract unchanged;
- no persistent factory hook remains after transaction completion;
- no new thread/timer introduced.

---

## R4 — Extract Queue Validation and Candidate Construction

### Goal

Separate "what did XeFG create?" from "is this a valid REFramework render binding?".

### Move in this PR

- queue identity snapshot capture;
- COM identity comparison;
- device identity comparison;
- command queue type validation;
- current queue-relation classification;
- internal swapchain interface validation;
- HWND validation used by current policy;
- selected presentation queue decision;
- observe-only/render-mode decision already present in current master;
- immutable candidate creation.

Recommended candidate shape:

```cpp
struct XeFGBindingCandidate {
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12CommandQueue> presentation_queue;
    ComPtr<ID3D12Device4> device;
    HWND hwnd{};
    XeFGQueueRelation relation{};
    bool observe_only{true};
};
```

### Keep out of this PR

- pending candidate delivery into the live hook;
- active binding mutation;
- VtableHook replacement;
- resize/present behavior.

### Critical invariants

- presentation-creation queue remains authoritative on the proven distinct-same-device path;
- a device mismatch is not accepted as a render binding;
- non-DIRECT presentation queue is not silently accepted;
- public `GetSwapChainPtr` proxy does not become render authority;
- invalid observation produces no active mutation.

### Estimated size

```text
effective:        ~150–250 LOC
GitHub add+delete: ~280–500 LOC
```

### Validation

- Release build;
- validation is deterministic from one discovery observation;
- no `D3D12Hook` renderer reset occurs here;
- rejected candidate leaves active state untouched.

### Runtime gate after R4

Run the discovery wave smoke:

```text
DD2 + OptiScaler + XeFG
    internal swapchain still selected
    presentation queue still selected
    both overlays visible

known multi-runtime game / Pragmata case where available
    exact-module behavior does not regress
```

---

## R5 — Extract Pending Candidate Handoff

### Goal

Move the pending candidate storage/consume mechanics out of `D3D12Hook` implementation while leaving the actual active bind method unchanged.

### Why this is separate

Candidate construction and active binding currently meet through lifecycle-sensitive code. Keeping that handoff as its own PR makes failures easy to attribute:

- R4 failure = discovery/validation;
- R5 failure = candidate delivery/lifecycle timing;
- later failure = active binding implementation.

### Move in this PR

- pending candidate storage;
- publish/replace semantics for pending candidate;
- consume-on-hook path;
- immediate handoff to an existing `D3D12Hook` under the existing lifecycle-mutex contract;
- constructor-time fallback semantics currently required before the framework lifecycle mutex is published.

### Keep out of this PR

- changes to `bind_external_swapchain()` behavior;
- changes to `replace_xefg_binding()` behavior;
- COM ownership relocation;
- resize/present changes.

### Critical invariant

The current capture-before-hook case must not become a lost-candidate race.

Do not replace the current bounded handoff with polling, arbitrary sleep, or a background worker.

### Estimated size

```text
effective:        ~100–180 LOC
GitHub add+delete: ~180–320 LOC
```

### Validation

- Release build;
- candidate published before hook creation is still consumed;
- candidate published with an existing hook still takes the current immediate path;
- no duplicate binding caused by publish+consume overlap.

---

## R6 — Extract Active XeFG Strong Ownership and Identity

### Goal

Create the dedicated active XeFG binding-state object, but do **not** yet redesign the physical bind/rebind transaction.

### Move in this PR

Semantic state only:

- strong `ComPtr<IDXGISwapChain3>`;
- strong selected `ComPtr<ID3D12CommandQueue>`;
- strong `ComPtr<ID3D12Device4>`;
- active/inactive state;
- binding generation;
- observe-only/render mode;
- identity comparison helpers;
- complete-binding health predicate;
- getters used to keep legacy raw D3D12Hook aliases synchronized.

### Important compatibility rule

Do not rewrite normal REFramework renderer code to use the new object.

While XeFG is active, existing raw fields may remain aliases:

```text
m_swap_chain    == binding.swapchain().Get()
m_command_queue == binding.queue().Get()
m_device        == binding.device().Get()
```

This limits upstream-sensitive changes.

### Keep out of this PR

- changed-object hook replacement protocol;
- resize hold;
- Present suppression;
- hook-monitor caller cleanup.

### Critical invariant

Strong ownership must remain alive for the full lifetime of the physical instance hook.

This PR must not accidentally make `VtableHook` the owner of the COM target; it is not.

### Estimated size

```text
effective:        ~140–240 LOC
GitHub add+delete: ~250–450 LOC
```

### Validation

- Release build;
- health predicate requires all current binding invariants;
- raw aliases remain consistent when XeFG is active;
- native D3D12 path retains raw ownership semantics unchanged.

---

## R7 — Isolate Initial Bind and Transactional Rebind Protocol

### Goal

Move/encapsulate the active XeFG binding mutation protocol around the new binding-state object while keeping physical callback functions in `D3D12Hook`.

This is the highest-risk PR in the sequence and is intentionally isolated.

### In scope

#### Initial bind

- validate required inputs;
- acquire strong candidate ownership before installing the instance hook;
- install current required slots:
  - Present[8];
  - Present1[22];
  - ResizeBuffers[13];
  - ResizeTarget[14];
  - ResizeBuffers1[39];
- synchronize legacy D3D12Hook raw aliases;
- commit source/mode/generation.

#### Same-object update

- detect queue/mode change on same swapchain;
- reset renderer once;
- retain physical instance hook;
- replace strong queue/device ownership;
- update mode;
- increment generation.

#### Changed-object replacement

Required sequence remains:

```text
validate candidate
-> acquire strong candidate ownership
-> prepare complete new VtableHook
-> if preparation fails: old binding untouched
-> reset old renderer
-> remove old hook while old COM ownership is still alive
-> commit new strong ownership + hook + aliases
-> increment generation
```

### Keep out of this PR

- resize transition state extraction;
- Present suppression cleanup;
- hook-monitor caller changes;
- log level cleanup.

### Forbidden simplifications

- no hook-first/release-old-first ordering;
- no `Release` hook addition;
- no switch to public XeFG proxy;
- no queue-policy change;
- no hooking-library replacement.

### Estimated size

```text
effective:        ~220–340 LOC
GitHub add+delete: ~400–650 LOC
```

### Validation

- Release build;
- identical candidate is no-op;
- same swapchain + changed queue/mode updates safely;
- changed swapchain prepares new hook before old destruction;
- simulated/preparation failure preserves old binding;
- generation increments only on accepted change.

### Runtime gate after R7

Binding wave smoke:

```text
DD2 initial XeFG bind
both overlays visible
Alt+Tab/re-entry smoke
known re-init/rebind scenario where available
no hook-monitor rehook loop
```

---

## R8 — Extract XeFG Resize Lifecycle State

### Goal

Move XeFG-specific resize state and transition-hold state out of `D3D12Hook` while preserving physical DXGI callback ownership.

### Move in this PR

Semantic lifecycle state:

- resize event id;
- last resize kind;
- last resize timestamp if still required by diagnostics;
- transition-hold active flag;
- transition trigger event id;
- suppressed-present count;
- post-resize diagnostic ordinal/budget may move now or remain until logging cleanup if that makes the PR safer;
- arm/complete/clear transition semantics;
- `suppress_renderer()` query.

### Keep in `D3D12Hook`

Physical callbacks:

- `resize_target()`;
- `resize_buffers()`;
- `resize_buffers1()`;
- calls to the original DXGI methods;
- normal REFramework callback ownership.

### Critical invariant

Current PR #16 behavior remains exact:

```text
tracked XeFG ResizeTarget
-> renderer reset
-> hold armed
-> original ResizeTarget forwarded

hold active
-> Present/Present1 still forwarded
-> REFramework render callback suppressed

successful tracked ResizeBuffers/ResizeBuffers1
-> hold completed

failed ResizeTarget
-> stale hold cleared

rebind/unhook
-> stale hold cleared
```

No timeout or Present-count recovery is added.

### Estimated size

```text
effective:        ~150–250 LOC
GitHub add+delete: ~280–480 LOC
```

### Validation

- Release build;
- state transitions unit-testable where practical;
- hold cannot affect native source;
- failed completion keeps hold according to current policy;
- rebind/unhook clears stale state.

---

## R9 — Reduce XeFG Logic Inside Present / Resize Callbacks

### Goal

Make the physical D3D12 callbacks ask the compatibility binding/lifecycle state for narrow decisions instead of directly manipulating many XeFG fields.

### Change in this PR

In `present_common()` reduce direct XeFG state access to semantic queries such as:

```text
is this the active XeFG instance?
should REFramework renderer callbacks be suppressed?
is real Present activity sufficient to keep monitor liveness?
```

In resize callbacks reduce direct state manipulation to notifications such as:

```text
begin resize event
arm hold after reset
complete hold after successful ResizeBuffers/1
clear hold on failure
```

### Do not change

- native Present order;
- recursive Present behavior;
- original Present/Present1 forwarding;
- `m_on_present` / `m_on_post_present` semantics when compatibility is inactive;
- normal resize behavior;
- renderer reset ordering.

### Estimated size

```text
effective:        ~120–220 LOC
GitHub add+delete: ~220–420 LOC
```

### Validation

- Release build;
- code audit confirms native branch is behavior-equivalent;
- XeFG hold still suppresses REFramework rendering but never blocks original Present.

### Runtime gate after R9

Lifecycle wave smoke:

```text
DD2 repeated Alt+Tab
MHW Alt+Enter / ResizeTarget path if available
resolution/window transition
REFramework overlay returns after successful resize completion
OptiScaler overlay remains functional
no device-removed regression attributable to REF renderer lifetime
```

---

## R10 — Hook-Monitor Isolation and Final Upstream-Surface Cleanup

### Goal

Complete the refactor by reducing XeFG knowledge in `REFramework.cpp` and the public/protected `D3D12Hook.hpp` surface to intentional bridge points only.

### Part A — hook monitor

Replace direct core knowledge such as:

```text
has_active_xefg_instance_binding()
get_xefg_binding_generation()
XeFG-specific raw fields used for preservation
```

with one narrow semantic query owned by the compatibility/binding layer, e.g. conceptually:

```cpp
should_preserve_active_binding_on_monitor_timeout(...)
```

The generic monitor timing and native recovery path must not change.

### Part B — D3D12Hook header cleanup

Remove XeFG implementation details that are no longer needed by the physical D3D12 hook:

- runtime registry APIs;
- runtime dispatch APIs;
- loader/probe statics;
- discovery transaction APIs;
- pending-candidate internals;
- strong XeFG COM fields now owned elsewhere;
- resize lifecycle state now owned elsewhere;
- direct diagnostic state that can wait for final logging cleanup where necessary.

Keep only bridge surface actually required by physical D3D12 callbacks and renderer integration.

### Part C — dependency audit

The intended final dependency is:

```text
REFramework.cpp
    -> very small XeFGCompatibility façade calls only

D3D12Hook
    -> native REFramework D3D12 mechanics
    -> narrow XeFG binding/lifecycle bridge at physical Present/resize points

compatibility/xefg
    -> all XeFG runtime/discovery/binding policy
```

### Critical invariants

- healthy complete XeFG binding is still protected from destructive timeout rehook;
- incomplete/stale binding is not protected merely because XeFG was once detected;
- generic/native hook monitor behavior is unchanged when XeFG is inactive;
- no FSRFG/DLSSG behavior is touched.

### Estimated size

```text
effective:        ~120–220 LOC
GitHub add+delete: ~200–380 LOC
```

### Final runtime gate for R1–R10

Minimum:

```text
Native D3D12 REFramework without OptiScaler/XeFG
    overlay opens
    scripts/plugins/mod initialization unaffected
    native hook monitor still recovers normally

DD2 + OptiScaler + XeFG
    XeFG works
    OptiScaler overlay works
    REFramework overlay works
    repeated Alt+Tab survives

MHW or equivalent ResizeTarget path
    transition hold still behaves as PR #16

Known multi-runtime / Pragmata path where available
    no launch regression
```

---

# 6. R11 — Logging / Debug UI — deliberately last

This is intentionally not designed in detail yet.

After R1–R10 are merged, inspect the **actual final logging call sites** and then create the logging work order from that code.

Current direction only:

- REFramework overlay `Configuration` gets `Debug Logging` checkbox;
- default = OFF;
- setting persists through the existing config mechanism;
- normal/default log retains concise support-critical XeFG state and actual failures;
- high-volume diagnostics move behind debug mode;
- do not require normal users to reproduce every support issue with a second debug log just to determine basic compatibility state.

Estimated size is intentionally provisional:

```text
effective: ~150–300 LOC
```

Do not perform logging cleanup opportunistically in R1–R10 except where compilation requires moving an existing log with its owning code.

---

# 7. Summary table

| PR | Primary responsibility | Effective LOC estimate | Git diff estimate |
|---|---|---:|---:|
| R1 | XeFG exact-HMODULE runtime registry / export hooks | 180–280 | 350–600 |
| R2 | Loader/probe handoff to compatibility façade | 80–160 | 140–280 |
| R3 | InitDesc transaction + temporary factory capture | 180–280 | 350–600 |
| R4 | Queue/device validation + candidate construction | 150–250 | 280–500 |
| R5 | Pending candidate publication/consumption | 100–180 | 180–320 |
| R6 | Active strong COM ownership + binding identity/health | 140–240 | 250–450 |
| R7 | Initial bind + transactional rebind protocol | 220–340 | 400–650 |
| R8 | Resize lifecycle / transition-hold state | 150–250 | 280–480 |
| R9 | Present/resize callback policy bridge cleanup | 120–220 | 220–420 |
| R10 | Hook-monitor isolation + final D3D12Hook/core surface cleanup | 120–220 | 200–380 |
| R11 | Final logging + Debug Logging UI | 150–300 provisional | TBD |

R7 is intentionally the largest semantic PR because the prepare-before-destroy replacement protocol should not be cut into a half-migrated unsafe state.

---

# 8. Validation cadence

Running a full manual game matrix after every move-only PR creates unnecessary load. Use wave gates instead.

## Every PR

- inspect latest master before implementation;
- Release build;
- repository tests/static audits relevant to touched code;
- `git diff --check`;
- confirm no unrelated cleanup;
- confirm no FSRFG/DLSSG changes;
- confirm no new OptiScaler-private dependency;
- review diff specifically for REFramework native-path changes.

## Runtime wave gates

### After R2 — runtime/loader wave

- DD2 XeFG launch smoke.

### After R4 — discovery wave

- DD2 full overlay smoke;
- multi-runtime launch smoke where available.

### After R7 — binding wave

- DD2 initial bind/rebind/Alt+Tab;
- verify both overlays.

### After R9 — lifecycle wave

- DD2 repeated Alt+Tab;
- MHW/equivalent `ResizeTarget`/Alt+Enter path;
- resolution/window transition.

### After R10 — final architecture gate

- native D3D12 REFramework smoke without XeFG;
- DD2 XeFG;
- ResizeTarget path;
- multi-runtime case where available.

A failed wave gate should be fixed before starting the next wave. Do not stack additional refactor PRs on top of a runtime regression.

---

# 9. Review policy for this refactor

Review should block only concrete regressions or violations of established invariants.

Blocking examples:

- native REFramework behavior changed without necessity;
- wrong XeFG runtime original function can be dispatched;
- current presentation queue selection semantics changed;
- candidate failure mutates working binding;
- strong COM lifetime ends before vtable restoration;
- changed-object rebind destroys old hook before new hook preparation succeeds;
- Present/Present1 not forwarded during transition hold;
- generic hook monitor disabled globally;
- FSRFG/DLSSG behavior changed;
- dependency on OptiScaler private class/layout/symbol introduced.

Normally non-blocking unless evidence shows real impact:

- naming/style preferences;
- speculative future provider abstraction;
- theoretical races without a realistic path under the current serialized protocol;
- broad modernization suggestions unrelated to XeFG compatibility.

---

# 10. Rules for each future work order

Every R1–R10 work order should explicitly include:

1. exact master SHA it was written against;
2. previous R-series PR assumed merged;
3. one primary responsibility only;
4. exact functions/state to move or change;
5. explicit "must not change" list;
6. preserved P2/P3 invariants relevant to that PR;
7. expected files touched;
8. approximate effective LOC target;
9. build/static validation;
10. whether that PR is a runtime wave gate;
11. instruction to stop rather than opportunistically start the next R-series task.

The agent should not implement the next architectural step merely because nearby code makes it convenient.

---

## Final execution order

```text
R1  Runtime registry
 ↓
R2  Loader/probe façade
 ↓   [runtime wave gate]
R3  Init transaction/factory capture
 ↓
R4  Queue validation/candidate construction
 ↓   [discovery wave gate]
R5  Candidate handoff
 ↓
R6  Binding ownership/identity
 ↓
R7  Initial bind + transactional rebind
 ↓   [binding wave gate]
R8  Resize lifecycle state
 ↓
R9  Present/resize policy bridge
 ↓   [lifecycle wave gate]
R10 Hook-monitor + upstream-surface cleanup
 ↓   [final architecture gate]
R11 Logging/debug UI, designed from final code
```

This sequence is intentionally conservative. It favors small, reviewable, bisectable changes over completing the architecture in the fewest pull requests.
