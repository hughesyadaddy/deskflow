<#
.SYNOPSIS
  Windows sampler wrapper for tools/fleet-soak (Task Scheduler, every 60 s).

.DESCRIPTION
  Locates the process by executable path with Get-Process, collects
  PrivateMemorySize64 / WorkingSet64 / HandleCount / thread count / GDI+USER
  object counts and the Service Control Manager "entered the running state"
  count for the named service (restart_count), then feeds the JSON to
  `fleet-soak sample --probe-json -` which appends one sample to the JSONL.

  Register (per process, run as the logged-in user so GetGuiResources works):

    schtasks /Create /SC MINUTE /MO 1 /TN "fleet-soak-deskflow-core" /TR ^
      "powershell -NoProfile -ExecutionPolicy Bypass -File C:\src\deskflow\tools\fleet-soak-task.ps1 ^
       -Label deskflow-core -Exe \"C:\Program Files\Deskflow\deskflow-core.exe\" -SoakDir C:\src\deskflow\harness\soak"

.PARAMETER Label
  Service name (deskflow-daemon) or a short label for a non-service process.
.PARAMETER Exe
  Absolute path of the executable to sample.
.PARAMETER Out
  JSONL path. Mutually exclusive with -SoakDir.
.PARAMETER SoakDir
  harness/soak base; fleet-soak appends into <SoakDir>/latest (starting it if absent) and names
  the file <seat>-<proc>.jsonl.
.PARAMETER Python
  Python interpreter (default: python).
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)] [string] $Label,
  [Parameter(Mandatory = $true)] [string] $Exe,
  [string] $Out,
  [string] $SoakDir,
  [string] $Proc,
  [int] $Interval = 60,
  [string] $Python = 'python'
)

$ErrorActionPreference = 'SilentlyContinue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$fleetSoak = Join-Path $here 'fleet-soak'

$p = Get-Process | Where-Object { $_.Path -eq $Exe } | Sort-Object Id | Select-Object -First 1

$restarts = $null
try {
  $restarts = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Service Control Manager'; Id = 7036 } -MaxEvents 5000 |
    Where-Object { $_.Message -like "*$Label*" -and $_.Message -like '*running*' }).Count
} catch { $restarts = $null }

$scenario = 'unknown'
$log = Get-ChildItem -Path (Join-Path $env:USERPROFILE 'deskflow*.log'), (Join-Path $env:APPDATA 'Deskflow\*.log') -File |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($log) {
  $m = Select-String -Path $log.FullName -Pattern 'starting (server|client) epoch' | Select-Object -Last 1
  if ($m) { $scenario = $m.Matches[0].Groups[1].Value }
}

$o = [ordered]@{ pid = $null }
if ($p) {
  $o = [ordered]@{
    pid              = $p.Id
    start_time       = $p.StartTime.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    exe              = $p.Path
    private_bytes_mb = [math]::Round($p.PrivateMemorySize64 / 1MB, 3)
    rss_mb           = [math]::Round($p.WorkingSet64 / 1MB, 3)
    handles          = $p.HandleCount
    threads          = $p.Threads.Count
  }
  try {
    Add-Type -Name U -Namespace W -MemberDefinition '[DllImport("user32.dll")] public static extern int GetGuiResources(IntPtr h, int f);'
    $o.gdi = [W.U]::GetGuiResources($p.Handle, 0)
    $o.user = [W.U]::GetGuiResources($p.Handle, 1)
  } catch { $o.gdi = $null; $o.user = $null }
}
$o.restart_count = $restarts
$o.scenario = $scenario
$json = $o | ConvertTo-Json -Compress

$argv = @($fleetSoak, 'sample', '--label', $Label, '--exe', $Exe, '--once', '--interval', $Interval, '--probe-json', '-')
if ($Proc) { $argv += @('--proc', $Proc) }
if ($Out) { $argv += @('--out', $Out) }
elseif ($SoakDir) { $argv += @('--soak', $SoakDir) }
else { Write-Error 'Specify -Out or -SoakDir'; exit 2 }

$json | & $Python @argv
exit $LASTEXITCODE
