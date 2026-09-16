# Runs INSIDE the interactive console session (via a one-shot scheduled task).
# Releases every modifier the OS reports held down, then reports state.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KB {
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@
$vks = @{ 'LSHIFT'=0xA0; 'RSHIFT'=0xA1; 'LCTRL'=0xA2; 'RCTRL'=0xA3; 'LALT'=0xA4; 'RALT'=0xA5; 'LWIN'=0x5B; 'RWIN'=0x5C }
$log = "$env:TEMP\release-modifiers.log"
"=== $(Get-Date -Format o) session $([System.Diagnostics.Process]::GetCurrentProcess().SessionId)" | Out-File $log -Append
foreach ($name in $vks.Keys) {
  $vk = $vks[$name]
  $down = ([KB]::GetAsyncKeyState($vk) -band 0x8000) -ne 0
  "$name down=$down" | Out-File $log -Append
  if ($down) {
    [KB]::keybd_event([byte]$vk, 0, 0x0002, [UIntPtr]::Zero)   # KEYEVENTF_KEYUP
    Start-Sleep -Milliseconds 30
    $after = ([KB]::GetAsyncKeyState($vk) -band 0x8000) -ne 0
    "  released -> down=$after" | Out-File $log -Append
  }
}
# Caps Lock: report only (toggle state), do not change it here
$caps = ([KB]::GetAsyncKeyState(0x14) -band 0x0001) -ne 0
"CAPSLOCK toggled=$caps" | Out-File $log -Append
