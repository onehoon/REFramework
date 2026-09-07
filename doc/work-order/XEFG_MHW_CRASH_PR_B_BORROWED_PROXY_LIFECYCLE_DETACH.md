# Work Order: XeFG MHW Crash PR-B — Borrowed Proxy Ownership and Deterministic Lifecycle Detach

Date: 2026-09-08  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `afce97edf93a8ff5b75838195990475a84d3548e` (`XeFG PR-A: add runtime lifecycle observability for MHW crash investigation`, PR #34 merged)

This work order is the second implementation PR in the MHW / Intel XeFG lifecycle investigation.

PR-A deliberately added runtime identity and Destroy/re-init observability without changing ownership. PR-B now uses that identity to correct REFramework's proxy-swapchain ownership and teardown ordering.

The primary objective is:

> REFramework must not retain a long-lived COM reference to an Intel XeFG proxy/internal presentation swapchain across XeFG runtime Destroy or re-init, and it must remove its renderer/vtable-hook relationship while the proxy is still alive and before the vendor lifecycle call continues.

This is a focused lifecycle change. It is **not** the hook-monitor timeout recovery PR. Sustained-timeout policy remains a later PR-C concern.

---

# 1. Investigation Background

Target configuration:

```text
Monster Hunter Wilds
+ onehoon/REFramework
+ OptiScaler
+ Intel XeFG output
```

The earlier Intel crash remains materially different from the NVIDIA startup failure.

The NVIDIA MHW startup problem was directly affected by OptiScaler's aggressive backbuffer `GetBuffer -> Release` loop in Resize/Resize1. Disabling that block fixed the NVIDIA startup crash.

The Intel long-session crash can still occur with those OptiScaler blocks disabled, so PR-B must not assume the NVIDIA fix explains the Intel failure.

Observed Intel failure pattern from the investigation:

```text
XeFG init succeeds
-> REFramework binds generation 1
-> ordinary resize/reset activity can survive
-> Present/Present1 can later stop reaching the tracked binding
-> repeated present_timeout preservation can occur
-> a later XeFG/D3D12 lifecycle transition occurs
-> crash can occur inside the Intel / D3D12 layered-device recreation path
```

The direct crash stack did not prove REFramework was the crashing frame. The REF-side ownership issue is therefore a source-level lifecycle defect/risk to remove, not a claim that PR-B alone is already proven to be the complete Intel crash fix.

---

# 2. Why PR-B Is Required After PR-A

Current master still strongly owns the active XeFG swapchain in `XeFGBinding`:

```cpp
private:
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue{};
    Microsoft::WRL::ComPtr<ID3D12Device4> m_device{};
    uint64_t m_generation{};
    bool m_observe_only{};
    RuntimeIdentity m_runtime{};
```

`XeFGBinding::clear()` currently releases all three COM references:

```cpp
void XeFGBinding::clear() noexcept {
    m_swapchain.Reset();
    m_queue.Reset();
    m_device.Reset();
    m_generation = 0;
    m_observe_only = false;
    m_runtime = {};
}
```

PR-A added the missing runtime identity:

```cpp
struct RuntimeIdentity {
    size_t slot{kInvalidRuntimeSlot};
    void* context{};
    HWND hwnd{};
};
```

and exact runtime `xefgSwapChainDestroy` interception, but `dispatch_destroy()` is still observation-only:

```cpp
const auto binding_before_destroy = active_binding_snapshot();
log_runtime_lifecycle("destroy_enter", ...);
const auto result = original(context);
log_runtime_lifecycle("destroy_return", ...);
return result;
```

Likewise, `dispatch_init_desc()` currently logs `pre_init` and then immediately enters `XeFGDiscovery::observe_init()`, which invokes the original Intel initialization call before any old active REF binding is detached.

That is the critical gap PR-B closes.

---

# 3. Intel XeFG Lifecycle Contract Relevant to This PR

Reference:

https://www.intel.com/content/www/us/en/developer/articles/technical/xess-fg-developer-guide.html

The relevant lifecycle rules are:

- XeFG exposes/uses a proxy swapchain.
- The application must release references to the XeFG proxy before `xefgSwapChainDestroy()`.
- A Destroy failure due to outstanding proxy references means the XeFG swapchain/context was not destroyed.
- A replacement swapchain/context must not be layered on top of a still-live old context as though teardown succeeded.
- `InitFromSwapChainDesc` creates/associates a swapchain with the target HWND, so stale ownership or stale hook state from an earlier binding must not be carried into a relevant re-init transaction.

For REFramework this produces two distinct rules:

```text
LONG-LIVED ownership of the XeFG proxy: forbidden

SHORT-LIVED keepalive used only while safely removing/installing a VtableHook:
allowed, but it must be released before returning to the vendor Destroy/re-init call
```

Do not confuse these two cases.

---

# 4. Latest-Master Findings That Change the PR-B Design

PR-B must be based on the actual post-PR-A code, not on the earlier pre-refactor plan.

## 4.1 Active binding now has exact runtime identity

Current `XeFGBinding` can answer:

```text
runtime slot
runtime context
HWND
generation
swapchain
queue
device
observe-only mode
```

Use this identity. Do not add a second parallel lifecycle identity store.

## 4.2 Destroy is now intercepted at the exact runtime slot

`XeFGRuntimeRegistry` / `XeFGCompatibility::dispatch_destroy()` provides a deterministic pre-Destroy point for an exact runtime module slot and context.

This is the correct place to prepare REF state before Intel Destroy.

## 4.3 Init is still called before candidate publication

Current order is:

```text
log pre_init
-> XeFGDiscovery::observe_init(...)
   -> original InitFromSwapChainDesc(...)
-> build candidate
-> publish candidate
-> bind/rebind D3D12Hook
```

Therefore post-init rebind is too late to solve a stale REF relationship that interferes **inside** the original re-init call.

PR-B needs a pre-init detach for the relevant active binding.

## 4.4 `XeFGBindingCandidate` must remain strongly owned during discovery/handoff

Current candidate deliberately contains:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain{};
Microsoft::WRL::ComPtr<ID3D12CommandQueue> selected_queue{};
Microsoft::WRL::ComPtr<ID3D12Device4> device{};
```

Do **not** convert the candidate wholesale to raw pointers.

A candidate may need to survive the gap between runtime discovery and a live `D3D12Hook`. Strong candidate ownership is useful for making that handoff safe.

However, current `XeFGCandidateHandoff::s_pending_candidate` can retain that strong swapchain reference if no live hook exists. Therefore PR-B must explicitly discard a matching pending candidate before Destroy/re-init so a pending handoff cannot become an outstanding REF proxy reference at the vendor lifecycle boundary.

This pending-candidate case is important and must not be omitted.

---

# 5. Required PR-B Invariants

These are merge-blocking invariants.

## Invariant B1 — active proxy is borrowed

After a XeFG binding is committed, `XeFGBinding` must not keep a long-lived `ComPtr<IDXGISwapChain3>`.

Target semantic shape:

```cpp
class XeFGBinding {
    IDXGISwapChain3* m_swapchain{}; // borrowed identity / hook target only
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue{};
    Microsoft::WRL::ComPtr<ID3D12Device4> m_device{};
    uint64_t m_generation{};
    bool m_observe_only{};
    RuntimeIdentity m_runtime{};
};
```

Queue/device ownership remains strong in PR-B.

## Invariant B2 — no hook may outlive its proxy target

A raw/borrowed swapchain pointer makes teardown ordering more important, not less important.

Before a matching XeFG proxy can be destroyed:

```text
renderer state referring to old backbuffers must be reset
-> instance VtableHook must be removed while object is still alive
-> raw D3D12Hook aliases must stop pointing to the old binding
-> active binding metadata must be cleared
-> every temporary REF proxy keepalive must be released
-> only then may original xefgSwapChainDestroy() run
```

A naked `ComPtr -> raw pointer` conversion without deterministic detach is a blocking defect.

## Invariant B3 — pending candidates cannot cross a matching runtime transition

Before a matching Destroy or relevant same-window re-init, drop any pending candidate that represents the old runtime/window.

Dropping the pending candidate releases its candidate-owned `ComPtr` references before the vendor call.

## Invariant B4 — REF locks are not held across vendor Init/Destroy

Use the existing hook-monitor lifecycle mutex to serialize REF hook/binding mutations, but release it **before** calling:

```text
original xefgSwapChainDestroy
original xefgSwapChainD3D12InitFromSwapChainDesc
```

Do not introduce a lock inversion surface inside Intel/OptiScaler callbacks.

## Invariant B5 — failed vendor transitions remain fail-closed

If Destroy fails after REF detached:

- do not reconstruct the old `VtableHook`;
- do not reattach from cached raw pointers;
- do not restore an old pending candidate;
- remain detached and wait for a future validated candidate/lifecycle event.

If re-init fails after pre-init detach, apply the same rule.

Safety is more important than preserving the overlay through an uncertain vendor state.

---

# 6. Recommended PR Identity

Suggested branch:

```text
fix/xefg-pr-b-borrowed-proxy-lifecycle
```

Suggested PR title:

```text
XeFG PR-B: borrow proxy swapchain and detach before runtime transitions
```

Suggested commit title:

```text
fix: detach XeFG binding before destroy and re-init
```

---

# 7. Expected Files in Scope

Primary files:

```text
src/compatibility/xefg/XeFGBinding.hpp
src/compatibility/xefg/XeFGBinding.cpp
src/compatibility/xefg/XeFGCompatibility.cpp
src/compatibility/xefg/XeFGCandidateHandoff.hpp
src/compatibility/xefg/XeFGCandidateHandoff.cpp
src/D3D12Hook.hpp
src/D3D12Hook.cpp
```

Potentially touched only if necessary for a clean compile or focused test seam:

```text
src/compatibility/xefg/XeFGDiscovery.*
```

Normally unchanged:

```text
src/compatibility/xefg/XeFGRuntimeRegistry.*
src/compatibility/xefg/XeFGResizeLifecycle.*
REFramework config/UI
native non-XeFG discovery
Streamline handling
OptiScaler source
MHW ResizeHold policy
hook-monitor timeout policy
```

Do not add unrelated refactoring because the code is nearby.

---

# 8. Change B1 — Convert Only the Active XeFG Swapchain to Borrowed Ownership

Change `XeFGBinding` so only the swapchain is borrowed.

Recommended API direction:

```cpp
class XeFGBinding {
public:
    void commit_initial(
        IDXGISwapChain3* swapchain,
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue,
        Microsoft::WRL::ComPtr<ID3D12Device4> device,
        bool observe_only,
        RuntimeIdentity runtime = {});

    void commit_replacement(
        IDXGISwapChain3* swapchain,
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue,
        Microsoft::WRL::ComPtr<ID3D12Device4> device,
        bool observe_only,
        RuntimeIdentity runtime = {});

private:
    IDXGISwapChain3* m_swapchain{}; // borrowed
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue{};
    Microsoft::WRL::ComPtr<ID3D12Device4> m_device{};
    ...
};
```

Then update accessors/comparison helpers from `.Get()` to the raw pointer.

Example:

```cpp
IDXGISwapChain3* XeFGBinding::swapchain() const noexcept {
    return m_swapchain;
}

bool XeFGBinding::complete() const noexcept {
    return m_swapchain != nullptr && m_queue != nullptr && m_device != nullptr;
}

void XeFGBinding::clear() noexcept {
    m_swapchain = nullptr; // no Release: borrowed
    m_queue.Reset();
    m_device.Reset();
    m_generation = 0;
    m_observe_only = false;
    m_runtime = {};
}
```

Do not weaken queue/device lifetime in the same PR.

## Important: temporary `ComPtr` is still allowed

`D3D12Hook` currently builds local strong references while validating a candidate and preparing a hook. Keep that pattern where it protects a short hook-edit transaction.

For example:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> next_swapchain = swapchain;
```

may remain as a local preparation keepalive.

The requirement is that this local reference dies before returning from the transaction and never becomes the long-lived active binding owner.

---

# 9. Change B2 — Add a Targeted XeFG Runtime-Transition Detach

Do **not** call full `D3D12Hook::unhook()` from XeFG Destroy/re-init dispatch.

Add a narrow method that removes only the active XeFG instance relationship while leaving the D3D12Hook object available for a subsequent validated XeFG candidate.

Possible API:

```cpp
enum class XeFGRuntimeDetachReason : uint8_t {
    Destroy,
    Reinit,
};

bool detach_xefg_binding_for_runtime_transition(
    XeFGBinding::RuntimeIdentity incoming,
    XeFGRuntimeDetachReason reason,
    bool allow_same_hwnd_match);
```

Exact naming is flexible.

The implementation should revalidate the active binding under the hook-monitor mutex and only detach when the transition is relevant.

## Destroy match

Destroy has no HWND argument. Require exact runtime identity:

```text
active runtime slot == destroy slot
AND
active runtime context == destroy context
```

Do not detach another active XeFG context merely because it came from the same runtime module slot.

## Pre-init match

For re-init, detach when either:

```text
exact slot + exact context match
```

or:

```text
incoming HWND != null
AND active binding HWND == incoming HWND
```

The same-HWND rule is necessary because a replacement/re-init may use a different context identity while replacing the presentation object associated with the same game window.

Do **not** use runtime slot alone as a re-init match.

---

# 10. Change B3 — Detach Ordering Inside `D3D12Hook`

The detach method must preserve the old target long enough to remove its hook safely, but not across the vendor lifecycle call.

Recommended sequence:

```cpp
bool D3D12Hook::detach_xefg_binding_for_runtime_transition(...) {
    if (m_swapchain_source != SwapchainSource::XeFGInternal
        || !m_xefg_binding.active()
        || !runtime_transition_matches(...)) {
        return false;
    }

    const auto snapshot = m_xefg_binding.lifecycle_snapshot();

    // Bounded keepalive only for renderer/vtable teardown.
    Microsoft::WRL::ComPtr<IDXGISwapChain3> old_keepalive = snapshot.swapchain;

    spdlog::info("[XeFG][LifecycleDetach] stage = begin, ...");

    // Release REF renderer references while the proxy is definitely alive.
    if (g_framework != nullptr) {
        g_framework->on_reset();
    }

    clear_xefg_resize_transition_hold("runtime_transition");

    // Remove physical hooks before releasing the temporary keepalive.
    m_present_hook.reset();
    m_swapchain_hook.reset();

    if (m_swap_chain == snapshot.swapchain) {
        m_swap_chain = nullptr;
    }
    if (m_command_queue == snapshot.queue) {
        m_command_queue = nullptr;
    }
    if (m_device == snapshot.device) {
        m_device = nullptr;
    }

    m_xefg_binding.clear();

    // `old_keepalive` is released on return, BEFORE original vendor Init/Destroy.
    spdlog::info("[XeFG][LifecycleDetach] stage = complete, ...");
    return true;
}
```

This is pseudocode. Adapt to the actual renderer/reset invariants in current `D3D12Hook`.

### Why the temporary keepalive is important

R6 intentionally relied on long-lived active ownership to guarantee:

```text
old proxy alive
-> remove old VtableHook
-> release old ownership
```

PR-B removes that long-lived ownership. Therefore any path that edits/removes a hook on the borrowed object must establish a bounded keepalive while the hook is being removed.

The temporary keepalive must not escape the function and must not exist when Intel Destroy is called.

---

# 11. Change B4 — Audit Ordinary Rebind/Unhook Ordering for Borrowed Ownership

Changing `XeFGBinding::m_swapchain` to raw affects more than the new Destroy callback.

Audit these current paths:

```text
D3D12Hook::replace_xefg_binding()
D3D12Hook::bind_external_swapchain()
D3D12Hook::unhook()
```

Current code/comments rely on the old R6 invariant that active XeFG ownership keeps the hook target alive while `m_swapchain_hook.reset()` runs.

For example, current master contains the semantic assumption:

```cpp
// Existing XeFG ownership keeps the old instance alive through hook removal.
m_present_hook.reset();
m_swapchain_hook.reset();
m_xefg_binding.clear();
```

That comment/invariant becomes false after B1.

Replace it with explicit bounded keepalive ordering where an old XeFG hook target is being removed.

Conceptual pattern:

```cpp
Microsoft::WRL::ComPtr<IDXGISwapChain3> old_keepalive;
if (m_xefg_binding.active()) {
    old_keepalive = m_xefg_binding.swapchain();
}

m_present_hook.reset();
m_swapchain_hook.reset();

// clear aliases / semantic state only after hook removal
m_xefg_binding.clear();

// local keepalive releases here
```

Do not add a new persistent owner field in `D3D12Hook`; that would recreate the original problem under another name.

For the new candidate, local `next_swapchain` ownership may remain while the new VtableHook is prepared and committed. Once the bind/rebind call returns, the active binding must be borrowed.

---

# 12. Change B5 — Discard Matching Pending Candidates Before Runtime Transition

Current handoff can store:

```cpp
static std::optional<XeFGBindingCandidate> s_pending_candidate;
```

and `XeFGBindingCandidate::swapchain` is a strong `ComPtr`.

Add a narrow invalidation API in `XeFGCandidateHandoff`.

Possible shape:

```cpp
static bool discard_pending_for_runtime_transition(
    size_t runtime_slot,
    void* context,
    HWND hwnd,
    bool allow_same_hwnd_match,
    const char* reason);
```

Matching rules must mirror active-binding detach:

### Destroy

```text
pending.runtime.slot == slot
AND
pending.runtime.context == context
```

### Re-init

```text
exact slot/context
OR
same non-null HWND
```

When matched:

```cpp
s_pending_candidate.reset();
```

This intentionally releases candidate-owned swapchain/queue/device COM references before vendor Init/Destroy.

Log the discarded candidate identity under XeFG Debug Log.

Do not clear unrelated pending candidates for another HWND/context.

---

# 13. Change B6 — Prepare Before Original `InitFromSwapChainDesc`

Current `dispatch_init_desc()` must change from:

```text
pre_init log
-> original Init through XeFGDiscovery::observe_init
-> candidate build/publish
```

to:

```text
pre_init snapshot/log
-> serialize REF lifecycle state
   -> discard matching pending candidate
   -> detach matching active XeFG binding
-> release REF lifecycle lock
-> original Init through XeFGDiscovery::observe_init
-> build/publish only on accepted observation
```

A helper in `XeFGCompatibility` is preferred so `dispatch_init_desc()` stays readable.

Conceptual helper:

```cpp
void prepare_for_xefg_runtime_transition(
    size_t slot,
    void* context,
    HWND hwnd,
    RuntimeTransitionKind kind) {

    if (g_framework == nullptr) {
        XeFGCandidateHandoff::discard_pending_for_runtime_transition(...);
        return;
    }

    std::scoped_lock lifecycle_lock{g_framework->get_hook_monitor_mutex()};

    // Lock order: hook-monitor lifecycle mutex -> pending mutex.
    XeFGCandidateHandoff::discard_pending_for_runtime_transition(...);

    if (auto* hook = D3D12Hook::current_xefg_handoff_target(); hook != nullptr) {
        hook->detach_xefg_binding_for_runtime_transition(...);
    }
}
```

Do not hold `lifecycle_lock` while `XeFGDiscovery::observe_init()` invokes Intel.

## Re-init failure semantics

If original Init fails after detach:

```text
old REF hook remains detached
old pending candidate remains discarded
no stale raw pointer is restored
no candidate is published
```

Do not attempt rollback to a pointer whose vendor lifetime is now uncertain.

---

# 14. Change B7 — Detach Before Original `xefgSwapChainDestroy`

Current PR-A Destroy flow:

```text
snapshot old binding
-> destroy_enter log
-> original Destroy
-> destroy_return log using cached snapshot
```

PR-B target:

```text
snapshot old binding for diagnostics
-> destroy_enter log using old snapshot
-> serialize REF lifecycle state
   -> discard exact matching pending candidate
   -> detach exact matching active binding
-> release REF lifecycle lock
-> original Destroy(context)
-> destroy_return log using cached pre-destroy snapshot
```

The PR-A cached snapshot behavior is valuable and should remain. It allows `destroy_return` to explain what was active before detach without dereferencing post-Destroy objects.

Do not perform COM calls on the cached swapchain after original Destroy returns.

## Destroy failure semantics

If Intel Destroy returns an error:

- log the failure/result as PR-A already does;
- keep REF detached;
- do not restore the old binding;
- do not retry Destroy from REFramework;
- do not force-Release proxy references;
- let the owner of the XeFG lifecycle decide whether/when to retry.

This complements the OptiScaler-side fail-closed lifecycle hardening without coupling REFramework to OptiScaler internals.

---

# 15. Synchronization and Lock Ordering

Current relevant synchronization:

```text
REFramework hook-monitor mutex: recursive lifecycle serialization for live D3D12Hook replacement/handoff
XeFGCandidateHandoff pending mutex: protects one pending candidate
XeFGDiscovery transaction mutex: protects one Init observation transaction
```

Required lock rule for new transition preparation:

```text
hook-monitor mutex
    -> pending-candidate mutex
```

This matches current `XeFGCandidateHandoff::publish()` behavior when it has a framework/lifecycle mutex.

`consume_pending()` currently releases the pending mutex before entering active binding logic; preserve that property.

Never hold the hook-monitor mutex or pending mutex across original Intel Init/Destroy.

Do not introduce a new global mutex unless a concrete race cannot be solved using the existing lifecycle mutex.

---

# 16. Generation Semantics

PR-A made generation visible in runtime lifecycle logs.

PR-B should treat a runtime detach as an end of the active binding generation sequence:

```text
old binding active, generation N
-> runtime transition detach
-> binding clear => generation 0 / inactive
-> successful later candidate commit_initial
-> fresh active binding generation 1
```

Do not increment the old generation merely because Destroy/re-init was observed.

Do not preserve a stale generation across an exact runtime lifecycle boundary.

Same-object updates that do not cross a runtime detach may keep the existing increment behavior.

---

# 17. Logging Requirements

Reuse the existing persistent XeFG Debug Log option.

Add concise transition logs sufficient to prove ordering.

Suggested tags:

```text
[XeFG][LifecycleDetach] stage = evaluate
[XeFG][LifecycleDetach] stage = begin
[XeFG][LifecycleDetach] stage = hook_removed
[XeFG][LifecycleDetach] stage = complete
[XeFG][LifecycleDetach] stage = pending_candidate_dropped
```

Useful fields:

```text
reason = destroy | reinit
match = exact_runtime | same_hwnd | none
runtime_slot
context
hwnd
binding_generation
binding_context
binding_hwnd
swapchain
queue
device
observe_only
```

Do not log every Present.

Keep existing PR-A:

```text
pre_init
init_return
destroy_enter
destroy_return
```

The combined field log should make this ordering visible:

```text
destroy_enter (old binding snapshot)
LifecycleDetach begin
LifecycleDetach hook_removed
LifecycleDetach complete
Destroy original executes
destroy_return
```

For re-init:

```text
pre_init (old binding snapshot)
LifecycleDetach ... reason=reinit
original Init
Bind accepted
fresh binding committed
init_return
```

Normal-user log volume must remain bounded.

---

# 18. Explicit Non-Goals

PR-B must NOT:

- change `should_preserve_active_binding_on_monitor_timeout()` policy;
- add sustained-timeout counters or automatic timeout detach/recovery;
- generic-rehook after a single timeout;
- modify MHW ResizeTarget/ResizeBuffers/ResizeBuffers1 transition-hold behavior;
- add `Release until refcount <= N` loops;
- call `Release()` repeatedly to guess vendor ownership;
- change OptiScaler code;
- change XeFG runtime registry capacity or thunk architecture;
- remove exact-runtime Destroy instrumentation from PR-A;
- convert queue/device active ownership to raw pointers;
- convert the validated `XeFGBindingCandidate` wholesale to raw pointers;
- remove the pending-candidate handoff architecture;
- change native/non-XeFG D3D12 discovery;
- refactor Streamline/DLSS-G handling;
- do broad log cleanup;
- change Special K policy or loader topology;
- claim the Intel MHW crash is fixed without runtime validation.

Timeout/stale-generation recovery belongs to PR-C after PR-B behavior is validated.

---

# 19. Source-Level Validation Checklist

Before runtime testing, verify all of the following.

## Active binding ownership

- `XeFGBinding` no longer contains `ComPtr<IDXGISwapChain3>`.
- `XeFGBinding::swapchain()` returns the borrowed pointer.
- `clear()` nulls the borrowed pointer without `Release()`.
- queue/device remain strongly owned.
- no replacement persistent swapchain `ComPtr` was added elsewhere in `D3D12Hook`.

## Candidate ownership

- `XeFGBindingCandidate::swapchain` remains strong during validation/handoff.
- matching pending candidate is dropped before Destroy.
- matching pending candidate is dropped before relevant same-window re-init.
- unrelated pending candidate is not dropped.

## Hook lifetime

- every path that removes an active XeFG `VtableHook` keeps the target alive through the hook removal operation.
- that keepalive is local/bounded.
- the temporary keepalive is released before original vendor Init/Destroy executes.
- no raw alias remains intentionally usable after runtime detach.

## Dispatch ordering

- `dispatch_destroy()` detaches before original Destroy.
- `dispatch_init_desc()` detaches before `observe_init()` calls original Init.
- no REF lifecycle mutex is held across either vendor call.
- PR-A cached pre-Destroy snapshot remains diagnostic-only after Destroy.

## Failure semantics

- failed Destroy does not reconstruct stale REF state.
- failed re-init does not reconstruct stale REF state.
- no automatic retry/forced COM release is added.

---

# 20. Build / Static Validation

Run the repository's existing Release x64 build path used by the XeFG work.

At minimum:

```text
Release x64 build: PASS
git diff --check: PASS
```

Also inspect the final diff for accidental changes outside the PR-B file set.

Useful source checks:

```text
no long-lived ComPtr<IDXGISwapChain3> in XeFGBinding
no new persistent XeFG proxy owner in D3D12Hook
no forced Release loop
no change to HookMonitor timeout policy
no change to MHW ResizeHold policy
```

Do not invent a new test framework solely for this PR if the repository does not already provide a suitable focused unit-test harness.

---

# 21. Runtime Validation — Monster Hunter Wilds / Intel XeFG

Runtime validation is required before declaring the crash investigation resolved.

Use XeFG Debug Log enabled.

## Test B1 — clean startup

Expected:

```text
pre_init
-> no stale binding or relevant detach if none exists
-> init success
-> candidate accepted
-> overlay bind succeeds
-> Present/Present1 continues
```

Verify REFramework and OptiScaler overlays both remain functional.

## Test B2 — ordinary resize / fullscreen / Alt+Tab

Exercise:

```text
Alt+Tab
window/fullscreen transitions
resolution changes
in-game graphics changes that resize the swapchain
```

Expected:

- existing ResizeLifecycle behavior remains intact;
- no premature runtime detach from ordinary ResizeTarget/ResizeBuffers/ResizeBuffers1 alone;
- renderer resumes on Present/Present1 as before.

## Test B3 — observed XeFG Destroy

When `destroy_enter` occurs for the active runtime/context, verify exact ordering:

```text
destroy_enter reports old active generation/context
-> pending candidate dropped if matching
-> LifecycleDetach exact_runtime
-> renderer reset
-> VtableHook removed
-> active binding cleared
-> original Destroy
-> destroy_return
```

Most important evidence:

> Intel Destroy must no longer be reached while REFramework still owns the active proxy through `XeFGBinding` or a matching pending candidate.

## Test B4 — re-init for same HWND

Expected:

```text
pre_init sees old active binding
-> same-window/exact-runtime detach occurs before original Init
-> original Init succeeds
-> new candidate accepted
-> fresh active generation begins
-> Present/Present1 resumes
```

## Test B5 — failure path

If Intel Destroy fails for reasons outside REF ownership:

```text
REF stays detached
no stale hook restoration
no old raw pointer reuse
no second REF binding until a new validated candidate is published
```

Capture logs; do not add a recovery loop during this PR.

## Test B6 — long gameplay soak

Because the historical Intel failure can appear well after startup, a short menu-only launch is not sufficient evidence.

Run a real gameplay soak long enough to include multiple resize/reset/lifecycle events.

Record:

```text
last successful Present/Present1
last ResizeLifecycle event
any sustained present_timeout sequence
pre_init/destroy lifecycle sequence
binding generation transitions
crash/no crash
```

PR-B success does not require changing the current timeout preservation policy. If sustained timeout remains but lifecycle ownership is correct, use that evidence for PR-C.

---

# 22. Regression Validation

At minimum verify no obvious regression in:

```text
non-XeFG D3D12 path
XeFG initial binding
Present + Present1
ResizeBuffers + ResizeBuffers1 + ResizeTarget
REFramework overlay rendering
OptiScaler overlay coexistence
Alt+Tab
hook-monitor normal operation
```

Where available, compare against the known working DD2 XeFG path as a sanity check.

The separate OptiScaler MHW backbuffer-release experiment must remain independent from this PR.

---

# 23. Acceptance Criteria

PR-B is ready for review only when all are true:

1. Latest `master` / PR-A runtime identity is used directly.
2. Active `XeFGBinding` no longer strongly owns the XeFG swapchain.
3. Active queue/device ownership is unchanged.
4. Candidate strong ownership remains for discovery/handoff safety.
5. Matching pending candidates are discarded before Destroy/re-init.
6. Matching active XeFG binding is detached before original Destroy.
7. Relevant exact-runtime or same-HWND binding is detached before original re-init.
8. Renderer reset occurs before removing the old instance hook.
9. Old instance hook is removed while the proxy is guaranteed alive via a bounded keepalive.
10. All temporary proxy keepalives are gone before vendor Init/Destroy executes.
11. Raw aliases are cleared when the active runtime binding is detached.
12. No REF lifecycle lock is held across vendor Init/Destroy.
13. Failed Destroy/re-init does not restore stale REF state.
14. Fresh successful post-transition binding starts from a clean inactive binding state.
15. No timeout-recovery policy is added.
16. No forced COM release loop is added.
17. Release x64 build passes.
18. `git diff --check` passes.
19. Runtime logs prove the intended ordering on at least one real XeFG lifecycle transition before the crash is declared fixed.

---

# 24. Review Hotspots

Reviewers should pay special attention to these failure modes.

### Blocker: raw pointer introduced without pre-Destroy detach

```text
active swapchain becomes raw
but m_swapchain_hook survives until/after vendor destroys proxy
=> potential dangling VtableHook / UAF
```

### Blocker: pending candidate still owns proxy at Destroy

```text
active binding borrowed correctly
but s_pending_candidate still has ComPtr<swapchain>
=> Intel still sees a REF-owned outstanding reference
```

### Blocker: hook-monitor mutex held across Intel call

```text
REF lifecycle mutex held
-> original Init/Destroy
-> Intel/OptiScaler re-enters REF/hook path
=> lock inversion / deadlock risk
```

### Blocker: old state restored after failed vendor call

```text
cached raw pointer restored after failed Init/Destroy
=> pointer lifetime no longer proven
```

### Blocker: ordinary rebind assumes active binding still strongly owns old proxy

Any old comment/code path equivalent to:

```text
"existing XeFG ownership keeps old instance alive through hook removal"
```

must be audited after changing the binding to borrowed ownership.

---

# 25. Codex Implementation Guidance

Implement this as one focused PR, but stage the code internally in this order:

```text
1. Convert XeFGBinding active swapchain storage/API to borrowed semantics.
2. Fix D3D12Hook ordinary bind/rebind/unhook ordering with bounded old-target keepalive.
3. Add targeted runtime-transition detach API.
4. Add pending-candidate invalidation API.
5. Wire pre-Destroy transition preparation.
6. Wire pre-init transition preparation.
7. Add bounded debug lifecycle logs.
8. Build/static validation.
9. Runtime validation notes in PR body.
```

After each step, re-check the invariant:

> No physical VtableHook may target a proxy whose lifetime REFramework no longer knows, and no REFramework-owned proxy reference may survive into the original XeFG Destroy/re-init call.

Do not solve PR-C timeout behavior while implementing PR-B, even if the nearby code makes it tempting.

---

# 26. Expected End State

Before PR-B:

```text
XeFG candidate
-> active XeFGBinding strongly owns proxy indefinitely
-> hook remains attached
-> vendor Destroy/re-init may begin while REF relationship is still active
```

After PR-B:

```text
XeFG candidate strongly owns proxy only through validation/handoff
-> D3D12Hook installs instance hook using bounded local keepalive
-> active XeFGBinding stores proxy as borrowed identity
-> normal presentation runs without a long-lived REF proxy AddRef

matching Destroy/re-init begins
-> drop matching pending candidate
-> bounded keepalive old proxy
-> reset renderer
-> remove old VtableHook
-> clear raw aliases/binding metadata
-> release bounded keepalive
-> release REF lifecycle locks
-> call original Intel Destroy/re-init

successful later init
-> validated candidate
-> clean fresh binding
-> new generation / hook
```

That is the complete scope of PR-B.

PR-C should only begin after this ownership/lifecycle behavior is reviewed and runtime-tested, because only then can sustained `present_timeout` be interpreted without the active-binding ownership ambiguity.