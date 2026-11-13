# Sleeping Dogs (Xbox 360 retail) for Nintendo Switch: static recompilation host application.
#
# The game code is not in this repository: generated/ holds the C++ recompiled from your own default.xex
# (rexglue codegen sdretail_manifest.toml). A cross build cannot run rexglue itself.
#
# Build (devkitA64 container):
#   cmake -S sdretail -B out/nx-sd -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/tools/switch/cmake/switch-devkitA64.cmake \
#     -DREXGLUE_SWITCH_NVK_SDK=<mesa sdk>/opt/devkitpro/portlibs/switch
#   cmake --build out/nx-sd -j8
# Output: out/nx-sd/sdretail.nro

cmake_minimum_required(VERSION 3.25)
project(sdretail LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK is built from source in this repository unless another tree is given.
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

include(generated/rexglue.cmake)

set(SDRETAIL_SOURCES
    src/main.cpp
    src/sdnx_fps_limit.cpp
    src/sdnx_alloc_trace.cpp
    src/sdnx_verify_data.cpp
    src/sdnx_perf_hooks.cpp
    src/sdnx_memset.cpp
)

add_executable(sdretail ${SDRETAIL_SOURCES})
set_source_files_properties(${SDRETAIL_SOURCES} PROPERTIES COMPILE_OPTIONS "-g1")

# Xenos GPU plugin stays available; no native renderer yet.
rexglue_setup_target(sdretail GPU_PLUGINS xenos)

# The SDK sets these with add_compile_options in its own directory only, so the game code built here did not get
# them. Without -ffp-contract=off GCC fuses separate PPC fmul + fadd into one FMA on aarch64 and the results drift
# by one ulp: the Scaleform hash capacity 1 << int(log2(n - 1) + 1) came out 64 instead of 32 on the console, the
# UI heap went another way and, a few hundred requests later, asked for 0x41FFF928 bytes (first console runs,
# 2026-11-11). The PC build (clang, x86 without FMA) never fuses. The game code itself is in sdretail_recomp.
foreach(_target sdretail sdretail_recomp)
    if(TARGET ${_target})
        target_compile_options(${_target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-ffp-contract=off>
            $<$<CXX_COMPILER_ID:GNU>:-fno-strict-aliasing>)
    endif()
endforeach()

# Zero fills of 160+ bytes without newlib's DC ZVA path (src/sdnx_memset.cpp: 10.6 % of the GPU thread on the console).
if(REXGLUE_PLATFORM_SWITCH)
    target_link_options(sdretail PRIVATE -Wl,--wrap=memset)
endif()

# sdretail_register.cpp is a 4 MB table of 82k function registrations: -O3 on it costs tens of
# minutes of compile time and changes nothing at runtime (no hot code path).
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sdretail_register.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sdretail_init.cpp"
    PROPERTIES COMPILE_OPTIONS "-O1")

if(REXGLUE_PLATFORM_SWITCH)
    set(SDRETAIL_VERSION "0.1.0" CACHE STRING "NRO display version")
    rexglue_switch_add_nro(sdretail
        NAME "Sleeping Dogs"
        AUTHOR "SDNX"
        VERSION "${SDRETAIL_VERSION}")
endif()
