# FPS of the desktop bench without RenderDoc: boots into the first gameplay scene (START presses), logs flips/s
# every 5 s (sdretail_fps_log). Extra command line through -Extra. -Sample: every 5 s after 50 s also prints the
# process 3D GPU engine load and the busiest threads (% of one core).
param([int]$Total = 170, [int]$StartKeyAt = 40, [string[]]$Extra = @(), [string]$Log = "D:\CODE\SDNX\port\fps_bench.log",
      [switch]$Sample, [string]$LogLevel = "info", [int]$Presses = 8, [string[]]$Keys = @(), [switch]$Post,
      [string]$Record = "", [string]$Replay = "")
# -Record keys.txt: the presses of the person at the PC (keyboard while the game window is in front, and XInput pad
#   buttons translated to the bound keys) are written as "<second> <vk hex> down|up"; no automatic START presses.
# -Replay keys.txt: those presses are posted to the game window at the same seconds (implies -Post, no START presses).
if ($Record -or $Replay) { $Presses = 0 }
if ($Replay) { $Post = $true }
# -Post: keys go to the game window with PostMessage (no focus change: works while the PC is in use).
# -Keys "60:0x28","62:0x26": extra key presses at second:virtual-key (arrows are bound to the D-pad).
Add-Type @"
using System; using System.Runtime.InteropServices;
public class K5 {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [StructLayout(LayoutKind.Sequential)] public struct XS { public uint packet; public ushort buttons; public byte lt, rt; public short lx, ly, rx, ry; }
  [DllImport("xinput1_4.dll")] public static extern uint XInputGetState(uint user, out XS state);
}
"@
$wd = "D:\CODE\SDNX\nx\desktop\out"
Remove-Item "$Log*" -ErrorAction SilentlyContinue
$args2 = @("--game_data_root", "D:\CODE\SDNX\port\testroot", "--log_file", $Log, "--log_level", $LogLevel, "--mnk_mode",
  "--keybind_start", "Return", "--keybind_a", "Space", "--keybind_b", "Escape", "--keybind_dpad_up", "Up",
  "--keybind_dpad_down", "Down", "--keybind_dpad_left", "Left", "--keybind_dpad_right", "Right", "--sdretail_fps_log", "true") + $Extra
$p = Start-Process -FilePath "$wd\sdretail.exe" -WorkingDirectory $wd -PassThru -ArgumentList $args2
function Post-Key([byte]$vk, [bool]$down) {
  $p.Refresh(); if ($p.MainWindowHandle -eq 0) { return }
  $sc = [K5]::MapVirtualKey($vk, 0); $ext = if ($vk -ge 0x25 -and $vk -le 0x28) { 0x01000000 } else { 0 }
  if ($down) { [K5]::PostMessage($p.MainWindowHandle, 0x100, [IntPtr][int]$vk, [IntPtr](1 -bor ($sc -shl 16) -bor $ext)) | Out-Null }
  else { [K5]::PostMessage($p.MainWindowHandle, 0x101, [IntPtr][int]$vk, [IntPtr]([int64]0xC0000001 -bor ($sc -shl 16) -bor $ext)) | Out-Null }
}
# Pad button -> bound key (see the --keybind_* arguments): dpad, START, A, B.
$padMap = @{ 1 = 0x26; 2 = 0x28; 4 = 0x25; 8 = 0x27; 16 = 0x0D; 4096 = 0x20; 8192 = 0x1B }  # plain hashtable: [ordered][int] is positional
$inv = [Globalization.CultureInfo]::InvariantCulture
$recDown = @{}; $padPrev = 0; $recLines = New-Object System.Collections.Generic.List[string]
$replayEv = @(); $replayAt = 0
if ($Replay) { $replayEv = @(Get-Content $Replay | Where-Object { $_ -match "^\d" } | ForEach-Object { $a = $_ -split " "; ,@([double]::Parse($a[0], [Globalization.CultureInfo]::InvariantCulture), [Convert]::ToInt32($a[1], 16), ($a[2] -eq "down")) }) }
function Send-Key([byte]$vk) {
  $p.Refresh()
  if ($Post) {
    if ($p.MainWindowHandle -eq 0) { return }
    $sc = [K5]::MapVirtualKey($vk, 0); $ext = if ($vk -ge 0x25 -and $vk -le 0x28) { 0x01000000 } else { 0 }
    [K5]::PostMessage($p.MainWindowHandle, 0x100, [IntPtr][int]$vk, [IntPtr](1 -bor ($sc -shl 16) -bor $ext)) | Out-Null
    Start-Sleep -Milliseconds 80
    [K5]::PostMessage($p.MainWindowHandle, 0x101, [IntPtr][int]$vk, [IntPtr]([int64]0xC0000001 -bor ($sc -shl 16) -bor $ext)) | Out-Null
  } else {
    if ($p.MainWindowHandle -ne 0) { [K5]::SetForegroundWindow($p.MainWindowHandle) | Out-Null }
    Start-Sleep -Milliseconds 150
    [K5]::keybd_event($vk, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [K5]::keybd_event($vk, 0, 2, [UIntPtr]::Zero)
  }
}
$start = Get-Date; $presses = 0; $lastSample = 0; $prev = $null; $keyDone = @{}
while (((Get-Date) - $start).TotalSeconds -lt $Total -and -not $p.HasExited) {
  $t = ((Get-Date) - $start).TotalSeconds
  if ($t -ge ($StartKeyAt + 4 * $presses) -and $presses -lt $Presses) {
    Send-Key 0x0D
    $presses++
  }
  for ($i = 0; $i -lt $Keys.Count; $i++) {
    $kt, $kv = $Keys[$i] -split ':'
    if (-not $keyDone[$i] -and $t -ge [double]$kt) {
      $keyDone[$i] = $true
      Send-Key ([byte][Convert]::ToInt32($kv, 16))
    }
  }
  if ($Record) {
    $p.Refresh(); $front = $p.MainWindowHandle -ne 0 -and [K5]::GetForegroundWindow() -eq $p.MainWindowHandle
    for ($vk = 8; $vk -lt 255; $vk++) {
      $d = $front -and (([K5]::GetAsyncKeyState($vk) -band 0x8000) -ne 0)
      if ($d -ne [bool]$recDown[$vk]) { $recDown[$vk] = $d; $recLines.Add([string]::Format($inv, "{0:F2} {1:X2} {2}", $t, $vk, $(if ($d) { "down" } else { "up" }))) }
    }
    $xs = New-Object K5+XS
    $pad = if ([K5]::XInputGetState(0, [ref]$xs) -eq 0) { [int]$xs.buttons } else { 0 }
    foreach ($b in $padMap.Keys) {
      if (($pad -band $b) -ne ($padPrev -band $b)) { $recLines.Add([string]::Format($inv, "{0:F2} {1:X2} {2}", $t, [int]$padMap[[int]$b], $(if ($pad -band $b) { "down" } else { "up" }))) }
    }
    $padPrev = $pad
  }
  while ($replayAt -lt $replayEv.Count -and $t -ge $replayEv[$replayAt][0]) {
    Post-Key ([byte]$replayEv[$replayAt][1]) $replayEv[$replayAt][2]; $replayAt++
  }
  Start-Sleep -Milliseconds $(if ($Record -or $Replay) { 15 } else { 250 })
  if ($Sample -and $t -ge 50 -and ($t - $lastSample) -ge 5) {
    $p.Refresh()
    $now = @{}
    foreach ($th in $p.Threads) { try { $now[$th.Id] = $th.TotalProcessorTime.TotalMilliseconds } catch {} }
    if ($prev) {
      $dt = ($t - $lastSample) * 1000
      $busy = foreach ($k in $now.Keys) { if ($prev.ContainsKey($k)) { [math]::Round(100 * ($now[$k] - $prev[$k]) / $dt) } }
      $gpu = 0
      try {
        $gpu = ((Get-Counter "\GPU Engine(pid_$($p.Id)*engtype_3D)\Utilization Percentage" -ErrorAction Stop).CounterSamples |
          Measure-Object CookedValue -Sum).Sum
      } catch {}
      "t={0,4:N0}s gpu3d={1,5:N1}% threads%: {2}" -f $t, $gpu, (($busy | Sort-Object -Descending | Select-Object -First 5) -join ' ')
    }
    $prev = $now; $lastSample = $t
  }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
if ($Record) { $recLines | Set-Content -Encoding ascii $Record; "recorded $($recLines.Count) key events -> $Record" }
if ($Replay) { "replayed $replayAt of $($replayEv.Count) key events" }
$fl = @(Select-String -Path "$Log*" -Pattern "flips/s" | ForEach-Object { [double](($_.Line -split '\] ')[-1] -split ' ')[0] })
$dr = @(Select-String -Path "$Log*" -Pattern "sdnx_draws" | ForEach-Object { [double](($_.Line -split '5 s: ')[-1] -split ' ')[0] })
$k = [math]::Min(12, [math]::Min($fl.Count, [math]::Max($dr.Count, 1)))
$fl = $fl | Select-Object -Last $k
if ($dr.Count) {
  $dr = $dr | Select-Object -Last $k
  $fps = ($fl | Measure-Object -Average).Average; $dpf = (($dr | Measure-Object -Average).Average / 5) / $fps
  "fps avg {0:N1} (min {1:N1} max {2:N1}), draws/frame {3:N0}, draws/s {4:N0}" -f $fps, ($fl | Measure-Object -Minimum).Minimum, ($fl | Measure-Object -Maximum).Maximum, $dpf, ($dpf * $fps)
} else { "fps last: " + ($fl -join ' ') }
