# P5-A Work Order — XeFG Destroy Lifecycle Result Semantics

Date: 2026-09-14

## Status

Implementation work order. This document is intentionally limited to the REFramework side of the XeFG Destroy lifecycle-result contract.

P5-A must remain a small, reviewable REF-only change. Do not fold P5-B, XeLL lifecycle work, mutex-order changes, or OptiScaler ownership changes into this PR.

## Verified remote baseline

This work order was prepared against the current remote heads below:

- REFramework: `onehoon/REFramework` `master`
  - commit: `f000aa75e1c30009b0db26f8bb290cc477b11032`
  - message: `fix: count nested XeFG Presents as monitor activity`
- OptiScaler compatibility reference: `onehoon/OptiScaler` `reframework-0.9`
  - commit: `7b19ecbefec9dc872b11d3e3f2e59e018fa278bc`

OptiScaler `master` is not the target of this work. The compatibility contract for P5-A is the current `reframework-0.9` lifecycle behavior.

## Objective

Make REFramework distinguish between:

1. a generic XeFG call that returned a non-negative result, and
2. a XeFG **Destroy lifecycle operation that is actually complete**.

For the current OptiScaler `reframework-0.9` contract, only an exact XeFG Destroy success (`result == 0`) is lifecycle-complete.

A positive result must not cause REF to reconcile and discard its detached/uncertain lifecycle state.

## Proven current mismatch

### REFramework generic result helper

`src/compatibility/xefg/XeFGResult.hpp` currently defines:

```cpp
constexpr bool succeeded(int32_t result) noexcept {
    return result >= 0;
}

constexpr bool failed(int32_t result) noexcept {
    return result < 0;
}
```

This generic helper is currently also used for Destroy lifecycle reconciliation.

### REFramework Destroy reconciliation

`src/compatibility/xefg/XeFGPresentationSession.cpp` currently evaluates Destroy as:

```cpp
XeFGPresentationSession::DestroyReconciliation XeFGPresentationSession::evaluate_destroy_result(
    size_t runtime_slot,
    void* context,
    int32_t result) const noexcept {
    return {
        m_detached_state.active
            && xefg_result::succeeded(result)
            && m_detached_state.previous_runtime.slot == runtime_slot
            && m_detached_state.previous_runtime.context == context,
        m_detached_state.previous_generation,
    };
}
```

Therefore any positive result passes the same lifecycle-completion predicate as `0`.

`D3D12Hook::note_xefg_destroy_result(...)` then treats an accepted reconciliation as a completed Destroy transition. The current accepted path commits the reconciliation and clears/resets hook/runtime state associated with the detached XeFG binding.

The effective call path is:

```text
XeFGCompatibility::dispatch_destroy
  -> original XeFG Destroy
  -> D3D12Hook::note_xefg_destroy_result
  -> XeFGPresentationSession::evaluate_destroy_result
  -> accepted
  -> commit_destroy_reconciliation
  -> clear/reset detached XeFG hook/runtime state
```

### OptiScaler `reframework-0.9` lifecycle contract

Current `OptiScaler/framegen/xefg/XeFG_Dx12.cpp` uses exact success for Destroy completion:

```cpp
const auto result = XeFGProxy::Destroy()(context);

if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
{
    _swapchainRecreationBlocked = true;

    if (IsXeFGWarning(result))
    {
        _swapChainContext = nullptr;
        // warning is quarantined
    }
    else
    {
        _swapChainContext = context;
        // failed context is retained
    }

    return false;
}
```

In that branch, `IsXeFGWarning(result)` is `static_cast<int32_t>(result) > 0`.

So the current cross-project state machines disagree for a positive Destroy result:

```text
OptiScaler reframework-0.9:
  result > 0
  -> Destroy NOT lifecycle-complete
  -> recreation blocked
  -> warning state quarantined

REFramework master:
  result > 0
  -> generic succeeded(result) == true
  -> Destroy may be reconciled as complete
  -> detached/recovery state may be cleared
```

This is a code-proven semantic mismatch, not a speculative race.

## Required implementation

### 1. Add a Destroy-specific completion predicate

File:

- `src/compatibility/xefg/XeFGResult.hpp`

Keep the existing generic helpers unchanged.

Add a dedicated predicate for lifecycle completion, conceptually:

```cpp
constexpr bool destroy_completed(int32_t result) noexcept {
    return result == 0;
}
```

Naming may be adjusted to local style if necessary, but the semantic requirement is fixed:

- `0` -> Destroy lifecycle complete
- `> 0` -> Destroy lifecycle incomplete
- `< 0` -> Destroy lifecycle incomplete

Do **not** redefine `xefg_result::succeeded()` globally as `result == 0`.

P5-A is not intended to change how other XeFG APIs interpret non-negative return values.

### 2. Use the Destroy-specific predicate in presentation-session reconciliation

File:

- `src/compatibility/xefg/XeFGPresentationSession.cpp`

Change only the result predicate inside `evaluate_destroy_result(...)`:

```cpp
m_detached_state.active
    && xefg_result::destroy_completed(result)
    && m_detached_state.previous_runtime.slot == runtime_slot
    && m_detached_state.previous_runtime.context == context
```

Preserve the existing checks for:

- active detached state,
- runtime slot identity,
- runtime context identity,
- previous generation bookkeeping.

Do not weaken identity matching to compensate for a failed/warning Destroy.

### 3. Preserve the current accepted-success path

File:

- `src/D3D12Hook.cpp`

No structural rewrite is required for P5-A.

After the predicate change, `note_xefg_destroy_result(...)` should enter its current reconciliation/clear path only when `result == 0` and the existing identity checks match.

Verify that positive and negative results return through the existing `!reconciliation.accepted` path without calling:

- `commit_destroy_reconciliation(...)`,
- detached-state clear/reset operations,
- runtime discovery/recovery resets that are conditioned on accepted Destroy reconciliation.

Do not add a fallback clear for warnings.

### 4. Keep Destroy dispatch ordering unchanged

File:

- `src/compatibility/xefg/XeFGCompatibility.cpp`

P5-A must not reorder the actual XeFG Destroy call, REF detach preparation, or result notification.

Verify only that the original integer result is passed unchanged to `note_xefg_destroy_result(...)` exactly as today.

Any pre-final-proxy retirement handoff belongs to P5-B, not this PR.

## Required behavior matrix

| XeFG Destroy result | Generic `succeeded(result)` | Destroy lifecycle complete | REF action |
| --- | --- | --- | --- |
| `0` | `true` | `true` | Reconcile matching detached state and run existing clear/reset path |
| `> 0` | `true` | `false` | Keep detached/uncertain state; do not reconcile as Destroy success |
| `< 0` | `false` | `false` | Keep detached/uncertain state; do not reconcile |

The important regression guard is that the generic success column does **not** change in this PR.

## Logging

Existing `destroy_success` / `clear_detached` logging should only be emitted for the exact-success reconciliation path after this change.

If an additional diagnostic is added for incomplete Destroy results, keep it low-noise and lifecycle-specific. It should clearly include at least:

- result integer,
- runtime slot,
- context,
- whether detached state remains active.

Do not convert expected positive-warning handling into an error-level log solely because lifecycle reconciliation is deferred.

Logging changes are optional if the existing diagnostics already make the retained state observable.

## Validation

### Source-level validation

Confirm all of the following:

1. `xefg_result::succeeded(1)` remains `true`.
2. the new Destroy completion predicate is true only for `0`.
3. `XeFGPresentationSession::evaluate_destroy_result(..., 0)` can still accept a matching detached runtime.
4. the same matching runtime with a positive result is not accepted.
5. the same matching runtime with a negative result is not accepted.
6. slot/context mismatch remains rejected even for result `0`.

If there is an existing lightweight test location for this compatibility code, add focused cases there. Do not introduce a new test framework solely for P5-A.

A small compile-time check for the pure helper is acceptable if consistent with repository style, for example:

```cpp
static_assert(xefg_result::destroy_completed(0));
static_assert(!xefg_result::destroy_completed(1));
static_assert(!xefg_result::destroy_completed(-1));
```

These assertions are optional; behavior is mandatory.

### Build validation

Build the same REFramework targets/configuration currently used by the fork for the XeFG-compatible Capcom path. P5-A must introduce no new warnings or linkage dependencies.

### Runtime validation

Where practical, exercise or simulate three Destroy outcomes:

```text
0   -> detached state reconciles and clears
+N  -> detached state remains quarantined/uncertain
-N  -> detached state remains quarantined/uncertain
```

For the positive case, verify specifically that generic recovery is not reopened merely because the result is non-negative. The monitor/recovery policy must continue to observe the retained detached-uncertain state.

A real runtime that never naturally returns a positive Destroy result does not block this PR; the bug is visible directly in the state-machine predicates and can be validated with a focused call/test or temporary diagnostic injection.

## Non-goals / forbidden scope expansion

Do not include any of the following in P5-A:

- OptiScaler code changes.
- OptiScaler `master` lifecycle backports.
- final XeFG proxy pre-retire notification/detach handoff — P5-B.
- XeLL create/destroy ownership or fail-closed changes — P6.
- D3D12 command-queue ownership changes — P7.
- FG mutex / REF hook-monitor mutex lock-order redesign — P7.
- generic XeFG return-semantics changes outside Destroy reconciliation.
- broad `D3D12Hook` cleanup or renderer lifecycle refactors.
- PR17/PR18 experimental OptiScaler changes.

## Review checklist

Before merge, reviewer should be able to answer **yes** to every item below:

- Is generic `result >= 0` behavior still intact for non-Destroy use?
- Is exact `0` now the only result that can complete REF's Destroy reconciliation?
- Can a positive Destroy warning no longer clear the detached lifecycle state?
- Are runtime slot/context identity checks unchanged?
- Does exact-success behavior retain the current reconciliation path?
- Did the PR avoid OptiScaler, XeLL, proxy-retire, mutex, and queue-lifetime scope?
- Does the code build cleanly?

## Definition of done

P5-A is complete when REFramework no longer treats a positive XeFG Destroy result as proof that the detached XeFG runtime lifecycle has completed, while all generic XeFG result semantics and the exact-success (`0`) Destroy path remain unchanged.

The intended end state is:

```text
Destroy exact success
  -> REF may reconcile and clear matching detached state

Destroy warning or failure
  -> REF retains detached/uncertain state
  -> no premature lifecycle reconciliation
```

This PR establishes only the Destroy-result contract. The separate P5-B work must address the final-public-proxy retirement ordering between OptiScaler and REFramework.