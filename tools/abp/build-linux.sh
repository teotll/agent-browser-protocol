#!/bin/bash
# Build Chrome with ABP for Linux
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHROMIUM_SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Read ABP_VERSION from package.json if not set
if [[ -z "${ABP_VERSION:-}" ]]; then
    _pkg_json="$SCRIPT_DIR/../abp-npm/package.json"
    if [[ -f "$_pkg_json" ]]; then
        ABP_VERSION="$(python3 -c "import json; print(json.load(open('$_pkg_json'))['version'])")"
        export ABP_VERSION
    else
        echo "ERROR: ABP_VERSION not set and $_pkg_json not found"
        exit 1
    fi
fi

# Read CHROME_VERSION from chrome/VERSION if not set
if [[ -z "${CHROME_VERSION:-}" ]]; then
    _version_file="$CHROMIUM_SRC/chrome/VERSION"
    if [[ -f "$_version_file" ]]; then
        CHROME_VERSION="$(awk -F= '/^MAJOR/{maj=$2} /^MINOR/{min=$2} /^BUILD/{bld=$2} /^PATCH/{pat=$2} END{print maj"."min"."bld"."pat}' "$_version_file")"
        export CHROME_VERSION
    else
        echo "ERROR: CHROME_VERSION not set and chrome/VERSION not found"
        exit 1
    fi
fi

# Validate we're in chromium source
if [[ ! -f "$CHROMIUM_SRC/BUILD.gn" ]]; then
    echo "ERROR: Must be run from chromium source directory"
    echo "Expected BUILD.gn at: $CHROMIUM_SRC/BUILD.gn"
    exit 1
fi

FORCE="${FORCE:-0}"

# Skip if already built (unless FORCE=1)
if [[ "$FORCE" != "1" && -f "$CHROMIUM_SRC/out/Release/abp" ]]; then
    echo "=== Skipping build (already exists at out/Release/abp, set FORCE=1 to rebuild) ==="
    exit 0
fi

echo "=== Building ABP Chrome for Linux ==="
echo "ABP Version: $ABP_VERSION"
echo "Chrome Version: $CHROME_VERSION"
echo "Source: $CHROMIUM_SRC"

cd "$CHROMIUM_SRC"

# Configure release build
GN_ARGS='is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0'

echo "=== Configuring build with GN ==="
gn gen out/Release --args="$GN_ARGS"

echo "=== Building ABP ==="
autoninja -C out/Release chrome

echo "=== Build complete ==="
echo "Output: $CHROMIUM_SRC/out/Release/abp"
