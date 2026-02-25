#!/usr/bin/env bash
# generate-icons.sh — Generate all ABP browser icons from a source PNG.
#
# Usage:
#   ./tools/abp/generate-icons.sh [source.png]
#
# Defaults to $SRC_ROOT/abp_logo.png if no argument given.
# Requires: sips, iconutil, actool (macOS), magick (ImageMagick 7+)

set -euo pipefail

# This script must be run on macOS (requires sips, iconutil, actool).

# ---------------------------------------------------------------------------
# Preflight: verify required tools
# ---------------------------------------------------------------------------
for cmd in sips iconutil actool magick; do
  if ! command -v "$cmd" &>/dev/null; then
    echo "ERROR: Required command not found: $cmd" >&2
    exit 1
  fi
done

# ---------------------------------------------------------------------------
# Resolve paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

SOURCE_PNG="${1:-${SRC_ROOT}/abp_logo.png}"

# Temp directory for intermediate files; cleaned up on exit.
TMPDIR_ICONS="$(mktemp -d "${TMPDIR:-/tmp}/abp-icons.XXXXXX")"
cleanup() {
  rm -rf "$TMPDIR_ICONS"
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Validate source image
# ---------------------------------------------------------------------------
if [[ ! -f "$SOURCE_PNG" ]]; then
  echo "ERROR: Source PNG not found: $SOURCE_PNG" >&2
  exit 1
fi

WIDTH="$(sips -g pixelWidth "$SOURCE_PNG" | awk '/pixelWidth/{print $2}')"
HEIGHT="$(sips -g pixelHeight "$SOURCE_PNG" | awk '/pixelHeight/{print $2}')"

if [[ "$WIDTH" -lt 1024 || "$HEIGHT" -lt 1024 ]]; then
  echo "ERROR: Source PNG must be at least 1024x1024 (got ${WIDTH}x${HEIGHT})" >&2
  exit 1
fi

echo "Source: $SOURCE_PNG (${WIDTH}x${HEIGHT})"
echo "Temp:   $TMPDIR_ICONS"
echo ""

# ---------------------------------------------------------------------------
# Helper: resize source PNG to a given size and output path.
# ---------------------------------------------------------------------------
resize() {
  local size="$1"
  local output="$2"
  mkdir -p "$(dirname "$output")"
  sips --resampleHeightWidth "$size" "$size" "$SOURCE_PNG" --out "$output" >/dev/null 2>&1
  echo "  [${size}x${size}] $output"
}

# ---------------------------------------------------------------------------
# 1. macOS — AppIcon.appiconset
# ---------------------------------------------------------------------------
echo "=== macOS: AppIcon.appiconset ==="
APPICONSET="${SRC_ROOT}/chrome/app/theme/chromium/mac/Assets.xcassets/AppIcon.appiconset"
for size in 16 32 64 128 256 512 1024; do
  resize "$size" "${APPICONSET}/appicon_${size}.png"
done

# ---------------------------------------------------------------------------
# 2. macOS — Icon.iconset (for Assets.xcassets)
# ---------------------------------------------------------------------------
echo ""
echo "=== macOS: Icon.iconset ==="
ICONSET_DIR="${SRC_ROOT}/chrome/app/theme/chromium/mac/Assets.xcassets/Icon.iconset"
resize 256 "${ICONSET_DIR}/icon_256x256.png"
resize 512 "${ICONSET_DIR}/icon_256x256@2x.png"

# ---------------------------------------------------------------------------
# 3. macOS — app.icns via iconutil
# ---------------------------------------------------------------------------
echo ""
echo "=== macOS: app.icns ==="
ICNS_ICONSET="${TMPDIR_ICONS}/app.iconset"
mkdir -p "$ICNS_ICONSET"

# iconutil requires specific filenames: icon_NxN.png and icon_NxN@2x.png
sips --resampleHeightWidth 16 16 "$SOURCE_PNG" --out "${ICNS_ICONSET}/icon_16x16.png" >/dev/null 2>&1
sips --resampleHeightWidth 32 32 "$SOURCE_PNG" --out "${ICNS_ICONSET}/icon_16x16@2x.png" >/dev/null 2>&1
sips --resampleHeightWidth 32 32 "$SOURCE_PNG" --out "${ICNS_ICONSET}/icon_32x32.png" >/dev/null 2>&1
sips --resampleHeightWidth 64 64 "$SOURCE_PNG" --out "${ICNS_ICONSET}/icon_32x32@2x.png" >/dev/null 2>&1
sips --resampleHeightWidth 128 128 "$SOURCE_PNG" --out "${ICNS_ICONSET}/icon_128x128.png" >/dev/null 2>&1
sips --resampleHeightWidth 256 256 "$SOURCE_PNG" --out "${ICNS_ICONSET}/icon_128x128@2x.png" >/dev/null 2>&1
sips --resampleHeightWidth 256 256 "$SOURCE_PNG" --out "${ICNS_ICONSET}/icon_256x256.png" >/dev/null 2>&1
sips --resampleHeightWidth 512 512 "$SOURCE_PNG" --out "${ICNS_ICONSET}/icon_256x256@2x.png" >/dev/null 2>&1
sips --resampleHeightWidth 512 512 "$SOURCE_PNG" --out "${ICNS_ICONSET}/icon_512x512.png" >/dev/null 2>&1
sips --resampleHeightWidth 1024 1024 "$SOURCE_PNG" --out "${ICNS_ICONSET}/icon_512x512@2x.png" >/dev/null 2>&1

ICNS_OUTPUT="${SRC_ROOT}/chrome/app/theme/chromium/mac/app.icns"
iconutil --convert icns --output "$ICNS_OUTPUT" "$ICNS_ICONSET"
echo "  [icns] $ICNS_OUTPUT"

# ---------------------------------------------------------------------------
# 4. macOS — Assets.car via actool
# ---------------------------------------------------------------------------
echo ""
echo "=== macOS: Assets.car ==="
XCASSETS_DIR="${SRC_ROOT}/chrome/app/theme/chromium/mac/Assets.xcassets"
CAR_OUTPUT_DIR="${SRC_ROOT}/chrome/app/theme/chromium/mac"
actool --compile "$CAR_OUTPUT_DIR" \
  --platform macosx \
  --minimum-deployment-target 10.15 \
  "$XCASSETS_DIR" >/dev/null
echo "  [car] ${CAR_OUTPUT_DIR}/Assets.car"

# ---------------------------------------------------------------------------
# 5. macOS — Delete dynamic icon directory
# ---------------------------------------------------------------------------
echo ""
echo "=== macOS: Remove AppIcon.icon ==="
APPICON_DYNAMIC="${SRC_ROOT}/chrome/app/theme/chromium/mac/AppIcon.icon"
if [[ -d "$APPICON_DYNAMIC" ]]; then
  rm -rf "$APPICON_DYNAMIC"
  echo "  [deleted] $APPICON_DYNAMIC"
else
  echo "  [skipped] $APPICON_DYNAMIC (not found)"
fi

# ---------------------------------------------------------------------------
# 6. Windows — chromium.ico (multi-size ICO)
# ---------------------------------------------------------------------------
echo ""
echo "=== Windows: chromium.ico ==="
WIN_DIR="${SRC_ROOT}/chrome/app/theme/chromium/win"

# Generate individual PNGs for ICO embedding
ICO_16="${TMPDIR_ICONS}/ico_16.png"
ICO_32="${TMPDIR_ICONS}/ico_32.png"
ICO_48="${TMPDIR_ICONS}/ico_48.png"
ICO_256="${TMPDIR_ICONS}/ico_256.png"
sips --resampleHeightWidth 16 16 "$SOURCE_PNG" --out "$ICO_16" >/dev/null 2>&1
sips --resampleHeightWidth 32 32 "$SOURCE_PNG" --out "$ICO_32" >/dev/null 2>&1
sips --resampleHeightWidth 48 48 "$SOURCE_PNG" --out "$ICO_48" >/dev/null 2>&1
sips --resampleHeightWidth 256 256 "$SOURCE_PNG" --out "$ICO_256" >/dev/null 2>&1

magick "$ICO_16" "$ICO_32" "$ICO_48" "$ICO_256" "${WIN_DIR}/chromium.ico"
echo "  [ico] ${WIN_DIR}/chromium.ico"

# ---------------------------------------------------------------------------
# 7. Windows — Tiles
# ---------------------------------------------------------------------------
echo ""
echo "=== Windows: Tiles ==="
resize 600 "${WIN_DIR}/tiles/Logo.png"
resize 176 "${WIN_DIR}/tiles/SmallLogo.png"

# ---------------------------------------------------------------------------
# 8. Linux — product logos
# ---------------------------------------------------------------------------
echo ""
echo "=== Linux: product_logo_*.png ==="
LINUX_DIR="${SRC_ROOT}/chrome/app/theme/chromium/linux"
for size in 24 48 64 128 256; do
  resize "$size" "${LINUX_DIR}/product_logo_${size}.png"
done

# ---------------------------------------------------------------------------
# 9. Cross-platform — product logos
# ---------------------------------------------------------------------------
echo ""
echo "=== Cross-platform: product_logo_*.png ==="
PRODUCT_DIR="${SRC_ROOT}/chrome/app/theme/chromium"
for size in 16 24 48 64 128 256; do
  resize "$size" "${PRODUCT_DIR}/product_logo_${size}.png"
done

# ---------------------------------------------------------------------------
# 10. Scaled resources — 100%
# ---------------------------------------------------------------------------
echo ""
echo "=== Scaled 100%: product_logo_*.png ==="
SCALE_100="${SRC_ROOT}/chrome/app/theme/default_100_percent/chromium"
resize 16 "${SCALE_100}/product_logo_16.png"
resize 32 "${SCALE_100}/product_logo_32.png"

echo ""
echo "=== Scaled 100% Linux: product_logo_*.png ==="
resize 16 "${SCALE_100}/linux/product_logo_16.png"
resize 32 "${SCALE_100}/linux/product_logo_32.png"

# ---------------------------------------------------------------------------
# 11. Scaled resources — 200%
# ---------------------------------------------------------------------------
echo ""
echo "=== Scaled 200%: product_logo_*.png ==="
SCALE_200="${SRC_ROOT}/chrome/app/theme/default_200_percent/chromium"
resize 32 "${SCALE_200}/product_logo_16.png"   # 200% of 16 = 32x32
resize 64 "${SCALE_200}/product_logo_32.png"   # 200% of 32 = 64x64

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
echo "=== Done ==="
echo "All icons generated successfully."
