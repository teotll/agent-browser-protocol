#include "chrome/browser/abp/abp_action_context.h"

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/abp/abp_controller.h"
#include "chrome/browser/abp/abp_event_collector.h"
#include "chrome/browser/abp/abp_history_controller.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"

namespace abp {

// static
void AbpActionContext::Run(AbpController* controller,
                           const std::string& tab_id,
                           const std::string& action_type,
                           const base::Value::Dict& params,
                           ActionCallback action,
                           ResponseCallback response) {
  RunWithOptions(controller, tab_id, action_type, params, Options(),
                 std::move(action), std::move(response));
}

// static
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
    ctx->RejectBeforeStart(429, "QUEUE_FULL",
                           "Too many queued actions for this tab");
  }
}

AbpActionContext::AbpActionContext(AbpController* controller,
                                   const std::string& tab_id,
                                   const std::string& action_type,
                                   const base::Value::Dict& params,
                                   const Options& options,
                                   ActionCallback action,
                                   ResponseCallback response)
    : controller_(controller->GetWeakPtr()),
      tab_id_(tab_id),
      action_type_(action_type),
      params_(params.Clone()),
      options_(options),
      action_(std::move(action)),
      response_callback_(std::move(response)) {}

AbpActionContext::~AbpActionContext() {
  if (controller_) {
    ReleaseDeterministicSlot();
  }
}

void AbpActionContext::RejectBeforeStart(int status,
                                         const std::string& error_code,
                                         const std::string& error_message) {
  if (!response_callback_ || !controller_) {
    return;
  }
  controller_->SendError(status, error_message, std::move(response_callback_));
}

void AbpActionContext::StartOnDeterministicSlot(uint64_t action_epoch) {
  action_epoch_ = action_epoch;
  deterministic_slot_active_ = true;
  Start();
}

void AbpActionContext::Start() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!controller_) {
    // Controller destroyed before action started -- can't send HTTP response
    // without the controller. Just clean up and let the HTTP connection close.
    prevent_destroy_ = nullptr;
    return;
  }

  VLOG(1) << "ABP ActionContext: Start() action=" << action_type_;

  // Hold a self-reference to prevent destruction during async operations
  prevent_destroy_ = this;

  // Start action-level timeout watchdog
  action_timeout_timer_.Start(
      FROM_HERE, kActionTimeout,
      base::BindOnce(&AbpActionContext::OnActionTimeout,
                     base::Unretained(this)));

  start_time_ms_ = base::Time::Now().InMillisecondsSinceUnixEpoch();
  start_ticks_ = base::TimeTicks::Now();

  // Parse screenshot options from action params
  const base::Value::Dict* ss_params = params_.FindDict("screenshot");
  if (ss_params) {
    const std::string* markup = ss_params->FindString("markup");
    if (markup) {
      screenshot_markup_ = *markup;
    }
    const std::string* format = ss_params->FindString("format");
    if (format) {
      screenshot_format_ = *format;
    }
    std::optional<int> quality = ss_params->FindInt("quality");
    if (quality.has_value()) {
      screenshot_quality_ = *quality;
    }
  }

  // Capture virtual time at start
  virtual_time_at_start_ = controller_->GetVirtualTimeMs(tab_id_);

  // Validate tab exists
  web_contents_ = controller_->FindWebContents(tab_id_);
  if (!web_contents_) {
    Fail(404, "TAB_NOT_FOUND", "Tab not found");
    return;
  }

  // Get CDP client
  client_ = controller_->GetOrCreateCdpClient(web_contents_);
  if (!client_) {
    Fail(500, "CDP_ERROR", "Failed to create CDP client");
    return;
  }

  // Start event capture
  StartEventCapture();

  // Start the flow: resume execution first
  ResumeExecutionIfNeeded();
}

void AbpActionContext::StartEventCapture() {
  if (controller_->event_collector()) {
    controller_->event_collector()->StartCapturing(tab_id_);
  }
}

bool AbpActionContext::IsCurrentAction() const {
  if (!deterministic_slot_active_) {
    return false;
  }
  if (!controller_) {
    return false;
  }
  return controller_->IsDeterministicActionCurrent(tab_id_, action_epoch_);
}

void AbpActionContext::ReleaseDeterministicSlot() {
  if (!deterministic_slot_active_) {
    return;
  }
  deterministic_slot_active_ = false;
  if (controller_) {
    controller_->FinishDeterministicAction(tab_id_, action_epoch_);
  }
}

void AbpActionContext::ResumeExecutionIfNeeded() {
  VLOG(1) << "ABP ActionContext: ResumeExecutionIfNeeded() action=" << action_type_;
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kResumeStarted);
  }
  // Check if resume should be skipped for this action
  if (options_.skip_resume) {
    OnExecutionResumed();
    return;
  }

  // Check if execution control is enabled globally AND for this specific tab.
  // Only resume if this tab was explicitly paused — don't auto-enable
  // execution control just because the global flag is set, as that would
  // cause tabs without execution control to get paused after every action.
  if (!controller_->IsExecutionControlEnabled()) {
    OnExecutionResumed();
    return;
  }

  auto it = controller_->tab_states_.find(tab_id_);
  if (it == controller_->tab_states_.end() ||
      !it->second.execution.IsEnabled()) {
    // Tab doesn't have execution control enabled — don't auto-enable
    OnExecutionResumed();
    return;
  }

  // Use the controller's ResumeExecution which handles state tracking
  controller_->ResumeExecution(
      tab_id_,
      base::BindOnce(&AbpActionContext::OnExecutionResumed,
                     weak_factory_.GetWeakPtr()));
}

void AbpActionContext::OnExecutionResumed() {
  if (!IsCurrentAction()) {
    return;
  }
  VLOG(1) << "ABP ActionContext: OnExecutionResumed() action=" << action_type_;
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kResumeCompleted);
  }
  if (has_error_) {
    return;
  }

  // Give the renderer main thread time to process the debugger resume
  // before requesting ForceRedraw. The Debugger.resume CDP response
  // arrives at the browser before the renderer main thread is unblocked.
  // Without this delay, ForceRedraw arrives while the main thread is
  // still in the debugger pause and BeginMainFrame can't fire.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpActionContext::CaptureBeforeScreenshot,
                     weak_factory_.GetWeakPtr()),
      base::Milliseconds(100));
}

void AbpActionContext::CaptureBeforeScreenshot() {
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kBeforeScreenshotStarted);
  }
  AbpController::ScreenshotOptions opts;
  opts.format = screenshot_format_;
  opts.quality = screenshot_quality_;
  opts.markup = screenshot_markup_;

  controller_->CaptureActionScreenshot(
      tab_id_, start_time_ms_, true, opts,
      base::BindOnce(
          [](base::WeakPtr<AbpActionContext> ctx,
             AbpController::ActionScreenshotResult r) {
            if (!ctx) return;
            ctx->OnBeforeScreenshotCaptured(
                std::move(r.history_path), std::move(r.base64),
                r.width, r.height);
          },
          weak_factory_.GetWeakPtr()));
}

void AbpActionContext::OnBeforeScreenshotCaptured(std::string history_path,
                                                    std::string base64,
                                                    int width,
                                                    int height) {
  if (!IsCurrentAction()) {
    return;
  }
  VLOG(1) << "ABP ActionContext: OnBeforeScreenshotCaptured() action=" << action_type_;
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kBeforeScreenshotCompleted);
  }
  screenshot_before_path_ = std::move(history_path);
  screenshot_before_base64_ = std::move(base64);
  screenshot_before_width_ = width;
  screenshot_before_height_ = height;

  if (has_error_) {
    return;
  }

  // Execute the action
  ExecuteAction();
}

void AbpActionContext::ExecuteAction() {
  // Invoke the user-provided action callback
  // The action should call OnActionDispatched() when done
  if (action_) {
    std::move(action_).Run(this);
  } else {
    // No action provided, go directly to wait
    OnActionDispatched();
  }
}

void AbpActionContext::OnActionDispatched() {
  if (!IsCurrentAction()) {
    return;
  }
  VLOG(1) << "ABP ActionContext: OnActionDispatched() action=" << action_type_;
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kActionExecuted);
  }
  action_end_ticks_ = base::TimeTicks::Now();

  if (has_error_) {
    return;
  }

  // Center cursor early (before wait) so it's visible during page load
  if (options_.center_cursor_after) {
    controller_->CenterCursorInTab(
        tab_id_,
        base::BindOnce([](base::WeakPtr<AbpActionContext> ctx) {
                           if (ctx) {
                             ctx->DoWaitUntil();
                           }
                       },
                       weak_factory_.GetWeakPtr()));
  } else {
    DoWaitUntil();
  }
}

void AbpActionContext::OnActionError(const std::string& error_code,
                                     const std::string& error_message) {
  if (!IsCurrentAction()) {
    return;
  }
  has_error_ = true;
  error_code_ = error_code;
  error_message_ = error_message;

  // Still need to pause and record before sending error response
  PauseExecutionIfNeeded();
}

void AbpActionContext::SetResult(base::Value::Dict result) {
  result_ = std::move(result);
}

void AbpActionContext::DoWaitUntil() {
  // Check if a custom wait_until condition is specified in params
  const base::Value::Dict* wait_until = params_.FindDict("wait_until");
  if (wait_until) {
    controller_->WaitFor(
        tab_id_, *wait_until,
        base::BindOnce(&AbpActionContext::OnWaitUntilComplete,
                       weak_factory_.GetWeakPtr()));
    return;
  }

  // Without execution control, use the standard wait mechanism
  // Pass the configured min_wait_time (longer for navigation, shorter for clicks)
  controller_->WaitForActionComplete(
      tab_id_,
      base::BindOnce(&AbpActionContext::OnWaitUntilComplete,
                     weak_factory_.GetWeakPtr()),
      options_.min_wait_time);
}

void AbpActionContext::OnWaitUntilComplete() {
  if (!IsCurrentAction()) {
    return;
  }
  VLOG(1) << "ABP ActionContext: OnWaitUntilComplete() action=" << action_type_;
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kWaitUntilCompleted);
  }
  wait_completed_ms_ = base::Time::Now().InMillisecondsSinceUnixEpoch();

  // Cursor centering already happened in OnActionDispatched (before wait).
  OnCursorCentered();
}

void AbpActionContext::OnCursorCentered() {
  // Stop event capture and get scroll position
  StopEventCaptureAndGetScrollPosition();
}

void AbpActionContext::StopEventCaptureAndGetScrollPosition() {
  // Stop event capture and save events
  if (controller_->event_collector()) {
    captured_events_ = controller_->event_collector()->StopCapturing();
  }

  // Defensive guard: execution may have been paused externally (e.g., via
  // the /execution API endpoint). The auto-pause timer no longer fires
  // during actions, but other pause sources are still possible.
  // GetScrollPosition uses Runtime.evaluate which requires JS to be
  // running, so resume first. PauseExecutionIfNeeded() later will re-pause.
  auto it = controller_->tab_states_.find(tab_id_);
  bool externally_paused =
      it != controller_->tab_states_.end() && it->second.execution.IsPaused();
  if (externally_paused && controller_->IsExecutionControlEnabled()) {
    LOG(INFO) << "ABP ActionContext: Execution was paused externally during "
              << "wait, resuming before GetScrollPosition";
    controller_->ResumeExecution(
        tab_id_,
        base::BindOnce(
            [](base::WeakPtr<AbpActionContext> ctx) {
              if (!ctx || !ctx->controller_) return;
              ctx->controller_->GetScrollPosition(
                  ctx->tab_id_,
                  base::BindOnce(
                      &AbpActionContext::OnScrollPositionReceived,
                      ctx->weak_factory_.GetWeakPtr()));
            },
            weak_factory_.GetWeakPtr()));
    return;
  }

  // Get scroll position (JS is already running)
  controller_->GetScrollPosition(
      tab_id_,
      base::BindOnce(&AbpActionContext::OnScrollPositionReceived,
                     weak_factory_.GetWeakPtr()));
}

void AbpActionContext::OnScrollPositionReceived(base::Value::Dict scroll_info) {
  if (!IsCurrentAction()) {
    return;
  }
  VLOG(1) << "ABP ActionContext: OnScrollPositionReceived() action=" << action_type_;
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kScrollPositionReceived);
  }
  scroll_info_ = std::move(scroll_info);

  // Capture screenshots BEFORE pausing execution.  The compositor only
  // produces frames while virtual time is running; pausing virtual time
  // freezes the compositor surface so any screenshot taken afterward
  // reflects the state at the moment of the pause, not the action's result.
  EnsureVirtualCursorVisible();
}

void AbpActionContext::FlushCompositorFrame() {
  OnCompositorFrameFlushed();
}

void AbpActionContext::OnCompositorFrameFlushed() {
  PauseExecutionIfNeeded();
}

void AbpActionContext::EnsureVirtualCursorVisible() {
  // Re-enable and re-position the virtual cursor from last known state
  // so it appears in every screenshot regardless of action type.
  auto& tab_state = controller_->GetOrCreateTabState(tab_id_);
  if (tab_state.cursor.active && web_contents_) {
    controller_->SetVirtualCursorEnabledViaMojo(web_contents_, true);
    controller_->SetVirtualCursorViaMojo(web_contents_, tab_state.cursor.x,
                                          tab_state.cursor.y, true);
  }
  // ForceRedraw in CaptureActionScreenshot is on the associated Mojo pipe,
  // guaranteed to be processed AFTER SetPosition. No InsertVisualStateCallback
  // needed — CopyFromSurface reads the compositor surface directly.
  CaptureAfterScreenshot();
}

void AbpActionContext::CaptureAfterScreenshot() {
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kAfterScreenshotStarted);
  }
  AbpController::ScreenshotOptions opts;
  opts.format = screenshot_format_;
  opts.quality = screenshot_quality_;
  opts.markup = screenshot_markup_;

  controller_->CaptureActionScreenshot(
      tab_id_, start_time_ms_, false, opts,
      base::BindOnce(
          [](base::WeakPtr<AbpActionContext> ctx,
             AbpController::ActionScreenshotResult r) {
            if (!ctx) return;
            ctx->OnAfterScreenshotCaptured(
                std::move(r.history_path), std::move(r.base64),
                r.width, r.height);
          },
          weak_factory_.GetWeakPtr()));
}

void AbpActionContext::OnAfterScreenshotCaptured(std::string history_path,
                                                   std::string base64,
                                                   int width,
                                                   int height) {
  if (!IsCurrentAction()) {
    return;
  }
  VLOG(1) << "ABP ActionContext: OnAfterScreenshotCaptured() action=" << action_type_;
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kAfterScreenshotCompleted);
  }
  screenshot_after_path_ = std::move(history_path);
  screenshot_after_base64_ = std::move(base64);
  screenshot_after_width_ = width;
  screenshot_after_height_ = height;

  // Capture virtual time at end
  virtual_time_at_end_ = controller_->GetVirtualTimeMs(tab_id_);

  // Pause execution — no separate screenshot capture step needed anymore
  PauseExecutionIfNeeded();
}

void AbpActionContext::PauseExecutionIfNeeded() {
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kPauseStarted);
  }
  // Check if pause should be skipped for this action
  if (options_.skip_pause) {
    OnExecutionPaused();
    return;
  }

  if (!controller_->IsExecutionControlEnabled()) {
    OnExecutionPaused();
    return;
  }

  // Only re-pause if this tab actually has execution control enabled.
  // Don't auto-pause tabs that were never explicitly paused.
  auto it = controller_->tab_states_.find(tab_id_);
  if (it == controller_->tab_states_.end() ||
      !it->second.execution.IsEnabled()) {
    OnExecutionPaused();
    return;
  }

  controller_->PauseExecution(
      tab_id_,
      base::BindOnce(&AbpActionContext::OnExecutionPaused,
                     weak_factory_.GetWeakPtr()));
}

void AbpActionContext::OnExecutionPaused() {
  if (!IsCurrentAction()) {
    return;
  }
  VLOG(1) << "ABP ActionContext: OnExecutionPaused() action=" << action_type_;

  // Both before and after screenshots are already captured.
  // Go directly to finalizing the response.
  FinalizeResponse();
}

void AbpActionContext::FinalizeResponse() {
  VLOG(1) << "ABP ActionContext: FinalizeResponse() action=" << action_type_;

  // Record to history
  RecordHistory(!has_error_, error_code_, error_message_);

  // Build full response envelope and send
  if (has_error_) {
    // For errors, send an error response but still include any partial result
    SendErrorResponse(500, error_code_, error_message_);
  } else {
    BuildResponseEnvelope();
    SendResponse();
  }
}

void AbpActionContext::RecordHistory(bool success,
                                     const std::string& error_code,
                                     const std::string& error_message) {
  if (!controller_->history_controller_) {
    return;
  }

  int64_t end_time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
  int64_t duration_ms = end_time_ms - start_time_ms_;

  base::Value result_value(result_.Clone());
  controller_->history_controller_->RecordAction(
      tab_id_, action_type_, params_, &result_value, success, error_code,
      error_message, start_time_ms_, duration_ms, screenshot_before_path_,
      screenshot_after_path_);
}

void AbpActionContext::BuildResponseEnvelope() {
  // The response envelope is built in SendResponse()
  // This method is a placeholder for any pre-processing
}

void AbpActionContext::SendResponse() {
  action_timeout_timer_.Stop();
  if (!IsCurrentAction()) {
    return;
  }
  VLOG(1) << "ABP ActionContext: SendResponse() action=" << action_type_;
  if (!response_callback_) {
    LOG(WARNING) << "ABP ActionContext: SendResponse() - NO CALLBACK!";
    ReleaseDeterministicSlot();
    prevent_destroy_ = nullptr;
    return;
  }

  // Build full response envelope
  base::Value::Dict envelope;

  // 1. Add action result
  envelope.Set("result", std::move(result_));

  // 2. Add before screenshot
  if (!screenshot_before_base64_.empty()) {
    base::Value::Dict sb;
    sb.Set("data", screenshot_before_base64_);
    sb.Set("width", screenshot_before_width_);
    sb.Set("height", screenshot_before_height_);
    sb.Set("virtual_time_ms", static_cast<double>(virtual_time_at_start_));
    sb.Set("format", screenshot_format_);
    envelope.Set("screenshot_before", std::move(sb));
  }

  // 3. Add after screenshot
  if (!screenshot_after_base64_.empty()) {
    base::Value::Dict sa;
    sa.Set("data", screenshot_after_base64_);
    sa.Set("width", screenshot_after_width_);
    sa.Set("height", screenshot_after_height_);
    sa.Set("virtual_time_ms", static_cast<double>(virtual_time_at_end_));
    sa.Set("format", screenshot_format_);
    envelope.Set("screenshot_after", std::move(sa));
  }

  // 4. Add scroll position
  if (!scroll_info_.empty()) {
    envelope.Set("scroll", std::move(scroll_info_));
  }

  // 5. Add captured events
  base::Value::List events_list;
  for (auto& event : captured_events_) {
    base::Value::Dict event_dict;
    event_dict.Set("type", event.type);
    event_dict.Set("virtual_time_ms", static_cast<double>(event.virtual_time_ms));
    event_dict.Set("data", std::move(event.data));
    events_list.Append(std::move(event_dict));
  }
  envelope.Set("events", std::move(events_list));

  // 6. Add timing info
  base::Value::Dict timing;
  timing.Set("action_started_ms", static_cast<double>(start_time_ms_));
  timing.Set("action_completed_ms",
             static_cast<double>(start_time_ms_ +
                 (action_end_ticks_ - start_ticks_).InMilliseconds()));
  timing.Set("wait_completed_ms", static_cast<double>(wait_completed_ms_));
  timing.Set("duration_ms",
             static_cast<int>(wait_completed_ms_ - start_time_ms_));
  envelope.Set("timing", std::move(timing));

  // 7. Add virtual time info if execution control is enabled
  if (controller_->IsExecutionControlEnabled()) {
    auto it = controller_->tab_states_.find(tab_id_);
    if (it != controller_->tab_states_.end()) {
      const auto& exec = it->second.execution;
      base::Value::Dict virtual_time;
      virtual_time.Set("paused", exec.IsPaused());
      virtual_time.Set("base_ticks_ms", exec.virtual_time_base_ticks_ms);
      envelope.Set("virtual_time", std::move(virtual_time));
    }
  }

  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kResponseSent);
  }
  controller_->SendJson(200, base::Value(std::move(envelope)),
                        std::move(response_callback_));

  ReleaseDeterministicSlot();
  // Clear self-reference to allow destruction
  prevent_destroy_ = nullptr;
}

void AbpActionContext::SendErrorResponse(int status,
                                         const std::string& error_code,
                                         const std::string& error_message) {
  action_timeout_timer_.Stop();
  if (!IsCurrentAction()) {
    return;
  }
  if (!response_callback_) {
    ReleaseDeterministicSlot();
    prevent_destroy_ = nullptr;
    return;
  }

  // Record the error in history if we have enough context
  if (!has_error_) {
    has_error_ = true;
    error_code_ = error_code;
    error_message_ = error_message;
    RecordHistory(false, error_code, error_message);
  }

  controller_->SendError(status, error_message, std::move(response_callback_));

  ReleaseDeterministicSlot();
  // Clear self-reference to allow destruction
  prevent_destroy_ = nullptr;
}

void AbpActionContext::Fail(int http_status,
                            const std::string& error_code,
                            const std::string& error_message) {
  action_timeout_timer_.Stop();

  if (!IsCurrentAction()) {
    ReleaseDeterministicSlot();
    prevent_destroy_ = nullptr;
    return;
  }

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

}  // namespace abp
