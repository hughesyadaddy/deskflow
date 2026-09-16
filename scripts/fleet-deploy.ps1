# Fleet deploy controller (Windows seat) — same CLI as scripts/fleet-deploy.sh.
#requires -Version 5.1
#
#   .\scripts\fleet-deploy.ps1 [--dry-run] [--json PATH|-] [--self-test] [--ref REF]
#                              [--rollback] [--host ID] [--app deskflow|mouser]
#                              [--deskflow-only] [--mouser-only] [--reconfigure] [--pull-only]
#
# Every FLEET_HOSTS entry is targeted exactly once: the seat whose id matches
# $env:COMPUTERNAME (case-insensitive; FLEET_LOCAL_ID overrides) runs
# scripts\fleet-deploy-windows.ps1 directly, remote Macs get
# `ssh user@host 'cd … && git … && bash scripts/fleet-deploy-macos.sh'`,
# remote Windows seats get ssh + powershell. Clients first, server
# (FLEET_ROLE_<id>=server, default hackintosh) last. Lock = New-Item on
# tools\state\deploy.lock.d (same path as the bash controller's mkdir lock).
# Dot-source the file to load its functions without running (Pester).
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$script:FleetRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# --- native seams (Pester mocks these) -------------------------------------
function Get-FleetHostname {
  if ($env:COMPUTERNAME) { return $env:COMPUTERNAME }
  return (& hostname)
}
function Invoke-Ssh {
  param([string]$Target, [string]$Command)
  $out = & ssh -o BatchMode=yes $Target $Command 2>&1
  $rc = $LASTEXITCODE
  return @{ Code = $rc; Output = ($out | Out-String) }
}
function Invoke-Native {
  # Runs one native executable in a directory; the exit code is read from $LASTEXITCODE.
  param([string]$Exe, [string[]]$ArgList, [string]$WorkingDirectory)
  Push-Location $WorkingDirectory
  try {
    $out = & $Exe @ArgList 2>&1
    $rc = $LASTEXITCODE
  } finally { Pop-Location }
  return @{ Code = $rc; Output = ($out | Out-String) }
}
$script:HealthJson = ''
$script:LastJson = ''
function Test-ProcessAlive { param([int]$ProcessId) return ($null -ne (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) }

# --- config ----------------------------------------------------------------
function Read-FleetEnv {
  param([string]$Path)
  if (-not (Test-Path $Path)) { throw "Missing $Path - copy from fleet.env.example:`n  Copy-Item scripts\fleet.env.example scripts\fleet.env" }
  $envMap = @{}
  foreach ($line in Get-Content $Path) {
    $t = $line.Trim()
    if (-not $t -or $t.StartsWith('#')) { continue }
    $eq = $t.IndexOf('=')
    if ($eq -lt 1) { continue }
    $k = $t.Substring(0, $eq).Trim()
    $v = $t.Substring($eq + 1).Trim()
    if ($v.Length -ge 2 -and (($v[0] -eq '"' -and $v[-1] -eq '"') -or ($v[0] -eq "'" -and $v[-1] -eq "'"))) { $v = $v.Substring(1, $v.Length - 2) }
    $envMap[$k] = $v
  }
  if (-not $envMap.ContainsKey('FLEET_BRANCH')) { $envMap['FLEET_BRANCH'] = 'main' }
  if (-not $envMap.ContainsKey('FLEET_HOSTS')) { $envMap['FLEET_HOSTS'] = 'hackintosh macbookpro tiny11' }
  return $envMap
}
function Get-EnvValue { param($Map, [string]$Key, $Default = '') if ($Map.ContainsKey($Key) -and $Map[$Key] -ne '') { return $Map[$Key] } return $Default }
function Get-FleetHostIds { param($Map) return @(($Map['FLEET_HOSTS'] -split '\s+') | Where-Object { $_ }) }

function Get-LocalId {
  param($Map)
  $hn = (Get-FleetHostname).ToLowerInvariant()
  $want = if ($env:FLEET_LOCAL_ID) { $env:FLEET_LOCAL_ID.ToLowerInvariant() } else { $hn }
  foreach ($id in Get-FleetHostIds $Map) { if ($id.ToLowerInvariant() -eq $want) { return $id } }
  throw "no FLEET_HOSTS entry matches this seat (hostname '$hn'; set FLEET_LOCAL_ID to override)"
}

function Get-FleetPlan {
  param($Map, [string]$LocalId)
  $ids = Get-FleetHostIds $Map
  $server = $null
  foreach ($id in $ids) { if ((Get-EnvValue $Map "FLEET_ROLE_$id" 'client').ToLowerInvariant() -eq 'server') { $server = $id; break } }
  if (-not $server) { foreach ($id in $ids) { if ($id.ToLowerInvariant() -eq 'hackintosh') { $server = $id; break } } }
  $seen = @{}; $ordered = @()
  foreach ($id in $ids) {
    $lc = $id.ToLowerInvariant()
    if ($seen.ContainsKey($lc)) { Write-Warning "duplicate host '$id' in FLEET_HOSTS ignored"; continue }
    $seen[$lc] = $true
    if ($id -ne $server) { $ordered += $id }
  }
  if ($server) { $ordered += $server }
  if ($ordered.Count -eq 0) { throw 'FLEET_HOSTS is empty' }
  $plan = @(); $n = 0
  foreach ($id in $ordered) {
    $os = (Get-EnvValue $Map "FLEET_OS_$id" $(if ($id.ToLowerInvariant() -eq 'tiny11') { 'windows' } else { 'macos' })).ToLowerInvariant()
    $ssh = Get-EnvValue $Map "FLEET_SSH_$id" ''
    if (-not $ssh -or $ssh -eq 'local') { $ssh = $id }   # "local" never defines locality
    $user = Get-EnvValue $Map "FLEET_SSH_USER_$id" $(if ($os -eq 'windows') { 'alexh' } else { 'alexhughes' })
    $n++
    $plan += [pscustomobject]@{
      id = $id; order = $n; os = $os
      role = $(if ($id -eq $server) { 'server' } else { 'client' })
      target = $(if ($id -eq $LocalId) { 'local' } else { "$user@$ssh" })
      deskflowPath = $(if ($os -eq 'windows') { Get-EnvValue $Map 'FLEET_DESKFLOW_PATH_windows' 'C:/Users/alexh/Desktop/deskflow' } else { Get-EnvValue $Map 'FLEET_DESKFLOW_PATH_macos' '~/Desktop/deskflow' })
      mouserPath = $(if ($os -eq 'windows') { Get-EnvValue $Map 'FLEET_MOUSER_PATH_windows' 'C:/Users/alexh/Desktop/Mouser' } else { Get-EnvValue $Map 'FLEET_MOUSER_PATH_macos' '~/Desktop/Mouser' })
    }
  }
  return $plan
}

# --- lock ------------------------------------------------------------------
function Enter-FleetLock {
  param([string]$StateDir)
  $lockDir = Join-Path $StateDir 'deploy.lock.d'
  if (-not (Test-Path $StateDir)) { New-Item -ItemType Directory -Path $StateDir | Out-Null }
  try {
    New-Item -ItemType Directory -Path $lockDir -ErrorAction Stop | Out-Null
  } catch {
    $pidFile = Join-Path $lockDir 'pid'
    $heldBy = if (Test-Path $pidFile) { (Get-Content $pidFile -First 1).Trim() } else { '' }
    if ($heldBy -and -not (Test-ProcessAlive ([int]$heldBy))) {
      Write-Warning "reclaiming stale deploy lock left by pid $heldBy"
      Remove-Item -Recurse -Force $lockDir
      New-Item -ItemType Directory -Path $lockDir -ErrorAction Stop | Out-Null
    } else {
      throw "deploy lock held ($lockDir$(if ($heldBy) { ", pid $heldBy" })) - another fleet-deploy is running; refusing"
    }
  }
  Set-Content -Path (Join-Path $lockDir 'pid') -Value $PID
  return $lockDir
}
function Exit-FleetLock { param([string]$LockDir) if ($LockDir -and (Test-Path $LockDir)) { Remove-Item -Recurse -Force $LockDir } }

# --- last-good -------------------------------------------------------------
function Read-LastGood {
  param([string]$Path)
  $map = @{}
  if (-not (Test-Path $Path)) { return $map }
  $obj = Get-Content $Path -Raw | ConvertFrom-Json
  foreach ($h in $obj.PSObject.Properties) {
    $apps = @{}
    foreach ($a in $h.Value.PSObject.Properties) { $apps[$a.Name] = @{ commit = $a.Value.commit; ts = $a.Value.ts } }
    $map[$h.Name] = $apps
  }
  return $map
}
function Write-LastGood {
  param([string]$Path, [string]$HostId, [string]$App, [string]$Commit)
  $map = Read-LastGood $Path
  if (-not $map.ContainsKey($HostId)) { $map[$HostId] = @{} }
  $map[$HostId][$App] = @{ commit = $Commit; ts = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') }
  $dir = Split-Path -Parent $Path
  if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
  Set-Content -Path $Path -Value ($map | ConvertTo-Json -Depth 5) -Encoding UTF8
}

# --- command builders ------------------------------------------------------
function ConvertTo-ShPath { param([string]$Path) if ($Path.StartsWith('~')) { return '$HOME' + $Path.Substring(1) } return $Path }
function Get-ShGitSync {
  param([string]$Path, [string]$Ref, [string]$Branch)
  $p = ConvertTo-ShPath $Path
  if (-not $Ref) { return "cd `"$p`" && git fetch origin && git checkout `"$Branch`" && git pull --ff-only origin `"$Branch`"" }
  if ($Ref -eq 'HEAD') { return "cd `"$p`"" }
  return "cd `"$p`" && git fetch origin && git checkout --detach `"$Ref`""
}
function Get-ShMouserSync {
  param([string]$Path, [string]$Ref)
  if (-not $Ref -or $Ref -eq 'HEAD') { return '' }
  $p = ConvertTo-ShPath $Path
  return " && if [ -d `"$p/.git`" ]; then git -C `"$p`" fetch fork && git -C `"$p`" checkout --detach `"$Ref`"; fi"
}
function Get-ShExports {
  param($Opt, [string]$DeskflowPath, [string]$MouserPath, [string]$DRef, [string]$MRef)
  $d = ConvertTo-ShPath $DeskflowPath; $m = ConvertTo-ShPath $MouserPath
  return "export FLEET_BRANCH=`"$($Opt.Branch)`" FLEET_DEPLOY_DESKFLOW=`"$($Opt.DeployDeskflow)`" FLEET_DEPLOY_MOUSER=`"$($Opt.DeployMouser)`" FLEET_RECONFIGURE=`"$($Opt.Reconfigure)`" FLEET_DESKFLOW_ROOT=`"$d`" FLEET_MOUSER_ROOT=`"$m`" FLEET_SKIP_GIT_PULL=1 FLEET_DESKFLOW_REF=`"$DRef`" FLEET_MOUSER_REF=`"$MRef`""
}
function Get-CmdGitSync {
  # cmd.exe fragment for Windows seats (local via cmd, remote via ssh's default shell).
  param([string]$Ref, [string]$Branch)
  if (-not $Ref) { return "git fetch origin && git checkout $Branch && git pull --ff-only origin $Branch && " }
  if ($Ref -eq 'HEAD') { return '' }
  return "git fetch origin && git checkout --detach $Ref && "
}
function Get-MacRemoteCommand {
  param($Opt, $Entry, [string]$DRef, [string]$MRef)
  $prelude = 'set -euo pipefail; if [ -x /opt/homebrew/bin/brew ]; then eval "$(/opt/homebrew/bin/brew shellenv)"; elif [ -x /usr/local/bin/brew ]; then eval "$(/usr/local/bin/brew shellenv)"; else export PATH="/opt/homebrew/bin:/usr/local/bin:${PATH}"; fi; '
  $sync = (Get-ShGitSync $Entry.deskflowPath $DRef $Opt.Branch) + (Get-ShMouserSync $Entry.mouserPath $MRef)
  if ($Opt.PullOnly) { return "$prelude$sync && git log -1 --oneline" }
  return "$prelude$(Get-ShExports $Opt $Entry.deskflowPath $Entry.mouserPath $DRef $MRef); $sync && bash scripts/fleet-deploy-macos.sh"
}
function Get-WinRemoteCommand {
  param($Opt, $Entry, [string]$DRef, [string]$MRef)
  $d = $Entry.deskflowPath; $m = $Entry.mouserPath
  if ($Opt.PullOnly) { return "cd /d `"$d`" && $(Get-CmdGitSync $DRef $Opt.Branch)git log -1 --oneline" }
  $ps = "`$ErrorActionPreference='Stop'; `$env:FLEET_BRANCH='$($Opt.Branch)'; `$env:FLEET_DEPLOY_DESKFLOW='$($Opt.DeployDeskflow)'; `$env:FLEET_DEPLOY_MOUSER='$($Opt.DeployMouser)'; " +
        "`$env:FLEET_DESKFLOW_ROOT='$d'; `$env:FLEET_MOUSER_ROOT='$m'; `$env:FLEET_SKIP_GIT_PULL='1'; `$env:FLEET_DESKFLOW_REF='$DRef'; `$env:FLEET_MOUSER_REF='$MRef'; Set-Location '$d'; "
  if (-not $DRef) { $ps += "git fetch origin; if (`$LASTEXITCODE) { exit `$LASTEXITCODE }; git checkout '$($Opt.Branch)'; if (`$LASTEXITCODE) { exit `$LASTEXITCODE }; git pull --ff-only origin '$($Opt.Branch)'; if (`$LASTEXITCODE) { exit `$LASTEXITCODE }; " }
  elseif ($DRef -ne 'HEAD') { $ps += "git fetch origin; if (`$LASTEXITCODE) { exit `$LASTEXITCODE }; git checkout --detach '$DRef'; if (`$LASTEXITCODE) { exit `$LASTEXITCODE }; " }
  $ps += "& '$d/scripts/fleet-deploy-windows.ps1'; exit `$LASTEXITCODE"
  return "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command `"$ps`""
}
function Get-LocalWinSteps {
  # Steps for this Windows seat: [exe, args...] each run via Invoke-Native in the deskflow root.
  param($Opt, $Entry, [string]$DRef, [string]$MRef)
  $steps = @()
  if (-not $DRef) {
    $steps += , @('git', 'fetch', 'origin'); $steps += , @('git', 'checkout', $Opt.Branch); $steps += , @('git', 'pull', '--ff-only', 'origin', $Opt.Branch)
  } elseif ($DRef -ne 'HEAD') {
    $steps += , @('git', 'fetch', 'origin'); $steps += , @('git', 'checkout', '--detach', $DRef)
  }
  if ($Opt.PullOnly) { $steps += , @('git', 'log', '-1', '--oneline'); return $steps }
  $steps += , @('powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$($Entry.deskflowPath)/scripts/fleet-deploy-windows.ps1")
  return $steps
}
function Invoke-LocalWinDeploy {
  param($Opt, $Entry, [string]$DRef, [string]$MRef)
  $saved = @{}
  $vars = @{ FLEET_BRANCH = $Opt.Branch; FLEET_DEPLOY_DESKFLOW = "$($Opt.DeployDeskflow)"; FLEET_DEPLOY_MOUSER = "$($Opt.DeployMouser)"; FLEET_RECONFIGURE = "$($Opt.Reconfigure)"
              FLEET_DESKFLOW_ROOT = $Entry.deskflowPath; FLEET_MOUSER_ROOT = $Entry.mouserPath; FLEET_SKIP_GIT_PULL = '1'; FLEET_DESKFLOW_REF = $DRef; FLEET_MOUSER_REF = $MRef }
  foreach ($k in $vars.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $vars[$k]) }
  try {
    foreach ($step in Get-LocalWinSteps $Opt $Entry $DRef $MRef) {
      $r = Invoke-Native $step[0] @($step | Select-Object -Skip 1) $Entry.deskflowPath
      Write-Host $r.Output
      if ($r.Code -ne 0) { return $r.Code }
    }
    return 0
  } finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
  }
}

# --- per-host --------------------------------------------------------------
function Get-HeadCommit {
  param($Entry, [string]$Path)
  if ($Entry.target -eq 'local') {
    $r = Invoke-Native 'git' @('rev-parse', 'HEAD') $Path
  } else {
    $r = Invoke-Ssh $Entry.target "git -C `"$(ConvertTo-ShPath $Path)`" rev-parse HEAD"
  }
  if ($r.Code -ne 0) { return 'unknown' }
  $lines = @($r.Output -split "`r?`n" | Where-Object { $_.Trim() })
  if ($lines.Count -eq 0) { return 'unknown' }
  return $lines[-1].Trim()
}
function Invoke-FleetHealth {
  # Returns the exit code of tools\fleet-health.ps1, or -1 when it is not installed. Output lands in $script:HealthJson.
  param([string[]]$HealthArgs)
  $ps1 = Join-Path $script:FleetRoot 'tools\fleet-health.ps1'
  if (-not (Test-Path $ps1)) { return -1 }
  $script:HealthJson = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ps1 @HealthArgs 2>&1 | Out-String)
  return $LASTEXITCODE
}
function Invoke-HostDeploy {
  param($Opt, $Entry, [string]$DRef, [string]$MRef)
  if ($Entry.target -eq 'local') {
    if ($Entry.os -ne 'windows') { throw "local seat '$($Entry.id)' is $($Entry.os) - run scripts/fleet-deploy.sh there" }
    Write-Host ">>> LOCAL deploy: $($Entry.id)"
    $rc = Invoke-LocalWinDeploy $Opt $Entry $DRef $MRef
    if ($rc -ne 0) { return "fail($rc)" }
    return 'ok'
  }
  Write-Host ">>> SSH deploy: $($Entry.id) ($($Entry.target))"
  $cmd = if ($Entry.os -eq 'windows') { Get-WinRemoteCommand $Opt $Entry $DRef $MRef } else { Get-MacRemoteCommand $Opt $Entry $DRef $MRef }
  $r = Invoke-Ssh $Entry.target $cmd
  Write-Host $r.Output
  if ($r.Code -eq 255) { return 'unreachable(ssh 255)' }
  if ($r.Code -ne 0) { return "fail($($r.Code))" }
  return 'ok'
}

# --- CLI -------------------------------------------------------------------
function ConvertTo-FleetOptions {
  param([string[]]$CliArgs)
  if ($null -eq $CliArgs) { $CliArgs = @() }
  $o = @{ DryRun = $false; Json = ''; SelfTest = $false; Ref = ''; Rollback = $false; Host = ''; PullOnly = $false; DeployDeskflow = $null; DeployMouser = $null; Reconfigure = $null; Help = $false }
  $i = 0
  while ($i -lt $CliArgs.Count) {
    $a = $CliArgs[$i]
    switch ($a) {
      '--dry-run' { $o.DryRun = $true }
      '--json' { $i++; if ($i -ge $CliArgs.Count) { throw '--json needs PATH or -' }; $o.Json = $CliArgs[$i] }
      '--self-test' { $o.SelfTest = $true }
      '--ref' { $i++; if ($i -ge $CliArgs.Count) { throw '--ref needs REF' }; $o.Ref = $CliArgs[$i] }
      '--rollback' { $o.Rollback = $true }
      '--host' { $i++; if ($i -ge $CliArgs.Count) { throw '--host needs ID' }; $o.Host = $CliArgs[$i].ToLowerInvariant() }
      '--app' {
        $i++; if ($i -ge $CliArgs.Count) { throw '--app needs deskflow|mouser' }
        switch ($CliArgs[$i]) { 'deskflow' { $o.DeployMouser = 0 } 'mouser' { $o.DeployDeskflow = 0 } default { throw "--app must be deskflow or mouser (got '$($CliArgs[$i])')" } }
      }
      '--deskflow-only' { $o.DeployMouser = 0 }
      '--mouser-only' { $o.DeployDeskflow = 0 }
      '--reconfigure' { $o.Reconfigure = 1 }
      '--pull-only' { $o.PullOnly = $true }
      { $_ -in '-h', '--help' } { $o.Help = $true }
      default { throw "unknown option: $a" }
    }
    $i++
  }
  if ($o.SelfTest -and $o.Rollback) { throw '--self-test and --rollback are exclusive' }
  if ($o.Rollback -and $o.Ref) { throw '--rollback picks its own commits; drop --ref' }
  if ($o.SelfTest -and -not $o.Ref) { $o.Ref = 'HEAD' }
  return $o
}

function Write-FleetJson {
  param([string]$Target, $Object)
  if (-not $Target) { return }
  $json = $Object | ConvertTo-Json -Depth 6 -Compress
  $script:LastJson = $json
  if ($Target -eq '-') { [Console]::Out.WriteLine($json); return }
  $dir = Split-Path -Parent $Target
  if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
  Set-Content -Path $Target -Value $json -Encoding UTF8
}

function Invoke-FleetDeploy {
  param([string[]]$CliArgs)
  $opt = ConvertTo-FleetOptions $CliArgs
  if ($opt.Help) { Get-Content $PSCommandPath | Select-Object -Skip 1 -First 15 | ForEach-Object { $_ -replace '^#\s?', '' }; return 0 }
  $root = $script:FleetRoot
  $envFile = if ($env:FLEET_ENV_FILE) { $env:FLEET_ENV_FILE } else { Join-Path $root 'scripts\fleet.env' }
  $stateDir = Join-Path $root 'tools\state'
  $lastGood = Join-Path $stateDir 'last-good.json'
  $map = Read-FleetEnv $envFile
  $opt.Branch = $map['FLEET_BRANCH']
  if ($null -eq $opt.DeployDeskflow) { $opt.DeployDeskflow = [int](Get-EnvValue $map 'FLEET_DEPLOY_DESKFLOW' 1) }
  if ($null -eq $opt.DeployMouser) { $opt.DeployMouser = [int](Get-EnvValue $map 'FLEET_DEPLOY_MOUSER' 1) }
  if ($null -eq $opt.Reconfigure) { $opt.Reconfigure = [int](Get-EnvValue $map 'FLEET_RECONFIGURE' 0) }
  $apps = @(); if ($opt.DeployDeskflow -eq 1) { $apps += 'deskflow' }; if ($opt.DeployMouser -eq 1) { $apps += 'mouser' }
  if ($apps.Count -eq 0) { throw 'nothing to deploy: both FLEET_DEPLOY_DESKFLOW and FLEET_DEPLOY_MOUSER are 0' }

  $localId = Get-LocalId $map
  $plan = @(Get-FleetPlan $map $localId)
  if ($opt.Host) {
    if (-not ($plan | Where-Object { $_.id.ToLowerInvariant() -eq $opt.Host })) { throw "--host '$($opt.Host)' is not in FLEET_HOSTS ($($map['FLEET_HOSTS']))" }
    $plan = @($plan | Where-Object { $_.id.ToLowerInvariant() -eq $opt.Host })
  }

  if ($opt.DryRun) {
    if ($opt.Json) {
      Write-FleetJson $opt.Json @{ hosts = @($plan | ForEach-Object { [ordered]@{ id = $_.id; target = $_.target; order = $_.order; role = $_.role; os = $_.os } }) }
    } else {
      Write-Host "plan (local=$localId, branch=$($opt.Branch)$(if ($opt.Ref) { ", ref=$($opt.Ref)" })):"
      foreach ($e in $plan) { Write-Host ('  {0}. {1,-12} {2,-8} {3,-7} {4}' -f $e.order, $e.id, $e.role, $e.os, $e.target) }
    }
    return 0
  }

  if ($opt.Rollback -and -not (Test-Path $lastGood)) { throw "--rollback: $lastGood does not exist (no successful deploy recorded yet)" }
  $lock = Enter-FleetLock $stateDir
  $rows = @(); $allOk = $true
  try {
    foreach ($e in $plan) {
      $dref = $opt.Ref; $mref = $opt.Ref
      if ($opt.Rollback) {
        $lg = Read-LastGood $lastGood
        foreach ($app in $apps) {
          if (-not ($lg.ContainsKey($e.id) -and $lg[$e.id].ContainsKey($app) -and $lg[$e.id][$app].commit)) { throw "--rollback: no last-good commit for $($e.id)/$app in $lastGood" }
          if ($app -eq 'deskflow') { $dref = $lg[$e.id][$app].commit } else { $mref = $lg[$e.id][$app].commit }
        }
        Write-Host ">>> rollback $($e.id): deskflow=$dref mouser=$mref"
      }
      $result = Invoke-HostDeploy $opt $e $dref $mref
      $healthy = $false
      if ($result -eq 'ok' -and -not $opt.PullOnly) {
        $hc = Invoke-FleetHealth @('--host', $e.id)
        if ($hc -eq 0 -or $hc -eq -1) { $healthy = $true } else { $result = 'unhealthy' }
      }
      if ($result -ne 'ok') { $allOk = $false }
      foreach ($app in $apps) {
        $commit = '-'
        if ($result -eq 'ok' -or $result -eq 'unhealthy') { $commit = Get-HeadCommit $e $(if ($app -eq 'deskflow') { $e.deskflowPath } else { $e.mouserPath }) }
        if ($healthy -and $commit -ne 'unknown' -and $commit -ne '-') { Write-LastGood $lastGood $e.id $app $commit }
        $rows += [ordered]@{ id = $e.id; target = $e.target; app = $app; commit = $commit; signedBy = '-'; tcc = '-'; mesh = '-'; result = $result }
      }
    }

    if ($opt.SelfTest) {
      $hc = Invoke-FleetHealth @('--check', 'all', '--host', 'all', '--json')
      if ($hc -eq -1) { Write-Warning 'tools\fleet-health.ps1 not found - self-test cannot verify health'; $allOk = $false }
      else {
        if ($hc -ne 0) { $allOk = $false; Write-Warning 'fleet-health --check all reported failures' }
        $hj = $null
        try { $hj = $script:HealthJson | ConvertFrom-Json } catch { $hj = $null }
        if ($hj) {
          foreach ($row in $rows) {
            $h = $null
            if ($hj.PSObject.Properties['hosts']) { $h = $hj.hosts | Where-Object { ($_.id -eq $row.id) -or ($_.PSObject.Properties['host'] -and $_.host -eq $row.id) } | Select-Object -First 1 }
            elseif ($hj.PSObject.Properties[$row.id]) { $h = $hj.($row.id) }
            if ($null -eq $h) { continue }
            foreach ($k in 'signedBy', 'tcc', 'mesh') { if ($h.PSObject.Properties[$k]) { $row[$k] = [string]$h.$k } }
            if ($h.PSObject.Properties['ok'] -and -not $h.ok) { $allOk = $false; if ($row.result -eq 'ok') { $row.result = 'unhealthy' } }
          }
        }
      }
    }
  } finally {
    Exit-FleetLock $lock
  }

  Write-Host ''
  Write-Host ('{0,-12} | {1,-8} | {2,-12} | {3,-24} | {4,-6} | {5,-6} | {6}' -f 'host', 'app', 'commit', 'signed-by', 'tcc', 'mesh', 'result')
  Write-Host ('{0}-+-{1}-+-{2}-+-{3}-+-{4}-+-{5}-+-{6}' -f ('-' * 12), ('-' * 8), ('-' * 12), ('-' * 24), ('-' * 6), ('-' * 6), ('-' * 6))
  foreach ($row in $rows) {
    $c = [string]$row.commit; if ($c.Length -gt 12) { $c = $c.Substring(0, 12) }
    $s = [string]$row.signedBy; if ($s.Length -gt 24) { $s = $s.Substring(0, 24) }
    Write-Host ('{0,-12} | {1,-8} | {2,-12} | {3,-24} | {4,-6} | {5,-6} | {6}' -f $row.id, $row.app, $c, $s, $row.tcc, $row.mesh, $row.result)
  }
  Write-FleetJson $opt.Json ([ordered]@{ ok = $allOk; hosts = $rows })
  if ($allOk) { Write-Host '=== Fleet deploy complete ==='; return 0 }
  Write-Host '=== Fleet deploy FAILED on at least one host ==='
  return 1
}

if ($MyInvocation.InvocationName -ne '.') {
  try {
    $code = @(Invoke-FleetDeploy @($args))[-1]
  } catch {
    Write-Host "fleet-deploy: error: $($_.Exception.Message)"
    $code = 1
  }
  exit $code
}
