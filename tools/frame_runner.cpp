#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gba/core/constants.hpp"
#include "gba/core/emulator.hpp"
#include "gba/core/bus.hpp"

namespace {

using namespace gba;

struct Options {
    std::filesystem::path rom_path{};
    std::optional<std::filesystem::path> bios_path{};
    std::filesystem::path output_dir{"frame_dumps"};
    u32 frames = 600;
    u32 capture_every = 60;
    bool use_stub_bios = false;
    bool skip_bios = false;
};

void print_usage(const char* argv0) {
    std::fprintf(
        stderr,
        "Usage: %s --rom <path> [--bios <path>] [--stub-bios] [--skip-bios] [--bios-boot] [--frames <count>] [--capture-every <count>] [--output-dir <dir>]\n",
        argv0
    );
}

bool parse_u32(std::string_view text, u32& value) {
    char* end = nullptr;
    const auto parsed = std::strtoul(text.data(), &end, 0);
    if (end == text.data() || *end != '\0' || parsed > 0xFFFFFFFFul) {
        return false;
    }
    value = static_cast<u32>(parsed);
    return true;
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
            if (value == nullptr) {
                return false;
            }
            options.rom_path = value;
        } else if (arg == "--bios") {
            const auto* value = require_value(arg);
            if (value == nullptr) {
                return false;
            }
            options.bios_path = std::filesystem::path(value);
        } else if (arg == "--output-dir") {
            const auto* value = require_value(arg);
            if (value == nullptr) {
                return false;
            }
            options.output_dir = value;
        } else if (arg == "--frames") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.frames)) {
                std::fprintf(stderr, "Invalid value for --frames\n");
                return false;
            }
        } else if (arg == "--capture-every") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u32(value, options.capture_every)) {
                std::fprintf(stderr, "Invalid value for --capture-every\n");
                return false;
            }
        } else if (arg == "--stub-bios") {
            options.use_stub_bios = true;
        } else if (arg == "--skip-bios") {
            options.skip_bios = true;
        } else if (arg == "--bios-boot") {
            options.skip_bios = false;
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
    return true;
}

void write_u32(std::vector<u8>& buf, u32 offset, u32 value) {
    buf[offset] = static_cast<u8>(value);
    buf[offset + 1] = static_cast<u8>(value >> 8);
    buf[offset + 2] = static_cast<u8>(value >> 16);
    buf[offset + 3] = static_cast<u8>(value >> 24);
}

std::vector<u8> make_bios_stub() {
    std::vector<u8> bios(kBiosSize, 0xFF);
    const auto put = [&](u32 addr, u32 instr) { write_u32(bios, addr, instr); };

    put(0x00, 0xEA00001E);
    put(0x04, 0xEAFFFFFE);
    put(0x08, 0xEAFFFFFE);
    put(0x0C, 0xEAFFFFFE);
    put(0x10, 0xEAFFFFFE);
    put(0x14, 0xEAFFFFFE);
    put(0x18, 0xEA000008);
    put(0x1C, 0xEAFFFFFE);

    put(0x40, 0xE92D500F);
    put(0x44, 0xE59FC01C);
    put(0x48, 0xE59CC000);
    put(0x4C, 0xE35C0000);
    put(0x50, 0x0A000002);
    put(0x54, 0xE1A0E00F);
    put(0x58, 0xE12FFF1C);
    put(0x5C, 0xE89D500F);
    put(0x60, 0xE25EF004);
    put(0x64, 0xEAFFFFFE);
    put(0x68, 0x03007FFC);

    put(0x80, 0xE321F0D2);
    put(0x84, 0xE59FD044);
    put(0x88, 0xE321F0D3);
    put(0x8C, 0xE59FD034);
    put(0x90, 0xE321F0DF);
    put(0x94, 0xE59FD024);
    put(0x98, 0xE3A00000);
    put(0x9C, 0xE59F101C);
    put(0xA0, 0xE5810000);
    put(0xA4, 0xE59FF010);

    put(0xBC, 0x08000000);
    put(0xC0, 0x03007F00);
    put(0xC4, 0x03007FFC);
    put(0xC8, 0x03007FE0);
    put(0xD0, 0x03007FA0);

    return bios;
}

std::string sanitize_stem(const std::filesystem::path& path) {
    std::string stem = path.stem().string();
    for (char& ch : stem) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) {
            ch = '_';
        }
    }
    if (stem.empty()) {
        stem = "rom";
    }
    return stem;
}

void write_ppm(const std::filesystem::path& path, std::span<const u16> framebuffer) {
    std::ofstream output(path, std::ios::binary);
    output << "P6\n240 160\n255\n";
    for (const auto pixel : framebuffer) {
        const auto r5 = static_cast<u8>(pixel & 0x1Fu);
        const auto g5 = static_cast<u8>((pixel >> 5) & 0x1Fu);
        const auto b5 = static_cast<u8>((pixel >> 10) & 0x1Fu);
        const auto r8 = static_cast<char>((r5 << 3) | (r5 >> 2));
        const auto g8 = static_cast<char>((g5 << 3) | (g5 >> 2));
        const auto b8 = static_cast<char>((b5 << 3) | (b5 >> 2));
        output.put(r8);
        output.put(g8);
        output.put(b8);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options{};
    if (!parse_args(argc, argv, options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    Emulator emulator;
    bool loaded_real_bios = false;
    if (!options.use_stub_bios && options.bios_path.has_value()) {
        loaded_real_bios = emulator.load_bios_from_file(*options.bios_path);
        if (!loaded_real_bios) {
            std::fprintf(
                stderr,
                "WARNING: Failed to load BIOS from %s, falling back to stub BIOS\n",
                options.bios_path->c_str()
            );
        }
    }
    if (!loaded_real_bios) {
        emulator.load_bios(make_bios_stub());
        options.skip_bios = true;
    }

    if (!emulator.load_rom_from_file(options.rom_path)) {
        std::fprintf(stderr, "Failed to load ROM from %s\n", options.rom_path.c_str());
        return EXIT_FAILURE;
    }

    emulator.set_save_type(SaveType::Flash128K);
    emulator.reset(options.skip_bios);
    emulator.cpu().pc_trace_enabled_ = true;
    std::filesystem::create_directories(options.output_dir);

    const auto rom_prefix = sanitize_stem(options.rom_path);
    const auto capture_every = options.capture_every == 0 ? options.frames : options.capture_every;

    std::printf("ROM:  %s\n", options.rom_path.c_str());
    if (loaded_real_bios) {
        std::printf("BIOS: %s\n", options.bios_path->c_str());
    } else {
        std::printf("BIOS: stub\n");
    }
    std::printf("RESET: %s\n", options.skip_bios ? "skip BIOS" : "BIOS boot");

    for (u32 frame = 1; frame <= options.frames; ++frame) {
        emulator.run_frame();

        if (emulator.cpu().state().regs[15] == 0x00000004u) {
            std::printf("CRASHED AT PC=0x04\nPC Trace:\n");
            for (u32 i = 0; i < emulator.cpu().kPcTraceSize; ++i) {
                std::printf("  [%2u] 0x%08X\n", i, emulator.cpu().pc_trace_[(emulator.cpu().pc_trace_pos_ + i) % emulator.cpu().kPcTraceSize]);
            }
            return EXIT_FAILURE;
        }

        if (frame % capture_every != 0 && frame != options.frames) {
            continue;
        }

        const auto output_path =
            options.output_dir / (rom_prefix + "_f" + std::to_string(frame) + ".ppm");
        write_ppm(output_path, emulator.framebuffer());

        const auto& cpu = emulator.cpu();
        const auto& state = cpu.state();
        const auto& irq = emulator.irq();
        const auto rom_stats = emulator.last_rom_stats();
        std::printf(
            "frame=%u file=%s pc=0x%08X cpsr=0x%08X halted=%d IE=0x%04X IF=0x%04X DISPSTAT=0x%04X IME=%d cycle=%llu rom_reads=%llu rom_misses=%llu rom_hits=%llu rom_unique_pages=%u rom_prefetches=%llu\n",
            frame,
            output_path.c_str(),
            state.regs[15],
            state.cpsr,
            state.halted ? 1 : 0,
            irq.ie(),
            irq.iflags(),
            emulator.ppu().dispstat(),
            irq.ime(),
            static_cast<unsigned long long>(cpu.current_cycle()),
            static_cast<unsigned long long>(rom_stats.byte_reads),
            static_cast<unsigned long long>(rom_stats.cache_misses),
            static_cast<unsigned long long>(rom_stats.cache_hits),
            rom_stats.unique_pages,
            static_cast<unsigned long long>(rom_stats.prefetches)
        );
    }

    return EXIT_SUCCESS;
}
