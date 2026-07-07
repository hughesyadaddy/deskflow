#requires -Version 5.1
<#
.SYNOPSIS
  Quit Deskflow, install the Release build to Program Files, restart service + GUI.
.DESCRIPTION
  Stops every Deskflow process (any path), removes rogue install copies, copies the
  windeployqt-staged build from build/bin/Release into DESKFLOW_INSTALL_DIR,
  registers the Deskflow service, starts it once, and launches a single deskflow.exe
  from the canonical install directory.

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

$script:DeskflowProcessNames = @(
  'deskflow', 'deskflow-core', 'deskflow-daemon', 'deskflow-vhid-bridge'
)

function Invoke-TaskKill {
  param([string]$ImageName)
  # taskkill writes to stderr on failure; must not trip $ErrorActionPreference = 'Stop'
  cmd.exe /c "taskkill /F /T /IM `"$ImageName`" >nul 2>&1"
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

function Get-DeskflowProcesses {
  Get-CimInstance Win32_Process -Filter "Name LIKE 'deskflow%'" -ErrorAction SilentlyContinue
}

function Stop-ProcessTree {
  param([int]$ProcessId)
  # /T tree-kills watchdog-spawned children; elevated /F reaches SYSTEM (session 0).
  cmd.exe /c "taskkill /F /T /PID $ProcessId >nul 2>&1"
  Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue
}

function Remove-DeskflowService {
  # The daemon is a watchdog: it respawns deskflow-core. It MUST be stopped and
  # removed before killing cores, or the killed core is immediately relaunched.
  $svc = Get-CimInstance Win32_Service -Filter "Name='Deskflow'" -ErrorAction SilentlyContinue
  if (-not $svc) { return }

  if ($svc.State -eq 'Running') {
    Stop-Service -Name Deskflow -Force -ErrorAction SilentlyContinue
  }

  # Wait for the SCM to report Stopped; force-kill the daemon PID if it hangs so
  # the watchdog thread cannot spawn another core.
  $deadline = (Get-Date).AddSeconds(10)
  while ((Get-Date) -lt $deadline) {
    $s = Get-Service -Name Deskflow -ErrorAction SilentlyContinue
    if (-not $s -or $s.Status -eq 'Stopped') { break }
    Start-Sleep -Milliseconds 500
  }
  $running = Get-CimInstance Win32_Service -Filter "Name='Deskflow'" -ErrorAction SilentlyContinue
  if ($running -and $running.ProcessId -gt 0) {
    Write-Host "  force-killing daemon service PID $($running.ProcessId)"
    Stop-ProcessTree -ProcessId $running.ProcessId
  }

  sc.exe stop Deskflow 2>$null | Out-Null
  sc.exe delete Deskflow 2>$null | Out-Null

  # Wait until the service is fully removed before continuing so a re-create
  # later cannot collide with a delete that is still pending.
  $deadline = (Get-Date).AddSeconds(10)
  while ((Get-Date) -lt $deadline) {
    if (-not (Get-Service -Name Deskflow -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Milliseconds 500
  }
}

function Stop-DeskflowAll {
  Write-Host '== Stopping Deskflow service and all processes =='

  Remove-DeskflowService

  # Kill every Deskflow process in EVERY session. A stale core can run as SYSTEM
  # in session 0 (watchdog/secure-desktop) alongside a user-session core, because
  # the single-instance guard uses per-session namespaces. Tree-kill by PID so
  # both die regardless of session.
  $deadline = (Get-Date).AddSeconds(25)
  while ((Get-Date) -lt $deadline) {
    $procs = @(Get-DeskflowProcesses)
    if ($procs.Count -eq 0) { break }

    foreach ($proc in $procs) {
      Write-Host "  killing PID $($proc.ProcessId) $($proc.Name) session=$($proc.SessionId) ($($proc.ExecutablePath))"
      Stop-ProcessTree -ProcessId $proc.ProcessId
    }

    foreach ($name in $script:DeskflowProcessNames) {
      Invoke-TaskKill "$name.exe"
    }

    Start-Sleep -Milliseconds 750
  }

  $remaining = @(Get-DeskflowProcesses)
  if ($remaining.Count -gt 0) {
    $detail = ($remaining | ForEach-Object { "$($_.Name) pid=$($_.ProcessId) session=$($_.SessionId) path=$($_.ExecutablePath)" }) -join '; '
    throw "Could not stop all Deskflow processes: $detail"
  }

  Write-Host '== All Deskflow processes stopped =='
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

  Stop-DeskflowAll
  foreach ($rogue in $Paths) {
    Write-Host "Removing rogue install: $rogue"
    Remove-Item -LiteralPath $rogue -Recurse -Force -ErrorAction SilentlyContinue
  }
}

function Ensure-DeskflowService {
  param([string]$DaemonPath)

  if (-not (Test-Path $DaemonPath)) {
    throw "deskflow-daemon.exe not found at $DaemonPath"
  }

  $binPath = "`"$DaemonPath`""
  if (Get-Service -Name Deskflow -ErrorAction SilentlyContinue) {
    Write-Host '== Updating Deskflow Windows service =='
    sc.exe config Deskflow binPath= $binPath start= auto | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc.exe config failed ($LASTEXITCODE)" }
  } else {
    Write-Host '== Creating Deskflow Windows service =='
    sc.exe create Deskflow binPath= $binPath start= auto DisplayName= "Deskflow" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc.exe create failed ($LASTEXITCODE)" }
  }
}

function Start-DeskflowService {
  $svc = Get-Service -Name Deskflow -ErrorAction SilentlyContinue
  if (-not $svc) { return }

  if ($svc.Status -eq 'Running') {
    Write-Host '== Deskflow service already running; restarting =='
    Restart-Service Deskflow -Force
  } else {
    Write-Host '== Starting Deskflow service =='
    Start-Service Deskflow
  }
  Write-Host ("== Service status: " + (Get-Service Deskflow).Status + " ==")
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

function Get-InteractiveSession {
  # The session that owns explorer.exe is the interactive console session.
  # Returns @{ SessionId; User } or $null when nobody is logged in.
  $explorer = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue |
    Sort-Object SessionId | Select-Object -First 1
  if (-not $explorer) { return $null }
  $owner = Invoke-CimMethod -InputObject $explorer -MethodName GetOwner -ErrorAction SilentlyContinue
  if (-not $owner -or -not $owner.User) { return $null }
  $user = if ($owner.Domain) { "$($owner.Domain)\$($owner.User)" } else { $owner.User }
  [pscustomobject]@{ SessionId = [int]$explorer.SessionId; User = $user }
}

function Start-GuiInSession {
  # Start-Process from a session-0 (service/SSH) context lands the GUI in session 0,
  # invisible to the user and prone to spawning a duplicate core. An interactive
  # scheduled task launches into the user's active console session instead.
  param([string]$Gui, [string]$WorkDir, [string]$User)

  $taskName = 'DeskflowInstallLaunch'
  Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
  # No --show: deploys should restart the GUI silently to tray, not pop the window.
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

function Start-DeskflowGui {
  param([string]$InstallRoot)

  $gui = Join-Path $InstallRoot 'deskflow.exe'
  if (-not (Test-Path $gui)) { throw "deskflow.exe not found at $gui" }

  $canonicalGui = [System.IO.Path]::GetFullPath($gui).ToLowerInvariant()

  foreach ($proc in @(Get-DeskflowProcesses)) {
    $path = if ($proc.ExecutablePath) { $proc.ExecutablePath.ToLowerInvariant() } else { '' }
    if ($proc.Name -ieq 'deskflow.exe' -and $path -eq $canonicalGui) {
      Write-Host "== deskflow.exe already running from $InstallRoot; not launching a second GUI =="
      return
    }
    if ($proc.Name -ieq 'deskflow.exe') {
      Write-Host "  stopping extra GUI PID $($proc.ProcessId) ($($proc.ExecutablePath))"
      Stop-ProcessTree -ProcessId $proc.ProcessId
    }
  }

  Start-Sleep -Seconds 1
  if (Get-Process -Name deskflow -ErrorAction SilentlyContinue) {
    throw 'deskflow.exe still running after cleanup; refusing to launch another instance.'
  }

  $mySession = [System.Diagnostics.Process]::GetCurrentProcess().SessionId
  $interactive = Get-InteractiveSession

  if ($interactive -and $interactive.SessionId -ne $mySession) {
    Write-Host "== Launching GUI in active session $($interactive.SessionId) as $($interactive.User) =="
    Start-GuiInSession -Gui $gui -WorkDir $InstallRoot -User $interactive.User
  } elseif ($mySession -ne 0) {
    Write-Host "== Launching single GUI (tray): $gui =="
    Start-Process -FilePath $gui -WorkingDirectory $InstallRoot
  } else {
    Write-Host '== No interactive session; GUI will start at next login (Run key) =='
  }
}

function Ensure-DeskflowSigning {
  # UIAccess (letting the core reach elevated windows so an elevated PowerToys
  # doesn't block its input) requires the core exe to be Authenticode-signed by
  # a cert that chains to a trusted root, and installed under Program Files.
  # For a private fleet we self-sign with a machine-local cert and trust it in
  # LocalMachine\Root + TrustedPublisher. No CA, no cost, Secure Boot untouched.
  param([string[]]$Paths)

  $subject = 'CN=Deskflow Fleet Code Signing'
  $cert = Get-ChildItem Cert:\LocalMachine\My -CodeSigningCert -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -eq $subject } | Sort-Object NotAfter -Descending | Select-Object -First 1

  if (-not $cert) {
    Write-Host "== Creating self-signed Deskflow fleet code-signing cert =="
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
      -CertStoreLocation Cert:\LocalMachine\My -KeyUsage DigitalSignature `
      -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(10)
  }

  # Trust the cert so the signature verifies (root) and is an allowed publisher.
  foreach ($store in @('Root', 'TrustedPublisher')) {
    $path = "Cert:\LocalMachine\$store"
    $exists = Get-ChildItem $path -ErrorAction SilentlyContinue | Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
    if (-not $exists) {
      $store2 = New-Object System.Security.Cryptography.X509Certificates.X509Store($store, 'LocalMachine')
      $store2.Open('ReadWrite')
      $store2.Add($cert)
      $store2.Close()
      Write-Host "  trusted fleet cert in LocalMachine\$store"
    }
  }

  foreach ($p in $Paths) {
    if (-not (Test-Path $p)) { continue }
    $res = Set-AuthenticodeSignature -FilePath $p -Certificate $cert -HashAlgorithm SHA256
    if ($res.Status -ne 'Valid') {
      throw "failed to sign $p (status: $($res.Status))"
    }
    Write-Host "  signed $(Split-Path $p -Leaf)"
  }
}

function Assert-CanonicalRuntime {
  param([string]$InstallDir)

  $canonical = [System.IO.Path]::GetFullPath($InstallDir).ToLowerInvariant()
  $foreign = @(Get-DeskflowProcesses | Where-Object {
      $_.ExecutablePath -and
      (-not ($_.ExecutablePath.ToLowerInvariant().StartsWith($canonical)))
    })

  if ($foreign.Count -gt 0) {
    $detail = ($foreign | ForEach-Object { "$($_.Name) $($_.ExecutablePath)" }) -join '; '
    throw "Deskflow still running from non-canonical paths: $detail"
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

Stop-DeskflowAll

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

$daemon = Join-Path $InstallDir 'deskflow-daemon.exe'
$gui = Join-Path $InstallDir 'deskflow.exe'
$core = Join-Path $InstallDir 'deskflow-core.exe'

# Sign the installed binaries so the core's UIAccess manifest bit is honored
# (Windows silently ignores UIAccess on an unsigned or non-Program-Files exe).
Ensure-DeskflowSigning -Paths @($core, $gui, $daemon)

Ensure-DeskflowService -DaemonPath $daemon
Set-DeskflowRunRegistry -GuiPath $gui
Start-DeskflowService

if (-not $NoRestart) {
  Start-DeskflowGui -InstallRoot $InstallDir
}

Assert-CanonicalRuntime -InstallDir $InstallDir

Write-Host "== Done: single install at $InstallDir =="
$svcPath = (Get-CimInstance Win32_Service -Filter "Name='Deskflow'" -ErrorAction SilentlyContinue).PathName
Write-Host "== Service: $svcPath =="
} finally {
  if ($TranscriptPath) { Stop-Transcript | Out-Null }
}
