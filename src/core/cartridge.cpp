#include "gba/core/cartridge.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <time.h>

#ifndef GBA_PLATFORM_ESP32
#include <fstream>
#include <iterator>
#endif

#include "gba/core/constants.hpp"

namespace gba {

namespace {

constexpr u32 kGpioData = 0xC4u;
constexpr u32 kGpioDirection = 0xC6u;
constexpr u32 kGpioControl = 0xC8u;
constexpr u8 kRtcPinSck = 1u << 0u;
constexpr u8 kRtcPinSio = 1u << 1u;
constexpr u8 kRtcPinCs = 1u << 2u;
constexpr std::size_t kRomFeatureScanChunkSize = 16u * 1024u;
constexpr std::array<int, 8> kRtcRegisterLengths = {
    0,  // Force reset
    0,  // Unused
    7,  // Datetime
    0,  // Force IRQ
    1,  // Control
    0,  // Unused
    3,  // Time
    0,  // Free
};

[[nodiscard]] u8 decimal_to_bcd(int value) {
    value %= 100;
    if (value < 0) {
        value += 100;
    }
    const auto ones = value % 10;
    const auto tens = value / 10;
    return static_cast<u8>((tens << 4) | ones);
}

[[nodiscard]] u8 reverse_bits(u8 value) {
    value = static_cast<u8>(((value & 0x33u) << 2u) | ((value & 0xCCu) >> 2u));
    value = static_cast<u8>(((value & 0x55u) << 1u) | ((value & 0xAAu) >> 1u));
    return value;
}

[[nodiscard]] std::tm local_time_now() {
    std::tm result{};
    const auto timestamp = std::time(nullptr);
#if defined(_WIN32)
    localtime_s(&result, &timestamp);
#else
    if (localtime_r(&timestamp, &result) == nullptr) {
        result = {};
    }
#endif
    return result;
}

#ifndef GBA_PLATFORM_ESP32
[[nodiscard]] std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }

    return std::vector<u8>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
#endif

}  // namespace

#ifndef GBA_PLATFORM_ESP32
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
    if (rom.size() > kMaxRomSize) {
        rom.resize(kMaxRomSize);
    }
    set_rom_provider(std::make_unique<MemoryRomProvider>(std::move(rom)));
}

void Cartridge::set_rom_provider(std::unique_ptr<RomProvider> rom) {
    rom_ = std::move(rom);
    rom_feature_scan_ = {};
    auto_detect_rtc();
    reset_gpio_state();
}

void Cartridge::set_bios(std::vector<u8> bios) {
    bios_ = std::move(bios);
    if (bios_.size() > kBiosSize) {
        bios_.resize(kBiosSize);
    }
}

void Cartridge::auto_detect_save_type() {
    save_type_ = SaveType::None;
    if (!rom_ || rom_->empty()) return;
    const auto features = scan_rom_features();
    gpio_rtc_present_ = features.rtc_v || features.siirtc || features.irtc_v;

    if (features.sram) {
        set_save_type(SaveType::Sram);
    } else if (features.eeprom) {
        set_save_type(SaveType::Eeprom);
    } else if (features.flash1m) {
        set_save_type(SaveType::Flash128K);
    } else if (features.flash512 || features.flash) {
        set_save_type(SaveType::Flash64K);
    } else {
        // Default to SRAM if nothing found (common fallback)
        set_save_type(SaveType::Sram);
    }
}

void Cartridge::set_rtc_enabled(bool enabled) {
    gpio_rtc_present_ = enabled;
    reset_gpio_state();
}

void Cartridge::load_save(std::vector<u8> save_data) {
    if (save_data.empty()) return;
    
    // If we haven't set a save type but we have a save file, try to infer from size
    if (save_type_ == SaveType::None) {
        if (save_data.size() == 128 * 1024) set_save_type(SaveType::Flash128K);
        else if (save_data.size() == 64 * 1024) set_save_type(SaveType::Flash64K);
        else if (save_data.size() <= 8 * 1024) set_save_type(SaveType::Eeprom);
        else set_save_type(SaveType::Sram);
    }
    
    save_ = std::move(save_data);
    if (save_.size() != 64*1024 && save_.size() != 128*1024 && save_.size() != 8*1024) {
        resize_save_storage(); // ensure exact size
    }
    save_dirty_ = false;
}

bool Cartridge::is_save_dirty() const {
    return save_dirty_;
}

void Cartridge::clear_save_dirty() {
    save_dirty_ = false;
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
    if (!rom_ || rom_->size() < 0x00AC) {
        return {};
    }

    std::string title;
    for (u32 index = 0x00A0; index < 0x00AC && index < rom_->size(); ++index) {
        const auto byte = rom_->read_byte(index);
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

bool Cartridge::rtc_enabled() const {
    return gpio_rtc_present_;
}

bool Cartridge::has_bios() const {
    return !bios_.empty();
}

u32 Cartridge::read_bios(u32 address, BusWidth width) const {
    return read_vector(bios_, address, width);
}

u32 Cartridge::read_rom(u32 address, BusWidth width) const {
    if (!rom_ || rom_->empty()) {
        return 0xFFFFFFFFu;
    }

    const auto read_byte = [&](u32 byte_address) -> u32 {
        u8 gpio_value = 0;
        if (read_gpio_byte(byte_address, gpio_value)) {
            return gpio_value;
        }
        return byte_address < rom_->size() ? rom_->read_byte(byte_address) : 0xFFu;
    };

    const auto has_gpio_byte = [&](u32 start, u32 count) -> bool {
        for (u32 i = 0; i < count; ++i) {
            u8 gpio_value = 0;
            if (read_gpio_byte(start + i, gpio_value)) {
                return true;
            }
        }
        return false;
    };

    switch (width) {
    case BusWidth::Byte:
        if (address >= rom_->size() && !has_gpio_byte(address, 1u)) {
            return 0xFFFFFFFFu;
        }
        return read_byte(address);
    case BusWidth::Half: {
        const auto aligned = align_down(address, 2u);
        if (aligned + 2u > rom_->size() && !has_gpio_byte(aligned, 2u)) {
            return 0xFFFFFFFFu;
        }
        return read_byte(aligned) | (read_byte(aligned + 1u) << 8u);
    }
    case BusWidth::Word: {
        const auto aligned = align_down(address, 4u);
        if (aligned + 4u > rom_->size() && !has_gpio_byte(aligned, 4u)) {
            return 0xFFFFFFFFu;
        }
        return read_byte(aligned) | (read_byte(aligned + 1u) << 8u) | (read_byte(aligned + 2u) << 16u) |
               (read_byte(aligned + 3u) << 24u);
    }
    }
    return 0xFFFFFFFFu;
}

void Cartridge::prefetch_rom(u32 address, std::size_t bytes) const {
    if (rom_) {
        rom_->prefetch(address, bytes);
    }
}

void Cartridge::write_rom(u32 address, u32 value, BusWidth width) {
    if (!gpio_rtc_present_) {
        return;
    }

    switch (width) {
    case BusWidth::Byte:
        write_gpio_byte(address, static_cast<u8>(value & 0xFFu));
        break;
    case BusWidth::Half: {
        const auto aligned = align_down(address, 2u);
        write_gpio_byte(aligned, static_cast<u8>(value & 0xFFu));
        break;
    }
    case BusWidth::Word: {
        const auto aligned = align_down(address, 4u);
        write_gpio_byte(aligned, static_cast<u8>(value & 0xFFu));
        write_gpio_byte(aligned + 2u, static_cast<u8>((value >> 16u) & 0xFFu));
        break;
    }
    }
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
        save_dirty_ = true;
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
            save_dirty_ = true;
            flash_erase_enable_ = false;
        }
        break;
    case 0x30u:  // Sector erase (address determines sector)
        if (flash_erase_enable_) {
            const auto sector_base = flash_physical(address & 0xF000u);
            const auto sector_end = std::min(sector_base + 4096u, static_cast<u32>(save_.size()));
            std::fill(save_.begin() + sector_base, save_.begin() + sector_end, 0xFFu);
            save_dirty_ = true;
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
            save_dirty_ = true;
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
    return rom_ ? rom_->contiguous_span() : std::span<const u8>{};
}

std::size_t Cartridge::rom_size() const {
    return rom_ ? rom_->size() : 0;
}

RomAccessStats Cartridge::rom_frame_stats() const {
    return rom_ ? rom_->frame_stats() : RomAccessStats{};
}

void Cartridge::reset_rom_frame_stats() const {
    if (rom_) {
        rom_->reset_frame_stats();
    }
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

void Cartridge::auto_detect_rtc() {
    if (!rom_ || rom_->empty()) {
        gpio_rtc_present_ = false;
        return;
    }

    const auto features = scan_rom_features();
    gpio_rtc_present_ = features.rtc_v || features.siirtc || features.irtc_v;
}

Cartridge::RomFeatureScan Cartridge::scan_rom_features() const {
    if (rom_feature_scan_.valid) {
        return rom_feature_scan_;
    }

    RomFeatureScan scan{};
    struct Needle {
        std::string_view text;
        bool RomFeatureScan::*field;
    };
    static constexpr std::array<Needle, 8> kNeedles = {{
        {"SRAM_V", &RomFeatureScan::sram},
        {"EEPROM_V", &RomFeatureScan::eeprom},
        {"FLASH1M_V", &RomFeatureScan::flash1m},
        {"FLASH512_V", &RomFeatureScan::flash512},
        {"FLASH_V", &RomFeatureScan::flash},
        {"RTC_V", &RomFeatureScan::rtc_v},
        {"SIIRTC", &RomFeatureScan::siirtc},
        {"IRTC_V", &RomFeatureScan::irtc_v},
    }};

    std::size_t max_needle = 0;
    for (const auto& needle : kNeedles) {
        max_needle = std::max(max_needle, needle.text.size());
    }

    if (rom_ && max_needle != 0) {
        std::vector<u8> buffer(kRomFeatureScanChunkSize + max_needle - 1u);
        std::size_t carry = 0;
        for (std::size_t offset = 0; offset < rom_->size();) {
            const auto chunk = std::min(kRomFeatureScanChunkSize, rom_->size() - offset);
            if (!rom_->read_bytes(static_cast<u32>(offset), std::span<u8>(buffer.data() + carry, chunk))) {
                break;
            }

            const auto valid = carry + chunk;
            for (const auto& needle : kNeedles) {
                if (scan.*(needle.field)) {
                    continue;
                }
                if (std::search(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(valid),
                                needle.text.begin(), needle.text.end(), [](u8 lhs, char rhs) {
                                    return lhs == static_cast<u8>(rhs);
                                }) != buffer.begin() + static_cast<std::ptrdiff_t>(valid)) {
                    scan.*(needle.field) = true;
                }
            }

            bool complete = true;
            for (const auto& needle : kNeedles) {
                complete = complete && scan.*(needle.field);
            }
            if (complete) {
                break;
            }

            carry = std::min(max_needle - 1u, valid);
            if (carry != 0) {
                std::memmove(buffer.data(), buffer.data() + valid - carry, carry);
            }
            offset += chunk;
        }
    }

    scan.valid = true;
    rom_feature_scan_ = scan;
    return scan;
}

void Cartridge::reset_gpio_state() {
    gpio_data_ = 0;
    gpio_direction_ = 0;
    gpio_control_ = 0;
    rtc_state_ = RtcState::Complete;
    rtc_control_ = 0x40u;
    rtc_register_ = 0;
    rtc_data_ = 0;
    rtc_buffer_.fill(0);
    rtc_current_bit_ = 0;
    rtc_current_byte_ = 0;
    rtc_port_sck_ = 0;
    rtc_port_sio_ = 0;
    rtc_port_cs_ = 0;
}

bool Cartridge::read_gpio_byte(u32 address, u8& value) const {
    if (!gpio_rtc_present_ || (gpio_control_ & 1u) == 0u) {
        return false;
    }

    switch (address & 0x01FFFFFFu) {
    case kGpioData:
        value = gpio_data_read();
        return true;
    case kGpioData + 1u:
        value = 0;
        return true;
    case kGpioDirection:
        value = static_cast<u8>(gpio_direction_ & 0x0Fu);
        return true;
    case kGpioDirection + 1u:
        value = 0;
        return true;
    case kGpioControl:
        value = static_cast<u8>(gpio_control_ & 1u);
        return true;
    case kGpioControl + 1u:
        value = 0;
        return true;
    default:
        return false;
    }
}

void Cartridge::write_gpio_byte(u32 address, u8 value) {
    if (!gpio_rtc_present_) {
        return;
    }

    switch (address & 0x01FFFFFFu) {
    case kGpioData:
        gpio_data_ = static_cast<u8>((gpio_data_ & static_cast<u8>(~gpio_direction_)) | (value & gpio_direction_));
        trace_rtc("gpio-data", static_cast<u8>(address), value, gpio_data_);
        rtc_gpio_write(gpio_data_);
        break;
    case kGpioDirection:
        gpio_direction_ = static_cast<u8>(value & 0x0Fu);
        trace_rtc("gpio-dir", static_cast<u8>(address), value, gpio_direction_);
        break;
    case kGpioControl:
        gpio_control_ = static_cast<u8>(value & 1u);
        trace_rtc("gpio-ctl", static_cast<u8>(address), value, gpio_control_);
        break;
    default:
        break;
    }
}

u8 Cartridge::gpio_data_read() const {
    const auto external = rtc_gpio_read();
    return static_cast<u8>(((gpio_data_ & gpio_direction_) | (external & static_cast<u8>(~gpio_direction_))) & 0x0Fu);
}

u8 Cartridge::rtc_gpio_read() const {
    return static_cast<u8>((rtc_port_sio_ != 0u && rtc_port_cs_ != 0u) ? kRtcPinSio : 0u);
}

void Cartridge::rtc_gpio_write(u8 value) {
    const auto old_sck = rtc_port_sck_;
    const auto old_cs = rtc_port_cs_;

    if ((gpio_direction_ & kRtcPinCs) != 0u) {
        rtc_port_cs_ = static_cast<u8>((value & kRtcPinCs) != 0u ? 1u : 0u);
    }
    if ((gpio_direction_ & kRtcPinSck) != 0u) {
        rtc_port_sck_ = static_cast<u8>((value & kRtcPinSck) != 0u ? 1u : 0u);
    }
    if ((gpio_direction_ & kRtcPinSio) != 0u) {
        rtc_port_sio_ = static_cast<u8>((value & kRtcPinSio) != 0u ? 1u : 0u);
    }

    if (rtc_port_cs_ == 0u) {
        rtc_state_ = RtcState::Complete;
        rtc_current_bit_ = 0;
        rtc_current_byte_ = 0;
        rtc_data_ = 0;
        return;
    }

    if (old_cs == 0u) {
        rtc_state_ = RtcState::Command;
        rtc_current_bit_ = 0;
        rtc_current_byte_ = 0;
        rtc_data_ = 0;
        trace_rtc("start", value, 0, 0);
        return;
    }

    if (old_sck == 0u && rtc_port_sck_ != 0u) {
        switch (rtc_state_) {
        case RtcState::Command:
            rtc_receive_command();
            break;
        case RtcState::Receiving:
            rtc_receive_buffer();
            break;
        case RtcState::Sending:
            rtc_transmit_buffer();
            break;
        case RtcState::Complete:
            break;
        }
    }
}

bool Cartridge::rtc_read_sio_bit() {
    const auto mask = static_cast<u8>(1u << static_cast<unsigned>(rtc_current_bit_));
    rtc_data_ = static_cast<u8>(rtc_data_ & static_cast<u8>(~mask));
    if (rtc_port_sio_ != 0u) {
        rtc_data_ = static_cast<u8>(rtc_data_ | mask);
    }

    ++rtc_current_bit_;
    if (rtc_current_bit_ == 8) {
        rtc_current_bit_ = 0;
        return true;
    }
    return false;
}

void Cartridge::rtc_receive_command() {
    if (!rtc_read_sio_bit()) {
        return;
    }

    auto command = rtc_data_;
    if ((command >> 4u) == 6u) {
        command = reverse_bits(command);
    } else if ((command & 0x0Fu) != 6u) {
        trace_rtc("bad-command", rtc_data_, command, 0);
        rtc_state_ = RtcState::Complete;
        return;
    }

    rtc_register_ = static_cast<u8>((command >> 4u) & 7u);
    trace_rtc("command", rtc_data_, command, rtc_register_);
    rtc_current_bit_ = 0;
    rtc_current_byte_ = 0;
    rtc_data_ = 0;

    if ((command & 0x80u) != 0u) {
        rtc_read_register();
        rtc_state_ = rtc_register_length() > 0 ? RtcState::Sending : RtcState::Complete;
    } else if (rtc_register_length() > 0) {
        rtc_buffer_.fill(0);
        rtc_state_ = RtcState::Receiving;
    } else {
        rtc_write_register();
        rtc_state_ = RtcState::Complete;
    }
}

void Cartridge::rtc_receive_buffer() {
    if (rtc_current_byte_ >= rtc_register_length()) {
        rtc_state_ = RtcState::Complete;
        return;
    }

    if (!rtc_read_sio_bit()) {
        return;
    }

    rtc_buffer_[static_cast<std::size_t>(rtc_current_byte_)] = rtc_data_;
    ++rtc_current_byte_;
    rtc_data_ = 0;
    if (rtc_current_byte_ == rtc_register_length()) {
        rtc_write_register();
        rtc_state_ = RtcState::Complete;
    }
}

void Cartridge::rtc_transmit_buffer() {
    if (rtc_current_byte_ >= rtc_register_length()) {
        rtc_state_ = RtcState::Complete;
        return;
    }

    auto& byte = rtc_buffer_[static_cast<std::size_t>(rtc_current_byte_)];
    rtc_port_sio_ = static_cast<u8>(byte & 1u);
    byte = static_cast<u8>(byte >> 1u);

    ++rtc_current_bit_;
    if (rtc_current_bit_ == 8) {
        rtc_current_bit_ = 0;
        ++rtc_current_byte_;
        if (rtc_current_byte_ == rtc_register_length()) {
            rtc_state_ = RtcState::Complete;
        }
    }
}

void Cartridge::rtc_read_register() {
    switch (rtc_register_) {
    case 2u: {
        auto now = local_time_now();
        auto hour = now.tm_hour;
        if ((rtc_control_ & 0x40u) == 0u && hour >= 12) {
            hour = (hour - 12) | 0x80;
        }
        rtc_buffer_[0] = decimal_to_bcd(now.tm_year);
        rtc_buffer_[1] = decimal_to_bcd(now.tm_mon + 1);
        rtc_buffer_[2] = decimal_to_bcd(now.tm_mday);
        rtc_buffer_[3] = decimal_to_bcd(now.tm_wday);
        rtc_buffer_[4] = decimal_to_bcd(hour);
        rtc_buffer_[5] = decimal_to_bcd(now.tm_min);
        rtc_buffer_[6] = decimal_to_bcd(now.tm_sec);
        trace_rtc("read-datetime", rtc_buffer_[0], rtc_buffer_[1], rtc_buffer_[2]);
        break;
    }
    case 4u:
        rtc_buffer_[0] = static_cast<u8>(rtc_control_ & 0xEAu);
        rtc_control_ = static_cast<u8>(rtc_control_ & ~0x80u);
        trace_rtc("read-control", rtc_buffer_[0], 0, 0);
        break;
    case 6u: {
        auto now = local_time_now();
        auto hour = now.tm_hour;
        if ((rtc_control_ & 0x40u) == 0u && hour >= 12) {
            hour = (hour - 12) | 0x80;
        }
        rtc_buffer_[0] = decimal_to_bcd(hour);
        rtc_buffer_[1] = decimal_to_bcd(now.tm_min);
        rtc_buffer_[2] = decimal_to_bcd(now.tm_sec);
        trace_rtc("read-time", rtc_buffer_[0], rtc_buffer_[1], rtc_buffer_[2]);
        break;
    }
    default:
        rtc_buffer_.fill(0xFFu);
        trace_rtc("read-unknown", rtc_register_, 0, 0);
        break;
    }
}

void Cartridge::rtc_write_register() {
    switch (rtc_register_) {
    case 0u:
        rtc_control_ = 0;
        rtc_buffer_.fill(0);
        break;
    case 4u:
        rtc_control_ = static_cast<u8>(rtc_buffer_[0] & 0x6Au);
        break;
    default:
        break;
    }
}

int Cartridge::rtc_register_length() const {
    return kRtcRegisterLengths[static_cast<std::size_t>(rtc_register_ & 7u)];
}

void Cartridge::trace_rtc(const char* event, u8 a, u8 b, u8 c) const {
#ifndef GBA_PLATFORM_ESP32
    static const bool enabled = std::getenv("GBA_RTC_TRACE") != nullptr;
    if (enabled) {
        std::fprintf(stderr, "RTC %-13s %02X %02X %02X ctl=%02X dir=%02X data=%02X cs=%u sck=%u sio=%u\n",
                     event,
                     a,
                     b,
                     c,
                     gpio_control_,
                     gpio_direction_,
                     gpio_data_,
                     static_cast<unsigned>(rtc_port_cs_),
                     static_cast<unsigned>(rtc_port_sck_),
                     static_cast<unsigned>(rtc_port_sio_));
    }
#else
    (void)event;
    (void)a;
    (void)b;
    (void)c;
#endif
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
