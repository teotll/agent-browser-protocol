# Validate ABP Chrome is functional
param(
    [Parameter(Mandatory=$true)]
    [string]$ChromeBinary,

    [int]$TimeoutSeconds = 20
)

$ErrorActionPreference = "Stop"

# Bypass proxy for localhost
[System.Net.WebRequest]::DefaultWebProxy = $null

if (-not (Test-Path $ChromeBinary)) {
    Write-Error "ERROR: Chrome binary not found: $ChromeBinary"
    exit 1
}

Write-Host "=== Validating ABP Chrome ===" -ForegroundColor Cyan
Write-Host "Binary: $ChromeBinary"
Write-Host "Timeout: ${TimeoutSeconds}s"

# Create isolated user data dir to avoid attaching to existing Chrome instance
$tempUserDataDir = Join-Path $env:TEMP "abp-validate-$([System.Guid]::NewGuid().ToString('N').Substring(0,8))"
New-Item -ItemType Directory -Path $tempUserDataDir -Force | Out-Null

$chromeDir = Split-Path -Parent $ChromeBinary
Write-Host "Starting ABP..."
Write-Host "  Directory: $chromeDir"
Write-Host "  User data dir: $tempUserDataDir"

# Start with isolated profile to ensure fresh instance with ABP server
$chromeProcess = Start-Process -FilePath $ChromeBinary -ArgumentList "--user-data-dir=`"$tempUserDataDir`"" -WorkingDirectory $chromeDir -PassThru
Write-Host "  Process started with PID: $($chromeProcess.Id)"

# Wait for browser to initialize before polling
Write-Host "  Waiting 5s for browser startup..."
Start-Sleep -Seconds 5

try {
    # Wait for ABP endpoint to be ready
    Write-Host "Waiting for ABP endpoint at http://localhost:8222/api/v1/tabs ..."
    $elapsed = 0
    while ($elapsed -lt $TimeoutSeconds) {
        # Check if process is still running
        $chromeProcess.Refresh()
        if ($chromeProcess.HasExited) {
            Write-Host "  WARNING: Process $($chromeProcess.Id) has exited with code $($chromeProcess.ExitCode)" -ForegroundColor Yellow
        }

        try {
            Write-Host "  [$elapsed] Attempting HTTP request..."
            $response = Invoke-WebRequest -Uri "http://localhost:8222/api/v1/tabs" -UseBasicParsing -TimeoutSec 2 -ErrorAction Stop
            Write-Host "  [$elapsed] Got response: StatusCode=$($response.StatusCode)"
            if ($response.StatusCode -eq 200) {
                Write-Host "ABP endpoint responding (HTTP 200)"
                Write-Host "  Response: $($response.Content.Substring(0, [Math]::Min(200, $response.Content.Length)))..."

                # Verify response contains tab objects
                if ($response.Content -match '"id"') {
                    Write-Host "=== Validation PASSED ===" -ForegroundColor Green
                    exit 0
                } else {
                    Write-Error "ERROR: Unexpected response format: $($response.Content)"
                    exit 3
                }
            }
        } catch {
            $errMsg = $_.Exception.Message
            Write-Host "  [$elapsed] Request failed: $errMsg" -ForegroundColor Gray
        }
        Start-Sleep -Seconds 1
        $elapsed++
    }

    # Try one final request to see what's happening
    Write-Host "  Trying final request..."
    try {
        $finalResponse = Invoke-WebRequest -Uri "http://localhost:8222/api/v1/tabs" -UseBasicParsing -TimeoutSec 5
        Write-Host "  Response Status: $($finalResponse.StatusCode)"
        Write-Host "  Response Content: $($finalResponse.Content)"
    } catch {
        Write-Host "  Request failed: $($_.Exception.Message)"
    }

    # Also check what's on port 8222
    Write-Host "  Checking port 8222..."
    $netstat = & netstat -ano 2>&1 | Select-String "8222"
    Write-Host "  Netstat: $netstat"

    Write-Error "ERROR: ABP endpoint did not respond within ${TimeoutSeconds}s"
    exit 3
} finally {
    # Cleanup process
    $chromeProcess.Refresh()
    Write-Host "Final process state: HasExited=$($chromeProcess.HasExited)"
    if (-not $chromeProcess.HasExited) {
        Write-Host "Stopping Chrome (PID: $($chromeProcess.Id))..."
        Stop-Process -Id $chromeProcess.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 1
    }
    # Cleanup temp user data dir
    if (Test-Path $tempUserDataDir) {
        Remove-Item -Recurse -Force $tempUserDataDir -ErrorAction SilentlyContinue
    }
}
