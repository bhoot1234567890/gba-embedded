# GBA emulator main component
#
# Two build modes controlled by GBA_BUILD_RUNTIME:
#   OFF  (default) — test runner (gba_test_main.cpp), for QEMU
#   ON           — full runtime with dual-core + SPI display

set(GBA_BUILD_RUNTIME OFF CACHE BOOL "Build runtime instead of tests")

if(GBA_BUILD_RUNTIME)
    idf_component_register(
        SRCS
            gba_display.cpp
            gba_runtime.cpp
        REQUIRES gba_core driver esp_lcd
    )
else()
    idf_component_register(
        SRCS gba_test_main.cpp
        REQUIRES gba_core
    )
endif()
