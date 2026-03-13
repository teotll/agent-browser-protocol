# Animation Wait for browser_wait

## Problem

Many websites have intro animations (CSS transitions, JS animation libraries like anime.js, loading sequences) that run for several seconds after initial navigation before interactive elements become visible. ABP's `browser_wait` currently only waits for network settling, so the agent captures screenshots before animations complete and attempts to interact with elements that aren't yet visible.

Example: https://www.deveillance.com/ animates for 5+ seconds before navigation items appear.

## Design

### Approach

Add an `animation` boolean parameter to `browser_wait`. When set, the action waiter enforces a **5-second minimum page execution time** running in parallel with network tracking. The action completes when **both** network settling AND the 5s animation timer have elapsed — whichever takes longer.

Agent-triggered: the agent explicitly opts in via the parameter. No browser-side animation detection.

### API Surface

**MCP tool (`browser_wait`)** — new optional boolean parameter:

```
animation (boolean, optional): "Guarantees 5s of page execution for animations to play forward. Set true on first wait after navigation."
```

**REST endpoint** — `/api/v1/tabs/{id}/wait_for_network` accepts `"animation": true` in the JSON body.

### Implementation

**4 files, ~30 lines of logic.**

#### 1. `abp_action_context.h` — Options struct

Add field:

```cpp
base::TimeDelta animation_wait_time;  // Zero = no animation wait
```

#### 2. `abp_controller.h` — ActionCompleteWaiter struct

Add fields:

```cpp
bool animation_time_elapsed = false;
bool animation_timer_started = false;
```

Update `IsComplete()`:

```cpp
if (wait_type == "action_complete") {
  bool animation_ok = !animation_timer_started || animation_time_elapsed;
  return load_fired && dom_content_loaded_fired &&
         first_paint_fired && min_time_elapsed &&
         tracked_requests_resolved && post_tracking_settled && animation_ok;
}
```

When `animation_timer_started` is false (normal calls without `animation: true`), `animation_ok` is always true — zero behavior change for existing callers.

#### 3. `abp_controller.cc` — WaitForNetwork + WaitForActionComplete + DoWaitUntil

**WaitForNetwork**: Read `"animation"` from params. If true, set `options.animation_wait_time = base::Seconds(5)`.

**WaitForActionComplete**: Add `base::TimeDelta animation_wait_time` parameter (default zero). If non-zero, start the animation timer **immediately** when the waiter is created (not gated on page load events like `min_wait_time`). The timer sets `animation_time_elapsed = true` and calls `CheckActionComplete()` when it fires. Set `animation_timer_started = true`.

**DoWaitUntil** (`abp_action_context.cc`): Forward `options_.animation_wait_time` to the new `WaitForActionComplete` parameter.

#### Plumbing path

```
WaitForNetwork (reads "animation" from JSON params)
  → Options.animation_wait_time = 5s
    → AbpActionContext::RunWithOptions (stores Options)
      → DoWaitUntil (reads options_.animation_wait_time)
        → WaitForActionComplete(animation_wait_time=5s)
          → starts immediate 5s timer in ActionCompleteWaiter
```

#### Timer semantics

- **Starts immediately** in `WaitForActionComplete`, not gated on load/DCL/first-paint (unlike `min_wait_time`). This ensures the 5s measures total wall time from action start.
- **Not reset on mid-wait `Page.frameNavigated`**: If a client-side redirect occurs 2s into the wait, the timer keeps running. The purpose is "5s total execution time," not "5s per page."
- **Bounded by 10s safety timeout**: `OnWaitTimeout` force-completes the waiter at 10s regardless, so the animation timer can never cause an indefinite hang.

#### 4. `abp_mcp_handler.cc` / `abp_tool_builder.cc` — MCP layer

- Add `animation` boolean parameter to `browser_wait` tool schema in `abp_tool_builder.cc`
- Pass `animation` through in `CallBrowserWait` body dict in `abp_mcp_handler.cc`
- Update `browser_wait` tool description to include animation guidance

### Behavior Summary

| Call | min_wait (150ms) | Network tracking (5s) | Post-settle (750ms) | Animation (5s) |
|------|------------------|-----------------------|---------------------|----------------|
| `browser_wait()` | Yes | Yes | Yes | No |
| `browser_wait(animation=true)` | Yes | Yes | Yes | Yes (parallel) |

Total wall time for `animation=true`: max(network_settle_total, 5s). On fast-loading pages the 5s floor dominates. On slow pages the network tracking dominates — no penalty added.

### Non-goals

- No browser-side animation detection (CSS `getAnimations()`, `requestAnimationFrame` heuristics, etc.)
- No configurable timeout — hardcoded 5s, can add later if needed
- No changes to other endpoints (navigate, click, etc.)
