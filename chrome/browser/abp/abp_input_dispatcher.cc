// Copyright 2025 ARP Software LLC. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_input_dispatcher.h"

#include <cctype>

#include "base/strings/string_number_conversions.h"
#include "chrome/browser/abp/abp_action_context.h"
#include "chrome/browser/abp/abp_controller.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/browser_thread.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/synthetic_web_input_event_builders.h"
#include "ui/events/keycodes/dom/keycode_converter.h"

namespace abp {

namespace {

// Convert ABP modifier bitmask (1=Alt, 2=Ctrl, 4=Meta, 8=Shift) to
// blink::WebInputEvent modifier flags.
int ModifierFlagsToWebModifiers(int flags) {
  int result = 0;
  if (flags & 1)
    result |= blink::WebInputEvent::kAltKey;
  if (flags & 2)
    result |= blink::WebInputEvent::kControlKey;
  if (flags & 4)
    result |= blink::WebInputEvent::kMetaKey;
  if (flags & 8)
    result |= blink::WebInputEvent::kShiftKey;
  return result;
}

// US keyboard layout mapping for symbols/punctuation.
// Maps a character to its physical key's DOM code string, Windows virtual key
// code, and whether Shift is required to produce it.
struct UsKeyMapping {
  const char* code;          // DOM physical key code (e.g. "Digit4")
  int windows_virtual_key;   // VK code of the physical key
  bool shift;                // Whether Shift is required
};

// Returns the US keyboard mapping for a symbol/punctuation character,
// or nullptr if not found.
const UsKeyMapping* GetUsKeyMapping(char c) {
  // clang-format off
  static constexpr struct { char ch; UsKeyMapping mapping; } kTable[] = {
      // Shifted digit row: Shift + Digit → symbol
      {'!', {"Digit1",       '1',  true}},
      {'@', {"Digit2",       '2',  true}},
      {'#', {"Digit3",       '3',  true}},
      {'$', {"Digit4",       '4',  true}},
      {'%', {"Digit5",       '5',  true}},
      {'^', {"Digit6",       '6',  true}},
      {'&', {"Digit7",       '7',  true}},
      {'*', {"Digit8",       '8',  true}},
      {'(', {"Digit9",       '9',  true}},
      {')', {"Digit0",       '0',  true}},

      // OEM keys: unshifted
      {'`', {"Backquote",    0xC0, false}},
      {'-', {"Minus",        0xBD, false}},
      {'=', {"Equal",        0xBB, false}},
      {'[', {"BracketLeft",  0xDB, false}},
      {']', {"BracketRight", 0xDD, false}},
      {'\\',{"Backslash",    0xDC, false}},
      {';', {"Semicolon",    0xBA, false}},
      {'\'',{"Quote",        0xDE, false}},
      {',', {"Comma",        0xBC, false}},
      {'.', {"Period",       0xBE, false}},
      {'/', {"Slash",        0xBF, false}},

      // OEM keys: shifted
      {'~', {"Backquote",    0xC0, true}},
      {'_', {"Minus",        0xBD, true}},
      {'+', {"Equal",        0xBB, true}},
      {'{', {"BracketLeft",  0xDB, true}},
      {'}', {"BracketRight", 0xDD, true}},
      {'|', {"Backslash",    0xDC, true}},
      {':', {"Semicolon",    0xBA, true}},
      {'"', {"Quote",        0xDE, true}},
      {'<', {"Comma",        0xBC, true}},
      {'>', {"Period",       0xBE, true}},
      {'?', {"Slash",        0xBF, true}},
  };
  // clang-format on

  for (const auto& entry : kTable) {
    if (entry.ch == c)
      return &entry.mapping;
  }
  return nullptr;
}

}  // namespace

AbpInputDispatcher::AbpInputDispatcher(AbpController* controller)
    : controller_(controller) {}

AbpInputDispatcher::~AbpInputDispatcher() = default;

void AbpInputDispatcher::ForwardKeyEvent(content::WebContents* wc,
                                         blink::WebInputEvent::Type type,
                                         const KeyInfo& info,
                                         int web_modifiers) {
  // Mark as debugger-originated so ABP's system input filter in
  // RenderInputRouter allows the event through (same as CDP mouse events).
  web_modifiers |= blink::WebInputEvent::kFromDebugger;

  auto* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv)
    return;
  auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
      rwhv->GetRenderWidgetHost());
  if (!rwhi)
    return;

  // Get the focused widget (handles iframes, etc.)
  if (rwhi->delegate()) {
    auto* target = rwhi->delegate()->GetFocusedRenderWidgetHost(rwhi);
    if (target)
      rwhi = target;
  }

  if (type == blink::WebInputEvent::Type::kKeyDown) {
    // Send the full three-event sequence that real keyboard input produces:
    //   1. kRawKeyDown → DOM "keydown"
    //   2. kChar → DOM "keypress" (only for keys that produce text)
    //   3. (kKeyUp sent separately by caller)
    // This matches macOS's native event sequence and is more reliable than
    // relying on blink's kKeyDown→kChar fallthrough logic.

    // 1. Send kRawKeyDown (generates DOM "keydown")
    base::TimeTicks now = base::TimeTicks::Now();
    input::NativeWebKeyboardEvent raw_down(
        blink::WebInputEvent::Type::kRawKeyDown, web_modifiers, now);
    raw_down.windows_key_code = info.windows_virtual_key;
    raw_down.native_key_code = info.native_virtual_key;
    raw_down.dom_code = static_cast<int>(
        ui::KeycodeConverter::CodeStringToDomCode(info.code));
    raw_down.dom_key = static_cast<int>(
        ui::KeycodeConverter::KeyStringToDomKey(info.key));
    if (!info.text.empty()) {
      raw_down.text[0] = static_cast<char16_t>(info.text[0]);
      raw_down.unmodified_text[0] = raw_down.text[0];
    }
    raw_down.skip_if_unhandled = true;
    rwhi->ForwardKeyboardEvent(raw_down);

    // 2. Send kChar (generates DOM "keypress") — only if key produces text
    //    Use a distinct timestamp so the input pipeline doesn't coalesce it
    //    with the preceding kRawKeyDown.
    if (!info.text.empty()) {
      base::TimeTicks char_time = now + base::Microseconds(1);
      input::NativeWebKeyboardEvent char_event(
          blink::WebInputEvent::Type::kChar, web_modifiers, char_time);
      char_event.windows_key_code = info.windows_virtual_key;
      char_event.native_key_code = info.native_virtual_key;
      char_event.dom_code = static_cast<int>(
          ui::KeycodeConverter::CodeStringToDomCode(info.code));
      char_event.dom_key = static_cast<int>(
          ui::KeycodeConverter::KeyStringToDomKey(info.key));
      char_event.text[0] = static_cast<char16_t>(info.text[0]);
      char_event.unmodified_text[0] = char_event.text[0];
      char_event.skip_if_unhandled = true;
      rwhi->ForwardKeyboardEvent(char_event);
    }
  } else {
    // kKeyUp or other types — send as-is
    input::NativeWebKeyboardEvent event(type, web_modifiers,
                                        base::TimeTicks::Now());
    event.windows_key_code = info.windows_virtual_key;
    event.native_key_code = info.native_virtual_key;
    event.dom_code = static_cast<int>(
        ui::KeycodeConverter::CodeStringToDomCode(info.code));
    event.dom_key = static_cast<int>(
        ui::KeycodeConverter::KeyStringToDomKey(info.key));
    event.skip_if_unhandled = true;
    rwhi->ForwardKeyboardEvent(event);
  }
}

void AbpInputDispatcher::ForwardWheelEvent(content::WebContents* wc,
                                           double x,
                                           double y,
                                           double delta_x,
                                           double delta_y) {
  auto* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv)
    return;
  auto* rwhi = static_cast<content::RenderWidgetHostImpl*>(
      rwhv->GetRenderWidgetHost());
  if (!rwhi)
    return;

  // Get the focused widget (handles iframes, etc.)
  if (rwhi->delegate()) {
    auto* target = rwhi->delegate()->GetFocusedRenderWidgetHost(rwhi);
    if (target)
      rwhi = target;
  }

  float fx = static_cast<float>(x);
  float fy = static_cast<float>(y);
  // Negate deltas: ABP API convention (positive = down/right) is opposite
  // to WebMouseWheelEvent convention (positive = up/left).
  float fdx = -static_cast<float>(delta_x);
  float fdy = -static_cast<float>(delta_y);

  // Mark as kFromDebugger so ABP's input filter in RenderInputRouter allows
  // the event through (ABP blocks non-debugger input by default).
  int modifiers = blink::WebInputEvent::kFromDebugger;

  // macOS scroll handling requires the full gesture phase sequence:
  // kPhaseBegan -> kPhaseEnded. A lone kPhaseChanged is dropped.

  // 1. Send kPhaseBegan with the scroll deltas
  blink::WebMouseWheelEvent begin_event =
      blink::SyntheticWebMouseWheelEventBuilder::Build(
          fx, fy, fdx, fdy, modifiers,
          ui::ScrollGranularity::kScrollByPrecisePixel);
  begin_event.phase = blink::WebMouseWheelEvent::kPhaseBegan;
  rwhi->ForwardWheelEvent(begin_event);

  // 2. Send kPhaseEnded with zero deltas to close the gesture
  blink::WebMouseWheelEvent end_event =
      blink::SyntheticWebMouseWheelEventBuilder::Build(
          fx, fy, 0, 0, modifiers,
          ui::ScrollGranularity::kScrollByPrecisePixel);
  end_event.phase = blink::WebMouseWheelEvent::kPhaseEnded;
  rwhi->ForwardWheelEvent(end_event);
}

void AbpInputDispatcher::Click(const std::string& tab_id,
                               const base::Value::Dict& params,
                               ResponseCallback callback) {
  // Validate params early
  auto x_opt = params.FindDouble("x");
  auto y_opt = params.FindDouble("y");
  if (!x_opt || !y_opt) {
    controller_->SendError(400, "Missing 'x' or 'y' parameter",
                           std::move(callback));
    return;
  }

  double click_x = *x_opt;
  double click_y = *y_opt;

  // Read optional button (default: "left")
  const std::string* button_param = params.FindString("button");
  std::string button = (button_param && (*button_param == "right" ||
                                          *button_param == "middle"))
                            ? *button_param
                            : "left";

  // Read optional clickCount (default: 1)
  int click_count = params.FindInt("click_count").value_or(1);
  if (click_count < 1) click_count = 1;
  if (click_count > 3) click_count = 3;

  // Read optional modifiers
  int mod_flags = 0;
  const base::Value::List* mod_list = params.FindList("modifiers");
  if (mod_list) {
    std::vector<std::string> modifiers;
    for (const auto& mod : *mod_list) {
      if (mod.is_string()) {
        modifiers.push_back(mod.GetString());
      }
    }
    mod_flags = ModifiersToFlags(modifiers);
  }

  // Use AbpActionContext for unified action flow:
  // Resume -> BeforeScreenshot -> Action -> Wait -> Pause -> AfterScreenshot -> Response
  AbpActionContext::Run(
      controller_, tab_id, "click", params,
      // Action callback - performs the actual click
      base::BindOnce(
          [](double coord_x, double coord_y, std::string btn, int count,
             int modifiers, AbpActionContext* ctx) {
            // Log cursor position before the action
            auto& tab_state = ctx->controller()->GetOrCreateTabState(ctx->tab_id());
            VLOG(1) << "ABP: Click action started - target=(" << coord_x << ", " << coord_y
                      << ") button=" << btn << " count=" << count
                      << " before_cursor=(" << tab_state.cursor.x << ", " << tab_state.cursor.y
                      << ") active=" << tab_state.cursor.active;

            // Update virtual cursor state via controller
            ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), coord_x,
                                                        coord_y);

            // Enable and set virtual cursor via Mojo for on-screen rendering
            content::WebContents* wc = ctx->web_contents();
            if (wc) {
              ctx->controller()->SetVirtualCursorEnabledViaMojo(wc, true);
              ctx->controller()->SetVirtualCursorViaMojo(wc, coord_x, coord_y,
                                                          true);
            } else {
              LOG(WARNING) << "ABP: Click action - WebContents not found for tab " << ctx->tab_id();
            }

            // Keep context alive through async fences + input dispatch.
            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            ctx->controller()->InsertVisualStateFence(
                ctx->tab_id(),
                base::BindOnce(
                    [](double x, double y, std::string button, int click_count,
                       int mods, scoped_refptr<AbpActionContext> action_ctx,
                       bool fence_ready) {
                      VLOG(1) << "ABP: Visual state fence "
                                << (fence_ready ? "ready" : "failed")
                                << " for click at (" << x << ", " << y << ")";

                      if (!fence_ready) {
                        action_ctx->OnActionError(
                            "VISUAL_STATE_ERROR",
                            "Failed to establish visual-state fence before click");
                        return;
                      }

                      AbpCdpClient* cdp_client = action_ctx->client();
                      if (!cdp_client) {
                        action_ctx->OnActionError("CDP_ERROR",
                                                  "CDP client lost");
                        return;
                      }

                      // Send mousePressed after cursor is confirmed visible.
                      base::Value::Dict press_params;
                      press_params.Set("type", "mousePressed");
                      press_params.Set("x", x);
                      press_params.Set("y", y);
                      press_params.Set("button", button);
                      press_params.Set("clickCount", click_count);
                      press_params.Set("modifiers", mods);

                      VLOG(1) << "ABP: Sending Input.dispatchMouseEvent (mousePressed) at ("
                                << x << ", " << y << ") button=" << button;

                      cdp_client->SendCommand(
                          "Input.dispatchMouseEvent", std::move(press_params),
                          base::BindOnce(
                              [](double x, double y, std::string button,
                                 int click_count, int mods,
                                 scoped_refptr<AbpActionContext> action_ctx,
                                 bool success, const std::string& result) {
                                if (!success) {
                                  LOG(ERROR) << "ABP: Input.dispatchMouseEvent (mousePressed) failed: " << result;
                                  action_ctx->OnActionError("CDP_ERROR", result);
                                  return;
                                }

                                VLOG(1) << "ABP: Input.dispatchMouseEvent (mousePressed) succeeded";

                                AbpCdpClient* cdp_client = action_ctx->client();
                                if (!cdp_client) {
                                  action_ctx->OnActionError(
                                      "CDP_ERROR", "CDP client lost");
                                  return;
                                }

                                // Send mouseReleased.
                                base::Value::Dict release_params;
                                release_params.Set("type", "mouseReleased");
                                release_params.Set("x", x);
                                release_params.Set("y", y);
                                release_params.Set("button", button);
                                release_params.Set("clickCount", click_count);
                                release_params.Set("modifiers", mods);

                                VLOG(1) << "ABP: Sending Input.dispatchMouseEvent (mouseReleased) at ("
                                          << x << ", " << y << ")";

                                cdp_client->SendCommand(
                                    "Input.dispatchMouseEvent",
                                    std::move(release_params),
                                    base::BindOnce(
                                        [](scoped_refptr<AbpActionContext> c,
                                           bool success,
                                           const std::string& result) {
                                          if (!success) {
                                            LOG(ERROR) << "ABP: Input.dispatchMouseEvent (mouseReleased) failed: " << result;
                                            c->OnActionError("CDP_ERROR",
                                                             result);
                                            return;
                                          }

                                          VLOG(1) << "ABP: Input.dispatchMouseEvent (mouseReleased) succeeded - click complete";

                                          base::Value::Dict res;
                                          res.Set("status", "clicked");
                                          c->SetResult(std::move(res));
                                          c->OnActionDispatched();
                                        },
                                        action_ctx));
                              },
                              x, y, button, click_count, mods, action_ctx));
                    },
                    coord_x, coord_y, std::move(btn), count, modifiers,
                    std::move(ctx_ref)));
          },
          click_x, click_y, std::move(button), click_count, mod_flags),
      std::move(callback));
}

void AbpInputDispatcher::Type(const std::string& tab_id,
                              const base::Value::Dict& params,
                              ResponseCallback callback) {
  // Validate params early
  const std::string* text = params.FindString("text");
  if (!text) {
    controller_->SendError(400, "Missing 'text' parameter", std::move(callback));
    return;
  }

  std::string text_copy = *text;

  // Use AbpActionContext for unified action flow.
  // Type uses native keyboard events — each character gets its own
  // keyDown + keyUp pair with a 2ms delay between characters, producing
  // the same DOM event chain as real typing (keydown → keypress → input → keyup).
  AbpActionContext::Run(
      controller_, tab_id, "type", params,
      base::BindOnce(
          [](std::string text_str, AbpInputDispatcher* dispatcher,
             AbpActionContext* ctx) {
            if (text_str.empty()) {
              base::Value::Dict res;
              res.Set("status", "typed");
              ctx->SetResult(std::move(res));
              ctx->OnActionDispatched();
              return;
            }
            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            dispatcher->TypeNextCharacter(ctx_ref, std::move(text_str), 0);
          },
          std::move(text_copy), this),
      std::move(callback));
}

void AbpInputDispatcher::TypeNextCharacter(
    scoped_refptr<AbpActionContext> ctx,
    std::string text,
    size_t char_index) {
  if (!ctx->web_contents() || char_index >= text.size()) {
    base::Value::Dict res;
    res.Set("status", "typed");
    ctx->SetResult(std::move(res));
    ctx->OnActionDispatched();
    return;
  }

  char c = text[char_index];
  int web_mods = 0;

  // Build KeyInfo for this character
  KeyInfo char_info;
  char_info.text = std::string(1, c);

  if (c >= 'a' && c <= 'z') {
    char_info.key = std::string(1, c);
    char_info.code = std::string("Key") + static_cast<char>(std::toupper(c));
    char_info.windows_virtual_key = std::toupper(c);
  } else if (c >= 'A' && c <= 'Z') {
    // Uppercase: send Shift + lowercase key
    char_info.key = std::string(1, c);
    char_info.code = std::string("Key") + c;
    char_info.windows_virtual_key = c;
    web_mods = blink::WebInputEvent::kShiftKey;
  } else if (c >= '0' && c <= '9') {
    char_info.key = std::string(1, c);
    char_info.code = std::string("Digit") + c;
    char_info.windows_virtual_key = c;
  } else if (c == ' ') {
    char_info.key = " ";
    char_info.code = "Space";
    char_info.windows_virtual_key = 32;
  } else if (c == '\n' || c == '\r') {
    char_info.key = "Enter";
    char_info.code = "Enter";
    char_info.text = "\r";
    char_info.windows_virtual_key = 13;
  } else if (c == '\t') {
    char_info.key = "Tab";
    char_info.code = "Tab";
    char_info.text = "\t";
    char_info.windows_virtual_key = 9;
  } else if (const UsKeyMapping* mapping = GetUsKeyMapping(c)) {
    // Symbol/punctuation with known US keyboard mapping.
    // Use the physical key's code and VK, with Shift if required.
    char_info.key = std::string(1, c);
    char_info.code = mapping->code;
    char_info.windows_virtual_key = mapping->windows_virtual_key;
    if (mapping->shift)
      web_mods = blink::WebInputEvent::kShiftKey;
  } else {
    // Unknown character — best effort: use character as key.
    char_info.key = std::string(1, c);
    char_info.windows_virtual_key = std::toupper(c);
  }
  char_info.native_virtual_key = char_info.windows_virtual_key;

  ForwardKeyEvent(ctx->web_contents(),
                  blink::WebInputEvent::Type::kKeyDown, char_info, web_mods);
  ForwardKeyEvent(ctx->web_contents(),
                  blink::WebInputEvent::Type::kKeyUp, char_info, web_mods);

  // 2ms delay before next character to simulate realistic typing speed
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpInputDispatcher::TypeNextCharacter,
                     base::Unretained(this), ctx, std::move(text),
                     char_index + 1),
      base::Milliseconds(2));
}

void AbpInputDispatcher::Move(const std::string& tab_id,
                              const base::Value::Dict& params,
                              ResponseCallback callback) {
  // Validate params early.
  auto x_opt = params.FindDouble("x");
  auto y_opt = params.FindDouble("y");
  if (!x_opt || !y_opt) {
    controller_->SendError(400, "Missing 'x' or 'y' parameter",
                           std::move(callback));
    return;
  }

  double move_x = *x_opt;
  double move_y = *y_opt;

  // Use AbpActionContext for unified action flow.
  AbpActionContext::Options options;
  AbpActionContext::RunWithOptions(
      controller_, tab_id, "move", params, options,
      // Action callback - performs the cursor move.
      base::BindOnce(
          [](double coord_x, double coord_y, AbpActionContext* ctx) {
            // Update virtual cursor state via controller.
            ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), coord_x,
                                                        coord_y);

            // Enable and set virtual cursor via Mojo for on-screen rendering.
            // The renderer will detect cursor type via hit-testing in SetPosition().
            content::WebContents* wc = ctx->web_contents();
            if (wc) {
              ctx->controller()->SetVirtualCursorEnabledViaMojo(wc, true);
              ctx->controller()->SetVirtualCursorViaMojo(wc, coord_x, coord_y,
                                                          true);
            }

            // Take a scoped_refptr to keep context alive through async calls.
            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            ctx->controller()->InsertVisualStateFence(
                ctx->tab_id(),
                base::BindOnce(
                    [](double final_x, double final_y,
                       scoped_refptr<AbpActionContext> action_ctx,
                       bool fence_ready) {
                      if (!fence_ready) {
                        action_ctx->OnActionError(
                            "VISUAL_STATE_ERROR",
                            "Failed to establish visual-state fence before move");
                        return;
                      }

                      AbpCdpClient* cdp_client = action_ctx->client();
                      if (!cdp_client) {
                        action_ctx->OnActionError("CDP_ERROR",
                                                  "CDP client lost");
                        return;
                      }

                      // Send mouseMoved event for page interaction
                      // (hover states, etc.).
                      base::Value::Dict move_params;
                      move_params.Set("type", "mouseMoved");
                      move_params.Set("x", final_x);
                      move_params.Set("y", final_y);

                      VLOG(1) << "ABP Move: Sending Input.dispatchMouseEvent ("
                              << final_x << ", " << final_y << ")";
                      cdp_client->SendCommand(
                          "Input.dispatchMouseEvent", std::move(move_params),
                          base::BindOnce(
                              [](double final_x, double final_y,
                                 scoped_refptr<AbpActionContext> action_ctx,
                                 bool success, const std::string& result) {
                                VLOG(1) << "ABP Move: Input.dispatchMouseEvent callback, success="
                                        << success;
                                if (!success) {
                                  action_ctx->OnActionError("CDP_ERROR",
                                                            result);
                                  return;
                                }

                                base::Value::Dict res;
                                res.Set("status", "moved");
                                res.Set("x", final_x);
                                res.Set("y", final_y);
                                action_ctx->SetResult(std::move(res));
                                action_ctx->OnActionDispatched();
                              },
                              final_x, final_y, action_ctx));
                    },
                    coord_x, coord_y, std::move(ctx_ref)));
          },
          move_x, move_y),
      std::move(callback));
}

void AbpInputDispatcher::Scroll(const std::string& tab_id,
                                const base::Value::Dict& params,
                                ResponseCallback callback) {
  // x, y specify the center of the element to scroll (where the scroll wheel
  // event is dispatched). Required to simulate real mouse-over-element behavior.
  auto x_opt = params.FindDouble("x");
  auto y_opt = params.FindDouble("y");
  if (!x_opt || !y_opt) {
    controller_->SendError(400, "Missing required 'x' or 'y' parameter",
                           std::move(callback));
    return;
  }
  double x = *x_opt;
  double y = *y_opt;
  double delta_x = params.FindDouble("delta_x").value_or(0);
  double delta_y = params.FindDouble("delta_y").value_or(0);

  if (delta_x == 0 && delta_y == 0) {
    controller_->SendError(
        400, "At least one of 'delta_x' or 'delta_y' must be non-zero",
        std::move(callback));
    return;
  }

  // Use AbpActionContext for consistent resume/pause/screenshot flow.
  // Use native mouse wheel events for both vertical and horizontal scrolling.
  // This simulates real user behavior: moving mouse over element and scrolling.
  AbpActionContext::Options options;
  options.min_wait_time = base::Milliseconds(500);
  AbpActionContext::RunWithOptions(
      controller_, tab_id, "scroll", params, options,
      // Action callback - performs the scroll via mouse wheel
      base::BindOnce(
          [](double scroll_x, double scroll_y, double dx, double dy,
             AbpInputDispatcher* dispatcher, AbpActionContext* ctx) {
            content::WebContents* wc = ctx->web_contents();
            if (!wc) {
              ctx->OnActionError("TAB_ERROR", "WebContents lost");
              return;
            }

            // Dispatch mouse wheel event at the specified coordinates
            // This simulates scrolling while the mouse is over the element
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

void AbpInputDispatcher::KeyPress(const std::string& tab_id,
                                  const base::Value::Dict& params,
                                  ResponseCallback callback) {
  const std::string* key = params.FindString("key");
  if (!key || key->empty()) {
    controller_->SendError(400, "Missing 'key' parameter", std::move(callback));
    return;
  }

  // Get modifiers from params
  std::vector<std::string> modifiers;
  const base::Value::List* mod_list = params.FindList("modifiers");
  if (mod_list) {
    for (const auto& mod : *mod_list) {
      if (mod.is_string()) {
        modifiers.push_back(mod.GetString());
      }
    }
  }

  std::string key_copy = *key;

  // Use AbpActionContext for unified action flow.
  // KeyPress uses native keyboard events via ForwardKeyEvent — no CDP.
  // All events are synchronous (ForwardKeyboardEvent dispatches immediately
  // to the renderer via IPC), so no async chaining needed.
  AbpActionContext::Run(
      controller_, tab_id, "key_press", params,
      base::BindOnce(
          [](std::string pressed_key, std::vector<std::string> mods,
             AbpInputDispatcher* dispatcher, AbpActionContext* ctx) {
            content::WebContents* wc = ctx->web_contents();
            if (!wc) {
              ctx->OnActionError("TAB_ERROR", "WebContents lost");
              return;
            }

            KeyInfo key_info = GetKeyInfo(pressed_key);
            int mod_flags = ModifiersToFlags(mods);
            int web_mods = ModifierFlagsToWebModifiers(mod_flags);

            // Press modifier keys down
            for (const auto& mod_name : mods) {
              KeyInfo mod_info = GetKeyInfo(mod_name);
              dispatcher->ForwardKeyEvent(
                  wc, blink::WebInputEvent::Type::kKeyDown, mod_info,
                  web_mods);
            }

            // Press and release the main key
            dispatcher->ForwardKeyEvent(
                wc, blink::WebInputEvent::Type::kKeyDown, key_info, web_mods);
            dispatcher->ForwardKeyEvent(
                wc, blink::WebInputEvent::Type::kKeyUp, key_info, web_mods);

            // Release modifier keys in reverse order
            for (auto it = mods.rbegin(); it != mods.rend(); ++it) {
              KeyInfo mod_info = GetKeyInfo(*it);
              dispatcher->ForwardKeyEvent(
                  wc, blink::WebInputEvent::Type::kKeyUp, mod_info, 0);
            }

            base::Value::Dict res;
            res.Set("status", "pressed");
            res.Set("key", key_info.key);
            if (!mods.empty()) {
              base::Value::List mod_result;
              for (const auto& m : mods) {
                mod_result.Append(m);
              }
              res.Set("modifiers", std::move(mod_result));
            }
            ctx->SetResult(std::move(res));
            ctx->OnActionDispatched();
          },
          std::move(key_copy), std::move(modifiers), this),
      std::move(callback));
}

void AbpInputDispatcher::KeyDown(const std::string& tab_id,
                                 const base::Value::Dict& params,
                                 ResponseCallback callback) {
  const std::string* key = params.FindString("key");
  if (!key || key->empty()) {
    controller_->SendError(400, "Missing 'key' parameter", std::move(callback));
    return;
  }

  std::string key_copy = *key;

  // Use AbpActionContext for unified action flow.
  // KeyDown uses native keyboard events — no CDP.
  AbpActionContext::Run(
      controller_, tab_id, "key_down", params,
      base::BindOnce(
          [](std::string pressed_key, AbpController* controller,
             AbpInputDispatcher* dispatcher, AbpActionContext* ctx) {
            content::WebContents* wc = ctx->web_contents();
            if (!wc) {
              ctx->OnActionError("TAB_ERROR", "WebContents lost");
              return;
            }

            KeyInfo key_info = GetKeyInfo(pressed_key);

            // Track the held key
            auto& held_state =
                controller->GetOrCreateTabState(ctx->tab_id()).held_keys;
            held_state.held_keys.insert(pressed_key);
            if (key_info.is_modifier) {
              held_state.current_modifiers |= key_info.modifier_flag;
            }

            int web_mods =
                ModifierFlagsToWebModifiers(held_state.current_modifiers);

            dispatcher->ForwardKeyEvent(
                wc, blink::WebInputEvent::Type::kKeyDown, key_info, web_mods);

            base::Value::Dict res;
            res.Set("status", "key_down");
            res.Set("key", pressed_key);
            ctx->SetResult(std::move(res));
            ctx->OnActionDispatched();
          },
          std::move(key_copy), controller_, this),
      std::move(callback));
}

void AbpInputDispatcher::KeyUp(const std::string& tab_id,
                               const base::Value::Dict& params,
                               ResponseCallback callback) {
  const std::string* key = params.FindString("key");
  if (!key || key->empty()) {
    controller_->SendError(400, "Missing 'key' parameter", std::move(callback));
    return;
  }

  std::string key_copy = *key;

  // Use AbpActionContext for unified action flow.
  // KeyUp uses native keyboard events — no CDP.
  AbpActionContext::Run(
      controller_, tab_id, "key_up", params,
      base::BindOnce(
          [](std::string released_key, AbpController* controller,
             AbpInputDispatcher* dispatcher, AbpActionContext* ctx) {
            content::WebContents* wc = ctx->web_contents();
            if (!wc) {
              ctx->OnActionError("TAB_ERROR", "WebContents lost");
              return;
            }

            KeyInfo key_info = GetKeyInfo(released_key);

            // Update held key tracking
            auto& held_state =
                controller->GetOrCreateTabState(ctx->tab_id()).held_keys;
            held_state.held_keys.erase(released_key);
            if (key_info.is_modifier) {
              held_state.current_modifiers &= ~key_info.modifier_flag;
            }

            int web_mods =
                ModifierFlagsToWebModifiers(held_state.current_modifiers);

            dispatcher->ForwardKeyEvent(
                wc, blink::WebInputEvent::Type::kKeyUp, key_info, web_mods);

            base::Value::Dict res;
            res.Set("status", "key_up");
            res.Set("key", released_key);
            ctx->SetResult(std::move(res));
            ctx->OnActionDispatched();
          },
          std::move(key_copy), controller_, this),
      std::move(callback));
}

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

}  // namespace abp
