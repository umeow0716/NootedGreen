#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

// Experimental owned-data bridge. Caller must provide readable pinned native
// records; no discovery, foreign-memory writes, or callback forwarding here.
class TglCapabilityAdapter {
public:
    using Native = std::array<uint8_t, 0x430>;
    using Consumer = std::array<uint8_t, 0x438>;
    bool build(const std::vector<const Native *> &input) {
        clear();
        if (input.empty() || input.size() > 85) return false;
        for (size_t i = 0; i < input.size(); ++i) {
            if (!input[i]) return false;
            for (size_t j = 0; j < i; ++j)
                if (input[i] == input[j]) return false;
        }
        std::vector<Consumer> candidate(input.size());
        std::vector<size_t> next(input.size(), input.size());
        for (size_t i = 0; i < input.size(); ++i) {
            uintptr_t link = 0;
            std::memcpy(&link, input[i]->data() + 0x428, sizeof(link));
            if (link) {
                size_t j = 0;
                for (; j < input.size(); ++j)
                    if (link == reinterpret_cast<uintptr_t>(input[j])) break;
                if (j == input.size()) return false;
                next[i] = j;
            }
            std::memcpy(candidate[i].data(), input[i]->data(), 0x18);
            std::memcpy(candidate[i].data() + 0x20,
                        input[i]->data() + 0x18, 0x410);
        }
        for (size_t i = 0; i < input.size(); ++i) {
            size_t cursor = i, steps = 0;
            while (cursor != input.size()) {
                if (++steps > input.size()) return false;
                cursor = next[cursor];
            }
        }
        for (size_t i = 0; i < input.size(); ++i) {
            uintptr_t link = next[i] == input.size() ? 0 :
                reinterpret_cast<uintptr_t>(&candidate[next[i]]);
            std::memcpy(candidate[i].data() + 0x430, &link, sizeof(link));
        }
        native_ = input;
        consumer_.swap(candidate); // preserve allocated addresses in links
        return true;
    }
    const Consumer *consumer(size_t index) const {
        return index < consumer_.size() ? &consumer_[index] : nullptr;
    }
    const Native *native(const Consumer *record) const {
        for (size_t i = 0; i < consumer_.size(); ++i)
            if (record == &consumer_[i]) return native_[i];
        return nullptr;
    }
    void clear() { consumer_.clear(); native_.clear(); }
    TglCapabilityAdapter() = default;
    TglCapabilityAdapter(const TglCapabilityAdapter &) = delete;
    TglCapabilityAdapter &operator=(const TglCapabilityAdapter &) = delete;
private:
    std::vector<const Native *> native_;
    std::vector<Consumer> consumer_;
};

// Exact signed TGL blob observed at image+531550. Only entry 78's CPU
// dependency metadata changes; offsets, lengths and all GPU kernels stay intact.
struct TglKernelMetadataAdapter {
    static constexpr size_t blobBytes = 0xf86ec;
    static constexpr size_t metadataOffset = 0xd30 + 344224;
    static constexpr std::array<uint32_t, 56> pinned = {{
        0xd0,13,13,1,
        1,0x10013,1,0x2001e,1,0x30021,1,0x40022,
        1,0x50004,1,0x60005,1,0x70036,1,0x80051,
        1,0x90052,1,0xa0212,1,0xb0280,1,0xc0281,
        1,6,0,0x10009,0,0x2000a,0,0x3000b,
        0,0x4000c,0,0x50007,0,0x60008,0,0x7000d,
        0,0x8000e,0,0x9000f,0,0xa0010,0,0xb0011,
        0,0xc0012,0,0x169
    }};
    static bool translate(std::vector<uint8_t> &owned) {
        if (owned.size() != blobBytes) return false;
        uint32_t offsets[2]{};
        std::memcpy(offsets, owned.data() + 78 * 4, sizeof(offsets));
        if (offsets[0] != 344224 || offsets[1] != 344448 ||
            std::memcmp(owned.data() + metadataOffset, pinned.data(), sizeof(pinned))) return false;
        auto converted = pinned;
        converted[0] = 0x10000;
        converted[1] = 0;
        converted[2] = 13; // native count of flag=0 records
        converted[3] = 13; // native count of flag=1 records
        unsigned counts[2]{};
        for (size_t i = 4; i < converted.size(); i += 2) {
            const auto flag = pinned[i], descriptor = pinned[i + 1];
            if (flag > 1 || (descriptor & 0xffff) >= 0x34b) return false;
            ++counts[flag];
            converted[i] = descriptor;
            converted[i + 1] = flag;
        }
        if (counts[0] != converted[2] || counts[1] != converted[3]) return false;
        std::memcpy(owned.data() + metadataOffset, converted.data(), sizeof(converted));
        return true;
    }
};
