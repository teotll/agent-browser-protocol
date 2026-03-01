// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_action_context.h"

#include <set>

#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/abp/abp_controller.h"
#include "ui/base/cursor/mojom/cursor_type.mojom.h"
#include "chrome/browser/abp/abp_event_collector.h"
#include "chrome/browser/abp/abp_history_controller.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"

namespace {

std::string CursorTypeToString(ui::mojom::CursorType type) {
  switch (type) {
    case ui::mojom::CursorType::kPointer:
    case ui::mojom::CursorType::kNull:
      return "pointer";
    case ui::mojom::CursorType::kHand:
      return "hand";
    case ui::mojom::CursorType::kIBeam:
      return "text";
    case ui::mojom::CursorType::kCross:
      return "crosshair";
    case ui::mojom::CursorType::kWait:
      return "wait";
    case ui::mojom::CursorType::kHelp:
      return "help";
    case ui::mojom::CursorType::kMove:
      return "move";
    case ui::mojom::CursorType::kProgress:
      return "progress";
    case ui::mojom::CursorType::kNotAllowed:
      return "not-allowed";
    case ui::mojom::CursorType::kNoDrop:
      return "no-drop";
    case ui::mojom::CursorType::kGrab:
      return "grab";
    case ui::mojom::CursorType::kGrabbing:
      return "grabbing";
    case ui::mojom::CursorType::kZoomIn:
      return "zoom-in";
    case ui::mojom::CursorType::kZoomOut:
      return "zoom-out";
    case ui::mojom::CursorType::kContextMenu:
      return "context-menu";
    case ui::mojom::CursorType::kCell:
      return "cell";
    case ui::mojom::CursorType::kAlias:
      return "alias";
    case ui::mojom::CursorType::kCopy:
      return "copy";
    case ui::mojom::CursorType::kNone:
      return "none";
    case ui::mojom::CursorType::kVerticalText:
      return "vertical-text";
    case ui::mojom::CursorType::kColumnResize:
      return "col-resize";
    case ui::mojom::CursorType::kRowResize:
      return "row-resize";
    case ui::mojom::CursorType::kNorthResize:
      return "n-resize";
    case ui::mojom::CursorType::kSouthResize:
      return "s-resize";
    case ui::mojom::CursorType::kEastResize:
      return "e-resize";
    case ui::mojom::CursorType::kWestResize:
      return "w-resize";
    case ui::mojom::CursorType::kNorthEastResize:
      return "ne-resize";
    case ui::mojom::CursorType::kNorthWestResize:
      return "nw-resize";
    case ui::mojom::CursorType::kSouthEastResize:
      return "se-resize";
    case ui::mojom::CursorType::kSouthWestResize:
      return "sw-resize";
    case ui::mojom::CursorType::kNorthSouthResize:
      return "ns-resize";
    case ui::mojom::CursorType::kEastWestResize:
      return "ew-resize";
    case ui::mojom::CursorType::kNorthEastSouthWestResize:
      return "nesw-resize";
    case ui::mojom::CursorType::kNorthWestSouthEastResize:
      return "nwse-resize";
    default:
      return "pointer";
  }
}

}  // namespace

namespace abp {

// static
std::string AbpActionContext::GenerateActionId() {
  static constexpr std::string_view kChars(
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
  std::string id;
  id.reserve(6);
  for (int i = 0; i < 6; ++i) {
    id += kChars[static_cast<size_t>(base::RandInt(0, 35))];
  }
  return id;
}

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
  ctx->profile_queued_at_ = base::TimeTicks::Now();

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
      action_id_(GenerateActionId()),
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
  profile_slot_acquired_ = base::TimeTicks::Now();
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

  // Parse screenshot options from action params.
  // No markup overlays by default; clients specify which ones to enable via "markup".
  // Legacy "disable_markup" is also supported (all tags minus disabled ones).
  const base::Value::Dict* ss_params = params_.FindDict("screenshot");
  if (ss_params) {
    const base::Value::List* markup_list = ss_params->FindList("markup");
    const base::Value::List* disable_list = ss_params->FindList("disable_markup");
    if (markup_list) {
      // New API: client specifies exactly which overlays to enable.
      for (const auto& tag : *markup_list) {
        if (tag.is_string()) {
          screenshot_markup_tags_.push_back(tag.GetString());
        }
      }
    } else if (disable_list) {
      // Legacy API: all tags minus disabled ones.
      std::set<std::string> disabled;
      for (const auto& tag : *disable_list) {
        if (tag.is_string()) {
          disabled.insert(tag.GetString());
        }
      }
      for (const char* tag : kAllMarkupTags) {
        if (disabled.find(tag) == disabled.end()) {
          screenshot_markup_tags_.emplace_back(tag);
        }
      }
    }
    // Validate whatever tags we ended up with.
    std::string invalid_tag;
    if (!AbpController::ValidateMarkupTags(screenshot_markup_tags_,
                                           &invalid_tag)) {
      LOG(WARNING) << "ABP: Unknown markup tag '" << invalid_tag
                   << "', ignoring all markup";
      screenshot_markup_tags_.clear();
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

  // Record whether the page's main document was already loaded before resume.
  // This prevents waiting for load/dcl/paint lifecycle events that either
  // already fired or will never fire (because they are one-shot per navigation
  // and fired before this waiter's observer was created).
  //
  // Three checks, any one sufficient:
  // 1. IsDocumentOnLoadCompleted — main document fully loaded
  // 2. !IsLoading — no pending loads at all
  // 3. Execution control is paused — the page was frozen by a prior action,
  //    meaning DCL/paint already fired (we only freeze after JS has executed).
  //    Even if load hasn't completed (e.g. ad scripts still pending), the
  //    main document is rendered and interactive.
  {
    auto tab_it = controller_->tab_states_.find(tab_id_);
    bool exec_paused = tab_it != controller_->tab_states_.end() &&
                       tab_it->second.execution.IsPaused();
    page_was_loaded_before_action_ =
        web_contents_->IsDocumentOnLoadCompletedInPrimaryMainFrame() ||
        !web_contents_->IsLoading() ||
        exec_paused;
  }

  // Start event capture
  StartEventCapture();

  // Capture before screenshot while JS is still paused — the screen buffer
  // is frozen so we can grab it directly without ForceRedraw.
  profile_before_ss_start_ = base::TimeTicks::Now();
  CaptureBeforeScreenshot();
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

  // Before screenshot was already captured from the frozen buffer before
  // resume.  Proceed directly to executing the action.
  profile_action_start_ = base::TimeTicks::Now();
  ExecuteAction();
}

void AbpActionContext::CaptureBeforeScreenshot() {
  VLOG(1) << "ABP ActionContext: CaptureBeforeScreenshot() action="
          << action_type_ << " tab=" << tab_id_;
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kBeforeScreenshotStarted);
  }
  AbpController::ScreenshotOptions opts;
  opts.format = screenshot_format_;
  opts.quality = screenshot_quality_;
  // No markup_tags — markup CSS can't be painted without a ForceRedraw,
  // and we're capturing from the frozen buffer while JS is still paused.

  controller_->CaptureScreenshotFromBuffer(
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
  VLOG(1) << "ABP ActionContext: OnBeforeScreenshotCaptured() action="
            << action_type_ << " base64_len=" << base64.size()
            << " width=" << width << " height=" << height
            << " tab=" << tab_id_;
  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kBeforeScreenshotCompleted);
  }
  screenshot_before_path_ = std::move(history_path);
  screenshot_before_base64_ = std::move(base64);
  screenshot_before_width_ = width;
  screenshot_before_height_ = height;
  profile_before_ss_end_ = base::TimeTicks::Now();

  if (has_error_) {
    return;
  }

  // Now resume execution — the before screenshot is already captured
  profile_resume_start_ = base::TimeTicks::Now();
  ResumeExecutionIfNeeded();
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
  profile_action_end_ = action_end_ticks_;

  if (has_error_) {
    return;
  }

  // Center cursor early (before wait) so it's visible during page load
  profile_wait_start_ = base::TimeTicks::Now();
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
      options_.min_wait_time,
      options_.request_tracking_timeout,
      options_.post_tracking_settle_time,
      page_was_loaded_before_action_);
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
  profile_wait_end_ = base::TimeTicks::Now();

  // Stop event capture
  if (controller_->event_collector()) {
    captured_events_ = controller_->event_collector()->StopCapturing();
  }

  // Scroll position is now read from compositor RenderFrameMetadata
  // after ForceRedraw in the after-screenshot pipeline — zero CDP
  // round-trips, zero main-thread contention.
  profile_scroll_start_ = base::TimeTicks::Now();
  profile_scroll_end_ = profile_scroll_start_;  // 0ms — read happens in screenshot

  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kScrollPositionReceived);
  }

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
  opts.markup_tags = screenshot_markup_tags_;

  controller_->CaptureActionScreenshot(
      tab_id_, start_time_ms_, false, opts,
      base::BindOnce(
          [](base::WeakPtr<AbpActionContext> ctx,
             AbpController::ActionScreenshotResult r) {
            if (!ctx) return;
            // Extract scroll info from compositor metadata (populated after
            // ForceRedraw in OnActionScreenshotCaptured).
            ctx->scroll_info_ = std::move(r.scroll_info);
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

  // Send the response immediately — don't wait for pause.
  // The deterministic slot is held until pause completes, so the next action
  // still waits for the page to be frozen before starting.
  FinalizeResponse();

  // Pause in background while the client already has the response.
  profile_pause_start_ = base::TimeTicks::Now();
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

  // Global execution control is enabled — delegate to PauseExecution which
  // handles auto-enabling per-tab execution control if not yet enabled.
  controller_->PauseExecution(
      tab_id_,
      base::BindOnce(&AbpActionContext::OnExecutionPaused,
                     weak_factory_.GetWeakPtr()));
}

void AbpActionContext::OnExecutionPaused() {
  if (!IsCurrentAction()) {
    return;
  }
  profile_pause_end_ = base::TimeTicks::Now();
  VLOG(1) << "ABP ActionContext: OnExecutionPaused() action=" << action_type_
          << " pause_ms="
          << (profile_pause_end_ - profile_pause_start_).InMilliseconds();

  // Response was already sent in OnAfterScreenshotCaptured.
  // Just release the deterministic slot so the next action can start.
  ReleaseDeterministicSlot();
  prevent_destroy_ = nullptr;
}

void AbpActionContext::LogProfilingSummary() {
  auto ms = [](base::TimeTicks start, base::TimeTicks end) -> int64_t {
    if (start.is_null() || end.is_null()) return -1;
    return (end - start).InMilliseconds();
  };
  auto total = ms(start_ticks_, base::TimeTicks::Now());
  auto queue_wait = ms(profile_queued_at_, profile_slot_acquired_);
  VLOG(1) << "ABP PROFILE [" << action_type_ << "] tab=" << tab_id_
            << " total=" << total << "ms"
            << " | queue_wait=" << queue_wait << "ms"
            << " | before_ss=" << ms(profile_before_ss_start_, profile_before_ss_end_) << "ms"
            << " | resume=" << ms(profile_resume_start_, profile_resume_end_) << "ms"
            << " | action=" << ms(profile_action_start_, profile_action_end_) << "ms"
            << " | wait=" << ms(profile_wait_start_, profile_wait_end_) << "ms"
            << " | scroll_pos=" << ms(profile_scroll_start_, profile_scroll_end_) << "ms"
            << " | after_ss=" << ms(profile_after_ss_start_, profile_after_ss_end_) << "ms"
            << " | pause=" << ms(profile_pause_start_, profile_pause_end_) << "ms";
}

void AbpActionContext::FinalizeResponse() {
  VLOG(1) << "ABP ActionContext: FinalizeResponse() action=" << action_type_;

  LogProfilingSummary();

  // Send response first to minimize client-perceived latency.
  // History recording happens after, in parallel with pause.
  if (has_error_) {
    SendErrorResponse(500, error_code_, error_message_);
  } else {
    BuildResponseEnvelope();
    SendResponse();
  }

  // Record to history after response is sent (non-blocking for client)
  RecordHistory(!has_error_, error_code_, error_message_);
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
      action_id_, tab_id_, action_type_, params_, &result_value, success,
      error_code, error_message, start_time_ms_, duration_ms,
      screenshot_before_path_, screenshot_after_path_);
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
    // Slot release and destroy happen in OnExecutionPaused after pause completes.
    return;
  }

  // Build full response envelope
  base::Value::Dict envelope;

  // 0. Add action ID
  envelope.Set("action_id", action_id_);

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
  // Note: response is sent before pause completes (pause runs in background),
  // so we report paused=true since the pause will happen before the next
  // action can start (deterministic slot is held until pause completes).
  if (controller_->IsExecutionControlEnabled()) {
    auto it = controller_->tab_states_.find(tab_id_);
    if (it != controller_->tab_states_.end()) {
      const auto& exec = it->second.execution;
      base::Value::Dict virtual_time;
      virtual_time.Set("paused", !options_.skip_pause);
      virtual_time.Set("base_ticks_ms", exec.virtual_time_base_ticks_ms);
      envelope.Set("virtual_time", std::move(virtual_time));
    }
  }

  // 8. Add profiling breakdown (ms per phase)
  {
    auto ms = [](base::TimeTicks start, base::TimeTicks end) -> int {
      if (start.is_null() || end.is_null()) return -1;
      return static_cast<int>((end - start).InMilliseconds());
    };
    base::Value::Dict profiling;
    profiling.Set("queue_wait_ms", ms(profile_queued_at_, profile_slot_acquired_));
    profiling.Set("before_screenshot_ms", ms(profile_before_ss_start_, profile_before_ss_end_));
    profiling.Set("resume_ms", ms(profile_resume_start_, profile_resume_end_));
    profiling.Set("action_ms", ms(profile_action_start_, profile_action_end_));
    profiling.Set("wait_ms", ms(profile_wait_start_, profile_wait_end_));
    profiling.Set("after_screenshot_ms", ms(profile_after_ss_start_, profile_after_ss_end_));
    profiling.Set("total_ms", ms(profile_queued_at_, profile_after_ss_end_));
    envelope.Set("profiling", std::move(profiling));
  }

  // 9. Add virtual cursor position
  {
    auto it = controller_->tab_states_.find(tab_id_);
    if (it != controller_->tab_states_.end()) {
      const auto& cursor = it->second.cursor;
      base::Value::Dict cursor_dict;
      cursor_dict.Set("x", cursor.x);
      cursor_dict.Set("y", cursor.y);
      cursor_dict.Set("cursor_type", CursorTypeToString(cursor.cursor_type));
      envelope.Set("cursor", std::move(cursor_dict));
    }
  }

  if (controller_->lifecycle_observer_for_testing_) {
    controller_->lifecycle_observer_for_testing_.Run(
        tab_id_, action_type_,
        AbpController::LifecycleStep::kResponseSent);
  }
  controller_->SendJson(200, base::Value(std::move(envelope)),
                        std::move(response_callback_));

  // Don't release slot or clear self-ref here — OnExecutionPaused does that
  // after the background pause completes, ensuring the next action waits
  // for the page to be frozen.
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

  // Build error response with action_id
  base::Value::Dict envelope;
  envelope.Set("action_id", action_id_);
  envelope.Set("error", error_code);
  envelope.Set("message", error_message);
  controller_->SendJson(status, base::Value(std::move(envelope)),
                        std::move(response_callback_));

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
    base::Value::Dict envelope;
    envelope.Set("action_id", action_id_);
    envelope.Set("error", error_code);
    envelope.Set("message", error_message);
    controller_->SendJson(http_status, base::Value(std::move(envelope)),
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
