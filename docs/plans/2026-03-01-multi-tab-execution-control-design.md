# Multi-Tab Execution Control: Debugger Handoff Design

**Date**: 2026-03-01
**Status**: Draft

## Problem

When tab A has `Debugger.pause` active and a new tab B becomes active (via API
creation or page interaction), Chrome shows "debugger is paused in another tab"
with a greyed-out screen on tab B. This blocks the agent from interacting with
the new tab.

## Solution

Release all execution control (debugger + virtual time) on tabs when they lose
focus. Background tabs rely solely on Chrome's natural throttling. Re-establish
execution control from scratch when a tab becomes active and an action targets
it.

## Requirements

1. **API-created tabs** (POST /tabs): new tab becomes foreground, old tab fully
   released
2. **Page-interaction tabs** (click → window.open, target=_blank): new tab opens
   as foreground naturally, old tab fully released
3. **Background tabs**: no ABP execution control — Chrome's natural throttling
   only
4. **Switch back**: re-establish full execution control (debugger + virtual time)
   when agent activates a tab
5. **Multiple new tabs**: Chrome foregrounds the last one opened, intermediate
   tabs are background. All background tabs are identical (no execution control).
   No special-case logic.

## Design

### 1. New State: `backgrounded` Flag

Add `bool backgrounded = false` to `TabState`. This flag serves two purposes:

- **Guards inflight CDP callbacks**: When a tab is backgrounded while execution
  control CDP commands are in-flight (sent to renderer via Mojo but callbacks not
  yet received), the callbacks check this flag and no-op instead of
  re-establishing state.
- **Signals intent**: Distinguishes "execution control not yet set up" from
  "execution control intentionally torn down".

```cpp
struct TabState {
  // ...existing fields...
  bool backgrounded = false;
};
```

### 2. Backgrounding a Tab

Triggered when focus leaves a tab. Entry points: `CreateTab()`, `ActivateTab()`,
page-interaction tab opens detected via tab strip observer.

**Sequence:**

```
BackgroundTab(old_tab_id):
  1. Set tab_states_[old_tab_id].backgrounded = true
  2. If phase is kPaused (debugger actively paused):
     → Send Debugger.resume(disableOnResume=true)
       (atomic resume + disable, exits nested RunLoop safely)
  3. Unconditionally send Debugger.disable
     (idempotent safety net for inflight Debugger.enable/pause commands
      that may have landed in the renderer after step 2)
  4. If execution was ever enabled for this tab (phase != kDisabled):
     → Send Emulation.setVirtualTimePolicy("realtime")
       (idempotent, releases virtual time fences)
  5. Set phase = kDisabled
  6. Ignore all cleanup command responses (fire-and-forget)
```

**Why both resume AND disable?**

- `Debugger.resume(disableOnResume=true)` handles the case where the debugger is
  currently paused — it atomically resumes execution and disables the debugger
  before JS continues (prevents anti-debugging `debugger;` re-entry).
- `Debugger.disable` (unconditional) handles inflight commands: if a
  `Debugger.enable` or `Debugger.pause` was in the Mojo pipe and lands in the
  renderer after the resume, the subsequent disable cleans it up. `Debugger.disable`
  is idempotent — returns success immediately if already disabled.

**CDP idempotency (verified in V8 source):**

| Command | Idempotent? | On repeat call |
|---------|-------------|----------------|
| `Debugger.enable` | Yes | Early return success |
| `Debugger.disable` | Yes | Early return success |
| `Debugger.pause` | Yes | Silent success if already paused |
| `Debugger.resume` | **No** | Error if not paused |
| `setVirtualTimePolicy` | Yes | Switches policy freely |

Only `Debugger.resume` is non-idempotent, which is why we only call it
conditionally (when we know the tab is in kPaused phase). `Debugger.disable` is
the unconditional safety net.

**Open question**: Does `Debugger.disable` correctly exit the nested RunLoop when
the debugger is paused? If so, the conditional `resume` in step 2 is unnecessary
and we can simplify to just unconditional `Debugger.disable`. Needs verification
in V8's `V8DebuggerAgentImpl::disable()` → `V8Debugger::disable()` path.

### 3. Inflight CDP Callback Guards

Every CDP callback in the execution control chain checks `backgrounded` before
proceeding. If the tab has been backgrounded, skip the operation and run the
`then` continuation immediately.

**Guarded callbacks:**

- `OnDebuggerEnabled()`
- `SendDeterministicPause()` inner callback
- `EnableVirtualTimeAfterDebugger()`
- `OnVirtualTimeEnabled()`
- `OnVirtualTimePaused()`
- `ForceRedrawThenResumeVirtualTime()`
- `SwitchToRealtimeVirtualTime()`
- `OnVirtualTimeResumed()`

**Pattern:**

```cpp
void AbpController::OnDebuggerEnabled(const std::string& tab_id,
                                       /* ... */) {
  auto it = tab_states_.find(tab_id);
  if (it != tab_states_.end() && it->second.backgrounded) {
    VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, skipping OnDebuggerEnabled";
    std::move(then).Run();
    return;
  }
  // ... existing logic ...
}
```

**Why this works with Mojo ordering**: Mojo IPC preserves message ordering per
channel. The cleanup commands in `BackgroundTab()` are sent after setting the
flag. Any inflight commands that were already in the pipe will have their
callbacks arrive and be no-op'd. The cleanup commands arrive after those inflight
commands and undo whatever the renderer applied.

### 4. Foregrounding a Tab

Triggered when focus returns to a tab. Entry points: `ActivateTab()`.

**Sequence:**

```
ForegroundTab(tab_id):
  1. Set tab_states_[tab_id].backgrounded = false
  2. If IsExecutionControlEnabled() (global flag):
     → Call EnableExecutionControl(tab_id) which starts in kPaused state
       (Debugger.enable → Debugger.pause → setVirtualTimePolicy("pause"))
  3. Tab is now frozen and ready for the next action
```

The action lifecycle already handles the case where execution control isn't
enabled — `ResumeExecution()` calls `EnableExecutionControl()` if needed. But
calling it eagerly in `ForegroundTab()` ensures the tab is frozen before any
action arrives, matching the "auto re-pause on switch back" requirement.

### 5. CreateTab Changes

```
CreateTab(params, callback):
  1. Identify currently active tab (if any)
  2. BackgroundTab(active_tab_id)  ← NEW
  3. Create new tab as NEW_FOREGROUND_TAB (existing behavior)
  4. Register popup interceptor, permission observer (existing)
  5. Center virtual cursor (existing)
  6. Send response (existing)
  // Note: new tab has no execution control yet.
  // First action on it triggers EnableExecutionControl via action lifecycle.
```

### 6. ActivateTab Changes

```
ActivateTab(tab_id, callback):
  1. Find the target tab
  2. Identify currently active tab
  3. If old_tab != new_tab:
     a. BackgroundTab(old_tab_id)  ← NEW
     b. ForegroundTab(tab_id)     ← NEW
  4. ActivateTabAt(index) (existing)
  5. Bring browser window to front (existing)
  6. Send response (existing)
```

### 7. Page-Interaction Tab Opens

When a page opens a new tab via `window.open`, `target=_blank`, etc.:

1. Chrome naturally makes the new tab foreground
2. Tab strip model observer (`OnTabStripModelChanged`) detects the change
3. Identifies the previously active tab
4. Calls `BackgroundTab(old_tab_id)`
5. New tab loads normally with no execution control
6. Agent receives a tab-opened event via the event observer
7. Agent can interact with the new tab via actions (first action triggers
   execution control setup)

**Detection mechanism**: Use `TabStripModelObserver::OnTabStripModelChanged()`
to detect `TabStripModelChange::kInserted` events (new tab added) or
`TabStripSelectionChange` (active tab changed). This catches both API-created
tabs and page-interaction tabs uniformly.

### 8. Multiple Tab Opens

When a page opens multiple tabs (e.g., JS `window.open` in a loop):

- Chrome foregrounds the last one opened
- Intermediate tabs become background naturally
- `TabStripModelObserver` fires for each insertion/activation change
- Each triggers `BackgroundTab` on the previously active tab
- All background tabs end up identical: no execution control, Chrome throttles
- Agent gets events for each and can activate whichever one it wants
- No special-case logic needed

### 9. PauseAllTabs at Startup

`PauseAllTabs()` continues to work as-is. At startup, `backgrounded` is `false`
for all tabs and no tab switching has occurred. All tabs get paused.

**Future optimization**: Only pause the active tab at startup since background
tabs will be released immediately on first tab switch anyway. Not required for
this change.

### 10. Race Condition Analysis

**Race 1: Inflight EnableExecutionControl when tab is backgrounded**

```
T=0  EnableExecutionControl sends Debugger.enable (Mojo IPC)
T=1  New tab opens, BackgroundTab(old_tab) sets backgrounded=true
T=2  BackgroundTab sends Debugger.disable (Mojo IPC)
T=3  Renderer processes Debugger.enable → debugger enabled
T=4  OnDebuggerEnabled callback → sees backgrounded=true → no-op
T=5  Renderer processes Debugger.disable → debugger disabled (cleanup)
```

Result: Renderer ends up clean. Mojo ordering guarantees disable arrives after
enable.

**Race 2: Inflight PauseExecution when tab is backgrounded**

```
T=0  PauseExecution sends Debugger.enable + Debugger.pause (chained)
T=1  New tab opens, BackgroundTab(old_tab) sets backgrounded=true
T=2  BackgroundTab sends Debugger.disable (Mojo IPC)
T=3  Renderer processes enable + pause → debugger paused
T=4  Callbacks arrive → no-op (backgrounded)
T=5  Renderer processes Debugger.disable → exits pause + disables
```

Result: Clean. The unconditional `Debugger.disable` cleans up. (Pending
verification that `Debugger.disable` exits nested RunLoop when paused.)

**Race 3: Inflight ResumeExecution when tab is backgrounded**

```
T=0  ResumeExecution sends Debugger.resume(disableOnResume=true)
T=1  New tab opens, BackgroundTab(old_tab) sets backgrounded=true
T=2  BackgroundTab sends Debugger.disable + setVirtualTimePolicy("realtime")
T=3  Renderer processes resume(disableOnResume) → resumes + disables debugger
T=4  Callback arrives → sees backgrounded → skips ForceRedraw/virtual time switch
T=5  Renderer processes Debugger.disable → no-op (already disabled, idempotent)
T=6  Renderer processes setVirtualTimePolicy("realtime") → releases fences
```

Result: Clean. Idempotent commands handle the overlap.

**Race 4: BackgroundTab during action in flight**

An action is running on a tab (phase = kRunning) when a new tab opens. The
action has already resumed execution and is waiting for completion.

```
T=0  Action running on tab A (kRunning, debugger disabled, vtime realtime)
T=1  Page JS opens new tab → BackgroundTab(tab_A)
T=2  backgrounded=true, phase was kRunning
T=3  Debugger.disable → no-op (already disabled from disableOnResume)
T=4  setVirtualTimePolicy("realtime") → no-op (already realtime)
T=5  phase = kDisabled
T=6  Action's PauseExecutionIfNeeded callback fires → sees backgrounded → no-op
```

Result: Action completes, response already sent (response is sent before pause
phase). The tab is left in released state. The action's pause phase is skipped
because the tab is backgrounded.

### 11. Files Changed

- **`abp_controller.h`**:
  - Add `bool backgrounded = false` to `TabState`
  - Declare `BackgroundTab()` and `ForegroundTab()` methods
  - Possibly add `TabStripModelObserver` inheritance for page-interaction detection

- **`abp_controller.cc`**:
  - Implement `BackgroundTab()`: set flag, send cleanup commands
  - Implement `ForegroundTab()`: clear flag, re-enable execution control
  - `CreateTab()`: call `BackgroundTab` on old active tab
  - `ActivateTab()`: call `BackgroundTab` on old, `ForegroundTab` on new
  - All CDP callbacks in execution control chain: add `backgrounded` guard
  - Add `TabStripModelObserver` handling for page-interaction tab opens
  - `PauseExecution()` / action lifecycle: check `backgrounded` before pausing

### 12. What This Does NOT Change

- Action lifecycle (`AbpActionContext`) — unchanged
- Virtual time mechanics — unchanged, just released on background tabs
- Screenshot system — unchanged
- Event observer — unchanged
- Download/dialog/permission handling — unchanged
- CDP idempotency — no changes to V8 or Blink needed
