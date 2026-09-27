# RE4 XeSS Production — Load-State Accessor Diagnostic Work Order

Base branch: `feature/re4-xess`  
Production code baseline: `378df3aed5da28fed1c4c4366e381bfff12277f9`  
PR target: `feature/re4-xess`  
Suggested implementation branch: `feature/re4-xess-load-state-accessor-diagnostic`  
Date: 2026-09-27  
Scope: identify and minimally correct the production Load Save accessor that currently blocks every XeSS frame after successful worker/runtime initialization  
Do not merge without review.

Primary architecture:

- `doc/RE4_XESS_PRODUCTION_ARCHITECTURE_2026-09-27.md`

Canonical reverse-engineering evidence:

- `doc/RE4_XESS_BRIDGE_ARCHITECTURE_AND_RE_STATUS_2026-09-26.md`

Runtime evidence:

- `GoogleDrive/ETS2ATS/RE4/PR4-Log/re2_framework_log.txt`
- `GoogleDrive/ETS2ATS/RE4/PR4-Log/OptiScaler.log`

---

## 1. Objective

The PR65 worker/runtime correction succeeded far enough to prove the standard XeSS producer path reaches stock OptiScaler.

The current blocker is now the production RE4 Load Save observation accessor.

The next PR must answer one narrow question:

~~~text
Why does read_game_load_snapshot() return std::nullopt on every frame
even though the Capture 22 types are present in the running game?
~~~

Do not widen this PR into new temporal heuristics, XeSS execution changes, output-handoff changes, or OptiScaler changes.

---

## 2. New runtime facts from PR4-Log

### 2.1 Runtime discovery and worker ownership are fixed

Observed:

~~~text
[RE4XeSS][Worker] started thread=29452

candidate 1 missing:
    <RE4>\libxess.dll

candidate 2 selected:
    <RE4>\OptiScaler\libxess.dll

LoadLibraryExW succeeded
~~~

Pre-Overlay callbacks moved across many RE Engine threads, while the dedicated worker remained thread 29452.

No callback-thread migration quarantine occurred.

### 2.2 Stock OptiScaler is loaded and intercepting the public XeSS producer calls

OptiScaler logged:

~~~text
OptiScaler v10.0.0-dev loaded
CheckWorkingMode OptiScaler working as dxgi.dll
XeSSProxy::InitXeSS LoadResult: true

hk_xessGetVersion
hk_xessD3D12CreateContext
hk_xessGetOptimalInputResolution
hk_xessD3D12Init
hk_xessSetVelocityScale
~~~

The first Ultra Quality configuration matched exactly:

~~~text
display       = 2560x1440
optimal input = 1969x1107
velocityScale = 984.5, -553.5
~~~

Therefore do not modify OptiScaler integration in this PR.

### 2.3 No actual XeSS dispatch occurred

Counts from the capture:

~~~text
OptiScaler hk_xessGetVersion          = 18
OptiScaler hk_xessD3D12CreateContext  = 18
OptiScaler hk_xessD3D12Init           = 18
OptiScaler hk_xessSetVelocityScale    = 18

RE4XeSS Worker submit                 = 0
RE4XeSS Output handoff                = 0
RE4XeSS first resetHistory packet     = 0
OptiScaler hk_xessD3D12Execute        = 0
~~~

The repeated context creation/destruction corresponds to the user changing XeSS quality modes during the test and is not the current blocker.

### 2.4 Load-state observation failed continuously

Observed over the same session:

~~~text
load-state-observation-unavailable = 18,743
load-history-invalid               = 18,726
~~~

No valid production load snapshot was observed.

This blocks `on_scene_layer_update()` here:

~~~cpp
if (m_inhibit_departure_pending ||
    m_load_transition_active ||
    m_startup_mid_load ||
    !m_load_observation_valid) {
    invalidate_history("load-history-invalid");
    return;
}
~~~

and therefore prevents frame packet creation, worker submit, XeSS Execute, and PR4 output handoff.

---

## 3. Capture 22 contract remains valid

Do not discard the existing Load Save design.

Capture 22 proved the semantic production window:

~~~text
SceneLoadZoneManager._Pause false -> true
    => arm history invalid

_Pause true -> false
    => history remains invalid

GameSituationManager.InhibitBit returns to remembered
pre-load normal value
    => first valid gameplay frame uses resetHistory = true
    => temporal accumulation resumes
~~~

Production requirements remain:

- remember the normal `InhibitBit` dynamically;
- do not hardcode `0xB9`;
- keep history invalid across the full pause/inhibit transition;
- do not replace this with camera-angle thresholds;
- do not treat Pause false alone as load completion.

The current issue is accessor implementation, not the state-machine semantics.

---

## 4. Current accessor under investigation

Current code resolves:

~~~cpp
pause_type =
    sdk::find_type_definition("chainsaw.SceneLoadZoneManager");

pause_instance_getter =
    pause_type->get_method("get_Instance");

pause_field =
    pause_type->get_field("_Pause");

situation_type =
    sdk::find_type_definition("chainsaw.GameSituationManager");

situation_instance_getter =
    situation_type->get_method("get_Instance");

inhibit_field =
    situation_type->get_field("InhibitBit");
~~~

Then every frame:

~~~cpp
auto* pause_manager =
    pause_instance_getter->call_safe<REManagedObject*>(
        sdk::get_thread_context());

auto* situation_manager =
    situation_instance_getter->call_safe<REManagedObject*>(
        sdk::get_thread_context());

const auto pause =
    read_bool_field(pause_field, pause_manager);

const auto inhibit =
    read_integral_field(inhibit_field, situation_manager);
~~~

Every failure currently collapses to:

~~~cpp
return std::nullopt;
~~~

That is insufficient to identify the contradiction.

---

## 5. Add a structured diagnostic result

Replace the opaque internal `std::optional<GameLoadSnapshot>` diagnostic path with enough internal state to distinguish failures.

Suggested model:

~~~cpp
enum class LoadSnapshotFailure {
    None,

    PauseTypeUnavailable,
    PauseGetterUnavailable,
    PauseFieldUnavailable,

    SituationTypeUnavailable,
    SituationGetterUnavailable,
    InhibitFieldUnavailable,

    ThreadContextUnavailable,

    PauseInstanceUnavailable,
    SituationInstanceUnavailable,

    PauseFieldTypeUnavailable,
    PauseFieldTypeMismatch,
    PauseFieldAddressUnavailable,
    PauseFieldValueInvalid,

    InhibitFieldTypeUnavailable,
    InhibitStorageTypeUnavailable,
    InhibitStorageTypeMismatch,
    InhibitStorageWidthInvalid,
    InhibitFieldAddressUnavailable,
};

struct GameLoadSnapshotResult {
    std::optional<GameLoadSnapshot> snapshot;
    LoadSnapshotFailure failure{LoadSnapshotFailure::None};
};
~~~

Exact names may differ.

Do not expose this as a public API.

---

## 6. Instrument each accessor stage

The diagnostic must separately prove all of the following.

### Type resolution

~~~text
chainsaw.SceneLoadZoneManager
chainsaw.GameSituationManager
~~~

For each:

- pointer present/null;
- full type name when present.

The current REFramework startup log already proves both singleton types exist somewhere in the runtime, but the production accessor must prove its own lookup result.

### Method resolution

For each type:

~~~text
get_Instance
~~~

Log:

- method pointer present/null;
- declaring type;
- method name;
- whether method is static if that metadata is available through the current SDK without new invasive code;
- parameter count if available.

Do not assume `get_Instance` exists just because the type exists.

### Field resolution

For:

~~~text
SceneLoadZoneManager._Pause
GameSituationManager.InhibitBit
~~~

Log:

- field pointer;
- declaring type;
- field type full name;
- field size;
- enum underlying type where applicable.

### Thread context

Before invoking the getter:

~~~text
sdk::get_thread_context()
~~~

Log present/null once per diagnostic state transition.

Do not change the worker ownership design.

The load-state accessor remains callback-side.

### Instance lookup

Log separately:

~~~text
pause manager instance
situation manager instance
~~~

Do not report only a combined failure.

### Raw field access

For `_Pause`:

- field type must be `System.Boolean`;
- size must match the RE managed bool representation already expected by current code;
- raw field address must be non-null;
- value must be 0 or 1.

For `InhibitBit`:

- resolve enum underlying type if enum;
- underlying type must be one of the currently supported integral managed types;
- width must be 1, 2, 4, or 8;
- raw field address must be non-null.

---

## 7. Logging policy

Do not repeat 18,000 identical lines again.

Use:

~~~text
[RE4XeSS][LoadAccessor]
~~~

Log on:

- first observation;
- failure-stage transition;
- recovery from failure to valid snapshot;
- instance identity change;
- field metadata change;
- first 32 valid snapshot samples;
- actual Pause/Inhibit value transition.

Example:

~~~text
[RE4XeSS][LoadAccessor]
stage=PauseGetterUnavailable
pauseType=0x...
pauseGetter=null
pauseField=0x...
situationType=0x...
situationGetter=0x...
inhibitField=0x...
threadContext=0x...
~~~

When valid:

~~~text
[RE4XeSS][LoadAccessor]
stage=Valid
pauseManager=0x...
situationManager=0x...
pause=0
inhibit=0xb9
~~~

Do not emit the same unchanged failure every frame.

---

## 8. Separate diagnostic observation from history invalidation logging

The current code emits:

~~~text
load-state-observation-unavailable
load-history-invalid
~~~

nearly every frame.

Keep fail-closed behavior, but deduplicate reset logging.

The state may remain invalid every frame without logging the same reset reason every frame.

Suggested rule:

~~~text
history validity behavior:
    unchanged

debug log:
    emit when reason changes
    or when state transitions invalid -> valid / valid -> invalid
~~~

Do not make temporal history valid just to reduce log volume.

---

## 9. Minimal correction is allowed only when diagnostic evidence is unambiguous

This PR may include the actual accessor correction **only** if static/code evidence makes the required change unambiguous.

Examples:

### Case A — wrong method name / inherited accessor

If the actual runtime metadata exposes a clearly correct singleton accessor and `get_Instance` is absent/wrong:

- use the proven accessor;
- keep the same semantic manager objects;
- document the exact resolved method.

### Case B — singleton instance must come from an existing REFramework singleton path

If existing REFramework SDK code already exposes these managed singleton instances by a proven path:

- reuse that existing repository mechanism;
- do not invent another singleton cache.

### Case C — field representation mismatch

If the field metadata proves the current bool/integer reader is too strict but the correct managed representation is unambiguous:

- fix only the reader required by the proven metadata;
- keep width/type validation.

### Case D — accessor timing

If getters/instances are unavailable only before the gameplay managers are constructed, but later become valid:

- keep fail-closed startup behavior;
- retry until valid;
- do not classify startup absence as a permanent fault.

---

## 10. Do not guess the correction

If the first runtime diagnostic proves only:

~~~text
PauseInstanceUnavailable
~~~

or any other stage without proving why, stop after the diagnostic PR/test.

Do not:

- substitute another similarly named manager;
- scan arbitrary managed objects;
- hardcode singleton addresses;
- hardcode `InhibitBit=0xB9`;
- assume `pause=false`;
- mark load observation valid when snapshot is missing;
- disable the Load Save gate;
- use camera discontinuity as the normal replacement;
- move load-state observation onto the XeSS worker just because the callback accessor failed.

A second evidence-driven patch is preferable to a speculative workaround.

---

## 11. Important render-size safety rule

The current capture demonstrates a partial-active condition:

~~~text
SceneView render size may already be overridden
while no XeSS Execute can occur because load observation is invalid
~~~

This is undesirable as a long-term fail-closed state.

For this diagnostic PR, review and explicitly report the current behavior of:

~~~cpp
on_view_get_size()
~~~

If `is_temporal_active()` can remain true while `m_load_observation_valid == false`, add a narrow safety gate so persistent load-accessor failure cannot leave RE4 rendering below display resolution indefinitely with zero XeSS Execute.

Preferred fail-closed behavior outside a confirmed transient load transition:

~~~text
production load observation unavailable
    => no low-resolution SceneView override
    => native rendering until the observation gate recovers
~~~

However, do **not** disable the intended low-resolution render during a known valid load transition solely because history is invalid.

The distinction is:

~~~text
snapshot accessor unavailable
    !=
valid snapshot says load transition active
~~~

If this cannot be implemented without destabilizing PR2 temporal sizing, document it as a separate blocker instead of guessing.

---

## 12. Files expected to change

Primary expected file:

~~~text
src/mods/re4_xess/RE4XeSS.cpp
~~~

Possibly:

~~~text
src/mods/re4_xess/RE4XeSS.hpp
~~~

only if diagnostic state/dedup state belongs on the class.

No expected changes in:

~~~text
src/mods/re4_xess/RE4XeSSWorker.*
src/mods/re4_xess/RE4XeSSD3D12.*
src/mods/re4_xess/RE4XeSSOutputHandoff.*
src/mods/re4_xess/RE4XeSSRuntime.*
src/D3D12Hook.*
src/compatibility/xefg/**
~~~

Do not modify OptiScaler.

---

## 13. Static acceptance checks

Confirm:

### Load-state semantics unchanged

The valid-snapshot state machine still implements:

~~~text
normal inhibit baseline
-> inhibit departure / Pause
-> Pause release still invalid
-> remembered normal inhibit restored
-> first valid frame resetHistory=1
~~~

### Failure stage is observable

No path from `read_game_load_snapshot` to `std::nullopt` remains completely opaque.

Every failure category must map to a bounded `[RE4XeSS][LoadAccessor]` diagnostic.

### No bypass

Search for any newly introduced behavior equivalent to:

~~~text
if snapshot missing -> assume valid
if pause unavailable -> false
if inhibit unavailable -> previous/default value
hardcoded 0xb9
~~~

None is allowed.

### OptiScaler untouched

No changes in producer interception/backend-selection logic.

---

## 14. Build validation

Run:

~~~powershell
cmake -S . -B build-load-accessor ^
  -G "Visual Studio 18 2026" ^
  -A x64 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DDEVELOPER_MODE=ON

cmake --build build-load-accessor --config Release --target REFramework -- /m
~~~

Required:

~~~text
0 compile errors
0 link errors
git diff --check passes
~~~

---

## 15. Runtime test 1 — identify the accessor failure

Use the same deployment:

~~~text
REFramework new DLL
stock OptiScaler as dxgi.dll
OptiScaler\libxess.dll in existing location
FG disabled
RE4 XeSS Quality or Ultra Quality
Debug Log enabled
~~~

Do not move `libxess.dll`.

Stay in actual gameplay for at least 20 seconds.

Required evidence:

~~~text
[RE4XeSS][LoadAccessor] first resolved state
exact failure stage or Valid
~~~

If invalid, the log must answer exactly which type/method/field/context/instance/value step failed.

---

## 16. Runtime test 2 — if the accessor becomes valid

If the same build contains an evidence-backed correction and LoadAccessor reaches Valid:

Expected immediate progression:

~~~text
valid load snapshot
    ->
scene temporal packet
    ->
[RE4XeSS][Frame] first valid resetHistory packet
    ->
[RE4XeSS][Worker] submit
    ->
OptiScaler hk_xessD3D12Execute
    ->
PR4 OutputHandoff activity
~~~

This is the next milestone.

Do not declare PR4 output integration correct merely because Execute begins.

Output-state and handoff validation still follows the existing PR4 acceptance gates.

---

## 17. Runtime test 3 — Load Save after Execute is reached

Only after normal gameplay XeSS Execute is confirmed:

1. remain in gameplay until several valid XeSS frames complete;
2. load a save;
3. capture:
   - normal remembered InhibitBit;
   - pre-load departure;
   - `_Pause false -> true`;
   - `_Pause true -> false`;
   - inhibit restoration;
   - first post-load submitted frame with `resetHistory=1`.

Expected:

~~~text
normal XeSS accumulation
    ->
load invalidation
    ->
no temporal submit while load gate invalid
    ->
load recovery
    ->
first resumed Execute resetHistory=1
~~~

This verifies that the accessor fix did not weaken Capture 22 semantics.

---

## 18. Acceptance criteria

This work is complete when one of these outcomes is reached.

### Outcome A — diagnostic-only success

- exact failing accessor stage is proven;
- no speculative fallback is introduced;
- logs are bounded/deduplicated;
- production remains fail closed;
- next minimal correction is obvious from evidence.

This is an acceptable PR if the fix still requires another runtime fact.

### Outcome B — diagnostic + minimal correction success

- exact original failure stage is documented;
- minimal correction is supported by runtime/repository evidence;
- LoadAccessor reaches Valid in gameplay;
- worker ownership remains stable;
- OptiScaler interception remains active;
- at least one real `xessD3D12Execute` is observed;
- no load-gate bypass was introduced.

PR4 handoff visual/state validation remains a later acceptance item after Execute is first reached.

---

## 19. PR description requirements

Open as Draft against `feature/re4-xess`.

Include:

1. production base commit;
2. PR4-Log evidence counts;
3. proof that OptiScaler is already loaded/intercepting;
4. proof that Execute is still zero;
5. exact LoadAccessor diagnostic stages added;
6. any accessor correction made, with evidence;
7. whether LoadAccessor reached Valid;
8. whether `xessD3D12Execute` was reached;
9. whether Load Save runtime testing was performed;
10. tests remaining for the user.

Do not merge automatically.

---

## 20. Stop conditions

Stop and report instead of widening the patch if:

- both types/methods/fields resolve but the managed instances remain unavailable with no proven alternative instance path;
- field metadata contradicts the Capture 22 names/semantics;
- reading these managers safely requires a different RE Engine thread/context not already proven;
- fixing the accessor appears to require generic managed-object scanning;
- normal gameplay still never produces a valid snapshot after the exact accessor failure is fixed;
- the load gate must be bypassed to reach Execute;
- the next correction would require changes to OptiScaler, XeSS worker ownership, D3D12Hook, or XeFG compatibility.

Those are new evidence questions, not permission to guess.
