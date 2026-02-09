# Action Serialization & Epoch Guard Hardening

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Harden ABP's per-tab action serialization with timeouts, proper cleanup on tab close, queue backpressure, controller lifetime safety, a formalized execution state machine, consolidated error paths, and an action-aware auto-pause timer.

**Architecture:** All changes are in `chrome/browser/abp/`. The serialization layer lives in `AbpController` (queue + epoch) and `AbpActionContext` (lifecycle + guards). Execution control (debugger + virtual time) is spread across `AbpController` methods. The auto-pause timer is in `AbpHttpServer`. We introduce an explicit `ExecutionPhase` enum to replace ad-hoc booleans, add a `Fail()` method to consolidate error teardown, and wire timeout/cleanup/backpressure around the existing queue.

**Tech Stack:** C++ (Chromium base library), CDP (Chrome DevTools Protocol), `base::OneShotTimer`, `base::WeakPtr`, `base::RefCounted`

---

## Task 1: Add Action-Level Timeout to AbpActionContext

Prevents permanent queue stalls when a CDP command hangs or `WaitForActionComplete` never returns.

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.h:178-234` (add timer field + constant)
- Modify: `chrome/browser/abp/abp_action_context.cc:66-115` (start timer in `Start()`)
- Modify: `chrome/browser/abp/abp_action_context.cc:483-568` (cancel timer in `SendResponse()`)
- Modify: `chrome/browser/abp/abp_action_context.cc:570-595` (cancel timer in `SendErrorResponse()`)

**Step 1: Add timeout constant and timer field to header**

In `abp_action_context.h`, add inside the private section (after line 232, before `weak_factory_`):

```cpp
  // Action-level timeout to prevent permanent queue stalls.
  static constexpr base::TimeDelta kActionTimeout = base::Seconds(30);
  base::OneShotTimer action_timeout_timer_;
```

Also add `#include "base/timer/timer.h"` to the includes at the top.

Add private method declaration (after `SendErrorResponse` declarations, ~line 176):

```cpp
  void OnActionTimeout();
```

**Step 2: Start the timer in Start()**

In `abp_action_context.cc`, inside `Start()` after `prevent_destroy_ = this;` (after line 71), add:

```cpp
  // Start action-level timeout watchdog
  action_timeout_timer_.Start(
      FROM_HERE, kActionTimeout,
      base::BindOnce(&AbpActionContext::OnActionTimeout,
                     base::Unretained(this)));
```

Using `Unretained` is safe here because `prevent_destroy_` holds a ref to `this` for the entire action lifetime, and the timer is owned by `this`.

**Step 3: Implement OnActionTimeout()**

In `abp_action_context.cc`, add after `SendErrorResponse()`:

```cpp
void AbpActionContext::OnActionTimeout() {
  LOG(WARNING) << "ABP ActionContext: Action timed out after "
               << kActionTimeout.InSeconds() << "s, action=" << action_type_
               << " tab=" << tab_id_;
  if (!IsCurrentAction()) {
    return;
  }
  has_error_ = true;
  error_code_ = "ACTION_TIMEOUT";
  error_message_ = "Action timed out after " +
                    base::NumberToString(kActionTimeout.InSeconds()) + " seconds";

  // Skip the normal pause flow — just release and respond immediately.
  // The next queued action will re-establish correct execution state.
  RecordHistory(false, error_code_, error_message_);
  if (response_callback_) {
    controller_->SendError(504, error_message_, std::move(response_callback_));
  }
  ReleaseDeterministicSlot();
  prevent_destroy_ = nullptr;
}
```

Add `#include "base/strings/number_string_conversions.h"` to the `.cc` includes.

**Step 4: Cancel the timer on normal completion**

In `SendResponse()`, add at the top of the function (after line 483, before the `IsCurrentAction` check):

```cpp
  action_timeout_timer_.Stop();
```

In `SendErrorResponse()`, add at the top of the function (after line 570, before the `IsCurrentAction` check):

```cpp
  action_timeout_timer_.Stop();
```

**Step 5: Build and verify**

Run:
```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```
Expected: Build succeeds with no errors.

**Step 6: Commit**

```bash
git add chrome/browser/abp/abp_action_context.h chrome/browser/abp/abp_action_context.cc
git commit -m "feat(abp): add 30s action-level timeout to prevent permanent queue stalls"
```

---

## Task 2: Fail Queued Actions on Tab Close

When a tab is closed, all queued actions should receive an error response instead of silently hanging.

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:595-645` (store response callbacks alongside starters)
- Modify: `chrome/browser/abp/abp_controller.cc:133-172` (update `CleanupTabState`, `RunOrQueueDeterministicAction`, `FinishDeterministicAction`)

**Step 1: Change the queued starter type to include a response callback**

The current queue stores `base::OnceCallback<void(uint64_t)>` — the starter that creates an `AbpActionContext`. The `AbpActionContext` already owns its `response_callback_`, so when the `AbpActionContext` is destroyed (because its starter was in the deque that got erased), the response is dropped.

The fix: instead of changing the queue type, we change `CleanupTabState` to drain the queue by invoking each queued starter with a sentinel epoch of 0. The `AbpActionContext::StartOnDeterministicSlot` will set `action_epoch_ = 0` and `deterministic_slot_active_ = true`, then call `Start()`. Inside `Start()`, `IsCurrentAction()` will return false (since the tab state was just erased), triggering a clean exit. But we need a better approach — the `AbpActionContext` holds a `scoped_refptr` via the bound callback, so it stays alive.

Actually, the simplest approach: invoke each queued starter, and the `AbpActionContext` will find the tab missing in `Start()` and call `SendErrorResponse(404, ...)`, which releases the slot and sends an error to the client. But the tab state is already erased, so `ReleaseDeterministicSlot()` → `FinishDeterministicAction()` will do nothing (tab not found in map).

In `abp_controller.cc`, replace `CleanupTabState`:

```cpp
void AbpController::CleanupTabState(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) {
    return;
  }

  // Drain queued actions — each AbpActionContext will discover the tab
  // is gone during Start() and send a 404 error response to the client.
  auto queued = std::move(it->second.queued_action_starters);
  tab_states_.erase(it);

  for (auto& starter : queued) {
    // Epoch 0 is a sentinel — IsDeterministicActionCurrent will return false
    // since the tab state no longer exists, but the AbpActionContext::Start()
    // will run far enough to find the tab missing and send an error response.
    std::move(starter).Run(0);
  }
}
```

**Step 2: Build and verify**

Run:
```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```
Expected: Build succeeds.

**Step 3: Manual verification**

1. Launch browser: `./out/Default/Chromium.app/Contents/MacOS/Chromium --enable-abp --no-first-run`
2. Create a tab, start a long action (e.g., navigate to a slow page)
3. While it's loading, queue another action via curl
4. Close the tab via `DELETE /api/v1/tabs/{id}`
5. Verify the queued action's curl gets a 404 error response, not a hang

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc
git commit -m "fix(abp): drain queued actions on tab close instead of silently dropping"
```

---

## Task 3: Add Queue Depth Limit with HTTP 429 Backpressure

Prevent unbounded memory growth from a misbehaving client flooding actions.

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:643-648` (add constant + new method)
- Modify: `chrome/browser/abp/abp_controller.cc:137-149` (check queue depth)

**Step 1: Add constant and method declaration**

In `abp_controller.h`, add near the other deterministic action declarations (~line 647):

```cpp
  static constexpr size_t kMaxQueuedActionsPerTab = 50;
```

**Step 2: Add queue depth check in RunOrQueueDeterministicAction**

In `abp_controller.cc`, modify `RunOrQueueDeterministicAction`. Replace lines 137-149 with:

```cpp
void AbpController::RunOrQueueDeterministicAction(
    const std::string& tab_id,
    base::OnceCallback<void(uint64_t)> starter) {
  TabState& state = GetOrCreateTabState(tab_id);
  if (state.action_in_flight) {
    if (state.queued_action_starters.size() >= kMaxQueuedActionsPerTab) {
      LOG(WARNING) << "ABP: Action queue full for tab " << tab_id
                   << " (" << kMaxQueuedActionsPerTab << " queued), rejecting";
      // Invoke with sentinel epoch 0 — the context will discover the
      // epoch mismatch and fail. But we need the context to get a proper
      // 429 response. Instead, just invoke the starter; the context will
      // run Start() and succeed, but we need a way to signal rejection.
      // Simplest: just invoke with epoch 0; Start() will proceed but
      // IsCurrentAction() will be false for subsequent callbacks.
      // However, Start() sends error before any IsCurrentAction check.
      // Actually, Start() doesn't check IsCurrentAction — it checks
      // if tab exists and CDP client works. We need a different approach.
      //
      // Better: don't invoke the starter at all. The AbpActionContext is
      // ref-counted and held alive by the bound starter callback. When we
      // destroy the starter without invoking it, the context destructor
      // fires, calling ReleaseDeterministicSlot() (no-op since slot was
      // never acquired) and the response_callback_ is destroyed.
      // That drops the HTTP connection without a response.
      //
      // Best approach: add a rejection callback path.
      return;
    }
    state.queued_action_starters.push_back(std::move(starter));
    return;
  }

  state.action_in_flight = true;
  state.active_action_epoch = ++state.next_action_epoch;
  std::move(starter).Run(state.active_action_epoch);
}
```

Wait — the above has a problem. The starter callback binds an `AbpActionContext` that owns the `response_callback_`. If we drop the starter, the response callback is destroyed without being invoked. We need a separate rejection mechanism.

**Better approach:** Add a rejection callback to `RunOrQueueDeterministicAction`, or handle the check at the call site (`AbpActionContext::RunWithOptions`).

Replace `RunOrQueueDeterministicAction` signature in the header (~line 644):

```cpp
  // Returns true if the action was started or queued, false if rejected
  // (queue full). Caller is responsible for sending 429 on rejection.
  bool RunOrQueueDeterministicAction(
      const std::string& tab_id,
      base::OnceCallback<void(uint64_t)> starter);
```

In `abp_controller.cc`, replace the implementation:

```cpp
bool AbpController::RunOrQueueDeterministicAction(
    const std::string& tab_id,
    base::OnceCallback<void(uint64_t)> starter) {
  TabState& state = GetOrCreateTabState(tab_id);
  if (state.action_in_flight) {
    if (state.queued_action_starters.size() >= kMaxQueuedActionsPerTab) {
      LOG(WARNING) << "ABP: Action queue full for tab " << tab_id
                   << " (" << kMaxQueuedActionsPerTab << " queued)";
      return false;
    }
    state.queued_action_starters.push_back(std::move(starter));
    return true;
  }

  state.action_in_flight = true;
  state.active_action_epoch = ++state.next_action_epoch;
  std::move(starter).Run(state.active_action_epoch);
  return true;
}
```

Then modify the call site in `AbpActionContext::RunWithOptions` (`abp_action_context.cc:25-39`):

```cpp
void AbpActionContext::RunWithOptions(AbpController* controller,
                                      const std::string& tab_id,
                                      const std::string& action_type,
                                      const base::Value::Dict& params,
                                      const Options& options,
                                      ActionCallback action,
                                      ResponseCallback response) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  auto ctx = base::MakeRefCounted<AbpActionContext>(
      controller, tab_id, action_type, params, options, std::move(action),
      std::move(response));

  bool accepted = controller->RunOrQueueDeterministicAction(
      tab_id,
      base::BindOnce(&AbpActionContext::StartOnDeterministicSlot, ctx));

  if (!accepted) {
    // Queue is full — send 429 directly. The context was never started
    // so we bypass the normal flow and just consume the response callback.
    ctx->SendErrorResponse(429, "QUEUE_FULL",
                           "Too many queued actions for this tab");
  }
}
```

**Problem:** `SendErrorResponse` checks `IsCurrentAction()` which will return false since we never acquired a slot. We need to handle this. Add a special case at the top of `SendErrorResponse` or bypass it:

Actually, `SendErrorResponse` at line 573 checks `IsCurrentAction()` and returns if false. Since we never called `StartOnDeterministicSlot`, `deterministic_slot_active_` is false, so `IsCurrentAction()` returns false. We need a different path.

Create a direct rejection method. In `abp_action_context.h`, add a public method:

```cpp
  // Send a rejection response without entering the action flow.
  // Used when the action is rejected before starting (e.g., queue full).
  void RejectBeforeStart(int status,
                         const std::string& error_code,
                         const std::string& error_message);
```

In `abp_action_context.cc`:

```cpp
void AbpActionContext::RejectBeforeStart(int status,
                                         const std::string& error_code,
                                         const std::string& error_message) {
  if (!response_callback_) {
    return;
  }
  controller_->SendError(status, error_message, std::move(response_callback_));
}
```

Then the call site becomes:

```cpp
  if (!accepted) {
    ctx->RejectBeforeStart(429, "QUEUE_FULL",
                           "Too many queued actions for this tab");
  }
```

**Step 3: Build and verify**

Run:
```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```
Expected: Build succeeds.

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_action_context.h chrome/browser/abp/abp_action_context.cc chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): add queue depth limit (50) with HTTP 429 backpressure"
```

---

## Task 4: Protect Against Controller Destruction

Prevent use-after-free if `AbpController` is destroyed while action contexts exist.

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.h:179` (change `raw_ptr` to `base::WeakPtr`)
- Modify: `chrome/browser/abp/abp_action_context.cc` (add null checks after controller access)
- Modify: `chrome/browser/abp/abp_controller.h` (add `WeakPtrFactory` if not present)

**Step 1: Check if AbpController already has a WeakPtrFactory**

Look for `weak_factory_` in `abp_controller.h`. It already exists (used by `PauseExecution`, `ResumeExecution`, etc.). Confirm the type.

In `abp_action_context.h`, change line 179:

```cpp
  // Old:
  raw_ptr<AbpController> controller_;
  // New:
  base::WeakPtr<AbpController> controller_;
```

**Step 2: Update constructor**

In `abp_action_context.cc`, update constructor (~line 41-54). Change:

```cpp
    : controller_(controller),
```
to:
```cpp
    : controller_(controller->GetWeakPtr()),
```

Add a `GetWeakPtr()` method to `AbpController` if it doesn't exist. In `abp_controller.h`, add in the public section:

```cpp
  base::WeakPtr<AbpController> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }
```

**Step 3: Add null guard in destructor**

In `abp_action_context.cc`, update the destructor:

```cpp
AbpActionContext::~AbpActionContext() {
  if (controller_) {
    ReleaseDeterministicSlot();
  }
}
```

**Step 4: Add null guard in ReleaseDeterministicSlot**

In `abp_action_context.cc`, update `ReleaseDeterministicSlot()`:

```cpp
void AbpActionContext::ReleaseDeterministicSlot() {
  if (!deterministic_slot_active_) {
    return;
  }
  deterministic_slot_active_ = false;
  if (controller_) {
    controller_->FinishDeterministicAction(tab_id_, action_epoch_);
  }
}
```

**Step 5: Add null guard in IsCurrentAction**

```cpp
bool AbpActionContext::IsCurrentAction() const {
  if (!deterministic_slot_active_) {
    return false;
  }
  if (!controller_) {
    return false;
  }
  return controller_->IsDeterministicActionCurrent(tab_id_, action_epoch_);
}
```

**Step 6: Audit all other `controller_->` calls in abp_action_context.cc**

Each method that calls `controller_->` is already guarded by an `IsCurrentAction()` check (which now validates the controller is alive). The exceptions are:
- `Start()` (line 66): First call — `controller_` must be valid since `RunWithOptions` just used it. Add a guard anyway:
  ```cpp
  if (!controller_) {
    SendErrorResponse(500, "CONTROLLER_GONE", "Controller was destroyed");
    return;
  }
  ```
  But `SendErrorResponse` also uses `controller_`. Use the response callback directly:
  ```cpp
  if (!controller_) {
    if (response_callback_) {
      // Can't use controller_->SendError, so write directly
      // Actually, we can't send HTTP response without the controller.
      // Just release and let the HTTP connection close.
    }
    prevent_destroy_ = nullptr;
    return;
  }
  ```

This is a very rare edge case (controller destroyed mid-action). For pragmatism, add null checks only where they prevent crashes: destructor, `ReleaseDeterministicSlot`, `IsCurrentAction`, and `RejectBeforeStart`. All other `controller_->` calls are after `IsCurrentAction()` checks.

**Step 7: Build and verify**

Run:
```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```
Expected: Build succeeds.

**Step 8: Commit**

```bash
git add chrome/browser/abp/abp_action_context.h chrome/browser/abp/abp_action_context.cc chrome/browser/abp/abp_controller.h
git commit -m "fix(abp): use WeakPtr for controller in AbpActionContext to prevent use-after-free"
```

---

## Task 5: Formalize Execution State Machine

Replace scattered booleans with an explicit enum and add DCHECK assertions on valid transitions.

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:490-496` (replace `ExecutionState` struct)
- Modify: `chrome/browser/abp/abp_controller.cc` (update all sites that read/write execution state booleans)

**Step 1: Define the enum and update the struct**

In `abp_controller.h`, replace the `ExecutionState` struct (lines 490-496):

```cpp
  // Execution control state machine.
  // Transitions:
  //   kDisabled → kPaused    (EnableExecutionControl)
  //   kPaused → kResuming    (ResumeExecution called)
  //   kResuming → kRunning   (Debugger.resume + virtual time resume confirmed)
  //   kRunning → kPausing    (PauseExecution called)
  //   kPausing → kPaused     (virtual time pause + Debugger.pause confirmed)
  //   any → kDisabled        (tab close / cleanup)
  enum class ExecutionPhase {
    kDisabled,   // Execution control not enabled for this tab
    kPaused,     // JS halted + virtual time frozen
    kResuming,   // Resume in progress (CDP commands sent, awaiting callbacks)
    kRunning,    // JS running + virtual time advancing
    kPausing,    // Pause in progress (CDP commands sent, awaiting callbacks)
  };

  struct ExecutionState {
    ExecutionPhase phase = ExecutionPhase::kDisabled;
    double virtual_time_base_ticks_ms = 0;

    bool IsEnabled() const { return phase != ExecutionPhase::kDisabled; }
    bool IsPaused() const { return phase == ExecutionPhase::kPaused; }
    bool IsRunning() const { return phase == ExecutionPhase::kRunning; }
  };
```

**Step 2: Update EnableExecutionControl**

Find the function in `abp_controller.cc` (search for `EnableExecutionControl` or `Debugger.enable`). The function that sets `debugger_enabled = true` and `virtual_time_enabled` is around line 3160-3170. Update:

- Replace `it->second.execution.debugger_enabled = true;` with `it->second.execution.phase = ExecutionPhase::kPaused;`
- Remove any `virtual_time_enabled = true` assignment (captured by phase)

**Step 3: Update ResumeExecution**

Find `ResumeExecution` (~line 3079). Update:
- Replace the guard `!it->second.execution.debugger_enabled` with `!it->second.execution.IsEnabled()`
- Add at entry: `DCHECK(it->second.execution.IsPaused() || it->second.execution.phase == ExecutionPhase::kResuming);`
- Set `it->second.execution.phase = ExecutionPhase::kResuming;` at entry
- Replace `it->second.execution.paused = false;` (line 3208) with `it->second.execution.phase = ExecutionPhase::kRunning;`

**Step 4: Update PauseExecution**

Find `PauseExecution` (~line 3229). Update:
- Replace guard `!it->second.execution.debugger_enabled` with `!it->second.execution.IsEnabled()`
- Set `it->second.execution.phase = ExecutionPhase::kPausing;` at entry
- Replace `it->second.execution.paused = true;` (line 3312) with `it->second.execution.phase = ExecutionPhase::kPaused;`

**Step 5: Update all reads of the old booleans**

Use grep to find all remaining references to `execution.paused`, `execution.debugger_enabled`, `execution.virtual_time_enabled` and update them:

| Old | New |
|-----|-----|
| `execution.paused` | `execution.IsPaused()` |
| `execution.debugger_enabled` | `execution.IsEnabled()` |
| `execution.virtual_time_enabled` | `execution.IsEnabled()` |
| `execution.paused = false` | `execution.phase = ExecutionPhase::kRunning` |
| `execution.paused = true` | `execution.phase = ExecutionPhase::kPaused` |

Key locations to update (from grep results):
- Line 109: `IsIdle()` — change `!execution.debugger_enabled` to `!execution.IsEnabled()`
- Line 3079: `ResumeExecution` guard
- Line 3232: `PauseExecution` guard
- Line 3322-3323: `GetVirtualTimeMs` guard — `!execution.paused || !execution.virtual_time_enabled` → `!execution.IsPaused()`
- Line 3350: another guard — `execution.paused` → `execution.IsPaused()`
- Line 3356: `!execution.virtual_time_enabled` → `!execution.IsEnabled()`
- Line 3437: guard — `!execution.debugger_enabled` → `!execution.IsEnabled()`
- Line 3985: `execution.virtual_time_enabled` → `execution.IsEnabled()`

Also update `StopEventCaptureAndGetScrollPosition` in `abp_action_context.cc` line 314:
- `it->second.execution.paused` → `it->second.execution.IsPaused()`

And the `SendResponse` virtual time check in `abp_action_context.cc` line 554:
- `exec.paused` → `exec.IsPaused()` and update the virtual_time_base_ticks_ms read

**Step 6: Build and verify**

Run:
```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```
Expected: Build succeeds. Fix any remaining references the grep didn't catch.

**Step 7: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc chrome/browser/abp/abp_action_context.cc
git commit -m "refactor(abp): replace execution state booleans with explicit ExecutionPhase state machine"
```

---

## Task 6: Consolidate Error Paths into Single Fail() Method

Currently there are two different error flows (early `SendErrorResponse` in `Start()` vs. `OnActionError()` → `PauseExecution` → `FinalizeResponse`). Unify them.

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.h` (add `Fail()` method)
- Modify: `chrome/browser/abp/abp_action_context.cc` (implement `Fail()`, update all error sites)

**Step 1: Add Fail() declaration**

In `abp_action_context.h`, add in the private section (near `SendErrorResponse`):

```cpp
  // Unified teardown: pause → record → respond → release slot → clear self-ref.
  // Safe to call from any point in the action lifecycle.
  void Fail(int http_status,
            const std::string& error_code,
            const std::string& error_message);
```

**Step 2: Implement Fail()**

In `abp_action_context.cc`, add:

```cpp
void AbpActionContext::Fail(int http_status,
                            const std::string& error_code,
                            const std::string& error_message) {
  action_timeout_timer_.Stop();

  has_error_ = true;
  error_code_ = error_code;
  error_message_ = error_message;

  RecordHistory(false, error_code, error_message);

  if (response_callback_ && controller_) {
    controller_->SendError(http_status, error_message,
                           std::move(response_callback_));
  }

  ReleaseDeterministicSlot();
  prevent_destroy_ = nullptr;
}
```

**Step 3: Update early error sites in Start()**

Replace the two `SendErrorResponse` calls in `Start()`:

```cpp
  // Tab not found (line ~99)
  if (!web_contents_) {
    Fail(404, "TAB_NOT_FOUND", "Tab not found");
    return;
  }

  // CDP error (line ~106)
  if (!client_) {
    Fail(500, "CDP_ERROR", "Failed to create CDP client");
    return;
  }
```

**Step 4: Update OnActionError()**

Replace `OnActionError` to use `Fail()` instead of jumping to `PauseExecutionIfNeeded()`:

```cpp
void AbpActionContext::OnActionError(const std::string& error_code,
                                     const std::string& error_message) {
  if (!IsCurrentAction()) {
    return;
  }
  Fail(500, error_code, error_message);
}
```

Note: This changes behavior — previously `OnActionError` would still pause execution before responding. If preserving the pause-before-error-response behavior is important (to leave the tab in a deterministic state for the next action), keep the old `OnActionError` flow and only use `Fail()` for the early-exit cases in `Start()` and `OnActionTimeout()`. The implementing engineer should evaluate this trade-off. If the next action always does `ResumeExecutionIfNeeded()` at start, then skipping the pause on error is safe.

**Step 5: Update OnActionTimeout() to use Fail()**

```cpp
void AbpActionContext::OnActionTimeout() {
  LOG(WARNING) << "ABP ActionContext: Action timed out after "
               << kActionTimeout.InSeconds() << "s, action=" << action_type_
               << " tab=" << tab_id_;
  if (!IsCurrentAction()) {
    return;
  }
  Fail(504, "ACTION_TIMEOUT",
       "Action timed out after " +
           base::NumberToString(kActionTimeout.InSeconds()) + " seconds");
}
```

**Step 6: Remove now-redundant SendErrorResponse if all callers migrated**

If all error paths now use `Fail()`, `SendErrorResponse()` can be removed. Otherwise keep it for `FinalizeResponse()` which calls it on `has_error_`. Evaluate which is cleaner.

**Step 7: Build and verify**

Run:
```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```
Expected: Build succeeds.

**Step 8: Commit**

```bash
git add chrome/browser/abp/abp_action_context.h chrome/browser/abp/abp_action_context.cc
git commit -m "refactor(abp): consolidate error paths into unified Fail() teardown method"
```

---

## Task 7: Make Auto-Pause Timer Action-Aware

The auto-pause timer (`AbpHttpServer::AutoPauseAllTabs`) fires unconditionally after 5 seconds, mutating execution state mid-action. Make it check `action_in_flight` and defer.

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h` (add `PauseAllTabsIfIdle()` or similar)
- Modify: `chrome/browser/abp/abp_controller.cc:3369-3386` (add action-in-flight check)
- Modify: `chrome/browser/abp/abp_http_server.cc:197-206` (use the new method)

**Step 1: Update PauseAllTabs to skip tabs with actions in flight**

In `abp_controller.cc`, modify `PauseAllTabs()` (~line 3369-3386):

```cpp
void AbpController::PauseAllTabs() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!IsExecutionControlEnabled()) {
    return;
  }

  LOG(INFO) << "ABP: Auto-pausing all idle tabs on startup";
  for (Browser* browser : *BrowserList::GetInstance()) {
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
      std::string tab_id = host->GetId();

      // Skip tabs with an action currently executing — the action's
      // own PauseExecutionIfNeeded() will pause when it completes.
      auto it = tab_states_.find(tab_id);
      if (it != tab_states_.end() && it->second.action_in_flight) {
        LOG(INFO) << "ABP: Skipping auto-pause for tab " << tab_id
                  << " (action in flight)";
        continue;
      }

      PauseExecution(tab_id, base::DoNothing());
    }
  }
}
```

**Step 2: Remove the workaround in StopEventCaptureAndGetScrollPosition**

Now that the auto-pause timer won't fire during an action, the "externally paused" check in `AbpActionContext::StopEventCaptureAndGetScrollPosition()` (lines 308-331) should no longer be needed. However, keep it as a defensive guard — there could be other reasons execution gets paused externally (e.g., user API call to `/tabs/{id}/execution` endpoint). Add a comment explaining the remaining purpose:

```cpp
  // Defensive guard: execution may have been paused externally (e.g., via
  // the /execution API endpoint). The auto-pause timer no longer fires
  // during actions, but other pause sources are still possible.
```

**Step 3: Build and verify**

Run:
```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```
Expected: Build succeeds.

**Step 4: Integration test**

1. Launch browser with `--enable-abp`
2. Within 5 seconds (before auto-pause fires), send a navigate action
3. Verify the navigate completes normally (action pauses at end on its own)
4. Verify tabs without actions are paused after 5 seconds

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc chrome/browser/abp/abp_action_context.cc
git commit -m "fix(abp): make auto-pause timer skip tabs with actions in flight"
```

---

## Integration Testing

After all 7 tasks, run the full integration test suite:

```bash
cd chrome/browser/abp/test_pages
# Start a test page server
python3 -m http.server 8081 &

# In another terminal, launch the browser
./out/Default/Chromium.app/Contents/MacOS/Chromium --enable-abp --abp-session-dir=sessions/test --no-first-run &

# Wait for browser ready
sleep 8

# Run the test suite
bash run_tests.sh
```

Expected: All 13 tests pass. The changes are backward-compatible — no API surface changed (except the new 429 response on queue overflow, which only triggers under pathological load).

---

## Summary

| Task | What | Risk | LOC |
|------|------|------|-----|
| 1 | Action timeout (30s) | Low — only fires on stuck actions | ~30 |
| 2 | Drain queue on tab close | Low — queued contexts discover tab missing | ~15 |
| 3 | Queue depth limit (50) + 429 | Low — only affects flood scenarios | ~40 |
| 4 | WeakPtr for controller | Medium — touches all controller accesses | ~25 |
| 5 | ExecutionPhase state machine | Medium — touches many execution control sites | ~60 |
| 6 | Consolidated Fail() method | Low — replaces duplicate error paths | ~30 |
| 7 | Action-aware auto-pause | Low — simple guard in PauseAllTabs | ~15 |
