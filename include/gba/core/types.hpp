#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace gba {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

enum class BusWidth : u32 {
    Byte = 1,
    Half = 2,
    Word = 4,
};

enum class AccessType : u8 {
    NonSequential = 0,
    Sequential = 1u << 0,
    CodeFetch = 1u << 1,
    Dma = 1u << 2,
    Io = 1u << 3,
    CpuOutsideBios = 1u << 4,
};

[[nodiscard]] constexpr AccessType operator|(AccessType lhs, AccessType rhs) {
    return static_cast<AccessType>(static_cast<u8>(lhs) | static_cast<u8>(rhs));
}

[[nodiscard]] constexpr AccessType operator&(AccessType lhs, AccessType rhs) {
    return static_cast<AccessType>(static_cast<u8>(lhs) & static_cast<u8>(rhs));
}

constexpr AccessType& operator|=(AccessType& lhs, AccessType rhs) {
    lhs = lhs | rhs;
    return lhs;
}

[[nodiscard]] constexpr bool has_access_flag(AccessType access, AccessType flag) {
    return (static_cast<u8>(access) & static_cast<u8>(flag)) != 0;
}

struct BusAccessResult {
    u32 value = 0;
    u32 cycles = 0;
    bool open_bus = false;
    bool breaks_fetch_burst = false;
};

template <typename T>
[[nodiscard]] constexpr bool test_bit(T value, unsigned bit) {
    return (value & (static_cast<T>(1u) << bit)) != 0;
}

template <typename T>
constexpr void assign_bit(T& value, unsigned bit, bool set) {
    const auto mask = static_cast<T>(static_cast<T>(1u) << bit);
    value = set ? static_cast<T>(value | mask) : static_cast<T>(value & ~mask);
}

template <typename T>
[[nodiscard]] constexpr T align_down(T value, u32 alignment) {
    return static_cast<T>(value & ~static_cast<T>(alignment - 1u));
}

template <typename T>
[[nodiscard]] constexpr T rotate_right(T value, unsigned shift) {
    constexpr unsigned bits = std::numeric_limits<T>::digits;
    return std::rotr(value, static_cast<int>(shift % bits));
}

template <typename T>
[[nodiscard]] constexpr T rotate_left(T value, unsigned shift) {
    constexpr unsigned bits = std::numeric_limits<T>::digits;
    return std::rotl(value, static_cast<int>(shift % bits));
}

template <unsigned Bits, typename T>
[[nodiscard]] constexpr s32 sign_extend(T value) {
    static_assert(Bits > 0 && Bits <= 32);
    const auto shift = 32u - Bits;
    return static_cast<s32>(static_cast<s32>(value << shift) >> shift);
}

}  // namespace gba
