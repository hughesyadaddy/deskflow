#requires -Modules @{ ModuleName = 'Pester'; ModuleVersion = '5.0' }
<#
  Pester tests for scripts/fleet-deploy-windows.ps1.

  The script runs its deploy at top level, so it is not dot-sourced. Instead
  the AST is parsed to assert structural guarantees (every git call goes
  through the exit-code-checked wrapper, every native call is checked, Mouser
  output is signed, build-mouser.bat is not rewritten), and the helper
  functions are lifted out of the AST and exercised with a fake `git`.
#>

BeforeAll {
  $script:ScriptPath = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'scripts\fleet-deploy-windows.ps1'
  $script:ScriptPath = $script:ScriptPath -replace '\\', [IO.Path]::DirectorySeparatorChar
  if (-not (Test-Path $script:ScriptPath)) { throw "fleet-deploy-windows.ps1 not found at $script:ScriptPath" }

  $tokens = $null; $errors = $null
  $script:Ast = [System.Management.Automation.Language.Parser]::ParseFile($script:ScriptPath, [ref]$tokens, [ref]$errors)
  $script:ParseErrors = @($errors)
  $script:Text = Get-Content -LiteralPath $script:ScriptPath -Raw

  $script:Functions = @{}
  foreach ($fn in $script:Ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
    $script:Functions[$fn.Name] = $fn
  }

  function Get-CommandCalls {
    param([System.Management.Automation.Language.Ast]$Node, [string]$Name)
    @($Node.FindAll({
        $a = $args[0]
        $a -is [System.Management.Automation.Language.CommandAst] -and $a.GetCommandName() -eq $Name
      }, $true))
  }

  # Lift Invoke-Git / Invoke-Native into this scope so they can be exercised.
  Invoke-Expression $script:Functions['Invoke-Git'].Extent.Text
  Invoke-Expression $script:Functions['Invoke-Native'].Extent.Text
}

Describe 'fleet-deploy-windows.ps1 structure' {
  It 'parses without errors' {
    $script:ParseErrors.Count | Should -Be 0
  }

  It 'defines the expected functions' {
    foreach ($n in 'Invoke-Git', 'Invoke-Native', 'Sync-DeskflowRepo', 'Deploy-Deskflow', 'Sync-MouserRepo', 'Deploy-Mouser') {
      $script:Functions.Keys | Should -Contain $n
    }
  }

  It 'routes every git call through Invoke-Git (exit-code checked)' {
    $bare = Get-CommandCalls -Node $script:Ast -Name 'git'
    # Allowed: the single `& git` inside Invoke-Git itself, and the read-only
    # `git remote` probe in Sync-MouserRepo.
    $outside = @($bare | Where-Object {
        $inInvokeGit = $script:Functions['Invoke-Git'].Extent.StartOffset -le $_.Extent.StartOffset -and
          $_.Extent.EndOffset -le $script:Functions['Invoke-Git'].Extent.EndOffset
        $isRemoteProbe = ($_.CommandElements.Count -ge 2) -and ("$($_.CommandElements[1])" -eq 'remote') -and
          ($_.CommandElements.Count -eq 2)
        -not $inInvokeGit -and -not $isRemoteProbe
      })
    $outside | ForEach-Object { $_.Extent.Text } | Should -BeNullOrEmpty
    (Get-CommandCalls -Node $script:Ast -Name 'Invoke-Git').Count | Should -BeGreaterOrEqual 8
  }

  It 'Invoke-Git throws on a non-zero exit code' {
    $body = $script:Functions['Invoke-Git'].Body.Extent.Text
    $body | Should -Match '\$LASTEXITCODE'
    $body | Should -Match 'throw'
  }

  It 'pulls Deskflow with fetch / checkout / pull --ff-only on $Branch' {
    $sync = $script:Functions['Sync-DeskflowRepo'].Extent.Text
    $sync | Should -Match 'Invoke-Git fetch origin'
    $sync | Should -Match 'Invoke-Git checkout \$Branch'
    $sync | Should -Match 'Invoke-Git pull --ff-only origin \$Branch'
  }

  It 'pulls Mouser from the fork remote, adding it when missing, with --ff-only' {
    $sync = $script:Functions['Sync-MouserRepo'].Extent.Text
    $sync | Should -Match "Invoke-Git remote add fork"
    $sync | Should -Match 'Invoke-Git fetch fork'
    $sync | Should -Match 'Invoke-Git checkout \$MouserBranch'
    $sync | Should -Match 'Invoke-Git pull --ff-only fork \$MouserBranch'
    $sync | Should -Not -Match '\|\| true'
  }

  It 'checks $LASTEXITCODE after the Deskflow build/install' {
    $script:Functions['Deploy-Deskflow'].Extent.Text | Should -Match 'build-windows\.ps1[^\n]*-Install[\s\S]*if \(\$LASTEXITCODE -ne 0\)[^\n]*throw'
  }

  It 'runs the Mouser build (bat and python paths) through the exit-code-checked wrapper' {
    $deploy = $script:Functions['Deploy-Mouser']
    $native = Get-CommandCalls -Node $deploy -Name 'Invoke-Native'
    $native.Count | Should -BeGreaterOrEqual 4
    ($native | ForEach-Object { $_.Extent.Text }) -join "`n" | Should -Match 'build-mouser\.bat'
    ($native | ForEach-Object { $_.Extent.Text }) -join "`n" | Should -Match 'build_and_install\.py'
    ($native | ForEach-Object { $_.Extent.Text }) -join "`n" | Should -Match 'pip'
    # No unchecked direct invocations of cmd / python remain in Deploy-Mouser.
    (Get-CommandCalls -Node $deploy -Name 'cmd').Count | Should -Be 0
    $ampCalls = @($deploy.FindAll({
        $a = $args[0]
        $a -is [System.Management.Automation.Language.CommandAst] -and $a.InvocationOperator -eq 'Ampersand'
      }, $true))
    # The only `&` call left in Deploy-Mouser is the sign-windows.ps1 invocation
    # (a .ps1 that throws on failure rather than setting an exit code).
    ($ampCalls | ForEach-Object { $_.Extent.Text }) | Should -Not -BeNullOrEmpty
    foreach ($c in $ampCalls) { $c.Extent.Text | Should -Match 'sign-windows\.ps1' }
  }

  It 'signs the Mouser dist output with sign-windows.ps1 after the build' {
    $deploy = $script:Functions['Deploy-Mouser'].Extent.Text
    $deploy | Should -Match "\`$dist = Join-Path \`$MouserRoot 'dist\\Mouser'"
    $deploy | Should -Match "sign-windows\.ps1'\) -Root \`$dist"
    # Signing must come after the build calls.
    $deploy.IndexOf('sign-windows.ps1') | Should -BeGreaterThan $deploy.IndexOf('build_and_install.py')
    $deploy.IndexOf('sign-windows.ps1') | Should -BeGreaterThan $deploy.IndexOf('build-mouser.bat')
  }

  It 'does not rewrite build-mouser.bat itself and only bootstraps when something is missing' {
    $script:Text | Should -Not -Match 'Set-Content[^\n]*build-mouser\.bat'
    $script:Text | Should -Not -Match 'Out-File[^\n]*build-mouser\.bat'
    $script:Text | Should -Match '\$needsBootstrap'
    $script:Text | Should -Match "if \(\`$DeployMouser -eq 1 -and \`$needsBootstrap -and \(Test-Path \`$sync\)\)"
  }

  It 'contains no "|| true"-style swallowing of failures' {
    $script:Text | Should -Not -Match '\|\|\s*true'
    $script:Text | Should -Not -Match '-ErrorAction\s+SilentlyContinue[^\n]*git'
  }
}

Describe 'Invoke-Git behaviour' {
  BeforeEach {
    $global:GitCalls = @()
  }

  It 'passes arguments through and succeeds on exit 0' {
    function global:git {
      $global:GitCalls += , @($args)
      $global:LASTEXITCODE = 0
    }
    try {
      $ErrorActionPreference = 'Stop'
      { Invoke-Git fetch origin } | Should -Not -Throw
      { Invoke-Git pull --ff-only origin main } | Should -Not -Throw
      $global:GitCalls.Count | Should -Be 2
      ($global:GitCalls[1] -join ' ') | Should -Be 'pull --ff-only origin main'
    } finally {
      Remove-Item Function:\global:git -ErrorAction SilentlyContinue
    }
  }

  It 'throws with the failing command when git exits non-zero' {
    function global:git {
      $global:GitCalls += , @($args)
      $global:LASTEXITCODE = 128
    }
    try {
      { Invoke-Git checkout nope } | Should -Throw -ExpectedMessage '*git checkout nope failed (exit 128)*'
    } finally {
      Remove-Item Function:\global:git -ErrorAction SilentlyContinue
    }
  }
}

Describe 'Invoke-Native behaviour' {
  It 'throws with the label when the native command exits non-zero' {
    function global:fake-native { $global:LASTEXITCODE = 3 }
    try {
      { Invoke-Native -Label 'Mouser build' -FilePath 'fake-native' -ArgumentList @('a') } |
        Should -Throw -ExpectedMessage '*Mouser build failed (exit 3)*'
    } finally {
      Remove-Item Function:\global:fake-native -ErrorAction SilentlyContinue
    }
  }

  It 'does not throw on exit 0' {
    function global:fake-native { $global:LASTEXITCODE = 0 }
    try {
      { Invoke-Native -Label 'ok' -FilePath 'fake-native' } | Should -Not -Throw
    } finally {
      Remove-Item Function:\global:fake-native -ErrorAction SilentlyContinue
    }
  }
}
