#pragma once
#include "tgl_capability_adapter.hpp"

struct AvdDescriptor {
    uint32_t groups, reserved;
    uintptr_t groupList;
    uint32_t count, reserved2;
    uintptr_t roots;
};
struct AvdMetadata {
    uint32_t renderer, reserved;
    uintptr_t descriptor;
    uint64_t version, reserved2;
};
static_assert(sizeof(AvdDescriptor) == 0x20 && sizeof(AvdMetadata) == 0x20);

// Native factory retains ownership of all borrowed memory until native destroy.
class AvdDescriptorBridge {
public:
    template<class Read> bool build(uintptr_t address, Read read) {
        // Invalidate the previous publication before any fallible rebuild.
        // adapter_.build may release its old consumers even when it fails.
        descriptor_ = {};
        roots_.fill(0);
        adapter_.clear();
        AvdDescriptor source{};
        std::array<uintptr_t, 35> roots{};
        if (!read(address, &source, sizeof(source)) || source.groups != 3 ||
            source.count != roots.size() ||
            !read(source.roots, roots.data(), sizeof(roots))) return false;
        std::vector<uintptr_t> addresses;
        std::vector<const TglCapabilityAdapter::Native *> records;
        for (uintptr_t root : roots) {
            uintptr_t cursor = root;
            size_t hops = 0;
            while (cursor) {
                if (++hops > 85) return false;
                bool seen = false;
                for (uintptr_t old : addresses) if (old == cursor) seen = true;
                if (seen) break;
                if (addresses.size() == 85) return false;
                TglCapabilityAdapter::Native snapshot{};
                if (!read(cursor, snapshot.data(), snapshot.size())) return false;
                addresses.push_back(cursor);
                records.push_back(reinterpret_cast<const TglCapabilityAdapter::Native *>(cursor));
                std::memcpy(&cursor, snapshot.data() + 0x428, sizeof(cursor));
            }
        }
        if (!adapter_.build(records)) return false;
        for (size_t i = 0; i < roots.size(); ++i) {
            roots_[i] = 0;
            if (!roots[i]) continue;
            size_t j = 0;
            for (; j < addresses.size(); ++j) if (roots[i] == addresses[j]) break;
            if (j == addresses.size()) return false;
            roots_[i] = reinterpret_cast<uintptr_t>(adapter_.consumer(j));
        }
        descriptor_ = source;
        descriptor_.roots = reinterpret_cast<uintptr_t>(roots_.data());
        return true;
    }
    const AvdDescriptor *descriptor() const { return &descriptor_; }
    AvdDescriptorBridge() = default;
    AvdDescriptorBridge(const AvdDescriptorBridge &) = delete;
    AvdDescriptorBridge &operator=(const AvdDescriptorBridge &) = delete;
private:
    AvdDescriptor descriptor_{};
    std::array<uintptr_t, 35> roots_{};
    TglCapabilityAdapter adapter_;
};
