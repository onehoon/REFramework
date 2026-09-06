# Work Order: XeFG Refactor R4 — Queue Validation and Binding Candidate Construction

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `cfb6efe5102f330c9ddf6fdadb7e9af984ce0678` (`Refactor R3: extract XeFG InitDesc transaction and factory capture (#21)`)

Relevant merged baseline:

- R1 / PR #17: `65f9b3ee81971c3e2aac6df49518fa2dd588365d` — exact-HMODULE XeFG runtime registry extraction
- R2 / PR #18: `aa3a53e516b882a77d399c929efa0ef29d1426b0` — XeFG loader / probe handoff isolation
- PR #19: `3f83b8af0184f931daec44dc45257f7fc46966a4` — register XeFG compatibility sources in tracked `CMakeLists.txt`
- PR #20: `74042e1686f62a54a50540e1a113a3ae648778c1` — MHW-only XeFG ResizeTarget transition hold
- R3 / PR #21: `cfb6efe5102f330c9ddf6fdadb7e9af984ce0678` — serialized InitDesc observation + temporary factory capture extraction

Related documents:

- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_ARCHITECTURE_2026-09-06.md`
- `doc/refactor/REFramework_OPTISCALER_XEFG_REFACTOR_PR_SPLIT_PLAN_2026-09-06.md`
- `doc/work-order/XEFG_REFACTOR_R1_RUNTIME_REGISTRY_EXTRACTION.md`
- `doc/work-order/XEFG_REFACTOR_R2_LOADER_PROBE_HANDOFF.md`
- `doc/work-order/XEFG_REFACTOR_R3_INIT_TRANSACTION_FACTORY_CAPTURE.md`

This work order implements **R4 only** from the fine-grained refactor plan.

---

# 1. Recommended PR Identity

Suggested branch:

```text
refactor/xefg-r4-queue-validation-candidate
```

Suggested PR title:

```text
Refactor R4: extract XeFG queue validation and binding candidate
```

Suggested commit title:

```text
refactor: extract XeFG binding candidate validation
```

This PR is primarily a **behavior-preserving ownership extraction**.

The one primary responsibility is:

> Convert one completed `XeFGDiscovery::Observation` into either a validated, strongly-owned XeFG binding candidate or a deterministic rejection result.

Do not begin R5 pending handoff or R6/R7 active binding ownership work in this PR.

---

# 2. Current State After R3

R3 established the first half of the discovery boundary:

```text
XeFG InitFromSwapChainDesc
    -> XeFGDiscovery::observe_init()
       - serialized transaction
       - temporary factory instance hook
       - CreateSwapChainForHwnd[15] capture
       - raw internal swapchain pointer
       - raw init queue
       - raw presentation queue
       - original InitDesc result
    -> D3D12Hook::publish_xefg_candidate(observation)
       - interface validation
       - HWND validation
       - queue/device COM identity validation
       - queue relation classification
       - selected queue decision
       - observe-only/render decision
       - PendingXefgBinding construction
       - immediate/pending lifecycle handoff
```

The split is still incomplete because `D3D12Hook.cpp` owns the policy that decides whether an observation is safe and usable.

Current master still contains XeFG-specific validation machinery conceptually equivalent to:

```cpp
enum class XefgQueueRelation {
    SameComIdentity,
    DistinctSameDevice,
    DeviceMismatch,
    InitQueueUnavailable,
    PresentationQueueUnavailable,
    PresentationQueueNotDirect,
};

struct QueueIdentitySnapshot {
    ID3D12CommandQueue* queue{};
    void* com_identity{};
    void* device_identity{};
    D3D12_COMMAND_QUEUE_DESC desc{};
    bool device_available{};
    bool valid{};
};

QueueIdentitySnapshot capture_queue_identity(...);
void log_xefg_queue_identity(...);
```

and `D3D12Hook::publish_xefg_candidate()` currently performs all of the following before lifecycle handoff:

```text
InitDesc success check
factory capture success check
IDXGISwapChain3 QI
GetHwnd comparison
candidate device retrieval
candidate device COM identity retrieval
init queue identity capture
presentation queue identity capture
queue relation classification
candidate device vs init queue device comparison
selected queue choice
render vs observe-only choice
rejection / probe reason selection
```

R4 moves those responsibilities out of `D3D12Hook`.

---

# 3. Target Architecture After R4

The desired flow after this PR is:

```text
XeFGRuntimeRegistry (R1)
    |
    v
D3D12Hook::xefg_init_desc_dispatch()
    |
    v
XeFGDiscovery::observe_init()                 (R3)
    |
    | raw observation
    v
XeFGDiscovery::build_binding_candidate()      (R4)
    |- validate internal swapchain interface
    |- validate HWND
    |- capture queue/device COM identities
    |- classify queue relation
    |- validate candidate device identity
    |- choose authoritative queue
    |- choose render vs observe-only
    |- return strong candidate or rejection
    |
    v
D3D12Hook::publish_xefg_candidate()
    |- log candidate outcome / existing probe mode
    |- copy candidate into current PendingXefgBinding
    |- perform current immediate/live handoff
    |- or store current pending binding
    |
    v
existing D3D12Hook binding implementation
```

R4 therefore separates:

```text
what XeFG created       = R3 Observation
is it safe/useful       = R4 Candidate validation
when to deliver it      = R5 (NOT THIS PR)
how active binding owns = R6/R7 (NOT THIS PR)
```

---

# 4. Preferred File Strategy

**Preferred:** extend the existing R3 discovery component rather than create another compatibility subsystem.

Expected files:

```text
MODIFY src/compatibility/xefg/XeFGDiscovery.hpp
MODIFY src/compatibility/xefg/XeFGDiscovery.cpp
MODIFY src/D3D12Hook.cpp
MODIFY src/D3D12Hook.hpp only if signature/type exposure requires it
```

Normally unchanged:

```text
src/REFramework.cpp
src/compatibility/xefg/XeFGCompatibility.*
src/compatibility/xefg/XeFGRuntimeRegistry.*
cmake.toml
CMakeLists.txt
src/mods/*
shared/*
```

Because R4 should not need new source files, **no CMake change is expected**.

If implementation genuinely requires a new `.cpp/.hpp`, remember PR #19: the checked-in `CMakeLists.txt` must also list it. However, adding a separate generic candidate subsystem is not preferred for R4.

Do not regenerate the entire CMake file just for this work.

---

# 5. Move into `XeFGDiscovery` in R4

Move the following ownership from `D3D12Hook.cpp` into the XeFG compatibility/discovery layer:

## 5.1 Queue relation type

Move/rename current:

```cpp
XefgQueueRelation
```

to a reusable XeFG-scoped type, preferably:

```cpp
enum class XeFGQueueRelation : uint8_t {
    SameComIdentity,
    DistinctSameDevice,
    DeviceMismatch,
    InitQueueUnavailable,
    PresentationQueueUnavailable,
    PresentationQueueNotDirect,
};
```

Do not add new relation states unless required to preserve a current distinction.

## 5.2 Queue identity snapshot and capture

Move:

```text
QueueIdentitySnapshot
capture_queue_identity()
queue_relation_name()
queue_type_name()       if only used by XeFG validation diagnostics
log_xefg_queue_identity()
```

These are XeFG candidate-validation implementation details and should no longer live in generic `D3D12Hook.cpp`.

## 5.3 Internal swapchain validation

Move the current checks for:

```text
raw internal swapchain exists
QueryInterface(IDXGISwapChain3)
GetHwnd succeeds
candidate HWND == observation HWND
candidate device is available
candidate device COM identity is available
```

## 5.4 Queue relation classification

Preserve the exact current classification order:

```cpp
if (!init_queue.valid) {
    relation = XeFGQueueRelation::InitQueueUnavailable;
} else if (!presentation_queue.valid) {
    relation = XeFGQueueRelation::PresentationQueueUnavailable;
} else if (init_queue.device_identity != presentation_queue.device_identity) {
    relation = XeFGQueueRelation::DeviceMismatch;
} else if (presentation_queue.desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
    relation = XeFGQueueRelation::PresentationQueueNotDirect;
} else if (init_queue.com_identity == presentation_queue.com_identity) {
    relation = XeFGQueueRelation::SameComIdentity;
} else {
    relation = XeFGQueueRelation::DistinctSameDevice;
}
```

**Order matters. Do not casually reorder these checks.**

## 5.5 Candidate device vs init queue device check

Preserve the current safety condition:

```text
candidate swapchain device identity must match init queue device identity
```

A mismatch remains a hard rejection (`device_mismatch`).

## 5.6 Selected queue + rendering mode decision

Preserve the current policy exactly:

```text
DistinctSameDevice
    -> selected queue = captured presentation queue
    -> observe_only = false
    -> REFramework renderer callbacks allowed

SameComIdentity
PresentationQueueUnavailable
DeviceMismatch (between init queue and presentation queue)
PresentationQueueNotDirect
    -> selected queue = init queue
    -> observe_only = true
    -> original Present/Present1 liveness remains observable
    -> no overlay submission through unproven presentation path
```

Important distinction:

> `XeFGQueueRelation::DeviceMismatch` means the **captured presentation queue** does not belong to the same device as the init queue. Under current policy this is an observe-only fallback, not automatically a total observation rejection, provided the internal swapchain itself matches the init queue device.

Do not accidentally convert this current observe-only fallback into a hard rejection.

---

# 6. Strong Binding Candidate

R4 should produce a candidate that owns everything required for safe downstream delivery.

Recommended type:

```cpp
struct XeFGBindingCandidate {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> selected_queue{};
    Microsoft::WRL::ComPtr<ID3D12Device4> device{};
    HWND hwnd{};
    XeFGQueueRelation relation{XeFGQueueRelation::InitQueueUnavailable};
    bool observe_only{true};

    bool valid() const noexcept {
        return swapchain != nullptr
            && selected_queue != nullptr
            && device != nullptr;
    }
};
```

The exact field name `selected_queue` is preferred over `presentation_queue` because the current observe-only path intentionally uses the **init queue**, not the captured presentation queue.

The candidate is conceptually immutable after construction:

```text
validate all inputs
choose final queue/mode
construct candidate once
hand candidate downstream
```

Do not expose setters or mutate its identity after it is returned.

---

# 7. Candidate Device Ownership

The R4 candidate should strongly own the internal swapchain's D3D12 device.

Preferred acquisition:

```cpp
Microsoft::WRL::ComPtr<ID3D12Device4> candidate_device;
if (FAILED(candidate_swapchain->GetDevice(IID_PPV_ARGS(&candidate_device)))) {
    // reject
}
```

For COM identity comparison, obtain `IUnknown` identity from this device or from an equivalent base `ID3D12Device` interface.

The candidate device must represent the device of the **internal XeFG presentation swapchain**.

Do not use:

- a device inferred from the public XeFG proxy;
- a random global REFramework device;
- only the init queue's device without checking the internal swapchain;
- an OptiScaler-private device pointer.

### Failure timing note

Current downstream `bind_external_swapchain()` ultimately requires an `ID3D12Device4` from the selected swapchain. R4 may reject earlier if that interface is unavailable, because such a candidate is not bindable by the existing implementation.

If this happens, keep the failure explicit and localized. Do not add a fallback device path.

---

# 8. Recommended Validation Result Type

Do not force `D3D12Hook` to reconstruct policy reasons after validation has moved.

Recommended shape:

```cpp
struct XeFGBindingCandidateResult {
    std::optional<XeFGBindingCandidate> candidate{};
    const char* reject_reason{};
    const char* bind_reason{"init_success"};
    const char* probe_reason{"distinct_same_device"};

    bool accepted() const noexcept {
        return candidate.has_value();
    }
};
```

Or an equivalent enum-backed design is acceptable.

A stronger typed version is also acceptable:

```cpp
enum class XeFGCandidateRejectReason : uint8_t {
    None,
    InitFailed,
    NoCandidate,
    QueueDeviceUnavailable,
    NoIdxgiSwapchain3,
    HwndMismatch,
    CandidateDeviceUnavailable,
    DeviceMismatch,
};
```

with conversion to existing log strings at the boundary.

Avoid storing `std::string` for fixed internal reason values unless there is a concrete need.

---

# 9. Recommended `XeFGDiscovery` API

Conceptual example:

```cpp
class XeFGDiscovery {
public:
    // R3
    struct Observation { /* existing fields */ };
    class ObservationScope { /* existing transaction lock lifetime */ };

    // R4
    static XeFGBindingCandidateResult build_binding_candidate(
        const Observation& observation);
};
```

Alternative names such as:

```text
validate_observation()
evaluate_observation()
make_binding_candidate()
```

are fine.

The API should communicate that it is deterministic transformation from one completed observation to one decision.

Do not pass `D3D12Hook&`, `REFramework&`, or renderer state into this function.

That would collapse R4 into R5/R7.

---

# 10. Detailed Validation Algorithm

The R4 builder should remain semantically equivalent to the current first half of `D3D12Hook::publish_xefg_candidate()`.

Conceptual implementation:

```cpp
XeFGBindingCandidateResult XeFGDiscovery::build_binding_candidate(
    const Observation& observation) {
    XeFGBindingCandidateResult result{};

    if (observation.init_result != kXefgSuccess) {
        result.reject_reason = "init_failed";
        return result;
    }

    if (!observation.factory_create_succeeded
        || observation.internal_swapchain == nullptr) {
        result.reject_reason = "no_candidate";
        return result;
    }

    if (observation.init_queue == nullptr) {
        result.reject_reason = "queue_device_unavailable";
        return result;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
    if (FAILED(observation.internal_swapchain->QueryInterface(
            IID_PPV_ARGS(&swapchain)))) {
        result.reject_reason = "no_idxgi_swapchain3";
        return result;
    }

    HWND candidate_hwnd{};
    if (FAILED(swapchain->GetHwnd(&candidate_hwnd))
        || candidate_hwnd != observation.hwnd) {
        result.reject_reason = "hwnd_mismatch";
        return result;
    }

    Microsoft::WRL::ComPtr<ID3D12Device4> device;
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device)))) {
        result.reject_reason = "candidate_device_unavailable";
        return result;
    }

    Microsoft::WRL::ComPtr<IUnknown> candidate_device_identity;
    if (FAILED(device.As(&candidate_device_identity))) {
        result.reject_reason = "candidate_device_unavailable";
        return result;
    }

    const auto init_queue = capture_queue_identity(observation.init_queue);
    const auto presentation_queue =
        capture_queue_identity(observation.presentation_queue);

    const auto relation = classify_queue_relation(
        init_queue, presentation_queue);

    log_queue_identity(
        observation.context,
        swapchain.Get(),
        init_queue,
        presentation_queue,
        relation);

    if (!init_queue.valid) {
        result.reject_reason = "queue_device_unavailable";
        return result;
    }

    if (candidate_device_identity.Get() != init_queue.device_identity) {
        result.reject_reason = "device_mismatch";
        return result;
    }

    XeFGBindingCandidate candidate{};
    candidate.swapchain = swapchain;
    candidate.device = device;
    candidate.hwnd = observation.hwnd;
    candidate.relation = relation;

    if (relation == XeFGQueueRelation::DistinctSameDevice) {
        candidate.selected_queue = presentation_queue.queue;
        candidate.observe_only = false;
        result.bind_reason = "init_success";
        result.probe_reason = "distinct_same_device";
    } else {
        candidate.selected_queue = init_queue.queue;
        candidate.observe_only = true;
        result.bind_reason = "init_success_observe_only";
        result.probe_reason = probe_reason_for(
            relation, observation, presentation_queue);
    }

    if (!candidate.valid()) {
        // This should map to an existing meaningful rejection path.
        result.reject_reason = "queue_device_unavailable";
        return result;
    }

    result.candidate = std::move(candidate);
    return result;
}
```

This is a structural example, not a mandate to copy exact syntax.

---

# 11. Preserve Existing Probe-Reason Semantics

Current master emits different probe reasons for observe-only fallback.

Preserve these meanings:

```text
SameComIdentity
    -> same_com_identity

PresentationQueueUnavailable
    -> presentation_queue_unavailable
       OR presentation_queue_device_unavailable
       according to current device-availability diagnostic

DeviceMismatch
    -> presentation_queue_device_mismatch

PresentationQueueNotDirect
    -> presentation_queue_not_direct

DistinctSameDevice
    -> distinct_same_device
```

Do not rename these strings in R4 unless required by compilation.

Logging cleanup is intentionally deferred to R11.

---

# 12. What `D3D12Hook::publish_xefg_candidate()` Should Look Like After R4

After R4, this function should no longer contain COM identity classification or queue policy.

Conceptually:

```cpp
void D3D12Hook::publish_xefg_candidate(
    const XeFGDiscovery::Observation& observation) {
    XeFGBindingCandidateResult decision{};

    {
        // Preserve current lock ordering/scope unless there is a proven reason
        // to narrow it in a later dedicated PR.
        std::scoped_lock lock{g_xefg_state_mutex};
        decision = XeFGDiscovery::build_binding_candidate(observation);

        if (!decision.accepted()) {
            spdlog::warn(
                "[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = {}",
                reinterpret_cast<uintptr_t>(observation.internal_swapchain),
                decision.reject_reason);
        } else {
            const auto& candidate = *decision.candidate;
            spdlog::info(
                "[XeFG][Bind] candidate = 0x{:x}, accepted = true, reason = {}",
                reinterpret_cast<uintptr_t>(candidate.swapchain.Get()),
                decision.bind_reason);

            spdlog::info(
                "[XeFG][P2.1Probe] mode = {}, selected_queue = 0x{:x}, "
                "render_callbacks = {}, reason = {}",
                candidate.observe_only
                    ? (candidate.relation == XeFGQueueRelation::SameComIdentity
                        ? "observe_only_same_queue"
                        : "observe_only_invalid_presentation_queue")
                    : "presentation_queue_render",
                reinterpret_cast<uintptr_t>(candidate.selected_queue.Get()),
                !candidate.observe_only,
                decision.probe_reason);
        }
    }

    if (!decision.accepted()) {
        return;
    }

    const auto& candidate = *decision.candidate;

    PendingXefgBinding pending{};
    pending.swapchain = candidate.swapchain;
    pending.selected_queue = candidate.selected_queue;
    pending.hwnd = candidate.hwnd;
    pending.relation = candidate.relation;
    pending.observe_only = candidate.observe_only;

    // EVERYTHING BELOW HERE stays behaviorally unchanged in R4.
    // Existing immediate/live handoff + pending fallback remains in D3D12Hook.
    ...
}
```

The important boundary is:

```text
candidate construction ends
-------------------------- R4 boundary
pending/live delivery begins
```

Do not move the code below that boundary yet.

---

# 13. Pending Binding Remains in `D3D12Hook`

R4 must **not** remove or redesign:

```cpp
struct PendingXefgBinding {
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12CommandQueue> selected_queue;
    HWND hwnd;
    XeFGQueueRelation relation;
    bool observe_only;
};

std::optional<PendingXefgBinding> g_pending_xefg_binding;
```

It is acceptable that R4 temporarily has both:

```text
XeFGBindingCandidate      = validated immutable R4 output
PendingXefgBinding        = existing R5 delivery envelope
```

This duplication is intentional for PR isolation.

R5 will remove the handoff duplication later.

Do not collapse R4 and R5 just to reduce a few lines.

---

# 14. Locking Requirements

R4 should not redesign lock ownership.

Current relevant ordering after R3 is conceptually:

```text
XeFGDiscovery transaction mutex
    held across observation + D3D12Hook publication

D3D12Hook g_xefg_state_mutex
    acquired during current candidate validation / pending-state operations

framework hook-monitor lifecycle mutex
    acquired later for immediate binding delivery
```

Preserve this ordering.

In particular:

- do not acquire the framework hook-monitor mutex from candidate validation;
- do not call renderer reset from candidate validation;
- do not call `bind_external_swapchain()` from `XeFGDiscovery`;
- do not call `replace_xefg_binding()` from `XeFGDiscovery`;
- do not introduce a new global mutex solely around immutable candidate objects;
- do not introduce polling, sleeps, timers, or background work.

The candidate builder must be a bounded synchronous operation.

---

# 15. COM Identity Rules

R4 must preserve COM identity semantics, not raw interface-pointer assumptions.

For queue identity:

```cpp
queue->QueryInterface(IID_PPV_ARGS(&queue_identity));
queue->GetDevice(...);
device.As(&device_identity);
```

Comparisons must remain based on canonical COM `IUnknown` identity where current code uses that identity.

Do not replace:

```text
IUnknown identity comparison
```

with:

```text
raw typed-interface pointer equality
```

unless proving they are equivalent for the exact comparison, which is out of scope.

The current distinction between:

```text
same COM queue identity
vs
distinct queue objects on the same device
```

is essential to the proven XeFG path.

---

# 16. The Proven Render Path Must Not Change

For the known working OptiScaler + XeFG path:

```text
init queue              = DIRECT queue A
presentation queue      = DIRECT queue B
A != B by COM identity
A.device == B.device
internal swapchain.device == A.device
```

R4 must produce:

```text
relation       = DistinctSameDevice
selected_queue = presentation queue B
observe_only   = false
```

This is the path that allows the REFramework overlay to render on the XeFG internal post-frame-generation presentation boundary.

Do not accidentally choose the init queue because it is easier to validate.

Do not choose the public XeFG `GetSwapChainPtr` proxy.

---

# 17. Observe-Only Paths Must Also Stay Alive

Current fallback behavior is deliberate.

When the presentation queue path is not proven safe, REFramework still needs to preserve original Present/Present1 forwarding and instance liveness without submitting overlay GPU work through an unproven queue.

Therefore these current states remain observe-only candidate paths where current policy allows them:

```text
SameComIdentity
PresentationQueueUnavailable
DeviceMismatch between init/presentation queues
PresentationQueueNotDirect
```

Candidate construction in these cases should continue to use:

```text
selected_queue = init queue
observe_only = true
```

provided the core internal-swapchain/init-queue device validation succeeds.

Do not reinterpret `observe_only` as rejection.

---

# 18. Hard Rejection Paths

The following current conditions should continue to yield **no candidate**:

```text
original XeFG InitDesc failed
factory did not produce an internal candidate
internal swapchain pointer missing
init queue missing / identity unavailable
internal object cannot provide IDXGISwapChain3
internal swapchain HWND does not match InitDesc HWND
internal swapchain device unavailable
internal swapchain device identity unavailable
internal swapchain device != init queue device
final selected queue unavailable
candidate cannot provide the device interface required by current binding
```

No hard rejection may reset or unhook the current good REFramework/XeFG binding.

R4 returns a rejection result and stops before lifecycle mutation.

---

# 19. Failure Isolation Requirement

This PR must preserve the core invariant:

```text
invalid new observation
    -> no validated candidate
    -> no pending/live delivery
    -> current good binding untouched
```

Do not add behavior such as:

```text
candidate rejected
    -> unhook active XeFG binding
    -> reset renderer
    -> fall back to native immediately
```

Those actions would be a functional regression and a blocker.

---

# 20. `GetSwapChainPtr` Remains Diagnostic Only

R3 retained:

```cpp
XeFGDiscovery::current_internal_swapchain_for_diagnostics()
```

for comparison against the public XeFG `GetSwapChainPtr` result.

R4 must not convert the public proxy into:

- candidate source;
- selected render swapchain;
- fallback render target;
- ownership authority.

The public proxy remains diagnostic evidence only.

---

# 21. Do Not Move Runtime Registry or Loader Responsibilities

R1 and R2 ownership is settled for this series.

Do not modify behavior of:

```text
XeFGRuntimeRegistry
    exact HMODULE registration
    stable slots
    InitDesc/GetSwapChainPtr FunctionHook ownership
    original trampoline resolution

XeFGCompatibility
    loader notification pending state
    exact post-LdrLoadDll handoff
    already-loaded module enumeration
```

R4 begins only after a completed R3 observation exists.

---

# 22. PR #20 Resize Policy Is Frozen

Latest master still intentionally scopes XeFG ResizeTarget transition hold to Monster Hunter Wilds:

```cpp
if (event_id != 0
    && renderer_reset_performed
    && !d3d12->m_xefg_p21_observe_only
    && sdk::GameIdentity::get().is_mhwilds()) {
    d3d12->arm_xefg_resize_transition_hold(event_id);
}
```

R4 must not change:

- `is_mhwilds()` gating;
- ResizeTarget ordering;
- ResizeBuffers/ResizeBuffers1 behavior;
- hold arm/clear state;
- Present suppression;
- renderer reset ordering.

Any diff in those regions is presumptive scope leakage.

---

# 23. No Generalized Frame Generation Refactor

This repository fork is being optimized specifically for stable REFramework + OptiScaler XeFG coexistence.

R4 must not introduce:

```text
IFrameGenerationProvider
FrameGenerationCandidate
generic FG queue classifier
FSRFG provider
DLSSG provider
generic swapchain compatibility registry
```

Do not touch FSRFG/DLSSG just because queue-validation code could theoretically be shared.

Generalization has no value in this PR and increases upstream-maintenance cost.

---

# 24. No OptiScaler Private Dependencies

Do not reference or hook private OptiScaler implementation details such as:

```text
WrappedIDXGISwapChain4
FGHooks
MenuOverlayDx
private class layouts
private symbols
hard-coded offsets
```

The candidate must continue to be derived only from:

```text
public XeFG InitDesc observation
DXGI factory CreateSwapChainForHwnd capture
standard COM identity/device queries
```

---

# 25. Header / Upstream-Surface Guidance

Prefer to keep XeFG-only types in:

```text
src/compatibility/xefg/XeFGDiscovery.hpp
```

rather than expanding generic public `D3D12Hook.hpp` unnecessarily.

`D3D12Hook.hpp` already references the R3 observation type, so a minimal candidate-related signature change is acceptable if needed.

Do not expose:

- queue identity snapshots;
- raw relation-classification helpers;
- reject-reason internals;
- validation helper functions

through `D3D12Hook.hpp`.

The refactor objective is to reduce XeFG implementation surface in upstream-sensitive files.

---

# 26. Logging Policy for R4

Do not perform logging cleanup yet.

Preserve support-relevant log semantics including:

```text
[XeFG][QueueIdentity]
[XeFG][Bind]
[XeFG][P2.1Probe]
```

It is acceptable for `[XeFG][QueueIdentity]` to move physically into `XeFGDiscovery.cpp` because the snapshot data moves there.

It is preferable for final bind/probe outcome logs to remain near `D3D12Hook::publish_xefg_candidate()` for this PR, because R5 still owns the delivery boundary there.

Do not change normal/info/debug levels in R4.

R11 will handle logging policy and the persistent Debug Logging UI.

---

# 27. Suggested Helper Layout

One reasonable `XeFGDiscovery.cpp` internal layout is:

```cpp
namespace {

struct QueueIdentitySnapshot {
    ID3D12CommandQueue* queue{};
    void* com_identity{};
    void* device_identity{};
    D3D12_COMMAND_QUEUE_DESC desc{};
    bool device_available{};
    bool valid{};
};

QueueIdentitySnapshot capture_queue_identity(
    ID3D12CommandQueue* queue);

XeFGQueueRelation classify_queue_relation(
    const QueueIdentitySnapshot& init,
    const QueueIdentitySnapshot& presentation);

const char* queue_relation_name(XeFGQueueRelation relation);
const char* queue_type_name(D3D12_COMMAND_LIST_TYPE type);

void log_queue_identity(
    void* context,
    IDXGISwapChain3* swapchain,
    const QueueIdentitySnapshot& init,
    const QueueIdentitySnapshot& presentation,
    XeFGQueueRelation relation);

const char* probe_reason_for(
    XeFGQueueRelation relation,
    const XeFGDiscovery::Observation& observation,
    const QueueIdentitySnapshot& presentation);

} // namespace
```

Keep these implementation-only helpers private to the `.cpp` where possible.

---

# 28. Do Not Add Unnecessary Tests-by-Mocking COM

This codebase does not currently have an isolated COM mock harness for this path.

Do not inflate R4 with a large fake DXGI/D3D12 test framework solely for this extraction.

Primary validation is:

- compile/static audit;
- exact logic comparison against pre-R4 master;
- runtime discovery wave after R4.

Small deterministic helper tests are welcome only if the repository already has a natural place for them and they do not force architecture changes.

---

# 29. Build / Source Registration Gate

Because R4 should modify existing R3 files only, source registration should already be correct.

Required checks:

```text
XeFGDiscovery.cpp is present in tracked CMakeLists.txt
XeFGDiscovery.hpp is present in tracked CMakeLists.txt
no new source omitted from CMake target
```

If no new files are added:

```text
CMakeLists.txt should remain unchanged.
```

Do not modify `cmake.toml`; its recursive glob is already correct.

---

# 30. Static Review Checklist

Before opening the PR, verify all of the following.

## Ownership

- [ ] `XefgQueueRelation` equivalent no longer lives as a D3D12Hook.cpp-specific type.
- [ ] queue identity snapshot/capture moved out of D3D12Hook.cpp.
- [ ] relation classification moved out of D3D12Hook.cpp.
- [ ] candidate interface/HWND/device validation moved out.
- [ ] queue selection and observe-only decision moved out.
- [ ] validated candidate owns swapchain + selected queue + device strongly.

## R4 boundary

- [ ] `PendingXefgBinding` remains in D3D12Hook.
- [ ] `g_pending_xefg_binding` remains in D3D12Hook.
- [ ] immediate existing-hook delivery remains in D3D12Hook.
- [ ] capture-before-hook pending fallback remains unchanged.
- [ ] `consume_pending_xefg_binding()` remains behaviorally unchanged.
- [ ] `bind_external_swapchain()` unchanged.
- [ ] `replace_xefg_binding()` unchanged.

## Proven policy

- [ ] DistinctSameDevice selects presentation queue and render mode.
- [ ] SameComIdentity stays observe-only.
- [ ] unavailable presentation queue stays observe-only.
- [ ] init/presentation queue device mismatch stays observe-only under current fallback policy.
- [ ] non-DIRECT presentation queue stays observe-only.
- [ ] internal swapchain device mismatch vs init queue hard-rejects.
- [ ] public GetSwapChainPtr remains diagnostic only.

## Scope protection

- [ ] no `REFramework.cpp` changes.
- [ ] no loader changes.
- [ ] no runtime registry changes.
- [ ] no Present/resize changes.
- [ ] no MHW hold-policy changes.
- [ ] no FSRFG/DLSSG changes.
- [ ] no logging cleanup.
- [ ] no generic provider abstraction.

---

# 31. Required Build / Mechanical Validation

Use latest master as the base.

Minimum required:

```powershell
cmake -S . -B build
cmake --build build --config Release --target REFramework -- /m:4
git diff --check
```

Also inspect:

```powershell
git diff --stat master...HEAD
git diff master...HEAD -- src/D3D12Hook.cpp src/D3D12Hook.hpp src/compatibility/xefg/XeFGDiscovery.cpp src/compatibility/xefg/XeFGDiscovery.hpp
```

Expected primary diff:

```text
src/compatibility/xefg/XeFGDiscovery.hpp
src/compatibility/xefg/XeFGDiscovery.cpp
src/D3D12Hook.cpp
possibly minimal src/D3D12Hook.hpp
```

Unexpected without strong justification:

```text
src/REFramework.cpp
src/mods/*
shared/*
cmake.toml
CMakeLists.txt
XeFGCompatibility.*
XeFGRuntimeRegistry.*
```

---

# 32. Runtime Gate After R4

R4 is a designated runtime-wave boundary in the fine-grained plan.

After code/build review passes, run the discovery smoke when hardware/game access is available.

## 32.1 Dragon's Dogma 2 + OptiScaler + XeFG

Expected:

```text
game launches
OptiScaler initializes XeFG
REFramework does not crash during InitDesc
internal XeFG presentation swapchain still selected
DistinctSameDevice path still detected where expected
actual presentation queue remains selected
observe_only = false on proven path
OptiScaler overlay appears
REFramework overlay appears
no repeated destructive rehook loop
```

Relevant logs should still prove:

```text
[XeFG][InternalSwapchain]
[XeFG][QueueIdentity]
[XeFG][Bind]
[XeFG][P2.1Probe]
[D3D12][ExternalBind]
```

## 32.2 Multi-runtime / Pragmata case when available

Expected:

```text
exact-HMODULE runtime dispatch still correct
multiple libxess_fg.dll modules do not collapse to one original trampoline
candidate validation uses the observation from the currently dispatched InitDesc
no registry regression
```

## 32.3 Native / non-XeFG sanity

At minimum audit that when XeFG is absent:

```text
native REFramework D3D12 path is unchanged
D3D11 path is unchanged
FSRFG/DLSSG branches are unchanged
```

---

# 33. Runtime Failure Triage

If R4 runtime smoke fails, classify before broad changes.

## Case A — `no_idxgi_swapchain3` / `hwnd_mismatch`

Check whether R4 changed interface/HWND validation semantics from master.

Do not weaken validation just to make the candidate pass.

## Case B — queue relation changed

Compare old and new:

```text
init queue raw pointer
init queue IUnknown identity
init queue device identity
presentation queue raw pointer
presentation queue IUnknown identity
presentation queue device identity
presentation queue type
```

If the same observation now classifies differently, treat as R4 regression.

## Case C — previously render path becomes observe-only

Check classification order and selected queue assignment first.

The proven path must remain:

```text
DistinctSameDevice -> captured presentation queue -> observe_only=false
```

## Case D — candidate rejected because device4 unavailable

Confirm whether current downstream master would also fail `bind_external_swapchain()` on the same interface acquisition.

Do not add an alternate device source without a separate design review.

## Case E — binding/lifecycle issue after accepted candidate

If validation output is identical to pre-R4 but live handoff fails, do not expand R4 into R5/R7. Revert/fix only any accidental handoff diff; otherwise classify the issue separately.

---

# 34. Blocking Review Findings

Treat these as blockers:

1. **DistinctSameDevice no longer selects the captured presentation queue.**
2. **Public XeFG proxy becomes a binding source.**
3. **Candidate validation mutates active D3D12 binding state.**
4. **Rejected candidate unhooks/resets a current good binding.**
5. **Presentation queue mismatch/non-DIRECT path is silently promoted to render mode.**
6. **Current observe-only fallback is accidentally converted to hard rejection without explicit design justification.**
7. **Internal swapchain device mismatch vs init queue is accepted.**
8. **COM identity comparison is replaced by unsafe raw typed-pointer assumptions.**
9. **Pending/live handoff is substantially moved into XeFGDiscovery (R5 leakage).**
10. **`bind_external_swapchain()` or `replace_xefg_binding()` behavior changes (R6/R7 leakage).**
11. **Present/resize or MHW-only hold logic changes.**
12. **FSRFG/DLSSG/general FG abstraction is added.**
13. **New source file is added but omitted from tracked CMake target.**
14. **Release build fails.**

---

# 35. Non-Blocking / Theoretical Findings

Do not block R4 solely for:

- naming preferences between `evaluate`, `validate`, or `build`;
- private helper ordering inside `XeFGDiscovery.cpp`;
- whether fixed reason strings use `const char*` or a small enum;
- theoretical module unload scenarios not supported by current design;
- hypothetical >8 XeFG runtime scenarios (R1 capacity intentionally unchanged);
- general-purpose FG extensibility;
- style-only requests unrelated to safety/ownership;
- logging verbosity that is already scheduled for R11.

The project review policy is to block realistic/material defects, not speculative robustness work.

---

# 36. Suggested PR Description

```markdown
## Summary

- move XeFG queue/device COM identity validation into `XeFGDiscovery`
- extract queue relation classification and selected-queue/render-mode policy
- construct a strongly-owned validated `XeFGBindingCandidate`
- keep current pending/live D3D12 binding handoff unchanged

## Preserved behavior

- `DistinctSameDevice` still selects the captured XeFG presentation queue for rendering
- unproven presentation queue paths remain observe-only using the init queue
- internal swapchain device mismatch still rejects the candidate
- public `GetSwapChainPtr` remains diagnostic only
- no Present/resize/MHW hold behavior changes

## Not in scope

- pending candidate handoff extraction (R5)
- active binding ownership (R6)
- transactional rebind changes (R7)
- resize/Present cleanup (R8-R10)
- logging cleanup / Debug Logging UI (R11)

## Validation

- Release build
- `git diff --check`
- static comparison of pre/post queue classification and selected queue policy
- R4 discovery runtime smoke when available
```

---

# 37. Expected Size

Target from the refactor split plan:

```text
effective:        ~150–250 LOC
GitHub add+delete: ~280–500 LOC
```

Because this is mainly a move from `D3D12Hook.cpp` into the existing R3 component, GitHub may show more add/delete than effective logic changed.

Do not split the queue-validation transaction merely to hit a line-count target.

Do not combine R5 just because R4 happens to be smaller than expected.

---

# 38. Definition of Done

R4 is complete only when all of the following are true:

```text
R3 Observation remains raw discovery output
        +
XeFG queue/device/interface/HWND validation lives outside D3D12Hook
        +
queue relation classification lives outside D3D12Hook
        +
selected queue + observe-only/render decision lives outside D3D12Hook
        +
validated candidate strongly owns swapchain/selected queue/device
        +
D3D12Hook receives accepted candidate or rejection result
        +
PendingXefgBinding/live delivery remains behaviorally unchanged
        +
active bind/rebind implementation remains unchanged
        +
Present/resize/MHW hold remains unchanged
        +
Release build passes
        +
`git diff --check` passes
```

At that point the architecture boundary is:

```text
R1: runtime/export ownership
R2: loader/probe handoff
R3: InitDesc/factory observation
R4: queue validation + immutable candidate
R5: pending/live candidate delivery
```

Do **not** start R5 in this PR.
