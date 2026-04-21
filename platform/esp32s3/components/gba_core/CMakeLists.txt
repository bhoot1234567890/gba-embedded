idf_component_register(
    SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/core/apu.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/core/bus.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/core/cartridge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/core/cpu.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/core/dma.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/core/emulator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/core/irq.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/core/log.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/core/ppu.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/core/scheduler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/core/timers.cpp
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include
    REQUIRES esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-shadow
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-double-promotion
    -Wno-pedantic
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    GBA_PLATFORM_ESP32=1
)
