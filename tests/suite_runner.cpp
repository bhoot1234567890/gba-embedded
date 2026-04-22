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
constexpr int kBootFrames = 60;
constexpr int kKeyDelay = 3;
constexpr int kPostBootAFrames = 120;
constexpr int kPostBootBFrames = 120;
constexpr int kMenuScrollFrames = 30;
constexpr int kSuitePollFrames = 1;
constexpr int kSuiteSettleFrames = 90;
constexpr int kSuiteMaxFrames = 1800;

static std::vector<std::string> g_test_output;
static bool g_capture = false;

static void debug_callback(const char* msg) {
    if (!msg || !msg[0]) return;
    std::printf("[DBG] %s\n", msg);
    if (g_capture) g_test_output.push_back(msg);
}

static void press_key(Emulator& emu, u16 key) {
    emu.set_keys(key);
    for (int i = 0; i < kKeyDelay; ++i) emu.run_frame();
    emu.set_keys(0);
    for (int i = 0; i < kKeyDelay; ++i) emu.run_frame();
}

static void run_frames(Emulator& emu, int count) {
    for (int i = 0; i < count; ++i) emu.run_frame();
}

struct RunnerConfig {
    std::filesystem::path rom_path;
    std::filesystem::path bios_path;
    bool use_stub_bios = false;
};

struct SuiteResult {
    int suite_index = 0;
    std::string suite_name;
    std::vector<std::string> debug_lines;
    std::string sram_text;
    u32 pc = 0;
    u32 cpsr = 0;
    bool halted = false;
    u64 cycle = 0;
    int frames_run = 0;
};

static void write_u32(std::vector<u8>& buf, u32 offset, u32 val) {
    buf[offset] = static_cast<u8>(val);
    buf[offset + 1] = static_cast<u8>(val >> 8);
    buf[offset + 2] = static_cast<u8>(val >> 16);
    buf[offset + 3] = static_cast<u8>(val >> 24);
}

static std::vector<u8> make_bios_stub() {
    std::vector<u8> bios(kBiosSize, 0xFF);

    auto put = [&](u32 addr, u32 instr) { write_u32(bios, addr, instr); };

    // Exception vectors
    put(0x00, 0xEA00001E); // B reset_handler (0x80)
    put(0x04, 0xEAFFFFFE); // B . (undefined loop)
    put(0x08, 0xEAFFFFFE); // B . (SWI - HLE intercepts)
    put(0x0C, 0xEAFFFFFE); // B . (prefetch abort)
    put(0x10, 0xEAFFFFFE); // B . (data abort)
    put(0x14, 0xEAFFFFFE); // B . (reserved)
    put(0x18, 0xEA000008); // B irq_handler (0x40)
    put(0x1C, 0xEAFFFFFE); // B . (FIQ)

    // irq_handler at 0x40
    // STMFD SP!, {R0-R3,R12,LR}
    put(0x40, 0xE92D500F);
    // LDR R12, [PC, #20] → load 0x03007FFC from 0x40+8+20 = 0x5C
    put(0x44, 0xE59FC014);
    // LDR R12, [R12] → load handler address
    put(0x48, 0xE59CF000);
    // CMP R12, #0
    put(0x4C, 0xE35C0000);
    // LDMEQFD SP!, {R0-R3,R12,LR} (no handler, return)
    put(0x50, 0xE89D500F); // LDMFD
    // SUBEQS PC, LR, #4
    put(0x54, 0xE25EF004); // SUBS PC, LR, #4 (always return from IRQ)
    // MOV LR, PC
    put(0x58, 0xE1A0E00F);
    // MOV PC, R12 → call user handler
    put(0x5C, 0xE1A0F00C);
    // After handler returns: LDMFD SP!, {R0-R3,R12,LR}
    put(0x60, 0xE89D500F);
    // SUBS PC, LR, #4 → return from IRQ
    put(0x64, 0xE25EF004);
    // Data: pointer to handler pointer
    put(0x68, 0x03007FFC);

    // Wait, the LDR at 0x44 loads from PC+8+20 = 0x44+8+20 = 0x50. That's wrong.
    // Let me recalculate. At 0x44, PC=0x4C. LDR R12, [PC, #imm].
    // PC + imm = 0x4C + imm. We want to load from address containing 0x03007FFC.
    // Put the data word at the end of the handler.
    // Actually let me reorganize:

    // Let me redo the irq handler more carefully:
    // irq_handler at 0x40:
    // 0x40: STMFD SP!, {R0-R3,R12,LR}      = E92D500F
    // 0x44: LDR R12, [PC, #0x1C]            loads from 0x44+8+0x1C = 0x68
    //       = E59FC01C
    // 0x48: LDR R12, [R12,#0]               = E59CF000
    // 0x4C: CMP R12, #0                     = E35C0000
    // 0x50: LDMFDNE SP!, {R0-R3,R12,LR}    skip restore if handler exists... no
    // Actually let me simplify - just always try to call handler
    // 0x50: MOV LR, PC                      = E1A0E00F
    // 0x54: MOV PC, R12                     = E1A0F00C
    // 0x58: LDMFD SP!, {R0-R3,R12,LR}      = E89D500F
    // 0x5C: SUBS PC, LR, #4                = E25EF004
    // 0x60: B .                             = EAFFFFFE (safety loop)
    // ...
    // 0x68: .word 0x03007FFC               (data for LDR at 0x44)

    // Overwrite what we wrote above
    put(0x40, 0xE92D500F); // STMFD SP!, {R0-R3,R12,LR}
    put(0x44, 0xE59FC01C); // LDR R12, [PC, #0x1C] → 0x4C+0x1C=0x68
    put(0x48, 0xE59CF000); // LDR R12, [R12]
    put(0x4C, 0xE35C0000); // CMP R12, #0
    put(0x50, 0x01A0E00F); // MOVEQ LR, PC (skip if no handler)
    put(0x54, 0x01A0F004); // MOVEQ PC, LR+4? No...
    // Just branch to return if no handler
    put(0x50, 0x0A000002); // BEQ return (skip to 0x60)
    put(0x54, 0xE1A0E00F); // MOV LR, PC
    put(0x58, 0xE1A0F00C); // MOV PC, R12 → call handler
    // return:
    put(0x5C, 0xE89D500F); // LDMFD SP!, {R0-R3,R12,LR}
    put(0x60, 0xE25EF004); // SUBS PC, LR, #4 → return from IRQ
    put(0x64, 0xEAFFFFFE); // B . (safety)
    put(0x68, 0x03007FFC); // data: pointer to handler pointer

    // reset_handler at 0x80:
    // Set up IRQ mode stack
    put(0x80, 0xE321F0D2); // MSR CPSR_c, #0xD2 (IRQ mode, I+F)
    put(0x84, 0xE59FD044); // LDR SP, [PC, #0x44] → 0x8C+0x44=0xD0
    // Set up Supervisor mode stack
    put(0x88, 0xE321F0D3); // MSR CPSR_c, #0xD3 (SVC mode, I+F)
    put(0x8C, 0xE59FD038); // LDR SP, [PC, #0x38] → 0x94+0x38=0xCC... wait

    // Let me be more careful with PC-relative loads.
    // At address X, PC reads as X+8.
    // LDR Rd, [PC, #imm] loads from X+8+imm.

    // reset_handler at 0x80:
    // 0x80: MSR CPSR_c, #0xD2          → E321F0D2
    // 0x84: LDR SP, [PC, #0x44]        → loads from 0x84+8+0x44 = 0xD0
    //       → E59FD044
    // 0x88: MSR CPSR_c, #0xD3          → E321F0D3
    // 0x8C: LDR SP, [PC, #0x34]        → loads from 0x8C+8+0x34 = 0xC8
    //       → E59FD034
    // 0x90: MSR CPSR_c, #0xDF          → E321F0DF (System mode, no I/F)
    // 0x94: LDR SP, [PC, #0x24]        → loads from 0x94+8+0x24 = 0xC0
    //       → E59FD024
    // 0x98: MOV R0, #0                 → E3A00000
    // 0x9C: LDR R1, [PC, #0x1C]        → loads from 0x9C+8+0x1C = 0xC4
    //       → E59F101C
    // 0xA0: STR R0, [R1]               → E5810000
    // 0xA4: MOV R0, #0x08000000>>24?   Can't encode directly
    // Let me use LDR PC instead:
    // 0xA4: LDR PC, [PC, #0x10]        → loads from 0xA4+8+0x10 = 0xBC
    //       → E59FF010
    // Data:
    // 0xBC: .word 0x08000000
    // 0xC0: .word 0x03007F00  (System SP)
    // 0xC4: .word 0x03007FFC  (handler ptr)
    // 0xC8: .word 0x03007FE0  (SVC SP)
    // 0xD0: .word 0x03007FA0  (IRQ SP)

    put(0x80, 0xE321F0D2); // MSR CPSR_c, #0xD2
    put(0x84, 0xE59FD044); // LDR SP, [PC, #0x44] → 0xD0
    put(0x88, 0xE321F0D3); // MSR CPSR_c, #0xD3
    put(0x8C, 0xE59FD034); // LDR SP, [PC, #0x34] → 0xC8
    put(0x90, 0xE321F0DF); // MSR CPSR_c, #0xDF
    put(0x94, 0xE59FD024); // LDR SP, [PC, #0x24] → 0xC0
    put(0x98, 0xE3A00000); // MOV R0, #0
    put(0x9C, 0xE59F101C); // LDR R1, [PC, #0x1C] → 0xC4
    put(0xA0, 0xE5810000); // STR R0, [R1]
    put(0xA4, 0xE59FF010); // LDR PC, [PC, #0x10] → 0xBC

    // Data pool
    put(0xBC, 0x08000000); // ROM entry point
    put(0xC0, 0x03007F00); // System SP
    put(0xC4, 0x03007FFC); // IRQ handler pointer address
    put(0xC8, 0x03007FE0); // SVC SP
    put(0xD0, 0x03007FA0); // IRQ SP

    return bios;
}

static bool load_bios(Emulator& emulator, const RunnerConfig& config) {
    if (!config.use_stub_bios && !config.bios_path.empty()) {
        if (emulator.load_bios_from_file(config.bios_path)) {
            return true;
        }

        std::fprintf(stderr, "WARNING: Failed to load BIOS from %s, falling back to stub BIOS\n",
                     config.bios_path.c_str());
    }

    emulator.load_bios(make_bios_stub());
    return false;
}

static bool configure_emulator(Emulator& emulator, const RunnerConfig& config, bool* loaded_real_bios = nullptr) {
    emulator.bus().set_debug_output(debug_callback);

    const bool real_bios = load_bios(emulator, config);
    if (loaded_real_bios) {
        *loaded_real_bios = real_bios;
    }

    if (!emulator.load_rom_from_file(config.rom_path)) {
        std::fprintf(stderr, "ERROR: Failed to load ROM from %s\n", config.rom_path.c_str());
        return false;
    }

    emulator.set_save_type(SaveType::Sram);
    emulator.reset();
    return true;
}

static std::string read_sram_text(Emulator& emulator) {
    auto& bus = emulator.bus();
    const auto cycle = emulator.cpu().current_cycle();
    std::string text;
    bool saw_payload = false;
    for (u32 i = 0; i < 0x8000; ++i) {
        const auto result = bus.read(0x0E000000u + i, BusWidth::Byte, AccessType::Io, cycle);
        const auto ch = static_cast<unsigned char>(result.value & 0xFFu);
        if (ch == 0xFF || ch == '\0') {
            if (!text.empty() && text.back() != '\n') {
                text.push_back('\n');
            }
            if (ch == 0xFF && saw_payload) {
                break;
            }
            continue;
        }
        saw_payload = true;
        text.push_back(static_cast<char>(ch));
    }

    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

static bool has_meaningful_suite_output(const std::string& text) {
    if (text.empty()) {
        return false;
    }

    if (text == "Game Boy Advance Test Suite\n===" ||
        text == "Game Boy Advance Test Suite\n===\n") {
        return false;
    }

    return text.find("FAIL") != std::string::npos || text.find("PASS") != std::string::npos ||
           text.find("END:") != std::string::npos || text.size() > 32u;
}

static bool has_end_in_debug_output() {
    for (const auto& line : g_test_output) {
        if (line.compare(0, 4, "END:") == 0) {
            return true;
        }
    }
    return false;
}

static void navigate_to_suite_menu(Emulator& emulator) {
    run_frames(emulator, kBootFrames);
    press_key(emulator, kKeyA);
    run_frames(emulator, kPostBootAFrames);
    press_key(emulator, kKeyB);
    run_frames(emulator, kPostBootBFrames);
}

static SuiteResult run_suite(const RunnerConfig& config, int suite_index, const char* suite_name) {
    Emulator emulator;
    if (!configure_emulator(emulator, config)) {
        std::exit(1);
    }
    g_test_output.clear();
    g_capture = true;

    navigate_to_suite_menu(emulator);
    for (int i = 0; i < suite_index; ++i) {
        press_key(emulator, kKeyDown);
        run_frames(emulator, kMenuScrollFrames);
    }

    press_key(emulator, kKeyA);

    std::string best_sram_text;
    int stable_frames = 0;
    int frames_run = 0;
    std::size_t last_debug_count = 0;
    while (frames_run < kSuiteMaxFrames) {
        run_frames(emulator, kSuitePollFrames);
        frames_run += kSuitePollFrames;

        const auto sram_text = read_sram_text(emulator);
        const bool debug_changed = g_test_output.size() != last_debug_count;
        last_debug_count = g_test_output.size();
        if (sram_text != best_sram_text) {
            best_sram_text = sram_text;
            stable_frames = 0;
        } else if (debug_changed) {
            stable_frames = 0;
        } else if (has_end_in_debug_output() || best_sram_text.find("END:") != std::string::npos) {
            break;
        } else if (has_meaningful_suite_output(best_sram_text) || !g_test_output.empty()) {
            stable_frames += kSuitePollFrames;
            if (stable_frames >= kSuiteSettleFrames) {
                break;
            }
        }
    }

    g_capture = false;

    auto& cpu = emulator.cpu();
    SuiteResult result;
    result.suite_index = suite_index;
    result.suite_name = suite_name;
    result.debug_lines = g_test_output;
    result.sram_text = std::move(best_sram_text);
    result.pc = cpu.state().regs[15];
    result.cpsr = cpu.state().cpsr;
    result.halted = cpu.state().halted;
    result.cycle = cpu.current_cycle();
    result.frames_run = frames_run;
    return result;
}

static void print_suite_result(const SuiteResult& result) {
    std::printf("\n--- Suite %02d: %s ---\n", result.suite_index + 1, result.suite_name.c_str());
    std::printf("Frames: %d  PC=0x%08X  CPSR=0x%08X  halted=%d  cycle=%llu\n",
                result.frames_run, result.pc, result.cpsr, result.halted,
                static_cast<unsigned long long>(result.cycle));

    if (!result.debug_lines.empty()) {
        std::printf("[debug]\n");
        for (const auto& line : result.debug_lines) {
            std::printf("%s\n", line.c_str());
        }
    }

    std::printf("[sram]\n");
    if (result.sram_text.empty()) {
        std::printf("(empty)\n");
    } else {
        std::printf("%s\n", result.sram_text.c_str());
    }
    std::fflush(stdout);
}

static void accumulate_totals(const SuiteResult& result, int& total_pass, int& total_tests,
                              int& suites_with_results) {
    for (const auto& line : result.debug_lines) {
        if (line.compare(0, 4, "END:") == 0) {
            const auto slash = line.find('/');
            if (slash == std::string::npos) {
                continue;
            }
            try {
                total_pass += std::stoi(line.substr(4, slash - 4));
                total_tests += std::stoi(line.substr(slash + 1));
                ++suites_with_results;
            } catch (...) {
            }
        }
    }
}

int main(int argc, char** argv) {
    std::printf("=== GBA Test Suite Runner ===\n");
    std::fflush(stdout);

    RunnerConfig config{
        .rom_path = std::filesystem::path("tests/assets/roms/suite.gba"),
        .bios_path = std::filesystem::path("tests/assets/roms/gba_bios.bin"),
        .use_stub_bios = false,
    };
    int single_suite = -1;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            config.rom_path = argv[++i];
        } else if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            config.bios_path = argv[++i];
            config.use_stub_bios = false;
        } else if (std::strcmp(argv[i], "--stub-bios") == 0) {
            config.use_stub_bios = true;
        } else if (std::strcmp(argv[i], "--suite") == 0 && i + 1 < argc) {
            single_suite = std::atoi(argv[++i]);
        }
    }

    Emulator boot_emulator;
    bool loaded_real_bios = false;
    if (!configure_emulator(boot_emulator, config, &loaded_real_bios)) {
        return 1;
    }

    std::printf("ROM:  %s\n", config.rom_path.c_str());
    if (loaded_real_bios) {
        std::printf("BIOS: %s\n", config.bios_path.c_str());
    } else {
        std::printf("BIOS: stub (%zu bytes)\n", (size_t)kBiosSize);
    }
    std::printf("Booting...\n");
    std::fflush(stdout);

    run_frames(boot_emulator, kBootFrames);
    auto& cpu = boot_emulator.cpu();
    std::printf("After %d frames: PC=0x%08X CPSR=0x%08X halted=%d cycle=%llu\n",
                kBootFrames, cpu.state().regs[15], cpu.state().cpsr,
                cpu.state().halted, (unsigned long long)cpu.current_cycle());
    std::fflush(stdout);

    const char* suite_names[] = {
        "Memory", "I/O read", "Timing", "Timer count-up",
        "Timer IRQ", "Shifter", "Carry", "Multiply long",
        "BIOS math", "DMA", "SIO register R/W", "SIO timing",
        "Misc. edge case", "Video"};

    int total_pass = 0;
    int total_tests = 0;
    int suites_with_results = 0;
    const int first_suite = single_suite >= 0 ? single_suite : 0;
    const int last_suite = single_suite >= 0 ? single_suite + 1 : kNumSuites;
    if (first_suite < 0 || last_suite > kNumSuites) {
        std::fprintf(stderr, "ERROR: --suite must be in range [0, %d)\n", kNumSuites);
        return 1;
    }

    for (int suite = first_suite; suite < last_suite; ++suite) {
        const auto result = run_suite(config, suite, suite_names[suite]);
        print_suite_result(result);
        accumulate_totals(result, total_pass, total_tests, suites_with_results);
    }

    std::printf("\n=== Summary ===\n");
    if (suites_with_results > 0 || total_tests > 0) {
        std::printf("Total: %d/%d passed\n", total_pass, total_tests);
    } else {
        std::printf("No END: summaries were emitted yet; use the per-suite SRAM output above.\n");
    }
    return 0;
}
