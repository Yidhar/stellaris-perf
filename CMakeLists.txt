cmake_minimum_required(VERSION 3.20)
project(StellarisPerf LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# MinHook (the perf plugin's inline hooks)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
)
FetchContent_MakeAvailable(minhook)

# stellaris_perf.dll: the optimizations
add_library(stellaris_perf SHARED
    perf/src/perf_main.cpp
    perf/src/perf_tweaks.cpp
)
target_include_directories(stellaris_perf PRIVATE perf/include sdk)
target_link_libraries(stellaris_perf PRIVATE minhook)

# stellaris_bench.dll: frame counter, pause and speed control for the benchmark scripts
add_library(stellaris_bench SHARED
    bench/src/bench_main.cpp
    bench/src/bench_frame.cpp
)
target_include_directories(stellaris_bench PRIVATE bench/include sdk)
target_link_libraries(stellaris_bench PRIVATE d3d11 dxgi)

foreach(target stellaris_perf stellaris_bench)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if (MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${target} PREFIX "")
endforeach()

# The plugin folder as the launcher expects it (docs/PLUGINS.md of the launcher): stl-plugin.json, the DLL, defaults\.
# Build\plugin\stellaris-perf can be installed or linked with `stl plugin install [--link] build\plugin\stellaris-perf`.
set(PERF_PLUGIN_DIR "${CMAKE_BINARY_DIR}/plugin/stellaris-perf")
add_custom_target(stellaris_perf_plugin ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PERF_PLUGIN_DIR}/defaults"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:stellaris_perf>" "${PERF_PLUGIN_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/plugin/stl-plugin.json" "${PERF_PLUGIN_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/plugin/defaults/stellaris_perf.ini" "${PERF_PLUGIN_DIR}/defaults/"
    DEPENDS stellaris_perf
    COMMENT "Assembling ${PERF_PLUGIN_DIR}")
