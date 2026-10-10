#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include "tgl_capability_adapter.hpp"
#include "descriptor_bridge.hpp"
#include "owned_resource.hpp"

static void testOwnedResourceLifecycle() {
    struct Backend {
        int allocations = 0, releases = 0, status = 0;
        bool makeBacking = true;
        int allocate(std::array<uint8_t, 0x148> &s, const TglResourceKey &) noexcept {
            ++allocations;
            for (auto byte : s) assert(byte == 0);
            s[0] = 1; // partially created descriptor even on failure
            s[1] = makeBacking;
            return status;
        }
        bool backed(const std::array<uint8_t, 0x148> &s) const noexcept { return s[1] != 0; }
        void release(std::array<uint8_t, 0x148> &s) noexcept {
            assert(s[0] == 1);
            ++releases;
            // Deliberately leave stale data: the owner must clear it itself.
        }
    } backend;
    using Owner = TglOwnedResource<Backend>;
    const TglResourceKey original{1920, 1080, 0x19, 0, 0, false};
    {
        Owner owner(backend);
        assert(owner.state() == Owner::State::Empty);
        auto r = owner.ensure(original);
        assert(r.status == 0 && r.changed && r.state == Owner::State::Backed);
        r = owner.ensure(original);
        assert(!r.changed && backend.allocations == 1 && backend.releases == 0);
        for (unsigned field = 0; field < 6; ++field) {
            auto key = original;
            switch (field) {
                case 0: ++key.width; break;
                case 1: ++key.height; break;
                case 2: ++key.format; break;
                case 3: ++key.tile; break;
                case 4: ++key.compressionMode; break;
                case 5: key.compressed = true; break;
            }
            r = owner.ensure(key);
            assert(r.status == 0 && r.changed && r.state == Owner::State::Backed);
            r = owner.ensure(original);
            assert(r.changed);
        }
        backend.status = 25;
        auto changed = original;
        ++changed.width;
        const int before = backend.releases;
        r = owner.ensure(changed);
        assert(r.status == 25 && r.state == Owner::State::Empty && !r.changed);
        assert(backend.releases == before + 2); // old + partial replacement
        for (auto byte : owner.storage()) assert(byte == 0);
        owner.reset();
        assert(backend.releases == before + 2); // no double release
        backend.status = 0;
        backend.makeBacking = false;
        r = owner.ensure(original);
        assert(r.status == 0 && r.state == Owner::State::DescriptorOnly && r.changed);
        const int allocated = backend.allocations;
        r = owner.ensure(original);
        assert(r.changed && backend.allocations == allocated + 1); // never reuse absent backing
        backend.makeBacking = true;
        r = owner.ensure(original);
        assert(r.state == Owner::State::Backed);
    }
    assert(backend.allocations == backend.releases);
}

// Offline layout hypothesis only: not an ABI-complete or deployable adapter.
// The pinned consumer sites shift 0x20/0x24/0x228/0x428 by eight bytes.
int main() {
    testOwnedResourceLifecycle();
    std::vector<uint8_t> blob(TglKernelMetadataAdapter::blobBytes, 0x5a);
    const uint32_t offsets[] = {344224, 344448};
    std::memcpy(blob.data() + 78 * 4, offsets, sizeof(offsets));
    std::memcpy(blob.data() + TglKernelMetadataAdapter::metadataOffset,
        TglKernelMetadataAdapter::pinned.data(), sizeof(TglKernelMetadataAdapter::pinned));
    const auto originalBlob = blob;
    assert(TglKernelMetadataAdapter::translate(blob));
    for (size_t i = 0; i < blob.size(); ++i)
        if (i < TglKernelMetadataAdapter::metadataOffset ||
            i >= TglKernelMetadataAdapter::metadataOffset + sizeof(TglKernelMetadataAdapter::pinned))
            assert(blob[i] == originalBlob[i]);
    uint32_t converted[56]{};
    std::memcpy(converted, blob.data() + TglKernelMetadataAdapter::metadataOffset, sizeof(converted));
    assert(converted[0] == 0x10000 && converted[2] == 13 && converted[3] == 13);
    for (unsigned i = 4; i < 56; i += 2) {
        assert(converted[i] == TglKernelMetadataAdapter::pinned[i + 1]);
        assert(converted[i + 1] == TglKernelMetadataAdapter::pinned[i]);
    }
    for (size_t i = 0; i < sizeof(TglKernelMetadataAdapter::pinned); ++i) {
        auto bad = originalBlob;
        bad[TglKernelMetadataAdapter::metadataOffset + i] ^= 1;
        const auto savedBad = bad;
        assert(!TglKernelMetadataAdapter::translate(bad) && bad == savedBad);
    }
    auto badSize = originalBlob;
    badSize.pop_back();
    assert(!TglKernelMetadataAdapter::translate(badSize));
    auto badOffset = originalBlob;
    badOffset[78 * 4] ^= 1;
    assert(!TglKernelMetadataAdapter::translate(badOffset));
    std::array<uint8_t, 0x430> native{};
    for (size_t i = 0; i < native.size(); ++i)
        native[i] = static_cast<uint8_t>((i * 17 + i / 256) & 255);
    const auto original = native;
    std::array<uint8_t, 0x438> consumer{};
    std::memcpy(consumer.data(), native.data(), 0x18);
    std::memcpy(consumer.data() + 0x20, native.data() + 0x18,
                native.size() - 0x18);
    for (size_t offset : {size_t(0x20), size_t(0x24),
                          size_t(0x228), size_t(0x428)}) {
        assert(std::memcmp(native.data() + offset,
                           consumer.data() + offset + 8, 8) == 0);
    }
    assert(std::memcmp(native.data(), consumer.data(), 0x18) == 0);
    assert(native == original);
    TglCapabilityAdapter::Native first{}, second{};
    uintptr_t link = reinterpret_cast<uintptr_t>(&second);
    std::memcpy(first.data() + 0x428, &link, sizeof(link));
    const auto saved = first;
    TglCapabilityAdapter adapter;
    assert(adapter.build({&first, &second}));
    uintptr_t convertedLink = 0;
    std::memcpy(&convertedLink, adapter.consumer(0)->data() + 0x430, 8);
    assert(convertedLink == reinterpret_cast<uintptr_t>(adapter.consumer(1)));
    assert(adapter.native(adapter.consumer(1)) == &second);
    assert(first == saved);
    assert(!adapter.build({&first, &first}));
    assert(!adapter.consumer(0));
    assert(!adapter.build({&first})); // foreign link
    link = reinterpret_cast<uintptr_t>(&first);
    std::memcpy(second.data() + 0x428, &link, 8);
    assert(!adapter.build({&first, &second})); // cycle
    assert(!adapter.build({nullptr}));
    assert(!adapter.build({}));
    second.fill(0);
    std::array<uintptr_t, 35> roots{};
    roots.fill(reinterpret_cast<uintptr_t>(&first));
    // Group-list bytes remain native-owned and opaque to the layout bridge.
    const std::array<uint64_t, 3> groups{{0x11, 0x22, 0x33}};
    AvdDescriptor descriptor{3, 0, reinterpret_cast<uintptr_t>(groups.data()),
                             35, 0, reinterpret_cast<uintptr_t>(roots.data())};
    auto read = [](uintptr_t address, void *out, size_t size) {
        if (!address) return false;
        std::memcpy(out, reinterpret_cast<const void *>(address), size);
        return true;
    };
    AvdDescriptorBridge bridge;
    assert(bridge.build(reinterpret_cast<uintptr_t>(&descriptor), read));
    assert(bridge.descriptor()->groupList == descriptor.groupList);
    assert(bridge.descriptor()->groups == descriptor.groups);
    assert(bridge.descriptor()->count == descriptor.count);
    assert(bridge.descriptor()->reserved == descriptor.reserved);
    assert(bridge.descriptor()->reserved2 == descriptor.reserved2);
    assert((groups == std::array<uint64_t, 3>{{0x11, 0x22, 0x33}}));
    auto convertedRoots = reinterpret_cast<const uintptr_t *>(bridge.descriptor()->roots);
    assert(convertedRoots[0] != roots[0]);
    assert(convertedRoots[0] == convertedRoots[34]);
    assert(first == saved);
    // Native create matches every linked node and requires simultaneous ends.
    // Exercise a decoder -> processing -> scaler topology with shared roots.
    TglCapabilityAdapter::Native decoder{}, processing{}, scaler{};
    auto header = [](auto &record, uint32_t type, uint64_t usage) {
        const uint64_t group = 1001;
        std::memcpy(record.data(), &type, sizeof(type));
        std::memcpy(record.data() + 8, &usage, sizeof(usage));
        std::memcpy(record.data() + 16, &group, sizeof(group));
    };
    header(decoder, 1, 0x200);
    header(processing, 0x10, 1);
    header(scaler, 2, 0x800);
    link = reinterpret_cast<uintptr_t>(&processing);
    std::memcpy(decoder.data() + 0x428, &link, sizeof(link));
    link = reinterpret_cast<uintptr_t>(&scaler);
    std::memcpy(processing.data() + 0x428, &link, sizeof(link));
    const auto savedDecoder = decoder, savedProcessing = processing, savedScaler = scaler;
    roots.fill(reinterpret_cast<uintptr_t>(&decoder));
    roots[1] = reinterpret_cast<uintptr_t>(&processing);
    roots[2] = reinterpret_cast<uintptr_t>(&scaler);
    assert(bridge.build(reinterpret_cast<uintptr_t>(&descriptor), read));
    convertedRoots = reinterpret_cast<const uintptr_t *>(bridge.descriptor()->roots);
    uintptr_t next = 0;
    for (size_t i = 0; i < 3; ++i) {
        const auto *converted = reinterpret_cast<const uint8_t *>(convertedRoots[i]);
        const auto *source = reinterpret_cast<const uint8_t *>(roots[i]);
        assert(std::memcmp(converted, source, 0x18) == 0);
        assert(std::memcmp(converted + 0x20, source + 0x18, 0x410) == 0);
        std::memcpy(&next, converted + 0x430, sizeof(next));
        assert(next == (i == 2 ? 0 : convertedRoots[i + 1]));
    }
    assert(convertedRoots[34] == convertedRoots[0]);
    assert(decoder == savedDecoder && processing == savedProcessing && scaler == savedScaler);
    auto unpublished = [&] {
        const AvdDescriptor empty{};
        assert(std::memcmp(bridge.descriptor(), &empty, sizeof(empty)) == 0);
    };
    descriptor.count = 34;
    assert(!bridge.build(reinterpret_cast<uintptr_t>(&descriptor), read));
    unpublished();
    descriptor.count = 35;
    assert(bridge.build(reinterpret_cast<uintptr_t>(&descriptor), read));
    auto denyRead = [](uintptr_t, void *, size_t) { return false; };
    assert(!bridge.build(reinterpret_cast<uintptr_t>(&descriptor), denyRead));
    unpublished();
    assert(bridge.build(reinterpret_cast<uintptr_t>(&descriptor), read));
    link = reinterpret_cast<uintptr_t>(&decoder);
    std::memcpy(scaler.data() + 0x428, &link, sizeof(link));
    assert(!bridge.build(reinterpret_cast<uintptr_t>(&descriptor), read));
    unpublished();
}
