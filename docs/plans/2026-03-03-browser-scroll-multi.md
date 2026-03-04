# Multi-Scroll with Intermediate Screenshots Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extend `browser_scroll` to accept a `scrolls` array of up to 3 `{delta_x, delta_y}` objects, capturing and returning a screenshot after each scroll as sequential MCP image blocks.

**Architecture:** In `AbpInputDispatcher::Scroll()`, detect a `scrolls` array. For each non-final scroll, dispatch the wheel event, wait 500ms, then call `CaptureScreenshotFromBuffer` to capture an intermediate screenshot stored in the result dict as `intermediate_screenshots`. The final scroll calls `OnActionDispatched()` so `AbpActionContext` handles the final `screenshot_after` as usual. In `OnControllerResponse`, extract `intermediate_screenshots` from the `result` dict and emit each as an MCP image block before the `screenshot_after` image block. Single-scroll (existing `delta_x`/`delta_y`) is unchanged.

**Tech Stack:** C++17, base::Value, AbpActionContext, AbpInputDispatcher, AbpMcpHandler, ToolBuilder

---

### Task 1: Update `browser_scroll` MCP tool schema and description

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:270-291`

**Step 1: Replace the existing `browser_scroll` tool definition**

Find the block starting with `tools.Append(ToolBuilder("browser_scroll")` at line ~270. Replace it entirely with:

```cpp
  // 2. browser_scroll — standalone scroll, optionally multi-scroll
  {
    base::Value::Dict scroll_item_props;
    {
      base::Value::Dict dx;
      dx.Set("type", "number");
      dx.Set("description",
             "Horizontal scroll in pixels (positive=right, negative=left)");
      scroll_item_props.Set("delta_x", std::move(dx));
    }
    {
      base::Value::Dict dy;
      dy.Set("type", "number");
      dy.Set("description",
             "Vertical scroll in pixels (negative=up, positive=down)");
      scroll_item_props.Set("delta_y", std::move(dy));
    }
    tools.Append(
        ToolBuilder("browser_scroll")
            .Description(
                "Scroll using mouse wheel at element coordinates. "
                "Simulates moving mouse over element and scrolling. At "
                "least one of delta_x or delta_y must be non-zero. "
                "Optionally accepts a scrolls array of up to 3 "
                "{delta_x, delta_y} objects to scroll multiple viewport "
                "heights in one action — a screenshot is captured after "
                "each scroll and returned as sequential image blocks.")
            .OptionalString("tab_id", "Target tab ID")
            .RequiredNumber(
                "x",
                "X pixel coordinate of element center where mouse wheel "
                "fires. Read from the red grid on your screenshot. Must "
                "be within viewport bounds.")
            .RequiredNumber(
                "y",
                "Y pixel coordinate of element center where mouse wheel "
                "fires. Read from the red grid on your screenshot. Must "
                "be within viewport bounds.")
            .OptionalNumber("delta_x",
                            "Horizontal scroll in pixels (positive=right, "
                            "negative=left, default=0)")
            .OptionalNumber("delta_y",
                            "Vertical scroll in pixels (negative=up, "
                            "positive=down, default=0)")
            .OptionalObjectArray(
                "scrolls",
                "Array of up to 3 scroll events. Each item scrolls from "
                "the same x,y coordinates. Use instead of delta_x/delta_y "
                "for multi-viewport scrolling.",
                std::move(scroll_item_props),
                base::Value::List())
            .Build());
  }
```

**Step 2: Build to check for compile errors**

```bash
cd /Users/hanwang/src/src
autoninja -C out/Default chrome/browser/abp:abp 2>&1 | tail -20
```

Expected: no errors.

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.cc
git commit -m "feat(abp): update browser_scroll MCP schema for multi-scroll"
```

---

### Task 2: Implement multi-scroll dispatch in `AbpInputDispatcher::Scroll()`

**Files:**
- Modify: `chrome/browser/abp/abp_input_dispatcher.cc:641-697`

**Step 1: Add the `DispatchMultiScroll` free function before `AbpInputDispatcher::Scroll()`**

Insert this static function immediately before the `AbpInputDispatcher::Scroll` definition (around line 641). It uses the same recursive pattern as `DispatchBatchAction` in `abp_controller.cc`.

```cpp
// Forward declaration for recursive call
static void DispatchMultiScroll(double x,
                                double y,
                                std::vector<std::pair<double, double>> scrolls,
                                size_t index,
                                base::Value::List intermediate_screenshots,
                                std::string tab_id,
                                AbpController* controller,
                                AbpInputDispatcher* dispatcher,
                                scoped_refptr<AbpActionContext> ctx);

static void DispatchMultiScroll(double x,
                                double y,
                                std::vector<std::pair<double, double>> scrolls,
                                size_t index,
                                base::Value::List intermediate_screenshots,
                                std::string tab_id,
                                AbpController* controller,
                                AbpInputDispatcher* dispatcher,
                                scoped_refptr<AbpActionContext> ctx) {
  content::WebContents* wc = ctx->web_contents();
  if (!wc) {
    ctx->OnActionError("TAB_ERROR", "WebContents lost");
    return;
  }

  auto [dx, dy] = scrolls[index];
  dispatcher->ForwardWheelEvent(wc, x, y, dx, dy);

  bool is_last = (index == scrolls.size() - 1);
  if (is_last) {
    // Final scroll: let AbpActionContext handle screenshot_after normally.
    base::Value::Dict res;
    res.Set("status", "scrolled");
    res.Set("scrolls_executed", static_cast<int>(scrolls.size()));
    res.Set("x", x);
    res.Set("y", y);
    if (!intermediate_screenshots.empty()) {
      res.Set("intermediate_screenshots", std::move(intermediate_screenshots));
    }
    ctx->SetResult(std::move(res));
    ctx->OnActionDispatched();
    return;
  }

  // Non-final scroll: wait 500ms for scroll to render, then capture.
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](double x, double y,
             std::vector<std::pair<double, double>> scrolls, size_t next_index,
             base::Value::List intermediate_screenshots, std::string tab_id,
             AbpController* controller, AbpInputDispatcher* dispatcher,
             scoped_refptr<AbpActionContext> ctx) {
            AbpController::ScreenshotOptions opts;
            // Use webp at quality 80 for intermediate screenshots.
            opts.format = "webp";
            opts.quality = 80;
            controller->CaptureScreenshotFromBuffer(
                tab_id, /*timestamp=*/0, /*is_before=*/false, opts,
                base::BindOnce(
                    [](double x, double y,
                       std::vector<std::pair<double, double>> scrolls,
                       size_t next_index,
                       base::Value::List intermediate_screenshots,
                       std::string tab_id, AbpController* controller,
                       AbpInputDispatcher* dispatcher,
                       scoped_refptr<AbpActionContext> ctx,
                       AbpController::ActionScreenshotResult result) {
                      if (!result.base64.empty()) {
                        base::Value::Dict ss;
                        ss.Set("data", std::move(result.base64));
                        ss.Set("width", result.width);
                        ss.Set("height", result.height);
                        ss.Set("format", "webp");
                        intermediate_screenshots.Append(std::move(ss));
                      }
                      DispatchMultiScroll(x, y, std::move(scrolls), next_index,
                                         std::move(intermediate_screenshots),
                                         std::move(tab_id), controller,
                                         dispatcher, std::move(ctx));
                    },
                    x, y, std::move(scrolls), next_index,
                    std::move(intermediate_screenshots), tab_id, controller,
                    dispatcher, std::move(ctx)));
          },
          x, y, std::move(scrolls), index + 1,
          std::move(intermediate_screenshots), std::move(tab_id), controller,
          dispatcher, std::move(ctx)),
      base::Milliseconds(500));
}
```

**Step 2: Replace the `AbpInputDispatcher::Scroll()` method body**

Replace the entire body of `Scroll()` (lines 641-697) with the following. The single-scroll path is identical to what exists today; multi-scroll is detected first.

```cpp
void AbpInputDispatcher::Scroll(const std::string& tab_id,
                                const base::Value::Dict& params,
                                ResponseCallback callback) {
  auto x_opt = params.FindDouble("x");
  auto y_opt = params.FindDouble("y");
  if (!x_opt || !y_opt) {
    controller_->SendError(400, "Missing required 'x' or 'y' parameter",
                           std::move(callback));
    return;
  }
  double x = *x_opt;
  double y = *y_opt;

  // --- Multi-scroll path ---
  const base::Value::List* scrolls_list = params.FindList("scrolls");
  if (scrolls_list && !scrolls_list->empty()) {
    if (scrolls_list->size() > 3) {
      controller_->SendError(
          400, "'scrolls' array must have at most 3 elements",
          std::move(callback));
      return;
    }

    // Validate and extract scroll deltas.
    std::vector<std::pair<double, double>> scrolls;
    scrolls.reserve(scrolls_list->size());
    for (size_t i = 0; i < scrolls_list->size(); i++) {
      const base::Value::Dict* item = (*scrolls_list)[i].GetIfDict();
      if (!item) {
        controller_->SendError(
            400, "Each 'scrolls' item must be an object",
            std::move(callback));
        return;
      }
      double dx = item->FindDouble("delta_x").value_or(0);
      double dy = item->FindDouble("delta_y").value_or(0);
      if (dx == 0 && dy == 0) {
        controller_->SendError(
            400,
            "Each 'scrolls' item must have at least one non-zero delta",
            std::move(callback));
        return;
      }
      scrolls.emplace_back(dx, dy);
    }

    auto options = controller_->GetDefaultActionOptions();
    options.min_wait_time = base::Milliseconds(500);
    AbpActionContext::RunWithOptions(
        controller_, tab_id, "scroll", params, options,
        base::BindOnce(
            [](double scroll_x, double scroll_y,
               std::vector<std::pair<double, double>> scrolls,
               std::string tab_id, AbpController* controller,
               AbpInputDispatcher* dispatcher, AbpActionContext* ctx) {
              scoped_refptr<AbpActionContext> ctx_ref(ctx);
              DispatchMultiScroll(scroll_x, scroll_y, std::move(scrolls), 0,
                                  base::Value::List(), std::move(tab_id),
                                  controller, dispatcher, std::move(ctx_ref));
            },
            x, y, std::move(scrolls), tab_id, controller_, this),
        std::move(callback));
    return;
  }

  // --- Single-scroll path (existing behavior, unchanged) ---
  double delta_x = params.FindDouble("delta_x").value_or(0);
  double delta_y = params.FindDouble("delta_y").value_or(0);

  if (delta_x == 0 && delta_y == 0) {
    controller_->SendError(
        400, "At least one of 'delta_x' or 'delta_y' must be non-zero",
        std::move(callback));
    return;
  }

  auto options = controller_->GetDefaultActionOptions();
  options.min_wait_time = base::Milliseconds(500);
  AbpActionContext::RunWithOptions(
      controller_, tab_id, "scroll", params, options,
      base::BindOnce(
          [](double scroll_x, double scroll_y, double dx, double dy,
             AbpInputDispatcher* dispatcher, AbpActionContext* ctx) {
            content::WebContents* wc = ctx->web_contents();
            if (!wc) {
              ctx->OnActionError("TAB_ERROR", "WebContents lost");
              return;
            }
            dispatcher->ForwardWheelEvent(wc, scroll_x, scroll_y, dx, dy);
            base::Value::Dict res;
            res.Set("status", "scrolled");
            res.Set("x", scroll_x);
            res.Set("y", scroll_y);
            res.Set("delta_x", dx);
            res.Set("delta_y", dy);
            ctx->SetResult(std::move(res));
            ctx->OnActionDispatched();
          },
          x, y, delta_x, delta_y, this),
      std::move(callback));
}
```

**Step 3: Build**

```bash
autoninja -C out/Default chrome/browser/abp:abp 2>&1 | tail -20
```

Expected: no errors. Fix any issues (likely: missing `#include` for `std::vector` — check if `<vector>` is already included at the top of `abp_input_dispatcher.cc`; add if missing).

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_input_dispatcher.cc
git commit -m "feat(abp): implement multi-scroll with intermediate screenshot capture"
```

---

### Task 3: Update `OnControllerResponse` to emit intermediate screenshots as image blocks

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc` — `OnControllerResponse()` at line ~1604

**Step 1: Add intermediate screenshot extraction after the `response_dict.Remove("screenshot_before")` line**

Find this block in `OnControllerResponse` (around line 1643):

```cpp
    // Strip before screenshot entirely from MCP response (not useful to agents)
    response_dict.Remove("screenshot_before");
```

Immediately after that line, add the intermediate screenshot extraction:

```cpp
    // Extract intermediate screenshots from result.intermediate_screenshots
    // (multi-scroll actions). Strip data from JSON; emit as image blocks.
    std::vector<std::pair<std::string, std::string>> intermediate_images;
    if (base::Value::Dict* result_dict = response_dict.FindDict("result")) {
      if (base::Value::List* intermed =
              result_dict->FindList("intermediate_screenshots")) {
        for (auto& item : *intermed) {
          if (!item.is_dict()) continue;
          std::string img_data, img_mime;
          extract_image(&item.GetDict(), img_data, img_mime);
          if (!img_data.empty()) {
            intermediate_images.emplace_back(std::move(img_data), img_mime);
          }
        }
      }
    }
```

**Step 2: Emit intermediate image blocks before the `screenshot_after` image block**

Find this block (around line 1685):

```cpp
    // Add screenshot as image content block
    if (!after_data.empty()) {
      base::Value::Dict img;
      img.Set("type", "image");
      img.Set("data", std::move(after_data));
      img.Set("mimeType", after_mime);
      content.Append(std::move(img));
    }
```

Replace it with:

```cpp
    // Add intermediate screenshots as image blocks (multi-scroll).
    // These appear before the final screenshot_after image block.
    for (auto& [img_data, img_mime] : intermediate_images) {
      base::Value::Dict img;
      img.Set("type", "image");
      img.Set("data", std::move(img_data));
      img.Set("mimeType", img_mime);
      content.Append(std::move(img));
    }

    // Add final screenshot (screenshot_after or single).
    if (!after_data.empty()) {
      base::Value::Dict img;
      img.Set("type", "image");
      img.Set("data", std::move(after_data));
      img.Set("mimeType", after_mime);
      content.Append(std::move(img));
    }
```

**Step 3: Build the full chrome target**

```bash
autoninja -C out/Default chrome 2>&1 | tail -30
```

Expected: no errors.

**Step 4: Smoke test with the browser**

Start the browser:
```bash
./out/Default/ABP.app/Contents/MacOS/ABP --no-first-run &
sleep 3
```

Navigate somewhere scrollable:
```bash
TAB=$(curl -s http://localhost:8222/api/v1/tabs | python3 -c "import sys,json; print(json.load(sys.stdin)['tabs'][0]['id'])")
curl -s -X POST http://localhost:8222/api/v1/tabs/$TAB/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"https://en.wikipedia.org/wiki/Chromium_(web_browser)"}' | python3 -m json.tool
sleep 2
```

Test single scroll (backward compat — should return 1 screenshot):
```bash
curl -s -X POST http://localhost:8222/api/v1/tabs/$TAB/scroll \
  -H "Content-Type: application/json" \
  -d '{"x":700,"y":400,"delta_y":600}' | python3 -c "
import sys,json
r=json.load(sys.stdin)
ss=r.get('screenshot_after',{})
print('single scroll: screenshot_after data len:', len(ss.get('data','')))
print('intermediate_screenshots:', r.get('result',{}).get('intermediate_screenshots','none'))
"
```

Expected: `screenshot_after data len: >0`, `intermediate_screenshots: none`.

Test multi-scroll (3 scrolls — should return `intermediate_screenshots` with 2 entries + `screenshot_after`):
```bash
curl -s -X POST http://localhost:8222/api/v1/tabs/$TAB/scroll \
  -H "Content-Type: application/json" \
  -d '{
    "x": 700,
    "y": 400,
    "scrolls": [
      {"delta_y": 600},
      {"delta_y": 600},
      {"delta_y": 600}
    ]
  }' | python3 -c "
import sys,json
r=json.load(sys.stdin)
intermed = r.get('result',{}).get('intermediate_screenshots',[])
after = r.get('screenshot_after',{})
print('scrolls_executed:', r.get('result',{}).get('scrolls_executed'))
print('intermediate_screenshots count:', len(intermed))
for i,ss in enumerate(intermed):
    print(f'  [{i}] data len={len(ss.get(\"data\",\"\"))}, size={ss.get(\"width\")}x{ss.get(\"height\")}')
print('screenshot_after data len:', len(after.get('data','')))
"
```

Expected:
```
scrolls_executed: 3
intermediate_screenshots count: 2
  [0] data len=<non-zero>, size=<width>x<height>
  [1] data len=<non-zero>, size=<width>x<height>
screenshot_after data len: <non-zero>
```

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.cc
git commit -m "feat(abp): emit intermediate_screenshots as MCP image blocks in multi-scroll"
```
