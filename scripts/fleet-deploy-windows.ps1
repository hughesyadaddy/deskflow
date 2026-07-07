# Runs ON tiny11 (local or via SSH). Pull fleet branch, build, install Deskflow + Mouser.
#requires -Version 5.1
param(
  [string]$DeskflowRoot = $env:FLEET_DESKFLOW_ROOT,
  [string]$MouserRoot = $env:FLEET_MOUSER_ROOT,
  [string]$Branch = $(if ($env:FLEET_BRANCH) { $env:FLEET_BRANCH } else { 'refactor/fleet-state-hub' }),
  [int]$DeployDeskflow = $(if ($null -ne $env:FLEET_DEPLOY_DESKFLOW) { [int]$env:FLEET_DEPLOY_DESKFLOW } else { 1 }),
  [int]$DeployMouser = $(if ($null -ne $env:FLEET_DEPLOY_MOUSER) { [int]$env:FLEET_DEPLOY_MOUSER } else { 1 })
)

$ErrorActionPreference = 'Stop'
$hostName = $env:COMPUTERNAME

if (-not $DeskflowRoot) {
  $DeskflowRoot = Join-Path $env:USERPROFILE 'Desktop\deskflow'
}
if (-not $MouserRoot) {
  $MouserRoot = Join-Path $env:USERPROFILE 'Desktop\Mouser'
}

function Sync-DeskflowRepo {
  Write-Host "== [$hostName] deskflow pull ($Branch) =="
  Set-Location $DeskflowRoot
  git fetch origin
  git checkout $Branch
  git pull --ff-only origin $Branch
  git log -1 --oneline
}

function Deploy-Deskflow {
  Write-Host "== [$hostName] deskflow build + install =="
  Set-Location $DeskflowRoot
  & (Join-Path $DeskflowRoot 'scripts\build-windows.ps1') -Install
  if ($LASTEXITCODE -ne 0) { throw "deskflow build/install failed (exit $LASTEXITCODE)" }
}

function Deploy-Mouser {
  if ($DeployMouser -ne 1) { return }
  if (-not (Test-Path $MouserRoot)) {
    Write-Host "skip Mouser: missing $MouserRoot"
    return
  }
  Write-Host "== [$hostName] Mouser build + install =="
  $bat = Join-Path $env:USERPROFILE 'build-mouser.bat'
  if (Test-Path $bat) {
    cmd /c $bat
  } else {
    Set-Location $MouserRoot
    $py = Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe" |
      Select-Object -First 1 -ExpandProperty FullName
    if (-not $py) { throw 'Python not found for Mouser' }
    if (-not (Test-Path (Join-Path $MouserRoot '.venv'))) {
      & $py -m venv (Join-Path $MouserRoot '.venv')
      & (Join-Path $MouserRoot '.venv\Scripts\python.exe') -m pip install --quiet -r requirements.txt pyinstaller
    }
    Stop-Process -Name Mouser -Force -ErrorAction SilentlyContinue
    & (Join-Path $MouserRoot '.venv\Scripts\python.exe') (Join-Path $MouserRoot 'scripts\build_and_install.py')
  }
}

Write-Host "=== fleet-deploy-windows on $hostName ==="

# Sync helper scripts + Mouser repo layout
$sync = Join-Path $DeskflowRoot 'scripts\sync-desktop-repos-tiny11.ps1'
if (Test-Path $sync) {
  & $sync
}

if ($DeployDeskflow -eq 1) {
  Sync-DeskflowRepo
  Deploy-Deskflow
}
Deploy-Mouser

Write-Host "=== done: $hostName ==="
