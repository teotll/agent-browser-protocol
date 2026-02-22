# Copyright 2026 Han Wang. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Build, validate, and package ABP Chrome for Windows
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

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "ABP Chrome Release - Windows x64" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "ABP Version: $env:ABP_VERSION"
Write-Host "============================================" -ForegroundColor Cyan

# Step 1: Build
Write-Host ""
Write-Host ">>> Step 1/3: Building..." -ForegroundColor Yellow
& "$ScriptDir\build-win.ps1"
if ($LASTEXITCODE -ne 0) {
    Write-Error "ERROR: Build failed"
    exit 2
}

# Step 2: Validate
if ($env:SKIP_VALIDATION -eq "1") {
    Write-Host ""
    Write-Host ">>> Step 2/3: Validation SKIPPED (SKIP_VALIDATION=1)" -ForegroundColor Yellow
} else {
    Write-Host ""
    Write-Host ">>> Step 2/3: Validating..." -ForegroundColor Yellow
    & "$ScriptDir\common\validate.ps1" -ChromeBinary "$ChromiumSrc\out\Release\abp.exe"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "ERROR: Validation failed"
        exit 3
    }
}

# Step 3: Package
Write-Host ""
Write-Host ">>> Step 3/3: Packaging..." -ForegroundColor Yellow
& "$ScriptDir\package-win.ps1"
if ($LASTEXITCODE -ne 0) {
    Write-Error "ERROR: Packaging failed"
    exit 4
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host "Release complete!" -ForegroundColor Green
Write-Host "Archive: $ChromiumSrc\dist\abp-$env:ABP_VERSION-win-x64.zip" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Green
