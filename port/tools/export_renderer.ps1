# Copies the native renderer (sdnx backend) from the SDK fork it is developed in (D:\CODE\me-sdk) into renderer/ of
# this repository: src/graphics/sdnx -> renderer/src, include/rex/graphics/sdnx -> renderer/include.
param([string]$Sdk = "D:\CODE\me-sdk")
$r = Join-Path $PSScriptRoot "..\..\renderer"
New-Item -ItemType Directory -Force "$r\src\shaders", "$r\include" | Out-Null
Copy-Item "$Sdk\src\graphics\sdnx\*" "$r\src" -Recurse -Force
Copy-Item "$Sdk\include\rex\graphics\sdnx\*" "$r\include" -Force
Get-ChildItem $r -Recurse -File | Measure-Object | ForEach-Object { "renderer: $($_.Count) files" }
