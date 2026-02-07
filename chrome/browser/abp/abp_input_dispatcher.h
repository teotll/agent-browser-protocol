#ifndef CHROME_BROWSER_ABP_ABP_INPUT_DISPATCHER_H_
#define CHROME_BROWSER_ABP_ABP_INPUT_DISPATCHER_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/values.h"
#include "chrome/browser/abp/abp_types.h"
#include "third_party/blink/public/common/input/web_input_event.h"

namespace content {
class WebContents;
}

namespace abp {

class AbpActionContext;
struct KeyInfo;

class AbpController;

// Handles all input-related ABP actions.
// Extracted from AbpController for better modularity.
//
// Input methods:
// - Click: Mouse click at coordinates
// - Type: Text input
// - Move: Mouse move to coordinates
// - Scroll: Scroll the page
// - KeyPress: Press and release a key
// - KeyDown: Press a key (hold)
// - KeyUp: Release a held key
//
// All methods use AbpActionContext for unified action flow:
// Resume -> BeforeScreenshot -> Action -> Wait -> Pause -> AfterScreenshot -> Response
class AbpInputDispatcher {
 public:
  explicit AbpInputDispatcher(AbpController* controller);
  ~AbpInputDispatcher();

  AbpInputDispatcher(const AbpInputDispatcher&) = delete;
  AbpInputDispatcher& operator=(const AbpInputDispatcher&) = delete;

  // Mouse click at specified coordinates
  void Click(const std::string& tab_id,
             const base::Value::Dict& params,
             ResponseCallback callback);

  // Type text into the focused element
  void Type(const std::string& tab_id,
            const base::Value::Dict& params,
            ResponseCallback callback);

  // Move mouse to specified coordinates
  void Move(const std::string& tab_id,
            const base::Value::Dict& params,
            ResponseCallback callback);

  // Scroll the page
  void Scroll(const std::string& tab_id,
              const base::Value::Dict& params,
              ResponseCallback callback);

  // Press and release a key
  void KeyPress(const std::string& tab_id,
                const base::Value::Dict& params,
                ResponseCallback callback);

  // Press a key (hold it down)
  void KeyDown(const std::string& tab_id,
               const base::Value::Dict& params,
               ResponseCallback callback);

  // Release a held key
  void KeyUp(const std::string& tab_id,
             const base::Value::Dict& params,
             ResponseCallback callback);

 private:
  // Forward a native keyboard event to the renderer (bypasses CDP entirely).
  // Uses the same code path as real keyboard input.
  void ForwardKeyEvent(content::WebContents* wc,
                       blink::WebInputEvent::Type type,
                       const KeyInfo& info,
                       int web_modifiers);

  // Type characters one-by-one with delays (recursive via PostDelayedTask)
  void TypeNextCharacter(scoped_refptr<AbpActionContext> ctx,
                         std::string text,
                         size_t char_index);

  // Controller reference (not owned)
  raw_ptr<AbpController> controller_;
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_INPUT_DISPATCHER_H_
