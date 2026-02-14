# ABP Developer Experience Improvements

**Date**: 2026-02-13
**Status**: Approved

## Summary

Three changes to improve the developer experience from first install to launch:

1. Rename binary from "Chromium" to "ABP" on all platforms
2. Remove `--enable-abp` flag — ABP is always on
3. Default screenshots to ON

## Change 1: Rename Binary to "ABP"

### BRANDING File

**File**: `chrome/app/theme/chromium/BRANDING`

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

### macOS (derived automatically from BRANDING)

- App bundle: `ABP.app`
- Executable: `ABP.app/Contents/MacOS/ABP`
- Framework: `ABP Framework.framework`
- Helpers: `ABP Helper.app`, `ABP Helper (Alerts).app`, etc.

### Linux

- `chrome/installer/linux/common/chromium-browser.info` — Change `PROGNAME=chrome` to `PROGNAME=abp`
- `chrome/BUILD.gn` — Change `_chrome_output_name = "chrome"` to `"abp"` (non-Windows path, line ~154)

### Windows

- `chrome/app/chrome_exe.ver` — `ORIGINAL_FILENAME=abp.exe`
- `chrome/app/chrome_dll.ver` — `ORIGINAL_FILENAME=abp.dll`
- `chrome/BUILD.gn` — `_chrome_output_name = "initialexe/abp"` (Windows path, line ~152) and DLL `output_name = "abp"` (line ~412)
- `chrome/installer/mini_installer/chrome.release` — Replace `chrome.exe` → `abp.exe`, `chrome.dll` → `abp.dll`

### Script & Documentation Updates

- `tools/abp/build-mac.sh` — Replace `Chromium.app` references
- `tools/abp/package-mac.sh` — Replace `Chromium.app` references
- `tools/abp/release-mac.sh` — Replace `Chromium.app` references
- `CLAUDE.md` — Update launch commands and paths
- `tools/abp-claude-skill/abp-browser.md` — Update references

## Change 2: Remove `--enable-abp` Flag

ABP is always on — this is the ABP browser, not Chromium-with-a-plugin.

### Files to Change

1. **`chrome/browser/chrome_browser_main.cc`** — Remove `HasSwitch(kEnableAbp)` conditional. Always start `AbpHttpServer`. Add guard: skip ABP startup when `--test-type=browser` is present (prevents segfaults in browser tests from leaked server + auto-pause timer).

2. **`content/browser/renderer_host/render_widget_host_impl.cc`** — Remove `HasSwitch("enable-abp")` check. Always call `SetAbpEnabled(true)` and check `--allow-system-inputs`.

3. **`components/input/render_input_router.cc`** — Remove `HasSwitch("enable-abp")` check. Always apply input filtering (respecting `--allow-system-inputs` and gesture exemptions).

4. **`chrome/browser/abp/abp_switches.h`** — Remove `kEnableAbp` declaration.

5. **`chrome/browser/abp/abp_switches.cc`** — Remove `kEnableAbp` definition.

6. **Documentation** — Remove `--enable-abp` from CLAUDE.md, skill files, etc.

### Remaining Flags (unchanged)

- `--abp-port` — Custom HTTP server port (default: 8222)
- `--abp-session-dir` — Session data directory
- `--abp-config` — Config file path
- `--allow-system-inputs` — Allow real mouse/keyboard input
- `--abp-disable-pause` — Disable execution control

### Test Safety

Browser tests (`InProcessBrowserTest`) will be protected by checking `--test-type=browser` on the command line. ABP server won't start in test mode, preserving existing test behavior.

## Change 3: Default Screenshots ON

**File**: `chrome/browser/abp/abp_config.cc` (line 82)

- Change `config.history.screenshots.enabled = false` to `true`
- Remove the TODO comment: `// TODO(abp): Re-enable history screenshots after fixing CopyFromSurface hang`
- The `CopyFromSurface` hang is no longer relevant — screenshots now use `ForceRedraw + GrabViewSnapshot`

## New Launch Command

```bash
# macOS
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run

# Linux
./out/Default/abp --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run
```

No `--enable-abp` needed. Screenshots saved by default.
