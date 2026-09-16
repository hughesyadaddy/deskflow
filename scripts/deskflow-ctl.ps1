#requires -Version 5.1
<#
.SYNOPSIS
  deskflow-ctl — the ONE way to stop/start/restart Deskflow on Windows.

.DESCRIPTION
  Verbs:
    stop           Stop-Service Deskflow (poll <=10 s), taskkill the service PID
                   if the SCM hangs, then taskkill /F /T every process whose
                   ExecutablePath is under the install root (ALL sessions).
                   Loops <=25 s; throws if anything remains. Never touches Mouser.
    start          sign (scripts/sign-windows.ps1) -> sc create/config ->
                   Start-Service -> wait <=15 s for a deskflow-core.exe whose
                   ParentProcessId is the service PID -> launch the GUI into the
                   interactive console session via a scheduled task.
    restart        stop then start.
    status         inventory: service state/PID + every Deskflow process with
                   path, session, parent — prints, never throws on findings.
    assert-single  exactly { deskflow-daemon.exe: 1 in session 0 (== service PID),
                   deskflow-core.exe: 1 with parent == service PID in the console
                   session, deskflow.exe: 1 in the console session,
                   deskflow-vhid-bridge.exe: 0 }, all under the install root.
                   Throws (exit 1) on any mismatch.

  Elevation: interactive sessions self-elevate (RunAs). Over SSH (no
  interactive desktop) the caller must already be elevated — `whoami /groups`
  must show "High Mandatory Level" — otherwise the script throws.

  Nothing here stops, starts or kills Mouser: Mouser is an independent app
  owned by its own installer.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deskflow-ctl.ps1 stop
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deskflow-ctl.ps1 assert-single
#>
[CmdletBinding()]
param(
  [Parameter(Position = 0)]
  [ValidateSet('stop', 'start', 'restart', 'status', 'assert-single')]
  [string]$Verb,
  [string]$InstallDir,
  [string]$ServiceName = 'Deskflow',
  [switch]$NoGui,
  [switch]$NoSign,
  [int]$StopTimeoutSec = 25,
  [int]$StartTimeoutSec = 15
)

$ErrorActionPreference = 'Stop'
$script:Root = Split-Path $PSScriptRoot -Parent

# ------------------------------------------------------------------ helpers

function Resolve-InstallDir {
  param([string]$Explicit)
  if ($Explicit) { return [System.IO.Path]::GetFullPath($Explicit) }
  if ($env:DESKFLOW_INSTALL_DIR) { return [System.IO.Path]::GetFullPath($env:DESKFLOW_INSTALL_DIR) }
  return [System.IO.Path]::GetFullPath((Join-Path ${env:ProgramFiles} 'Deskflow'))
}

function Test-IsAdmin {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = New-Object Security.Principal.WindowsPrincipal($identity)
  return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-HighIntegrity {
  # `whoami /groups` lists the token's integrity label; an elevated token
  # carries "High Mandatory Level" (or System). UAC-filtered admin tokens do not.
  $groups = & whoami.exe /groups 2>$null | Out-String
  return ($groups -match 'High Mandatory Level' -or $groups -match 'System Mandatory Level')
}

function Test-InteractiveDesktop {
  # SSH / service contexts have no interactive window station; RunAs cannot
  # prompt there. [Environment]::UserInteractive is false for services, and
  # SSH sessions carry SSH_CONNECTION.
  if ($env:SSH_CONNECTION -or $env:SSH_CLIENT -or $env:SSH_TTY) { return $false }
  return [Environment]::UserInteractive
}

function Assert-Elevated {
  param([string[]]$ForwardArgs)
  if ((Test-IsAdmin) -and (Test-HighIntegrity)) { return }
  if (-not (Test-InteractiveDesktop)) {
    throw "deskflow-ctl $Verb needs an elevated token (whoami /groups must show 'High Mandatory Level'); over SSH run from an elevated shell — RunAs cannot prompt here."
  }
  Write-Host "Re-launching elevated for deskflow-ctl $Verb..."
  $scriptPath = if ($PSCommandPath) { $PSCommandPath } else { $MyInvocation.MyCommand.Path }
  $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $scriptPath) + $ForwardArgs
  $proc = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argList -PassThru -Wait
  exit $proc.ExitCode
}

function Invoke-TaskKillPid {
  param([int]$ProcessId)
  # /T tree-kills children; elevated /F reaches SYSTEM (session 0). taskkill
  # writes to stderr on a missing PID; must not trip $ErrorActionPreference.
  cmd.exe /c "taskkill /F /T /PID $ProcessId >nul 2>&1"
}

function Get-DeskflowService {
  Get-CimInstance Win32_Service -Filter "Name='$ServiceName'" -ErrorAction SilentlyContinue
}

function Get-DeskflowProcesses {
  # Every deskflow*.exe in every session, by executable path. Name-based
  # discovery only; ownership decisions are made on ExecutablePath below.
  @(Get-CimInstance Win32_Process -Filter "Name LIKE 'deskflow%'" -ErrorAction SilentlyContinue)
}

function Test-UnderRoot {
  param([string]$Path, [string]$RootDir)
  if (-not $Path) { return $false }
  $p = [System.IO.Path]::GetFullPath($Path).TrimEnd('\').ToLowerInvariant()
  $r = [System.IO.Path]::GetFullPath($RootDir).TrimEnd('\').ToLowerInvariant()
  return $p.StartsWith($r + '\')
}

function Get-ConsoleSessionId {
  # The session that owns explorer.exe is the interactive console session.
  $explorer = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue |
    Sort-Object SessionId | Select-Object -First 1
  if (-not $explorer) { return $null }
  return [int]$explorer.SessionId
}

function Get-InteractiveSession {
  # @{ SessionId; User } for the console session, or $null when nobody is logged in.
  $explorer = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue |
    Sort-Object SessionId | Select-Object -First 1
  if (-not $explorer) { return $null }
  $owner = Invoke-CimMethod -InputObject $explorer -MethodName GetOwner -ErrorAction SilentlyContinue
  if (-not $owner -or -not $owner.User) { return $null }
  $user = if ($owner.Domain) { "$($owner.Domain)\$($owner.User)" } else { $owner.User }
  [pscustomobject]@{ SessionId = [int]$explorer.SessionId; User = $user }
}

function Start-GuiInSession {
  # Start-Process from a session-0 (service/SSH) context lands the GUI in
  # session 0, invisible to the user and prone to spawning a duplicate core.
  # An interactive scheduled task launches into the user's console session.
  param([string]$Gui, [string]$WorkDir, [string]$User)
  $taskName = 'DeskflowCtlLaunch'
  Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
  # No --show: restarts go silently to the tray.
  $action = New-ScheduledTaskAction -Execute $Gui -WorkingDirectory $WorkDir
  $principal = New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive
  Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Force | Out-Null
  try {
    Start-ScheduledTask -TaskName $taskName
    Start-Sleep -Seconds 3
  } finally {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
  }
}

# --------------------------------------------------------------------- stop

function Stop-Deskflow {
  param([string]$RootDir)
  Write-Host "== deskflow-ctl stop ($RootDir) =="

  $svc = Get-DeskflowService
  if ($svc) {
    if ($svc.State -ne 'Stopped') {
      Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
    }
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
      $s = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
      if (-not $s -or $s.Status -eq 'Stopped') { break }
      Start-Sleep -Milliseconds 500
    }
    $svc = Get-DeskflowService
    if ($svc -and $svc.ProcessId -gt 0) {
      Write-Host "  service did not stop cleanly; taskkill /F /T /PID $($svc.ProcessId)"
      Invoke-TaskKillPid -ProcessId $svc.ProcessId
    }
  }

  # Every Deskflow process under the install root, in every session. A stale
  # core can run as SYSTEM in session 0 alongside a user-session core because
  # the single-instance guard uses per-session namespaces; kill by PID so both
  # die. Never by image name (would not distinguish paths) and never Mouser.
  $deadline = (Get-Date).AddSeconds($StopTimeoutSec)
  while ($true) {
    $procs = @(Get-DeskflowProcesses | Where-Object { Test-UnderRoot $_.ExecutablePath $RootDir })
    if ($procs.Count -eq 0) { break }
    if ((Get-Date) -ge $deadline) { break }
    foreach ($p in $procs) {
      Write-Host "  taskkill /F /T /PID $($p.ProcessId) $($p.Name) session=$($p.SessionId) ($($p.ExecutablePath))"
      Invoke-TaskKillPid -ProcessId $p.ProcessId
    }
    Start-Sleep -Milliseconds 750
  }

  $remaining = @(Get-DeskflowProcesses | Where-Object { Test-UnderRoot $_.ExecutablePath $RootDir })
  if ($remaining.Count -gt 0) {
    $detail = ($remaining | ForEach-Object { "$($_.Name) pid=$($_.ProcessId) session=$($_.SessionId) path=$($_.ExecutablePath)" }) -join '; '
    throw "deskflow-ctl stop: could not stop all Deskflow processes: $detail"
  }
  Write-Host '== deskflow-ctl: stopped =='
}

# -------------------------------------------------------------------- start

function Invoke-Signing {
  param([string]$RootDir)
  # UIAccess needs Authenticode-signed binaries under Program Files. All
  # signing goes through scripts/sign-windows.ps1, which throws on any
  # missing prerequisite or verify failure.
  $signer = Join-Path $PSScriptRoot 'sign-windows.ps1'
  if (-not (Test-Path $signer)) { throw "sign-windows.ps1 missing at $signer" }
  & $signer -Root $RootDir
}

function Register-DeskflowService {
  param([string]$DaemonPath)
  if (-not (Test-Path $DaemonPath)) { throw "deskflow-daemon.exe not found at $DaemonPath" }
  $binPath = "`"$DaemonPath`""
  if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
    Write-Host "== sc config $ServiceName =="
    sc.exe config $ServiceName binPath= $binPath start= auto | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc.exe config failed ($LASTEXITCODE)" }
  } else {
    Write-Host "== sc create $ServiceName =="
    sc.exe create $ServiceName binPath= $binPath start= auto DisplayName= $ServiceName | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc.exe create failed ($LASTEXITCODE)" }
  }
}

function Wait-CoreUnderService {
  param([int]$ServicePid, [string]$RootDir, [int]$TimeoutSec)
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  while ((Get-Date) -lt $deadline) {
    $core = @(Get-DeskflowProcesses | Where-Object {
        $_.Name -ieq 'deskflow-core.exe' -and $_.ParentProcessId -eq $ServicePid -and (Test-UnderRoot $_.ExecutablePath $RootDir)
      })
    if ($core.Count -ge 1) { return $core[0] }
    Start-Sleep -Milliseconds 500
  }
  return $null
}

function Start-Deskflow {
  param([string]$RootDir)
  Write-Host "== deskflow-ctl start ($RootDir) =="
  $daemon = Join-Path $RootDir 'deskflow-daemon.exe'
  $gui = Join-Path $RootDir 'deskflow.exe'
  if (-not (Test-Path $daemon)) { throw "deskflow-daemon.exe not found at $daemon" }
  if (-not (Test-Path $gui)) { throw "deskflow.exe not found at $gui" }

  if (-not $NoSign) { Invoke-Signing -RootDir $RootDir }
  Register-DeskflowService -DaemonPath $daemon

  $svc = Get-Service -Name $ServiceName
  if ($svc.Status -ne 'Running') {
    Write-Host "== Start-Service $ServiceName =="
    Start-Service -Name $ServiceName
  }
  $svcPid = (Get-DeskflowService).ProcessId
  if (-not $svcPid -or $svcPid -le 0) { throw "service $ServiceName has no PID after Start-Service" }

  $core = Wait-CoreUnderService -ServicePid $svcPid -RootDir $RootDir -TimeoutSec $StartTimeoutSec
  if (-not $core) {
    throw "no deskflow-core.exe with ParentProcessId == service PID $svcPid appeared within ${StartTimeoutSec}s"
  }
  Write-Host "== service pid $svcPid; core pid $($core.ProcessId) session=$($core.SessionId) =="

  if ($NoGui) { return }
  $guiFull = [System.IO.Path]::GetFullPath($gui).ToLowerInvariant()
  $existing = @(Get-DeskflowProcesses | Where-Object {
      $_.Name -ieq 'deskflow.exe' -and $_.ExecutablePath -and $_.ExecutablePath.ToLowerInvariant() -eq $guiFull
    })
  if ($existing.Count -gt 0) {
    Write-Host "== deskflow.exe already running from $RootDir; not launching a second GUI =="
    return
  }
  $mySession = [System.Diagnostics.Process]::GetCurrentProcess().SessionId
  $interactive = Get-InteractiveSession
  if ($interactive -and $interactive.SessionId -ne $mySession) {
    Write-Host "== Launching GUI in console session $($interactive.SessionId) as $($interactive.User) =="
    Start-GuiInSession -Gui $gui -WorkDir $RootDir -User $interactive.User
  } elseif ($mySession -ne 0) {
    Write-Host "== Launching GUI (tray): $gui =="
    Start-Process -FilePath $gui -WorkingDirectory $RootDir
  } else {
    Write-Host '== No interactive session; GUI will start at next login (Run key) =='
  }
}

# ---------------------------------------------------------- status / assert

function Get-DeskflowInventory {
  param([string]$RootDir)
  $svc = Get-DeskflowService
  $svcPid = if ($svc) { [int]$svc.ProcessId } else { 0 }
  $svcExe = if ($svc -and $svc.PathName) { ($svc.PathName -replace '^"([^"]+)".*$', '$1') } else { '' }
  $console = Get-ConsoleSessionId
  $rows = foreach ($p in Get-DeskflowProcesses) {
    $path = $p.ExecutablePath
    # ExecutablePath of a SYSTEM (session 0) process is null to a non-elevated
    # caller; the service's own PID is known from the SCM, so use its binPath.
    if (-not $path -and $svcPid -gt 0 -and [int]$p.ProcessId -eq $svcPid) { $path = $svcExe }
    [pscustomobject]@{
      Name      = $p.Name
      Pid       = [int]$p.ProcessId
      Parent    = [int]$p.ParentProcessId
      Session   = [int]$p.SessionId
      Path      = $(if ($path) { $path } else { '<unreadable: run elevated>' })
      Canonical = (Test-UnderRoot $path $RootDir)
    }
  }
  [pscustomobject]@{
    Service        = $(if ($svc) { $svc.State } else { 'absent' })
    ServicePid     = $svcPid
    ConsoleSession = $console
    Processes      = @($rows)
  }
}

function Show-DeskflowStatus {
  param([string]$RootDir)
  $inv = Get-DeskflowInventory -RootDir $RootDir
  Write-Host "install root: $RootDir"
  Write-Host "service $ServiceName`: $($inv.Service) pid=$($inv.ServicePid) console-session=$($inv.ConsoleSession)"
  Write-Host 'processes:'
  foreach ($r in $inv.Processes) {
    $flag = if ($r.Canonical) { 'yes' } else { 'NO' }
    Write-Host ("  {0,-26} pid={1,-6} parent={2,-6} session={3,-2} canonical={4} {5}" -f $r.Name, $r.Pid, $r.Parent, $r.Session, $flag, $r.Path)
  }
}

function Get-AssertSingleProblems {
  # Pure: takes an inventory, returns the list of violations (empty = OK).
  param($Inventory)
  $problems = @()
  $svcPid = $Inventory.ServicePid
  $console = $Inventory.ConsoleSession
  $procs = @($Inventory.Processes)

  if ($Inventory.Service -ne 'Running' -or $svcPid -le 0) {
    $problems += "service is $($Inventory.Service) (pid=$svcPid), want Running"
  }
  if ($null -eq $console) { $problems += 'no interactive console session (explorer.exe not found)' }

  foreach ($p in $procs | Where-Object { -not $_.Canonical }) {
    $problems += "non-canonical process $($p.Name) pid=$($p.Pid) session=$($p.Session) path=$($p.Path)"
  }
  $canon = @($procs | Where-Object { $_.Canonical })

  $daemons = @($canon | Where-Object { $_.Name -ieq 'deskflow-daemon.exe' })
  if ($daemons.Count -ne 1) { $problems += "deskflow-daemon.exe count=$($daemons.Count) (want 1)" }
  elseif ($daemons[0].Session -ne 0) { $problems += "deskflow-daemon.exe pid=$($daemons[0].Pid) in session $($daemons[0].Session) (want 0)" }
  elseif ($svcPid -gt 0 -and $daemons[0].Pid -ne $svcPid) { $problems += "deskflow-daemon.exe pid=$($daemons[0].Pid) is not the service PID $svcPid" }

  $cores = @($canon | Where-Object { $_.Name -ieq 'deskflow-core.exe' })
  if ($cores.Count -ne 1) { $problems += "deskflow-core.exe count=$($cores.Count) (want 1)" }
  else {
    if ($svcPid -gt 0 -and $cores[0].Parent -ne $svcPid) { $problems += "deskflow-core.exe pid=$($cores[0].Pid) parent=$($cores[0].Parent) is not the service PID $svcPid" }
    if ($null -ne $console -and $cores[0].Session -ne $console) { $problems += "deskflow-core.exe pid=$($cores[0].Pid) in session $($cores[0].Session) (want console $console)" }
  }

  $guis = @($canon | Where-Object { $_.Name -ieq 'deskflow.exe' })
  if ($guis.Count -ne 1) { $problems += "deskflow.exe count=$($guis.Count) (want 1)" }
  elseif ($null -ne $console -and $guis[0].Session -ne $console) { $problems += "deskflow.exe pid=$($guis[0].Pid) in session $($guis[0].Session) (want console $console)" }

  $bridges = @($procs | Where-Object { $_.Name -ieq 'deskflow-vhid-bridge.exe' })
  if ($bridges.Count -ne 0) { $problems += "deskflow-vhid-bridge.exe count=$($bridges.Count) (want 0)" }

  return ,$problems
}

function Assert-DeskflowSingle {
  param([string]$RootDir)
  $inv = Get-DeskflowInventory -RootDir $RootDir
  $problems = @(Get-AssertSingleProblems -Inventory $inv)
  if ($problems.Count -gt 0) {
    throw ("deskflow-ctl assert-single: FAIL`n  " + ($problems -join "`n  "))
  }
  Write-Host "deskflow-ctl assert-single: OK (daemon=1 session 0 pid $($inv.ServicePid); core=1 child of service in session $($inv.ConsoleSession); gui=1; bridge=0)"
}

# --------------------------------------------------------------------- main

if ($MyInvocation.InvocationName -ne '.') {
  if (-not $Verb) { throw 'usage: deskflow-ctl.ps1 stop|start|restart|status|assert-single [-InstallDir DIR] [-NoGui] [-NoSign]' }
  $rootDir = Resolve-InstallDir -Explicit $InstallDir
  if ($Verb -in @('stop', 'start', 'restart')) {
    $fwd = @($Verb, '-InstallDir', $rootDir)
    if ($NoGui) { $fwd += '-NoGui' }
    if ($NoSign) { $fwd += '-NoSign' }
    Assert-Elevated -ForwardArgs $fwd
  }
  switch ($Verb) {
    'stop'          { Stop-Deskflow -RootDir $rootDir }
    'start'         { Start-Deskflow -RootDir $rootDir }
    'restart'       { Stop-Deskflow -RootDir $rootDir; Start-Deskflow -RootDir $rootDir }
    'status'        { Show-DeskflowStatus -RootDir $rootDir }
    'assert-single' { Assert-DeskflowSingle -RootDir $rootDir }
  }
}
