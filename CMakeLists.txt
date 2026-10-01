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
