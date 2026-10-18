# SPDX-License-Identifier: GPL-3.0-only
<#
.SYNOPSIS
  Build, flash and monitor the C5 receiver and the S3 Wi-Fi bridge.

.EXAMPLE
  .\tools\build.ps1                                  # build both
  .\tools\build.ps1 bridge -Port COM7 -Flash -Monitor
  .\tools\build.ps1 receiver -Port COM8 -Flash
  .\tools\build.ps1 bridge -Menuconfig               # Wi-Fi name, password, pins, frame rate
#>
param(
    [ValidateSet('all', 'receiver', 'bridge')][string]$Target = 'all',
    [string]$Port,
    [switch]$Flash,
    [switch]$Monitor,
    [switch]$Menuconfig
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$idf = if ($env:IDF_PATH) { $env:IDF_PATH } else { 'C:\esp\v6.0.2\esp-idf' }
. (Join-Path $idf 'export.ps1') | Out-Null

function Invoke-Project([string]$name, [string]$dir, [string[]]$extra) {
    $build = Join-Path $root "build\$name"
    $idfArgs = @('-C', (Join-Path $root $dir), '-B', $build) + $extra
    if ($Menuconfig) { $idfArgs += 'menuconfig' }
    $idfArgs += 'build'
    if ($Flash -or $Monitor) {
        if (-not $Port) { throw "-Flash/-Monitor need -Port COMx (python tools\bridge_tool.py ports lists them)" }
        $idfArgs = @('-p', $Port) + $idfArgs
        if ($Flash) { $idfArgs += 'flash' }
        if ($Monitor) { $idfArgs += 'monitor' }
    }
    Write-Host "== ${name}: idf.py $($idfArgs -join ' ')" -ForegroundColor Cyan
    & idf.py @idfArgs
    if ($LASTEXITCODE -ne 0) { throw "$name failed (exit $LASTEXITCODE)" }
}

# The receiver is the unmodified upstream submodule: its sdkconfig lives in
# the build folder so the submodule stays clean.
if ($Target -in 'all', 'receiver') {
    Invoke-Project 'receiver' 'receiver' @('-D', "SDKCONFIG=$(Join-Path $root 'build\receiver\sdkconfig')")
}
if ($Target -in 'all', 'bridge') {
    Invoke-Project 'bridge' 'bridge' @()
}
