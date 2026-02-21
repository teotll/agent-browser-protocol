# Browser Slider Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a `slider` REST endpoint and MCP tool that calculates drag coordinates from logical slider values and delegates to existing drag infrastructure.

**Architecture:** Thin wrapper in `AbpInputDispatcher` that parses discriminated union params (horizontal/vertical), performs linear interpolation to compute target pixel position, then calls `DragRaw()`. Follows exact same patterns as existing `Drag()`/`DragRaw()`.

**Tech Stack:** C++ (Chromium), CDP Input.dispatchMouseEvent, InProcessBrowserTest

**Design doc:** `docs/plans/2026-02-20-browser-slider-design.md`

---

### Task 1: Add Slider() and SliderRaw() declarations to header

**Files:**
- Modify: `chrome/browser/abp/abp_input_dispatcher.h:81-113`

**Step 1: Add Slider() declaration after Drag()**

After line 84 (`ResponseCallback callback);` closing Drag), add:

```cpp
  // Slider: calculate target position from logical value and drag to it
  void Slider(const std::string& tab_id,
              const base::Value::Dict& params,
              ResponseCallback callback);
```

**Step 2: Add SliderRaw() declaration after DragRaw()**

After line 113 (`RawCallback callback);` closing DragRaw), add:

```cpp
  void SliderRaw(const std::string& tab_id,
                 const base::Value::Dict& params,
                 RawCallback callback);
```

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_input_dispatcher.h
git commit -m "feat(slider): add Slider/SliderRaw declarations to input dispatcher"
```

---

### Task 2: Implement SliderRaw() — the core interpolation + delegation

**Files:**
- Modify: `chrome/browser/abp/abp_input_dispatcher.cc` (add after `DragRaw` at ~line 1511)

**Step 1: Implement SliderRaw()**

Add after the `DragNextStepRaw` method (find the end of DragRaw-related code). This is the core logic — parse discriminated params, interpolate, build drag params, delegate:

```cpp
void AbpInputDispatcher::SliderRaw(const std::string& tab_id,
                                   const base::Value::Dict& params,
                                   RawCallback callback) {
  const std::string* orientation = params.FindString("orientation");
  if (!orientation || (*orientation != "horizontal" && *orientation != "vertical")) {
    // Can't send error in Raw mode, just complete
    std::move(callback).Run();
    return;
  }

  auto min_val = params.FindDouble("min");
  auto max_val = params.FindDouble("max");
  auto target = params.FindDouble("target_value");
  if (!min_val || !max_val || !target) {
    std::move(callback).Run();
    return;
  }
  if (*min_val >= *max_val) {
    std::move(callback).Run();
    return;
  }

  double ratio = (*target - *min_val) / (*max_val - *min_val);

  base::Value::Dict drag_params;
  drag_params.Set("steps", 10);

  if (*orientation == "horizontal") {
    auto y = params.FindDouble("y");
    auto x_start = params.FindDouble("x_start");
    auto x_end = params.FindDouble("x_end");
    auto current_x = params.FindDouble("current_x");
    if (!y || !x_start || !x_end || !current_x) {
      std::move(callback).Run();
      return;
    }
    double target_x = *x_start + ratio * (*x_end - *x_start);
    target_x = std::clamp(target_x, std::min(*x_start, *x_end),
                          std::max(*x_start, *x_end));
    drag_params.Set("start_x", *current_x);
    drag_params.Set("start_y", *y);
    drag_params.Set("end_x", target_x);
    drag_params.Set("end_y", *y);
  } else {
    auto x = params.FindDouble("x");
    auto y_start = params.FindDouble("y_start");
    auto y_end = params.FindDouble("y_end");
    auto current_y = params.FindDouble("current_y");
    if (!x || !y_start || !y_end || !current_y) {
      std::move(callback).Run();
      return;
    }
    double target_y = *y_start + ratio * (*y_end - *y_start);
    target_y = std::clamp(target_y, std::min(*y_start, *y_end),
                          std::max(*y_start, *y_end));
    drag_params.Set("start_x", *x);
    drag_params.Set("start_y", *current_y);
    drag_params.Set("end_x", *x);
    drag_params.Set("end_y", target_y);
  }

  DragRaw(tab_id, drag_params, std::move(callback));
}
```

**Step 2: Commit**

```bash
git add chrome/browser/abp/abp_input_dispatcher.cc
git commit -m "feat(slider): implement SliderRaw with interpolation and drag delegation"
```

---

### Task 3: Implement Slider() — the action-context wrapper

**Files:**
- Modify: `chrome/browser/abp/abp_input_dispatcher.cc` (add after `Drag()` method at ~line 987)

**Step 1: Implement Slider()**

This follows the exact pattern of `Drag()` — parse, validate, then wrap in `AbpActionContext::Run`. The validation is richer here because we can send error responses.

```cpp
void AbpInputDispatcher::Slider(const std::string& tab_id,
                                const base::Value::Dict& params,
                                ResponseCallback callback) {
  const std::string* orientation = params.FindString("orientation");
  if (!orientation ||
      (*orientation != "horizontal" && *orientation != "vertical")) {
    controller_->SendError(
        400, "orientation must be 'horizontal' or 'vertical'",
        std::move(callback));
    return;
  }

  auto min_val = params.FindDouble("min");
  auto max_val = params.FindDouble("max");
  auto target = params.FindDouble("target_value");
  if (!min_val || !max_val || !target) {
    controller_->SendError(
        400, "Missing required parameter: min, max, target_value",
        std::move(callback));
    return;
  }
  if (*min_val >= *max_val) {
    controller_->SendError(400, "min must be less than max",
                           std::move(callback));
    return;
  }
  if (*target < *min_val || *target > *max_val) {
    controller_->SendError(400, "target_value must be between min and max",
                           std::move(callback));
    return;
  }

  double ratio = (*target - *min_val) / (*max_val - *min_val);
  double start_x, start_y, end_x, end_y;

  if (*orientation == "horizontal") {
    auto y = params.FindDouble("y");
    auto x_start = params.FindDouble("x_start");
    auto x_end = params.FindDouble("x_end");
    auto current_x = params.FindDouble("current_x");
    if (!y || !x_start || !x_end || !current_x) {
      controller_->SendError(
          400,
          "horizontal orientation requires y, x_start, x_end, current_x",
          std::move(callback));
      return;
    }
    if (*x_start == *x_end) {
      controller_->SendError(400, "x_start and x_end must be different",
                             std::move(callback));
      return;
    }
    double lo = std::min(*x_start, *x_end);
    double hi = std::max(*x_start, *x_end);
    if (*current_x < lo || *current_x > hi) {
      controller_->SendError(
          400, "current_x must be within track bounds (x_start to x_end)",
          std::move(callback));
      return;
    }
    double target_x = *x_start + ratio * (*x_end - *x_start);
    target_x = std::clamp(target_x, lo, hi);
    start_x = *current_x;
    start_y = *y;
    end_x = target_x;
    end_y = *y;
  } else {
    auto x = params.FindDouble("x");
    auto y_start = params.FindDouble("y_start");
    auto y_end = params.FindDouble("y_end");
    auto current_y = params.FindDouble("current_y");
    if (!x || !y_start || !y_end || !current_y) {
      controller_->SendError(
          400,
          "vertical orientation requires x, y_start, y_end, current_y",
          std::move(callback));
      return;
    }
    if (*y_start == *y_end) {
      controller_->SendError(400, "y_start and y_end must be different",
                             std::move(callback));
      return;
    }
    double lo = std::min(*y_start, *y_end);
    double hi = std::max(*y_start, *y_end);
    if (*current_y < lo || *current_y > hi) {
      controller_->SendError(
          400, "current_y must be within track bounds (y_start to y_end)",
          std::move(callback));
      return;
    }
    double target_y = *y_start + ratio * (*y_end - *y_start);
    target_y = std::clamp(target_y, lo, hi);
    start_x = *x;
    start_y = *current_y;
    end_x = *x;
    end_y = target_y;
  }

  // Build drag params and delegate to Drag()
  base::Value::Dict drag_params;
  drag_params.Set("start_x", start_x);
  drag_params.Set("start_y", start_y);
  drag_params.Set("end_x", end_x);
  drag_params.Set("end_y", end_y);
  drag_params.Set("steps", 10);

  // Use Drag() which handles AbpActionContext lifecycle
  Drag(tab_id, drag_params, std::move(callback));
}
```

Note: `Slider()` delegates to `Drag()` (not `DragRaw()`) because `Drag()` already wraps everything in an `AbpActionContext` with resume/pause/screenshot lifecycle. No need to duplicate that.

**Step 2: Commit**

```bash
git add chrome/browser/abp/abp_input_dispatcher.cc
git commit -m "feat(slider): implement Slider() with full validation and drag delegation"
```

---

### Task 4: Add controller routing and wrapper

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:424-426`
- Modify: `chrome/browser/abp/abp_controller.cc:1944-1945` (route) and `2686-2690` (wrapper)

**Step 1: Add declaration in controller header**

After the `Drag()` declaration (line 426), add:

```cpp
  void Slider(const std::string& tab_id,
              const base::Value::Dict& params,
              ResponseCallback callback);
```

**Step 2: Add route in HandleRequest**

After line 1945 (`Drag(tab_id, params, std::move(callback));`), add:

```cpp
    } else if (action == "slider") {
      Slider(tab_id, params, std::move(callback));
```

**Step 3: Add wrapper method**

After the `Drag()` wrapper (line 2690), add:

```cpp
void AbpController::Slider(const std::string& tab_id,
                           const base::Value::Dict& params,
                           ResponseCallback callback) {
  input_dispatcher_->Slider(tab_id, params, std::move(callback));
}
```

**Step 4: Add batch dispatch routing**

In the `DispatchBatchAction` function, after line 2784 (`dispatcher->DragRaw(tab_id, params, std::move(dispatch_next));`), add:

```cpp
  } else if (*type == "mouse_slider") {
    dispatcher->SliderRaw(tab_id, params, std::move(dispatch_next));
```

**Step 5: Add batch validation**

In `HandleBatchRequest`, after the `mouse_drag` validation block (after line 2884), add:

```cpp
    } else if (*type == "mouse_slider") {
      const std::string* orient = action.FindString("orientation");
      if (!orient || (*orient != "horizontal" && *orient != "vertical")) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: mouse_slider orientation must be 'horizontal' or 'vertical'"})",
                i));
        return;
      }
      auto min_v = action.FindDouble("min");
      auto max_v = action.FindDouble("max");
      auto tgt = action.FindDouble("target_value");
      if (!min_v || !max_v || !tgt) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: mouse_slider requires min, max, target_value"})",
                i));
        return;
      }
      if (*min_v >= *max_v) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: mouse_slider min must be less than max"})",
                i));
        return;
      }
```

**Step 6: Update the unknown-type error message**

At line 2950, update the error string to include `mouse_slider`:

```cpp
R"({"error":"action %zu: unknown type '%s'. Valid: mouse_click, keyboard_type, keyboard_press, mouse_hover, mouse_drag, mouse_slider"})",
```

**Step 7: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "feat(slider): add REST route and batch dispatch for slider endpoint"
```

---

### Task 5: Add MCP tool schema

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:179-200`

**Step 1: Add mouse_slider variant to oneOf**

After the `mouse_drag` variant block (after line 200, before line 202 `// -- actions array`), add:

```cpp
    // mouse_slider: slider macro with orientation-discriminated params
    {
      base::Value::Dict variant;
      variant.Set("type", "object");
      base::Value::Dict props;
      props.Set("type", make_const_type("mouse_slider"));

      base::Value::Dict orient_prop;
      orient_prop.Set("type", "string");
      base::Value::List orient_enum;
      orient_enum.Append("horizontal");
      orient_enum.Append("vertical");
      orient_prop.Set("enum", std::move(orient_enum));
      orient_prop.Set("description",
          "Slider orientation. Use 'horizontal' with y, x_start, x_end, "
          "current_x. Use 'vertical' with x, y_start, y_end, current_y.");
      props.Set("orientation", std::move(orient_prop));

      props.Set("y", num_prop.Clone());
      props.Set("x_start", num_prop.Clone());
      props.Set("x_end", num_prop.Clone());
      props.Set("current_x", num_prop.Clone());
      props.Set("x", num_prop.Clone());
      props.Set("y_start", num_prop.Clone());
      props.Set("y_end", num_prop.Clone());
      props.Set("current_y", num_prop.Clone());
      props.Set("min", num_prop.Clone());
      props.Set("max", num_prop.Clone());
      props.Set("target_value", num_prop.Clone());

      variant.Set("properties", std::move(props));
      base::Value::List req;
      req.Append("type");
      req.Append("orientation");
      req.Append("min");
      req.Append("max");
      req.Append("target_value");
      variant.Set("required", std::move(req));
      variant.Set("additionalProperties", false);
      one_of.Append(std::move(variant));
    }
```

Note: All axis-specific fields (y, x_start, x_end, current_x, x, y_start, y_end, current_y) are listed but not required at schema level — the C++ validation in `Slider()` enforces which ones are needed based on orientation. This avoids JSON Schema `oneOf`-within-`oneOf` complexity that MCP clients may not handle well.

**Step 2: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.cc
git commit -m "feat(slider): add mouse_slider to MCP browser_action tool schema"
```

---

### Task 6: Add test page

**Files:**
- Create: `chrome/browser/abp/test_pages/slider_test.html`

**Step 1: Create test page**

```html
<!DOCTYPE html>
<html><body>
  <div style="margin:50px;">
    <label>Horizontal: <span id="h-val">50</span></label><br>
    <input type="range" id="h-slider" min="0" max="100" value="50"
           style="width:400px;" oninput="document.getElementById('h-val').textContent=this.value">
  </div>
  <div style="margin:50px;">
    <label>Vertical: <span id="v-val">50</span></label><br>
    <input type="range" id="v-slider" min="0" max="100" value="50"
           orient="vertical" style="height:400px;writing-mode:bt-lr;-webkit-appearance:slider-vertical;"
           oninput="document.getElementById('v-val').textContent=this.value">
  </div>
</body></html>
```

**Step 2: Commit**

```bash
git add chrome/browser/abp/test_pages/slider_test.html
git commit -m "feat(slider): add slider test page"
```

---

### Task 7: Add browser tests

**Files:**
- Modify: `chrome/browser/abp/abp_action_lifecycle_browsertest.cc`

**Step 1: Add horizontal slider test**

Add at the end of the test file, before the closing namespace:

```cpp
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, SliderHorizontal) {
  std::string tab_id = CreateTabAndNavigate("/slider_test.html");
  ASSERT_FALSE(tab_id.empty());

  base::Value::Dict body;
  body.Set("orientation", "horizontal");
  body.Set("y", 75);
  body.Set("x_start", 50);
  body.Set("x_end", 450);
  body.Set("current_x", 250);
  body.Set("min", 0.0);
  body.Set("max", 100.0);
  body.Set("target_value", 75.0);
  auto result = SendAction(tab_id, "slider", std::move(body));
  EXPECT_EQ(result.status, 200);
}

IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, SliderVertical) {
  std::string tab_id = CreateTabAndNavigate("/slider_test.html");
  ASSERT_FALSE(tab_id.empty());

  base::Value::Dict body;
  body.Set("orientation", "vertical");
  body.Set("x", 75);
  body.Set("y_start", 150);
  body.Set("y_end", 550);
  body.Set("current_y", 350);
  body.Set("min", 0.0);
  body.Set("max", 100.0);
  body.Set("target_value", 25.0);
  auto result = SendAction(tab_id, "slider", std::move(body));
  EXPECT_EQ(result.status, 200);
}
```

**Step 2: Add validation error tests**

```cpp
IN_PROC_BROWSER_TEST_F(AbpActionLifecycleTest, SliderValidationErrors) {
  std::string tab_id = CreateTabAndNavigate("/slider_test.html");
  ASSERT_FALSE(tab_id.empty());

  // Missing orientation
  {
    base::Value::Dict body;
    body.Set("y", 75);
    body.Set("x_start", 50);
    body.Set("x_end", 450);
    body.Set("current_x", 250);
    body.Set("min", 0.0);
    body.Set("max", 100.0);
    body.Set("target_value", 50.0);
    auto result = SendAction(tab_id, "slider", std::move(body));
    EXPECT_EQ(result.status, 400);
  }

  // min >= max
  {
    base::Value::Dict body;
    body.Set("orientation", "horizontal");
    body.Set("y", 75);
    body.Set("x_start", 50);
    body.Set("x_end", 450);
    body.Set("current_x", 250);
    body.Set("min", 100.0);
    body.Set("max", 0.0);
    body.Set("target_value", 50.0);
    auto result = SendAction(tab_id, "slider", std::move(body));
    EXPECT_EQ(result.status, 400);
  }

  // target_value out of range
  {
    base::Value::Dict body;
    body.Set("orientation", "horizontal");
    body.Set("y", 75);
    body.Set("x_start", 50);
    body.Set("x_end", 450);
    body.Set("current_x", 250);
    body.Set("min", 0.0);
    body.Set("max", 100.0);
    body.Set("target_value", 150.0);
    auto result = SendAction(tab_id, "slider", std::move(body));
    EXPECT_EQ(result.status, 400);
  }

  // Missing horizontal fields
  {
    base::Value::Dict body;
    body.Set("orientation", "horizontal");
    body.Set("min", 0.0);
    body.Set("max", 100.0);
    body.Set("target_value", 50.0);
    auto result = SendAction(tab_id, "slider", std::move(body));
    EXPECT_EQ(result.status, 400);
  }
}
```

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_action_lifecycle_browsertest.cc
git commit -m "feat(slider): add browser tests for slider endpoint"
```

---

### Task 8: Build and verify

**Step 1: Build**

```bash
cd /Users/hanwang/src/src
autoninja -C out/Default browser_tests
```

Expected: Build succeeds with no errors.

**Step 2: Run tests**

```bash
./out/Default/browser_tests --gtest_filter='AbpActionLifecycleTest.Slider*'
```

Expected: All 3 tests pass (SliderHorizontal, SliderVertical, SliderValidationErrors).

**Step 3: Commit (if any fixes needed)**

Fix any compilation or test failures, then commit fixes.
