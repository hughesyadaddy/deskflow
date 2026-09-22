#requires -Modules @{ ModuleName = 'Pester'; ModuleVersion = '5.0' }
<#
  Pester tests for scripts/sign-windows.ps1.

  signtool and Get-AuthenticodeSignature are mocked, so these run on any
  platform with pwsh + Pester 5 (tools/tests/run.sh skips *.Tests.ps1 when
  pwsh is absent). Run:  Invoke-Pester tools/tests/SignWindows.Tests.ps1
#>

BeforeAll {
  $script:ScriptPath = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'scripts\sign-windows.ps1'
  $script:ScriptPath = $script:ScriptPath -replace '\\', [IO.Path]::DirectorySeparatorChar
  if (-not (Test-Path $script:ScriptPath)) { throw "sign-windows.ps1 not found at $script:ScriptPath" }

  # Get-AuthenticodeSignature only exists on Windows; provide a stub so Pester
  # has a command to mock elsewhere.
  if (-not (Get-Command Get-AuthenticodeSignature -ErrorAction SilentlyContinue)) {
    function Get-AuthenticodeSignature { param([string]$FilePath) throw 'stub: should be mocked' }
  }

  . $script:ScriptPath

  $script:Tp = 'ABCDEF0123456789ABCDEF0123456789ABCDEF01'

  function New-FixtureTree {
    param([string]$Base)
    $dirs = @($Base, (Join-Path $Base 'sub'), (Join-Path $Base 'sub/deep'))
    foreach ($d in $dirs) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
    $files = @(
      (Join-Path $Base 'deskflow-core.exe'),
      (Join-Path $Base 'Qt6Core.dll'),
      (Join-Path $Base 'sub/deskflow-vhid-bridge.exe'),
      (Join-Path $Base 'sub/deep/plugin.DLL'),
      # PyInstaller dist layout: CPython extension modules and a driver.
      (Join-Path $Base 'sub/_internal/_ssl.pyd'),
      (Join-Path $Base 'sub/_internal/hidapi.sys'),
      (Join-Path $Base 'README.txt'),
      (Join-Path $Base 'sub/config.json'),
      (Join-Path $Base 'sub/_internal/base_library.zip'),
      (Join-Path $Base 'sub/_internal/module.pyc')
    )
    foreach ($f in $files) {
      New-Item -ItemType Directory -Force -Path (Split-Path $f -Parent) | Out-Null
      Set-Content -Path $f -Value 'x'
    }
    return @($files | Where-Object { $_ -match '(?i)\.(exe|dll|pyd|sys)$' } | ForEach-Object { (Resolve-Path $_).Path })
  }

  function New-FakeKits {
    param([string]$Base, [string[]]$Versions)
    foreach ($v in $Versions) {
      $d = Join-Path (Join-Path $Base $v) 'x64'
      New-Item -ItemType Directory -Force -Path $d | Out-Null
      Set-Content -Path (Join-Path $d 'signtool.exe') -Value 'fake'
    }
    return $Base
  }

  function Get-ValidSig {
    param([string]$Thumb = $script:Tp, [bool]$Timestamped = $true)
    [pscustomobject]@{
      Status = 'Valid'; StatusMessage = 'Signature verified.'
      SignerCertificate = [pscustomobject]@{ Thumbprint = $Thumb }
      TimeStamperCertificate = $(if ($Timestamped) { [pscustomobject]@{ Subject = 'CN=DigiCert Timestamp 2024' } } else { $null })
    }
  }
}

Describe 'sign-windows.ps1' {
  BeforeEach {
    $script:SavedEnvTp = $env:DESKFLOW_SIGN_THUMBPRINT
    $env:DESKFLOW_SIGN_THUMBPRINT = $null
    # The fixtures sign with $script:Tp, so make it the fleet cert for the
    # suite; the fleet-cert tests below override this per case.
    $script:SavedFleetTp = $env:DESKFLOW_FLEET_THUMBPRINT
    $env:DESKFLOW_FLEET_THUMBPRINT = $script:Tp
    $script:SavedFleetDeploy = $env:FLEET_DEPLOY
    $env:FLEET_DEPLOY = $null
    $script:Root = Join-Path $TestDrive 'install'
    $script:Expected = New-FixtureTree -Base $script:Root
    $script:Kits = New-FakeKits -Base (Join-Path $TestDrive 'kits') -Versions @('10.0.19041.0', '10.0.26100.0', '10.0.22621.0')
    $script:EmptyKits = Join-Path $TestDrive 'nokits'
    New-Item -ItemType Directory -Force -Path $script:EmptyKits | Out-Null
    $script:MissingEnv = Join-Path $TestDrive 'does-not-exist.env'
    Mock Get-Command { $null } -ParameterFilter { $Name -eq 'signtool.exe' }
    Mock Invoke-SignTool { [pscustomobject]@{ ExitCode = 0; Output = 'Successfully signed' } }
    Mock Get-AuthenticodeSignature { Get-ValidSig }
  }
  AfterEach {
    $env:DESKFLOW_SIGN_THUMBPRINT = $script:SavedEnvTp
    $env:DESKFLOW_FLEET_THUMBPRINT = $script:SavedFleetTp
    $env:FLEET_DEPLOY = $script:SavedFleetDeploy
  }

  Context 'fleet certificate' {
    It 'defaults the fleet thumbprint to the self-signed fleet cert when nothing is configured' {
      $env:DESKFLOW_FLEET_THUMBPRINT = $null
      Resolve-FleetThumbprint -Explicit '' -FleetEnvPath $script:MissingEnv | Should -Be 'FBB49069A6C594E83714724217C7A5F54885FAEC'
    }

    It 'reads DESKFLOW_FLEET_THUMBPRINT from scripts/fleet.env (normalized) and prefers -FleetThumbprint' {
      $env:DESKFLOW_FLEET_THUMBPRINT = $null
      $fleetEnv = Join-Path $TestDrive 'fleet.env'
      Set-Content -Path $fleetEnv -Value @('FLEET_BRANCH=main', "DESKFLOW_FLEET_THUMBPRINT=$($script:Tp.ToLowerInvariant())")
      Resolve-FleetThumbprint -Explicit '' -FleetEnvPath $fleetEnv | Should -Be $script:Tp
      Resolve-FleetThumbprint -Explicit '0000000000000000000000000000000000000000' -FleetEnvPath $fleetEnv |
        Should -Be '0000000000000000000000000000000000000000'
    }

    It 'throws when the signing thumbprint is not the fleet cert (never signs)' {
      $env:DESKFLOW_FLEET_THUMBPRINT = '0000000000000000000000000000000000000000'
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false -FleetEnvFileArg $script:MissingEnv } |
        Should -Throw -ExpectedMessage '*is not the fleet certificate*'
      Should -Invoke Invoke-SignTool -Times 0 -Exactly
    }

    It 'refuses -AllowNoTimestamp under FLEET_DEPLOY=1 before touching anything' {
      $env:FLEET_DEPLOY = '1'
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false -NoTimestampOk $true } |
        Should -Throw -ExpectedMessage '*-AllowNoTimestamp is refused under a fleet deploy*'
      Should -Invoke Invoke-SignTool -Times 0 -Exactly
      Should -Invoke Get-AuthenticodeSignature -Times 0 -Exactly
    }

    It 'still allows -AllowNoTimestamp outside a fleet deploy' {
      $env:FLEET_DEPLOY = '0'
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false -NoTimestampOk $true 3>$null } |
        Should -Not -Throw
    }

    It 'throws when a fleet-signed file carries no RFC 3161 timestamp' {
      $script:Bad = $script:Expected[2]
      Mock Get-AuthenticodeSignature {
        if ($FilePath -eq $script:Bad) { Get-ValidSig -Timestamped $false } else { Get-ValidSig }
      }
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false } |
        Should -Throw -ExpectedMessage '*no RFC 3161 timestamp*'
    }

    It 'accepts a vendor-signed file without a timestamp' {
      $script:Vendor = $script:Expected[1]
      Mock Get-AuthenticodeSignature {
        if ($FilePath -eq $script:Vendor) { Get-ValidSig -Thumb '1111111111111111111111111111111111111111' -Timestamped $false } else { Get-ValidSig }
      }
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false } |
        Should -Not -Throw
      Should -Invoke Invoke-SignTool -Times 0 -Exactly -ParameterFilter { $Arguments -contains $script:Vendor }
    }
  }

  Context 'thumbprint resolution' {
    It 'throws when no thumbprint is available anywhere (never silently skips)' {
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg '' -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false } |
        Should -Throw -ExpectedMessage '*thumbprint*'
      Should -Invoke Invoke-SignTool -Times 0 -Exactly
    }

    It 'reads DESKFLOW_SIGN_THUMBPRINT from the environment' {
      $env:DESKFLOW_SIGN_THUMBPRINT = $script:Tp.ToLowerInvariant()
      Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg '' -SignToolArg '' `
        -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false | Out-Null
      Should -Invoke Invoke-SignTool -Times 1 -Exactly -ParameterFilter { $Arguments -ccontains $script:Tp }
    }

    It 'falls back to DESKFLOW_SIGN_THUMBPRINT= in .env and normalizes spaces/colons/case' {
      $envFile = Join-Path $TestDrive 'test.env'
      $spaced = ($script:Tp.ToLowerInvariant() -replace '(..)', '$1 ').Trim()
      Set-Content -Path $envFile -Value @(
        '# comment', 'OTHER=1', "DESKFLOW_SIGN_THUMBPRINT=`"$spaced`""
      )
      Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg '' -SignToolArg '' `
        -EnvFileArg $envFile -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false | Out-Null
      Should -Invoke Invoke-SignTool -Times 1 -Exactly -ParameterFilter { $Arguments -ccontains $script:Tp }
    }

    It 'rejects a malformed thumbprint' {
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg 'not-a-thumbprint' -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false } |
        Should -Throw -ExpectedMessage '*40-hex*'
    }
  }

  Context 'signtool discovery' {
    It 'throws when signtool is neither on PATH nor under the Windows Kits root' {
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:EmptyKits -OnlyVerify $false } |
        Should -Throw -ExpectedMessage '*signtool.exe not found*'
      Should -Invoke Invoke-SignTool -Times 0 -Exactly
    }

    It 'prefers signtool from PATH when present' {
      $script:OnPath = Join-Path $TestDrive 'onpath-signtool.exe'
      Set-Content -Path $script:OnPath -Value 'fake'
      Mock Get-Command { [pscustomobject]@{ Source = $script:OnPath; Path = $script:OnPath } } -ParameterFilter { $Name -eq 'signtool.exe' }
      Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
        -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false | Out-Null
      Should -Invoke Invoke-SignTool -Times 1 -Exactly -ParameterFilter { $SignTool -eq $script:OnPath }
    }

    It 'picks the newest SDK version under the Windows Kits root' {
      Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
        -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false | Out-Null
      Should -Invoke Invoke-SignTool -Times 1 -Exactly -ParameterFilter { $SignTool -like '*10.0.26100.0*signtool.exe' }
    }

    It 'throws when -SignToolPath points at a missing file' {
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg (Join-Path $TestDrive 'nope.exe') `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false } |
        Should -Throw -ExpectedMessage '*signtool not found*'
    }
  }

  Context 'signing' {
    It 'signs every .exe/.dll/.pyd/.sys under the root (and nothing else) with the required flags' {
      $signed = Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
        -EnvFileArg $script:MissingEnv -Timestamp 'http://timestamp.digicert.com' -KitsRoot $script:Kits -OnlyVerify $false

      $script:Expected.Count | Should -Be 6
      @($signed).Count | Should -Be 6
      foreach ($f in $script:Expected) {
        $script:Cur = $f
        $signed | Should -Contain $f
        Should -Invoke Invoke-SignTool -Times 1 -Exactly -ParameterFilter { $Arguments -contains $script:Cur }
      }
      # G3 regression: PyInstaller extension modules (.pyd) and drivers (.sys) must be signed.
      @($signed | Where-Object { $_ -like '*_ssl.pyd' }).Count | Should -Be 1
      @($signed | Where-Object { $_ -like '*hidapi.sys' }).Count | Should -Be 1
      Should -Invoke Invoke-SignTool -Times 0 -Exactly -ParameterFilter {
        ($Arguments -join ' ') -match 'README\.txt|config\.json|base_library\.zip|module\.pyc'
      }
      Should -Invoke Invoke-SignTool -Times 1 -Exactly -ParameterFilter {
        $Arguments[0] -eq 'sign' -and
        ($Arguments -join ' ') -like "*/sha1 $($script:Tp) /fd SHA256 /td SHA256 /tr http://timestamp.digicert.com*"
      }
      # Every signed file is verified.
      foreach ($f in $script:Expected) {
        $script:Cur = $f
        Should -Invoke Get-AuthenticodeSignature -Times 1 -Exactly -ParameterFilter { $FilePath -eq $script:Cur }
      }
    }

    It 'signs multiple roots in one run' {
      $root2 = Join-Path $TestDrive 'mouser'
      $expected2 = New-FixtureTree -Base $root2
      $signed = Invoke-SignWindows -Roots @($script:Root, $root2) -ThumbprintArg $script:Tp -SignToolArg '' `
        -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false
      @($signed).Count | Should -Be 12
      foreach ($f in ($script:Expected + $expected2)) { $signed | Should -Contain $f }
    }

    It 'throws when a root has no binaries' {
      $empty = Join-Path $TestDrive 'empty'
      New-Item -ItemType Directory -Force -Path $empty | Out-Null
      { Invoke-SignWindows -Roots @($empty) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false } |
        Should -Throw -ExpectedMessage '*No .exe/.dll*'
    }

    It 'throws when a root does not exist' {
      { Invoke-SignWindows -Roots @((Join-Path $TestDrive 'missing')) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false } |
        Should -Throw -ExpectedMessage '*does not exist*'
    }

    It 'is fatal by default when the timestamp server fails (never retries without /tr)' {
      # G5 regression: an untimestamped signature dies with the cert.
      Mock Invoke-SignTool {
        if ($Arguments -contains '/tr') {
          [pscustomobject]@{ ExitCode = 1; Output = 'SignTool Error: The specified timestamp server either could not be reached or returned an invalid response.' }
        } else {
          [pscustomobject]@{ ExitCode = 0; Output = 'Successfully signed' }
        }
      }
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false } |
        Should -Throw -ExpectedMessage '*Refusing to sign without a timestamp*AllowNoTimestamp*'
      Should -Invoke Invoke-SignTool -Times 1 -Exactly
      Should -Invoke Invoke-SignTool -Times 0 -Exactly -ParameterFilter { $Arguments -notcontains '/tr' }
      # Nothing is reported as verified when signing aborted.
      Should -Invoke Get-AuthenticodeSignature -Times 0 -Exactly
    }

    It 'retries once without /tr and warns when the timestamp server fails and -AllowNoTimestamp is set' {
      Mock Invoke-SignTool {
        if ($Arguments -contains '/tr') {
          [pscustomobject]@{ ExitCode = 1; Output = 'SignTool Error: The specified timestamp server either could not be reached or returned an invalid response.' }
        } else {
          [pscustomobject]@{ ExitCode = 0; Output = 'Successfully signed' }
        }
      }
      $warnings = @()
      $out = Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
        -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false -NoTimestampOk $true 3>&1
      $warnings = @($out | Where-Object { $_ -is [System.Management.Automation.WarningRecord] })
      $warnings.Count | Should -BeGreaterOrEqual 1
      ($warnings | ForEach-Object { $_.Message }) -join "`n" | Should -Match 'timestamp'
      Should -Invoke Invoke-SignTool -Times 2 -Exactly
      Should -Invoke Invoke-SignTool -Times 1 -Exactly -ParameterFilter { $Arguments -contains '/tr' }
      Should -Invoke Invoke-SignTool -Times 1 -Exactly -ParameterFilter {
        ($Arguments -notcontains '/tr') -and ($Arguments -notcontains '/td') -and ($Arguments -contains '/sha1')
      }
    }

    It 'throws (without retry) on a non-timestamp signtool failure' {
      Mock Invoke-SignTool { [pscustomobject]@{ ExitCode = 1; Output = 'SignTool Error: No certificates were found that met all the given criteria.' } }
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false } |
        Should -Throw -ExpectedMessage '*signtool sign failed*'
      Should -Invoke Invoke-SignTool -Times 1 -Exactly
    }

    It 'throws when the -AllowNoTimestamp retry also fails' {
      Mock Invoke-SignTool { [pscustomobject]@{ ExitCode = 1; Output = 'SignTool Error: timestamp server unreachable' } }
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false -NoTimestampOk $true 3>$null } |
        Should -Throw -ExpectedMessage '*signtool sign failed*'
      Should -Invoke Invoke-SignTool -Times 2 -Exactly
    }

    It 'matches signable extensions case-insensitively' {
      $root2 = Join-Path $TestDrive 'caps'
      New-Item -ItemType Directory -Force -Path $root2 | Out-Null
      foreach ($n in @('A.EXE', 'b.Dll', 'c.PYD', 'd.SYS', 'e.TXT')) { Set-Content -Path (Join-Path $root2 $n) -Value 'x' }
      $signed = Invoke-SignWindows -Roots @($root2) -ThumbprintArg $script:Tp -SignToolArg '' `
        -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false
      @($signed).Count | Should -Be 4
      @($signed | Where-Object { $_ -like '*e.TXT' }).Count | Should -Be 0
    }
  }

  Context 'verification' {
    It 'throws when any file is not Valid after signing' {
      $script:Bad = $script:Expected[1]
      Mock Get-AuthenticodeSignature {
        if ($FilePath -eq $script:Bad) {
          [pscustomobject]@{ Status = 'NotSigned'; StatusMessage = 'The file is not digitally signed.'; SignerCertificate = $null }
        } else { Get-ValidSig }
      }
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false } |
        Should -Throw -ExpectedMessage '*Signature verification failed*NotSigned*'
    }

    It 'throws when a file is Valid but signed by a different certificate' {
      $script:Bad = $script:Expected[0]
      Mock Get-AuthenticodeSignature {
        if ($FilePath -eq $script:Bad) { Get-ValidSig -Thumb '0000000000000000000000000000000000000000' } else { Get-ValidSig }
      }
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false } |
        Should -Throw -ExpectedMessage '*signed by 0000000000000000000000000000000000000000, expected*'
    }

    It 'accepts a signer thumbprint that differs only in case' {
      Mock Get-AuthenticodeSignature { Get-ValidSig -Thumb $script:Tp.ToLowerInvariant() }
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false } |
        Should -Not -Throw
    }

    It '-VerifyOnly never calls signtool but still verifies every file' {
      Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
        -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:EmptyKits -OnlyVerify $true | Out-Null
      Should -Invoke Invoke-SignTool -Times 0 -Exactly
      Should -Invoke Get-AuthenticodeSignature -Times 6 -Exactly
    }

    It '-VerifyOnly throws on an unsigned file' {
      Mock Get-AuthenticodeSignature { [pscustomobject]@{ Status = 'NotSigned'; StatusMessage = ''; SignerCertificate = $null } }
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:EmptyKits -OnlyVerify $true } |
        Should -Throw -ExpectedMessage '*Signature verification failed*'
    }
  }

  Context 'entry point' {
    It 'throws when -Root is omitted' {
      { Invoke-SignWindows -Roots @() -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false } |
        Should -Throw -ExpectedMessage '*-Root is required*'
    }
  }
}
