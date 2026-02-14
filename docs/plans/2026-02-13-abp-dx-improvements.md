# ABP Developer Experience Improvements — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Rename binary to "ABP", remove `--enable-abp` flag (always-on), default screenshots to ON.

**Architecture:** Three independent changes to branding, startup flow, and config defaults. The rename touches build config + scripts. The flag removal touches browser startup + input filtering. Screenshots is a one-liner.

**Tech Stack:** C++ (Chromium), GN build system, shell scripts

---

### Task 1: Rename BRANDING file

**Files:**
- Modify: `chrome/app/theme/chromium/BRANDING` (all lines)

**Step 1: Update BRANDING**

Replace entire contents of `chrome/app/theme/chromium/BRANDING` with:

```
COMPANY_FULLNAME=The Chromium Authors
COMPANY_SHORTNAME=The Chromium Authors
PRODUCT_FULLNAME=ABP
PRODUCT_SHORTNAME=ABP
PRODUCT_INSTALLER_FULLNAME=ABP Installer
PRODUCT_INSTALLER_SHORTNAME=ABP Installer
COPYRIGHT=Copyright @LASTCHANGE_YEAR@ The Chromium Authors. All rights reserved.
MAC_BUNDLE_ID=com.chromium.abp
MAC_CREATOR_CODE=ABPb
MAC_TEAM_ID=
```

**Step 2: Commit**

```bash
git add chrome/app/theme/chromium/BRANDING
git commit -m "chore: rename product branding from Chromium to ABP"
```

---

### Task 2: Rename binary output on Linux

**Files:**
- Modify: `chrome/BUILD.gn:154`
- Modify: `chrome/installer/linux/common/chromium-browser.info:9,12,15,21`

**Step 1: Change binary output name in BUILD.gn**

In `chrome/BUILD.gn` line 154, change:
```cpp
    _chrome_output_name = "chrome"
```
to:
```cpp
    _chrome_output_name = "abp"
```

**Step 2: Update Linux package info**

In `chrome/installer/linux/common/chromium-browser.info`, change:
- Line 9: `PACKAGE="chromium-browser"` → `PACKAGE="abp-browser"`
- Line 12: `PROGNAME=chrome` → `PROGNAME=abp`
- Line 15: `INSTALLDIR=/opt/chromium.org/chromium` → `INSTALLDIR=/opt/chromium.org/abp`
- Line 21: `MENUNAME="Chromium Web Browser"` → `MENUNAME="ABP"`

**Step 3: Commit**

```bash
git add chrome/BUILD.gn chrome/installer/linux/common/chromium-browser.info
git commit -m "chore: rename Linux binary from chrome to abp"
```

---

### Task 3: Rename binary output on Windows

**Files:**
- Modify: `chrome/BUILD.gn:152,412`
- Modify: `chrome/app/chrome_exe.ver` (full file, 2 lines)
- Modify: `chrome/app/chrome_dll.ver` (full file, 2 lines)
- Modify: `chrome/installer/mini_installer/chrome.release:9,21`

**Step 1: Change Windows binary output name in BUILD.gn**

In `chrome/BUILD.gn` line 152, change:
```cpp
    _chrome_output_name = "initialexe/chrome"
```
to:
```cpp
    _chrome_output_name = "initialexe/abp"
```

In `chrome/BUILD.gn` line 412, change:
```cpp
    output_name = "chrome"
```
to:
```cpp
    output_name = "abp"
```

**Step 2: Update Windows version resource files**

Replace `chrome/app/chrome_exe.ver`:
```
INTERNAL_NAME=abp_exe
ORIGINAL_FILENAME=abp.exe
```

Replace `chrome/app/chrome_dll.ver`:
```
INTERNAL_NAME=abp_dll
ORIGINAL_FILENAME=abp.dll
```

**Step 3: Update Windows installer manifest**

In `chrome/installer/mini_installer/chrome.release`:
- Line 9: `chrome.exe: %(ChromeDir)s\` → `abp.exe: %(ChromeDir)s\`
- Line 21: `chrome.dll: %(VersionDir)s\` → `abp.dll: %(VersionDir)s\`

**Step 4: Commit**

```bash
git add chrome/BUILD.gn chrome/app/chrome_exe.ver chrome/app/chrome_dll.ver chrome/installer/mini_installer/chrome.release
git commit -m "chore: rename Windows binary from chrome to abp"
```

---

### Task 4: Update macOS build/release scripts

**Files:**
- Modify: `tools/abp/build-mac.sh` (lines 36,37,63,64,66)
- Modify: `tools/abp/package-mac.sh` (lines 28,29,46)
- Modify: `tools/abp/release-mac.sh` (lines 56,62,85,86,104,106,113,127,128,130,133,183,252)

**Step 1: Update build-mac.sh**

Replace all occurrences of `Chromium.app` with `ABP.app` in `tools/abp/build-mac.sh`.

**Step 2: Update package-mac.sh**

Replace all occurrences of `Chromium.app` with `ABP.app` in `tools/abp/package-mac.sh`.

**Step 3: Update release-mac.sh**

Replace all occurrences of:
- `Chromium.app` → `ABP.app`
- `Chromium Framework` → `ABP Framework`
- `Contents/MacOS/Chromium` → `Contents/MacOS/ABP`
- `Chromium-notarize.zip` → `ABP-notarize.zip`

**Step 4: Commit**

```bash
git add tools/abp/build-mac.sh tools/abp/package-mac.sh tools/abp/release-mac.sh
git commit -m "chore: update macOS scripts for ABP rename"
```

---

### Task 5: Update Linux/Windows build scripts

**Files:**
- Modify: `tools/abp/build-linux.sh:45`
- Modify: `tools/abp/release-linux.sh:43`
- Modify: `tools/abp/package-linux.sh:24,25,49`
- Modify: `tools/abp/build-win.ps1:46`
- Modify: `tools/abp/release-win.ps1:43`

**Step 1: Update Linux scripts**

In all Linux scripts, replace references to the `chrome` binary with `abp`:
- `out/Release/chrome` → `out/Release/abp`
- `"Chrome binary"` → `"ABP binary"` (in error messages)

In `tools/abp/package-linux.sh` line 49: `cp "$BUILD_DIR/chrome"` → `cp "$BUILD_DIR/abp"`

**Step 2: Update Windows scripts**

In all Windows scripts, replace `chrome.exe` with `abp.exe`:
- `out\Release\chrome.exe` → `out\Release\abp.exe`

**Step 3: Update validation scripts**

In `tools/abp/common/validate.sh` lines 24-25:
- `"Starting Chrome with --enable-abp..."` → `"Starting ABP..."`
- Remove `--enable-abp` from the launch command

In `tools/abp/common/validate.ps1` lines 21-22:
- `"Starting Chrome with --enable-abp..."` → `"Starting ABP..."`
- Remove `"--enable-abp",` from the argument list

**Step 4: Commit**

```bash
git add tools/abp/build-linux.sh tools/abp/release-linux.sh tools/abp/package-linux.sh tools/abp/build-win.ps1 tools/abp/release-win.ps1 tools/abp/common/validate.sh tools/abp/common/validate.ps1
git commit -m "chore: update Linux/Windows scripts for ABP rename and remove --enable-abp"
```

---

### Task 6: Remove `--enable-abp` flag — switches

**Files:**
- Modify: `chrome/browser/abp/abp_switches.h:6-7`
- Modify: `chrome/browser/abp/abp_switches.cc:5`

**Step 1: Remove kEnableAbp from header**

In `chrome/browser/abp/abp_switches.h`, remove lines 6-7:
```cpp
// Enable ABP HTTP server
extern const char kEnableAbp[];
```

**Step 2: Remove kEnableAbp from implementation**

In `chrome/browser/abp/abp_switches.cc`, remove line 5:
```cpp
const char kEnableAbp[] = "enable-abp";
```

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_switches.h chrome/browser/abp/abp_switches.cc
git commit -m "chore: remove kEnableAbp switch declaration"
```

---

### Task 7: Remove `--enable-abp` flag — browser startup

**Files:**
- Modify: `chrome/browser/chrome_browser_main.cc:1628-1643`

**Step 1: Make ABP always start (with test guard)**

Replace lines 1628-1643 in `chrome/browser/chrome_browser_main.cc`:

```cpp
  // Start ABP HTTP server if enabled via command line
  {
    auto* command_line = base::CommandLine::ForCurrentProcess();
    if (command_line->HasSwitch(abp::switches::kEnableAbp)) {
      int port = 8222;
      if (command_line->HasSwitch(abp::switches::kAbpPort)) {
        base::StringToInt(
            command_line->GetSwitchValueASCII(abp::switches::kAbpPort), &port);
      }
      // Use raw pointer to avoid exit-time destructor. The server lives
      // for the lifetime of the browser process.
      static abp::AbpHttpServer* g_abp_server = nullptr;
      g_abp_server = new abp::AbpHttpServer(port);
      g_abp_server->Start();
    }
  }
```

With:

```cpp
  // Start ABP HTTP server (always-on, skip in browser tests)
  {
    auto* command_line = base::CommandLine::ForCurrentProcess();
    if (command_line->GetSwitchValueASCII("test-type") != "browser") {
      int port = 8222;
      if (command_line->HasSwitch(abp::switches::kAbpPort)) {
        base::StringToInt(
            command_line->GetSwitchValueASCII(abp::switches::kAbpPort), &port);
      }
      // Use raw pointer to avoid exit-time destructor. The server lives
      // for the lifetime of the browser process.
      static abp::AbpHttpServer* g_abp_server = nullptr;
      g_abp_server = new abp::AbpHttpServer(port);
      g_abp_server->Start();
    }
  }
```

**Step 2: Remove the `#include` for abp_switches.h if kEnableAbp was the only reason it was included**

Check if other ABP switches (kAbpPort, etc.) are still referenced in this file. If so, keep the include. If kEnableAbp was the only one, the include is still needed for `kAbpPort` — keep it.

**Step 3: Commit**

```bash
git add chrome/browser/chrome_browser_main.cc
git commit -m "feat: ABP server always starts (skip in browser tests)"
```

---

### Task 8: Remove `--enable-abp` flag — input filtering

**Files:**
- Modify: `content/browser/renderer_host/render_widget_host_impl.cc:3968-3974`
- Modify: `components/input/render_input_router.cc:363-373`

**Step 1: Update SetupRenderInputRouter**

In `content/browser/renderer_host/render_widget_host_impl.cc`, replace lines 3968-3974:

```cpp
  // ABP: Set up virtual cursor tracking flags from command line
  const auto* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch("enable-abp")) {
    render_input_router_->SetAbpEnabled(true);
    render_input_router_->SetAllowSystemInputs(
        command_line->HasSwitch("allow-system-inputs"));
  }
```

With:

```cpp
  // ABP: Always enabled, check allow-system-inputs flag
  render_input_router_->SetAbpEnabled(true);
  render_input_router_->SetAllowSystemInputs(
      base::CommandLine::ForCurrentProcess()->HasSwitch("allow-system-inputs"));
```

**Step 2: Update input filter in render_input_router.cc**

In `components/input/render_input_router.cc`, replace line 368:

```cpp
  if (base::CommandLine::ForCurrentProcess()->HasSwitch("enable-abp") &&
```

With:

```cpp
  if (abp_enabled_ &&
```

This uses the already-set `abp_enabled_` member instead of re-checking the command line. The member is always true now (set in Step 1), and this is cleaner since it uses the existing state rather than parsing command line flags on every input event.

**Step 3: Commit**

```bash
git add content/browser/renderer_host/render_widget_host_impl.cc components/input/render_input_router.cc
git commit -m "feat: ABP input filtering always active, use member state"
```

---

### Task 9: Default screenshots ON

**Files:**
- Modify: `chrome/browser/abp/abp_config.cc:81-82`

**Step 1: Flip the default**

In `chrome/browser/abp/abp_config.cc`, replace lines 81-82:

```cpp
  // TODO(abp): Re-enable history screenshots after fixing CopyFromSurface hang
  config.history.screenshots.enabled = false;
```

With:

```cpp
  config.history.screenshots.enabled = true;
```

**Step 2: Commit**

```bash
git add chrome/browser/abp/abp_config.cc
git commit -m "feat: enable screenshots by default"
```

---

### Task 10: Update documentation

**Files:**
- Modify: `CLAUDE.md`
- Modify: `tools/abp-claude-skill/abp-browser.md`

**Step 1: Update CLAUDE.md**

Update the following references:
- Line 61: `├── abp_switches.h/cc            # --enable-abp, --abp-port flags` → `├── abp_switches.h/cc            # --abp-port, --abp-session-dir flags`
- Line 146: `./out/Default/chrome --enable-abp --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S)` → `./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S)` (macOS) or `./out/Default/abp --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S)` (Linux)
- Line 153: `./out/Default/chrome --enable-abp` → `./out/Default/ABP.app/Contents/MacOS/ABP`
- Any other `--enable-abp` references — remove the flag

**Step 2: Update skill file**

In `tools/abp-claude-skill/abp-browser.md`, update the launch command on lines 14-15:
- Remove `--enable-abp` from the command
- Change `Chromium.app/Contents/MacOS/Chromium` → `ABP.app/Contents/MacOS/ABP`

**Step 3: Commit**

```bash
git add CLAUDE.md tools/abp-claude-skill/abp-browser.md
git commit -m "docs: update documentation for ABP rename and flag removal"
```

---

### Task 11: Update test references

**Files:**
- Modify: `tools/abp-tests/test_navigation.py:23`

**Step 1: Update test binary path**

In `tools/abp-tests/test_navigation.py` line 23, change:
```python
CHROME_PATH = "/home/paladin/src/chromium/out/Default/chrome"
```
to:
```python
CHROME_PATH = "/home/paladin/src/chromium/out/Default/abp"
```

**Step 2: Commit**

```bash
git add tools/abp-tests/test_navigation.py
git commit -m "test: update binary path for ABP rename"
```

---

### Task 12: Build and verify

**Step 1: Regenerate build files**

```bash
gn gen out/Default --args='is_debug=true is_component_build=true symbol_level=1 dcheck_always_on=true'
```

Expected: Success. Verify that `out/Default/ABP.app` (macOS) or `out/Default/abp` (Linux) is the target.

**Step 2: Build**

```bash
autoninja -C out/Default chrome
```

Note: The ninja target is still `chrome` (the GN target name), but the output binary will be `ABP`/`abp`.

**Step 3: Launch and verify**

macOS:
```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/test --no-first-run
```

Verify:
```bash
curl http://localhost:8222/api/v1/browser/status
```

Expected: ABP server responds without needing `--enable-abp`.

**Step 4: Verify screenshots are on by default**

```bash
curl http://localhost:8222/api/v1/browser/session-data
```

Expected: `"screenshots_enabled": true`
