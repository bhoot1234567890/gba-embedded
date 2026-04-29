# GBA emulator main component
set(GBA_BUILD_RUNTIME OFF CACHE BOOL "Build runtime instead of tests")
set(GBA_BUILD_DISPLAY_TEST OFF CACHE BOOL "Build display hardware test")

if(GBA_BUILD_DISPLAY_TEST)
    idf_component_register(
        SRCS gba_display.cpp gba_display_test_main.cpp
        REQUIRES driver esp_lcd esp_driver_gpio esp_timer heap
        PRIV_REQUIRES esp_mm
    )
elseif(GBA_BUILD_RUNTIME)
    idf_component_register(
        SRCS gba_audio.cpp gba_display.cpp gba_input.cpp gba_runtime.cpp
        REQUIRES gba_core driver esp_lcd esp_driver_gpio esp_driver_i2s esp_psram esp_timer fatfs heap sdmmc vfs
        PRIV_REQUIRES esp_mm
    )
else()
    idf_component_register(
        SRCS gba_test_main.cpp
        REQUIRES gba_core driver esp_lcd esp_driver_gpio esp_timer fatfs heap sdmmc vfs
    )
endif()

set(GBA_BOARD_COMPILE_DEFS
    GBA_DISPLAY_DRIVER
    GBA_DISPLAY_WIDTH
    GBA_DISPLAY_HEIGHT
    GBA_DISPLAY_PIN_SCLK
    GBA_DISPLAY_PIN_MOSI
    GBA_DISPLAY_PIN_DC
    GBA_DISPLAY_PIN_CS
    GBA_DISPLAY_PIN_RST
    GBA_DISPLAY_PIN_BL
    GBA_DISPLAY_SPI_CLK_HZ
    GBA_DISPLAY_SWAP_BYTES
    GBA_DISPLAY_COL_OFFSET
    GBA_DISPLAY_ROW_OFFSET
    GBA_DISPLAY_SSD1351_MUX_RATIO
    GBA_DISPLAY_SSD1351_STARTLINE
    GBA_STORAGE_MODE
    GBA_SDMMC_PIN_CLK
    GBA_SDMMC_PIN_CMD
    GBA_SDMMC_PIN_D0
    GBA_SDMMC_PIN_D1
    GBA_SDMMC_PIN_D2
    GBA_SDMMC_PIN_D3
    GBA_SDSPI_HOST
    GBA_SDSPI_PIN_MISO
    GBA_SDSPI_PIN_MOSI
    GBA_SDSPI_PIN_SCLK
    GBA_SDSPI_PIN_CS
    GBA_INPUT_PIN_A
    GBA_INPUT_PIN_B
    GBA_INPUT_PIN_SELECT
    GBA_INPUT_PIN_START
    GBA_INPUT_PIN_RIGHT
    GBA_INPUT_PIN_LEFT
    GBA_INPUT_PIN_UP
    GBA_INPUT_PIN_DOWN
    GBA_INPUT_PIN_R
    GBA_INPUT_PIN_L
    GBA_AUDIO_PIN_BCK
    GBA_AUDIO_PIN_WS
    GBA_AUDIO_PIN_DOUT
    GBA_AUDIO_WRITE_TIMEOUT_MS
    GBA_ROM_BACKEND
    GBA_ROM_SD_CACHE_BYTES
    GBA_ROM_SD_CACHE_PAGE_BYTES
    GBA_ROM_MMAP_WINDOW_BYTES
    GBA_ROM_MMAP_WINDOW_COUNT
)

foreach(def IN LISTS GBA_BOARD_COMPILE_DEFS)
    if(DEFINED ${def})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${def}=${${def}})
    endif()
endforeach()

target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_mm)
