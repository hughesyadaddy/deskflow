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

    signtool sign /sha1 <tp> /fd SHA256 /td SHA256 /tr http://timestamp.digicert.com

  then verifies every file with Get-AuthenticodeSignature. Any file whose Status
  is not 'Valid' or whose signer thumbprint differs from the requested one is
  fatal.

  A timestamp-server failure is fatal by default: a signature without an RFC
  3161 timestamp stops validating the moment the certificate expires. Pass
  -AllowNoTimestamp to retry once without /tr (with a warning) for throwaway
  local builds only.

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
  fleet-deployed or released binaries.
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
  [switch]$AllowNoTimestamp
)

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

function Invoke-SignFiles {
  param([string]$SignTool, [string]$Tp, [string[]]$Files, [string]$Timestamp, [bool]$NoTimestampOk = $false)
  if ($Files.Count -eq 0) { return }
  $base = @('sign', '/sha1', $Tp, '/fd', 'SHA256')
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
  param([string[]]$Files, [string]$Tp)
  $bad = @()
  foreach ($f in $Files) {
    $sig = Get-AuthenticodeSignature -FilePath $f
    $status = "$($sig.Status)"
    $signer = $null
    $cert = if ($sig.PSObject.Properties['SignerCertificate']) { $sig.SignerCertificate } else { $null }
    if ($cert) { $signer = "$($cert.Thumbprint)".ToUpperInvariant() }
    $msg = if ($sig.PSObject.Properties['StatusMessage']) { $sig.StatusMessage } else { '' }
    if ($status -ne 'Valid') {
      $bad += "$f : status $status ($msg)"
    } elseif ($signer -ne $Tp) {
      $bad += "$f : signed by $signer, expected $Tp"
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
    [bool]$NoTimestampOk = $false
  )
  if (-not $Roots -or $Roots.Count -eq 0) { throw 'sign-windows.ps1: -Root is required.' }

  $envPath = Get-SignEnvFile -Override $EnvFileArg
  $tp = Resolve-SignThumbprint -Explicit $ThumbprintArg -EnvFilePath $envPath
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

  Test-SignedFiles -Files $files -Tp $tp
  Write-Host "== Signature OK: $($files.Count) file(s) signed by $tp =="
  return $files
}

# Dot-source (". scripts\sign-windows.ps1") loads the functions without running,
# which is how the Pester tests exercise it.
if ($MyInvocation.InvocationName -ne '.') {
  $null = Invoke-SignWindows -Roots $Root -ThumbprintArg $Thumbprint -SignToolArg $SignToolPath `
    -EnvFileArg $EnvFile -Timestamp $TimestampUrl -KitsRoot $KitsBinRoot -OnlyVerify ([bool]$VerifyOnly) `
    -NoTimestampOk ([bool]$AllowNoTimestamp)
}
