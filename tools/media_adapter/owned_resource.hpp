#pragma once
#include <array>
#include <cstdint>
#include <utility>
#include <cstring>
#include <limits>
#include <cmath>

// Owned lifecycle only. A Darwin backend must separately prove allocation,
// backing inspection and release ABI; this class never fabricates backing.
// Native evidence: 4e060 reuse/reallocation; 63720 failure -> 63c00 cleanup.
struct TglResourceKey {
    uint32_t width, height, format, tile, compressionMode;
    bool compressed;
    bool operator==(const TglResourceKey &b) const noexcept {
        return width == b.width && height == b.height && format == b.format &&
            tile == b.tile && compressionMode == b.compressionMode && compressed == b.compressed;
    }
};

// Pinned native 62a40 predicate, not a GPU execution/completion guarantee.
inline bool tglNativeBackingPresent(const std::array<uint8_t, 0x148> &s) noexcept {
    uint32_t type = 0;
    uint64_t a = 0, b = 0;
    std::memcpy(&type, s.data() + 0x14, sizeof(type));
    if (type == 0) {
        std::memcpy(&a, s.data() + 0x20, sizeof(a));
        std::memcpy(&b, s.data() + 0x50, sizeof(b));
        return a != 0 || b != 0;
    }
    if (type == 1) {
        std::memcpy(&a, s.data() + 0x58, sizeof(a));
        return a != 0;
    }
    return false;
}

// 637e5..6380d rounds in 64 bits then stores only 32 bits. Require the
// caller's authenticated native page size; do not assume a host page size.
inline bool tglBufferSizeFits(uint32_t bytes, uint64_t pageSize) noexcept {
    if (!bytes || !pageSize || (pageSize & (pageSize - 1)) ||
        pageSize > uint64_t{UINT32_MAX}) return false;
    const uint64_t rounded = (uint64_t{bytes} + pageSize - 1) & ~(pageSize - 1);
    return rounded != 0 && rounded <= UINT32_MAX;
}

// Exact 52bf0 surface-to-command descriptor conversion. The +40 pointer
// borrows shell storage, so shell must outlive every consumer of the result.
// This converts existing state; it does not create backing or validate formats.
inline std::array<uint8_t, 0x48> tglSurfaceDescriptor(
        const std::array<uint8_t, 0x2a8> &shell) noexcept {
    std::array<uint8_t, 0x48> out{};
    const auto get = [&](size_t offset) {
        uint32_t value = 0;
        std::memcpy(&value, shell.data() + offset, sizeof(value));
        return value;
    };
    const auto put = [&](size_t offset, uint32_t value) {
        std::memcpy(out.data() + offset, &value, sizeof(value));
    };
    put(0, 1);
    put(8, get(0x130)); put(0xc, get(0xd8)); put(0x10, get(0xdc));
    put(0x14, get(0xe0)); put(0x18, get(0x13c)); put(0x28, get(0xe4));
    std::memcpy(out.data() + 0x2c, shell.data() + 0x50, 16);
    static_assert(sizeof(uintptr_t) == 8, "pinned x86_64 ABI");
    const uintptr_t resource = reinterpret_cast<uintptr_t>(shell.data() + 0x148);
    std::memcpy(out.data() + 0x40, &resource, sizeof(resource));
    out[4] = shell[0x29a] & 1;
    const uint32_t pitch = get(0xe0);
    if (pitch) put(0x24, (get(0x100) - get(0xf0)) / pitch + get(0x108));
    return out;
}

// 52d60: five optional borrowed shells, no allocation or ownership transfer.
inline std::array<uint8_t, 0x170> tglSurfaceCommandSet(
        const std::array<const std::array<uint8_t, 0x2a8> *, 5> &surfaces,
        uint8_t di) noexcept {
    std::array<uint8_t, 0x170> out{};
    out[0x168] = di & 1;
    for (size_t i = 0; i < surfaces.size(); ++i) {
        if (!surfaces[i]) continue;
        const auto descriptor = tglSurfaceDescriptor(*surfaces[i]);
        std::memcpy(out.data() + i * 0x48, descriptor.data(), descriptor.size());
        if (i < 2) std::memcpy(out.data() + i * 0x48 + 0x20,
                               surfaces[i]->data() + 0xf8, 4);
    }
    out[0x169] = surfaces[1] ? 1 : 0;
    return out;
}

// 1295c0: query selector5 once, preserve its failure and caller outputs.
// query's platform implementation remains separately required; do not import
// ICL's statistics size into the TGL producer.
template<class Query>
int tglStatisticsOffsets(uint32_t width, uint32_t height, uint8_t flag1b88,
                         uint8_t flag1b89, uint32_t &first, uint32_t &second,
                         Query query) noexcept {
    static_assert(noexcept(query(uint32_t{5}, std::declval<uint32_t &>())),
                  "native query must return status without throwing");
    uint32_t size = 0;
    const int status = query(5, size);
    if (status) return status;
    const uint32_t pixels = width * height; // native 32-bit arithmetic
    if (flag1b89 & 1) {
        first = pixels + size;
        second = pixels + 3u * size;
    } else if (flag1b88 & 1) {
        first = pixels;
        second = pixels + size;
    } else {
        first = 0;
        second = size;
    }
    return 0;
}

// Intel Gen12 VeboxQueryStatLayout (2a32c7f), enum values independently
// matched to Darwin's query call. Hardware layout only, no Linux object ABI.
inline int tglStatisticsQuery(uint32_t selector, uint32_t &value) noexcept {
    switch (selector) {
        case 0: value = 0; return 0;
        case 2: value = 0x2c; return 0;
        case 3: value = 0x44; return 0;
        case 5: value = 128; return 0;
        default: return 31;
    }
}

struct TglStatisticsAllocation {
    uint32_t rowBytes = 0, blockRows = 0, totalRows = 0, bytes = 0;
};
// Gen12 allocation: 64-byte rows, ceil(height/4) per-block rows, plus
// ceil(1024/rowBytes) reserved rows. Publication is atomic on validation.
inline int tglStatisticsAllocationSize(uint32_t boundaryWidth, uint32_t boundaryHeight,
                                      TglStatisticsAllocation &out) noexcept {
    if (!boundaryWidth || !boundaryHeight) return 5;
    const uint64_t row = (uint64_t{boundaryWidth} + 63) & ~uint64_t{63};
    const uint64_t blocks = (uint64_t{boundaryHeight} + 3) / 4;
    const uint64_t total = blocks + (1024 + row - 1) / row;
    const uint64_t bytes = row * total;
    if (row > UINT32_MAX || total > UINT32_MAX || bytes > UINT32_MAX) return 5;
    out = {static_cast<uint32_t>(row), static_cast<uint32_t>(blocks),
           static_cast<uint32_t>(total), static_cast<uint32_t>(bytes)};
    return 0;
}

// 170ef0 valid-input boundary contract. Reject malformed float conversions
// and rounding overflow instead of reproducing native truncation hazards.
inline int tglSurfaceBoundary(uint32_t format, uint32_t width, uint32_t height,
                              float sourceWidth, float sourceHeight,
                              bool di, bool alignDi64, uint32_t &outWidth,
                              uint32_t &outHeight) noexcept {
    if (!std::isfinite(sourceWidth) || !std::isfinite(sourceHeight) ||
        sourceWidth < 0 || sourceHeight < 0 ||
        double(sourceWidth) > UINT32_MAX || double(sourceHeight) > UINT32_MAX)
        return 5;
    uint32_t wa = 1, ha = 1;
    if (format == 0x19) { wa = 2; ha = di ? 4 : 2; }
    else if (format >= 0xd && format <= 0x13) { wa = 2; ha = di ? 2 : 1; }
    else if (format == 0x14 || format == 0x15) wa = 2;
    if (di && alignDi64) wa = 64;
    const uint32_t sw = static_cast<uint32_t>(sourceWidth);
    const uint32_t sh = static_cast<uint32_t>(sourceHeight);
    const uint32_t wc = sw > 64 ? sw : 64, hc = sh > 16 ? sh : 16;
    const uint64_t w = (uint64_t{width < wc ? width : wc} + wa - 1) & ~uint64_t{wa - 1};
    const uint64_t h = (uint64_t{height < hc ? height : hc} + ha - 1) & ~uint64_t{ha - 1};
    if (w > UINT32_MAX || h > UINT32_MAX) return 5;
    outWidth = static_cast<uint32_t>(w); outHeight = static_cast<uint32_t>(h);
    return 0;
}

struct TglNativeResourceBinding {
    uintptr_t context = 0, allocate = 0, release = 0;
    // 132ec0..132ed6: renderer stores RenderHal at +2cc0 and borrows
    // RenderHal[0] as OS at +2cc8. Never acquire ownership of either object.
    // Caller must keep RenderHal alive through all resource destruction.
    template<class Read, class Qualify, class Executable>
    bool resolveFromRenderHal(uintptr_t renderHal, uintptr_t image, Read read,
                              Qualify qualify, Executable executable) {
        *this = {};
        uintptr_t os = 0;
        if (!renderHal || renderHal > std::numeric_limits<uintptr_t>::max() - sizeof(os) ||
            !read(renderHal, &os, sizeof(os))) return false;
        return resolve(os, image, read, qualify, executable);
    }
    // Must run under the owner's lifetime lock. Qualify must authenticate the
    // loaded native image; anchors alone are not an image identity check.
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t os, uintptr_t image, Read read, Qualify qualify,
                 Executable executable) {
        *this = {};
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        if (!os || !image || os > max - 0x208 || image > max - 0x63c10 ||
            !qualify(image)) return false;
        uintptr_t alloc = 0, free = 0;
        if (!read(os + 0x1f8, &alloc, sizeof(alloc)) ||
            !read(os + 0x200, &free, sizeof(free)) ||
            alloc != image + 0x63720 || free != image + 0x63c00 ||
            !executable(alloc, 16) || !executable(free, 16)) return false;
        constexpr std::array<uint8_t, 16> allocAnchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x70,
            0x48,0x89,0x7d,0xf0,0x48,0x89,0x75,0xe8};
        constexpr std::array<uint8_t, 16> freeAnchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,
            0x31,0xc0,0x89,0xc1,0x48,0x89,0x7d,0xf8};
        std::array<uint8_t, 16> a{}, f{};
        if (!read(alloc, a.data(), a.size()) || !read(free, f.data(), f.size()) ||
            a != allocAnchor || f != freeAnchor) return false;
        context = os; allocate = alloc; release = free;
        return true;
    }
};

// Injection boundary only: no symbol lookup or unproved OS-object construction.
// Caller must validate native image, entrypoints and context lifetime separately.
class TglNativeResourceBackend {
public:
    using Storage = std::array<uint8_t, 0x148>;
    using Allocate = int (*)(void *, const void *, void *) noexcept;
    using Release = void (*)(void *, void *) noexcept;
    TglNativeResourceBackend(void *context, Allocate allocate, Release release,
                             uint32_t resourceType, uint64_t nativePageSize = 0) noexcept
        : context_(context), allocate_(allocate), release_(release), type_(resourceType),
          pageSize_(nativePageSize) {}
    int allocate(Storage &storage, const TglResourceKey &key) noexcept {
        if (!context_ || !allocate_ || !release_) return 5;
        if (type_ > 1 || key.compressed || key.compressionMode) return 25;
        if (!key.width || !key.height) return 5;
        if (type_ == 0 && !tglBufferSizeFits(key.width, pageSize_)) return 5;
        // Exact known fields only; compression allocation ABI remains unproved.
        alignas(8) std::array<uint8_t, 0x50> params{};
        const auto put = [&](size_t offset, uint32_t value) {
            std::memcpy(params.data() + offset, &value, sizeof(value));
        };
        put(0, type_);
        put(0x14, key.width); // type0: requested byte count; type1: width
        put(0x18, key.height);
        put(0x24, key.tile);
        put(0x28, key.format);
        return allocate_(context_, params.data(), storage.data());
    }
    bool backed(const Storage &storage) const noexcept { return tglNativeBackingPresent(storage); }
    void release(Storage &storage) noexcept {
        if (!context_ || !release_) return;
        for (auto byte : storage) if (byte != 0) {
            release_(context_, storage.data());
            return;
        }
    }
private:
    void *context_;
    Allocate allocate_;
    Release release_;
    uint32_t type_;
    uint64_t pageSize_;
};

template<class Backend> class TglOwnedResource {
public:
    using Storage = std::array<uint8_t, 0x148>;
    static_assert(noexcept(std::declval<Backend &>().allocate(
        std::declval<Storage &>(), std::declval<const TglResourceKey &>())),
        "allocation must report failure through status, not exceptions");
    static_assert(noexcept(std::declval<Backend &>().release(std::declval<Storage &>())),
        "release must not throw during unwind");
    static_assert(noexcept(std::declval<Backend &>().backed(std::declval<const Storage &>())),
        "backing inspection must not throw");
    enum class State { Empty, DescriptorOnly, Backed };
    struct Result { int status; State state; bool changed; };

    explicit TglOwnedResource(Backend &backend) noexcept : backend_(backend) {}
    ~TglOwnedResource() { reset(); }
    TglOwnedResource(const TglOwnedResource &) = delete;
    TglOwnedResource &operator=(const TglOwnedResource &) = delete;
    TglOwnedResource(TglOwnedResource &&) = delete;
    TglOwnedResource &operator=(TglOwnedResource &&) = delete;

    Result ensure(const TglResourceKey &key) noexcept {
        if (live_ && state_ == State::Backed && key_ == key && backend_.backed(storage_))
            return {0, state_, false};
        // Match native ordering: retire old backing before replacement.
        reset();
        live_ = true; // partial allocation must also be released on failure
        const int status = backend_.allocate(storage_, key);
        if (status != 0) {
            reset();
            return {status, State::Empty, false}; // preserve allocation error
        }
        key_ = key;
        state_ = backend_.backed(storage_) ? State::Backed : State::DescriptorOnly;
        return {0, state_, true}; // native success is NOT proof of GPU backing
    }
    void reset() noexcept {
        if (live_) backend_.release(storage_);
        storage_.fill(0);
        key_ = {};
        live_ = false;
        state_ = State::Empty;
    }
    State state() const noexcept { return state_; }
    const Storage &storage() const noexcept { return storage_; }

private:
    Backend &backend_; // must outlive this owner
    alignas(8) Storage storage_{};
    TglResourceKey key_{};
    bool live_ = false;
    State state_ = State::Empty;
};

// One borrowed-OS resource lifetime. The renderer must close every such scope
// before retiring RenderHal/OS. Member order guarantees resource destruction
// while its backend still exists; close is terminal and idempotent.
class TglNativeResourceScope {
public:
    using Owner = TglOwnedResource<TglNativeResourceBackend>;
    TglNativeResourceScope(void *context, TglNativeResourceBackend::Allocate allocate,
                           TglNativeResourceBackend::Release release,
                           uint32_t type, uint64_t nativePageSize = 0) noexcept
        : backend_(context, allocate, release, type, nativePageSize), owner_(backend_) {}
    ~TglNativeResourceScope() { close(); }
    TglNativeResourceScope(const TglNativeResourceScope &) = delete;
    TglNativeResourceScope &operator=(const TglNativeResourceScope &) = delete;
    Owner::Result ensure(const TglResourceKey &key) noexcept {
        if (closed_) return {5, Owner::State::Empty, false};
        return owner_.ensure(key);
    }
    void close() noexcept {
        owner_.reset();
        closed_ = true;
    }
private:
    TglNativeResourceBackend backend_;
    Owner owner_;
    bool closed_ = false;
};
