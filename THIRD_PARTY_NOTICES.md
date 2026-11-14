# Third-party notices

This repository contains or builds against the following projects.

| Component | Where | License |
| --- | --- | --- |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (hedge-dev), with this port's changes | `nx/shaders/XenosRecomp` | MIT (`nx/shaders/LICENSE.md`) |
| Shader tools from masseffect-nx's `shaders/` folder | `nx/shaders/tools` | MIT (`nx/shaders/LICENSE.md`) |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and its Switch fork | not included; `renderer/` is a backend for it | BSD-3-Clause |
| deko3d core from [rexglue-sdk-nx](https://github.com/nx-mod/rexglue-sdk-nx) (`src/ui/deko3d/nucleo.*`, Tom Clay, from nfsmw-nx) | `nx/deko3d` | BSD-3-Clause (`nx/deko3d/LICENSE.rexglue-sdk-nx`) |
| [deko3d](https://github.com/devkitPro/deko3d), [uam](https://github.com/devkitPro/uam) | not included; Switch GPU library and its shader compiler | zlib / GPL-2.0+ (uam is a build tool only) |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | not included; SPIR-V to GLSL for uam | Apache-2.0 |
| masseffect-nx (Switch port on ReXGlue) | not included; its SDK fork and build tools are used to build the NRO | GPL-3.0 (its app code is not used here) |
| [Mesa](https://mesa3d.org) NVK, built for Horizon | not included; the Vulkan driver of the NRO | MIT |
| [libnx](https://github.com/switchbrew/libnx), devkitA64 ([devkitPro](https://devkitpro.org)) | not included; toolchain | ISC / GPL toolchain |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | not included; HLSL to SPIR-V | University of Illinois / LLVM |
| [xxHash](https://github.com/Cyan4973/xxHash) | not included; used through the SDK | BSD-2-Clause |

The game, its executable, data and shaders are not part of this repository.
