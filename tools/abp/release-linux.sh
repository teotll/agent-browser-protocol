#!/bin/bash
# Copyright 2026 Han Wang. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Build, validate, and package ABP Chrome for Linux
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

echo "============================================"
echo "ABP Chrome Release - Linux x64"
echo "============================================"
echo "ABP Version: $ABP_VERSION"
echo "============================================"

# Step 1: Build
echo ""
echo ">>> Step 1/3: Building..."
if ! "$SCRIPT_DIR/build-linux.sh"; then
    echo "ERROR: Build failed"
    exit 2
fi

# Step 2: Validate
if [[ "${SKIP_VALIDATION:-}" == "1" ]]; then
    echo ""
    echo ">>> Step 2/3: Validation SKIPPED (SKIP_VALIDATION=1)"
else
    echo ""
    echo ">>> Step 2/3: Validating..."
    if ! "$SCRIPT_DIR/common/validate.sh" "$CHROMIUM_SRC/out/Release/abp"; then
        echo "ERROR: Validation failed"
        exit 3
    fi
fi

# Step 3: Package
echo ""
echo ">>> Step 3/3: Packaging..."
if ! "$SCRIPT_DIR/package-linux.sh"; then
    echo "ERROR: Packaging failed"
    exit 4
fi

echo ""
echo "============================================"
echo "Release complete!"
echo "Archive: $CHROMIUM_SRC/dist/abp-${ABP_VERSION}-linux-x64.tar.gz"
echo "============================================"
