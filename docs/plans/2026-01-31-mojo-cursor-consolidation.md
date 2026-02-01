# Mojo Virtual Cursor Consolidation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove all CDP `Overlay.setVirtualCursor` usage from ABP and fix the Mojo-based virtual cursor to work reliably at browser startup.

**Architecture:** The codebase currently has two virtual cursor systems running in parallel: a CDP-based system (`Overlay.setVirtualCursor` → `InspectorOverlayAgent` → `VirtualCursorTool`) and a Mojo-based system (`SetVirtualCursorEnabled` → `RenderWidgetHostImpl` → `WebFrameWidgetImpl` → `FrameOverlay`). The CDP system works but relies on DevTools internals. The Mojo system is cleaner but has a startup timing bug. This plan fixes the Mojo system and removes all CDP cursor usage from ABP.

**Tech Stack:** C++, Mojo IPC, Blink FrameOverlay, Chrome DevTools Protocol (removal)

---

## Background

### Two Cursor Systems (Current State)

| System | Entry Point | Renderer Implementation | Status |
|--------|-------------|-------------------------|--------|
| CDP | `Overlay.setVirtualCursor` | `InspectorOverlayAgent::setVirtualCursor()` → `VirtualCursorTool` | Works, but wrong abstraction layer |
| Mojo | `SetVirtualCursorEnabled/Position/Type` | `WebFrameWidgetImpl` → `VirtualCursorOverlayDelegate` → `FrameOverlay` | Broken at startup |

### The Startup Bug

The Mojo cursor doesn't appear at startup because:
1. `PollForReadyAndCenterCursor()` calls `CenterCursorInTab()` when `IsBrowserReady()` returns true
2. `IsBrowserReady()` checks if `GetRenderWidgetHostView()` is non-null
3. BUT `blink_frame_widget_` in `RenderWidgetHostImpl` may not be bound yet
4. So the Mojo call stores pending state, but when `BindFrameWidgetInterfaces()` is called, the pending state isn't applied correctly OR the overlay isn't painting

### CDP Usage in ABP (To Remove)

1. **`abp_input_dispatcher.cc`** - Click/Move actions send `Overlay.setVirtualCursor` for cursor style detection
2. **`abp_controller.cc`** - Screenshot code sends `Overlay.setVirtualCursor` to position cursor before capture

---

## Task 1: Debug and Fix Mojo Cursor Startup

**Files:**
- Modify: `content/browser/renderer_host/render_widget_host_impl.cc`
- Modify: `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc`

**Step 1: Add diagnostic logging to trace the startup flow**

In `render_widget_host_impl.cc`, add logging at key points:

```cpp
// In SetVirtualCursorEnabled()
void RenderWidgetHostImpl::SetVirtualCursorEnabled(bool enabled) {
  LOG(INFO) << "ABP: SetVirtualCursorEnabled(" << enabled << ")"
            << " blink_frame_widget_bound=" << (blink_frame_widget_ ? "true" : "false")
            << " virtual_cursor_remote_bound=" << virtual_cursor_remote_.is_bound();

  pending_virtual_cursor_state_.has_enabled = true;
  pending_virtual_cursor_state_.enabled = enabled;
  // ... rest of implementation
}

// In BindFrameWidgetInterfaces()
void RenderWidgetHostImpl::BindFrameWidgetInterfaces(...) {
  // ... existing binding code ...

  LOG(INFO) << "ABP: BindFrameWidgetInterfaces called"
            << " pending_enabled=" << pending_virtual_cursor_state_.has_enabled
            << " pending_position=" << pending_virtual_cursor_state_.has_position;

  // Apply pending state...
}
```

**Step 2: Run browser and capture logs**

Run: `./out/Default/Chromium.app/Contents/MacOS/Chromium --enable-abp --enable-logging --v=0 2>&1 | tee /tmp/cursor-startup.log`

Expected: Log output showing the sequence of calls and whether pending state is being set/applied.

**Step 3: Fix the root cause**

Based on logs, fix the timing issue. The likely fix is one of:

A) **If pending state isn't being stored**: `CenterCursorInTab()` runs AFTER `BindFrameWidgetInterfaces()`, so pending state mechanism is never used. Fix: Make `IsBrowserReady()` also check that `blink_frame_widget_` is bound.

B) **If pending state is stored but not applied**: The `BindFrameWidgetInterfaces()` code has a bug. Fix: Ensure the pending state application code is correct.

C) **If pending state is applied but overlay doesn't paint**: The `FrameOverlay` creation or `ScheduleAnimation()` call has a timing issue. Fix: Ensure the overlay is created and scheduled to paint correctly.

**Step 4: Verify fix**

Run: `./out/Default/Chromium.app/Contents/MacOS/Chromium --enable-abp`
Expected: Virtual cursor visible immediately on the New Tab page without any ABP action.

**Step 5: Commit**

```bash
git add content/browser/renderer_host/render_widget_host_impl.cc
git add third_party/blink/renderer/core/frame/web_frame_widget_impl.cc
git commit -m "fix(abp): fix virtual cursor not appearing at browser startup

The Mojo-based virtual cursor wasn't appearing at startup because
[describe root cause based on findings].

Fixed by [describe fix].

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 2: Add Cursor Style Detection to Mojo System

**Files:**
- Modify: `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc`
- Modify: `third_party/blink/renderer/core/frame/web_frame_widget_impl.h`

The CDP system returns detected cursor style (pointer, hand, text, etc.) from hit-testing. The Mojo system needs this capability to remove CDP dependency.

**Step 1: Verify DetectCursorStyleAtPosition exists**

Read the existing implementation in `web_frame_widget_impl.cc` to confirm hit-testing is already implemented.

**Step 2: Ensure cursor type is set after SetPosition**

In `WebFrameWidgetImpl::SetPosition()`, verify it calls `DetectCursorStyleAtPosition()` and updates the delegate:

```cpp
void WebFrameWidgetImpl::SetPosition(float x, float y, bool visible) {
  virtual_cursor_x_ = x;
  virtual_cursor_y_ = y;
  virtual_cursor_visible_ = visible;

  if (virtual_cursor_delegate_) {
    virtual_cursor_delegate_->SetPosition(x, y);
    virtual_cursor_delegate_->SetVisible(visible);

    // Detect and set cursor type
    ui::mojom::CursorType cursor_type = DetectCursorStyleAtPosition(x, y);
    virtual_cursor_delegate_->SetCursorType(cursor_type);

    // Schedule repaint
    if (LocalFrame* frame = LocalRootImpl()->GetFrame()) {
      if (frame->GetPage()) {
        frame->GetPage()->GetChromeClient().ScheduleAnimation(frame->View());
      }
    }
  }
}
```

**Step 3: Verify the implementation matches expected behavior**

Run: Take screenshot, move cursor over a link, take screenshot again.
Expected: Cursor should change from pointer to hand when over a link.

**Step 4: Commit**

```bash
git add third_party/blink/renderer/core/frame/web_frame_widget_impl.cc
git commit -m "feat(abp): ensure cursor style detection in Mojo path

Verify that SetPosition() performs hit-testing and updates cursor type
automatically, matching the behavior of CDP Overlay.setVirtualCursor.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 3: Remove CDP Cursor from Click Action

**Files:**
- Modify: `chrome/browser/abp/abp_input_dispatcher.cc`

**Step 1: Read the Click action implementation**

Read `abp_input_dispatcher.cc` lines 17-144 to understand the current flow.

**Step 2: Remove CDP Overlay.setVirtualCursor call**

The Click action currently:
1. Calls `UpdateVirtualCursorState()` (internal state)
2. Calls `SetVirtualCursorEnabledViaMojo()` and `SetVirtualCursorViaMojo()` (Mojo path)
3. Calls `Overlay.setVirtualCursor` via CDP (redundant, remove this)

Remove the CDP call and its callback handling. The Mojo path already handles cursor positioning.

Before:
```cpp
// Set virtual cursor position via CDP overlay (for style detection)
client->SendCommand(
    "Overlay.setVirtualCursor", std::move(cursor_params),
    base::BindOnce([...](bool success, const std::string& result) {
      // Parse detected cursor style...
      // Dispatch mouse events...
    }));
```

After:
```cpp
// Mojo path handles cursor positioning and style detection
// Dispatch mouse events directly
DispatchMouseEventsForClick(ctx, coord_x, coord_y, button, click_count);
```

**Step 3: Rebuild and test**

Run: `autoninja -C out/Default chrome`
Test: Click on various elements and verify cursor appears and style changes correctly.

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_input_dispatcher.cc
git commit -m "refactor(abp): remove CDP cursor from Click action

Use only the Mojo virtual cursor path for cursor positioning.
The Mojo system now handles cursor style detection via hit-testing.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 4: Remove CDP Cursor from Move Action

**Files:**
- Modify: `chrome/browser/abp/abp_input_dispatcher.cc`

**Step 1: Read the Move action implementation**

Read `abp_input_dispatcher.cc` lines 198-269 to understand the current flow.

**Step 2: Remove CDP Overlay.setVirtualCursor call**

Same pattern as Click - remove the CDP call and use only Mojo path.

**Step 3: Rebuild and test**

Run: `autoninja -C out/Default chrome`
Test: Move cursor around and verify it updates correctly.

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_input_dispatcher.cc
git commit -m "refactor(abp): remove CDP cursor from Move action

Use only the Mojo virtual cursor path for cursor positioning.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 5: Remove CDP Cursor from Screenshot

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc`

**Step 1: Read the Screenshot implementation**

Read `abp_controller.cc` lines 1294-1479 to understand the current flow.

**Step 2: Identify CDP cursor calls in screenshot path**

The screenshot code calls `Overlay.setVirtualCursor` at lines 1460 and 1583 to:
1. Set cursor position before capture
2. Get detected cursor style for the response

**Step 3: Remove CDP calls and use existing Mojo cursor state**

The cursor is already positioned via Mojo from previous input actions. For screenshots:
- The cursor overlay is already rendered at the correct position
- Just capture the screen with `CopyFromSurface()` - cursor will be included
- Remove the `OnCursorSetForScreenshot` callback chain

Before:
```cpp
client->SendCommand(
    "Overlay.setVirtualCursor", cursor_params,
    base::BindOnce(&AbpController::OnCursorSetForScreenshot, ...));
```

After:
```cpp
// Cursor already positioned via Mojo, just capture
CaptureScreenshotWithCursor(wc, options, std::move(callback));
```

**Step 4: Rebuild and test**

Run: `autoninja -C out/Default chrome`
Test: Take screenshots and verify cursor appears at correct position.

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc
git commit -m "refactor(abp): remove CDP cursor from Screenshot

Use the existing Mojo cursor overlay state instead of setting cursor
position via CDP before screenshot capture.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 6: Remove CDP Cursor Response Handling

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc`
- Modify: `chrome/browser/abp/abp_controller.h`

**Step 1: Find all OnCursorSet* callback methods**

Search for methods that handle `Overlay.setVirtualCursor` responses:
- `OnCursorSetForScreenshot`
- Any other cursor-related callbacks

**Step 2: Remove these callback methods**

Delete the methods and their declarations from both .cc and .h files.

**Step 3: Remove any helper methods that are now unused**

Check if there are helper methods only used for CDP cursor that can be removed.

**Step 4: Rebuild and verify**

Run: `autoninja -C out/Default chrome`
Expected: Build succeeds with no undefined references.

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc chrome/browser/abp/abp_controller.h
git commit -m "refactor(abp): remove CDP cursor callback handlers

Remove OnCursorSetForScreenshot and related methods that handled
Overlay.setVirtualCursor CDP responses.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 7: Update Plans Documentation

**Files:**
- Modify: `plans/virtual_cursor.md`

**Step 1: Read current documentation**

Review the existing `plans/virtual_cursor.md` to understand what needs updating.

**Step 2: Update architecture section**

Remove references to "dual path" (CDP + Mojo) approach. Update to reflect single Mojo path:

```markdown
### Architecture

The virtual cursor uses a single Mojo-based path:

1. **Browser Process**: `AbpController` maintains cursor state and sends updates via `RenderWidgetHostImpl`
2. **Mojo IPC**: `VirtualCursor.mojom` interface carries position/type/visibility
3. **Renderer Process**: `WebFrameWidgetImpl` receives updates and manages `VirtualCursorOverlayDelegate`
4. **Rendering**: `FrameOverlay` with cursor bitmap drawn via `InspectorCursorDrawer`

**Removed**: CDP `Overlay.setVirtualCursor` path is no longer used by ABP.
```

**Step 3: Update data flow diagrams**

Remove the "CDP Path (async)" from the data flow diagrams. Simplify to show only Mojo path.

**Step 4: Add warning about CDP approach**

Add a clear note:

```markdown
## Important: Do Not Use CDP for Cursor

The Mojo-based virtual cursor system is the correct approach for ABP.
Do NOT add CDP `Overlay.setVirtualCursor` calls back into ABP code.

Reasons:
1. CDP cursor relies on DevTools/Inspector overlay internals
2. Mojo path is cleaner and doesn't require DevTools to be attached
3. Single source of truth simplifies debugging
```

**Step 5: Commit**

```bash
git add plans/virtual_cursor.md
git commit -m "docs(plans): update virtual cursor to reflect Mojo-only approach

Remove references to CDP Overlay.setVirtualCursor dual-path approach.
Add warning to not re-add CDP cursor usage.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 8: Verify Complete Removal of CDP Cursor

**Files:**
- All files in `chrome/browser/abp/`

**Step 1: Search for any remaining CDP cursor references**

Run: `grep -r "setVirtualCursor\|Overlay.setVirtualCursor" chrome/browser/abp/`

Expected: No matches found.

**Step 2: Search for VirtualCursorTool references**

Run: `grep -r "VirtualCursorTool" chrome/browser/abp/`

Expected: No matches found (VirtualCursorTool is the CDP cursor implementation).

**Step 3: End-to-end test**

Test all cursor scenarios:
1. Browser startup - cursor appears without any action
2. Click action - cursor moves to click position
3. Move action - cursor follows move commands
4. Screenshot - cursor appears in screenshot at correct position
5. Cursor style - changes when hovering over links, inputs, etc.

**Step 4: Final commit with test verification**

```bash
git add -A
git commit -m "test(abp): verify complete removal of CDP virtual cursor

All ABP cursor functionality now uses only the Mojo-based path.
Verified: startup, click, move, screenshot, cursor style detection.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Verification Checklist

After completing all tasks, verify:

- [ ] Virtual cursor appears immediately at browser startup (no action required)
- [ ] Click action moves cursor to correct position
- [ ] Move action moves cursor smoothly
- [ ] Cursor style changes when hovering over different element types
- [ ] Screenshot includes cursor at correct position
- [ ] Screenshot with `cursor: false` excludes cursor
- [ ] No `Overlay.setVirtualCursor` or `VirtualCursorTool` references in ABP code
- [ ] `plans/virtual_cursor.md` updated with Mojo-only architecture
- [ ] Build succeeds with no warnings related to cursor

---

## Rollback Plan

If issues are discovered after deployment:

1. The CDP cursor code still exists in Blink's Inspector (`InspectorOverlayAgent`, `VirtualCursorTool`)
2. Re-adding CDP calls to ABP is straightforward if needed
3. Keep git commits atomic so individual changes can be reverted

---

## Files Summary

**Modified:**
- `content/browser/renderer_host/render_widget_host_impl.cc` - Fix startup timing
- `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc` - Verify cursor style detection
- `chrome/browser/abp/abp_input_dispatcher.cc` - Remove CDP from Click/Move
- `chrome/browser/abp/abp_controller.cc` - Remove CDP from Screenshot
- `chrome/browser/abp/abp_controller.h` - Remove CDP callback declarations
- `plans/virtual_cursor.md` - Update documentation

**Not Modified (kept for other uses):**
- `third_party/blink/renderer/core/inspector/inspector_overlay_agent.cc` - CDP still available for DevTools
- `third_party/blink/renderer/core/inspector/virtual_cursor_tool.cc` - CDP still available for DevTools
