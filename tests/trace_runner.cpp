#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "gba/core/constants.hpp"
#include "gba/core/emulator.hpp"

namespace {

using namespace gba;

struct Options {
    std::filesystem::path rom_path{};
    std::optional<std::filesystem::path> bios_path{};
    std::filesystem::path output_path{"trace.csv"};
    u64 steps = 5000;
    u32 start_pc = 0x08000000u;
    bool start_thumb = false;
};

void print_usage(const char* argv0) {
    std::cout << "Usage: " << argv0
              << " --rom <path> [--bios <path>] [--steps <count>] [--output <file>] [--start-pc <hex>] [--thumb]\n";
}

bool parse_u64(std::string_view text, u64& out) {
    errno = 0;
    const std::string owned(text);
    char* end = nullptr;
    const auto parsed = std::strtoull(owned.c_str(), &end, 0);
    if (errno != 0 || end == owned.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<u64>(parsed);
    return true;
}

bool parse_u32(std::string_view text, u32& out) {
    u64 parsed = 0;
    if (!parse_u64(text, parsed) || parsed > 0xFFFFFFFFull) {
        return false;
    }
    out = static_cast<u32>(parsed);
    return true;
}

bool parse_args(int argc, char** argv, Options& options, bool& show_help) {
    show_help = false;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        const auto require_value = [&](std::string_view name) -> const char* {
            if (index + 1 >= argc) {
                std::cerr << "Missing value for " << name << '\n';
                return nullptr;
            }
            ++index;
            return argv[index];
        };

        if (arg == "--rom") {
            const auto value = require_value("--rom");
            if (value == nullptr) {
                return false;
            }
            options.rom_path = value;
        } else if (arg == "--bios") {
            const auto value = require_value("--bios");
            if (value == nullptr) {
                return false;
            }
            options.bios_path = std::filesystem::path(value);
        } else if (arg == "--steps") {
            const auto value = require_value("--steps");
            if (value == nullptr || !parse_u64(value, options.steps)) {
                std::cerr << "Invalid value for --steps\n";
                return false;
            }
        } else if (arg == "--output") {
            const auto value = require_value("--output");
            if (value == nullptr) {
                return false;
            }
            options.output_path = value;
        } else if (arg == "--start-pc") {
            const auto value = require_value("--start-pc");
            if (value == nullptr || !parse_u32(value, options.start_pc)) {
                std::cerr << "Invalid value for --start-pc\n";
                return false;
            }
        } else if (arg == "--thumb") {
            options.start_thumb = true;
        } else if (arg == "--help" || arg == "-h") {
            show_help = true;
            return true;
        } else {
            std::cerr << "Unknown option: " << arg << '\n';
            return false;
        }
    }

    if (options.rom_path.empty()) {
        std::cerr << "--rom is required\n";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options{};
    bool show_help = false;
    if (!parse_args(argc, argv, options, show_help)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (show_help) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }

    Emulator emulator;
    if (options.bios_path.has_value() && !emulator.load_bios_from_file(*options.bios_path)) {
        std::cerr << "Failed to load BIOS from " << options.bios_path->string() << '\n';
        return EXIT_FAILURE;
    }
    if (!emulator.load_rom_from_file(options.rom_path)) {
        std::cerr << "Failed to load ROM from " << options.rom_path.string() << '\n';
        return EXIT_FAILURE;
    }
    emulator.reset();

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 29) | (options.start_thumb ? (1u << 5) : 0u);
    state.regs[13] = 0x03007F00u;
    state.regs[14] = options.start_pc;
    state.regs[15] = options.start_pc;

    std::ofstream output(options.output_path);
    if (!output) {
        std::cerr << "Failed to open output file " << options.output_path.string() << '\n';
        return EXIT_FAILURE;
    }

    output << "step,cycle,cpsr,pc,r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r13,r14,r15\n";
    for (u64 step = 0; step < options.steps; ++step) {
        const auto& snapshot = cpu.state();
        output << step << ',' << cpu.current_cycle() << ',' << snapshot.cpsr << ',' << snapshot.regs[15];
        for (const auto reg : snapshot.regs) {
            output << ',' << reg;
        }
        output << '\n';
        cpu.step();
    }

    std::cout << "Wrote " << options.steps << " trace rows to " << options.output_path.string() << '\n';
    return EXIT_SUCCESS;
}
