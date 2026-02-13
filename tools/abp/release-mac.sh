#!/bin/bash
# Build, validate, sign, notarize, and package ABP Chrome for macOS
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHROMIUM_SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Validate environment
if [[ -z "${ABP_VERSION:-}" ]]; then
    echo "ERROR: ABP_VERSION environment variable is required"
    echo "Usage: ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 [BUILD_ARCH=arm64|x64|universal|all] $0"
    exit 1
fi

if [[ -z "${CHROME_VERSION:-}" ]]; then
    echo "ERROR: CHROME_VERSION environment variable is required"
    echo "Usage: ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 [BUILD_ARCH=arm64|x64|universal|all] $0"
    exit 1
fi

BUILD_ARCH="${BUILD_ARCH:-all}"
SIGNING_IDENTITY="${SIGNING_IDENTITY:-Developer ID Application: Han Wang (72YUDGUH4G)}"
ENTITLEMENTS="$CHROMIUM_SRC/chrome/app/app-entitlements.plist"

# Notarization credentials (required unless SKIP_NOTARIZATION=1)
# NOTARIZE_KEY       - Path to App Store Connect API key (.p8 file)
# NOTARIZE_KEY_ID    - API key ID
# NOTARIZE_ISSUER    - API key issuer ID (from App Store Connect > Users and Access > Integrations > Team Key)
if [[ "${SKIP_NOTARIZATION:-}" != "1" && "${SKIP_SIGNING:-}" != "1" ]]; then
    missing=()
    [[ -z "${NOTARIZE_KEY:-}" ]] && missing+=("NOTARIZE_KEY")
    [[ -z "${NOTARIZE_KEY_ID:-}" ]] && missing+=("NOTARIZE_KEY_ID")
    [[ -z "${NOTARIZE_ISSUER:-}" ]] && missing+=("NOTARIZE_ISSUER")
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "ERROR: Missing notarization environment variables: ${missing[*]}"
        echo ""
        echo "Required:"
        echo "  NOTARIZE_KEY      - Path to App Store Connect API key (.p8 file)"
        echo "  NOTARIZE_KEY_ID   - API key ID"
        echo "  NOTARIZE_ISSUER   - API key issuer ID"
        echo ""
        echo "Generate at: App Store Connect > Users and Access > Integrations > Team Keys"
        echo "Set SKIP_NOTARIZATION=1 to skip notarization."
        exit 1
    fi
    # Expand ~ in path
    NOTARIZE_KEY="${NOTARIZE_KEY/#\~/$HOME}"
    if [[ ! -f "$NOTARIZE_KEY" ]]; then
        echo "ERROR: API key file not found: $NOTARIZE_KEY"
        exit 1
    fi
fi

sign_app() {
    local build_dir=$1
    local app="$build_dir/Chromium.app"

    echo ">>> Signing $app..."
    echo "    Identity: $SIGNING_IDENTITY"

    if [[ ! -d "$app" ]]; then
        echo "ERROR: Chromium.app not found at $app"
        exit 1
    fi

    # Sign from inside out: helpers first, then framework, then main app

    # Sign helper apps (Alerts, GPU, Plugin, Renderer, etc.)
    while IFS= read -r helper; do
        echo "    Signing helper: $(basename "$helper")"
        codesign --force --options runtime --timestamp \
            --entitlements "$ENTITLEMENTS" \
            --sign "$SIGNING_IDENTITY" "$helper"
    done < <(find "$app/Contents/Frameworks" -name "*.app" -maxdepth 5)

    # Sign all dylibs and .so files
    while IFS= read -r lib; do
        codesign --force --options runtime --timestamp \
            --sign "$SIGNING_IDENTITY" "$lib"
    done < <(find "$app" -name "*.dylib" -o -name "*.so")

    # Sign all standalone Mach-O executables inside the framework
    # (e.g. chrome_crashpad_handler, app_mode_loader, web_app_shortcut_copier)
    # These must be signed BEFORE the framework bundle itself
    local framework_dir="$app/Contents/Frameworks/Chromium Framework.framework"
    local framework_name="Chromium Framework"
    while IFS= read -r exe; do
        local rel="${exe#$framework_dir/}"
        # Skip files inside .app bundles (already signed above)
        [[ "$rel" == *".app/"* ]] && continue
        # Skip dylibs/so (already signed above)
        [[ "$exe" == *.dylib ]] && continue
        [[ "$exe" == *.so ]] && continue
        # Skip the main framework binary (signed with the .framework bundle below)
        [[ "$(basename "$exe")" == "$framework_name" ]] && continue
        if file "$exe" | grep -q "Mach-O"; then
            echo "    Signing executable: $(basename "$exe")"
            codesign --force --options runtime --timestamp \
                --sign "$SIGNING_IDENTITY" "$exe"
        fi
    done < <(find "$framework_dir" -type f -perm -u+x 2>/dev/null)

    # Sign the framework
    local framework="$app/Contents/Frameworks/Chromium Framework.framework"
    if [[ -d "$framework" ]]; then
        echo "    Signing framework: Chromium Framework.framework"
        codesign --force --options runtime --timestamp \
            --entitlements "$ENTITLEMENTS" \
            --sign "$SIGNING_IDENTITY" "$framework"
    fi

    # Sign the main app bundle
    echo "    Signing main app: Chromium.app"
    codesign --force --options runtime --timestamp \
        --entitlements "$ENTITLEMENTS" \
        --sign "$SIGNING_IDENTITY" "$app"

    # Verify signature
    echo "    Verifying signature..."
    codesign --verify --deep --strict "$app"
    echo "    Signature valid."
}

notarize_app() {
    local build_dir=$1
    local arch=$2
    local app="$build_dir/Chromium.app"
    local notarize_zip="$build_dir/Chromium-notarize.zip"

    echo ">>> Notarizing $app..."

    if [[ ! -d "$app" ]]; then
        echo "ERROR: Chromium.app not found at $app"
        exit 1
    fi

    # Create a zip for notarization submission (separate from distribution zip)
    echo "    Creating submission archive..."
    ditto -c -k --keepParent "$app" "$notarize_zip"

    # Submit for notarization
    echo "    Submitting to Apple notary service..."
    xcrun notarytool submit "$notarize_zip" \
        --key "$NOTARIZE_KEY" \
        --key-id "$NOTARIZE_KEY_ID" \
        --issuer "$NOTARIZE_ISSUER" \
        --wait

    # Clean up submission zip
    rm -f "$notarize_zip"

    # Staple the notarization ticket to the app
    echo "    Stapling notarization ticket..."
    xcrun stapler staple "$app"

    # Verify staple
    echo "    Verifying notarization..."
    xcrun stapler validate "$app"
    echo "    Notarization complete."
}

release_arch() {
    local arch=$1
    local build_dir="$CHROMIUM_SRC/out/Release-$arch"

    echo ""
    echo "============================================"
    echo "ABP Chrome Release - macOS $arch"
    echo "============================================"

    # Build
    echo ""
    echo ">>> Building $arch..."
    BUILD_ARCH="$arch" "$SCRIPT_DIR/build-mac.sh"

    # Validate
    if [[ "${SKIP_VALIDATION:-}" == "1" ]]; then
        echo ""
        echo ">>> Validation SKIPPED (SKIP_VALIDATION=1)"
    else
        echo ""
        echo ">>> Validating $arch..."
        local chrome_bin="$build_dir/Chromium.app/Contents/MacOS/Chromium"
        if ! "$SCRIPT_DIR/common/validate.sh" "$chrome_bin"; then
            echo "ERROR: Validation failed for $arch"
            exit 3
        fi
    fi

    # Sign
    if [[ "${SKIP_SIGNING:-}" == "1" ]]; then
        echo ""
        echo ">>> Signing SKIPPED (SKIP_SIGNING=1)"
    else
        echo ""
        sign_app "$build_dir"
    fi

    # Notarize (requires signing)
    if [[ "${SKIP_NOTARIZATION:-}" == "1" || "${SKIP_SIGNING:-}" == "1" ]]; then
        echo ""
        echo ">>> Notarization SKIPPED"
    else
        echo ""
        notarize_app "$build_dir" "$arch"
    fi

    # Package (after notarization so the stapled ticket is included in the zip)
    echo ""
    echo ">>> Packaging $arch..."
    BUILD_ARCH="$arch" "$SCRIPT_DIR/package-mac.sh"
}

echo "============================================"
echo "ABP Chrome Release - macOS"
echo "============================================"
echo "ABP Version: $ABP_VERSION"
echo "Chrome Version: $CHROME_VERSION"
echo "Architecture(s): $BUILD_ARCH"
echo "============================================"

case "$BUILD_ARCH" in
    arm64)
        release_arch "arm64"
        ;;
    x64)
        release_arch "x64"
        ;;
    universal)
        release_arch "universal"
        ;;
    all)
        # Build all architectures (arm64 + x64 + merge into universal)
        BUILD_ARCH="all" "$SCRIPT_DIR/build-mac.sh"

        # Validate, sign, notarize, and package each output
        for arch in arm64 universal; do
            local_build_dir="$CHROMIUM_SRC/out/Release-$arch"

            echo ""
            echo "============================================"
            echo "ABP Chrome Release - macOS $arch"
            echo "============================================"

            # Validate
            if [[ "${SKIP_VALIDATION:-}" == "1" ]]; then
                echo ""
                echo ">>> Validation SKIPPED (SKIP_VALIDATION=1)"
            else
                echo ""
                echo ">>> Validating $arch..."
                chrome_bin="$local_build_dir/Chromium.app/Contents/MacOS/Chromium"
                if ! "$SCRIPT_DIR/common/validate.sh" "$chrome_bin"; then
                    echo "ERROR: Validation failed for $arch"
                    exit 3
                fi
            fi

            # Sign
            if [[ "${SKIP_SIGNING:-}" == "1" ]]; then
                echo ""
                echo ">>> Signing SKIPPED (SKIP_SIGNING=1)"
            else
                echo ""
                sign_app "$local_build_dir"
            fi

            # Notarize
            if [[ "${SKIP_NOTARIZATION:-}" == "1" || "${SKIP_SIGNING:-}" == "1" ]]; then
                echo ""
                echo ">>> Notarization SKIPPED"
            else
                echo ""
                notarize_app "$local_build_dir" "$arch"
            fi

            # Package (after notarization so stapled ticket is included)
            echo ""
            echo ">>> Packaging $arch..."
            BUILD_ARCH="$arch" "$SCRIPT_DIR/package-mac.sh"
        done
        ;;
    *)
        echo "ERROR: Invalid BUILD_ARCH: $BUILD_ARCH"
        exit 1
        ;;
esac

echo ""
echo "============================================"
echo "All macOS releases complete!"
echo "Archives in: $CHROMIUM_SRC/dist/"
echo "============================================"
