#!/bin/bash
# Build Chrome with ABP for macOS
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHROMIUM_SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Validate environment
if [[ -z "${ABP_VERSION:-}" ]]; then
    echo "ERROR: ABP_VERSION environment variable is required"
    echo "Usage: ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 [BUILD_ARCH=arm64|x64|universal] $0"
    exit 1
fi

if [[ -z "${CHROME_VERSION:-}" ]]; then
    echo "ERROR: CHROME_VERSION environment variable is required"
    echo "Usage: ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 [BUILD_ARCH=arm64|x64|universal] $0"
    exit 1
fi

# Validate we're in chromium source
if [[ ! -f "$CHROMIUM_SRC/BUILD.gn" ]]; then
    echo "ERROR: Must be run from chromium source directory"
    echo "Expected BUILD.gn at: $CHROMIUM_SRC/BUILD.gn"
    exit 1
fi

BUILD_ARCH="${BUILD_ARCH:-all}"
FORCE="${FORCE:-0}"

build_arch() {
    local arch=$1
    local out_dir="out/Release-$arch"

    # Skip if already built (unless --force)
    if [[ "$FORCE" != "1" && -d "$CHROMIUM_SRC/$out_dir/ABP.app" ]]; then
        echo "=== Skipping $arch build (already exists at $out_dir/ABP.app, set FORCE=1 to rebuild) ==="
        return 0
    fi

    echo "=== Building ABP Chrome for macOS ($arch) ==="
    echo "ABP Version: $ABP_VERSION"
    echo "Chrome Version: $CHROME_VERSION"
    echo "Source: $CHROMIUM_SRC"
    echo "Output: $out_dir"

    cd "$CHROMIUM_SRC"

    # Configure release build
    local GN_ARGS='is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0'
    GN_ARGS="$GN_ARGS target_cpu=\"$arch\""

    echo "=== Configuring build with GN ==="
    gn gen "$out_dir" --args="$GN_ARGS"

    echo "=== Building Chrome ==="
    autoninja -C "$out_dir" chrome

    echo "=== Build complete for $arch ==="
}

merge_universal() {
    local arm64_app="$CHROMIUM_SRC/out/Release-arm64/ABP.app"
    local x64_app="$CHROMIUM_SRC/out/Release-x64/ABP.app"
    local universal_dir="$CHROMIUM_SRC/out/Release-universal"
    local universal_app="$universal_dir/ABP.app"

    if [[ ! -d "$arm64_app" ]]; then
        echo "ERROR: arm64 build not found at $arm64_app"
        exit 1
    fi
    if [[ ! -d "$x64_app" ]]; then
        echo "ERROR: x64 build not found at $x64_app"
        exit 1
    fi

    echo "=== Merging arm64 + x64 into universal binary ==="

    # Clean previous universal output
    rm -rf "$universal_app"
    mkdir -p "$universal_dir"

    python3 "$CHROMIUM_SRC/chrome/installer/mac/universalizer.py" \
        "$arm64_app" "$x64_app" "$universal_app"

    echo "=== Universal merge complete ==="
}

case "$BUILD_ARCH" in
    arm64)
        build_arch "arm64"
        ;;
    x64)
        build_arch "x64"
        ;;
    universal)
        build_arch "arm64"
        build_arch "x64"
        merge_universal
        ;;
    all)
        build_arch "arm64"
        build_arch "x64"
        merge_universal
        ;;
    *)
        echo "ERROR: Invalid BUILD_ARCH: $BUILD_ARCH"
        echo "Valid values: arm64, x64, universal, all"
        exit 1
        ;;
esac

echo "=== All macOS builds complete ==="
