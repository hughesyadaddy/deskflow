#requires -Version 5.1
<#
.SYNOPSIS
  Stop Deskflow via deskflow-ctl, install the Release build to Program Files, start it again.
.DESCRIPTION
  Process lifecycle goes through scripts/deskflow-ctl.ps1 only:
    ctl stop  -> remove rogue install copies -> copy the windeployqt-staged
    build from build/bin/Release into DESKFLOW_INSTALL_DIR -> ctl start
    (sign, sc create/config, Start-Service, wait for the service-owned core,
    launch one GUI into the console session) -> ctl assert-single.
  This script never kills by image name and never touches Mouser (Mouser has
  its own installer).

  Re-launches elevated when installing under Program Files without admin rights.
.EXAMPLE
  pwsh scripts\install-windows.ps1
  pwsh scripts\install-windows.ps1 -NoRestart
#>
param(
  [switch]$NoRestart,
  [string]$BuildDir,
  [string]$InstallDir,
  [string]$TranscriptPath
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$script:Ctl = Join-Path $PSScriptRoot 'deskflow-ctl.ps1'

function Invoke-DeskflowCtl {
  # Run a deskflow-ctl verb in-process (we are already elevated by Assert-Admin).
  param([Parameter(Mandatory)][string]$CtlVerb, [string]$Dir, [switch]$NoGui)
  if (-not (Test-Path $script:Ctl)) { throw "deskflow-ctl.ps1 missing at $script:Ctl" }
  $ctlArgs = @{ Verb = $CtlVerb; InstallDir = $Dir }
  if ($NoGui) { $ctlArgs.NoGui = $true }
  & $script:Ctl @ctlArgs
}

function Import-DeskflowEnv {
  $envFile = Join-Path $root '.env'
  if (-not (Test-Path $envFile)) { return }
  Get-Content $envFile | ForEach-Object {
    if ($_ -match '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$' -and $_ -notmatch '^\s*#') {
      Set-Item -Path "env:$($matches[1])" -Value ($matches[2].Trim().Trim('"'))
    }
  }
}

function Test-IsAdmin {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = New-Object Security.Principal.WindowsPrincipal($identity)
  return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Assert-Admin {
  if (Test-IsAdmin) { return }
  Write-Host 'Re-launching elevated for Program Files install...'
  $scriptPath = if ($PSCommandPath) { $PSCommandPath } else { $MyInvocation.MyCommand.Path }
  if (-not $scriptPath) { throw 'Could not resolve install script path for elevation.' }
  $logPath = Join-Path $env:TEMP 'deskflow-install.log'
  $argList = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $scriptPath,
    '-TranscriptPath', $logPath
  )
  if ($NoRestart) { $argList += '-NoRestart' }
  if ($BuildDir) { $argList += @('-BuildDir', $BuildDir) }
  if ($InstallDir) { $argList += @('-InstallDir', $InstallDir) }
  # Note: -Wait would block on the whole process tree (the installer launches
  # the GUI, which keeps running). WaitForExit() waits on the direct child only.
  $proc = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argList -PassThru
  $proc.WaitForExit()
  if ($proc.ExitCode -ne 0 -and (Test-Path $logPath)) {
    Write-Host "Install failed (exit $($proc.ExitCode)). Elevated log: $logPath"
    Get-Content $logPath -Tail 30 | ForEach-Object { Write-Host $_ }
  }
  exit $proc.ExitCode
}

function Get-RogueInstallPaths {
  param([string]$CanonicalDir)

  $paths = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  foreach ($candidate in @(
      (Join-Path $env:LOCALAPPDATA 'Programs\Deskflow'),
      (Join-Path ${env:ProgramFiles(x86)} 'Deskflow'),
      'C:\Program'
    )) {
    if ($candidate -and (Test-Path (Join-Path $candidate 'deskflow.exe'))) {
      [void]$paths.Add([System.IO.Path]::GetFullPath($candidate))
    }
  }

  $canonical = [System.IO.Path]::GetFullPath($CanonicalDir)
  return @($paths | Where-Object { $_ -ne $canonical })
}

function Remove-RogueInstalls {
  param(
    [string[]]$Paths,
    [string]$CanonicalDir
  )

  if ($Paths.Count -eq 0) { return }

  # ctl stop only reaches processes under the canonical root; a rogue copy
  # running from elsewhere is stopped by PID here before its directory goes.
  foreach ($rogue in $Paths) {
    $rogueFull = [System.IO.Path]::GetFullPath($rogue).TrimEnd('\').ToLowerInvariant()
    Get-CimInstance Win32_Process -Filter "Name LIKE 'deskflow%'" -ErrorAction SilentlyContinue |
      Where-Object { $_.ExecutablePath -and $_.ExecutablePath.ToLowerInvariant().StartsWith($rogueFull + '\') } |
      ForEach-Object {
        Write-Host "  stopping rogue PID $($_.ProcessId) ($($_.ExecutablePath))"
        cmd.exe /c "taskkill /F /T /PID $($_.ProcessId) >nul 2>&1"
      }
  }
  foreach ($rogue in $Paths) {
    Write-Host "Removing rogue install: $rogue"
    Remove-Item -LiteralPath $rogue -Recurse -Force -ErrorAction SilentlyContinue
  }
}

function Set-DeskflowRunRegistry {
  param([string]$GuiPath)

  $runKey = 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
  $target = "`"$GuiPath`""
  $existing = (Get-ItemProperty -Path $runKey -Name 'Deskflow' -ErrorAction SilentlyContinue).Deskflow
  if ($existing -ne $target) {
    Write-Host "Updating login startup entry -> $GuiPath"
    Set-ItemProperty -Path $runKey -Name 'Deskflow' -Value $target
  }
}

Import-DeskflowEnv
if ($TranscriptPath) {
  Start-Transcript -Path $TranscriptPath -Force | Out-Null
}
try {
Assert-Admin

if (-not $BuildDir) {
  $BuildDir = if ($env:DESKFLOW_BUILD_DIR) { $env:DESKFLOW_BUILD_DIR } else { 'build' }
}
if (-not $InstallDir) {
  $InstallDir = if ($env:DESKFLOW_INSTALL_DIR) { $env:DESKFLOW_INSTALL_DIR } else { Join-Path ${env:ProgramFiles} 'Deskflow' }
}

$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
# Multi-config generators (MSVC) emit bin\Release; single-config (Ninja) emits bin.
$releaseDir = Join-Path $buildPath 'bin\Release'
if (-not (Test-Path (Join-Path $releaseDir 'deskflow.exe'))) {
  $releaseDir = Join-Path $buildPath 'bin'
}
$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)

if (-not (Test-Path (Join-Path $releaseDir 'deskflow.exe'))) {
  throw "Release build not found at $releaseDir - configure and build first."
}

Invoke-DeskflowCtl -CtlVerb stop -Dir $InstallDir

$roguePaths = Get-RogueInstallPaths -CanonicalDir $InstallDir
Remove-RogueInstalls -Paths $roguePaths -CanonicalDir $InstallDir

Write-Host "== Installing to $InstallDir =="
if (Test-Path $InstallDir) {
  Remove-Item -LiteralPath $InstallDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

Write-Host "Copying runtime from $releaseDir..."
Copy-Item -Path (Join-Path $releaseDir '*') -Destination $InstallDir -Recurse -Force
Remove-Item (Join-Path $InstallDir 'legacytests.exe') -Force -ErrorAction SilentlyContinue

$srcWidgets = Get-Item -LiteralPath (Join-Path $releaseDir 'Qt6Widgets.dll')
$dstWidgets = Get-Item -LiteralPath (Join-Path $InstallDir 'Qt6Widgets.dll')
if ($srcWidgets.Length -ne $dstWidgets.Length) {
  throw "Qt runtime mismatch after install (expected $($srcWidgets.Length) bytes, got $($dstWidgets.Length))."
}

$gui = Join-Path $InstallDir 'deskflow.exe'
Set-DeskflowRunRegistry -GuiPath $gui

# ctl start: sign (sign-windows.ps1) -> sc create/config -> Start-Service ->
# wait for the service-owned deskflow-core -> one GUI in the console session.
# Signing happens inside ctl start, before anything is running, because
# signtool needs write access to the images and running exes are locked.
if ($NoRestart) {
  Invoke-DeskflowCtl -CtlVerb start -Dir $InstallDir -NoGui
} else {
  Invoke-DeskflowCtl -CtlVerb start -Dir $InstallDir
  Invoke-DeskflowCtl -CtlVerb 'assert-single' -Dir $InstallDir
}

Write-Host "== Done: single install at $InstallDir =="
$svcPath = (Get-CimInstance Win32_Service -Filter "Name='Deskflow'" -ErrorAction SilentlyContinue).PathName
Write-Host "== Service: $svcPath =="
} finally {
  if ($TranscriptPath) { Stop-Transcript | Out-Null }
}
