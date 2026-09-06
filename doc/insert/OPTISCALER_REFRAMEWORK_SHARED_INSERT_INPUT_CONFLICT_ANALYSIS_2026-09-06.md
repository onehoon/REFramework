# OptiScaler + REFramework Shared Insert Input Conflict Analysis

Date: 2026-09-06

Status: Deferred follow-up / analysis only

Current reference master at time of writing: `cfb6efe5102f330c9ddf6fdadb7e9af984ce0678` (`Refactor R3: extract XeFG InitDesc transaction and factory capture (#21)`)

## 1. Purpose

Record the currently observed input coexistence behavior between OptiScaler and REFramework when both overlays use `VK_INSERT` as their menu hotkey.

This document is intentionally analysis-only. It does not prescribe an immediate code change and should not block the ongoing XeFG compatibility refactor.

The issue is separate from the XeFG presentation/rendering failures that were handled by the P3.x work. The REFramework overlay is now renderable in the affected PRAGMATA test path; the remaining observation is game-dependent menu-toggle behavior when OptiScaler and REFramework share the same key.

## 2. Observed behavior

### Shared-key configuration in the problematic game

When both components use Insert:

- OptiScaler menu hotkey: `VK_INSERT`
- REFramework menu hotkey: `VK_INSERT`

Observed behavior:

- OptiScaler continues to react to Insert consistently.
- REFramework reacts intermittently.
- The REFramework pattern appears close to alternating success/failure, for example:
  - first Insert: works
  - second Insert: ignored by REFramework
  - third Insert: works
  - fourth Insert: ignored by REFramework
- The REFramework overlay itself is capable of rendering; the failure is in the menu-toggle input path rather than a persistent overlay rendering failure.

### Control test with separate hotkeys

A direct control test was performed in the problematic game:

- OptiScaler hotkey changed away from Insert.
- REFramework kept Insert as its only Insert consumer.

Result:

- REFramework Insert no longer exhibited the intermittent missed-toggle behavior.

This remains strong evidence that shared-key coexistence is involved in that game's failure mode.

### Control game with the same shared Insert hotkey

A different tested game used the same `VK_INSERT` hotkey for both overlays and did **not** reproduce the missed REFramework toggle.

Observed behavior:

- REFramework overlay responds first.
- OptiScaler overlay follows slightly later, approximately one update/frame behind.
- The same ordering is visible when opening the overlays and when closing them.
- Both overlays remain usable.

This control result is important because it proves that sharing Insert is **not inherently broken** and is **not sufficient by itself** to reproduce the problem.

The more accurate current interpretation is:

> Shared `VK_INSERT` is a triggering condition in at least one game, but the actual failure depends on game/context-specific input/WndProc behavior and/or OptiScaler input-blocking state.

The visible REFramework-first / OptiScaler-later ordering in the healthy control game is not considered a bug by itself because both overlays still toggle correctly.

## 3. What is confirmed

The following is considered confirmed by runtime testing:

1. At least one game shows intermittent REFramework Insert misses when OptiScaler and REFramework share `VK_INSERT`.
2. Separating the hotkeys removes the reproduced REFramework Insert miss in that game.
3. OptiScaler remains responsive while REFramework misses some Insert toggles in the problematic case.
4. At least one other game works normally with both overlays sharing `VK_INSERT`.
5. In that healthy control game, REFramework visibly toggles first and OptiScaler follows slightly later for both open and close.
6. Therefore, same-key sharing is **not a universal conflict** and is not sufficient by itself to reproduce the failure.
7. The issue is not equivalent to the earlier "REFramework overlay cannot render under XeFG" failure.
8. The remaining problem should be treated as a game-dependent input/WndProc coexistence issue unless later evidence disproves that classification.

## 4. Relevant REFramework input behavior

Current REFramework menu toggling is driven by window messages in `REFramework::on_message`.

Relevant behavior is conceptually:

```cpp
case WM_KEYDOWN:
case WM_SYSKEYDOWN: {
    const auto menu_key = REFrameworkConfig::get()->get_menu_key()->value();

    if (w_param == menu_key && !m_last_keys[w_param]) {
        std::lock_guard _{m_input_mutex};
        set_draw_ui(!m_draw_ui);
    }

    m_last_keys[w_param] = true;
    break;
}

case WM_KEYUP:
case WM_SYSKEYUP:
    m_last_keys[w_param] = false;
    break;

case WM_KILLFOCUS:
    std::fill(std::begin(m_last_keys), std::end(m_last_keys), false);
    break;
```

Important property:

- A menu toggle depends on `m_last_keys[menu_key]` being false when a key-down message is received.
- REFramework therefore depends on receiving a corresponding key-up, focus reset, or another path that clears that tracked key state before the next physical press can be treated as a fresh edge.

The currently disabled DirectInput menu-toggle path does not provide an independent fallback for this behavior.

## 5. Relevant REFramework WndProc behavior

`WindowsMessageHook` replaces the target window's `GWLP_WNDPROC` directly using `SetWindowLongPtr`.

It stores the previous procedure and forwards to it with `CallWindowProc`.

Its `is_hook_intact()` check considers the hook intact only when the current `GWLP_WNDPROC` is exactly REFramework's `window_proc`.

This means REFramework participates in a classic chained WndProc topology and can be sensitive to another injected component installing or refreshing its own WndProc above or below REFramework.

Relevant source files:

- `src/REFramework.cpp`
- `src/REFramework.hpp`
- `src/WindowsMessageHook.cpp`
- `src/WindowsMessageHook.hpp`

## 6. Relevant OptiScaler behavior

Current OptiScaler uses Insert as its default overlay shortcut.

Relevant configuration behavior:

- default menu shortcut: `VK_INSERT`
- INI setting: `[Menu] ShortcutKey`

OptiScaler also has its own input/window-message handling and installs its own window subclass/WndProc path using `SetWindowLongPtrW(GWLP_WNDPROC, ...)`.

Its input layer tracks keyboard state independently and can block keyboard messages depending on menu/input-capture state.

OptiScaler's menu shortcut processing occurs through its own input/update path rather than REFramework's immediate `set_draw_ui()` call from the window-message handler. This is consistent with the healthy control game where REFramework visibly reacts first and OptiScaler follows slightly later.

OptiScaler's WndProc implementation also explicitly accounts for another component installing a WndProc above it and for preserving a previously captured WndProc chain.

Relevant upstream OptiScaler source areas at the time of analysis:

- `OptiScaler/Config.h`
- `OptiScaler/Config.cpp`
- `OptiScaler/menu/menu_common.cpp`
- `OptiScaler/menu/input/input_system_window.cpp`
- `OptiScaler/menu/input/input_system_messages.cpp`

## 7. Leading hypothesis: asymmetric key-down/key-up delivery

The strongest current hypothesis for the problematic game is that the shared-key interaction can cause REFramework to observe a key-down without observing the matching key-up in some menu-state/order combinations.

Possible sequence:

```text
Insert DOWN
  -> OptiScaler receives it
  -> REFramework receives it
  -> REFramework toggles menu
  -> REFramework sets m_last_keys[VK_INSERT] = true

OptiScaler menu/input blocking state changes

Insert UP
  -> OptiScaler receives/processes it
  -> REFramework does not receive this particular key-up

REFramework state remains:
  m_last_keys[VK_INSERT] == true

Next physical Insert DOWN
  -> OptiScaler still handles it normally
  -> REFramework may receive the down message
  -> but !m_last_keys[VK_INSERT] is false
  -> REFramework ignores the menu toggle

If the subsequent key-up reaches REFramework:
  m_last_keys[VK_INSERT] becomes false again

Next Insert DOWN
  -> REFramework toggles normally again
```

This sequence explains the observed near-alternating pattern particularly well.

However, this exact key-up-loss sequence is **not yet proven by message-level logging** and must remain a hypothesis until instrumented.

The healthy shared-key control game makes it especially important not to generalize this hypothesis as a universal OptiScaler/REFramework Insert conflict.

## 8. Secondary hypothesis: WndProc chain ordering / refresh interaction

A second plausible contributor is WndProc ordering.

Both REFramework and OptiScaler modify the same game window's WndProc chain.

Potential effects include:

- component A installs after component B and becomes top-level
- one component later validates or refreshes its subclass
- chain order changes during runtime
- forwarding behavior depends on which component currently sits above the other
- input blocking in the upper component may prevent the lower component from seeing selected messages
- different games may produce different window/input lifecycles even with the same two injected components

This hypothesis is compatible with the problematic shared-key test but is not independently proven yet.

The healthy control game's REFramework-first / OptiScaler-later visible toggle order does not, by itself, prove which component is physically top-most in the WndProc chain. The two overlays process their shortcut state at different points.

Do not assume that the issue is simply "OptiScaler always consumes Insert." That is contradicted by both the intermittent pattern in the problematic game and the fully working shared-key control game.

## 9. What is not yet proven

The following points must not be treated as established facts yet:

- that OptiScaler definitely consumes exactly the REFramework-missing `WM_KEYUP`
- that WndProc chain replacement is occurring on every failed toggle
- that the issue is caused by XeFG specifically
- that the issue is caused by the P3.3B/P3.3B.1 resize-hold code
- that REFramework's ImGui backend is failing
- that a generic hotkey implementation change is safe or necessary for all REFramework games
- that all games sharing Insert will reproduce the issue

## 10. Relationship to the XeFG compatibility work

This issue was discovered while validating OptiScaler + XeFG + REFramework coexistence, but it should remain logically separate from the D3D12/XeFG presentation lifecycle work.

Current interpretation:

```text
XeFG presentation path             PASS in this test context
REFramework D3D12 render path       PASS
REFramework overlay can render      PASS
P3.3B generic ResizeHold regression addressed separately
Shared Insert input coexistence     GAME-DEPENDENT / DEFERRED
```

Do not mix this investigation into unrelated XeFG lifecycle refactor PRs unless direct evidence later shows a coupling.

## 11. Current workaround and product decision

No general code change is currently required.

Reasoning:

- The REFramework overlay is not generally missing.
- Shared Insert works normally in other tested games.
- The remaining problem is game/context dependent.
- A global input semantic change could regress games where current behavior already works.
- The current project priority remains OptiScaler + XeFG + REFramework rendering/presentation compatibility.

For a game that does reproduce the shared-key issue, use different menu hotkeys for the two overlays.

Example:

```text
REFramework = Insert
OptiScaler   = Home (or another unused key)
```

The control test with separated hotkeys removed the reproduced REFramework Insert miss in the problematic game.

For normal games where both overlays appear and operate correctly, leave the current behavior unchanged.

## 12. Recommended future investigation

Only resume this work if the game-dependent shared-key behavior becomes important enough to justify a separate input-coexistence PR.

Start with diagnostics rather than an immediate behavioral fix.

### A. Add bounded REFramework menu-key diagnostics

For only the configured menu key, log enough data to establish the message sequence without creating general keyboard-log noise.

Suggested fields:

```text
message             WM_KEYDOWN / WM_KEYUP / WM_SYSKEYDOWN / WM_SYSKEYUP / WM_KILLFOCUS
wParam              virtual key
lParam              raw key message state
previous_state_bit  lParam bit 30
transition_bit      lParam bit 31
m_last_keys_before
m_last_keys_after
m_draw_ui_before
m_draw_ui_after
hwnd
current WndProc
REFramework hook intact yes/no
```

The logging should be diagnostic/debug-only and bounded to the configured menu key.

### B. Correlate with OptiScaler input logs

In the same run, capture OptiScaler's input/WndProc verbose logging around Insert.

Specifically determine whether a failing REFramework toggle corresponds to:

- REFramework missing `WM_KEYDOWN`
- REFramework receiving `WM_KEYDOWN` while `m_last_keys[VK_INSERT]` is stale true
- REFramework missing `WM_KEYUP`
- OptiScaler reporting keyboard blocking on the same message
- a WndProc top-level/order change

### C. Snapshot WndProc chain transitions

At initialization and whenever hook integrity changes, record:

```text
HWND
GetWindowLongPtr(GWLP_WNDPROC)
REFramework window_proc address
REFramework saved original proc
hook intact yes/no
```

Compare those timestamps with OptiScaler's subclass install/validation logs.

### D. Re-run a three-way validation matrix

```text
A: Problem game, OptiScaler Insert + REFramework Insert
B: Problem game, OptiScaler other key + REFramework Insert
C: Healthy control game, OptiScaler Insert + REFramework Insert
```

Expected current behavior:

```text
A -> intermittent REFramework misses
B -> no reproduced REFramework misses
C -> both overlays work; REFramework visibly reacts first, OptiScaler follows slightly later
```

The diagnostic patch should preserve this behavior and only explain it.

## 13. Possible future fix directions after proof

Do not implement these until the diagnostics identify the actual failing path.

### Option 1: stop depending on prior REFramework key-up for menu edge detection

Win32 key messages expose previous key state in `lParam` bit 30.

A future implementation could potentially distinguish a fresh physical key-down from key autorepeat using the message's own previous-state bit instead of relying only on REFramework's persistent `m_last_keys` state.

Conceptually:

```cpp
const bool was_down = (l_param & (1LL << 30)) != 0;

if (w_param == menu_key && !was_down) {
    set_draw_ui(!m_draw_ui);
}
```

This could make the menu toggle resilient to a lost previous key-up.

Risks to validate before adoption:

- `WM_SYSKEYDOWN` behavior
- unusual game input paths
- synthetic/post messages
- repeat semantics
- interaction with REFramework's broader `m_last_keys` use
- native non-OptiScaler behavior
- games where shared Insert already works correctly

### Option 2: improve WndProc coexistence semantics

If diagnostics show chain-order/reinstall problems rather than stale key state, the correct fix may belong in `WindowsMessageHook` rather than menu edge detection.

Do not redesign the WndProc hook preemptively. This affects all supported games and all injected coexistence scenarios.

## 14. Non-goals for the deferred fix

Do not use the eventual Insert fix as a reason to:

- redesign all REFramework input handling
- add polling loops
- duplicate OptiScaler input logic
- special-case PRAGMATA without evidence
- special-case XeFG presentation code
- change D3D12 binding/rebind logic
- change the P3.3R hook-monitor policy
- change P3.3B MHW resize-hold policy
- change default user hotkeys solely to hide the bug
- modify working shared-Insert games without evidence

The future fix should be the smallest evidence-based coexistence correction.

## 15. Reproduction checklist for future work

Environment:

```text
OptiScaler loaded
REFramework loaded
Both overlays functional/renderable
Game window focused
```

Test 1 - problematic game, shared hotkey:

```text
OptiScaler ShortcutKey = Insert
REFramework menu key   = Insert

Press/release Insert repeatedly.
Record whether OptiScaler toggles and whether REFramework toggles for every press.
```

Current observed result:

```text
OptiScaler: reliable
REFramework: intermittent, approximately alternating in the observed session
```

Test 2 - problematic game, separated hotkeys:

```text
OptiScaler ShortcutKey = another key
REFramework menu key   = Insert

Press/release Insert repeatedly.
```

Current observed result:

```text
REFramework: no reproduced misses
```

Test 3 - healthy control game, shared hotkey:

```text
OptiScaler ShortcutKey = Insert
REFramework menu key   = Insert
```

Current observed result:

```text
REFramework: reliable and visibly reacts first
OptiScaler: reliable and follows slightly later
Open and close show the same ordering
```

## 16. Current conclusion

The current evidence supports the following conclusion:

> Shared `VK_INSERT` use by OptiScaler and REFramework is not inherently broken. One tested game shows intermittent REFramework menu-toggle misses when both overlays share Insert, and separating the hotkeys removes that reproduced failure. Another tested game works normally with the same shared Insert key, with REFramework visibly toggling first and OptiScaler following slightly later. The remaining issue is therefore game/context dependent. A lost or blocked matching key-up remains the leading explanation for the problematic game's near-alternating pattern, but this exact message-loss mechanism still requires bounded message-level logging before any code fix is selected.

Current project decision:

> Leave general behavior unchanged. Do not prioritize an input fix while overlays are otherwise functional. Revisit only if the game-dependent shared-hotkey coexistence issue becomes important enough for a separate diagnostic/fix PR.
