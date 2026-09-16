# Pester 5 tests for tools/fleet-health.ps1.
# Run:  Invoke-Pester -Path tools/tests/FleetHealth.Tests.ps1
# All OS interaction (Authenticode, sc.exe, quser, processes, TCP) is mocked; nothing
# here touches a real host.

BeforeAll {
  $script:Script = Join-Path (Split-Path -Parent $PSScriptRoot) "fleet-health.ps1"
  . $script:Script   # dot-sourced: the main block is guarded by InvocationName -ne "."

  function New-FakeSig([string]$Status, [string]$Thumb) {
    $cert = if ($Thumb) { [pscustomobject]@{ Thumbprint = $Thumb } } else { $null }
    [pscustomobject]@{ Status = $Status; SignerCertificate = $cert }
  }
  function New-FakeFile([string]$Name) {
    [pscustomobject]@{ Name = $Name; FullName = "C:\Program Files\Deskflow\$Name" }
  }
  $script:Thumb = "AB12CD34EF56AB12CD34EF56AB12CD34EF56AB12"
}

Describe "Get-EnvValueFromFile" {
  It "parses KEY=VALUE with quotes, export and comments" {
    $f = Join-Path $TestDrive "x.env"
    @('# c', 'export DESKFLOW_SIGN_THUMBPRINT="ABC"', 'OTHER=1') | Set-Content $f
    Get-EnvValueFromFile $f "DESKFLOW_SIGN_THUMBPRINT" | Should -Be "ABC"
    Get-EnvValueFromFile $f "MISSING" | Should -Be ""
    Get-EnvValueFromFile (Join-Path $TestDrive "nope.env") "X" | Should -Be ""
  }
}

Describe "Test-Authenticode" {
  BeforeEach {
    Mock Test-Path { $true }
    Mock Get-ChildItem { @((New-FakeFile "deskflow.exe"), (New-FakeFile "deskflow-core.exe"), (New-FakeFile "Qt6Core.dll")) }
  }

  It "fails when no thumbprint is configured" {
    $r = Test-Authenticode "" @("C:\Program Files\Deskflow")
    $r.check | Should -Be "authenticode"
    $r.status | Should -Be "FAIL"
    $r.detail | Should -Match "DESKFLOW_SIGN_THUMBPRINT not set"
  }

  It "passes when every binary is Valid with the fleet thumbprint (case-insensitive)" {
    Mock Get-AuthenticodeSignature { New-FakeSig "Valid" $script:Thumb.ToLower() }
    $r = Test-Authenticode $script:Thumb @("C:\Program Files\Deskflow")
    $r.status | Should -Be "PASS"
    $r.detail | Should -Match "3 binaries Valid"
  }

  It "fails on a NotSigned binary and names it" {
    Mock Get-AuthenticodeSignature {
      if ($LiteralPath -like "*Qt6Core.dll") { New-FakeSig "NotSigned" "" } else { New-FakeSig "Valid" $script:Thumb }
    }
    $r = Test-Authenticode $script:Thumb @("C:\Program Files\Deskflow")
    $r.status | Should -Be "FAIL"
    $r.detail | Should -Match "Qt6Core.dll: NotSigned"
    $r.detail | Should -Not -Match "deskflow.exe"
  }

  It "fails on a Valid signature from a different thumbprint" {
    Mock Get-AuthenticodeSignature { New-FakeSig "Valid" "DEADBEEF" }
    $r = Test-Authenticode $script:Thumb @("C:\Program Files\Deskflow")
    $r.status | Should -Be "FAIL"
    $r.detail | Should -Match "thumbprint DEADBEEF"
  }

  It "fails when the install roots contain no binaries" {
    Mock Get-ChildItem { @() }
    $r = Test-Authenticode $script:Thumb @("C:\Program Files\Deskflow")
    $r.status | Should -Be "FAIL"
    $r.detail | Should -Match "no \*\.exe/\*\.dll found"
  }
}

Describe "Test-Session" {
  It "passes with service RUNNING, GUI processes outside session 0 and an Active quser session" {
    Mock Invoke-ScQuery { "SERVICE_NAME: Deskflow`n        STATE              : 4  RUNNING" }
    Mock Invoke-Quser { " USERNAME  SESSIONNAME  ID  STATE   IDLE TIME`n alexh     console      1   Active  none" }
    Mock Get-Process { @([pscustomobject]@{ Name = $Name; SessionId = 1 }) }
    $r = Test-Session "Deskflow" @("deskflow", "Mouser")
    $r.status | Should -Be "PASS"
  }

  It "fails when the service is STOPPED" {
    Mock Invoke-ScQuery { "SERVICE_NAME: Deskflow`n        STATE              : 1  STOPPED" }
    Mock Invoke-Quser { "alexh console 1 Active" }
    Mock Get-Process { @([pscustomobject]@{ Name = $Name; SessionId = 1 }) }
    $r = Test-Session "Deskflow" @("deskflow")
    $r.status | Should -Be "FAIL"
    $r.detail | Should -Match "service Deskflow is STOPPED"
  }

  It "fails when a GUI process runs only in session 0" {
    Mock Invoke-ScQuery { "STATE : 4 RUNNING" }
    Mock Invoke-Quser { "alexh console 1 Active" }
    Mock Get-Process {
      if ($Name -eq "Mouser") { @([pscustomobject]@{ Name = "Mouser"; SessionId = 0 }) }
      else { @([pscustomobject]@{ Name = $Name; SessionId = 2 }) }
    }
    $r = Test-Session "Deskflow" @("deskflow", "Mouser")
    $r.status | Should -Be "FAIL"
    $r.detail | Should -Match "Mouser only in session 0"
    $r.detail | Should -Not -Match "deskflow not running"
  }

  It "fails when a GUI process is missing and quser has no Active session" {
    Mock Invoke-ScQuery { "STATE : 4 RUNNING" }
    Mock Invoke-Quser { "alexh rdp-tcp#0 2 Disc" }
    Mock Get-Process { @() }
    $r = Test-Session "Deskflow" @("deskflow")
    $r.status | Should -Be "FAIL"
    $r.detail | Should -Match "deskflow not running"
    $r.detail | Should -Match "no Active interactive session"
  }
}

Describe "Test-Mesh" {
  It "emits one PASS/FAIL per peer" {
    Mock Test-TcpPeer { param($Peer, $P) $Peer -eq "hackintosh" }
    $r = @(Test-Mesh @("hackintosh", "macbookpro") 24800)
    $r.Count | Should -Be 2
    ($r | Where-Object { $_.detail -match "hackintosh:24800" }).status | Should -Be "PASS"
    ($r | Where-Object { $_.detail -match "macbookpro:24800" }).status | Should -Be "FAIL"
  }

  It "skips with no peers" {
    $r = @(Test-Mesh @() 24800)
    $r[0].status | Should -Be "SKIP"
  }
}

Describe "Invoke-FleetHealth" {
  It "routes checks, skips macOS-only ones, and honours -Port for mesh" {
    Mock Test-Authenticode { New-Result "authenticode" "PASS" "ok" }
    Mock Test-Session { New-Result "session" "PASS" "ok" }
    Mock Test-TcpPeer { param($Peer, $P) $P -eq 24801 }
    $r = @(Invoke-FleetHealth -Checks "authenticode,session,mesh,tcc" -Thumbprint "X" -Peers "a,b" -Port 24801 `
      -InstallRoots @("C:\x") -ServiceName "Deskflow" -GuiProcesses @("deskflow"))
    ($r | ForEach-Object { $_.check }) | Should -Be @("authenticode", "session", "mesh", "mesh", "tcc")
    ($r | Where-Object { $_.check -eq "mesh" } | ForEach-Object { $_.status }) | Should -Be @("PASS", "PASS")
    ($r | Where-Object { $_.check -eq "tcc" }).status | Should -Be "SKIP"
  }

  It "emits a JSON array when run as a script" {
    # Drive the script through powershell so the main block runs; everything mocked away via
    # an empty install root (authenticode FAIL is expected) - we only assert the JSON envelope.
    $exe = if (Get-Command pwsh -ErrorAction SilentlyContinue) { "pwsh" } else { "powershell.exe" }
    $out = & $exe -NoProfile -ExecutionPolicy Bypass -File $script:Script -Checks authenticode -Thumbprint X `
      -InstallRoots (Join-Path $TestDrive "empty")
    $LASTEXITCODE | Should -Be 1
    $json = ($out | Out-String).Trim()
    $json | Should -Match '^\['
    $parsed = @($json | ConvertFrom-Json)
    $parsed[0].check | Should -Be "authenticode"
    $parsed[0].status | Should -Be "FAIL"
  }
}

Describe "Test-Instances" {
  It "passes when deskflow-ctl.ps1 assert-single exits cleanly" {
    Mock Test-Path { $true }
    Mock Invoke-CtlAssertSingle { @{ ok = $true; text = "deskflow-ctl assert-single: OK (daemon=1 session 0 pid 1000; core=1 child of service in session 1; gui=1; bridge=0)" } }
    $r = Test-Instances "C:\x\scripts\deskflow-ctl.ps1"
    $r.check | Should -Be "instances"
    $r.status | Should -Be "PASS"
    $r.detail | Should -Match "core=1"
  }

  It "fails with the ctl's problem list when assert-single throws" {
    Mock Invoke-CtlAssertSingle { @{ ok = $false; text = "deskflow-ctl assert-single: FAIL`n  deskflow-core.exe count=2 (want 1)`n  deskflow.exe count=0 (want 1)" } }
    $r = Test-Instances "C:\x\scripts\deskflow-ctl.ps1"
    $r.status | Should -Be "FAIL"
    $r.detail | Should -Match "deskflow-core.exe count=2"
    $r.detail | Should -Match "deskflow.exe count=0"
  }

  It "fails when the ctl script is missing" {
    $r = Test-Instances (Join-Path $TestDrive "nope\deskflow-ctl.ps1")
    $r.status | Should -Be "FAIL"
    $r.detail | Should -Match "missing"
  }

  It "is part of the default check set and of 'all'" {
    Mock Test-Authenticode { New-Result "authenticode" "PASS" "" }
    Mock Test-Session { New-Result "session" "PASS" "" }
    Mock Test-Mesh { @(New-Result "mesh" "SKIP" "no peers") }
    Mock Test-Instances { New-Result "instances" "PASS" "ok" }
    $r = Invoke-FleetHealth -Checks "all" -Thumbprint "" -Peers "" -Port 24800 -InstallRoots @() -ServiceName "Deskflow" -GuiProcesses @()
    @($r | ForEach-Object { $_.check }) | Should -Contain "instances"
    Should -Invoke Test-Instances -Times 1
  }
}
