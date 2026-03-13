# Animation Wait Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an `animation` boolean parameter to `browser_wait` that guarantees 5 seconds of page execution time for CSS/JS animations to complete, running in parallel with network settling.

**Architecture:** New `animation_wait_time` field flows through `Options` → `DoWaitUntil` → `WaitForActionComplete` → `ActionCompleteWaiter`. A dedicated timer starts immediately (not gated on load events) and runs in parallel with all other wait conditions. `IsComplete()` requires the animation timer to elapse before returning true.

**Tech Stack:** C++ (Chromium), base::TimeDelta, PostDelayedTask

**Spec:** `docs/plans/2026-03-13-animation-wait-design.md`

---

## Task 1: Add animation_wait_time to Options struct

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.h:96-106`

- [ ] **Step 1: Add field to Options**

After line 96 (`post_tracking_settle_time`), add:

```cpp
    // Animation wait: minimum page execution time for CSS/JS animations.
    // Runs in parallel with all other wait phases. Zero = disabled.
    base::TimeDelta animation_wait_time;
```

- [ ] **Step 2: Commit**

```bash
git add chrome/browser/abp/abp_action_context.h
git commit -m "feat(abp): add animation_wait_time to Options struct"
```

---

## Task 2: Add animation fields to ActionCompleteWaiter and update IsComplete()

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:827-882`

- [ ] **Step 1: Add animation fields to ActionCompleteWaiter**

After line 828 (`bool min_wait_timer_started = false;`), add:

```cpp
    bool animation_time_elapsed = false;
    bool animation_timer_started = false;
```

- [ ] **Step 2: Update IsComplete()**

Replace the `action_complete` branch at lines 879-882:

```cpp
      if (wait_type == "action_complete") {
        return load_fired && dom_content_loaded_fired &&
               first_paint_fired && min_time_elapsed &&
               tracked_requests_resolved && post_tracking_settled;
      }
```

With:

```cpp
      if (wait_type == "action_complete") {
        bool animation_ok = !animation_timer_started || animation_time_elapsed;
        return load_fired && dom_content_loaded_fired &&
               first_paint_fired && min_time_elapsed &&
               tracked_requests_resolved && post_tracking_settled &&
               animation_ok;
      }
```

- [ ] **Step 3: Commit**

```bash
git add chrome/browser/abp/abp_controller.h
git commit -m "feat(abp): add animation timer fields to ActionCompleteWaiter"
```

---

## Task 3: Wire animation_wait_time through WaitForActionComplete

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:410-417` (declaration)
- Modify: `chrome/browser/abp/abp_action_context.cc:620-628` (DoWaitUntil forwarding)
- Modify: `chrome/browser/abp/abp_controller.cc:5105-5166` (WaitForActionComplete impl)

- [ ] **Step 1: Add parameter to WaitForActionComplete declaration**

In `abp_controller.h`, update the declaration at line 410-417 to add the new parameter with a default of zero:

```cpp
  void WaitForActionComplete(
      const std::string& tab_id,
      base::OnceClosure on_complete,
      base::TimeDelta min_wait_time = base::Milliseconds(250),
      base::TimeDelta request_tracking_timeout = base::Seconds(1),
      base::TimeDelta post_tracking_settle_time = base::Milliseconds(750),
      bool page_was_loaded_before_action = false,
      bool all_requests = false,
      base::TimeDelta animation_wait_time = base::TimeDelta());
```

- [ ] **Step 2: Forward from DoWaitUntil**

In `abp_action_context.cc`, update the call at lines 620-628 to pass the new field:

```cpp
  controller_->WaitForActionComplete(
      tab_id_,
      base::BindOnce(&AbpActionContext::OnWaitUntilComplete,
                     weak_factory_.GetWeakPtr()),
      options_.min_wait_time,
      options_.request_tracking_timeout,
      options_.post_tracking_settle_time,
      page_was_loaded_before_action_,
      options_.all_requests,
      options_.animation_wait_time);
```

- [ ] **Step 3: Update WaitForActionComplete implementation**

In `abp_controller.cc`, add the parameter to the function signature at line 5105:

```cpp
void AbpController::WaitForActionComplete(
    const std::string& tab_id,
    base::OnceClosure on_complete,
    base::TimeDelta min_wait_time,
    base::TimeDelta request_tracking_timeout,
    base::TimeDelta post_tracking_settle_time,
    bool page_was_loaded_before_action,
    bool all_requests,
    base::TimeDelta animation_wait_time) {
```

After line 5130 (`waiter->all_requests = all_requests;`), add the animation timer startup:

```cpp
  // Start animation timer immediately (not gated on load events).
  // Runs in parallel with all other wait conditions.
  if (!animation_wait_time.is_zero()) {
    waiter->animation_timer_started = true;
    VLOG(1) << "ABP PROFILE [wait] animation_timer STARTED"
            << " duration=" << animation_wait_time.InMilliseconds() << "ms"
            << " tab=" << tab_id;
    content::GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AbpController::OnAnimationWaitTimeElapsed,
                       weak_factory_.GetWeakPtr(), tab_id),
        animation_wait_time);
  }
```

Update the VLOG at line 5150 to include animation info. After the `settle=` line, add:

```cpp
            << " animation=" << animation_wait_time.InMilliseconds() << "ms";
```

- [ ] **Step 4: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_action_context.cc chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): wire animation_wait_time through WaitForActionComplete"
```

---

## Task 4: Add OnAnimationWaitTimeElapsed handler and VLOG updates

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h` (declare new method)
- Modify: `chrome/browser/abp/abp_controller.cc` (implement handler, update timeout VLOG)

- [ ] **Step 1: Declare OnAnimationWaitTimeElapsed**

In `abp_controller.h`, add the declaration near `OnMinWaitTimeElapsed` (find it with grep — it's a private method):

```cpp
  void OnAnimationWaitTimeElapsed(const std::string& tab_id);
```

- [ ] **Step 2: Implement OnAnimationWaitTimeElapsed**

In `abp_controller.cc`, add the implementation right after `OnMinWaitTimeElapsed` (follows the same pattern). Place it after the `OnMinWaitTimeElapsed` function body:

```cpp
void AbpController::OnAnimationWaitTimeElapsed(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();
  waiter->animation_time_elapsed = true;
  VLOG(1) << "ABP PROFILE [wait] animation_timer ELAPSED"
          << " elapsed=" << (base::TimeTicks::Now() - waiter->action_start_time).InMilliseconds() << "ms"
          << " tab=" << tab_id;

  CheckActionCompleteConditions(tab_id);
}
```

- [ ] **Step 3: Update OnWaitTimeout VLOG**

In `abp_controller.cc` at line 5572, after `<< " post_settled="`, add animation state to the VLOG:

```cpp
               << " anim_elapsed=" << it->second.action_waiter->animation_time_elapsed
```

- [ ] **Step 4: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): add OnAnimationWaitTimeElapsed handler"
```

---

## Task 5: Read animation param in WaitForNetwork

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc:3086-3100`

- [ ] **Step 1: Read animation flag from params**

In `WaitForNetwork`, after line 3093 (`options.all_requests = true;`), add:

```cpp
  if (params.FindBool("animation").value_or(false)) {
    options.animation_wait_time = base::Seconds(5);
  }
```

- [ ] **Step 2: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): read animation param in WaitForNetwork"
```

---

## Task 6: Add animation parameter to MCP browser_wait tool

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:387-411` (tool schema + passthrough)

- [ ] **Step 1: Add animation parameter to tool schema**

In `abp_mcp_handler.cc`, find the `browser_wait` ToolBuilder chain at line 389 (`ToolBuilder("browser_wait")`). After the `.OptionalString("network_tag", ...)` call (line 407-409) and before `.Build()` (line 411), add:

```cpp
          .OptionalBoolean("animation",
              "Guarantees 5s of page execution for animations to play "
              "forward. Set true on first wait after navigation.")
```

- [ ] **Step 2: Pass animation flag in CallBrowserWait**

In `abp_mcp_handler.cc`, in `CallBrowserWait` at line 1356, after the `body_dict.Set("screenshot", ...)` line (line 1364) and before the `network_tag` block (line 1366), add:

```cpp
  if (args.FindBool("animation").value_or(false)) {
    body_dict.Set("animation", true);
  }
```

- [ ] **Step 3: Update browser_wait tool description**

In the same `ToolBuilder("browser_wait")` chain, update the `.Description(...)` string (lines 390-399) to mention animation. Append to the existing description before the closing `")`:

```cpp
              " Set animation=true on first wait after navigation to "
              "guarantee 5s of page execution for CSS/JS animations.")
```

- [ ] **Step 4: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.cc
git commit -m "feat(abp): add animation parameter to MCP browser_wait tool"
```

---

## Task 7: Manual smoke test

- [ ] **Step 1: Build**

```bash
autoninja -C out/Default chrome
```

- [ ] **Step 2: Launch ABP**

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run
```

- [ ] **Step 3: Test without animation (baseline)**

```bash
# Navigate to animation-heavy site
curl -X POST http://localhost:8222/api/v1/tabs/$(curl -s http://localhost:8222/api/v1/tabs | python3 -c "import sys,json; print(json.load(sys.stdin)[0]['id'])")/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"https://www.deveillance.com/"}'

# Wait WITHOUT animation — should complete in ~5s (network settle)
curl -X POST http://localhost:8222/api/v1/tabs/$(curl -s http://localhost:8222/api/v1/tabs | python3 -c "import sys,json; print(json.load(sys.stdin)[0]['id'])")/wait_for_network \
  -H "Content-Type: application/json" \
  -d '{}'
```

- [ ] **Step 4: Test with animation**

```bash
# Navigate again
curl -X POST http://localhost:8222/api/v1/tabs/$(curl -s http://localhost:8222/api/v1/tabs | python3 -c "import sys,json; print(json.load(sys.stdin)[0]['id'])")/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"https://www.deveillance.com/"}'

# Wait WITH animation — should take at least 5s
curl -X POST http://localhost:8222/api/v1/tabs/$(curl -s http://localhost:8222/api/v1/tabs | python3 -c "import sys,json; print(json.load(sys.stdin)[0]['id'])")/wait_for_network \
  -H "Content-Type: application/json" \
  -d '{"animation": true}'
```

Verify: the response with `animation: true` takes at least 5s and the screenshot shows the page with animations completed.

- [ ] **Step 5: Test via MCP**

```bash
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test","version":"1.0"},"capabilities":{}}}'

curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
```

Verify: `browser_wait` tool listing includes the `animation` parameter.

- [ ] **Step 6: Commit version bump or any final cleanup**
