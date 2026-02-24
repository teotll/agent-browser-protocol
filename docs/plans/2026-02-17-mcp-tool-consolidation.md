# MCP Tool Consolidation (33 to 12) Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Consolidate 33 MCP tools to 12, adding a batched `browser_action` tool with a backing REST `/batch` endpoint.

**Architecture:** New `POST /api/v1/tabs/{id}/batch` REST endpoint executes 1-3 input actions within a single `AbpActionContext` lifecycle with 20ms delays between actions. MCP handler rewired from 33 tools to 12, with `browser_action` delegating to the batch endpoint. Key normalization validates and canonicalizes keyboard input.

**Tech Stack:** C++ (Chromium), `base::Value`, `AbpActionContext`, `AbpInputDispatcher`, `AbpToolBuilder`

**Design doc:** `plans/mcp-tool-consolidation.md`

---

## Task 1: Add NormalizeKey utility to AbpController

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc:384-501` (key mapping area)
- Modify: `chrome/browser/abp/abp_controller.h`

**Step 1: Add NormalizeKey declaration to header**

In `abp_controller.h`, add a free function declaration near the top (before the class), or as a static method. Since `GetKeyInfo()` and `ModifiersToFlags()` are already free functions in the .cc file, add alongside them:

```cpp
// In abp_controller.h, add near the top with other forward declarations:
// Key normalization for batch endpoint — uppercases and expands abbreviations
// Returns std::nullopt if the key is not in the valid set.
std::optional<std::string> NormalizeKey(const std::string& input);
```

**Step 2: Implement NormalizeKey in abp_controller.cc**

Add after the existing `ModifiersToFlags()` function (around line 502). This function uppercases the input, expands abbreviations (CTRL->CONTROL, CMD->META, etc.), and validates against the known key set.

```cpp
namespace {

const base::flat_map<std::string, std::string>& GetKeyAbbreviations() {
  static const base::NoDestructor<base::flat_map<std::string, std::string>> kMap({
      {"CTRL", "CONTROL"},
      {"\xe2\x8c\x83", "CONTROL"},  // ⌃
      {"CMD", "META"},
      {"COMMAND", "META"},
      {"\xe2\x8c\x98", "META"},     // ⌘
      {"OPT", "ALT"},
      {"OPTION", "ALT"},
      {"\xe2\x8c\xa5", "ALT"},      // ⌥
      {"\xe2\x87\xa7", "SHIFT"},     // ⇧
      {"ESC", "ESCAPE"},
      {"DEL", "DELETE"},
      {"BS", "BACKSPACE"},
      {"CR", "ENTER"},
      {"RETURN", "ENTER"},
      {"INS", "INSERT"},
      {"PGUP", "PAGEUP"},
      {"PGDN", "PAGEDOWN"},
      {"PGDOWN", "PAGEDOWN"},
      {"UP", "ARROWUP"},
      {"DOWN", "ARROWDOWN"},
      {"LEFT", "ARROWLEFT"},
      {"RIGHT", "ARROWRIGHT"},
  });
  return *kMap;
}

const base::flat_set<std::string>& GetValidKeys() {
  static const base::NoDestructor<base::flat_set<std::string>> kSet([] {
    base::flat_set<std::string> s;
    // Letters A-Z
    for (char c = 'A'; c <= 'Z'; c++)
      s.insert(std::string(1, c));
    // Digits 0-9
    for (char c = '0'; c <= '9'; c++)
      s.insert(std::string(1, c));
    // Function keys F1-F24
    for (int i = 1; i <= 24; i++)
      s.insert("F" + base::NumberToString(i));
    // Navigation
    for (const char* k : {"ARROWUP", "ARROWDOWN", "ARROWLEFT", "ARROWRIGHT",
                          "HOME", "END", "PAGEUP", "PAGEDOWN"})
      s.insert(k);
    // Editing
    for (const char* k : {"BACKSPACE", "DELETE", "INSERT", "ENTER", "TAB",
                          "ESCAPE", "SPACE"})
      s.insert(k);
    // Modifiers
    for (const char* k : {"SHIFT", "CONTROL", "ALT", "META"})
      s.insert(k);
    // Symbols
    for (const char* k : {"COMMA", "PERIOD", "SLASH", "BACKSLASH",
                          "SEMICOLON", "QUOTE", "BRACKETLEFT",
                          "BRACKETRIGHT", "MINUS", "EQUAL", "BACKQUOTE"})
      s.insert(k);
    return s;
  }());
  return *kSet;
}

}  // namespace

std::optional<std::string> NormalizeKey(const std::string& input) {
  std::string upper = base::ToUpperASCII(input);

  // Check abbreviation map
  const auto& abbrevs = GetKeyAbbreviations();
  auto it = abbrevs.find(upper);
  if (it != abbrevs.end()) {
    upper = it->second;
  }

  // Also check non-uppercased input for Unicode symbols (⌘, ⌥, etc.)
  if (upper == input) {
    auto sym_it = abbrevs.find(input);
    if (sym_it != abbrevs.end()) {
      upper = sym_it->second;
    }
  }

  // Validate
  if (GetValidKeys().contains(upper)) {
    return upper;
  }
  return std::nullopt;
}
```

**Step 3: Build and verify compilation**

Run: `autoninja -C out/Default chrome/browser/abp:abp`
Expected: Compiles without errors

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc chrome/browser/abp/abp_controller.h
git commit -m "feat(abp): add NormalizeKey utility for keyboard input validation"
```

---

## Task 2: Add batch REST endpoint route and HandleBatchRequest

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:406-429`
- Modify: `chrome/browser/abp/abp_controller.cc`
- Modify: `chrome/browser/abp/abp_http_server.cc:259-289`

**Step 1: Add HandleBatchRequest declaration to header**

In `abp_controller.h`, add near the other Handle methods:

```cpp
void HandleBatchRequest(const std::string& tab_id,
                        const base::Value::Dict& params,
                        ResponseCallback callback);
```

**Step 2: Add route in abp_http_server.cc**

The HTTP server routes everything under `/api/v1/` to `controller_->HandleRequest()` by default (line 289). The controller's `HandleRequest` router (abp_controller.cc:1728-1990) dispatches by path segments. Add the batch route in the controller's `HandleRequest` method, in the tab action dispatch section (around line 1820), alongside `click`, `type`, `move`, etc.:

```cpp
} else if (action == "batch") {
  // Batch endpoint - parse body params already available
  HandleBatchRequest(tab_id, params, std::move(callback));
```

**Step 3: Implement HandleBatchRequest in abp_controller.cc**

Add the method after the existing input handlers (around line 2590). This validates all actions upfront, then delegates to `ExecuteBatchActions`.

```cpp
void AbpController::HandleBatchRequest(
    const std::string& tab_id,
    const base::Value::Dict& params,
    ResponseCallback callback) {
  const base::Value::List* actions = params.FindList("actions");
  if (!actions || actions->empty()) {
    std::move(callback).Run(
        400, "application/json",
        R"({"error":"'actions' array is required and must not be empty"})");
    return;
  }
  if (actions->size() > 3) {
    std::move(callback).Run(
        400, "application/json",
        R"({"error":"'actions' array must have at most 3 elements"})");
    return;
  }

  // Get viewport size for coordinate validation
  auto* web_contents = GetWebContentsForTab(tab_id);
  if (!web_contents) {
    std::move(callback).Run(
        404, "application/json",
        R"({"error":"Tab not found"})");
    return;
  }
  gfx::Size viewport = web_contents->GetContainerBounds().size();

  // Validate all actions upfront
  base::Value::List validated_actions;
  for (size_t i = 0; i < actions->size(); i++) {
    if (!(*actions)[i].is_dict()) {
      std::move(callback).Run(
          400, "application/json",
          base::StringPrintf(R"({"error":"action %zu: must be an object"})", i));
      return;
    }
    const base::Value::Dict& action = (*actions)[i].GetDict();
    const std::string* type = action.FindString("type");
    if (!type) {
      std::move(callback).Run(
          400, "application/json",
          base::StringPrintf(R"({"error":"action %zu: missing 'type'"})", i));
      return;
    }

    base::Value::Dict validated = action.Clone();

    if (*type == "mouse_click" || *type == "mouse_hover") {
      auto x = action.FindDouble("x");
      auto y = action.FindDouble("y");
      if (!x || !y) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: %s requires 'x' and 'y'"})",
                i, type->c_str()));
        return;
      }
      if (*x < 0 || *x >= viewport.width() || *y < 0 || *y >= viewport.height()) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: %s coordinates (%.0f, %.0f) outside viewport (%dx%d)"})",
                i, type->c_str(), *x, *y, viewport.width(), viewport.height()));
        return;
      }
    } else if (*type == "mouse_drag") {
      auto sx = action.FindDouble("start_x");
      auto sy = action.FindDouble("start_y");
      auto ex = action.FindDouble("end_x");
      auto ey = action.FindDouble("end_y");
      if (!sx || !sy || !ex || !ey) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: mouse_drag requires start_x, start_y, end_x, end_y"})", i));
        return;
      }
      // Validate both start and end coordinates
      if (*sx < 0 || *sx >= viewport.width() || *sy < 0 || *sy >= viewport.height() ||
          *ex < 0 || *ex >= viewport.width() || *ey < 0 || *ey >= viewport.height()) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: mouse_drag coordinates outside viewport (%dx%d)"})",
                i, viewport.width(), viewport.height()));
        return;
      }
    } else if (*type == "keyboard_type") {
      if (!action.FindString("text")) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: keyboard_type requires 'text'"})", i));
        return;
      }
    } else if (*type == "keyboard_press") {
      const std::string* key = action.FindString("key");
      if (!key) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: keyboard_press requires 'key'"})", i));
        return;
      }
      // Normalize key
      auto normalized = NormalizeKey(*key);
      if (!normalized) {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: keyboard_press invalid key '%s'"})",
                i, key->c_str()));
        return;
      }
      validated.Set("key", *normalized);

      // Normalize modifiers if present
      const base::Value::List* mods = action.FindList("modifiers");
      if (mods) {
        base::Value::List normalized_mods;
        for (const auto& mod : *mods) {
          if (!mod.is_string()) continue;
          auto norm_mod = NormalizeKey(mod.GetString());
          if (!norm_mod) {
            std::move(callback).Run(
                400, "application/json",
                base::StringPrintf(
                    R"({"error":"action %zu: keyboard_press invalid modifier '%s'"})",
                    i, mod.GetString().c_str()));
            return;
          }
          normalized_mods.Append(*norm_mod);
        }
        validated.Set("modifiers", std::move(normalized_mods));
      }

      // Normalize action param if present
      const std::string* key_action = action.FindString("action");
      if (key_action && *key_action != "press" && *key_action != "down" && *key_action != "up") {
        std::move(callback).Run(
            400, "application/json",
            base::StringPrintf(
                R"({"error":"action %zu: keyboard_press action must be press/down/up"})", i));
        return;
      }
    } else {
      std::move(callback).Run(
          400, "application/json",
          base::StringPrintf(
              R"({"error":"action %zu: unknown type '%s'. Valid: mouse_click, keyboard_type, keyboard_press, mouse_hover, mouse_drag"})",
              i, type->c_str()));
      return;
    }

    validated_actions.Append(std::move(validated));
  }

  // Extract screenshot config
  const base::Value::Dict* screenshot_config = params.FindDict("screenshot");
  base::Value::Dict sc_copy;
  if (screenshot_config) {
    sc_copy = screenshot_config->Clone();
  }

  // All validation passed — start batch execution within a single action context
  ExecuteBatchActions(tab_id, std::move(validated_actions), 0,
                      std::move(sc_copy), params.Clone(), std::move(callback));
}
```

**Step 4: Build and verify compilation**

Run: `autoninja -C out/Default chrome/browser/abp:abp`
Expected: Compiles (ExecuteBatchActions not yet implemented, so will fail — that's Task 3)

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_http_server.cc
git commit -m "feat(abp): add batch endpoint validation and routing"
```

---

## Task 3: Implement ExecuteBatchActions with 20ms inter-action delay

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h`
- Modify: `chrome/browser/abp/abp_controller.cc`

**Step 1: Add ExecuteBatchActions declaration to header**

```cpp
void ExecuteBatchActions(const std::string& tab_id,
                         base::Value::List actions,
                         int current_index,
                         base::Value::Dict screenshot_config,
                         base::Value::Dict original_params,
                         ResponseCallback callback);
```

**Step 2: Implement ExecuteBatchActions**

This uses `AbpActionContext::Run()` for the first action to get the lifecycle (pause/resume/screenshot), then chains subsequent actions with 20ms delays within the same action context. The key insight is that the first action uses the normal action context lifecycle, and subsequent actions are dispatched as PostDelayedTasks within the action callback.

However, looking at the codebase, each input method (Click, Type, etc.) creates its own `AbpActionContext`. For batching, we need a different approach: create ONE action context that dispatches all actions sequentially.

```cpp
void AbpController::ExecuteBatchActions(
    const std::string& tab_id,
    base::Value::List actions,
    int current_index,
    base::Value::Dict screenshot_config,
    base::Value::Dict original_params,
    ResponseCallback callback) {

  // Build combined params for action context
  base::Value::Dict batch_params;
  batch_params.Set("actions", actions.Clone());
  if (!screenshot_config.empty()) {
    batch_params.Set("screenshot", screenshot_config.Clone());
  }

  // Use AbpActionContext for the entire batch
  AbpActionContext::Run(
      this, tab_id, "batch", batch_params,
      // Action callback — this runs after execution is resumed
      base::BindOnce(
          [](base::Value::List actions, int start_index,
             std::string tab_id, AbpInputDispatcher* dispatcher,
             AbpActionContext* ctx) {
            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            // Dispatch all actions with 20ms delays
            DispatchBatchAction(std::move(actions), start_index,
                               std::move(tab_id), dispatcher, ctx_ref);
          },
          std::move(actions), current_index, tab_id,
          input_dispatcher_.get()),
      std::move(callback));
}
```

**Step 3: Implement DispatchBatchAction as a static helper**

This is the recursive function that dispatches one action, then schedules the next after 20ms:

```cpp
namespace {

void DispatchBatchAction(base::Value::List actions,
                         int index,
                         std::string tab_id,
                         AbpInputDispatcher* dispatcher,
                         scoped_refptr<AbpActionContext> ctx) {
  if (index >= static_cast<int>(actions.size())) {
    // All actions dispatched
    base::Value::Dict result;
    result.Set("actions_executed", static_cast<int>(actions.size()));
    ctx->SetResult(std::move(result));
    ctx->OnActionDispatched();
    return;
  }

  const base::Value::Dict& action = actions[index].GetDict();
  const std::string* type = action.FindString("type");

  // Build params dict for the dispatcher (exclude "type" field)
  base::Value::Dict params = action.Clone();
  params.Remove("type");

  // For keyboard_press with action param, route to press/down/up
  if (*type == "keyboard_press") {
    const std::string* key_action = params.FindString("action");
    std::string actual_action = key_action ? *key_action : "press";
    params.Remove("action");

    auto dispatch_next = base::BindOnce(
        [](base::Value::List actions, int next_index, std::string tab_id,
           AbpInputDispatcher* dispatcher,
           scoped_refptr<AbpActionContext> ctx) {
          content::GetUIThreadTaskRunner({})->PostDelayedTask(
              FROM_HERE,
              base::BindOnce(&DispatchBatchAction,
                             std::move(actions), next_index,
                             std::move(tab_id), dispatcher, ctx),
              base::Milliseconds(20));
        },
        std::move(actions), index + 1, tab_id, dispatcher, ctx);

    if (actual_action == "press") {
      dispatcher->KeyPressRaw(tab_id, params, std::move(dispatch_next));
    } else if (actual_action == "down") {
      dispatcher->KeyDownRaw(tab_id, params, std::move(dispatch_next));
    } else {
      dispatcher->KeyUpRaw(tab_id, params, std::move(dispatch_next));
    }
    return;
  }

  // For all other types, dispatch and chain
  auto dispatch_next = base::BindOnce(
      [](base::Value::List actions, int next_index, std::string tab_id,
         AbpInputDispatcher* dispatcher,
         scoped_refptr<AbpActionContext> ctx) {
        content::GetUIThreadTaskRunner({})->PostDelayedTask(
            FROM_HERE,
            base::BindOnce(&DispatchBatchAction,
                           std::move(actions), next_index,
                           std::move(tab_id), dispatcher, ctx),
            base::Milliseconds(20));
      },
      std::move(actions), index + 1, tab_id, dispatcher, ctx);

  if (*type == "mouse_click") {
    dispatcher->ClickRaw(tab_id, params, std::move(dispatch_next));
  } else if (*type == "keyboard_type") {
    dispatcher->TypeRaw(tab_id, params, std::move(dispatch_next));
  } else if (*type == "mouse_hover") {
    dispatcher->MoveRaw(tab_id, params, std::move(dispatch_next));
  } else if (*type == "mouse_drag") {
    dispatcher->DragRaw(tab_id, params, std::move(dispatch_next));
  }
}

}  // namespace
```

**Important:** The existing `Click()`, `Type()`, etc. on `AbpInputDispatcher` each create their own `AbpActionContext`. For batching, we need "raw" versions that just dispatch the input event and call a completion callback without creating an action context. These are `ClickRaw`, `TypeRaw`, etc. — see Task 4.

**Step 4: Build (will fail — depends on Task 4 for Raw methods)**

Run: `autoninja -C out/Default chrome/browser/abp:abp`
Expected: Build error — `ClickRaw`, `TypeRaw` etc. not yet defined

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc chrome/browser/abp/abp_controller.h
git commit -m "feat(abp): implement batch action execution with 20ms inter-action delay"
```

---

## Task 4: Add Raw dispatch methods to AbpInputDispatcher

**Files:**
- Modify: `chrome/browser/abp/abp_input_dispatcher.h:46-83`
- Modify: `chrome/browser/abp/abp_input_dispatcher.cc`

The existing `Click()`, `Type()`, etc. create their own `AbpActionContext`. For batch execution, we need methods that just dispatch the input event and call a simple completion callback. These extract the core dispatch logic from the existing methods.

**Step 1: Add Raw method declarations to header**

```cpp
// Raw dispatch methods — dispatch input without creating an action context.
// Used by batch execution. Call completion_callback when event is dispatched.
using RawCallback = base::OnceCallback<void()>;

void ClickRaw(const std::string& tab_id,
              const base::Value::Dict& params,
              RawCallback callback);
void TypeRaw(const std::string& tab_id,
             const base::Value::Dict& params,
             RawCallback callback);
void MoveRaw(const std::string& tab_id,
             const base::Value::Dict& params,
             RawCallback callback);
void KeyPressRaw(const std::string& tab_id,
                 const base::Value::Dict& params,
                 RawCallback callback);
void KeyDownRaw(const std::string& tab_id,
                const base::Value::Dict& params,
                RawCallback callback);
void KeyUpRaw(const std::string& tab_id,
              const base::Value::Dict& params,
              RawCallback callback);
void DragRaw(const std::string& tab_id,
             const base::Value::Dict& params,
             RawCallback callback);
```

**Step 2: Implement Raw methods**

Each Raw method extracts the core dispatch logic from its corresponding full method, but skips `AbpActionContext` creation and calls the completion callback directly. For example, `ClickRaw` does the same mouse event dispatch as `Click` but within the already-active action context from the batch.

The implementation should refactor the common logic: extract the event dispatch code from each method into a shared helper, then have both the full method (via action context) and the Raw method call the same helper.

See `abp_input_dispatcher.cc:244-415` for Click — the core is the CDP `Input.dispatchMouseEvent` calls. For Raw, we send the same events but call `RawCallback` instead of `ctx->OnActionDispatched()`.

**Step 3: Build and verify**

Run: `autoninja -C out/Default chrome/browser/abp:abp`
Expected: Compiles successfully (all Raw methods + batch chain compiles)

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_input_dispatcher.h chrome/browser/abp/abp_input_dispatcher.cc
git commit -m "feat(abp): add raw input dispatch methods for batch execution"
```

---

## Task 5: Update MCP tool definitions (33 to 12)

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:26-307` (GetToolDefinitions)

**Step 1: Replace all 30 ToolBuilder blocks with 12**

Replace the entire `GetToolDefinitions()` body with the new 12 tool definitions. The exact definitions are in the design doc `plans/mcp-tool-consolidation.md` sections "MCP Tool Schema" and "Step 2".

Key tools:
1. `browser_action` — batched input (custom schema with actions array)
2. `browser_scroll` — standalone scroll
3. `browser_navigate` — url + back/forward/reload
4. `browser_screenshot` — unchanged
5. `browser_tabs` — list/new/close/info/activate/stop
6. `browser_javascript` — renamed from execute_javascript
7. `browser_text` — renamed from get_text
8. `browser_dialog` — check/accept/dismiss
9. `browser_downloads` — list/status/cancel
10. `browser_files` — renamed from provide_files
11. `browser_get_status` — unchanged
12. `browser_shutdown` — unchanged

For `browser_action`, the ToolBuilder doesn't have `RequiredArray` — build the schema as a raw `base::Value::Dict` directly:

```cpp
// browser_action — build schema manually since ToolBuilder lacks array support
{
  base::Value::Dict tool;
  tool.Set("name", "browser_action");
  tool.Set("description",
      "Execute one or more browser actions (max 3). Each action has a "
      "'type' and type-specific params. Actions run sequentially with "
      "a 20ms pause between each. Screenshot is taken once after all "
      "actions complete.\n\n"
      "Batch these common workflows:\n"
      "- mouse_click -> keyboard_type -> keyboard_press(ENTER) — click field, type text, submit\n"
      "- mouse_click -> keyboard_type — click field, type text\n"
      "keyboard_press handles key combos: key:'A', modifiers:['CONTROL'] for Ctrl+A.\n"
      "Use a single action for standalone clicks, keypresses, etc.\n"
      "Use browser_scroll for scrolling (not part of this tool).\n\n"
      "Action params by type:\n"
      "- mouse_click: x, y, button?, click_count?, modifiers?\n"
      "- keyboard_type: text\n"
      "- keyboard_press: key (ENTER, TAB, ESCAPE, A-Z, F1-F12, ARROWUP, etc.), "
      "modifiers? ([SHIFT, CONTROL, ALT, META]), action? (press|down|up). "
      "Common abbreviations accepted: CTRL->CONTROL, CMD->META, ESC->ESCAPE, DEL->DELETE.\n"
      "- mouse_hover: x, y\n"
      "- mouse_drag: start_x, start_y, end_x, end_y, steps?");

  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict properties;

  // actions array
  base::Value::Dict actions_prop;
  actions_prop.Set("type", "array");

  base::Value::Dict item_schema;
  item_schema.Set("type", "object");
  base::Value::Dict item_props;

  // type enum
  base::Value::Dict type_prop;
  type_prop.Set("type", "string");
  base::Value::List type_enum;
  type_enum.Append("mouse_click");
  type_enum.Append("keyboard_type");
  type_enum.Append("keyboard_press");
  type_enum.Append("mouse_hover");
  type_enum.Append("mouse_drag");
  type_prop.Set("enum", std::move(type_enum));
  item_props.Set("type", std::move(type_prop));

  // All possible params as optional
  base::Value::Dict num_prop;
  num_prop.Set("type", "number");
  item_props.Set("x", num_prop.Clone());
  item_props.Set("y", num_prop.Clone());
  item_props.Set("start_x", num_prop.Clone());
  item_props.Set("start_y", num_prop.Clone());
  item_props.Set("end_x", num_prop.Clone());
  item_props.Set("end_y", num_prop.Clone());
  item_props.Set("click_count", num_prop.Clone());
  item_props.Set("steps", num_prop.Clone());

  base::Value::Dict str_prop;
  str_prop.Set("type", "string");
  item_props.Set("text", str_prop.Clone());
  item_props.Set("key", str_prop.Clone());

  // button enum
  base::Value::Dict button_prop;
  button_prop.Set("type", "string");
  base::Value::List button_enum;
  button_enum.Append("left");
  button_enum.Append("right");
  button_enum.Append("middle");
  button_prop.Set("enum", std::move(button_enum));
  item_props.Set("button", std::move(button_prop));

  // action enum (for keyboard_press)
  base::Value::Dict action_prop;
  action_prop.Set("type", "string");
  base::Value::List action_enum;
  action_enum.Append("press");
  action_enum.Append("down");
  action_enum.Append("up");
  action_prop.Set("enum", std::move(action_enum));
  item_props.Set("action", std::move(action_prop));

  // modifiers array
  base::Value::Dict mods_prop;
  mods_prop.Set("type", "array");
  base::Value::Dict mod_item;
  mod_item.Set("type", "string");
  base::Value::List mod_enum;
  mod_enum.Append("SHIFT");
  mod_enum.Append("CONTROL");
  mod_enum.Append("ALT");
  mod_enum.Append("META");
  mod_item.Set("enum", std::move(mod_enum));
  mods_prop.Set("items", std::move(mod_item));
  item_props.Set("modifiers", std::move(mods_prop));

  item_schema.Set("properties", std::move(item_props));
  base::Value::List required_type;
  required_type.Append("type");
  item_schema.Set("required", std::move(required_type));

  actions_prop.Set("items", std::move(item_schema));
  actions_prop.Set("minItems", 1);
  actions_prop.Set("maxItems", 3);
  properties.Set("actions", std::move(actions_prop));

  // tab_id
  properties.Set("tab_id", str_prop.Clone());

  // screenshot object (open schema)
  base::Value::Dict ss_prop;
  ss_prop.Set("type", "object");
  properties.Set("screenshot", std::move(ss_prop));

  schema.Set("properties", std::move(properties));
  base::Value::List required;
  required.Append("actions");
  schema.Set("required", std::move(required));

  tool.Set("inputSchema", std::move(schema));
  tools.Append(std::move(tool));
}
```

**Step 2: Build and verify**

Run: `autoninja -C out/Default chrome/browser/abp:abp`
Expected: Compiles successfully

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.cc
git commit -m "feat(abp): replace 30 MCP tool definitions with 12"
```

---

## Task 6: Update MCP handler Call* methods and dispatch table

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.h:54-152`
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:547-634` (HandleToolsCall dispatch)
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:695-1200+` (Call* implementations)

**Step 1: Replace 30 Call* declarations with 12 in header**

Replace lines 54-152 with:

```cpp
void CallBrowserAction(const base::Value::Dict& args,
                       base::Value request_id,
                       ResponseWithHeadersCallback callback);
void CallBrowserScroll(const base::Value::Dict& args,
                       base::Value request_id,
                       ResponseWithHeadersCallback callback);
void CallBrowserNavigate(const base::Value::Dict& args,
                         base::Value request_id,
                         ResponseWithHeadersCallback callback);
void CallBrowserScreenshot(const base::Value::Dict& args,
                           base::Value request_id,
                           ResponseWithHeadersCallback callback);
void CallBrowserTabs(const base::Value::Dict& args,
                     base::Value request_id,
                     ResponseWithHeadersCallback callback);
void CallBrowserJavascript(const base::Value::Dict& args,
                           base::Value request_id,
                           ResponseWithHeadersCallback callback);
void CallBrowserText(const base::Value::Dict& args,
                     base::Value request_id,
                     ResponseWithHeadersCallback callback);
void CallBrowserDialog(const base::Value::Dict& args,
                       base::Value request_id,
                       ResponseWithHeadersCallback callback);
void CallBrowserDownloads(const base::Value::Dict& args,
                          base::Value request_id,
                          ResponseWithHeadersCallback callback);
void CallBrowserFiles(const base::Value::Dict& args,
                      base::Value request_id,
                      ResponseWithHeadersCallback callback);
void CallBrowserGetStatus(const base::Value::Dict& args,
                          base::Value request_id,
                          ResponseWithHeadersCallback callback);
void CallBrowserShutdown(const base::Value::Dict& args,
                         base::Value request_id,
                         ResponseWithHeadersCallback callback);
```

**Step 2: Replace the 30-entry dispatch table with 12 entries**

Replace the if/else chain in `HandleToolsCall` (lines 564-633):

```cpp
if (*name == "browser_action") {
  CallBrowserAction(*args, std::move(request_id), std::move(callback));
} else if (*name == "browser_scroll") {
  CallBrowserScroll(*args, std::move(request_id), std::move(callback));
} else if (*name == "browser_navigate") {
  CallBrowserNavigate(*args, std::move(request_id), std::move(callback));
} else if (*name == "browser_screenshot") {
  CallBrowserScreenshot(*args, std::move(request_id), std::move(callback));
} else if (*name == "browser_tabs") {
  CallBrowserTabs(*args, std::move(request_id), std::move(callback));
} else if (*name == "browser_javascript") {
  CallBrowserJavascript(*args, std::move(request_id), std::move(callback));
} else if (*name == "browser_text") {
  CallBrowserText(*args, std::move(request_id), std::move(callback));
} else if (*name == "browser_dialog") {
  CallBrowserDialog(*args, std::move(request_id), std::move(callback));
} else if (*name == "browser_downloads") {
  CallBrowserDownloads(*args, std::move(request_id), std::move(callback));
} else if (*name == "browser_files") {
  CallBrowserFiles(*args, std::move(request_id), std::move(callback));
} else if (*name == "browser_get_status") {
  CallBrowserGetStatus(*args, std::move(request_id), std::move(callback));
} else if (*name == "browser_shutdown") {
  CallBrowserShutdown(*args, std::move(request_id), std::move(callback));
} else {
  SendJsonRpcError(std::move(request_id), kMethodNotFound,
                   "Unknown tool: " + *name, std::move(callback));
}
```

**Step 3: Implement all 12 Call* methods**

Most follow the same `ResolveTabId → clone args → remove tab_id → serialize → controller_->HandleRequest()` pattern from the existing code. Key new methods:

**CallBrowserAction** — delegates to batch endpoint:
```cpp
void AbpMcpHandler::CallBrowserAction(const base::Value::Dict& args,
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
      "POST", "/api/v1/tabs/" + tab_id + "/batch", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}
```

**CallBrowserNavigate** — route by url vs action:
```cpp
void AbpMcpHandler::CallBrowserNavigate(const base::Value::Dict& args,
                                        base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) { /* error */ return; }

  const std::string* url = args.FindString("url");
  const std::string* action = args.FindString("action");

  if (url) {
    base::Value::Dict body;
    body.Set("url", *url);
    std::string body_str;
    base::JSONWriter::Write(base::Value(std::move(body)), &body_str);
    controller_->HandleRequest("POST", "/api/v1/tabs/" + tab_id + "/navigate",
                               body_str, /* callback */);
  } else if (action) {
    if (*action == "back") {
      controller_->HandleRequest("POST", "/api/v1/tabs/" + tab_id + "/back", "", /* callback */);
    } else if (*action == "forward") {
      controller_->HandleRequest("POST", "/api/v1/tabs/" + tab_id + "/forward", "", /* callback */);
    } else if (*action == "reload") {
      controller_->HandleRequest("POST", "/api/v1/tabs/" + tab_id + "/reload", "", /* callback */);
    }
  } else {
    SendJsonRpcError(/* "Provide 'url' or 'action'" */);
  }
}
```

**CallBrowserTabs** — route by action param:
```cpp
void AbpMcpHandler::CallBrowserTabs(const base::Value::Dict& args,
                                    base::Value request_id,
                                    ResponseWithHeadersCallback callback) {
  const std::string* action = args.FindString("action");
  std::string act = action ? *action : "list";

  if (act == "list") {
    controller_->HandleRequest("GET", "/api/v1/tabs", "", /* callback */);
  } else if (act == "new") {
    // Reuse existing new tab logic with url param
    std::string body = "{}";
    const std::string* url = args.FindString("url");
    if (url) {
      base::Value::Dict d;
      d.Set("url", *url);
      base::JSONWriter::Write(base::Value(std::move(d)), &body);
    }
    controller_->HandleRequest("POST", "/api/v1/tabs", body, /* callback */);
  } else {
    std::string tab_id = ResolveTabId(args);
    if (tab_id.empty()) { /* error */ return; }
    if (act == "close") {
      controller_->HandleRequest("DELETE", "/api/v1/tabs/" + tab_id, "", /* callback */);
    } else if (act == "info") {
      controller_->HandleRequest("GET", "/api/v1/tabs/" + tab_id, "", /* callback */);
    } else if (act == "activate") {
      controller_->HandleRequest("POST", "/api/v1/tabs/" + tab_id + "/activate", "", /* callback */);
    } else if (act == "stop") {
      controller_->HandleRequest("POST", "/api/v1/tabs/" + tab_id + "/stop", "", /* callback */);
    }
  }
}
```

Apply same pattern for `CallBrowserDialog` and `CallBrowserDownloads` (see design doc Step 5).

Remaining tools (`CallBrowserScroll`, `CallBrowserScreenshot`, `CallBrowserJavascript`, `CallBrowserText`, `CallBrowserFiles`, `CallBrowserGetStatus`, `CallBrowserShutdown`) are direct renames of existing implementations — same body, new method name.

**Step 4: Build and verify**

Run: `autoninja -C out/Default chrome/browser/abp:abp`
Expected: Compiles successfully

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.cc chrome/browser/abp/abp_mcp_handler.h
git commit -m "feat(abp): implement 12 consolidated MCP Call* methods and dispatch table"
```

---

## Task 7: Update kGuideContent resource

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:310-384` (kGuideContent)

**Step 1: Replace kGuideContent**

Replace the tool reference section with the updated 12-tool reference from the design doc (Step 7). Keep the non-tool-reference sections (How ABP Works, Debugging, Tips) unchanged.

**Step 2: Build and verify**

Run: `autoninja -C out/Default chrome/browser/abp:abp`
Expected: Compiles

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.cc
git commit -m "docs(abp): update kGuideContent for 12-tool surface"
```

---

## Task 8: Update skill file

**Files:**
- Modify: `tools/abp-claude-skill/abp-browser.md`

**Step 1: Update tool documentation**

Replace tool references with new names. Document the batch pattern with examples.

**Step 2: Commit**

```bash
git add tools/abp-claude-skill/abp-browser.md
git commit -m "docs(abp): update skill file for consolidated MCP tools"
```

---

## Task 9: Full build and manual verification

**Step 1: Full build**

Run: `autoninja -C out/Default chrome`
Expected: Clean build

**Step 2: Launch and verify tool count**

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/test --no-first-run
```

Then in another terminal:
```bash
# Initialize MCP
curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test","version":"1.0"},"capabilities":{}}}'

# List tools — should show exactly 12
curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' | python3 -c "import sys,json; tools=json.load(sys.stdin)['result']['tools']; print(f'{len(tools)} tools:'); [print(f'  {t[\"name\"]}') for t in tools]"
```
Expected: 14 tools listed

**Step 3: Test batch endpoint**

```bash
# Get a tab ID
TAB=$(curl -s http://localhost:8222/api/v1/tabs | python3 -c "import sys,json; print(json.load(sys.stdin)[0]['id'])")

# Single click action
curl -s -X POST "http://localhost:8222/api/v1/tabs/$TAB/batch" \
  -H "Content-Type: application/json" \
  -d '{"actions":[{"type":"mouse_click","x":450,"y":320}]}'

# Validation error: bad coordinates
curl -s -X POST "http://localhost:8222/api/v1/tabs/$TAB/batch" \
  -H "Content-Type: application/json" \
  -d '{"actions":[{"type":"mouse_click","x":99999,"y":99999}]}'

# Key normalization
curl -s -X POST "http://localhost:8222/api/v1/tabs/$TAB/batch" \
  -H "Content-Type: application/json" \
  -d '{"actions":[{"type":"keyboard_press","key":"esc"}]}'

# Invalid key
curl -s -X POST "http://localhost:8222/api/v1/tabs/$TAB/batch" \
  -H "Content-Type: application/json" \
  -d '{"actions":[{"type":"keyboard_press","key":"FOOBAR"}]}'

# Full click-type-enter batch
curl -s -X POST "http://localhost:8222/api/v1/tabs/$TAB/batch" \
  -H "Content-Type: application/json" \
  -d '{"actions":[{"type":"mouse_click","x":450,"y":320},{"type":"keyboard_type","text":"hello"},{"type":"keyboard_press","key":"ENTER"}]}'
```

**Step 4: Verify existing REST endpoints still work**

```bash
curl -s http://localhost:8222/api/v1/tabs
curl -s -X POST "http://localhost:8222/api/v1/tabs/$TAB/click" \
  -H "Content-Type: application/json" -d '{"x":100,"y":100}'
```

**Step 5: Commit any fixes from testing**

```bash
git add -A && git commit -m "fix(abp): address issues found during manual verification"
```
