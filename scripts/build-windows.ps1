$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$sdr = 'C:\Program Files\SDRplay\API'
if (-not (Test-Path "$sdr\inc\sdrplay_api.h")) {
    throw "SDRplay API development headers not found at $sdr\inc. Install SDRplay Hardware API 3.15 first."
}
cmake -S $root -B "$root\build" -A x64 "-DSDRPLAY_ROOT=$sdr"
cmake --build "$root\build" --config Release --target duotcp protocol_tests
& "$root\build\Release\protocol_tests.exe"
New-Item -ItemType Directory -Force "$root\dist" | Out-Null
Copy-Item "$root\build\Release\duotcp.exe" "$root\dist\duotcp.exe" -Force
Write-Host "Built: $root\dist\duotcp.exe"
