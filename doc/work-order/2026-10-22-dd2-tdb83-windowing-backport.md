# Work Order: Backport DD2 TDB83 Windowing Fix

**Priority:** P1  
**Repository:** `onehoon/REFramework`  
**Target branch:** `master`  
**Upstream:** `praydog/REFramework` `master`  
**Source commit:** [d4611aef1d63effd3a736bdf9404ea702428017d](https://github.com/praydog/REFramework/commit/d4611aef1d63effd3a736bdf9404ea702428017d)

## Goal
Bring the upstream Dragon's Dogma 2 (DD2) ultrawide/window-mode compatibility fix into our fork's `master` with the smallest possible change. The fix updates DD2 TDB83 window structure dispatch and guards borderless window-size writes.

## Implementation instructions
1. Start a dedicated work branch from the current `onehoon/REFramework master`. Do not use `feature/*`, `pd-upscaler`, or RE4 XeSS experimental branches.
2. Fetch `praydog/REFramework master`. Check whether the source commit or an equivalent fix has already been incorporated.
3. Cherry-pick the complete original commit, preserving attribution:
   ```bash
   git cherry-pick -x d4611aef1d63effd3a736bdf9404ea702428017d
   ```
4. Review the resulting diff, including `shared/sdk/ViaDispatch.hpp`, `src/mods/Graphics.cpp`, the DD2 window structure, and generated TDB83 headers. Ensure DD2 selects the correct layout and guarded write behavior while other game dispatch paths remain unchanged.
5. If the cherry-pick conflicts, investigate the current master code and resolve only the necessary differences. Do not force a patch or replace fork-specific changes. If no safe resolution is evident, stop and report the conflict.
6. Do not add unrelated refactors, broaden support scope, or split this small logical backport into multiple PRs.

## Validation
- Build the relevant Windows x64 Release configuration; check generated headers and dispatch types compile.
- Where DD2 is available, test Windowed, Borderless, and Fullscreen transitions, including 21:9 and 32:9, with the ultrawide fix enabled and disabled.
- Smoke-test window sizing/graphics initialization in other supported RE Engine titles, where available.
- Check rendering, resize, shutdown, and existing frame-generation paths for regressions.
- Document any unavailable game/hardware tests as **Not tested** rather than claiming they passed.

## Deliverable
- A single focused PR targeting `master`, with the upstream SHA, cherry-pick provenance, concise changed-file summary, build results, and runtime test status.
- **Do not merge, release, or deploy.** No automatic PR creation is requested by this work order.
