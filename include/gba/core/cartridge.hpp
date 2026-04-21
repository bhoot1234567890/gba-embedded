#pragma once

#ifndef GBA_PLATFORM_ESP32
#include <filesystem>
#endif

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
    void set_bios(std::vector<u8> bios);
    void set_save_type(SaveType save_type);

    [[nodiscard]] std::string title() const;
    [[nodiscard]] SaveType save_type() const;
    [[nodiscard]] bool has_bios() const;

    [[nodiscard]] u32 read_bios(u32 address, BusWidth width) const;
    [[nodiscard]] u32 read_rom(u32 address, BusWidth width) const;
    [[nodiscard]] u32 read_save(u32 address, BusWidth width) const;
    void write_save(u32 address, u32 value, BusWidth width);

    [[nodiscard]] std::span<const u8> bios() const;
    [[nodiscard]] std::span<const u8> rom() const;
    [[nodiscard]] std::span<const u8> save() const;

private:
    void resize_save_storage();
    [[nodiscard]] u32 read_vector(std::span<const u8> bytes, u32 address, BusWidth width) const;
    void write_vector(std::vector<u8>& bytes, u32 address, u32 value, BusWidth width);

    std::vector<u8> bios_;
    std::vector<u8> rom_;
    std::vector<u8> save_;
    SaveType save_type_ = SaveType::None;
};

}  // namespace gba
