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
      (Join-Path $Base 'README.txt'),
      (Join-Path $Base 'sub/config.json')
    )
    foreach ($f in $files) { Set-Content -Path $f -Value 'x' }
    return @($files | Where-Object { $_ -match '\.(exe|dll)$' } | ForEach-Object { (Resolve-Path $_).Path })
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
    param([string]$Thumb = $script:Tp)
    [pscustomobject]@{
      Status = 'Valid'; StatusMessage = 'Signature verified.'
      SignerCertificate = [pscustomobject]@{ Thumbprint = $Thumb }
    }
  }
}

Describe 'sign-windows.ps1' {
  BeforeEach {
    $script:SavedEnvTp = $env:DESKFLOW_SIGN_THUMBPRINT
    $env:DESKFLOW_SIGN_THUMBPRINT = $null
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
    It 'signs every .exe and .dll under the root (and nothing else) with the required flags' {
      $signed = Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
        -EnvFileArg $script:MissingEnv -Timestamp 'http://timestamp.digicert.com' -KitsRoot $script:Kits -OnlyVerify $false

      $script:Expected.Count | Should -Be 4
      @($signed).Count | Should -Be 4
      foreach ($f in $script:Expected) {
        $script:Cur = $f
        $signed | Should -Contain $f
        Should -Invoke Invoke-SignTool -Times 1 -Exactly -ParameterFilter { $Arguments -contains $script:Cur }
      }
      Should -Invoke Invoke-SignTool -Times 0 -Exactly -ParameterFilter { ($Arguments -join ' ') -match 'README\.txt|config\.json' }
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
      @($signed).Count | Should -Be 8
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

    It 'retries once without /tr and warns when the timestamp server fails' {
      Mock Invoke-SignTool {
        if ($Arguments -contains '/tr') {
          [pscustomobject]@{ ExitCode = 1; Output = 'SignTool Error: The specified timestamp server either could not be reached or returned an invalid response.' }
        } else {
          [pscustomobject]@{ ExitCode = 0; Output = 'Successfully signed' }
        }
      }
      $warnings = @()
      $out = Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
        -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false 3>&1
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

    It 'throws when the timestamp retry also fails' {
      Mock Invoke-SignTool { [pscustomobject]@{ ExitCode = 1; Output = 'SignTool Error: timestamp server unreachable' } }
      { Invoke-SignWindows -Roots @($script:Root) -ThumbprintArg $script:Tp -SignToolArg '' `
          -EnvFileArg $script:MissingEnv -Timestamp 'http://ts' -KitsRoot $script:Kits -OnlyVerify $false 3>$null } |
        Should -Throw -ExpectedMessage '*signtool sign failed*'
      Should -Invoke Invoke-SignTool -Times 2 -Exactly
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
      Should -Invoke Get-AuthenticodeSignature -Times 4 -Exactly
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
