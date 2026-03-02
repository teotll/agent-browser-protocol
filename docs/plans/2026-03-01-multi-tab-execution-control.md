# Multi-Tab Execution Control Handoff Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Release all execution control (debugger + virtual time) on background tabs to prevent "debugger is paused in another tab" grey-out, and re-establish execution control when tabs are foregrounded.

**Architecture:** Add a `backgrounded` flag to `TabState`. When a tab loses focus (via `CreateTab`, `ActivateTab`, or page-interaction tab opens), release all execution control and set the flag. CDP callbacks in the execution control chain check the flag and no-op if set. Unconditional `Debugger.disable` + `setVirtualTimePolicy("realtime")` cleanup commands handle inflight Mojo commands. When a tab gains focus, clear the flag and re-establish execution control.

**Tech Stack:** C++ (Chromium), CDP (Chrome DevTools Protocol), `TabStripModelObserver` for page-interaction detection

**Design doc:** `docs/plans/2026-03-01-multi-tab-execution-control-design.md`

---

### Task 1: Add `backgrounded` Flag to TabState

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:812-859` (TabState struct)

**Step 1: Add the field**

Add `bool backgrounded = false;` to the `TabState` struct, after the `action_in_flight` field (line 840):

```cpp
    // Deterministic action loop state.
    bool action_in_flight = false;
    uint64_t active_action_epoch = 0;
    uint64_t next_action_epoch = 0;
    std::deque<base::OnceCallback<void(uint64_t)>> queued_action_starters;

    // True when this tab is not the active tab. Execution control is fully
    // released on backgrounded tabs. CDP callbacks in the execution control
    // chain check this flag and no-op if set.
    bool backgrounded = false;
```

**Step 2: Build**

Run: `autoninja -C out/Default chrome`
Expected: Compiles with no errors (field is added but not yet used).

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_controller.h
git commit -m "feat(abp): add backgrounded flag to TabState"
```

---

### Task 2: Implement `BackgroundTab()` and `ForegroundTab()`

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h` (declare methods)
- Modify: `chrome/browser/abp/abp_controller.cc` (implement methods)

**Step 1: Declare methods in header**

Add after `PauseAllTabs()` declaration (line 343):

```cpp
  // Release execution control on a tab that is losing focus.
  // Sets backgrounded=true, sends Debugger.disable + setVirtualTimePolicy("realtime").
  void BackgroundTab(const std::string& tab_id);

  // Re-establish execution control on a tab that is gaining focus.
  // Clears backgrounded=false, calls EnableExecutionControl if global flag is set.
  void ForegroundTab(const std::string& tab_id);
```

**Step 2: Implement `BackgroundTab` in abp_controller.cc**

Add after `PauseAllTabs()` implementation (after line 4219):

```cpp
void AbpController::BackgroundTab(const std::string& tab_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) {
    return;
  }

  TabState& tab = it->second;
  tab.backgrounded = true;

  VLOG(1) << "ABP: Backgrounding tab " << tab_id
          << " phase=" << static_cast<int>(tab.execution.phase);

  // If the debugger is actively paused, resume+disable atomically first.
  // Debugger.resume is NOT idempotent (errors if not paused), so only
  // call it when we know the tab is in kPaused phase.
  if (tab.execution.IsPaused()) {
    content::WebContents* wc = FindWebContents(tab_id);
    if (wc) {
      AbpCdpClient* client = GetOrCreateCdpClient(wc);
      if (client) {
        base::Value::Dict resume_params;
        resume_params.Set("disableOnResume", true);
        VLOG(1) << "ABP: BackgroundTab sending Debugger.resume(disableOnResume) tab=" << tab_id;
        client->SendCommand(
            "Debugger.resume", resume_params,
            base::BindOnce([](bool, const std::string&) {}));
      }
    }
  }

  // Unconditional Debugger.disable — idempotent safety net for any inflight
  // Debugger.enable/pause commands that may have landed in the renderer via
  // Mojo after the resume above. Mojo ordering guarantees this arrives after
  // any inflight commands on the same channel.
  content::WebContents* wc = FindWebContents(tab_id);
  if (wc) {
    AbpCdpClient* client = GetOrCreateCdpClient(wc);
    if (client) {
      base::Value::Dict empty;
      VLOG(1) << "ABP: BackgroundTab sending Debugger.disable tab=" << tab_id;
      client->SendCommand(
          "Debugger.disable", empty,
          base::BindOnce([](bool, const std::string&) {}));

      // Release virtual time fences if execution control was ever enabled.
      if (tab.execution.IsEnabled()) {
        base::Value::Dict vt_params;
        vt_params.Set("policy", "realtime");
        VLOG(1) << "ABP: BackgroundTab sending setVirtualTimePolicy(realtime) tab=" << tab_id;
        client->SendCommand(
            "Emulation.setVirtualTimePolicy", vt_params,
            base::BindOnce([](bool, const std::string&) {}));
      }
    }
  }

  tab.execution.phase = ExecutionPhase::kDisabled;
}
```

**Step 3: Implement `ForegroundTab` in abp_controller.cc**

Add immediately after `BackgroundTab`:

```cpp
void AbpController::ForegroundTab(const std::string& tab_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  auto it = tab_states_.find(tab_id);
  if (it != tab_states_.end()) {
    it->second.backgrounded = false;
  } else {
    GetOrCreateTabState(tab_id).backgrounded = false;
  }

  VLOG(1) << "ABP: Foregrounding tab " << tab_id;

  // Re-establish execution control if global flag is set.
  // EnableExecutionControl starts in kPaused state (debugger paused + vtime frozen).
  if (IsExecutionControlEnabled()) {
    EnableExecutionControl(tab_id, std::nullopt, base::DoNothing());
  }
}
```

**Step 4: Build**

Run: `autoninja -C out/Default chrome`
Expected: Compiles. Methods declared and implemented but not yet called.

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): implement BackgroundTab and ForegroundTab methods"
```

---

### Task 3: Add Backgrounded Guards to CDP Callbacks

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc`

Add an early-return guard at the top of each CDP callback in the execution control chain. The guard checks if the tab has been backgrounded and skips the operation if so.

**Step 1: Add guard to `OnDebuggerEnabled` (line ~3562)**

At the top of the method body, after extracting `tab_id`, add:

```cpp
  // Guard: skip if tab was backgrounded while CDP command was inflight
  {
    auto bit = tab_states_.find(tab_id);
    if (bit != tab_states_.end() && bit->second.backgrounded) {
      VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, skipping OnDebuggerEnabled";
      std::move(then).Run();
      return;
    }
  }
```

**Step 2: Add guard to `EnableVirtualTimeAfterDebugger` (line ~3602)**

Same pattern at the top of the method:

```cpp
  {
    auto bit = tab_states_.find(tab_id);
    if (bit != tab_states_.end() && bit->second.backgrounded) {
      VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, skipping EnableVirtualTimeAfterDebugger";
      std::move(then).Run();
      return;
    }
  }
```

**Step 3: Add guard to `OnVirtualTimeEnabled` (line ~3633)**

Same pattern.

**Step 4: Add guard to `SendDeterministicPause` (line ~3902)**

Same pattern. This one is important — it's the entry point for the pause chain.

**Step 5: Add guard inside `SendDeterministicPause`'s inner lambda callbacks**

The `SendDeterministicPause` method has chained lambdas for `Debugger.enable` → `Debugger.pause`. Add the backgrounded check inside the `Debugger.enable` callback lambda (line ~3926) and the `Debugger.pause` callback lambda (line ~3943):

```cpp
// Inside Debugger.enable callback:
if (ctrl->tab_states_.count(tid) && ctrl->tab_states_[tid].backgrounded) {
  VLOG(1) << "ABP: Tab " << tid << " backgrounded, skipping pause chain";
  std::move(cb).Run();
  return;
}
```

**Step 6: Add guard to `PauseVirtualTimeAfterDebugger` (line ~3864)**

Same pattern.

**Step 7: Add guard to `OnVirtualTimePaused` (line ~3960)**

Same pattern.

**Step 8: Add guard to `ForceRedrawThenResumeVirtualTime` (line ~3769)**

Same pattern.

**Step 9: Add guard to `SwitchToRealtimeVirtualTime` (line ~3800)**

Same pattern.

**Step 10: Add guard to `OnVirtualTimeResumed` (line ~3747)**

Same pattern.

**Step 11: Add guard to `OnDebuggerPausedEvent` (line ~4040)**

This is the handler for the `Debugger.paused` CDP event. If the tab has been backgrounded, skip processing the event:

```cpp
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) return;
  if (it->second.backgrounded) {
    VLOG(1) << "ABP: Tab " << tab_id << " backgrounded, ignoring Debugger.paused event";
    return;
  }
```

**Step 12: Build**

Run: `autoninja -C out/Default chrome`
Expected: Compiles with no errors.

**Step 13: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): add backgrounded guards to execution control CDP callbacks"
```

---

### Task 4: Wire `BackgroundTab`/`ForegroundTab` into `CreateTab` and `ActivateTab`

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc`

**Step 1: Update `CreateTab` (line ~2281)**

After getting the browser but before creating the new tab, background the currently active tab:

```cpp
  // Background the currently active tab before creating a new foreground tab.
  // This releases execution control (debugger + virtual time) on the old tab
  // so the new tab doesn't show "debugger paused in another tab".
  std::string old_active_tab = GetActiveTabId();
  if (!old_active_tab.empty()) {
    BackgroundTab(old_active_tab);
  }
```

Insert this block after the browser null check (line ~2300), before `NavigateParams nav_params`.

**Step 2: Update `ActivateTab` (line ~3167)**

After finding the target tab but before activating it, background the old tab and foreground the new one:

```cpp
        // Background old tab, foreground new tab
        std::string old_active_tab = GetActiveTabId();
        if (!old_active_tab.empty() && old_active_tab != tab_id) {
          BackgroundTab(old_active_tab);
        }
        ForegroundTab(tab_id);

        // Activate the tab
        tab_strip->ActivateTabAt(i);
```

Insert the background/foreground block before `tab_strip->ActivateTabAt(i)` (line 3180).

**Step 3: Build**

Run: `autoninja -C out/Default chrome`
Expected: Compiles.

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): wire BackgroundTab/ForegroundTab into CreateTab and ActivateTab"
```

---

### Task 5: Handle Page-Interaction Tab Opens via TabStripModelObserver

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h` (add observer inheritance + members)
- Modify: `chrome/browser/abp/abp_controller.cc` (implement observer, register/unregister)

**Step 1: Add TabStripModelObserver to AbpController**

In `abp_controller.h`, change the class declaration (line 158):

```cpp
class AbpController : public TabStripModelObserver {
```

Add the required include at the top of the header:

```cpp
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
```

Add the observer method override in the private section (after `PauseAllTabs` or near other private methods):

```cpp
  // TabStripModelObserver:
  void OnTabStripModelChanged(TabStripModel* tab_strip_model,
                              const TabStripModelChange& change,
                              const TabStripSelectionChange& selection) override;
```

Add a member to track which tab strip models we're observing:

```cpp
  // Tab strip models we're observing for page-interaction tab opens.
  std::set<TabStripModel*> observed_tab_strips_;
```

Add the required include:

```cpp
#include <set>
```

**Step 2: Register as observer on existing browsers**

In `abp_controller.cc`, add registration in `PauseAllTabs()` (since this runs at startup and iterates all browsers). After the existing `for (Browser* browser : *BrowserList::GetInstance())` loop body, add observer registration:

```cpp
      // Register as tab strip observer to detect page-interaction tab opens
      if (observed_tab_strips_.find(tab_strip) == observed_tab_strips_.end()) {
        tab_strip->AddObserver(this);
        observed_tab_strips_.insert(tab_strip);
      }
```

Also add registration in `CreateTab()` for any new browser:

```cpp
  // Ensure we're observing this browser's tab strip
  TabStripModel* tab_strip = browser->tab_strip_model();
  if (observed_tab_strips_.find(tab_strip) == observed_tab_strips_.end()) {
    tab_strip->AddObserver(this);
    observed_tab_strips_.insert(tab_strip);
  }
```

**Step 3: Unregister in destructor**

In `AbpController::~AbpController()`, add cleanup:

```cpp
  for (TabStripModel* ts : observed_tab_strips_) {
    ts->RemoveObserver(this);
  }
  observed_tab_strips_.clear();
```

**Step 4: Implement `OnTabStripModelChanged`**

```cpp
void AbpController::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // We only care about active tab changes (selection changes).
  // This catches page-interaction tab opens (window.open, target=_blank)
  // where Chrome automatically foregrounds the new tab.
  if (!selection.active_tab_changed()) {
    return;
  }

  // Don't react if execution control is not enabled globally.
  if (!IsExecutionControlEnabled()) {
    return;
  }

  // Background the old active tab
  if (selection.old_contents) {
    auto old_host = content::DevToolsAgentHost::GetOrCreateFor(
        selection.old_contents);
    std::string old_tab_id = old_host->GetId();

    auto it = tab_states_.find(old_tab_id);
    if (it != tab_states_.end() && !it->second.backgrounded) {
      BackgroundTab(old_tab_id);
    }
  }

  // Foreground the new active tab
  if (selection.new_contents) {
    auto new_host = content::DevToolsAgentHost::GetOrCreateFor(
        selection.new_contents);
    std::string new_tab_id = new_host->GetId();

    auto it = tab_states_.find(new_tab_id);
    // Only foreground if not already foregrounded (avoid duplicate work
    // when ActivateTab already called ForegroundTab).
    if (it == tab_states_.end() || it->second.backgrounded) {
      ForegroundTab(new_tab_id);
    }
  }
}
```

Add required includes in `abp_controller.cc`:

```cpp
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
```

(Check if these are already included; skip if so.)

**Step 5: Build**

Run: `autoninja -C out/Default chrome`
Expected: Compiles.

**Step 6: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): add TabStripModelObserver for page-interaction tab opens"
```

---

### Task 6: Guard `PauseExecution` in Action Lifecycle

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.cc:656-679`

**Step 1: Add backgrounded check to `PauseExecutionIfNeeded`**

The action lifecycle calls `PauseExecution` after an action completes. If the tab
has been backgrounded mid-action (e.g., JS opened a new tab during the action),
skip the pause:

In `PauseExecutionIfNeeded()` (line 656), after the `skip_pause` and
`IsExecutionControlEnabled` checks, add:

```cpp
  // If tab was backgrounded during this action (e.g., page JS opened a new
  // tab), skip pausing — the tab has already been fully released.
  {
    auto it = controller_->tab_states_.find(tab_id_);
    if (it != controller_->tab_states_.end() && it->second.backgrounded) {
      VLOG(1) << "ABP ActionContext: Tab " << tab_id_
              << " backgrounded, skipping PauseExecutionIfNeeded";
      OnExecutionPaused();
      return;
    }
  }
```

Insert after the `IsExecutionControlEnabled` check (line 668-671) and before the
`controller_->PauseExecution(...)` call (line 675).

**Step 2: Build**

Run: `autoninja -C out/Default chrome`
Expected: Compiles.

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_action_context.cc
git commit -m "feat(abp): skip pause in action lifecycle when tab is backgrounded"
```

---

### Task 7: Write Browser Test for Tab Switch Handoff

**Files:**
- Modify: `chrome/browser/abp/abp_action_lifecycle_browsertest.cc`
- Create: `chrome/browser/abp/test_pages/multi_tab_test.html`

**Step 1: Create test page**

Create a minimal test page for multi-tab testing:

```html
<!DOCTYPE html>
<html>
<head><title>Multi-Tab Test</title></head>
<body>
  <h1>Multi-Tab Test Page</h1>
  <p id="status">loaded</p>
</body>
</html>
```

**Step 2: Write browser test for CreateTab backgrounding**

Add a new test to `abp_action_lifecycle_browsertest.cc`:

```cpp
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, CreateTabBackgroundsOldTab) {
  // Navigate the initial tab
  GURL url = embedded_test_server()->GetURL("/multi_tab_test.html");
  auto nav_result = SendRequest("POST", "/api/v1/tabs/" + GetActiveTab() + "/navigate",
                                "{\"url\":\"" + url.spec() + "\"}");
  EXPECT_EQ(nav_result.status, 200);

  // Enable execution control on the initial tab
  std::string initial_tab = GetActiveTab();
  auto exec_result = SendRequest("POST", "/api/v1/tabs/" + initial_tab + "/execution",
                                 "{\"paused\":true}");
  EXPECT_EQ(exec_result.status, 200);

  // Verify initial tab is paused
  auto state_result = SendRequest("GET", "/api/v1/tabs/" + initial_tab + "/execution");
  EXPECT_EQ(state_result.status, 200);
  ASSERT_TRUE(state_result.parsed.FindBool("paused").has_value());
  EXPECT_TRUE(*state_result.parsed.FindBool("paused"));

  // Create a new tab — this should background the initial tab
  auto create_result = SendRequest("POST", "/api/v1/tabs",
                                   "{\"url\":\"" + url.spec() + "\"}");
  EXPECT_EQ(create_result.status, 201);
  const std::string* new_tab_id = create_result.parsed.FindString("id");
  ASSERT_TRUE(new_tab_id);
  EXPECT_NE(*new_tab_id, initial_tab);

  // Verify the initial tab's execution control is disabled (backgrounded)
  auto old_state = SendRequest("GET", "/api/v1/tabs/" + initial_tab + "/execution");
  EXPECT_EQ(old_state.status, 200);
  // Phase should be kDisabled after backgrounding
  ASSERT_TRUE(old_state.parsed.FindBool("paused").has_value());
  EXPECT_FALSE(*old_state.parsed.FindBool("paused"));
}
```

**Step 3: Write browser test for ActivateTab re-pauses**

```cpp
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, ActivateTabForegroundsAndPauses) {
  GURL url = embedded_test_server()->GetURL("/multi_tab_test.html");

  // Navigate initial tab
  std::string tab_a = GetActiveTab();
  SendRequest("POST", "/api/v1/tabs/" + tab_a + "/navigate",
              "{\"url\":\"" + url.spec() + "\"}");

  // Enable execution control
  SendRequest("POST", "/api/v1/tabs/" + tab_a + "/execution",
              "{\"paused\":true}");

  // Create tab B (backgrounds tab A)
  auto create_result = SendRequest("POST", "/api/v1/tabs",
                                   "{\"url\":\"" + url.spec() + "\"}");
  ASSERT_EQ(create_result.status, 201);
  std::string tab_b = *create_result.parsed.FindString("id");

  // Tab A should be backgrounded (execution disabled)
  auto state_a = SendRequest("GET", "/api/v1/tabs/" + tab_a + "/execution");
  EXPECT_FALSE(*state_a.parsed.FindBool("paused"));

  // Activate tab A — should foreground and re-pause it
  auto activate_result = SendRequest("POST", "/api/v1/tabs/" + tab_a + "/activate", "{}");
  EXPECT_EQ(activate_result.status, 200);

  // Wait briefly for EnableExecutionControl to complete (async CDP chain)
  base::RunLoop run_loop;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, run_loop.QuitClosure(), base::Milliseconds(500));
  run_loop.Run();

  // Tab A should be paused again
  auto state_a2 = SendRequest("GET", "/api/v1/tabs/" + tab_a + "/execution");
  EXPECT_EQ(state_a2.status, 200);
  ASSERT_TRUE(state_a2.parsed.FindBool("paused").has_value());
  EXPECT_TRUE(*state_a2.parsed.FindBool("paused"));
}
```

**Step 4: Add `GetActiveTab` helper if not already present**

Check if the test fixture already has this helper. If not, add:

```cpp
  std::string GetActiveTab() {
    auto result = SendRequest("GET", "/api/v1/tabs");
    // Find the active tab from the list, or use the controller directly
    return controller_->GetActiveTabId();
  }
```

**Step 5: Build and run tests**

Run: `autoninja -C out/Default browser_tests`
Run: `./out/Default/browser_tests --gtest_filter='AbpActionLifecycleTest.CreateTabBackgroundsOldTab:AbpActionLifecycleTest.ActivateTabForegroundsAndPauses'`
Expected: Both tests pass.

**Step 6: Commit**

```bash
git add chrome/browser/abp/abp_action_lifecycle_browsertest.cc chrome/browser/abp/test_pages/multi_tab_test.html
git commit -m "test(abp): add browser tests for multi-tab execution control handoff"
```

---

### Task 8: Manual Integration Test

**Files:**
- No code changes — manual verification

**Step 1: Build**

Run: `autoninja -C out/Default chrome`

**Step 2: Launch ABP**

Run: `./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run`

**Step 3: Test CreateTab handoff**

```bash
# Check status
curl http://localhost:8222/api/v1/browser/status

# Get initial tab
curl http://localhost:8222/api/v1/tabs

# Navigate initial tab
TAB_A=$(curl -s http://localhost:8222/api/v1/tabs | python3 -c "import sys,json; print(json.load(sys.stdin)[0]['id'])")
curl -X POST http://localhost:8222/api/v1/tabs/$TAB_A/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'

# Wait for page load
sleep 2

# Verify tab A is paused (execution control auto-enabled)
curl http://localhost:8222/api/v1/tabs/$TAB_A/execution

# Create new tab — should not show grey-out
TAB_B=$(curl -s -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.org"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")

# Verify: no "debugger paused in another tab" on tab B
# Verify: tab A execution is disabled
curl http://localhost:8222/api/v1/tabs/$TAB_A/execution

# Switch back to tab A — should re-pause
curl -X POST http://localhost:8222/api/v1/tabs/$TAB_A/activate \
  -H "Content-Type: application/json" -d '{}'
sleep 1
curl http://localhost:8222/api/v1/tabs/$TAB_A/execution
```

**Step 4: Test page-interaction tab open**

```bash
# Navigate to a page with a link that opens a new tab
curl -X POST http://localhost:8222/api/v1/tabs/$TAB_A/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'
sleep 2

# Execute JS to open a new tab
curl -X POST http://localhost:8222/api/v1/tabs/$TAB_A/execute \
  -H "Content-Type: application/json" \
  -d '{"script":"window.open(\"https://example.org\", \"_blank\")"}'

# Verify: new tab opens without grey-out
# Verify: tab A is backgrounded
curl http://localhost:8222/api/v1/tabs/$TAB_A/execution
```

Expected: No "debugger paused in another tab" grey-out in any scenario. Background tabs show `paused: false`. Foregrounded tabs show `paused: true` after re-establishment.
