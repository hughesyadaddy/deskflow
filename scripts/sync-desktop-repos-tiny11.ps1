$ErrorActionPreference = 'Stop'
$deskflow = Join-Path $env:USERPROFILE 'Desktop\deskflow'
$mouser = Join-Path $env:USERPROFILE 'Desktop\Mouser'
$py = Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe" |
  Select-Object -First 1 -ExpandProperty FullName

# Remove stale non-Desktop copies
foreach ($stale in @(
    (Join-Path $env:USERPROFILE 'code\Mouser'),
    (Join-Path $env:USERPROFILE 'code\deskflow')
  )) {
  if (Test-Path $stale) {
    Write-Host "Removing stale: $stale"
    Remove-Item -Recurse -Force $stale
  }
}

if (-not (Test-Path $deskflow)) {
  throw "Missing canonical deskflow at $deskflow"
}
Set-Location $deskflow
git pull --ff-only origin refactor/fleet-state-hub | Out-Host
Write-Host "deskflow: $(git log -1 --oneline)"

if (-not (Test-Path $mouser)) {
  Write-Host "Cloning Mouser to $mouser"
  $zip = Join-Path $env:TEMP 'mouser.zip'
  Invoke-WebRequest -Uri 'https://codeload.github.com/hughesyadaddy/Mouser/zip/refs/heads/working' -OutFile $zip
  $extract = Join-Path $env:TEMP 'mouser-extract'
  if (Test-Path $extract) { Remove-Item -Recurse -Force $extract }
  Expand-Archive -Path $zip -DestinationPath $extract -Force
  Move-Item (Join-Path $extract 'Mouser-working') $mouser
}
Set-Location $mouser
# The bootstrap above unpacks a ZIP, which leaves no .git -- so this pull was
# skipped forever and every build used the same stale snapshot (Mouser fixes
# silently never reached Windows). Convert a ZIP-sourced tree into a real
# clone once, then keep it current.
if (-not (Test-Path (Join-Path $mouser '.git'))) {
  Write-Host 'Mouser: converting ZIP snapshot to a git clone'
  $prevEAP = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  git init -q 2>&1 | Out-Null
  git remote add fork 'https://github.com/hughesyadaddy/Mouser.git' 2>&1 | Out-Null
  git fetch --depth=1 fork working 2>&1 | Out-Null
  git checkout -q -f -B working FETCH_HEAD 2>&1 | Out-Null
  git branch --set-upstream-to=fork/working working 2>&1 | Out-Null
  $ErrorActionPreference = $prevEAP
}
if (Test-Path (Join-Path $mouser '.git')) {
  # git writes progress to stderr; with ErrorActionPreference=Stop that is a
  # NativeCommandError which aborted the whole deploy before the build ran.
  $prevEAP = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  git pull --ff-only 2>&1 | Out-Null
  $ErrorActionPreference = $prevEAP
  Write-Host ("Mouser source: " + (git log --oneline -1))
}
Write-Host "Mouser source at $mouser"
if (-not (Test-Path (Join-Path $mouser '.venv'))) {
  if (-not $py) { throw 'Python not found' }
  & $py -m venv .venv
  & "$mouser\.venv\Scripts\python.exe" -m pip install --quiet -r requirements.txt pyinstaller
}

# Fix helper scripts to canonical Desktop paths
@'
@echo off
set MOUSERSRC=C:\Users\alexh\Desktop\Mouser
if exist "%MOUSERSRC%\build" rmdir /s /q "%MOUSERSRC%\build"
if exist "%MOUSERSRC%\dist" rmdir /s /q "%MOUSERSRC%\dist"
"%MOUSERSRC%\.venv\Scripts\python.exe" "%MOUSERSRC%\scripts\build_and_install.py"
'@ | Set-Content -Path (Join-Path $env:USERPROFILE 'build-mouser.bat') -Encoding ASCII

@'
@echo off
cd /d C:\Users\alexh\Desktop\deskflow
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Install
'@ | Set-Content -Path (Join-Path $env:USERPROFILE 'build_deskflow.bat') -Encoding ASCII

@'
$ErrorActionPreference = 'Continue'
$src = Join-Path $env:USERPROFILE 'Desktop\Mouser'
$py = Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe" | Select-Object -First 1 -ExpandProperty FullName
if (-not (Test-Path $src)) { throw "Missing $src" }
Set-Location $src
if (-not (Test-Path "$src\.venv")) { & $py -m venv .venv; & "$src\.venv\Scripts\python.exe" -m pip install --quiet -r requirements.txt pyinstaller }
Stop-Process -Name Mouser -Force -ErrorAction SilentlyContinue
$env:MOUSER_PYTHON = "$src\.venv\Scripts\python.exe"
& "$src\.venv\Scripts\python.exe" scripts\build_and_install.py 2>&1 | Select-Object -Last 6
'@ | Set-Content -Path (Join-Path $env:USERPROFILE 'mouser-build-tiny11.ps1') -Encoding UTF8

Write-Host '=== Canonical repos ==='
Write-Host "deskflow: $deskflow"
Write-Host "Mouser:   $mouser"
