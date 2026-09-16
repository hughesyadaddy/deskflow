#requires -Version 5.1
<#
.SYNOPSIS
  Stop the Deskflow Windows service and every Deskflow process (thin wrapper).
.DESCRIPTION
  Delegates to scripts/deskflow-ctl.ps1 stop, the single owner of the
  Deskflow process lifecycle on Windows (Stop-Service, then taskkill by PID
  for every process under the install root, in every session). Never touches
  Mouser. Use it before a debug session so the installed copy and the service
  do not conflict with a build-tree run.
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\quit-all-windows.ps1
#>
param([string]$InstallDir)

$ErrorActionPreference = 'Stop'
$ctl = Join-Path $PSScriptRoot 'deskflow-ctl.ps1'
if (-not (Test-Path $ctl)) { throw "deskflow-ctl.ps1 missing at $ctl" }
$ctlArgs = @{ Verb = 'stop' }
if ($InstallDir) { $ctlArgs.InstallDir = $InstallDir }
& $ctl @ctlArgs
