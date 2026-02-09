# Design: ABP Action Lifecycle Timing Tests

## Problem

ABP has no automated tests. The action lifecycle has multiple async boundaries
(CDP commands, Mojo IPC, compositor, OS screenshot) where timing bugs hide.
Recent fixes (deterministic `Debugger.pause`, `ForceRedrawWithCallback`,
resume-to-screenshot delay) were validated manually. We need browser tests that
catch regressions in lifecycle phase ordering, screenshot capture, and
pause/resume determinism.

## Approach

**InProcessBrowserTest with lifecycle observer hooks.**

Tests drive `AbpController::HandleRequest()` directly (no HTTP), with a
callback-based observer that fires on each action lifecycle transition. This
gives real browser + compositor + CDP execution with full observability into
intermediate states.

Why not the alternatives:
- **Full HTTP API tests**: No visibility into intermediate phases. Flaky timing.
- **Unit tests with mocks**: Can't reproduce compositor timing, Mojo pipe
  ordering, or debugger pause races — the exact bugs we're testing.

## Production Code Changes

### 1. Lifecycle Observer Callback (~20 lines)

**`chrome/browser/abp/abp_controller.h`** — public section:

```cpp
enum class LifecycleStep {
  kResumeStarted,
  kResumeCompleted,
  kBeforeScreenshotStarted,
  kBeforeScreenshotCompleted,
  kActionExecuted,
  kWaitUntilCompleted,
  kScrollPositionReceived,
  kAfterScreenshotStarted,
  kAfterScreenshotCompleted,
  kPauseStarted,
  kPauseConfirmed,       // Debugger.paused event received
  kPauseTimedOut,        // 2s safety timeout fired
  kResponseSent,
};

using LifecycleObserverCallback =
    base::RepeatingCallback<void(const std::string& tab_id,
                                 const std::string& action_type,
                                 LifecycleStep step)>;

void SetLifecycleObserverForTesting(LifecycleObserverCallback cb);
```

**`chrome/browser/abp/abp_controller.cc`**:

```cpp
void AbpController::SetLifecycleObserverForTesting(LifecycleObserverCallback cb) {
  lifecycle_observer_for_testing_ = std::move(cb);
}
```

**`chrome/browser/abp/abp_action_context.cc`** — fire at each transition:

```cpp
if (controller_->lifecycle_observer_for_testing_) {
  controller_->lifecycle_observer_for_testing_.Run(
      tab_id_, action_type_, AbpController::LifecycleStep::kResumeCompleted);
}
```

No `#ifdef` needed. Null callback in production = zero overhead.

### 2. Controller Instance Accessor

**`chrome/browser/abp/abp_controller.h`**:

```cpp
static AbpController* GetInstanceForTesting();
```

One-liner returning the existing singleton pointer set during
`AbpHttpServer::Start()`.

### 3. ForceRedraw Queued Counter

**`content/browser/renderer_host/render_widget_host_impl.h`** — public section:

```cpp
static int force_redraw_queued_count_for_testing() {
  return force_redraw_queued_count_for_testing_;
}
static void ResetForceRedrawCountersForTesting() {
  force_redraw_queued_count_for_testing_ = 0;
}
```

**`content/browser/renderer_host/render_widget_host_impl.cc`** — in
`ForceRedrawWithCallback`, when `blink_widget_` is null:

```cpp
++force_redraw_queued_count_for_testing_;
```

Static counter. Zero overhead (single integer increment on an already-cold path).

## Test Infrastructure

### Test Base Class

**File: `chrome/browser/abp/abp_action_lifecycle_browsertest.cc`**

```cpp
class AbpActionLifecycleTest : public InProcessBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    command_line->AppendSwitch("enable-abp");
    command_line->AppendSwitch("no-first-run");
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->ServeFilesFromSourceDirectory(
        "chrome/browser/abp/test_pages");
    ASSERT_TRUE(embedded_test_server()->Start());

    controller_ = AbpController::GetInstanceForTesting();
    ASSERT_TRUE(controller_);

    controller_->SetLifecycleObserverForTesting(base::BindRepeating(
        &AbpActionLifecycleTest::OnLifecycleStep, base::Unretained(this)));
  }

  void TearDownOnMainThread() override {
    controller_->SetLifecycleObserverForTesting({});
    InProcessBrowserTest::TearDownOnMainThread();
  }

  // --- Lifecycle event log ---

  struct LifecycleEvent {
    std::string tab_id;
    std::string action_type;
    AbpController::LifecycleStep step;
  };

  void OnLifecycleStep(const std::string& tab_id,
                       const std::string& action_type,
                       AbpController::LifecycleStep step) {
    base::AutoLock lock(events_lock_);
    events_.push_back({tab_id, action_type, step});
    if (step_waiter_) step_waiter_->Run();
  }

  void WaitForStep(AbpController::LifecycleStep target) {
    ASSERT_TRUE(base::test::RunUntil([&] {
      base::AutoLock lock(events_lock_);
      return base::ranges::any_of(events_, [&](const auto& e) {
        return e.step == target;
      });
    }));
  }

  // Subsequence match — expected steps must appear in order, other steps
  // may interleave. Not brittle to new steps added later.
  void ExpectStepOrder(std::vector<AbpController::LifecycleStep> expected) {
    base::AutoLock lock(events_lock_);
    std::vector<AbpController::LifecycleStep> actual;
    for (const auto& e : events_) actual.push_back(e.step);

    size_t ei = 0;
    for (size_t ai = 0; ai < actual.size() && ei < expected.size(); ++ai) {
      if (actual[ai] == expected[ei]) ++ei;
    }
    EXPECT_EQ(ei, expected.size())
        << "Steps missing or out of order. Expected " << expected.size()
        << " in sequence, matched " << ei;
  }

  void ClearEvents() {
    base::AutoLock lock(events_lock_);
    events_.clear();
  }

  // --- Request helpers ---

  // Calls HandleRequest directly, blocks until response via TestFuture.
  base::Value::Dict SendAction(const std::string& tab_id,
                               const std::string& action,
                               base::Value::Dict body) {
    base::test::TestFuture<int, base::Value> future;
    std::string path = "/api/v1/tabs/" + tab_id + "/" + action;
    std::string body_str;
    base::JSONWriter::Write(body, &body_str);

    controller_->HandleRequest("POST", path, body_str,
        base::BindOnce([](base::test::TestFuture<int, base::Value>* f,
                          int status, base::Value response) {
          f->SetValue(status, std::move(response));
        }, &future));

    auto [status, response] = future.Take();
    base::Value::Dict result;
    result.Set("status", status);
    if (response.is_dict()) result.Merge(std::move(response).TakeDict());
    return result;
  }

  std::string CreateTabAndNavigate(const std::string& path) {
    base::Value::Dict body;
    body.Set("url", embedded_test_server()->GetURL(path).spec());
    // POST /api/v1/tabs
    auto result = SendAction("", "tabs", std::move(body));
    std::string* tab_id = result.FindString("id");
    EXPECT_TRUE(tab_id);
    content::WebContents* wc = controller_->FindWebContents(*tab_id);
    EXPECT_TRUE(content::WaitForLoadStop(wc));
    return *tab_id;
  }

  raw_ptr<AbpController> controller_ = nullptr;
  base::Lock events_lock_;
  std::vector<LifecycleEvent> events_ GUARDED_BY(events_lock_);
  base::OnceClosure* step_waiter_ = nullptr;
};
```

### Test Pages

Three HTML files in `chrome/browser/abp/test_pages/`:

**`lifecycle_basic.html`** — clickable button with visible JS feedback:

```html
<!DOCTYPE html>
<html><body>
  <button id="target" style="width:100px;height:40px;margin:50px;">
    Click me
  </button>
  <div id="counter">0</div>
  <script>
    document.getElementById('target').addEventListener('click', () => {
      const c = document.getElementById('counter');
      c.textContent = parseInt(c.textContent) + 1;
    });
  </script>
</body></html>
```

**`lifecycle_active_js.html`** — continuously running JS (50ms interval):

```html
<!DOCTYPE html>
<body>
  <div id="tick">0</div>
  <script>
    setInterval(() => {
      document.getElementById('tick').textContent =
        parseInt(document.getElementById('tick').textContent) + 1;
    }, 50);
  </script>
</body>
```

**`lifecycle_stall.html`** — static page for timeout test:

```html
<!DOCTYPE html>
<body><div>Stall test</div></body>
```

## Test Cases

### Test 1: `PhaseOrderingIsCorrect`

Golden-path test. Send a click action, assert all lifecycle steps fire in the
correct order.

```cpp
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, PhaseOrderingIsCorrect) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_basic.html");
  ClearEvents();

  base::Value::Dict body;
  body.Set("x", 100);
  body.Set("y", 70);
  auto result = SendAction(tab_id, "click", std::move(body));
  EXPECT_EQ(*result.FindInt("status"), 200);

  ExpectStepOrder({
      LifecycleStep::kResumeStarted,
      LifecycleStep::kResumeCompleted,
      LifecycleStep::kBeforeScreenshotStarted,
      LifecycleStep::kBeforeScreenshotCompleted,
      LifecycleStep::kActionExecuted,
      LifecycleStep::kWaitUntilCompleted,
      LifecycleStep::kScrollPositionReceived,
      LifecycleStep::kAfterScreenshotStarted,
      LifecycleStep::kAfterScreenshotCompleted,
      LifecycleStep::kPauseStarted,
      LifecycleStep::kPauseConfirmed,
      LifecycleStep::kResponseSent,
  });
}
```

### Test 2: `PauseIsDeterministic_EventNotTimeout`

Validates `Runtime.evaluate("void 0")` triggers `Debugger.paused` CDP event
instead of falling through to the 2s safety timeout.

```cpp
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest,
                       PauseIsDeterministic_EventNotTimeout) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_active_js.html");
  ClearEvents();

  base::Value::Dict body;
  body.Set("x", 50);
  body.Set("y", 50);
  SendAction(tab_id, "click", std::move(body));

  {
    base::AutoLock lock(events_lock_);
    bool confirmed = base::ranges::any_of(events_, [](const auto& e) {
      return e.step == LifecycleStep::kPauseConfirmed;
    });
    bool timed_out = base::ranges::any_of(events_, [](const auto& e) {
      return e.step == LifecycleStep::kPauseTimedOut;
    });
    EXPECT_TRUE(confirmed) << "Debugger.paused event never arrived";
    EXPECT_FALSE(timed_out) << "Pause fell through to 2s safety timeout";
  }
}
```

### Test 3: `ScreenshotsAreNonEmptyAfterResume`

Validates the 100ms resume delay + ForceRedraw + GrabViewSnapshot pipeline
produces actual image data.

```cpp
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest,
                       ScreenshotsAreNonEmptyAfterResume) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_basic.html");
  ClearEvents();

  base::Value::Dict body;
  body.Set("x", 100);
  body.Set("y", 70);
  base::Value::Dict screenshot;
  screenshot.Set("format", "webp");
  body.Set("screenshot", std::move(screenshot));
  auto result = SendAction(tab_id, "click", std::move(body));

  EXPECT_EQ(*result.FindInt("status"), 200);

  const std::string* after_b64 =
      result.FindStringByDottedPath("screenshot.after.data");
  ASSERT_TRUE(after_b64 && !after_b64->empty());

  // Decode and verify non-trivial size (blank signature is 8654 bytes)
  std::string decoded;
  ASSERT_TRUE(base::Base64Decode(*after_b64, &decoded));
  EXPECT_GT(decoded.size(), 9000u)
      << "Screenshot appears to be the blank-window signature";
}
```

### Test 4: `ForceRedrawSurvivesRendererSwap`

Cross-origin navigation triggers renderer swap. Verifies
`ForceRedrawWithCallback` queues when `blink_widget_` is null and fires
after `BindWidgetInterfaces`.

```cpp
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest,
                       ForceRedrawSurvivesRendererSwap) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_basic.html");

  content::RenderWidgetHostImpl::ResetForceRedrawCountersForTesting();

  // Cross-origin navigation — triggers renderer swap
  base::Value::Dict nav_body;
  nav_body.Set("url", "about:blank");
  SendAction(tab_id, "navigate", std::move(nav_body));

  // Immediately send click — ForceRedraw should hit null blink_widget_
  ClearEvents();
  base::Value::Dict click_body;
  click_body.Set("x", 50);
  click_body.Set("y", 50);
  auto result = SendAction(tab_id, "click", std::move(click_body));

  EXPECT_GE(
      content::RenderWidgetHostImpl::force_redraw_queued_count_for_testing(),
      1) << "ForceRedraw never hit null blink_widget_ — renderer swap "
         "may not have been triggered";

  EXPECT_EQ(*result.FindInt("status"), 200);

  const std::string* after_b64 =
      result.FindStringByDottedPath("screenshot.after.data");
  EXPECT_TRUE(after_b64 && !after_b64->empty());
}
```

### Test 5: `SkipResumeDoesNotPause`

Navigate action uses `skip_resume=true`. Verifies resume is skipped but
pause still happens.

```cpp
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, SkipResumeDoesNotPause) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_basic.html");
  ClearEvents();

  base::Value::Dict body;
  body.Set("url", embedded_test_server()->GetURL("/lifecycle_basic.html").spec());
  SendAction(tab_id, "navigate", std::move(body));

  {
    base::AutoLock lock(events_lock_);
    bool resumed = base::ranges::any_of(events_, [](const auto& e) {
      return e.step == LifecycleStep::kResumeStarted;
    });
    bool paused = base::ranges::any_of(events_, [](const auto& e) {
      return e.step == LifecycleStep::kPauseConfirmed ||
             e.step == LifecycleStep::kPauseTimedOut;
    });
    EXPECT_FALSE(resumed) << "Resume should be skipped for navigate";
    EXPECT_TRUE(paused) << "Pause should still happen after navigate";
  }
}
```

### Test 6: `QueuedActionsExecuteInOrder`

Three concurrent clicks. Verifies deterministic action queue serializes them
and each fully completes before the next starts.

```cpp
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, QueuedActionsExecuteInOrder) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_basic.html");
  ClearEvents();

  // Fire 3 clicks concurrently
  base::test::TestFuture<base::Value::Dict> f1, f2, f3;
  auto fire = [&](base::test::TestFuture<base::Value::Dict>* f) {
    base::Value::Dict body;
    body.Set("x", 100);
    body.Set("y", 70);
    std::string path = "/api/v1/tabs/" + tab_id + "/click";
    std::string body_str;
    base::JSONWriter::Write(body, &body_str);
    controller_->HandleRequest("POST", path, body_str,
        base::BindOnce([](base::test::TestFuture<base::Value::Dict>* f,
                          int status, base::Value response) {
          base::Value::Dict r;
          r.Set("status", status);
          if (response.is_dict()) r.Merge(std::move(response).TakeDict());
          f->SetValue(std::move(r));
        }, f));
  };
  fire(&f1); fire(&f2); fire(&f3);

  auto r1 = f1.Take(); auto r2 = f2.Take(); auto r3 = f3.Take();
  EXPECT_EQ(*r1.FindInt("status"), 200);
  EXPECT_EQ(*r2.FindInt("status"), 200);
  EXPECT_EQ(*r3.FindInt("status"), 200);

  // Verify ordering: each action's ResponseSent must precede the next
  // action's ResumeStarted in the event log
  base::AutoLock lock(events_lock_);
  int response_count = 0;
  int resume_after_first_response = 0;
  for (const auto& e : events_) {
    if (e.step == LifecycleStep::kResponseSent) response_count++;
    if (e.step == LifecycleStep::kResumeStarted && response_count > 0)
      resume_after_first_response++;
  }
  EXPECT_EQ(response_count, 3);
  EXPECT_EQ(resume_after_first_response, 2)
      << "Actions 2 and 3 should each start after the previous completed";
}
```

### Test 7: `ActionTimeoutFiresOnStall`

Execute action with infinite loop. Verifies the 30s watchdog fires and
doesn't permanently stall the action queue.

```cpp
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, ActionTimeoutFiresOnStall) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_stall.html");
  ClearEvents();

  base::Value::Dict body;
  body.Set("script", "while(true){}");
  auto result = SendAction(tab_id, "execute", std::move(body));

  // Should get error response, not hang forever
  EXPECT_NE(*result.FindInt("status"), 200);

  // Verify queue is unblocked — a subsequent action should work
  // (after the stalled tab is recovered)
  ClearEvents();
  base::Value::Dict click_body;
  click_body.Set("x", 50);
  click_body.Set("y", 50);
  auto result2 = SendAction(tab_id, "click", std::move(click_body));
  // May fail (tab is in bad state) but must not hang
}
```

## BUILD.gn

Add to `chrome/browser/abp/BUILD.gn`:

```gn
if (is_browser_test) {
  source_set("browser_tests") {
    testonly = true
    sources = [
      "abp_action_lifecycle_browsertest.cc",
    ]
    deps = [
      ":abp",
      "//base/test:test_support",
      "//chrome/test:test_support",
      "//content/public/test:test_support",
      "//content/test:test_support",
      "//net:test_support",
    ]
  }
}
```

## File Summary

| File | Change |
|------|--------|
| `chrome/browser/abp/abp_controller.h` | Add `LifecycleStep` enum, `SetLifecycleObserverForTesting()`, `GetInstanceForTesting()`, `lifecycle_observer_for_testing_` field |
| `chrome/browser/abp/abp_controller.cc` | Implement `SetLifecycleObserverForTesting`, `GetInstanceForTesting` |
| `chrome/browser/abp/abp_action_context.cc` | Add ~13 observer fire points at each lifecycle transition |
| `content/browser/renderer_host/render_widget_host_impl.h` | Add `force_redraw_queued_count_for_testing_` static counter + accessors |
| `content/browser/renderer_host/render_widget_host_impl.cc` | Increment counter in `ForceRedrawWithCallback` null path |
| `chrome/browser/abp/abp_action_lifecycle_browsertest.cc` | **New** — test base class + 7 test cases |
| `chrome/browser/abp/test_pages/lifecycle_basic.html` | **New** — button with click counter |
| `chrome/browser/abp/test_pages/lifecycle_active_js.html` | **New** — setInterval counter |
| `chrome/browser/abp/test_pages/lifecycle_stall.html` | **New** — static page for timeout test |
| `chrome/browser/abp/BUILD.gn` | Add `browser_tests` source set |

## Implementation Order

1. Production hooks: `LifecycleStep` enum + observer callback + `GetInstanceForTesting` + ForceRedraw counter
2. Observer fire points in `abp_action_context.cc`
3. Test pages (3 HTML files)
4. Test base class + helpers
5. Test cases (7 tests)
6. BUILD.gn integration
7. Build and run: `autoninja -C out/Default browser_tests && ./out/Default/browser_tests --gtest_filter="AbpActionLifecycle*"`
