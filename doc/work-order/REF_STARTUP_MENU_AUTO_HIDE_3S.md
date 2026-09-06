# Work Order: REFramework Startup Menu Auto-Hide After 3 Seconds

Date: 2026-09-06  
Repository: `onehoon/REFramework`  
Target branch base: latest `master`  
Master at planning time: `acf542e3b1099cff1b8b2bcf9b5a8b00d2888c23` (`Refactor R10: isolate XeFG hook-monitor policy and final core surface (#28)`)

Related project goal:

- `doc/REFramework_OptiScaler_XeFG_SpecialK_Removal_Analysis_Plan_2026-09-05.md`

Relevant current files:

- `src/mods/REFrameworkConfig.hpp`
- `src/mods/REFrameworkConfig.cpp`
- `src/REFramework.hpp`
- `src/REFramework.cpp`

This is a deliberately small UX/default-behavior change. It is **not** part of the XeFG hook architecture and must not modify D3D11/D3D12, swapchain, OptiScaler, XeFG, loader, or hook-monitor behavior.

---

# 1. Objective

Change the default REFramework menu startup behavior so that a normal user gets this experience on every game launch:

```text
Game starts
    ↓
REFramework initializes normally
    ↓
REFramework menu is visible
    ↓
Menu remains visible for approximately 3 seconds
    ↓
Menu automatically closes once
    ↓
User may press Insert later to open/close the menu normally
    ↓
No further automatic closing occurs during that process lifetime
```

The purpose is simple:

1. The user still gets an immediate visual indication that REFramework loaded successfully.
2. The REFramework menu no longer remains permanently open by default and obscure the game until the user manually closes it.
3. Existing manual menu behavior must remain unchanged after the one-time startup auto-hide completes.
4. Existing explicit `Remember Menu Open/Closed State` behavior must remain authoritative when the user enables it.

The delay is fixed at **3 seconds**.

Do **not** add a user-facing setting for the timeout in this PR.

---

# 2. Current Behavior

Current master initializes the runtime menu visibility as open:

```cpp
bool m_draw_ui{true};
```

`REFrameworkConfig` also defines:

```cpp
ModToggle::Ptr m_menu_open{
    ModToggle::create(generate_name("MenuOpen"), true)
};

ModToggle::Ptr m_remember_menu_state{
    ModToggle::create(generate_name("RememberMenuState"), false)
};
```

The current config-load behavior is:

```cpp
if (m_remember_menu_state->value()) {
    g_framework->set_draw_ui(m_menu_open->value(), false);
}
```

Therefore, with the default configuration:

```ini
REFrameworkConfig_RememberMenuState=false
```

`MenuOpen` is not applied during startup and the runtime default remains:

```cpp
m_draw_ui == true
```

There is currently no timeout or startup auto-hide path. The menu remains visible indefinitely until the user closes it or presses the menu key.

The menu key path already calls:

```cpp
set_draw_ui(!m_draw_ui);
```

and must remain unchanged.

---

# 3. Required Behavior

## 3.1 Default case

When:

```ini
REFrameworkConfig_RememberMenuState=false
```

REFramework must:

1. start with the menu visible using the existing default behavior;
2. begin a one-shot 3-second countdown only after normal REFramework frame processing is active;
3. automatically close the menu after the countdown;
4. never auto-close again for the remainder of the process lifetime;
5. preserve normal Insert/X/manual open-close behavior after that point.

Expected flow:

```text
startup
  -> menu visible
  -> first valid REFramework frame arms startup auto-hide
  -> 3 seconds elapse
  -> set_draw_ui(false, false)
  -> startup auto-hide permanently completes
  -> Insert works exactly as before
```

## 3.2 User closes the menu before 3 seconds

If the user manually closes the menu before the timeout:

```text
startup
  -> menu visible
  -> countdown begins
  -> user closes menu at 1.5 s
  -> startup auto-hide is marked completed/cancelled
```

The timer must **not** remain armed.

If the user later presses Insert to reopen the menu, it must stay open until the user closes it.

Do not allow this sequence:

```text
user closes menu early
user reopens menu
old startup deadline fires
menu unexpectedly closes again
```

That would be a user-visible regression.

## 3.3 Explicit RememberMenuState must remain authoritative

When the user enables:

```ini
REFrameworkConfig_RememberMenuState=true
```

startup auto-hide must be disabled for that process.

This preserves the existing meaning of the option.

### Case A: remembered closed

```ini
REFrameworkConfig_RememberMenuState=true
REFrameworkConfig_MenuOpen=false
```

Expected:

```text
startup
  -> config load applies closed state
  -> menu does not appear
  -> startup auto-hide immediately becomes irrelevant/completed
  -> Insert still works normally
```

### Case B: remembered open

```ini
REFrameworkConfig_RememberMenuState=true
REFrameworkConfig_MenuOpen=true
```

Expected:

```text
startup
  -> config load applies open state
  -> menu remains open
  -> NO 3-second auto-hide
```

This is intentional.

A user who explicitly enables `Remember Menu Open/Closed State` has opted out of the new default startup policy and should retain upstream-style remembered behavior.

## 3.4 No new config key

Do not add options such as:

```text
StartupMenuAutoHide
StartupMenuTimeout
AutoHideMenu
```

Do not add a checkbox or slider to the Configuration UI.

The new behavior is a fork default policy, not a configurable feature.

The existing config file remains backward-compatible.

---

# 4. Recommended Implementation Location

Implement the behavior inside `REFrameworkConfig`.

Preferred files:

```text
src/mods/REFrameworkConfig.hpp
src/mods/REFrameworkConfig.cpp
```

Do not put this logic in:

- `REFramework::hook_monitor()`;
- D3D11 Present handling;
- D3D12 Present handling;
- `D3D12Hook`;
- XeFG compatibility code;
- Windows message hook code;
- a worker thread;
- a detached timer thread;
- a polling thread.

`REFrameworkConfig::on_frame()` is already called through normal mod frame callbacks after REFramework/game-data initialization succeeds. It is the narrowest and least invasive place for this one-shot UI policy.

Current structure:

```cpp
void REFramework::call_on_frame() {
    const bool is_init_ok = m_error.empty() && m_game_data_initialized;

    if (is_init_ok) {
        m_mods->on_frame();
    }
}
```

`REFrameworkConfig::on_frame()` currently only handles the cursor hotkey:

```cpp
void REFrameworkConfig::on_frame() {
    if (m_show_cursor_key->is_key_down_once()) {
        m_always_show_cursor->toggle();
    }
}
```

Extend this existing callback rather than creating a new lifecycle mechanism.

---

# 5. Timing Policy

Use:

```cpp
std::chrono::steady_clock
```

Do not use:

- `GetTickCount`;
- wall clock / `system_clock`;
- frame count;
- sleep;
- thread timers;
- game delta time;
- renderer-specific Present counters.

The timer must represent elapsed monotonic real time and remain independent of game framerate.

The timeout is exactly:

```cpp
std::chrono::seconds{3}
```

Prefer a file-local named constant in `REFrameworkConfig.cpp`, for example:

```cpp
namespace {
constexpr auto STARTUP_MENU_AUTO_HIDE_DELAY = std::chrono::seconds{3};
}
```

Exact naming may follow project style.

Do not expose the duration through the config system.

---

# 6. Timer Start Point

Do not start the 3-second timeout in the `REFramework` constructor.

Do not start it at DLL process attach.

Do not start it before game-data/mod initialization is complete.

The user should receive approximately 3 seconds of actual menu-visible runtime, not 3 seconds measured while the game and REFramework are still initializing.

Recommended rule:

> Arm the timer on the first `REFrameworkConfig::on_frame()` call that is eligible for startup auto-hide and observes the menu currently drawing.

Conceptually:

```text
first eligible on_frame
    |
    +-- RememberMenuState enabled? -> permanently skip auto-hide
    |
    +-- menu already closed? -> permanently skip/cancel auto-hide
    |
    +-- no deadline armed? -> arm now + 3 s
    |
    +-- deadline reached? -> close once and finish
```

This naturally places the timeout after normal initialization and avoids tying the feature to D3D11/D3D12 startup details.

---

# 7. State Model

Keep the state intentionally tiny.

A full state machine class is unnecessary.

Recommended state is equivalent to:

```cpp
bool m_startup_menu_auto_hide_done{false};
std::chrono::steady_clock::time_point m_startup_menu_open_time{};
```

or:

```cpp
bool m_startup_menu_auto_hide_done{false};
std::optional<std::chrono::steady_clock::time_point> m_startup_menu_hide_deadline{};
```

Either representation is acceptable.

Prefer whichever results in the clearest code with the fewest moving parts.

Requirements:

- one boolean or equivalent must permanently terminate the startup behavior after completion/cancellation;
- one timestamp/deadline is sufficient;
- no atomic is needed if the implementation remains inside the existing frame callback lifecycle;
- no mutex is needed solely for this feature;
- no dynamic allocation is needed;
- no thread is needed.

Do not build a reusable generic timer abstraction for this one use case.

---

# 8. Required `on_frame()` Logic

The implementation should preserve the existing cursor-key behavior first.

Conceptually:

```cpp
void REFrameworkConfig::on_frame() {
    if (m_show_cursor_key->is_key_down_once()) {
        m_always_show_cursor->toggle();
    }

    // startup menu auto-hide logic
}
```

Recommended ordering for the new logic:

## Step 1: Already completed

```cpp
if (m_startup_menu_auto_hide_done) {
    return;
}
```

After completion, the feature should cost only a trivial boolean branch per frame.

## Step 2: Respect explicit remembered-state mode

```cpp
if (m_remember_menu_state->value()) {
    m_startup_menu_auto_hide_done = true;
    return;
}
```

This is important. Do not auto-hide a menu that the user explicitly requested to remember as open.

## Step 3: Detect manual/remembered early closure

```cpp
if (!g_framework->is_drawing_ui()) {
    m_startup_menu_auto_hide_done = true;
    return;
}
```

This cancels the one-shot behavior if the menu is already closed before the timeout.

It also prevents a stale startup timer from affecting a later manual reopen.

## Step 4: Arm the timeout once

For a start-time representation:

```cpp
const auto now = std::chrono::steady_clock::now();

if (m_startup_menu_open_time == std::chrono::steady_clock::time_point{}) {
    m_startup_menu_open_time = now;
    return;
}
```

For a deadline representation:

```cpp
if (!m_startup_menu_hide_deadline.has_value()) {
    m_startup_menu_hide_deadline = now + STARTUP_MENU_AUTO_HIDE_DELAY;
    return;
}
```

## Step 5: Close once at 3 seconds

Use the existing framework API:

```cpp
g_framework->set_draw_ui(false, false);
```

The second argument must be `false`.

Reason:

> Startup auto-hide is transient runtime policy and must not request a config save merely because the default startup menu was automatically closed.

Set the done flag before or immediately around the close call so this code cannot fire repeatedly:

```cpp
m_startup_menu_auto_hide_done = true;
g_framework->set_draw_ui(false, false);
```

Do not directly assign private REFramework UI state.

Do not bypass `set_draw_ui()`.

---

# 9. Why `set_draw_ui(false, false)` Is Required

`REFramework::set_draw_ui()` is the existing authority for changing menu visibility.

It already performs the required runtime state update and synchronizes `REFrameworkConfig_MenuOpen` when game data is initialized.

Use it instead of adding a new path.

The `should_save=false` argument is intentional.

The auto-hide action should not generate an immediate save request because:

1. The feature is a startup presentation policy, not a new user preference.
2. `RememberMenuState=false` means the saved `MenuOpen` value is not authoritative for next startup anyway.
3. Avoiding an unnecessary config write keeps this feature minimally invasive.

Do not modify `set_draw_ui()` itself for this PR.

Do not change its default `should_save=true` behavior for real user actions.

---

# 10. Preserve Existing Menu Input Semantics

Do not modify menu-key handling.

Current logic equivalent to:

```cpp
if (w_param == menu_key && !m_last_keys[w_param]) {
    std::lock_guard _{m_input_mutex};
    set_draw_ui(!m_draw_ui);
}
```

must remain untouched unless a compile-only adaptation is absolutely necessary.

After startup auto-hide completes:

```text
Insert -> open
Insert -> close
Insert -> open
...
```

must behave exactly as upstream/current fork does today.

The new code must not implement:

- repeated inactivity auto-hide;
- auto-hide every time the menu opens;
- auto-hide after Insert;
- mouse-idle close;
- focus-loss close;
- Alt+Tab close;
- fullscreen-transition close.

This feature is **startup-only**.

---

# 11. Configuration Semantics That Must Not Change

The existing UI option:

```text
Remember Menu Open/Closed State
```

must continue to work exactly as its label implies.

Required matrix:

| RememberMenuState | MenuOpen saved value | Startup result | Auto-hide? |
|---|---|---|---|
| `false` | `true` | menu opens from runtime default | yes, after ~3 s |
| `false` | `false` | menu still opens from runtime default | yes, after ~3 s |
| `true` | `true` | menu opens and remains open | no |
| `true` | `false` | menu starts closed | no |

Important:

`MenuOpen=false` while `RememberMenuState=false` is allowed to remain a stale persisted value. It must not be repurposed as an implicit disable flag for startup display.

Do not change defaults to:

```cpp
m_menu_open = false
```

and do not change:

```cpp
m_draw_ui{true}
```

to false.

The desired UX is **visible briefly, then hidden**, not “never show by default.”

---

# 12. Handling User Interaction During the 3-Second Window

The feature must be conservative around user actions.

## 12.1 User manually closes before timeout

Required:

```text
cancel startup auto-hide permanently
```

Do not reopen the menu.

Do not wait until 3 seconds and call `set_draw_ui(false)` again.

## 12.2 User closes and later reopens

Required:

```text
later reopen is purely manual state
startup timer must be dead
```

## 12.3 User changes Remember Menu State during startup window

If the user enables `Remember Menu Open/Closed State` before the startup timeout fires, the next frame should permanently stop startup auto-hide.

This is a natural consequence of checking:

```cpp
m_remember_menu_state->value()
```

before the deadline each frame.

Do not add special event plumbing for this case.

## 12.4 User changes other Configuration options

Font size, font selection, cursor options, etc. must remain unrelated to startup auto-hide.

Do not reset or restart the 3-second timer because a config setting changed.

---

# 13. Scope Constraints

This PR should be extremely small.

Expected production-code surface:

```text
src/mods/REFrameworkConfig.hpp
src/mods/REFrameworkConfig.cpp
```

Expected implementation size:

- roughly 10–30 LOC production code;
- small additional comments only where behavior is not obvious;
- no new source files;
- no new library dependency;
- no new CMake entries.

Do not use this PR to refactor `REFrameworkConfig` generally.

Do not rename unrelated members.

Do not clean up formatting outside touched lines.

Do not change unrelated defaults.

---

# 14. Explicit Non-Goals

Do not modify or redesign any of the following:

- XeFG compatibility;
- OptiScaler integration;
- Special K removal architecture;
- D3D12 hook monitor;
- Present/Present1 behavior;
- ResizeBuffers/ResizeBuffers1 behavior;
- swapchain binding;
- loader notification;
- frame-generation integration;
- D3D11 hooking;
- D3D12 hooking;
- ImGui backend initialization;
- menu layout;
- menu styling;
- font scaling;
- cursor behavior;
- window-message input handling;
- config file format;
- log retention;
- logging levels;
- startup splash/notification systems;
- generic timeout infrastructure.

Do not add an OptiScaler-specific condition.

This menu policy should work identically whether the game uses:

- no frame generation;
- FSR FG;
- DLSS FG;
- XeFG;
- OptiScaler;
- no OptiScaler.

---

# 15. Interaction With XeFG Project Work

This repository is currently focused on making the following topology reliable:

```text
OptiScaler  = dxgi.dll
REFramework = dinput8.dll
Special K   = absent
```

This startup-menu change must remain orthogonal to that work.

In particular, do not use “menu disappeared after 3 seconds” as evidence that rendering or XeFG lifecycle is healthy.

The menu auto-hide only changes `m_draw_ui` through the existing UI API.

It must not affect:

- whether D3D12 Present callbacks are reached;
- whether XeFG is bound;
- hook-monitor preservation decisions;
- swapchain ownership;
- renderer initialization;
- backbuffer handling.

If any XeFG/D3D code needs modification to implement this feature, reconsider the design before proceeding.

---

# 16. Recommended Code Shape

The following is illustrative, not mandatory byte-for-byte code.

## `REFrameworkConfig.hpp`

Example minimal members:

```cpp
#include <chrono>

...

private:
    bool m_startup_menu_auto_hide_done{false};
    std::chrono::steady_clock::time_point m_startup_menu_open_time{};
```

Place them near the other local UI/config state, not inside `m_options`.

These are runtime-only state and must never be serialized.

## `REFrameworkConfig.cpp`

Example constant:

```cpp
namespace {
constexpr auto STARTUP_MENU_AUTO_HIDE_DELAY = std::chrono::seconds{3};
}
```

Example logic:

```cpp
void REFrameworkConfig::on_frame() {
    if (m_show_cursor_key->is_key_down_once()) {
        m_always_show_cursor->toggle();
    }

    if (m_startup_menu_auto_hide_done) {
        return;
    }

    if (m_remember_menu_state->value() || !g_framework->is_drawing_ui()) {
        m_startup_menu_auto_hide_done = true;
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (m_startup_menu_open_time == std::chrono::steady_clock::time_point{}) {
        m_startup_menu_open_time = now;
        return;
    }

    if (now - m_startup_menu_open_time >= STARTUP_MENU_AUTO_HIDE_DELAY) {
        m_startup_menu_auto_hide_done = true;
        g_framework->set_draw_ui(false, false);
    }
}
```

This shape is intentionally preferred because it is easy to audit.

If the implementation uses a deadline/`optional` instead, preserve exactly the same semantics.

Do not add more state unless a concrete test demonstrates it is required.

---

# 17. Logging Policy

No new normal-runtime logging is required for this feature.

Do not add an info log every frame.

Do not add logs such as:

```text
Startup menu timer tick...
Waiting to close menu...
```

If a one-time debug diagnostic is judged necessary during development, it should be removed before merge unless it has lasting diagnostic value.

This behavior is simple enough that the final implementation should normally add **zero new log noise**.

---

# 18. Build / Static Verification

At minimum, verify the normal project build path used by this repository.

Required checks:

1. `REFrameworkConfig.hpp` compiles with any new `std::chrono` type used in the class definition.
2. No accidental new serialization entry is added to `m_options`.
3. No warning is introduced for unused timer state or constants.
4. `set_draw_ui(false, false)` resolves through the existing public API.
5. No D3D/XeFG source file changes are present in the final diff.
6. No CMake/source-list change is required.

Review the final diff and confirm that production changes are limited to the intended menu-config files unless there is a concrete build reason otherwise.

---

# 19. Runtime Validation Matrix

Manual runtime validation is important because this is a user-visible timing behavior.

Use at least one normally supported RE Engine title.

If convenient, also validate on a known OptiScaler + XeFG title, but XeFG is not required to prove the basic logic.

## Test 1 — clean/default behavior

Configuration:

```ini
REFrameworkConfig_RememberMenuState=false
```

Expected:

1. Launch game.
2. REFramework menu appears normally.
3. Menu remains visible for about 3 seconds after normal REF frame processing begins.
4. Menu automatically closes.
5. Game continues normally.
6. Press Insert.
7. Menu opens.
8. Wait at least 5–10 seconds.
9. Menu remains open.
10. Press Insert again.
11. Menu closes normally.

Pass criteria:

> Auto-hide occurs once only.

## Test 2 — saved MenuOpen=false but remember disabled

Configuration:

```ini
REFrameworkConfig_RememberMenuState=false
REFrameworkConfig_MenuOpen=false
```

Expected:

- menu still appears at startup due to current runtime-default semantics;
- menu auto-hides after about 3 seconds;
- Insert works normally afterward.

This verifies that the PR did not accidentally change existing config-load semantics.

## Test 3 — user closes before timeout

Configuration:

```ini
REFrameworkConfig_RememberMenuState=false
```

Steps:

1. Launch game.
2. As soon as the menu appears, close it manually before 3 seconds.
3. Wait until well past the original 3-second deadline.
4. Press Insert to open the menu.
5. Keep it open for at least 5 seconds.

Expected:

- no delayed second close occurs;
- manually reopened menu remains open.

## Test 4 — remember closed

Configuration:

```ini
REFrameworkConfig_RememberMenuState=true
REFrameworkConfig_MenuOpen=false
```

Expected:

- menu starts closed;
- no flash/open-for-3-seconds behavior;
- Insert opens normally;
- opened menu is not auto-hidden by startup logic.

## Test 5 — remember open

Configuration:

```ini
REFrameworkConfig_RememberMenuState=true
REFrameworkConfig_MenuOpen=true
```

Expected:

- menu starts open;
- menu remains open beyond 3 seconds;
- no auto-hide occurs;
- manual close/open continues normally.

This is a required regression test because it validates that the explicit remember-state option remains authoritative.

## Test 6 — enable Remember Menu State during startup countdown

Configuration initially:

```ini
REFrameworkConfig_RememberMenuState=false
```

Steps:

1. Launch game.
2. Before 3 seconds elapse, enable `Remember Menu Open/Closed State` from the REF configuration UI.
3. Leave the menu open.
4. Wait beyond 3 seconds.

Expected:

- startup auto-hide is cancelled;
- menu remains open.

Optional if awkward to perform, but the implementation should naturally support it.

## Test 7 — OptiScaler/XeFG smoke test

Use an already known-good Special-K-free XeFG test topology if available:

```text
OptiScaler  = dxgi.dll
REFramework = dinput8.dll
Special K   = absent
```

Expected:

- REF overlay initially renders;
- REF menu closes once after ~3 seconds;
- XeFG continues presenting;
- OptiScaler overlay remains functional;
- Insert reopens REF menu normally;
- no new hook-monitor recovery or renderer lifecycle regression appears merely because auto-hide fired.

Do not expand this PR if an unrelated existing XeFG problem is observed. Record unrelated findings separately.

---

# 20. Timing Tolerance

Do not attempt millisecond-perfect UI timing.

The behavior is frame-callback-driven, so the actual close occurs on the first eligible frame at or after 3 seconds.

For runtime validation, acceptable behavior is conceptually:

```text
3 seconds + up to one normal frame/callback scheduling interval
```

Do not add high-resolution timer threads or synchronization to make the close happen at an exact wall-clock instant.

A game stall may naturally delay the callback. That is acceptable and preferable to adding lifecycle complexity.

---

# 21. Safety / Edge-Case Policy

Keep edge-case handling proportional to the feature.

Required realistic cases are:

- default startup;
- user closes early;
- user later reopens;
- RememberMenuState enabled;
- low/high framerate;
- normal game startup delays.

Do not add complexity solely for highly theoretical cases such as multiple open/close transitions occurring entirely between two mod-frame callbacks.

Do not add atomics, locks, queues, or message interception unless a real normal-use failure path is demonstrated.

---

# 22. Acceptance Criteria

The PR is complete only when all of the following are true:

- [ ] Default `RememberMenuState=false` behavior shows the REF menu at launch.
- [ ] The default menu automatically closes after approximately 3 seconds.
- [ ] Auto-hide occurs only once per process lifetime.
- [ ] User closing the menu before the timeout cancels the startup timer permanently.
- [ ] A later manual Insert reopen is never auto-closed by the old startup timer.
- [ ] `RememberMenuState=true + MenuOpen=false` still starts closed.
- [ ] `RememberMenuState=true + MenuOpen=true` still starts open and stays open.
- [ ] Insert behavior remains unchanged after startup.
- [ ] The feature uses `std::chrono::steady_clock` or equivalent monotonic timing.
- [ ] The close uses `g_framework->set_draw_ui(false, false)`.
- [ ] No new config option is added.
- [ ] No new UI checkbox/slider is added.
- [ ] No new thread or timer infrastructure is added.
- [ ] No new routine info-log noise is added.
- [ ] No D3D11/D3D12/XeFG/OptiScaler hook behavior is changed.
- [ ] Existing build passes.
- [ ] Final diff is narrow and easy to review.

---

# 23. Recommended PR Identity

Suggested branch:

```text
ux/startup-menu-auto-hide-3s
```

Suggested PR title:

```text
Auto-hide the startup REFramework menu after 3 seconds
```

Suggested commit title:

```text
ux: auto-hide startup menu after 3 seconds
```

Primary responsibility:

> Keep the REFramework menu visible briefly on normal startup as a load confirmation, then automatically close it once after 3 seconds without changing explicit remember-state behavior or later manual menu interaction.

Keep this as one small PR.

---

# 24. Final Diff Review Checklist

Before opening the PR, inspect the final diff specifically for scope creep.

Expected changed files should normally be only:

```text
src/mods/REFrameworkConfig.hpp
src/mods/REFrameworkConfig.cpp
```

Confirm there are no changes to:

```text
src/REFramework.cpp
src/REFramework.hpp
src/D3D12Hook.cpp
src/D3D12Hook.hpp
src/compatibility/xefg/**
```

unless a strictly necessary compile fix is demonstrated.

The ideal implementation should be obvious from a short review:

```text
runtime-only timestamp
+ one-shot done flag
+ 3-second constant
+ small on_frame guard sequence
```

If the implementation becomes substantially larger than this, simplify it before merge.

---

# 25. Implementation Summary for Codex

Implement only the following behavior:

```text
When RememberMenuState is false:
    if startup menu is open:
        begin one-shot 3-second timer on first eligible frame
        if user closes it before deadline:
            cancel permanently
        else after 3 seconds:
            close via set_draw_ui(false, false)
            finish permanently

When RememberMenuState is true:
    do not run startup auto-hide at all

After startup auto-hide is completed or cancelled:
    never intervene in menu visibility again during that process
```

Do not add configuration, threads, generalized timers, hook changes, or unrelated refactoring.

Runtime behavior and explicit user preference preservation are the contract.