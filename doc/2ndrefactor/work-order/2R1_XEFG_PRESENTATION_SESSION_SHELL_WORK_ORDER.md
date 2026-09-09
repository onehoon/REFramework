# Work Order — 2R1: Introduce XeFGPresentationSession Shell and Move Monitor-Only Types

Date: 2026-09-09  
Repository: `onehoon/REFramework`  
PR base branch: `REFforXeFG`  
Planning baseline: `REFforXeFG` @ `28bc7a30019d333a89e877a9576a6d01c7a71b13`  
Architecture authority: `doc/2ndrefactor/REFramework_OPTISCALER_XEFG_SECOND_STAGE_REFACTOR_DESIGN_2026-09-09.md`

---

## 1. Objective

Implement the first, deliberately small step of the second-stage OptiScaler + Intel XeFG refactor.

This PR must establish the new XeFG presentation-session boundary without moving active binding ownership or changing runtime behavior.

The scope is intentionally smaller than the full conceptual 2R1 described in the architecture document. Because `REFforXeFG` is an unreleased integration branch, we prefer smaller reviewable PRs over forcing multiple ownership changes into one diff.

The primary goal is:

> Add `XeFGPresentationSession` as a real source-level boundary and move only monitor/session-local type definitions out of `D3D12Hook.hpp/.cpp`.

This is a structural PR, not a behavior PR.

---

## 2. Branch / PR Rules

Create a new implementation branch from the current `REFforXeFG` tip.

Suggested branch name:

```text
refactor/xefg-2r1-session-shell
```

Open the PR against:

```text
base: REFforXeFG
```

Do **not** target `master`.

Do not rebase the work onto upstream `praydog/REFramework` during this PR.

---

## 3. Non-Negotiable Scope

This is XeFG-only work.

Do not intentionally modify:

- D3D11;
- DLSSG / Streamline behavior;
- FSRFG behavior;
- native D3D12 discovery;
- native Present behavior;
- native Present1 behavior;
- native ResizeBuffers / ResizeTarget behavior;
- REFramework renderer behavior;
- Lua, plugins, mods, input, VR, UI, anti-tamper/integrity behavior;
- OptiScaler code;
- Intel XeFG private object handling.

Do not add a generic frame-generation abstraction.

Forbidden examples:

```text
IFrameGenerationProvider
FrameGenerationSession
PresentationProvider
GenericSwapchainSession
multi-provider registry
```

The new type must remain explicitly XeFG-specific.

---

## 4. Current Code to Review Before Editing

Review the current `REFforXeFG` versions of:

```text
src/D3D12Hook.hpp
src/D3D12Hook.cpp
src/compatibility/xefg/XeFGBinding.hpp
src/compatibility/xefg/XeFGResizeLifecycle.hpp
src/compatibility/xefg/XeFGCompatibility.hpp
src/compatibility/xefg/XeFGCompatibility.cpp
cmake.toml
CMakeLists.txt
```

The current `D3D12Hook.hpp` contains these session-local types directly:

```cpp
struct XeFGMonitorBindingKey {
    uint64_t generation{};
    size_t runtime_slot{XeFGBinding::kInvalidRuntimeSlot};
    void* runtime_context{};
    IDXGISwapChain3* swapchain{};
    void* hook_target{};

    bool operator==(const XeFGMonitorBindingKey& other) const noexcept;
};

class XeFGHookMonitorState {
public:
    enum class TimeoutClass : uint8_t {
        Grace,
        Sustained,
    };

    TimeoutClass note_timeout(
        const XeFGMonitorBindingKey& key,
        uint64_t present_entry_count,
        int64_t present_age_ms) noexcept;

    void clear() noexcept;
    uint32_t consecutive_timeouts() const noexcept;

private:
    static constexpr uint32_t kSustainedTimeoutThreshold = 3;
    static constexpr int64_t kMinimumSustainedPresentAgeMs = 20000;
    ...
};
```

`D3D12Hook` also contains a nested detached state type:

```cpp
struct XeFGDetachedState {
    bool active{};
    XeFGBinding::RuntimeIdentity previous_runtime{};
    uint64_t previous_generation{};
    const char* reason{};
};
```

The corresponding `XeFGHookMonitorState::note_timeout()` / `clear()` implementation currently lives in `D3D12Hook.cpp`.

These are the only semantic state types that should move in this PR.

---

## 5. Add New Files

Add:

```text
src/compatibility/xefg/XeFGPresentationSession.hpp
src/compatibility/xefg/XeFGPresentationSession.cpp
```

The new header should define the session shell and the types moved from `D3D12Hook.hpp`.

A suitable initial shape is:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

#include <d3d12.h>
#include <dxgi1_4.h>

#include "XeFGBinding.hpp"

struct XeFGMonitorBindingKey {
    uint64_t generation{};
    size_t runtime_slot{XeFGBinding::kInvalidRuntimeSlot};
    void* runtime_context{};
    IDXGISwapChain3* swapchain{};
    void* hook_target{};

    bool operator==(const XeFGMonitorBindingKey& other) const noexcept {
        return generation == other.generation
            && runtime_slot == other.runtime_slot
            && runtime_context == other.runtime_context
            && swapchain == other.swapchain
            && hook_target == other.hook_target;
    }
};

class XeFGHookMonitorState {
public:
    enum class TimeoutClass : uint8_t {
        Grace,
        Sustained,
    };

    TimeoutClass note_timeout(
        const XeFGMonitorBindingKey& key,
        uint64_t present_entry_count,
        int64_t present_age_ms) noexcept;

    void clear() noexcept;

    uint32_t consecutive_timeouts() const noexcept {
        return m_consecutive_timeouts;
    }

private:
    static constexpr uint32_t kSustainedTimeoutThreshold = 3;
    static constexpr int64_t kMinimumSustainedPresentAgeMs = 20000;

    XeFGMonitorBindingKey m_key{};
    uint64_t m_last_present_entry_count{};
    uint32_t m_consecutive_timeouts{};
    bool m_initialized{};
};

struct XeFGDetachedState {
    bool active{};
    XeFGBinding::RuntimeIdentity previous_runtime{};
    uint64_t previous_generation{};
    const char* reason{};
};

class XeFGPresentationSession {
public:
    XeFGPresentationSession() = default;
};
```

Exact formatting may follow repository style.

Do not add active session members yet.

In particular, do **not** add these in 2R1:

```cpp
XeFGBinding m_binding;
XeFGResizeLifecycle m_resize;
XeFGHookMonitorState m_monitor;
XeFGDetachedState m_detached;
```

Those ownership moves belong to later PRs.

The class shell exists now only to establish the future compatibility boundary and source registration.

---

## 6. Move Monitor Implementation Only

Move the existing implementations of:

```cpp
XeFGHookMonitorState::note_timeout(...)
XeFGHookMonitorState::clear()
```

from:

```text
src/D3D12Hook.cpp
```

to:

```text
src/compatibility/xefg/XeFGPresentationSession.cpp
```

Preserve the implementation exactly.

Current semantics are contractual:

```text
new key OR Present progress
    -> timeout count resets to 1
    -> Grace

same key + unchanged Present progress
    -> increment timeout count

Sustained only when:
    consecutive_timeouts >= 3
    AND present_age_ms >= 20000
```

Do not tune either threshold.

Do not change arithmetic, atomic access, comparison rules, or action classification.

---

## 7. Minimal D3D12Hook Header Change

In `src/D3D12Hook.hpp`:

1. include the new XeFG session header;
2. remove the local definitions of:
   - `XeFGMonitorBindingKey`;
   - `XeFGHookMonitorState`;
   - nested `XeFGDetachedState`;
3. continue using the moved types for the existing fields.

For this PR, these existing D3D12Hook members should remain where they are:

```cpp
XeFGBinding m_xefg_binding{};
XeFGResizeLifecycle m_xefg_resize_lifecycle{};
XeFGDetachedState m_xefg_detached_state{};
XeFGHookMonitorState m_xefg_monitor_state{};
const char* m_last_xefg_monitor_action{};
```

Yes, this means the new session class exists but does not own them yet.

That temporary state is intentional.

Do not add `m_xefg_session` to `D3D12Hook` in this PR unless the compiler or dependency structure requires it. Prefer no new runtime object construction yet.

---

## 8. D3D12Hook.cpp Must Remain Behaviorally Identical

Other than removing the two moved monitor method definitions and adding/updating includes, do not change XeFG runtime code in `D3D12Hook.cpp`.

Specifically, do not edit the behavior of:

```text
has_consistent_active_xefg_binding()
has_xefg_monitor_state()
get_xefg_monitor_binding_key()
note_xefg_monitor_timeout()
note_xefg_monitor_action()
clear_xefg_monitor_state()
detach_xefg_binding_for_runtime_transition()
note_xefg_destroy_result()
bind_external_swapchain()
replace_xefg_binding()
apply_xefg_candidate()
present()
present1()
present_common()
resize_buffers()
resize_buffers1()
resize_target()
unhook()
```

These methods may refer to the moved types through the new header, but their logic must remain unchanged.

---

## 9. Ownership Must Not Change

This PR must not change any COM lifetime rule.

Current required ownership remains:

```text
XeFGBindingCandidate swapchain
    = strong ComPtr while candidate is pending/applied

active XeFGBinding swapchain
    = borrowed raw pointer

active XeFGBinding queue/device
    = strong ComPtr

old active swapchain during hook removal
    = bounded local ComPtr keepalive
```

Do not convert the active swapchain back to a persistent `ComPtr`.

Do not add any manual `AddRef()` / `Release()` calls.

Do not add refcount-drain loops.

---

## 10. Build Registration

`cmake.toml` uses source globs, but the checked-in generated `CMakeLists.txt` contains explicit generated source lists.

After adding the new `.hpp/.cpp`:

1. run the repository's normal cmkr generation flow;
2. regenerate `CMakeLists.txt` from `cmake.toml`;
3. verify the new session files are registered in the REFramework target;
4. commit the generated `CMakeLists.txt` change.

Do not manually treat `CMakeLists.txt` as the authoritative build definition.

Expected registration should include:

```text
src/compatibility/xefg/XeFGPresentationSession.cpp
src/compatibility/xefg/XeFGPresentationSession.hpp
```

---

## 11. Explicit Non-Goals for 2R1

Do not implement any part of 2R2+ in this PR.

Not allowed:

- moving `XeFGBinding` into `XeFGPresentationSession`;
- moving `XeFGResizeLifecycle` into the session;
- moving detached/monitor field ownership into the session;
- changing runtime detach behavior;
- changing Destroy handling;
- changing monitor actions or thresholds;
- changing candidate handoff;
- changing initial bind/rebind transactions;
- moving `m_swapchain_hook` ownership;
- changing raw alias synchronization;
- moving Present suppression policy;
- moving resize-hold policy;
- moving the MHW-specific hold rule;
- changing `REFramework.cpp` hook-monitor cadence;
- removing native Present1 support;
- logging cleanup.

If one of these appears necessary only for cleanliness, leave it for the later planned PR.

---

## 12. Validation

This is an unreleased integration branch, so a full runtime smoke is not required for this small structural PR.

Compilation and static integrity are still required.

Run at minimum:

```text
cmkr gen
cmake -S . -B <build-dir> -G "Visual Studio 17 2022" -A x64
cmake --build <build-dir> --config Release --target REFramework --parallel 4
python dev/audit_direct_access_clang.py
git diff --check
```

Equivalent supported repository presets are acceptable if available in the environment.

Required results:

```text
Release x64 build: PASS
direct-access audit: 0 violations
git diff --check: PASS
```

Do not claim DD2/MHW/Intel XeFG runtime validation unless it was actually performed.

---

## 13. Static Review Checklist

Before opening the PR, verify all of the following:

- `XeFGMonitorBindingKey` has exactly the same fields and equality semantics as before.
- `XeFGHookMonitorState` has the same threshold constants as before.
- `note_timeout()` has the same state transitions as before.
- `clear()` resets the same fields as before.
- `XeFGDetachedState` has the same fields and defaults as before.
- `D3D12Hook` still owns all existing active XeFG fields.
- no active binding ownership changed.
- no physical VtableHook ownership changed.
- no Present/Resize call order changed.
- no native D3D12 condition changed.
- no Streamline/DLSSG code changed.
- no FSRFG path changed.
- generated `CMakeLists.txt` includes the new source files.

A useful final source audit is:

```text
search for:
XeFGMonitorBindingKey
XeFGHookMonitorState
XeFGDetachedState
```

Expected result:

```text
definitions -> XeFGPresentationSession.hpp/.cpp
uses        -> existing D3D12Hook / XeFG compatibility code as required
```

There must not be duplicate type definitions left in `D3D12Hook.hpp`.

---

## 14. Expected Diff Shape

Keep this PR intentionally small.

Expected touched files:

```text
src/compatibility/xefg/XeFGPresentationSession.hpp   NEW
src/compatibility/xefg/XeFGPresentationSession.cpp   NEW
src/D3D12Hook.hpp                                    SMALL
src/D3D12Hook.cpp                                    SMALL
CMakeLists.txt                                       GENERATED
```

`cmake.toml` should normally not need a content change because the target already uses source globs.

If the diff starts modifying `REFramework.cpp`, `XeFGCompatibility.cpp`, `XeFGCandidateHandoff.cpp`, bind/rebind code, Present/Resize logic, or game-specific policy, stop and reduce scope.

---

## 15. PR Description Requirements

Open the PR against `REFforXeFG` with a concise description containing:

### Summary

```text
- add XeFGPresentationSession source boundary
- move XeFG monitor key/state and detached-state type definitions out of D3D12Hook
- move monitor-state implementation without semantic changes
- no active session ownership or runtime-policy changes yet
```

### Scope

Explicitly state:

```text
2R1 shell/type-extraction only.
No XeFG binding, resize, Present, hook-monitor policy, candidate handoff, COM ownership, or native REFramework behavior changes.
```

### Validation

Include exact commands and results.

### Runtime evidence boundary

If no game test was performed, state that clearly.

---

## 16. Stop Conditions

Stop and report instead of expanding the PR if any of the following becomes necessary:

- native D3D12 Present logic must be rewritten;
- `m_swapchain_hook` ownership must move;
- `XeFGBinding` ownership must move;
- monitor thresholds must change;
- runtime lifecycle behavior must change;
- `REFramework.cpp` needs substantial changes;
- Streamline/DLSSG code must change;
- a generic FG abstraction appears necessary;
- build requires unrelated source cleanup.

Those conditions mean the boundary needs reconsideration, not that 2R1 should become larger.

---

## 17. Completion Criteria

2R1 is complete when:

1. `XeFGPresentationSession.hpp/.cpp` exist and build as part of REFramework;
2. monitor key/state and detached-state type definitions no longer live in `D3D12Hook.hpp`;
3. monitor state implementation no longer lives in `D3D12Hook.cpp`;
4. all runtime state ownership remains exactly where it was before this PR;
5. Release build passes;
6. direct-access audit passes with zero violations;
7. `git diff --check` passes;
8. PR targets `REFforXeFG`;
9. no unrelated cleanup is included.

The next PR should begin the actual session ownership migration. Do not pre-implement it here.
