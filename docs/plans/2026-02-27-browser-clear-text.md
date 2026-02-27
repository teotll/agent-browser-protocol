# browser_clear_text Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a `browser_clear_text` MCP macro tool that clicks to focus an input, reads its current value, then clears it via repeated Backspace keystrokes in batches of 20 at 240 WPM. Checks after each batch — exits when empty or when a batch produces no change (stuck). Max 10,000 keystrokes.

**Architecture:** MCP tool → REST endpoint → controller forwarder → input dispatcher. Uses `AbpActionContext::RunWithOptions` for action lifecycle. Click via CDP `Input.dispatchMouseEvent`, backspaces via native `ForwardKeyEvent`, value checks via CDP `Runtime.evaluate`.

**Tech Stack:** C++ (Chromium), CDP, native keyboard input

**Algorithm:**
1. Click `(x, y)` to focus the input
2. Read `activeElement.value` (or `.textContent`) via `Runtime.evaluate`
3. If empty/null → send 1 batch of 20 backspaces as a safety sweep → done
4. If has content → enter batch loop:
   - Send 20 backspaces at 240 WPM (50ms per keystroke)
   - Read value again
   - If empty → done
   - If value unchanged from before this batch → done (stuck, element doesn't respond to backspace)
   - If `total_sent >= 10000` → done
   - Otherwise → next batch with current value as new `previous_value`

**JS expression for reading value** (used with `disableBreaks: true`):
```js
(function(){var e=document.activeElement;if(!e)return '';
if(typeof e.value==='string')return e.value;return e.textContent||'';})()
```

**Helper to parse CDP Runtime.evaluate result:**
CDP returns JSON like `{"result":{"type":"string","value":"..."}}`. Extract the `value` string. If parsing fails or no value key, treat as empty string.

---

### Task 1: Add ClearText to AbpInputDispatcher header

**Files:**
- Modify: `chrome/browser/abp/abp_input_dispatcher.h:90-93` (public section, after Slider)
- Modify: `chrome/browser/abp/abp_input_dispatcher.h:180-198` (private section, after KeyPressKeyUpRaw)

**Step 1: Add public declaration after Slider (line 93)**

```cpp
  // Clear text: click to focus, read value, then backspace in batches of 20
  void ClearText(const std::string& tab_id,
                 const base::Value::Dict& params,
                 ResponseCallback callback);
```

**Step 2: Add private helper declarations before DragNextStep (line 181)**

```cpp
  // Read active element value, then decide whether to clear
  void ClearTextCheckInitial(scoped_refptr<AbpActionContext> ctx);

  // Send a batch of 20 backspace keystrokes
  void ClearTextBatch(scoped_refptr<AbpActionContext> ctx,
                      int total_sent,
                      std::string previous_value);

  // Send keyUp for backspace, then schedule next key or check
  void ClearTextKeyUp(scoped_refptr<AbpActionContext> ctx,
                      int total_sent,
                      int batch_count,
                      std::string previous_value);

  // After inter-key gap: send next backspace or check if cleared
  void ClearTextNextKey(scoped_refptr<AbpActionContext> ctx,
                        int total_sent,
                        int batch_count,
                        std::string previous_value);

  // Parse value string from CDP Runtime.evaluate JSON result
  static std::string ParseEvalStringResult(const std::string& cdp_result);
```

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_input_dispatcher.h
git commit -m "feat(abp): add ClearText declarations to input dispatcher header"
```

---

### Task 2: Implement ClearText in AbpInputDispatcher

**Files:**
- Modify: `chrome/browser/abp/abp_input_dispatcher.cc` (after Slider implementation, ~line 1101)

**Step 1: Add ParseEvalStringResult helper**

Insert after `Slider()` closing brace:

```cpp
// static
std::string AbpInputDispatcher::ParseEvalStringResult(
    const std::string& cdp_result) {
  auto parsed = base::JSONReader::Read(cdp_result);
  if (parsed && parsed->is_dict()) {
    const base::Value::Dict* result_obj =
        parsed->GetDict().FindDict("result");
    if (result_obj) {
      const std::string* val = result_obj->FindString("value");
      if (val) {
        return *val;
      }
    }
  }
  return "";
}
```

**Step 2: Add ClearText entry point**

```cpp
void AbpInputDispatcher::ClearText(const std::string& tab_id,
                                   const base::Value::Dict& params,
                                   ResponseCallback callback) {
  auto x_opt = params.FindDouble("x");
  auto y_opt = params.FindDouble("y");
  if (!x_opt || !y_opt) {
    controller_->SendError(400, "Missing required 'x' or 'y' parameter",
                           std::move(callback));
    return;
  }

  double click_x = *x_opt;
  double click_y = *y_opt;

  AbpActionContext::RunWithOptions(
      controller_, tab_id, "clear_text", params,
      controller_->GetDefaultActionOptions(),
      base::BindOnce(
          [](double x, double y, AbpInputDispatcher* dispatcher,
             AbpActionContext* ctx) {
            // Update virtual cursor to click position
            ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), x, y);
            content::WebContents* wc = ctx->web_contents();
            if (wc) {
              ctx->controller()->SetVirtualCursorEnabledViaMojo(wc, true);
              ctx->controller()->SetVirtualCursorViaMojo(wc, x, y, true);
            }

            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            ctx->controller()->InsertVisualStateFence(
                ctx->tab_id(),
                base::BindOnce(
                    [](double x, double y, AbpInputDispatcher* disp,
                       scoped_refptr<AbpActionContext> action_ctx,
                       bool fence_ready) {
                      if (!fence_ready) {
                        action_ctx->OnActionError(
                            "VISUAL_STATE_ERROR",
                            "Failed to establish visual-state fence");
                        return;
                      }

                      AbpCdpClient* cdp_client = action_ctx->client();
                      if (!cdp_client) {
                        action_ctx->OnActionError("CDP_ERROR",
                                                  "CDP client lost");
                        return;
                      }

                      // Click to focus: mousePressed
                      base::Value::Dict press_params;
                      press_params.Set("type", "mousePressed");
                      press_params.Set("x", x);
                      press_params.Set("y", y);
                      press_params.Set("button", "left");
                      press_params.Set("clickCount", 1);

                      cdp_client->SendCommand(
                          "Input.dispatchMouseEvent", std::move(press_params),
                          base::BindOnce(
                              [](double x, double y,
                                 AbpInputDispatcher* disp,
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

                                // Click to focus: mouseReleased
                                base::Value::Dict release_params;
                                release_params.Set("type", "mouseReleased");
                                release_params.Set("x", x);
                                release_params.Set("y", y);
                                release_params.Set("button", "left");
                                release_params.Set("clickCount", 1);

                                cdp_client->SendCommand(
                                    "Input.dispatchMouseEvent",
                                    std::move(release_params),
                                    base::BindOnce(
                                        [](AbpInputDispatcher* disp,
                                           scoped_refptr<AbpActionContext>
                                               action_ctx,
                                           bool success,
                                           const std::string& result) {
                                          if (!success) {
                                            action_ctx->OnActionError(
                                                "CDP_ERROR", result);
                                            return;
                                          }
                                          // Click done — read initial value
                                          disp->ClearTextCheckInitial(
                                              action_ctx);
                                        },
                                        disp, action_ctx));
                              },
                              x, y, disp, action_ctx));
                    },
                    x, y, dispatcher, std::move(ctx_ref)));
          },
          click_x, click_y, this),
      std::move(callback));
}
```

**Step 3: Add ClearTextCheckInitial — reads value and decides path**

```cpp
void AbpInputDispatcher::ClearTextCheckInitial(
    scoped_refptr<AbpActionContext> ctx) {
  AbpCdpClient* cdp_client = ctx->client();
  if (!cdp_client) {
    ctx->OnActionError("CDP_ERROR", "CDP client lost");
    return;
  }

  base::Value::Dict eval_params;
  eval_params.Set("expression",
      "(function(){var e=document.activeElement;if(!e)return '';"
      "if(typeof e.value==='string')return e.value;"
      "return e.textContent||'';})()");
  eval_params.Set("returnByValue", true);
  eval_params.Set("disableBreaks", true);

  cdp_client->SendCommand(
      "Runtime.evaluate", std::move(eval_params),
      base::BindOnce(
          [](scoped_refptr<AbpActionContext> ctx,
             AbpInputDispatcher* dispatcher, bool success,
             const std::string& result) {
            if (!success) {
              ctx->OnActionError("CDP_ERROR", result);
              return;
            }

            std::string initial_value = ParseEvalStringResult(result);

            if (initial_value.empty()) {
              // Already empty — send 1 safety batch of 20 backspaces, then done
              dispatcher->ClearTextBatch(ctx, 0, "");
            } else {
              // Has content — enter batch loop with stuck detection
              dispatcher->ClearTextBatch(ctx, 0, std::move(initial_value));
            }
          },
          ctx, this));
}
```

**Step 4: Add ClearTextBatch — fires first backspace in a batch**

```cpp
void AbpInputDispatcher::ClearTextBatch(
    scoped_refptr<AbpActionContext> ctx,
    int total_sent,
    std::string previous_value) {
  // Max 10,000 keystrokes
  if (total_sent >= 10000) {
    base::Value::Dict res;
    res.Set("status", "cleared");
    res.Set("keystrokes_sent", total_sent);
    ctx->SetResult(std::move(res));
    ctx->OnActionDispatched();
    return;
  }

  content::WebContents* wc = ctx->web_contents();
  if (!wc) {
    ctx->OnActionError("TAB_ERROR", "WebContents lost");
    return;
  }

  // Send first backspace keyDown in this batch
  KeyInfo bs_info = GetKeyInfo("Backspace");
  ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyDown, bs_info, 0);

  // Schedule keyUp after dwell (20ms)
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpInputDispatcher::ClearTextKeyUp,
                     base::Unretained(this), ctx, total_sent + 1, 1,
                     std::move(previous_value)),
      base::Milliseconds(kKeyDwellMs));
}
```

**Step 5: Add ClearTextKeyUp — sends keyUp, schedules next key**

```cpp
void AbpInputDispatcher::ClearTextKeyUp(
    scoped_refptr<AbpActionContext> ctx,
    int total_sent,
    int batch_count,
    std::string previous_value) {
  content::WebContents* wc = ctx->web_contents();
  if (!wc) {
    ctx->OnActionError("TAB_ERROR", "WebContents lost");
    return;
  }

  KeyInfo bs_info = GetKeyInfo("Backspace");
  ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyUp, bs_info, 0);

  // 30ms gap to next key (20ms dwell + 30ms gap = 50ms per key = 240 WPM)
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpInputDispatcher::ClearTextNextKey,
                     base::Unretained(this), ctx, total_sent, batch_count,
                     std::move(previous_value)),
      base::Milliseconds(30));
}
```

**Step 6: Add ClearTextNextKey — sends next key or checks if cleared**

```cpp
void AbpInputDispatcher::ClearTextNextKey(
    scoped_refptr<AbpActionContext> ctx,
    int total_sent,
    int batch_count,
    std::string previous_value) {
  // If batch not done (20 keys per batch), send next backspace
  if (batch_count < 20) {
    content::WebContents* wc = ctx->web_contents();
    if (!wc) {
      ctx->OnActionError("TAB_ERROR", "WebContents lost");
      return;
    }

    KeyInfo bs_info = GetKeyInfo("Backspace");
    ForwardKeyEvent(wc, blink::WebInputEvent::Type::kKeyDown, bs_info, 0);

    content::GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AbpInputDispatcher::ClearTextKeyUp,
                       base::Unretained(this), ctx, total_sent + 1,
                       batch_count + 1, std::move(previous_value)),
        base::Milliseconds(kKeyDwellMs));
    return;
  }

  // Batch of 20 done — check if input is cleared via Runtime.evaluate
  AbpCdpClient* cdp_client = ctx->client();
  if (!cdp_client) {
    ctx->OnActionError("CDP_ERROR", "CDP client lost");
    return;
  }

  base::Value::Dict eval_params;
  eval_params.Set("expression",
      "(function(){var e=document.activeElement;if(!e)return '';"
      "if(typeof e.value==='string')return e.value;"
      "return e.textContent||'';})()");
  eval_params.Set("returnByValue", true);
  eval_params.Set("disableBreaks", true);

  cdp_client->SendCommand(
      "Runtime.evaluate", std::move(eval_params),
      base::BindOnce(
          [](scoped_refptr<AbpActionContext> ctx, int total_sent,
             std::string prev_value, AbpInputDispatcher* dispatcher,
             bool success, const std::string& result) {
            if (!success) {
              ctx->OnActionError("CDP_ERROR", result);
              return;
            }

            std::string current_value = ParseEvalStringResult(result);

            if (current_value.empty()) {
              // Input is cleared
              base::Value::Dict res;
              res.Set("status", "cleared");
              res.Set("keystrokes_sent", total_sent);
              ctx->SetResult(std::move(res));
              ctx->OnActionDispatched();
              return;
            }

            if (current_value == prev_value) {
              // Value didn't change — element doesn't respond to backspace
              base::Value::Dict res;
              res.Set("status", "cleared");
              res.Set("keystrokes_sent", total_sent);
              ctx->SetResult(std::move(res));
              ctx->OnActionDispatched();
              return;
            }

            // Value changed but not empty — send another batch
            dispatcher->ClearTextBatch(ctx, total_sent,
                                       std::move(current_value));
          },
          ctx, total_sent, std::move(previous_value), this));
}
```

**Step 7: Commit**

```bash
git add chrome/browser/abp/abp_input_dispatcher.cc
git commit -m "feat(abp): implement ClearText backspace loop in input dispatcher"
```

---

### Task 3: Add ClearText to AbpController

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:482` (after Slider declaration)
- Modify: `chrome/browser/abp/abp_controller.cc:1990` (REST routing)
- Modify: `chrome/browser/abp/abp_controller.cc:2835` (after Slider forwarder)

**Step 1: Add declaration in header after Slider (line 482)**

```cpp
  void ClearText(const std::string& tab_id,
                 const base::Value::Dict& params,
                 ResponseCallback callback);
```

**Step 2: Add REST routing after "slider" case (line 1990)**

```cpp
      } else if (action == "clear_text") {
        ClearText(tab_id, params, std::move(callback));
```

**Step 3: Add thin forwarder after Slider() (line 2835)**

```cpp
void AbpController::ClearText(const std::string& tab_id,
                               const base::Value::Dict& params,
                               ResponseCallback callback) {
  input_dispatcher_->ClearText(tab_id, params, std::move(callback));
}
```

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): add ClearText routing to controller"
```

---

### Task 4: Add browser_clear_text MCP tool

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.h:99` (after CallBrowserSlider)
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:507` (tool schema, after browser_slider)
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:848` (dispatch routing, after browser_slider)
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:1496` (Call implementation, after CallBrowserSlider)
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:579,586` (system prompt tool reference)

**Step 1: Add declaration in header after CallBrowserSlider (line 99)**

```cpp
  void CallBrowserClearText(const base::Value::Dict& args,
                            base::Value request_id,
                            ResponseWithHeadersCallback callback);
```

**Step 2: Add tool schema after browser_slider definition (after line 507)**

```cpp
  // 16. browser_clear_text — clear focused input via backspace
  tools.Append(
      ToolBuilder("browser_clear_text")
          .Description(
              "Clear the text content of an input element by clicking to "
              "focus it, then sending repeated Backspace keystrokes. Sends "
              "batches of 20 keystrokes at 240 WPM, checking after each "
              "batch whether the input is empty. Automatically stops if the "
              "input is already empty, if backspace has no effect, or after "
              "10,000 keystrokes. Use this when you need to clear an input "
              "field before typing new text.")
          .OptionalString("tab_id", "Target tab ID")
          .RequiredNumber("x",
              "X coordinate of the center of the input field")
          .RequiredNumber("y",
              "Y coordinate of the center of the input field")
          .Build());
```

Note: renumber `respond_to_permission` from 16 to 17.

**Step 3: Add dispatch routing after browser_slider case (line 848)**

```cpp
  } else if (*name == "browser_clear_text") {
    CallBrowserClearText(*args, std::move(request_id), std::move(callback));
```

**Step 4: Add Call implementation after CallBrowserSlider (after line 1496)**

```cpp
// --- 16. browser_clear_text: clear input via backspace ---
void AbpMcpHandler::CallBrowserClearText(
    const base::Value::Dict& args,
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
      "POST", "/api/v1/tabs/" + tab_id + "/clear_text", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}
```

**Step 5: Update system prompt tool reference (line 579, 586)**

Change `Tool Reference (16 tools)` to `Tool Reference (17 tools)`.

After the `browser_slider` bullet (line 586), add:
```
- `browser_clear_text` — x, y (center of input). Clicks to focus, then sends Backspace keystrokes in batches of 20 at 240 WPM until empty. Stops early if already empty or if backspace has no effect. Max 10,000 keystrokes.
```

**Step 6: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.h chrome/browser/abp/abp_mcp_handler.cc
git commit -m "feat(abp): add browser_clear_text MCP tool"
```

---

### Task 5: Build and verify

**Step 1: Build**

```bash
autoninja -C out/Default chrome
```

Expected: Build succeeds with no errors.

**Step 2: Commit if any fixes needed**

---

### Task 6: Manual test

**Step 1: Launch browser**

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run
```

**Step 2: Navigate to a page with input fields**

Use MCP `browser_navigate` to go to a page with text inputs (e.g., a search page or form).

**Step 3: Type text into an input**

Use MCP `browser_action` with `keyboard_type` to enter text in an input field.

**Step 4: Call browser_clear_text**

Call `browser_clear_text` with the `x`, `y` coordinates of the input's center. Verify:
- The input gets clicked (focused)
- Text is cleared
- Response contains `{"status": "cleared", "keystrokes_sent": N}` where N is reasonable
- After screenshot shows empty input

**Step 5: Test with long text**

Type a long string (50+ characters), then clear. Verify it takes multiple batches and clears fully.

**Step 6: Test with empty input**

Call `browser_clear_text` on an already-empty input. Verify it sends only 1 batch (20 keystrokes) then exits.

**Step 7: Test stuck detection**

Click on a non-editable element (e.g., a `<span>`) and call `browser_clear_text`. Verify it exits after 1 batch since the value doesn't change.
