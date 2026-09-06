# P3.3B.1 Work Order — Scope XeFG ResizeTarget Hold to Monster Hunter Wilds

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch: `master`  
Planning baseline: `aa3a53e516b882a77d399c929efa0ef29d1426b0`  
Baseline commit: `refactor: isolate XeFG loader and probe handoff (#18)`  
Refactor state: R1 and R2 merged; R3 has not been included in this work order  
Suggested implementation branch: `fix/xefg-p3-3b-1-mhw-only-resize-hold`  
Suggested PR title: `P3.3B.1: scope XeFG ResizeTarget hold to MHW`

---

## 1. Purpose

This is a small runtime-regression correction to P3.3B, not a new XeFG architecture PR and not part of the R3+ refactor sequence.

P3.3B / PR #16 introduced a XeFG resize-transition render hold to protect Monster Hunter Wilds from an observed Alt+Enter failure. The original implementation arms that hold for every tracked XeFG `ResizeTarget` event.

New PRAGMATA runtime evidence proves that this all-games arm policy is too broad.

The required correction is:

> Keep the existing P3.3B hold mechanism, completion rules, Present forwarding, reset ordering, and cleanup behavior unchanged, but arm the hold only when the running game is Monster Hunter Wilds (`MHWILDS`).

All other games must continue to receive the normal existing REFramework `ResizeTarget` reset behavior, but must not enter the P3.3B renderer hold merely because a tracked XeFG `ResizeTarget` occurred.

This is a positive allow-list based on reproduced failure evidence.

Do **not** implement a PRAGMATA blacklist.

---

## 2. Why This Must Be Fixed Before Continuing the Refactor

The current fine-grained XeFG refactor has already merged:

```text
R1 / PR #17
    Extract XeFG exact-HMODULE runtime registry

R2 / PR #18
    Isolate XeFG loader / probe handoff
```

Current `master` for this work order is:

```text
aa3a53e516b882a77d399c929efa0ef29d1426b0
refactor: isolate XeFG loader and probe handoff (#18)
```

The future refactor sequence plans to move resize lifecycle/hold state later, especially in R8 and R9.

Do not carry a known-bad all-games policy through R3-R9 and then fix it after migration.

Instead:

```text
R1 merged
R2 merged
    ↓
P3.3B.1 runtime scope correction   <-- this work order
    ↓
R3
R4
...
R8: migrate the corrected MHW-only hold semantics
R9: reduce callback policy surface without changing that corrected behavior
```

This preserves the intended behavior-preserving nature of the later extraction PRs.

### Important documentation constraint

Do **not** modify either existing refactor planning document as part of this PR:

```text
doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_ARCHITECTURE_2026-09-06.md
doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_PR_SPLIT_PLAN_2026-09-06.md
```

The new runtime evidence in this work order supersedes only the old assumption that the P3.3B hold should arm on every tracked XeFG `ResizeTarget`.

Do not opportunistically rewrite the refactor documentation in this hotfix.

---

## 3. Existing P3.3B Behavior That Must Be Preserved

P3.3B was introduced to protect a specific transition observed in Monster Hunter Wilds.

Current high-level behavior is:

```text
tracked XeFG ResizeTarget
    -> existing REFramework renderer reset/release
    -> arm XeFG resize-transition hold
    -> forward original ResizeTarget

while hold active:
    -> original Present / Present1 still forwarded
    -> REFramework render callback suppressed
    -> REFramework post-render callback suppressed
    -> real Present activity continues to maintain hook-monitor liveness

successful tracked ResizeBuffers or ResizeBuffers1
    -> complete hold
    -> subsequent Present may rebuild/render normally

failed ResizeTarget
    -> clear hold

binding replacement / external bind / unhook
    -> clear stale hold state
```

The hotfix must **not** redesign this state machine.

Only the **arm eligibility policy** changes.

---

## 4. Root Runtime Evidence — Monster Hunter Wilds

Monster Hunter Wilds is currently the only game in this project with a reproduced Alt+Enter crash that justifies the P3.3B hold.

The observed failing lifecycle before P3.3B was:

```text
tracked XeFG internal ResizeTarget
    -> REFramework releases renderer/backbuffer ownership
    -> original ResizeTarget returns successfully
    -> intermediate Present1 arrives almost immediately
    -> REFramework renderer initializes again
    -> REFramework reacquires internal presentation backbuffers
    -> outer OptiScaler / Streamline / XeFG ResizeBuffers is still progressing
    -> XeFG reports outstanding backbuffer references
    -> outer resize returns E_PENDING (0x8000000A)
    -> game later fails/crashes
```

The critical observation was that REFramework reacquired the same internal presentation resources during the wider XeFG transition, before the outer resize completed.

P3.3B was therefore intentionally designed to suppress REFramework renderer callbacks across that transition while still forwarding real Present calls.

For MHWILDS, that behavior remains required.

This PR must **not** weaken the MHWILDS hold by adding:

- a timeout;
- a Present-count escape;
- a sleep;
- a GPU wait;
- forced `Release()` loops;
- early renderer reacquisition;
- public XeFG proxy binding.

---

## 5. New Runtime Evidence — PRAGMATA

PRAGMATA demonstrates that a tracked XeFG `ResizeTarget` does **not** universally mean a buffer-resize transaction is in progress.

Two independent PRAGMATA sessions reproduced the same regression with different OptiScaler versions while using the same REFramework build.

### 5.1 Session 09

Observed sequence:

```text
15:50:16.295
    tracked XeFG ResizeTarget, event_id = 8
    renderer reset completed
    [XeFG][ResizeHold] action = arm

15:50:16.300
    first Present1 after ResizeTarget
    [XeFG][ResizeHold] action = suppress_present

15:50:16.304
    second suppressed Present1

15:50:16.308
    third suppressed Present1

...

15:51:06.627
    tracked ResizeBuffers1 returns S_OK
    [XeFG][ResizeHold] action = complete
    suppressed_presents = 6368
```

The hold remained active for roughly 50 seconds and suppressed 6,368 REFramework render opportunities.

### 5.2 Session 10

Observed sequence:

```text
16:00:26.873
    tracked XeFG ResizeTarget, event_id = 9
    renderer reset completed
    [XeFG][ResizeHold] action = arm

16:00:26.878
    first suppressed Present1

16:00:26.882
    second suppressed Present1

16:00:26.886
    third suppressed Present1

...

16:01:57.384
    tracked ResizeBuffers1 returns S_OK
    [XeFG][ResizeHold] action = complete
    suppressed_presents = 15458
```

The hold remained active for roughly 90 seconds and suppressed 15,458 REFramework render opportunities.

### 5.3 User-visible behavior

The user observed:

```text
Game launch:
    REFramework menu may initially auto-open once and is visible.

Later:
    Insert is pressed.
    OptiScaler overlay still opens.
    REFramework overlay does not appear.

Then:
    in-game upscaler is changed to another mode
    then changed back
    a real resize/reinitialization path occurs
    ResizeBuffers1 completes
    P3.3B hold clears
    REFramework overlay works normally afterward
```

This behavior is directly explained by the existing hold implementation.

The issue is not currently evidence of:

- an Insert input failure;
- an ImGui visibility flag failure;
- a dead Present hook;
- an OptiScaler overlay conflict;
- a generic D3D12 hook-monitor failure.

The regression is that REFramework itself suppresses its renderer callbacks for an extended period after a legitimate standalone PRAGMATA `ResizeTarget`.

### 5.4 OptiScaler version difference is not causal

The two PRAGMATA sessions used different OptiScaler versions but reproduced the same P3.3B hold behavior.

Do not add OptiScaler-version branching.

---

## 6. Corrected Policy

The corrected evidence-based policy is:

```text
MHWILDS + validated XeFG internal binding + eligible tracked ResizeTarget
    -> normal existing REFramework reset
    -> arm P3.3B resize-transition hold
    -> existing P3.3B behavior continues unchanged

Any other game + validated XeFG internal binding + tracked ResizeTarget
    -> normal existing REFramework reset
    -> DO NOT arm P3.3B resize-transition hold
    -> forward original ResizeTarget normally
    -> subsequent Present may rebuild/render using existing behavior
```

### 6.1 Positive allow-list, not negative exceptions

Required:

```text
MHWILDS -> hold enabled
everything else -> hold disabled
```

Rejected:

```text
PRAGMATA -> hold disabled
all other games -> hold enabled
```

The second form merely hides the currently known regression and would reproduce it in the next game that legitimately issues standalone `ResizeTarget` calls.

### 6.2 Unknown games

`GameID::Unknown` must not arm the hold.

No speculative fallback is required.

If another game later reproduces the same MHW-specific Alt+Enter outstanding-reference failure, add that game only after obtaining equivalent evidence.

---

## 7. Use the Existing Canonical Game Identity

Latest master already has canonical runtime game identity in:

```text
shared/sdk/GameIdentity.hpp
```

It defines:

```cpp
bool is_mhwilds() const { return m_game == GameID::MHWILDS; }
```

Use:

```cpp
sdk::GameIdentity::get().is_mhwilds()
```

Do **not** add another game-detection mechanism.

Specifically do not:

- inspect `GetModuleFileName` again;
- compare process names manually;
- compare `game_name()` strings when `is_mhwilds()` already exists;
- add an environment variable;
- add a config toggle for this hotfix;
- infer the game from OptiScaler logs/modules;
- infer MHW from GPU/vendor behavior.

If the translation unit requires a direct include for the canonical identity API, add the narrow existing SDK include consistent with repository conventions. Do not change `GameIdentity` itself.

---

## 8. Current Source Seam on R2 Master

At planning baseline `aa3a53e516b882a77d399c929efa0ef29d1426b0`, the relevant implementation remains in:

```text
src/D3D12Hook.cpp
```

The current ResizeTarget arm site is effectively:

```cpp
if (event_id != 0 && renderer_reset_performed && !d3d12->m_xefg_p21_observe_only) {
    d3d12->arm_xefg_resize_transition_hold(event_id);
}
```

The existing helper also validates the XeFG source/observe-only/event state internally.

R1 and R2 have not moved this resize lifecycle policy yet.

This makes the desired hotfix intentionally small.

---

## 9. Required Implementation

### 9.1 Preferred minimal implementation

Prefer changing only the arm predicate at the current ResizeTarget call site.

Conceptually:

```cpp
if (event_id != 0
    && renderer_reset_performed
    && !d3d12->m_xefg_p21_observe_only
    && sdk::GameIdentity::get().is_mhwilds()) {
    d3d12->arm_xefg_resize_transition_hold(event_id);
}
```

Equivalent form if readability is improved:

```cpp
const bool should_arm_xefg_resize_hold =
    event_id != 0
    && renderer_reset_performed
    && !d3d12->m_xefg_p21_observe_only
    && sdk::GameIdentity::get().is_mhwilds();

if (should_arm_xefg_resize_hold) {
    d3d12->arm_xefg_resize_transition_hold(event_id);
}
```

Use the smaller form unless repository style or compiler diagnostics justify the local variable.

### 9.2 Do not move the game check into unrelated layers

For this hotfix, do not move MHW policy into:

- `XeFGRuntimeRegistry`;
- `XeFGCompatibility` loader/probe handoff;
- InitDesc discovery;
- queue/candidate validation;
- hook monitor.

Those layers do not own ResizeTarget render-hold policy.

R8/R9 will later relocate lifecycle policy as part of the planned refactor.

### 9.3 Do not change normal reset behavior

This point is critical.

The change must **not** become:

```cpp
if (is_mhwilds()) {
    // perform reset and hold
}
```

The existing renderer reset triggered by the tracked XeFG ResizeTarget remains applicable independent of whether the MHW-only hold arms.

Correct conceptual order:

```text
tracked XeFG ResizeTarget
    -> existing lifecycle event accounting
    -> existing REFramework renderer reset where current code requires it
    -> snapshot/logging behavior unchanged
    -> if MHWILDS and current P3.3B eligibility passes:
           arm hold
       else:
           no hold
    -> call original ResizeTarget
```

Do not skip the normal reset for PRAGMATA, DD2, RE9, or other games merely because the hold is disabled.

---

## 10. Behavior That Must Remain Unchanged When Hold Is Active

For MHWILDS, preserve all current P3.3B semantics.

### Present / Present1

While hold is active:

```text
original Present / Present1 -> MUST still be called
REFramework render callback -> suppressed
REFramework post-render callback -> suppressed
real Present activity -> continues to update hook-monitor liveness using the existing path
```

Do not fake Present activity when no Present occurred.

### Resize completion

Successful tracked:

```text
ResizeBuffers
or
ResizeBuffers1
```

continues to complete the hold according to the existing implementation.

Failed buffer resize must retain the existing policy.

### Failure/teardown cleanup

Keep current stale-hold cleanup on:

- failed `ResizeTarget` where already implemented;
- initial/external binding changes where already implemented;
- same-object binding update where already implemented;
- changed-object binding replacement where already implemented;
- unhook where already implemented.

Do not broaden cleanup semantics in this PR.

---

## 11. Files Expected to Change

Preferred functional scope:

```text
src/D3D12Hook.cpp
```

Potentially a direct include may be added if needed for canonical `GameIdentity` access.

Expected functional change size:

```text
~5-30 LOC
```

This should remain a one-purpose patch.

Do not change:

```text
doc/refactor/*
src/compatibility/xefg/XeFGRuntimeRegistry.*
src/compatibility/xefg/XeFGCompatibility.*
```

unless compilation exposes an unavoidable direct dependency issue. If that occurs, prefer the smallest include/declaration fix and document it in the PR body.

---

## 12. Explicit Non-Goals

This PR does **not**:

- redesign the P3.3B hold state machine;
- create a generic game-policy framework;
- add a generic frame-generation provider abstraction;
- modify R1 exact-HMODULE runtime registry behavior;
- modify R2 loader/probe façade behavior;
- implement R3 discovery extraction;
- implement R4 candidate validation extraction;
- implement R5-R10 refactor work;
- change presentation queue selection;
- change public-vs-internal XeFG swapchain authority;
- change Present/Present1 hook slots;
- change ResizeBuffers/ResizeTarget/ResizeBuffers1 hook slots;
- change P3.2 transactional rebind ordering;
- change P3.3R hook-monitor preservation;
- change logging levels or add the Debug Logging UI;
- solve unrelated Alt+Tab/minimize issues;
- change OptiScaler;
- depend on OptiScaler private implementation details;
- depend on Intel private object layouts.

---

## 13. Forbidden Implementations

Do not implement any of the following.

### 13.1 PRAGMATA blacklist

```cpp
if (!sdk::GameIdentity::get().is_pragmata()) {
    arm_hold();
}
```

Rejected because it leaves the broad unsupported assumption intact.

### 13.2 Timer-based hold escape

```cpp
if (hold_age > 500ms) {
    clear_hold();
}
```

Rejected because MHW's wider resize transition is asynchronous enough that an arbitrary timeout can reopen the original race.

### 13.3 Present-count escape

```cpp
if (suppressed_presents >= 3) {
    clear_hold();
}
```

Rejected for the same reason.

### 13.4 Manual COM reference forcing

Do not add loops such as:

```cpp
while (resource->Release() > expected) {
}
```

REFramework must release only references it owns through normal renderer reset/lifetime management.

### 13.5 Global game special casing outside the hold

Do not make MHW alter:

- XeFG discovery;
- queue selection;
- active binding acceptance;
- rebind identity;
- hook-monitor timeout policy.

The special case is limited to P3.3B ResizeTarget hold eligibility.

### 13.6 New game-name parsing

Do not duplicate `GameIdentity`.

### 13.7 Folding this into R3

Do not combine this behavior fix with InitDesc/factory-capture extraction.

A runtime regression correction should remain independently reviewable and independently revertible.

---

## 14. Expected Runtime Behavior After the Fix

### 14.1 PRAGMATA

Expected:

```text
tracked XeFG ResizeTarget
    -> existing REFramework renderer reset
    -> NO [XeFG][ResizeHold] action = arm for that event
    -> original ResizeTarget called
    -> subsequent Present/Present1 not suppressed by P3.3B
    -> renderer may initialize/reacquire through existing normal path
    -> Insert can display REFramework overlay without an upscaler toggle
```

There must no longer be a PRAGMATA sequence like:

```text
ResizeTarget
-> ResizeHold arm
-> suppress_present x 6368
```

or:

```text
ResizeTarget
-> ResizeHold arm
-> suppress_present x 15458
```

### 14.2 Monster Hunter Wilds

Expected existing P3.3B sequence remains:

```text
Alt+Enter / relevant mode transition
    -> tracked XeFG ResizeTarget
    -> existing REFramework reset
    -> ResizeHold arm
    -> intermediate Present/Present1 forwarded but REF renderer suppressed
    -> successful tracked ResizeBuffers or ResizeBuffers1
    -> ResizeHold complete
    -> next eligible Present rebuilds renderer
    -> no XeFG outstanding-reference E_PENDING attributable to early REF reacquisition
```

### 14.3 DD2 and other non-MHW games

Expected:

- no new MHW hold behavior;
- existing XeFG binding and rendering behavior remains unchanged;
- both overlays continue to work;
- no effect on queue authority or binding generation.

### 14.4 Unknown game identity

Expected:

- no hold arm;
- normal existing non-MHW XeFG reset/present behavior.

---

## 15. Logging Expectations

This PR should not introduce high-volume new logging.

Existing MHW hold logs are sufficient to confirm that the hold remains active there:

```text
[XeFG][ResizeHold] action = arm
[XeFG][ResizeHold] action = suppress_present
[XeFG][ResizeHold] action = complete
```

For PRAGMATA, the important evidence is the **absence** of `action = arm` following the standalone ResizeTarget while normal resize lifecycle logs continue.

Do not add a permanent per-ResizeTarget message such as:

```text
hold skipped because not MHW
```

unless implementation debugging proves it necessary. The project already plans a separate later logging cleanup; do not add noise now.

If a temporary diagnostic is used locally, remove it before PR submission unless it is concise and materially useful for future support.

---

## 16. Static / Build Validation

Before opening the PR:

1. Confirm implementation base is current `master` containing R1 + R2.
2. Build REFramework Release using the repository's current supported build path.
3. Run the existing direct-access/static audit if it remains part of the repository workflow.
4. Run:

```text
git diff --check
```

5. Inspect the final diff manually.
6. Confirm no `doc/refactor/*` file changed.
7. Confirm no R3+ extraction code was introduced.
8. Confirm no OptiScaler/private Intel dependency was introduced.
9. Confirm native/non-XeFG paths were not modified.

Expected source diff should be trivially attributable to the MHW-only arm predicate.

---

## 17. Runtime Acceptance Matrix

### A. PRAGMATA — required regression validation

Reproduce the same environment that previously showed the missing REF overlay.

Check:

```text
[ ] Game launches with XeFG active.
[ ] OptiScaler overlay works.
[ ] REFramework initial renderer initializes.
[ ] Standalone tracked ResizeTarget may occur.
[ ] Existing renderer reset occurs as before.
[ ] No ResizeHold arm occurs for PRAGMATA.
[ ] No long suppress_present sequence follows that ResizeTarget.
[ ] Insert opens REFramework overlay.
[ ] Upscaler away/back toggle is no longer required to recover REF overlay.
[ ] No new crash/device-removed error appears.
```

PASS condition:

```text
PRAGMATA remains functional and REF overlay can be reopened after the standalone ResizeTarget without requiring a later ResizeBuffers1 to clear a hold.
```

### B. Monster Hunter Wilds — required safety validation

Use the same Alt+Enter path that previously produced E_PENDING/crash.

Check:

```text
[ ] tracked XeFG ResizeTarget occurs.
[ ] existing renderer reset occurs before hold arm.
[ ] ResizeHold action = arm still occurs.
[ ] intermediate Present/Present1 is forwarded.
[ ] REFramework renderer callbacks remain suppressed during the hold.
[ ] successful tracked ResizeBuffers/ResizeBuffers1 completes the hold.
[ ] renderer returns after completion.
[ ] no XeFG "Back buffers have outstanding references" failure attributable to REF reacquisition.
[ ] no outer ResizeBuffers E_PENDING 0x8000000A regression.
[ ] game survives Alt+Enter.
```

If MHW stops arming the hold, this PR fails even if PRAGMATA is fixed.

### C. DD2 — recommended smoke

Check:

```text
[ ] XeFG active.
[ ] OptiScaler overlay visible.
[ ] REFramework overlay visible.
[ ] normal Present1 flow continues.
[ ] no unexpected ResizeHold arm solely because DD2 is using XeFG.
[ ] basic Alt+Tab smoke does not regress.
```

### D. Native REFramework path — static/review assurance

Because the functional change is inside the already-XeFG-specific ResizeTarget hold path, a full unrelated native matrix is not required solely for this tiny hotfix unless CI/refactor state demands it.

However, code review must verify no normal D3D12/native branch was restructured.

---

## 18. Failure Interpretation

### PRAGMATA still shows `ResizeHold arm`

Likely causes:

- game identity predicate not applied at the actual arm site;
- another arm path exists;
- implementation accidentally used inverted logic.

Do not add a timer as compensation. Find the actual arm source.

### PRAGMATA has no hold but overlay still cannot open

Then P3.3B was one proven cause but another overlay lifecycle issue remains.

Capture a new bounded log around:

```text
Insert state
Present callback entry
renderer initialized state
ImGui frame/render state
```

Do not restore generic hold behavior merely because a second issue exists.

### MHW no longer crashes but hold never completes

Do not add a timeout in this PR.

That is separate lifecycle evidence and requires its own investigation.

### MHW E_PENDING returns despite correct MHW-only hold

Compare the new timeline to the original proven P3.3A sequence:

- whether hold armed before the intermediate Present;
- whether REF renderer callback was truly suppressed;
- whether REF backbuffers remained released;
- whether the failing outer resize is the same lifecycle.

Do not expand this scope to public proxy hooking without evidence.

---

## 19. Interaction With the Ongoing Refactor

This hotfix changes the runtime contract that later refactor PRs must preserve.

After this PR merges, R8/R9 must treat the corrected behavior as authoritative:

```text
ResizeTarget hold state exists as a XeFG lifecycle mechanism,
but its current evidence-backed activation policy is MHWILDS only.
```

R8 may move the state into the planned XeFG lifecycle/binding component.

R9 may reduce direct policy handling inside physical callbacks.

Neither should silently broaden the activation back to all games.

Do not implement those refactor steps in this PR.

---

## 20. Review Checklist

A reviewer should reject the PR if any of these are true:

```text
[ ] PRAGMATA-specific blacklist is used instead of MHW positive allow-list.
[ ] Existing normal ResizeTarget renderer reset is disabled for non-MHW games.
[ ] MHW hold semantics are weakened by timeout/count recovery.
[ ] MHW no longer arms the hold on the previously eligible tracked XeFG ResizeTarget path.
[ ] R1 runtime registry is modified without necessity.
[ ] R2 loader/probe handoff is modified without necessity.
[ ] R3+ refactor work is mixed in.
[ ] hook-monitor policy is changed.
[ ] presentation queue selection is changed.
[ ] public XeFG proxy becomes render authority.
[ ] custom game-name/process detection is added despite GameIdentity.
[ ] doc/refactor files are edited.
[ ] logging noise is significantly increased.
```

A reviewer should expect the functional change to be small enough to reason about directly.

---

## 21. Suggested Implementation Diff Shape

The desired semantic diff is approximately:

```diff
- if (event_id != 0 && renderer_reset_performed && !d3d12->m_xefg_p21_observe_only) {
+ if (event_id != 0
+     && renderer_reset_performed
+     && !d3d12->m_xefg_p21_observe_only
+     && sdk::GameIdentity::get().is_mhwilds()) {
      d3d12->arm_xefg_resize_transition_hold(event_id);
  }
```

This is an example, not a command to ignore current source context.

Before editing, re-read the latest source and adapt to any harmless formatting/include changes that occurred after this planning baseline.

Do not refactor surrounding code merely to make this diff look cleaner.

---

## 22. Suggested PR Body

```markdown
## Summary

- restrict the P3.3B XeFG `ResizeTarget` renderer hold to canonical `MHWILDS` game identity
- preserve the existing tracked XeFG `ResizeTarget` renderer reset for all games
- preserve all existing hold behavior when active: Present/Present1 forwarding, REF renderer suppression, successful ResizeBuffers/ResizeBuffers1 completion, and stale-state cleanup

## Runtime evidence

P3.3B was introduced for the reproduced Monster Hunter Wilds Alt+Enter path where REF could reacquire XeFG internal backbuffers during a wider resize transition and contribute to `E_PENDING` / outstanding-reference failure.

PRAGMATA provides a counterexample to the previous all-games arm policy: a legitimate standalone tracked `ResizeTarget` can occur without a prompt completing `ResizeBuffers/ResizeBuffers1`. In two sessions the current hold suppressed 6,368 and 15,458 Presents respectively, preventing the REFramework overlay from rendering until an upscaler change later triggered `ResizeBuffers1` and cleared the hold.

## Scope

- positive allow-list: MHWILDS only
- no PRAGMATA blacklist
- no timeout / Present-count fallback
- no R3+ refactor work
- no R1 registry or R2 loader/probe behavior changes
- no hook-monitor or binding/rebind policy changes
- no OptiScaler/private Intel dependency
- no edits to `doc/refactor/*`

## Validation

- Release build
- repository static/direct-access audit where applicable
- `git diff --check`
- runtime: PRAGMATA overlay recovery path
- runtime: MHW Alt+Enter hold/crash protection
- recommended DD2 XeFG smoke
```

---

## 23. Completion Criteria

The implementation is complete only when all code-review criteria are met and available runtime evidence supports both sides of the correction:

```text
PRAGMATA:
    standalone ResizeTarget does not arm the P3.3B hold
    REF overlay no longer depends on a later upscaler-toggle resize to recover

MHWILDS:
    the proven Alt+Enter ResizeTarget path still arms the hold
    intermediate REF renderer reacquisition remains blocked
    hold completes through the existing successful tracked ResizeBuffers/ResizeBuffers1 path
    original E_PENDING/outstanding-reference crash does not regress
```

The central rule for this PR is:

> Fix the scope of the proven workaround, not the workaround mechanism itself.

Keep the patch small, game-identity-driven, evidence-based, and independent from the ongoing R3+ XeFG compatibility refactor.
