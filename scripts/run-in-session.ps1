param([string]$Script, [string]$TaskName = "FleetRunInSession")
$log = "C:\Users\alexh\run-in-session.log"
$inner = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command `"Start-Transcript -Path $log -Force | Out-Null; try { & '$Script' } catch { `$_ | Out-String }; Stop-Transcript | Out-Null`""
$a = New-ScheduledTaskAction -Execute "powershell.exe" -Argument $inner
$p = New-ScheduledTaskPrincipal -UserId "alexh" -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $TaskName -Action $a -Principal $p -Force | Out-Null
Start-ScheduledTask -TaskName $TaskName
$deadline = (Get-Date).AddSeconds(20)
do { Start-Sleep -Milliseconds 500; $st = (Get-ScheduledTask -TaskName $TaskName).State } while ($st -eq 'Running' -and (Get-Date) -lt $deadline)
$info = Get-ScheduledTask -TaskName $TaskName | Get-ScheduledTaskInfo
"task state=$st lastResult=$($info.LastTaskResult)"
Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
