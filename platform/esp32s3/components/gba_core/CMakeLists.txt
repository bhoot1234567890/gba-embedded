set(GBA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)

idf_component_register(
    SRCS
        ${GBA_ROOT}/src/core/apu.cpp
        ${GBA_ROOT}/src/core/bus.cpp
        ${GBA_ROOT}/src/core/cartridge.cpp
        ${GBA_ROOT}/src/core/cpu.cpp
        ${GBA_ROOT}/src/core/dma.cpp
        ${GBA_ROOT}/src/core/emulator.cpp
        ${GBA_ROOT}/src/core/irq.cpp
        ${GBA_ROOT}/src/core/log.cpp
        ${GBA_ROOT}/src/core/ppu.cpp
        ${GBA_ROOT}/src/core/scheduler.cpp
        ${GBA_ROOT}/src/core/timers.cpp
    INCLUDE_DIRS
        ${GBA_ROOT}/include
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
