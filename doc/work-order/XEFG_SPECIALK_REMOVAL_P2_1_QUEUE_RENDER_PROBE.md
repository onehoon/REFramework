# XeFG Special K Removal — P2.1 Queue Identity and Render-Safety Probe

## 1. Purpose

P2 solved the original REFramework starvation problem under Intel XeFG: REFramework now captures the XeFG-created presentation path, binds the captured swapchain, and receives `Present1[22]` callbacks without Special K.

The first Intel Dragon's Dogma 2 P2 runtime, however, crashes shortly after REFramework initializes its D3D12 renderer and starts drawing through the captured XeFG path.

This P2.1 work order is a **targeted root-cause / corrective probe** for that failure.

The immediate question is:

```text
Is REFramework submitting overlay work to the wrong D3D12 command queue,
or is direct rendering into the XeFG internal presentation backbuffer itself invalid?
```

P2.1 must answer that question with minimal code and produce a runtime result that directly determines the next implementation step.

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

Only this runtime is in scope:

```text
REFramework fork = dinput8.dll
OptiScaler       = dxgi.dll
Frame generation = Intel XeFG
Special K        = absent
D3D12 RE Engine games
```

Primary runtime target:

```text
Intel GPU + Dragon's Dogma 2
```

Secondary confirmation target:

```text
Intel GPU + Monster Hunter Wilds
```

MHW already shows the same user-visible D3D failure popup under P2. Do not spend time analyzing the old MHW P2 log before implementing this probe. Use MHW only after DD2 gives a clear P2.1 result.

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

The first ten logged Present1 entries all used the same bound swapchain.

This means P2 solved the original P1 starvation failure. Do not redesign API interception or Present1 capture in P2.1.

### 4.2 Failure begins only after REFramework D3D12 rendering initializes

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

Therefore the leading fault boundary is no longer "Present1 hook not reached". It is now the transition from **observing the XeFG Present1 path** to **submitting REFramework D3D12 overlay work into that path**.

### 4.3 A previously invisible queue difference was observed

The P2 binding uses the queue passed to the outer public XeFG init:

```text
InitFromSwapChainDesc queue
= 0x2A2D2CCF630
```

But OptiScaler logged the queue-like `pDevice` argument used for the actual `CreateSwapChainForHwnd` transaction that produced the captured wrapper:

```text
WrappedIDXGISwapChain4 = 0x2A2E917A550
pDevice                = 0x2A2CB938010
real swapchain          = 0x2A2F37E46D0
```

These raw queue pointers differ:

```text
0x2A2D2CCF630 != 0x2A2CB938010
```

Current P2 validation only proves that the captured swapchain device and the **outer Init queue device** resolve to the same D3D12 device. That does not prove that the outer Init queue is the queue that owns/serializes presentation work for the internal swapchain.

This is the primary P2.1 hypothesis.

---

## 5. Important Runtime-Build Caveat

The captured DD2 P2 log reports:

```text
Commit hash: b2de58aa4652652bd96705096bccb9a3330d2c41
Branch: refactor/xefg-p2-init-commit-gate
```

while final P2 was later squash-merged as:

```text
f207f131540d8e7b4c8c815143711cb4290f8f4c
```

The failing binary clearly contains the direct XeFG capture/bind/Present1 implementation, so the queue/render failure evidence is valid for designing P2.1. However, **P2.1 runtime acceptance must use a fresh build from final P2 master (`f207f131...`) plus the P2.1 commit.**

Do not use an older PR artifact for final P2.1 conclusions.

The runtime log must identify the P2.1 commit unambiguously.

---

## 6. Current Code Assumption to Revisit

Current `XefgInitTransaction` retains only the public-init queue:

```cpp
ID3D12CommandQueue* command_queue{};
```

Current `create_xefg_swapchain()` receives this DXGI parameter:

```cpp
IUnknown* device
```

but only captures the returned swapchain.

For D3D12 `CreateSwapChainForHwnd`, that `IUnknown*` is expected to represent the D3D12 command queue used for swapchain creation.

Current publish logic then validates only:

```text
candidate device
vs
outer InitFromSwapChainDesc command queue device
```

and publishes:

```cpp
pending = {
    candidate.Get(),
    transaction.command_queue,
    transaction.hwnd
};
```

Finally `bind_external_swapchain()` makes that outer Init queue authoritative:

```cpp
m_command_queue = command_queue;
```

P2.1 must distinguish the two queues explicitly.

---

## 7. Capture the Actual Presentation-Creation Queue

Extend the XeFG init transaction with a non-owning pointer for the queue observed in `CreateSwapChainForHwnd`.

Suggested minimal shape:

```cpp
struct XefgInitTransaction {
    void* context{};
    HWND hwnd{};

    // Public XeFG InitFromSwapChainDesc argument.
    ID3D12CommandQueue* init_queue{};

    // ID3D12CommandQueue observed as the D3D12 CreateSwapChainForHwnd
    // pDevice argument for the successful internal presentation candidate.
    ID3D12CommandQueue* presentation_queue{};

    IDXGIFactory2* factory{};
    IDXGISwapChain1* candidate{};
    bool factory_create_succeeded{false};
    int32_t init_result{-1};
};
```

Naming may differ, but do not keep calling both values simply `command_queue` after this PR.

### 7.1 Capture rule

Inside `create_xefg_swapchain()`:

1. call the preserved original `CreateSwapChainForHwnd` exactly as P2 does now,
2. only for the successful candidate transaction, `QueryInterface` the incoming `IUnknown* device` to `ID3D12CommandQueue`,
3. record the resulting queue pointer as the presentation-creation queue,
4. do not reinterpret-cast an arbitrary `IUnknown*` without QI,
5. do not permanently AddRef the queue merely for diagnostics.

Temporary `ComPtr` use is fine.

Example shape:

```cpp
Microsoft::WRL::ComPtr<ID3D12CommandQueue> presentation_queue;

if (device != nullptr &&
    SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&presentation_queue))) &&
    presentation_queue != nullptr) {
    g_xefg_transaction.presentation_queue = presentation_queue.Get();
}
```

The stored pointer remains non-owning, consistent with the existing P2 queue model.

Do not alter the existing OptiScaler / DXGI call chain.

---

## 8. Compare COM Identity, Not Raw Pointer Only

Raw pointer inequality alone is not enough. P2.1 must determine whether the two queue interfaces resolve to the same COM identity.

For both:

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
queue type
priority
flags
node mask
device identity
```

Suggested helper concept:

```cpp
struct QueueIdentitySnapshot {
    ID3D12CommandQueue* queue{};
    void* com_identity{};
    void* device_identity{};
    D3D12_COMMAND_QUEUE_DESC desc{};
    bool valid{};
};
```

Do not retain `ComPtr` references in this diagnostic snapshot beyond the local comparison.

### 8.1 Required relation classification

Classify the queues into one of these states:

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

This is the important case for the current DD2 hypothesis.

### 8.2 Queue type guard

Before any experimental rendering through the captured presentation queue, require:

```cpp
presentation_queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT
```

If not, do not submit REFramework overlay work through it.

---

## 9. Strengthen Candidate Validation

Keep every existing P2 acceptance condition:

```text
outer InitFromSwapChainDesc == XEFG_SWAPCHAIN_RESULT_SUCCESS
CreateSwapChainForHwnd succeeded
candidate != null
candidate supports IDXGISwapChain3
candidate HWND matches transaction HWND
candidate GetDevice succeeds
outer init queue GetDevice succeeds
candidate device == outer init queue device identity
```

Add the presentation-queue checks:

```text
presentation queue was captured successfully
presentation queue GetDevice succeeds
candidate device == presentation queue device identity
presentation queue type == DIRECT
```

If presentation queue validation fails, do not experimentally render through it.

Use explicit reject reasons such as:

```text
presentation_queue_unavailable
presentation_queue_device_unavailable
presentation_queue_device_mismatch
presentation_queue_not_direct
```

Do not weaken the outer XeFG Init success commit gate added in P1/P2 review.

---

## 10. P2.1 Automatic Probe Policy

Do not add a permanent user-facing configuration option.

Use the measured queue relation to choose one of two diagnostic runtime paths automatically.

### Case A — `distinct_same_device`

This is the leading DD2 hypothesis.

Publish/bind the captured swapchain using the **presentation-creation queue**, not the outer public-init queue:

```text
selected queue = presentation_queue
render callbacks = enabled
probe mode = presentation_queue_render
```

The outer init queue is still retained in diagnostic state/logs for comparison, but is not authoritative for REFramework overlay submission in this probe mode.

Expected structured log:

```text
[XeFG][QueueIdentity]
relation = distinct_same_device
init_queue = 0x...
init_identity = 0x...
presentation_queue = 0x...
presentation_identity = 0x...
device_identity = 0x...

[XeFG][P2.1Probe]
mode = presentation_queue_render
selected_queue = 0x...
render_callbacks = true
```

### Case B — `same_com_identity`

If both raw interfaces resolve to the same canonical COM identity, changing queue pointers is not a meaningful fix test.

In that case automatically run **observe-only**:

```text
selected queue = existing validated queue
render callbacks = disabled
original Present / Present1 = still called
probe mode = observe_only_same_queue
```

Expected log:

```text
[XeFG][QueueIdentity]
relation = same_com_identity

[XeFG][P2.1Probe]
mode = observe_only_same_queue
render_callbacks = false
```

If the game remains stable in this mode, the evidence shifts strongly toward direct XeFG backbuffer rendering being the invalid operation.

### Case C — invalid presentation queue

If presentation queue validation fails:

```text
render callbacks = disabled
probe mode = observe_only_invalid_presentation_queue
```

Do not fall back to the known-crashing P2 render path merely to keep the overlay enabled.

---

## 11. Implement Observe-Only Without Breaking Present Accounting

The observe-only path must preserve the successful P2 hook mechanics.

It must still perform:

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

It must skip REFramework renderer work:

```text
m_on_present
m_on_post_present
```

for the XeFG P2.1 observe-only mode.

Recommended structure inside `present_common()`:

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

Preserve existing `m_ignore_next_present`, recursion, mutex, and error paths.

Do not bypass `Present1` itself.

Observe-only success is intentionally expected to show **no REFramework overlay**. It is a control mode, not a final product state.

---

## 12. Bind the Selected Queue Explicitly

Extend `PendingXefgBinding` only as much as necessary to retain the distinction.

Suggested shape:

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

`bind_external_swapchain()` may continue to receive a single authoritative queue, but for P2.1 that queue must be `selected_queue`, not blindly the original outer-init queue.

Add a diagnostic field/state so `present_common()` knows whether this binding is observe-only.

Do not change native binding semantics.

---

## 13. Add First-Render Boundary Diagnostics

The DD2 failure occurs immediately after the REFramework D3D12 renderer starts using the bound swapchain.

For the XeFG `presentation_queue_render` probe, add concise one-time diagnostics around the first actual render callback boundary.

Example:

```text
[XeFG][P2.1Probe] render_callback = enter, present_call = ...
[XeFG][P2.1Probe] render_callback = returned, present_call = ...
```

Do not log this every frame.

If the following original Present/Present1 returns `DXGI_ERROR_DEVICE_REMOVED`, also log the D3D12 device removed reason directly from the active device:

```text
[XeFG][P2.1Probe]
present_result = 0x887A0005
device_removed_reason = 0x887A002B
```

This removes dependence on the game crash reporter for the decisive boundary.

Do not add DRED collection in this PR unless it is already trivially available. P2.1 should remain small.

---

## 14. Required Logs

A successful diagnostic build must emit a compact block similar to:

```text
[XeFG][QueueIdentity]
context = 0x...
swapchain = 0x...
init_queue = 0x...
init_identity = 0x...
init_device_identity = 0x...
init_type = DIRECT
init_priority = ...
init_flags = ...
init_node_mask = ...
presentation_queue = 0x...
presentation_identity = 0x...
presentation_device_identity = 0x...
presentation_type = DIRECT
presentation_priority = ...
presentation_flags = ...
presentation_node_mask = ...
relation = same_com_identity | distinct_same_device | ...
```

Then:

```text
[XeFG][P2.1Probe]
mode = presentation_queue_render | observe_only_same_queue | observe_only_invalid_presentation_queue
selected_queue = 0x...
render_callbacks = true | false
```

And existing P2 logs must still show:

```text
[XeFG][InitDesc] result = 0
[XeFG][Bind] accepted = true
[D3D12][ExternalBind] source = xefg_internal
[D3D12][PresentEntry] kind = Present1
```

---

## 15. DD2 Runtime Decision Tree

Run **Intel DD2 first** with a fresh P2.1 build.

### Outcome A — queues are distinct and presentation-queue rendering is stable

Evidence:

```text
relation = distinct_same_device
mode = presentation_queue_render
REFramework overlay appears
XeFG remains active
Present1 continues
no DXGI_ERROR_DEVICE_REMOVED
no 0x887A002B
```

Conclusion:

```text
P2 root cause = wrong authoritative queue.
```

Then retain the presentation-creation queue as the XeFG internal swapchain authoritative queue and proceed to MHW confirmation before P3.

### Outcome B — queues are distinct but device removal still occurs immediately after render callback

Evidence:

```text
relation = distinct_same_device
mode = presentation_queue_render
render_callback returned
next/original Present1 -> DXGI_ERROR_DEVICE_REMOVED
```

Conclusion:

```text
Queue mismatch was real but not sufficient.
Direct rendering to the captured XeFG internal backbuffer is likely invalid.
```

Stop P2.1 there.

Do **not** try random resource-state transitions, forced barriers, extra AddRef/Release loops, or public-proxy rendering in the same PR.

The next work item should be a dedicated P2.2 architecture investigation of where the overlay must be composited relative to XeFG.

### Outcome C — queues resolve to the same COM identity

Expected P2.1 behavior:

```text
mode = observe_only_same_queue
```

If DD2 remains stable with continuous Present1 callbacks:

```text
wrong-queue hypothesis = rejected
render-side/backbuffer hypothesis = strengthened
```

Proceed to P2.2 rather than forcing another queue pointer.

### Outcome D — observe-only itself crashes

If REFramework render callbacks are disabled and the same device removal still occurs:

```text
P2 hook/binding path itself remains suspect.
```

Capture the exact first failing Present1 sequence before any further rendering changes.

This would invalidate the current assumption that renderer submission is the trigger.

---

## 16. MHW Secondary Validation

Do not gate implementation on historical MHW P2 logs.

After DD2 produces one of the clear outcomes above, run the same P2.1 build on Intel MHW.

Required checks:

```text
queue relation classification
selected probe mode
Present1 continuity
renderer initialization boundary
DXGI_ERROR_DEVICE_REMOVED occurrence or absence
```

Remember that MHW previously showed more Streamline / DXGI proxy reuse and recreation than DD2. Do not implement the P3 rebind state machine here if MHW later exposes a different swapchain identity.

If the current P2 guard reports:

```text
reason = p3_rebind_deferred
```

record it and leave it for P3.

---

## 17. Do Not Do These Things

Do not:

```text
reintroduce Special K
bind REFramework rendering to public XefgInterpolationSwapChain
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

Do not expand testing to native/no-FG or FSRFG.

---

## 18. Build / Static Validation

Required before producing the runtime artifact:

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

The runtime log must clearly identify the P2.1 commit/build.

Do not claim runtime success from CI alone.

---

## 19. Acceptance Criteria

P2.1 is complete when all of the following are true:

- [ ] Based on current `master` containing merged P2 (`f207f131...` or later).
- [ ] `CreateSwapChainForHwnd` D3D12 `pDevice` is explicitly QI'd/captured as `ID3D12CommandQueue`.
- [ ] Outer XeFG Init queue and presentation-creation queue are tracked separately.
- [ ] Raw pointer and canonical COM identity are logged for both queues.
- [ ] Queue D3D12 device identities are compared.
- [ ] Queue desc/type is logged and presentation rendering requires a DIRECT queue.
- [ ] Existing P2 outer-init success gate remains intact.
- [ ] Candidate device matches both validated queues before any experimental render path.
- [ ] `distinct_same_device` selects presentation-creation queue for the XeFG render probe.
- [ ] `same_com_identity` automatically selects observe-only rather than pretending the raw pointer change is meaningful.
- [ ] Invalid presentation queue also selects observe-only.
- [ ] Observe-only preserves Present1/original call/liveness accounting while skipping REFramework render callbacks.
- [ ] First render callback boundary is logged once.
- [ ] `DXGI_ERROR_DEVICE_REMOVED` logs `GetDeviceRemovedReason()` from REFramework when encountered.
- [ ] No P3 recreation/ResizeBuffers1 state machine is added.
- [ ] Release build passes.
- [ ] Intel DD2 runtime produces a decisive queue relation + probe result.
- [ ] Intel MHW is tested only after DD2 has classified the result.

---

## 20. Expected Final Report

The implementing PR final report must state exactly one DD2 classification:

```text
A. Wrong authoritative queue confirmed; presentation queue rendering is stable.
B. Queues are distinct, but presentation queue rendering still device-removes; direct backbuffer rendering remains suspect.
C. Queues are the same COM identity; wrong-queue hypothesis rejected; observe-only stable/unstable result recorded.
D. Observe-only itself fails; P2 hook/binding path must be revisited.
```

Include the observed queue identities and device identities in the report.

Do not claim the Special K removal project is solved until the REFramework overlay actually renders stably with XeFG.

---

## 21. Working Hypothesis

The leading hypothesis entering P2.1 is:

```text
P2 captured the correct XeFG internal presentation swapchain
and successfully hooked its Present1 path,

but REFramework selected the outer public XeFG Init queue as authoritative,
while the actual CreateSwapChainForHwnd transaction used a different queue.

REFramework renderer initialization then submitted overlay work through the wrong queue,
causing the subsequent Present1 to return DXGI_ERROR_DEVICE_REMOVED / 0x887A002B.
```

P2.1 must **prove or reject** that hypothesis rather than assume it.

If it is rejected, the next hypothesis is:

```text
The XeFG internal presentation backbuffers are not a valid direct REFramework overlay render target,
even when the correct presentation queue is used.
```

That second hypothesis belongs to the next architecture step, not to speculative fixes inside P2.1.
