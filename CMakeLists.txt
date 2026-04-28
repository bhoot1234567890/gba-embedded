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
option(GBA_ENABLE_HLE_BIOS "Enable HLE BIOS (SWI) implementation" ON)

if(GBA_ENABLE_HLE_BIOS)
    add_compile_definitions(GBA_ENABLE_HLE_BIOS)
endif()

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

    add_executable(
        gba_alyosha_runner
        tools/run_alyosha_tests.cpp
    )

    target_link_libraries(gba_alyosha_runner PRIVATE gba_core)

    add_executable(
        gba_frame_runner
        tools/frame_runner.cpp
    )

    target_link_libraries(gba_frame_runner PRIVATE gba_core)

    add_executable(
        gba_bug_harness
        tools/bug_harness.cpp
    )

    target_link_libraries(gba_bug_harness PRIVATE gba_core)

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 QUIET sdl2)
    endif()

    if(SDL2_FOUND)
        add_executable(
            gba_sdl_frontend
            tools/sdl_frontend.cpp
        )

        target_include_directories(gba_sdl_frontend PRIVATE ${SDL2_INCLUDE_DIRS})
        target_compile_options(gba_sdl_frontend PRIVATE ${SDL2_CFLAGS_OTHER})
        target_link_directories(gba_sdl_frontend PRIVATE ${SDL2_LIBRARY_DIRS})
        target_link_libraries(gba_sdl_frontend PRIVATE gba_core ${SDL2_LIBRARIES})
    else()
        message(STATUS "SDL2 not found via pkg-config; skipping gba_sdl_frontend")
    endif()

    find_path(MGBA_INCLUDE_DIR mgba/core/core.h
        PATHS
            /opt/homebrew/include
            /opt/homebrew/Cellar/mgba/0.10.5_2/include
            /usr/local/include
    )
    find_library(MGBA_LIBRARY NAMES mgba libmgba
        PATHS
            /opt/homebrew/lib
            /opt/homebrew/Cellar/mgba/0.10.5_2/lib
            /usr/local/lib
    )
    if(MGBA_INCLUDE_DIR AND MGBA_LIBRARY)
        add_executable(
            gba_mgba_ref_harness
            tools/mgba_ref_harness.cpp
        )

        target_include_directories(gba_mgba_ref_harness PRIVATE ${MGBA_INCLUDE_DIR})
        target_link_libraries(gba_mgba_ref_harness PRIVATE ${MGBA_LIBRARY})
    else()
        message(STATUS "mGBA development files not found; skipping gba_mgba_ref_harness")
    endif()
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
