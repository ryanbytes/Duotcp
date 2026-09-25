$ErrorActionPreference = 'Continue'
$exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\duotcp.exe'
if (-not (Test-Path $exe)) { throw "Missing $exe. Run build-windows.ps1 first." }
while ($true) {
    & $exe @args
    $code = $LASTEXITCODE
    Write-Warning "DuoTCP exited with code $code; restarting in 2 seconds"
    Start-Sleep -Seconds 2
}
