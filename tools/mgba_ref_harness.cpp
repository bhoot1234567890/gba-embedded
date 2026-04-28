#include <mgba-util/vfs.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/input.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/memory.h>

#include <fcntl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct InputEvent {
    unsigned frame = 0;
    unsigned duration = 1;
    unsigned mask = 0;
    std::string label;
};

struct Options {
    std::filesystem::path rom_path{};
    std::filesystem::path bios_path{"tests/assets/roms/gba_bios.bin"};
    std::filesystem::path output_path{"harness_out/mgba_ref_frames.csv"};
    std::filesystem::path output_dir{"harness_out/mgba_ref"};
    unsigned frames = 1500;
    unsigned log_every = 120;
    unsigned start_frame = 0;
    unsigned start_hold = 12;
    unsigned dump_frame = 0;
    bool no_bios = false;
    std::vector<InputEvent> events;
};

struct Metrics {
    unsigned frame = 0;
    int32_t frame_counter = 0;
    uint32_t pc = 0;
    uint32_t cpsr = 0;
    int halted = 0;
    uint16_t keyinput = 0;
    uint16_t dispcnt = 0;
    uint16_t dispstat = 0;
    uint16_t vcount = 0;
    uint16_t ie = 0;
    uint16_t iflags = 0;
    uint16_t ime = 0;
    uint16_t waitcnt = 0;
    uint64_t frame_hash = 0;
    uint32_t unique_colors = 0;
    uint32_t dominant_color = 0;
    double dominant_ratio = 0.0;
    double white_ratio = 0.0;
    double black_ratio = 0.0;
    unsigned input_mask = 0;
    std::string input_label;
};

void print_usage(const char* argv0) {
    std::fprintf(
        stderr,
        "Usage: %s --rom <path> [--bios <path>|--no-bios] [--frames N]\n"
        "       [--start-frame N] [--start-hold N] [--press frame:duration:buttons]\n"
        "       [--output file] [--output-dir dir] [--dump-frame N] [--log-every N]\n",
        argv0
    );
}

void quiet_log(mLogger*, int, mLogLevel, const char*, va_list) {
}

bool parse_u32(std::string_view text, unsigned& value) {
    const std::string copy{text};
    char* end = nullptr;
    const auto parsed = std::strtoul(copy.c_str(), &end, 0);
    if (end == copy.c_str() || *end != '\0' || parsed > std::numeric_limits<unsigned>::max()) {
        return false;
    }
    value = static_cast<unsigned>(parsed);
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

std::optional<unsigned> button_mask(std::string_view text) {
    const auto token = lower_copy(text);
    if (token == "a") return 1u << GBA_KEY_A;
    if (token == "b") return 1u << GBA_KEY_B;
    if (token == "select" || token == "sel") return 1u << GBA_KEY_SELECT;
    if (token == "start" || token == "st") return 1u << GBA_KEY_START;
    if (token == "right") return 1u << GBA_KEY_RIGHT;
    if (token == "left") return 1u << GBA_KEY_LEFT;
    if (token == "up") return 1u << GBA_KEY_UP;
    if (token == "down") return 1u << GBA_KEY_DOWN;
    if (token == "r") return 1u << GBA_KEY_R;
    if (token == "l") return 1u << GBA_KEY_L;
    return std::nullopt;
}

std::optional<unsigned> parse_buttons(std::string_view text) {
    unsigned mask = 0;
    for (const auto& part : split_buttons(text)) {
        const auto bit = button_mask(part);
        if (!bit.has_value()) {
            return std::nullopt;
        }
        mask |= *bit;
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
        } else if (arg == "--no-bios") {
            options.no_bios = true;
        } else if (arg == "--output") {
            const auto* value = require_value(arg);
            if (value == nullptr) return false;
            options.output_path = value;
        } else if (arg == "--output-dir") {
            const auto* value = require_value(arg);
            if (value == nullptr) return false;
            options.output_dir = value;
        } else if (arg == "--frames") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.frames)) return false;
        } else if (arg == "--dump-frame") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.dump_frame)) return false;
        } else if (arg == "--log-every") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.log_every)) return false;
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
    if (options.start_frame != 0) {
        options.events.push_back(InputEvent{options.start_frame, options.start_hold, 1u << GBA_KEY_START, "start"});
    }
    if (options.log_every == 0) {
        options.log_every = 1;
    }
    return true;
}

std::string hex_u32(uint32_t value, int width = 8) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(width) << value;
    return out.str();
}

std::string hex_u16(uint32_t value) {
    return hex_u32(value & 0xFFFFu, 4);
}

uint64_t hash_pixels(const color_t* pixels, unsigned width, unsigned height) {
    uint64_t hash = 1469598103934665603ull;
    constexpr uint64_t prime = 1099511628211ull;
    for (unsigned i = 0; i < width * height; ++i) {
        const auto pixel = static_cast<uint32_t>(pixels[i]);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<uint8_t>((pixel >> shift) & 0xFFu);
            hash *= prime;
        }
    }
    return hash;
}

void analyze_pixels(const color_t* pixels, unsigned width, unsigned height, Metrics& metrics) {
    std::array<uint32_t, 32768> histogram{};
    uint32_t white = 0;
    uint32_t black = 0;
    uint32_t unique = 0;
    uint32_t dominant = 0;
    uint32_t dominant_color = 0;

    for (unsigned i = 0; i < width * height; ++i) {
        const auto pixel = static_cast<uint32_t>(pixels[i]);
        const auto r = pixel & 0xFFu;
        const auto g = (pixel >> 8u) & 0xFFu;
        const auto b = (pixel >> 16u) & 0xFFu;
        const auto gba = static_cast<uint32_t>((r >> 3u) | ((g >> 3u) << 5u) | ((b >> 3u) << 10u));
        auto& count = histogram[gba];
        if (count == 0) {
            ++unique;
        }
        ++count;
        if (count > dominant) {
            dominant = count;
            dominant_color = gba;
        }
        if (gba == 0x7FFFu) {
            ++white;
        } else if (gba == 0u) {
            ++black;
        }
    }

    const double total = static_cast<double>(width * height);
    metrics.frame_hash = hash_pixels(pixels, width, height);
    metrics.unique_colors = unique;
    metrics.dominant_color = dominant_color;
    metrics.dominant_ratio = static_cast<double>(dominant) / total;
    metrics.white_ratio = static_cast<double>(white) / total;
    metrics.black_ratio = static_cast<double>(black) / total;
}

unsigned input_for_frame(const Options& options, unsigned frame, std::string& label_out) {
    unsigned mask = 0;
    label_out.clear();
    for (const auto& event : options.events) {
        const auto end = event.frame + event.duration;
        if (frame >= event.frame && frame < end) {
            mask |= event.mask;
            if (!label_out.empty()) {
                label_out += "+";
            }
            label_out += event.label;
        }
    }
    return mask;
}

Metrics collect_metrics(mCore* core,
                        const color_t* pixels,
                        unsigned width,
                        unsigned height,
                        unsigned frame,
                        unsigned input_mask,
                        const std::string& input_label) {
    Metrics metrics{};
    metrics.frame = frame;
    metrics.frame_counter = static_cast<int32_t>(core->frameCounter(core));
    metrics.input_mask = input_mask;
    metrics.input_label = input_label;

    const auto* cpu = static_cast<const ARMCore*>(core->cpu);
    metrics.pc = static_cast<uint32_t>(cpu->gprs[ARM_PC]);
    metrics.cpsr = static_cast<uint32_t>(cpu->cpsr.packed);
    metrics.halted = cpu->halted;
    metrics.keyinput = static_cast<uint16_t>(core->busRead16(core, 0x04000000u + REG_KEYINPUT));
    metrics.dispcnt = static_cast<uint16_t>(core->busRead16(core, 0x04000000u + REG_DISPCNT));
    metrics.dispstat = static_cast<uint16_t>(core->busRead16(core, 0x04000000u + REG_DISPSTAT));
    metrics.vcount = static_cast<uint16_t>(core->busRead16(core, 0x04000000u + REG_VCOUNT));
    metrics.ie = static_cast<uint16_t>(core->busRead16(core, 0x04000000u + REG_IE));
    metrics.iflags = static_cast<uint16_t>(core->busRead16(core, 0x04000000u + REG_IF));
    metrics.ime = static_cast<uint16_t>(core->busRead16(core, 0x04000000u + REG_IME));
    metrics.waitcnt = static_cast<uint16_t>(core->busRead16(core, 0x04000000u + REG_WAITCNT));
    analyze_pixels(pixels, width, height, metrics);
    return metrics;
}

void write_header(std::ofstream& out) {
    out << "frame,frame_counter,pc,cpsr,halted,keyinput,dispcnt,dispstat,vcount,ie,if,ime,waitcnt,"
           "frame_hash,unique_colors,dominant_color,dominant_ratio,white_ratio,black_ratio,input_mask,input_label\n";
}

void write_row(std::ofstream& out, const Metrics& m) {
    out << m.frame << ','
        << m.frame_counter << ','
        << hex_u32(m.pc) << ','
        << hex_u32(m.cpsr) << ','
        << m.halted << ','
        << hex_u16(m.keyinput) << ','
        << hex_u16(m.dispcnt) << ','
        << hex_u16(m.dispstat) << ','
        << m.vcount << ','
        << hex_u16(m.ie) << ','
        << hex_u16(m.iflags) << ','
        << m.ime << ','
        << hex_u16(m.waitcnt) << ','
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

void print_metrics(const Metrics& m) {
    std::cout << "REF frame=" << m.frame
              << " pc=" << hex_u32(m.pc)
              << " cpsr=" << hex_u32(m.cpsr)
              << " halted=" << m.halted
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

bool load_bios(mCore* core, const std::filesystem::path& bios_path) {
    VFile* bios = VFileOpen(bios_path.c_str(), O_RDONLY);
    if (bios == nullptr) {
        return false;
    }
    const bool ok = core->loadBIOS(core, bios, 0);
    bios->close(bios);
    return ok;
}

void dump_memory_block(mCore* core,
                       uint32_t base,
                       size_t wanted_size,
                       const std::filesystem::path& path) {
    size_t block_size = 0;
    auto* block = static_cast<const uint8_t*>(mCoreGetMemoryBlock(core, base, &block_size));
    if (block == nullptr || block_size == 0) {
        std::fprintf(stderr, "mGBA block unavailable at 0x%08X\n", base);
        return;
    }
    const auto size = std::min(block_size, wanted_size);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(block), static_cast<std::streamsize>(size));
}

void dump_memory(mCore* core, const std::filesystem::path& output_dir, unsigned frame) {
    std::filesystem::create_directories(output_dir);
    const auto prefix = output_dir / ("frame_" + std::to_string(frame) + "_");
    dump_memory_block(core, BASE_WORKING_RAM, SIZE_WORKING_RAM, prefix.string() + "ewram.bin");
    dump_memory_block(core, BASE_WORKING_IRAM, SIZE_WORKING_IRAM, prefix.string() + "iwram.bin");
    dump_memory_block(core, BASE_VRAM, SIZE_VRAM, prefix.string() + "vram.bin");
    dump_memory_block(core, BASE_PALETTE_RAM, SIZE_PALETTE_RAM, prefix.string() + "palette.bin");
    dump_memory_block(core, BASE_OAM, SIZE_OAM, prefix.string() + "oam.bin");
}

}  // namespace

int main(int argc, char** argv) {
    Options options{};
    if (!parse_args(argc, argv, options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    mLogger quiet_logger{};
    quiet_logger.log = quiet_log;
    mLogSetDefaultLogger(&quiet_logger);

    std::filesystem::create_directories(options.output_path.parent_path());
    std::filesystem::create_directories(options.output_dir);

    mCore* core = mCoreFind(options.rom_path.c_str());
    if (core == nullptr) {
        core = GBACoreCreate();
    }
    if (core == nullptr) {
        std::fprintf(stderr, "Failed to create mGBA core\n");
        return EXIT_FAILURE;
    }
    mCoreInitConfig(core, "gba_mgba_ref_harness");
    if (!core->init(core)) {
        std::fprintf(stderr, "Failed to initialize mGBA core\n");
        return EXIT_FAILURE;
    }
    mCoreLoadConfig(core);

    if (!options.no_bios && !load_bios(core, options.bios_path)) {
        std::fprintf(stderr, "Failed to load BIOS from %s\n", options.bios_path.c_str());
        core->deinit(core);
        return EXIT_FAILURE;
    }

    if (!mCoreLoadFile(core, options.rom_path.c_str())) {
        std::fprintf(stderr, "Failed to load ROM from %s\n", options.rom_path.c_str());
        core->deinit(core);
        return EXIT_FAILURE;
    }

    unsigned width = 0;
    unsigned height = 0;
    core->desiredVideoDimensions(core, &width, &height);
    if (width == 0 || height == 0) {
        width = 240;
        height = 160;
    }
    std::vector<color_t> pixels(width * height);
    core->setVideoBuffer(core, pixels.data(), width);
    core->reset(core);

    std::ofstream output(options.output_path);
    if (!output) {
        std::fprintf(stderr, "Failed to open output CSV: %s\n", options.output_path.c_str());
        core->deinit(core);
        return EXIT_FAILURE;
    }
    write_header(output);

    std::cout << "mGBA reference output: " << options.output_path << '\n';
    std::cout << "ROM: " << options.rom_path << '\n';
    if (!options.no_bios) {
        std::cout << "BIOS: " << options.bios_path << '\n';
    }
    for (const auto& event : options.events) {
        std::cout << "Input event: frame=" << event.frame << " duration=" << event.duration
                  << " mask=" << hex_u16(event.mask) << " label=" << event.label << '\n';
    }

    for (unsigned frame = 1; frame <= options.frames; ++frame) {
        std::string input_label;
        const auto input_mask = input_for_frame(options, frame, input_label);
        core->setKeys(core, input_mask);
        core->runFrame(core);

        const auto metrics = collect_metrics(core, pixels.data(), width, height, frame, input_mask, input_label);
        write_row(output, metrics);
        if (frame % options.log_every == 0 || input_mask != 0) {
            print_metrics(metrics);
        }
        if (options.dump_frame != 0 && frame == options.dump_frame) {
            dump_memory(core, options.output_dir, frame);
        }
    }

    output.flush();
    // libmGBA can trip platform-specific teardown paths in this embedded
    // harness; the OS reclaim is sufficient for this short-lived debug tool.
    return EXIT_SUCCESS;
}
