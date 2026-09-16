$ErrorActionPreference = 'Stop'
$root = 'C:\Users\alexh\Desktop\deskflow'
$mouser = 'C:\Users\alexh\Desktop\Mouser'
$branch = 'fleet/memory-program'
$log = 'C:\Users\alexh\fleet-deploy-tiny11.log'
Start-Transcript -Path $log -Force | Out-Null
try {
  "== $(Get-Date -Format o) deploy $branch on $env:COMPUTERNAME"
  # thumbprint (public) into .env if empty
  $envPath = "$root\.env"
  $content = Get-Content $envPath
  if ($content -match '^DESKFLOW_SIGN_THUMBPRINT=\s*$') {
    $content = $content -replace '^DESKFLOW_SIGN_THUMBPRINT=\s*$', 'DESKFLOW_SIGN_THUMBPRINT=FBB49069A6C594E83714724217C7A5F54885FAEC'
    Set-Content -Path $envPath -Value $content
    "set DESKFLOW_SIGN_THUMBPRINT in .env"
  }
  # sync both checkouts to the pushed branch
  git -C $root fetch origin; if ($LASTEXITCODE) { throw "deskflow fetch failed" }
  git -C $root checkout $branch; if ($LASTEXITCODE) { throw "deskflow checkout failed" }
  git -C $root pull --ff-only origin $branch; if ($LASTEXITCODE) { throw "deskflow pull failed" }
  git -C $mouser fetch fork; if ($LASTEXITCODE) { throw "mouser fetch failed" }
  git -C $mouser checkout $branch; if ($LASTEXITCODE) { throw "mouser checkout failed" }
  git -C $mouser pull --ff-only fork $branch; if ($LASTEXITCODE) { throw "mouser pull failed" }
  "deskflow HEAD: $(git -C $root rev-parse --short HEAD)"
  "mouser HEAD:   $(git -C $mouser rev-parse --short HEAD)"
  $env:FLEET_BRANCH = $branch
  $env:FLEET_DEPLOY_DESKFLOW = '1'
  $env:FLEET_DEPLOY_MOUSER = '1'
  $env:FLEET_DESKFLOW_ROOT = $root
  $env:FLEET_MOUSER_ROOT = $mouser
  $env:FLEET_SKIP_GIT_PULL = '1'
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root\scripts\fleet-deploy-windows.ps1"
  "fleet-deploy-windows exit: $LASTEXITCODE"
  if ($LASTEXITCODE) { throw "fleet-deploy-windows.ps1 failed ($LASTEXITCODE)" }
  "== DEPLOY OK $(Get-Date -Format o)"
} catch {
  "== DEPLOY FAILED: $($_ | Out-String)"
} finally {
  Stop-Transcript | Out-Null
}
