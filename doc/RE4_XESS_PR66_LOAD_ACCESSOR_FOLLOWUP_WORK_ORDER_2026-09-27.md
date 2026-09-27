# RE4 XeSS Production — PR66 Load-Accessor Follow-up Work Order

Base branch: `feature/re4-xess`  
Active PR: `#66 [RE4 XeSS] Add bounded Load Save accessor diagnostics`  
Implementation branch: `feature/re4-xess-load-state-accessor-diagnostic`  
PR66 head before this follow-up: `a59a4bcb82f540873d0e0d183e510bdd642a5176`  
Runtime test build reported: `734bd211af738a9439e12b2d009c1fb0768dbae2`  
Date: 2026-09-27  
Scope: add one evidence-focused follow-up commit to PR66 that identifies why the production accessor cannot resolve `GameSituationManager.InhibitBit`, and proves the correct managed storage width for `System.Boolean`.  
Do not merge without review.

Primary architecture:

- `doc/RE4_XESS_PRODUCTION_ARCHITECTURE_2026-09-27.md`

Canonical reverse-engineering evidence:

- `doc/RE4_XESS_BRIDGE_ARCHITECTURE_AND_RE_STATUS_2026-09-26.md`

Original PR66 work order:

- `doc/RE4_XESS_LOAD_STATE_ACCESSOR_DIAGNOSTIC_WORK_ORDER_2026-09-27.md`

Relevant diagnostic oracle:

- `src/mods/RE4TemporalProbe.cpp` on `refactor/re4-temporal-diagnostic`

Runtime evidence:

- `GoogleDrive/ETS2ATS/RE4/PR4-Log/re2_framework_log.txt`
- `GoogleDrive/ETS2ATS/RE4/PR4-Log/OptiScaler.log`

---

## 1. Objective

PR66 succeeded as a diagnostic PR.

The public XeSS producer still reaches stock OptiScaler, but no real Execute occurs because the production Load Save accessor remains invalid.

The new runtime evidence narrowed the first production failure to:

~~~text
[RE4XeSS][LoadAccessor]
stage=InhibitFieldUnavailable
~~~

This follow-up commit must answer two narrow questions:

~~~text
1. Why does
   sdk::find_type_definition("chainsaw.GameSituationManager")
       ->get_field("InhibitBit")
   return null,
   even though Capture 22 observed an InhibitBit field from the
   GameSituationManager singleton?

2. Why does the PR66 diagnostic report System.Boolean size/storageWidth=17,
   when the proven Capture 22 integral reader treated System.Boolean as
   one byte by managed type identity?
~~~

Do not widen this work into XeSS execution, OutputHandoff, OptiScaler, XeFG, or new temporal heuristics.

---

## 2. Runtime facts now proven

### 2.1 PR66 diagnostic is running on the expected RE4 TDB

The runtime log reports:

~~~text
TDB Version: 71
~~~

The production diagnostic resolved both manager type definitions and both singleton getter methods.

### 2.2 First failing stage is exact

Observed:

~~~text
pauseType      = chainsaw.SceneLoadZoneManager
pauseGetter    = get_Instance, static=true, params=0
pauseField     = _Pause

situationType   = chainsaw.GameSituationManager
situationGetter = get_Instance, static=true, params=0
inhibitField    = null

stage = InhibitFieldUnavailable
~~~

Therefore do not investigate OptiScaler as the current blocker.

### 2.3 The Pause metadata exposes a second likely reader mismatch

Observed:

~~~text
Pause field name = _Pause
Pause field type = System.Boolean
type size        = 17
fieldSize        = 17
storageWidth     = 17
~~~

The current production diagnostic derives width from the RE type size metadata.

That is not how Capture 22 read integral fields.

---

## 3. Important Capture 22 implementation difference

The diagnostic branch is an evidence oracle here.

Capture 22 did **not** resolve the load-state fields only from the declared manager type by calling:

~~~cpp
type->get_field("InhibitBit");
~~~

Instead it:

1. obtained the singleton through the native `get_Instance` method;
2. took the returned object's actual runtime type:
   ~~~cpp
   auto* type = object->get_type_definition();
   ~~~
3. walked the full runtime type hierarchy:
   ~~~cpp
   for (auto* current_type = type;
        current_type != nullptr;
        current_type = current_type->get_parent_type()) {
       for (auto* field : current_type->get_fields()) {
           ...
       }
   }
   ~~~
4. identified and logged integral fields from that concrete hierarchy.

Capture 22 also did **not** use `RETypeDefinition::get_size()` as the storage width for Boolean/integer values.

Its proven helper used the managed type name:

~~~cpp
System.Boolean / System.SByte / System.Byte -> 1
System.Char / System.Int16 / System.UInt16  -> 2
System.Int32 / System.UInt32                -> 4
System.Int64 / System.UInt64                -> 8
~~~

and used the enum underlying type when the field type was an enum.

This difference is now the primary lead.

Do not blindly copy the old probe code into production. Reuse only the proven access semantics needed to resolve the contradiction.

---

## 4. Follow-up commit requirements

### 4.1 Do not return before collecting the concrete situation-manager evidence

The current diagnostic reaches `InhibitFieldUnavailable` before a useful concrete singleton/object dump can prove where the field actually lives.

For diagnostic purposes only, restructure the bounded observation path so that when:

~~~text
situation type       = valid
situation get_Instance = valid
inhibitField          = null
~~~

the code may still obtain the situation singleton instance and inspect its metadata.

This does **not** make the snapshot valid.

Production behavior remains:

~~~text
named/proven Inhibit field unresolved
    => load observation invalid
    => fail closed
    => no temporal submit
~~~

The extra instance lookup is diagnostic evidence only.

### 4.2 Log the actual runtime singleton type

Once the situation singleton is available, log once:

~~~text
declared type:
    chainsaw.GameSituationManager

runtime object pointer:
    0x...

runtime object type:
    <full name>

runtime type == declared type:
    true/false
~~~

Also log the parent type chain.

This directly tests whether Capture 22 saw `InhibitBit` on a concrete/derived runtime type that the current declared-type lookup does not cover.

### 4.3 Bounded field-schema dump for the runtime hierarchy

Walk the runtime object's type hierarchy exactly as Capture 22 did.

Log a bounded schema under:

~~~text
[RE4XeSS][LoadAccessorSchema]
~~~

For each field, include:

~~~text
runtimeType
declaringType
fieldName
fieldType
static
literal
offset
enum
enumUnderlyingType when applicable
managedIntegralWidth
~~~

Requirements:

- emit the schema only once per distinct runtime type/metadata state;
- cap output to a reasonable maximum such as 128 fields;
- if the cap is hit, log one truncation marker;
- do not dump field values for arbitrary non-integral fields;
- do not repeat the schema every frame.

At minimum the output must make it possible to answer:

~~~text
Does exact field name "InhibitBit" exist anywhere in the concrete runtime type hierarchy?
If yes:
    which declaring type?
    which managed type?
    which width?
    which offset?
If no:
    which similarly named inhibit-related fields/methods exist?
~~~

### 4.4 Bounded inhibit-related method/property metadata

If exact `InhibitBit` is absent from the runtime field hierarchy, log methods whose names contain `Inhibit`, case-insensitive, once.

Include:

~~~text
declaringType
methodName
static
parameterCount
returnType
function pointer
~~~

This is diagnostic only.

Do not switch production semantics to a getter/property unless the metadata proves an unambiguous equivalent to the Capture 22 field.

### 4.5 Correct the diagnostic width model

Add explicit width reporting based on the same managed type identity rules proven by Capture 22.

For every integral/enum field diagnostic, report separately:

~~~text
typeSizeMetadata
valueTypeSizeMetadata
managedIntegralWidth
~~~

where:

~~~text
managedIntegralWidth
    = width derived from managed primitive name
      or enum underlying managed primitive name
~~~

Do not treat `RETypeDefinition::get_size()` as raw field storage width.

For `System.Boolean`, the diagnostic must report:

~~~text
managedIntegralWidth = 1
~~~

even if RE TDB metadata reports `get_size() == 17`.

Also log `get_valuetype_size()` if safely available, but treat it as evidence, not as the sole storage-width authority.

---

## 5. Minimal production correction allowed in this same follow-up commit

A correction is allowed only where the repository evidence is already unambiguous.

### 5.1 Boolean/integral width correction is allowed

Capture 22 already proves the width rule used successfully against these managers.

It is acceptable to change the production load-accessor integral reader so that:

- managed primitive name determines 1/2/4/8-byte width;
- enum fields use their underlying managed primitive type;
- `get_size()` is not used as the storage width;
- raw address validation remains;
- `System.Boolean` still requires the read value to be 0 or 1.

Keep the correction local to the RE4 XeSS load accessor unless there is already an exact shared helper that implements the same semantics.

Do not make an unrelated SDK-wide type-size change in PR66.

### 5.2 InhibitBit resolution correction requires concrete runtime proof

Do **not** pre-emptively change:

~~~cpp
situation_type->get_field("InhibitBit")
~~~

to a guessed alternative.

A production correction is allowed only if the new diagnostic/repository comparison proves one of these:

#### Case A — exact InhibitBit exists on the concrete runtime type hierarchy

Then use a narrow runtime-type-aware resolution path equivalent to:

~~~text
singleton instance
    -> actual runtime type
    -> inherited hierarchy
    -> exact field name "InhibitBit"
~~~

Do not select a similarly named field.

#### Case B — exact field belongs to a specific proven declaring type

If the concrete dump proves the field is declared on a different exact type but remains the same Capture 22 semantic field, resolve that exact declaring type/field.

#### Case C — Capture 22 evidence is contradicted

If no exact `InhibitBit` exists anywhere in the live runtime hierarchy, stop after diagnostics.

Do not invent a replacement from a similarly named field or property.

---

## 6. Singleton invocation comparison

Capture 22 obtained singletons via:

~~~cpp
sdk::find_native_method(type, "get_Instance")
~~~

cast to a native zero-argument function pointer.

Production currently uses the reflected method plus `call_safe` and a thread context.

The current failure occurs before instance invocation, so this difference is **not yet proven to be a bug**.

However, the follow-up diagnostic must record whether the current reflected invocation successfully returns the same kind of managed singleton object once field-resolution early return no longer hides that evidence.

Do not switch production singleton invocation to `find_native_method` merely because Capture 22 used it.

Only change it if the runtime test proves the reflected `call_safe` path fails while the existing, proven native method path succeeds for the exact same `get_Instance`.

---

## 7. Logging policy

Keep PR66 bounded.

Existing prefix:

~~~text
[RE4XeSS][LoadAccessor]
~~~

New one-shot schema prefix:

~~~text
[RE4XeSS][LoadAccessorSchema]
~~~

Log schema only on:

- first concrete runtime type observation;
- runtime type identity change;
- relevant metadata signature change.

Do not emit one schema dump per frame.

The existing failure-stage transition/dedup behavior remains.

---

## 8. Files expected to change

Primary:

~~~text
src/mods/re4_xess/RE4XeSS.cpp
~~~

Possibly:

~~~text
src/mods/re4_xess/RE4XeSS.hpp
~~~

only if bounded schema/dedup state belongs on the class.

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

Do not reintroduce `RE4TemporalProbe` into the production build.

---

## 9. Static validation

Confirm all of the following.

### Fail-closed behavior preserved

~~~text
Inhibit field unresolved
    => m_load_observation_valid remains false
    => no temporal frame packet
    => no Worker submit
    => no OutputHandoff install
~~~

### Capture 22 semantics preserved

Do not change:

~~~text
remember normal inhibit dynamically
inhibit departure arms invalid history
Pause true confirms load
Pause false alone does not resume
remembered inhibit restoration resumes
first resumed frame resetHistory = true
~~~

### No guessed field/property fallback

Search for newly introduced behavior equivalent to:

~~~text
try InhibitBit, else some similarly named field
missing inhibit => previous/default value
missing pause => false
hardcoded 0xB9
~~~

None is allowed.

### No SDK-wide speculative change

Do not modify generic REFramework TDB size semantics merely to make this accessor pass.

---

## 10. Build validation

Use the same build configuration as PR66.

Required:

~~~text
0 compile errors
0 link errors
git diff --check passes
~~~

Keep the PR as Draft.

---

## 11. Runtime test A — schema proof

Deploy the updated PR66 build with the same environment:

~~~text
stock OptiScaler as dxgi.dll
OptiScaler\libxess.dll unchanged
FG disabled
RE4 XeSS Quality or Ultra Quality
Debug Log enabled
~~~

Stay in gameplay long enough to capture the first follow-up schema.

Required log evidence:

~~~text
[RE4XeSS][LoadAccessor] ...
[RE4XeSS][LoadAccessorSchema] ...
~~~

The capture must answer:

~~~text
actual situation singleton object pointer
actual runtime type
parent type chain
exact presence/absence of InhibitBit
exact declaring type if present
field managed type
managedIntegralWidth
Pause System.Boolean:
    typeSizeMetadata
    valueTypeSizeMetadata
    managedIntegralWidth=1
~~~

---

## 12. Runtime test B — if exact InhibitBit resolution is corrected

If the same commit can safely apply an evidence-backed exact-field correction, expected progression is:

~~~text
[RE4XeSS][LoadAccessor] stage=Valid
    ->
valid scene temporal packet
    ->
first resetHistory packet
    ->
[RE4XeSS][Worker] submit
    ->
OptiScaler hk_xessD3D12Execute
    ->
OutputHandoff activity
~~~

If `stage=Valid` is not reached, stop at the new exact failure stage.

Do not bypass the gate just to reach Execute.

---

## 13. OptiScaler is not part of this fix

Current runtime evidence already proves stock OptiScaler loads its XeSS proxy and receives the producer's public XeSS setup calls.

The absence of:

~~~text
hk_xessD3D12Execute
~~~

is downstream of the RE4 production frame gate, not evidence of a missing OptiScaler XeSS hook.

Do not change:

- OptiScaler configuration;
- XeSS proxy interception;
- backend selection;
- DLL search order;
- XeSS worker ownership.

---

## 14. Acceptance criteria for the additional PR66 commit

The follow-up commit is acceptable when:

1. the concrete `GameSituationManager` runtime type and hierarchy are visible in bounded diagnostics;
2. exact `InhibitBit` presence/absence is proven;
3. the declaring type and managed storage semantics are known if it exists;
4. Boolean/integral width diagnostics no longer treat TDB type size `17` as a 17-byte Boolean storage width;
5. production remains fail closed;
6. no guessed field/property/address is introduced;
7. no unrelated XeSS/OutputHandoff/OptiScaler/XeFG code changes occur.

Preferred same-PR completion, if the evidence is unambiguous:

8. `LoadAccessor stage=Valid`;
9. at least one Worker submit;
10. at least one OptiScaler `hk_xessD3D12Execute`.

Load Save transition validation remains required after normal gameplay Execute is first reached.

---

## 15. Stop conditions

Stop and report rather than widening PR66 if:

- the concrete singleton cannot be obtained safely;
- exact `InhibitBit` is absent from the concrete runtime hierarchy;
- only similarly named fields/properties are found without proof of semantic identity;
- reflected singleton invocation fails and no direct comparison proves why;
- the fix would require hardcoded offsets/addresses;
- the fix would require generic managed-object scanning;
- the Load Save state machine must be weakened;
- the next change would touch OptiScaler, XeSS worker ownership, D3D12Hook, OutputHandoff, or XeFG compatibility.

Those outcomes require new evidence, not a speculative workaround.
