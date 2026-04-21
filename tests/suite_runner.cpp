#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "gba/core/cartridge.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/emulator.hpp"

using namespace gba;

constexpr int kNumSuites = 14;
constexpr int kBootFrames = 1200;
constexpr int kSuiteRunFrames = 600;
constexpr int kKeyDelay = 3;

static std::vector<std::string> g_test_output;
static bool g_capture = false;

static void debug_callback(const char* msg) {
    if (!msg || !msg[0]) {
        return;
    }
    std::printf("[DBG] %s\n", msg);
    if (g_capture) {
        g_test_output.push_back(msg);
    }
}

static void press_key(Emulator& emu, u16 key) {
    emu.set_keys(key);
    for (int i = 0; i < kKeyDelay; ++i) {
        emu.run_frame();
    }
    emu.set_keys(0);
    for (int i = 0; i < kKeyDelay; ++i) {
        emu.run_frame();
    }
}

static void run_frames(Emulator& emu, int count) {
    for (int i = 0; i < count; ++i) {
        emu.run_frame();
    }
}

int main(int argc, char** argv) {
    auto rom_path = std::filesystem::path("tests/assets/roms/suite.gba");
    auto bios_path = std::filesystem::path("tests/assets/roms/gba_bios.bin");

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            rom_path = argv[++i];
        } else if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            bios_path = argv[++i];
        }
    }

    Emulator emulator;
    emulator.bus().set_debug_output(debug_callback);

    if (!emulator.load_bios_from_file(bios_path)) {
        std::fprintf(stderr, "ERROR: Failed to load BIOS from %s\n", bios_path.c_str());
        return 1;
    }
    if (!emulator.load_rom_from_file(rom_path)) {
        std::fprintf(stderr, "ERROR: Failed to load ROM from %s\n", rom_path.c_str());
        return 1;
    }

    emulator.set_save_type(SaveType::Sram);
    emulator.reset();

    std::printf("=== GBA Test Suite Runner ===\n");
    std::printf("BIOS: %s\n", bios_path.c_str());
    std::printf("ROM:  %s\n", rom_path.c_str());
    std::printf("Booting BIOS...\n");

    g_capture = false;
    run_frames(emulator, kBootFrames);
    std::printf("Boot complete (%d frames). Starting test suites...\n\n", kBootFrames);

    const char* suite_names[] = {
        "Memory",           "I/O read",    "Timing",         "Timer count-up",
        "Timer IRQ",        "Shifter",     "Carry",          "Multiply long",
        "BIOS math",        "DMA",         "SIO register R/W", "SIO timing",
        "Misc. edge case",  "Video"};

    g_capture = true;

    for (int suite = 0; suite < kNumSuites; ++suite) {
        std::printf("--- Suite %02d: %s ---\n", suite + 1, suite_names[suite]);

        press_key(emulator, kKeyA);
        run_frames(emulator, kSuiteRunFrames);
        press_key(emulator, kKeyB);
        run_frames(emulator, 5);

        if (suite < kNumSuites - 1) {
            press_key(emulator, kKeyDown);
            run_frames(emulator, 3);
        }
    }

    g_capture = false;

    std::printf("\n=== SRAM Output ===\n");
    auto& bus = emulator.bus();
    auto cycle = emulator.cpu().current_cycle();
    std::string sram_text;
    for (u32 i = 0; i < 0x8000; ++i) {
        auto r = bus.read(0x0E000000u + i, BusWidth::Byte, AccessType::Io, cycle);
        auto ch = static_cast<unsigned char>(r.value & 0xFFu);
        if (ch == 0xFF || ch == '\0') {
            if (!sram_text.empty() && sram_text.back() != '\n') {
                sram_text.push_back('\n');
            }
            if (ch == 0xFF && i > 0) {
                break;
            }
            continue;
        }
        sram_text.push_back(static_cast<char>(ch));
    }
    std::printf("%s\n", sram_text.c_str());

    int total_pass = 0;
    int total_tests = 0;
    for (auto& line : g_test_output) {
        if (line.compare(0, 4, "END:") == 0) {
            auto slash = line.find('/');
            if (slash != std::string::npos) {
                try {
                    int p = std::stoi(line.substr(4, slash - 4));
                    int t = std::stoi(line.substr(slash + 1));
                    total_pass += p;
                    total_tests += t;
                } catch (...) {}
            }
        }
    }

    std::printf("\n=== Summary ===\n");
    std::printf("Total: %d/%d passed\n", total_pass, total_tests);

    return 0;
}
