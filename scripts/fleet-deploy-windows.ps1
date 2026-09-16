#requires -Version 5.1
# Runs ON tiny11 (local or via SSH). Pull fleet branch, build, install Deskflow + Mouser.
# Deskflow's process lifecycle is owned by scripts/deskflow-ctl.ps1 (called from
# install-windows.ps1); this script never stops or kills Mouser -- the Mouser
# installer restarts Mouser itself when MOUSER_RESTART=1.
param(
  [string]$DeskflowRoot = $env:FLEET_DESKFLOW_ROOT,
  [string]$MouserRoot = $env:FLEET_MOUSER_ROOT,
  [string]$Branch = $(if ($env:FLEET_BRANCH) { $env:FLEET_BRANCH } else { 'main' }),
  # Mouser follows $Branch unless FLEET_MOUSER_BRANCH says otherwise.
  [string]$MouserBranch = $env:FLEET_MOUSER_BRANCH,
  [string]$MouserRemoteUrl = 'https://github.com/hughesyadaddy/Mouser.git',
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
if (-not $MouserBranch) { $MouserBranch = $Branch }

function Invoke-Git {
  # git writes progress/informational text to stderr. Under Windows PowerShell
  # 5.1 with $ErrorActionPreference = 'Stop', a native command's stderr in a
  # pipeline becomes a terminating error even on success. Run with 'Continue',
  # stream everything to the host, and judge the call by its exit code.
  param([Parameter(ValueFromRemainingArguments = $true)][string[]]$GitArgs)
  $prev = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & git @GitArgs 2>&1 | ForEach-Object { "$_" } | Out-Host
    $code = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $prev
  }
  if ($code -ne 0) { throw "git $($GitArgs -join ' ') failed (exit $code)" }
}

function Invoke-Native {
  # Run a native exe / script and throw on a non-zero exit code.
  param([string]$Label, [string]$FilePath, [string[]]$ArgumentList = @())
  & $FilePath @ArgumentList
  if ($LASTEXITCODE -ne 0) { throw "$Label failed (exit $LASTEXITCODE)" }
}

function Sync-DeskflowRepo {
  Set-Location $DeskflowRoot
  if ($env:FLEET_SKIP_GIT_PULL -eq '1') {
    # The controller already synced this checkout (branch, --ref or --rollback).
    Write-Host "== [$hostName] deskflow sync skipped (FLEET_SKIP_GIT_PULL=1) =="
    Invoke-Git log -1 --oneline
    return
  }
  $ref = $env:FLEET_DESKFLOW_REF
  if ($ref -and $ref -ne 'HEAD') {
    Write-Host "== [$hostName] deskflow checkout --detach $ref =="
    Invoke-Git fetch origin
    Invoke-Git checkout --detach $ref
  } elseif (-not $ref) {
    Write-Host "== [$hostName] deskflow pull ($Branch) =="
    Invoke-Git fetch origin
    Invoke-Git checkout $Branch
    Invoke-Git pull --ff-only origin $Branch
  } else {
    Write-Host "== [$hostName] deskflow: building checked-out HEAD =="
  }
  Invoke-Git log -1 --oneline
}

function Deploy-Deskflow {
  Write-Host "== [$hostName] deskflow build + install =="
  Set-Location $DeskflowRoot
  # build-windows.ps1 -Install signs via scripts/sign-windows.ps1 and re-verifies
  # the install root; any signing problem throws through here.
  $global:LASTEXITCODE = 0
  & (Join-Path $DeskflowRoot 'scripts\build-windows.ps1') -Install
  if ($LASTEXITCODE -ne 0) { throw "deskflow build/install failed (exit $LASTEXITCODE)" }
}

function Assert-DeskflowSingle {
  if ($DeployDeskflow -ne 1) { return }
  $ctl = Join-Path $DeskflowRoot 'scripts\deskflow-ctl.ps1'
  if (-not (Test-Path $ctl)) { throw "deskflow-ctl.ps1 missing at $ctl" }
  Write-Host "== [$hostName] deskflow-ctl assert-single =="
  & $ctl assert-single
}

function Sync-MouserRepo {
  Set-Location $MouserRoot
  if (-not (Test-Path (Join-Path $MouserRoot '.git'))) {
    throw "Mouser at $MouserRoot is not a git clone; bootstrap it first (scripts\sync-desktop-repos-tiny11.ps1)."
  }
  if ($env:FLEET_SKIP_GIT_PULL -eq '1') {
    # The controller already synced this checkout (branch, --ref or --rollback).
    Write-Host "== [$hostName] Mouser sync skipped (FLEET_SKIP_GIT_PULL=1) =="
    Invoke-Git log -1 --oneline
    return
  }
  $prev = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $remotes = @(& git remote 2>$null)
  } finally {
    $ErrorActionPreference = $prev
  }
  if ($remotes -notcontains 'fork') {
    Write-Host "adding Mouser remote 'fork' -> $MouserRemoteUrl"
    Invoke-Git remote add fork $MouserRemoteUrl
  }
  $ref = $env:FLEET_MOUSER_REF
  if ($ref -and $ref -ne 'HEAD') {
    Write-Host "== [$hostName] Mouser checkout --detach $ref =="
    Invoke-Git fetch fork
    Invoke-Git checkout --detach $ref
  } elseif (-not $ref) {
    Write-Host "== [$hostName] Mouser pull (fork/$MouserBranch) =="
    Invoke-Git fetch fork
    Invoke-Git checkout $MouserBranch
    Invoke-Git pull --ff-only fork $MouserBranch
  } else {
    Write-Host "== [$hostName] Mouser: building checked-out HEAD =="
  }
  Invoke-Git log -1 --oneline
}

function Deploy-Mouser {
  if ($DeployMouser -ne 1) { return }
  if (-not (Test-Path $MouserRoot)) {
    Write-Host "skip Mouser: missing $MouserRoot"
    return
  }
  Sync-MouserRepo

  # MOUSER_RESTART=1 is scoped to the Mouser step only: the Mouser installer
  # (build_and_install.py) owns Mouser's restart. Nothing in this script stops
  # or kills Mouser.
  Write-Host "== [$hostName] Mouser build + install (MOUSER_RESTART=1) =="
  $prevRestart = $env:MOUSER_RESTART
  $env:MOUSER_RESTART = '1'
  try {
    $bat = Join-Path $env:USERPROFILE 'build-mouser.bat'
    if (Test-Path $bat) {
      Invoke-Native -Label 'Mouser build (build-mouser.bat)' -FilePath 'cmd.exe' -ArgumentList @('/c', $bat)
    } else {
      Set-Location $MouserRoot
      $py = Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
      if (-not $py) { throw 'Python not found for Mouser' }
      $venvPy = Join-Path $MouserRoot '.venv\Scripts\python.exe'
      if (-not (Test-Path (Join-Path $MouserRoot '.venv'))) {
        Invoke-Native -Label 'Mouser venv create' -FilePath $py -ArgumentList @('-m', 'venv', (Join-Path $MouserRoot '.venv'))
        Invoke-Native -Label 'Mouser pip install' -FilePath $venvPy `
          -ArgumentList @('-m', 'pip', 'install', '--quiet', '-r', 'requirements.txt', 'pyinstaller')
      }
      Invoke-Native -Label 'Mouser build_and_install.py' -FilePath $venvPy `
        -ArgumentList @((Join-Path $MouserRoot 'scripts\build_and_install.py'))
    }
  } finally {
    if ($null -eq $prevRestart) { Remove-Item Env:MOUSER_RESTART -ErrorAction SilentlyContinue } else { $env:MOUSER_RESTART = $prevRestart }
  }

  # Sign the PyInstaller output (Mouser.exe + every bundled .dll) with the
  # fleet cert. sign-windows.ps1 throws if the thumbprint/signtool are missing
  # or any file fails verification -- never ship an unsigned Mouser silently.
  $dist = Join-Path $MouserRoot 'dist\Mouser'
  if (-not (Test-Path $dist)) { throw "Mouser build output missing at $dist" }
  & (Join-Path $DeskflowRoot 'scripts\sign-windows.ps1') -Root $dist
}

Write-Host "=== fleet-deploy-windows on $hostName ==="

# Bootstrap helper: sync-desktop-repos-tiny11.ps1 clones Mouser, creates its
# venv and writes ~\build-mouser.bat. It rewrites the .bat unconditionally, so
# only run it when something it provides is actually missing.
$sync = Join-Path $DeskflowRoot 'scripts\sync-desktop-repos-tiny11.ps1'
$needsBootstrap = (-not (Test-Path $MouserRoot)) -or
  (-not (Test-Path (Join-Path $MouserRoot '.venv'))) -or
  (-not (Test-Path (Join-Path $env:USERPROFILE 'build-mouser.bat')))
if ($DeployMouser -eq 1 -and $needsBootstrap -and (Test-Path $sync)) {
  Write-Host "== [$hostName] bootstrapping Mouser layout (sync-desktop-repos-tiny11.ps1) =="
  $global:LASTEXITCODE = 0
  & $sync
  if ($LASTEXITCODE -ne 0) { throw "sync-desktop-repos-tiny11.ps1 failed (exit $LASTEXITCODE)" }
}

if ($DeployDeskflow -eq 1) {
  Sync-DeskflowRepo
  Deploy-Deskflow
}
Deploy-Mouser

# Final gate: exactly one daemon (session 0), one service-owned core and one
# GUI in the console session, all from the canonical install root.
Assert-DeskflowSingle

Write-Host "=== done: $hostName ==="
