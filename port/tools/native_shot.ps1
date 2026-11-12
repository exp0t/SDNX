# One gameplay frame of the native renderer on the bench (keys posted to the window, no focus needed):
#   native_shot.ps1 -Name x [-At 50] [-Addr 0x10bff000] [-Env @{SDNX_NO_CULL="1"}] [-LogFrame]
# Writes port/shots/<Name>.png (the front buffer, or the resolved image at -Addr) and port/shots/<Name>.log.
param([string]$Name = "shot", [int]$At = 50, [string]$Addr = "", [hashtable]$Env = @{}, [switch]$LogFrame,
      [int]$Presses = 2, [string[]]$Keys = @(),
      [string]$Replay = "$PSScriptRoot\keys_continue.txt")
# -Replay: recorded presses that take the game from boot to gameplay (intros skipped, "Continue", ~20 s; recorded
# with fps_bench.ps1 -Record, pad A as Space). -Replay "" falls back to the old START presses.
$dir = "D:\CODE\SDNX\port\shots"; New-Item -ItemType Directory -Force $dir | Out-Null
$vars = @{ SDNX_NATIVE = "1"; SDNX_DUMP_AT = "$At"; SDNX_DUMP_ADDR = $Addr; SDNX_LOG_AT = $(if ($LogFrame) { "$At" } else { "" }) }
foreach ($k in $Env.Keys) { $vars[$k] = $Env[$k] }
$old = @{}; foreach ($k in $vars.Keys) { $old[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $vars[$k]) }
Remove-Item D:\CODE\SDNX\nx\desktop\out\front_dump.ppm -ErrorAction SilentlyContinue
& "$PSScriptRoot\fps_bench.ps1" -Post -Total ($At + 10) -StartKeyAt 40 -Presses $Presses -Keys $Keys -Replay $Replay -Log "$dir\$Name.log" | Select-Object -Last 1
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $old[$k]) }
python -c "from PIL import Image; Image.open(r'D:\CODE\SDNX\nx\desktop\out\front_dump.ppm').convert('RGB').save(r'$dir\$Name.png')"
