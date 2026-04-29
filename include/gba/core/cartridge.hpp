#pragma once

#ifndef GBA_PLATFORM_ESP32
#include <filesystem>
#endif

#include <array>
#include <memory>

#include "gba/core/rom_provider.hpp"
#include "gba/core/types.hpp"

namespace gba {

enum class SaveType {
    None,
    Sram,
    Flash64K,
    Flash128K,
    Eeprom,
};

class Cartridge {
public:
#ifndef GBA_PLATFORM_ESP32
    [[nodiscard]] bool load_rom_from_file(const std::filesystem::path& path);
    [[nodiscard]] bool load_bios_from_file(const std::filesystem::path& path);
#endif

    void set_rom(std::vector<u8> rom);
    void set_rom_provider(std::unique_ptr<RomProvider> rom);
    void set_bios(std::vector<u8> bios);
    void set_save_type(SaveType save_type);
    void auto_detect_save_type();
    void set_rtc_enabled(bool enabled);
    void load_save(std::vector<u8> save_data);
    
    [[nodiscard]] bool is_save_dirty() const;
    void clear_save_dirty();

    [[nodiscard]] std::string title() const;
    [[nodiscard]] SaveType save_type() const;
    [[nodiscard]] bool rtc_enabled() const;
    [[nodiscard]] bool has_bios() const;

    [[nodiscard]] u32 read_bios(u32 address, BusWidth width) const;
    [[nodiscard]] u32 read_rom(u32 address, BusWidth width) const;
    void prefetch_rom(u32 address, std::size_t bytes) const;
    void write_rom(u32 address, u32 value, BusWidth width);
    [[nodiscard]] u32 read_save(u32 address, BusWidth width) const;
    void write_save(u32 address, u32 value, BusWidth width);

    [[nodiscard]] std::span<const u8> bios() const;
    [[nodiscard]] std::span<const u8> rom() const;
    [[nodiscard]] std::size_t rom_size() const;
    [[nodiscard]] RomAccessStats rom_frame_stats() const;
    void reset_rom_frame_stats() const;
    [[nodiscard]] std::span<const u8> save() const;

private:
    struct RomFeatureScan {
        bool valid = false;
        bool sram = false;
        bool eeprom = false;
        bool flash1m = false;
        bool flash512 = false;
        bool flash = false;
        bool rtc_v = false;
        bool siirtc = false;
        bool irtc_v = false;
    };

    void resize_save_storage();
    void auto_detect_rtc();
    [[nodiscard]] RomFeatureScan scan_rom_features() const;
    [[nodiscard]] u32 read_vector(std::span<const u8> bytes, u32 address, BusWidth width) const;
    void write_vector(std::vector<u8>& bytes, u32 address, u32 value, BusWidth width);

    // Game Pak GPIO and S-3511-compatible RTC.
    void reset_gpio_state();
    [[nodiscard]] bool read_gpio_byte(u32 address, u8& value) const;
    void write_gpio_byte(u32 address, u8 value);
    [[nodiscard]] u8 gpio_data_read() const;
    [[nodiscard]] u8 rtc_gpio_read() const;
    void rtc_gpio_write(u8 value);
    [[nodiscard]] bool rtc_read_sio_bit();
    void rtc_receive_command();
    void rtc_receive_buffer();
    void rtc_transmit_buffer();
    void rtc_read_register();
    void rtc_write_register();
    [[nodiscard]] int rtc_register_length() const;
    void trace_rtc(const char* event, u8 a = 0, u8 b = 0, u8 c = 0) const;

    // Flash command state machine
    void flash_write(u32 address, u8 value);
    void flash_handle_command(u32 address, u8 command);
    void flash_handle_extended(u32 address, u8 value);
    [[nodiscard]] u32 flash_physical(u32 address) const;

    std::vector<u8> bios_;
    std::unique_ptr<RomProvider> rom_;
    RomFastAccess rom_fast_{};
    const u8* rom_data_ = nullptr;
    std::size_t rom_size_ = 0;
    mutable RomFeatureScan rom_feature_scan_{};
    std::vector<u8> save_;
    SaveType save_type_ = SaveType::None;
    bool save_dirty_ = false;

    // Flash state
    int flash_phase_ = 0;
    bool flash_chip_id_ = false;
    bool flash_erase_enable_ = false;
    bool flash_write_enable_ = false;
    bool flash_bank_select_ = false;
    int flash_bank_ = 0;

    bool gpio_rtc_present_ = false;
    u8 gpio_data_ = 0;
    u8 gpio_direction_ = 0;
    u8 gpio_control_ = 0;

    enum class RtcState : u8 {
        Command,
        Sending,
        Receiving,
        Complete,
    };

    RtcState rtc_state_ = RtcState::Complete;
    u8 rtc_control_ = 0x40;
    u8 rtc_register_ = 0;
    u8 rtc_data_ = 0;
    std::array<u8, 7> rtc_buffer_{};
    int rtc_current_bit_ = 0;
    int rtc_current_byte_ = 0;
    u8 rtc_port_sck_ = 0;
    u8 rtc_port_sio_ = 0;
    u8 rtc_port_cs_ = 0;
};

}  // namespace gba
