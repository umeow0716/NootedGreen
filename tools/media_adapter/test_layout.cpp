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

static void testNativeResourceBoundary() {
    using Backend = TglNativeResourceBackend;
    Backend::Storage s{};
    const auto put = [&](size_t offset, auto value) {
        std::memcpy(s.data() + offset, &value, sizeof(value));
    };
    assert(!tglNativeBackingPresent(s));
    put(0x20, uint64_t{1});
    assert(tglNativeBackingPresent(s));
    put(0x20, uint64_t{0});
    put(0x50, uint64_t{1});
    assert(tglNativeBackingPresent(s));
    put(0x14, uint32_t{1});
    assert(!tglNativeBackingPresent(s)); // buffer token is not a surface
    put(0x58, uint64_t{1});
    assert(tglNativeBackingPresent(s));
    for (uint32_t type : {2u, 3u, UINT32_MAX}) {
        put(0x14, type);
        assert(!tglNativeBackingPresent(s));
    }
    struct Fixture {
        int allocations = 0, releases = 0;
        int status = 31;
        bool clearsFailure = false;
        std::array<uint8_t, 0x50> expected{};
    } fixture;
    const auto allocate = +[](void *ctx, const void *params, void *output) noexcept -> int {
        auto &f = *static_cast<Fixture *>(ctx);
        assert(std::memcmp(params, f.expected.data(), f.expected.size()) == 0);
        ++f.allocations;
        uint32_t type = 0;
        std::memcpy(&type, params, sizeof(type));
        std::memcpy(static_cast<uint8_t *>(output) + 0x14, &type, sizeof(type));
        const uint64_t token = 1;
        std::memcpy(static_cast<uint8_t *>(output) + (type == 1 ? 0x58 : 0x20),
                    &token, sizeof(token));
        if (f.status && f.clearsFailure) std::memset(output, 0, 0x148);
        return f.status;
    };
    const auto release = +[](void *ctx, void *output) noexcept {
        ++static_cast<Fixture *>(ctx)->releases;
        std::memset(output, 0, 0x148);
    };
    const TglResourceKey key{4096, 1, 0x3e, 4, 0, false};
    const auto set = [&](size_t offset, uint32_t value) {
        std::memcpy(fixture.expected.data() + offset, &value, sizeof(value));
    };
    set(0x14, key.width); set(0x18, key.height);
    set(0x24, key.tile); set(0x28, key.format);
    assert(tglBufferSizeFits(1, 4096));
    assert(tglBufferSizeFits(0xfffff000u, 4096));
    assert(!tglBufferSizeFits(0xfffff001u, 4096));
    assert(!tglBufferSizeFits(UINT32_MAX, 4096));
    assert(!tglBufferSizeFits(4096, 0));
    assert(!tglBufferSizeFits(4096, 4095));
    assert(!tglBufferSizeFits(4096, uint64_t{1} << 32));
    assert(!tglBufferSizeFits(0, 4096));
    Backend backend(&fixture, allocate, release, 0, 4096);
    TglOwnedResource<Backend> owner(backend);
    auto result = owner.ensure(key);
    assert(result.status == 31 && result.state == decltype(owner)::State::Empty);
    assert(fixture.allocations == 1 && fixture.releases == 1);
    auto oversized = key; oversized.width = UINT32_MAX;
    assert(owner.ensure(oversized).status == 5);
    Backend unknownPage(&fixture, allocate, release, 0);
    TglOwnedResource<Backend> noPage(unknownPage);
    assert(noPage.ensure(key).status == 5);
    assert(fixture.allocations == 1 && fixture.releases == 1);
    auto compressed = key;
    compressed.compressed = true;
    assert(owner.ensure(compressed).status == 25);
    compressed = key; compressed.compressionMode = 1;
    assert(owner.ensure(compressed).status == 25);
    auto zero = key; zero.width = 0;
    assert(owner.ensure(zero).status == 5);
    zero = key; zero.height = 0;
    assert(owner.ensure(zero).status == 5);
    Backend unknown(&fixture, allocate, release, 2);
    TglOwnedResource<Backend> unsupported(unknown);
    assert(unsupported.ensure(key).status == 25);
    Backend missing(&fixture, allocate, nullptr, 0);
    TglOwnedResource<Backend> noRelease(missing);
    assert(noRelease.ensure(key).status == 5);
    assert(fixture.allocations == 1 && fixture.releases == 1);
    fixture.clearsFailure = true;
    assert(owner.ensure(key).status == 31);
    assert(fixture.allocations == 2 && fixture.releases == 1); // native already cleaned up
    fixture.clearsFailure = false;
    fixture.status = 0;
    const TglResourceKey surfaceKey{1920, 1080, 0x19, 2, 0, false};
    set(0, 1); set(0x14, surfaceKey.width); set(0x18, surfaceKey.height);
    set(0x24, surfaceKey.tile); set(0x28, surfaceKey.format);
    {
        Backend surfaceBackend(&fixture, allocate, release, 1);
        TglOwnedResource<Backend> surface(surfaceBackend);
        assert(surface.ensure(surfaceKey).state == decltype(surface)::State::Backed);
        assert(!surface.ensure(surfaceKey).changed);
    }
    assert(fixture.allocations == 3 && fixture.releases == 2);
    {
        TglNativeResourceScope scope(&fixture, allocate, release, 1);
        assert(scope.ensure(surfaceKey).status == 0);
        scope.close();
        assert(fixture.releases == 3);
        scope.close();
        assert(scope.ensure(surfaceKey).status == 5);
        assert(fixture.allocations == 4 && fixture.releases == 3);
    }
    assert(fixture.releases == 3); // closed destructor cannot double-release
    {
        TglNativeResourceScope scope(&fixture, allocate, release, 1);
        assert(scope.ensure(surfaceKey).status == 0);
    }
    assert(fixture.allocations == 5 && fixture.releases == 4);
}

static void testResourceBinding() {
    constexpr uintptr_t os = 0x1000, image = 0x100000, renderHal = 0x2000;
    uintptr_t borrowedOs = os;
    uintptr_t allocate = image + 0x63720, release = image + 0x63c00;
    std::array<uint8_t, 16> allocAnchor{
        0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x70,
        0x48,0x89,0x7d,0xf0,0x48,0x89,0x75,0xe8};
    std::array<uint8_t, 16> freeAnchor{
        0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,
        0x31,0xc0,0x89,0xc1,0x48,0x89,0x7d,0xf8};
    bool identity = true, rx = true, readable = true;
    const auto read = [&](uintptr_t p, void *out, size_t n) {
        if (!readable) return false;
        const void *source = nullptr;
        if (p == renderHal && n == sizeof(borrowedOs)) source = &borrowedOs;
        if (p == os + 0x1f8 && n == sizeof(allocate)) source = &allocate;
        if (p == os + 0x200 && n == sizeof(release)) source = &release;
        if (p == image + 0x63720 && n == allocAnchor.size()) source = allocAnchor.data();
        if (p == image + 0x63c00 && n == freeAnchor.size()) source = freeAnchor.data();
        if (!source) return false;
        std::memcpy(out, source, n);
        return true;
    };
    const auto qualify = [&](uintptr_t p) { return identity && p == image; };
    const auto executable = [&](uintptr_t p, size_t n) {
        return rx && n == 16 && (p == image + 0x63720 || p == image + 0x63c00);
    };
    TglNativeResourceBinding binding;
    const auto resolve = [&] { return binding.resolve(os, image, read, qualify, executable); };
    const auto empty = [&] {
        assert(!binding.context && !binding.allocate && !binding.release);
    };
    assert(resolve() && binding.context == os && binding.allocate == allocate && binding.release == release);
    identity = false; assert(!resolve()); empty(); identity = true;
    assert(resolve()); rx = false; assert(!resolve()); empty(); rx = true;
    assert(resolve()); readable = false; assert(!resolve()); empty(); readable = true;
    ++allocate; assert(!resolve()); empty(); --allocate;
    ++release; assert(!resolve()); empty(); --release;
    allocAnchor[15] ^= 1; assert(!resolve()); empty(); allocAnchor[15] ^= 1;
    freeAnchor[15] ^= 1; assert(!resolve()); empty(); freeAnchor[15] ^= 1;
    assert(resolve());
    assert(!binding.resolve(UINTPTR_MAX, image, read, qualify, executable)); empty();
    assert(!binding.resolve(os, UINTPTR_MAX, read, qualify, executable)); empty();
    assert(!binding.resolve(0, image, read, qualify, executable)); empty();
    assert(!binding.resolve(os, 0, read, qualify, executable)); empty();
    const auto borrowed = [&] {
        return binding.resolveFromRenderHal(renderHal, image, read, qualify, executable);
    };
    assert(borrowed() && binding.context == os);
    borrowedOs = 0; assert(!borrowed()); empty(); borrowedOs = os;
    assert(borrowed()); readable = false; assert(!borrowed()); empty(); readable = true;
    assert(!binding.resolveFromRenderHal(0, image, read, qualify, executable)); empty();
    assert(!binding.resolveFromRenderHal(UINTPTR_MAX, image, read, qualify, executable)); empty();
    identity = false; assert(!borrowed()); empty();
}

// Offline layout hypothesis only: not an ABI-complete or deployable adapter.
// The pinned consumer sites shift 0x20/0x24/0x228/0x428 by eight bytes.
int main() {
    testOwnedResourceLifecycle();
    testNativeResourceBoundary();
    testResourceBinding();
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
