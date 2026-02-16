# Drag and Drop Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a single atomic drag-and-drop action (`POST /tabs/{id}/drag` + MCP `browser_drag`) that takes start/end coordinates and dispatches the full mouse event sequence via CDP.

**Architecture:** Follows the same pattern as Click — uses `AbpActionContext::Run` for the unified action lifecycle, CDP `Input.dispatchMouseEvent` for mouse events, and a recursive step function (like `TypeNextCharacter`) for interpolated mouseMoved events with delays.

**Tech Stack:** C++ (Chromium), CDP `Input.dispatchMouseEvent`, `AbpActionContext`, `AbpInputDispatcher`

---

### Task 1: Add Drag to AbpInputDispatcher header

**Files:**
- Modify: `chrome/browser/abp/abp_input_dispatcher.h`

**Step 1: Add method declarations**

Add after the `KeyUp` declaration (line 78) and before the `private:` section (line 80):

```cpp
  // Drag from start to end coordinates with interpolated mouse moves
  void Drag(const std::string& tab_id,
            const base::Value::Dict& params,
            ResponseCallback callback);
```

Add in the `private:` section, after `TypeNextCharacter` (line 99):

```cpp
  // Dispatch the next step in a drag sequence (recursive via PostDelayedTask)
  void DragNextStep(scoped_refptr<AbpActionContext> ctx,
                    double start_x,
                    double start_y,
                    double end_x,
                    double end_y,
                    int current_step,
                    int total_steps);
```

**Step 2: Commit**

```bash
git add chrome/browser/abp/abp_input_dispatcher.h
git commit -m "feat(abp): add Drag/DragNextStep declarations to AbpInputDispatcher"
```

---

### Task 2: Implement Drag in AbpInputDispatcher

**Files:**
- Modify: `chrome/browser/abp/abp_input_dispatcher.cc`

**Step 1: Add the `Drag` method**

Add before the closing `}  // namespace abp` at the end of the file. This method:
- Validates `start_x`, `start_y`, `end_x`, `end_y` (required)
- Reads optional `steps` (default 10, clamp to 1..100)
- Uses `AbpActionContext::Run` with action type `"drag"`
- Inside the action callback: updates virtual cursor to start position, sets cursor via Mojo, inserts visual state fence, then sends `mouseMoved` to start → `mousePressed` at start → kicks off `DragNextStep`

```cpp
void AbpInputDispatcher::Drag(const std::string& tab_id,
                              const base::Value::Dict& params,
                              ResponseCallback callback) {
  auto sx = params.FindDouble("start_x");
  auto sy = params.FindDouble("start_y");
  auto ex = params.FindDouble("end_x");
  auto ey = params.FindDouble("end_y");
  if (!sx || !sy || !ex || !ey) {
    controller_->SendError(
        400, "Missing required parameter: start_x, start_y, end_x, end_y",
        std::move(callback));
    return;
  }

  double start_x = *sx;
  double start_y = *sy;
  double end_x = *ex;
  double end_y = *ey;
  int steps = params.FindInt("steps").value_or(10);
  if (steps < 1) steps = 1;
  if (steps > 100) steps = 100;

  AbpActionContext::Run(
      controller_, tab_id, "drag", params,
      base::BindOnce(
          [](double s_x, double s_y, double e_x, double e_y, int num_steps,
             AbpInputDispatcher* dispatcher, AbpActionContext* ctx) {
            // Update virtual cursor to start position
            ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), s_x,
                                                        s_y);
            content::WebContents* wc = ctx->web_contents();
            if (wc) {
              ctx->controller()->SetVirtualCursorEnabledViaMojo(wc, true);
              ctx->controller()->SetVirtualCursorViaMojo(wc, s_x, s_y, true);
            }

            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            ctx->controller()->InsertVisualStateFence(
                ctx->tab_id(),
                base::BindOnce(
                    [](double s_x, double s_y, double e_x, double e_y,
                       int num_steps, AbpInputDispatcher* dispatcher,
                       scoped_refptr<AbpActionContext> action_ctx,
                       bool fence_ready) {
                      if (!fence_ready) {
                        action_ctx->OnActionError(
                            "VISUAL_STATE_ERROR",
                            "Failed to establish visual-state fence before drag");
                        return;
                      }

                      AbpCdpClient* cdp_client = action_ctx->client();
                      if (!cdp_client) {
                        action_ctx->OnActionError("CDP_ERROR",
                                                  "CDP client lost");
                        return;
                      }

                      // 1. mouseMoved to start position
                      base::Value::Dict move_params;
                      move_params.Set("type", "mouseMoved");
                      move_params.Set("x", s_x);
                      move_params.Set("y", s_y);

                      cdp_client->SendCommand(
                          "Input.dispatchMouseEvent", std::move(move_params),
                          base::BindOnce(
                              [](double s_x, double s_y, double e_x,
                                 double e_y, int num_steps,
                                 AbpInputDispatcher* dispatcher,
                                 scoped_refptr<AbpActionContext> action_ctx,
                                 bool success, const std::string& result) {
                                if (!success) {
                                  action_ctx->OnActionError("CDP_ERROR",
                                                            result);
                                  return;
                                }

                                AbpCdpClient* cdp_client =
                                    action_ctx->client();
                                if (!cdp_client) {
                                  action_ctx->OnActionError("CDP_ERROR",
                                                            "CDP client lost");
                                  return;
                                }

                                // 2. mousePressed at start
                                base::Value::Dict press_params;
                                press_params.Set("type", "mousePressed");
                                press_params.Set("x", s_x);
                                press_params.Set("y", s_y);
                                press_params.Set("button", "left");
                                press_params.Set("clickCount", 1);

                                cdp_client->SendCommand(
                                    "Input.dispatchMouseEvent",
                                    std::move(press_params),
                                    base::BindOnce(
                                        [](double s_x, double s_y, double e_x,
                                           double e_y, int num_steps,
                                           AbpInputDispatcher* dispatcher,
                                           scoped_refptr<AbpActionContext>
                                               action_ctx,
                                           bool success,
                                           const std::string& result) {
                                          if (!success) {
                                            action_ctx->OnActionError(
                                                "CDP_ERROR", result);
                                            return;
                                          }

                                          // 3. Start interpolated moves
                                          dispatcher->DragNextStep(
                                              action_ctx, s_x, s_y, e_x, e_y,
                                              1, num_steps);
                                        },
                                        s_x, s_y, e_x, e_y, num_steps,
                                        dispatcher, action_ctx));
                              },
                              s_x, s_y, e_x, e_y, num_steps, dispatcher,
                              action_ctx));
                    },
                    s_x, s_y, e_x, e_y, num_steps, dispatcher,
                    std::move(ctx_ref)));
          },
          start_x, start_y, end_x, end_y, steps, this),
      std::move(callback));
}
```

**Step 2: Add the `DragNextStep` method**

Add right after `Drag`. This method:
- If `current_step <= total_steps`: sends `mouseMoved` to interpolated position, then recurses via `PostDelayedTask` with 5ms delay
- If `current_step > total_steps`: sends `mouseReleased` at end position, updates virtual cursor to end, sets result, calls `OnActionDispatched`

```cpp
void AbpInputDispatcher::DragNextStep(
    scoped_refptr<AbpActionContext> ctx,
    double start_x,
    double start_y,
    double end_x,
    double end_y,
    int current_step,
    int total_steps) {
  AbpCdpClient* cdp_client = ctx->client();
  if (!cdp_client) {
    ctx->OnActionError("CDP_ERROR", "CDP client lost");
    return;
  }

  if (current_step <= total_steps) {
    // Interpolate position
    double t = static_cast<double>(current_step) / total_steps;
    double x = start_x + (end_x - start_x) * t;
    double y = start_y + (end_y - start_y) * t;

    // Update virtual cursor as we drag
    ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), x, y);
    content::WebContents* wc = ctx->web_contents();
    if (wc) {
      ctx->controller()->SetVirtualCursorViaMojo(wc, x, y, true);
    }

    base::Value::Dict move_params;
    move_params.Set("type", "mouseMoved");
    move_params.Set("x", x);
    move_params.Set("y", y);
    move_params.Set("button", "left");

    cdp_client->SendCommand(
        "Input.dispatchMouseEvent", std::move(move_params),
        base::BindOnce(
            [](scoped_refptr<AbpActionContext> ctx, double s_x, double s_y,
               double e_x, double e_y, int step, int total,
               AbpInputDispatcher* dispatcher, bool success,
               const std::string& result) {
              if (!success) {
                ctx->OnActionError("CDP_ERROR", result);
                return;
              }

              // Schedule next step with 5ms delay
              content::GetUIThreadTaskRunner({})->PostDelayedTask(
                  FROM_HERE,
                  base::BindOnce(&AbpInputDispatcher::DragNextStep,
                                 base::Unretained(dispatcher), ctx, s_x, s_y,
                                 e_x, e_y, step + 1, total),
                  base::Milliseconds(5));
            },
            ctx, start_x, start_y, end_x, end_y, current_step, total_steps,
            this));
  } else {
    // All steps done — send mouseReleased at end position
    ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), end_x, end_y);
    content::WebContents* wc = ctx->web_contents();
    if (wc) {
      ctx->controller()->SetVirtualCursorViaMojo(wc, end_x, end_y, true);
    }

    base::Value::Dict release_params;
    release_params.Set("type", "mouseReleased");
    release_params.Set("x", end_x);
    release_params.Set("y", end_y);
    release_params.Set("button", "left");
    release_params.Set("clickCount", 1);

    cdp_client->SendCommand(
        "Input.dispatchMouseEvent", std::move(release_params),
        base::BindOnce(
            [](double s_x, double s_y, double e_x, double e_y,
               scoped_refptr<AbpActionContext> ctx, bool success,
               const std::string& result) {
              if (!success) {
                ctx->OnActionError("CDP_ERROR", result);
                return;
              }

              base::Value::Dict res;
              res.Set("status", "dragged");
              res.Set("start_x", s_x);
              res.Set("start_y", s_y);
              res.Set("end_x", e_x);
              res.Set("end_y", e_y);
              ctx->SetResult(std::move(res));
              ctx->OnActionDispatched();
            },
            start_x, start_y, end_x, end_y, ctx));
  }
}
```

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_input_dispatcher.cc
git commit -m "feat(abp): implement Drag and DragNextStep in AbpInputDispatcher"
```

---

### Task 3: Wire Drag into AbpController

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h`
- Modify: `chrome/browser/abp/abp_controller.cc`

**Step 1: Add declaration to header**

In `abp_controller.h`, add `Drag` in the private Input section after `Scroll` (around line 410):

```cpp
  void Drag(const std::string& tab_id,
            const base::Value::Dict& params,
            ResponseCallback callback);
```

**Step 2: Add forwarding method in .cc**

Add after the `AbpController::Scroll` method (around line 2328):

```cpp
void AbpController::Drag(const std::string& tab_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback) {
  input_dispatcher_->Drag(tab_id, params, std::move(callback));
}
```

**Step 3: Add routing in HandleRequest**

In the routing section of `HandleRequest`, add after the `"scroll"` case (around line 1596):

```cpp
      } else if (action == "drag") {
        Drag(tab_id, params, std::move(callback));
```

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): route POST /tabs/{id}/drag to AbpInputDispatcher::Drag"
```

---

### Task 4: Add MCP browser_drag tool

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.h`
- Modify: `chrome/browser/abp/abp_mcp_handler.cc`

**Step 1: Add declaration to header**

In `abp_mcp_handler.h`, add after `CallBrowserMouseMove` (line 101):

```cpp
  void CallBrowserDrag(const base::Value::Dict& args,
                       base::Value request_id,
                       ResponseWithHeadersCallback callback);
```

**Step 2: Add tool definition in HandleToolsList**

In `abp_mcp_handler.cc`, in the `HandleToolsList` method, add after the `browser_mouse_move` tool definition (after line 168):

```cpp
  tools.Append(ToolBuilder("browser_drag")
                   .Description(
                       "Drag and drop from one position to another. Performs "
                       "mousedown at start, interpolated mousemoves along the "
                       "path, and mouseup at end. IMPORTANT: Determine "
                       "coordinates by reading the red coordinate grid overlay "
                       "on your most recent screenshot.")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredNumber("start_x",
                       "X coordinate of drag start. Read from the red grid on "
                       "your screenshot.")
                   .RequiredNumber("start_y",
                       "Y coordinate of drag start. Read from the red grid on "
                       "your screenshot.")
                   .RequiredNumber("end_x",
                       "X coordinate of drop target. Read from the red grid on "
                       "your screenshot.")
                   .RequiredNumber("end_y",
                       "Y coordinate of drop target. Read from the red grid on "
                       "your screenshot.")
                   .OptionalNumber("steps",
                       "Number of intermediate mouse move events (default: 10)")
                   .Build());
```

**Step 3: Add routing in HandleToolsCall**

In the tool dispatch chain, add after `browser_mouse_move` (after line 558):

```cpp
  } else if (*name == "browser_drag") {
    CallBrowserDrag(*args, std::move(request_id), std::move(callback));
```

**Step 4: Add implementation method**

Add after `CallBrowserMouseMove` method (after line 1048):

```cpp
void AbpMcpHandler::CallBrowserDrag(const base::Value::Dict& args,
                                    base::Value request_id,
                                    ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/drag", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}
```

**Step 5: Update the Tool Reference in system prompt**

In the `kSystemPrompt` string, update the Input line (around line 307) to include `browser_drag`:

Change:
```
**Input:** `browser_click` (x, y required), `browser_type` (text required), `browser_keyboard_press` (key required, modifiers), `browser_keyboard_down` (key), `browser_keyboard_up` (key), `browser_scroll` (x, y required; delta_x, delta_y), `browser_mouse_move` (x, y required)
```
To:
```
**Input:** `browser_click` (x, y required), `browser_type` (text required), `browser_keyboard_press` (key required, modifiers), `browser_keyboard_down` (key), `browser_keyboard_up` (key), `browser_scroll` (x, y required; delta_x, delta_y), `browser_mouse_move` (x, y required), `browser_drag` (start_x, start_y, end_x, end_y required; steps)
```

**Step 6: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.h chrome/browser/abp/abp_mcp_handler.cc
git commit -m "feat(abp): add browser_drag MCP tool"
```

---

### Task 5: Build and validate

**Step 1: Build**

```bash
cd /Users/hanwang/src/src
autoninja -C out/Default chrome
```

**Step 2: Fix any compile errors**

Address any issues from the build.

**Step 3: Commit fixes if needed**

```bash
git add -u
git commit -m "fix(abp): fix drag build errors"
```

---

### Task 6: Manual smoke test

**Step 1: Launch ABP**

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run
```

**Step 2: Test REST API**

```bash
# Navigate to a page with draggable elements
curl -X POST http://localhost:8222/api/v1/tabs/$(curl -s http://localhost:8222/api/v1/tabs | python3 -c "import sys,json;print(json.load(sys.stdin)[0]['id'])")/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"data:text/html,<div id=\"d\" draggable=\"true\" style=\"width:100px;height:100px;background:red;position:absolute;left:50px;top:50px\" ondragend=\"this.style.left=event.clientX+\\\"px\\\";this.style.top=event.clientY+\\\"px\\\"\">Drag me</div>"}'

# Perform drag
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/drag \
  -H "Content-Type: application/json" \
  -d '{"start_x":100,"start_y":100,"end_x":400,"end_y":300}'
```

Expected: `{"status":"dragged","start_x":100,"start_y":100,"end_x":400,"end_y":300}` with screenshot in response.

**Step 3: Test MCP tool**

The MCP tool can be tested via the embedded MCP endpoint or by using Claude Desktop with the browser MCP server configured.
