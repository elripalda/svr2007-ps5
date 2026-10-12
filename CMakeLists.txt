# WWE SmackDown vs. Raw 2007 - PS5 port (HollywoodAkeem's SVR07-Recomp on the
# ReXGlue runtime's PS5 platform layer). Build:
#   cmake -S . -B out/ps5 -G Ninja -DCMAKE_TOOLCHAIN_FILE=<svr2011 port>/cmake/ps5-toolchain.cmake \
#         -DPS5_PAYLOAD_SDK=<ps5-payload-sdk> -DREXSDK_DIR=<rexglue-sdk, ps5 branch>
# Code: run 'rexglue codegen svr07_manifest.toml' on the host first (it reads
# assets/default.xex - your own disc's, never in this repository).
cmake_minimum_required(VERSION 3.25)

project(svr07 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

add_executable(svr07 src/main.cpp src/svr07_app.cpp src/crash_report_posix.cpp)
rexglue_setup_target(svr07 GPU_PLUGINS xenos)
if(REXSDK_DIR AND TARGET imgui)
    # SDK source tree: ImGui (built into the runtime) is not on the public
    # include path the way the installed SDK bundles it.
    target_include_directories(svr07 PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
