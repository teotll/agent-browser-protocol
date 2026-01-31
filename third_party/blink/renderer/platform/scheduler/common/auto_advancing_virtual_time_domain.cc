// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/scheduler/common/auto_advancing_virtual_time_domain.h"

#include <atomic>

#include "base/time/time_override.h"
#include "build/build_config.h"
#include "third_party/blink/renderer/platform/scheduler/common/scheduler_helper.h"

namespace blink {
namespace scheduler {

AutoAdvancingVirtualTimeDomain::AutoAdvancingVirtualTimeDomain(
    base::Time initial_time,
    base::TimeTicks initial_time_ticks,
    SchedulerHelper* helper)
    : task_starvation_count_(0),
      max_task_starvation_count_(0),
      can_advance_virtual_time_(true),
      helper_(helper),
      time_override_(ProcessTimeOverrideCoordinator::CreateOverride(
          initial_time,
          initial_time_ticks,
          base::BindRepeating(
              &AutoAdvancingVirtualTimeDomain::NotifyPolicyChanged,
              base::Unretained(this)))),
      initial_time_ticks_(time_override_->NowTicks()) {
  helper_->AddTaskObserver(this);
}

AutoAdvancingVirtualTimeDomain::~AutoAdvancingVirtualTimeDomain() {
  helper_->RemoveTaskObserver(this);
}

base::TimeTicks AutoAdvancingVirtualTimeDomain::NowTicks() const {
  // In realtime mode, advance virtual time in sync with wall-clock time.
  // This ensures Date.now() and performance.now() advance smoothly,
  // enabling proper animation playback after pause/resume.
  if (realtime_mode_enabled_) {
    base::TimeTicks wall_clock_now =
        base::subtle::TimeTicksNowIgnoringOverride();
    base::TimeDelta wall_clock_elapsed =
        wall_clock_now - realtime_mode_wall_clock_base_;
    base::TimeTicks target_virtual_time =
        realtime_mode_virtual_time_base_ + wall_clock_elapsed;

    // Try to advance the time override to match wall-clock elapsed time.
    // TryAdvancingTime will update the global time override that Date.now()
    // and performance.now() read from.
    base::TimeTicks actual_time = time_override_->TryAdvancingTime(target_virtual_time);
    return actual_time;
  }
  return time_override_->NowTicks();
}

bool AutoAdvancingVirtualTimeDomain::MaybeFastForwardToWakeUp(
    std::optional<base::sequence_manager::WakeUp> wakeup,
    bool quit_when_idle_requested) {
  if (!can_advance_virtual_time_)
    return false;

  if (!wakeup)
    return false;

  // In realtime mode, calculate max allowed virtual time based on wall clock
  if (realtime_mode_enabled_) {
    // IMPORTANT: Use TimeTicksNowIgnoringOverride to get actual wall-clock time.
    // Regular TimeTicks::Now() would return the overridden virtual time since
    // ProcessTimeOverrideCoordinator overrides it, which would break the
    // comparison (we'd be comparing virtual time against itself).
    base::TimeTicks wall_clock_now =
        base::subtle::TimeTicksNowIgnoringOverride();
    base::TimeDelta wall_clock_elapsed =
        wall_clock_now - realtime_mode_wall_clock_base_;
    base::TimeTicks max_virtual_time =
        realtime_mode_virtual_time_base_ + wall_clock_elapsed;

    // Only advance if the wake up time is within our allowed virtual time
    if (wakeup->time <= max_virtual_time) {
      if (MaybeAdvanceVirtualTime(wakeup->time)) {
        task_starvation_count_ = 0;
        return true;
      }
    }
    // Wake up is in the future - don't fast forward, scheduler will idle
    return false;
  }

  // Standard advance mode - fast forward to wake up time
  if (MaybeAdvanceVirtualTime(wakeup->time)) {
    task_starvation_count_ = 0;
    return true;
  }

  return false;
}

void AutoAdvancingVirtualTimeDomain::SetCanAdvanceVirtualTime(
    bool can_advance_virtual_time) {
  can_advance_virtual_time_ = can_advance_virtual_time;
  if (can_advance_virtual_time_)
    NotifyPolicyChanged();
}

void AutoAdvancingVirtualTimeDomain::SetMaxVirtualTimeTaskStarvationCount(
    int max_task_starvation_count) {
  max_task_starvation_count_ = max_task_starvation_count;
  if (max_task_starvation_count_ == 0)
    task_starvation_count_ = 0;
}

void AutoAdvancingVirtualTimeDomain::SetVirtualTimeFence(
    base::TimeTicks virtual_time_fence) {
  virtual_time_fence_ = virtual_time_fence;
  if (!requested_next_virtual_time_.is_null())
    MaybeAdvanceVirtualTime(requested_next_virtual_time_);
}

void AutoAdvancingVirtualTimeDomain::SetRealtimeMode(bool enabled) {
  if (realtime_mode_enabled_ == enabled)
    return;

  realtime_mode_enabled_ = enabled;
  if (enabled) {
    // Record the current wall clock and virtual time as the base.
    // IMPORTANT: Use TimeTicksNowIgnoringOverride to get actual wall-clock time.
    // Regular TimeTicks::Now() would return the overridden virtual time since
    // ProcessTimeOverrideCoordinator overrides it.
    realtime_mode_wall_clock_base_ =
        base::subtle::TimeTicksNowIgnoringOverride();
    realtime_mode_virtual_time_base_ = NowTicks();
  }
  NotifyPolicyChanged();
}

bool AutoAdvancingVirtualTimeDomain::MaybeAdvanceVirtualTime(
    base::TimeTicks new_virtual_time) {
  // If set, don't advance past the end of |virtual_time_fence_|.
  if (!virtual_time_fence_.is_null() &&
      new_virtual_time > virtual_time_fence_) {
    requested_next_virtual_time_ = new_virtual_time;
    new_virtual_time = virtual_time_fence_;
  } else {
    requested_next_virtual_time_ = base::TimeTicks();
  }

  // Currently, a virtual time pauser may try to advance time to
  // a value from the past.
  // TODO(caseq): make sure we don't try "advancing" to past values.
  if (new_virtual_time <= NowTicks()) {
    return false;
  }

  return time_override_->TryAdvancingTime(new_virtual_time) == new_virtual_time;
}

const char* AutoAdvancingVirtualTimeDomain::GetName() const {
  return "AutoAdvancingVirtualTimeDomain";
}

void AutoAdvancingVirtualTimeDomain::WillProcessTask(
    const base::PendingTask& pending_task,
    bool was_blocked_or_low_priority) {}

void AutoAdvancingVirtualTimeDomain::DidProcessTask(
    const base::PendingTask& pending_task) {
  if (max_task_starvation_count_ == 0 ||
      ++task_starvation_count_ < max_task_starvation_count_) {
    return;
  }

  // In realtime mode, don't allow task starvation to force time jumps.
  // The MaybeFastForwardToWakeUp method handles realtime advancement properly
  // by checking against wall clock elapsed time.
  if (realtime_mode_enabled_) {
    task_starvation_count_ = 0;
    return;
  }

  // Delayed tasks are being excessively starved, so allow virtual time to
  // advance.
  auto wake_up = helper_->GetNextWakeUp();
  if (wake_up && MaybeAdvanceVirtualTime(wake_up->time))
    task_starvation_count_ = 0;
}

}  // namespace scheduler
}  // namespace blink
