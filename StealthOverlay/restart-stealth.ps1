# restart-stealth.ps1
# One-click restart for StealthOverlayApp
# Usage: .\restart-stealth.ps1

# Stop any running instance (silent if not running)
Get-Process StealthOverlayApp -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force }

Start-Sleep -Milliseconds 200

# Path to built executable (relative to repo root)
$exe = Join-Path $PSScriptRoot 'x64\Release\StealthOverlayApp.exe'

if (Test-Path $exe) {
    Start-Process $exe
} else {
    Write-Error "Executable not found: $exe"
    exit 1
}

Start-Sleep -Milliseconds 300

# Print process info
Get-Process StealthOverlayApp -ErrorAction SilentlyContinue | Select-Object Id, ProcessName, MainWindowTitle
