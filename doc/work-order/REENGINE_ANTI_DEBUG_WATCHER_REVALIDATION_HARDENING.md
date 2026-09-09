# Work Order — RE Engine `DbgUiRemoteBreakin` Anti-Debug Watcher Revalidation Hardening

## Target

- Repository: `onehoon/REFramework`
- Base / PR target: `master`
- Baseline reviewed: `781c0c876ba2082bbfa8f4b4b3dbb884313e7e76`
- Primary file: `src/mods/IntegrityCheckBypass.cpp`
- Expected implementation branch: `fix/reengine-antidebug-revalidation-hardening`
- Suggested PR title: `Harden RE Engine anti-debug watcher revalidation`

## Objective

Harden the existing RE Engine `IntegrityCheckBypass::anti_debug_watcher()` transaction so REFramework does **not** restore `ntdll!DbgUiRemoteBreakin` merely because a redirect was observed.

The watcher must only restore the captured original `DbgUiRemoteBreakin` bytes after the redirected anti-tamper payload has been safely revalidated and successfully neutralized.

If the redirect target is not ready yet, changes while being inspected, cannot be validated, or cannot be neutralized safely, leave `DbgUiRemoteBreakin` untouched and let the existing 500 ms watcher retry on a later cycle.

This work is derived from the hardening lessons from `onehoon/OptiPatcher` PR #1, but this PR is a **REFramework logic improvement**, not an OptiPatcher port.

---

## Current problem in `master`

The current watcher roughly does this:

```cpp
if (DbgUiRemoteBreakin differs from original) {
    if (E9 redirect) {
        resolve target;
        if (target looks heap allocated/readable) {
            nuke_heap_allocated_code(target);
        }
    } else if (FF 25 redirect) {
        resolve pointer slot;
        dereference target;
        if (target looks heap allocated/readable) {
            nuke_heap_allocated_code(target);
        }
    }

    // unconditional today
    restore original DbgUiRemoteBreakin bytes;
}
```

That creates a timing dependency.

Example failure sequence:

```text
Capcom installs E9 / FF25 redirect
    -> target allocation or FF25 slot is not fully ready yet
    -> REFramework watcher observes redirect
    -> target validation / neutralization does not happen
    -> REFramework nevertheless restores DbgUiRemoteBreakin
    -> next polling cycle no longer sees the redirect
    -> watcher loses the opportunity to neutralize the payload after it becomes ready
```

There is also a destructive-race issue in the successful-looking path:

- `FF25` can keep the same six instruction bytes while the indirect target slot changes.
- the executable private target region can be released, replaced, resized, or have its protection/type changed between the first check and the destructive write.
- the first redirect bytes can remain the same while later bytes in the 32-byte entry snapshot change before the full original entry is restored.

Because `nuke_heap_allocated_code()` overwrites an entire memory region with `0xC3`, the final destructive action must be preceded by strict identity revalidation.

---

# Required behavior

Treat one watcher cycle as a small validated transaction:

```text
observe redirect
  -> capture full entry snapshot
  -> resolve E9 / FF25 target
  -> validate target region
  -> immediately revalidate full entry + redirect target
  -> immediately revalidate target region identity
  -> neutralize payload
  -> verify entry still matches the observed snapshot
  -> restore original DbgUiRemoteBreakin bytes
```

Any failed validation before restoration means:

```text
DO NOT restore DbgUiRemoteBreakin
DO NOT guess a replacement target
DO NOT nuke an unvalidated region
return and retry on the next existing watcher cycle
```

The existing polling cadence and thread model remain unchanged.

---

# Mandatory invariants

## 1. Preserve original-byte acquisition

Do not redesign baseline capture in this PR.

Keep the existing preference for:

```cpp
utility::get_original_bytes(dbg_ui_remote_breakin)
```

and the existing fallback behavior.

The OptiPatcher-specific contaminated-live-baseline problem is not the purpose of this PR because REFramework already has an original-image byte source.

## 2. Preserve synchronous first watcher call

`init_anti_debug_watcher()` already calls:

```cpp
anti_debug_watcher();
```

before starting the `std::jthread`.

Do not change this behavior.

## 3. Preserve the 500 ms polling model

Do not add timers, event hooks, additional worker threads, or tighter polling.

The current background loop remains the retry mechanism.

## 4. Do not restore on incomplete neutralization

This is the core requirement.

Restoring `DbgUiRemoteBreakin` is allowed only after the currently observed redirect was revalidated and its target was successfully neutralized.

Unsupported redirect forms or temporarily invalid targets must remain untouched for a future retry.

## 5. `FF25` target identity must be re-resolved

For:

```asm
FF 25 xx xx xx xx
```

do not trust a target pointer resolved earlier in the cycle.

Immediately before destructive neutralization:

- re-read the full entry snapshot,
- re-resolve the RIP-relative pointer slot,
- re-read the target stored in that slot,
- require it to still equal the originally resolved target.

If it changed, abort the cycle without restoration.

## 6. Revalidate the complete observed entry

Use a full 32-byte observed entry snapshot for transaction identity.

Do not only compare the first 5 bytes for E9 or first 6 bytes for FF25 before restoring the full original baseline.

Before destructive neutralization and again before restoring the original entry, require the relevant live `DbgUiRemoteBreakin` bytes to still match the observed snapshot.

## 7. Revalidate target region immediately before destructive write

Do not rely only on an earlier `VirtualQuery()` result.

Immediately before changing protection / writing `0xC3`, re-query the target region and require the same region identity and eligibility.

At minimum require:

- `BaseAddress` unchanged,
- `AllocationBase` unchanged,
- `RegionSize` unchanged,
- `State == MEM_COMMIT`,
- `Type == MEM_PRIVATE`,
- executable page protection,
- no unsupported protection flags such as `PAGE_GUARD`.

The address being neutralized must still fall inside that same validated region.

## 8. Flush instruction cache after executable writes

After successfully overwriting the executable payload, call `FlushInstructionCache()` for the written region.

After restoring `DbgUiRemoteBreakin`, also flush the restored entry range.

A failed executable write / protection restore / cache flush must not be reported as a successful neutralization transaction.

## 9. No broad exception-based probing as normal control flow

Avoid adding new unsafe direct dereferences equivalent to the current unconditional FF25 slot dereference.

Prefer range/protection validation and safe reads before consuming indirect pointers.

## 10. Do not broaden anti-tamper scope

This PR is not permission to add new game-specific anti-tamper patches.

Do not add or change:

- MHW scanner/crasher signatures,
- RE9 heartbeat / slow-path logic,
- `createBLAS` handling,
- RE4/RE8 integrity patterns,
- OptiScaler integration,
- XeFG behavior,
- renderer hooks,
- game identity policy.

---

# Recommended implementation shape

The exact helper names are not mandatory, but keep the logic local to `IntegrityCheckBypass.cpp` unless a compelling existing abstraction already exists.

A small private snapshot structure is appropriate:

```cpp
namespace {
constexpr size_t ANTI_DEBUG_ENTRY_SIZE = 32;

enum class AntiDebugRedirectKind {
    DirectE9,
    IndirectFF25,
};

struct AntiDebugRedirectSnapshot {
    AntiDebugRedirectKind kind{};
    uintptr_t target{};
    size_t instruction_length{};
    std::array<uint8_t, ANTI_DEBUG_ENTRY_SIZE> observed_entry{};
    MEMORY_BASIC_INFORMATION target_region{};
};
}
```

Do not create a general anti-tamper framework or new public API for this one fix.

## Safe redirect resolution

Suggested semantics:

```cpp
std::optional<AntiDebugRedirectSnapshot> resolve_anti_debug_redirect(...);
```

### E9

For `E9 rel32`:

- require enough readable entry bytes,
- calculate the signed rel32 target correctly,
- reject overflow / invalid address ranges,
- validate the target region.

### FF25

For `FF 25 rel32`:

- calculate the RIP-relative slot address,
- validate that the pointer slot is readable,
- read the target safely,
- validate the target region,
- retain enough identity to re-resolve the slot later.

Do not treat `utility::get_module_within(target) == nullptr` alone as sufficient proof that the target is safe to destroy.

The destructive target must be a committed executable `MEM_PRIVATE` region.

---

# Convert neutralization into a success/failure operation

The current:

```cpp
void IntegrityCheckBypass::nuke_heap_allocated_code(uintptr_t addr)
```

cannot tell `anti_debug_watcher()` whether the payload was actually neutralized.

Change the internal contract so the caller receives a real success/failure result.

The implementation may either:

1. change `nuke_heap_allocated_code()` to return `bool`, or
2. replace it with a narrowly scoped helper that accepts the validated redirect snapshot.

Preferred conceptual shape:

```cpp
bool neutralize_anti_debug_payload(
    const AntiDebugRedirectSnapshot& expected,
    void* dbg_ui_remote_breakin);
```

Inside that helper, immediately before writing:

```text
re-read 32-byte entry
    -> must match expected.observed_entry
re-resolve redirect
    -> kind must match
    -> target must match
re-query target region
    -> region identity must match expected.target_region
change protection
write RET bytes
restore protection
flush instruction cache
return true only if all required operations succeeded
```

Do not perform the region nuke if any identity check fails.

---

# Restore transaction

After successful target neutralization, restore `DbgUiRemoteBreakin` only if the entry still belongs to the same transaction.

Suggested conceptual helper:

```cpp
bool restore_anti_debug_entry_if_unchanged(
    void* dbg_ui_remote_breakin,
    const AntiDebugRedirectSnapshot& expected,
    std::span<const uint8_t> original_bytes);
```

Required behavior:

```cpp
std::array<uint8_t, 32> current{};

if (!safe_read(dbg_ui_remote_breakin, current.data(), current.size())) {
    return false;
}

if (current != expected.observed_entry) {
    // Another writer changed DbgUiRemoteBreakin.
    // Do not overwrite it with our stale baseline.
    return false;
}

// ProtectionOverride or equivalent
// copy original bytes
// FlushInstructionCache
return true;
```

If the entry changed after payload neutralization, leave the newer entry untouched. The next watcher cycle can evaluate the new state.

---

# Revised watcher flow

The final logic should read approximately like this:

```cpp
void IntegrityCheckBypass::anti_debug_watcher() try {
    // existing ntdll / original-byte setup stays

    std::array<uint8_t, 32> observed{};
    if (!read_entry(observed)) {
        return;
    }

    if (entry_matches_original(observed)) {
        return;
    }

    const auto redirect = resolve_anti_debug_redirect(observed);
    if (!redirect) {
        // Unsupported or not yet safely resolvable.
        // Do not restore it just because it differs from the original.
        return;
    }

    if (!neutralize_anti_debug_payload(*redirect, dbg_ui_remote_breakin)) {
        // Target not ready / changed / region no longer eligible.
        // Preserve redirect so the next 500 ms cycle can retry.
        return;
    }

    if (!restore_anti_debug_entry_if_unchanged(
            dbg_ui_remote_breakin,
            *redirect,
            *original_dbg_ui_remote_breakin_bytes)) {
        return;
    }

    spdlog::info("[IntegrityCheckBypass]: Restored DbgUiRemoteBreakin after validated payload neutralization.");
} catch (...) {
    // existing exception handling policy
}
```

The exact implementation can differ. The behavioral ordering above is mandatory.

---

# Logging

Do not turn this PR into a logging refactor.

However, because failed validation now intentionally leaves the redirect installed for a later 500 ms retry, avoid creating high-volume INFO spam if the exact same transient state persists for many cycles.

Acceptable approaches:

- use DEBUG/TRACE for repeated retry-only diagnostics,
- or minimally deduplicate identical transient diagnostics.

Do not add a large diagnostic state machine.

Always keep a clear positive INFO diagnostic for a successful payload neutralization + entry restoration.

---

# Failure-path acceptance matrix

| Situation | Neutralize payload? | Restore `DbgUiRemoteBreakin`? | Retry later? |
|---|---:|---:|---:|
| Entry already equals original | No | No | Normal polling continues |
| Unsupported redirect form | No | No | Yes |
| E9 target cannot be validated yet | No | No | Yes |
| FF25 slot unreadable | No | No | Yes |
| FF25 target changed during transaction | No | No | Yes |
| Target not `MEM_PRIVATE` executable | No | No | Yes |
| Target region identity changes before write | No | No | Yes |
| Entry changes before target write | No | No | Yes |
| Payload write/protection/flush fails | Treat as failure | No | Yes |
| Payload neutralization succeeds, entry unchanged | Yes | Yes | Normal polling continues |
| Payload neutralization succeeds, entry changed before restore | Yes | No | Yes; evaluate new entry next cycle |

---

# Important non-goals

Do **not**:

- remove the existing watcher,
- replace the 500 ms polling thread,
- alter `init_anti_debug_watcher()` startup ordering,
- change which games initialize `IntegrityCheckBypass`,
- add OptiPatcher as a dependency,
- copy OptiPatcher architecture wholesale,
- modify XeFG / D3D12 presentation code,
- redesign `utility::get_original_bytes()`,
- generalize this into a reusable memory-patching framework,
- change unrelated `nuke_heap_allocated_code()` callers without first checking whether any exist.

If `nuke_heap_allocated_code()` has callers outside this exact anti-debug path, preserve their current behavior or introduce a dedicated new helper instead of silently changing unrelated semantics.

---

# Required code audit before editing

Before implementation, search the current tree for:

```text
nuke_heap_allocated_code
anti_debug_watcher
init_anti_debug_watcher
DbgUiRemoteBreakin
```

Confirm all call sites and keep the patch minimal.

Also inspect `ProtectionOverride` semantics before depending on it for success reporting. If it cannot report protection restoration failure, use explicit `VirtualProtect()` only inside the new transactional helper where success/failure must be known.

---

# Validation

## Mandatory static/build validation

Use the repository's current build configuration. At minimum:

```text
Release x64 build: PASS
git diff --check: PASS
```

Also verify that no unrelated generated/build files are committed.

If the existing direct-access audit used by the XeFG work is unaffected, this PR does not need new XeFG-specific audit logic.

## Source-level scenario validation

Explicitly reason through and document these cases in the PR description:

1. `E9` target valid immediately -> neutralize -> restore.
2. `E9` target invalid on cycle N, valid on cycle N+1 -> cycle N leaves redirect intact; N+1 neutralizes/restores.
3. `FF25` slot target changes between initial resolve and destructive phase -> no write, no restore.
4. target region changes identity/protection before destructive write -> no write, no restore.
5. full `DbgUiRemoteBreakin` 32-byte snapshot changes before restore -> do not overwrite newer entry.
6. successful path still restores the original bytes obtained by the existing baseline mechanism.

## Runtime validation

If practical, test at least one currently supported Capcom RE Engine title where this watcher is active.

Preferred broader smoke set if already available:

- Monster Hunter Wilds
- Dragon's Dogma 2
- Resident Evil 9
- PRAGMATA

Runtime validation should confirm:

- game starts normally,
- REFramework remains active,
- no new anti-debug retry log flood,
- successful anti-tamper neutralization is still observed when triggered,
- no crash/regression from the watcher changes.

Do not block implementation solely because the narrow transient-target timing window is difficult to reproduce naturally; the source-level transaction invariants are the primary acceptance requirement for that edge case.

---

# PR scope expectation

This should remain a **small hardening PR**.

Expected source changes are primarily:

```text
src/mods/IntegrityCheckBypass.cpp
```

Touch `IntegrityCheckBypass.hpp` only if a private signature genuinely needs to change.

Do not mix documentation cleanup, XeFG work, upstream synchronization, or unrelated anti-tamper research into the implementation PR.

---

# Definition of done

The PR is complete when all of the following are true:

- [ ] `DbgUiRemoteBreakin` is no longer restored unconditionally when a redirect is merely observed.
- [ ] E9 and FF25 targets are validated safely.
- [ ] FF25 indirect target is re-resolved immediately before destructive neutralization.
- [ ] Full 32-byte observed entry is revalidated before destructive neutralization and before full restore.
- [ ] Target region identity is re-queried immediately before destructive write.
- [ ] Destructive neutralization reports real success/failure.
- [ ] Original entry restoration happens only after successful validated neutralization.
- [ ] Failed/transient validation leaves the redirect intact for the existing 500 ms retry.
- [ ] Executable writes are followed by `FlushInstructionCache()`.
- [ ] Existing original-byte acquisition, synchronous first check, polling cadence, and game scope remain unchanged.
- [ ] No unrelated anti-tamper, OptiScaler, XeFG, renderer, or game-specific changes are included.
- [ ] Release x64 build passes.
- [ ] `git diff --check` passes.

When finished, open the implementation PR against **`master`** and include a concise description of the transaction ordering and the failure-path behavior.