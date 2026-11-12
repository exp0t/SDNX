# Composite debug run: python debug_composite.py <mode>, compile in WSL, swap into the bench package, one shot, restore.
#   debug_composite_run.ps1 -Mode 4 [-Addr 0xfeef000]  -> port/shots/dbg<mode>.png
param([int]$Mode = 0, [string]$Addr = "0xfeef000", [hashtable]$Env = @{})
$shots = "D:\CODE\SDNX\port\shots"; $pkg = "D:\CODE\SDNX\nx\desktop\out\sdnx_shaders\ps_195084cd918f58f5.spv"
python "$PSScriptRoot\debug_composite.py" $Mode | Out-Null
wsl -e bash -c "cd /mnt/d/CODE/SDNX/port/shots && LD_LIBRARY_PATH=`$HOME/tools/dxc/lib `$HOME/tools/dxc/bin/dxc -spirv -T ps_6_6 -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout -Fo ps_dbg.spv ps_195084cd918f58f5_debug$Mode.hlsl 2>&1 | grep -i error"
if (-not (Test-Path "$shots\ps_195084cd918f58f5_orig.spv")) { Copy-Item $pkg "$shots\ps_195084cd918f58f5_orig.spv" }
Copy-Item "$shots\ps_dbg.spv" $pkg -Force
$Env["SDNX_DUMP_NTH"] = "1"
try { & "$PSScriptRoot\native_shot.ps1" -Name "dbg$Mode" -Addr $Addr -Env $Env | Select-Object -Last 1 }
finally { Copy-Item "$shots\ps_195084cd918f58f5_orig.spv" $pkg -Force }
# A run where the game did not reach gameplay has ~30 draws/frame (menu over a stale scene): its picture is useless.
Select-String -Path "$shots\dbg$Mode.log" -Pattern "drawn (\d+)/frame" | Select-Object -Last 1 | ForEach-Object { "draws/frame: " + $_.Matches[0].Groups[1].Value }
