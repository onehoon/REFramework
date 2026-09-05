# XeFG Special K Removal — P2.1 Queue Identity and Render-Safety Probe

## 1. Purpose

P2 solved the original REFramework starvation problem under Intel XeFG: REFramework now captures the XeFG-created presentation path, binds the captured swapchain, and receives `Present1[22]` callbacks without Special K.

The first Intel Dragon's Dogma 2 P2 runtime crashes shortly after REFramework initializes its D3D12 renderer and starts drawing through the captured XeFG path.

This P2.1 work order is **code-change only**.

Codex must:

```text
modify the code
add the required diagnostics / queue selection logic
build and run static validation
produce the PR / artifact
stop
```

Codex must **not** run Dragon's Dogma 2 or Monster Hunter Wilds, must not classify the runtime result, and must not decide the next architecture step from simulated or CI evidence.

The user will run the resulting build on real Intel hardware and provide the logs separately for analysis.

The immediate technical question P2.1 prepares evidence for is:

```text
Is REFramework submitting overlay work to the wrong D3D12 command queue,
or is direct rendering into the XeFG internal presentation backbuffer itself invalid?
```

Do not turn this PR into P3 swapchain lifecycle work.

---

## 2. Repository / Base

Repository:

```text
onehoon/REFramework
```

Required base:

```text
master
f207f131540d8e7b4c8c815143711cb4290f8f4c
feat: bind REFramework directly to XeFG presentation (#7)
```

If `master` has advanced, rebase onto current `master` and preserve all merged P2 behavior.

Primary files expected to change:

```text
src/D3D12Hook.cpp
src/D3D12Hook.hpp
```

Touch `src/REFramework.cpp` only if strictly necessary for a small diagnostic guard. Prefer keeping P2.1 localized to `D3D12Hook`.

Keep the implementation comfortably below ~500 LOC if practical.

---

## 3. Scope

Only this runtime is in scope conceptually:

```text
REFramework fork = dinput8.dll
OptiScaler       = dxgi.dll
Frame generation = Intel XeFG
Special K        = absent
D3D12 RE Engine games
```

The user's hardware validation will be performed after the PR is built, primarily with:

```text
Intel GPU + Dragon's Dogma 2
```

and later, if useful:

```text
Intel GPU + Monster Hunter Wilds
```

Codex does not perform either runtime test.

Explicitly out of scope:

```text
native / no-FG compatibility
FSRFG
Special K compatibility
NVIDIA MHW release-storm investigation
ResizeBuffers1[39]
full XeFG swapchain recreation / rebind state machine
fullscreen / Alt+Tab lifecycle hardening
public XefgInterpolationSwapChain rendering
```

---

## 4. Existing P2 Runtime Evidence

### 4.1 P2 capture and Present1 binding succeeded

The DD2 P2 run captured and accepted an internal presentation candidate:

```text
[XeFG][InternalSwapchain]
context   = 0x2a2e54d32d0
candidate = 0x2a2e917a550
provisional = true

[XeFG][InitDesc]
queue  = 0x2a2d2ccf630
result = 0

[XeFG][Bind]
candidate = 0x2a2e917a550
accepted = true
reason = init_success
```

When the REFramework D3D12 hook object was created, the pending binding was consumed successfully:

```text
[D3D12][ExternalBind]
source    = xefg_internal
swapchain = 0x2a2e917a550
queue     = 0x2a2d2ccf630

Hooked DirectX 12 through pending XeFG binding
```

`Present1` then entered REFramework correctly:

```text
[D3D12][PresentEntry]
call = 1
kind = Present1
source = xefg_internal
phase = instance
swapchain = 0x2a2e917a550
```

This means P2 solved the original P1 starvation failure. Do not redesign API interception or Present1 capture in P2.1.

### 4.2 Failure begins after REFramework D3D12 rendering initializes

The same run remained alive through the initial XeFG Present1 callbacks, then REFramework initialized its D3D12 renderer:

```text
00:07:51.694 Attempting to initialize DirectX 12
00:07:51.701 Creating render targets
00:07:51.701 Swapchain buffer count: 3
00:07:51.701 Back buffer format is 24
00:07:51.706 Initializing ImGui
00:07:51.784 REFramework initialized
```

Immediately afterwards OptiScaler reported:

```text
00:07:51.861 Original present result: 887A0005
00:07:51.861 Device removed reason: 887A002B
```

The DD2 crash report records:

```text
Fatal D3D error
DXGI_ERROR_DEVICE_REMOVED (0x887A0005)
DeviceRemovedReason(0x887A002B)
```

The leading fault boundary is therefore the transition from observing the XeFG Present1 path to submitting REFramework D3D12 overlay work into that path.

### 4.3 A queue difference was observed

P2 currently binds the queue passed to outer public XeFG init:

```text
InitFromSwapChainDesc queue
= 0x2A2D2CCF630
```

But the actual `CreateSwapChainForHwnd` transaction that produced the captured internal wrapper received:

```text
WrappedIDXGISwapChain4 = 0x2A2E917A550
pDevice                = 0x2A2CB938010
real swapchain          = 0x2A2F37E46D0
```

The raw pointers differ:

```text
0x2A2D2CCF630 != 0x2A2CB938010
```

Current P2 validation proves only that the captured swapchain device and the outer-init queue device resolve to the same D3D12 device. It does not prove that the outer-init queue is the correct queue for submitting work associated with the internal presentation swapchain.

P2.1 must expose and distinguish these queues explicitly.

---

## 5. Current Code Assumption to Revisit

Current `XefgInitTransaction` retains only the public-init queue:

```cpp
ID3D12CommandQueue* command_queue{};
```

Current `create_xefg_swapchain()` receives:

```cpp
IUnknown* device
```

but only captures the returned swapchain.

For D3D12 `CreateSwapChainForHwnd`, this `IUnknown*` should resolve to the command queue supplied for swapchain creation.

Current publish logic then publishes the outer-init queue:

```cpp
pending = {
    candidate.Get(),
    transaction.command_queue,
    transaction.hwnd
};
```

and `bind_external_swapchain()` makes that queue authoritative:

```cpp
m_command_queue = command_queue;
```

P2.1 must distinguish the outer-init queue from the queue observed during actual internal swapchain creation.

---

## 6. Required Code Change — Capture the Presentation-Creation Queue

Extend the XeFG init transaction with a separate non-owning pointer.

Suggested minimal shape:

```cpp
struct XefgInitTransaction {
    void* context{};
    HWND hwnd{};

    ID3D12CommandQueue* init_queue{};
    ID3D12CommandQueue* presentation_queue{};

    IDXGIFactory2* factory{};
    IDXGISwapChain1* candidate{};
    bool factory_create_succeeded{false};
    int32_t init_result{-1};
};
```

Naming may differ, but the two queue roles must remain explicit.

Inside `create_xefg_swapchain()`:

1. call the preserved original `CreateSwapChainForHwnd` exactly as P2 does now,
2. only for the successful candidate transaction, `QueryInterface` the incoming `IUnknown* device` to `ID3D12CommandQueue`,
3. record that queue as `presentation_queue`,
4. do not reinterpret-cast arbitrary `IUnknown*`,
5. do not add a permanent artificial COM reference as a lifetime workaround.

Example:

```cpp
Microsoft::WRL::ComPtr<ID3D12CommandQueue> presentation_queue;

if (device != nullptr &&
    SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&presentation_queue))) &&
    presentation_queue != nullptr) {
    g_xefg_transaction.presentation_queue = presentation_queue.Get();
}
```

Keep the existing OptiScaler / DXGI call chain unchanged.

---

## 7. Required Code Change — Compare COM Identity

Raw pointer inequality is diagnostic only. Compare canonical COM identity for both queues.

For:

```text
init_queue
presentation_queue
```

collect transiently:

```text
raw ID3D12CommandQueue pointer
IUnknown identity pointer
ID3D12Device pointer / IUnknown device identity
D3D12_COMMAND_QUEUE_DESC
```

At minimum log:

```text
queue pointer
IUnknown identity
device identity
queue type
priority
flags
node mask
```

A compact helper is acceptable, for example:

```cpp
struct QueueIdentitySnapshot {
    ID3D12CommandQueue* queue{};
    void* com_identity{};
    void* device_identity{};
    D3D12_COMMAND_QUEUE_DESC desc{};
    bool valid{};
};
```

Do not retain diagnostic `ComPtr` references after the comparison.

Classify the relation with machine-readable values:

```text
same_com_identity
distinct_same_device
device_mismatch
init_queue_unavailable
presentation_queue_unavailable
presentation_queue_not_direct
```

`distinct_same_device` means:

```text
IUnknown(init_queue) != IUnknown(presentation_queue)
AND
IUnknown(init_queue.device) == IUnknown(presentation_queue.device)
```

---

## 8. Required Code Change — Strengthen Candidate Validation

Keep every existing P2 acceptance condition:

```text
outer InitFromSwapChainDesc == XEFG_SWAPCHAIN_RESULT_SUCCESS
CreateSwapChainForHwnd succeeded
candidate != null
candidate supports IDXGISwapChain3
candidate HWND matches transaction HWND
candidate GetDevice succeeds
init queue GetDevice succeeds
candidate device == init queue device identity
```

Add:

```text
presentation queue captured successfully
presentation queue GetDevice succeeds
candidate device == presentation queue device identity
presentation queue type == D3D12_COMMAND_LIST_TYPE_DIRECT
```

Use explicit reject reasons where appropriate:

```text
presentation_queue_unavailable
presentation_queue_device_unavailable
presentation_queue_device_mismatch
presentation_queue_not_direct
```

Do not weaken the outer XeFG Init success commit gate.

---

## 9. Required Code Change — Produce One Diagnostic Build

P2.1 must produce **one build**, not several runtime variants.

The build behavior should be deterministic from the measured queue relation.

### 9.1 If relation is `distinct_same_device`

Use the validated `presentation_queue` as the authoritative queue for the XeFG internal binding:

```text
selected_queue = presentation_queue
render_callbacks = enabled
mode = presentation_queue_render
```

This is the leading hypothesis test and requires no second compile-time variant.

### 9.2 If relation is `same_com_identity`

Do not pretend that changing raw interface pointers is a meaningful test. Keep the existing validated queue but suppress REFramework render callbacks for this XeFG path:

```text
selected_queue = validated existing queue
render_callbacks = disabled
mode = observe_only_same_queue
```

Original `Present` / `Present1` must still be called normally.

### 9.3 If presentation queue validation fails

Use observe-only:

```text
render_callbacks = disabled
mode = observe_only_invalid_presentation_queue
```

Do not fall back to the known-crashing P2 render path.

Do not add a user-facing toggle or multiple build flavors.

---

## 10. Required Code Change — Observe-Only Must Preserve Hook Liveness

Observe-only must preserve:

```text
PresentEntry accounting
m_last_present_entry_time update
inside_present lifecycle
active swapchain/device tracking
recursion guard
original Present / Present1 call
original return value propagation
hook-monitor liveness
```

It must skip only REFramework render callbacks:

```text
m_on_present
m_on_post_present
```

for the XeFG P2.1 observe-only mode.

Recommended shape inside `present_common()`:

```cpp
const bool suppress_render_callbacks =
    d3d12->m_swapchain_source == SwapchainSource::XeFGInternal &&
    d3d12->m_xefg_p21_observe_only;

if (!suppress_render_callbacks && d3d12->m_on_present) {
    d3d12->m_on_present(*d3d12);
}

++g_present_depth;
const auto result = original_call();
--g_present_depth;

if (!suppress_render_callbacks && d3d12->m_on_post_present) {
    d3d12->m_on_post_present(*d3d12);
}
```

Preserve existing `m_ignore_next_present`, recursion, mutex, and error behavior.

Do not bypass `Present1` itself.

---

## 11. Required Logs

The resulting build must emit enough information for later analysis from user-provided runtime logs.

Required queue block:

```text
[XeFG][QueueIdentity]
context = 0x...
swapchain = 0x...
init_queue = 0x...
init_identity = 0x...
init_device_identity = 0x...
init_type = ...
init_priority = ...
init_flags = ...
init_node_mask = ...
presentation_queue = 0x...
presentation_identity = 0x...
presentation_device_identity = 0x...
presentation_type = ...
presentation_priority = ...
presentation_flags = ...
presentation_node_mask = ...
relation = same_com_identity | distinct_same_device | ...
```

Required selected-mode block:

```text
[XeFG][P2.1Probe]
mode = presentation_queue_render | observe_only_same_queue | observe_only_invalid_presentation_queue
selected_queue = 0x...
render_callbacks = true | false
```

Existing P2 logs must remain available:

```text
[XeFG][InitDesc] result = 0
[XeFG][Bind] accepted = true
[D3D12][ExternalBind] source = xefg_internal
[D3D12][PresentEntry] kind = Present1
```

For the render-enabled path, add one-time first-render boundary logs:

```text
[XeFG][P2.1Probe] render_callback = enter, present_call = ...
[XeFG][P2.1Probe] render_callback = returned, present_call = ...
```

Do not log this every frame.

If original Present/Present1 returns `DXGI_ERROR_DEVICE_REMOVED`, log the active device removed reason directly:

```text
[XeFG][P2.1Probe]
present_result = 0x887A0005
device_removed_reason = 0x...
```

Do not add DRED collection in this PR.

---

## 12. Binding State

Extend `PendingXefgBinding` only as much as required to carry the selected queue and observe-only state.

For example:

```cpp
struct PendingXefgBinding {
    IDXGISwapChain3* swapchain{};
    ID3D12CommandQueue* init_queue{};
    ID3D12CommandQueue* presentation_queue{};
    ID3D12CommandQueue* selected_queue{};
    HWND hwnd{};
    XefgQueueRelation relation{};
    bool observe_only{};
};
```

Equivalent compact state is acceptable.

`bind_external_swapchain()` may continue to receive one authoritative queue, but for P2.1 it must receive `selected_queue` rather than blindly receiving the outer-init queue.

Do not change native binding semantics.

---

## 13. Do Not Do These Things

Do not:

```text
run DD2
run MHW
claim runtime success
classify the final runtime cause inside the PR
create A/B/C/D runtime test variants
create multiple diagnostic builds
reintroduce Special K
bind rendering to public XefgInterpolationSwapChain
hard-code Intel driver DLL addresses
hard-code queue addresses from the DD2 log
use private Intel object offsets
use the old D3D12 swapchain + 0x140 queue scan for XeFG
add ResizeBuffers1 in P2.1
add full recreate/rebind lifecycle handling
change OptiScaler code
add permanent user-facing XeFG debug settings
force-release backbuffers or swapchains
retain artificial COM references as a lifetime fix
suppress DXGI_ERROR_DEVICE_REMOVED and continue
```

Do not expand validation to native/no-FG or FSRFG.

---

## 14. Build / Static Validation Only

Codex must perform only local build/static validation:

```text
cmake --preset vs2022
cmake --build build --config Release --target REFramework
python dev/audit_direct_access_clang.py
git diff --check
```

The Release output must produce:

```text
build/bin/REFramework/dinput8.dll
```

Do not substitute CI/build success for runtime evidence.

Do not launch a game as part of this work order.

---

## 15. Acceptance Criteria for Codex

Codex's work is complete when all of the following are true:

- [ ] Based on current `master` containing merged P2 (`f207f131...` or later).
- [ ] `CreateSwapChainForHwnd` D3D12 `pDevice` is explicitly QI'd/captured as `ID3D12CommandQueue`.
- [ ] Outer XeFG init queue and presentation-creation queue are tracked separately.
- [ ] Raw pointer and canonical COM identity are logged for both queues.
- [ ] Queue D3D12 device identities are compared.
- [ ] Queue desc/type is logged and presentation rendering requires a DIRECT queue.
- [ ] Existing outer XeFG Init success gate remains intact.
- [ ] Candidate device matches both validated queues before render-enabled binding.
- [ ] `distinct_same_device` selects the presentation-creation queue.
- [ ] `same_com_identity` selects observe-only.
- [ ] Invalid presentation queue selects observe-only.
- [ ] Observe-only preserves Present1/original-call/liveness accounting while skipping REFramework render callbacks.
- [ ] First render callback boundary is logged once on the render-enabled path.
- [ ] `DXGI_ERROR_DEVICE_REMOVED` logs `GetDeviceRemovedReason()` when encountered.
- [ ] No P3 recreation/ResizeBuffers1 state machine is added.
- [ ] Release build passes.
- [ ] Static validation passes.
- [ ] Exactly one P2.1 diagnostic build/artifact is produced.
- [ ] No DD2/MHW runtime execution is performed by Codex.

Runtime stability and root-cause determination are **not** Codex acceptance criteria.

---

## 16. Final PR Report

The implementing PR final report should contain only implementation/build facts:

```text
- files changed
- queue identity/capture logic added
- selected-queue / observe-only behavior implemented
- diagnostic log points added
- build/static validation results
- produced dinput8.dll path/artifact
```

Do not include a DD2/MHW runtime classification unless the user explicitly supplies runtime logs later.

Do not claim the Special K removal project is solved.

After merge/build, the user will test the artifact on Intel hardware and provide `re2_framework_log.txt`, `OptiScaler.log`, and crash information if applicable. Those logs will be analyzed separately before the next work order is defined.
