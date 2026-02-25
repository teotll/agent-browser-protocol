# ABP Icon Replacement Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the Chromium blue globe icon with the ABP logo across macOS, Windows, and Linux.

**Architecture:** A single shell script (`tools/abp/generate-icons.sh`) takes the 1024x1024 source PNG and generates all ~36 icon files using `sips` (resize), `iconutil` (.icns), `actool` (Assets.car), and `magick` (.ico). Files are placed directly into the existing Chromium branding paths.

**Tech Stack:** Bash, sips (macOS), iconutil (macOS), actool (Xcode), ImageMagick 7

**Design doc:** `docs/plans/2026-02-24-abp-icon-replacement-design.md`

---

### Task 1: Write the icon generation script

**Files:**
- Create: `tools/abp/generate-icons.sh`

**Step 1: Create the script**

Write `tools/abp/generate-icons.sh` with the following content:

```bash
#!/bin/bash
# generate-icons.sh — Generate all ABP icon files from a single source PNG.
# Usage: ./tools/abp/generate-icons.sh [source.png]
# Requires: sips, iconutil, actool (Xcode), magick (ImageMagick 7)

set -euo pipefail

# Resolve paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SOURCE="${1:-$SRC_ROOT/abp_logo.png}"
THEME="$SRC_ROOT/chrome/app/theme/chromium"
THEME_100="$SRC_ROOT/chrome/app/theme/default_100_percent/chromium"
THEME_200="$SRC_ROOT/chrome/app/theme/default_200_percent/chromium"
TMPDIR_ICONS="$(mktemp -d)"

trap "rm -rf $TMPDIR_ICONS" EXIT

# Validate source
if [ ! -f "$SOURCE" ]; then
  echo "ERROR: Source PNG not found: $SOURCE"
  exit 1
fi

WIDTH=$(sips -g pixelWidth "$SOURCE" | awk '/pixelWidth/{print $2}')
HEIGHT=$(sips -g pixelHeight "$SOURCE" | awk '/pixelHeight/{print $2}')
if [ "$WIDTH" -lt 1024 ] || [ "$HEIGHT" -lt 1024 ]; then
  echo "ERROR: Source must be at least 1024x1024 (got ${WIDTH}x${HEIGHT})"
  exit 1
fi

echo "Source: $SOURCE (${WIDTH}x${HEIGHT})"

# --- Helper: resize source to NxN PNG ---
resize() {
  local size=$1 output=$2
  sips --resampleHeightWidth "$size" "$size" "$SOURCE" --out "$output" > /dev/null 2>&1
  echo "  Generated: $output (${size}x${size})"
}

# ============================================================
# macOS — AppIcon.appiconset PNGs
# ============================================================
echo ""
echo "=== macOS: AppIcon.appiconset ==="
APPICONSET="$THEME/mac/Assets.xcassets/AppIcon.appiconset"
for size in 16 32 64 128 256 512 1024; do
  resize $size "$APPICONSET/appicon_${size}.png"
done

# ============================================================
# macOS — Icon.iconset PNGs
# ============================================================
echo ""
echo "=== macOS: Icon.iconset ==="
ICONSET="$THEME/mac/Assets.xcassets/Icon.iconset"
resize 256 "$ICONSET/icon_256x256.png"
resize 512 "$ICONSET/icon_256x256@2x.png"

# ============================================================
# macOS — app.icns (via temporary .iconset)
# ============================================================
echo ""
echo "=== macOS: app.icns ==="
TEMP_ICONSET="$TMPDIR_ICONS/app.iconset"
mkdir -p "$TEMP_ICONSET"
resize 16   "$TEMP_ICONSET/icon_16x16.png"
resize 32   "$TEMP_ICONSET/icon_16x16@2x.png"
resize 32   "$TEMP_ICONSET/icon_32x32.png"
resize 64   "$TEMP_ICONSET/icon_32x32@2x.png"
resize 128  "$TEMP_ICONSET/icon_128x128.png"
resize 256  "$TEMP_ICONSET/icon_128x128@2x.png"
resize 256  "$TEMP_ICONSET/icon_256x256.png"
resize 512  "$TEMP_ICONSET/icon_256x256@2x.png"
resize 512  "$TEMP_ICONSET/icon_512x512.png"
resize 1024 "$TEMP_ICONSET/icon_512x512@2x.png"
iconutil --convert icns --output "$THEME/mac/app.icns" "$TEMP_ICONSET"
echo "  Generated: $THEME/mac/app.icns"

# ============================================================
# macOS — Assets.car (compiled asset catalog)
# ============================================================
echo ""
echo "=== macOS: Assets.car ==="
actool --compile "$THEME/mac" \
  --platform macosx \
  --minimum-deployment-target 10.15 \
  "$THEME/mac/Assets.xcassets" > /dev/null 2>&1
echo "  Generated: $THEME/mac/Assets.car"

# ============================================================
# macOS — Delete AppIcon.icon (dynamic icon)
# ============================================================
echo ""
echo "=== macOS: Removing AppIcon.icon (dynamic icon) ==="
if [ -d "$THEME/mac/AppIcon.icon" ]; then
  rm -rf "$THEME/mac/AppIcon.icon"
  echo "  Deleted: $THEME/mac/AppIcon.icon/"
else
  echo "  Already absent: $THEME/mac/AppIcon.icon/"
fi

# ============================================================
# Windows — chromium.ico
# ============================================================
echo ""
echo "=== Windows: chromium.ico ==="
WIN_DIR="$THEME/win"
# Generate temp PNGs for ICO
for size in 16 32 48 256; do
  resize $size "$TMPDIR_ICONS/win_${size}.png"
done
magick "$TMPDIR_ICONS/win_16.png" "$TMPDIR_ICONS/win_32.png" \
       "$TMPDIR_ICONS/win_48.png" "$TMPDIR_ICONS/win_256.png" \
       "$WIN_DIR/chromium.ico"
echo "  Generated: $WIN_DIR/chromium.ico"

# ============================================================
# Windows — Tile images
# ============================================================
echo ""
echo "=== Windows: Tile images ==="
resize 600 "$WIN_DIR/tiles/Logo.png"
resize 176 "$WIN_DIR/tiles/SmallLogo.png"

# ============================================================
# Linux — product logos
# ============================================================
echo ""
echo "=== Linux: product logos ==="
for size in 24 48 64 128 256; do
  resize $size "$THEME/linux/product_logo_${size}.png"
done

# ============================================================
# Cross-platform — product logos (root chromium theme)
# ============================================================
echo ""
echo "=== Cross-platform: product logos ==="
for size in 16 24 48 64 128 256; do
  resize $size "$THEME/product_logo_${size}.png"
done

# ============================================================
# Scaled resources — 100%
# ============================================================
echo ""
echo "=== Scaled: 100% ==="
resize 16 "$THEME_100/product_logo_16.png"
resize 32 "$THEME_100/product_logo_32.png"
resize 16 "$THEME_100/linux/product_logo_16.png"
resize 32 "$THEME_100/linux/product_logo_32.png"

# ============================================================
# Scaled resources — 200%
# ============================================================
echo ""
echo "=== Scaled: 200% ==="
resize 32 "$THEME_200/product_logo_16.png"
resize 64 "$THEME_200/product_logo_32.png"

# ============================================================
echo ""
echo "=== Done! ==="
echo "All icons generated from: $SOURCE"
echo ""
echo "Next steps:"
echo "  1. Build: autoninja -C out/Default chrome"
echo "  2. Verify icons in the built app"
echo "  3. git add -p chrome/app/theme/ && git commit"
```

**Step 2: Make the script executable**

Run: `chmod +x tools/abp/generate-icons.sh`

**Step 3: Commit the script**

```bash
git add tools/abp/generate-icons.sh
git commit -m "feat: add icon generation script for ABP branding"
```

---

### Task 2: Run the icon generation script

**Step 1: Run the script**

Run: `./tools/abp/generate-icons.sh abp_logo.png`

Expected output: Lines showing each generated file, ending with "=== Done! ==="

**Step 2: Verify file counts**

Run: `find chrome/app/theme/chromium/mac/Assets.xcassets/AppIcon.appiconset -name '*.png' | wc -l`
Expected: `7`

Run: `find chrome/app/theme/chromium/mac/Assets.xcassets/Icon.iconset -name '*.png' | wc -l`
Expected: `2`

Run: `ls chrome/app/theme/chromium/mac/app.icns`
Expected: File exists

Run: `ls chrome/app/theme/chromium/mac/Assets.car`
Expected: File exists

Run: `ls chrome/app/theme/chromium/win/chromium.ico`
Expected: File exists, size > 0

Run: `find chrome/app/theme/chromium/linux -name 'product_logo_*.png' | wc -l`
Expected: `5`

Run: `ls -d chrome/app/theme/chromium/mac/AppIcon.icon 2>&1`
Expected: "No such file or directory" (deleted)

**Step 3: Spot-check image dimensions**

Run: `sips -g pixelHeight -g pixelWidth chrome/app/theme/chromium/mac/Assets.xcassets/AppIcon.appiconset/appicon_128.png`
Expected: 128x128

Run: `sips -g pixelHeight -g pixelWidth chrome/app/theme/chromium/win/tiles/Logo.png`
Expected: 600x600

Run: `sips -g pixelHeight -g pixelWidth chrome/app/theme/default_200_percent/chromium/product_logo_32.png`
Expected: 64x64 (200% scale of 32x32)

---

### Task 3: Commit all generated icon files

**Step 1: Review the changes**

Run: `git status`
Expected: Modified files in `chrome/app/theme/chromium/` and `chrome/app/theme/default_*_percent/chromium/`, plus deleted files under `mac/AppIcon.icon/`.

**Step 2: Stage and commit**

```bash
git add chrome/app/theme/chromium/ chrome/app/theme/default_100_percent/chromium/ chrome/app/theme/default_200_percent/chromium/
git commit -m "feat: replace Chromium icons with ABP logo

Generated from abp_logo.png (1024x1024) using tools/abp/generate-icons.sh.
Covers macOS (.icns, Assets.car, appiconset), Windows (.ico, tiles),
Linux (product logos), and scaled UI resources (100%/200%).
Removed macOS dynamic icon (AppIcon.icon) — falls back to standard icon."
```

---

### Task 4: Build and verify visually

**Step 1: Build**

Run: `autoninja -C out/Default chrome`

This is an incremental build — only resource packing should re-run.

**Step 2: Launch and verify**

Run: `./out/Default/ABP.app/Contents/MacOS/ABP --no-first-run`

Verify:
- Dock icon shows the ABP logo (green aperture in browser window on white background)
- Window title bar icon shows ABP logo
- About page (`chrome://settings/help`) shows ABP logo

**Step 3: If anything is wrong, re-run the script and rebuild**

The script is idempotent — re-run `./tools/abp/generate-icons.sh abp_logo.png` if the source logo changes.
