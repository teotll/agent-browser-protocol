// Copyright 2025 ARP Software LLC. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_input_dispatcher.h"

#include "chrome/browser/abp/abp_action_context.h"
#include "chrome/browser/abp/abp_controller.h"
#include "base/strings/string_number_conversions.h"

namespace abp {

AbpInputDispatcher::AbpInputDispatcher(AbpController* controller)
    : controller_(controller) {}

AbpInputDispatcher::~AbpInputDispatcher() = default;

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
            // Update virtual cursor state via controller
            ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), coord_x,
                                                        coord_y);

            // Enable and set virtual cursor via Mojo for on-screen rendering
            content::WebContents* wc = ctx->web_contents();
            if (wc) {
              ctx->controller()->SetVirtualCursorEnabledViaMojo(wc, true);
              ctx->controller()->SetVirtualCursorViaMojo(wc, coord_x, coord_y,
                                                          true);
            }

            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            // Take a scoped_refptr to keep context alive through async calls
            scoped_refptr<AbpActionContext> ctx_ref(ctx);

            // Send mousePressed
            base::Value::Dict press_params;
            press_params.Set("type", "mousePressed");
            press_params.Set("x", coord_x);
            press_params.Set("y", coord_y);
            press_params.Set("button", btn);
            press_params.Set("clickCount", count);
            press_params.Set("modifiers", modifiers);

            client->SendCommand(
                "Input.dispatchMouseEvent", std::move(press_params),
                base::BindOnce(
                    [](double x, double y, std::string button, int click_count,
                       int mods,
                       scoped_refptr<AbpActionContext> action_ctx, bool success,
                       const std::string& result) {
                      if (!success) {
                        action_ctx->OnActionError("CDP_ERROR", result);
                        return;
                      }

                      AbpCdpClient* cdp_client = action_ctx->client();
                      if (!cdp_client) {
                        action_ctx->OnActionError("CDP_ERROR",
                                                  "CDP client lost");
                        return;
                      }

                      // Send mouseReleased
                      base::Value::Dict release_params;
                      release_params.Set("type", "mouseReleased");
                      release_params.Set("x", x);
                      release_params.Set("y", y);
                      release_params.Set("button", button);
                      release_params.Set("clickCount", click_count);
                      release_params.Set("modifiers", mods);

                      cdp_client->SendCommand(
                          "Input.dispatchMouseEvent",
                          std::move(release_params),
                          base::BindOnce(
                              [](scoped_refptr<AbpActionContext> c,
                                 bool success, const std::string& result) {
                                if (!success) {
                                  c->OnActionError("CDP_ERROR", result);
                                  return;
                                }

                                // Set result and signal action complete
                                base::Value::Dict res;
                                res.Set("status", "clicked");
                                c->SetResult(std::move(res));
                                c->OnActionDispatched();
                              },
                              action_ctx));
                    },
                    coord_x, coord_y, btn, count, modifiers, ctx_ref));
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

  // Use AbpActionContext for unified action flow
  AbpActionContext::Run(
      controller_, tab_id, "type", params,
      // Action callback - performs the actual type
      base::BindOnce(
          [](std::string text, AbpActionContext* ctx) {
            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            // Take a scoped_refptr to keep context alive through async call
            scoped_refptr<AbpActionContext> ctx_ref(ctx);

            // CDP: Input.insertText - simpler than key events
            base::Value::Dict cdp_params;
            cdp_params.Set("text", text);

            client->SendCommand(
                "Input.insertText", std::move(cdp_params),
                base::BindOnce(
                    [](scoped_refptr<AbpActionContext> action_ctx, bool success,
                       const std::string& result) {
                      if (!success) {
                        action_ctx->OnActionError("CDP_ERROR", result);
                        return;
                      }

                      base::Value::Dict res;
                      res.Set("status", "typed");
                      action_ctx->SetResult(std::move(res));
                      action_ctx->OnActionDispatched();
                    },
                    ctx_ref));
          },
          std::move(text_copy)),
      std::move(callback));
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

            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            // Take a scoped_refptr to keep context alive through async calls.
            scoped_refptr<AbpActionContext> ctx_ref(ctx);

            // Send mouseMoved event for page interaction (hover states, etc.).
            base::Value::Dict move_params;
            move_params.Set("type", "mouseMoved");
            move_params.Set("x", coord_x);
            move_params.Set("y", coord_y);

            VLOG(1) << "ABP Move: Sending Input.dispatchMouseEvent ("
                         << coord_x << ", " << coord_y << ")";
            client->SendCommand(
                "Input.dispatchMouseEvent", std::move(move_params),
                base::BindOnce(
                    [](double final_x, double final_y,
                       scoped_refptr<AbpActionContext> action_ctx,
                       bool success, const std::string& result) {
                      VLOG(1) << "ABP Move: Input.dispatchMouseEvent callback, success="
                                   << success;
                      if (!success) {
                        action_ctx->OnActionError("CDP_ERROR", result);
                        return;
                      }

                      // Set result and signal action complete.
                      base::Value::Dict res;
                      res.Set("status", "moved");
                      res.Set("x", final_x);
                      res.Set("y", final_y);
                      action_ctx->SetResult(std::move(res));
                      action_ctx->OnActionDispatched();
                    },
                    coord_x, coord_y, ctx_ref));
          },
          move_x, move_y),
      std::move(callback));
}

void AbpInputDispatcher::Scroll(const std::string& tab_id,
                                const base::Value::Dict& params,
                                ResponseCallback callback) {
  // Default scroll coordinates to virtual cursor's last known position,
  // simulating human behavior (scroll wheel fires where the mouse is).
  auto& tab_state = controller_->GetOrCreateTabState(tab_id);
  double default_x = tab_state.cursor.active ? tab_state.cursor.x : 500;
  double default_y = tab_state.cursor.active ? tab_state.cursor.y : 500;
  double x = params.FindDouble("x").value_or(default_x);
  double y = params.FindDouble("y").value_or(default_y);
  double delta_x = params.FindDouble("delta_x").value_or(0);
  double delta_y = params.FindDouble("delta_y").value_or(0);

  if (delta_x == 0 && delta_y == 0) {
    controller_->SendError(
        400, "At least one of 'delta_x' or 'delta_y' must be non-zero",
        std::move(callback));
    return;
  }

  // Use AbpActionContext for consistent resume/pause/screenshot flow.
  // Use window.scrollBy() via Runtime.evaluate instead of
  // Input.dispatchMouseEvent(mouseWheel) which hangs when virtual time
  // has been active (Chromium renderer ack issue).
  AbpActionContext::Options options;
  options.min_wait_time = base::Milliseconds(500);
  AbpActionContext::RunWithOptions(
      controller_, tab_id, "scroll", params, options,
      // Action callback - performs the scroll via JS
      base::BindOnce(
          [](double scroll_x, double scroll_y, double dx, double dy,
             AbpActionContext* ctx) {
            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            scoped_refptr<AbpActionContext> ctx_ref(ctx);

            // Use Runtime.evaluate with window.scrollBy for reliable scrolling
            std::string script =
                "window.scrollBy(" + base::NumberToString(dx) + "," +
                base::NumberToString(dy) +
                "); JSON.stringify({scrollX: window.scrollX, scrollY: "
                "window.scrollY})";

            base::Value::Dict eval_params;
            eval_params.Set("expression", script);
            eval_params.Set("returnByValue", true);

            client->SendCommand(
                "Runtime.evaluate", std::move(eval_params),
                base::BindOnce(
                    [](double final_x, double final_y, double final_dx,
                       double final_dy,
                       scoped_refptr<AbpActionContext> action_ctx, bool success,
                       const std::string& result) {
                      if (!success) {
                        action_ctx->OnActionError("CDP_ERROR", result);
                        return;
                      }

                      base::Value::Dict res;
                      res.Set("status", "scrolled");
                      res.Set("x", final_x);
                      res.Set("y", final_y);
                      res.Set("delta_x", final_dx);
                      res.Set("delta_y", final_dy);
                      action_ctx->SetResult(std::move(res));
                      action_ctx->OnActionDispatched();
                    },
                    scroll_x, scroll_y, dx, dy, ctx_ref));
          },
          x, y, delta_x, delta_y),
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

  // Use AbpActionContext for unified action flow
  AbpActionContext::Run(
      controller_, tab_id, "key_press", params,
      base::BindOnce(
          [](std::string pressed_key, std::vector<std::string> mods,
             AbpActionContext* ctx) {
            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            KeyInfo key_info = GetKeyInfo(pressed_key);
            int mod_flags = ModifiersToFlags(mods);

            // Helper to send a key event
            auto send_key_event =
                [](AbpCdpClient* cdp_client, const std::string& type,
                   const KeyInfo& info, int modifiers,
                   base::OnceCallback<void(bool, const std::string&)>
                       callback) {
                  base::Value::Dict key_params;
                  key_params.Set("type", type);
                  key_params.Set("key", info.key);
                  key_params.Set("code", info.code);
                  key_params.Set("windowsVirtualKeyCode", info.windows_virtual_key);
                  key_params.Set("nativeVirtualKeyCode", info.native_virtual_key);
                  key_params.Set("modifiers", modifiers);
                  cdp_client->SendCommand("Input.dispatchKeyEvent",
                                          std::move(key_params),
                                          std::move(callback));
                };

            // For shortcuts with modifiers: press modifiers down, press key,
            // release key, release modifiers
            // For simple key press: just keyDown + keyUp

            if (mods.empty()) {
              // Simple key press: keyDown then keyUp
              send_key_event(
                  client, "keyDown", key_info, mod_flags,
                  base::BindOnce(
                      [](KeyInfo info, int flags, AbpCdpClient* cdp_client,
                         scoped_refptr<AbpActionContext> action_ctx,
                         bool success, const std::string& result) {
                        if (!success) {
                          action_ctx->OnActionError("CDP_ERROR", result);
                          return;
                        }

                        // Now send keyUp
                        base::Value::Dict up_params;
                        up_params.Set("type", "keyUp");
                        up_params.Set("key", info.key);
                        up_params.Set("code", info.code);
                        up_params.Set("windowsVirtualKeyCode",
                                      info.windows_virtual_key);
                        up_params.Set("nativeVirtualKeyCode",
                                      info.native_virtual_key);
                        up_params.Set("modifiers", flags);

                        cdp_client->SendCommand(
                            "Input.dispatchKeyEvent", std::move(up_params),
                            base::BindOnce(
                                [](std::string key_name,
                                   scoped_refptr<AbpActionContext> ctx,
                                   bool success, const std::string& result) {
                                  if (!success) {
                                    ctx->OnActionError("CDP_ERROR", result);
                                    return;
                                  }

                                  base::Value::Dict res;
                                  res.Set("status", "pressed");
                                  res.Set("key", key_name);
                                  ctx->SetResult(std::move(res));
                                  ctx->OnActionDispatched();
                                },
                                info.key, action_ctx));
                      },
                      key_info, mod_flags, client, ctx_ref));
            } else {
              // Shortcut: need to press modifiers first, then key, then release
              // in reverse. For simplicity, we'll send all modifier keyDowns,
              // then main key down+up, then modifier keyUps

              // This is a bit complex - we need to chain multiple CDP calls
              // Let's do it step by step using a state machine approach

              // State machine for chaining modifier key presses/releases.
              // Uses static methods with unique_ptr ownership transfer
              // through each async callback for automatic cleanup.
              struct ShortcutState {
                std::vector<std::string> modifiers;
                KeyInfo main_key;
                int mod_flags;
                size_t mod_index = 0;
                raw_ptr<AbpCdpClient> client;
                scoped_refptr<AbpActionContext> ctx;

                static void PressNextModifier(
                    std::unique_ptr<ShortcutState> state) {
                  if (state->mod_index < state->modifiers.size()) {
                    KeyInfo mod_info =
                        GetKeyInfo(state->modifiers[state->mod_index]);
                    state->mod_index++;

                    base::Value::Dict params;
                    params.Set("type", "keyDown");
                    params.Set("key", mod_info.key);
                    params.Set("code", mod_info.code);
                    params.Set("windowsVirtualKeyCode",
                               mod_info.windows_virtual_key);
                    params.Set("nativeVirtualKeyCode",
                               mod_info.native_virtual_key);
                    // Modifiers accumulate as we press them
                    int current_mods = 0;
                    for (size_t i = 0; i < state->mod_index; i++) {
                      KeyInfo ki = GetKeyInfo(state->modifiers[i]);
                      current_mods |= ki.modifier_flag;
                    }
                    params.Set("modifiers", current_mods);

                    AbpCdpClient* c = state->client;
                    c->SendCommand(
                        "Input.dispatchKeyEvent", std::move(params),
                        base::BindOnce(
                            [](std::unique_ptr<ShortcutState> s, bool success,
                               const std::string& result) {
                              if (!success) {
                                s->ctx->OnActionError("CDP_ERROR", result);
                                return;
                              }
                              PressNextModifier(std::move(s));
                            },
                            std::move(state)));
                  } else {
                    // All modifiers pressed, now press the main key
                    PressMainKey(std::move(state));
                  }
                }

                static void PressMainKey(
                    std::unique_ptr<ShortcutState> state) {
                  base::Value::Dict params;
                  params.Set("type", "keyDown");
                  params.Set("key", state->main_key.key);
                  params.Set("code", state->main_key.code);
                  params.Set("windowsVirtualKeyCode",
                             state->main_key.windows_virtual_key);
                  params.Set("nativeVirtualKeyCode",
                             state->main_key.native_virtual_key);
                  params.Set("modifiers", state->mod_flags);

                  AbpCdpClient* c = state->client;
                  c->SendCommand(
                      "Input.dispatchKeyEvent", std::move(params),
                      base::BindOnce(
                          [](std::unique_ptr<ShortcutState> s, bool success,
                             const std::string& result) {
                            if (!success) {
                              s->ctx->OnActionError("CDP_ERROR", result);
                              return;
                            }
                            ReleaseMainKey(std::move(s));
                          },
                          std::move(state)));
                }

                static void ReleaseMainKey(
                    std::unique_ptr<ShortcutState> state) {
                  base::Value::Dict params;
                  params.Set("type", "keyUp");
                  params.Set("key", state->main_key.key);
                  params.Set("code", state->main_key.code);
                  params.Set("windowsVirtualKeyCode",
                             state->main_key.windows_virtual_key);
                  params.Set("nativeVirtualKeyCode",
                             state->main_key.native_virtual_key);
                  params.Set("modifiers", state->mod_flags);

                  AbpCdpClient* c = state->client;
                  c->SendCommand(
                      "Input.dispatchKeyEvent", std::move(params),
                      base::BindOnce(
                          [](std::unique_ptr<ShortcutState> s, bool success,
                             const std::string& result) {
                            if (!success) {
                              s->ctx->OnActionError("CDP_ERROR", result);
                              return;
                            }
                            s->mod_index = s->modifiers.size();
                            ReleaseNextModifier(std::move(s));
                          },
                          std::move(state)));
                }

                static void ReleaseNextModifier(
                    std::unique_ptr<ShortcutState> state) {
                  if (state->mod_index > 0) {
                    state->mod_index--;
                    KeyInfo mod_info =
                        GetKeyInfo(state->modifiers[state->mod_index]);

                    // Calculate remaining modifiers
                    int remaining_mods = 0;
                    for (size_t i = 0; i < state->mod_index; i++) {
                      KeyInfo ki = GetKeyInfo(state->modifiers[i]);
                      remaining_mods |= ki.modifier_flag;
                    }

                    base::Value::Dict params;
                    params.Set("type", "keyUp");
                    params.Set("key", mod_info.key);
                    params.Set("code", mod_info.code);
                    params.Set("windowsVirtualKeyCode",
                               mod_info.windows_virtual_key);
                    params.Set("nativeVirtualKeyCode",
                               mod_info.native_virtual_key);
                    params.Set("modifiers", remaining_mods);

                    AbpCdpClient* c = state->client;
                    c->SendCommand(
                        "Input.dispatchKeyEvent", std::move(params),
                        base::BindOnce(
                            [](std::unique_ptr<ShortcutState> s, bool success,
                               const std::string& result) {
                              if (!success) {
                                s->ctx->OnActionError("CDP_ERROR", result);
                                return;
                              }
                              ReleaseNextModifier(std::move(s));
                            },
                            std::move(state)));
                  } else {
                    // All done!
                    base::Value::Dict res;
                    res.Set("status", "pressed");
                    res.Set("key", state->main_key.key);
                    base::Value::List mod_list;
                    for (const auto& m : state->modifiers) {
                      mod_list.Append(m);
                    }
                    res.Set("modifiers", std::move(mod_list));
                    state->ctx->SetResult(std::move(res));
                    state->ctx->OnActionDispatched();
                  }
                }
              };

              auto state = std::make_unique<ShortcutState>();
              state->modifiers = std::move(mods);
              state->main_key = key_info;
              state->mod_flags = mod_flags;
              state->client = client;
              state->ctx = ctx_ref;
              ShortcutState::PressNextModifier(std::move(state));
            }
          },
          std::move(key_copy), std::move(modifiers)),
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

  // Use AbpActionContext for unified action flow
  AbpActionContext::Run(
      controller_, tab_id, "key_down", params,
      base::BindOnce(
          [](std::string pressed_key, AbpController* controller,
             AbpActionContext* ctx) {
            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            KeyInfo key_info = GetKeyInfo(pressed_key);

            // Track the held key
            auto& held_state =
                controller->GetOrCreateTabState(ctx->tab_id()).held_keys;
            held_state.held_keys.insert(pressed_key);
            if (key_info.is_modifier) {
              held_state.current_modifiers |= key_info.modifier_flag;
            }

            int current_mods = held_state.current_modifiers;

            base::Value::Dict key_params;
            key_params.Set("type", "keyDown");
            key_params.Set("key", key_info.key);
            key_params.Set("code", key_info.code);
            key_params.Set("windowsVirtualKeyCode", key_info.windows_virtual_key);
            key_params.Set("nativeVirtualKeyCode", key_info.native_virtual_key);
            key_params.Set("modifiers", current_mods);

            client->SendCommand(
                "Input.dispatchKeyEvent", std::move(key_params),
                base::BindOnce(
                    [](std::string key_name,
                       scoped_refptr<AbpActionContext> action_ctx, bool success,
                       const std::string& result) {
                      if (!success) {
                        action_ctx->OnActionError("CDP_ERROR", result);
                        return;
                      }

                      base::Value::Dict res;
                      res.Set("status", "key_down");
                      res.Set("key", key_name);
                      action_ctx->SetResult(std::move(res));
                      action_ctx->OnActionDispatched();
                    },
                    pressed_key, ctx_ref));
          },
          std::move(key_copy), controller_),
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

  // Use AbpActionContext for unified action flow
  AbpActionContext::Run(
      controller_, tab_id, "key_up", params,
      base::BindOnce(
          [](std::string released_key, AbpController* controller,
             AbpActionContext* ctx) {
            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            scoped_refptr<AbpActionContext> ctx_ref(ctx);
            KeyInfo key_info = GetKeyInfo(released_key);

            // Update held key tracking
            auto& held_state =
                controller->GetOrCreateTabState(ctx->tab_id()).held_keys;
            held_state.held_keys.erase(released_key);
            if (key_info.is_modifier) {
              held_state.current_modifiers &= ~key_info.modifier_flag;
            }

            int current_mods = held_state.current_modifiers;

            base::Value::Dict key_params;
            key_params.Set("type", "keyUp");
            key_params.Set("key", key_info.key);
            key_params.Set("code", key_info.code);
            key_params.Set("windowsVirtualKeyCode", key_info.windows_virtual_key);
            key_params.Set("nativeVirtualKeyCode", key_info.native_virtual_key);
            key_params.Set("modifiers", current_mods);

            client->SendCommand(
                "Input.dispatchKeyEvent", std::move(key_params),
                base::BindOnce(
                    [](std::string key_name,
                       scoped_refptr<AbpActionContext> action_ctx, bool success,
                       const std::string& result) {
                      if (!success) {
                        action_ctx->OnActionError("CDP_ERROR", result);
                        return;
                      }

                      base::Value::Dict res;
                      res.Set("status", "key_up");
                      res.Set("key", key_name);
                      action_ctx->SetResult(std::move(res));
                      action_ctx->OnActionDispatched();
                    },
                    released_key, ctx_ref));
          },
          std::move(key_copy), controller_),
      std::move(callback));
}

}  // namespace abp
