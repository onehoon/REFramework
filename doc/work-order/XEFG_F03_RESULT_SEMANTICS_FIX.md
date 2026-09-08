# Work Order: XeFG F-03 — Correct Intel Result Success / Warning Semantics

Date: 2026-09-08  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `dfcf5c5f635d4b814ad2bbb0695239c630ff4a9a` (`XeFG PR-C: harden hook monitor across runtime transitions and stale bindings`, PR #36)

This is a **narrow correctness fix** for the fork REFramework XeFG integration.

Do not expand this PR into a general XeFG lifecycle refactor.

The only confirmed REFramework-side defect targeted here is:

> REFramework currently treats only numeric result `0` as XeFG success in several lifecycle paths, while Intel XeSS-FG defines positive result values as warnings / non-failures and negative values as errors.

The required semantic rule is therefore:

```text
result == 0  -> success
result > 0   -> warning / non-failure
result < 0   -> failure
```

For lifecycle state decisions, REFramework must use:

```cpp
result >= 0
```

for success/non-failure and:

```cpp
result < 0
```

for failure.

---

# 1. Why This PR Is Required

Intel's current XeSS-FG developer guide explicitly defines result handling as:

```text
Success:  XEFG_SWAPCHAIN_RESULT_SUCCESS (zero)
Warnings: positive values - not a failure
Errors:   negative values
```

Intel also provides the recommended success predicate conceptually as:

```cpp
return static_cast<int>(result) >= 0;
```

Reference:

- `intel/xess`
- `doc/xess_fg_developer_guide_english.md`
- current official documentation in the Intel XeSS repository

This matters because REFramework's XeFG integration now tracks runtime lifecycle state, borrowed internal swapchain binding state, detached fail-closed state, and hook-monitor recovery decisions.

Misclassifying an Intel warning as a failure can therefore change persistent REFramework state rather than merely changing a log message.

The most important example is Destroy reconciliation:

```text
REF pre-Destroy detach
-> Intel Destroy returns a positive warning
-> Intel semantics: non-failure
-> current REF semantics: not equal to 0, therefore treated like failure
-> detached_uncertain may remain active
-> hook monitor continues suppressing generic recovery
-> game / XeFG may continue, while REF overlay remains detached
```

That is a real correctness mismatch in the fork and must be fixed independently of any OptiScaler change.

---

# 2. Current Master Behavior To Audit

Do not assume the locations below are the complete list.

Before editing, search the **current branch after rebasing onto latest `master`** for all XeFG result comparisons that can influence control flow or lifecycle state.

At planning time, the confirmed locations are the following.

## 2.1 `src/compatibility/xefg/XeFGDiscovery.cpp`

Current code defines:

```cpp
constexpr int32_t kXefgSuccess = 0;
```

and `XeFGDiscovery::build_binding_candidate()` currently contains logic equivalent to:

```cpp
if (observation.init_result != kXefgSuccess) {
    result.reject_reason = "init_failed";
    return result;
}
```

This is wrong for a positive warning result.

A successful-with-warning Intel Init must remain eligible for normal candidate validation.

The rest of candidate validation must still run normally:

```text
factory capture present
internal swapchain present
queue valid
IDXGISwapChain3 available
HWND matches
candidate device valid
queue/device relationship valid
```

Do not bypass those checks merely because Init was non-negative.

## 2.2 `src/compatibility/xefg/XeFGCompatibility.cpp`

`dispatch_get_swapchain()` currently recognizes the returned public proxy for diagnostics only when the result equals exact success (`0`).

Logic equivalent to:

```cpp
if (result == kXefgSuccess && swap_chain != nullptr && *swap_chain != nullptr) {
    ...
}
```

must use the Intel non-failure semantics instead.

A positive warning plus a valid output pointer is not an API failure for REFramework lifecycle classification.

Also audit this file for any other XeFG API result comparisons introduced before implementation begins.

## 2.3 `src/D3D12Hook.cpp`

`D3D12Hook::note_xefg_destroy_result(...)` currently contains logic equivalent to:

```cpp
if (!m_xefg_detached_state.active
    || result != 0
    || m_xefg_detached_state.previous_runtime.slot != runtime_slot
    || m_xefg_detached_state.previous_runtime.context != context) {
    return;
}
```

The `result != 0` condition is incorrect.

The detached state should be reconciled as a successfully completed Destroy when Intel returns any non-negative result for the matching runtime identity.

A negative result must continue to preserve the existing fail-closed behavior.

Do **not** weaken runtime identity matching.

The following must remain required:

```text
matching runtime slot
matching runtime context
active detached state
```

---

# 3. Required Implementation

## 3.1 Introduce one shared XeFG result predicate

Do not scatter new `>= 0` / `< 0` checks throughout unrelated code if a small shared helper can keep the Intel semantic rule explicit.

Preferred shape:

```cpp
#pragma once

#include <cstdint>

namespace xefg_result {

constexpr bool succeeded(int32_t result) noexcept {
    return result >= 0;
}

constexpr bool failed(int32_t result) noexcept {
    return result < 0;
}

} // namespace xefg_result
```

A dedicated small header under:

```text
src/compatibility/xefg/
```

is acceptable, for example:

```text
XeFGResult.hpp
```

If the current source layout already has a clearly better XeFG-common location after rebasing, use that instead.

Do not make this a broad utility outside the XeFG compatibility layer.

Do not include Intel SDK headers solely to implement this helper; REFramework's runtime interception already operates on `int32_t` return values.

## 3.2 Use the helper for lifecycle-affecting Intel result checks

At minimum update the confirmed paths:

```text
XeFGDiscovery::build_binding_candidate()
XeFGCompatibility::dispatch_get_swapchain()
D3D12Hook::note_xefg_destroy_result()
```

Conceptual replacements:

```cpp
if (xefg_result::failed(observation.init_result)) {
    result.reject_reason = "init_failed";
    return result;
}
```

```cpp
if (xefg_result::succeeded(result)
    && swap_chain != nullptr
    && *swap_chain != nullptr) {
    ...
}
```

```cpp
if (!m_xefg_detached_state.active
    || xefg_result::failed(result)
    || m_xefg_detached_state.previous_runtime.slot != runtime_slot
    || m_xefg_detached_state.previous_runtime.context != context) {
    return;
}
```

Exact naming may differ if a better local naming convention exists, but the semantics must remain obvious.

---

# 4. Mandatory Full Audit Before Completing The PR

Search the current REFramework source for XeFG API result handling, not just the three known lines.

At minimum search for patterns equivalent to:

```text
kXefgSuccess
XEFG_SWAPCHAIN_RESULT_SUCCESS
result == 0
result != 0
result > 0
result < 0
init_result == 0
init_result != 0
```

Scope the review to code that is actually processing Intel XeFG/XeSS-FG API return values.

Do not blindly replace unrelated HRESULT, Win32, DXGI, D3D12, Detours, or internal REFramework result comparisons.

For every XeFG result comparison found, classify it as one of:

```text
A. lifecycle success/failure decision
B. diagnostic-only exact-success check
C. intentional exact enum comparison
D. unrelated numeric result
```

Rules:

- Category A must follow Intel non-negative-success semantics.
- Category B should normally follow the same semantics if a positive warning still leaves valid output/state to inspect.
- Category C may stay exact only when the exact enum value itself is materially required; document why in the PR if such a case exists.
- Category D must not be changed.

The PR description should state which XeFG comparisons were audited and whether any intentional exact-zero checks remain.

---

# 5. Required Lifecycle Semantics After The Fix

## 5.1 Init

Required behavior:

```text
Intel Init result < 0
-> candidate rejected as init_failed
-> existing fail-closed behavior preserved

Intel Init result == 0
-> normal candidate validation continues

Intel Init result > 0
-> normal candidate validation continues
-> warning must not be converted into lifecycle failure by REF
```

A positive warning does **not** mean REFramework should automatically accept a candidate.

All existing physical/semantic candidate checks remain mandatory.

## 5.2 GetSwapChainPtr

Required behavior:

```text
result < 0
-> no successful public-proxy handling

result >= 0 and output pointer valid
-> allow current diagnostic/public-proxy handling
```

Do not create any new strong ownership of the proxy as part of this PR.

The borrowed/internal swapchain architecture from PR-B/PR-C must remain unchanged.

## 5.3 Destroy

Required behavior:

```text
matching detached state
+ matching runtime slot/context
+ Destroy result < 0
-> keep detached_uncertain fail-closed state
-> do not restore old borrowed hook
-> do not allow speculative generic recovery

matching detached state
+ matching runtime slot/context
+ Destroy result >= 0
-> treat Destroy as completed for REF lifecycle reconciliation
-> clear the matching detached uncertainty according to current PR-C behavior
```

Do not reinterpret a positive warning as proof that every external system is healthy.

This PR only says:

> Intel did not report an API failure, so REF must not retain a state that specifically means "vendor Destroy failed" solely because the return value was non-zero.

---

# 6. Logging Requirements

Preserve the existing raw numeric Intel result in lifecycle logs.

Do not normalize a warning to zero before logging.

For example, existing logs such as:

```text
[XeFG][RuntimeLifecycle] ... result = N
```

must continue to expose the original `N`.

It is acceptable, but not required, to add a small textual classification if it stays low-noise, for example:

```text
result_class = success
result_class = warning
result_class = error
```

Do not turn this narrow correctness PR into a general logging redesign.

Normal-user log volume must not materially increase.

---

# 7. Explicit Non-Goals

The following are **out of scope** for this PR even though they are relevant to the wider XeFG investigation.

## 7.1 Do not modify OptiScaler behavior

Do not attempt to compensate for upstream OptiScaler's:

```text
backbuffer Release-until-refcount-limit logic
FGPreserveSwapChain behavior
DestroySwapchainContext handling
ReleaseSwapchain handling
currentFGSwapchain/currentRealSwapchain state
```

Those are separate interaction/upstream issues.

## 7.2 Do not change COM ownership

Do not:

```text
turn the borrowed active XeFG swapchain into a long-lived ComPtr
add persistent AddRef on the Intel internal swapchain
hold a proxy keepalive across Intel Destroy
force Release unknown external references
```

PR-B's borrowed ownership model must remain intact.

## 7.3 Do not change VtableHook lifecycle

Do not modify:

```text
pre-Init detach
pre-Destroy detach
hook replacement ordering
physical VtableHook target identity checks
pending candidate handoff
```

unless a compilation-only adjustment is unavoidable.

## 7.4 Do not change hook-monitor policy

Do not change:

```text
runtime-transition suppression
detached-uncertain suppression
sustained-timeout quarantine
inconsistent-binding quarantine
Present timestamp/count logic
```

The only expected hook-monitor behavior change is the natural consequence of correctly classifying a non-negative Destroy result.

## 7.5 Do not add speculative Init/Destroy serialization

The separate question of whether Intel Init and Destroy can overlap across threads remains a runtime-evidence issue.

Do not add new mutex/state-machine complexity in this PR.

## 7.6 Do not address the NVIDIA + MHW startup issue

The NVIDIA MHW backbuffer-release/startup problem is separate from this Intel result-semantics fix.

---

# 8. Validation Requirements

## 8.1 Compile-time semantic verification

The implementation must make these cases unambiguous in review:

```text
-1  -> failed
 0  -> succeeded
+1  -> succeeded
```

If the repository has an appropriate lightweight test location for this helper, add focused tests there.

If there is no established unit-test target for this compatibility code, do not introduce a new test framework solely for this PR. In that case, keep the helper `constexpr` and make the semantics trivially reviewable.

## 8.2 Build

Build the affected REFramework target using the repository-supported Windows/CMake workflow.

At minimum verify that:

```text
REFramework target compiles successfully
new helper includes do not create include cycles
no warning/error is introduced by the result helper
```

## 8.3 Static audit

Before completion, repeat the source search for XeFG exact-zero result checks.

The final report must list any remaining exact-zero XeFG checks and explain why they are intentionally exact.

## 8.4 Existing lifecycle invariants

Verify by code review that the patch does not alter these existing invariants:

```text
active XeFG swapchain remains borrowed
candidate ownership remains temporary
pending candidate ownership is released before vendor Destroy
renderer reset remains before matching runtime detach
VtableHook is removed before matching vendor Destroy/re-init
vendor Init/Destroy is not called while holding REF hook_monitor_mutex
generic D3D12 recovery remains suppressed during runtime transitions
negative Destroy result remains fail-closed
```

---

# 9. Suggested Focused Test Matrix

Runtime Intel hardware testing is useful but is not required to prove the numeric predicate itself.

If a practical mock/unit seam already exists, cover:

| API path | Result | Expected REF classification |
|---|---:|---|
| InitFromSwapChainDesc | `-1` | failure / candidate rejected |
| InitFromSwapChainDesc | `0` | non-failure / candidate validation continues |
| InitFromSwapChainDesc | `+1` | non-failure / candidate validation continues |
| GetSwapChainPtr | `-1` | failure |
| GetSwapChainPtr | `0` + valid pointer | normal diagnostic handling |
| GetSwapChainPtr | `+1` + valid pointer | normal diagnostic handling |
| Destroy | `-1` | detached uncertainty preserved |
| Destroy | `0` | successful lifecycle reconciliation |
| Destroy | `+1` | successful lifecycle reconciliation |

Do not invent a fake Intel warning enum name if the local headers do not expose one.

Numeric positive test values are enough to validate the documented result-class semantics.

---

# 10. Recommended Patch Shape

Expected source footprint should be small.

Likely files:

```text
src/compatibility/xefg/XeFGResult.hpp          # new, if using a dedicated helper
src/compatibility/xefg/XeFGDiscovery.cpp
src/compatibility/xefg/XeFGCompatibility.cpp
src/D3D12Hook.cpp
```

Potentially fewer files are acceptable if the helper is placed in an already appropriate XeFG-common header.

A patch that touches unrelated renderer, hook-monitor, VtableHook, COM ownership, or OptiScaler compatibility behavior should be treated as scope drift and justified before merge.

---

# 11. Example Minimal Implementation

Example only; adapt includes/naming to the current tree.

```cpp
// src/compatibility/xefg/XeFGResult.hpp
#pragma once

#include <cstdint>

namespace xefg_result {

constexpr bool succeeded(int32_t result) noexcept {
    return result >= 0;
}

constexpr bool failed(int32_t result) noexcept {
    return result < 0;
}

} // namespace xefg_result
```

Then:

```cpp
#include "XeFGResult.hpp"
```

and:

```cpp
if (xefg_result::failed(observation.init_result)) {
    result.reject_reason = "init_failed";
    return result;
}
```

```cpp
if (xefg_result::succeeded(result)
    && swap_chain != nullptr
    && *swap_chain != nullptr) {
    ...
}
```

For `D3D12Hook.cpp`, include the helper through the explicit XeFG compatibility path rather than creating a generic dependency.

```cpp
if (!m_xefg_detached_state.active
    || xefg_result::failed(result)
    || m_xefg_detached_state.previous_runtime.slot != runtime_slot
    || m_xefg_detached_state.previous_runtime.context != context) {
    return;
}
```

---

# 12. PR Review Checklist

The PR is ready for merge only if all of the following are true.

- [ ] Branch was rebased/created from the latest `master`.
- [ ] Intel result semantics are centralized or otherwise consistently explicit.
- [ ] Positive XeFG result values are no longer treated as lifecycle failures merely because they are non-zero.
- [ ] Negative values still follow existing failure/fail-closed behavior.
- [ ] `build_binding_candidate()` still performs all existing validation after a non-negative Init result.
- [ ] `dispatch_get_swapchain()` does not introduce new proxy ownership.
- [ ] `note_xefg_destroy_result()` still requires exact runtime slot/context identity.
- [ ] Failed Destroy remains detached/fail-closed.
- [ ] Borrowed XeFG swapchain ownership remains unchanged.
- [ ] No VtableHook lifecycle redesign was introduced.
- [ ] No hook-monitor timeout policy redesign was introduced.
- [ ] No OptiScaler workaround was introduced.
- [ ] All XeFG result comparisons in the fork were audited.
- [ ] Any remaining intentional exact-zero XeFG checks are documented in the PR.
- [ ] REFramework builds successfully.

---

# 13. PR Description Requirements

The PR description should include:

1. **Problem**
   - Intel XeSS-FG uses non-negative result values for success/warning and negative values for errors.
   - REF previously used exact-zero success checks in lifecycle-sensitive paths.

2. **Fix**
   - central result predicate
   - audited XeFG result comparisons
   - corrected Init / GetSwapChainPtr / Destroy handling

3. **Behavioral safety**
   - negative failures remain fail-closed
   - borrowed swapchain ownership unchanged
   - no new COM references
   - no hook-monitor redesign

4. **Validation**
   - build result
   - list of audited XeFG result checks
   - remaining exact-zero checks, if any, with rationale

---

# 14. Final Required Outcome

After this PR, the following must be true:

```text
Intel XeFG result < 0
    -> REFramework treats the API operation as failed

Intel XeFG result == 0
    -> REFramework treats the API operation as successful

Intel XeFG result > 0
    -> REFramework treats the API operation as non-failed / warning
    -> lifecycle state is not incorrectly left detached or rejected solely because the result is non-zero
```

This PR should be small, reviewable, and isolated.

Do not use it as an opportunity for further speculative XeFG hardening.
