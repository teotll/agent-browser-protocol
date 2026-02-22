#!/bin/bash
# Sign and notarize ABP Chrome for macOS
# Usage: ./sign-mac.sh <build_dir>
#   e.g. ./sign-mac.sh out/Release-arm64
#
# Environment variables:
#   SIGNING_IDENTITY    - Code signing identity (default: "Developer ID Application: Han Wang (72YUDGUH4G)")
#   SKIP_NOTARIZATION   - Set to "1" to skip notarization
#   NOTARIZE_KEY        - Path to App Store Connect API key (.p8 file)
#   NOTARIZE_KEY_ID     - API key ID
#   NOTARIZE_ISSUER     - API key issuer ID
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHROMIUM_SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"

BUILD_DIR="${1:?Usage: $0 <build_dir>}"

# Resolve relative paths against CHROMIUM_SRC
if [[ "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="$CHROMIUM_SRC/$BUILD_DIR"
fi

APP="$BUILD_DIR/ABP.app"
if [[ ! -d "$APP" ]]; then
    echo "ERROR: ABP.app not found at $APP"
    exit 1
fi

SIGNING_IDENTITY="${SIGNING_IDENTITY:-Developer ID Application: Han Wang (72YUDGUH4G)}"
ENTITLEMENTS="$CHROMIUM_SRC/chrome/app/app-entitlements.plist"
ENTITLEMENTS_RENDERER="$CHROMIUM_SRC/chrome/app/helper-renderer-entitlements.plist"
ENTITLEMENTS_GPU="$CHROMIUM_SRC/chrome/app/helper-gpu-entitlements.plist"

# Validate notarization credentials upfront
if [[ "${SKIP_NOTARIZATION:-}" != "1" ]]; then
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

echo "============================================"
echo "ABP Chrome Sign - macOS"
echo "============================================"
echo "App: $APP"
echo "Identity: $SIGNING_IDENTITY"
echo "Notarize: $( [[ "${SKIP_NOTARIZATION:-}" == "1" ]] && echo "SKIP" || echo "YES" )"
echo "============================================"

# --- Signing ---

echo ""
echo ">>> Signing $APP..."

# Sign from inside out: helpers first, then framework, then main app

# Sign helper apps with per-helper entitlements
# Renderer and GPU helpers need com.apple.security.cs.allow-jit for V8 JIT
while IFS= read -r helper; do
    helper_name="$(basename "$helper")"
    echo "    Signing helper: $helper_name"
    helper_ent="$ENTITLEMENTS"
    if [[ "$helper_name" == *"Renderer"* ]]; then
        helper_ent="$ENTITLEMENTS_RENDERER"
    elif [[ "$helper_name" == *"GPU"* ]]; then
        helper_ent="$ENTITLEMENTS_GPU"
    fi
    codesign --force --options runtime --timestamp \
        --entitlements "$helper_ent" \
        --sign "$SIGNING_IDENTITY" "$helper"
done < <(find "$APP/Contents/Frameworks" -name "*.app" -maxdepth 5)

# Sign all dylibs and .so files
while IFS= read -r lib; do
    codesign --force --options runtime --timestamp \
        --sign "$SIGNING_IDENTITY" "$lib"
done < <(find "$APP" -name "*.dylib" -o -name "*.so")

# Sign all standalone Mach-O executables inside the framework
# (e.g. chrome_crashpad_handler, app_mode_loader, web_app_shortcut_copier)
# These must be signed BEFORE the framework bundle itself
framework_dir="$APP/Contents/Frameworks/ABP Framework.framework"
framework_name="ABP Framework"
while IFS= read -r exe; do
    rel="${exe#$framework_dir/}"
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
framework="$APP/Contents/Frameworks/ABP Framework.framework"
if [[ -d "$framework" ]]; then
    echo "    Signing framework: ABP Framework.framework"
    codesign --force --options runtime --timestamp \
        --entitlements "$ENTITLEMENTS" \
        --sign "$SIGNING_IDENTITY" "$framework"
fi

# Sign the main app bundle
echo "    Signing main app: ABP.app"
codesign --force --options runtime --timestamp \
    --entitlements "$ENTITLEMENTS" \
    --sign "$SIGNING_IDENTITY" "$APP"

# Verify signature
echo "    Verifying signature..."
codesign --verify --deep --strict "$APP"
echo "    Signature valid."

# --- Notarization ---

if [[ "${SKIP_NOTARIZATION:-}" == "1" ]]; then
    echo ""
    echo ">>> Notarization SKIPPED (SKIP_NOTARIZATION=1)"
else
    echo ""
    echo ">>> Notarizing $APP..."

    notarize_zip="$BUILD_DIR/ABP-notarize.zip"

    # Create a zip for notarization submission (separate from distribution zip)
    echo "    Creating submission archive..."
    ditto -c -k --keepParent "$APP" "$notarize_zip"

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
    xcrun stapler staple "$APP"

    # Verify staple
    echo "    Verifying notarization..."
    xcrun stapler validate "$APP"
    echo "    Notarization complete."
fi

echo ""
echo "============================================"
echo "Signing complete!"
echo "============================================"
