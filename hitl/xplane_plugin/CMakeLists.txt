# Copyright CamSim Contributors. All Rights Reserved.
#
# CamSimTruth X-Plane 11 plugin (API level XPLM303).
#
#   ./fetch_sdk.sh
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build && ctest --test-dir build --output-on-failure
#
# Output: build/CamSimTruth/<lin|mac|win>_x64/CamSimTruth.xpl
cmake_minimum_required(VERSION 3.21) # 3.21: C_STANDARD 11 on MSVC (/std:c11)

if(APPLE)
    # One universal .xpl for Intel and Apple Silicon X-Plane 11.50+.
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.14" CACHE STRING "")
endif()

project(CamSimTruth VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Windows: static CRT, so the .xpl doesn't depend on a particular VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(XPLANE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.sdk/SDK" CACHE PATH "X-Plane SDK root (contains CHeaders/ and Libraries/)")
if(NOT EXISTS "${XPLANE_SDK_DIR}/CHeaders/XPLM/XPLMDefs.h")
    message(FATAL_ERROR "X-Plane SDK not found in ${XPLANE_SDK_DIR}: run ./fetch_sdk.sh or set -DXPLANE_SDK_DIR=")
endif()

if(WIN32)
    set(CST_PLAT win_x64)
    set(CST_PLAT_DEFS IBM=1 APL=0 LIN=0 _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
elseif(APPLE)
    set(CST_PLAT mac_x64)
    set(CST_PLAT_DEFS IBM=0 APL=1 LIN=0)
else()
    set(CST_PLAT lin_x64)
    set(CST_PLAT_DEFS IBM=0 APL=0 LIN=1)
endif()

# XPLM303 = X-Plane 11.50+. Deliberately not XPLM400 (X-Plane 12 only).
set(CST_XPLM_DEFS XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1)

if(MSVC)
    set(CST_WARN /W4)
else()
    set(CST_WARN -Wall -Wextra -Wno-unused-parameter)
endif()

# Plugin code as an object library: linked into the .xpl and into the test harness.
add_library(camsim_truth_obj OBJECT
    src/camsim_truth.c
    src/cst_config.c
    src/cst_platform.c)
target_include_directories(camsim_truth_obj PUBLIC
    src
    "${XPLANE_SDK_DIR}/CHeaders/XPLM")
target_compile_definitions(camsim_truth_obj PUBLIC ${CST_PLAT_DEFS} ${CST_XPLM_DEFS})
target_compile_options(camsim_truth_obj PRIVATE ${CST_WARN})

add_library(CamSimTruth MODULE $<TARGET_OBJECTS:camsim_truth_obj>)
set(CST_OUT_DIR "${CMAKE_BINARY_DIR}/CamSimTruth/${CST_PLAT}")
set_target_properties(CamSimTruth PROPERTIES
    PREFIX ""
    SUFFIX ".xpl"
    OUTPUT_NAME CamSimTruth
    LIBRARY_OUTPUT_DIRECTORY "${CST_OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CST_OUT_DIR}")
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${cfg} CFG)
    set_target_properties(CamSimTruth PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${CFG} "${CST_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_${CFG} "${CST_OUT_DIR}")
endforeach()

if(WIN32)
    target_link_libraries(CamSimTruth PRIVATE "${XPLANE_SDK_DIR}/Libraries/Win/XPLM_64.lib" ws2_32)
elseif(APPLE)
    find_library(XPLM_FRAMEWORK XPLM PATHS "${XPLANE_SDK_DIR}/Libraries/Mac" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(CamSimTruth PRIVATE "${XPLM_FRAMEWORK}")
else()
    # Nothing from X-Plane is linked on Linux: XPLM symbols resolve when X-Plane
    # loads the plugin. Static libgcc keeps the .xpl free of toolchain runtime deps.
    target_link_options(CamSimTruth PRIVATE -static-libgcc)
endif()

# Example config, next to the platform folders (copy to CamSimTruth.ini to use).
configure_file(CamSimTruth.ini.example "${CMAKE_BINARY_DIR}/CamSimTruth/CamSimTruth.ini.example" COPYONLY)

# cmake --install build --prefix "<X-Plane 11>/Resources/plugins"
install(TARGETS CamSimTruth LIBRARY DESTINATION CamSimTruth/${CST_PLAT} RUNTIME DESTINATION CamSimTruth/${CST_PLAT})
install(FILES CamSimTruth.ini.example DESTINATION CamSimTruth)

# ---------------------------------------------------------------------------
# Tests (no X-Plane needed): stub XPLM + UDP receiver + Python struct decode.
option(CAMSIM_TRUTH_TESTS "Build the test harness" ON)
if(CAMSIM_TRUTH_TESTS AND NOT WIN32)
    enable_testing()
    add_executable(camsim_truth_test
        tests/test_plugin.c
        tests/xplm_stub.c
        $<TARGET_OBJECTS:camsim_truth_obj>)
    target_include_directories(camsim_truth_test PRIVATE src "${XPLANE_SDK_DIR}/CHeaders/XPLM")
    target_compile_definitions(camsim_truth_test PRIVATE ${CST_PLAT_DEFS} ${CST_XPLM_DEFS})
    target_compile_options(camsim_truth_test PRIVATE ${CST_WARN})

    set(CST_DUMP "${CMAKE_BINARY_DIR}/tests/sample_datagram.bin")
    add_test(NAME plugin_harness COMMAND camsim_truth_test "${CMAKE_BINARY_DIR}/tests" "${CST_DUMP}")
    set_tests_properties(plugin_harness PROPERTIES FIXTURES_SETUP datagram_dump TIMEOUT 60)

    if(NOT APPLE)
        # Load the real .xpl against a shared stub XPLM, as X-Plane does on Linux.
        add_library(xplm_stub SHARED tests/xplm_stub.c)
        target_include_directories(xplm_stub PRIVATE "${XPLANE_SDK_DIR}/CHeaders/XPLM")
        target_compile_definitions(xplm_stub PRIVATE ${CST_PLAT_DEFS} ${CST_XPLM_DEFS} XPLM=1)
        set_target_properties(xplm_stub PROPERTIES C_VISIBILITY_PRESET default)
        add_executable(camsim_truth_dlopen tests/test_dlopen.c)
        target_link_libraries(camsim_truth_dlopen PRIVATE ${CMAKE_DL_LIBS})
        add_test(NAME xpl_dlopen
            COMMAND camsim_truth_dlopen $<TARGET_FILE:xplm_stub> $<TARGET_FILE:CamSimTruth> "${CMAKE_BINARY_DIR}/dlopen_lin_x64")
    endif()

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME python_struct_decode
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/decode_check.py" "${CST_DUMP}")
        set_tests_properties(python_struct_decode PROPERTIES FIXTURES_REQUIRED datagram_dump)
        if(NOT APPLE)
            add_test(NAME xpl_symbols
                COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_symbols.py" $<TARGET_FILE:CamSimTruth>)
        endif()
    endif()
endif()
