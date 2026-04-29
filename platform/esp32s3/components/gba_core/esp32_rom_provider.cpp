#include "gba/platform/esp32_rom_provider.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

namespace gba {

namespace {

constexpr u32 kInvalidPage = std::numeric_limits<u32>::max();
const char* kTag = "rom_provider";

struct HeapCapsDeleter {
    void operator()(u8* ptr) const {
        if (ptr) {
            heap_caps_free(ptr);
        }
    }
};

[[nodiscard]] u8* alloc_rom_cache(std::size_t bytes) {
    auto* ptr = static_cast<u8*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ptr) {
        ptr = static_cast<u8*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    }
    return ptr;
}

class Esp32MmapRomProvider final : public RomProvider {
public:
    Esp32MmapRomProvider(const esp_partition_t* partition,
                         std::size_t size,
                         std::size_t window_bytes,
                         std::size_t window_count)
        : partition_(partition),
          size_(size),
          window_bytes_(std::max<std::size_t>(4096u, window_bytes)),
          windows_(std::max<std::size_t>(1u, window_count)) {}

    ~Esp32MmapRomProvider() override {
        for (auto& window : windows_) {
            if (window.valid) {
                esp_partition_munmap(window.handle);
            }
        }
    }

    [[nodiscard]] std::size_t size() const override { return size_; }

    [[nodiscard]] u8 read_byte(u32 address) const override {
        u8 value = 0xFF;
        (void)read_bytes(address, std::span<u8>(&value, 1));
        return value;
    }

    [[nodiscard]] bool read_bytes(u32 address, std::span<u8> out) const override {
        if (out.empty()) {
            return true;
        }
        if (static_cast<std::size_t>(address) + out.size() > size_) {
            return false;
        }

        frame_stats_.byte_reads += out.size();
        std::size_t copied = 0;
        while (copied < out.size()) {
            const auto absolute = static_cast<std::size_t>(address) + copied;
            const auto page = static_cast<u32>(absolute / window_bytes_);
            const auto offset_in_window = absolute % window_bytes_;
            const auto chunk = std::min(out.size() - copied, window_bytes_ - offset_in_window);
            const auto* window = ensure_window(page, false);
            if (!window) {
                std::fill(out.begin() + static_cast<std::ptrdiff_t>(copied),
                          out.begin() + static_cast<std::ptrdiff_t>(copied + chunk),
                          0xFFu);
            } else {
                std::memcpy(out.data() + copied, window + offset_in_window, chunk);
            }
            copied += chunk;
        }
        return true;
    }

    void prefetch(u32 address, std::size_t bytes) const override {
        if (bytes == 0 || static_cast<std::size_t>(address) >= size_) {
            return;
        }
        ++frame_stats_.prefetches;
        frame_stats_.prefetch_bytes += bytes;
        const auto end = std::min<std::size_t>(size_, static_cast<std::size_t>(address) + bytes);
        for (auto cursor = static_cast<std::size_t>(address); cursor < end; cursor = ((cursor / window_bytes_) + 1u) * window_bytes_) {
            (void)ensure_window(static_cast<u32>(cursor / window_bytes_), true);
        }
    }

    [[nodiscard]] RomAccessStats frame_stats() const override {
        return frame_stats_;
    }

    void reset_frame_stats() const override {
        frame_stats_ = {};
    }

private:
    struct Window {
        bool valid = false;
        u32 page = kInvalidPage;
        const u8* data = nullptr;
        std::size_t mapped_size = 0;
        u64 age = 0;
        esp_partition_mmap_handle_t handle = 0;
    };

    [[nodiscard]] const u8* ensure_window(u32 page, bool) const {
        for (auto& window : windows_) {
            if (window.valid && window.page == page) {
                ++frame_stats_.cache_hits;
                window.age = ++clock_;
                return window.data;
            }
        }

        ++frame_stats_.cache_misses;
        auto* victim = &windows_.front();
        for (auto& window : windows_) {
            if (!window.valid) {
                victim = &window;
                break;
            }
            if (window.age < victim->age) {
                victim = &window;
            }
        }

        if (victim->valid) {
            esp_partition_munmap(victim->handle);
            *victim = {};
        }

        const auto offset = static_cast<std::size_t>(page) * window_bytes_;
        if (offset >= size_) {
            return nullptr;
        }

        const auto bytes = std::min(window_bytes_, size_ - offset);
        const void* ptr = nullptr;
        esp_partition_mmap_handle_t handle = 0;
        const auto ret = esp_partition_mmap(partition_, offset, bytes, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "esp_partition_mmap page=%lu offset=%u size=%u failed: %s",
                     static_cast<unsigned long>(page),
                     static_cast<unsigned>(offset),
                     static_cast<unsigned>(bytes),
                     esp_err_to_name(ret));
            return nullptr;
        }

        victim->valid = true;
        victim->page = page;
        victim->data = static_cast<const u8*>(ptr);
        victim->mapped_size = bytes;
        victim->age = ++clock_;
        victim->handle = handle;
        return victim->data;
    }

    const esp_partition_t* partition_ = nullptr;
    std::size_t size_ = 0;
    std::size_t window_bytes_ = 64u * 1024u;
    mutable std::vector<Window> windows_;
    mutable u64 clock_ = 0;
    mutable RomAccessStats frame_stats_{};
};

class Esp32SdCacheRomProvider final : public RomProvider {
public:
    Esp32SdCacheRomProvider(FILE* file,
                            std::size_t size,
                            std::unique_ptr<u8, HeapCapsDeleter> cache,
                            std::size_t page_bytes,
                            std::size_t page_count)
        : file_(file, &std::fclose),
          size_(size),
          cache_(std::move(cache)),
          page_bytes_(page_bytes),
          pages_(std::max<std::size_t>(1u, page_count)) {
        for (std::size_t index = 0; index < pages_.size(); ++index) {
            pages_[index].data = cache_.get() + index * page_bytes_;
        }
    }

    [[nodiscard]] std::size_t size() const override { return size_; }

    [[nodiscard]] u8 read_byte(u32 address) const override {
        u8 value = 0xFF;
        (void)read_bytes(address, std::span<u8>(&value, 1));
        return value;
    }

    [[nodiscard]] bool read_bytes(u32 address, std::span<u8> out) const override {
        if (out.empty()) {
            return true;
        }
        if (static_cast<std::size_t>(address) + out.size() > size_) {
            return false;
        }

        frame_stats_.byte_reads += out.size();
        std::size_t copied = 0;
        while (copied < out.size()) {
            const auto absolute = static_cast<std::size_t>(address) + copied;
            const auto page = static_cast<u32>(absolute / page_bytes_);
            const auto offset_in_page = absolute % page_bytes_;
            const auto chunk = std::min(out.size() - copied, page_bytes_ - offset_in_page);
            const auto* page_data = ensure_page(page, false);
            if (!page_data) {
                std::fill(out.begin() + static_cast<std::ptrdiff_t>(copied),
                          out.begin() + static_cast<std::ptrdiff_t>(copied + chunk),
                          0xFFu);
            } else {
                std::memcpy(out.data() + copied, page_data + offset_in_page, chunk);
            }
            copied += chunk;
        }
        return true;
    }

    void prefetch(u32 address, std::size_t bytes) const override {
        if (bytes == 0 || static_cast<std::size_t>(address) >= size_) {
            return;
        }
        ++frame_stats_.prefetches;
        frame_stats_.prefetch_bytes += bytes;
        const auto end = std::min<std::size_t>(size_, static_cast<std::size_t>(address) + bytes);
        for (auto cursor = static_cast<std::size_t>(address); cursor < end; cursor = ((cursor / page_bytes_) + 1u) * page_bytes_) {
            (void)ensure_page(static_cast<u32>(cursor / page_bytes_), true);
        }
    }

    [[nodiscard]] RomAccessStats frame_stats() const override {
        return frame_stats_;
    }

    void reset_frame_stats() const override {
        frame_stats_ = {};
    }

private:
    struct Page {
        bool valid = false;
        u32 index = kInvalidPage;
        u8* data = nullptr;
        u64 age = 0;
    };

    [[nodiscard]] const u8* ensure_page(u32 page_index, bool) const {
        for (auto& page : pages_) {
            if (page.valid && page.index == page_index) {
                ++frame_stats_.cache_hits;
                page.age = ++clock_;
                return page.data;
            }
        }

        ++frame_stats_.cache_misses;
        auto* victim = &pages_.front();
        for (auto& page : pages_) {
            if (!page.valid) {
                victim = &page;
                break;
            }
            if (page.age < victim->age) {
                victim = &page;
            }
        }

        const auto offset = static_cast<std::size_t>(page_index) * page_bytes_;
        if (offset >= size_) {
            return nullptr;
        }

        if (std::fseek(file_.get(), static_cast<long>(offset), SEEK_SET) != 0) {
            ESP_LOGE(kTag, "ROM cache seek failed offset=%u", static_cast<unsigned>(offset));
            return nullptr;
        }

        const auto remaining = std::min(page_bytes_, size_ - offset);
        const auto read = std::fread(victim->data, 1, remaining, file_.get());
        if (read != remaining && std::ferror(file_.get())) {
            ESP_LOGE(kTag, "ROM cache read failed page=%lu", static_cast<unsigned long>(page_index));
            std::clearerr(file_.get());
            return nullptr;
        }
        if (read < page_bytes_) {
            std::memset(victim->data + read, 0xFF, page_bytes_ - read);
        }

        victim->valid = true;
        victim->index = page_index;
        victim->age = ++clock_;
        return victim->data;
    }

    std::unique_ptr<FILE, int (*)(FILE*)> file_;
    std::size_t size_ = 0;
    std::unique_ptr<u8, HeapCapsDeleter> cache_;
    std::size_t page_bytes_ = 32u * 1024u;
    mutable std::vector<Page> pages_;
    mutable u64 clock_ = 0;
    mutable RomAccessStats frame_stats_{};
};

}  // namespace

std::unique_ptr<RomProvider> make_esp32_mmap_rom_provider(const char* partition_label,
                                                          std::size_t max_size,
                                                          std::size_t window_bytes,
                                                          std::size_t window_count) {
    const auto* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, partition_label);
    if (!partition) {
        ESP_LOGW(kTag, "ROM mmap partition '%s' not found", partition_label);
        return nullptr;
    }

    const auto size = std::min<std::size_t>(partition->size, max_size);
    ESP_LOGI(kTag, "ROM mmap provider partition=%s size=%u window=%u count=%u",
             partition_label,
             static_cast<unsigned>(size),
             static_cast<unsigned>(window_bytes),
             static_cast<unsigned>(window_count));
    return std::make_unique<Esp32MmapRomProvider>(partition, size, window_bytes, window_count);
}

std::unique_ptr<RomProvider> make_esp32_sd_cache_rom_provider(const char* path,
                                                             std::size_t cache_bytes,
                                                             std::size_t page_bytes) {
    FILE* file = std::fopen(path, "rb");
    if (!file) {
        ESP_LOGE(kTag, "failed to open ROM file %s", path);
        return nullptr;
    }

    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return nullptr;
    }
    const auto size_long = std::ftell(file);
    if (size_long <= 0) {
        std::fclose(file);
        return nullptr;
    }
    std::rewind(file);

    auto page_count = std::max<std::size_t>(1u, cache_bytes / page_bytes);
    while (page_count > 0) {
        const auto bytes = page_count * page_bytes;
        std::unique_ptr<u8, HeapCapsDeleter> cache(alloc_rom_cache(bytes));
        if (cache) {
            ESP_LOGI(kTag, "ROM SD cache provider path=%s size=%u cache=%u pages=%u",
                     path,
                     static_cast<unsigned>(size_long),
                     static_cast<unsigned>(bytes),
                     static_cast<unsigned>(page_count));
            return std::make_unique<Esp32SdCacheRomProvider>(
                file, static_cast<std::size_t>(size_long), std::move(cache), page_bytes, page_count);
        }
        page_count /= 2u;
    }

    ESP_LOGE(kTag, "failed to allocate ROM cache for %s", path);
    std::fclose(file);
    return nullptr;
}

}  // namespace gba
