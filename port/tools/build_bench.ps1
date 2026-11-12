# Bench build: VS environment, reconfigure (a changed SDK header otherwise stops ninja after codegen, portlog 16.28),
# ninja sdretail, then the freshly linked rexgpu-xenos.dll next to the exe (only copied when sdretail relinks).
param([switch]$NoConfigure)
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"; $out = "D:\CODE\SDNX\nx\desktop\out"
$log = "D:\CODE\SDNX\nx\desktop\build_last.log"
$cfg = if ($NoConfigure) { "" } else { "cmake $out >nul 2>&1 && " }
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && $cfg ninja -C $out sdretail > $log 2>&1"
$rc = $LASTEXITCODE
Select-String -Path $log -Pattern ' error |FAILED' | Select-Object -First 8 | ForEach-Object { $_.Line }
Get-ChildItem D:\CODE\me-sdk\out\win-amd64\*.dll | ForEach-Object { Copy-Item $_.FullName $out -Force }  # rexgpu-xenos, rexruntime, ...
"build exit $rc"
