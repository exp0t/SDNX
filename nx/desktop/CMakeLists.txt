# Sleeping Dogs: Windows build against the masseffect-nx SDK fork (D:\CODE\me-sdk), Vulkan backend.
#
# Purpose: run on the PC the same renderer code (Xenos emulation over Vulkan) and the same generated code
# (perf_overrides, post-codegen patches) as the Switch NRO, so graphics defects can be found and fixed here.
# The app sources are shared with the NRO (../sdretail/src). generated/ is a copy of ~/masseffect-nx/sdretail/generated
# (nx/tools/sd_codegen.sh output).
#
#   cmake -S nx/desktop -B nx/desktop/out -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
#         -DCMAKE_CXX_COMPILER=clang++ -DREXSDK_DIR=D:/CODE/me-sdk -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D12=OFF
#         -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_TESTS=OFF -DCMAKE_C_FLAGS=-march=native -DCMAKE_CXX_FLAGS=-march=native

cmake_minimum_required(VERSION 3.25)
project(sdretail LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

set(APP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../sdretail/src")
set(SDRETAIL_SOURCES
    ${APP_SRC}/main.cpp
    ${APP_SRC}/sdnx_fps_limit.cpp
    ${APP_SRC}/sdnx_alloc_trace.cpp
    ${APP_SRC}/sdnx_verify_data.cpp
    ${APP_SRC}/sdnx_perf_hooks.cpp
)
add_executable(sdretail WIN32 ${SDRETAIL_SOURCES})
target_include_directories(sdretail PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${APP_SRC}" "${REXSDK_DIR}/thirdparty/imgui")

rexglue_setup_target(sdretail GPU_PLUGINS xenos)

# (No -O1 override for sdretail_register/init here: clang rejects a precompiled header built at another -O level.)

# Profiling: -DSDNX_PROFILE_SYMBOLS=ON adds CodeView debug info (PDB) and frame pointers to the SDK runtime and GPU
# DLLs only (the generated game code stays as is), for port/tools/thread_sample.ps1 symbolization.
option(SDNX_PROFILE_SYMBOLS "PDB + frame pointers for rexruntime / rexgpu-xenos" OFF)
if(SDNX_PROFILE_SYMBOLS)
  foreach(t rexruntime rexgpu-xenos)
    if(TARGET ${t})
      target_compile_options(${t} PRIVATE -gcodeview -g -fno-omit-frame-pointer)
      target_link_options(${t} PRIVATE -g)
    endif()
  endforeach()
endif()
