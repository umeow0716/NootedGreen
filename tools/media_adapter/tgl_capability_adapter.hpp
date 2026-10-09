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
