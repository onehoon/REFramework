# RE4 XeSS Production — PR 4 Runtime Blocker Fix Work Order

Base branch: `feature/re4-xess`  
Production code baseline before this documentation: `7c8e005550f697da44443bea15f8f3075bb582f8`  
Implementation must branch from the latest `feature/re4-xess` so this work order and architecture corrections are included.  
PR target: `feature/re4-xess`  
Suggested implementation branch: `feature/re4-xess-pr4-runtime-blocker-fix`  
Date: 2026-09-27  
Scope: fix the first real PR4 runtime blockers before any further output-handoff or XeFG validation  
Do not merge without review.

Primary architecture:

- `doc/RE4_XESS_PRODUCTION_ARCHITECTURE_2026-09-27.md`

Runtime evidence:

- GoogleDrive/ETS2ATS/RE4/PR4 Log/re2_framework_log.txt
- GoogleDrive/ETS2ATS/RE4/PR4 Log/OptiScaler.log

Validated RE4 build:

- Resident Evil 4 2023
- game version 1.5.9.0
- D3D12
- NVIDIA GeForce RTX 4070 SUPER in this capture

---

## 1. Why this corrective PR exists

The first real PR4 runtime test disproved two implementation assumptions before XeSS context creation or execution was reached.

### 1.1 Runtime-candidate fallback bug

REFramework logged:

~~~text
candidate 1 = <RE4>\libxess.dll
candidate 2 = <RE4>\OptiScaler\libxess.dll
selected = <unavailable>
load result = failure

Could not inspect runtime candidate:
The system cannot find the file specified.
~~~

At the same time OptiScaler logged:

~~~text
XeSSProxy::InitXeSS Trying to load libxess.dll
Util::LoadProxyLibrary libxess.dll loaded from Opti dll path:
<RE4>\OptiScaler\libxess.dll
XeSSProxy::InitXeSS LoadResult: true
~~~

The file layout is therefore valid.

Current `RE4XeSSRuntime::initialize()` treats the expected absence of candidate 1 as a fatal `std::filesystem::is_regular_file(..., error_code)` error and never reaches candidate 2.

### 1.2 Pre-Overlay callback thread is not stable

REFramework logged:

~~~text
[RE4XeSS][Init] owner thread established: 1304
[RE4XeSS][Init] pre-Overlay callback owner verified:
    ownerThread=1304 callbackThread=1304

approximately 5 ms later:

[RE4XeSS][Failure]
pre-Overlay callback moved from XeSS owner thread 1304 to 28692;
all XeSS work is blocked
~~~

This is direct runtime evidence that the previous architecture assumption

~~~text
true pre-Overlay callback thread == stable XeSS API owner thread
~~~

is false for the tested RE4 runtime.

The semantic pre-Overlay **boundary remains valid**.

Only its CPU thread identity is unstable.

### 1.3 What did not run

The capture contains no evidence of:

~~~text
xessD3D12CreateContext
xessGetOptimalInputResolution
xessD3D12Init
xessSetVelocityScale
xessD3D12Execute
RE4XeSS RG16F MV conversion
PR4 output handoff install
~~~

OptiScaler also contains no intercepted XeSS producer API calls from REFramework.

Therefore do not modify PR3/PR4 GPU/output semantics based on this capture.

This PR fixes only the two blockers that prevented reaching them.

---

## 2. Frozen correction

Replace the unstable callback-thread ownership model with a **dedicated RE4XeSS worker thread**.

Target:

~~~text
RE4 callbacks
    on_frame
    on_pre_overlay_layer_draw
    on_device_reset
        |
        | control state / synchronous work request
        v
RE4XeSSWorker
    one dedicated OS thread for its entire lifetime
        |
        +-- RE4XeSSRuntime
        |     xessGetVersion
        |     xessD3D12CreateContext
        |     xessGetOptimalInputResolution
        |     xessD3D12Init
        |     xessSetVelocityScale
        |     xessD3D12Execute
        |     xessDestroyContext
        |
        +-- RE4XeSSD3D12
              poll
              initialize
              submit
              shutdown/quarantine
~~~

The worker is the only CPU owner of:

~~~text
RE4XeSSRuntime
RE4XeSSD3D12
all public XeSS API calls
bridge command allocator/list state
bridge writer fence state
~~~

The callback side continues to own:

~~~text
SceneView/jitter integration
RE4 semantic resource discovery
RE4XeSSOutputHandoff TargetState creation/install/restore
post-Present downstream retirement marker
UI/config
load-state observation
~~~

Callback-side ownership does **not** mean one fixed Windows thread.

The first PR4 capture proved that consecutive true pre-Overlay callbacks may execute on different engine threads. OutputHandoff therefore follows the semantic pre-Overlay coordinator, not a callback thread ID.

---

## 3. New component

Add:

~~~text
src/mods/re4_xess/RE4XeSSWorker.hpp
src/mods/re4_xess/RE4XeSSWorker.cpp
~~~

No external threading library is required.

Use standard C++ synchronization, for example:

~~~text
std::thread
std::mutex
std::condition_variable
~~~

or an equivalent repository-compatible implementation.

Do not add a general-purpose global thread pool.

This is a private RE4-only single worker.

### 3.1 Worker lifetime

Start the worker from RE4XeSS initialization.

Starting the thread must not itself load XeSS or create D3D12 resources.

When mode is Off the worker stays idle.

Set a useful thread description where available:

~~~text
RE4XeSS Worker
~~~

Log its Windows thread ID once:

~~~text
[RE4XeSS][Worker] started thread=<id>
~~~

### 3.2 No callback-thread affinity

Delete the assumption that any RE4 callback thread is the XeSS owner.

The pre-Overlay callback is allowed to move between threads on consecutive frames.

Do not quarantine solely because:

~~~text
GetCurrentThreadId() changed between pre-Overlay callbacks
~~~

The worker thread ID is the only XeSS API owner identity.

---

## 4. Worker API

Exact class/member names may differ, but keep the boundary explicit.

Recommended result model:

~~~cpp
class RE4XeSSWorker {
public:
    struct ControlRequest;
    struct ControlResult;
    struct SubmitRequest;
    struct SubmitResult;

    bool start(std::string& error);
    ControlResult service_sync(const ControlRequest& request);
    SubmitResult submit_sync(const SubmitRequest& request);
    void stop() noexcept;

    DWORD thread_id() const noexcept;
};
~~~

### 4.1 Synchronous means CPU ordering only

`service_sync()` and `submit_sync()` may block the calling pre-Overlay callback until the worker has:

~~~text
processed the control request
or
recorded + submitted the bridge command list
~~~

They must **not** wait for the GPU fence to complete.

No per-frame CPU wait on bridge fence is introduced.

This preserves the semantic ordering:

~~~text
pre-Overlay callback
    -> worker records/submits XeSS list
    -> caller receives Submitted
    -> caller installs Overlay handoff
    -> RE4 downstream work continues
~~~

### 4.2 One outstanding worker operation

Do not build a deep asynchronous queue.

The initial corrective implementation should allow at most one synchronous caller request at a time.

The mailbox must be bounded.

Do not let old frame requests accumulate.

### 4.3 The callback coordinator is also single-entry

The worker mailbox alone does not serialize callback-side temporal state or OutputHandoff mutation.

Add an explicit non-reentrant gate around the full true pre-Overlay coordinator.

Suggested state:

~~~cpp
std::atomic_flag m_pre_overlay_in_progress = ATOMIC_FLAG_INIT;
std::atomic<uint64_t> m_pre_overlay_overlap_epoch{};
~~~

At callback entry:

~~~cpp
if (m_pre_overlay_in_progress.test_and_set(std::memory_order_acquire)) {
    m_pre_overlay_overlap_epoch.fetch_add(1, std::memory_order_acq_rel);

    // Fail closed. Do not restore/install handoff, do not dispatch XeSS,
    // and do not mutate temporal history from the overlapping callback.
    return true;
}

const auto gate = gsl::finally([&] {
    m_pre_overlay_in_progress.clear(std::memory_order_release);
});
~~~

Use any equivalent RAII cleanup available in the repository; do not add a dependency only for this helper.

The callback that owns the gate snapshots `m_pre_overlay_overlap_epoch` before worker dispatch and verifies that it is unchanged before post-submit handoff installation/history commit.

If an overlapping callback was observed while the owner callback waited on the worker, the current frame is considered stale even if the worker submitted GPU work:

~~~text
no Overlay handoff install
no temporal-history commit
invalidate history for next valid submission
retire submitted output after writer completion
~~~

Do not block/spin waiting for the other callback.

This is a fail-closed guard until runtime evidence proves pre-Overlay callbacks are never concurrent.

---

## 5. Worker ownership of RE4XeSSRuntime

Move `RE4XeSSRuntime` into the worker, or otherwise make it inaccessible to non-worker callers.

Every public XeSS call must execute on the dedicated worker thread:

~~~text
xessGetVersion
xessD3D12CreateContext
xessGetOptimalInputResolution
xessD3D12Init
xessSetVelocityScale
xessD3D12Execute
xessDestroyContext
~~~

Retain `RE4XeSSRuntime` owner-thread validation as a defensive assertion.

On first public XeSS operation:

~~~text
runtime owner thread id == worker thread id
~~~

Any mismatch is a programming error and fails closed.

### 5.1 Remove obsolete RE4 callback owner state

Remove/replace the current RE4XeSS fields and logic tied to callback thread identity, including the current intent of:

~~~text
m_execution_owner_thread_id
m_owner_thread_violation
m_owner_thread_logged
"pre-Overlay callback moved from XeSS owner thread"
~~~

The producer must no longer enter permanent quarantine merely because the RE4 callback moved from thread 1304 to 28692 or any other valid engine thread.

---

## 6. Worker ownership of RE4XeSSD3D12

The worker also owns `RE4XeSSD3D12`.

All of these happen only on the worker:

~~~text
has_generation / poll
initialize
matches
ready / idle state transition
submit
shutdown
shutdown_after_device_removed
quarantine
~~~

Do not have alternating RE4 callback threads mutate the bridge object directly.

### 6.1 Worker status snapshot

The worker returns enough state for the callback-side coordinator without exposing the bridge object.

At minimum expose:

~~~text
producer/context ready
execution ready
draining/faulted
quality
display extent
input resolution
device identity
queue identity
bridge idle
bridge quarantined
failure reason
accepted control generation
accepted device-reset generation
~~~

The callback-side `RE4XeSSOutputHandoff::poll_retirement()` uses the returned `bridge_idle` bit instead of calling `m_bridge.idle()` directly.

---

## 7. Control request

The pre-Overlay callback supplies plain control state to the worker.

Suggested request:

~~~cpp
struct ControlRequest {
    bool active{};
    UpscalingMode mode{};
    std::optional<xess_quality_settings_t> quality{};

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;

    xess_2d_t display{};

    uint64_t control_generation{};
    uint64_t device_reset_generation{};
};
~~~

Strong device/queue references are acceptable for the duration of the synchronous request and worker-owned generation.

Validate DIRECT queue on the worker before bridge creation.

The existing control behavior remains:

- bootstrap;
- optimal input query;
- SR init;
- bridge poll/drain;
- mode/quality/display/device change;
- device removal;
- explicit retry generation.

The worker must echo the exact accepted `control_generation` and `device_reset_generation` in `ControlResult`.

A result from a different generation is stale and cannot make callback-side temporal state ready.

Do not change XeSS quality mapping.

---

## 8. Submit request and CPU lifetime

The pre-Overlay callback still discovers and validates the semantic RE4 resources.

Before sending them to another CPU thread, pin them for the request lifetime.

Suggested shape:

~~~cpp
struct SubmitRequest {
    RE4XeSSFrame frame{};

    Microsoft::WRL::ComPtr<ID3D12Resource> color_pin;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_pin;
    Microsoft::WRL::ComPtr<ID3D12Resource> velocity_pin;
    Microsoft::WRL::ComPtr<ID3D12Resource> output_pin;

    RE4XeSSD3D12::OutputBinding output{};

    uint64_t control_generation{};
    uint64_t device_reset_generation{};
};
~~~

Before dispatch:

~~~text
frame.color    = color_pin.Get()
frame.depth    = depth_pin.Get()
frame.velocity = velocity_pin.Get()
output.resource = output_pin.Get()
~~~

The request pins guarantee that the raw pointers remain valid while the caller waits for the worker to consume them.

After successful bridge submission:

- RE4XeSSD3D12 retains its own per-slot GPU lifetime pins as already implemented;
- request pins may release when `submit_sync()` returns.

Do not remove the existing slot pins.

They protect a different lifetime interval.

### 8.1 Submit generation contract

`SubmitResult` must echo:

~~~text
control_generation
device_reset_generation
submitted / busy / faulted
bridge_idle
bridge_quarantined
~~~

Before recording a submit, the worker must reject the request if its generation does not match the worker's currently accepted control configuration.

Suggested worker-side check:

~~~cpp
if (request.control_generation != m_configuration.control_generation ||
    request.device_reset_generation != m_configuration.device_reset_generation) {
    return SubmitResult{
        .status = SubmitStatus::Stale,
        .control_generation = request.control_generation,
        .device_reset_generation = request.device_reset_generation,
    };
}
~~~

This prevents requests that were already stale before worker execution.

### 8.2 Generation can still change while synchronous submit is running

Mode Off, quality change, or device reset may be requested by another callback/thread after the request passed the worker-side check.

Therefore, after `submit_sync()` returns and **before** OutputHandoff install/history commit, reload:

~~~cpp
const auto current_control =
    m_control_generation.load(std::memory_order_acquire);
const auto current_reset =
    m_device_reset_generation.load(std::memory_order_acquire);
const auto current_overlap =
    m_pre_overlay_overlap_epoch.load(std::memory_order_acquire);
~~~

Require all of:

~~~text
result.control_generation == request.control_generation == current_control
result.device_reset_generation == request.device_reset_generation == current_reset
current_overlap == overlap_epoch_captured_before_dispatch
requested mode is still the request mode
~~~

If any check fails, the result is stale.

#### Stale before GPU submission

If worker returns `Stale` without submitting:

~~~text
do not install OutputHandoff
do not commit history
invalidate history
service the newer control generation on the next coordinator pass
~~~

#### Stale after GPU submission

If the worker already returned `Submitted` but the callback-side post-return check finds a newer generation:

~~~text
call OutputHandoff::note_submission_succeeded()
DO NOT install Overlay main TargetState
DO NOT clear resetHistory/history-invalid state
request retirement of that output generation
keep bridge/output lifetime until writer fence proves the submitted work complete
no downstream-retirement marker is required because the handoff was never installed/read by RE4
~~~

The next coordinator pass services the new control/device generation.

Do not treat stale-after-submit as a device fault.

This rule prevents an old Quality/device generation from becoming visible after Mode Off or reset was requested.

---

## 9. Pre-Overlay callback flow after this fix

The semantic callback remains the orchestration point, but it is no longer the XeSS API owner.

Target order:

~~~text
on_pre_overlay_layer_draw(current engine thread may vary)
    |
    | acquire non-reentrant coordinator gate
    | snapshot control/reset/overlap generations
    v
restore previous PR4 handoff
    |
    v
build ControlRequest
    |
    | synchronous CPU dispatch
    v
worker.service_sync()
    |
    | verify echoed generations are still current
    | returns producer state + bridge_idle
    v
callback polls OutputHandoff retirement
    |
    +-- worker not ready/stale
    |      fail closed for this frame
    |
    v
validate temporal state
resolve Color/Depth/Velocity
build RE4XeSSFrame
prepare engine-visible OutputHandoff
pin request resources
    |
    | synchronous CPU dispatch
    v
worker.submit_sync()
    |
    | reload control/reset/overlap generations
    v
    +-- Submitted + still current
    |      OutputHandoff note_submission_succeeded
    |      install TargetState on this callback thread
    |      commit temporal history
    |
    +-- Submitted + stale after return
    |      note submission for state tracking
    |      NO install
    |      NO history commit
    |      retire output after writer completion
    |      reset history next valid submit
    |
    +-- Stale / Busy
    |      no install
    |      reset history next valid submit
    |
    +-- Faulted
           no install
           existing quarantine/fail-closed policy
~~~

Do not move Overlay TargetState mutation onto the worker.

---

## 10. Runtime candidate discovery fix

Fix `RE4XeSSRuntime::initialize()`.

Supported order remains exactly:

~~~text
1. <REFramework directory>\libxess.dll
2. <REFramework directory>\OptiScaler\libxess.dll
~~~

Expected absence of candidate 1 is not an error.

Recommended Win32 implementation:

~~~cpp
for (const auto& candidate : m_candidates) {
    const auto attrs = GetFileAttributesW(candidate.c_str());

    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();

        if (error == ERROR_FILE_NOT_FOUND ||
            error == ERROR_PATH_NOT_FOUND) {
            continue;
        }

        fail(
            "Could not inspect runtime candidate '" +
            path_for_log(candidate) +
            "', Win32 error " +
            std::to_string(error));
        return false;
    }

    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        continue;
    }

    m_selected_path = candidate;
    break;
}
~~~

Equivalent code is acceptable.

Do not reintroduce arbitrary PATH/current-working-directory search.

### 10.1 Exact path loading remains

After selection keep:

~~~cpp
LoadLibraryExW(
    selected_full_path,
    nullptr,
    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
~~~

When OptiScaler already loaded the same `OptiScaler\libxess.dll`, exact-path LoadLibraryExW is allowed to obtain another module reference to that same DLL.

Do not special-case or detect OptiScaler.

---

## 11. Callback-side OutputHandoff stays outside the worker, but its thread-affinity behavior must change

Keep `RE4XeSSOutputHandoff` outside the worker.

Reasons:

- it mutates RE Engine `Overlay::main_target_state`;
- next-pre-Overlay restore must happen at the semantic engine boundary;
- `on_post_present()` owns downstream retirement marker logic;
- it performs no XeSS API call.

However, the current implementation has an obsolete fixed-callback-thread restriction:

~~~text
prepare:
    m_owner_thread_id = GetCurrentThreadId()

restore:
    if current_thread != m_owner_thread_id:
        quarantine
~~~

This must be removed.

The runtime evidence proves the next valid semantic pre-Overlay callback can run on another engine thread.

### 11.1 New OutputHandoff CPU ownership rule

OutputHandoff install/restore/prepare operations are owned by the **single-entry pre-Overlay coordinator**, not by a Windows thread ID.

Therefore:

- remove `m_owner_thread_id` from OutputHandoff generation state;
- remove the `current_thread` ownership argument/check from `restore()`, or make it diagnostic-only with no failure behavior;
- do not set a callback owner thread in `prepare()`;
- allow frame N install on thread A and frame N+1 restore on thread B;
- retain all existing Overlay identity, TargetState identity, marker, fence, and generation checks.

The coordinator gate in section 4.3 is what serializes callback-side mutation.

`on_post_present()` remains independently synchronized by the existing retirement mutex and does not mutate Overlay/TargetState.

### 11.2 Thread migration acceptance

Required runtime sequence:

~~~text
frame N pre-Overlay callback thread = A
    install handoff

frame N post-Present
    retirement marker queued

frame N+1 pre-Overlay callback thread = B
    restore succeeds
    no quarantine due only to A != B
~~~

Log the first 32 migrations under Debug Log.

Thread migration itself is informational, not an error.

The worker returns `bridge_idle` and failure/quarantine state for retirement decisions.

Do not weaken the PR4 dual-fence lifetime contract.

---

## 12. on_post_present remains non-XeSS

Keep:

~~~text
RE4XeSS::on_post_present
    -> RE4XeSSOutputHandoff::on_post_present
    -> same-generation DIRECT queue retirement Signal
~~~

No public XeSS API call is allowed from `on_post_present()`.

It does not need to run on the worker thread.

Do not route it through the worker.

---

## 13. Mode Off / reset / teardown

### 13.1 Mode Off

Mode change remains control-plane state.

At the next pre-Overlay:

- restore handoff;
- worker service sees inactive generation;
- worker drains bridge writer work;
- worker destroys XeSS only on its dedicated thread;
- OutputHandoff separately applies downstream retirement proof.

### 13.2 Device reset

`on_device_reset()` still only increments/request state.

It must not directly call runtime or bridge.

The next worker service handles old generation drain/device removal and new device setup.

### 13.3 Destructor / process shutdown

`RE4XeSS::~RE4XeSS()` must not directly call XeSS APIs or bridge methods anymore.

Call worker shutdown.

Worker shutdown logic:

~~~text
if bridge DeviceRemoved:
    shutdown_after_device_removed
    runtime.shutdown on worker

else if bridge idle:
    bridge.shutdown
    runtime.shutdown on worker

else:
    bridge.quarantine
    runtime.quarantine on worker
~~~

Then stop/join the worker.

Do not wait indefinitely for a GPU fence during process teardown.

Do not call xessDestroyContext from the destructor thread.

---

## 14. Failure semantics

### Worker startup failure

Fail closed:

~~~text
Off/native rendering
no SceneView low-resolution override
no jitter
no XeSS
no handoff
~~~

### Worker dispatch failure

If the worker terminated unexpectedly or cannot accept the synchronous request:

- no XeSS submit;
- no handoff install;
- invalidate temporal history;
- expose a stable failure reason;
- require explicit mode toggle/device-reset generation for retry, unless implementation safely recreates the worker.

### Runtime candidate failure

Only unexpected file-inspection errors or both supported candidates being absent are failures.

Expected candidate-1 absence must not fault.

---

## 15. Logging

Add bounded evidence.

### Worker

~~~text
[RE4XeSS][Worker] started thread=<id>
[RE4XeSS][Worker] service callerThread=<id> workerThread=<id>
[RE4XeSS][Worker] submit callerThread=<id> workerThread=<id>
[RE4XeSS][Worker] shutdown thread=<id>
~~~

Do not log every frame forever.

First 32 submit dispatches are enough under Debug Log.

### Runtime discovery

Log each candidate result:

~~~text
candidate 1: missing -> continue
candidate 2: file -> selected
selected exact path
LoadLibraryExW result
~~~

### Thread migration proof

For the first 32 pre-Overlay callbacks under Debug Log, log the callback thread ID separately from the worker ID.

Expected:

~~~text
callbackThread may change
workerThread remains constant
~~~

Thread migration is not a failure.

---

## 16. Expected changed-file set

Expected approximately:

~~~text
CMakeLists.txt

src/mods/re4_xess/RE4XeSSWorker.hpp
src/mods/re4_xess/RE4XeSSWorker.cpp

src/mods/re4_xess/RE4XeSS.hpp
src/mods/re4_xess/RE4XeSS.cpp

src/mods/re4_xess/RE4XeSSRuntime.cpp
src/mods/re4_xess/RE4XeSSRuntime.hpp   # only if small ownership/log helpers are needed

src/mods/re4_xess/RE4XeSSOutputHandoff.hpp
src/mods/re4_xess/RE4XeSSOutputHandoff.cpp
~~~

`RE4XeSSD3D12.*` should not need algorithmic changes.

Its ownership changes to the worker, but PR3 bridge semantics remain intact.

`RE4XeSSOutputHandoff.*` **does require the targeted behavioral correction described in section 11**: remove fixed callback-thread affinity while preserving all semantic/generation/retirement invariants.

No changes expected in:

~~~text
src/D3D12Hook.*
src/compatibility/xefg/**
shared/sdk/Renderer.*
~~~

If those become necessary, stop and report.

---

## 17. Static acceptance checks

### No unstable callback owner assumption

Search the final code for concepts equivalent to:

~~~text
pre-Overlay callback moved from XeSS owner thread
m_execution_owner_thread_id
m_owner_thread_violation
OutputHandoff m_owner_thread_id
"restoration was requested from a non-owner thread"
~~~

They should be removed from production failure/control flow.

### Callback coordinator non-reentrancy

Static review must show that the complete pre-Overlay coordinator is guarded by a nonblocking single-entry gate.

An overlapping callback must not:

~~~text
restore/install Overlay handoff
dispatch another XeSS request
mutate temporal history
~~~

It marks the current owner pass stale and returns fail-closed.

### Generation post-check

After worker `service_sync()` and especially after `submit_sync()`, code must compare returned/request generations with the latest control/reset generation.

A stale `Submitted` result must not install Overlay state or commit history.

### One XeSS API CPU owner

All runtime entry points must be reachable only from RE4XeSSWorker.

No callback directly calls:

~~~text
m_runtime.initialize
m_runtime.query_optimal_input_resolution
m_runtime.initialize_sr
m_runtime.execute
m_runtime.shutdown
~~~

### One bridge CPU owner

No callback directly calls bridge mutation/poll/submit functions.

They execute on worker.

### Candidate fallback

A missing top-level `<REF>\libxess.dll` must continue to `<REF>\OptiScaler\libxess.dll`.

### Protected paths

No behavior changes in existing XeFG compatibility or D3D12Hook.

---

## 18. Build validation

Run:

~~~powershell
cmake -S . -B build-pr4-runtime-fix ^
  -G "Visual Studio 18 2026" ^
  -A x64 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DDEVELOPER_MODE=ON

cmake --build build-pr4-runtime-fix --config Release --target REFramework -- /m
~~~

Required:

~~~text
0 compile errors
0 link errors
git diff --check passes
~~~

PE check:

- no static libxess.dll import;
- no new external runtime dependency.

---

## 19. First runtime retest

Use the same physical deployment that produced the blocker log.

Keep:

~~~text
<RE4>\dinput8.dll = new REFramework build
<RE4>\dxgi.dll = stock OptiScaler
<RE4>\OptiScaler\libxess.dll = existing OptiScaler file
FG disabled
RE4 XeSS Quality
REFramework Debug Log enabled
~~~

Do not move/copy libxess.dll to the game root merely to make candidate 1 pass.

Candidate 2 must work.

### 19.1 Runtime discovery acceptance

Expected:

~~~text
candidate 1 missing -> continue
candidate 2 selected
LoadLibraryExW success
xessGetVersion success
xessD3D12CreateContext success
~~~

OptiScaler should observe the public XeSS producer calls.

### 19.2 Thread and handoff acceptance

Expected even if RE4 callback IDs alternate:

~~~text
frame N:
    preOverlay callbackThread=1304
    handoff install succeeds

frame N+1:
    preOverlay callbackThread=28692
    previous handoff restore succeeds

RE4XeSS workerThread=<one constant id>
runtime ownerThread=<same worker id>
~~~

No quarantine from callback migration.

OutputHandoff must not report a non-owner-thread restoration failure.

### 19.3 Coordinator overlap test

Runtime evidence does not prove callbacks are non-overlapping.

Add bounded logging for coordinator entry/exit and overlap detection.

Normal expected result:

~~~text
overlapCount = 0
~~~

If overlap occurs:

~~~text
second callback fails closed immediately
owner callback becomes stale before install/history commit
no second worker submit is queued
next valid frame resets history
~~~

A detected overlap is not permission to add a blocking callback mutex.

Use the nonblocking guard defined in section 4.3.

### 19.4 Generation-change test

Exercise at least:

~~~text
Quality -> Off
Quality -> Balanced
device/swapchain reset if practical
~~~

around active worker dispatch.

Required invariant:

~~~text
old-generation worker result may finish
but if current control/reset generation changed:
    no old handoff install
    no old history commit
~~~

If an old-generation command list was already submitted, logs must show:

~~~text
stale-after-submit
writer retirement/drain
no downstream marker required unless that output had actually been installed
~~~

### 19.5 Producer progression

The retest must reach beyond the old stop point:

~~~text
xessGetOptimalInputResolution
xessD3D12Init
xessSetVelocityScale
xessD3D12Execute
~~~

Only after these succeed should PR3/PR4 GPU/output validation continue.

### 19.6 OptiScaler evidence

OptiScaler.log should contain intercepted XeSS producer activity.

If REF reports successful Execute but OptiScaler shows no corresponding interception, stop and inspect the frontend/proxy call path.

---

## 20. Do not widen scope based on load-state observation yet

The first log also contains:

~~~text
load-state-observation-unavailable
~~~

Do not fix that in this corrective PR unless it independently prevents gameplay execution after the two primary blockers are solved.

Menu/boot scenes may legitimately lack the gameplay load-state singleton.

Retest in actual gameplay first.

If load-state observation remains the next blocker after XeSS context/init becomes reachable, capture that as a separate evidence-driven fix.

---

## 21. Acceptance criteria

This corrective PR is complete when:

- missing candidate 1 no longer prevents candidate 2 discovery;
- `OptiScaler\libxess.dll` is selected by exact path in the tested layout;
- a dedicated RE4XeSS worker thread exists;
- every public XeSS API call runs on that one worker thread;
- RE4 pre-Overlay callback thread migration is tolerated;
- OutputHandoff restore/install is serialized by the semantic coordinator and does not require a fixed callback thread;
- pre-Overlay callback re-entry is nonblocking fail-closed;
- worker control/submit results are generation-tagged;
- stale worker results cannot install handoff or commit history;
- stale-after-submit output remains alive through writer retirement;
- RE4XeSSD3D12 is owned/serialized by the same worker;
- callback-to-worker resource lifetime is protected by request ComPtr pins;
- existing bridge slot GPU pins remain intact;
- OutputHandoff remains callback-side and unchanged in semantics;
- post-Present retirement remains callback-side and calls no XeSS API;
- mode/reset/destructor teardown calls XeSS only on worker;
- no per-frame GPU fence wait is introduced;
- existing XeFG compatibility remains untouched;
- local x64 Release build succeeds;
- the runtime retest reaches at least public XeSS context creation/init;
- ideally the retest reaches real `xessD3D12Execute`, after which PR3/PR4 validation resumes.

---

## 22. PR creation requirements

Open as a Draft PR against:

~~~text
feature/re4-xess
~~~

PR description must include:

1. this is a corrective PR for the first PR4 runtime test, not the next feature milestone;
2. base production commit `7c8e005550f697da44443bea15f8f3075bb582f8`;
3. exact runtime evidence: top-level candidate missing, OptiScaler candidate exists;
4. exact runtime evidence: pre-Overlay callback moved 1304 -> 28692;
5. candidate fallback fix;
6. dedicated worker ownership model;
7. all public XeSS calls occur on the worker;
8. bridge ownership moved to the worker;
9. OutputHandoff/post-Present remain callback-side;
10. build result;
11. runtime retest performed;
12. highest XeSS stage actually reached;
13. tests remaining for the user.

Do not merge automatically.

---

## 23. Stop conditions

Stop and report if:

- Intel/OptiScaler XeSS frontend rejects public API calls from the dedicated worker despite all calls using the same worker thread;
- D3D12 command recording/submission from the worker breaks the proven same-queue pre-Overlay ordering;
- synchronous CPU worker dispatch causes a re-entrant deadlock;
- RE4 requires the bridge command list to be recorded on the callback thread;
- moving bridge ownership to the worker would require changes in D3D12Hook or XeFG compatibility;
- OutputHandoff TargetState mutation would need to move to the worker;
- callback overlap cannot be handled fail-closed without blocking/re-entering RE Engine;
- a stale-after-submit generation cannot be retained safely until writer completion;
- candidate 2 exact-path loading fails even though the file is present, with a dependency/load error that requires global DLL search mutation.

These are new architecture contradictions and must be reviewed before widening the patch.
