#requires -Modules @{ ModuleName = 'Pester'; ModuleVersion = '5.0' }
<#
  Pester 5 tests for scripts/deskflow-ctl.ps1.
  Run:  Invoke-Pester -Path tools/tests/DeskflowCtl.Tests.ps1

  All OS interaction (CIM, SCM, taskkill, scheduled tasks, signing) is mocked;
  nothing here touches a real host. The script is dot-sourced: its main block
  is guarded by InvocationName -ne '.'.

  NOTE: pwsh is not installed on the macOS seats; this suite is written to be
  run on tiny11 (or any host with Pester 5) and has not been executed there yet.
#>

BeforeAll {
  $script:Script = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'scripts\deskflow-ctl.ps1'
  $script:Script = $script:Script -replace '\\', [IO.Path]::DirectorySeparatorChar
  if (-not (Test-Path $script:Script)) { throw "deskflow-ctl.ps1 not found at $script:Script" }
  . $script:Script

  $script:Root = 'C:\Program Files\Deskflow'

  # Cmdlets that only exist on Windows / with the ScheduledTasks module: define
  # stubs so Mock has something to replace when the suite runs elsewhere.
  foreach ($n in 'Stop-Service', 'Start-Service', 'Get-Service', 'Register-ScheduledTask', 'Unregister-ScheduledTask',
      'New-ScheduledTaskAction', 'New-ScheduledTaskPrincipal', 'Start-ScheduledTask', 'Invoke-CimMethod') {
    if (-not (Get-Command $n -ErrorAction SilentlyContinue)) {
      Set-Item -Path "Function:\global:$n" -Value { param([Parameter(ValueFromRemainingArguments)]$Rest) }
    }
  }

  function New-Proc([string]$Name, [int]$ProcessId, [int]$Parent, [int]$Session, [string]$Path) {
    [pscustomobject]@{ Name = $Name; ProcessId = $ProcessId; ParentProcessId = $Parent; SessionId = $Session; ExecutablePath = $Path }
  }
  function New-Svc([string]$State, [int]$ProcessId) {
    [pscustomobject]@{ Name = 'Deskflow'; State = $State; ProcessId = $ProcessId; PathName = "`"$script:Root\deskflow-daemon.exe`"" }
  }
  function New-Inventory([string]$Service, [int]$SvcPid, $Console, $Procs) {
    [pscustomobject]@{ Service = $Service; ServicePid = $SvcPid; ConsoleSession = $Console; Processes = @($Procs) }
  }
  function New-Row([string]$Name, [int]$ProcessId, [int]$Parent, [int]$Session, [bool]$Canonical) {
    [pscustomobject]@{ Name = $Name; Pid = $ProcessId; Parent = $Parent; Session = $Session; Path = "$script:Root\$Name"; Canonical = $Canonical }
  }
  function New-RunEntry([string]$Hive, [string]$Name, [string]$Exe) {
    [pscustomobject]@{ Hive = $Hive; Name = $Name; Command = "`"$Exe`""; Exe = $Exe }
  }
  # A healthy process table plus the launcher inventory fields.
  function New-LauncherInventory($Run, $Hklm, $Startup, $Tasks) {
    $inv = New-Inventory 'Running' 1000 1 @(
      (New-Row 'deskflow-daemon.exe' 1000 4 0 $true),
      (New-Row 'deskflow-core.exe' 1001 1000 1 $true),
      (New-Row 'deskflow.exe' 1003 500 1 $true)
    )
    $inv | Add-Member -NotePropertyName GuiExe -NotePropertyValue "$script:Root\deskflow.exe"
    $inv | Add-Member -NotePropertyName RunEntries -NotePropertyValue @($Run)
    $inv | Add-Member -NotePropertyName HklmRunEntries -NotePropertyValue @($Hklm)
    $inv | Add-Member -NotePropertyName StartupShortcuts -NotePropertyValue @($Startup)
    $inv | Add-Member -NotePropertyName ScheduledTasks -NotePropertyValue @($Tasks)
    $inv
  }
}

Describe 'Test-UnderRoot' {
  It 'matches only paths strictly under the root, case-insensitively' {
    Test-UnderRoot 'C:\Program Files\Deskflow\deskflow-core.exe' $script:Root | Should -BeTrue
    Test-UnderRoot 'c:\program files\deskflow\sub\deskflow.exe' $script:Root | Should -BeTrue
    Test-UnderRoot 'C:\Program Files\Deskflow' $script:Root | Should -BeFalse
    Test-UnderRoot 'C:\Program Files\Deskflow2\deskflow.exe' $script:Root | Should -BeFalse
    Test-UnderRoot 'C:\Users\alexh\Desktop\deskflow\build\bin\Release\deskflow-core.exe' $script:Root | Should -BeFalse
    Test-UnderRoot '' $script:Root | Should -BeFalse
    Test-UnderRoot $null $script:Root | Should -BeFalse
  }
}

Describe 'Stop-Deskflow' {
  BeforeEach {
    Mock Suspend-DeskflowServiceRecovery {}
    $script:Killed = [System.Collections.Generic.List[int]]::new()
    Mock Invoke-TaskKillPid { $script:Killed.Add($ProcessId); $script:Table.RemoveAll({ param($p) $p.ProcessId -eq $ProcessId }) | Out-Null }
    Mock Start-Sleep {}
    Mock Stop-Service {}
    $script:Table = [System.Collections.Generic.List[object]]::new()
  }

  It 'stops the service, then taskkills every process under the install root in every session, never Mouser' {
    $script:Table.Add((New-Proc 'deskflow-daemon.exe' 1000 4 0 "$script:Root\deskflow-daemon.exe"))
    $script:Table.Add((New-Proc 'deskflow-core.exe' 1001 1000 1 "$script:Root\deskflow-core.exe"))
    $script:Table.Add((New-Proc 'deskflow-core.exe' 1002 1000 0 "$script:Root\deskflow-core.exe"))  # stale SYSTEM core
    $script:Table.Add((New-Proc 'deskflow.exe' 1003 500 1 "$script:Root\deskflow.exe"))
    $script:Table.Add((New-Proc 'deskflow-core.exe' 1004 600 1 'C:\Users\alexh\Desktop\deskflow\build\bin\Release\deskflow-core.exe'))
    $script:Table.Add((New-Proc 'Mouser.exe' 1005 500 1 'C:\Program Files\Mouser\Mouser.exe'))
    $script:SvcState = 'Running'
    Mock Get-CimInstance {
      if ($ClassName -eq 'Win32_Service') { return (New-Svc $script:SvcState 0) }
      if ($Filter -like "Name LIKE 'deskflow%'") { return @($script:Table.ToArray()) }
      return @()
    }
    Mock Get-Service { [pscustomobject]@{ Name = 'Deskflow'; Status = 'Stopped' } }

    Stop-Deskflow -RootDir $script:Root

    Should -Invoke Stop-Service -Times 1 -ParameterFilter { $Name -eq 'Deskflow' -and $Force }
    $script:Killed | Sort-Object | Should -Be @(1000, 1001, 1002, 1003)
    $script:Killed | Should -Not -Contain 1004   # build-tree core: not under the root, left alone
    $script:Killed | Should -Not -Contain 1005   # Mouser: never
    $script:Table | Where-Object { $_.Name -eq 'Mouser.exe' } | Should -Not -BeNullOrEmpty
  }

  It 'taskkills the service PID when the SCM does not report Stopped within the poll' {
    $script:Table.Add((New-Proc 'deskflow-daemon.exe' 1000 4 0 "$script:Root\deskflow-daemon.exe"))
    Mock Get-CimInstance {
      if ($ClassName -eq 'Win32_Service') { return (New-Svc 'StopPending' 1000) }
      if ($Filter -like "Name LIKE 'deskflow%'") { return @($script:Table.ToArray()) }
      return @()
    }
    Mock Get-Service { [pscustomobject]@{ Name = 'Deskflow'; Status = 'StopPending' } }

    # NOTE: the SCM StopPending poll is now >= 25 s of wall clock (the daemon's
    # STOP_PENDING waitHint is 30 s because its watchdog stop can take 25 s), and
    # Stop-Deskflow clamps $ServiceStopTimeoutSec to that floor. Start-Sleep is
    # mocked, so this case busy-spins ~25 s on Get-Date; mock Get-Date with a
    # fake clock if that is too slow on tiny11.
    Stop-Deskflow -RootDir $script:Root
    $script:Killed | Should -Contain 1000
  }

  It 'never polls the SCM for less than 25 s before taskkilling, even when asked to' {
    # The floor exists because deskflow-daemon's watchdog stop takes up to 25 s
    # (20 s core shutdown + 5 s thread join); killing earlier interrupts a clean teardown.
    $script:Table.Add((New-Proc 'deskflow-daemon.exe' 1000 4 0 "$script:Root\deskflow-daemon.exe"))
    Mock Get-CimInstance {
      if ($ClassName -eq 'Win32_Service') { return (New-Svc 'StopPending' 1000) }
      if ($Filter -like "Name LIKE 'deskflow%'") { return @($script:Table.ToArray()) }
      return @()
    }
    $script:Polls = 0
    Mock Get-Service { $script:Polls++; [pscustomobject]@{ Name = 'Deskflow'; Status = 'StopPending' } }
    $ServiceStopTimeoutSec = 1
    $started = Get-Date
    Stop-Deskflow -RootDir $script:Root
    ((Get-Date) - $started).TotalSeconds | Should -BeGreaterOrEqual 24
    $script:Polls | Should -BeGreaterThan 1
    $script:Killed | Should -Contain 1000
  }

  It 'throws naming the survivors when something under the root cannot be killed' {
    $script:Table.Add((New-Proc 'deskflow-core.exe' 1001 1000 1 "$script:Root\deskflow-core.exe"))
    Mock Invoke-TaskKillPid {}   # kill does nothing
    Mock Get-CimInstance {
      if ($ClassName -eq 'Win32_Service') { return $null }
      if ($Filter -like "Name LIKE 'deskflow%'") { return @($script:Table.ToArray()) }
      return @()
    }
    Mock Get-Service { $null }
    { Stop-Deskflow -RootDir $script:Root -ErrorAction Stop } | Should -Throw -ExpectedMessage '*could not stop all Deskflow processes*deskflow-core.exe pid=1001*'
  }

  It 'is a no-op (no throw, no kills) when nothing Deskflow is running' {
    Mock Get-CimInstance { if ($ClassName -eq 'Win32_Service') { return $null }; return @() }
    Mock Get-Service { $null }
    { Stop-Deskflow -RootDir $script:Root } | Should -Not -Throw
    $script:Killed.Count | Should -Be 0
    Should -Invoke Stop-Service -Times 0
  }
}

Describe 'Start-Deskflow' {
  BeforeEach {
    Mock Test-Path { $true }
    Mock Invoke-Signing {}
    Mock Register-DeskflowService {}
    Mock Start-Sleep {}
    Mock Start-Service {}
    Mock Get-Service { [pscustomobject]@{ Name = 'Deskflow'; Status = 'Stopped' } }
    Mock Register-ScheduledTask {}
    Mock Unregister-ScheduledTask {}
    Mock New-ScheduledTaskAction { 'action' }
    Mock New-ScheduledTaskPrincipal { 'principal' }
    Mock Start-ScheduledTask {}
    Mock Start-Process {}
    Mock Get-InteractiveSession { [pscustomobject]@{ SessionId = 1; User = 'TINY11\alexh' } }
  }

  It 'signs, registers, starts the service, waits for the service-owned core, then launches one GUI via a scheduled task' {
    $script:Procs = @(
      (New-Proc 'deskflow-daemon.exe' 1000 4 0 "$script:Root\deskflow-daemon.exe"),
      (New-Proc 'deskflow-core.exe' 1001 1000 1 "$script:Root\deskflow-core.exe")
    )
    Mock Get-CimInstance {
      if ($ClassName -eq 'Win32_Service') { return (New-Svc 'Running' 1000) }
      if ($Filter -like "Name LIKE 'deskflow%'") { return $script:Procs }
      return @()
    }
    Start-Deskflow -RootDir $script:Root
    Should -Invoke Invoke-Signing -Times 1 -ParameterFilter { $RootDir -eq $script:Root }
    Should -Invoke Register-DeskflowService -Times 1
    Should -Invoke Start-Service -Times 1 -ParameterFilter { $Name -eq 'Deskflow' }
    Should -Invoke Start-ScheduledTask -Times 1 -ParameterFilter { $TaskName -eq 'DeskflowCtlLaunch' }
    Should -Invoke New-ScheduledTaskPrincipal -Times 1 -ParameterFilter { $UserId -eq 'TINY11\alexh' -and $LogonType -eq 'Interactive' }
    Should -Invoke Unregister-ScheduledTask -Times 2
    Should -Invoke Start-Process -Times 0
  }

  It 'throws when no deskflow-core.exe with ParentProcessId == service PID appears' {
    Mock Get-CimInstance {
      if ($ClassName -eq 'Win32_Service') { return (New-Svc 'Running' 1000) }
      if ($Filter -like "Name LIKE 'deskflow%'") {
        # a core exists, but it is someone else's child
        return @((New-Proc 'deskflow-core.exe' 1001 777 1 "$script:Root\deskflow-core.exe"))
      }
      return @()
    }
    { Start-Deskflow -RootDir $script:Root } | Should -Throw -ExpectedMessage '*ParentProcessId == service PID 1000*'
    Should -Invoke Start-ScheduledTask -Times 0
  }

  It 'does not launch a second GUI when deskflow.exe already runs from the install root' {
    Mock Get-CimInstance {
      if ($ClassName -eq 'Win32_Service') { return (New-Svc 'Running' 1000) }
      if ($Filter -like "Name LIKE 'deskflow%'") {
        return @(
          (New-Proc 'deskflow-core.exe' 1001 1000 1 "$script:Root\deskflow-core.exe"),
          (New-Proc 'deskflow.exe' 1003 500 1 "$script:Root\deskflow.exe")
        )
      }
      return @()
    }
    Start-Deskflow -RootDir $script:Root
    Should -Invoke Start-ScheduledTask -Times 0
    Should -Invoke Start-Process -Times 0
  }

  It '-NoSign skips signing and -NoGui skips the GUI launch' {
    Mock Get-CimInstance {
      if ($ClassName -eq 'Win32_Service') { return (New-Svc 'Running' 1000) }
      if ($Filter -like "Name LIKE 'deskflow%'") { return @((New-Proc 'deskflow-core.exe' 1001 1000 1 "$script:Root\deskflow-core.exe")) }
      return @()
    }
    $NoSign = $true; $NoGui = $true
    Start-Deskflow -RootDir $script:Root
    Should -Invoke Invoke-Signing -Times 0
    Should -Invoke Start-ScheduledTask -Times 0
  }
}

Describe 'Get-AssertSingleProblems' {
  It 'is empty for exactly {daemon:1 session 0 == service PID, core:1 child of service in console, gui:1 in console, bridge:0}' {
    $inv = New-Inventory 'Running' 1000 1 @(
      (New-Row 'deskflow-daemon.exe' 1000 4 0 $true),
      (New-Row 'deskflow-core.exe' 1001 1000 1 $true),
      (New-Row 'deskflow.exe' 1003 500 1 $true)
    )
    @(Get-AssertSingleProblems -Inventory $inv) | Should -BeNullOrEmpty
  }

  It 'fails on two cores' {
    $inv = New-Inventory 'Running' 1000 1 @(
      (New-Row 'deskflow-daemon.exe' 1000 4 0 $true),
      (New-Row 'deskflow-core.exe' 1001 1000 1 $true),
      (New-Row 'deskflow-core.exe' 1002 1000 0 $true),
      (New-Row 'deskflow.exe' 1003 500 1 $true)
    )
    $p = @(Get-AssertSingleProblems -Inventory $inv)
    $p | Should -Contain 'deskflow-core.exe count=2 (want 1)'
  }

  It 'fails when the core is not the service''s child or lives outside the console session' {
    $inv = New-Inventory 'Running' 1000 1 @(
      (New-Row 'deskflow-daemon.exe' 1000 4 0 $true),
      (New-Row 'deskflow-core.exe' 1001 777 0 $true),
      (New-Row 'deskflow.exe' 1003 500 1 $true)
    )
    $p = @(Get-AssertSingleProblems -Inventory $inv) -join "`n"
    $p | Should -Match 'deskflow-core.exe pid=1001 parent=777 is not the service PID 1000'
    $p | Should -Match 'deskflow-core.exe pid=1001 in session 0 \(want console 1\)'
  }

  It 'fails on a non-canonical path, a missing GUI, a running bridge, or a stopped service' {
    $bad = New-Row 'deskflow-core.exe' 2000 1 1 $false
    $bad.Path = 'C:\Users\alexh\Desktop\deskflow\build\bin\Release\deskflow-core.exe'
    $inv = New-Inventory 'Stopped' 0 1 @(
      (New-Row 'deskflow-core.exe' 1001 1000 1 $true),
      (New-Row 'deskflow-vhid-bridge.exe' 1009 1 1 $true),
      $bad
    )
    $p = @(Get-AssertSingleProblems -Inventory $inv) -join "`n"
    $p | Should -Match 'service is Stopped'
    $p | Should -Match 'non-canonical process deskflow-core.exe pid=2000 .*build\\bin\\Release'
    $p | Should -Match 'deskflow-daemon.exe count=0'
    $p | Should -Match 'deskflow.exe count=0'
    $p | Should -Match 'deskflow-vhid-bridge.exe count=1 \(want 0\)'
  }

  It 'is empty with exactly one HKCU Run entry at the canonical exe and no other launcher' {
    $inv = New-LauncherInventory @((New-RunEntry 'HKCU' 'Deskflow' "$script:Root\deskflow.exe")) @() @() @()
    @(Get-AssertSingleProblems -Inventory $inv) | Should -BeNullOrEmpty
  }

  It 'fails on two HKCU Run entries, a missing one, or one at a non-canonical exe' {
    $two = New-LauncherInventory @(
      (New-RunEntry 'HKCU' 'Deskflow' "$script:Root\deskflow.exe"),
      (New-RunEntry 'HKCU' 'Deskflow (old)' 'C:\Users\alexh\Desktop\deskflow\build\bin\Release\deskflow.exe')
    ) @() @() @()
    (@(Get-AssertSingleProblems -Inventory $two) -join "`n") | Should -Match 'HKCU Run entries for deskflow count=2'

    $none = New-LauncherInventory @() @() @() @()
    (@(Get-AssertSingleProblems -Inventory $none) -join "`n") | Should -Match 'HKCU Run entries for deskflow count=0'

    $wrong = New-LauncherInventory @((New-RunEntry 'HKCU' 'Deskflow' 'C:\Users\alexh\Desktop\deskflow\build\bin\Release\deskflow.exe')) @() @() @()
    (@(Get-AssertSingleProblems -Inventory $wrong) -join "`n") | Should -Match 'HKCU Run\\Deskflow launches .*build\\bin\\Release\\deskflow.exe, not the canonical'
  }

  It 'fails on an HKLM Run entry, a Startup shortcut or a scheduled task launching deskflow' {
    $inv = New-LauncherInventory @((New-RunEntry 'HKCU' 'Deskflow' "$script:Root\deskflow.exe")) `
      @((New-RunEntry 'HKLM' 'Deskflow' "$script:Root\deskflow.exe")) `
      @([pscustomobject]@{ Path = 'C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Startup\Deskflow.lnk'; Target = "$script:Root\deskflow.exe" }) `
      @([pscustomobject]@{ TaskName = 'DeskflowAtLogon'; TaskPath = '\'; Execute = "$script:Root\deskflow.exe"; State = 'Ready' })
    $p = @(Get-AssertSingleProblems -Inventory $inv) -join "`n"
    $p | Should -Match 'HKLM Run\\Deskflow launches deskflow'
    $p | Should -Match 'Startup folder launcher .*Deskflow.lnk'
    $p | Should -Match 'scheduled task \\DeskflowAtLogon runs'
  }

  It 'skips the launcher rules for a process-only inventory (older callers)' {
    $inv = New-Inventory 'Running' 1000 1 @(
      (New-Row 'deskflow-daemon.exe' 1000 4 0 $true),
      (New-Row 'deskflow-core.exe' 1001 1000 1 $true),
      (New-Row 'deskflow.exe' 1003 500 1 $true)
    )
    @(Get-AssertSingleProblems -Inventory $inv) | Should -BeNullOrEmpty
  }

  It 'Assert-DeskflowSingle throws with every problem listed' {
    Mock Get-DeskflowInventory { New-Inventory 'Running' 1000 1 @((New-Row 'deskflow-daemon.exe' 1000 4 0 $true)) }
    { Assert-DeskflowSingle -RootDir $script:Root } | Should -Throw -ExpectedMessage '*assert-single: FAIL*deskflow-core.exe count=0*deskflow.exe count=0*'
  }
}

Describe 'Assert-Elevated' {
  It 'throws over SSH when the token is not High Mandatory Level instead of trying RunAs' {
    Mock Test-IsAdmin { $true }
    Mock Test-HighIntegrity { $false }
    Mock Test-InteractiveDesktop { $false }
    Mock Start-Process {}
    { Assert-Elevated -ForwardArgs @('stop') } | Should -Throw -ExpectedMessage '*High Mandatory Level*'
    Should -Invoke Start-Process -Times 0
  }

  It 'returns silently when already elevated' {
    Mock Test-IsAdmin { $true }
    Mock Test-HighIntegrity { $true }
    Mock Start-Process {}
    { Assert-Elevated -ForwardArgs @('stop') } | Should -Not -Throw
    Should -Invoke Start-Process -Times 0
  }
}

Describe 'Suspend-DeskflowServiceRecovery' {
  It 'clears the recovery actions so a taskkill during stop cannot restart the service' {
    $script:ScCalls = [System.Collections.Generic.List[string]]::new()
    function global:sc.exe { $script:ScCalls.Add(($args -join ' ')); $global:LASTEXITCODE = 0 }
    try { Suspend-DeskflowServiceRecovery } finally { Remove-Item Function:\global:sc.exe -ErrorAction SilentlyContinue }
    $script:ScCalls | Should -Contain 'failure Deskflow reset= 0 actions= '
  }
  It 'is invoked by Stop-Deskflow before the service is stopped' {
    $script:Order = [System.Collections.Generic.List[string]]::new()
    Mock Suspend-DeskflowServiceRecovery { $script:Order.Add('suspend') }
    Mock Stop-Service { $script:Order.Add('stop') }
    Mock Get-Service { [pscustomobject]@{ Name = 'Deskflow'; Status = 'Stopped' } }
    Mock Start-Sleep {}
    Mock Get-CimInstance {
      if ($ClassName -eq 'Win32_Service') { return (New-Svc 'Running' 1000) }
      return @()
    }
    Stop-Deskflow -RootDir $script:Root
    $script:Order[0] | Should -Be 'suspend'
    $script:Order[1] | Should -Be 'stop'
  }
}

Describe 'Register-DeskflowService recovery' {
  It 'registers restart-on-failure actions (1s/5s/30s, reset 24h) with failureflag and creates ProgramData\Deskflow' {
    $script:ScCalls = [System.Collections.Generic.List[string]]::new()
    Mock Test-Path { $true } -ParameterFilter { $Path -like '*deskflow-daemon.exe' }
    Mock Test-Path { $false } -ParameterFilter { $LiteralPath -like '*Deskflow' }
    Mock Get-Service { [pscustomobject]@{ Name = 'Deskflow'; Status = 'Stopped' } }
    Mock New-Item {}
    function global:sc.exe { $script:ScCalls.Add(($args -join ' ')); $global:LASTEXITCODE = 0 }
    try {
      Register-DeskflowService -DaemonPath 'C:\Program Files\Deskflow\deskflow-daemon.exe'
    } finally {
      Remove-Item Function:\global:sc.exe -ErrorAction SilentlyContinue
    }
    $script:ScCalls | Should -Contain 'config Deskflow binPath= "C:\Program Files\Deskflow\deskflow-daemon.exe" start= auto'
    $script:ScCalls | Should -Contain 'failure Deskflow reset= 86400 actions= restart/1000/restart/5000/restart/30000'
    $script:ScCalls | Should -Contain 'failureflag Deskflow 1'
    Should -Invoke New-Item -Times 1 -ParameterFilter { $Path -like '*Deskflow' -and $ItemType -eq 'Directory' }
  }
}

Describe 'script hygiene' {
  It 'never kills by image name and never references Mouser as a target' {
    $text = Get-Content -LiteralPath $script:Script -Raw
    $text | Should -Not -Match 'taskkill /F /T /IM'
    $text | Should -Not -Match 'Stop-Process -Name'
    $text | Should -Not -Match "Get-Process -Name 'Mouser'"
    $text | Should -Not -Match 'Stop-Process[^\n]*Mouser'
  }
}
