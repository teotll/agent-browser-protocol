# ABP Action Lifecycle Latency Profiling

**Test scenario**: newtab → amazon.com (navigate + click)
**Build**: Debug component build, macOS arm64

## Run 3 (2026-02-17): JPEG + Compositor Scroll + Parallel Pause + Markup Opt-in

Optimizations applied since Run 2:
1. **JPEG encoding** (7ms vs 1,300ms WebP) — 190x faster
2. **Compositor-based scroll position** (0ms vs 94-1,232ms Runtime.evaluate)
3. **Parallel pause** (Debugger + VT policy in parallel, runs async after response)
4. **Markup opt-in** (no markup inject unless requested — saves 334-1,310ms)
5. **Faster Retina downscale** (RESIZE_LANCZOS2: 95ms vs 217ms RESIZE_GOOD)

### Three-Run Comparison

| Stage | Run 1 (baseline) | Run 2 (-ForceRedraw) | Run 3 (current) |
|-------|-------------------|-------------------|----------------|
| before_ss | 1,584ms | 1,585ms | **183ms** |
| resume | 3,006ms | 48ms | **412ms** |
| action | 969ms | 1,147ms | **1,182ms** |
| wait | 501ms | 541ms | **501ms** |
| scroll_pos | 94ms | 239ms | **0ms** |
| after_ss | 3,606ms | 3,489ms | **1,061ms** |
| pause | 196ms | 181ms | **async** |
| **total** | **9,958ms** | **7,233ms** | **3,341ms** |

**Click: 10.0s → 7.2s → 3.3s** (3x improvement from baseline)

### Run 3 Top Offenders (click action)

| Offender | Duration | % of total |
|----------|----------|-----------|
| Action dispatch + JS processing | 1,182ms | 35% |
| After-screenshot ForceRedraw+CA+Grab | 916ms | 27% |
| Wait (min 500ms) | 501ms | 15% |
| Resume (VT realtime switch) | 412ms | 12% |
| Before-screenshot (grab 42ms + encode 140ms) | 183ms | 5% |
| After-screenshot encode (JPEG 142ms) | 142ms | 4% |
| Scroll position (compositor) | 0ms | 0% |
| Pause (async, not on critical path) | — | — |

---

## Instrumentation

Profiling timestamps added at every stage boundary in `abp_action_context.cc` and sub-stage timing in `abp_controller.cc`. All output uses `LOG(INFO)` with prefix `ABP PROFILE` for easy filtering:

```bash
grep "ABP PROFILE" /tmp/abp_profile.log
```

## Summary: Navigate Action (17.5s total)

```
before_ss=1306ms | resume=3018ms | action=72ms | wait=7048ms | scroll_pos=1044ms | after_ss=4523ms | pause=450ms
```

## Summary: Click Action (10.0s total)

```
before_ss=1584ms | resume=3006ms | action=969ms | wait=501ms | scroll_pos=94ms | after_ss=3606ms | pause=196ms
```

## Detailed Breakdown

### Navigate (newtab → amazon.com): 17.5s

| Stage | Duration | Substage Breakdown |
|-------|----------|-------------------|
| **before_ss** | **1,306ms** | GrabViewSnapshot: 82ms, WebP encode: 982ms |
| **resume** | **3,018ms** | Debugger.resume: 8ms, **ForceRedraw: 3,001ms (TIMEOUT)**, setVirtualTimePolicy: 1ms |
| **action** | **72ms** | Navigation dispatch |
| **wait** | **7,048ms** | Page load + 10s min_wait (completed early at 7s) |
| **scroll_pos** | **1,044ms** | GetScrollPosition Runtime.evaluate |
| **after_ss** | **4,523ms** | Markup inject: 1,310ms, ForceRedraw+CA+Grab: 1,540ms, WebP encode: 1,380ms |
| **pause** | **450ms** | setVirtualTimePolicy: 172ms, Debugger.enable: 192ms, Debugger.pause: 80ms, paused event: 5ms |

### Click (on amazon.com): 10.0s

| Stage | Duration | Substage Breakdown |
|-------|----------|-------------------|
| **before_ss** | **1,584ms** | GrabViewSnapshot: 40ms, WebP encode: 1,288ms |
| **resume** | **3,006ms** | Debugger.resume: 1ms, **ForceRedraw: 3,001ms (TIMEOUT)**, setVirtualTimePolicy: 2ms |
| **action** | **969ms** | Click dispatch + JS processing |
| **wait** | **501ms** | Min wait (page already loaded) |
| **scroll_pos** | **94ms** | GetScrollPosition |
| **after_ss** | **3,606ms** | Markup inject: 692ms, ForceRedraw: 1,058ms, CA wait: 167ms, WebP encode: 1,380ms |
| **pause** | **196ms** | setVirtualTimePolicy: 88ms, Debugger.enable: 75ms, Debugger.pause: 24ms, paused event: 7ms |

## Top Latency Offenders

### 1. ForceRedraw during resume: 3,001ms (TIMEOUT) — both actions

The two-phase resume calls `ForceRedrawWithCallback` while virtual time fences are still up. This is designed to be fast (main thread free, no blink task flood), but hits the 3s safety timeout on both the navigate and click actions. The ForceRedraw callback never fires; the timeout falls through to `SwitchToRealtimeVirtualTime`.

**Evidence** (navigate):
```
164330.170023 [resume] ForceRedraw SEND (fences up) renderer_initialized=1
164333.171553 [resume] setVirtualTimePolicy(realtime) SEND  ← 3001ms later, timeout path
```

**Evidence** (click):
```
164359.329811 [resume] ForceRedraw SEND (fences up) renderer_initialized=1
164402.331125 [resume] setVirtualTimePolicy(realtime) SEND  ← 3001ms later, timeout path
```

The timeout log (`ForceRedrawThenResumeVirtualTime timed out (3s)`) appears in the non-PROFILE log lines, confirming this is the timeout path.

**Root cause hypothesis**: With virtual time fences up, `BeginMainFrame` can run but the compositor's `cc::Scheduler` delay calculation may still be affected by the virtual-time/real-time gap (frozen virtual `Now()` vs real-time `BeginFrameArgs`). The BeginFrameArgs translation fix in `cc/scheduler/scheduler.cc` should handle this, but the fenced state may prevent the compositor from seeing BeginFrameArgs at all if the frame source is paused.

### 2. Screenshot pipeline: ~1,500ms per screenshot

Two screenshots per action (before + after) = ~3,000ms. Pipeline sub-stage breakdown:

**Evidence** (all 4 screenshots from navigate + click on amazon.com):
```
[screenshot] pipeline raw=2560x1488 scaled=1280x744 format=webp quality=80 encoded_bytes=19348
  | toSkBitmap=36ms | scale=200ms | encode=995ms  | base64=0ms | total=1231ms  (before, newtab)

[screenshot] pipeline raw=2560x1600 scaled=1280x800 format=webp quality=80 encoded_bytes=92902
  | toSkBitmap=39ms | scale=218ms | encode=1381ms | base64=0ms | total=1639ms  (after, amazon nav)

[screenshot] pipeline raw=2560x1600 scaled=1280x800 format=webp quality=80 encoded_bytes=79090
  | toSkBitmap=37ms | scale=214ms | encode=1287ms | base64=0ms | total=1539ms  (before, amazon click)

[screenshot] pipeline raw=2560x1600 scaled=1280x800 format=webp quality=80 encoded_bytes=93620
  | toSkBitmap=37ms | scale=220ms | encode=1379ms | base64=0ms | total=1637ms  (after, amazon click)
```

**Sub-stage breakdown** (1280x800 amazon page, typical):

| Sub-stage | Time | Notes |
|-----------|------|-------|
| `toSkBitmap` | **37ms** | Convert `gfx::Image` to `SkBitmap` |
| `scale` | **217ms** | Downscale 2560x1600 → 1280x800 (2x Retina) via `skia::ImageOperations::Resize(RESIZE_GOOD)` |
| `encode` | **1,349ms** | WebP encode at quality 80 (debug build, unoptimized) |
| `base64` | **0ms** | `base::Base64Encode` on ~90KB payload |
| `disk_write` | **2-25ms** | `base::WriteFile` on background thread (non-blocking) |
| **total** | **~1,600ms** | |

**Key findings**:
- **WebP encode dominates**: 1,349ms / 1,600ms = **84%** of the pipeline. Debug build; expect 5-10x faster in release.
- **Retina downscale costs 217ms**: `RESIZE_GOOD` (Lanczos3) on 2560x1600 → 1280x800.
- **Base64 is free**: 0ms for ~90KB → ~125KB.
- **Disk write is non-blocking**: Already on `base::ThreadPool`, only 2-25ms.

**Optimization ideas**:
- Switch to JPEG (faster encoder than WebP, especially in debug)
- Use `RESIZE_LANCZOS2` or `RESIZE_BOX` instead of `RESIZE_GOOD` (Lanczos3) for ~2x scale speedup
- Encode on a background thread (currently blocks UI thread)
- Skip Retina downscale: encode at 2x resolution (avoids 217ms scale but larger encode)

### 3. Markup CSS injection via Runtime.evaluate: 700-1,300ms

Injecting the overlay CSS for interactive element markup is slow on amazon.com.

**Evidence**:
```
[screenshot] markup inject SEND → DONE elapsed=1310ms  ← navigate
[screenshot] markup inject SEND → DONE elapsed=692ms   ← click
```

The markup injection script queries the DOM for all clickable/typeable/scrollable elements and builds CSS overlays. On a complex page like amazon.com with thousands of elements, this is expensive.

### 4. After-screenshot ForceRedraw: 1,058ms

The ForceRedraw for the after-screenshot (with virtual time running, main thread busy) takes over 1s.

**Evidence** (click):
```
164404.591465 [screenshot] ForceRedraw SEND
164405.649635 [screenshot] ForceRedraw DONE elapsed=1058ms
164405.649660 [screenshot] CoreAnimation wait 167ms
164405.860158 [screenshot] GrabViewSnapshot success total_from_forceredraw=1268ms
```

Breakdown: ForceRedraw 1,058ms + CoreAnimation 167ms + GrabViewSnapshot ~43ms = 1,268ms total.

### 5. GetScrollPosition: 94-1,044ms

**Evidence**:
```
[scroll] GetScrollPosition SEND → DONE elapsed=1044ms  ← navigate (JS heavy during load)
[scroll] GetScrollPosition SEND → DONE elapsed=94ms    ← click (page idle)
```

The 1,044ms for navigate is because `Runtime.evaluate` is competing with amazon.com's heavy JS execution right after page load.

### 6. Pause sequence CDP round-trips: 196-450ms

Multiple sequential CDP commands: `setVirtualTimePolicy(pause)` → `Debugger.enable` → `Debugger.pause` → wait for `Debugger.paused` event.

**Evidence** (click):
```
setVirtualTimePolicy(pause): 88ms
Debugger.enable: 75ms
Debugger.pause: 24ms
Debugger.paused event wait: 7ms
Total: 196ms
```

**Evidence** (navigate — slower due to heavy page):
```
setVirtualTimePolicy(pause): 172ms
Debugger.enable: 192ms
Debugger.pause: 80ms
Debugger.paused event wait: 5ms
Total: 450ms
```

## Optimization Opportunities (ranked by impact)

### High Impact

1. **Fix ForceRedraw timeout during resume** (-3,000ms per action)
   - The ForceRedraw with fences up never completes. Investigate whether the compositor can actually produce a frame when virtual time is paused. If not, skip this ForceRedraw entirely — it was added for visual update but may not be needed since the after-screenshot ForceRedraw handles that.

2. **Move WebP encoding off UI thread** (-1,300ms per screenshot, -2,600ms per action)
   - Encode on `base::ThreadPool` instead of blocking the UI thread. The bitmap can be copied and encoded asynchronously.

3. **Switch to JPEG encoding** (estimated 5-10x faster than WebP)
   - JPEG encoding is much simpler than WebP. At quality 80, visual quality is comparable for screenshots.

### Medium Impact

4. **Optimize markup injection script** (-500-1,000ms)
   - Cache element queries, reduce DOM traversal scope, or use a more efficient overlay method.

5. **Pipeline before-screenshot encode with resume** (-1,000ms)
   - Currently: capture → encode → resume. Could: capture → start encode on bg thread → resume immediately.

6. **Reduce min_wait_time for click** (currently 500ms, could be 100-200ms)

### Low Impact

7. **Batch pause CDP commands** (-50-100ms)
   - `Debugger.enable` + `Debugger.pause` could potentially be batched.

8. **Skip before-screenshot for non-first actions** (-1,300ms when applicable)
   - The before-screenshot of action N is the same as the after-screenshot of action N-1.

## Raw Log Evidence

### Navigate Action (full sequence)

```
164328.834534 [pause] Debugger.enable SEND           ← initial pause (from execution enable)
164328.835093 [pause] Debugger.enable DONE elapsed=0ms
164328.835142 [pause] Debugger.pause SEND
164328.835581 [pause] Debugger.pause DONE elapsed=0ms
164328.836730 [pause] Debugger.paused EVENT wait=1ms
164328.848516 [before_ss] GrabViewSnapshot SEND
164328.931349 [before_ss] GrabViewSnapshot DONE elapsed=82ms empty=0
164330.153639 [screenshot] encode webp 1280x744 elapsed=982ms
164330.154780 [resume] Debugger.resume SEND
164330.162934 [resume] Debugger.resume DONE elapsed=8ms success=1
164330.170023 [resume] ForceRedraw SEND (fences up) renderer_initialized=1
164333.171553 [resume] setVirtualTimePolicy(realtime) SEND        ← 3s timeout hit
164333.173495 [resume] setVirtualTimePolicy(realtime) DONE elapsed=1ms
164340.293749 [wait] complete elapsed=7013ms load=1 dcl=1 paint=1
164340.293853 [scroll] GetScrollPosition SEND
164341.338589 [scroll] GetScrollPosition DONE elapsed=1044ms
164341.338957 [screenshot] markup inject SEND
164342.649264 [screenshot] markup inject DONE elapsed=1310ms
164342.649357 [screenshot] ForceRedraw SEND
164344.189531 [screenshot] GrabViewSnapshot success total_from_forceredraw=1540ms retry=0
164345.828621 [screenshot] encode webp 1280x800 elapsed=1380ms
164345.861985 [pause] setVirtualTimePolicy(pause) SEND
164346.034642 [pause] setVirtualTimePolicy(pause) DONE elapsed=172ms
164346.034741 [pause] Debugger.enable SEND
164346.227210 [pause] Debugger.enable DONE elapsed=192ms
164346.227276 [pause] Debugger.pause SEND
164346.307553 [pause] Debugger.pause DONE elapsed=80ms
164346.312726 [pause] Debugger.paused EVENT wait=5ms
164346.312793 PROFILE [navigate] total=17464ms | before_ss=1306ms | resume=3018ms | action=72ms | wait=7048ms | scroll_pos=1044ms | after_ss=4523ms | pause=450ms
```

### Click Action (full sequence)

```
164357.742630 [before_ss] GrabViewSnapshot SEND
164357.783055 [before_ss] GrabViewSnapshot DONE elapsed=40ms empty=0
164359.326037 [screenshot] encode webp 1280x800 elapsed=1288ms
164359.327306 [resume] Debugger.resume SEND
164359.328395 [resume] Debugger.resume DONE elapsed=1ms success=1
164359.329811 [resume] ForceRedraw SEND (fences up) renderer_initialized=1
164402.331125 [resume] setVirtualTimePolicy(realtime) SEND        ← 3s timeout hit
164402.333335 [resume] setVirtualTimePolicy(realtime) DONE elapsed=2ms
164403.803923 [wait] complete elapsed=501ms load=1 dcl=1 paint=1
164403.804015 [scroll] GetScrollPosition SEND
164403.898720 [scroll] GetScrollPosition DONE elapsed=94ms
164403.898994 [screenshot] markup inject SEND
164404.591365 [screenshot] markup inject DONE elapsed=692ms
164404.591465 [screenshot] ForceRedraw SEND
164405.649635 [screenshot] ForceRedraw DONE elapsed=1058ms
164405.649660 [screenshot] CoreAnimation wait 167ms
164405.860158 [screenshot] GrabViewSnapshot success total_from_forceredraw=1268ms retry=0
164407.496524 [screenshot] encode webp 1280x800 elapsed=1380ms
164407.505065 [pause] setVirtualTimePolicy(pause) SEND
164407.593861 [pause] setVirtualTimePolicy(pause) DONE elapsed=88ms
164407.593967 [pause] Debugger.enable SEND
164407.669093 [pause] Debugger.enable DONE elapsed=75ms
164407.669159 [pause] Debugger.pause SEND
164407.693935 [pause] Debugger.pause DONE elapsed=24ms
164407.701019 [pause] Debugger.paused EVENT wait=7ms
164407.701053 PROFILE [click] total=9958ms | before_ss=1584ms | resume=3006ms | action=969ms | wait=501ms | scroll_pos=94ms | after_ss=3606ms | pause=196ms
```

## Time Budget: Where 10s Goes (Click on Amazon)

```
                    0s     1s     2s     3s     4s     5s     6s     7s     8s     9s    10s
                    |------|------|------|------|------|------|------|------|------|------|
before_ss grab      |==|                                                                     40ms
before_ss encode    |  |========================|                                           1288ms
resume dbg.resume   |                           |                                              1ms
resume ForceRedraw  |                           |============================|              3001ms TIMEOUT
resume VT realtime  |                                                        |                 2ms
action (click)      |                                                        |==========|    969ms
wait (min 500ms)    |                                                                  |==|  501ms
scroll position     |                                                                     |  94ms
after_ss markup     |                                                                     |=======|       692ms
after_ss ForceRedraw|                                                                             |=======| 1058ms
after_ss CA wait    |                                                                                     || 167ms
after_ss grab       |                                                                                      | 43ms
after_ss encode     |                                                                                      |=====| 1380ms
pause (all)         |                                                                                            || 196ms
```

## Raw Log Evidence (After Optimization — Run 2)

### Navigate Action (16.4s)

```
181028.357402 [pause] Debugger.enable SEND           ← initial pause
181028.378622 [pause] Debugger.enable DONE elapsed=21ms
181028.378706 [pause] Debugger.pause SEND
181028.395352 [pause] Debugger.pause DONE elapsed=16ms
181028.401401 [pause] Debugger.paused EVENT wait=5ms
181028.409729 [before_ss] GrabViewSnapshot SEND
181028.503095 [before_ss] GrabViewSnapshot DONE elapsed=93ms
181029.740358 [screenshot] encode webp 1280x744 elapsed=998ms
181029.741002 [resume] Debugger.resume SEND
181029.743829 [resume] Debugger.resume DONE elapsed=2ms
181029.752893 [resume] setVirtualTimePolicy(realtime) SEND      ← NO ForceRedraw, straight to VT
181029.760618 [resume] setVirtualTimePolicy(realtime) DONE elapsed=7ms
181038.829303 [wait] complete elapsed=8967ms load=1 dcl=1 paint=1
181038.829396 [scroll] GetScrollPosition SEND
181040.061654 [scroll] GetScrollPosition DONE elapsed=1232ms
181040.062031 [screenshot] markup inject SEND
181040.833876 [screenshot] markup inject DONE elapsed=771ms
181040.833975 [screenshot] ForceRedraw SEND
181042.376817 [screenshot] GrabViewSnapshot success total_from_forceredraw=1542ms retry=0
181044.014934 [screenshot] encode webp 1280x800 elapsed=1379ms
181044.043212 [pause] setVirtualTimePolicy(pause) SEND
181044.571146 [pause] setVirtualTimePolicy(pause) DONE elapsed=527ms
181044.571260 [pause] Debugger.enable SEND
181044.793243 [pause] Debugger.enable DONE elapsed=221ms
181044.793323 [pause] Debugger.pause SEND
181044.798755 [pause] Debugger.pause DONE elapsed=5ms
181044.800060 [pause] Debugger.paused EVENT wait=1ms
181044.800098 PROFILE [navigate] total=16390ms | before_ss=1331ms | resume=20ms | action=53ms | wait=9014ms | scroll_pos=1232ms | after_ss=3981ms | pause=756ms
```

### Click Action (7.2s)

```
181044.823887 [before_ss] GrabViewSnapshot SEND
181044.864590 [before_ss] GrabViewSnapshot DONE elapsed=40ms
181046.406729 [screenshot] encode webp 1280x800 elapsed=1288ms
181046.409682 [resume] Debugger.resume SEND
181046.411070 [resume] Debugger.resume DONE elapsed=1ms
181046.416225 [resume] setVirtualTimePolicy(realtime) SEND
181046.458453 [resume] setVirtualTimePolicy(realtime) DONE elapsed=38ms
181048.147247 [wait] complete elapsed=541ms load=1 dcl=1 paint=1
181048.147336 [scroll] GetScrollPosition SEND
181048.386338 [scroll] GetScrollPosition DONE elapsed=238ms
181048.386667 [screenshot] markup inject SEND
181048.721592 [screenshot] markup inject DONE elapsed=334ms
181048.721702 [screenshot] ForceRedraw SEND
181050.017661 [screenshot] ForceRedraw DONE elapsed=1295ms
181050.017684 [screenshot] CoreAnimation wait 167ms
181050.228616 [screenshot] GrabViewSnapshot success total_from_forceredraw=1506ms retry=0
181051.862570 [screenshot] encode webp 1280x800 elapsed=1377ms
181051.875938 [pause] setVirtualTimePolicy(pause) SEND
181051.974347 [pause] setVirtualTimePolicy(pause) DONE elapsed=98ms
181051.974449 [pause] Debugger.enable SEND
181052.048680 [pause] Debugger.enable DONE elapsed=74ms
181052.048748 [pause] Debugger.pause SEND
181052.049206 [pause] Debugger.pause DONE elapsed=0ms
181052.057748 [pause] Debugger.paused EVENT wait=8ms
181052.057797 PROFILE [click] total=7233ms | before_ss=1585ms | resume=48ms | action=1147ms | wait=541ms | scroll_pos=239ms | after_ss=3489ms | pause=181ms
```

### Time Budget: Click on Amazon (Run 2)

```
                    0s     1s     2s     3s     4s     5s     6s     7s
                    |------|------|------|------|------|------|------|
before_ss grab      ||                                                    40ms
before_ss encode    ||======================|                           1288ms
resume (all)        |                       |                             48ms
action (click)      |                       |=================|        1147ms
wait (min 500ms)    |                                         |===|     541ms
scroll position     |                                             |=|   239ms
after_ss markup     |                                               |==| 334ms
after_ss ForceRedraw|                                                  |==================| 1295ms
after_ss CA wait    |                                                                      || 167ms
after_ss grab       |                                                                       | 44ms
after_ss encode     |                                                                       |====================| 1377ms
pause (all)         |                                                                                             || 181ms
```

---

## Run 3 Raw Log Evidence (2026-02-17)

### Navigate Action (9.6s wall clock)

```
013051.806524 [before_ss] GrabViewSnapshot SEND
013051.887376 [before_ss] GrabViewSnapshot DONE elapsed=80ms empty=0
013052.019792 [screenshot] pipeline raw=2560x1488 scaled=1280x744 format=jpeg quality=80 encoded_bytes=40090
  | toSkBitmap=36ms | scale=89ms | encode=6ms | base64=0ms | total=132ms
013052.020822 [screenshot] disk_write elapsed=0ms
013059.799543 [wait] complete elapsed=7687ms load=1 dcl=1 paint=1
013059.799945 [screenshot] ForceRedraw SEND
013101.084625 [screenshot] ForceRedraw DONE elapsed=1284ms
013101.084646 [screenshot] CoreAnimation wait 167ms
013101.291375 [screenshot] GrabViewSnapshot success total_from_forceredraw=1491ms retry=0
013101.435412 [screenshot] pipeline raw=2560x1600 scaled=1280x800 format=jpeg quality=80 encoded_bytes=135836
  | toSkBitmap=39ms | scale=96ms | encode=7ms | base64=0ms | total=143ms
013101.436363 [screenshot] disk_write elapsed=0ms
013101.436413 PROFILE [navigate] total=9629ms | before_ss=214ms | resume=0ms | action=50ms | wait=7728ms | scroll_pos=0ms | after_ss=1636ms | pause=-1ms
013102.684384 [pause] Debugger.enable SEND           ← async, after response sent
013103.149027 [pause] Debugger.enable DONE elapsed=464ms
013103.149091 [pause] Debugger.pause SEND
013103.149615 [pause] Debugger.pause DONE elapsed=0ms
013103.156638 [pause] Debugger.paused EVENT wait=7ms
```

### Click Action (3.3s wall clock)

```
013108.387331 [before_ss] GrabViewSnapshot SEND
013108.429447 [before_ss] GrabViewSnapshot DONE elapsed=42ms empty=0
013108.569624 [screenshot] pipeline raw=2560x1600 scaled=1280x800 format=jpeg quality=80 encoded_bytes=136116
  | toSkBitmap=37ms | scale=95ms | encode=7ms | base64=0ms | total=140ms
013108.571251 [screenshot] disk_write elapsed=1ms
013108.571295 [resume] Debugger.resume SEND
013108.572689 [resume] Debugger.resume DONE elapsed=1ms success=1
013108.579618 [resume] setVirtualTimePolicy(realtime) SEND
013108.983776 [resume] setVirtualTimePolicy(realtime) DONE elapsed=404ms
013110.667640 [wait] complete elapsed=501ms load=1 dcl=1 paint=1
013110.667975 [screenshot] ForceRedraw SEND
013111.375619 [screenshot] ForceRedraw DONE elapsed=707ms
013111.375642 [screenshot] CoreAnimation wait 167ms
013111.584892 [screenshot] GrabViewSnapshot success total_from_forceredraw=916ms retry=0
013111.727640 [screenshot] pipeline raw=2560x1600 scaled=1280x800 format=jpeg quality=80 encoded_bytes=136808
  | toSkBitmap=37ms | scale=97ms | encode=7ms | base64=0ms | total=142ms
013111.729188 [screenshot] disk_write elapsed=1ms
013111.729222 PROFILE [click] total=3341ms | before_ss=183ms | resume=412ms | action=1182ms | wait=501ms | scroll_pos=0ms | after_ss=1061ms | pause=-1ms
013111.740847 [pause] Debugger.enable SEND           ← async, after response sent
013111.850409 [pause] Debugger.enable DONE elapsed=109ms
013111.850485 [pause] Debugger.pause SEND
013111.850963 [pause] Debugger.pause DONE elapsed=0ms
013111.863482 [pause] Debugger.paused EVENT wait=12ms
013111.863601 [pause] setVirtualTimePolicy(pause) SEND
013111.932715 [pause] setVirtualTimePolicy(pause) DONE elapsed=69ms
```

### Time Budget: Click on Amazon (Run 3 — 3.3s)

```
                    0s        1s        2s        3s
                    |---------|---------|---------|--
before_ss grab      ||                                  42ms
before_ss encode    ||=|                               140ms
resume dbg.resume   |  |                                 1ms
resume VT realtime  |  |====|                          404ms
action (click)      |       |================|       1182ms
wait (min 500ms)    |                        |===|    501ms
after_ss ForceRedraw|                            |========| 707ms
after_ss CA wait    |                                    || 167ms
after_ss grab       |                                     | 42ms
after_ss encode     |                                     |=| 142ms
scroll position     |                                       | 0ms (compositor)
pause               |                                       | async (not on critical path)
```

### Key Improvements from Run 2 → Run 3

| What changed | Before | After | Savings |
|--------------|--------|-------|---------|
| Screenshot encode (JPEG vs WebP) | 1,288ms | 140ms | **-1,148ms** per screenshot |
| Scroll position (compositor vs JS) | 239ms | 0ms | **-239ms** |
| Markup inject (opt-in, not requested) | 334ms | 0ms | **-334ms** |
| Retina downscale (Lanczos2 vs Lanczos3) | 217ms | 95ms | **-122ms** |
| Pause (async after response) | 181ms | 0ms on critical path | **-181ms** |
| After-screenshot total | 3,489ms | 1,061ms | **-2,428ms** |
| Before-screenshot total | 1,585ms | 183ms | **-1,402ms** |
| **Total per click** | **7,233ms** | **3,341ms** | **-3,892ms (54%)** |
