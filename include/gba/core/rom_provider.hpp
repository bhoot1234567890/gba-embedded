#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "gba/core/types.hpp"

namespace gba {

struct RomAccessStats {
    u64 byte_reads = 0;
    u64 cache_hits = 0;
    u64 cache_misses = 0;
    u64 sequential_hits = 0;
    u64 prefetches = 0;
    u64 prefetch_bytes = 0;
    u64 prefetch_misses = 0;
    u64 prefetch_hits = 0;
    u64 miss_penalty_us = 0;
    u64 miss_penalty_cycles = 0;
    u32 unique_pages = 0;
};

class RomProvider {
public:
    virtual ~RomProvider() = default;

    [[nodiscard]] virtual std::size_t size() const = 0;
    [[nodiscard]] virtual u8 read_byte(u32 address) const = 0;
    [[nodiscard]] virtual u16 read16(u32 address) const;
    [[nodiscard]] virtual u32 read32(u32 address) const;

    [[nodiscard]] bool empty() const { return size() == 0; }
    [[nodiscard]] virtual std::span<const u8> contiguous_span() const { return {}; }
    [[nodiscard]] virtual std::span<const u8> direct_read_span() const { return {}; }
    [[nodiscard]] virtual bool read_bytes(u32 address, std::span<u8> out) const;
    [[nodiscard]] bool contains(std::string_view needle) const;

    virtual void prefetch(u32 address, std::size_t bytes) const;
    [[nodiscard]] virtual RomAccessStats frame_stats() const;
    virtual void reset_frame_stats() const;
};

class MemoryRomProvider final : public RomProvider {
public:
    explicit MemoryRomProvider(std::vector<u8> rom, std::size_t profile_cache_pages = default_profile_cache_pages());

    [[nodiscard]] std::size_t size() const override { return rom_.size(); }
    [[nodiscard]] u8 read_byte(u32 address) const override;
    [[nodiscard]] u16 read16(u32 address) const override;
    [[nodiscard]] u32 read32(u32 address) const override;
    [[nodiscard]] std::span<const u8> contiguous_span() const override { return rom_; }
    [[nodiscard]] std::span<const u8> direct_read_span() const override;
    [[nodiscard]] bool read_bytes(u32 address, std::span<u8> out) const override;
    void prefetch(u32 address, std::size_t bytes) const override;
    [[nodiscard]] RomAccessStats frame_stats() const override;
    void reset_frame_stats() const override;

    [[nodiscard]] static std::size_t default_profile_cache_pages();

private:
    void touch_range(u32 address, std::size_t bytes, bool prefetch) const;
    [[nodiscard]] bool touch_page(u32 page, bool prefetch, bool sequential) const;
    [[nodiscard]] bool note_demand_access(u32 address, std::size_t bytes) const;
    void maybe_prefetch_after_read(u32 address, std::size_t bytes, bool sequential, bool had_miss) const;

    std::vector<u8> rom_;
    std::size_t profile_cache_pages_ = 0;
    mutable RomAccessStats frame_stats_{};
    mutable std::vector<u32> lru_pages_;
    mutable std::vector<u32> unique_pages_;
    mutable std::vector<u32> prefetched_pages_;
    mutable bool has_last_demand_ = false;
    mutable u32 last_demand_end_ = 0;
    mutable u32 sequential_run_ = 0;
    mutable u32 last_prefetch_base_page_ = 0xFFFFFFFFu;
};

}  // namespace gba
