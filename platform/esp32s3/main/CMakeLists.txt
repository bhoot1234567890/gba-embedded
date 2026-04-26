# GBA emulator main component
set(GBA_BUILD_RUNTIME OFF CACHE BOOL "Build runtime instead of tests")

if(GBA_BUILD_RUNTIME)
    idf_component_register(
        SRCS gba_display.cpp gba_runtime.cpp
        REQUIRES gba_core driver esp_lcd esp_driver_gpio esp_timer fatfs sdmmc vfs
    )
else()
    idf_component_register(
        SRCS gba_test_main.cpp
        REQUIRES gba_core driver esp_lcd esp_driver_gpio esp_timer fatfs sdmmc vfs
    )
endif()
