$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $root "Netplay-Diagnostics-$stamp.zip"
$temp = Join-Path $env:TEMP "pcsx2-netplay-diag-$stamp"

New-Item -ItemType Directory -Force -Path $temp | Out-Null

$logDir = Join-Path $root 'logs\netplay'
if (Test-Path $logDir) {
    Copy-Item $logDir (Join-Path $temp 'netplay-logs') -Recurse -Force
}

foreach ($name in @('NETPLAY_BUILD_INFO.txt', 'PCSX2_UPSTREAM_SHA.txt', 'PCSX2_UPSTREAM_VERSION.txt')) {
    $source = Join-Path $root $name
    if (Test-Path $source) {
        Copy-Item $source (Join-Path $temp $name) -Force
    }
}

$summary = @"
PCSX2 Modern Netplay diagnostic package
Created: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss K')

Please create this package on BOTH computers after a failed test and send both ZIP files together.
The package contains Netplay logs plus build/upstream revision metadata.
It does not intentionally include BIOS files, game images, memory cards, save states, or controller profiles.
"@
$summary | Set-Content -Encoding UTF8 (Join-Path $temp 'README-DIAGNOSTICS.txt')

Compress-Archive -Path (Join-Path $temp '*') -DestinationPath $out -Force
Remove-Item $temp -Recurse -Force

Write-Host "Diagnostic package created:" -ForegroundColor Green
Write-Host $out -ForegroundColor Cyan
Start-Process explorer.exe "/select,`"$out`""
