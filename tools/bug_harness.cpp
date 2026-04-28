#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#define private public
#include "gba/core/emulator.hpp"
#undef private

namespace {

using namespace gba;

struct InputEvent {
    u32 frame = 0;
    u32 duration = 1;
    u16 mask = 0;
    std::string label;
};

struct Options {
    std::filesystem::path rom_path{};
    std::filesystem::path bios_path{"tests/assets/roms/gba_bios.bin"};
    std::filesystem::path output_dir{"harness_out"};
    std::optional<std::filesystem::path> save_path{};
    std::string save_type{"auto"};
    bool no_save = false;
    u32 frames = 1500;
    u32 log_every = 1;
    u32 capture_every = 0;
    u32 start_frame = 0;
    u32 start_hold = 12;
    u32 blank_after = 0;
    u32 stop_after_blank = 20;
    u32 stop_after_no_cycle = 5;
    u32 stop_after_same_state = 600;
    u32 event_trace_start = 0;
    u32 event_trace_frames = 0;
    u32 max_events_per_traced_frame = 20000;
    double blank_ratio = 0.985;
    bool stop_on_blank = true;
    bool stop_on_stall = true;
    bool dump_memory_on_stop = true;
    std::vector<InputEvent> events;
};

struct Metrics {
    u32 frame = 0;
    u64 cycle = 0;
    u32 pc = 0;
    u32 cpsr = 0;
    bool halted = false;
    bool bus_halted = false;
    u16 keyinput = 0;
    u16 dispcnt = 0;
    u16 dispstat = 0;
    u16 vcount = 0;
    u16 ie = 0;
    u16 iflags = 0;
    u16 ime = 0;
    u16 waitcnt = 0;
    u64 next = 0;
    u64 ppu_next = 0;
    u64 timers_next = 0;
    u64 dma_next = 0;
    u64 apu_next = 0;
    u64 serial_next = 0;
    u64 irq_next = 0;
    u64 frame_hash = 0;
    u16 dominant_color = 0;
    u32 dominant_count = 0;
    u32 unique_colors = 0;
    double dominant_ratio = 0.0;
    double white_ratio = 0.0;
    double black_ratio = 0.0;
    u16 input_mask = 0;
    std::string input_label;
};

void print_usage(const char* argv0) {
    std::fprintf(
        stderr,
        "Usage: %s --rom <path> [--bios <path>] [--frames N] [--start-frame N] [--start-hold N]\n"
        "       [--press frame:duration:buttons] [--output-dir dir] [--capture-every N]\n"
        "       [--log-every N]\n"
        "       [--save path|--no-save] [--save-type auto|flash128|flash64|sram|eeprom|none]\n"
        "       [--blank-after N] [--blank-ratio 0.985] [--stop-after-blank N]\n"
        "       [--event-trace-start N] [--event-trace-frames N]\n",
        argv0
    );
}

bool parse_u32(std::string_view text, u32& value) {
    const std::string copy{text};
    char* end = nullptr;
    const auto parsed = std::strtoul(copy.c_str(), &end, 0);
    if (end == copy.c_str() || *end != '\0' || parsed > std::numeric_limits<u32>::max()) {
        return false;
    }
    value = static_cast<u32>(parsed);
    return true;
}

bool parse_double(std::string_view text, double& value) {
    const std::string copy{text};
    char* end = nullptr;
    const auto parsed = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0') {
        return false;
    }
    value = parsed;
    return true;
}

std::string lower_copy(std::string_view text) {
    std::string out{text};
    for (auto& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::vector<std::string> split_buttons(std::string_view text) {
    std::vector<std::string> parts;
    std::string current;
    for (const auto ch : text) {
        if (ch == '+' || ch == ',' || ch == '|') {
            if (!current.empty()) {
                parts.push_back(lower_copy(current));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        parts.push_back(lower_copy(current));
    }
    return parts;
}

std::optional<u16> button_mask(std::string_view text) {
    const auto token = lower_copy(text);
    if (token == "a") {
        return kKeyA;
    }
    if (token == "b") {
        return kKeyB;
    }
    if (token == "select" || token == "sel") {
        return kKeySelect;
    }
    if (token == "start" || token == "st") {
        return kKeyStart;
    }
    if (token == "right") {
        return kKeyRight;
    }
    if (token == "left") {
        return kKeyLeft;
    }
    if (token == "up") {
        return kKeyUp;
    }
    if (token == "down") {
        return kKeyDown;
    }
    if (token == "r") {
        return kKeyR;
    }
    if (token == "l") {
        return kKeyL;
    }
    return std::nullopt;
}

std::optional<u16> parse_buttons(std::string_view text) {
    u16 mask = 0;
    for (const auto& part : split_buttons(text)) {
        const auto bit = button_mask(part);
        if (!bit.has_value()) {
            return std::nullopt;
        }
        mask = static_cast<u16>(mask | *bit);
    }
    return mask;
}

std::optional<InputEvent> parse_press(std::string_view text) {
    const auto first = text.find(':');
    const auto second = first == std::string_view::npos ? std::string_view::npos : text.find(':', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos) {
        return std::nullopt;
    }

    InputEvent event{};
    if (!parse_u32(text.substr(0, first), event.frame) ||
        !parse_u32(text.substr(first + 1, second - first - 1), event.duration)) {
        return std::nullopt;
    }

    const auto buttons = text.substr(second + 1);
    const auto mask = parse_buttons(buttons);
    if (!mask.has_value()) {
        return std::nullopt;
    }

    event.mask = *mask;
    event.label = std::string(buttons);
    return event;
}

bool parse_args(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg{argv[index]};
        const auto require_value = [&](std::string_view name) -> const char* {
            if (index + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %.*s\n", static_cast<int>(name.size()), name.data());
                return nullptr;
            }
            ++index;
            return argv[index];
        };

        if (arg == "--rom") {
            const auto* value = require_value(arg);
            if (value == nullptr) return false;
            options.rom_path = value;
        } else if (arg == "--bios") {
            const auto* value = require_value(arg);
            if (value == nullptr) return false;
            options.bios_path = value;
        } else if (arg == "--output-dir") {
            const auto* value = require_value(arg);
            if (value == nullptr) return false;
            options.output_dir = value;
        } else if (arg == "--save") {
            const auto* value = require_value(arg);
            if (value == nullptr) return false;
            options.save_path = std::filesystem::path(value);
        } else if (arg == "--no-save") {
            options.no_save = true;
        } else if (arg == "--save-type") {
            const auto* value = require_value(arg);
            if (value == nullptr) return false;
            options.save_type = value;
        } else if (arg == "--frames") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.frames)) return false;
        } else if (arg == "--log-every") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.log_every)) return false;
        } else if (arg == "--capture-every") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.capture_every)) return false;
        } else if (arg == "--start-frame") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.start_frame)) return false;
        } else if (arg == "--start-hold") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.start_hold)) return false;
        } else if (arg == "--press") {
            const auto* value = require_value(arg);
            if (value == nullptr) return false;
            auto event = parse_press(value);
            if (!event.has_value()) {
                std::fprintf(stderr, "Invalid --press format, expected frame:duration:buttons\n");
                return false;
            }
            options.events.push_back(std::move(*event));
        } else if (arg == "--blank-after") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.blank_after)) return false;
        } else if (arg == "--blank-ratio") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_double(value, options.blank_ratio)) return false;
        } else if (arg == "--stop-after-blank") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.stop_after_blank)) return false;
        } else if (arg == "--stop-after-no-cycle") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.stop_after_no_cycle)) return false;
        } else if (arg == "--stop-after-same-state") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.stop_after_same_state)) return false;
        } else if (arg == "--event-trace-start") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.event_trace_start)) return false;
        } else if (arg == "--event-trace-frames") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.event_trace_frames)) return false;
        } else if (arg == "--max-events-per-traced-frame") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.max_events_per_traced_frame)) return false;
        } else if (arg == "--no-stop-on-blank") {
            options.stop_on_blank = false;
        } else if (arg == "--no-stop-on-stall") {
            options.stop_on_stall = false;
        } else if (arg == "--no-memory-dump") {
            options.dump_memory_on_stop = false;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[index]);
            return false;
        }
    }

    if (options.rom_path.empty()) {
        std::fprintf(stderr, "--rom is required\n");
        return false;
    }
    if (options.frames == 0) {
        std::fprintf(stderr, "--frames must be greater than 0\n");
        return false;
    }
    if (options.log_every == 0) {
        options.log_every = 1;
    }
    if (options.start_frame != 0) {
        options.events.push_back(InputEvent{options.start_frame, options.start_hold, kKeyStart, "start"});
        if (options.blank_after == 0) {
            options.blank_after = options.start_frame;
        }
    }
    options.blank_ratio = std::clamp(options.blank_ratio, 0.0, 1.0);
    return true;
}

std::optional<SaveType> parse_save_type(const std::string& text) {
    if (text == "none") return SaveType::None;
    if (text == "sram") return SaveType::Sram;
    if (text == "flash64") return SaveType::Flash64K;
    if (text == "flash128") return SaveType::Flash128K;
    if (text == "eeprom") return SaveType::Eeprom;
    return std::nullopt;
}

const char* save_type_name(SaveType save_type) {
    switch (save_type) {
    case SaveType::None: return "none";
    case SaveType::Sram: return "sram";
    case SaveType::Flash64K: return "flash64";
    case SaveType::Flash128K: return "flash128";
    case SaveType::Eeprom: return "eeprom";
    }
    return "unknown";
}

std::vector<u8> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::filesystem::path default_save_path(const std::filesystem::path& rom_path) {
    auto path = rom_path;
    path.replace_extension(".sav");
    return path;
}

u64 hash_framebuffer(std::span<const u16> framebuffer) {
    u64 hash = 1469598103934665603ull;
    constexpr u64 prime = 1099511628211ull;
    for (const auto pixel : framebuffer) {
        hash ^= static_cast<u8>(pixel & 0xFFu);
        hash *= prime;
        hash ^= static_cast<u8>((pixel >> 8u) & 0xFFu);
        hash *= prime;
    }
    return hash;
}

void analyze_framebuffer(std::span<const u16> framebuffer, Metrics& metrics) {
    std::array<u32, 32768> histogram{};
    u32 white = 0;
    u32 black = 0;
    u32 unique = 0;
    u32 dominant = 0;
    u16 dominant_color = 0;

    for (const auto raw_pixel : framebuffer) {
        const auto pixel = static_cast<u16>(raw_pixel & 0x7FFFu);
        auto& count = histogram[pixel];
        if (count == 0) {
            ++unique;
        }
        ++count;
        if (count > dominant) {
            dominant = count;
            dominant_color = pixel;
        }
        if (pixel == 0x7FFFu) {
            ++white;
        } else if (pixel == 0u) {
            ++black;
        }
    }

    constexpr double total = static_cast<double>(kFramebufferPixels);
    metrics.frame_hash = hash_framebuffer(framebuffer);
    metrics.unique_colors = unique;
    metrics.dominant_count = dominant;
    metrics.dominant_color = dominant_color;
    metrics.dominant_ratio = static_cast<double>(dominant) / total;
    metrics.white_ratio = static_cast<double>(white) / total;
    metrics.black_ratio = static_cast<double>(black) / total;
}

Metrics collect_metrics(Emulator& emulator, u32 frame, u16 input_mask, const std::string& input_label) {
    emulator.refresh_schedule();

    Metrics metrics{};
    metrics.frame = frame;
    metrics.input_mask = input_mask;
    metrics.input_label = input_label;

    auto& cpu = emulator.cpu();
    const auto& state = cpu.state();
    metrics.cycle = cpu.current_cycle();
    metrics.pc = state.regs[15];
    metrics.cpsr = state.cpsr;
    metrics.halted = state.halted;
    metrics.bus_halted = emulator.bus_.halted();
    metrics.keyinput = emulator.bus_.keyinput();
    metrics.dispcnt = emulator.ppu_.dispcnt();
    metrics.dispstat = emulator.ppu_.dispstat();
    metrics.vcount = emulator.ppu_.vcount();
    metrics.ie = emulator.irq_.ie();
    metrics.iflags = emulator.irq_.iflags();
    metrics.ime = emulator.irq_.ime();
    metrics.waitcnt = emulator.bus_.waitcnt();
    metrics.next = emulator.scheduler_.next_event();
    metrics.ppu_next = emulator.ppu_.next_event_cycle();
    metrics.timers_next = emulator.timers_.next_event_cycle();
    metrics.dma_next = emulator.dma_.next_event_cycle();
    metrics.apu_next = emulator.apu_.next_event_cycle();
    metrics.serial_next = emulator.bus_.next_event_cycle();
    metrics.irq_next = emulator.irq_.next_event_cycle();
    analyze_framebuffer(emulator.framebuffer(), metrics);
    return metrics;
}

std::string hex_u32(u32 value, int width = 8) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(width) << value;
    return out.str();
}

std::string hex_u16(u16 value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(4) << value;
    return out.str();
}

std::string hex_u64(u64 value) {
    if (value == std::numeric_limits<u64>::max()) {
        return "max";
    }
    std::ostringstream out;
    out << value;
    return out.str();
}

void write_csv_header(std::ofstream& out) {
    out << "frame,cycle,pc,cpsr,halted,bus_halted,keyinput,dispcnt,dispstat,vcount,ie,if,ime,waitcnt,"
           "next,ppu_next,timers_next,dma_next,apu_next,serial_next,irq_next,"
           "frame_hash,unique_colors,dominant_color,dominant_ratio,white_ratio,black_ratio,input_mask,input_label\n";
}

void write_event_csv_header(std::ofstream& out) {
    out << "frame,event,cycle,pc,cpsr,halted,bus_halted,keyinput,dispcnt,dispstat,vcount,ie,if,ime,waitcnt,"
           "next,ppu_next,timers_next,dma_next,apu_next,serial_next,irq_next,"
           "frame_hash,unique_colors,dominant_color,dominant_ratio,white_ratio,black_ratio,input_mask,input_label\n";
}

void write_csv_row(std::ofstream& out, const Metrics& m) {
    out << m.frame << ','
        << m.cycle << ','
        << hex_u32(m.pc) << ','
        << hex_u32(m.cpsr) << ','
        << (m.halted ? 1 : 0) << ','
        << (m.bus_halted ? 1 : 0) << ','
        << hex_u16(m.keyinput) << ','
        << hex_u16(m.dispcnt) << ','
        << hex_u16(m.dispstat) << ','
        << m.vcount << ','
        << hex_u16(m.ie) << ','
        << hex_u16(m.iflags) << ','
        << m.ime << ','
        << hex_u16(m.waitcnt) << ','
        << hex_u64(m.next) << ','
        << hex_u64(m.ppu_next) << ','
        << hex_u64(m.timers_next) << ','
        << hex_u64(m.dma_next) << ','
        << hex_u64(m.apu_next) << ','
        << hex_u64(m.serial_next) << ','
        << hex_u64(m.irq_next) << ','
        << m.frame_hash << ','
        << m.unique_colors << ','
        << hex_u16(m.dominant_color) << ','
        << std::fixed << std::setprecision(6) << m.dominant_ratio << ','
        << std::fixed << std::setprecision(6) << m.white_ratio << ','
        << std::fixed << std::setprecision(6) << m.black_ratio << ','
        << hex_u16(m.input_mask) << ','
        << m.input_label
        << '\n';
}

void write_event_csv_row(std::ofstream& out, u32 event_index, const Metrics& m) {
    out << m.frame << ','
        << event_index << ','
        << m.cycle << ','
        << hex_u32(m.pc) << ','
        << hex_u32(m.cpsr) << ','
        << (m.halted ? 1 : 0) << ','
        << (m.bus_halted ? 1 : 0) << ','
        << hex_u16(m.keyinput) << ','
        << hex_u16(m.dispcnt) << ','
        << hex_u16(m.dispstat) << ','
        << m.vcount << ','
        << hex_u16(m.ie) << ','
        << hex_u16(m.iflags) << ','
        << m.ime << ','
        << hex_u16(m.waitcnt) << ','
        << hex_u64(m.next) << ','
        << hex_u64(m.ppu_next) << ','
        << hex_u64(m.timers_next) << ','
        << hex_u64(m.dma_next) << ','
        << hex_u64(m.apu_next) << ','
        << hex_u64(m.serial_next) << ','
        << hex_u64(m.irq_next) << ','
        << m.frame_hash << ','
        << m.unique_colors << ','
        << hex_u16(m.dominant_color) << ','
        << std::fixed << std::setprecision(6) << m.dominant_ratio << ','
        << std::fixed << std::setprecision(6) << m.white_ratio << ','
        << std::fixed << std::setprecision(6) << m.black_ratio << ','
        << hex_u16(m.input_mask) << ','
        << m.input_label
        << '\n';
}

void print_metrics(const Metrics& m, const char* prefix) {
    std::cout << prefix
              << " frame=" << m.frame
              << " cycle=" << m.cycle
              << " pc=" << hex_u32(m.pc)
              << " cpsr=" << hex_u32(m.cpsr)
              << " halted=" << (m.halted ? 1 : 0)
              << " IE=" << hex_u16(m.ie)
              << " IF=" << hex_u16(m.iflags)
              << " IME=" << m.ime
              << " DISPCNT=" << hex_u16(m.dispcnt)
              << " DISPSTAT=" << hex_u16(m.dispstat)
              << " VCOUNT=" << m.vcount
              << " hash=" << m.frame_hash
              << " colors=" << m.unique_colors
              << " dom=" << hex_u16(m.dominant_color)
              << " dom%=" << std::fixed << std::setprecision(3) << (m.dominant_ratio * 100.0)
              << " white%=" << std::fixed << std::setprecision(3) << (m.white_ratio * 100.0)
              << " input=" << hex_u16(m.input_mask)
              << (m.input_label.empty() ? "" : " ")
              << m.input_label
              << '\n';
}

void write_ppm(const std::filesystem::path& path, std::span<const u16> framebuffer) {
    std::ofstream output(path, std::ios::binary);
    output << "P6\n240 160\n255\n";
    for (const auto pixel : framebuffer) {
        const auto r5 = static_cast<u8>(pixel & 0x1Fu);
        const auto g5 = static_cast<u8>((pixel >> 5u) & 0x1Fu);
        const auto b5 = static_cast<u8>((pixel >> 10u) & 0x1Fu);
        output.put(static_cast<char>((r5 << 3u) | (r5 >> 2u)));
        output.put(static_cast<char>((g5 << 3u) | (g5 >> 2u)));
        output.put(static_cast<char>((b5 << 3u) | (b5 >> 2u)));
    }
}

void write_blob(const std::filesystem::path& path, std::span<const u8> data) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void dump_memory(Emulator& emulator, const std::filesystem::path& output_dir) {
    write_blob(output_dir / "ewram_stop.bin", emulator.bus().ewram());
    write_blob(output_dir / "iwram_stop.bin", emulator.bus().iwram());
    write_blob(output_dir / "vram_stop.bin", emulator.bus().vram());
    write_blob(output_dir / "palette_stop.bin", emulator.bus().palette());
    write_blob(output_dir / "oam_stop.bin", emulator.bus().oam());
}

void write_report(const std::filesystem::path& path,
                  const std::string& reason,
                  const std::vector<Metrics>& recent,
                  Emulator& emulator,
                  const Options& options) {
    std::ofstream out(path);
    out << "reason: " << reason << '\n';
    out << "rom: " << options.rom_path << '\n';
    out << "bios: " << options.bios_path << '\n';
    out << "frames requested: " << options.frames << '\n';
    out << "events:\n";
    for (const auto& event : options.events) {
        out << "  frame=" << event.frame << " duration=" << event.duration
            << " mask=" << hex_u16(event.mask) << " label=" << event.label << '\n';
    }
    out << "\nrecent frames:\n";
    for (const auto& m : recent) {
        out << "  frame=" << m.frame
            << " cycle=" << m.cycle
            << " pc=" << hex_u32(m.pc)
            << " cpsr=" << hex_u32(m.cpsr)
            << " halted=" << (m.halted ? 1 : 0)
            << " dispcnt=" << hex_u16(m.dispcnt)
            << " dispstat=" << hex_u16(m.dispstat)
            << " ie=" << hex_u16(m.ie)
            << " if=" << hex_u16(m.iflags)
            << " hash=" << m.frame_hash
            << " colors=" << m.unique_colors
            << " dominant=" << hex_u16(m.dominant_color)
            << " dominant_ratio=" << std::fixed << std::setprecision(6) << m.dominant_ratio
            << " white_ratio=" << std::fixed << std::setprecision(6) << m.white_ratio
            << " black_ratio=" << std::fixed << std::setprecision(6) << m.black_ratio
            << " next=" << hex_u64(m.next)
            << " ppu=" << hex_u64(m.ppu_next)
            << " timers=" << hex_u64(m.timers_next)
            << " dma=" << hex_u64(m.dma_next)
            << " apu=" << hex_u64(m.apu_next)
            << " serial=" << hex_u64(m.serial_next)
            << " irq=" << hex_u64(m.irq_next)
            << " input=" << hex_u16(m.input_mask)
            << " " << m.input_label
            << '\n';
    }

    out << "\npc trace ring:\n";
    auto& cpu = emulator.cpu();
    for (u32 i = 0; i < cpu.kPcTraceSize; ++i) {
        const auto index = (cpu.pc_trace_pos_ + i) % cpu.kPcTraceSize;
        out << "  [" << std::setw(2) << i << "] " << hex_u32(cpu.pc_trace_[index]) << '\n';
    }
}

u16 input_for_frame(const Options& options, u32 frame, std::string& label_out) {
    u16 mask = 0;
    label_out.clear();
    for (const auto& event : options.events) {
        const auto end = event.frame + event.duration;
        if (frame >= event.frame && frame < end) {
            mask = static_cast<u16>(mask | event.mask);
            if (!label_out.empty()) {
                label_out += "+";
            }
            label_out += event.label;
        }
    }
    return mask;
}

bool should_event_trace(const Options& options, u32 frame) {
    if (options.event_trace_start == 0 || options.event_trace_frames == 0) {
        return false;
    }
    const auto end = options.event_trace_start + options.event_trace_frames;
    return frame >= options.event_trace_start && frame < end;
}

bool run_frame_with_event_trace(Emulator& emulator,
                                const Options& options,
                                u32 frame,
                                u16 input_mask,
                                const std::string& input_label,
                                std::ofstream& event_csv) {
    u32 events = 0;
    while (!emulator.ppu_.frame_ready()) {
        emulator.step_scheduler_event();
        ++events;
        const auto metrics = collect_metrics(emulator, frame, input_mask, input_label);
        write_event_csv_row(event_csv, events, metrics);
        if (events >= options.max_events_per_traced_frame) {
            std::fprintf(stderr, "Event trace exceeded %u events on frame %u\n",
                         options.max_events_per_traced_frame, frame);
            return false;
        }
    }
    emulator.ppu_.consume_frame_ready();
    return true;
}

bool load_optional_save(Emulator& emulator, const Options& options) {
    if (options.no_save) {
        return true;
    }
    const auto save_path = options.save_path.value_or(default_save_path(options.rom_path));
    if (!std::filesystem::exists(save_path)) {
        return true;
    }
    auto save = read_binary_file(save_path);
    if (save.empty()) {
        std::fprintf(stderr, "Failed to read save: %s\n", save_path.c_str());
        return false;
    }
    emulator.cartridge().load_save(std::move(save));
    std::cout << "Loaded save: " << save_path << '\n';
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options{};
    if (!parse_args(argc, argv, options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::filesystem::create_directories(options.output_dir);

    Emulator emulator;
    if (!emulator.load_bios_from_file(options.bios_path)) {
        std::fprintf(stderr, "Failed to load BIOS from %s\n", options.bios_path.c_str());
        return EXIT_FAILURE;
    }
    if (!emulator.load_rom_from_file(options.rom_path)) {
        std::fprintf(stderr, "Failed to load ROM from %s\n", options.rom_path.c_str());
        return EXIT_FAILURE;
    }

    if (options.save_type == "auto") {
        emulator.cartridge().auto_detect_save_type();
    } else if (const auto save_type = parse_save_type(options.save_type)) {
        emulator.set_save_type(*save_type);
    } else {
        std::fprintf(stderr, "Unknown save type: %s\n", options.save_type.c_str());
        return EXIT_FAILURE;
    }
    if (!load_optional_save(emulator, options)) {
        return EXIT_FAILURE;
    }

    emulator.reset();
    emulator.cpu().pc_trace_enabled_ = true;

    const auto csv_path = options.output_dir / "frames.csv";
    std::ofstream csv(csv_path);
    write_csv_header(csv);
    const auto event_csv_path = options.output_dir / "events.csv";
    std::ofstream event_csv;
    if (options.event_trace_start != 0 && options.event_trace_frames != 0) {
        event_csv.open(event_csv_path);
        write_event_csv_header(event_csv);
    }

    std::cout << "Harness output: " << options.output_dir << '\n';
    std::cout << "ROM: " << options.rom_path << '\n';
    std::cout << "BIOS: " << options.bios_path << '\n';
    std::cout << "Save type: " << save_type_name(emulator.cartridge().save_type()) << '\n';
    for (const auto& event : options.events) {
        std::cout << "Input event: frame=" << event.frame << " duration=" << event.duration
                  << " mask=" << hex_u16(event.mask) << " label=" << event.label << '\n';
    }

    std::vector<Metrics> recent;
    recent.reserve(64);
    std::optional<Metrics> previous;
    std::string stop_reason;
    u32 blank_run = 0;
    u32 no_cycle_run = 0;
    u32 same_state_run = 0;

    for (u32 frame = 1; frame <= options.frames; ++frame) {
        std::string input_label;
        const auto input_mask = input_for_frame(options, frame, input_label);
        emulator.set_keys(input_mask);
        const auto before_cycle = emulator.cpu().current_cycle();
        if (should_event_trace(options, frame)) {
            if (!run_frame_with_event_trace(emulator, options, frame, input_mask, input_label, event_csv)) {
                stop_reason = "event trace exceeded max events";
            }
        } else {
            emulator.run_frame();
        }

        auto metrics = collect_metrics(emulator, frame, input_mask, input_label);
        write_csv_row(csv, metrics);

        if (frame % options.log_every == 0 || input_mask != 0) {
            print_metrics(metrics, "TRACE");
        }

        if (options.capture_every != 0 && frame % options.capture_every == 0) {
            write_ppm(options.output_dir / ("frame_" + std::to_string(frame) + ".ppm"), emulator.framebuffer());
        }

        recent.push_back(metrics);
        if (recent.size() > 64) {
            recent.erase(recent.begin());
        }

        const auto blank_like = metrics.frame >= options.blank_after &&
            (metrics.white_ratio >= options.blank_ratio ||
             (metrics.dominant_ratio >= options.blank_ratio && metrics.unique_colors <= 4u));
        blank_run = blank_like ? blank_run + 1u : 0u;

        if (metrics.cycle <= before_cycle || (previous.has_value() && metrics.cycle <= previous->cycle)) {
            ++no_cycle_run;
        } else {
            no_cycle_run = 0;
        }

        if (previous.has_value() &&
            metrics.frame_hash == previous->frame_hash &&
            metrics.pc == previous->pc &&
            metrics.cpsr == previous->cpsr &&
            metrics.dispcnt == previous->dispcnt &&
            metrics.dispstat == previous->dispstat &&
            metrics.ie == previous->ie &&
            metrics.iflags == previous->iflags &&
            metrics.ime == previous->ime) {
            ++same_state_run;
        } else {
            same_state_run = 0;
        }

        if (stop_reason.empty() && options.stop_on_blank && options.stop_after_blank != 0 &&
            blank_run >= options.stop_after_blank) {
            stop_reason = "blank-like framebuffer for " + std::to_string(blank_run) + " frames";
        } else if (stop_reason.empty() && options.stop_on_stall && options.stop_after_no_cycle != 0 &&
                   no_cycle_run >= options.stop_after_no_cycle) {
            stop_reason = "no cycle progress for " + std::to_string(no_cycle_run) + " frames";
        } else if (stop_reason.empty() && options.stop_on_stall && options.stop_after_same_state != 0 &&
                   same_state_run >= options.stop_after_same_state) {
            stop_reason = "same pc/register/framebuffer state for " + std::to_string(same_state_run) + " frames";
        }

        previous = metrics;
        if (!stop_reason.empty()) {
            print_metrics(metrics, "STOP");
            write_ppm(options.output_dir / ("stop_frame_" + std::to_string(frame) + ".ppm"), emulator.framebuffer());
            if (options.dump_memory_on_stop) {
                dump_memory(emulator, options.output_dir);
            }
            break;
        }
    }

    if (stop_reason.empty()) {
        stop_reason = "completed requested frame count";
    }

    write_report(options.output_dir / "summary.txt", stop_reason, recent, emulator, options);
    std::cout << "Stop reason: " << stop_reason << '\n';
    std::cout << "CSV: " << csv_path << '\n';
    if (event_csv.is_open()) {
        std::cout << "Event CSV: " << event_csv_path << '\n';
    }
    std::cout << "Summary: " << (options.output_dir / "summary.txt") << '\n';
    return EXIT_SUCCESS;
}
