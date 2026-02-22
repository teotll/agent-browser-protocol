# Copyright 2026 Han Wang. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Build Chrome with ABP for Windows
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ChromiumSrc = (Resolve-Path "$ScriptDir\..\..").Path

# Read ABP_VERSION from package.json if not set
if (-not $env:ABP_VERSION) {
    $PkgJson = "$ScriptDir\..\abp-npm\package.json"
    if (Test-Path $PkgJson) {
        $pkg = Get-Content $PkgJson | ConvertFrom-Json
        $env:ABP_VERSION = $pkg.version
    } else {
        Write-Error "ERROR: ABP_VERSION not set and $PkgJson not found"
        exit 1
    }
}

# Read CHROME_VERSION from chrome\VERSION if not set
if (-not $env:CHROME_VERSION) {
    $VersionFile = "$ChromiumSrc\chrome\VERSION"
    if (Test-Path $VersionFile) {
        $v = @{}
        Get-Content $VersionFile | ForEach-Object {
            $parts = $_ -split '='
            $v[$parts[0]] = $parts[1]
        }
        $env:CHROME_VERSION = "$($v['MAJOR']).$($v['MINOR']).$($v['BUILD']).$($v['PATCH'])"
    } else {
        Write-Error "ERROR: CHROME_VERSION not set and chrome\VERSION not found"
        exit 1
    }
}

# Validate we're in chromium source
if (-not (Test-Path "$ChromiumSrc\BUILD.gn")) {
    Write-Error "ERROR: Must be run from chromium source directory"
    Write-Host "Expected BUILD.gn at: $ChromiumSrc\BUILD.gn"
    exit 1
}

Write-Host "=== Building ABP Chrome for Windows ===" -ForegroundColor Cyan
Write-Host "ABP Version: $env:ABP_VERSION"
Write-Host "Chrome Version: $env:CHROME_VERSION"
Write-Host "Source: $ChromiumSrc"

Set-Location $ChromiumSrc

# Configure release build
$GnArgs = 'is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0'

Write-Host "=== Configuring build with GN ===" -ForegroundColor Cyan
& cmd /c "gn gen out/Release --args=`"$GnArgs`""
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "=== Building ABP ===" -ForegroundColor Cyan
& cmd /c "autoninja -C out/Release chrome"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "=== Build complete ===" -ForegroundColor Green
Write-Host "Output: $ChromiumSrc\out\Release\abp.exe"
