# Virtual Time "Realtime" Policy Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a new `kRealtime` virtual time policy that allows time to advance in sync with wall-clock time (not fast-forward), enabling smooth animation resume after pause.

**Architecture:** Extend the existing `VirtualTimePolicy` enum with a new `kRealtime` variant. This policy allows virtual time to advance but ties advancement to wall-clock intervals rather than jumping to the next scheduled task. When the scheduler becomes idle, instead of fast-forwarding to the next task, it waits for real time to catch up.

**Tech Stack:** Blink scheduler (C++), Chrome DevTools Protocol (PDL), Inspector Emulation Agent

---

## Background

### The Problem
The current `kAdvance` policy fast-forwards virtual time whenever the scheduler is idle:
```
Paused at T=1000ms
Resume with kAdvance
Scheduler idle → jumps to next task at T=5000ms instantly
Animations see 4000ms jump → "superspeed" effect
```

### The Solution
A new `kRealtime` policy that advances virtual time at wall-clock rate:
```
Paused at T=1000ms (wall clock = W1)
Resume with kRealtime
After 100ms wall-clock → virtual time = 1100ms
After 200ms wall-clock → virtual time = 1200ms
Animations see normal 16ms frame intervals → smooth playback
```

---

## File Modification Overview

| File | Change Type | Purpose |
|------|-------------|---------|
| `third_party/blink/renderer/platform/scheduler/public/virtual_time_controller.h` | Modify | Add `kRealtime` to enum |
| `third_party/blink/renderer/platform/scheduler/common/thread_scheduler_base.h` | Modify | Add realtime tracking state |
| `third_party/blink/renderer/platform/scheduler/common/thread_scheduler_base.cc` | Modify | Handle kRealtime in ApplyVirtualTimePolicy |
| `third_party/blink/renderer/platform/scheduler/common/auto_advancing_virtual_time_domain.h` | Modify | Add realtime mode methods |
| `third_party/blink/renderer/platform/scheduler/common/auto_advancing_virtual_time_domain.cc` | Modify | Implement realtime advancement logic |
| `third_party/blink/public/devtools_protocol/domains/Emulation.pdl` | Modify | Add "realtime" to VirtualTimePolicy enum |
| `third_party/blink/renderer/core/inspector/inspector_emulation_agent.cc` | Modify | Map "realtime" to kRealtime |
| `third_party/blink/renderer/core/scheduler_integration_tests/virtual_time_test.cc` | Modify | Add tests for kRealtime policy |

---

## Task 1: Add kRealtime to VirtualTimePolicy Enum

**Files:**
- Modify: `third_party/blink/renderer/platform/scheduler/public/virtual_time_controller.h:29-48`

**Step 1: Add the new enum value**

Edit the `VirtualTimePolicy` enum to add `kRealtime`:

```cpp
enum class VirtualTimePolicy {
  // In this policy virtual time is allowed to advance. If the blink scheduler
  // runs out of immediate work, the virtual timebase will be incremented so
  // that the next sceduled timer may fire.  NOTE Tasks will be run in time
  // order (as usual).
  kAdvance,

  // In this policy virtual time is not allowed to advance. Delayed tasks
  // posted to task runners owned by any child FrameSchedulers will be
  // paused, unless their scheduled run time is less than or equal to the
  // current virtual time.  Note non-delayed tasks will run as normal.
  kPause,

  // In this policy virtual time is allowed to advance unless there are
  // pending network fetches associated any child FrameScheduler, or a
  // document is being parsed on a background thread. Initially virtual time
  // is not allowed to advance until we have seen at least one load. The aim
  // being to try and make loading (more) deterministic.
  kDeterministicLoading,

  // In this policy virtual time advances in sync with wall-clock time.
  // Unlike kAdvance which fast-forwards to the next task, kRealtime waits
  // for wall-clock time to pass before advancing virtual time. This enables
  // smooth animation playback after pausing. Tasks scheduled in the future
  // will fire at their correct relative time, not instantly.
  kRealtime,
};
```

**Step 2: Verify compilation**

Run: `autoninja -C out/Default third_party/blink/renderer/platform/scheduler:scheduler`
Expected: Build succeeds (tests may fail until fully implemented)

**Step 3: Commit**

```bash
git add third_party/blink/renderer/platform/scheduler/public/virtual_time_controller.h
git commit -m "feat(scheduler): add kRealtime to VirtualTimePolicy enum

Add a new virtual time policy that advances time in sync with
wall-clock time instead of fast-forwarding. This is needed for
smooth animation resume after pause."
```

---

## Task 2: Add Realtime State Tracking to AutoAdvancingVirtualTimeDomain

**Files:**
- Modify: `third_party/blink/renderer/platform/scheduler/common/auto_advancing_virtual_time_domain.h`

**Step 1: Add state variables for realtime mode**

Add these private members to the `AutoAdvancingVirtualTimeDomain` class:

```cpp
 private:
  // ... existing members ...

  // Realtime mode state - tracks wall clock offset for sync advancement
  bool realtime_mode_enabled_ = false;
  base::TimeTicks realtime_mode_wall_clock_base_;  // Wall clock when realtime mode started
  base::TimeTicks realtime_mode_virtual_time_base_;  // Virtual time when realtime mode started
```

**Step 2: Add public methods for realtime mode**

Add these public methods to the class:

```cpp
 public:
  // ... existing methods ...

  // Enable realtime mode - virtual time advances with wall clock
  void SetRealtimeMode(bool enabled);

  // Check if realtime mode is active
  bool IsRealtimeModeEnabled() const { return realtime_mode_enabled_; }
```

**Step 3: Commit**

```bash
git add third_party/blink/renderer/platform/scheduler/common/auto_advancing_virtual_time_domain.h
git commit -m "feat(scheduler): add realtime mode state to AutoAdvancingVirtualTimeDomain"
```

---

## Task 3: Implement Realtime Mode in AutoAdvancingVirtualTimeDomain

**Files:**
- Modify: `third_party/blink/renderer/platform/scheduler/common/auto_advancing_virtual_time_domain.cc`

**Step 1: Implement SetRealtimeMode**

Add after `SetVirtualTimeFence`:

```cpp
void AutoAdvancingVirtualTimeDomain::SetRealtimeMode(bool enabled) {
  if (realtime_mode_enabled_ == enabled)
    return;

  realtime_mode_enabled_ = enabled;
  if (enabled) {
    // Record the current wall clock and virtual time as the base
    realtime_mode_wall_clock_base_ = base::TimeTicks::Now();
    realtime_mode_virtual_time_base_ = NowTicks();
  }
  NotifyPolicyChanged();
}
```

**Step 2: Modify MaybeFastForwardToWakeUp for realtime mode**

Replace the existing `MaybeFastForwardToWakeUp` implementation:

```cpp
bool AutoAdvancingVirtualTimeDomain::MaybeFastForwardToWakeUp(
    std::optional<base::sequence_manager::WakeUp> wakeup,
    bool quit_when_idle_requested) {
  if (!can_advance_virtual_time_)
    return false;

  if (!wakeup)
    return false;

  // In realtime mode, calculate max allowed virtual time based on wall clock
  if (realtime_mode_enabled_) {
    base::TimeTicks wall_clock_now = base::TimeTicks::Now();
    base::TimeDelta wall_clock_elapsed = wall_clock_now - realtime_mode_wall_clock_base_;
    base::TimeTicks max_virtual_time = realtime_mode_virtual_time_base_ + wall_clock_elapsed;

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
```

**Step 3: Verify compilation**

Run: `autoninja -C out/Default third_party/blink/renderer/platform/scheduler:scheduler`
Expected: Build succeeds

**Step 4: Commit**

```bash
git add third_party/blink/renderer/platform/scheduler/common/auto_advancing_virtual_time_domain.cc
git commit -m "feat(scheduler): implement realtime mode in AutoAdvancingVirtualTimeDomain

In realtime mode, virtual time only advances up to the wall-clock
elapsed time since mode was enabled. This prevents fast-forwarding
and allows smooth animation playback."
```

---

## Task 4: Handle kRealtime in ThreadSchedulerBase

**Files:**
- Modify: `third_party/blink/renderer/platform/scheduler/common/thread_scheduler_base.cc:171-201`

**Step 1: Add kRealtime case to ApplyVirtualTimePolicy**

Modify the switch statement in `ApplyVirtualTimePolicy`:

```cpp
void ThreadSchedulerBase::ApplyVirtualTimePolicy() {
  DCHECK(virtual_time_domain_);
  switch (virtual_time_policy_) {
    case VirtualTimePolicy::kAdvance:
      virtual_time_domain_->SetMaxVirtualTimeTaskStarvationCount(
          GetHelper().IsInNestedRunloop()
              ? 0
              : max_virtual_time_task_starvation_count_);
      virtual_time_domain_->SetVirtualTimeFence(base::TimeTicks());
      virtual_time_domain_->SetRealtimeMode(false);
      SetVirtualTimeStopped(false);
      break;
    case VirtualTimePolicy::kPause:
      virtual_time_domain_->SetMaxVirtualTimeTaskStarvationCount(0);
      virtual_time_domain_->SetVirtualTimeFence(GetTickClock()->NowTicks());
      virtual_time_domain_->SetRealtimeMode(false);
      SetVirtualTimeStopped(true);
      break;
    case VirtualTimePolicy::kDeterministicLoading:
      virtual_time_domain_->SetMaxVirtualTimeTaskStarvationCount(
          GetHelper().IsInNestedRunloop()
              ? 0
              : max_virtual_time_task_starvation_count_);
      virtual_time_domain_->SetRealtimeMode(false);
      // We pause virtual time while the run loop is nested because that implies
      // something modal is happening such as the DevTools debugger pausing the
      // system. We also pause while the renderer is waiting for various
      // asynchronous things e.g. resource load or navigation.
      SetVirtualTimeStopped(virtual_time_pause_count_ != 0 ||
                            GetHelper().IsInNestedRunloop());
      break;
    case VirtualTimePolicy::kRealtime:
      // Realtime mode: allow advancement but sync with wall clock
      virtual_time_domain_->SetMaxVirtualTimeTaskStarvationCount(
          GetHelper().IsInNestedRunloop()
              ? 0
              : max_virtual_time_task_starvation_count_);
      virtual_time_domain_->SetVirtualTimeFence(base::TimeTicks());
      virtual_time_domain_->SetRealtimeMode(true);
      SetVirtualTimeStopped(false);
      break;
  }
}
```

**Step 2: Verify compilation**

Run: `autoninja -C out/Default third_party/blink/renderer/platform/scheduler:scheduler`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add third_party/blink/renderer/platform/scheduler/common/thread_scheduler_base.cc
git commit -m "feat(scheduler): handle kRealtime policy in ThreadSchedulerBase

Enable realtime mode on the virtual time domain when kRealtime
policy is set. This ties virtual time advancement to wall-clock time."
```

---

## Task 5: Add "realtime" to CDP Emulation Domain

**Files:**
- Modify: `third_party/blink/public/devtools_protocol/domains/Emulation.pdl:70-78`

**Step 1: Add realtime to VirtualTimePolicy enum**

```pdl
  # advance: If the scheduler runs out of immediate work, the virtual time base may fast forward to
  # allow the next delayed task (if any) to run; pause: The virtual time base may not advance;
  # pauseIfNetworkFetchesPending: The virtual time base may not advance if there are any pending
  # resource fetches; realtime: Virtual time advances in sync with wall-clock time, enabling smooth
  # animation playback after pause.
  experimental type VirtualTimePolicy extends string
    enum
      advance
      pause
      pauseIfNetworkFetchesPending
      realtime
```

**Step 2: Regenerate protocol bindings**

Run: `autoninja -C out/Default third_party/blink/renderer/core/inspector:protocol_sources`
Expected: Protocol sources regenerated

**Step 3: Commit**

```bash
git add third_party/blink/public/devtools_protocol/domains/Emulation.pdl
git commit -m "feat(cdp): add 'realtime' to VirtualTimePolicy in Emulation domain"
```

---

## Task 6: Map "realtime" in InspectorEmulationAgent

**Files:**
- Modify: `third_party/blink/renderer/core/inspector/inspector_emulation_agent.cc:544-563`

**Step 1: Add mapping for realtime policy**

Modify the `setVirtualTimePolicy` method:

```cpp
protocol::Response InspectorEmulationAgent::setVirtualTimePolicy(
    const String& policy,
    std::optional<double> virtual_time_budget_ms,
    std::optional<int> max_virtual_time_task_starvation_count,
    std::optional<double> initial_virtual_time,
    double* virtual_time_ticks_base_ms) {
  VirtualTimeController::VirtualTimePolicy scheduler_policy =
      VirtualTimeController::VirtualTimePolicy::kPause;
  if (protocol::Emulation::VirtualTimePolicyEnum::Advance == policy) {
    scheduler_policy = VirtualTimeController::VirtualTimePolicy::kAdvance;
  } else if (protocol::Emulation::VirtualTimePolicyEnum::
                 PauseIfNetworkFetchesPending == policy) {
    scheduler_policy =
        VirtualTimeController::VirtualTimePolicy::kDeterministicLoading;
  } else if (protocol::Emulation::VirtualTimePolicyEnum::Realtime == policy) {
    scheduler_policy = VirtualTimeController::VirtualTimePolicy::kRealtime;
  } else {
    DCHECK_EQ(scheduler_policy,
              VirtualTimeController::VirtualTimePolicy::kPause);
    if (virtual_time_budget_ms.has_value()) {
      return protocol::Response::InvalidParams(
          "Can only specify budget for non-Pause policy");
    }
    if (max_virtual_time_task_starvation_count.has_value()) {
      return protocol::Response::InvalidParams(
          "Can only specify starvation count for non-Pause policy");
    }
  }
  // ... rest of method unchanged ...
```

**Step 2: Verify compilation**

Run: `autoninja -C out/Default chrome`
Expected: Full build succeeds

**Step 3: Commit**

```bash
git add third_party/blink/renderer/core/inspector/inspector_emulation_agent.cc
git commit -m "feat(inspector): map 'realtime' policy to kRealtime in EmulationAgent"
```

---

## Task 7: Update ABP Controller to Use Realtime Policy

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc`

**Step 1: Change resume to use "realtime" instead of "advance"**

In `OnDebuggerResumed` and `OnAnimationPlaybackRateResumed`, change:

```cpp
// Before:
params.Set("policy", "advance");

// After:
params.Set("policy", "realtime");
```

**Step 2: Verify compilation**

Run: `autoninja -C out/Default chrome`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): use 'realtime' virtual time policy for smooth resume"
```

---

## Task 8: Add Integration Tests

**Files:**
- Modify: `third_party/blink/renderer/core/scheduler_integration_tests/virtual_time_test.cc`

**Step 1: Add test for kRealtime policy**

Add a new test case:

```cpp
TEST_F(VirtualTimeTest, RealtimePolicyAdvancesWithWallClock) {
  // Enable realtime mode
  GetVirtualTimeController()->SetVirtualTimePolicy(
      VirtualTimeController::VirtualTimePolicy::kRealtime);

  // Get initial virtual time
  String initial_time = ExecuteJavaScript("Date.now().toString()");
  int64_t initial_ms = initial_time.ToInt64();

  // Wait 100ms real time
  base::PlatformThread::Sleep(base::Milliseconds(100));

  // Virtual time should have advanced approximately 100ms
  String after_time = ExecuteJavaScript("Date.now().toString()");
  int64_t after_ms = after_time.ToInt64();

  int64_t elapsed = after_ms - initial_ms;
  // Allow some tolerance (50-150ms)
  EXPECT_GE(elapsed, 50);
  EXPECT_LE(elapsed, 150);
}

TEST_F(VirtualTimeTest, RealtimePolicyDoesNotFastForward) {
  // Schedule a task 5 seconds in the future
  ExecuteJavaScript("window.taskFired = false; setTimeout(() => { window.taskFired = true; }, 5000);");

  // Enable realtime mode
  GetVirtualTimeController()->SetVirtualTimePolicy(
      VirtualTimeController::VirtualTimePolicy::kRealtime);

  // Wait only 100ms real time
  base::PlatformThread::Sleep(base::Milliseconds(100));

  // Task should NOT have fired (kAdvance would have fast-forwarded)
  String result = ExecuteJavaScript("window.taskFired.toString()");
  EXPECT_EQ(result, "false");
}
```

**Step 2: Run tests**

Run: `autoninja -C out/Default blink_unittests && ./out/Default/blink_unittests --gtest_filter="*VirtualTime*"`
Expected: All tests pass

**Step 3: Commit**

```bash
git add third_party/blink/renderer/core/scheduler_integration_tests/virtual_time_test.cc
git commit -m "test(scheduler): add integration tests for kRealtime virtual time policy"
```

---

## Task 9: Manual Testing with ABP

**Step 1: Build Chrome**

Run: `autoninja -C out/Default chrome`

**Step 2: Test with animejs.com**

```bash
# Start Chrome with ABP
./out/Default/Chromium.app/Contents/MacOS/Chromium --enable-abp

# Create tab and navigate
curl -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"https://animejs.com"}'

# Wait for page load, then pause
curl -X POST http://localhost:8222/api/v1/tabs/{TAB_ID}/execution \
  -H "Content-Type: application/json" \
  -d '{"paused": true}'

# Visually confirm animations are frozen

# Resume - animations should continue smoothly (not superspeed)
curl -X POST http://localhost:8222/api/v1/tabs/{TAB_ID}/execution \
  -H "Content-Type: application/json" \
  -d '{"paused": false}'
```

Expected: Animations resume smoothly without fast-forward "superspeed" effect

**Step 3: Final commit**

```bash
git commit --allow-empty -m "docs: complete kRealtime virtual time policy implementation

This implementation adds a new 'realtime' virtual time policy that
advances virtual time in sync with wall-clock time, preventing the
fast-forward 'superspeed' effect when resuming from pause.

Key changes:
- Added VirtualTimePolicy::kRealtime enum value
- Implemented realtime mode in AutoAdvancingVirtualTimeDomain
- Added 'realtime' to CDP Emulation.VirtualTimePolicy
- Updated ABP to use realtime policy on resume
- Added integration tests"
```

---

## Summary

This plan adds a fourth virtual time policy (`kRealtime`) that enables smooth animation resume by:

1. **Tracking wall-clock offset**: When realtime mode starts, we record both the current wall-clock time and virtual time
2. **Limiting advancement**: In `MaybeFastForwardToWakeUp`, we calculate the maximum allowed virtual time based on elapsed wall-clock time
3. **No fast-forward**: Tasks scheduled in the future won't trigger until wall-clock time catches up

The key insight is that `kAdvance` fast-forwards because `MaybeFastForwardToWakeUp` always jumps to the next wake-up time. `kRealtime` adds a check that prevents jumping past the wall-clock-derived limit.
