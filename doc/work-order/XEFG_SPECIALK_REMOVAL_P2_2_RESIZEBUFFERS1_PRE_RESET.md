# XeFG Special K Removal — P2.2 ResizeBuffers1 Pre-Reset

## 1. Purpose

P2.1 established two important facts on real Intel hardware with Dragon's Dogma 2:

1. The queue passed to public `xefgSwapChainD3D12InitFromSwapChainDesc` is **not** the same COM queue as the queue used when XeFG creates the internal presentation swapchain.
2. Rendering REFramework through the actual presentation-creation queue removes the earlier P2 `DXGI_ERROR_DEVICE_REMOVED / DXGI_ERROR_ACCESS_DENIED` failure, but exposes a new and much more specific failure during `IDXGISwapChain3::ResizeBuffers1`.

This P2.2 work order fixes only that newly identified lifecycle gap.

The required behavior is:

```text
XeFG internal ResizeBuffers1[39] entry
        ↓
REFramework releases its D3D12 renderer/backbuffer references
        ↓
call the preserved original ResizeBuffers1 with all arguments unchanged
        ↓
return the original HRESULT unchanged
```

This is a **code-change-only** work order.

Codex must:

```text
modify the code
add the ResizeBuffers1 hook/lifecycle handling
add bounded diagnostics
run static/build validation
produce the PR/artifact
stop
```

Codex must **not** run Dragon's Dogma 2 or Monster Hunter Wilds, must not claim runtime success, and must not decide the next architecture step from CI or simulated evidence.

The user will run the resulting build on real Intel hardware and provide the logs separately for analysis.

---

## 2. Repository / Required Base

Repository:

```text
onehoon/REFramework
```

Required base at the time this work order was written:

```text
master
eec9dbadd9846f0e7983fc7d04a7a5f995534444
feat: add XeFG P2.1 queue render probe (#9)
```

If `master` has advanced, rebase onto the current `master` and preserve all merged P2/P2.1 behavior.

Primary files expected to change:

```text
src/D3D12Hook.cpp
src/D3D12Hook.hpp
```

A change to `src/REFramework.cpp` should **not** be necessary. The existing callback wiring already does exactly what P2.2 needs:

```cpp
m_d3d12_hook->on_resize_buffers([this](D3D12Hook& hook) {
    on_reset();
});

m_d3d12_hook->on_resize_target([this](D3D12Hook& hook) {
    on_reset();
});
```

Do not add a second renderer-reset implementation when the existing `m_on_resize_buffers` callback can be reused.

Keep this PR small and focused. This should remain comfortably below ~300 LOC.

---

## 3. Scope

The target runtime remains:

```text
OptiScaler   = dxgi.dll
REFramework  = dinput8.dll (this fork)
Special K    = absent
FG Output    = Intel XeFG
Renderer     = D3D12
```

Primary user validation target after the PR is built:

```text
Intel GPU + Dragon's Dogma 2
```

Monster Hunter Wilds may be tested by the user later, but Codex must not run either game.

### Explicitly in scope

```text
IDXGISwapChain3::ResizeBuffers1 vtable slot 39
XeFGInternal swapchain only
pre-ResizeBuffers1 REFramework renderer reset
forwarding every original ResizeBuffers1 argument unchanged
bounded diagnostic logging
safe reentrancy handling
preserving P2.1 presentation queue selection
```

### Explicitly out of scope

```text
full XeFG swapchain recreation/rebind state machine
capturing a replacement internal swapchain
ResizeTarget redesign
fullscreen lifecycle redesign
Alt+Tab lifecycle redesign
manual COM refcount manipulation
manual backbuffer Release loops
OptiScaler code changes
Special K compatibility
native/no-FG acceptance work
FSRFG
NVIDIA MHW release-storm investigation
removing P2.1 queue diagnostics
public XefgInterpolationSwapChain rendering
```

P2.2 is intentionally the **minimum lifecycle fix required to let the current P2.1 direct-render path survive an actual XeFG `ResizeBuffers1`**.

---

## 4. Runtime Evidence That Motivates This Fix

The P2.1 DD2 run used the PR artifact and identified the build as:

```text
Commit hash: 2fe540b2063ac94438cb3976a9647c1fab22926e
Build time: 05.09.2026 16:32
Game name: dd2
```

The PR artifact was built from the P2.1 code that was later squash-merged to `master` as `eec9dbadd9846f0e7983fc7d04a7a5f995534444`.

### 4.1 Queue identity result

P2.1 logged:

```text
[XeFG][QueueIdentity]
init_queue                    = 0x18fd6865f80
init_identity                 = 0x18fd6865f50
init_device_identity          = 0x18fbb28a820
init_type                     = direct
init_priority                 = 0

presentation_queue            = 0x18fd68657a0
presentation_identity         = 0x18fd6865770
presentation_device_identity  = 0x18fbb28a820
presentation_type             = direct
presentation_priority         = 100

relation = distinct_same_device
```

Therefore P2.1 selected the presentation queue:

```text
[XeFG][P2.1Probe]
mode = presentation_queue_render
selected_queue = 0x18fd68657a0
render_callbacks = true
reason = distinct_same_device
```

The external bind then used that exact queue:

```text
[D3D12][ExternalBind]
source    = xefg_internal
swapchain = 0x18fe0253490
queue     = 0x18fd68657a0
```

### 4.2 The old P2 device-removed failure did not recur

The first XeFG-internal `Present1` reached REFramework and the P2.1 render-boundary probe completed:

```text
01:46:41.572 [XeFG][P2.1Probe] render_callback = enter, present_call = 1
01:46:41.573 [XeFG][P2.1Probe] render_callback = returned, present_call = 1
```

OptiScaler then reported normal presentation results repeatedly:

```text
LocalPresent Original present result: 0
FGHooks::hkFGPresent1 o_FGSCPresent result: 0
FGHooks::FGPresent Result: 0
```

The earlier P2 failure:

```text
DXGI_ERROR_DEVICE_REMOVED (0x887A0005)
DeviceRemovedReason = 0x887A002B
```

was not the leading failure in this P2.1 run.

Do not revert the P2.1 presentation-queue selection in P2.2.

### 4.3 First ResizeBuffers1 succeeds after REFramework reset

At approximately `01:46:44.441`, REFramework sees a resize target transition and executes its existing reset path:

```text
D3D12 resize target called
Reset!
```

Immediately afterwards OptiScaler/XeFG performs:

```text
WrappedIDXGISwapChain4::ResizeBuffers1
BufferCount: 3
Width: 1920
Height: 1200
NewFormat: 24
SwapChainFlags: 842

WrappedIDXGISwapChain4::ResizeBuffers1 result: 0
```

This resize succeeds.

### 4.4 REFramework then reacquires swapchain backbuffers

After that successful resize, REFramework recreates its D3D12 renderer resources:

```text
01:46:44.724 [D3D12] Creating render targets...
01:46:44.724 [D3D12] Swapchain buffer count: 3
01:46:44.724 [D3D12] Back buffer format is 24
01:46:44.730 [D3D12] Initializing ImGui...
01:46:44.730 REFramework initialized
```

OptiScaler presentation still succeeds immediately afterwards:

```text
01:46:44.732 LocalPresent Original present result: 0
```

### 4.5 Second ResizeBuffers1 fails because REFramework does not see slot 39

The next resize begins with:

```text
01:46:44.733 FGHooks::hkResizeBuffers
BufferCount: 3
Width: 1280
Height: 720
NewFormat: 24
SwapChainFlags: 842
```

OptiScaler sees all three existing backbuffers:

```text
Backbuffer 0: 1905701C480
Backbuffer 1: 1905701B920
Backbuffer 2: 1905701B780
```

Then the XeFG internal wrapper calls native `ResizeBuffers1`:

```text
01:46:45.111 WrappedIDXGISwapChain4::ResizeBuffers1
01:46:45.213 WrappedIDXGISwapChain4::ResizeBuffers1 result: 887A0001
01:46:45.213 XeFG Log: Native ResizeBuffers1 failed with 0x887a0001
```

The DD2 crash report ends with:

```text
Fatal D3D error (22, DXGI_ERROR_INVALID_CALL, 0x887a0001)
```

There is **no REFramework `Reset!` immediately before this second failing `ResizeBuffers1`**.

The current XeFG external bind hooks:

```text
Present[8]
Present1[22]
ResizeBuffers[13]
ResizeTarget[14]
```

but not:

```text
ResizeBuffers1[39]
```

The evidence therefore supports this concrete lifecycle gap:

```text
REFramework reacquires swapchain backbuffers
        ↓
XeFG later invokes ResizeBuffers1[39]
        ↓
REFramework does not receive a pre-resize callback
        ↓
REFramework-owned backbuffer / RTV / renderer references remain alive
        ↓
native ResizeBuffers1 returns DXGI_ERROR_INVALID_CALL
```

P2.2 must fix exactly this missing hook path.

---

## 5. Existing Code to Preserve

### 5.1 Current external XeFG bind

Current `bind_external_swapchain()` already centralizes the XeFG internal vtable hooks:

```cpp
m_swapchain_hook = std::make_unique<VtableHook>(Address{swapchain});
m_swapchain_hook->hook_method(8, Address{reinterpret_cast<void*>(&D3D12Hook::present)});
m_swapchain_hook->hook_method(22, Address{reinterpret_cast<void*>(&D3D12Hook::present1)});
m_swapchain_hook->hook_method(13, Address{reinterpret_cast<void*>(&D3D12Hook::resize_buffers)});
m_swapchain_hook->hook_method(14, Address{reinterpret_cast<void*>(&D3D12Hook::resize_target)});
```

P2.2 should extend **this location** for `XeFGInternal` instead of creating a parallel hook owner.

### 5.2 Current reset callback

`REFramework::hook_d3d12()` already wires:

```cpp
m_d3d12_hook->on_resize_buffers([this](D3D12Hook& hook) {
    on_reset();
});
```

`REFramework::on_reset()` then deinitializes the active D3D12 renderer:

```cpp
void REFramework::on_reset() {
    std::scoped_lock _{m_imgui_mtx};

    spdlog::info("Reset!");

    if (m_is_d3d12) {
        deinit_d3d12();
    }

    if (m_game_data_initialized) {
        m_mods->on_device_reset();
    }

    m_has_frame = false;
    m_first_initialize = false;
    m_initialized = false;
}
```

P2.2 must reuse this path.

### 5.3 Current ResizeBuffers[13] ordering

The existing slot-13 hook already demonstrates the required ordering:

```cpp
if (d3d12->m_on_resize_buffers) {
    d3d12->m_on_resize_buffers(*d3d12);
}

++g_resize_buffers_depth;
const auto result = resize_buffers_fn(
    swap_chain,
    buffer_count,
    width,
    height,
    new_format,
    swap_chain_flags);
--g_resize_buffers_depth;
```

P2.2 must preserve the same semantic rule for slot 39:

```text
REFramework reset FIRST
original ResizeBuffers1 SECOND
```

Do **not** call `on_reset()` only after the original function fails. At that point it is too late; the purpose is to remove REFramework's references before DXGI validates the resize.

---

## 6. Required Header Change

Add the exact `IDXGISwapChain3::ResizeBuffers1` detour declaration to `D3D12Hook.hpp` near the existing `resize_buffers` and `resize_target` declarations.

Recommended signature:

```cpp
static HRESULT WINAPI resize_buffers1(
    IDXGISwapChain3* swap_chain,
    UINT buffer_count,
    UINT width,
    UINT height,
    DXGI_FORMAT new_format,
    UINT swap_chain_flags,
    const UINT* creation_node_mask,
    IUnknown* const* present_queues);
```

Do not invent a reduced signature.

Every argument must remain available for exact forwarding to the original method.

The COM method represented by vtable slot 39 is conceptually:

```cpp
HRESULT ResizeBuffers1(
    UINT BufferCount,
    UINT Width,
    UINT Height,
    DXGI_FORMAT Format,
    UINT SwapChainFlags,
    const UINT* pCreationNodeMask,
    IUnknown* const* ppPresentQueue);
```

Because the hook is a static detour, the swapchain instance is the explicit first argument.

---

## 7. Required Hook Installation Change

### 7.1 Hook slot 39 only for the XeFG internal binding

In `D3D12Hook::bind_external_swapchain()`, after the existing slot 13/14 hooks, add slot 39 when and only when the source is `SwapchainSource::XeFGInternal`.

Recommended shape:

```cpp
m_swapchain_hook = std::make_unique<VtableHook>(Address{swapchain});

m_swapchain_hook->hook_method(
    8,
    Address{reinterpret_cast<void*>(&D3D12Hook::present)});

m_swapchain_hook->hook_method(
    22,
    Address{reinterpret_cast<void*>(&D3D12Hook::present1)});

m_swapchain_hook->hook_method(
    13,
    Address{reinterpret_cast<void*>(&D3D12Hook::resize_buffers)});

m_swapchain_hook->hook_method(
    14,
    Address{reinterpret_cast<void*>(&D3D12Hook::resize_target)});

if (source == SwapchainSource::XeFGInternal) {
    m_swapchain_hook->hook_method(
        39,
        Address{reinterpret_cast<void*>(&D3D12Hook::resize_buffers1)});
}
```

Do not add slot 39 to the generic phase-1/native transition path as part of this PR.

The target acceptance path is XeFG-only, and limiting slot 39 to the already validated XeFG internal candidate minimizes unrelated behavior changes.

### 7.2 Extend the external-bind log

The current external-bind log prints Present and Present1 originals. Extend it so the user can prove the slot-39 hook was installed and identify its original target.

Suggested pattern:

```cpp
void* resize_buffers1_original = nullptr;

if (source == SwapchainSource::XeFGInternal) {
    resize_buffers1_original =
        reinterpret_cast<void*>(
            m_swapchain_hook->get_method<decltype(D3D12Hook::resize_buffers1)*>(39));
}

spdlog::info(
    "[D3D12][ExternalBind] source = {}, swapchain = 0x{:x}, "
    "queue = 0x{:x}, device = 0x{:x}, "
    "Present[8].original = 0x{:x}, Present1[22].original = 0x{:x}, "
    "ResizeBuffers1[39].original = 0x{:x}",
    source == SwapchainSource::XeFGInternal ? "xefg_internal" : "native",
    reinterpret_cast<uintptr_t>(swapchain),
    reinterpret_cast<uintptr_t>(command_queue),
    reinterpret_cast<uintptr_t>(m_device),
    reinterpret_cast<uintptr_t>(m_swapchain_hook->get_method(8).ptr()),
    reinterpret_cast<uintptr_t>(m_swapchain_hook->get_method(22).ptr()),
    reinterpret_cast<uintptr_t>(resize_buffers1_original));
```

Adapt this example to the actual `VtableHook::get_method()` return type used by the tree. The intent is mandatory; exact syntax may differ.

Do not dereference arbitrary slot-39 pointers just for logging.

---

## 8. Required ResizeBuffers1 Detour

### 8.1 Lifecycle lock and hook lookup

`ResizeBuffers1` can race against hook-monitor recovery, teardown, or future swapchain lifecycle work. Follow the same lifecycle-ownership rule already established for P2 Present1:

```cpp
while (g_framework == nullptr) {
    std::this_thread::yield();
}

std::scoped_lock lifecycle_lock{
    g_framework->get_hook_monitor_mutex()
};
```

Only after acquiring this mutex should the detour read:

```text
g_d3d12_hook
m_swapchain_hook
m_swapchain_source
m_xefg_p21_observe_only
m_on_resize_buffers
```

Recommended guard:

```cpp
auto* d3d12 = g_d3d12_hook;

if (d3d12 == nullptr ||
    d3d12->m_swapchain_hook == nullptr ||
    swap_chain == nullptr) {
    return E_FAIL;
}
```

Then retrieve the preserved slot-39 function from the current vtable hook:

```cpp
using ResizeBuffers1Fn = decltype(D3D12Hook::resize_buffers1)*;

const auto original =
    d3d12->m_swapchain_hook->get_method<ResizeBuffers1Fn>(39);
```

Do not call `swap_chain->ResizeBuffers1(...)` from inside the detour. That would re-enter the hooked vtable slot.

Always call the preserved `original` pointer.

### 8.2 Confirm this is the tracked XeFG internal instance

If the detour is ever reached for an unexpected instance, do not run REFramework reset logic against it.

Recommended check:

```cpp
const bool is_tracked_instance =
    swap_chain == d3d12->m_swapchain_hook->get_instance();

const bool is_xefg_internal =
    d3d12->m_swapchain_source == SwapchainSource::XeFGInternal;

if (!is_tracked_instance || !is_xefg_internal) {
    return original(
        swap_chain,
        buffer_count,
        width,
        height,
        new_format,
        swap_chain_flags,
        creation_node_mask,
        present_queues);
}
```

This PR must not transform an unexpected slot-39 call into a renderer reset.

### 8.3 Update tracked dimensions before reset

Match the existing slot-13 behavior:

```cpp
d3d12->m_display_width = width;
d3d12->m_display_height = height;
```

Do this before invoking the reset callback.

### 8.4 Reset only for the rendering path

P2.1 has an observe-only mode that intentionally suppresses REFramework rendering. In that mode REFramework should not have acquired the XeFG backbuffers through the normal render callback path.

Therefore the pre-reset callback should be required for the actual direct-render mode:

```cpp
const bool should_reset_renderer =
    !d3d12->m_xefg_p21_observe_only &&
    static_cast<bool>(d3d12->m_on_resize_buffers);
```

Then:

```cpp
if (should_reset_renderer) {
    d3d12->m_on_resize_buffers(*d3d12);
}
```

This callback is already wired to `REFramework::on_reset()`.

Do not call `g_framework->on_reset()` directly if the callback is available. Reusing `m_on_resize_buffers` preserves the hook abstraction and keeps the behavior aligned with slot 13.

### 8.5 Forward every argument unchanged

The original call must receive the exact values received by the detour:

```cpp
const auto result = original(
    swap_chain,
    buffer_count,
    width,
    height,
    new_format,
    swap_chain_flags,
    creation_node_mask,
    present_queues);
```

Do **not**:

```text
replace creation_node_mask
replace present_queues
replace the selected P2.1 command queue
rewrite width/height
rewrite format
rewrite flags
force a buffer count
substitute nullptr for either array
```

`pCreationNodeMask` and `ppPresentQueue` are part of the `ResizeBuffers1` contract and may be important to XeFG's multi-queue/presentation behavior.

---

## 9. Reentrancy Handling

Add a dedicated depth counter for slot 39 rather than reusing `g_resize_buffers_depth` for slot 13.

Recommended:

```cpp
thread_local int32_t g_resize_buffers1_depth = 0;
```

The two methods are distinct COM entries and may legitimately call different internal paths. Sharing one counter can accidentally suppress a valid outer operation.

Recommended outer/nested behavior:

```cpp
if (g_resize_buffers1_depth > 0) {
    ++g_resize_buffers1_depth;
    const auto nested_result = original(
        swap_chain,
        buffer_count,
        width,
        height,
        new_format,
        swap_chain_flags,
        creation_node_mask,
        present_queues);
    --g_resize_buffers1_depth;
    return nested_result;
}

// Only the outermost call performs the REFramework reset.
if (should_reset_renderer) {
    d3d12->m_on_resize_buffers(*d3d12);
}

++g_resize_buffers1_depth;
const auto result = original(...);
--g_resize_buffers1_depth;
return result;
```

Do not copy the existing slot-13 byte-restoration fallback into P2.2 unless the current `VtableHook` implementation demonstrably requires it for slot 39.

P2.2 is using an **instance vtable hook with a preserved original method pointer**. The desired implementation should remain simple unless there is concrete evidence of recursion through the detour itself.

If Codex determines a recursion guard needs stronger protection, keep it local to slot 39 and explain it in the PR body.

---

## 10. Recommended Complete Detour Shape

The following is the preferred implementation shape. Adapt exact pointer-wrapper syntax to the current tree, but preserve the ordering and semantics.

```cpp
thread_local int32_t g_resize_buffers1_depth = 0;

HRESULT WINAPI D3D12Hook::resize_buffers1(
    IDXGISwapChain3* swap_chain,
    UINT buffer_count,
    UINT width,
    UINT height,
    DXGI_FORMAT new_format,
    UINT swap_chain_flags,
    const UINT* creation_node_mask,
    IUnknown* const* present_queues)
{
    while (g_framework == nullptr) {
        std::this_thread::yield();
    }

    // Protect g_d3d12_hook and m_swapchain_hook from monitor-driven
    // replacement/teardown while this vtable callback is executing.
    std::scoped_lock lifecycle_lock{
        g_framework->get_hook_monitor_mutex()
    };

    auto* d3d12 = g_d3d12_hook;
    if (d3d12 == nullptr ||
        d3d12->m_swapchain_hook == nullptr ||
        swap_chain == nullptr) {
        return E_FAIL;
    }

    using ResizeBuffers1Fn = decltype(D3D12Hook::resize_buffers1)*;
    const auto original =
        d3d12->m_swapchain_hook->get_method<ResizeBuffers1Fn>(39);

    const bool is_tracked_instance =
        swap_chain == d3d12->m_swapchain_hook->get_instance();

    const bool is_xefg_internal =
        d3d12->m_swapchain_source == SwapchainSource::XeFGInternal;

    // The hook should only be installed on XeFGInternal, but keep this
    // defensive forwarding path so an unexpected call never resets the
    // wrong renderer binding.
    if (!is_tracked_instance || !is_xefg_internal) {
        return original(
            swap_chain,
            buffer_count,
            width,
            height,
            new_format,
            swap_chain_flags,
            creation_node_mask,
            present_queues);
    }

    if (g_resize_buffers1_depth > 0) {
        ++g_resize_buffers1_depth;
        const auto nested_result = original(
            swap_chain,
            buffer_count,
            width,
            height,
            new_format,
            swap_chain_flags,
            creation_node_mask,
            present_queues);
        --g_resize_buffers1_depth;
        return nested_result;
    }

    d3d12->m_display_width = width;
    d3d12->m_display_height = height;

    const bool should_reset_renderer =
        !d3d12->m_xefg_p21_observe_only &&
        static_cast<bool>(d3d12->m_on_resize_buffers);

    spdlog::info(
        "[XeFG][ResizeBuffers1] stage = enter, swapchain = 0x{:x}, "
        "buffer_count = {}, width = {}, height = {}, format = {}, "
        "flags = 0x{:x}, creation_node_mask = 0x{:x}, "
        "present_queues = 0x{:x}, pre_reset = {}",
        reinterpret_cast<uintptr_t>(swap_chain),
        buffer_count,
        width,
        height,
        static_cast<uint32_t>(new_format),
        swap_chain_flags,
        reinterpret_cast<uintptr_t>(creation_node_mask),
        reinterpret_cast<uintptr_t>(present_queues),
        should_reset_renderer);

    if (should_reset_renderer) {
        spdlog::info(
            "[XeFG][ResizeBuffers1] stage = pre_reset_begin");

        d3d12->m_on_resize_buffers(*d3d12);

        spdlog::info(
            "[XeFG][ResizeBuffers1] stage = pre_reset_end");
    }

    ++g_resize_buffers1_depth;

    const auto result = original(
        swap_chain,
        buffer_count,
        width,
        height,
        new_format,
        swap_chain_flags,
        creation_node_mask,
        present_queues);

    --g_resize_buffers1_depth;

    spdlog::info(
        "[XeFG][ResizeBuffers1] stage = original_return, result = 0x{:08x}",
        static_cast<uint32_t>(result));

    return result;
}
```

If the actual `VtableHook::get_method<T>()` API returns a wrapper rather than a directly callable function pointer, adapt only that syntactic detail using the same pattern already used by `present1()` and `resize_buffers()`.

Do not alter the logical sequence.

---

## 11. Exception/Depth Safety

The existing codebase does not generally use C++ exceptions across these D3D/DXGI detours, and COM methods should not throw C++ exceptions. Therefore an RAII depth guard is optional.

However, if Codex introduces any new helper that can throw before the original call returns, ensure `g_resize_buffers1_depth` cannot remain permanently incremented.

A small local RAII helper is acceptable:

```cpp
struct ResizeDepthGuard {
    int32_t& depth;

    explicit ResizeDepthGuard(int32_t& value) : depth{value} {
        ++depth;
    }

    ~ResizeDepthGuard() {
        --depth;
    }
};
```

Then:

```cpp
ResizeDepthGuard depth_guard{g_resize_buffers1_depth};
const auto result = original(...);
```

Do not add a broad generic framework abstraction for this one PR.

---

## 12. Logging Requirements

The user's next test will be analyzed from logs, so the following evidence must be easy to grep.

### 12.1 Bind-time evidence

Required:

```text
[D3D12][ExternalBind]
source = xefg_internal
...
ResizeBuffers1[39].original = 0x...
```

### 12.2 ResizeBuffers1 entry

Required machine-readable prefix:

```text
[XeFG][ResizeBuffers1]
```

At entry log:

```text
stage = enter
swapchain
buffer_count
width
height
format
flags
creation_node_mask pointer
present_queues pointer
pre_reset = true/false
```

### 12.3 Reset boundary

When render callbacks are active:

```text
[XeFG][ResizeBuffers1] stage = pre_reset_begin
Reset!
[XeFG][ResizeBuffers1] stage = pre_reset_end
```

The order matters.

### 12.4 Original return

Required:

```text
[XeFG][ResizeBuffers1] stage = original_return, result = 0x00000000
```

or the actual failure HRESULT.

Do not spam per-frame logs. `ResizeBuffers1` is a lifecycle event and can be logged on every occurrence.

---

## 13. Critical Invariants

P2.2 must preserve all of these.

### 13.1 Keep P2.1 queue selection

Do not revert:

```text
relation = distinct_same_device
→ selected_queue = presentation_queue
→ rendering enabled
```

The P2.1 DD2 run no longer failed first with the old device-removed/access-denied signature after selecting the presentation queue.

### 13.2 Reset before original ResizeBuffers1

Required:

```text
on_reset / renderer resource release
BEFORE
original ResizeBuffers1
```

Not after.

### 13.3 Never manually force external COM refcounts

Forbidden:

```cpp
while (resource->Release() > 1) {
    // ...
}
```

Forbidden:

```text
manual release-until-count threshold
releasing resources not owned by REFramework
trying to force XeFG/OptiScaler backbuffer refcounts to a guessed value
```

The fix is to release **REFramework-owned renderer references through the existing `on_reset()` path**, not to mutate someone else's COM ownership.

### 13.4 Never rewrite ResizeBuffers1 queue arrays

Forward unchanged:

```text
pCreationNodeMask
ppPresentQueue
```

Do not substitute the P2.1 `selected_queue` into `ppPresentQueue`.

The selected queue is for REFramework command submission; the `ResizeBuffers1` present-queue array belongs to the caller/XeFG lifecycle contract.

### 13.5 Do not rebind to a new swapchain in P2.2

If the resize causes XeFG to create an actually different internal swapchain later, that is separate recreation/rebind work.

P2.2 only handles:

```text
same currently bound internal swapchain
ResizeBuffers1[39]
pre-release REFramework-owned renderer resources
forward call
```

Do not implement P3's replacement-swapchain state machine here.

### 13.6 Do not call rendering callbacks from ResizeBuffers1

Only the existing resize/reset callback belongs here.

Do not call:

```text
m_on_present
m_on_post_present
on_frame_d3d12
```

from the resize detour.

---

## 14. Interaction With Existing ResizeBuffers[13] / ResizeTarget[14]

Keep existing slot 13 and 14 hooks intact.

The resulting XeFG internal binding should have:

```text
Present[8]          → D3D12Hook::present
ResizeBuffers[13]   → D3D12Hook::resize_buffers
ResizeTarget[14]    → D3D12Hook::resize_target
Present1[22]        → D3D12Hook::present1
ResizeBuffers1[39]  → D3D12Hook::resize_buffers1
```

Multiple lifecycle notifications may occur during a resolution transition.

It is acceptable for an earlier `ResizeTarget` or `ResizeBuffers` to reset the renderer, followed later by `ResizeBuffers1` invoking the reset callback again when rendering mode is active. `REFramework::on_reset()` already tolerates being called when renderer initialization has been cleared/recreated across lifecycle changes.

Do not add a complicated cross-method deduplication state machine in P2.2 unless current code proves it is required.

The observed failure specifically occurred because REFramework had already reinitialized between two resize phases, so blindly suppressing a later reset would recreate the bug.

---

## 15. What Not to Infer From the P2.1 Run

Do not encode any of these as assumptions:

```text
all XeFG games always use priority 100 presentation queues
all Intel drivers always create two distinct queues
DD2 always resizes exactly 1920x1200 -> 1280x720
ResizeBuffers1 is the final lifecycle issue
MHW has the exact same resize sequence
```

The proven facts for this work order are narrower:

```text
DD2 P2.1 observed distinct same-device queues
presentation-queue rendering removed the old leading P2 device-removal signature
DD2 then failed in native ResizeBuffers1 with DXGI_ERROR_INVALID_CALL
an immediately earlier ResizeBuffers1 succeeded after REFramework had reset
REFramework currently does not hook slot 39
```

Implement the missing lifecycle hook; do not over-generalize the runtime model.

---

## 16. Codex Validation Requirements

Codex performs only repository/build validation.

Required:

```text
git diff --check
cmake configure using the repository's current supported preset/workflow
Release REFramework build
python dev/audit_direct_access_clang.py
```

The build must produce the expected `dinput8.dll` artifact.

Also perform a static diff audit confirming:

```text
ResizeBuffers1[39] is hooked only for XeFGInternal external binding
all seven original ResizeBuffers1 arguments are forwarded unchanged
m_on_resize_buffers is called before original slot 39 in rendering mode
the observe-only path does not acquire/render/reset XeFG backbuffers unnecessarily
P2.1 selected_queue logic is unchanged
no manual Release loop was added
no P3 swapchain-replacement state machine was added
```

Codex must not launch DD2/MHW.

Codex must not state that `DXGI_ERROR_INVALID_CALL` is fixed based only on compilation.

---

## 17. PR Body Requirements

The implementation PR body should state clearly:

```text
Root runtime evidence:
- P2.1 presentation-queue rendering no longer led with DEVICE_REMOVED/ACCESS_DENIED.
- DD2 later failed at XeFG internal ResizeBuffers1[39] with 0x887A0001.
- A prior ResizeBuffers1 in the same run succeeded when REFramework had just executed Reset!.

Change:
- Hook XeFGInternal ResizeBuffers1[39].
- Invoke the existing REFramework resize/reset callback before the original call.
- Forward pCreationNodeMask / ppPresentQueue and all other arguments unchanged.
- Preserve P2.1 queue selection.

Validation:
- build/static validation only
- no game/runtime result claimed
```

If implementation differs materially from the recommended code in this work order, explain why in the PR body.

---

## 18. User Runtime Evidence Expected After Merge/Artifact Build

This section is for log design only. **Codex does not perform this test.**

The user will run DD2 and provide the logs.

The useful expected sequence is:

```text
[XeFG][QueueIdentity] ... relation = distinct_same_device
[XeFG][P2.1Probe] mode = presentation_queue_render ...
[D3D12][ExternalBind] ... ResizeBuffers1[39].original = 0x...

...

[XeFG][ResizeBuffers1]
stage = enter
width = ...
height = ...
pre_reset = true

[XeFG][ResizeBuffers1] stage = pre_reset_begin
Reset!
[XeFG][ResizeBuffers1] stage = pre_reset_end

[XeFG][ResizeBuffers1]
stage = original_return
result = 0x00000000
```

If the original still returns an error, the log must still prove whether the REFramework reset completed before that call:

```text
pre_reset_begin
Reset!
pre_reset_end
original_return = 0x887A....
```

The user will provide that runtime log for follow-up analysis. Codex must stop after building the artifact/PR.

---

## 19. Completion Criteria

P2.2 implementation work is complete for Codex when all of the following are true:

- [ ] `D3D12Hook.hpp` declares the exact slot-39 detour signature.
- [ ] XeFGInternal external bind hooks vtable slot 39.
- [ ] Generic/native phase-1 behavior is not expanded unnecessarily.
- [ ] Slot 39 retrieves and calls the preserved original method.
- [ ] The detour holds `hook_monitor_mutex` while accessing the live hook state.
- [ ] The detour validates the tracked instance/source before reset logic.
- [ ] `m_display_width` / `m_display_height` are updated consistently.
- [ ] Rendering mode invokes existing `m_on_resize_buffers` before the original call.
- [ ] Observe-only mode does not perform unnecessary renderer reset work.
- [ ] `BufferCount` is forwarded unchanged.
- [ ] `Width` is forwarded unchanged.
- [ ] `Height` is forwarded unchanged.
- [ ] `Format` is forwarded unchanged.
- [ ] `SwapChainFlags` is forwarded unchanged.
- [ ] `pCreationNodeMask` is forwarded unchanged.
- [ ] `ppPresentQueue` is forwarded unchanged.
- [ ] A dedicated slot-39 reentrancy guard exists.
- [ ] No manual COM release loop is added.
- [ ] P2.1 presentation queue selection remains unchanged.
- [ ] Required lifecycle diagnostics are present.
- [ ] `git diff --check` passes.
- [ ] CMake configure passes.
- [ ] Release REFramework build passes and emits `dinput8.dll`.
- [ ] `dev/audit_direct_access_clang.py` reports no new violation.
- [ ] No DD2/MHW runtime result is claimed by Codex.

After this, stop and hand the artifact/PR back to the user for real hardware testing.
