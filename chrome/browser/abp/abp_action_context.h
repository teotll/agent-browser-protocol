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
//     -> ResumeExecutionIfNeeded()
//     -> OnExecutionResumed()
//     -> CaptureBeforeScreenshot()         // markup inject → CDP capture → cleanup → history save → return base64
//     -> OnBeforeScreenshotCaptured()      // store path + base64
//     -> ExecuteAction() (calls user-provided ActionCallback)
//     -> [action calls OnActionDispatched()]
//     -> WaitUntil() (handles wait_until from params)
//     -> OnWaitUntilComplete()
//     -> EnsureVirtualCursorVisible()
//     -> CaptureAfterScreenshot()          // markup inject → CDP capture → cleanup → history save → return base64
//     -> OnAfterScreenshotCaptured()       // store path + base64
//     -> PauseExecutionIfNeeded()
//     -> OnExecutionPaused()
//     -> FinalizeResponse()                // direct — no more EnsureCompositorActive dance
//     -> RecordHistory() + SendResponse()
//
class AbpActionContext : public base::RefCounted<AbpActionContext> {
 public:
  // Options for running an action
  struct Options {
    // If true, skip Debugger.resume at start.
    // Use for actions like Navigate that need JS to run during the action.
    bool skip_resume = false;

    // If true, skip Debugger.pause at end.
    // Leave false for most actions so the page freezes after completion.
    bool skip_pause = false;

    // If true, center the virtual cursor in the viewport after the action
    // completes. Use for navigation actions where cursor should reset to center.
    bool center_cursor_after = false;

    // Minimum wait time before considering action complete.
    // Use longer values (e.g., 10s) for navigation to allow page load.
    // Default 500ms is suitable for quick actions like click/type.
    base::TimeDelta min_wait_time = base::Milliseconds(500);
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
  void DoWaitUntil();
  void OnWaitUntilComplete();
  void OnCursorCentered();
  void StopEventCaptureAndGetScrollPosition();
  void OnScrollPositionReceived(base::Value::Dict scroll_info);
  void FlushCompositorFrame();
  void OnCompositorFrameFlushed();
  void PauseExecutionIfNeeded();
  void OnExecutionPaused();
  void EnsureVirtualCursorVisible();
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
  std::string action_type_;
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
  std::string screenshot_markup_ = "none";
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

  // Error state
  bool has_error_ = false;
  std::string error_code_;
  std::string error_message_;

  // Self-reference to prevent destruction during async operations.
  // Set in Start(), cleared in SendResponse()/SendErrorResponse().
  scoped_refptr<AbpActionContext> prevent_destroy_;

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
