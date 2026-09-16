# Pester 5 tests for scripts/fleet-deploy.ps1 (the Windows-seat controller).
# No real hosts: the native seams (Get-FleetHostname, Invoke-Ssh, Invoke-Native,
# Invoke-FleetHealth, Test-ProcessAlive) are mocked.
#
# Run:  Invoke-Pester tools/tests/FleetDeploy.Tests.ps1

BeforeAll {
  $src = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'scripts\fleet-deploy.ps1'
  $script:Repo = Join-Path $TestDrive 'repo'
  New-Item -ItemType Directory -Path (Join-Path $script:Repo 'scripts') -Force | Out-Null
  New-Item -ItemType Directory -Path (Join-Path $script:Repo 'tools') -Force | Out-Null
  Copy-Item $src (Join-Path $script:Repo 'scripts\fleet-deploy.ps1')
  Set-Content (Join-Path $script:Repo 'scripts\fleet.env') @"
FLEET_BRANCH=main
FLEET_HOSTS="hackintosh macbookpro tiny11"
# "local" must NOT make hackintosh local - this seat is tiny11.
FLEET_SSH_hackintosh=local
FLEET_SSH_macbookpro=macbookpro
FLEET_SSH_tiny11=tiny11
FLEET_SSH_USER_hackintosh=alex
FLEET_SSH_USER_macbookpro=alexhughes
FLEET_DEPLOY_DESKFLOW=1
FLEET_DEPLOY_MOUSER=1
FLEET_DESKFLOW_PATH_windows=$($script:Repo -replace '\\', '/')
FLEET_MOUSER_PATH_windows=C:/Users/alexh/Desktop/Mouser
"@
  # Dot-sourcing loads the functions and sets $script:FleetRoot to the temp repo.
  . (Join-Path $script:Repo 'scripts\fleet-deploy.ps1')
  $script:StateDir = Join-Path $script:Repo 'tools\state'
  $script:LastGood = Join-Path $script:StateDir 'last-good.json'
  # `--json -` writes to the console; the controller keeps the last document in $script:LastJson.
  function Invoke-DryRunJson { Invoke-FleetDeploy @('--dry-run', '--json', '-') | Out-Null; return ($script:LastJson | ConvertFrom-Json) }
}

BeforeEach {
  $script:SshLog = @()
  $script:NativeLog = @()
  if (Test-Path $script:StateDir) { Remove-Item -Recurse -Force $script:StateDir }
  Remove-Item Env:FLEET_LOCAL_ID -ErrorAction SilentlyContinue
  Mock Get-FleetHostname { 'TINY11' }
  Mock Invoke-Ssh {
    $script:SshLog += [pscustomobject]@{ Target = $Target; Command = $Command }
    if ($Command -like '*rev-parse*') { return @{ Code = 0; Output = "cafe0000$Target`n" } }
    return @{ Code = 0; Output = '' }
  }
  Mock Invoke-Native {
    $script:NativeLog += [pscustomobject]@{ Exe = $Exe; Args = ($ArgList -join ' ') }
    if (($ArgList -join ' ') -like '*rev-parse*') { return @{ Code = 0; Output = "beef000000000000000000000000000000000001`n" } }
    return @{ Code = 0; Output = '' }
  }
  Mock Invoke-FleetHealth { -1 }
}

Describe 'planning (--dry-run --json -)' {
  It 'has the {hosts:[{id,target,order,role}]} shape with three distinct targets' {
    $j = Invoke-DryRunJson
    $j.hosts.Count | Should -Be 3
    foreach ($h in $j.hosts) { foreach ($k in 'id', 'target', 'order', 'role') { $h.PSObject.Properties[$k] | Should -Not -BeNullOrEmpty } }
    @($j.hosts | ForEach-Object { $_.order }) | Should -Be @(1, 2, 3)
    @($j.hosts | ForEach-Object { $_.target } | Sort-Object -Unique).Count | Should -Be 3
  }

  It 'derives LOCAL_ID from the hostname, case-insensitively, never from FLEET_SSH_x=local' {
    $j = (Invoke-DryRunJson)
    ($j.hosts | Where-Object id -eq 'tiny11').target | Should -Be 'local'
    ($j.hosts | Where-Object id -eq 'hackintosh').target | Should -Be 'alex@hackintosh'
    ($j.hosts | Where-Object id -eq 'macbookpro').target | Should -Be 'alexhughes@macbookpro'
  }

  It 'puts the server last (default hackintosh)' {
    $j = (Invoke-DryRunJson)
    $j.hosts[-1].id | Should -Be 'hackintosh'
    $j.hosts[-1].role | Should -Be 'server'
  }

  It 'honours FLEET_ROLE_<id>=server' {
    Add-Content (Join-Path $script:Repo 'scripts\fleet.env') 'FLEET_ROLE_macbookpro=server'
    try {
      $j = (Invoke-DryRunJson)
      $j.hosts[-1].id | Should -Be 'macbookpro'
      ($j.hosts | Where-Object id -eq 'hackintosh').role | Should -Be 'client'
    } finally {
      $f = Join-Path $script:Repo 'scripts\fleet.env'
      Set-Content $f ((Get-Content $f) | Where-Object { $_ -ne 'FLEET_ROLE_macbookpro=server' })
    }
  }

  It 'errors when the hostname matches no FLEET_HOSTS entry, unless FLEET_LOCAL_ID overrides' {
    Mock Get-FleetHostname { 'STRANGER' }
    { Invoke-FleetDeploy @('--dry-run', '--json', '-') } | Should -Throw '*no FLEET_HOSTS entry matches*'
    $env:FLEET_LOCAL_ID = 'Hackintosh'
    $j = (Invoke-DryRunJson)
    ($j.hosts | Where-Object id -eq 'hackintosh').target | Should -Be 'local'
  }

  It 'runs nothing and takes no lock' {
    Invoke-FleetDeploy @('--dry-run', '--json', '-') | Out-Null
    Should -Invoke Invoke-Ssh -Times 0
    Should -Invoke Invoke-Native -Times 0
    Test-Path (Join-Path $script:StateDir 'deploy.lock.d') | Should -BeFalse
  }
}

Describe 'deploy' {
  It 'targets every host once: local via fleet-deploy-windows.ps1, remotes via ssh, server last' {
    $rc = Invoke-FleetDeploy @('--json', (Join-Path $TestDrive 'report.json')) | Select-Object -Last 1
    $rc | Should -Be 0
    @($script:NativeLog | Where-Object { $_.Args -like '*fleet-deploy-windows.ps1*' }).Count | Should -Be 1
    @($script:SshLog | Where-Object { $_.Target -like '*tiny11*' }).Count | Should -Be 0
    $deploys = @($script:SshLog | Where-Object { $_.Command -notlike '*rev-parse*' } | ForEach-Object { $_.Target })
    $deploys | Should -Be @('alexhughes@macbookpro', 'alex@hackintosh')
    $r = Get-Content (Join-Path $TestDrive 'report.json') -Raw | ConvertFrom-Json
    $r.ok | Should -BeTrue
    $r.hosts.Count | Should -Be 6
  }

  It 'sends remote Macs the exported env, the branch pull, and no keychain password' {
    Invoke-FleetDeploy @('--host', 'hackintosh') | Out-Null
    $cmd = ($script:SshLog | Where-Object { $_.Command -notlike '*rev-parse*' } | Select-Object -First 1).Command
    $cmd | Should -BeLike '*export FLEET_BRANCH="main"*FLEET_DEPLOY_DESKFLOW="1"*FLEET_DEPLOY_MOUSER="1"*FLEET_DESKFLOW_ROOT=*FLEET_MOUSER_ROOT=*'
    $cmd | Should -BeLike '*git fetch origin && git checkout "main" && git pull --ff-only origin "main" && bash scripts/fleet-deploy-macos.sh'
    $cmd | Should -Not -BeLike '*KEYCHAIN*'
  }

  It 'reports ssh exit 255 as unreachable and fails the run' {
    Mock Invoke-Ssh { if ($Target -eq 'alex@hackintosh') { return @{ Code = 255; Output = '' } }; return @{ Code = 0; Output = "cafe`n" } }
    $out = Invoke-FleetDeploy @('--json', (Join-Path $TestDrive 'r.json'))
    $out | Select-Object -Last 1 | Should -Be 1
    $r = Get-Content (Join-Path $TestDrive 'r.json') -Raw | ConvertFrom-Json
    $r.ok | Should -BeFalse
    @($r.hosts | Where-Object id -eq 'hackintosh' | ForEach-Object { $_.result } | Sort-Object -Unique) | Should -Be @('unreachable(ssh 255)')
  }

  It 'records last-good.json as {host:{app:{commit,ts}}} after a healthy deploy' {
    Invoke-FleetDeploy @() | Out-Null
    Test-Path $script:LastGood | Should -BeTrue
    $lg = Get-Content $script:LastGood -Raw | ConvertFrom-Json
    $lg.tiny11.deskflow.commit | Should -Be 'beef000000000000000000000000000000000001'
    $lg.hackintosh.mouser.commit | Should -BeLike 'cafe0000*'
    $lg.macbookpro.deskflow.ts | Should -Match '^\d{4}-\d{2}-\d{2}T'
  }

  It 'skips last-good when fleet-health --host fails' {
    Mock Invoke-FleetHealth { if ($HealthArgs -contains 'macbookpro') { return 3 }; return 0 }
    $rc = Invoke-FleetDeploy @() | Select-Object -Last 1
    $rc | Should -Be 1
    $lg = Get-Content $script:LastGood -Raw | ConvertFrom-Json
    $lg.PSObject.Properties['macbookpro'] | Should -BeNullOrEmpty
    $lg.tiny11.deskflow.commit | Should -Not -BeNullOrEmpty
  }
}

Describe 'rollback' {
  It 'checks out the commits recorded in last-good.json' {
    New-Item -ItemType Directory -Path $script:StateDir -Force | Out-Null
    Set-Content $script:LastGood '{"hackintosh":{"deskflow":{"commit":"d15ea5e0deadbeef","ts":"x"},"mouser":{"commit":"a11ce0c0de","ts":"x"}}}'
    $rc = Invoke-FleetDeploy @('--rollback', '--host', 'hackintosh') | Select-Object -Last 1
    $rc | Should -Be 0
    $cmd = ($script:SshLog | Where-Object { $_.Command -notlike '*rev-parse*' } | Select-Object -First 1).Command
    $cmd | Should -BeLike '*git checkout --detach "d15ea5e0deadbeef"*'
    $cmd | Should -BeLike '*git -C "$HOME/Desktop/Mouser" fetch fork && git -C "$HOME/Desktop/Mouser" checkout --detach "a11ce0c0de"*'
    $cmd | Should -BeLike '*FLEET_SKIP_GIT_PULL=1*'
    $cmd | Should -Not -BeLike '*git pull --ff-only*'
  }

  It 'rolls back the local seat with --app deskflow' {
    New-Item -ItemType Directory -Path $script:StateDir -Force | Out-Null
    Set-Content $script:LastGood '{"tiny11":{"deskflow":{"commit":"0ddba11","ts":"x"}}}'
    $rc = Invoke-FleetDeploy @('--rollback', '--host', 'tiny11', '--app', 'deskflow') | Select-Object -Last 1
    $rc | Should -Be 0
    @($script:NativeLog | Where-Object { $_.Args -eq 'checkout --detach 0ddba11' }).Count | Should -Be 1
    @($script:NativeLog | Where-Object { $_.Args -like '*fleet-deploy-windows.ps1*' }).Count | Should -Be 1
  }

  It 'refuses without a last-good entry' {
    { Invoke-FleetDeploy @('--rollback', '--host', 'hackintosh') } | Should -Throw '*does not exist*'
    New-Item -ItemType Directory -Path $script:StateDir -Force | Out-Null
    Set-Content $script:LastGood '{"tiny11":{"deskflow":{"commit":"abc","ts":"x"}}}'
    { Invoke-FleetDeploy @('--rollback', '--host', 'hackintosh') } | Should -Throw '*no last-good commit for hackintosh/deskflow*'
    Should -Invoke Invoke-Ssh -Times 0
  }
}

Describe 'self-test' {
  It 'implies --ref HEAD, folds fleet-health into the report, and exits 0 only when all ok' {
    Mock Invoke-FleetHealth {
      if ($HealthArgs -contains '--check') {
        $script:HealthJson = '{"hosts":[{"id":"hackintosh","signedBy":"Apple Development: Alex","tcc":"ok","mesh":"ok","ok":true},{"id":"macbookpro","ok":false,"tcc":"denied"},{"id":"tiny11","signedBy":"thumb","tcc":"n/a","mesh":"ok","ok":true}]}'
        return 1
      }
      return 0
    }
    $rc = Invoke-FleetDeploy @('--self-test', '--json', (Join-Path $TestDrive 'st.json')) | Select-Object -Last 1
    $rc | Should -Be 1
    $r = Get-Content (Join-Path $TestDrive 'st.json') -Raw | ConvertFrom-Json
    $r.ok | Should -BeFalse
    foreach ($k in 'id', 'target', 'app', 'commit', 'signedBy', 'tcc', 'mesh', 'result') { $r.hosts[0].PSObject.Properties[$k] | Should -Not -BeNullOrEmpty }
    ($r.hosts | Where-Object { $_.id -eq 'hackintosh' -and $_.app -eq 'deskflow' }).signedBy | Should -Be 'Apple Development: Alex'
    @($r.hosts | Where-Object id -eq 'macbookpro' | ForEach-Object { $_.result } | Sort-Object -Unique) | Should -Be @('unhealthy')
    @($script:SshLog | Where-Object { $_.Command -like '*git pull*' }).Count | Should -Be 0
    @($script:NativeLog | Where-Object { $_.Args -like 'pull*' }).Count | Should -Be 0
  }

  It 'cannot pass without tools/fleet-health (python)' {
    $rc = Invoke-FleetDeploy @('--self-test') | Select-Object -Last 1
    $rc | Should -Be 1
  }

  # NOTE: pwsh is absent on the authoring seat; these cases are written but unrun there.
  It 'folds the real fleet-health shape {ok,results:[{host,check,status,detail}]} per host' {
    $real = @'
{"ok": false, "results": [
 {"host":"hackintosh","check":"sign","status":"PASS","detail":"12 Mach-Os signed by Apple Development* team ABCDE12345"},
 {"host":"hackintosh","check":"tcc","status":"PASS","detail":"4 rows cert-based"},
 {"host":"hackintosh","check":"mesh","status":"PASS","detail":"hackintosh -> macbookpro ok"},
 {"host":"hackintosh","check":"mesh","status":"FAIL","detail":"hackintosh -> tiny11 rc=1"},
 {"host":"macbookpro","check":"sign","status":"PASS","detail":"12 Mach-Os signed"},
 {"host":"macbookpro","check":"tcc","status":"FAIL","detail":"kTCCServiceAccessibility: no deskflow client with auth_value=2"},
 {"host":"macbookpro","check":"mesh","status":"PASS","detail":"macbookpro -> hackintosh ok"},
 {"host":"tiny11","check":"sign","status":"SKIP","detail":"macOS-only check"},
 {"host":"tiny11","check":"tcc","status":"SKIP","detail":"macOS-only check"},
 {"host":"tiny11","check":"authenticode","status":"PASS","detail":"3 files signed by thumbprint 0123"},
 {"host":"tiny11","check":"mesh","status":"PASS","detail":"tiny11 -> hackintosh ok"}
]}
'@
    $by = ConvertFrom-FleetHealthJson $real
    $by['hackintosh'].signedBy | Should -Be '12 Mach-Os signed by Apple Development* team ABCDE12345'
    $by['hackintosh'].tcc | Should -Be 'PASS'
    $by['hackintosh'].mesh | Should -Be 'FAIL'      # one failing leg fails mesh
    $by['hackintosh'].ok | Should -BeFalse
    $by['macbookpro'].tcc | Should -Be 'FAIL'
    $by['macbookpro'].ok | Should -BeFalse
    $by['tiny11'].signedBy | Should -Be '3 files signed by thumbprint 0123'   # sign is SKIP on Windows
    $by['tiny11'].tcc | Should -Be 'SKIP'
    $by['tiny11'].ok | Should -BeTrue

    Mock Invoke-FleetHealth { if ($HealthArgs -contains '--check') { $script:HealthJson = $real; return 1 }; return 0 }
    $rc = Invoke-FleetDeploy @('--self-test', '--json', (Join-Path $TestDrive 'st2.json')) | Select-Object -Last 1
    $rc | Should -Be 1
    $r = Get-Content (Join-Path $TestDrive 'st2.json') -Raw | ConvertFrom-Json
    $r.ok | Should -BeFalse
    ($r.hosts | Where-Object { $_.id -eq 'hackintosh' -and $_.app -eq 'deskflow' }).mesh | Should -Be 'FAIL'
    @($r.hosts | Where-Object id -eq 'macbookpro' | ForEach-Object { $_.result } | Sort-Object -Unique) | Should -Be @('unhealthy')
    @($r.hosts | Where-Object id -eq 'tiny11' | ForEach-Object { $_.result } | Sort-Object -Unique) | Should -Be @('ok')
  }

  It 'runs python tools/fleet-health (not the fleet-health.ps1 collector) with --env <fleet.env>' {
    # The seam itself is mocked elsewhere; assert on its definition: the fleet
    # checker is the python script, invoked through Get-FleetPython, and the
    # controller's fleet.env is forwarded as --env.
    $def = (Get-Command Invoke-FleetHealth).Definition
    $def | Should -Match "Join-Path \`$script:FleetRoot 'tools\\fleet-health'"
    $def | Should -Not -Match 'fleet-health\.ps1'
    $def | Should -Match 'Get-FleetPython'
    $def | Should -Match "'--env', \`$script:HealthEnvFile"
    $rc = Invoke-FleetDeploy @('--dry-run') | Select-Object -Last 1
    $script:HealthEnvFile | Should -Be (Join-Path $script:Repo 'scripts\fleet.env')
  }
}

Describe 'Mouser sync (the per-OS scripts run with FLEET_SKIP_GIT_PULL=1)' {
  It 'fast-forwards Mouser from fork on remote Macs on a normal branch deploy, before the per-OS script' {
    $rc = Invoke-FleetDeploy @('--host', 'hackintosh') | Select-Object -Last 1
    $rc | Should -Be 0
    $cmd = ($script:SshLog | Where-Object { $_.Command -notlike '*rev-parse*' } | Select-Object -First 1).Command
    $cmd | Should -Match 'FLEET_SKIP_GIT_PULL=1'
    $cmd | Should -Match 'FLEET_MOUSER_BRANCH="main"'
    $cmd | Should -Match 'remote get-url fork >/dev/null 2>&1 \|\| git -C "\$HOME/Desktop/Mouser" remote add fork "https://github.com/hughesyadaddy/Mouser.git"'
    $cmd | Should -Match 'git -C "\$HOME/Desktop/Mouser" fetch fork && git -C "\$HOME/Desktop/Mouser" checkout "main" && git -C "\$HOME/Desktop/Mouser" pull --ff-only fork "main"'
    $cmd.IndexOf('pull --ff-only fork "main"') | Should -BeLessThan $cmd.IndexOf('bash scripts/fleet-deploy-macos.sh')
  }

  It 'syncs Mouser inside the powershell command for remote Windows seats' {
    $opt = ConvertTo-FleetOptions @(); $opt.Branch = 'main'; $opt.MouserBranch = 'main'; $opt.DeployDeskflow = 1; $opt.DeployMouser = 1; $opt.Reconfigure = 0
    $e = [pscustomobject]@{ id = 'winbox'; os = 'windows'; target = 'alexh@winbox'; deskflowPath = 'C:/d'; mouserPath = 'C:/m' }
    $cmd = Get-WinRemoteCommand $opt $e '' ''
    $cmd | Should -Match "\`$env:FLEET_MOUSER_BRANCH='main'"
    $cmd | Should -Match "if \(Test-Path 'C:/m/.git'\) \{ git -C 'C:/m' remote get-url fork; if \(\`$LASTEXITCODE\) \{ git -C 'C:/m' remote add fork 'https://github.com/hughesyadaddy/Mouser.git'"
    $cmd | Should -Match "git -C 'C:/m' fetch fork; if \(\`$LASTEXITCODE\) \{ exit \`$LASTEXITCODE \}; git -C 'C:/m' checkout 'main'; if \(\`$LASTEXITCODE\) \{ exit \`$LASTEXITCODE \}; git -C 'C:/m' pull --ff-only fork 'main'"
    $cmd.IndexOf("pull --ff-only fork 'main'") | Should -BeLessThan $cmd.IndexOf('fleet-deploy-windows.ps1')
    (Get-WinRemoteCommand $opt $e 'HEAD' 'HEAD') | Should -Not -Match 'fetch fork'
    (Get-WinRemoteCommand $opt $e 'v1' 'v1') | Should -Match "git -C 'C:/m' checkout --detach 'v1'"
    $opt.DeployMouser = 0
    (Get-WinRemoteCommand $opt $e '' '') | Should -Not -Match 'fetch fork'
  }

  It 'honours FLEET_MOUSER_BRANCH and detaches Mouser for --ref X' {
    $opt = ConvertTo-FleetOptions @(); $opt.Branch = 'main'; $opt.MouserBranch = 'mouser-dev'; $opt.DeployDeskflow = 1; $opt.DeployMouser = 1; $opt.Reconfigure = 0
    (Get-ShMouserSync '~/Desktop/Mouser' '' $opt) | Should -Match 'checkout "mouser-dev" && git -C "\$HOME/Desktop/Mouser" pull --ff-only fork "mouser-dev"'
    (Get-ShMouserSync '~/Desktop/Mouser' 'v1.2.3' $opt) | Should -Match 'fetch fork && git -C "\$HOME/Desktop/Mouser" checkout --detach "v1.2.3"'
    (Get-ShMouserSync '~/Desktop/Mouser' 'v1.2.3' $opt) | Should -Not -Match 'pull --ff-only'
    (Get-ShMouserSync '~/Desktop/Mouser' 'HEAD' $opt) | Should -Be ''
    (Get-ShExports $opt '~/d' '~/m' '' '') | Should -Match 'FLEET_MOUSER_BRANCH="mouser-dev"'
  }

  It 'syncs the local Windows seat''s Mouser checkout via git -C steps' {
    $m = Join-Path $TestDrive 'Mouser'
    New-Item -ItemType Directory -Path (Join-Path $m '.git') -Force | Out-Null
    $opt = ConvertTo-FleetOptions @(); $opt.Branch = 'main'; $opt.MouserBranch = 'main'; $opt.DeployDeskflow = 1; $opt.DeployMouser = 1; $opt.Reconfigure = 0
    Mock Invoke-Native {
      $script:NativeLog += [pscustomobject]@{ Exe = $Exe; Args = ($ArgList -join ' '); Dir = $WorkingDirectory }
      if (($ArgList -join ' ') -eq 'remote get-url fork') { return @{ Code = 2; Output = '' } }
      return @{ Code = 0; Output = '' }
    }
    $steps = @(Get-MouserSteps $m '' $opt)
    @($steps | ForEach-Object { $_ -join ' ' }) | Should -Be @(
      'git remote add fork https://github.com/hughesyadaddy/Mouser.git', 'git fetch fork', 'git checkout main', 'git pull --ff-only fork main')
    @(Get-MouserSteps $m 'v1' $opt | ForEach-Object { $_ -join ' ' }) | Should -Contain 'git checkout --detach v1'
    @(Get-MouserSteps $m 'HEAD' $opt).Count | Should -Be 0
    @(Get-MouserSteps (Join-Path $TestDrive 'nope') '' $opt).Count | Should -Be 0
  }
}

Describe 'lock' {
  It 'refuses while deploy.lock.d is held by a live process' {
    New-Item -ItemType Directory -Path (Join-Path $script:StateDir 'deploy.lock.d') -Force | Out-Null
    Set-Content (Join-Path $script:StateDir 'deploy.lock.d\pid') "$PID"
    { Invoke-FleetDeploy @() } | Should -Throw '*deploy lock held*'
    Should -Invoke Invoke-Ssh -Times 0
  }

  It 'reclaims a stale lock and releases its own on exit' {
    New-Item -ItemType Directory -Path (Join-Path $script:StateDir 'deploy.lock.d') -Force | Out-Null
    Set-Content (Join-Path $script:StateDir 'deploy.lock.d\pid') '2147483000'
    Mock Test-ProcessAlive { $false }
    $rc = Invoke-FleetDeploy @() | Select-Object -Last 1
    $rc | Should -Be 0
    Test-Path (Join-Path $script:StateDir 'deploy.lock.d') | Should -BeFalse
  }
}
