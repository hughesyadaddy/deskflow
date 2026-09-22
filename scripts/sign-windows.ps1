#requires -Version 5.1
<#
.SYNOPSIS
  Authenticode-sign every .exe/.dll/.pyd/.sys under one or more roots with the fleet cert.
.DESCRIPTION
  Locates signtool.exe (PATH first, then the newest Windows 10 SDK under
  "C:\Program Files (x86)\Windows Kits\10\bin\<ver>\x64"), resolves the signing
  thumbprint (-Thumbprint, then $env:DESKFLOW_SIGN_THUMBPRINT, then
  DESKFLOW_SIGN_THUMBPRINT= in the repo .env), signs every *.exe, *.dll, *.pyd
  (PyInstaller/CPython extension modules, e.g. the Mouser dist) and *.sys under
  each -Root with

    signtool sign /sm /sha1 <tp> /fd SHA256 /td SHA256 /tr http://timestamp.digicert.com

  then verifies every file with Get-AuthenticodeSignature. Any file whose Status
  is not 'Valid' or whose signer thumbprint differs from the requested one is
  fatal.

  A timestamp-server failure is fatal by default: a signature without an RFC
  3161 timestamp stops validating the moment the certificate expires. Pass
  -AllowNoTimestamp to retry once without /tr (with a warning) for throwaway
  local builds only. Under a fleet deploy ($env:FLEET_DEPLOY = '1', set by
  scripts/fleet-deploy-windows.ps1) -AllowNoTimestamp is refused outright.

  The verify gate is pinned to the fleet certificate: every fleet-signed file
  must carry SignerCertificate.Thumbprint equal to the fleet thumbprint
  (-FleetThumbprint, then $env:DESKFLOW_FLEET_THUMBPRINT, then
  DESKFLOW_FLEET_THUMBPRINT= in scripts/fleet.env, default
  FBB49069A6C594E83714724217C7A5F54885FAEC -- the self-signed fleet cert) and
  a TimeStamperCertificate. The signing thumbprint must be that same cert.

  Nothing here is best-effort: a missing thumbprint, a missing signtool, a
  signtool failure or a verify failure all throw. The script never silently
  skips signing.
.PARAMETER Root
  One or more directories to sign recursively.
.PARAMETER Thumbprint
  SHA-1 thumbprint of a code-signing cert in Cert:\LocalMachine\My (or
  CurrentUser\My). Falls back to $env:DESKFLOW_SIGN_THUMBPRINT, then .env.
.PARAMETER VerifyOnly
  Skip signing; only run the Get-AuthenticodeSignature gate over every file.
  Used by build-windows.ps1 as a post-install check (the install root is
  already in use by the service at that point, so re-signing is not possible).
.PARAMETER AllowNoTimestamp
  If the timestamp server fails, retry once WITHOUT /tr instead of throwing.
  The resulting signature expires with the certificate; never use for
  fleet-deployed or released binaries. Throws when $env:FLEET_DEPLOY is '1'.
.PARAMETER FleetThumbprint
  SHA-1 thumbprint of the fleet code-signing certificate every fleet-signed
  file must verify against. Falls back to $env:DESKFLOW_FLEET_THUMBPRINT,
  then DESKFLOW_FLEET_THUMBPRINT= in scripts/fleet.env, then the built-in
  default (the self-signed fleet cert).
.EXAMPLE
  powershell scripts\sign-windows.ps1 -Root 'C:\Program Files\Deskflow'
  powershell scripts\sign-windows.ps1 -Root C:\Users\alexh\Desktop\Mouser\dist\Mouser -Thumbprint <sha1>
#>
[CmdletBinding()]
param(
  [string[]]$Root,
  [string]$Thumbprint,
  [string]$SignToolPath,
  [string]$EnvFile,
  [string]$TimestampUrl = 'http://timestamp.digicert.com',
  [string]$KitsBinRoot = 'C:\Program Files (x86)\Windows Kits\10\bin',
  [switch]$VerifyOnly,
  [switch]$AllowNoTimestamp,
  [string]$FleetThumbprint,
  [string]$FleetEnvFile
)

# The self-signed fleet certificate (user decision 2026-09-22: keep it, harden
# the gate around it). Override only via -FleetThumbprint /
# DESKFLOW_FLEET_THUMBPRINT when the fleet cert is rotated.
$script:DefaultFleetThumbprint = 'FBB49069A6C594E83714724217C7A5F54885FAEC'

# Extensions that carry Authenticode signatures and ship in a Deskflow or
# Mouser (PyInstaller) install root. Keep in sync with Get-SignTargets and
# the Pester fixture in tools/tests/SignWindows.Tests.ps1.
$script:SignExtensions = @('.exe', '.dll', '.pyd', '.sys')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Get-SignEnvFile {
  param([string]$Override)
  if ($Override) { return $Override }
  $scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
  return (Join-Path (Split-Path $scriptDir -Parent) '.env')
}

function Read-DotEnvValue {
  param([string]$Path, [string]$Key)
  if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $null }
  foreach ($line in (Get-Content -LiteralPath $Path)) {
    if ($line -match '^\s*#') { continue }
    if ($line -match ('^\s*' + [regex]::Escape($Key) + '\s*=\s*(.*)$')) {
      $value = $matches[1].Trim()
      # Strip one layer of matching quotes.
      if ($value -match '^"(.*)"$' -or $value -match "^'(.*)'$") { $value = $matches[1] }
      return $value
    }
  }
  return $null
}

function Get-FleetEnvFile {
  param([string]$Override)
  if ($Override) { return $Override }
  $scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
  return (Join-Path $scriptDir 'fleet.env')
}

function Resolve-FleetThumbprint {
  # The certificate every fleet-signed binary must verify against.
  param([string]$Explicit, [string]$FleetEnvPath)
  $raw = $null
  if ($Explicit) {
    $raw = $Explicit
  } elseif ($env:DESKFLOW_FLEET_THUMBPRINT) {
    $raw = $env:DESKFLOW_FLEET_THUMBPRINT
  } else {
    $fromFile = Read-DotEnvValue -Path $FleetEnvPath -Key 'DESKFLOW_FLEET_THUMBPRINT'
    if ($fromFile) { $raw = $fromFile }
  }
  if (-not $raw) { $raw = $script:DefaultFleetThumbprint }
  return (ConvertTo-NormalizedThumbprint $raw)
}

function Test-FleetDeploy {
  # True inside scripts/fleet-deploy-windows.ps1 (it exports FLEET_DEPLOY=1).
  return ("$($env:FLEET_DEPLOY)" -eq '1')
}

function ConvertTo-NormalizedThumbprint {
  param([string]$Value)
  if (-not $Value) { return $null }
  $clean = ($Value -replace '[\s:]', '').ToUpperInvariant()
  if ($clean -notmatch '^[0-9A-F]{40}$') {
    throw "Thumbprint '$Value' is not a 40-hex-digit SHA-1 thumbprint."
  }
  return $clean
}

function Resolve-SignThumbprint {
  param([string]$Explicit, [string]$EnvFilePath)
  $source = $null
  $raw = $null
  if ($Explicit) {
    $raw = $Explicit; $source = '-Thumbprint'
  } elseif ($env:DESKFLOW_SIGN_THUMBPRINT) {
    $raw = $env:DESKFLOW_SIGN_THUMBPRINT; $source = 'env:DESKFLOW_SIGN_THUMBPRINT'
  } else {
    $fromFile = Read-DotEnvValue -Path $EnvFilePath -Key 'DESKFLOW_SIGN_THUMBPRINT'
    if ($fromFile) { $raw = $fromFile; $source = ".env ($EnvFilePath)" }
  }
  if (-not $raw) {
    throw ('No signing thumbprint. Pass -Thumbprint, set DESKFLOW_SIGN_THUMBPRINT, ' +
      "or add DESKFLOW_SIGN_THUMBPRINT=<sha1> to $EnvFilePath. Refusing to skip signing.")
  }
  $tp = ConvertTo-NormalizedThumbprint $raw
  Write-Verbose "signing thumbprint $tp (from $source)"
  return $tp
}

function Find-SignTool {
  param([string]$Explicit, [string]$KitsRoot)
  if ($Explicit) {
    if (-not (Test-Path -LiteralPath $Explicit)) { throw "signtool not found at -SignToolPath '$Explicit'." }
    return (Resolve-Path -LiteralPath $Explicit).Path
  }
  $onPath = Get-Command 'signtool.exe' -ErrorAction SilentlyContinue
  if ($onPath) {
    $src = if ($onPath.PSObject.Properties['Source'] -and $onPath.Source) { $onPath.Source } else { $onPath.Path }
    if ($src) { return $src }
  }
  $candidates = @()
  if ($KitsRoot -and (Test-Path -LiteralPath $KitsRoot)) {
    $candidates = @(Get-ChildItem -LiteralPath $KitsRoot -Directory -ErrorAction SilentlyContinue |
      ForEach-Object {
        $exe = Join-Path $_.FullName 'x64\signtool.exe'
        if (Test-Path -LiteralPath $exe) {
          $ver = $null
          try { $ver = [version]$_.Name } catch { $ver = [version]'0.0' }
          [pscustomobject]@{ Version = $ver; Path = $exe }
        }
      } | Sort-Object Version -Descending)
  }
  if ($candidates.Count -gt 0) { return $candidates[0].Path }
  throw ("signtool.exe not found on PATH or under $KitsRoot\*\x64. " +
    'Install the Windows 10/11 SDK (Signing Tools) or pass -SignToolPath. Refusing to skip signing.')
}

function Get-SignTargets {
  param([string[]]$Roots)
  $files = @()
  foreach ($r in $Roots) {
    if (-not (Test-Path -LiteralPath $r -PathType Container)) {
      throw "Sign root '$r' does not exist or is not a directory."
    }
    # No -Include: it only filters when the path ends in a wildcard, so match
    # on the (case-insensitive) extension instead.
    $files += @(Get-ChildItem -LiteralPath $r -Recurse -File |
      Where-Object { $script:SignExtensions -contains $_.Extension.ToLowerInvariant() } |
      Select-Object -ExpandProperty FullName)
  }
  return @($files | Sort-Object -Unique)
}

function Invoke-SignTool {
  # Thin wrapper so tests can mock the native call. Returns exit code + output.
  param([string]$SignTool, [string[]]$Arguments)
  $prev = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $out = & $SignTool @Arguments 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $prev
  }
  return [pscustomobject]@{ ExitCode = $code; Output = ($out -join "`n") }
}

function Get-VendorSignedFiles {
  # Files that already carry a Valid signature from another publisher
  # (Microsoft dxil.dll, Qt, OpenSSL) are left alone: re-signing vendor
  # binaries buys nothing and the verify gate accepts them as Valid.
  param([string[]]$Files, [string]$Tp)
  $vendor = @()
  foreach ($f in $Files) {
    $sig = Get-AuthenticodeSignature -FilePath $f
    $cert = if ($sig.PSObject.Properties['SignerCertificate']) { $sig.SignerCertificate } else { $null }
    $signer = if ($cert) { "$($cert.Thumbprint)".ToUpperInvariant() } else { '' }
    if ("$($sig.Status)" -eq 'Valid' -and $signer -ne $Tp -and $signer -ne '') { $vendor += $f }
  }
  return $vendor
}

function Invoke-SignFiles {
  param([string]$SignTool, [string]$Tp, [string[]]$Files, [string]$Timestamp, [bool]$NoTimestampOk = $false)
  $vendor = @(Get-VendorSignedFiles -Files $Files -Tp $Tp)
  if ($vendor.Count -gt 0) {
    Write-Host "== leaving $($vendor.Count) vendor-signed file(s) untouched =="
    $Files = @($Files | Where-Object { $vendor -notcontains $_ })
  }
  if ($Files.Count -eq 0) { return }
  $base = @('sign', '/sm', '/sha1', $Tp, '/fd', 'SHA256')
  $withTs = $base + @('/td', 'SHA256', '/tr', $Timestamp)

  # One signtool invocation per batch keeps timestamp round-trips low. Keep
  # batches well under the ~32K command-line limit.
  $batch = @()
  $batchLen = 0
  $batches = @()
  foreach ($f in $Files) {
    if ($batch.Count -gt 0 -and ($batchLen + $f.Length + 3) -gt 24000) {
      $batches += , $batch; $batch = @(); $batchLen = 0
    }
    $batch += $f; $batchLen += $f.Length + 3
  }
  if ($batch.Count -gt 0) { $batches += , $batch }

  foreach ($b in $batches) {
    $res = Invoke-SignTool -SignTool $SignTool -Arguments ($withTs + $b)
    if ($res.ExitCode -ne 0 -and $res.Output -match '(?i)timestamp') {
      if (-not $NoTimestampOk) {
        # An untimestamped signature dies with the cert; refuse rather than
        # silently ship one. Retry the run (or fix the timestamp server).
        throw ("signtool sign failed: timestamp server $Timestamp unreachable or rejected " +
          "(exit $($res.ExitCode)). Refusing to sign without a timestamp; pass -AllowNoTimestamp " +
          "only for throwaway local builds.`n$($res.Output)")
      }
      Write-Warning ("timestamp server $Timestamp failed; -AllowNoTimestamp set, retrying once " +
        'WITHOUT a timestamp (signature will expire with the cert).')
      Write-Warning $res.Output
      $res = Invoke-SignTool -SignTool $SignTool -Arguments ($base + $b)
    }
    if ($res.ExitCode -ne 0) {
      throw "signtool sign failed (exit $($res.ExitCode)):`n$($res.Output)"
    }
  }
}

function Test-SignedFiles {
  # Every non-vendor file must be Valid, signed by the FLEET certificate
  # ($FleetTp; $Tp is the thumbprint we signed with and must be the same
  # cert) and carry an RFC 3161 timestamp (TimeStamperCertificate), so the
  # signature outlives the certificate. Vendor-signed files (Qt, Microsoft,
  # PyInstaller runtime) are accepted as Valid without those two checks.
  param([string[]]$Files, [string]$Tp, [string[]]$Vendor = @(), [string]$FleetTp = '')
  if (-not $FleetTp) { $FleetTp = $Tp }
  $bad = @()
  foreach ($f in $Files) {
    $sig = Get-AuthenticodeSignature -FilePath $f
    $status = "$($sig.Status)"
    $signer = $null
    $cert = if ($sig.PSObject.Properties['SignerCertificate']) { $sig.SignerCertificate } else { $null }
    if ($cert) { $signer = "$($cert.Thumbprint)".ToUpperInvariant() }
    $tsCert = if ($sig.PSObject.Properties['TimeStamperCertificate']) { $sig.TimeStamperCertificate } else { $null }
    $msg = if ($sig.PSObject.Properties['StatusMessage']) { $sig.StatusMessage } else { '' }
    if ($status -ne 'Valid') {
      $bad += "$f : status $status ($msg)"
    } elseif ($Vendor.Contains($f)) {
      continue
    } elseif ($signer -ne $FleetTp) {
      $bad += "$f : signed by $signer, expected fleet cert $FleetTp"
    } elseif ($null -eq $tsCert) {
      $bad += "$f : no RFC 3161 timestamp (TimeStamperCertificate missing); the signature dies with the cert"
    }
  }
  if ($bad.Count -gt 0) {
    throw ("Signature verification failed for $($bad.Count) file(s):`n  " + ($bad -join "`n  "))
  }
}

function Invoke-SignWindows {
  param(
    [string[]]$Roots,
    [string]$ThumbprintArg,
    [string]$SignToolArg,
    [string]$EnvFileArg,
    [string]$Timestamp,
    [string]$KitsRoot,
    [bool]$OnlyVerify,
    [bool]$NoTimestampOk = $false,
    [string]$FleetThumbprintArg = '',
    [string]$FleetEnvFileArg = ''
  )
  if (-not $Roots -or $Roots.Count -eq 0) { throw 'sign-windows.ps1: -Root is required.' }
  if ($NoTimestampOk -and (Test-FleetDeploy)) {
    throw ('sign-windows.ps1: -AllowNoTimestamp is refused under a fleet deploy (FLEET_DEPLOY=1): ' +
      'an untimestamped signature stops validating when the fleet cert expires. Fix the timestamp server instead.')
  }

  $envPath = Get-SignEnvFile -Override $EnvFileArg
  $tp = Resolve-SignThumbprint -Explicit $ThumbprintArg -EnvFilePath $envPath
  $fleetTp = Resolve-FleetThumbprint -Explicit $FleetThumbprintArg -FleetEnvPath (Get-FleetEnvFile -Override $FleetEnvFileArg)
  if ($tp -ne $fleetTp) {
    throw ("sign-windows.ps1: signing thumbprint $tp is not the fleet certificate $fleetTp " +
      '(DESKFLOW_SIGN_THUMBPRINT must be the fleet cert; override the fleet cert only via -FleetThumbprint / DESKFLOW_FLEET_THUMBPRINT when it is rotated).')
  }
  # @() guards against PowerShell unrolling an empty result to $null.
  $files = @(Get-SignTargets -Roots $Roots)
  if ($files.Count -eq 0) {
    throw "No .exe/.dll/.pyd/.sys files found under: $($Roots -join ', ')"
  }

  if (-not $OnlyVerify) {
    $signtool = Find-SignTool -Explicit $SignToolArg -KitsRoot $KitsRoot
    Write-Host "== Signing $($files.Count) file(s) with $tp via $signtool =="
    Invoke-SignFiles -SignTool $signtool -Tp $tp -Files $files -Timestamp $Timestamp -NoTimestampOk $NoTimestampOk
  } else {
    Write-Host "== Verifying signatures on $($files.Count) file(s) against $tp =="
  }

  $vendor = @(Get-VendorSignedFiles -Files $files -Tp $tp)
  Test-SignedFiles -Files $files -Tp $tp -Vendor $vendor -FleetTp $fleetTp
  Write-Host "== Signature OK: $($files.Count - $vendor.Count) file(s) signed + timestamped by fleet cert $fleetTp, $($vendor.Count) vendor-signed =="
  return $files
}

# Dot-source (". scripts\sign-windows.ps1") loads the functions without running,
# which is how the Pester tests exercise it.
if ($MyInvocation.InvocationName -ne '.') {
  $null = Invoke-SignWindows -Roots $Root -ThumbprintArg $Thumbprint -SignToolArg $SignToolPath `
    -EnvFileArg $EnvFile -Timestamp $TimestampUrl -KitsRoot $KitsBinRoot -OnlyVerify ([bool]$VerifyOnly) `
    -NoTimestampOk ([bool]$AllowNoTimestamp) -FleetThumbprintArg $FleetThumbprint -FleetEnvFileArg $FleetEnvFile
}
