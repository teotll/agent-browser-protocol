# Configurable Wait Timings Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make the three-phase action wait timings (min_wait, tracking_timeout, post_settle) configurable via Chrome flags, env vars, NPM CLI, and NPM SDK.

**Architecture:** Add three timing fields to `AbpConfig`, resolved from env vars then Chrome flags. `AbpActionContext::Options` defaults read from config instead of hardcoded values. NPM CLI/SDK pass values as Chrome flags.

**Tech Stack:** C++ (Chromium), TypeScript (NPM package)

---

### Task 1: Add Chrome switches

**Files:**
- Modify: `chrome/browser/abp/abp_switches.h`
- Modify: `chrome/browser/abp/abp_switches.cc`

**Step 1: Add switch declarations to header**

In `abp_switches.h`, add these three lines before the closing `}  // namespace`:

```cpp
// Minimum wait time in ms before network snapshot (default: 250)
extern const char kAbpMinWait[];

// Request tracking timeout in ms (default: 1000)
extern const char kAbpTrackingTimeout[];

// Post-network settle time in ms (default: 750)
extern const char kAbpPostSettle[];
```

**Step 2: Add switch definitions to .cc**

In `abp_switches.cc`, add before the closing `}  // namespace`:

```cpp
const char kAbpMinWait[] = "abp-min-wait";
const char kAbpTrackingTimeout[] = "abp-tracking-timeout";
const char kAbpPostSettle[] = "abp-post-settle";
```

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_switches.h chrome/browser/abp/abp_switches.cc
git commit -m "feat: add Chrome switches for configurable wait timings"
```

---

### Task 2: Add timing fields to AbpConfig

**Files:**
- Modify: `chrome/browser/abp/abp_config.h`
- Modify: `chrome/browser/abp/abp_config.cc`

**Step 1: Add timing fields to AbpConfig struct**

In `abp_config.h`, add `#include "base/time/time.h"` to the includes, then add a `TimingConfig` struct inside `AbpConfig` after the `HistoryConfig` block (before `GetDefaults()`):

```cpp
  struct TimingConfig {
    // Phase 1: JS hook window before network snapshot
    base::TimeDelta min_wait = base::Milliseconds(250);
    // Phase 2: How long to track in-flight requests
    base::TimeDelta tracking_timeout = base::Milliseconds(1000);
    // Phase 3: Settle after tracked requests complete
    base::TimeDelta post_settle = base::Milliseconds(750);
  };
  TimingConfig timing;
```

**Step 2: Add env var + switch resolution to LoadAbpConfig**

In `abp_config.cc`, add a helper function inside the anonymous namespace:

```cpp
// Reads a millisecond timing value from env var, then command-line switch.
// Switch takes priority over env var. Returns nullopt if neither is set.
std::optional<base::TimeDelta> ReadTimingMs(const char* env_name,
                                             const char* switch_name) {
  auto* command_line = base::CommandLine::ForCurrentProcess();
  auto env = base::Environment::Create();

  // Check env var first (lower priority)
  std::optional<base::TimeDelta> result;
  std::string env_val;
  if (env->GetVar(env_name, &env_val)) {
    int ms = 0;
    if (base::StringToInt(env_val, &ms) && ms >= 0) {
      result = base::Milliseconds(ms);
    }
  }

  // Switch overrides env var
  if (command_line->HasSwitch(switch_name)) {
    std::string switch_val = command_line->GetSwitchValueASCII(switch_name);
    int ms = 0;
    if (base::StringToInt(switch_val, &ms) && ms >= 0) {
      result = base::Milliseconds(ms);
    }
  }

  return result;
}
```

Add `#include <optional>` and `#include "base/strings/string_number_conversions.h"` to the includes.

Then, at the **end** of `LoadAbpConfig()`, just before the final `return` statements (there are several return paths), add timing resolution. The cleanest place is to add a helper call that applies timing to any config. Add this function after `LoadAbpConfigFromFile`:

```cpp
void ApplyTimingOverrides(AbpConfig& config) {
  if (auto v = ReadTimingMs("ABP_MIN_WAIT", switches::kAbpMinWait))
    config.timing.min_wait = *v;
  if (auto v = ReadTimingMs("ABP_TRACKING_TIMEOUT", switches::kAbpTrackingTimeout))
    config.timing.tracking_timeout = *v;
  if (auto v = ReadTimingMs("ABP_POST_SETTLE", switches::kAbpPostSettle))
    config.timing.post_settle = *v;
}
```

Then call `ApplyTimingOverrides(config)` before each `return config;` in `LoadAbpConfig()`. There are 4 return paths (lines 186, 200, 207, 209). Add the call before each one:

```cpp
  ApplyTimingOverrides(config);
  return config;
```

**Step 3: Verify build**

```bash
cd /Users/hanwang/src/src && autoninja -C out/Default chrome/browser/abp:abp
```

Expected: builds successfully.

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_config.h chrome/browser/abp/abp_config.cc
git commit -m "feat: add timing config fields with env var and switch resolution"
```

---

### Task 3: Wire AbpConfig timing into AbpActionContext defaults

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.h`
- Modify: `chrome/browser/abp/abp_http_server.cc`
- Modify: `chrome/browser/abp/abp_controller.h`

**Step 1: Change Options defaults to new values**

In `abp_action_context.h`, update the three defaults in the `Options` struct:

```cpp
    // Phase 1: JS hook window. Minimum time before snapshot + completion check.
    base::TimeDelta min_wait_time = base::Milliseconds(250);

    // Phase 2: How long to wait for snapshotted requests to complete.
    base::TimeDelta request_tracking_timeout = base::Milliseconds(1000);

    // Phase 3: Settle time after tracked requests complete.
    // Lets the page process network responses and update DOM.
    base::TimeDelta post_tracking_settle_time = base::Milliseconds(750);
```

These are now the fallback defaults matching the design. The actual runtime defaults come from AbpConfig.

**Step 2: Pass config to AbpController**

In `abp_http_server.cc`, after line 107 (`controller_ = std::make_unique<AbpController>();`), pass the timing config:

```cpp
  controller_->SetTimingConfig(config.timing);
```

**Step 3: Add timing config storage to AbpController**

In `abp_controller.h`, add the include and storage. Add `#include "chrome/browser/abp/abp_config.h"` to includes, then add these members and method in the controller class:

```cpp
  // Set timing configuration (called once at startup)
  void SetTimingConfig(const AbpConfig::TimingConfig& timing);

  // Get default action options with timing from config
  AbpActionContext::Options GetDefaultActionOptions() const;
```

And as a private member:

```cpp
  AbpConfig::TimingConfig timing_config_;
```

**Step 4: Implement in abp_controller.cc**

Add the two method implementations:

```cpp
void AbpController::SetTimingConfig(const AbpConfig::TimingConfig& timing) {
  timing_config_ = timing;
}

AbpActionContext::Options AbpController::GetDefaultActionOptions() const {
  AbpActionContext::Options options;
  options.min_wait_time = timing_config_.min_wait;
  options.request_tracking_timeout = timing_config_.tracking_timeout;
  options.post_tracking_settle_time = timing_config_.post_settle;
  return options;
}
```

**Step 5: Update WaitForActionComplete default parameters**

In `abp_controller.h`, update the `WaitForActionComplete` signature to use the new defaults:

```cpp
  void WaitForActionComplete(
      const std::string& tab_id,
      base::OnceClosure on_complete,
      base::TimeDelta min_wait_time = base::Milliseconds(250),
      base::TimeDelta request_tracking_timeout = base::Seconds(1),
      base::TimeDelta post_tracking_settle_time = base::Milliseconds(750));
```

**Step 6: Verify build**

```bash
cd /Users/hanwang/src/src && autoninja -C out/Default chrome/browser/abp:abp
```

**Step 7: Commit**

```bash
git add chrome/browser/abp/abp_action_context.h chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc chrome/browser/abp/abp_http_server.cc
git commit -m "feat: wire AbpConfig timing into controller and action context"
```

---

### Task 4: Use GetDefaultActionOptions at all call sites

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc`
- Modify: `chrome/browser/abp/abp_input_dispatcher.cc`

**Step 1: Replace default-constructed Options with GetDefaultActionOptions**

Every place that creates `AbpActionContext::Options options;` and calls `RunWithOptions` needs to start from `GetDefaultActionOptions()` instead. Find and replace each occurrence:

In `abp_controller.cc`, at each call site that creates `AbpActionContext::Options options;`:

- **Navigate** (~line 2459): Change to `auto options = GetDefaultActionOptions();`
- **Reload** (~line 2495): Change to `auto options = GetDefaultActionOptions();`
- **Back** (~line 2531): Change to `auto options = GetDefaultActionOptions();`
- **Forward** (~line 2572): Change to `auto options = GetDefaultActionOptions();`
- **Screenshot** (~line 2609): Change to `auto options = GetDefaultActionOptions();` (keep the `options.min_wait_time = base::Milliseconds(100);` override)
- **File upload** (~line 5593): Change to `auto options = GetDefaultActionOptions();` (keep the `opts.min_wait_time = base::Milliseconds(500);` and `opts.request_tracking_timeout = base::Seconds(60);` overrides)

In `abp_input_dispatcher.cc`, at the scroll call site (~line 651):
- Change `AbpActionContext::Options options;` to `auto options = controller_->GetDefaultActionOptions();`
- Keep the `options.min_wait_time = base::Milliseconds(500);` override

Also update calls that use `AbpActionContext::Run()` (without options). These implicitly use `Options()` which now has the correct defaults from the struct, so they're fine as-is — the struct defaults match the new values (250/1000/750). But for correctness, any `Run()` call sites that should respect per-launch config need to switch to `RunWithOptions` with `GetDefaultActionOptions()`.

Search for `AbpActionContext::Run(` (not `RunWithOptions`) in `abp_controller.cc` and convert them:

```cpp
// Before:
AbpActionContext::Run(this, tab_id, "click", params, ...);

// After:
AbpActionContext::RunWithOptions(this, tab_id, "click", params,
                                  GetDefaultActionOptions(), ...);
```

**Step 2: Verify build**

```bash
cd /Users/hanwang/src/src && autoninja -C out/Default chrome/browser/abp:abp
```

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc chrome/browser/abp/abp_input_dispatcher.cc
git commit -m "feat: use GetDefaultActionOptions at all action call sites"
```

---

### Task 5: Add NPM CLI flags and env vars

**Files:**
- Modify: `tools/abp-npm/src/bin/abp.ts`

**Step 1: Add fields to ParsedArgs interface**

```typescript
interface ParsedArgs {
  port: number;
  headless: boolean;
  verbose: boolean;
  sessionDir?: string;
  minWait?: number;
  trackingTimeout?: number;
  postSettle?: number;
  chromeArgs: string[];
}
```

**Step 2: Add env var defaults and CLI parsing**

In `parseArgs`, after the existing env var defaults (line 18), add:

```typescript
  let minWait: number | undefined = process.env.ABP_MIN_WAIT
    ? parseInt(process.env.ABP_MIN_WAIT, 10)
    : undefined;
  let trackingTimeout: number | undefined = process.env.ABP_TRACKING_TIMEOUT
    ? parseInt(process.env.ABP_TRACKING_TIMEOUT, 10)
    : undefined;
  let postSettle: number | undefined = process.env.ABP_POST_SETTLE
    ? parseInt(process.env.ABP_POST_SETTLE, 10)
    : undefined;
```

In the argument parsing loop, add cases before the `--help` check:

```typescript
    } else if (argv[i] === "--min-wait" && i + 1 < argv.length) {
      minWait = parseInt(argv[i + 1], 10);
      i++;
    } else if (argv[i].startsWith("--min-wait=")) {
      minWait = parseInt(argv[i].split("=")[1], 10);
    } else if (argv[i] === "--tracking-timeout" && i + 1 < argv.length) {
      trackingTimeout = parseInt(argv[i + 1], 10);
      i++;
    } else if (argv[i].startsWith("--tracking-timeout=")) {
      trackingTimeout = parseInt(argv[i].split("=")[1], 10);
    } else if (argv[i] === "--post-settle" && i + 1 < argv.length) {
      postSettle = parseInt(argv[i + 1], 10);
      i++;
    } else if (argv[i].startsWith("--post-settle=")) {
      postSettle = parseInt(argv[i].split("=")[1], 10);
    }
```

Update the return statement:

```typescript
  return { port, headless, verbose, sessionDir, minWait, trackingTimeout, postSettle, chromeArgs };
```

**Step 3: Update help text**

Add to the Options section of the help string:

```
  --min-wait <ms>        Pre-network settlement wait in ms (default: 250)
  --tracking-timeout <ms> Request tracking timeout in ms (default: 1000)
  --post-settle <ms>     Post-network settle time in ms (default: 750)
```

Add to the Environment Variables section:

```
  ABP_MIN_WAIT           Pre-network settlement wait in ms
  ABP_TRACKING_TIMEOUT   Request tracking timeout in ms
  ABP_POST_SETTLE        Post-network settle time in ms
```

**Step 4: Pass to launch()**

In `main()`, update the launch call:

```typescript
  const { port, headless, verbose, sessionDir, minWait, trackingTimeout, postSettle, chromeArgs } = parseArgs(process.argv);

  const browser = await launch({ port, headless, verbose, sessionDir, minWait, trackingTimeout, postSettle, args: chromeArgs });
```

**Step 5: Commit**

```bash
git add tools/abp-npm/src/bin/abp.ts
git commit -m "feat: add --min-wait, --tracking-timeout, --post-settle CLI flags"
```

---

### Task 6: Add SDK LaunchOptions and pass as Chrome flags

**Files:**
- Modify: `tools/abp-npm/src/launch.ts`

**Step 1: Add fields to LaunchOptions**

```typescript
export interface LaunchOptions {
  port?: number;
  sessionDir?: string;
  executablePath?: string;
  headless?: boolean;
  /** Window size as [width, height]. Default: [1280, 800]. */
  windowSize?: [number, number];
  /** Pipe browser stdout/stderr to the parent process stderr. */
  verbose?: boolean;
  /** Pre-network settlement wait in ms. Default: 250. */
  minWait?: number;
  /** Request tracking timeout in ms. Default: 1000. */
  trackingTimeout?: number;
  /** Post-network settle time in ms. Default: 750. */
  postSettle?: number;
  args?: string[];
}
```

**Step 2: Destructure and pass as Chrome flags**

In the `launch()` function, update the destructuring:

```typescript
  const {
    port = 8222,
    sessionDir,
    executablePath,
    headless = false,
    windowSize,
    verbose = false,
    minWait,
    trackingTimeout,
    postSettle,
    args = [],
  } = options;
```

After the `if (headless)` block and before the `launchArgs.push(...args.map(...))` line, add:

```typescript
  if (minWait !== undefined) {
    launchArgs.push(`--abp-min-wait=${minWait}`);
  }

  if (trackingTimeout !== undefined) {
    launchArgs.push(`--abp-tracking-timeout=${trackingTimeout}`);
  }

  if (postSettle !== undefined) {
    launchArgs.push(`--abp-post-settle=${postSettle}`);
  }
```

**Step 3: Commit**

```bash
git add tools/abp-npm/src/launch.ts
git commit -m "feat: add timing options to LaunchOptions SDK interface"
```

---

### Task 7: Build and manual test

**Step 1: Build C++**

```bash
cd /Users/hanwang/src/src && autoninja -C out/Default chrome
```

Expected: builds successfully.

**Step 2: Test Chrome flags**

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-min-wait=500 --abp-tracking-timeout=2000 --abp-post-settle=100 --no-first-run
```

Verify the browser starts. Create a tab, perform a click action, and observe timing in action profiling logs (VLOG).

**Step 3: Build NPM package**

```bash
cd tools/abp-npm && npm run build
```

**Step 4: Test NPM CLI flags**

```bash
npx agent-browser-protocol --min-wait=500 --tracking-timeout=2000 --post-settle=100
```

**Step 5: Test env vars**

```bash
ABP_MIN_WAIT=500 ABP_POST_SETTLE=100 npx agent-browser-protocol
```

**Step 6: Commit (if any fixes needed)**

```bash
git add -A && git commit -m "fix: address issues found during manual testing"
```
