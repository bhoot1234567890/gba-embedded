#pragma once

#include <array>

#include "gba/core/log.hpp"
#include "gba/core/types.hpp"

namespace gba {

class Bus;
class IrqController;

enum class CpuMode : u32 {
    User = 0x10,
    Fiq = 0x11,
    Irq = 0x12,
    Supervisor = 0x13,
    Abort = 0x17,
    Undefined = 0x1B,
    System = 0x1F,
};

enum class ExceptionType {
    Reset,
    Undefined,
    SoftwareInterrupt,
    PrefetchAbort,
    DataAbort,
    Irq,
    Fiq,
};

struct CpuState {
    std::array<u32, 16> regs{};
    u32 cpsr = static_cast<u32>(CpuMode::System);
    std::array<u32, 5> banked_usr_r8_r12{};
    std::array<std::array<u32, 2>, 5> banked_r13_r14{};
    std::array<u32, 7> banked_fiq_r8_r14{};
    u32 spsr_svc = 0;
    u32 spsr_irq = 0;
    u32 spsr_abt = 0;
    u32 spsr_und = 0;
    u32 spsr_fiq = 0;
    bool halted = false;
    AccessType next_fetch_access = AccessType::NonSequential;
};

class Arm7tdmi {
public:
    Arm7tdmi(Bus& bus, IrqController& irq, TraceLogger* logger = nullptr);

    void reset(bool skip_bios = false);
    [[nodiscard]] CpuState& state();
    [[nodiscard]] const CpuState& state() const;
    [[nodiscard]] u64 current_cycle() const;
    void set_current_cycle(u64 cycle);

    u64 cpu_run_until(u64 target_cycle);
    u32 step();
    void raise_exception(ExceptionType type);

private:
    [[nodiscard]] bool thumb_state() const;
    [[nodiscard]] CpuMode mode() const;
    [[nodiscard]] bool condition_passed(u32 condition) const;
    [[nodiscard]] u32 fetch_arm();
    [[nodiscard]] u16 fetch_thumb();
    [[nodiscard]] u32 pc_visible() const;
    [[nodiscard]] u32 thumb_pc_visible() const;
    bool handle_hle_swi(u32 comment);

    u32 execute_arm(u32 instruction);
    u32 execute_thumb(u16 instruction);
    [[nodiscard]] u32 read_visible_reg(u32 index) const;
    void arm_write_pc(u32 value);
    [[nodiscard]] AccessType data_access(AccessType access = AccessType::NonSequential) const;
    void break_fetch_burst(const BusAccessResult& result);
    void break_fetch_burst_for_internal();
    [[nodiscard]] u32 arm_open_bus_word() const;
    [[nodiscard]] u32 thumb_open_bus_word() const;
    [[nodiscard]] u32 resolve_arm_open_bus_word(const BusAccessResult& result) const;
    [[nodiscard]] u32 resolve_thumb_open_bus_word(const BusAccessResult& result) const;
    void trace_arm_gpio(const char* op, u32 address, BusWidth width, u32 value) const;
    void trace_thumb_gpio(const char* op, u32 address, BusWidth width, u32 value) const;
    [[nodiscard]] u32 arm_read8(u32 address);
    [[nodiscard]] u32 arm_read16(u32 address);
    [[nodiscard]] u32 arm_read32(u32 address);
    void arm_write8(u32 address, u32 value);
    void arm_write16(u32 address, u32 value);
    void arm_write32(u32 address, u32 value);
    [[nodiscard]] u32 thumb_read8(u32 address);
    [[nodiscard]] u32 thumb_read16(u32 address);
    [[nodiscard]] u32 thumb_read32(u32 address);
    void thumb_write8(u32 address, u32 value);
    void thumb_write16(u32 address, u32 value);
    void thumb_write32(u32 address, u32 value);

    void switch_mode(CpuMode new_mode);
    void enter_exception(ExceptionType type, CpuMode target_mode, u32 vector, bool mask_irq, bool mask_fiq, u32 return_address);
    void branch_to(u32 address, bool thumb);
    u32 read_user_reg(u32 reg);
    void write_user_reg(u32 reg, u32 value);
    void update_nz(u32 value);
    void update_nzc_add(u32 lhs, u32 rhs, u64 result);
    void update_nzc_sub(u32 lhs, u32 rhs, u64 result);
    void update_nzcv_adc(u32 lhs, u32 rhs, bool carry_in, u32 result);
    void update_nzcv_sbc(u32 lhs, u32 rhs, bool carry_in, u32 result);

    Bus& bus_;
    IrqController& irq_;
    TraceLogger* logger_;
    CpuState state_{};
    u64 current_cycle_ = 0;
    u64 last_fetch_cycle_ = 0;
    bool last_fetch_gamepak_ = false;
    bool hle_swi_enabled_ = false;

public:
    static constexpr u32 kPcTraceSize = 32;
    std::array<u32, kPcTraceSize> pc_trace_{};
    u32 pc_trace_pos_ = 0;
    bool pc_trace_enabled_ = false;
};

}  // namespace gba
