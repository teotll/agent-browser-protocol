# Configurable Wait Timings

## Problem

The three-phase action wait (pre-network, request tracking, post-network settle) uses hardcoded defaults that can't be tuned without modifying source code. Different environments and use cases need different timing values.

## Design

### Timing Values

| Concept | Default | Chrome flag | Env var | NPM CLI | SDK option |
|---------|---------|-------------|---------|---------|------------|
| Pre-network wait | 250ms | `--abp-min-wait=250` | `ABP_MIN_WAIT` | `--min-wait=250` | `minWait` |
| Request tracking timeout | 1000ms | `--abp-tracking-timeout=1000` | `ABP_TRACKING_TIMEOUT` | `--tracking-timeout=1000` | `trackingTimeout` |
| Post-network settle | 750ms | `--abp-post-settle=750` | `ABP_POST_SETTLE` | `--post-settle=750` | `postSettle` |

All values in milliseconds.

### Priority Order

Highest priority wins:

1. Per-action hardcoded override (scroll=500ms, screenshot=100ms)
2. Launch-time config (Chrome flag > env var > hardcoded default)

No per-request overrides — timing is launch-time only.

### C++ Layer

**abp_switches.h/cc** — Three new switches: `--abp-min-wait`, `--abp-tracking-timeout`, `--abp-post-settle`.

**abp_config.h/cc** — Three new `base::TimeDelta` fields:
- `min_wait_time` (default 250ms)
- `tracking_timeout` (default 1000ms)
- `post_settle_time` (default 750ms)

Resolution order in `AbpConfig`:
1. Set hardcoded defaults
2. Check env vars — override if present
3. Check command-line switches — override if present

**abp_action_context.h** — `Options` struct defaults populated from `AbpConfig` instead of hardcoded values.

**abp_controller.cc** — When building `Options` for an action:
1. Start with `AbpConfig` defaults
2. Apply per-action overrides (scroll=500ms, screenshot=100ms) where they exist

### NPM CLI

Three new flags in `tools/abp-npm/src/bin/abp.ts`:
```
agent-browser-protocol --min-wait=250 --tracking-timeout=1000 --post-settle=750
```

Env var fallbacks: `ABP_MIN_WAIT`, `ABP_TRACKING_TIMEOUT`, `ABP_POST_SETTLE`.

Passed through as Chrome args: `--abp-min-wait`, `--abp-tracking-timeout`, `--abp-post-settle`.

### NPM SDK

New fields in `LaunchOptions` (`tools/abp-npm/src/launch.ts`):
```typescript
export interface LaunchOptions {
  // ... existing fields
  minWait?: number;        // ms, default 250
  trackingTimeout?: number; // ms, default 1000
  postSettle?: number;      // ms, default 750
}
```

Only passed as Chrome args if explicitly set — otherwise the C++ defaults apply.

## Files Changed

- `chrome/browser/abp/abp_switches.h/cc` — new switch constants
- `chrome/browser/abp/abp_config.h/cc` — timing fields, env var + switch resolution
- `chrome/browser/abp/abp_action_context.h` — Options defaults from AbpConfig
- `chrome/browser/abp/abp_controller.cc` — read config, apply to Options
- `tools/abp-npm/src/bin/abp.ts` — CLI flags + env var parsing
- `tools/abp-npm/src/launch.ts` — LaunchOptions fields, pass as Chrome args
