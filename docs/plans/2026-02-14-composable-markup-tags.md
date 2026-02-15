# Composable Markup Tags Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the single-string `markup` parameter with a composable array of tags (`clickable`, `typeable`, `scrollable`, `grid`).

**Architecture:** DOM injection via `Runtime.evaluate` with `disableBreaks: true`. Pure CSS tags (`clickable`, `typeable`) inject `<style>` rules. JS-assisted tags (`scrollable`) walk the DOM and add CSS classes. The `grid` tag injects a fixed overlay div with CSS gradients and coordinate labels. All markup is wrapped in a single `<div id="abp-markup-overlay">` for one-call cleanup.

**Tech Stack:** C++ (Chromium), TypeScript (npm package)

---

### Task 1: Update ScreenshotOptions struct and action context types

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:200-202`
- Modify: `chrome/browser/abp/abp_action_context.h:219-220`
- Modify: `chrome/browser/abp/abp_action_context.cc:190-205`
- Modify: `chrome/browser/abp/abp_action_context.cc:327`
- Modify: `chrome/browser/abp/abp_action_context.cc:558`

**Step 1: Update ScreenshotOptions in abp_controller.h**

Change line 202 from:
```cpp
    std::string markup = "none";     // none, interactive, clickable, typeable, inputs
```
to:
```cpp
    std::vector<std::string> markup_tags;  // clickable, typeable, scrollable, grid
```

Ensure `#include <vector>` is present at the top of the file (it likely already is).

**Step 2: Update action context member in abp_action_context.h**

Change line 220 from:
```cpp
  std::string screenshot_markup_ = "none";
```
to:
```cpp
  std::vector<std::string> screenshot_markup_tags_;
```

**Step 3: Update action context parsing in abp_action_context.cc**

Replace lines 193-196:
```cpp
    const std::string* markup = ss_params->FindString("markup");
    if (markup) {
      screenshot_markup_ = *markup;
    }
```
with:
```cpp
    const base::Value::List* markup_list = ss_params->FindList("markup");
    if (markup_list) {
      for (const auto& tag : *markup_list) {
        if (tag.is_string()) {
          screenshot_markup_tags_.push_back(tag.GetString());
        }
      }
    }
```

**Step 4: Update ScreenshotOptions assignment (two locations)**

At line 327, change:
```cpp
  opts.markup = screenshot_markup_;
```
to:
```cpp
  opts.markup_tags = screenshot_markup_tags_;
```

At line 558, change:
```cpp
  opts.markup = screenshot_markup_;
```
to:
```cpp
  opts.markup_tags = screenshot_markup_tags_;
```

**Step 5: Build to verify compilation**

Run: `autoninja -C out/Default chrome`
Expected: Compilation errors in abp_controller.cc where `options.markup` is referenced — that's expected, we'll fix those in Task 2.

**Step 6: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_action_context.h chrome/browser/abp/abp_action_context.cc
git commit -m "refactor: change markup from string to vector<string> in types"
```

---

### Task 2: Rewrite markup injection in abp_controller.cc

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc:906-976` (injection)
- Modify: `chrome/browser/abp/abp_controller.cc:1142-1157` (cleanup)

**Step 1: Add a helper to build the injection JS**

Add a new private helper method declaration in `abp_controller.h` (in the private section):

```cpp
  // Build JavaScript for markup tag injection
  static std::string BuildMarkupInjectionScript(
      const std::vector<std::string>& markup_tags);
  // Build JavaScript for markup tag cleanup
  static std::string BuildMarkupCleanupScript(
      const std::vector<std::string>& markup_tags);
```

**Step 2: Implement BuildMarkupInjectionScript in abp_controller.cc**

Add before `CaptureActionScreenshot`:

```cpp
// static
std::string AbpController::BuildMarkupInjectionScript(
    const std::vector<std::string>& markup_tags) {
  // Build CSS rules for pure-CSS tags
  std::string css_rules;
  for (const auto& tag : markup_tags) {
    if (tag == "clickable") {
      css_rules += R"(
        a,[role='link']{outline:2px solid #4CAF50!important;outline-offset:-2px!important}
        button,[role='button'],[onclick],[tabindex]:not([tabindex='-1']){outline:2px solid #4CAF50!important;outline-offset:-2px!important}
      )";
    } else if (tag == "typeable") {
      css_rules += R"(
        input:not([type='hidden']):not([type='checkbox']):not([type='radio']):not([type='submit']):not([type='button']),
        textarea,[contenteditable='true']{outline:2px solid #FF9800!important;outline-offset:-2px!important}
      )";
    } else if (tag == "scrollable") {
      css_rules += R"(
        .abp-scrollable{outline:2px dashed #9C27B0!important;outline-offset:-2px!important}
      )";
    }
  }

  // Check which JS-assisted tags are active
  bool has_scrollable = std::find(markup_tags.begin(), markup_tags.end(),
                                  "scrollable") != markup_tags.end();
  bool has_grid = std::find(markup_tags.begin(), markup_tags.end(),
                            "grid") != markup_tags.end();

  std::string script = "(function(){";

  // Remove any existing overlay
  script += "var old=document.getElementById('abp-markup-overlay');";
  script += "if(old)old.remove();";

  // Create wrapper div
  script += "var w=document.createElement('div');";
  script += "w.id='abp-markup-overlay';";

  // Add style element with CSS rules
  if (!css_rules.empty()) {
    script += "var s=document.createElement('style');";
    script += "s.textContent=`" + css_rules + "`;";
    script += "w.appendChild(s);";
  }

  // Scrollable: walk DOM and add classes
  if (has_scrollable) {
    script += R"(
      document.querySelectorAll('*').forEach(function(el){
        if(el.tagName==='BODY'||el.tagName==='HTML')return;
        var cs=getComputedStyle(el);
        var ov=cs.overflow+cs.overflowX+cs.overflowY;
        if(!/auto|scroll/.test(ov))return;
        if(el.scrollHeight>el.clientHeight||el.scrollWidth>el.clientWidth){
          el.classList.add('abp-scrollable');
        }
      });
    )";
  }

  // Grid: create overlay div with lines and labels
  if (has_grid) {
    script += R"(
      var g=document.createElement('div');
      g.id='abp-markup-grid';
      g.style.cssText='position:fixed;inset:0;z-index:2147483647;pointer-events:none;'+
        'background:repeating-linear-gradient(to right,rgba(255,0,0,0.3) 0px,rgba(255,0,0,0.3) 1px,transparent 1px,transparent 100px),'+
        'repeating-linear-gradient(to bottom,rgba(255,0,0,0.3) 0px,rgba(255,0,0,0.3) 1px,transparent 1px,transparent 100px)';
      var vw=document.documentElement.clientWidth;
      var vh=document.documentElement.clientHeight;
      for(var x=100;x<vw;x+=100){
        var lbl=document.createElement('div');
        lbl.style.cssText='position:absolute;top:0;left:'+x+'px;color:rgba(255,0,0,0.7);font:bold 10px monospace;padding:1px 2px;background:rgba(255,255,255,0.8)';
        lbl.textContent=x;
        g.appendChild(lbl);
      }
      for(var y=100;y<vh;y+=100){
        var lbl=document.createElement('div');
        lbl.style.cssText='position:absolute;left:0;top:'+y+'px;color:rgba(255,0,0,0.7);font:bold 10px monospace;padding:1px 2px;background:rgba(255,255,255,0.8)';
        lbl.textContent=y;
        g.appendChild(lbl);
      }
      w.appendChild(g);
    )";
  }

  // Append wrapper to document
  script += "document.documentElement.appendChild(w);";
  script += "return true;})()";

  return script;
}
```

**Step 3: Implement BuildMarkupCleanupScript**

```cpp
// static
std::string AbpController::BuildMarkupCleanupScript(
    const std::vector<std::string>& markup_tags) {
  std::string script = "(function(){";
  script += "var o=document.getElementById('abp-markup-overlay');";
  script += "if(o)o.remove();";

  bool has_scrollable = std::find(markup_tags.begin(), markup_tags.end(),
                                  "scrollable") != markup_tags.end();
  if (has_scrollable) {
    script += "document.querySelectorAll('.abp-scrollable').forEach(function(el){";
    script += "el.classList.remove('abp-scrollable');});";
  }

  script += "})()";
  return script;
}
```

**Step 4: Replace injection code in CaptureActionScreenshot**

Replace lines 906-976 (the entire `if (options.markup != "none")` block) with:

```cpp
  // If markup tags are requested, inject overlay first
  if (!options.markup_tags.empty()) {
    std::string script = BuildMarkupInjectionScript(options.markup_tags);

    base::Value::Dict js_params;
    js_params.Set("expression", script);
    js_params.Set("returnByValue", true);
    js_params.Set("disableBreaks", true);

    client->SendCommand(
        "Runtime.evaluate", js_params,
        base::BindOnce(
            [](base::WeakPtr<AbpController> ctrl, std::string tid,
               int64_t ts, bool before, ScreenshotOptions opts,
               ActionScreenshotCallback cb, base::FilePath h_path,
               int w, int h, bool success, const std::string& result) {
              if (!ctrl) {
                std::move(cb).Run(ActionScreenshotResult());
                return;
              }
              ctrl->CaptureActionScreenshotCdp(
                  tid, ts, before, opts, std::move(cb), h_path, w, h);
            },
            weak_factory_.GetWeakPtr(), tab_id, timestamp, is_before, options,
            std::move(callback), history_path, view_width, view_height));
    return;
  }
```

**Step 5: Replace cleanup code in OnActionScreenshotCaptured**

Replace lines 1142-1157 with:

```cpp
  // Clean up markup overlay (fire-and-forget)
  if (!options.markup_tags.empty()) {
    content::WebContents* wc = FindWebContents(tab_id);
    if (wc) {
      AbpCdpClient* client = GetOrCreateCdpClient(wc);
      if (client) {
        base::Value::Dict cleanup;
        cleanup.Set("expression",
            BuildMarkupCleanupScript(options.markup_tags));
        cleanup.Set("returnByValue", true);
        cleanup.Set("disableBreaks", true);
        client->SendCommand("Runtime.evaluate", cleanup,
                            base::BindOnce([](bool, const std::string&) {}));
      }
    }
  }
```

**Step 6: Build to verify**

Run: `autoninja -C out/Default chrome`
Expected: Compilation errors in BinaryScreenshot and MCP handler — fixed in Tasks 3 and 4.

**Step 7: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "feat: implement composable markup tag injection and cleanup"
```

---

### Task 3: Update request parsing (POST and GET endpoints)

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc:4265-4275` (GET query parsing)

**Step 1: Update BinaryScreenshot GET query parsing**

Replace lines 4265-4275:
```cpp
  // Parse query params for markup option
  // Format: ?markup=interactive or ?markup=none
  std::string markup = "none";
  if (!query.empty()) {
    size_t pos = query.find("markup=");
    if (pos != std::string::npos) {
      size_t start = pos + 7;
      size_t end = query.find('&', start);
      markup = query.substr(start, end == std::string::npos ? end : end - start);
    }
  }
```

with:

```cpp
  // Parse query params for markup tags
  // Format: ?markup=clickable,grid
  std::vector<std::string> markup_tags;
  if (!query.empty()) {
    size_t pos = query.find("markup=");
    if (pos != std::string::npos) {
      size_t start = pos + 7;
      size_t end = query.find('&', start);
      std::string markup_str =
          query.substr(start, end == std::string::npos ? end : end - start);
      // Split comma-separated tags
      size_t tag_start = 0;
      while (tag_start < markup_str.size()) {
        size_t comma = markup_str.find(',', tag_start);
        if (comma == std::string::npos) comma = markup_str.size();
        std::string tag = markup_str.substr(tag_start, comma - tag_start);
        if (!tag.empty()) {
          markup_tags.push_back(tag);
        }
        tag_start = comma + 1;
      }
    }
  }
```

**Step 2: Check if BinaryScreenshot uses the markup variable downstream**

The current GET endpoint parses `markup` but **never uses it** — it goes straight to `EnsureCompositorActive → GrabViewSnapshot`. The markup tags need to be injected before capture. Since the GET path doesn't go through `CaptureActionScreenshot`, we need to add injection here.

After the `markup_tags` parsing, if tags are non-empty, inject markup, ForceRedraw, then capture. Replace the `EnsureCompositorActive` block with markup-aware logic:

After the `wrapped_cb` definition (after line 4302), replace the `EnsureCompositorActive` call:

```cpp
  if (!markup_tags.empty()) {
    // Inject markup, then ForceRedraw + capture
    AbpCdpClient* client = GetOrCreateCdpClient(wc);
    if (!client) {
      SendError(500, "No CDP client", std::move(wrapped_cb));
      return;
    }
    std::string inject_script = BuildMarkupInjectionScript(markup_tags);
    base::Value::Dict js_params;
    js_params.Set("expression", inject_script);
    js_params.Set("returnByValue", true);
    js_params.Set("disableBreaks", true);

    client->SendCommand(
        "Runtime.evaluate", js_params,
        base::BindOnce(
            [](base::WeakPtr<AbpController> ctrl, std::string tid,
               std::vector<std::string> tags, ResponseCallback cb,
               bool success, const std::string& result) {
              if (!ctrl) return;
              // Use GetSnapshotFromBrowser (ForceRedraw path) to capture
              // injected markup, then cleanup
              ctrl->GetSnapshotFromBrowser(
                  tid, false,
                  base::BindOnce(
                      [](base::WeakPtr<AbpController> ctrl, std::string tid,
                         std::vector<std::string> tags, ResponseCallback cb,
                         const gfx::Image& snapshot) {
                        if (!ctrl) return;
                        // Cleanup markup
                        auto* wc = ctrl->FindWebContents(tid);
                        if (wc) {
                          auto* client = ctrl->GetOrCreateCdpClient(wc);
                          if (client) {
                            base::Value::Dict cleanup;
                            cleanup.Set("expression",
                                BuildMarkupCleanupScript(tags));
                            cleanup.Set("returnByValue", true);
                            cleanup.Set("disableBreaks", true);
                            client->SendCommand(
                                "Runtime.evaluate", cleanup,
                                base::BindOnce(
                                    [](bool, const std::string&) {}));
                          }
                        }
                        if (snapshot.IsEmpty()) {
                          std::move(cb).Run(
                              500, "application/json",
                              R"({"error":"Screenshot capture failed"})");
                          return;
                        }
                        const SkBitmap& bitmap = *snapshot.ToSkBitmap();
                        auto encoded = gfx::WebpCodec::Encode(bitmap, 80);
                        if (!encoded || encoded->empty()) {
                          std::move(cb).Run(
                              500, "application/json",
                              R"({"error":"Failed to encode screenshot"})");
                          return;
                        }
                        std::string binary(encoded->begin(), encoded->end());
                        std::move(cb).Run(200, "image/webp",
                                          std::move(binary));
                      },
                      ctrl->weak_factory_.GetWeakPtr(), tid, tags,
                      std::move(cb)));
            },
            weak_factory_.GetWeakPtr(), tab_id, markup_tags,
            std::move(wrapped_cb)));
    return;
  }

  // No markup — direct capture (existing EnsureCompositorActive path)
  EnsureCompositorActive(
      tab_id,
      // ... existing code unchanged ...
```

Note: `GetSnapshotFromBrowser` with `from_surface=false` does ForceRedraw internally, which ensures injected CSS is painted before capture.

**Step 3: Add validation for unknown tags**

Add a validation helper at the top of `abp_controller.cc` (in the anonymous namespace or as a static method):

```cpp
namespace {
const std::set<std::string> kValidMarkupTags = {
    "clickable", "typeable", "scrollable", "grid"};

bool ValidateMarkupTags(const std::vector<std::string>& tags,
                        std::string* invalid_tag) {
  for (const auto& tag : tags) {
    if (kValidMarkupTags.find(tag) == kValidMarkupTags.end()) {
      *invalid_tag = tag;
      return false;
    }
  }
  return true;
}
}  // namespace
```

Add validation in `BinaryScreenshot` after parsing tags:

```cpp
  std::string invalid_tag;
  if (!ValidateMarkupTags(markup_tags, &invalid_tag)) {
    SendError(400, "Unknown markup tag: " + invalid_tag, std::move(callback));
    return;
  }
```

Also add validation in `CaptureActionScreenshot` at the start of the markup injection block (before `BuildMarkupInjectionScript`). The POST path validates via the action context, but add a safety check:

```cpp
  if (!options.markup_tags.empty()) {
    std::string invalid_tag;
    if (!ValidateMarkupTags(options.markup_tags, &invalid_tag)) {
      std::move(callback).Run(ActionScreenshotResult());
      return;
    }
    // ... rest of injection code
```

**Step 4: Build and verify**

Run: `autoninja -C out/Default chrome`
Expected: Compilation error in `abp_mcp_handler.cc` — fixed in Task 4.

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc
git commit -m "feat: update GET/POST screenshot parsing for markup tag arrays"
```

---

### Task 4: Update MCP handler

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:101-104` (tool definition)
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:836-838` (argument mapping)

**Step 1: Update tool definition**

Replace lines 101-104:
```cpp
        .OptionalString("markup",
                        "Element markup overlay: none, interactive, "
                        "clickable, typeable, inputs")
```
with:
```cpp
        .OptionalStringArrayEnum("markup",
                                 "Markup overlays to apply to the screenshot",
                                 {"clickable", "typeable", "scrollable", "grid"})
```

**Step 2: Update argument mapping in CallBrowserScreenshot**

Replace lines 836-838:
```cpp
  if (const std::string* markup = args.FindString("markup")) {
    screenshot_opts.Set("markup", *markup);
  }
```
with:
```cpp
  if (const base::Value::List* markup = args.FindList("markup")) {
    screenshot_opts.Set("markup", markup->Clone());
  }
```

**Step 3: Build and verify full compilation**

Run: `autoninja -C out/Default chrome`
Expected: Clean build, no errors.

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.cc
git commit -m "feat: update MCP browser_screenshot tool for markup tag arrays"
```

---

### Task 5: Update npm package types and client

**Files:**
- Modify: `tools/abp-npm/src/types.ts:11`
- Modify: `tools/abp-npm/src/client.ts:194-195`

**Step 1: Update TypeScript types**

In `tools/abp-npm/src/types.ts`, change line 11 from:
```typescript
  markup?: "none" | "interactive" | "clickable" | "typeable" | "inputs";
```
to:
```typescript
  markup?: ("clickable" | "typeable" | "scrollable" | "grid")[];
```

**Step 2: Update screenshotBinary client method**

In `tools/abp-npm/src/client.ts`, change lines 194-195 from:
```typescript
  async screenshotBinary(tabId: string, options?: { markup?: string }): Promise<Buffer> {
    const query = options?.markup ? `?markup=${options.markup}` : "";
```
to:
```typescript
  async screenshotBinary(tabId: string, options?: { markup?: string[] }): Promise<Buffer> {
    const query = options?.markup?.length ? `?markup=${options.markup.join(",")}` : "";
```

**Step 3: Build npm package**

Run: `cd tools/abp-npm && npm run build`
Expected: Clean build, no TypeScript errors.

**Step 4: Commit**

```bash
git add tools/abp-npm/src/types.ts tools/abp-npm/src/client.ts
git commit -m "feat: update npm package types for composable markup tags"
```

---

### Task 6: Update documentation

**Files:**
- Modify: `plans/API.md` (screenshot markup docs)
- Modify: `tools/abp-npm/README.md` (examples)
- Modify: `tools/abp-claude-skill/abp-browser.md` (tool reference)

**Step 1: Update API.md**

Find the markup parameter documentation (the table with `none`, `interactive`, etc.) and replace with:

```markdown
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `markup` | string[] | `[]` | Markup overlays to apply. Valid tags: `clickable`, `typeable`, `scrollable`, `grid` |
```

Update the markup values table to:

```markdown
| Tag | Description |
|-----|-------------|
| `clickable` | Buttons, links, and other clickable elements (green outline) |
| `typeable` | Text inputs, textareas, contenteditable elements (orange outline) |
| `scrollable` | Elements with scrollable overflow content (purple dashed outline) |
| `grid` | 100px coordinate grid overlay with labels |
```

Update example requests from `"markup": "interactive"` to `"markup": ["clickable", "typeable"]`.

Update GET endpoint docs to show comma-separated format: `?markup=clickable,grid`.

**Step 2: Update README.md**

Update screenshot examples from single string markup to array format.

**Step 3: Update abp-browser.md**

Update the `browser_screenshot` tool docs to show `markup` as an array parameter with the new tag values.

**Step 4: Commit**

```bash
git add plans/API.md tools/abp-npm/README.md tools/abp-claude-skill/abp-browser.md
git commit -m "docs: update markup documentation for composable tags"
```

---

### Task 7: Manual testing

**Step 1: Launch ABP**

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run
```

**Step 2: Create a tab and navigate**

```bash
curl -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'
```

Note the tab_id from the response.

**Step 3: Test individual tags via GET**

```bash
# Clickable only
curl http://localhost:8222/api/v1/tabs/{tab_id}/screenshot?markup=clickable -o clickable.webp

# Grid only
curl http://localhost:8222/api/v1/tabs/{tab_id}/screenshot?markup=grid -o grid.webp

# Multiple tags
curl http://localhost:8222/api/v1/tabs/{tab_id}/screenshot?markup=clickable,typeable,grid -o combined.webp
```

Expected: `clickable.webp` shows green outlines on links. `grid.webp` shows 100px grid with labels. `combined.webp` shows both.

**Step 4: Test via POST (action envelope)**

```bash
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/screenshot \
  -H "Content-Type: application/json" \
  -d '{"screenshot":{"markup":["clickable","grid"],"format":"webp"}}'
```

Expected: JSON response with base64-encoded screenshot showing clickable outlines and grid.

**Step 5: Test scrollable tag**

Navigate to a page with scrollable containers (e.g., a complex web app), then:

```bash
curl http://localhost:8222/api/v1/tabs/{tab_id}/screenshot?markup=scrollable -o scrollable.webp
```

Expected: Purple dashed outlines on scrollable inner containers.

**Step 6: Test validation**

```bash
curl http://localhost:8222/api/v1/tabs/{tab_id}/screenshot?markup=invalid
```

Expected: HTTP 400 with error message about unknown tag.

**Step 7: Test MCP tool**

```bash
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test","version":"1.0"},"capabilities":{}}}'

curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"browser_screenshot","arguments":{"markup":["clickable","grid"]}}}'
```

Expected: MCP response with screenshot content showing markup overlays.

**Step 8: Test empty markup (no overlay)**

```bash
curl http://localhost:8222/api/v1/tabs/{tab_id}/screenshot -o no_markup.webp
```

Expected: Clean screenshot with no overlays.
