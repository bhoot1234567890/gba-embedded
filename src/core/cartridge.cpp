#include "gba/core/cartridge.hpp"

#include <algorithm>

#ifndef GBA_PLATFORM_ESP32
#include <fstream>
#include <iterator>
#endif

#include "gba/core/constants.hpp"

namespace gba {

#ifndef GBA_PLATFORM_ESP32
namespace {

[[nodiscard]] std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }

    return std::vector<u8>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}  // namespace

bool Cartridge::load_rom_from_file(const std::filesystem::path& path) {
    auto rom = read_file(path);
    if (rom.empty()) {
        return false;
    }
    set_rom(std::move(rom));
    return true;
}

bool Cartridge::load_bios_from_file(const std::filesystem::path& path) {
    auto bios = read_file(path);
    if (bios.empty()) {
        return false;
    }
    set_bios(std::move(bios));
    return true;
}
#endif

void Cartridge::set_rom(std::vector<u8> rom) {
    rom_ = std::move(rom);
    if (rom_.size() > kMaxRomSize) {
        rom_.resize(kMaxRomSize);
    }
}

void Cartridge::set_bios(std::vector<u8> bios) {
    bios_ = std::move(bios);
    if (bios_.size() > kBiosSize) {
        bios_.resize(kBiosSize);
    }
}

void Cartridge::set_save_type(SaveType save_type) {
    save_type_ = save_type;
    resize_save_storage();
}

std::string Cartridge::title() const {
    if (rom_.size() < 0x00AC) {
        return {};
    }

    std::string title;
    for (std::size_t index = 0x00A0; index < 0x00AC && index < rom_.size(); ++index) {
        const auto byte = rom_[index];
        if (byte == 0) {
            break;
        }
        title.push_back(static_cast<char>(byte));
    }
    return title;
}

SaveType Cartridge::save_type() const {
    return save_type_;
}

bool Cartridge::has_bios() const {
    return !bios_.empty();
}

u32 Cartridge::read_bios(u32 address, BusWidth width) const {
    return read_vector(bios_, address, width);
}

u32 Cartridge::read_rom(u32 address, BusWidth width) const {
    return read_vector(rom_, address, width);
}

u32 Cartridge::read_save(u32 address, BusWidth width) const {
    if (save_type_ == SaveType::None || save_.empty()) {
        return 0xFFFFFFFFu;
    }

    const auto offset = address % static_cast<u32>(save_.size());
    if (width == BusWidth::Byte) {
        return save_[offset];
    }

    const auto byte = save_[offset];
    if (width == BusWidth::Half) {
        return static_cast<u32>(byte) * 0x0101u;
    }
    return static_cast<u32>(byte) * 0x01010101u;
}

void Cartridge::write_save(u32 address, u32 value, BusWidth width) {
    if (save_type_ == SaveType::None || save_.empty()) {
        return;
    }

    const auto offset = address % static_cast<u32>(save_.size());
    switch (width) {
    case BusWidth::Byte:
        save_[offset] = static_cast<u8>(value);
        break;
    case BusWidth::Half:
    case BusWidth::Word:
        save_[offset] = static_cast<u8>(value & 0xFFu);
        break;
    }
}

std::span<const u8> Cartridge::bios() const {
    return bios_;
}

std::span<const u8> Cartridge::rom() const {
    return rom_;
}

std::span<const u8> Cartridge::save() const {
    return save_;
}

void Cartridge::resize_save_storage() {
    switch (save_type_) {
    case SaveType::None:
        save_.clear();
        break;
    case SaveType::Sram:
    case SaveType::Flash64K:
        save_.assign(64u * 1024u, 0xFF);
        break;
    case SaveType::Flash128K:
        save_.assign(128u * 1024u, 0xFF);
        break;
    case SaveType::Eeprom:
        save_.assign(8u * 1024u, 0xFF);
        break;
    }
}

u32 Cartridge::read_vector(std::span<const u8> bytes, u32 address, BusWidth width) const {
    if (bytes.empty()) {
        return 0xFFFFFFFFu;
    }

    const auto normalized = address % static_cast<u32>(bytes.size());
    const auto read_byte = [&](u32 offset) -> u32 {
        return bytes[offset % static_cast<u32>(bytes.size())];
    };

    switch (width) {
    case BusWidth::Byte:
        return read_byte(normalized);
    case BusWidth::Half: {
        const auto aligned = align_down(normalized, 2u);
        return read_byte(aligned) | (read_byte(aligned + 1) << 8u);
    }
    case BusWidth::Word: {
        const auto aligned = align_down(normalized, 4u);
        return read_byte(aligned) | (read_byte(aligned + 1) << 8u) | (read_byte(aligned + 2) << 16u) |
               (read_byte(aligned + 3) << 24u);
    }
    }
    return 0xFFFFFFFFu;
}

void Cartridge::write_vector(std::vector<u8>& bytes, u32 address, u32 value, BusWidth width) {
    if (bytes.empty()) {
        return;
    }

    const auto normalized = address % static_cast<u32>(bytes.size());
    switch (width) {
    case BusWidth::Byte:
        bytes[normalized] = static_cast<u8>(value);
        break;
    case BusWidth::Half: {
        const auto aligned = align_down(normalized, 2u);
        bytes[aligned % bytes.size()] = static_cast<u8>(value & 0xFFu);
        bytes[(aligned + 1) % bytes.size()] = static_cast<u8>((value >> 8u) & 0xFFu);
        break;
    }
    case BusWidth::Word: {
        const auto aligned = align_down(normalized, 4u);
        bytes[aligned % bytes.size()] = static_cast<u8>(value & 0xFFu);
        bytes[(aligned + 1) % bytes.size()] = static_cast<u8>((value >> 8u) & 0xFFu);
        bytes[(aligned + 2) % bytes.size()] = static_cast<u8>((value >> 16u) & 0xFFu);
        bytes[(aligned + 3) % bytes.size()] = static_cast<u8>((value >> 24u) & 0xFFu);
        break;
    }
    }
}

}  // namespace gba
