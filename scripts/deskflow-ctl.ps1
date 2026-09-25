#requires -Version 5.1
<#
.SYNOPSIS
  deskflow-ctl - the ONE way to stop/start/restart Deskflow on Windows.

.DESCRIPTION
  Verbs:
    stop           suspend the service's recovery actions, Stop-Service Deskflow
                   (poll <=30 s), taskkill the service PID if the SCM hangs, then taskkill /F /T every process whose
                   ExecutablePath is under the install root (ALL sessions).
                   Loops <=25 s; throws if anything remains. Never touches Mouser.
    start          sign (scripts/sign-windows.ps1) -> sc create/config ->
                   sc failure (restart 1s/5s/30s) + C:\ProgramData\Deskflow ->
                   Start-Service -> wait <=15 s for a deskflow-core.exe whose
                   ParentProcessId is the service PID -> launch the GUI into the
                   interactive console session via a scheduled task.
    restart        stop then start.
    status         inventory: service state/PID + every Deskflow process with
                   path, session, parent - prints, never throws on findings.
    assert-single  exactly { deskflow-daemon.exe: 1 in session 0 (== service PID),
                   deskflow-core.exe: 1 with parent == service PID in the console
                   session, deskflow.exe: 1 in the console session,
                   deskflow-vhid-bridge.exe: 0 }, all under the install root.
                   Throws (exit 1) on any mismatch.

  Elevation: interactive sessions self-elevate (RunAs). Over SSH (no
  interactive desktop) the caller must already be elevated - `whoami /groups`
  must show "High Mandatory Level" - otherwise the script throws.

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
  # How long to let the SCM report StopPending before taskkilling the daemon.
  # The daemon's STOP_PENDING waitHint is 30 s because its watchdog stop can
  # legitimately take 25 s (20 s core shutdown + 5 s thread join); polling for
  # less than that killed a daemon that was still tearing down cleanly.
  [int]$ServiceStopTimeoutSec = 30,
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
    throw "deskflow-ctl $Verb needs an elevated token (whoami /groups must show 'High Mandatory Level'); over SSH run from an elevated shell - RunAs cannot prompt here."
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
    # A taskkill of the daemon below would otherwise trip the SCM recovery
    # actions and restart the service we are stopping. Suspend them for the
    # duration; ctl start (Register-DeskflowService) restores them.
    Suspend-DeskflowServiceRecovery
    if ($svc.State -ne 'Stopped') {
      Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
    }
    # Poll at least as long as the watchdog stop can take (25 s; the daemon's
    # STOP_PENDING waitHint is 30 s). Never taskkill before that elapses.
    $pollSec = [Math]::Max($ServiceStopTimeoutSec, 25)
    $deadline = (Get-Date).AddSeconds($pollSec)
    while ((Get-Date) -lt $deadline) {
      $s = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
      if (-not $s -or $s.Status -eq 'Stopped') { break }
      Start-Sleep -Milliseconds 500
    }
    $svc = Get-DeskflowService
    if ($svc -and $svc.ProcessId -gt 0) {
      Write-Host "  service did not stop within ${pollSec}s; taskkill /F /T /PID $($svc.ProcessId)"
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
  Set-DeskflowServiceRecovery
  New-DeskflowProgramData
}

function Suspend-DeskflowServiceRecovery {
  # An empty `actions=` value (the documented way to clear failure actions)
  # is rejected as ERROR_INVALID_PARAMETER (1639) by sc.exe on some Windows
  # builds (reproduced on tiny11, every invocation form: native, cmd /c,
  # Start-Process, argument array) even though `sc /?` accepts it in
  # principle. A single restart action with the maximum delay (2^31-1 ms =
  # ~24.8 days) is syntactically a normal non-empty `actions=` value, so it
  # is accepted everywhere, and in practice never fires during a deploy —
  # functionally equivalent to "no recovery actions" for our purposes.
  # ctl start (Register-DeskflowService -> Set-DeskflowServiceRecovery)
  # always restores the real restart/1s/5s/30s policy afterwards.
  sc.exe failure $ServiceName reset= 0 actions= restart/2147483647 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "sc.exe failure (suspend) failed ($LASTEXITCODE)" }
}

function Set-DeskflowServiceRecovery {
  # A daemon crash used to stay down until someone ran ctl start: the service
  # was registered with no recovery actions. failureflag=1 also counts a
  # non-zero exit (not only an SCM-detected crash) as a failure.
  Write-Host "== sc failure $ServiceName (restart 1s/5s/30s, reset 24h) =="
  sc.exe failure $ServiceName reset= 86400 actions= restart/1000/restart/5000/restart/30000 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "sc.exe failure failed ($LASTEXITCODE)" }
  sc.exe failureflag $ServiceName 1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "sc.exe failureflag failed ($LASTEXITCODE)" }
}

function New-DeskflowProgramData {
  # The daemon log lives here and the daemon never creates the directory, so a
  # fresh seat silently logs nothing (LogOutputters.cpp) until it exists.
  $dir = Join-Path $env:ProgramData 'Deskflow'
  if (-not (Test-Path -LiteralPath $dir)) {
    Write-Host "== creating $dir =="
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
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

function Get-RunKeyEntries {
  # Every value under a Run key whose data mentions deskflow, as
  # @{ Hive; Name; Command; Exe } rows. Exe is the unquoted executable path.
  param([string]$Hive, [string]$KeyPath)
  $rows = @()
  $key = Get-Item -LiteralPath $KeyPath -ErrorAction SilentlyContinue
  if (-not $key) { return $rows }
  foreach ($name in $key.GetValueNames()) {
    $cmd = [string]$key.GetValue($name)
    if ($cmd -notmatch '(?i)deskflow') { continue }
    $exe = if ($cmd -match '^"([^"]+)"') { $Matches[1] } else { ($cmd -split '\s+')[0] }
    $rows += [pscustomobject]@{ Hive = $Hive; Name = $name; Command = $cmd; Exe = $exe }
  }
  return $rows
}

function Get-StartupShortcuts {
  # *.lnk/*.url/*.exe in the user's and the common Startup folders that
  # name deskflow or point at a deskflow executable.
  $rows = @()
  $shell = $null
  foreach ($dir in @((Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Startup'),
                     (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Startup'))) {
    if (-not (Test-Path -LiteralPath $dir)) { continue }
    foreach ($f in Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue) {
      $target = ''
      if ($f.Extension -ieq '.lnk') {
        try {
          if (-not $shell) { $shell = New-Object -ComObject WScript.Shell }
          $target = [string]$shell.CreateShortcut($f.FullName).TargetPath
        } catch { $target = '' }
      }
      if ($f.Name -match '(?i)deskflow' -or $target -match '(?i)deskflow') {
        $rows += [pscustomobject]@{ Path = $f.FullName; Target = $target }
      }
    }
  }
  return $rows
}

function Get-DeskflowScheduledTasks {
  # Scheduled tasks with an action that runs a deskflow executable. The
  # transient DeskflowCtlLaunch task (Start-GuiInSession) is unregistered in
  # its finally block; it is excluded so a race with `start` is not a launcher.
  $rows = @()
  $tasks = @(Get-ScheduledTask -ErrorAction SilentlyContinue)
  foreach ($t in $tasks) {
    if ($t.TaskName -eq 'DeskflowCtlLaunch') { continue }
    foreach ($a in @($t.Actions)) {
      $exe = [string]$a.Execute
      if ($exe -match '(?i)deskflow') {
        $rows += [pscustomobject]@{ TaskName = $t.TaskName; TaskPath = $t.TaskPath; Execute = $exe; State = [string]$t.State }
      }
    }
  }
  return $rows
}

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
  # Launchers besides the service: exactly one HKCU Run entry (the GUI tray,
  # written by MainWindow.cpp) at the canonical exe; nothing in HKLM, the
  # Startup folders or the task scheduler. Anything else is a second launcher
  # (the macOS BTM/LaunchAgent race, Windows edition).
  $hkcuRun = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
  $hklmRun = @('HKLM:\Software\Microsoft\Windows\CurrentVersion\Run',
               'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Run')
  $runEntries = @(Get-RunKeyEntries -Hive 'HKCU' -KeyPath $hkcuRun)
  $hklmEntries = @()
  foreach ($k in $hklmRun) { $hklmEntries += @(Get-RunKeyEntries -Hive 'HKLM' -KeyPath $k) }
  [pscustomobject]@{
    Service          = $(if ($svc) { $svc.State } else { 'absent' })
    ServicePid       = $svcPid
    ConsoleSession   = $console
    Processes        = @($rows)
    GuiExe           = (Join-Path $RootDir 'deskflow.exe')
    RunEntries       = $runEntries
    HklmRunEntries   = @($hklmEntries)
    StartupShortcuts = @(Get-StartupShortcuts)
    ScheduledTasks   = @(Get-DeskflowScheduledTasks)
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
  Write-Host 'launchers:'
  foreach ($e in @($inv.RunEntries) + @($inv.HklmRunEntries)) { Write-Host ("  {0} Run\{1} = {2}" -f $e.Hive, $e.Name, $e.Command) }
  foreach ($s in @($inv.StartupShortcuts)) { Write-Host ("  Startup {0} -> {1}" -f $s.Path, $s.Target) }
  foreach ($t in @($inv.ScheduledTasks)) { Write-Host ("  Task {0}{1} ({2}) -> {3}" -f $t.TaskPath, $t.TaskName, $t.State, $t.Execute) }
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

  # Launchers (only when the inventory collected them; older callers pass
  # process-only inventories). Exactly one HKCU Run entry at the canonical
  # GUI exe, zero anywhere else: the service is the only core launcher and
  # the Run entry the only GUI launcher.
  if ($null -ne $Inventory.PSObject.Properties['RunEntries']) {
    $guiExe = [string]$Inventory.GuiExe
    $run = @($Inventory.RunEntries)
    if ($run.Count -ne 1) {
      $problems += "HKCU Run entries for deskflow count=$($run.Count) (want exactly 1 at $guiExe): " + (($run | ForEach-Object { "$($_.Name)=$($_.Command)" }) -join ', ')
    } elseif ([string]::IsNullOrWhiteSpace([string]$run[0].Exe)) {
      $problems += "HKCU Run\$($run[0].Name) has no executable in its command ('$($run[0].Command)'); want `"$guiExe`""
    } elseif ($guiExe -and -not ([System.IO.Path]::GetFullPath($run[0].Exe).TrimEnd('\') -ieq [System.IO.Path]::GetFullPath($guiExe).TrimEnd('\'))) {
      $problems += "HKCU Run\$($run[0].Name) launches $($run[0].Exe), not the canonical $guiExe"
    }
    foreach ($e in @($Inventory.HklmRunEntries)) { $problems += "HKLM Run\$($e.Name) launches deskflow ($($e.Command)); want none (per-user Run entry only)" }
    foreach ($s in @($Inventory.StartupShortcuts)) { $problems += "Startup folder launcher $($s.Path) -> $($s.Target); want none" }
    foreach ($t in @($Inventory.ScheduledTasks)) { $problems += "scheduled task $($t.TaskPath)$($t.TaskName) runs $($t.Execute); want none" }
  }

  return $problems
}

function Assert-DeskflowSingle {
  param([string]$RootDir)
  $inv = Get-DeskflowInventory -RootDir $RootDir
  $problems = @(Get-AssertSingleProblems -Inventory $inv)
  if ($problems.Count -gt 0) {
    throw ("deskflow-ctl assert-single: FAIL`n  " + ($problems -join "`n  "))
  }
  Write-Host "deskflow-ctl assert-single: OK (daemon=1 session 0 pid $($inv.ServicePid); core=1 child of service in session $($inv.ConsoleSession); gui=1; bridge=0; launchers: HKCU Run only)"
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
