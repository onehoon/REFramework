# XeFG 2R8 — Same-Object Native → XeFG Hook Takeover Hardening Work Order

**Repository:** `onehoon/REFramework`  
**Target integration branch:** `REFforXeFG`  
**Required starting head:** `bd5714b285d9bf9d33d09f1fad52e994dbdd2f96` (`XeFG 2R7: collapse transitional D3D12 surface`)  
**Implementation branch suggestion:** `fix/xefg-2r8-same-object-hook-takeover`  
**Suggested PR title:** `XeFG 2R8: harden same-object native-to-XeFG hook takeover`

---

## 1. Purpose

This is a **narrow final-audit hardening PR** after completion of XeFG 2R1–2R7.

Do **not** redesign the second-stage architecture.

The final architecture remains valid:

- `XeFGPresentationSession` owns XeFG semantic lifecycle/policy state.
- `D3D12Hook` owns physical D3D12 hook objects and physical swapchain mechanism.
- active XeFG swapchain ownership remains borrowed.
- queue/device ownership remains strong `ComPtr` ownership.
- runtime detach, Present/Present1 policy, resize policy, monitor policy, and candidate semantics remain unchanged.

The only problem addressed here is a physical `VtableHook` lifetime hazard when an existing **native instance hook** and a new XeFG candidate refer to the **same swapchain object**.

This PR is the final acceptance blocker found by the whole-tree audit.

---

## 2. Problem Statement

Current 2R6 candidate handling prepares a new XeFG instance hook before destructive work for both:

```text
NoActiveBinding
ChangedSwapchainReplacement
```

Conceptually the current sequence is:

```cpp
if (requires_new_hook) {
    prepared = prepare_xefg_instance_hook(next_swapchain.Get());
    if (!prepared.ready()) {
        return false;
    }
}

if (plan.disposition == CandidateDisposition::NoActiveBinding) {
    ...
    m_present_hook.reset();
    m_swapchain_hook.reset();

    const auto commit = m_xefg_session.commit_prepared_candidate(...);
    ...
    m_swapchain_hook = std::move(prepared.hook);
}
```

This is correct when the existing physical hook target and the candidate are different objects.

The hazardous topology is:

```text
existing D3D12 native instance hook target = swapchain A
new XeFG candidate swapchain              = swapchain A
```

In this state `plan_candidate()` legitimately returns `NoActiveBinding`, because the current physical source is not XeFG even though `D3D12Hook` is already instance-hooking the object.

Preparing another `VtableHook` for the same object before destroying the existing hook stacks one REFramework-owned copied vtable on top of another.

---

## 3. Why This Is a Lifetime Hazard

The project currently pins kananlib commit:

```text
8c27b656734355db0f2893581fd62e838fa130ad
```

Its `VtableHook` behavior is important to this work order.

Construction immediately captures the object's current vtable as `m_old_vtable`, allocates a copied vtable, and writes the object to the copied vtable:

```cpp
m_vtable_ptr = target;
m_old_vtable = m_vtable_ptr.to<Address>();
m_vtable_size = get_vtable_size(m_old_vtable);
m_raw_data.resize(m_vtable_size + 1);
m_new_vtable = m_raw_data.data() + 1;
memcpy(...);
*m_vtable_ptr.as<Address*>() = m_new_vtable;
```

`get_method()` resolves originals through `m_old_vtable`.

Therefore, if `old_hook` already owns the object's copied vtable and `new_hook` is constructed on the same object:

```text
object A
  -> old_hook.m_new_vtable

construct new_hook(A)

object A
  -> new_hook.m_new_vtable
new_hook.m_old_vtable
  -> old_hook.m_new_vtable
```

Then destroying `old_hook` does not restore the object's vtable because the object is already pointing at `new_hook.m_new_vtable`.

After `old_hook` destruction, its `m_raw_data` is freed, while:

```text
new_hook.m_old_vtable -> freed old_hook vtable storage
```

A later `get_method()` can therefore read an original function pointer from freed memory.

This is a crash/hang/use-after-free class hazard.

The 2nd-refactor baseline did not have this exact ordering for initial XeFG bind: it removed the previous hook first, then constructed the new XeFG hook. The 2R6 prepare-before-destroy transaction introduced the same-object stacking possibility while improving rollback safety for normal replacement.

Do not solve this by reverting the whole 2R6 transaction.

---

## 4. Required Fix Strategy

For the exact normal topology:

```text
NoActiveBinding
+ current hook is active native instance hook
+ current physical renderer swapchain == candidate swapchain
+ current VtableHook target == candidate swapchain
```

**reuse/promote the existing native instance `VtableHook` instead of constructing a second `VtableHook` on the same object.**

The existing native instance hook already owns these methods:

```text
Present[8]
Present1[22]
ResizeBuffers[13]
ResizeTarget[14]
```

XeFG additionally requires:

```text
ResizeBuffers1[39]
```

Reusing the physical hook preserves the correct underlying `m_old_vtable` and avoids stacking REFramework-owned vtable copies.

This is a physical `D3D12Hook` concern. Do not move it into `XeFGPresentationSession`.

---

## 5. Strict Promotion Eligibility

Add a narrow physical predicate/helper or equivalent logic.

Conceptually:

```cpp
const bool candidate_matches_current_hook_target =
    m_swapchain_hook != nullptr
    && m_swapchain_hook->get_instance().ptr() == swapchain;

const bool can_promote_existing_native_instance_hook =
    plan.disposition == XeFGPresentationSession::CandidateDisposition::NoActiveBinding
    && m_hooked
    && !m_is_phase_1
    && m_swapchain_source != SwapchainSource::XeFGInternal
    && m_swap_chain == swapchain
    && candidate_matches_current_hook_target;
```

Exact helper names may differ.

Do not weaken these conditions casually.

In particular:

- do not treat a phase-1 global Present hook as reusable instance ownership;
- do not reuse a hook whose object target does not equal the candidate;
- do not reuse an already-XeFG hook through this `NoActiveBinding` path;
- do not infer reuse solely from semantic `XeFGBinding` state;
- do not compare only HWND, queue, device, or COM identity here; this is an exact physical object/hook-target decision.

---

## 6. Existing Hook Promotion

When the strict promotion predicate is true, do **not** call:

```cpp
prepare_xefg_instance_hook(candidate_swapchain)
```

for that candidate.

Instead, validate/reassert the mandatory XeFG method set on the existing `m_swapchain_hook`.

Conceptually:

```cpp
bool D3D12Hook::promote_existing_instance_hook_to_xefg() {
    if (m_swapchain_hook == nullptr) {
        return false;
    }

    const auto present_ok = m_swapchain_hook->hook_method(
        8, Address{reinterpret_cast<void*>(&D3D12Hook::present)});
    const auto present1_ok = m_swapchain_hook->hook_method(
        22, Address{reinterpret_cast<void*>(&D3D12Hook::present1)});
    const auto resize_buffers_ok = m_swapchain_hook->hook_method(
        13, Address{reinterpret_cast<void*>(&D3D12Hook::resize_buffers)});
    const auto resize_target_ok = m_swapchain_hook->hook_method(
        14, Address{reinterpret_cast<void*>(&D3D12Hook::resize_target)});
    const auto resize_buffers1_ok = m_swapchain_hook->hook_method(
        39, Address{reinterpret_cast<void*>(&D3D12Hook::resize_buffers1)});

    return present_ok
        && present1_ok
        && resize_buffers_ok
        && resize_target_ok
        && resize_buffers1_ok;
}
```

The exact helper shape may differ.

Important properties:

- reasserting 8/22/13/14 does not replace `m_old_vtable`;
- slot 39 is added to the already-owned copy;
- no second `VtableHook` is created;
- the existing original function chain remains the one captured when the native instance hook was first created.

If slot validation/promotion fails, fail the XeFG bind without destroying the current native instance hook.

Do not convert this failure into partial manual unhook/recreate logic.

---

## 7. Destructive Phase Ordering for Reuse

For the promotion path, preserve the existing renderer reset behavior but **do not reset `m_swapchain_hook`**.

Conceptually:

```cpp
if (plan.disposition == CandidateDisposition::NoActiveBinding) {
    const bool replacing_active_non_xefg = ...;

    if (plan.previous.active && g_framework != nullptr) {
        g_framework->on_reset();
    }

    if (replacing_active_non_xefg && g_framework != nullptr) {
        g_framework->on_reset();
    }

    m_present_hook.reset();

    if (!reuse_existing_native_instance_hook) {
        m_swapchain_hook.reset();
    }

    const auto commit = m_xefg_session.commit_prepared_candidate(...);
    if (!commit.committed) {
        return false;
    }

    sync_xefg_binding_aliases();

    if (!reuse_existing_native_instance_hook) {
        m_swapchain_hook = std::move(prepared.hook);
    }

    m_swapchain_source = SwapchainSource::XeFGInternal;
    m_is_phase_1 = false;
    m_hooked = true;
    ...
}
```

Preserve current semantic commit order unless a concrete compile/runtime reason requires a narrowly justified adjustment.

Do not clear or recreate the reused hook merely for symmetry with the different-object path.

---

## 8. Fail Closed on Unexpected Same-Target Collisions

The normal reusable case is the strict native-instance promotion topology above.

There may also be inconsistent physical state where:

```text
candidate == m_swapchain_hook target
```

but the strict reuse conditions are not satisfied.

Do **not** create a second `VtableHook` in that state.

Before calling `prepare_xefg_instance_hook()` for any candidate, add a collision guard equivalent to:

```cpp
if (candidate_matches_current_hook_target
    && !can_promote_existing_native_instance_hook) {
    // fail closed: do not stack a new VtableHook over our own current target
    return false;
}
```

A narrow diagnostic log is recommended, for example:

```text
[XeFG][Bind] accepted = false, reason = existing_hook_target_collision
```

or a similarly stable reason.

Do not automatically destroy the current hook to recover from an inconsistent state in this PR.

The hook monitor/session consistency machinery already owns broader inconsistent-state handling.

---

## 9. Different-Object Paths Must Stay Unchanged

The normal 2R6 prepare-before-destructive-work behavior remains correct for:

```text
native instance A -> XeFG candidate B
XeFG active A     -> changed XeFG candidate B
no existing instance hook -> XeFG candidate B
```

where the candidate is not the current REFramework-owned hook target.

Keep:

```text
GetDevice validation
-> prepare five-method candidate hook
-> ensure preparation succeeds
-> renderer reset if required
-> remove old physical hook
-> semantic commit
-> install prepared hook
```

Do not regress the rollback property that hook preparation failure leaves the old active state untouched.

---

## 10. Session Semantics Must Not Change

Do not modify the behavior of:

```cpp
XeFGPresentationSession::plan_candidate(...)
XeFGPresentationSession::commit_identical_candidate(...)
XeFGPresentationSession::commit_prepared_candidate(...)
```

The following dispositions/generation semantics remain exact:

```text
NoActiveBinding
    -> initial semantic commit, generation = 1

Identical
    -> no hook/reset/destructive work, generation unchanged

SameSwapchainUpdate
    -> generation += 1, existing XeFG hook retained

ChangedSwapchainReplacement
    -> generation += 1, prepared different-object hook transaction
```

Do not add `VtableHook` awareness to `XeFGPresentationSession`.

Do not add a new semantic candidate disposition merely for native physical hook reuse unless absolutely required; this should normally remain a physical implementation detail of `NoActiveBinding`.

---

## 11. Non-Negotiable Invariants

This PR must preserve all of the following.

### COM ownership

- active XeFG swapchain remains borrowed;
- active queue/device remain strong `ComPtr`;
- candidate swapchain may remain a strong `ComPtr` during the transaction;
- bounded old swapchain keepalive behavior remains unchanged;
- no forced Release loops;
- no refcount draining;
- no persistent strong ownership of the active XeFG swapchain.

### Locking

- lifecycle/hook-monitor mutex remains the serialization authority;
- lock order remains lifecycle mutex -> pending-candidate mutex;
- do not introduce new mutexes;
- do not hold lifecycle mutex across Intel vendor Init/Destroy calls;
- do not change pending-candidate COM release ordering.

### Present / resize

No behavior change to:

- `Present`
- `Present1`
- nested Present forwarding
- observe-only callback suppression
- first render-boundary tracking
- post-resize Present sampling
- MHW ResizeTarget hold
- ResizeBuffers/ResizeBuffers1 completion
- ResizeTarget failed-result hold clear
- native resize callback policy.

### Runtime / monitor

No behavior change to:

- runtime registry;
- loader handoff;
- runtime detach;
- Destroy reconciliation;
- detached uncertainty;
- hook-monitor timeout classification;
- quarantine/grace thresholds.

---

## 12. Files in Scope

Expected primary files:

```text
src/D3D12Hook.cpp
src/D3D12Hook.hpp
```

`D3D12Hook.hpp` is needed only if a narrow helper declaration is introduced.

Prefer **no changes** to:

```text
src/compatibility/xefg/XeFGPresentationSession.cpp
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGBinding.*
src/compatibility/xefg/XeFGCandidateHandoff.*
src/compatibility/xefg/XeFGCompatibility.*
src/compatibility/xefg/XeFGDiscovery.*
src/compatibility/xefg/XeFGRuntimeRegistry.*
src/compatibility/xefg/XeFGResizeLifecycle.*
src/REFramework.cpp
cmake.toml
CMakeLists.txt
```

No new source files are required.

If implementation begins changing semantic policy or compatibility subsystems outside `D3D12Hook`, stop and reassess rather than broadening the patch.

---

## 13. Required Acceptance Matrix

Review the final code against every row.

| Existing physical state | Candidate | Expected result |
|---|---|---|
| no instance hook / phase 1 | XeFG A | existing 2R6 prepared-hook initial bind |
| native instance hook A | XeFG B | existing different-object prepared-hook path |
| native instance hook A | XeFG A | **reuse/promote existing hook; never construct second VtableHook on A** |
| active XeFG A | identical XeFG A | existing non-destructive Identical path |
| active XeFG A | changed queue/mode, same A | existing SameSwapchainUpdate path |
| active XeFG A | XeFG B | existing ChangedSwapchainReplacement path |
| inconsistent state, hook target A but strict reuse predicate false | candidate A | fail closed; no second VtableHook; old state not destructively replaced |

Also verify both:

```text
observe_only candidate
render-capable candidate
```

for the native-A -> XeFG-A promotion topology.

---

## 14. Specific Source-Level Checks

After implementation, manually verify:

1. No code path can call `prepare_xefg_instance_hook(candidate)` while `candidate` is already the target of the current `m_swapchain_hook`, except if the old hook has first been safely removed and the object restored to its underlying vtable. The preferred implementation for the normal takeover case is reuse, not remove/recreate.
2. Native-A -> XeFG-A promotion never executes `m_swapchain_hook.reset()` before semantic commit.
3. Native-A -> XeFG-A promotion enables `ResizeBuffers1[39]`.
4. Existing original method chain for slots 8/22/13/14 remains owned by the reused `VtableHook` and is not replaced with a pointer into a destroyed hook's `m_raw_data`.
5. Different-object candidate preparation still occurs before destructive work.
6. Preparation failure on different-object paths still leaves the old hook/binding unchanged.
7. Promotion failure leaves the native instance hook usable and does not partially unhook it.
8. `CandidateDisposition`, generation semantics, resize hold reasons, and render-boundary semantics are unchanged.
9. No new strong active swapchain ownership is introduced.
10. No Present/Resize/monitor/runtime policy is changed.

---

## 15. Diagnostics

A small debug or warning diagnostic is acceptable for the two new physical decisions:

```text
native_instance_hook_promoted
existing_hook_target_collision
```

Do not add per-Present or per-frame logging.

Do not add new timers, retries, sleeps, polling, or asynchronous recovery.

---

## 16. Validation

Run the normal validation set:

```text
cmake -S . -B build-2r8 -G "Visual Studio 17 2022" -A x64
cmake --build build-2r8 --config Release --target REFramework --parallel 4
python dev/audit_direct_access_clang.py
git diff --check
```

If the local environment cannot perform the build, report that explicitly. Do not claim build/runtime validation that was not actually run.

### Diff audit

Expected implementation diff should be very small and concentrated in `D3D12Hook.cpp/.hpp`.

Review for accidental modifications to:

```text
Present / Present1
ResizeBuffers / ResizeBuffers1 / ResizeTarget policy
XeFGPresentationSession semantic decisions
runtime detach / monitor
candidate discovery
COM ownership
loader/runtime registry
```

There should be none beyond compile-only helper access if strictly necessary.

---

## 17. Runtime Regression Targets

The existing successful XeFG test matrix remains relevant:

```text
RE9
DD2
MHW
Pragmata
```

A repeat smoke test should confirm the normal pending initial-bind path remains unchanged.

However, the specific new regression target is the topology:

```text
native D3D12 instance hook established first
-> later XeFG candidate is the exact same swapchain object
-> native instance hook promoted to XeFG without nested VtableHook ownership
-> Present/Present1 continue resolving valid originals
-> ResizeBuffers1[39] is active
-> no crash/hang during takeover or later unhook/destroy
```

If this topology cannot be reproduced easily in an existing game, do not add production-only simulation code. Source-level validation of the ownership sequence plus the normal four-game smoke test is acceptable for this PR, with the untested topology stated clearly in the PR body.

---

## 18. Forbidden Changes

Do not:

- redesign 2R6 candidate planning;
- change `XeFGPresentationSession` ownership boundaries;
- introduce a generic frame-generation abstraction;
- touch Streamline/DLSSG/FSRFG behavior;
- change Special K behavior;
- change Intel runtime result semantics;
- change MHW-specific resize policy;
- change native Present behavior;
- remove native Present1 support as part of this work;
- add COM refcount drains;
- add persistent active swapchain AddRef ownership;
- add timers/retries/polling;
- make hook-monitor automatically destroy an inconsistent collision;
- alter candidate generation semantics;
- create a second REFramework `VtableHook` on the same currently-hooked object.

---

## 19. Definition of Done

2R8 is complete only when:

1. native instance hook A -> XeFG candidate A cannot stack a second `VtableHook` over A;
2. the existing native instance hook is safely promoted/reused for the normal same-object takeover;
3. `ResizeBuffers1[39]` is enabled on the reused hook;
4. unexpected same-target collisions fail closed without destructive replacement;
5. all different-object 2R6 transaction behavior remains unchanged;
6. semantic session behavior remains unchanged;
7. COM and lock invariants remain unchanged;
8. build/audit/diff validation passes or any unavailable validation is reported accurately;
9. the PR remains narrowly limited to physical D3D12 hook takeover hardening.

After this PR is reviewed and the regression smoke test is satisfactory, the XeFG second-stage refactor can be considered **final-acceptance complete** unless a new concrete runtime defect is observed.
