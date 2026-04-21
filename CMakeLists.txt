cmake_minimum_required(VERSION 3.25)

project(
    gba_embedded
    VERSION 0.1.0
    DESCRIPTION "Portable cycle-scheduled GBA emulator core for embedded targets"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GBA_BUILD_TESTS "Build emulator tests" ON)
option(GBA_BUILD_TOOLS "Build host-side emulator tools" ON)
option(GBA_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

add_library(
    gba_core
    STATIC
    src/core/apu.cpp
    src/core/bus.cpp
    src/core/cartridge.cpp
    src/core/cpu.cpp
    src/core/dma.cpp
    src/core/emulator.cpp
    src/core/irq.cpp
    src/core/log.cpp
    src/core/ppu.cpp
    src/core/scheduler.cpp
    src/core/timers.cpp
    src/platform/rt1170/rt1170_platform.cpp
)

target_include_directories(gba_core PUBLIC include)

target_compile_features(gba_core PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
        gba_core
        PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
    )
elseif(MSVC)
    target_compile_options(gba_core PRIVATE /W4 /permissive-)
endif()

if(GBA_BUILD_TOOLS)
    add_executable(
        gba_trace_runner
        tests/trace_runner.cpp
    )

    target_link_libraries(gba_trace_runner PRIVATE gba_core)

    add_executable(
        gba_suite_runner
        tests/suite_runner.cpp
    )

    target_link_libraries(gba_suite_runner PRIVATE gba_core)
endif()

if(GBA_WARNINGS_AS_ERRORS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(gba_core PRIVATE -Werror)
    elseif(MSVC)
        target_compile_options(gba_core PRIVATE /WX)
    endif()
endif()

if(GBA_BUILD_TESTS)
    enable_testing()

    add_executable(
        gba_tests
        tests/test_main.cpp
    )

    target_link_libraries(gba_tests PRIVATE gba_core)

    add_test(NAME gba_tests COMMAND gba_tests)
endif()
