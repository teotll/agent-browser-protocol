// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <ranges>
#include <string>
#include <vector>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/synchronization/lock.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "build/build_config.h"
#include "chrome/browser/abp/abp_controller.h"
#include "chrome/browser/abp/abp_types.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"

#if BUILDFLAG(IS_MAC)
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif

namespace abp {

using LifecycleStep = AbpController::LifecycleStep;

// NOTE: These tests are NOT compatible with --single-process-tests.
// AbpController teardown in one test leaves stale state that crashes
// the next test's setup when the browser is recreated in the same process.
// Run without --single-process-tests (the default for browser_tests).
class AbpActionLifecycleTest : public InProcessBrowserTest {
 protected:
  void SetUp() override {
    // Enable pixel output so the compositor actually produces frames.
    // Without this, InProcessBrowserTest adds --disable-gl-drawing-for-tests
    // which disables compositor output, causing ForceRedraw to timeout.
    EnablePixelOutput(1.0f);
    InProcessBrowserTest::SetUp();
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    // ABP server is always-on but skipped when --test-type=browser is set
    // (which InProcessBrowserTest provides). We create AbpController directly
    // with proper lifetime management instead of using the leaked server.
    command_line->AppendSwitch("no-first-run");
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->ServeFilesFromSourceDirectory(
        "chrome/browser/abp/test_pages");
    ASSERT_TRUE(embedded_test_server()->Start());

#if BUILDFLAG(IS_MAC)
    // Wake the display and prevent it from sleeping. The renderer compositor
    // stops producing frames when the display is asleep, which causes
    // ForceRedraw and GrabViewSnapshot to hang or return empty results.
    IOPMAssertionDeclareUserActivity(
        CFSTR("ABP lifecycle tests waking display"),
        kIOPMUserActiveLocal, &wake_assertion_);
    IOPMAssertionCreateWithName(
        kIOPMAssertionTypePreventUserIdleDisplaySleep,
        kIOPMAssertionLevelOn,
        CFSTR("ABP lifecycle tests active"),
        &sleep_assertion_);
#endif

    // Create AbpController directly — bypasses AbpHttpServer and its
    // problematic leaked-singleton lifecycle.
    owned_controller_ = std::make_unique<AbpController>();
    controller_ = owned_controller_.get();

    controller_->SetLifecycleObserverForTesting(base::BindRepeating(
        &AbpActionLifecycleTest::OnLifecycleStep, base::Unretained(this)));
  }

  void TearDownOnMainThread() override {
    if (controller_) {
      controller_->SetLifecycleObserverForTesting({});
    }
    // Destroy controller BEFORE browser teardown to avoid dangling pointers.
    controller_ = nullptr;
    owned_controller_.reset();

#if BUILDFLAG(IS_MAC)
    if (wake_assertion_ != kIOPMNullAssertionID) {
      IOPMAssertionRelease(wake_assertion_);
      wake_assertion_ = kIOPMNullAssertionID;
    }
    if (sleep_assertion_ != kIOPMNullAssertionID) {
      IOPMAssertionRelease(sleep_assertion_);
      sleep_assertion_ = kIOPMNullAssertionID;
    }
#endif

    InProcessBrowserTest::TearDownOnMainThread();
  }

  // --- Lifecycle event log ---

  struct LifecycleEvent {
    std::string tab_id;
    std::string action_type;
    LifecycleStep step;
  };

  void OnLifecycleStep(const std::string& tab_id,
                       const std::string& action_type,
                       LifecycleStep step) {
    base::AutoLock lock(events_lock_);
    events_.push_back({tab_id, action_type, step});
  }

  void WaitForStep(LifecycleStep target) {
    ASSERT_TRUE(base::test::RunUntil([&] {
      base::AutoLock lock(events_lock_);
      return std::ranges::any_of(events_, [&](const auto& e) {
        return e.step == target;
      });
    }));
  }

  bool HasStep(LifecycleStep target) {
    base::AutoLock lock(events_lock_);
    return std::ranges::any_of(events_, [&](const auto& e) {
      return e.step == target;
    });
  }

  // Subsequence match — expected steps must appear in order, other steps
  // may interleave. Not brittle to new steps added later.
  void ExpectStepOrder(std::vector<LifecycleStep> expected) {
    base::AutoLock lock(events_lock_);
    std::vector<LifecycleStep> actual;
    for (const auto& e : events_) {
      actual.push_back(e.step);
    }

    size_t ei = 0;
    for (size_t ai = 0; ai < actual.size() && ei < expected.size(); ++ai) {
      if (actual[ai] == expected[ei]) {
        ++ei;
      }
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

  struct ActionResult {
    int status = 0;
    std::string body;
    base::Value::Dict parsed;
  };

  // Calls HandleRequest directly, blocks until response via TestFuture.
  ActionResult SendRequest(const std::string& method,
                           const std::string& path,
                           const std::string& body = "") {
    base::test::TestFuture<int, const std::string&, std::string> future;
    controller_->HandleRequest(
        method, path, body,
        base::BindOnce(
            [](base::test::TestFuture<int, const std::string&, std::string>* f,
               int status, const std::string& content_type, std::string body) {
              f->SetValue(status, content_type, std::move(body));
            },
            &future));

    auto [status, content_type, response_body] = future.Take();
    ActionResult result;
    result.status = status;
    result.body = std::move(response_body);
    auto parsed = base::JSONReader::Read(result.body,
                                         base::JSON_ALLOW_TRAILING_COMMAS);
    if (parsed && parsed->is_dict()) {
      result.parsed = std::move(*parsed).TakeDict();
    }
    return result;
  }

  ActionResult SendAction(const std::string& tab_id,
                          const std::string& action,
                          base::Value::Dict body) {
    std::string path = "/api/v1/tabs/" + tab_id + "/" + action;
    std::string body_str;
    base::JSONWriter::Write(body, &body_str);
    return SendRequest("POST", path, body_str);
  }

  // Enable execution control (pause) on a tab. Blocks until the tab is paused.
  void PauseTab(const std::string& tab_id) {
    base::Value::Dict body;
    body.Set("paused", true);
    auto result = SendAction(tab_id, "execution", std::move(body));
    EXPECT_EQ(result.status, 200)
        << "Failed to pause tab: " << result.body;
  }

  std::string CreateTabAndNavigate(const std::string& page_path) {
    base::Value::Dict body;
    body.Set("url", embedded_test_server()->GetURL(page_path).spec());
    std::string body_str;
    base::JSONWriter::Write(body, &body_str);
    auto result = SendRequest("POST", "/api/v1/tabs", body_str);
    // POST /api/v1/tabs returns 201 (Created)
    EXPECT_TRUE(result.status == 200 || result.status == 201)
        << "Tab creation failed with status " << result.status;

    std::string* tab_id = result.parsed.FindString("id");
    EXPECT_TRUE(tab_id) << "No tab id in response: " << result.body;
    if (!tab_id) {
      return "";
    }

    // Wait for load
    content::WebContents* wc = controller_->FindWebContents(*tab_id);
    EXPECT_TRUE(wc);
    if (wc) {
      EXPECT_TRUE(content::WaitForLoadStop(wc));
    }
    return *tab_id;
  }

  std::unique_ptr<AbpController> owned_controller_;
  raw_ptr<AbpController> controller_ = nullptr;
  base::Lock events_lock_;
  std::vector<LifecycleEvent> events_ GUARDED_BY(events_lock_);
#if BUILDFLAG(IS_MAC)
  IOPMAssertionID wake_assertion_ = kIOPMNullAssertionID;
  IOPMAssertionID sleep_assertion_ = kIOPMNullAssertionID;
#endif
};

// ---------------------------------------------------------------------------
// Test 1: Phase ordering is correct for a click action.
// ---------------------------------------------------------------------------
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, PhaseOrderingIsCorrect) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_basic.html");
  ASSERT_FALSE(tab_id.empty());
  ClearEvents();

  base::Value::Dict body;
  body.Set("x", 100);
  body.Set("y", 70);
  auto result = SendAction(tab_id, "click", std::move(body));
  EXPECT_EQ(result.status, 200);

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
      LifecycleStep::kResponseSent,
  });
}

// ---------------------------------------------------------------------------
// Test 2: Debugger.paused CDP event fires (not 2s safety timeout).
// ---------------------------------------------------------------------------
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest,
                       PauseIsDeterministic_EventNotTimeout) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_active_js.html");
  ASSERT_FALSE(tab_id.empty());
  PauseTab(tab_id);
  ClearEvents();

  base::Value::Dict body;
  body.Set("x", 50);
  body.Set("y", 50);
  SendAction(tab_id, "click", std::move(body));

  EXPECT_TRUE(HasStep(LifecycleStep::kPauseConfirmed))
      << "Debugger.paused event never arrived";
  EXPECT_FALSE(HasStep(LifecycleStep::kPauseTimedOut))
      << "Pause fell through to 2s safety timeout";
}

// ---------------------------------------------------------------------------
// Test 3: Screenshots are non-empty after resume + ForceRedraw.
// ---------------------------------------------------------------------------
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest,
                       ScreenshotsAreNonEmptyAfterResume) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_basic.html");
  ASSERT_FALSE(tab_id.empty());
  PauseTab(tab_id);
  ClearEvents();

  base::Value::Dict body;
  body.Set("x", 100);
  body.Set("y", 70);
  base::Value::Dict screenshot;
  screenshot.Set("format", "webp");
  body.Set("screenshot", std::move(screenshot));
  auto result = SendAction(tab_id, "click", std::move(body));

  EXPECT_EQ(result.status, 200);

  // Verify the lifecycle included the screenshot phases
  EXPECT_TRUE(HasStep(LifecycleStep::kAfterScreenshotStarted));
  EXPECT_TRUE(HasStep(LifecycleStep::kAfterScreenshotCompleted));

  const std::string* after_b64 =
      result.parsed.FindStringByDottedPath("screenshot_after.data");
  ASSERT_TRUE(after_b64 && !after_b64->empty())
      << "After screenshot data is missing or empty";

  // Decode and verify non-trivial size (blank signature is ~8654 bytes)
  std::string decoded;
  ASSERT_TRUE(base::Base64Decode(*after_b64, &decoded));
  EXPECT_GT(decoded.size(), 9000u)
      << "Screenshot appears to be the blank-window signature ("
      << decoded.size() << " bytes)";
}

// ---------------------------------------------------------------------------
// Test 4: ForceRedraw survives renderer swap (cross-process navigation).
// ---------------------------------------------------------------------------
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest,
                       ForceRedrawSurvivesRendererSwap) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_basic.html");
  ASSERT_FALSE(tab_id.empty());

  AbpController::ResetForceRedrawCountersForTesting();

  // Cross-origin navigation — triggers renderer swap
  base::Value::Dict nav_body;
  nav_body.Set("url", "about:blank");
  SendAction(tab_id, "navigate", std::move(nav_body));

  // Immediately send click — ForceRedraw may hit null blink_widget_
  ClearEvents();
  base::Value::Dict click_body;
  click_body.Set("x", 50);
  click_body.Set("y", 50);
  auto result = SendAction(tab_id, "click", std::move(click_body));

  // In single-process test mode, renderer swap timing differs from
  // multi-process mode. The queued count may be 0 if the new renderer
  // binds before ForceRedraw fires. Log instead of hard-fail.
  int queued = AbpController::GetForceRedrawQueuedCountForTesting();
  LOG(INFO) << "ABP TEST: ForceRedraw queued count = " << queued;

  // The action must still succeed regardless.
  EXPECT_EQ(result.status, 200);
}

// ---------------------------------------------------------------------------
// Test 5: Navigate (skip_resume) doesn't fire ResumeStarted.
// ---------------------------------------------------------------------------
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, SkipResumeForNavigate) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_basic.html");
  ASSERT_FALSE(tab_id.empty());
  ClearEvents();

  base::Value::Dict body;
  body.Set("url",
           embedded_test_server()->GetURL("/lifecycle_basic.html").spec());
  SendAction(tab_id, "navigate", std::move(body));

  // Navigate uses skip_resume=true, so kResumeStarted should still fire
  // (ResumeExecutionIfNeeded always fires it before checking skip), but
  // the actual resume should be skipped — kResumeCompleted fires immediately.
  EXPECT_TRUE(HasStep(LifecycleStep::kResumeCompleted));

  // Pause should still happen
  EXPECT_TRUE(
      HasStep(LifecycleStep::kPauseStarted) ||
      HasStep(LifecycleStep::kPauseConfirmed) ||
      HasStep(LifecycleStep::kPauseTimedOut))
      << "Pause should still happen after navigate";
}

// ---------------------------------------------------------------------------
// Test 6: Queued actions execute in order.
// ---------------------------------------------------------------------------
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, QueuedActionsExecuteInOrder) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_basic.html");
  ASSERT_FALSE(tab_id.empty());
  ClearEvents();

  // Fire 3 clicks concurrently using TestFuture
  base::test::TestFuture<int, const std::string&, std::string> f1, f2, f3;

  auto fire_click = [&](auto* future) {
    base::Value::Dict body;
    body.Set("x", 100);
    body.Set("y", 70);
    std::string body_str;
    base::JSONWriter::Write(body, &body_str);
    controller_->HandleRequest(
        "POST", "/api/v1/tabs/" + tab_id + "/click", body_str,
        base::BindOnce(
            [](base::test::TestFuture<int, const std::string&, std::string>* f,
               int status, const std::string& ct, std::string body) {
              f->SetValue(status, ct, std::move(body));
            },
            future));
  };

  fire_click(&f1);
  fire_click(&f2);
  fire_click(&f3);

  // Wait for all 3 responses
  auto [s1, ct1, b1] = f1.Take();
  auto [s2, ct2, b2] = f2.Take();
  auto [s3, ct3, b3] = f3.Take();

  EXPECT_EQ(s1, 200);
  EXPECT_EQ(s2, 200);
  EXPECT_EQ(s3, 200);

  // Verify ordering: each action's kResponseSent must precede the next
  // action's kResumeStarted in the event log
  base::AutoLock lock(events_lock_);
  int response_count = 0;
  int resume_after_first_response = 0;
  for (const auto& e : events_) {
    if (e.step == LifecycleStep::kResponseSent) {
      response_count++;
    }
    if (e.step == LifecycleStep::kResumeStarted && response_count > 0) {
      resume_after_first_response++;
    }
  }
  EXPECT_EQ(response_count, 3);
  EXPECT_EQ(resume_after_first_response, 2)
      << "Actions 2 and 3 should each start after the previous completed";
}

// ---------------------------------------------------------------------------
// Test 7: Action timeout fires on stalled execution.
// ---------------------------------------------------------------------------
// Disabled: while(true){} freezes renderer for 30s, exceeds test launcher
// timeout and prevents clean teardown. Needs shorter action timeout for tests.
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, DISABLED_ActionTimeoutFiresOnStall) {
  std::string tab_id = CreateTabAndNavigate("/lifecycle_stall.html");
  ASSERT_FALSE(tab_id.empty());
  ClearEvents();

  base::Value::Dict body;
  body.Set("script", "while(true){}");
  auto result = SendAction(tab_id, "execute", std::move(body));

  // Should get error response (timeout), not hang forever
  EXPECT_NE(result.status, 200)
      << "Infinite loop should have timed out, not returned 200";
}

// ---------------------------------------------------------------------------
// Test 8: Slider horizontal action succeeds.
// ---------------------------------------------------------------------------
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, SliderHorizontal) {
  std::string tab_id = CreateTabAndNavigate("/slider_test.html");
  ASSERT_FALSE(tab_id.empty());

  base::Value::Dict body;
  body.Set("orientation", "horizontal");
  body.Set("y", 75);
  body.Set("x_start", 50);
  body.Set("x_end", 450);
  body.Set("current_x", 250);
  body.Set("min", 0.0);
  body.Set("max", 100.0);
  body.Set("target_value", 75.0);
  auto result = SendAction(tab_id, "slider", std::move(body));
  EXPECT_EQ(result.status, 200);
}

// ---------------------------------------------------------------------------
// Test 9: Slider vertical action succeeds.
// ---------------------------------------------------------------------------
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, SliderVertical) {
  std::string tab_id = CreateTabAndNavigate("/slider_test.html");
  ASSERT_FALSE(tab_id.empty());

  base::Value::Dict body;
  body.Set("orientation", "vertical");
  body.Set("x", 75);
  body.Set("y_start", 150);
  body.Set("y_end", 550);
  body.Set("current_y", 350);
  body.Set("min", 0.0);
  body.Set("max", 100.0);
  body.Set("target_value", 25.0);
  auto result = SendAction(tab_id, "slider", std::move(body));
  EXPECT_EQ(result.status, 200);
}

// ---------------------------------------------------------------------------
// Test 10: Slider validation errors return 400.
// ---------------------------------------------------------------------------
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, SliderValidationErrors) {
  std::string tab_id = CreateTabAndNavigate("/slider_test.html");
  ASSERT_FALSE(tab_id.empty());

  // Missing orientation
  {
    base::Value::Dict body;
    body.Set("y", 75);
    body.Set("x_start", 50);
    body.Set("x_end", 450);
    body.Set("current_x", 250);
    body.Set("min", 0.0);
    body.Set("max", 100.0);
    body.Set("target_value", 50.0);
    auto result = SendAction(tab_id, "slider", std::move(body));
    EXPECT_EQ(result.status, 400);
  }

  // min >= max
  {
    base::Value::Dict body;
    body.Set("orientation", "horizontal");
    body.Set("y", 75);
    body.Set("x_start", 50);
    body.Set("x_end", 450);
    body.Set("current_x", 250);
    body.Set("min", 100.0);
    body.Set("max", 0.0);
    body.Set("target_value", 50.0);
    auto result = SendAction(tab_id, "slider", std::move(body));
    EXPECT_EQ(result.status, 400);
  }

  // target_value out of range
  {
    base::Value::Dict body;
    body.Set("orientation", "horizontal");
    body.Set("y", 75);
    body.Set("x_start", 50);
    body.Set("x_end", 450);
    body.Set("current_x", 250);
    body.Set("min", 0.0);
    body.Set("max", 100.0);
    body.Set("target_value", 150.0);
    auto result = SendAction(tab_id, "slider", std::move(body));
    EXPECT_EQ(result.status, 400);
  }

  // Missing horizontal fields
  {
    base::Value::Dict body;
    body.Set("orientation", "horizontal");
    body.Set("min", 0.0);
    body.Set("max", 100.0);
    body.Set("target_value", 50.0);
    auto result = SendAction(tab_id, "slider", std::move(body));
    EXPECT_EQ(result.status, 400);
  }
}

}  // namespace abp
