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
    flash_phase_ = 0;
    flash_chip_id_ = false;
    flash_erase_enable_ = false;
    flash_write_enable_ = false;
    flash_bank_select_ = false;
    flash_bank_ = 0;
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

    const auto masked = address & 0xFFFFu;

    if (save_type_ == SaveType::Flash64K || save_type_ == SaveType::Flash128K) {
        if (flash_chip_id_ && masked < 2u) {
            const auto id_byte = save_type_ == SaveType::Flash128K
                ? (masked == 0u ? 0xC2u : 0x09u)   // Macronix 128K
                : (masked == 0u ? 0xBFu : 0xD4u);   // SST 64K
            if (width == BusWidth::Byte) return id_byte;
            if (width == BusWidth::Half) return id_byte | (id_byte << 8u);
            return id_byte * 0x01010101u;
        }
    }

    if (save_type_ == SaveType::Sram) {
        const auto offset = address & 0x7FFFu;
        return width == BusWidth::Byte ? save_[offset]
            : width == BusWidth::Half ? static_cast<u32>(save_[offset]) * 0x0101u
            : static_cast<u32>(save_[offset]) * 0x01010101u;
    }

    const auto offset = flash_physical(masked);
    if (width == BusWidth::Byte) return save_[offset];
    const auto byte = save_[offset];
    if (width == BusWidth::Half) return static_cast<u32>(byte) * 0x0101u;
    return static_cast<u32>(byte) * 0x01010101u;
}

void Cartridge::write_save(u32 address, u32 value, BusWidth width) {
    (void)width;
    if (save_type_ == SaveType::None || save_.empty()) {
        return;
    }

    const auto byte_val = static_cast<u8>(value & 0xFFu);

    if (save_type_ == SaveType::Sram) {
        save_[address & 0x7FFFu] = byte_val;
        return;
    }

    flash_write(address, byte_val);
}

void Cartridge::flash_write(u32 address, u8 value) {
    switch (flash_phase_) {
    case 0:
        if (address == 0x005555u && value == 0xAAu) {
            flash_phase_ = 1;
        }
        break;
    case 1:
        if (address == 0x002AAAu && value == 0x55u) {
            flash_phase_ = 2;
        } else {
            flash_phase_ = 0;
        }
        break;
    case 2:
        flash_handle_command(address, value);
        break;
    case 3:
        flash_handle_extended(address, value);
        break;
    }
}

void Cartridge::flash_handle_command(u32 address, u8 command) {
    flash_phase_ = 0;
    switch (command) {
    case 0x90u:  // Enter autoselect/chip ID mode
        flash_chip_id_ = true;
        break;
    case 0xF0u:  // Exit chip ID mode
        flash_chip_id_ = false;
        break;
    case 0x80u:  // Enable erase (next command sequence will be erase type)
        flash_erase_enable_ = true;
        break;
    case 0xA0u:  // Enable write
        flash_write_enable_ = true;
        flash_phase_ = 3;
        break;
    case 0xB0u:  // Bank select (128K only)
        if (save_type_ == SaveType::Flash128K) {
            flash_bank_select_ = true;
            flash_phase_ = 3;
        }
        break;
    case 0x10u:  // Chip erase
        if (flash_erase_enable_) {
            std::fill(save_.begin(), save_.end(), 0xFFu);
            flash_erase_enable_ = false;
        }
        break;
    case 0x30u:  // Sector erase (address determines sector)
        if (flash_erase_enable_) {
            const auto sector_base = flash_physical(address & 0xF000u);
            const auto sector_end = std::min(sector_base + 4096u, static_cast<u32>(save_.size()));
            std::fill(save_.begin() + sector_base, save_.begin() + sector_end, 0xFFu);
            flash_erase_enable_ = false;
        }
        break;
    default:
        break;
    }
}

void Cartridge::flash_handle_extended(u32 address, u8 value) {
    flash_phase_ = 0;
    if (flash_write_enable_) {
        flash_write_enable_ = false;
        const auto offset = flash_physical(address & 0xFFFFu);
        if (offset < save_.size()) {
            save_[offset] = value;
        }
    } else if (flash_bank_select_) {
        flash_bank_select_ = false;
        flash_bank_ = value & 1;
    }
}

u32 Cartridge::flash_physical(u32 address) const {
    if (save_type_ == SaveType::Flash128K) {
        return static_cast<u32>(flash_bank_) * 65536u + address;
    }
    return address;
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
    const auto read_byte = [&](u32 offset) -> u32 {
        return offset < bytes.size() ? bytes[offset] : 0xFFu;
    };

    switch (width) {
    case BusWidth::Byte:
        return address < bytes.size() ? read_byte(address) : 0xFFFFFFFFu;
    case BusWidth::Half: {
        const auto aligned = align_down(address, 2u);
        if (aligned + 2u > bytes.size()) {
            return 0xFFFFFFFFu;
        }
        return read_byte(aligned) | (read_byte(aligned + 1) << 8u);
    }
    case BusWidth::Word: {
        const auto aligned = align_down(address, 4u);
        if (aligned + 4u > bytes.size()) {
            return 0xFFFFFFFFu;
        }
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
