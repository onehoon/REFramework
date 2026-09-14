# Work Order: XeFG Nested Present Hook-Monitor Activity Accounting Fix

Date: 2026-09-14  
Repository: `onehoon/REFramework`  
Target base: latest `master`  
Master reviewed: `5ef0fe99f10e7214c0edef54f1e3d656c8c50a0e`  
Runtime source baseline included in that master: `6ac6da004e75750f81e467fb79d5dc5a67a1bc43`

## Purpose

Fix a narrow REFramework hook-monitor liveness-accounting bug in the direct-bound XeFG Present/Present1 path.

Current XeFG nested Present callbacks are counted as real Present entries by `D3D12Hook`, but the nested fast path returns without refreshing `REFramework::m_last_present_time`. As a result, the generic hook monitor can repeatedly report a Present timeout even while the tracked XeFG Present hook is still actively receiving calls.

This work must fix only the activity-accounting mismatch. It must **not** weaken or redesign the existing PR-C XeFG stale-binding/quarantine policy.

The observed target configuration is:

```text
Monster Hunter Wilds
NVIDIA RTX GPU
OptiScaler XeFG output
fork REFramework
repeated "Last chance encountered for hooking"
no corresponding generic D3D rehook
```

The OptiScaler PR19 trace from the same investigation shows continuing XeFG Present/Dispatch activity while REFramework periodically enters its hook-monitor timeout path. This work addresses that REFramework-side false liveness timeout. It does **not** claim to fix the separate MHW `E_ABORT (0x80004004)` root cause.

---

## Important correction to the previous crash hypothesis

Do **not** change this existing policy:

```text
sustained XeFG Present starvation
-> classify/quarantine the binding
-> suppress generic hook_d3d12 recovery
-> wait for Present recovery or an exact validated XeFG lifecycle transition
```

That behavior is intentional and was implemented by PR #36 (`XeFG PR-C: harden hook monitor across runtime transitions and stale bindings`).

PR-C explicitly requires that a sustained timeout alone must not trigger:

```text
borrowed-proxy AddRef / QueryInterface / GetDevice / GetHwnd
VtableHook removal from an uncertain proxy
generic D3D12 rehook
forced Release loops
```

The current 3-timeout / 20-second sustained threshold and runtime-transition/detached-state gates are therefore **out of scope** for this fix.

The actual defect is earlier and narrower: the hook-monitor's global activity clock can become stale even though the tracked XeFG callback itself is still entering.

---

## Current source-level behavior

### 1. The generic hook monitor watches `m_last_present_time`

`REFramework::hook_monitor()` treats the D3D Present path as stale when approximately five seconds have elapsed since `m_last_present_time` was refreshed.

It then enters the existing last-chance / XeFG classifier flow:

```text
m_last_present_time older than 5 s
-> "Last chance encountered for hooking"
-> XeFGCompatibility::evaluate_hook_monitor_timeout(...)
-> generic rehook only if action == AllowGenericRecovery
```

This high-level flow is correct and must remain unchanged.

### 2. The XeFG session also tracks Present-entry progress

`D3D12Hook::present_common()` increments:

```cpp
m_present_entry_count
m_last_present_entry_ticks
```

on tracked XeFG Present/Present1 entry.

`XeFGPresentationSession::evaluate_monitor_timeout()` uses that progress to distinguish a changing/live binding from a truly sustained no-progress binding.

If `present_entry_count` changes, the timeout state is reset to Grace. Only repeated samples with the same binding key and the same Present-entry count can become sustained.

That behavior is also correct and must remain unchanged.

### 3. The nested XeFG Present fast path does not refresh the global activity clock

Current `D3D12Hook::present_common()` performs the Present-entry accounting first and later contains this nested path:

```cpp
if (g_present_depth > 0) {
    ++g_present_depth;
    const auto result = original_call();
    --g_present_depth;
    d3d12->m_inside_present = false;
    return result;
}
```

This path deliberately bypasses renderer/mod callbacks, which is correct.

However, it also bypasses both normal ways that REFramework refreshes `m_last_present_time`:

```text
normal renderer path
-> m_on_post_present
-> REFramework::on_post_present_d3d12()
-> m_last_present_time updated

suppressed-render XeFG path
-> REFramework::note_present_activity()
-> m_last_present_time updated
```

Therefore a stream of nested tracked XeFG Presents can produce this inconsistent state:

```text
m_present_entry_count continues increasing
m_last_present_entry_ticks remains fresh
m_last_present_time becomes older than 5 s
```

The hook monitor then emits a false timeout. Because the session sees Present-entry progress, it remains in the safe XeFG grace/preservation path, which explains why the observed logs can repeatedly show `Last chance encountered for hooking` without an actual `Sending rehook request for D3D`.

---

## Required fix

For the tracked direct-bound XeFG nested Present/Present1 path, refresh REFramework's hook-monitor activity clock before returning.

The minimal intended shape is conceptually:

```cpp
if (g_present_depth > 0) {
    ++g_present_depth;
    const auto result = original_call();
    --g_present_depth;

    if (g_framework != nullptr && d3d12->is_xefg_source()) {
        g_framework->note_present_activity();
    }

    d3d12->m_inside_present = false;
    return result;
}
```

Exact placement may be adjusted for local style, but the semantics are mandatory.

`present_common()` already executes under `m_hook_monitor_mutex`; `note_present_activity()` is the existing narrow mechanism used by the XeFG suppressed-render path and does not run renderer/mod callbacks.

Do not introduce a new timing system when the existing helper expresses the desired operation.

---

## Required invariants

These are merge-blocking.

### A1 — tracked nested XeFG Present counts as hook activity

Once a tracked XeFG Present/Present1 callback has entered `present_common()` and reached the nested-forwarding path, that callback must refresh the hook-monitor liveness clock.

The watchdog is measuring hook delivery/liveness, not successful rendering. Therefore the activity timestamp should be refreshed even if the forwarded Present returns a failure HRESULT.

The HRESULT itself must still be returned unchanged.

### A2 — do not run rendering or mod callbacks from the nested path

The fix must not call:

```text
m_on_present
m_on_post_present
on_frame_d3d12
on_post_present_d3d12
renderer reset
ImGui rendering
GPU graphics-memory Commit
mod/plugin Present callbacks
```

The nested path remains a direct original-call forwarding path plus liveness accounting only.

### A3 — do not change PR-C monitor policy

Do not modify:

```text
XeFGHookMonitorState thresholds
kSustainedTimeoutThreshold
kMinimumSustainedPresentAgeMs
MonitorDisposition ordering
XeFGMonitorAction mapping
runtime-transition suppression
detached-uncertain suppression
inconsistent-binding quarantine
sustained-timeout quarantine
```

No timeout-driven COM probe or generic rehook may be added.

### A4 — do not change XeFG binding/lifecycle ownership

Do not modify:

```text
XeFGBinding borrowed swapchain policy
candidate commit/rebind transaction
VtableHook ownership
runtime detach / Destroy reconciliation
resize lifecycle / MHW ResizeHold
Present/Present1 original target selection
```

### A5 — native/non-XeFG behavior remains unchanged

This work is specifically for the direct-bound XeFG `present_common()` path.

Do not alter the generic/native `D3D12Hook::present()` recursion logic as part of this PR. If a similar generic issue is discovered later, handle it separately with its own evidence.

### A6 — do not turn activity accounting into success accounting

Do not condition `note_present_activity()` on `result == S_OK`.

A failing Present still proves that the tracked hook callback is alive and executing. Error handling remains the responsibility of the existing Present result path.

---

## Recommended implementation scope

Expected source change:

```text
src/D3D12Hook.cpp
```

Normally no header or compatibility-session change is necessary.

Do not touch `REFramework.cpp` unless a build/source constraint proves the existing `note_present_activity()` helper is insufficient. The current helper is preferred.

No configuration/UI change is required.

---

## Recommended PR identity

Suggested branch:

```text
fix/xefg-nested-present-monitor-activity
```

Suggested commit title:

```text
fix: count nested XeFG Presents as monitor activity
```

Suggested PR title:

```text
Fix XeFG nested Present hook-monitor activity accounting
```

Base the implementation on the then-latest `master`. Before editing, re-read `D3D12Hook::present_common()` to ensure the nested fast path has not moved since `5ef0fe99`.

---

## Diagnostics policy

A new per-frame log is not required.

Do not add unconditional logging inside the nested path because that can perturb the MHW timing investigation.

If implementation validation needs observability, prefer one of:

1. existing `Debug Log`-gated hook-monitor snapshots, or
2. a temporary local debug counter that is removed before merge.

Production diff should remain minimal.

---

## Source-level acceptance cases

### Case 1 — normal non-nested XeFG Present

```text
g_present_depth == 0
-> existing render/suppression policy
-> existing original Present
-> existing post-present or note_present_activity path
```

Expected: unchanged.

### Case 2 — nested tracked XeFG Present

```text
g_present_depth > 0
-> forward to original_call()
-> refresh hook-monitor activity with note_present_activity()
-> return exact original HRESULT
```

Expected: no renderer/mod callbacks, no lifecycle mutation.

### Case 3 — nested tracked XeFG Present returning failure

Expected:

```text
activity clock refreshed
failure HRESULT propagated unchanged
no recovery action injected here
```

### Case 4 — real XeFG Present starvation

No Present callback enters at all.

Expected: existing hook-monitor timer expires and existing PR-C classifier behaves exactly as before.

### Case 5 — runtime transition / detached uncertain state

Expected: existing PR-C suppression remains unchanged.

### Case 6 — native D3D12 path

Expected: no source or behavior change.

---

## Runtime validation

Primary A/B test:

```text
Monster Hunter Wilds
NVIDIA RTX GPU
OptiScaler XeFG output
fork REFramework
```

Use a scenario that previously produced repeated:

```text
Last chance encountered for hooking
```

while OptiScaler/XeFG continued presenting.

### Expected after the fix

During continuous tracked XeFG Present activity:

```text
no periodic false "Last chance encountered for hooking" caused solely by nested Present traffic
```

If Present traffic genuinely stops for more than the existing watchdog interval, the monitor must still activate normally.

Also verify:

- no new generic `Sending rehook request for D3D` during healthy XeFG operation,
- REFramework overlay behavior is unchanged,
- XeFG remains active,
- no new resize/fullscreen/Alt+Tab regression.

Recommended control titles when available:

```text
DD2 + OptiScaler XeFG
RE9/PRAGMATA + OptiScaler XeFG
native REFramework without OptiScaler
```

Do not claim the MHW E_ABORT crash is fixed unless separate runtime evidence proves it. The OptiScaler PR19 investigation remains the primary E_ABORT root-cause track.

---

## Static/build validation

Required:

```text
git diff --check
cmake configure for x64 Release
build REFramework Release target
python dev/audit_direct_access_clang.py
```

Final source audit must confirm:

- `XeFGPresentationSession` monitor classifier is unchanged,
- timeout thresholds are unchanged,
- no new COM operation exists in the timeout path,
- no new hook removal/rehook exists in the timeout path,
- nested Present returns the original HRESULT unchanged,
- only the intended XeFG activity timestamp update was added.

---

## Merge decision

This fix may be merged independently of the OptiScaler PR19 E_ABORT diagnosis because it corrects an internally inconsistent REFramework watchdog signal:

```text
tracked XeFG Present entry says "alive"
while
m_last_present_time says "inactive"
```

The desired invariant after this work is:

> Every tracked direct-bound XeFG Present/Present1 callback that actually enters REFramework keeps the hook-monitor liveness clock current, including the nested recursion fast path, while all PR-C stale-binding safety rules remain intact.
