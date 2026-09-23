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
# Fleet-deploy context for everything this script invokes that signs
# (build-windows.ps1 -Install, sign-windows.ps1): sign-windows.ps1 refuses
# -AllowNoTimestamp while FLEET_DEPLOY=1, so an untimestamped signature can
# never reach a fleet seat through here.
$env:FLEET_DEPLOY = '1'

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

# --- Mouser settings-survival proof ------------------------------------------
# Three checkpoints over %APPDATA%\Mouser\{config.json,last_device.json}
# (core/config.py CONFIG_DIR on Windows): before the install, right after it
# (must be byte-identical: the installer never touches settings) and after
# Mouser has run for FLEET_SETTINGS_SETTLE_S (default 30) seconds. The last
# one may differ ONLY by an allowed migration: config.json .version strictly
# increased and every pre-deploy key/value (minus version) still present.
# last_device.json is Mouser's HID warm-path cache, rewritten when a device
# reconnects, so after the run it is reported but not judged. Mouser's own
# save_config copies the NEW file to config.json.bak, so the
# config.json.pre-deploy-<ts> copy taken here is the authoritative restore
# point (newest 5 kept).
$script:MouserSettingsFiles = @('config.json', 'last_device.json')

function Get-MouserSettingsDir {
  return (Join-Path $env:APPDATA 'Mouser')
}

function Get-MouserSettingsSnapshot {
  param([string]$Dir)
  $snap = [ordered]@{}
  foreach ($n in $script:MouserSettingsFiles) {
    $f = Join-Path $Dir $n
    if (Test-Path -LiteralPath $f -PathType Leaf) {
      $snap[$n] = (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash.ToLowerInvariant()
    } else {
      $snap[$n] = 'absent'
    }
  }
  return $snap
}

function Read-MouserConfigText {
  param([string]$Dir)
  $f = Join-Path $Dir 'config.json'
  if (Test-Path -LiteralPath $f -PathType Leaf) { return (Get-Content -LiteralPath $f -Raw) }
  return $null
}

function Backup-MouserConfig {
  # config.json -> config.json.pre-deploy-<ts>; prune to the newest $Keep.
  param([string]$Dir, [int]$Keep = 5, [string]$Stamp = '')
  $cfg = Join-Path $Dir 'config.json'
  if (-not (Test-Path -LiteralPath $cfg -PathType Leaf)) { return $null }
  if (-not $Stamp) { $Stamp = Get-Date -Format 'yyyyMMdd-HHmmss' }
  $dest = Join-Path $Dir "config.json.pre-deploy-$Stamp"
  Copy-Item -LiteralPath $cfg -Destination $dest -Force
  $all = @(Get-ChildItem -LiteralPath $Dir -File | Where-Object { $_.Name -like 'config.json.pre-deploy-*' } |
    Sort-Object Name -Descending)
  if ($all.Count -gt $Keep) {
    $all | Select-Object -Skip $Keep | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
  }
  return $dest
}

function Test-JsonSubset {
  # $true when every key/value of $Pre (recursively) is present and equal in $Post.
  param($Pre, $Post)
  if ($Pre -is [System.Management.Automation.PSCustomObject]) {
    if (-not ($Post -is [System.Management.Automation.PSCustomObject])) { return $false }
    foreach ($prop in $Pre.PSObject.Properties) {
      $pp = $Post.PSObject.Properties[$prop.Name]
      if ($null -eq $pp) { return $false }
      if (-not (Test-JsonSubset $prop.Value $pp.Value)) { return $false }
    }
    return $true
  }
  if ($Pre -is [System.Array]) {
    if (-not ($Post -is [System.Array])) { return $false }
    if ($Pre.Count -ne $Post.Count) { return $false }
    for ($i = 0; $i -lt $Pre.Count; $i++) {
      if (-not (Test-JsonSubset $Pre[$i] $Post[$i])) { return $false }
    }
    return $true
  }
  if ($null -eq $Pre) { return ($null -eq $Post) }
  if ($null -eq $Post) { return $false }
  return (("$Pre" -eq "$Post") -and ($Pre.GetType() -eq $Post.GetType()))
}

function Test-MouserConfigMigration {
  # Allowed post-run change: .version strictly increased and del(.version) of
  # the pre-deploy config is a subset of the post-run config.
  param([string]$PreText, [string]$PostText)
  if (-not $PreText -or -not $PostText) { return $false }
  try {
    $pre = $PreText | ConvertFrom-Json
    $post = $PostText | ConvertFrom-Json
    $preNoVersion = $PreText | ConvertFrom-Json
  } catch { return $false }
  $pv = $pre.PSObject.Properties['version']
  $qv = $post.PSObject.Properties['version']
  if ($null -eq $pv -or $null -eq $qv) { return $false }
  if (-not ([int]$qv.Value -gt [int]$pv.Value)) { return $false }
  $preNoVersion.PSObject.Properties.Remove('version')
  return (Test-JsonSubset $preNoVersion $post)
}

function Get-MouserSettingsDiff {
  param($Pre, $Post)
  $diff = @()
  foreach ($n in $Pre.Keys) {
    if ("$($Pre[$n])" -ne "$($Post[$n])") {
      $a = "$($Pre[$n])"; $b = "$($Post[$n])"
      $diff += ('{0} {1} -> {2}' -f $n, $a.Substring(0, [Math]::Min(12, $a.Length)), $b.Substring(0, [Math]::Min(12, $b.Length)))
    }
  }
  return $diff
}

function Assert-MouserSettingsIdentical {
  param($Pre, $Post, [string]$Stage)
  $diff = @(Get-MouserSettingsDiff $Pre $Post)
  if ($diff.Count -gt 0) {
    throw ("Mouser settings changed {0}: {1} (restore from config.json.pre-deploy-<ts>)" -f $Stage, ($diff -join '; '))
  }
}

function Get-MouserSettingsVerdict {
  # After Mouser has run: 'ok' (config.json identical), 'changed' (allowed
  # migration only) or throw with a diff summary.
  param($Pre, $Post, [string]$PreText, [string]$PostText)
  $diff = @(Get-MouserSettingsDiff $Pre $Post)
  $cfgChanged = @($diff | Where-Object { $_ -like 'config.json *' })
  if ($cfgChanged.Count -eq 0) {
    if ($diff.Count -gt 0) { Write-Host ("Mouser cache rewritten while running (not judged): {0}" -f ($diff -join '; ')) }
    return 'ok'
  }
  if (Test-MouserConfigMigration -PreText $PreText -PostText $PostText) {
    Write-Host ("Mouser config.json migrated (version increased, pre-deploy keys preserved): {0}" -f ($cfgChanged -join '; '))
    return 'changed'
  }
  throw ("Mouser settings changed after 30 s of running and it is not an allowed migration (version must strictly increase and every pre-deploy key survive): {0} (restore from config.json.pre-deploy-<ts>)" -f ($diff -join '; '))
}

function Deploy-Mouser {
  if ($DeployMouser -ne 1) { return }
  if (-not (Test-Path $MouserRoot)) {
    Write-Host "skip Mouser: missing $MouserRoot"
    return
  }
  Sync-MouserRepo

  # Checkpoint 1: settings before anything is touched, plus the restore copy.
  $settingsDir = Get-MouserSettingsDir
  $preSnap = Get-MouserSettingsSnapshot $settingsDir
  $preText = Read-MouserConfigText $settingsDir
  $backup = Backup-MouserConfig -Dir $settingsDir
  Write-Host ("== [{0}] Mouser settings snapshot (pre-deploy): config.json={1} last_device.json={2} backup={3} ==" -f `
    $hostName, $preSnap['config.json'], $preSnap['last_device.json'], $(if ($backup) { $backup } else { 'none' }))

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
      # Highest installed 3.x wins (Python313 before Python312); the venv must match
      # Mouser's .python-version or build_and_install.py's provenance gate refuses it.
      $py = Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
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

  # Checkpoint 2: the installer must not have touched settings at all.
  try {
    Assert-MouserSettingsIdentical $preSnap (Get-MouserSettingsSnapshot $settingsDir) 'by the install'
  } catch {
    Write-Host 'FLEET_SETTINGS=FAIL'
    throw
  }
  Write-Host "== [$hostName] Mouser settings identical after install =="

  # Sign the PyInstaller output (Mouser.exe + every bundled .dll) with the
  # fleet cert. sign-windows.ps1 throws if the thumbprint/signtool are missing
  # or any file fails verification -- never ship an unsigned Mouser silently.
  $dist = Join-Path $MouserRoot 'dist\Mouser'
  if (-not (Test-Path $dist)) { throw "Mouser build output missing at $dist" }
  & (Join-Path $DeskflowRoot 'scripts\sign-windows.ps1') -Root $dist

  # Checkpoint 3: after Mouser has run. Only an allowed migration may differ.
  $settle = if ($env:FLEET_SETTINGS_SETTLE_S) { [int]$env:FLEET_SETTINGS_SETTLE_S } else { 30 }
  Write-Host "== [$hostName] Mouser settings: waiting $settle s of Mouser running =="
  Start-Sleep -Seconds $settle
  $verdict = 'FAIL'
  try {
    $verdict = Get-MouserSettingsVerdict -Pre $preSnap -Post (Get-MouserSettingsSnapshot $settingsDir) `
      -PreText $preText -PostText (Read-MouserConfigText $settingsDir)
  } catch {
    Write-Host 'FLEET_SETTINGS=FAIL'
    throw
  }
  Write-Host "FLEET_SETTINGS=$verdict"
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
