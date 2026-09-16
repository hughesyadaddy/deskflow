<#
.SYNOPSIS
  Windows-side collector for tools/fleet-health. Invoked over SSH; emits JSON.

.DESCRIPTION
  Emits a JSON array of { check, status, detail } objects, one per check
  (mesh emits one per peer). Status is PASS | FAIL | SKIP. Never throws on a
  failed check; a thrown error means the collector itself is broken.

  Checks:
    authenticode  every *.exe/*.dll under "C:\Program Files\Deskflow" and
                  "C:\Program Files\Mouser" has Get-AuthenticodeSignature
                  Status Valid and SignerCertificate.Thumbprint equal to the
                  fleet thumbprint (-Thumbprint, else $env:DESKFLOW_SIGN_THUMBPRINT,
                  else DESKFLOW_SIGN_THUMBPRINT from the repo .env / scripts/fleet.env).
    session       `sc query Deskflow` reports RUNNING; deskflow and Mouser
                  processes exist with SessionId -ne 0; quser shows an Active session.
    mesh          Test-NetConnection to each -Peers entry on -Port succeeds.
    instances     scripts\deskflow-ctl.ps1 assert-single exits 0: exactly one
                  daemon (session 0, == service PID), one service-owned core and
                  one GUI in the console session, all from the install root,
                  no bridge, nothing from any other path.
    bridge        the local Mouser bridge on 127.0.0.1:-BridgePort (19795) answers
                  a {"t":"status"} line with attached:true and peer:"deskflow-core"
                  (.NET TcpClient; 2 s connect + read timeout).

.EXAMPLE
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\fleet-health.ps1 -Checks authenticode,session
#>
[CmdletBinding()]
param(
  [string]$Checks = "authenticode,session,mesh,instances",
  [string]$Thumbprint = "",
  [string]$Peers = "",
  [int]$Port = 24800,
  [string[]]$InstallRoots = @("C:\Program Files\Deskflow", "C:\Program Files\Mouser"),
  [string]$ServiceName = "Deskflow",
  [string[]]$GuiProcesses = @("deskflow", "Mouser"),
  [string]$Ctl = "",
  [int]$BridgePort = 19795
)

Set-StrictMode -Version 2
$ErrorActionPreference = "Continue"

function New-Result([string]$Check, [string]$Status, [string]$Detail) {
  [pscustomobject]@{ check = $Check; status = $Status; detail = $Detail }
}

function Get-EnvValueFromFile([string]$Path, [string]$Key) {
  if (-not (Test-Path -LiteralPath $Path)) { return "" }
  foreach ($line in Get-Content -LiteralPath $Path) {
    $t = $line.Trim()
    if ($t -eq "" -or $t.StartsWith("#")) { continue }
    if ($t.StartsWith("export ")) { $t = $t.Substring(7).Trim() }
    $idx = $t.IndexOf("=")
    if ($idx -lt 1) { continue }
    if ($t.Substring(0, $idx).Trim() -ne $Key) { continue }
    $v = $t.Substring($idx + 1).Trim()
    if ($v.Length -ge 2 -and (($v[0] -eq '"' -and $v[-1] -eq '"') -or ($v[0] -eq "'" -and $v[-1] -eq "'"))) {
      $v = $v.Substring(1, $v.Length - 2)
    }
    return $v
  }
  return ""
}

function Resolve-Thumbprint([string]$Explicit) {
  if ($Explicit) { return $Explicit }
  if ($env:DESKFLOW_SIGN_THUMBPRINT) { return $env:DESKFLOW_SIGN_THUMBPRINT }
  $root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
  foreach ($f in @((Join-Path $root ".env"), (Join-Path $root "scripts\fleet.env"))) {
    $v = Get-EnvValueFromFile $f "DESKFLOW_SIGN_THUMBPRINT"
    if ($v) { return $v }
  }
  return ""
}

# --- checks -------------------------------------------------------------------

function Test-Authenticode([string]$Thumb, [string[]]$Roots) {
  if (-not $Thumb) {
    return New-Result "authenticode" "FAIL" "DESKFLOW_SIGN_THUMBPRINT not set (param, env, .env, scripts/fleet.env)"
  }
  $Thumb = $Thumb.ToUpperInvariant()
  $files = @()
  foreach ($r in $Roots) {
    if (Test-Path -LiteralPath $r) {
      $files += Get-ChildItem -LiteralPath $r -Recurse -File -Include *.exe, *.dll -ErrorAction SilentlyContinue
    }
  }
  if ($files.Count -eq 0) {
    return New-Result "authenticode" "FAIL" ("no *.exe/*.dll found under " + ($Roots -join ", "))
  }
  $bad = @()
  foreach ($f in $files) {
    $sig = Get-AuthenticodeSignature -LiteralPath $f.FullName
    $actual = if ($sig.SignerCertificate) { $sig.SignerCertificate.Thumbprint.ToUpperInvariant() } else { "" }
    if ($sig.Status -ne "Valid") {
      $bad += ("{0}: {1}" -f $f.Name, $sig.Status)
    } elseif ($actual -ne $Thumb) {
      $bad += ("{0}: thumbprint {1}" -f $f.Name, ($(if ($actual) { $actual } else { "none" })))
    }
  }
  if ($bad.Count -gt 0) {
    return New-Result "authenticode" "FAIL" ($bad -join "; ")
  }
  New-Result "authenticode" "PASS" ("{0} binaries Valid with thumbprint {1}" -f $files.Count, $Thumb)
}

function Invoke-ScQuery([string]$Service) { & sc.exe query $Service 2>&1 | Out-String }
function Invoke-Quser { & quser.exe 2>&1 | Out-String }
function Test-TcpPeer([string]$Peer, [int]$P) {
  $ok = $false
  try {
    $client = New-Object System.Net.Sockets.TcpClient
    $ar = $client.BeginConnect($Peer, $P, $null, $null)
    $ok = $ar.AsyncWaitHandle.WaitOne(3000, $false) -and $client.Connected
    $client.Close()
  } catch { $ok = $false }
  if (-not $ok -and (Get-Command Test-NetConnection -ErrorAction SilentlyContinue)) {
    $ok = [bool](Test-NetConnection -ComputerName $Peer -Port $P -InformationLevel Quiet -WarningAction SilentlyContinue)
  }
  $ok
}

function Test-Session([string]$Service, [string[]]$Procs) {
  $problems = @()
  $sc = Invoke-ScQuery $Service
  if ($sc -notmatch "RUNNING") {
    $state = if ($sc -match "STATE\s*:\s*\d+\s+(\w+)") { $Matches[1] } else { "not found" }
    $problems += ("service {0} is {1}" -f $Service, $state)
  }
  foreach ($p in $Procs) {
    $running = @(Get-Process -Name $p -ErrorAction SilentlyContinue)
    if ($running.Count -eq 0) {
      $problems += ("{0} not running" -f $p)
    } elseif (-not ($running | Where-Object { $_.SessionId -ne 0 })) {
      $problems += ("{0} only in session 0" -f $p)
    }
  }
  $q = Invoke-Quser
  if ($q -notmatch "\bActive\b") {
    $problems += "quser: no Active interactive session"
  }
  if ($problems.Count -gt 0) {
    return New-Result "session" "FAIL" ($problems -join "; ")
  }
  New-Result "session" "PASS" ("service {0} RUNNING; {1} in interactive session; quser Active" -f $Service, ($Procs -join "+"))
}

function Resolve-Ctl([string]$Explicit) {
  if ($Explicit) { return $Explicit }
  $root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
  return (Join-Path $root "scripts\deskflow-ctl.ps1")
}

function Invoke-CtlAssertSingle([string]$CtlPath) {
  # Returns @{ ok; text }. The ctl throws on any mismatch; the exception text
  # carries the problem list.
  if (-not (Test-Path -LiteralPath $CtlPath)) {
    return @{ ok = $false; text = "deskflow-ctl.ps1 missing at $CtlPath" }
  }
  try {
    $out = & $CtlPath assert-single 2>&1 | Out-String
    return @{ ok = $true; text = $out.Trim() }
  } catch {
    return @{ ok = $false; text = ("" + $_.Exception.Message).Trim() }
  }
}

function Test-Instances([string]$CtlPath) {
  $r = Invoke-CtlAssertSingle $CtlPath
  $text = ($r.text -split "`r?`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ }) -join "; "
  if ($r.ok) {
    return New-Result "instances" "PASS" $text
  }
  New-Result "instances" "FAIL" $text
}

function Invoke-BridgeStatus([int]$P) {
  # Sends {"t":"status"} to the local Mouser bridge and returns the first reply
  # line, or "" when nothing is listening / nothing answers within 2 s.
  $line = ""
  try {
    $client = New-Object System.Net.Sockets.TcpClient
    $ar = $client.BeginConnect("127.0.0.1", $P, $null, $null)
    if (-not ($ar.AsyncWaitHandle.WaitOne(2000, $false) -and $client.Connected)) {
      $client.Close()
      return ""
    }
    $client.EndConnect($ar)
    $stream = $client.GetStream()
    $stream.ReadTimeout = 2000
    $stream.WriteTimeout = 2000
    $bytes = [System.Text.Encoding]::UTF8.GetBytes("{`"t`":`"status`"}`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
    $line = $reader.ReadLine()
    if ($null -eq $line) { $line = "" }
    $client.Close()
  } catch { $line = "" }
  $line
}

function Test-BridgeReply([string]$Reply) {
  # Returns @{ ok; detail } for one status line; mirrors parse_bridge_status in tools/fleet-health.
  $text = ("" + $Reply).Trim()
  if (-not $text) {
    return @{ ok = $false; detail = "no reply from bridge (Mouser not listening or status unsupported)" }
  }
  try { $obj = $text | ConvertFrom-Json } catch { $obj = $null }
  if ($null -eq $obj) {
    return @{ ok = $false; detail = ("unparseable bridge reply: " + $text.Substring(0, [Math]::Min(120, $text.Length))) }
  }
  $problems = @()
  $attached = if ($obj.PSObject.Properties["attached"]) { $obj.attached } else { $null }
  $peer = if ($obj.PSObject.Properties["peer"]) { $obj.peer } else { $null }
  if ($attached -isnot [bool] -or -not $attached) {
    $problems += ("attached={0} (want true)" -f ($(if ($null -eq $attached) { "null" } else { "$attached".ToLowerInvariant() })))
  }
  if ($peer -ne "deskflow-core") {
    $problems += ("peer={0} (want 'deskflow-core')" -f ($(if ($null -eq $peer) { "null" } else { "`"$peer`"" })))
  }
  if ($problems.Count -gt 0) {
    return @{ ok = $false; detail = ($problems -join "; ") }
  }
  $extra = ""
  foreach ($k in @("session", "proto", "ver")) {
    if ($obj.PSObject.Properties[$k]) { $extra += (" {0}={1}" -f $k, $obj.$k) }
  }
  @{ ok = $true; detail = ("attached to deskflow-core" + $extra) }
}

function Test-Bridge([int]$P) {
  $r = Test-BridgeReply (Invoke-BridgeStatus $P)
  New-Result "bridge" ($(if ($r.ok) { "PASS" } else { "FAIL" })) $r.detail
}

function Test-Mesh([string[]]$PeerList, [int]$P) {
  $out = @()
  if ($PeerList.Count -eq 0) {
    return @(New-Result "mesh" "SKIP" "no peers")
  }
  foreach ($peer in $PeerList) {
    $ok = Test-TcpPeer $peer $P
    $detail = "{0} -> {1}:{2} {3}" -f $env:COMPUTERNAME, $peer, $P, ($(if ($ok) { "ok" } else { "unreachable" }))
    $out += New-Result "mesh" ($(if ($ok) { "PASS" } else { "FAIL" })) $detail
  }
  $out
}

# --- main ---------------------------------------------------------------------

function Invoke-FleetHealth {
  param([string]$Checks, [string]$Thumbprint, [string]$Peers, [int]$Port,
        [string[]]$InstallRoots, [string]$ServiceName, [string[]]$GuiProcesses, [string]$Ctl = "",
        [int]$BridgePort = 19795)
  $results = @()
  $wanted = @($Checks.Split(",") | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ })
  if ($wanted -contains "all") { $wanted = @("authenticode", "session", "mesh", "instances", "bridge") }
  foreach ($c in $wanted) {
    switch ($c) {
      "authenticode" { $results += Test-Authenticode (Resolve-Thumbprint $Thumbprint) $InstallRoots }
      "session"      { $results += Test-Session $ServiceName $GuiProcesses }
      "mesh"         { $results += Test-Mesh @($Peers.Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ }) $Port }
      "instances"    { $results += Test-Instances (Resolve-Ctl $Ctl) }
      "bridge"       { $results += Test-Bridge $BridgePort }
      default        { $results += New-Result $c "SKIP" "macOS-only check" }
    }
  }
  ,$results
}

if ($MyInvocation.InvocationName -ne ".") {
  $r = Invoke-FleetHealth -Checks $Checks -Thumbprint $Thumbprint -Peers $Peers -Port $Port `
    -InstallRoots $InstallRoots -ServiceName $ServiceName -GuiProcesses $GuiProcesses -Ctl $Ctl -BridgePort $BridgePort
  # Always emit a JSON *array*, even for a single result.
  $json = ConvertTo-Json -InputObject @($r) -Depth 3 -Compress
  if (-not $json.StartsWith("[")) { $json = "[" + $json + "]" }
  Write-Output $json
  if (@($r | Where-Object { $_.status -eq "FAIL" }).Count -gt 0) { exit 1 } else { exit 0 }
}
