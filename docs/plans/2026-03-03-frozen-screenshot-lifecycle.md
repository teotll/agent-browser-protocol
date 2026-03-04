# Frozen Screenshot Lifecycle Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reorder the ABP action lifecycle so the after-screenshot is captured from a frozen screen with markup overlays visible, and markup persists until the next action clears it.

**Architecture:** The action context's post-wait flow changes from "screenshot → send → pause (background)" to "inject markup → ForceRedraw → pause → capture frozen buffer → send". Markup cleanup moves from immediately-after-capture to start-of-next-action (fire-and-forget after resume). A new `ForceRedrawForTab` controller method provides ForceRedraw without the full screenshot pipeline. A new `last_markup_tags` field in `TabState` tracks which markup to clean up.

**Tech Stack:** C++ (Chromium), CDP (Runtime.evaluate), compositor (ForceRedraw + GrabViewSnapshot)

---

### Task 1: Add `last_markup_tags` to TabState and new controller methods

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:826-884` (TabState struct)
- Modify: `chrome/browser/abp/abp_controller.h:338-363` (public methods section)
- Modify: `chrome/browser/abp/abp_controller.cc` (new method implementations)

**Step 1: Add `last_markup_tags` field to TabState**

In `chrome/browser/abp/abp_controller.h`, inside `struct TabState` (after line 877, before `IsIdle()`):

```cpp
    // Markup tags left on screen from the previous action's after-screenshot.
    // Read and cleared at the start of the next action's cleanup phase.
    std::vector<std::string> last_markup_tags;
```

**Step 2: Add `ForceRedrawForTab` declaration**

In `chrome/browser/abp/abp_controller.h`, in the public section near `ResumeExecution`/`PauseExecution` (after line 341):

```cpp
  // Force the compositor to paint a frame for the given tab.
  // Calls back when the frame is committed (or after 1500ms timeout).
  // Virtual time must be running (not paused) for this to work.
  void ForceRedrawForTab(const std::string& tab_id, base::OnceClosure then);
```

**Step 3: Add `CleanupMarkupForTab` declaration**

In the same public section:

```cpp
  // Fire-and-forget cleanup of markup overlays left from a previous action.
  // Reads and clears last_markup_tags from TabState.
  void CleanupMarkupForTab(const std::string& tab_id);
```

**Step 4: Implement `ForceRedrawForTab`**

In `chrome/browser/abp/abp_controller.cc`, near `ForceRedrawThenResumeVirtualTime` (~line 3843):

```cpp
void AbpController::ForceRedrawForTab(const std::string& tab_id,
                                       base::OnceClosure then) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  if (!view) {
    std::move(then).Run();
    return;
  }

  auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
      view->GetRenderWidgetHost());

  // Shared state to prevent double-firing (timeout vs callback)
  auto done = std::make_shared<bool>(false);

  // Safety-net timeout
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<bool> d, base::OnceClosure cb) {
            if (*d) return;
            *d = true;
            LOG(WARNING) << "ABP: ForceRedrawForTab timed out after 1500ms";
            std::move(cb).Run();
          },
          done, std::move(then)),  // <-- problem: then is moved here
      base::Milliseconds(1500));

  // ... but then is also needed below. Fix: use weak closure pattern.
```

Actually, the timeout and callback both need `then`. Use a shared callback wrapper:

```cpp
void AbpController::ForceRedrawForTab(const std::string& tab_id,
                                       base::OnceClosure then) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  if (!view) {
    std::move(then).Run();
    return;
  }

  auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
      view->GetRenderWidgetHost());

  // Use OnceCallbackList to allow both timeout and callback to race.
  auto shared_cb = std::make_shared<base::OnceClosure>(std::move(then));
  auto done = std::make_shared<bool>(false);

  auto fire = [](std::shared_ptr<bool> d,
                 std::shared_ptr<base::OnceClosure> cb) {
    if (*d) return;
    *d = true;
    if (*cb) std::move(*cb).Run();
  };

  // Safety timeout
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<bool> d,
             std::shared_ptr<base::OnceClosure> cb) {
            if (*d) return;
            *d = true;
            LOG(WARNING) << "ABP: ForceRedrawForTab timed out after 1500ms";
            if (*cb) std::move(*cb).Run();
          },
          done, shared_cb),
      base::Milliseconds(1500));

  VLOG(1) << "ABP: ForceRedrawForTab SEND tab=" << tab_id;
  rwhi->ForceRedrawWithCallback(base::BindOnce(
      [](std::shared_ptr<bool> d,
         std::shared_ptr<base::OnceClosure> cb,
         std::string tid) {
        if (*d) return;
        *d = true;
        VLOG(1) << "ABP: ForceRedrawForTab DONE tab=" << tid;
        if (*cb) std::move(*cb).Run();
      },
      done, shared_cb, tab_id));
}
```

**Step 5: Implement `CleanupMarkupForTab`**

In `chrome/browser/abp/abp_controller.cc`, near `BuildMarkupCleanupScript`:

```cpp
void AbpController::CleanupMarkupForTab(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || it->second.last_markup_tags.empty()) {
    return;
  }

  std::vector<std::string> tags = std::move(it->second.last_markup_tags);
  it->second.last_markup_tags.clear();

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) return;

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) return;

  base::Value::Dict cleanup;
  cleanup.Set("expression", BuildMarkupCleanupScript(tags));
  cleanup.Set("returnByValue", true);
  cleanup.Set("disableBreaks", true);
  VLOG(1) << "ABP: CleanupMarkupForTab fire-and-forget tab=" << tab_id;
  client->SendCommand("Runtime.evaluate", cleanup,
                      base::BindOnce([](bool, const std::string&) {}));
}
```

**Step 6: Build and verify compilation**

Run: `autoninja -C out/Default chrome 2>&1 | tail -5`
Expected: Build succeeds (new methods aren't called yet, but must compile)

**Step 7: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): add ForceRedrawForTab, CleanupMarkupForTab, last_markup_tags"
```

---

### Task 2: Rewire the action context lifecycle

This is the core change. The `AbpActionContext` flow after `OnWaitUntilComplete` changes from:

```
EnsureVirtualCursorVisible → CaptureAfterScreenshot (inject+ForceRedraw+capture+cleanup) → FinalizeResponse → PauseExecution (background)
```

To:

```
EnsureVirtualCursorVisible → InjectMarkupIfNeeded → ForceRedrawFinalFrame → PauseExecution → CaptureFromFrozenBuffer → FinalizeResponse
```

And at the start, after resume, we add markup cleanup.

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.h:167-196` (flow method declarations)
- Modify: `chrome/browser/abp/abp_action_context.cc:565-766` (flow implementation)

**Step 1: Add new method declarations to `abp_action_context.h`**

In `chrome/browser/abp/abp_action_context.h`, in the private section (after `EnsureVirtualCursorVisible` around line 188, replacing `CaptureAfterScreenshot`):

Replace the existing declarations from `FlushCompositorFrame` through `CaptureAfterScreenshot` (lines 184-191) with:

```cpp
  void InjectMarkupIfNeeded();
  void OnMarkupInjected();
  void ForceRedrawFinalFrame();
  void OnFinalFrameDrawn();
  void CaptureFromFrozenBuffer();
  void CaptureAfterScreenshot();  // kept — but now calls CaptureFromFrozenBuffer flow
```

Also add a flag to track ForceRedraw completion (in the state section, around line 270):

```cpp
  // True once ForceRedrawFinalFrame has completed (prevents double-fire from timeout)
  bool final_frame_drawn_ = false;
```

**Step 2: Modify `OnExecutionResumed` to clean up previous markup**

In `abp_action_context.cc`, in `OnExecutionResumed()` (line 375-400), add markup cleanup right before proceeding to action execution. After `profile_resume_end_` is set and before the `action_before_resume` branch:

```cpp
void AbpActionContext::OnExecutionResumed() {
  if (!IsCurrentAction()) {
    return;
  }
  profile_resume_end_ = base::TimeTicks::Now();
  VLOG(1) << "ABP ActionContext: OnExecutionResumed() action=" << action_type_;
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kResumeCompleted);
  }
  if (has_error_) {
    return;
  }

  // Clean up markup overlays from previous action (fire-and-forget).
  // Must happen after resume since Runtime.evaluate needs the page running.
  controller_->CleanupMarkupForTab(tab_id_);

  if (options_.action_before_resume) {
    // Navigation path: action already executed, resume just completed.
    profile_resume_end_ = base::TimeTicks::Now();
    ProceedToWait();
  } else {
    // Normal path: resume completed, now execute the action.
    profile_action_start_ = base::TimeTicks::Now();
    ExecuteAction();
  }
}
```

**Step 3: Modify `EnsureVirtualCursorVisible` to flow into markup injection instead of capture**

Change `EnsureVirtualCursorVisible()` (line 634-650) — replace `CaptureAfterScreenshot()` at the end with `InjectMarkupIfNeeded()`:

```cpp
void AbpActionContext::EnsureVirtualCursorVisible() {
  CheckForTabSwitch();

  auto& tab_state = controller_->GetOrCreateTabState(tab_id_);
  if (tab_state.cursor.active && web_contents_) {
    controller_->SetVirtualCursorEnabledViaMojo(web_contents_, true);
    controller_->SetVirtualCursorViaMojo(web_contents_, tab_state.cursor.x,
                                          tab_state.cursor.y, true);
  }
  InjectMarkupIfNeeded();
}
```

**Step 4: Implement `InjectMarkupIfNeeded`**

New method — inject markup CSS while page is still running:

```cpp
void AbpActionContext::InjectMarkupIfNeeded() {
  if (screenshot_markup_tags_.empty()) {
    // No markup requested — skip straight to ForceRedraw
    ForceRedrawFinalFrame();
    return;
  }

  VLOG(1) << "ABP ActionContext: InjectMarkupIfNeeded() action=" << action_type_
          << " tags=" << screenshot_markup_tags_.size();

  std::string script =
      AbpController::BuildMarkupInjectionScript(screenshot_markup_tags_);

  base::Value::Dict js_params;
  js_params.Set("expression", script);
  js_params.Set("returnByValue", true);
  js_params.Set("disableBreaks", true);

  client_->SendCommand(
      "Runtime.evaluate", js_params,
      base::BindOnce(
          [](base::WeakPtr<AbpActionContext> ctx,
             bool success, const std::string& result) {
            if (!ctx) return;
            ctx->OnMarkupInjected();
          },
          weak_factory_.GetWeakPtr()));
}
```

**Step 5: Implement `OnMarkupInjected`**

```cpp
void AbpActionContext::OnMarkupInjected() {
  if (!IsCurrentAction()) return;
  VLOG(1) << "ABP ActionContext: OnMarkupInjected() action=" << action_type_;

  // Store markup tags in tab state so the next action can clean them up.
  auto& tab_state = controller_->GetOrCreateTabState(tab_id_);
  tab_state.last_markup_tags = screenshot_markup_tags_;

  ForceRedrawFinalFrame();
}
```

**Step 6: Implement `ForceRedrawFinalFrame`**

```cpp
void AbpActionContext::ForceRedrawFinalFrame() {
  VLOG(1) << "ABP ActionContext: ForceRedrawFinalFrame() action=" << action_type_;
  controller_->ForceRedrawForTab(
      tab_id_,
      base::BindOnce(&AbpActionContext::OnFinalFrameDrawn,
                     weak_factory_.GetWeakPtr()));
}
```

**Step 7: Implement `OnFinalFrameDrawn`**

```cpp
void AbpActionContext::OnFinalFrameDrawn() {
  if (!IsCurrentAction()) return;
  VLOG(1) << "ABP ActionContext: OnFinalFrameDrawn() action=" << action_type_;

  // Frame with markup is now committed to the GPU surface.
  // Pause execution — the frozen screen will have markup visible.
  profile_pause_start_ = base::TimeTicks::Now();
  PauseExecutionIfNeeded();
}
```

**Step 8: Modify `OnExecutionPaused` to capture screenshot from frozen buffer**

Currently `OnExecutionPaused` (line 753) just releases the slot. Change it to capture the after-screenshot:

```cpp
void AbpActionContext::OnExecutionPaused() {
  if (!IsCurrentAction()) {
    return;
  }
  profile_pause_end_ = base::TimeTicks::Now();
  VLOG(1) << "ABP ActionContext: OnExecutionPaused() action=" << action_type_
          << " pause_ms="
          << (profile_pause_end_ - profile_pause_start_).InMilliseconds();

  // Page is now frozen. Capture the after-screenshot from the frozen buffer.
  // The markup CSS is in the DOM and visible on the frozen screen.
  CaptureAfterScreenshot();
}
```

**Step 9: Rewrite `CaptureAfterScreenshot` to capture from frozen buffer**

Replace the existing `CaptureAfterScreenshot` (line 652-679):

```cpp
void AbpActionContext::CaptureAfterScreenshot() {
  profile_after_ss_start_ = base::TimeTicks::Now();
  VLOG(1) << "ABP ActionContext: CaptureAfterScreenshot() action=" << action_type_;
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kAfterScreenshotStarted);
  }

  AbpController::ScreenshotOptions opts;
  opts.format = screenshot_format_;
  opts.quality = screenshot_quality_;
  // No markup_tags in opts — markup is already in the DOM.
  // CaptureScreenshotFromBuffer will NOT inject or cleanup markup.

  controller_->CaptureScreenshotFromBuffer(
      tab_id_, start_time_ms_, false, opts,
      base::BindOnce(
          [](base::WeakPtr<AbpActionContext> ctx,
             AbpController::ActionScreenshotResult r) {
            if (!ctx) return;
            ctx->scroll_info_ = std::move(r.scroll_info);
            ctx->OnAfterScreenshotCaptured(
                std::move(r.history_path), std::move(r.base64),
                r.width, r.height);
          },
          weak_factory_.GetWeakPtr()));
}
```

**Step 10: Modify `OnAfterScreenshotCaptured` — remove pause call, finalize directly**

Replace the existing `OnAfterScreenshotCaptured` (line 681-714):

```cpp
void AbpActionContext::OnAfterScreenshotCaptured(std::string history_path,
                                                   std::string base64,
                                                   int width,
                                                   int height) {
  if (!IsCurrentAction()) {
    return;
  }
  VLOG(1) << "ABP ActionContext: OnAfterScreenshotCaptured() action="
            << action_type_ << " base64_len=" << base64.size()
            << " width=" << width << " height=" << height
            << " tab=" << tab_id_;
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kAfterScreenshotCompleted);
  }
  screenshot_after_path_ = std::move(history_path);
  screenshot_after_base64_ = std::move(base64);
  screenshot_after_width_ = width;
  screenshot_after_height_ = height;
  profile_after_ss_end_ = base::TimeTicks::Now();

  // Capture virtual time at end
  virtual_time_at_end_ = controller_->GetVirtualTimeMs(tab_id_);

  // Execution is already paused. Send response and release slot.
  FinalizeResponse();

  // Release deterministic slot and allow destruction.
  ReleaseDeterministicSlot();
  prevent_destroy_ = nullptr;
}
```

**Step 11: Remove `FlushCompositorFrame` and `OnCompositorFrameFlushed`**

These methods (lines 602-608) are no longer used. Delete them.

**Step 12: Build and verify compilation**

Run: `autoninja -C out/Default chrome 2>&1 | tail -5`
Expected: Build succeeds

**Step 13: Commit**

```bash
git add chrome/browser/abp/abp_action_context.h chrome/browser/abp/abp_action_context.cc
git commit -m "feat(abp): reorder action lifecycle — freeze before screenshot, persist markup"
```

---

### Task 3: Update profiling and cleanup

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.h` (remove stale declarations)
- Modify: `chrome/browser/abp/abp_action_context.cc` (update profiling, remove dead code)

**Step 1: Remove `FlushCompositorFrame` and `OnCompositorFrameFlushed` declarations**

In `abp_action_context.h`, remove lines 184-185:
```cpp
  void FlushCompositorFrame();
  void OnCompositorFrameFlushed();
```

**Step 2: Update the header comment describing the flow**

Replace the flow comment at the top of the class (lines 35-58) with:

```cpp
// AbpActionContext wraps any state-modifying ABP action with consistent:
// - Execution resume at start (Debugger.resume + virtual time advance)
// - Previous markup cleanup (fire-and-forget after resume)
// - Action execution
// - wait_until handling
// - Markup injection + ForceRedraw (final frame with overlays)
// - Execution pause (freeze JS + virtual time)
// - Screenshot capture from frozen buffer (page is frozen WITH markup)
// - Response formatting (timing + virtual_time info)
//
// Flow:
//   Run()
//     -> CaptureBeforeScreenshot()         // GrabViewSnapshot from frozen buffer (may have previous markup)
//     -> OnBeforeScreenshotCaptured()      // store path + base64
//     -> ResumeExecutionIfNeeded()
//     -> OnExecutionResumed()              // CleanupMarkupForTab() fire-and-forget
//     -> ExecuteAction() (calls user-provided ActionCallback)
//     -> [action calls OnActionDispatched()]
//     -> WaitUntil() (handles wait_until from params)
//     -> OnWaitUntilComplete()
//     -> EnsureVirtualCursorVisible()
//     -> InjectMarkupIfNeeded()            // markup CSS into DOM
//     -> OnMarkupInjected()                // store tags in TabState
//     -> ForceRedrawFinalFrame()           // compositor paints markup frame
//     -> OnFinalFrameDrawn()
//     -> PauseExecutionIfNeeded()          // freeze JS + virtual time
//     -> OnExecutionPaused()               // page is frozen WITH markup
//     -> CaptureAfterScreenshot()          // GrabViewSnapshot from frozen buffer
//     -> OnAfterScreenshotCaptured()       // store path + base64
//     -> FinalizeResponse()                // RecordHistory() + SendResponse()
//     -> ReleaseDeterministicSlot()        // allow next action
```

**Step 3: Verify profiling `LogProfilingSummary` still makes sense**

The profiling already tracks `pause_start`/`pause_end` and `after_ss_start`/`after_ss_end`. The order of these timestamps changes (pause now precedes after_ss) but the individual measurements are still valid. Add a `force_redraw` phase:

In `abp_action_context.h`, add profiling fields (near line 290):
```cpp
  base::TimeTicks profile_force_redraw_start_;
  base::TimeTicks profile_force_redraw_end_;
```

In `ForceRedrawFinalFrame()`, set `profile_force_redraw_start_`.
In `OnFinalFrameDrawn()`, set `profile_force_redraw_end_`.

Update `LogProfilingSummary` to include force_redraw:
```cpp
            << " | force_redraw=" << ms(profile_force_redraw_start_, profile_force_redraw_end_) << "ms"
```

And update the profiling in the response envelope (the `profiling` dict in `SendResponse`):
```cpp
    profiling.Set("force_redraw_ms", ms(profile_force_redraw_start_, profile_force_redraw_end_));
```

**Step 4: Build and verify**

Run: `autoninja -C out/Default chrome 2>&1 | tail -5`
Expected: Build succeeds

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_action_context.h chrome/browser/abp/abp_action_context.cc
git commit -m "refactor(abp): update flow comments and profiling for frozen screenshot lifecycle"
```

---

### Task 4: Manual testing

**Step 1: Launch ABP and verify the new lifecycle**

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run
```

**Step 2: Enable execution control and navigate**

```bash
# Create a tab
curl -s -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}' | python3 -m json.tool

# Get tab ID
TAB=$(curl -s http://localhost:8222/api/v1/tabs | python3 -c "import sys,json; print(json.load(sys.stdin)[0]['id'])")

# Enable execution control
curl -s -X POST http://localhost:8222/api/v1/tabs/$TAB/execution \
  -H "Content-Type: application/json" \
  -d '{"paused": true}'
```

**Step 3: Perform an action with markup and verify frozen screenshot**

```bash
# Click with markup requested
curl -s -X POST http://localhost:8222/api/v1/tabs/$TAB/click \
  -H "Content-Type: application/json" \
  -d '{"x":400,"y":300,"screenshot":{"markup":["clickable","grid"]}}' | python3 -c "
import sys, json, base64
r = json.load(sys.stdin)
print('After screenshot size:', len(r.get('screenshot_after',{}).get('data','')))
print('Profiling:', json.dumps(r.get('profiling',{}), indent=2))
# Verify pause happened BEFORE screenshot
p = r.get('profiling',{})
print('Pause completed before screenshot:', p.get('after_screenshot_ms', 0) > 0)
"
```

**Step 4: Verify markup persists on frozen screen**

Take a raw screenshot (no action) — should still show markup from previous action:

```bash
curl -s http://localhost:8222/api/v1/tabs/$TAB/screenshot -o /tmp/frozen_with_markup.webp
open /tmp/frozen_with_markup.webp
```

Expected: The screenshot shows the markup overlays from the previous click action.

**Step 5: Perform another action — verify cleanup**

```bash
# Another click — this should clean up previous markup first
curl -s -X POST http://localhost:8222/api/v1/tabs/$TAB/click \
  -H "Content-Type: application/json" \
  -d '{"x":400,"y":300,"screenshot":{"markup":["clickable"]}}' | python3 -c "
import sys, json
r = json.load(sys.stdin)
# Before screenshot should have the PREVIOUS markup (grid+clickable)
print('Before screenshot present:', bool(r.get('screenshot_before',{}).get('data')))
# After screenshot should have only the NEW markup (clickable, no grid)
print('After screenshot present:', bool(r.get('screenshot_after',{}).get('data')))
"
```

**Step 6: Verify action without markup**

```bash
# Action with no markup tags — should clean up previous markup, no new markup
curl -s -X POST http://localhost:8222/api/v1/tabs/$TAB/click \
  -H "Content-Type: application/json" \
  -d '{"x":400,"y":300}' | python3 -m json.tool
```

Take a raw screenshot — should show clean page (no markup):
```bash
curl -s http://localhost:8222/api/v1/tabs/$TAB/screenshot -o /tmp/clean_after.webp
open /tmp/clean_after.webp
```
