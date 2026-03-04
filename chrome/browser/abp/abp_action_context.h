// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_ACTION_CONTEXT_H_
#define CHROME_BROWSER_ABP_ABP_ACTION_CONTEXT_H_

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/abp/abp_event_collector.h"
#include "chrome/browser/abp/abp_types.h"

namespace content {
class WebContents;
}

namespace abp {

class AbpController;
class AbpCdpClient;

// Forward declaration for the action callback
class AbpActionContext;
using ActionCallback = base::OnceCallback<void(AbpActionContext* ctx)>;

// AbpActionContext wraps any state-modifying ABP action with consistent:
// - Execution resume at start (Debugger.resume + virtual time advance)
// - Action execution
// - wait_until handling
// - Execution pause after completion (virtual time pause + Debugger.pause)
// - Screenshot capture (page is frozen)
// - Response formatting (timing + virtual_time info)
//
// Flow:
//   Run()
//     -> CaptureBeforeScreenshot()         // GrabViewSnapshot from frozen buffer (no ForceRedraw, no markup)
//     -> OnBeforeScreenshotCaptured()      // store path + base64
//     -> ResumeExecutionIfNeeded()
//     -> OnExecutionResumed()              // also cleans up markup from previous action
//     -> ExecuteAction() (calls user-provided ActionCallback)
//     -> [action calls OnActionDispatched()]
//     -> WaitUntil() (handles wait_until from params)
//     -> OnWaitUntilComplete()
//     -> EnsureVirtualCursorVisible()
//     -> InjectMarkupIfNeeded()            // inject markup CSS while page is still running
//     -> OnMarkupInjected()                // store tags in tab state for next action cleanup
//     -> ForceRedrawFinalFrame()           // ForceRedraw to commit markup to GPU surface
//     -> OnFinalFrameDrawn()               // frame committed
//     -> PauseExecutionIfNeeded()           // freeze page — markup visible on frozen screen
//     -> OnExecutionPaused()                // page frozen
//     -> CaptureAfterScreenshot()           // capture from frozen buffer (no ForceRedraw)
//     -> OnAfterScreenshotCaptured()        // store path + base64
//     -> FinalizeResponse()                 // RecordHistory() + SendResponse() — client gets response
//     -> ReleaseDeterministicSlot()         // release slot + allow next action
//
class AbpActionContext : public base::RefCounted<AbpActionContext> {
 public:
  // Options for running an action
  struct Options {
    // If true, skip Debugger.resume at start.
    bool skip_resume = false;

    // If true, skip Debugger.pause at end.
    // Leave false for most actions so the page freezes after completion.
    bool skip_pause = false;

    // If true, execute the action BEFORE resuming JS. The action (e.g.
    // LoadURL) is dispatched while the debugger is still paused, then
    // resume happens after. This ensures the navigation IPC is queued
    // before old page JS can run, preventing teardown race conditions.
    bool action_before_resume = false;

    // If true, center the virtual cursor in the viewport after the action
    // completes. Use for navigation actions where cursor should reset to center.
    bool center_cursor_after = false;

    // Phase 1: JS hook window. Minimum time before snapshot + completion check.
    // 150ms default lets page JS fire event handlers and start requests.
    base::TimeDelta min_wait_time = base::Milliseconds(150);

    // Phase 2: How long to wait for snapshotted requests to complete.
    // 1s default for clicks/type/scroll. 60s for file uploads.
    base::TimeDelta request_tracking_timeout = base::Milliseconds(1000);

    // Phase 3: Settle time after tracked requests complete.
    // Lets the page process network responses and update DOM.
    base::TimeDelta post_tracking_settle_time = base::Milliseconds(350);

    // If true, WaitForActionComplete seeds tracked_requests from the tab's
    // persistent network tracker (all in-flight requests, not just new ones).
    // Used by browser_wait to catch requests started before the wait began.
    bool all_requests = false;
  };

  // Factory method - creates context and starts the action flow
  // The action callback will be invoked after execution is resumed.
  // The action should call ctx->OnActionDispatched() when the core
  // action logic is complete (e.g., after mouse release for click).
  static void Run(AbpController* controller,
                  const std::string& tab_id,
                  const std::string& action_type,
                  const base::Value::Dict& params,
                  ActionCallback action,
                  ResponseCallback response);

  // Factory method with options
  static void RunWithOptions(AbpController* controller,
                             const std::string& tab_id,
                             const std::string& action_type,
                             const base::Value::Dict& params,
                             const Options& options,
                             ActionCallback action,
                             ResponseCallback response);

  // Called by action implementation when action dispatch is complete.
  // This triggers the wait_until -> pause -> screenshot -> response flow.
  void OnActionDispatched();

  // Called by action implementation to set result data that will be
  // included in the response.
  void SetResult(base::Value::Dict result);

  // Called by action implementation on error.
  // This immediately sends an error response and stops the flow.
  void OnActionError(const std::string& error_code,
                     const std::string& error_message);

  // Send a rejection response without entering the action flow.
  // Used when the action is rejected before starting (e.g., queue full).
  void RejectBeforeStart(int status,
                         const std::string& error_code,
                         const std::string& error_message);

  // Access to CDP client for action implementation
  AbpCdpClient* client() const { return client_; }

  // Access to WebContents for action implementation
  content::WebContents* web_contents() const { return web_contents_; }

  // Access to params for action implementation
  const base::Value::Dict& params() const { return params_; }

  // Access to tab_id for action implementation
  const std::string& tab_id() const { return tab_id_; }

  // Access to action_id for action implementation
  const std::string& action_id() const { return action_id_; }

  // Access to controller for action implementation
  // Use sparingly - prefer using context methods when possible
  AbpController* controller() const { return controller_.get(); }

  // Constructor - prefer using Run() or RunWithOptions() factory methods
  AbpActionContext(AbpController* controller,
                   const std::string& tab_id,
                   const std::string& action_type,
                   const base::Value::Dict& params,
                   const Options& options,
                   ActionCallback action,
                   ResponseCallback response);

 private:
  friend class base::RefCounted<AbpActionContext>;
  ~AbpActionContext();

  static std::string GenerateActionId();

  // Internal flow methods
  void Start();
  void StartOnDeterministicSlot(uint64_t action_epoch);
  void StartEventCapture();
  bool IsCurrentAction() const;
  void ReleaseDeterministicSlot();
  void ResumeExecutionIfNeeded();
  void OnExecutionResumed();
  void CaptureBeforeScreenshot();
  void OnBeforeScreenshotCaptured(std::string history_path,
                                   std::string base64,
                                   int width,
                                   int height);
  void ExecuteAction();
  void ProceedToWait();
  void DoWaitUntil();
  void OnWaitUntilComplete();
  void PauseExecutionIfNeeded();
  void OnExecutionPaused();
  void EnsureVirtualCursorVisible();
  void CheckForTabSwitch();
  void SwitchToNewTab(const std::string& new_tab_id);
  void InjectMarkupIfNeeded();
  void OnMarkupInjected();
  void ForceRedrawFinalFrame();
  void OnFinalFrameDrawn();
  void CaptureAfterScreenshot();
  void OnAfterScreenshotCaptured(std::string history_path,
                                  std::string base64,
                                  int width,
                                  int height);
  void RecordHistory(bool success,
                     const std::string& error_code,
                     const std::string& error_message);
  void FinalizeResponse();
  void BuildResponseEnvelope();
  void SendResponse();
  void SendErrorResponse(int status,
                         const std::string& error_code,
                         const std::string& error_message);

  // Unified teardown: stop timer -> record -> respond -> release slot -> clear self-ref.
  // Safe to call from any point in the action lifecycle.
  void Fail(int http_status,
            const std::string& error_code,
            const std::string& error_message);

  void OnActionTimeout();

  // State
  base::WeakPtr<AbpController> controller_;
  std::string tab_id_;
  // Original tab ID for deterministic slot release. May differ from tab_id_
  // if a click opened a new tab and the action context followed it.
  std::string original_tab_id_;
  // True if the action context switched to a different tab during the action.
  bool tab_switched_ = false;
  std::string action_type_;
  std::string action_id_;
  base::Value::Dict params_;
  Options options_;
  ActionCallback action_;
  ResponseCallback response_callback_;
  raw_ptr<AbpCdpClient> client_ = nullptr;
  raw_ptr<content::WebContents> web_contents_ = nullptr;
  base::Value::Dict result_;

  // Screenshots — before
  std::string screenshot_before_path_;
  std::string screenshot_before_base64_;
  int screenshot_before_width_ = 0;
  int screenshot_before_height_ = 0;

  // Screenshots — after
  std::string screenshot_after_path_;
  std::string screenshot_after_base64_;
  int screenshot_after_width_ = 0;
  int screenshot_after_height_ = 0;

  // Screenshot options (parsed from action params)
  std::string screenshot_format_ = "webp";
  std::vector<std::string> screenshot_markup_tags_;
  int screenshot_quality_ = 80;

  // Timing
  int64_t start_time_ms_ = 0;
  base::TimeTicks start_ticks_;
  base::TimeTicks action_end_ticks_;

  // Virtual time info (captured at start and end)
  double virtual_time_at_start_ = 0;
  double virtual_time_at_end_ = 0;

  // Response envelope data
  std::vector<AbpEvent> captured_events_;
  base::Value::Dict scroll_info_;
  int64_t wait_completed_ms_ = 0;

  // Pre-resume loading state: true if page was NOT loading before resume.
  // Used to skip waiting for load/dcl/first_paint events when the page was
  // already loaded before the action was dispatched.
  bool page_was_loaded_before_action_ = false;

  // Error state
  bool has_error_ = false;
  std::string error_code_;
  std::string error_message_;

  // Self-reference to prevent destruction during async operations.
  // Set in Start(), cleared in SendResponse()/SendErrorResponse().
  scoped_refptr<AbpActionContext> prevent_destroy_;

  // Profiling: stage timestamps for latency breakdown
  base::TimeTicks profile_queued_at_;       // RunWithOptions() — HTTP request received
  base::TimeTicks profile_slot_acquired_;   // StartOnDeterministicSlot() — slot acquired
  base::TimeTicks profile_before_ss_start_;
  base::TimeTicks profile_before_ss_end_;
  base::TimeTicks profile_resume_start_;
  base::TimeTicks profile_resume_end_;
  base::TimeTicks profile_action_start_;
  base::TimeTicks profile_action_end_;
  base::TimeTicks profile_wait_start_;
  base::TimeTicks profile_wait_end_;
  base::TimeTicks profile_scroll_start_;
  base::TimeTicks profile_scroll_end_;
  base::TimeTicks profile_after_ss_start_;
  base::TimeTicks profile_after_ss_end_;
  base::TimeTicks profile_pause_start_;
  base::TimeTicks profile_pause_end_;
  void LogProfilingSummary();

  // Deterministic action runner epoch for stale-callback filtering.
  uint64_t action_epoch_ = 0;
  bool deterministic_slot_active_ = false;

  // Action-level timeout to prevent permanent queue stalls.
  static constexpr base::TimeDelta kActionTimeout = base::Seconds(30);
  base::OneShotTimer action_timeout_timer_;

  base::WeakPtrFactory<AbpActionContext> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_ACTION_CONTEXT_H_
