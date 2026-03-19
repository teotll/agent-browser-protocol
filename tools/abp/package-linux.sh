#!/bin/bash
# Copyright 2026 Han Wang. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Package Chrome with ABP for Linux distribution
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

BUILD_DIR="$CHROMIUM_SRC/out/Release"
DIST_DIR="$CHROMIUM_SRC/dist"
ARCHIVE_NAME="abp-${ABP_VERSION}-linux-x64.tar.gz"

# Validate build exists
if [[ ! -f "$BUILD_DIR/abp" ]]; then
    echo "ERROR: ABP binary not found at $BUILD_DIR/abp"
    echo "Run build-linux.sh first"
    exit 1
fi

echo "=== Packaging ABP Chrome for Linux ==="
echo "ABP Version: $ABP_VERSION"
echo "Build: $BUILD_DIR"
echo "Output: $DIST_DIR/$ARCHIVE_NAME"

# Create staging directory
STAGING_DIR=$(mktemp -d)
STAGING_APP="$STAGING_DIR/abp-chrome"
mkdir -p "$STAGING_APP"

cleanup() {
    rm -rf "$STAGING_DIR"
}
trap cleanup EXIT

echo "=== Copying files ==="

# Core binary
cp "$BUILD_DIR/abp" "$STAGING_APP/"

# Optional Chromium runtime binaries and metadata.
optional_files=(
    chrome_crashpad_handler
    chrome_management_service
    chrome_sandbox
    vk_swiftshader_icd.json
)
for file in "${optional_files[@]}"; do
    if [[ -f "$BUILD_DIR/$file" ]]; then
        cp "$BUILD_DIR/$file" "$STAGING_APP/"
    fi
done

# Shared libraries
cp "$BUILD_DIR"/*.so "$STAGING_APP/" 2>/dev/null || true
cp "$BUILD_DIR"/*.so.* "$STAGING_APP/" 2>/dev/null || true

# Resource files
cp "$BUILD_DIR"/*.pak "$STAGING_APP/"
cp "$BUILD_DIR/icudtl.dat" "$STAGING_APP/"

# V8 snapshot
if [[ -f "$BUILD_DIR/v8_context_snapshot.bin" ]]; then
    cp "$BUILD_DIR/v8_context_snapshot.bin" "$STAGING_APP/"
fi
if [[ -f "$BUILD_DIR/snapshot_blob.bin" ]]; then
    cp "$BUILD_DIR/snapshot_blob.bin" "$STAGING_APP/"
fi

# Locales
cp -r "$BUILD_DIR/locales" "$STAGING_APP/"

# Resources directory (if exists)
optional_dirs=(
    resources
    default_apps
    lib
    MEIPreload
    PrivacySandboxAttestationsPreloaded
    WidevineCdm
)
for dir in "${optional_dirs[@]}"; do
    if [[ -d "$BUILD_DIR/$dir" ]]; then
        cp -r "$BUILD_DIR/$dir" "$STAGING_APP/"
    fi
done

# Create dist directory
mkdir -p "$DIST_DIR"

# Create archive
echo "=== Creating archive ==="
tar -czf "$DIST_DIR/$ARCHIVE_NAME" -C "$STAGING_DIR" "abp-chrome"

# Report results
SIZE=$(du -h "$DIST_DIR/$ARCHIVE_NAME" | cut -f1)
echo "=== Package complete ==="
echo "Archive: $DIST_DIR/$ARCHIVE_NAME"
echo "Size: $SIZE"
