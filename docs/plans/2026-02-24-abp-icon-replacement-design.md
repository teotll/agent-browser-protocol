# ABP Icon Replacement Design

**Date:** 2026-02-24
**Status:** Approved

## Goal

Replace the default Chromium blue globe icon with the ABP logo (`abp_logo.png`) across macOS, Windows, and Linux. This covers the main app icon shown in the dock/taskbar, desktop shortcuts, window title bars, and about pages.

## Source

- **File:** `abp_logo.png` (project root)
- **Dimensions:** 1024x1024 PNG
- **Content:** White background, dark browser window outline, green aperture/eye icon
- **Background:** White (kept as-is, no transparency)

## Scope

### In Scope
- Main application icon across all three platforms
- Product logos used in the UI (about page, tab favicons, etc.)
- Windows tile images
- macOS `.icns`, Asset Catalog (`.car`), and iconset
- Linux product logo PNGs
- Scaled resource PNGs (100% and 200%)

### Out of Scope
- Document/PDF/incognito/app-list `.ico` variants (not visible in ABP)
- `product_logo_22_mono.png` (system tray monochrome icon)
- `product_logo_name_22*.png` (wordmark images)
- `product_logo_animation.svg`, `.ai` source files
- ChromeOS icons, webstore icons, password manager favicon
- `mac/AppIcon.icon/` dynamic icon (will be deleted; macOS falls back to standard icon)
- Windows installer icons (`mini_installer.ico`, `setup.ico`)

## Approach

A single shell script (`tools/abp/generate-icons.sh`) generates all icon files from the source PNG using macOS-native tools plus ImageMagick.

### Tools
| Tool | Purpose | Source |
|------|---------|--------|
| `sips` | PNG resizing (Lanczos) | macOS built-in |
| `iconutil` | `.iconset` → `.icns` | macOS built-in |
| `actool` | `.xcassets` → `Assets.car` | Xcode CLI tools |
| `magick` | PNG → `.ico` conversion | Homebrew (`imagemagick`) |

### Prerequisite
- ImageMagick 7: `brew install imagemagick`

## Files Generated

### macOS — `chrome/app/theme/chromium/mac/`

| File | Pixel Size |
|------|-----------|
| `Assets.xcassets/AppIcon.appiconset/appicon_16.png` | 16x16 |
| `Assets.xcassets/AppIcon.appiconset/appicon_32.png` | 32x32 |
| `Assets.xcassets/AppIcon.appiconset/appicon_64.png` | 64x64 |
| `Assets.xcassets/AppIcon.appiconset/appicon_128.png` | 128x128 |
| `Assets.xcassets/AppIcon.appiconset/appicon_256.png` | 256x256 |
| `Assets.xcassets/AppIcon.appiconset/appicon_512.png` | 512x512 |
| `Assets.xcassets/AppIcon.appiconset/appicon_1024.png` | 1024x1024 |
| `Assets.xcassets/Icon.iconset/icon_256x256.png` | 256x256 |
| `Assets.xcassets/Icon.iconset/icon_256x256@2x.png` | 512x512 |
| `app.icns` | Multi-size (generated via `iconutil`) |
| `Assets.car` | Compiled (generated via `actool`) |

**Deleted:** `mac/AppIcon.icon/` directory (dynamic icon — macOS falls back to `.icns`/Asset Catalog).

### Windows — `chrome/app/theme/chromium/win/`

| File | Details |
|------|---------|
| `chromium.ico` | 16, 32, 48, 256 px @ 32bpp |
| `tiles/Logo.png` | 600x600 |
| `tiles/SmallLogo.png` | 176x176 |

### Linux — `chrome/app/theme/chromium/linux/`

| File | Pixel Size |
|------|-----------|
| `product_logo_24.png` | 24x24 |
| `product_logo_48.png` | 48x48 |
| `product_logo_64.png` | 64x64 |
| `product_logo_128.png` | 128x128 |
| `product_logo_256.png` | 256x256 |

### Cross-Platform — `chrome/app/theme/chromium/`

| File | Pixel Size |
|------|-----------|
| `product_logo_16.png` | 16x16 |
| `product_logo_24.png` | 24x24 |
| `product_logo_48.png` | 48x48 |
| `product_logo_64.png` | 64x64 |
| `product_logo_128.png` | 128x128 |
| `product_logo_256.png` | 256x256 |

### Scaled Resources — 100% (`chrome/app/theme/default_100_percent/chromium/`)

| File | Pixel Size |
|------|-----------|
| `product_logo_16.png` | 16x16 |
| `product_logo_32.png` | 32x32 |
| `linux/product_logo_16.png` | 16x16 |
| `linux/product_logo_32.png` | 32x32 |

### Scaled Resources — 200% (`chrome/app/theme/default_200_percent/chromium/`)

| File | Pixel Size |
|------|-----------|
| `product_logo_16.png` | 32x32 |
| `product_logo_32.png` | 64x64 |

**Total: ~36 files** generated from the single 1024x1024 source.

## Script Design

```
tools/abp/generate-icons.sh [source_png]
```

1. Validate source PNG is 1024x1024
2. Create temp directory for intermediate files
3. Generate all PNG sizes via `sips --resampleHeightWidth`
4. Copy PNGs to their target locations in `chrome/app/theme/`
5. Build macOS `.icns` via temporary `.iconset` + `iconutil`
6. Compile `Assets.car` via `actool` from `Assets.xcassets`
7. Build Windows `.ico` via `magick convert` (multiple sizes → single `.ico`)
8. Delete `mac/AppIcon.icon/` directory
9. Clean up temp directory

## Build Integration

No build system changes needed. The icon files are referenced by their existing paths in:
- `chrome/BUILD.gn` (macOS bundle resources)
- `chrome/app/chrome_exe.rc` / `chrome_dll.rc` (Windows `IDR_MAINFRAME`)
- `chrome/installer/linux/BUILD.gn` (Linux package)
- `chrome/app/theme/chrome_unscaled_resources.grd` (UI resources)

The script replaces files in-place, so the build system picks them up automatically.
