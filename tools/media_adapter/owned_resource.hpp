#pragma once
#include <array>
#include <cstdint>
#include <utility>
#include <cstring>
#include <limits>
#include <cmath>
#include <cstddef>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

// Gen12 g12_base.cpp242 + MOS IS_PA_FORMAT, not DI's exclusion-only rule.
// Preserve the explicit generic Format_PA=-7 enum; do not truncate unknowns.
constexpr bool tglVeboxFormatSupported(uint32_t format) {
    if (format >= 0x0d && format <= 0x15) return true;
    switch (format) {
        case uint32_t(-7): case 0x17: case 0x19:
        case 0x52: case 0x53: case 0x58: case 0x59:
        case 0x4a: case 0x4c: case 0x4d: return true;
        default: return false;
    }
}

// Intel Gen12 IsDnFormatSupported (pinned g12_base.cpp329), with Darwin format
// values independently pinned by Tahoe ICL1fc530. Pure format predicate only:
// does not assert that DN is requested, eligible, allocated or GPU-complete.
constexpr bool tglVeboxDnFormatSupported(uint32_t format) {
    switch (format) {
        case 0x0e: case 0x11: case 0x0f: case 0x10: case 0x0d:
        case 0x4a: case 0x19: case 0x15: case 0x12: case 0x13:
        case 0x14: case 0x17: case 0x59: case 0x53: case 0x52:
        case 0x03: case 0x05: return true;
        default: return false;
    }
}
template<class Read, class Predicate>
bool tglVeboxSurfaceFormatPredicate(uintptr_t surface, Read read, Predicate predicate) {
    uint32_t format = 0;
    return surface && surface <= std::numeric_limits<uintptr_t>::max() - 0x133 &&
        read(surface + 0x130, &format, sizeof(format)) && predicate(format);
}
template<class Read>
bool tglVeboxSurfaceSupported(uintptr_t surface, Read read) {
    return tglVeboxSurfaceFormatPredicate(surface, read, tglVeboxFormatSupported);
}
// Gen12 IsRTFormatSupported: generic YUV/monochrome output, or the intersection
// of IS_RGB32_FORMAT and IS_RGB_NO_SWAP for BT2020 P010/P016 input.
// Darwin ICL1fc360 independently pins the two-surface ABI, ColorSpace at +0
// (BT2020=0xb), and Format at +0x130; do not reuse its older output whitelist.
constexpr bool tglVeboxRtFormatSupported(uint32_t sourceColorSpace,
                                       uint32_t sourceFormat, uint32_t targetFormat) {
    return tglVeboxFormatSupported(targetFormat) ||
        (sourceColorSpace == 0x0b && (sourceFormat == 0x53 || sourceFormat == 0x52) &&
         (targetFormat == 0x03 || targetFormat == 0x04 || targetFormat == 0x50));
}
template<class Read>
bool tglVeboxRtSurfaceSupported(uintptr_t source, uintptr_t target, Read read) {
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    if (!source || !target || source > max - 0x133 || target > max - 0x133) return false;
    uint32_t targetFormat = 0, sourceFormat = 0, sourceColorSpace = 0;
    if (!read(target + 0x130, &targetFormat, sizeof(targetFormat))) return false;
    if (tglVeboxFormatSupported(targetFormat)) return true;
    return read(source, &sourceColorSpace, sizeof(sourceColorSpace)) &&
        read(source + 0x130, &sourceFormat, sizeof(sourceFormat)) &&
        tglVeboxRtFormatSupported(sourceColorSpace, sourceFormat, targetFormat);
}
template<class Read>
bool tglVeboxDnSurfaceSupported(uintptr_t surface, Read read) {
    return tglVeboxSurfaceFormatPredicate(surface, read, tglVeboxDnFormatSupported);
}
// Gen12 g12_base.cpp370, NOT the shorter ICL1fc690 exclusion table.
// MOS_FORMAT values: AYUV15/Y41614/Y41017/ABGR3/ARGB1/B10..51/R10..50/
// ABGR64=5/ARGB64=6. This is the DI predicate only; generic format admission
// remains mandatory (a format not excluded here is NOT automatically usable).
constexpr bool tglVeboxDiFormatSupported(uint32_t format) {
    switch (format) {
        case 0x15: case 0x14: case 0x17: case 0x03: case 0x01:
        case 0x51: case 0x50: case 0x05: case 0x06: return false;
        default: return true;
    }
}
template<class Read>
bool tglVeboxDiSurfaceSupported(uintptr_t surface, Read read) {
    return tglVeboxSurfaceFormatPredicate(surface, read, tglVeboxDiFormatSupported);
}

struct TglSurfaceRect {
    float left, top, right, bottom;
    bool finite() const {
        return std::isfinite(left) && std::isfinite(top) &&
            std::isfinite(right) && std::isfinite(bottom);
    }
};
// Geometry portion of Gen12 IS_OUTPUT_PIPE_VEBOX_FEASIBLE. Darwin uses float
// rectangles at surface+30/+40/+50, not Linux integer RECT storage. Do not
// copy ICL1fb2ec's max-source SIZE EQUALITY in place of Gen12 containment.
// This alone is NOT direct-pipe admission: feature/format/CSC/alpha gates remain.
inline bool tglDirectVeboxGeometry(const TglSurfaceRect& source,
                                   const TglSurfaceRect& destination,
                                   const TglSurfaceRect& maximumSource,
                                   const TglSurfaceRect& targetDestination) {
    if (!source.finite() || !destination.finite() || !maximumSource.finite() ||
        !targetDestination.finite()) return false;
    auto sameSize = [](const TglSurfaceRect& a, const TglSurfaceRect& b) {
        const float aw = a.right - a.left, ah = a.bottom - a.top;
        const float bw = b.right - b.left, bh = b.bottom - b.top;
        return std::isfinite(aw) && std::isfinite(ah) && std::isfinite(bw) &&
            std::isfinite(bh) && aw == bw && ah == bh;
    };
    return sameSize(source, destination) && sameSize(destination, targetDestination) &&
        maximumSource.left <= source.left && maximumSource.top <= source.top &&
        maximumSource.right >= source.right && maximumSource.bottom >= source.bottom &&
        source.top == 0 && source.left == 0 && destination.top == 0 && destination.left == 0;
}

template<class Read>
bool tglDirectVeboxSurfaceGeometry(uintptr_t source, uintptr_t target, Read read) {
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    if (!source || !target || source > max - 0x5f || target > max - 0x4f) return false;
    static_assert(sizeof(TglSurfaceRect) == 16, "Darwin float rectangle ABI");
    TglSurfaceRect src{}, dst{}, maximum{}, targetDst{};
    return read(source + 0x30, &src, sizeof(src)) &&
        read(source + 0x40, &dst, sizeof(dst)) &&
        read(source + 0x50, &maximum, sizeof(maximum)) &&
        read(target + 0x40, &targetDst, sizeof(targetDst)) &&
        tglDirectVeboxGeometry(src, dst, maximum, targetDst);
}

// Gen12 IS_COMP_BYPASS_FEASIBLE; this is only the first output-pipe gate, not
// VEBOX/SFC admission. Darwin field reads pinned by ICL1fb16b..1fb205.
template<class Read>
bool tglCompositionBypassFeasible(uintptr_t params, uintptr_t source,
                                  uintptr_t pass, Read read) {
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    if (!params || !source || !pass || params > max - 0xdf || source > max - 0x82)
        return false;
    uint32_t sources = 0, targets = 0;
    uintptr_t blending = 0, lumaKey = 0, constriction = 0;
    uint8_t compNeeded = 0, interlaced = 0, weaving = 0;
    return read(pass, &compNeeded, 1) && !(compNeeded & 1) &&
        read(params, &sources, 4) && sources == 1 &&
        read(params + 0x90, &targets, 4) && targets == 1 &&
        read(source + 0x60, &blending, 8) && !blending &&
        read(source + 0x81, &interlaced, 1) && !(interlaced & 1) &&
        read(source + 0x82, &weaving, 1) && !(weaving & 1) &&
        read(source + 0x68, &lumaKey, 8) && !lumaKey &&
        read(params + 0xd8, &constriction, 8) && !constriction;
}

// Complete Gen12 direct-output feasibility after composition bypass. Caller
// owns the pass lock/image lease. queryTwoPass must call the authenticated TGL
// native CSC predicate, returning false if binding/read fails (not "no CSC").
// No mode/state is published here; SFC and composition remain separate branches.
template<class Read, class QueryTwoPass>
bool tglDirectVeboxFeasible(uintptr_t params, uintptr_t source, uintptr_t target,
                           uint32_t bypassMode, Read read, QueryTwoPass queryTwoPass) {
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    if (!bypassMode || !params || !source || !target || params > max - 0xf7 ||
        source > max - 0x293 || target > max - 0x133) return false;
    uint32_t targets = 0, sample = 0, rotation = 0, alphaMode = 0;
    uintptr_t actualTarget = 0, ief = 0, alpha = 0;
    uint8_t variance = 0;
    if (!read(params + 0x90, &targets, 4) || targets != 1 ||
        !read(params + 0x98, &actualTarget, 8) || actualTarget != target ||
        !tglDirectVeboxSurfaceGeometry(source, target, read) ||
        !read(source + 0x78, &ief, 8) || ief || // Gen12 requires NULL, not ICL disabled IEF
        !read(source + 0x138, &sample, 4) || sample != 0 ||
        !read(source + 0x290, &rotation, 4) || rotation != 0 ||
        !read(source + 0x83, &variance, 1) || (variance & 1) ||
        !tglVeboxSurfaceSupported(source, read) ||
        !tglVeboxRtSurfaceSupported(source, target, read)) return false;
    bool needsTwoPass = true;
    if (!queryTwoPass(source, target, needsTwoPass) || needsTwoPass) return false;
    if (!read(params + 0xf0, &alpha, 8)) return false;
    return !alpha || (alpha <= max - 7 && read(alpha + 4, &alphaMode, 4) && alphaMode != 2);
}

// Native CPU shell allocator437e0 zeroes storage and increments7f3218 only on
// success; release436d0 decrements the SAME counter then frees. Not GPU backing.
struct TglTrackedCpuBinding {
    uintptr_t image = 0, allocate = 0, release = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t base, Read read, Qualify qualify, Executable executable) {
        image = allocate = release = 0;
        constexpr std::array<uint8_t,16> allocBytes{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,
            0x48,0x89,0x7d,0xf8,0x48,0x8b,0x7d,0xf8};
        constexpr std::array<uint8_t,16> freeBytes{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,
            0x48,0x89,0x7d,0xf8,0x48,0x83,0x7d,0xf8};
        std::array<uint8_t,16> a{}, f{};
        if (!base || base > std::numeric_limits<uintptr_t>::max() - 0x757e80 ||
            !qualify(base) || !executable(base+0x437e0,16) || !executable(base+0x436d0,16) ||
            !read(base+0x437e0,a.data(),16) || a != allocBytes ||
            !read(base+0x436d0,f.data(),16) || f != freeBytes) return false;
        image = base; allocate = base+0x437e0; release = base+0x436d0; return true;
    }
};

// Invoker ABI: uintptr_t allocate(entry,size_t), void release(entry,pointer).
// Caller retains the loaded image lease until backend and all owners are gone.
struct TglNativeTrackedCpuInvoker {
    uintptr_t allocate(uintptr_t entry, size_t bytes) const {
        if (!entry || !bytes) return 0;
        using Allocate = void* (*)(size_t);
        return reinterpret_cast<uintptr_t>(reinterpret_cast<Allocate>(entry)(bytes));
    }
    void release(uintptr_t entry, uintptr_t pointer) const noexcept {
        if (!entry || !pointer) return;
        using Release = void (*)(void*);
        reinterpret_cast<Release>(entry)(reinterpret_cast<void*>(pointer));
    }
};

template<class Invoker> class TglTrackedCpuBackend {
    TglTrackedCpuBinding binding;
    Invoker invoke;
public:
    template<class Read, class Qualify, class Executable>
    TglTrackedCpuBackend(uintptr_t image, Read read, Qualify qualify,
                         Executable executable, Invoker invoker) : invoke(std::move(invoker)) {
        binding.resolve(image,read,qualify,executable);
    }
    bool bound() const { return binding.image != 0; }
    uintptr_t allocate(size_t bytes) {
        return bound() && bytes ? invoke.allocate(binding.allocate,bytes) : 0;
    }
    void release(uintptr_t pointer) noexcept {
        if (bound() && pointer) invoke.release(binding.release,pointer);
    }
};

// Backend must own the authenticated binding/image lease, call native paired
// allocate(size_t)/release(pointer), and outlive this non-transferable owner.
// Never adopt arbitrary pointers: that would corrupt native allocation accounting.
template<class Backend> class TglTrackedCpuOwner {
    Backend& backend;
    uintptr_t storage = 0;
    size_t extent = 0;
public:
    explicit TglTrackedCpuOwner(Backend& b) : backend(b) {}
    TglTrackedCpuOwner(const TglTrackedCpuOwner&) = delete;
    TglTrackedCpuOwner& operator=(const TglTrackedCpuOwner&) = delete;
    ~TglTrackedCpuOwner() { reset(); }
    bool allocate(size_t bytes) {
        if (storage || !bytes) return false;
        auto candidate = backend.allocate(bytes);
        if (!candidate) return false;
        storage = candidate; extent = bytes; return true;
    }
    void reset() noexcept {
        if (!storage) return;
        const auto old = storage; storage = 0; extent = 0;
        backend.release(old);
    }
    uintptr_t get() const { return storage; }
    size_t size() const { return extent; }
};

// Native Allocate126740 creates eight 0x2a8 CPU shells: 1b0/1b8,
// ba8[2],680[4]. This owned transaction publishes none until all exist.
// It does not install child fields or allocate GPU backing. Backend outlives it.
template<class Backend> class TglVeboxCpuShells {
    Backend& backend;
    std::array<uintptr_t,8> shells{};
public:
    explicit TglVeboxCpuShells(Backend& b) : backend(b) {}
    TglVeboxCpuShells(const TglVeboxCpuShells&) = delete;
    TglVeboxCpuShells& operator=(const TglVeboxCpuShells&) = delete;
    ~TglVeboxCpuShells() { reset(); }
    bool allocate() {
        if (shells[0]) return false;
        std::array<uintptr_t,8> candidate{};
        for (size_t i = 0; i < candidate.size(); ++i) {
            candidate[i] = backend.allocate(0x2a8);
            if (!candidate[i]) {
                while (i) backend.release(candidate[--i]);
                return false;
            }
        }
        shells = candidate;
        return true;
    }
    uintptr_t get(size_t i) const { return i < shells.size() ? shells[i] : 0; }
    // Private native-base storage only, after successful native construction.
    // No observer may access it during installation. Validate every field first,
    // then publish all pointers and revoke this owner's release authority.
    template<size_t N>
    bool installIntoPrivateBase(std::array<uint8_t,N>& storage) noexcept {
        if (N<0x1bc0) return false;
        constexpr size_t offsets[8]={0x1b0,0x1b8,0xba8,0xbb0,0x680,0x688,0x690,0x698};
        for (size_t i=0;i<8;++i) {
            uintptr_t existing=0;
            std::memcpy(&existing,storage.data()+offsets[i],8);
            if (existing || !shells[i]) return false;
        }
        for (size_t i=0;i<8;++i)
            std::memcpy(storage.data()+offsets[i],&shells[i],8);
        shells.fill(0);
        return true;
    }
    // Only after installing ALL eight fields into a private constructed child:
    // native non-deleting destructor12de30 then owns their paired436d0 frees.
    // Never retain ownership here as well, or native teardown double-frees them.
    // Caller must hold the child/image lease and handle all returned ownership.
    std::array<uintptr_t,8> relinquishToNative() noexcept {
        const auto transferred = shells;
        shells.fill(0);
        return transferred;
    }
    void reset() noexcept {
        const auto old = shells;
        shells.fill(0);
        for (size_t i = old.size(); i; --i)
            if (old[i-1]) backend.release(old[i-1]);
    }
};

// Native destructor12dfc8 uses RenderHal+a30, NOT OS+a30. The borrowed
// RenderHal and image lease must remain live through the last batch release.
// Qualification is supplied by the owner that authenticated the parent link.
struct TglBatchReleaseBinding {
    uintptr_t renderHal = 0, entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t object, uintptr_t image, Read read,
                 Qualify qualify, Executable executable) {
        renderHal = entry = 0;
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        uintptr_t callback = 0;
        if (!object || object > max - 0xa37 || !image || image > max - 0x23e03e ||
            !qualify(object,image) || !read(object+0xa30,&callback,8) ||
            callback < image+0x1160 || callback > image + 0x23e03e - 16 ||
            !executable(callback,16)) return false;
        renderHal = object; entry = callback; return true;
    }
};

// Native MOS_STATUS result is preserved; caller owns batch descriptor/backing
// and decides teardown policy. This alone neither allocates nor owns a buffer.
struct TglNativeBatchReleaseInvoker {
    uint32_t operator()(uintptr_t entry, uintptr_t renderHal, uintptr_t batch) const {
        using Release = uint32_t (*)(void*,void*);
        return reinterpret_cast<Release>(entry)(reinterpret_cast<void*>(renderHal),
                                                reinterpret_cast<void*>(batch));
    }
};

// Full VEBOX table, NOT a live child/object replacement. Owned constructor and
// destructor implementations must exist before this table is installed anywhere.
// +1e8 belongs to the following execution table's metadata and MUST NOT be copied.
struct TglOwnedVeboxVtable {
    static constexpr size_t count = 0x1e8 / 8;
    static constexpr std::array<size_t,17> missing{
        0x28,0x58,0x60,0x68,0x70,0x78,0x80,0x90,0x98,0xa0,0xa8,0xb0,0xb8,
        0x1c8,0x1d0,0x1d8,0x1e0};
    // Lifecycle slots cannot inherit native delete: owned resources/table need
    // teardown. +1b8 must implement Gen12 TRUE rather than base FALSE.
    static constexpr std::array<size_t,20> required{
        0,8,0x28,0x58,0x60,0x68,0x70,0x78,0x80,0x90,0x98,0xa0,0xa8,0xb0,0xb8,
        0x1b8,0x1c8,0x1d0,0x1d8,0x1e0};
    // Preserve the primary native-base ABI metadata. This does not invent
    // derived RTTI or prove a constructed object; owners must retain its lease.
    intptr_t offsetToTop = 0;
    uintptr_t typeInfo = 0;
    std::array<uintptr_t,count> entries{};
    uintptr_t addressPoint() const noexcept {
        return typeInfo ? reinterpret_cast<uintptr_t>(entries.data()) : 0;
    }
    template<class Read, class Qualify, class Executable>
    bool build(uintptr_t image, const std::array<uintptr_t,20>& owned,
               Read read, Qualify qualify, Executable executable) {
        offsetToTop = 0; typeInfo = 0; entries = {};
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        if (!image || image > max - 0x757ed0 || !qualify(image)) return false;
        std::array<uintptr_t,2> metadata{};
        if (!read(image+0x757c88,metadata.data(),sizeof(metadata)) ||
            metadata[0] != 0 || metadata[1] != image+0x757ed0) return false;
        std::array<uintptr_t,count> candidate{};
        if (!read(image + 0x757c98, candidate.data(), sizeof(candidate))) return false;
        for (size_t i = 0; i < count; ++i) {
            bool mustBeNull = false;
            for (size_t offset : missing) mustBeNull |= i * 8 == offset;
            if (mustBeNull != (candidate[i] == 0)) return false;
            if (candidate[i] && (candidate[i] < image+0x1160 ||
                candidate[i] - image >= 0x23e03f || !executable(candidate[i], 1))) return false;
        }
        if (candidate[0] != image+0x12e240 || candidate[1] != image+0x12e250 ||
            candidate[0x1b8/8] != image+0x71e80) return false;
        for (size_t i = 0; i < required.size(); ++i) {
            // Owned replacements are trusted compiled addresses, not pointers
            // discovered in an object. Never substitute another native callback.
            if (!owned[i] || (owned[i] >= image && owned[i] - image < 0x23e03f) ||
                !executable(owned[i], 1)) return false;
            candidate[required[i]/8] = owned[i];
        }
        entries = candidate; typeInfo = metadata[1]; return true;
    }
};
static_assert(offsetof(TglOwnedVeboxVtable, entries) == 16);
static_assert(sizeof(TglOwnedVeboxVtable) == 16 + 0x1e8);

// Authenticated native CSC entry. This binds code only: invocation also requires
// a fully constructed owned child with native execution and Gen12 overrides.
struct TglTwoPassCscBinding {
    uintptr_t image = 0, entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t base, Read read, Qualify qualify, Executable executable) {
        image = entry = 0;
        constexpr std::array<uint8_t,16> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,
            0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0};
        std::array<uint8_t,16> bytes{};
        if (!base || base > std::numeric_limits<uintptr_t>::max() - 0x757fd0 ||
            !qualify(base) || !executable(base + 0x12cfc0, anchor.size()) ||
            !read(base + 0x12cfc0, bytes.data(), bytes.size()) || bytes != anchor) return false;
        image = base; entry = base + 0x12cfc0; return true;
    }
};

// Read-only shape gate for an already-owned native execution object. Identity
// qualification must authenticate the loaded image; this is NOT proof that
// mode/state producers or GPU commands are valid. Run under owner's lock.
template<class Read, class Qualify>
bool tglExecutionShape(uintptr_t object, uintptr_t image, Read read, Qualify qualify) {
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    if (!object || !image || object > max - 0xd48 || image > max - 0x757fd0 ||
        !qualify(image)) return false;
    uintptr_t vt = 0, first = 0, second = 0;
    if (!read(object, &vt, 8) || vt != image + 0x757e90 ||
        !read(object + 0xd38, &first, 8) || !read(object + 0xd40, &second, 8) ||
        !first || !second || first == second || first == object || second == object ||
        first > max - 0x20 || second > max - 0x48) return false;
    uintptr_t firstVt = 0, secondVt = 0;
    return read(first, &firstVt, 8) && firstVt == image + 0x757f78 &&
           read(second, &secondVt, 8) && secondVt == image + 0x757fb8;
}

// trustedVtable/genericHook/cscSupportHook are addresses in our owned subclass,
// never values learned from an untrusted native object. Hold its owner lock and
// image lease throughout. Invoke ABI: bool entry(child, source, target).
template<class Read, class Qualify, class Executable, class Invoke>
bool tglQueryNativeTwoPassCsc(const TglTwoPassCscBinding& binding,
                            uintptr_t child, uintptr_t source, uintptr_t target,
                            uintptr_t trustedVtable, uintptr_t genericHook,
                            uintptr_t cscSupportHook, bool& needed,
                            Read read, Qualify qualify, Executable executable, Invoke invoke) {
    needed = true; // failed query must not admit direct output
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    if (!child || child > max - 0x8f || !source || !target ||
        source > max - 0x133 || target > max - 3 ||
        !trustedVtable || trustedVtable > max - 0x1bf || !genericHook || !cscSupportHook ||
        genericHook == cscSupportHook || !binding.image ||
        binding.image > max - 0x757fd0 || binding.entry != binding.image + 0x12cfc0 ||
        trustedVtable == binding.image + 0x757c98) return false;
    TglTwoPassCscBinding verified;
    if (!verified.resolve(binding.image, read, qualify, executable)) return false;
    uintptr_t vt = 0, getter = 0, generic = 0, support = 0, execution = 0;
    uint32_t sourceColor = 0, targetColor = 0;
    if (!read(child, &vt, 8) || vt != trustedVtable ||
        !read(vt + 0x48, &getter, 8) || getter != binding.image + 0x12ea30 ||
        !read(vt + 0x90, &generic, 8) || generic != genericHook ||
        !read(vt + 0x1b8, &support, 8) || support != cscSupportHook ||
        !executable(genericHook, 1) || !executable(cscSupportHook, 1) ||
        !read(child + 0x88, &execution, 8) ||
        !tglExecutionShape(execution, binding.image, read, qualify) ||
        !read(source, &sourceColor, 4) || !read(target, &targetColor, 4)) return false;
    needed = invoke(verified.entry, child, source, target);
    return true;
}

struct TglExecutionBinding {
    uintptr_t image = 0, create = 0, initialize = 0, destroy = 0;
    // Caller retains the authenticated image for the entire owner lifetime.
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t base, Read read, Qualify qualify, Executable executable) {
        *this = {};
        constexpr std::array<uintptr_t, 3> offsets{0x12ebc0, 0x12e420, 0x12e3f0};
        constexpr std::array<std::array<uint8_t, 16>, 3> anchors{{
            {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,0x48,0x8b,0x35,0x59,0x54,0x62,0x00,0xbf},
            {0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xb0,0x09,0x00,0x00,0x48,0x8b,0x05,0x56,0x5c},
            {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8}
        }};
        if (!base || base > std::numeric_limits<uintptr_t>::max() - 0x757fd0 ||
            !qualify(base)) return false;
        for (size_t i = 0; i < offsets.size(); ++i) {
            std::array<uint8_t, 16> bytes{};
            if (!executable(base + offsets[i], bytes.size()) ||
                !read(base + offsets[i], bytes.data(), bytes.size()) || bytes != anchors[i])
                return false;
        }
        image = base;
        create = base + offsets[0]; initialize = base + offsets[1]; destroy = base + offsets[2];
        return true;
    }
};

// Invoker is injected for offline tests. Native invoker must use the exact
// x86_64 signatures: uintptr_t factory(), int Init(uintptr_t), void delete(uintptr_t).
// Binding/image lease must remain valid until every owned object is destroyed.
template<class Read, class Qualify, class Invoker> class TglExecutionBackend {
    TglExecutionBinding binding;
    Read read;
    Qualify qualify;
    Invoker invoke;
public:
    template<class Executable>
    TglExecutionBackend(TglExecutionBinding b, Read r, Qualify q, Invoker i, Executable rx)
        : binding(b), read(r), qualify(q), invoke(i) {
        TglExecutionBinding verified;
        if (!bound() || !verified.resolve(b.image, read, qualify, rx)) binding = {};
        else binding = verified;
    }
    bool bound() const {
        return binding.image && binding.image <= std::numeric_limits<uintptr_t>::max() - 0x757fd0 &&
            binding.create == binding.image + 0x12ebc0 &&
            binding.initialize == binding.image + 0x12e420 &&
            binding.destroy == binding.image + 0x12e3f0 && qualify(binding.image);
    }
    uintptr_t create() noexcept {
        return bound() ? invoke.create(binding.create) : 0;
    }
    int initialize(uintptr_t object) noexcept {
        return bound() && object ? invoke.initialize(binding.initialize, object) : 5;
    }
    bool validate(uintptr_t object) noexcept {
        return bound() && tglExecutionShape(object, binding.image, read, qualify);
    }
    void destroy(uintptr_t object) noexcept {
        // Authenticated lease is a lifetime requirement, not reacquired here.
        if (object && binding.destroy) invoke.destroy(binding.destroy, object);
    }
};

// Integration must bind Create to the constructor factory12ebc0, NOT
// child factory12e9c0 (which discards Initialize errors). Backend validation
// must authenticate the image/ABI and nested d38/d40 objects. Their mere
// presence is necessary, not sufficient. Backend must outlive this owner.
template<class Backend> class TglExecutionOwner {
    Backend& backend;
    uintptr_t object = 0;
public:
    explicit TglExecutionOwner(Backend& b) noexcept : backend(b) {}
    TglExecutionOwner(const TglExecutionOwner&) = delete;
    TglExecutionOwner& operator=(const TglExecutionOwner&) = delete;
    ~TglExecutionOwner() { reset(); }
    void reset() noexcept {
        const auto old = object;
        object = 0;
        if (old) backend.destroy(old);
    }
    int ensure() noexcept {
        if (object) {
            if (backend.validate(object)) return 0;
            reset();
            return 5;
        }
        const auto candidate = backend.create();
        if (!candidate) return 1;
        const int result = backend.initialize(candidate);
        if (result || !backend.validate(candidate)) {
            backend.destroy(candidate);
            return result ? result : 5;
        }
        object = candidate; // publish only after native result AND validation
        return 0;
    }
    // Eligibility initializes execution on each admitted pass. ensure() alone
    // only supplies lifetime ownership; it must not preserve stale frame flags.
    // A newly created object is already initialized by ensure(), exactly once.
    int preparePass() noexcept {
        if (!object) return ensure();
        if (!backend.validate(object)) { reset(); return 5; }
        const int result = backend.initialize(object);
        if (result || !backend.validate(object)) {
            reset();
            return result ? result : 5;
        }
        return 0;
    }
    uintptr_t get() const noexcept { return object; }
};

// Borrowed MHW argument from creator1e66ef -> renderer Allocate. The image,
// interface, OS and heap leases must outlive every child using this snapshot.
// This authenticates interface identity/presence, NOT heap contents or commands.
struct TglVeboxHardwareBinding {
    uintptr_t interface = 0, os = 0, heap = 0;
    struct StateResource { uintptr_t resource = 0; uint32_t instanceOffset = 0; };
    // Keep the same external serialization/image lease across all stages.
    // Publication is atomic, not native-state rollback: assignment may advance
    // the heap even if the subsequent owned range validation fails.
    // Owned setup ONLY: native1296e0 already calls assignment at1297b7.
    // Never call this before/after that native setup for the same pass.
    template<class Read, class Qualify, class Executable, class Invoke>
    uint32_t prepareHeapState(uintptr_t image, bool kernelResource,
                              uint32_t bytes, uint32_t regionOffset,
                              Read read, Qualify qualify, Executable executable,
                              Invoke invoke, StateResource& out) const {
        out = {};
        uint32_t stride = 0;
        if (!bytes || !heap || heap > std::numeric_limits<uintptr_t>::max() - 0x2c ||
            !read(heap + 0x2c, &stride, 4) || !stride ||
            uint64_t(regionOffset) + bytes > stride) return 5;
        const uint32_t result = assignState(image, read, qualify, executable, invoke);
        if (result) return result;
        StateResource candidate;
        if (!selectStateResource(false, 0, kernelResource, read, candidate) ||
            !heapStateRange(candidate, bytes, read, regionOffset)) return 5;
        out = candidate;
        return 0;
    }
    // Reuse native refresh/wait/assignment, including OS reset policy and tag
    // wraparound. Caller serializes the borrowed interface and keeps its image
    // lease alive. Do not select a state or emit commands after nonzero result.
    template<class Read, class Qualify, class Executable, class Invoke>
    uint32_t assignState(uintptr_t image, Read read, Qualify qualify,
                         Executable executable, Invoke invoke) const {
        TglVeboxHardwareBinding fresh;
        if (!fresh.resolve(interface, os, image, read, qualify) || fresh.heap != heap) return 5;
        uint32_t count = 0, next = 0;
        if (heap > std::numeric_limits<uintptr_t>::max() - 0x3f ||
            !read(interface + 0x28, &count, 4) || !count || count > INT32_MAX ||
            !read(heap + 4, &next, 4) || next >= count) return 5;
        constexpr std::array<uint8_t, 16> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,
            0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8};
        std::array<uint8_t, 16> actual{};
        const uintptr_t entry = image + 0xfefa0;
        if (!executable(entry, anchor.size()) ||
            !read(entry, actual.data(), actual.size()) || actual != anchor) return 5;
        return invoke(entry, interface);
    }
    // Heap resources use the proven native buffer layout (resource size148).
    // Not a CM/surface validator. Caller holds the heap/resource lifetime lock;
    // this proves CPU descriptor bounds/backing only, not GPU completion/sync.
    // Include regionOffset (native174cba) as well as the instance base; checking
    // only the instance base would miss a region that crosses buffer capacity.
    template<class Read>
    bool heapStateRange(const StateResource& state, uint32_t bytes, Read read,
                        uint32_t regionOffset = 0) const {
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        if (!interface || !os || !heap || !bytes || heap > max - 0x2d0 ||
            (state.resource != heap + 0x40 && state.resource != heap + 0x188)) return false;
        uint32_t capacity = 0, type = 0, instanceBytes = 0;
        uintptr_t handle = 0, address = 0;
        return read(heap + 0x2c, &instanceBytes, 4) && instanceBytes &&
            uint64_t(regionOffset) + bytes <= instanceBytes &&
            state.instanceOffset % instanceBytes == 0 &&
            read(state.resource + 0x10, &capacity, 4) &&
            read(state.resource + 0x14, &type, 4) && type == 0 &&
            uint64_t(state.instanceOffset) + regionOffset + bytes <= capacity &&
            read(state.resource + 0x20, &handle, 8) &&
            read(state.resource + 0x50, &address, 8) && (handle || address);
    }
    // Native171351..1713ba. Selection only: no GPU address or command emitted.
    template<class Read>
    bool selectStateResource(bool cmBuffer, uintptr_t parameterSurface,
                             bool kernelResource, Read read, StateResource& out) const {
        out = {};
        if (!interface || !os || !heap) return false;
        if (cmBuffer) {
            if (!parameterSurface) return false;
            out.resource = parameterSurface;
            return true;
        }
        const uintptr_t resourceOffset = kernelResource ? 0x188 : 0x40;
        if (heap > std::numeric_limits<uintptr_t>::max() - 0x2d0) return false;
        uint32_t instanceBytes = 0, current = 0;
        if (!read(heap + 0x2c, &instanceBytes, 4) || !read(heap, &current, 4)) return false;
        const uint64_t offset = uint64_t(instanceBytes) * current;
        if (offset > UINT32_MAX) return false;
        out = {heap + resourceOffset, uint32_t(offset)};
        return true; // backing/sync/capacity must be validated before native use
    }
    template<class Read, class Qualify>
    bool resolve(uintptr_t mhw, uintptr_t expectedOs, uintptr_t image,
                 Read read, Qualify qualify) {
        *this = {};
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        if (!mhw || mhw > max - 0x28 || !expectedOs || !image ||
            image > max - 0x7595b0 || !qualify(image)) return false;
        uintptr_t vt = 0, actualOs = 0, actualHeap = 0;
        if (!read(mhw, &vt, 8) || vt != image + 0x759520 ||
            !read(mhw + 0x18, &actualOs, 8) || actualOs != expectedOs ||
            !read(mhw + 0x20, &actualHeap, 8) || !actualHeap) return false;
        interface = mhw; os = actualOs; heap = actualHeap;
        return true;
    }
};

// Exact base prefix touched by parent130ea0 -> nonvirtual126520. NOT a
// complete VEBOX object, not publishable as a native child by itself.
// SysV x86_64 native constructor12d4e0: six register arguments including this,
// followed by performance-data, cache-control reference and status on stack.
// This is an invocation mechanism, NOT storage qualification/publication.
// Owner must authenticate entry, complete extent, dependencies and image lease.
struct TglVeboxLifetimeBinding {
    uintptr_t image = 0, construct = 0, destroy = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t base, Read read, Qualify qualify, Executable executable) {
        image = construct = destroy = 0;
        constexpr std::array<uint8_t,16> constructor{
            0x55,0x48,0x89,0xe5,0x53,0x50,0xb8,0x88,
            0x10,0x00,0x00,0xe8,0xd0,0x41,0xef,0xff};
        constexpr std::array<uint8_t,16> destructor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x50,
            0x48,0x8d,0x05,0x49,0x9e,0x62,0x00,0x48};
        std::array<uint8_t,16> c{}, d{};
        if (!base || base > std::numeric_limits<uintptr_t>::max() - 0x12de40 ||
            !qualify(base) || !executable(base+0x12d4e0,16) ||
            !executable(base+0x12de30,16) || !read(base+0x12d4e0,c.data(),16) ||
            !read(base+0x12de30,d.data(),16) || c != constructor || d != destructor)
            return false;
        image = base; construct = base+0x12d4e0; destroy = base+0x12de30;
        return true;
    }
};

// Non-deleting native base teardown only; owned storage release is separate.
// Owned GPU resources must be cleaned before this resets the native vtable.
struct TglNativeVeboxDestructorInvoker {
    void operator()(uintptr_t entry, void* object) const noexcept {
        using Destroy = void (*)(void*);
        reinterpret_cast<Destroy>(entry)(object);
    }
};

struct TglNativeVeboxConstructorInvoker {
    void operator()(uintptr_t entry, void* object, void* os, void* mhw,
                    void* sfc, void* renderHal, void* history,
                    void* performanceData, const void* cacheControl, int32_t* status) const {
        using Construct = void (*)(void*,void*,void*,void*,void*,void*,void*,const void*,int32_t*);
        reinterpret_cast<Construct>(entry)(object,os,mhw,sfc,renderHal,history,
                                           performanceData,cacheControl,status);
    }
};

// Arm only after construction of private owned storage has completed. This
// scope does not infer object size, publish a child, or adopt native storage.
// Cleanup owns GPU descriptors; native base teardown owns transferred CPU
// shells. Storage release is last and must not call native operator delete.
template<class Cleanup, class Destroy, class Release> class TglVeboxTeardownScope {
    void* object_;
    Cleanup& cleanup_;
    Destroy& destroy_;
    Release& release_;
public:
    TglVeboxTeardownScope(void* constructedPrivateObject, Cleanup& cleanup,
                         Destroy& destroy, Release& release) noexcept
        : object_(constructedPrivateObject), cleanup_(cleanup),
          destroy_(destroy), release_(release) {
        static_assert(noexcept(cleanup_(object_)) && noexcept(destroy_(object_)) &&
                      noexcept(release_(object_)), "teardown must not throw");
    }
    TglVeboxTeardownScope(const TglVeboxTeardownScope&) = delete;
    TglVeboxTeardownScope& operator=(const TglVeboxTeardownScope&) = delete;
    ~TglVeboxTeardownScope() { reset(); }
    void reset() noexcept {
        void* object = object_;
        object_ = nullptr; // revoke before callbacks, including reentrant reset
        if (!object) return;
        cleanup_(object);
        destroy_(object);
        release_(object);
    }
};

struct TglVeboxPrefix {
    uintptr_t vtable = 0, slot08 = 0, os = 0, renderHal = 0, sku = 0, wa = 0;
    std::array<uint8_t, 8> slot30{};
    uintptr_t slot38 = 0, slot40 = 0;
    uint8_t flag48 = 0, flag49 = 0;
    std::array<uint8_t, 6> padding4a{};
    uintptr_t parentData = 0;
    uint32_t sourceValue = 0, padding5c = 0;
    // Constructor-data preparation only. No vtable is installed, no native
    // object is published, and no report/resource ownership is transferred.
    // Caller authenticates/leases both borrowed interfaces before this read.
    // platform belongs at full-child1bb0, NOT inside this 0x60 prefix.
    template<class Read>
    bool initializeBorrowed(uintptr_t nativeOs, uintptr_t nativeRenderHal,
                            uintptr_t performanceData, uint32_t& platform, Read read) {
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        if (!nativeOs || nativeOs > max - 0xb || !nativeRenderHal ||
            nativeRenderHal > max - 0x87) return false;
        // Do not overwrite a prefix that could already carry live ownership.
        if (vtable || slot08 || os || renderHal || sku || wa || slot38 || slot40 ||
            flag48 || flag49 || parentData || sourceValue) return false;
        uintptr_t nativeSku = 0, nativeWa = 0;
        uint32_t nativePlatform = 0;
        if (!read(nativeRenderHal+0x78,&nativeSku,8) ||
            !read(nativeRenderHal+0x80,&nativeWa,8) ||
            !read(nativeOs+8,&nativePlatform,4)) return false;
        TglVeboxPrefix candidate;
        candidate.os = nativeOs; candidate.renderHal = nativeRenderHal;
        candidate.sku = nativeSku; candidate.wa = nativeWa;
        candidate.slot38 = performanceData;
        *this = candidate; platform = nativePlatform;
        return true;
    }
    template<class Read>
    bool configure(uintptr_t parent, uintptr_t source, Read read) {
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        if (!parent || parent > max - 0x9f0 || !source || source > max - 0x120)
            return false;
        uintptr_t surface = 0;
        uint32_t kind = 0, value = 0;
        uint8_t flag = 0;
        if (!read(source + 0x98, &surface, sizeof(surface)) || !surface ||
            surface > max - 0x138 || !read(surface + 0x134, &kind, sizeof(kind)) ||
            !read(source + 0x110, &flag, sizeof(flag)) ||
            !read(source + 0x11c, &value, sizeof(value))) return false;
        flag48 = flag & 1;
        flag49 = kind == 5;
        parentData = parent + 0x9f0;
        sourceValue = value;
        return true; // all fields borrow parent/source lifetime; no ownership transfer
    }
};
static_assert(sizeof(TglVeboxPrefix) == 0x60);
static_assert(offsetof(TglVeboxPrefix, os) == 0x10);
static_assert(offsetof(TglVeboxPrefix, renderHal) == 0x18);
static_assert(offsetof(TglVeboxPrefix, flag48) == 0x48);
static_assert(offsetof(TglVeboxPrefix, flag49) == 0x49);
static_assert(offsetof(TglVeboxPrefix, parentData) == 0x50);
static_assert(offsetof(TglVeboxPrefix, sourceValue) == 0x58);

// child40 owns a separate28-byte CPU report (1335c0), NOT execution
// state child88. Native1338b0 initializes fields, leaving padding untouched.
struct TglVeboxReportState {
    std::array<uint8_t, 0x28> bytes{};
    void reset() noexcept {
        for (size_t i = 0; i <= 0x1c; ++i) bytes[i] = 0;
        for (size_t i = 0x20; i <= 0x25; ++i) bytes[i] = 0;
    }
};
static_assert(sizeof(TglVeboxReportState) == 0x28);

// 127502..12756c + virtualc8=12eaf0. Offsets are proven; semantic
// flag names remain unassigned. Do not reset DWORD4 when execution13 is false.
inline void tglUpdateVeboxReport(TglVeboxReportState& report,
                                 const std::array<uint8_t, 0x15>& execution) noexcept {
    report.bytes[0] = execution[0x0f] & 1;
    report.bytes[2] = execution[0x0b] & 1;
    if (execution[0x13] & 1) {
        const uint32_t value = (execution[0x14] & 1) && !(execution[8] & 1) ? 2 : 3;
        std::memcpy(report.bytes.data() + 4, &value, sizeof(value));
    }
}

// Native Render tail12cdcb..12ce07. This exports a decision already made
// by the platform producer; it must NOT choose or normalize outputPipe.
// execution+a0c is the output-pipe decision, NOT the persistent VEBOX
// execution/transition state at history+4 (child90). Never exchange them.
inline void tglFinalizeVeboxReport(TglVeboxReportState& report, uint32_t outputPipe,
                                   uint8_t execution18, uint8_t execution8f4) noexcept {
    std::memcpy(report.bytes.data() + 0xc, &outputPipe, sizeof(outputPipe));
    report.bytes[0x24] = (execution18 ^ 1) & 1;
    report.bytes[0x25] = execution8f4 & 1;
}

// Exact post-render export12d240 ->12d120/12d1a0. Source is the
// owned equivalent of child40, not the child object itself. Preserve every
// destination byte not written by native; source/destination may alias.
inline void tglExportVeboxReport(const std::array<uint8_t, 0x26>& state,
                                 uint8_t feature1ba9,
                                 std::array<uint8_t, 0x26>& report) noexcept {
    const auto source = state;
    for (auto offset : {0x00, 0x02, 0x13, 0x15, 0x17, 0x19, 0x24, 0x25})
        report[offset] = source[offset] & 1;
    for (auto offset : {0x14, 0x16, 0x18, 0x1a}) report[offset] = source[offset];
    std::memcpy(report.data() + 4, source.data() + 4, 4);
    std::memcpy(report.data() + 0xc, source.data() + 0xc, 4);
    report[0x10] = feature1ba9 & 1;
}
inline void tglExportVeboxReport(const TglVeboxReportState& state,
                                 uint8_t feature1ba9,
                                 std::array<uint8_t, 0x26>& report) noexcept {
    std::array<uint8_t, 0x26> fields{};
    std::memcpy(fields.data(), state.bytes.data(), fields.size());
    tglExportVeboxReport(fields, feature1ba9, report);
}

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

// 63030 allocates VM at resource+50, registers the resource at+20 and stores
// shared metadata at+28/+30. Only +50 is the buffer byte address. Caller holds
// resource lifetime; Writable and Fill must use native policy, not bypass it.
template<class Writable, class Fill>
int tglFillBuffer(const std::array<uint8_t, 0x148> &resource, uint32_t bytes,
                  uint8_t value, Writable writable, Fill fill) noexcept {
    static_assert(noexcept(writable(uintptr_t{}, uint32_t{})) &&
                  noexcept(fill(uintptr_t{}, uint32_t{}, uint8_t{})),
                  "memory access callbacks must report failure without exceptions");
    uint32_t type = 0, capacity = 0;
    uintptr_t address = 0, handle = 0;
    std::memcpy(&type, resource.data() + 0x14, sizeof(type));
    std::memcpy(&capacity, resource.data() + 0x10, sizeof(capacity));
    std::memcpy(&address, resource.data() + 0x50, sizeof(address));
    std::memcpy(&handle, resource.data() + 0x20, sizeof(handle));
    if (type || !bytes || bytes > capacity || !handle || !address ||
        address > std::numeric_limits<uintptr_t>::max() - bytes ||
        !writable(address, bytes)) return 5;
    return fill(address, bytes, value);
}

#if defined(__APPLE__)
inline bool tglNativeWritableRange(uintptr_t address, uint32_t bytes) noexcept {
    if (!address || !bytes || address > UINTPTR_MAX - bytes) return false;
    const mach_vm_address_t end = address + bytes;
    mach_vm_address_t cursor = address;
    for (unsigned regions = 0; cursor < end && regions < 64; ++regions) {
        mach_vm_address_t base = cursor;
        mach_vm_size_t size = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info{};
        for (;;) {
            mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
            if (mach_vm_region_recurse(mach_task_self(), &base, &size, &depth,
                reinterpret_cast<vm_region_recurse_info_t>(&info), &count) != KERN_SUCCESS ||
                count != VM_REGION_SUBMAP_INFO_COUNT_64 || base > cursor || !size ||
                base > UINT64_MAX - size || cursor >= base + size) return false;
            if (!info.is_submap) break;
            if (depth >= 16) return false;
            ++depth;
        }
        if (!(info.protection & VM_PROT_WRITE)) return false;
        cursor = base + size < end ? base + size : end;
    }
    return cursor == end;
}

// No mprotect/cs_allow_invalid/permission escalation. Mach decides each write.
inline int tglFillBufferNative(const std::array<uint8_t, 0x148> &resource,
                               uint32_t bytes, uint8_t value) noexcept {
    const auto write = [](uintptr_t address, uint32_t length, uint8_t byte) noexcept {
        std::array<uint8_t, 4096> block;
        block.fill(byte);
        for (uint32_t offset = 0; offset < length;) {
            const uint32_t n = length - offset < block.size() ? length - offset : block.size();
            const kern_return_t status = mach_vm_write(mach_task_self(), address + offset,
                reinterpret_cast<vm_offset_t>(block.data()), n);
            if (status != KERN_SUCCESS) return static_cast<int>(status);
            offset += n;
        }
        return 0;
    };
    return tglFillBuffer(resource, bytes, value, tglNativeWritableRange, write);
}
#endif

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
inline std::array<uint8_t, 0x48> tglSurfaceDescriptorBytes(const uint8_t* shell) noexcept {
    std::array<uint8_t, 0x48> out{};
    const auto get = [&](size_t offset) {
        uint32_t value = 0;
        std::memcpy(&value, shell + offset, sizeof(value));
        return value;
    };
    const auto put = [&](size_t offset, uint32_t value) {
        std::memcpy(out.data() + offset, &value, sizeof(value));
    };
    put(0, 1);
    put(8, get(0x130)); put(0xc, get(0xd8)); put(0x10, get(0xdc));
    put(0x14, get(0xe0)); put(0x18, get(0x13c)); put(0x28, get(0xe4));
    std::memcpy(out.data() + 0x2c, shell + 0x50, 16);
    static_assert(sizeof(uintptr_t) == 8, "pinned x86_64 ABI");
    const uintptr_t resource = reinterpret_cast<uintptr_t>(shell + 0x148);
    std::memcpy(out.data() + 0x40, &resource, sizeof(resource));
    out[4] = shell[0x29a] & 1;
    const uint32_t pitch = get(0xe0);
    if (pitch) put(0x24, (get(0x100) - get(0xf0)) / pitch + get(0x108));
    return out;
}

inline std::array<uint8_t,0x48> tglSurfaceDescriptor(
        const std::array<uint8_t,0x2a8>& shell) noexcept {
    return tglSurfaceDescriptorBytes(shell.data());
}

// 52d60: five optional borrowed shells, no allocation or ownership transfer.
inline std::array<uint8_t, 0x170> tglSurfaceCommandSetBytes(
        const std::array<const uint8_t*,5>& surfaces,
        uint8_t di) noexcept {
    std::array<uint8_t, 0x170> out{};
    out[0x168] = di & 1;
    for (size_t i = 0; i < surfaces.size(); ++i) {
        if (!surfaces[i]) continue;
        const auto descriptor = tglSurfaceDescriptorBytes(surfaces[i]);
        std::memcpy(out.data() + i * 0x48, descriptor.data(), descriptor.size());
        if (i < 2) std::memcpy(out.data() + i * 0x48 + 0x20,
                               surfaces[i] + 0xf8, 4);
    }
    out[0x169] = surfaces[1] ? 1 : 0;
    return out;
}

inline std::array<uint8_t,0x170> tglSurfaceCommandSet(
        const std::array<const std::array<uint8_t,0x2a8>*,5>& surfaces,
        uint8_t di) noexcept {
    std::array<const uint8_t*,5> views{};
    for (size_t i=0; i<views.size(); ++i)
        if (surfaces[i]) views[i] = surfaces[i]->data();
    return tglSurfaceCommandSetBytes(views,di);
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

struct TglNativeRegistrationBinding {
    uintptr_t context = 0, entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t os, uintptr_t image, Read read, Qualify qualify,
                 Executable executable) {
        *this = {};
        if (!os || !image || os > UINTPTR_MAX-0x24f || image > UINTPTR_MAX-0x645f6 ||
            !qualify(image)) return false;
        uintptr_t native = 0;
        constexpr std::array<uint8_t,22> expected{
            0x55,0x48,0x89,0xe5,0x31,0xc0,0x48,0x89,0x7d,0xf8,0x48,
            0x89,0x75,0xf0,0x89,0x55,0xec,0x89,0x4d,0xe8,0x5d,0xc3};
        std::array<uint8_t,22> actual{};
        if (!read(os+0x248,&native,sizeof(native)) || native != image+0x645e0 ||
            !executable(native,expected.size()) ||
            !read(native,actual.data(),actual.size()) || actual != expected) return false;
        context = os; entry = native;
        return true;
    }
};

// Call native even though the exact TGL implementation is a success stub.
// Registration does not establish resource backing or GPU completion.
struct TglNativeRegistrationInvoker {
    int operator()(uintptr_t entry, uintptr_t os, const void* resource,
                   bool write, bool read) const noexcept {
        using Register = int (*)(void*,const void*,uint32_t,uint32_t);
        return reinterpret_cast<Register>(entry)(reinterpret_cast<void*>(os),resource,
                                                 write ? 1u : 0u,read ? 1u : 0u);
    }
};

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

    explicit TglOwnedResource(Backend &backend) noexcept : backend_(backend), storage_(localStorage_) {}
    // Borrow storage, not pre-existing handles. External storage and backend
    // outlive this owner; caller serializes all native access under its lease.
    TglOwnedResource(Backend &backend, Storage& emptyStorage) noexcept
        : backend_(backend), storage_(emptyStorage) {
        admitted_ = reinterpret_cast<uintptr_t>(storage_.data()) % 8 == 0;
        for (auto byte : storage_) if (byte) admitted_ = false;
    }
    ~TglOwnedResource() { reset(); }
    TglOwnedResource(const TglOwnedResource &) = delete;
    TglOwnedResource &operator=(const TglOwnedResource &) = delete;
    TglOwnedResource(TglOwnedResource &&) = delete;
    TglOwnedResource &operator=(TglOwnedResource &&) = delete;

    Result ensure(const TglResourceKey &key) noexcept {
        if (!admitted_) return {5, State::Empty, false};
        if (!live_) for (auto byte : storage_)
            if (byte) return {5, State::Empty, false}; // never erase/adopt foreign handles
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
        if (!live_) return;
        live_ = false;
        state_ = State::Empty;
        key_ = {};
        backend_.release(storage_);
        storage_.fill(0);
    }
    State state() const noexcept { return state_; }
    const Storage &storage() const noexcept { return storage_; }

private:
    Backend &backend_; // must outlive this owner
    alignas(8) Storage localStorage_{};
    Storage& storage_;
    TglResourceKey key_{};
    bool live_ = false;
    State state_ = State::Empty;
    bool admitted_ = true;
};

// Owned representation of the proven Darwin 0x2a8 surface extent. Native
// OsResource is at +0x148; rotation and trailing fields start at +0x290.
// Not a Linux struct transplant and not a cast/adoption of an existing surface.
struct alignas(8) TglOwnedSurfaceStorage {
    std::array<uint8_t,0x148> prefix{};
    std::array<uint8_t,0x148> resource{};
    std::array<uint8_t,0x18> tail{};
};
static_assert(offsetof(TglOwnedSurfaceStorage,resource) == 0x148);
static_assert(offsetof(TglOwnedSurfaceStorage,tail) == 0x290);
static_assert(sizeof(TglOwnedSurfaceStorage) == 0x2a8);
// Exact ICL classifier4a500 and its 90-entry relative jump table4a55c.
// 3 is the native OTHER color pack, not an invalid format admission result.
inline uint32_t tglOwnedChromaColorPack(uint32_t format) noexcept {
    switch (format) {
        case 0x19: case 0x1b: case 0x20: case 0x21: case 0x22:
        case 0x23: case 0x29: case 0x2a: case 0x2b: case 0x52: case 0x53: return 0;
        case 0xd: case 0xe: case 0xf: case 0x10: case 0x11: case 0x12:
        case 0x13: case 0x1e: case 0x24: case 0x25: return 1;
        case 1: case 2: case 3: case 4: case 5: case 6: case 7:
        case 0xa: case 0xb: case 0xc: case 0x14: case 0x15: case 0x16:
        case 0x17: case 0x26: case 0x50: case 0x51: case 0x55: case 0x5a: return 2;
        default: return 3;
    }
}
// Exact Darwin chroma producer1fa603..673. Changes owned surface metadata
// only, not OS backing or GPU bytes. Color-pack classification is supplied
// by the qualified format classifier:0=420,1=422,2=444,3=other.
inline bool tglNormalizeOwnedChromaSiting(TglOwnedSurfaceStorage& surface,
                                        uint32_t colorPack) noexcept {
    if (colorPack > 3) return false;
    uint32_t siting=0;
    std::memcpy(&siting,surface.tail.data()+4,4);
    if (!siting) siting=0x21;
    if (colorPack == 1) siting=(siting&7)|0x10;
    else if (colorPack == 2) siting=0x11;
    std::memcpy(surface.tail.data()+4,&siting,4);
    return true;
}
// Exact Darwin1fa675..1faa18. Input siting has already been normalized.
// Only upsample fields are owned here; downsample and other state stay intact.
inline bool tglPrepareOwnedChromaUpsampling(uint32_t& state, uint32_t pack,
                                          uint32_t siting, bool needed, bool di) noexcept {
    if (pack > 3) return false;
    uint32_t candidate=(state&~0x41fu)|0x400u;
    if (needed) {
        const uint32_t masks[6]={0x21,0x22,0x11,0x12,0x41,0x42};
        for (uint32_t type=0;type<6;++type) {
            if ((siting&masks[type]) != masks[type]) continue;
            if (pack == 0 || (pack == 1 && (type==2 || type==3))) {
                const uint32_t horizontal=type&1;
                const uint32_t vertical=type<2 ? (di ? 2u : 1u) :
                                        type<4 ? 0u : (di ? 4u : 2u);
                candidate=(candidate&~0x41fu)|horizontal|(vertical<<2);
            }
            break; // Native ordered else-if chain, even with mixed siting bits.
        }
    }
    state=candidate;
    return true;
}
// Exact Darwin1faaa4..1fad78. Darwin enables downsampling only for pipe2;
// unlike later Linux code this does not add a deinterlace/non-YUY2 condition.
inline bool tglPrepareOwnedChromaDownsampling(uint32_t& state, uint32_t pack,
                                            uint32_t siting, uint32_t pipe) noexcept {
    if (pack > 3 || pipe > 2) return false;
    uint32_t candidate=(state&~0xbe0u)|0x800u;
    if (pipe == 2) {
        const uint32_t masks[6]={0x21,0x22,0x11,0x12,0x41,0x42};
        for (uint32_t type=0;type<6;++type) {
            if ((siting&masks[type]) != masks[type]) continue;
            if (pack==0 || (pack==1 && (type==2 || type==3))) {
                const uint32_t vertical=type<2 ? 1u : type<4 ? 0u : 2u;
                candidate=(candidate&~0xbe0u)|((type&1)<<5)|(vertical<<7);
            }
            break;
        }
    }
    state=candidate;
    return true;
}
// Owns only chroma fields of the state DWORD and metadata of owned surfaces.
// Native1fa55e initializes both directions before checking source; missing
// source skips target normalization, missing target preserves completed upsample.
inline bool tglPrepareOwnedChromaSampling(uint32_t& state,
                                        TglOwnedSurfaceStorage* source,
                                        TglOwnedSurfaceStorage* target,
                                        bool iecp, bool di, uint32_t pipe) noexcept {
    if (pipe>2) return false;
    state=(state&~0xfffu)|0xc00u;
    if (!source) return true;
    uint32_t format=0, siting=0;
    std::memcpy(&format,source->prefix.data()+0x130,4);
    const auto sourcePack=tglOwnedChromaColorPack(format);
    tglNormalizeOwnedChromaSiting(*source,sourcePack);
    std::memcpy(&siting,source->tail.data()+4,4);
    tglPrepareOwnedChromaUpsampling(state,sourcePack,siting,iecp,di);
    if (!target) return true;
    std::memcpy(&format,target->prefix.data()+0x130,4);
    const auto targetPack=tglOwnedChromaColorPack(format);
    tglNormalizeOwnedChromaSiting(*target,targetPack);
    std::memcpy(&siting,target->tail.data()+4,4);
    tglPrepareOwnedChromaDownsampling(state,targetPack,siting,pipe);
    return true;
}
// Closed state flags from Darwin1faecf..1faf2d and1fb086..1fb0f4.
// These are scalar inputs, not transplanted ICL object offsets. Other flags
// remain owned by the enclosing state producer until their sources are closed.
inline bool tglPrepareOwnedStateHistoryAndPipe(uint32_t& flags,
                                             bool firstFrame, bool dn, bool di,
                                             uint32_t pipe, bool skuSuppressPipe) noexcept {
    if (pipe>2) return false;
    uint32_t candidate=flags&~0x6020u;
    if (!firstFrame && (dn || di)) candidate|=0x20;
    if (!skuSuppressPipe && pipe==1) candidate|=0x2000;
    flags=candidate;
    return true;
}
// Exact ICL base+50 target1e6cb0..1e6d92, pure selection (no state writes).
// Semantic inputs avoid borrowing ICL layout for the owned TGL child.
inline bool tglSelectOwnedStateMode(uint32_t& mode, uint32_t pipe,
                                   bool exec19, bool exec14, uint32_t flags,
                                   uint32_t source138) noexcept {
    if (pipe>2) return false;
    const bool history=(flags&0x20)!=0;
    if (pipe==1 && !exec19)
        mode=(exec14 || history || source138==6 || source138==3 ||
              source138==1 || source138==0) ? 2u : 1u;
    else mode=(pipe==2 || history) ? 2u : 0u;
    return true;
}
// Darwin1faf78..1fb05f: base+50 return contributes only two bits; exec12
// contributes bit0. DN-only special input forces IECP, but a false special
// condition must not clear an IECP bit already chosen by earlier stages.
inline void tglPrepareOwnedStateDnControls(uint32_t& flags, uint32_t nativeMode,
                                          bool exec12, bool dnSpecial,
                                          uint32_t sourceType) noexcept {
    uint32_t candidate=(flags&~0x300c1u)|((nativeMode&3)<<6)|0x10000u;
    if (exec12) candidate|=1;
    const bool special=!(flags&0x10) && (flags&8) &&
                       (dnSpecial || sourceType==1 || sourceType==2);
    if (special) candidate|=0x20004;
    flags=candidate;
}
// Leading flags1faded..1faecd. primaryIeCp is the qualified semantic
// callback result, not a call through the incompatible TGL base+c8 slot.
inline bool tglPrepareOwnedStateLeadingFlags(uint32_t& flags, uint32_t pipe,
                                            bool primaryIeCp, bool di, bool dn) noexcept {
    if (pipe>2) return false;
    uint32_t candidate=flags&~0x8001cu;
    if (pipe==1 || pipe==2 || primaryIeCp) candidate|=4;
    if (di) candidate|=16;
    if (dn) candidate|=8;
    if (pipe==1 && (dn||di)) candidate|=0x80000;
    flags=candidate;
    return true;
}
struct TglOwnedStateInputs {
    uint32_t pipe=0;
    bool di=false, dn=false, firstFrame=false;
    bool exec19=false, exec14=false, exec12=false, dnSpecial=false;
    bool chromaIeCp=false, chromaDi=false, skuSuppressPipe=false;
    TglOwnedSurfaceStorage* source=nullptr;
    TglOwnedSurfaceStorage* target=nullptr;
};
struct TglOwnedSfcLineBufferSizes {
    uint32_t avs=0, ief=0;
};
// Exact Darwin 1df4a4..1df59e prefix, not later Linux enum values.
// IECP takes priority over DI; only IECP changes the source siting field.
// Caller initializes the rest of the b8-byte packet and owns both arguments.
inline void tglPrepareOwnedSfcInputChroma(std::array<uint8_t,0xb8>& packet,
                                         uint32_t& sourceSiting,uint32_t inputFormat,
                                         bool iecp,bool di) noexcept {
    uint32_t subsampling=0;
    uint8_t eightTap=0;
    if (iecp) { subsampling=4; eightTap=1; sourceSiting=0x11; }
    else if (di) subsampling=2;
    else if (inputFormat==0x19 || inputFormat==0x53 || inputFormat==0x52)
        subsampling=1;
    else {
        const auto pack=tglOwnedChromaColorPack(inputFormat);
        if (pack==1) subsampling=2;
        else if (pack==2) { subsampling=4; eightTap=1; }
    }
    packet[0]=1;
    std::memcpy(packet.data()+8,&subsampling,4);
    packet[0x6a]=eightTap;
}
// Darwin 1df5a6..1df6a5: output siting phases and chroma downsampling mode.
// Preserve conditional no-write branches; full producer starts with zero packet.
inline void tglPrepareOwnedSfcOutputChroma(std::array<uint8_t,0xb8>& packet,
                                          uint32_t outputSiting,uint32_t outputFormat) noexcept {
    const uint32_t vertical=(outputSiting&0x20)?4:(outputSiting&0x40)?8:0;
    const uint32_t horizontal=(outputSiting&2)?4:(outputSiting&4)?8:0;
    std::memcpy(packet.data()+0x1c,&vertical,4);
    std::memcpy(packet.data()+0x20,&horizontal,4);
    uint32_t input=0,mode=0;
    std::memcpy(&input,packet.data()+8,4);
    const auto pack=tglOwnedChromaColorPack(outputFormat);
    if (input==4) {
        if (pack==0) mode=1;
        else if (pack==1) mode=2;
        else return;
    } else if (input==2) {
        if (pack==0) mode=3;
        else return;
    }
    std::memcpy(packet.data()+0x18,&mode,4);
}
// ICL 1e0ca0..1e0dcb setup order. Producers operate on owned state;
// caller binds the closed argument ABI, never an ICL object to TGL code.
// Flags are native renderData+1/+2/+3 and base+9b8, low bit only.
template<class PlatformState, class StateParameters, class Avs, class Ief>
int tglSetupOwnedSfc(uint8_t scalingFlag,uint8_t forceAvs,
                    uint8_t iefFlag,uint8_t cscFlag,
                    PlatformState platformState,StateParameters stateParameters,
                    Avs avs,Ief ief) noexcept {
    static_assert(noexcept(platformState()) && noexcept(stateParameters()) &&
                  noexcept(avs()) && noexcept(ief()),"SFC producers must report status");
    // +a8 platform hook (ICL 1e0c80 no-op), then +90 state producer
    // (ICL 1df3e0, zeroes b8 bytes at borrowed renderData+18).
    int status=platformState();
    if (status) return status;
    status=stateParameters();
    if (status) return status;
    if ((scalingFlag|forceAvs)&1) {
        status=avs();
        if (status) return status;
    }
    // Native 1e0dbb calls slot98 but does not assign EAX to saved status.
    if ((iefFlag|cscFlag)&1) (void)ief();
    return status;
}
// ICL derived InitRenderData 1f7450 -> base 1e1080 then alloc(b8).
// Backend returns writable, CPU-owned native allocation, never a GPU address.
// Reinitialization deliberately retires the old state even if allocation fails.
template<class Backend> class TglOwnedSfcStateParameters {
    TglTrackedCpuOwner<Backend> owner;
public:
    explicit TglOwnedSfcStateParameters(Backend& backend) : owner(backend) {}
    bool initialize() {
        owner.reset();
        if (!owner.allocate(0xb8)) return false;
        std::memset(reinterpret_cast<void*>(owner.get()),0,0xb8);
        return true;
    }
    void reset() noexcept { owner.reset(); }
    uint8_t* data() noexcept { return reinterpret_cast<uint8_t*>(owner.get()); }
    const uint8_t* data() const noexcept { return reinterpret_cast<const uint8_t*>(owner.get()); }
};

// ICL SFC base+28 uses 4d4d0(400,200): one c00-byte allocation,
// Y-X/Y-Y/UV-X/UV-Y pointers at 10/18/20/28. Owned header only;
// never pass it to native 4d600, which would free our tracked allocation.
template<class Backend> class TglOwnedSfcAvsParameters {
    TglTrackedCpuOwner<Backend> coefficients;
    std::array<uint8_t,0x30> header{};
public:
    explicit TglOwnedSfcAvsParameters(Backend& backend) : coefficients(backend) {}
    bool initialize() {
        if (coefficients.get()) return true;
        if (!coefficients.allocate(0xc00)) return false;
        const uintptr_t base=coefficients.get();
        if (base>std::numeric_limits<uintptr_t>::max()-0xc00) {
            coefficients.reset(); return false;
        }
        header.fill(0);
        const uint32_t invalidFormat=0xffffffff;
        std::memcpy(header.data(),&invalidFormat,4);
        const std::array<uint64_t,4> pointers{{base,base+0x600,base+0x400,base+0xa00}};
        for (size_t i=0;i<pointers.size();++i)
            std::memcpy(header.data()+0x10+i*8,&pointers[i],8);
        return true;
    }
    void reset() noexcept { header.fill(0); coefficients.reset(); }
    const std::array<uint8_t,0x30>* parameters() const noexcept {
        return coefficients.get() ? &header : nullptr;
    }
};
// Darwin1df245/1df2c1: two linear byte buffers. Scalar dimensions only;
// do not copy ICL Format_Buffer3e into TGL (native TGL buffer enum3d).
inline bool tglOwnedSfcLineBufferSizes(TglOwnedSfcLineBufferSizes& output,
                                     uint32_t inputHeight, uint32_t scaledHeight) noexcept {
    constexpr auto max=std::numeric_limits<uint32_t>::max();
    if (!inputHeight || !scaledHeight || inputHeight>max/40 || scaledHeight>max/16)
        return false;
    output={inputHeight*40,scaledHeight*16};
    return true;
}
// Caller authenticates and retains the native execution object/image lease.
// Decode only closed scalar fields; preserve owned surface and child policy inputs.
template<class Read>
bool tglReadOwnedStateExecution(TglOwnedStateInputs& output, uintptr_t execution,
                                bool diArgument, Read read) {
    constexpr auto max=std::numeric_limits<uintptr_t>::max();
    if (!execution || execution>max-0xa0f) return false;
    std::array<uint8_t,0x12> bytes{}; // native execution+8..19
    uint32_t pipe=0;
    if (!read(execution+8,bytes.data(),bytes.size()) ||
        !read(execution+0xa0c,&pipe,4) || pipe>2) return false;
    auto candidate=output;
    candidate.pipe=pipe;
    candidate.di=diArgument;
    candidate.firstFrame=(bytes[0]&1)!=0;
    candidate.dn=(bytes[3]&1)!=0;
    candidate.chromaIeCp=(bytes[7]&1)!=0;
    candidate.exec12=(bytes[0xa]&1)!=0;
    candidate.chromaDi=(bytes[0xb]&1)!=0;
    candidate.exec14=(bytes[0xc]&1)!=0;
    candidate.exec19=(bytes[0x11]&1)!=0;
    output=candidate;
    return true;
}
// Pure owned-data composition of the closed Darwin state producer. Exact
// base+c8 (1e8390) reads the same exec0f as chroma IECP; primary+70 (1fa4b0)
// returns false. Do not expose contradictory duplicate callback inputs.
// This alone is not a live renderer implementation; other bytes start zero.
inline bool tglPrepareOwnedStatePacket(std::array<uint8_t,0x188>& output,
                                      const TglOwnedStateInputs& input) noexcept {
    if (input.pipe>2) return false;
    // Native mode selection/DN special can dereference currentSurface.
    if (!input.source) return false;
    uint32_t sourceType=0, source138=0;
    std::memcpy(&sourceType,input.source->prefix.data(),4);
    std::memcpy(&source138,input.source->prefix.data()+0x138,4);
    std::array<uint8_t,0x188> candidate{};
    uint32_t flags=0, mode=0, chroma=0;
    tglPrepareOwnedStateLeadingFlags(flags,input.pipe,input.chromaIeCp,input.di,input.dn);
    tglPrepareOwnedStateHistoryAndPipe(flags,input.firstFrame,input.dn,input.di,input.pipe,true);
    tglSelectOwnedStateMode(mode,input.pipe,input.exec19,input.exec14,flags,source138);
    tglPrepareOwnedStateDnControls(flags,mode,input.exec12,input.dnSpecial,sourceType);
    candidate[0xc]=0; // Exact primary+70 implementation1fa4b0..1fa4bd.
    tglPrepareOwnedChromaSampling(chroma,input.source,input.target,
                                 input.chromaIeCp,input.chromaDi,input.pipe);
    tglPrepareOwnedStateHistoryAndPipe(flags,input.firstFrame,input.dn,input.di,
                                     input.pipe,input.skuSuppressPipe);
    std::memcpy(candidate.data(),&flags,4);
    std::memcpy(candidate.data()+4,&chroma,4);
    output=candidate;
    return true;
}
// Exact Darwin caller1f9111/26 packs compressed bit0 and mode DWORD into
// an eight-byte parameter. TGL179530 consumes mode+4 and calls native OS1a0;
// do not replace the whole callback with a superficially equivalent bit OR.
struct TglSurfaceControlParams {
    uint8_t compressed = 0;
    std::array<uint8_t,3> padding{};
    uint32_t mode = 0;
};
static_assert(offsetof(TglSurfaceControlParams,mode) == 4);
static_assert(sizeof(TglSurfaceControlParams) == 8);
inline TglSurfaceControlParams tglSurfaceControlParams(
        const TglOwnedSurfaceStorage& surface) noexcept {
    TglSurfaceControlParams params;
    params.compressed=surface.tail[0xa]&1;
    std::memcpy(&params.mode,surface.tail.data()+0xc,4);
    return params;
}
struct TglNativeSurfaceControlInvoker {
    int operator()(uintptr_t entry, uintptr_t mhw,
            const TglOwnedSurfaceStorage& surface, uint32_t& value) const noexcept {
        using Control = int (*)(void*,const TglSurfaceControlParams*,uint32_t*);
        const auto params=tglSurfaceControlParams(surface);
        return reinterpret_cast<Control>(entry)(reinterpret_cast<void*>(mhw),&params,&value);
    }
};
// Hook78's borrowed surface inputs, not hook70's a8-byte DI/IECP state.
// Native52d60 consumes five pointers and flag28; builder's stack reserves30.
struct TglOwnedSurfaceInputs {
    std::array<const TglOwnedSurfaceStorage*,5> surfaces{};
    uint8_t di = 0;
    std::array<uint8_t,7> padding{};
};
static_assert(offsetof(TglOwnedSurfaceInputs,di) == 0x28);
static_assert(sizeof(TglOwnedSurfaceInputs) == 0x30);
// Hook70 packet: TGL MHW vtable759520+28 ->172080 independently proves
// resource20..70, paired control78..a0 and final DWORDa4. Preserve unknown
// prefix as bytes; this is storage, NOT a complete command producer.
// Resource pointers are borrowed: their owners must outlive native emission.
struct TglOwnedDiIecpPacket {
    std::array<uint8_t,0x20> prefix{};
    std::array<const std::array<uint8_t,0x148>*,11> resources{};
    std::array<uint32_t,12> controls{};
};
static_assert(offsetof(TglOwnedDiIecpPacket,resources) == 0x20);
static_assert(offsetof(TglOwnedDiIecpPacket,controls) == 0x78);
static_assert(sizeof(TglOwnedDiIecpPacket) == 0xa8);
// ICL producer1f9006/17 emits start=0/end=boundaryWidth-1, independently
// consumed as fourteen-bit values by TGL172a2e..68. Do not silently wrap
// malformed dimensions. This initializes storage only; subsequent feature,
// registration, compression and resource producers are still required.
inline bool tglInitializeDiIecpPacket(TglOwnedDiIecpPacket& output,
                                    uint32_t boundaryWidth) noexcept {
    if (!boundaryWidth || boundaryWidth > 0x4000) return false;
    TglOwnedDiIecpPacket candidate;
    const uint32_t end = boundaryWidth-1;
    std::memcpy(candidate.prefix.data(),&end,sizeof(end));
    output = candidate;
    return true;
}
struct TglNativeDiIecpBinding {
    uintptr_t context = 0, entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t mhw, uintptr_t image, Read read, Qualify qualify,
                 Executable executable) {
        *this = {};
        // Exact image's primary table and consumer, not an interchangeable
        // ICL callback. The caller leases the borrowed MHW object throughout.
        if (!mhw || !image || mhw > UINTPTR_MAX-7 ||
            image > UINTPTR_MAX-0x759550 || !qualify(image)) return false;
        uintptr_t table=0, native=0;
        constexpr std::array<uint8_t,11> expected{
            0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0x40,0x01,0x00,0x00};
        std::array<uint8_t,11> actual{};
        if (!read(mhw,&table,sizeof(table)) || table != image+0x759520 ||
            !read(table+0x28,&native,sizeof(native)) || native != image+0x172080 ||
            !executable(native,expected.size()) ||
            !read(native,actual.data(),actual.size()) || actual != expected) return false;
        context=mhw; entry=native;
        return true;
    }
};
struct TglNativeSurfaceControlBinding {
    uintptr_t context = 0, entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t mhw, uintptr_t image, Read read, Qualify qualify,
                 Executable executable) {
        *this = {};
        if (image > UINTPTR_MAX-0x759588) return false;
        TglNativeDiIecpBinding consumer;
        if (!consumer.resolve(mhw,image,read,qualify,executable)) return false;
        uintptr_t native=0;
        constexpr std::array<uint8_t,8> expected{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x50};
        std::array<uint8_t,8> actual{};
        if (!read(image+0x759580,&native,sizeof(native)) || native != image+0x179530 ||
            !executable(native,expected.size()) ||
            !read(native,actual.data(),actual.size()) || actual != expected) return false;
        context=mhw; entry=native;
        return true;
    }
};
// Native builder12a14c calls MHW+28 with exactly these three arguments.
// Binding/image authentication, backing and complete packet admission belong
// to the caller; this invoker neither creates resources nor changes status.
struct TglNativeDiIecpInvoker {
    int operator()(uintptr_t entry, uintptr_t mhw, void* commandBuffer,
                   const TglOwnedDiIecpPacket& packet) const noexcept {
        using Emit = int (*)(void*,void*,const TglOwnedDiIecpPacket*);
        return reinterpret_cast<Emit>(entry)(reinterpret_cast<void*>(mhw),
                                             commandBuffer,&packet);
    }
};
// Gen12 GetSurfOutput decision tree, independent of Darwin object offsets.
// TGL mode2/target60 is pinned separately; mode1 is SFC. Caller must decode
// and authenticate execution fields, and lease/back the selected surface.
struct TglOutputSurfaceSelection {
    const TglOwnedSurfaceStorage* surface = nullptr;
    bool valid = false;
};
struct TglSurfaceIndices {
    int32_t frame0 = 0, frame1 = 0, dnOut = 0, historyIn = 0, historyOut = 0;
};
struct TglDiIecpOutputs {
    const TglOwnedSurfaceStorage* current = nullptr;
    const TglOwnedSurfaceStorage* previous = nullptr;
    uint32_t currentOffset = 0;
};
// ICL1f89a0 output helper agrees with pinned Gen12 logical policy. Only
// packet48/50/10 are transported; never copy ICL C++ object offsets/vtables.
// Compression/control/registration remain separate required producer stages.
inline bool tglSelectDiIecpOutputs(TglDiIecpOutputs& output, uint32_t pipe,
        bool di, bool iecp, const TglSurfaceIndices& indices,
        const TglOwnedSurfaceStorage* target,
        const std::array<const TglOwnedSurfaceStorage*,4>& ffdi) noexcept {
    if (pipe > 2) return false;
    TglDiIecpOutputs candidate;
    if (pipe == 2) {
        if (!target) return false;
        candidate.current = target;
        std::memcpy(&candidate.currentOffset,target->prefix.data()+0x144,4);
    } else if (di) {
        if (indices.frame0 < 0 || indices.frame0 >= 4 ||
            indices.frame1 < 0 || indices.frame1 >= 4 ||
            !ffdi[indices.frame0] || !ffdi[indices.frame1]) return false;
        candidate.current = ffdi[indices.frame1];
        candidate.previous = ffdi[indices.frame0];
    } else if (iecp) {
        if (indices.dnOut < 0 || indices.dnOut >= 2 || !ffdi[indices.dnOut]) return false;
        candidate.current = ffdi[indices.dnOut];
    }
    output = candidate;
    return true;
}
struct TglDiIecpInputs {
    uint32_t boundaryWidth = 0, pipe = 0;
    bool di = false, iecp = false, referenceValid = false;
    bool dnNeeded = false, stmmNeeded = false;
    TglSurfaceIndices indices{};
    const TglOwnedSurfaceStorage* current = nullptr;
    const TglOwnedSurfaceStorage* previous = nullptr;
    const TglOwnedSurfaceStorage* target = nullptr;
    const TglOwnedSurfaceStorage* statistics = nullptr;
    std::array<const TglOwnedSurfaceStorage*,4> ffdi{};
    std::array<const TglOwnedSurfaceStorage*,2> ffdn{}, stmm{};
    std::array<uint32_t,11> controls{};
};
// Owned logical policy reconstructed from exact Darwin1f88d0/1f7500.
// Scalar decisions only; no ICL object offsets, vtables or GPU commands.
inline bool tglOwnedMmcFormatSupported(uint32_t format) noexcept {
    switch (format) {
        case 0x19: case 0xd: case 0xe: case 0x10: case 0xf: case 0x11:
        case 0x15: case 0x14: case 3: case 5: case 0x53: case 0x13: return true;
        default: return false;
    }
}
struct TglDiIecpControlPolicy {
    bool mmcEnabled = false;
    uint32_t executionMode = 0;
    const TglOwnedSurfaceStorage* target = nullptr;
    int operator()(const TglOwnedSurfaceStorage& surface,size_t slot,
                   bool directTarget,bool& enabled) const noexcept {
        enabled=false;
        if (slot > 6 || (directTarget && (slot != 5 || target != &surface))) return 5;
        if (!directTarget) { enabled=mmcEnabled; return 0; }
        uint32_t format=0;
        std::memcpy(&format,surface.prefix.data()+0x130,4);
        const auto params=tglSurfaceControlParams(surface);
        enabled=mmcEnabled && tglOwnedMmcFormatSupported(format) &&
                executionMode == 7 && params.mode == 1;
        return 0;
    }
};
// Producer orchestration, not native object-layout transplantation. Darwin
// ICL1f8ec0/1f89a0 supplies the branch/order evidence, TGL172080 the packet ABI.
// Caller leases every surface. Admit verifies backing/identity; control must
// implement qualified native compression policy, never silently omit it.
// Register/control side effects cannot be rolled back here (native also exits
// at first error), but no partial packet is published or emitted on failure.
template<class Admit, class Register, class Control>
int tglPrepareDiIecpPacket(TglOwnedDiIecpPacket& output,
        const TglDiIecpInputs& input, Admit admit, Register registerResource,
        Control control) {
    TglOwnedDiIecpPacket candidate;
    TglDiIecpOutputs selected;
    if (!tglInitializeDiIecpPacket(candidate,input.boundaryWidth) ||
        !tglSelectDiIecpOutputs(selected,input.pipe,input.di,input.iecp,
            input.indices,input.target,input.ffdi) || !input.current ||
        !input.statistics || (input.referenceValid && !input.previous)) return 5;
    struct Step { size_t slot; const TglOwnedSurfaceStorage* surface; bool write; bool directTarget; };
    std::array<Step,10> steps{};
    size_t count=0;
    auto add = [&](size_t slot,const TglOwnedSurfaceStorage* surface,bool write,bool directTarget=false) {
        steps[count++]={slot,surface,write,directTarget};
    };
    add(0,input.current,false);
    if (input.referenceValid) add(1,input.previous,false);
    if (selected.current) add(5,selected.current,true,input.pipe == 2);
    if (selected.previous) add(6,selected.previous,true);
    if (input.dnNeeded) {
        if (input.indices.dnOut < 0 || input.indices.dnOut >= 2 ||
            !input.ffdn[input.indices.dnOut]) return 5;
        add(4,input.ffdn[input.indices.dnOut],true);
        if (input.pipe == 1 && !input.di) {
            if (!input.ffdi[input.indices.dnOut]) return 5;
            add(5,input.ffdi[input.indices.dnOut],true);
        }
    }
    if (input.di || input.stmmNeeded) {
        if (input.indices.historyIn < 0 || input.indices.historyIn >= 2 ||
            input.indices.historyOut < 0 || input.indices.historyOut >= 2 ||
            !input.stmm[input.indices.historyIn] || !input.stmm[input.indices.historyOut]) return 5;
        add(2,input.stmm[input.indices.historyIn],false);
        add(3,input.stmm[input.indices.historyOut],true);
    }
    add(7,input.statistics,true);
    for (size_t i=0;i<count;++i) {
        const int result=admit(*steps[i].surface);
        if (result) return result;
    }
    uint32_t offset=0;
    std::memcpy(&offset,input.current->prefix.data()+0x144,4);
    std::memcpy(candidate.prefix.data()+8,&offset,4);
    if (input.referenceValid) {
        std::memcpy(&offset,input.previous->prefix.data()+0x144,4);
        std::memcpy(candidate.prefix.data()+0xc,&offset,4);
    }
    std::memcpy(candidate.prefix.data()+0x10,&selected.currentOffset,4);
    for (size_t i=0;i<count;++i) {
        const auto& step=steps[i];
        int result=registerResource(step.surface->resource,step.write,true);
        if (result) return result;
        uint32_t value=input.controls[step.slot];
        if (step.slot != 7) { // native statistics has no compression-control call
            result=control(*step.surface,value,step.slot,step.directTarget);
            if (result) return result;
        }
        candidate.resources[step.slot]=&step.surface->resource;
        candidate.controls[step.slot]=value;
    }
    output=candidate;
    return 0;
}
struct TglNativeDiIecpServices {
    TglNativeRegistrationBinding registration{};
    TglNativeSurfaceControlBinding control{};
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t os, uintptr_t mhw, uintptr_t image, Read read,
                 Qualify qualify, Executable executable) {
        *this={};
        TglNativeDiIecpServices candidate;
        if (!candidate.registration.resolve(os,image,read,qualify,executable) ||
            !candidate.control.resolve(mhw,image,read,qualify,executable)) return false;
        *this=candidate;
        return true;
    }
};
// The compression policy must distinguish native direct-output predicate80
// from global MMC policy for intermediate/input/history surfaces. No default
// "compression disabled" fallback; caller must supply qualified policy.
template<class Admit, class ControlPolicy>
int tglPrepareNativeDiIecpPacket(TglOwnedDiIecpPacket& output,
        const TglDiIecpInputs& input,const TglNativeDiIecpServices& services,
        Admit admit,ControlPolicy policy) {
    if (!services.registration.context || !services.registration.entry ||
        !services.control.context || !services.control.entry) return 5;
    return tglPrepareDiIecpPacket(output,input,admit,
        [&](const auto& resource,bool write,bool read) {
            return TglNativeRegistrationInvoker{}(services.registration.entry,
                services.registration.context,&resource,write,read);
        },
        [&](const auto& surface,uint32_t& value,size_t slot,bool directTarget) {
            bool enabled=false;
            const int result=policy(surface,slot,directTarget,enabled);
            if (result || !enabled) return result;
            return TglNativeSurfaceControlInvoker{}(services.control.entry,
                services.control.context,surface,value);
        });
}
// TGL12b9a0 writes these five execution fields. No pointer adoption or state
// mutation; failed reads/invalid indices preserve the caller's snapshot.
template<class Read>
bool tglReadSurfaceIndices(uintptr_t execution, TglSurfaceIndices& result, Read read) {
    if (!execution || execution > UINTPTR_MAX-0x33) return false;
    TglSurfaceIndices candidate;
    if (!read(execution+0x1c,&candidate.frame0,4) ||
        !read(execution+0x20,&candidate.frame1,4) ||
        !read(execution+0x28,&candidate.dnOut,4) ||
        !read(execution+0x2c,&candidate.historyIn,4) ||
        !read(execution+0x30,&candidate.historyOut,4)) return false;
    if (candidate.frame0 < 0 || candidate.frame0 >= 4 ||
        candidate.frame1 < 0 || candidate.frame1 >= 4 ||
        candidate.dnOut < 0 || candidate.dnOut >= 2 ||
        candidate.historyIn < 0 || candidate.historyIn >= 2 ||
        candidate.historyOut < 0 || candidate.historyOut >= 2) return false;
    result = candidate;
    return true;
}
inline TglOutputSurfaceSelection tglSelectOutputSurface(
        uint32_t pipe, bool di, bool iecp, bool denoise,
        int32_t frame0, int32_t dnOut, const TglOwnedSurfaceStorage* target,
        const std::array<const TglOwnedSurfaceStorage*,4>& ffdi,
        const std::array<const TglOwnedSurfaceStorage*,2>& ffdn) noexcept {
    if (pipe > 2) return {};
    if (pipe == 2) return {target,true};
    if (di) {
        if (frame0 < 0 || size_t(frame0) >= ffdi.size()) return {};
        return {ffdi[size_t(frame0)],true};
    }
    if (iecp) {
        if (dnOut < 0 || size_t(dnOut) >= ffdi.size()) return {};
        return {ffdi[size_t(dnOut)],true};
    }
    if (denoise) {
        if (dnOut < 0 || size_t(dnOut) >= ffdn.size()) return {};
        return {ffdn[size_t(dnOut)],true};
    }
    return {nullptr,pipe == 1}; // SFC intentionally writes no memory output
}

// Gen12 SetupSurfaceStates composition. This populates borrowed CPU shell
// pointers only; allocation/backing/feature admission remain separate gates.
// STMM and FFDN shells must exist even when that feature's backing is unused.
inline bool tglPrepareSurfaceInputs(
        TglOwnedSurfaceInputs& output, const TglOwnedSurfaceStorage* current,
        const TglOwnedSurfaceStorage* target, uint32_t pipe, bool di, bool iecp,
        bool denoise, const TglSurfaceIndices& indices,
        const std::array<const TglOwnedSurfaceStorage*,4>& ffdi,
        const std::array<const TglOwnedSurfaceStorage*,2>& ffdn,
        const std::array<const TglOwnedSurfaceStorage*,2>& stmm) noexcept {
    if (!current || indices.historyIn < 0 || indices.historyIn >= 2 ||
        indices.dnOut < 0 || indices.dnOut >= 2) return false;
    const auto selected = tglSelectOutputSurface(pipe,di,iecp,denoise,
        indices.frame0,indices.dnOut,target,ffdi,ffdn);
    if (!selected.valid || !stmm[size_t(indices.historyIn)] ||
        !ffdn[size_t(indices.dnOut)]) return false;
    // Only SFC without a memory-output feature intentionally selects null.
    if (!selected.surface && !(pipe == 1 && !di && !iecp && !denoise)) return false;
    TglOwnedSurfaceInputs candidate;
    candidate.surfaces = {current,selected.surface,stmm[size_t(indices.historyIn)],
                          ffdn[size_t(indices.dnOut)],nullptr};
    candidate.di = di ? 1 : 0;
    output = candidate;
    return true;
}
inline std::array<uint8_t,0x48> tglSurfaceDescriptor(
        const TglOwnedSurfaceStorage& shell) noexcept {
    return tglSurfaceDescriptorBytes(reinterpret_cast<const uint8_t*>(&shell));
}
inline std::array<uint8_t,0x170> tglSurfaceCommandSet(
        const std::array<const TglOwnedSurfaceStorage*,5>& surfaces,
        uint8_t di) noexcept {
    std::array<const uint8_t*,5> views{};
    for (size_t i=0; i<views.size(); ++i)
        if (surfaces[i]) views[i] = reinterpret_cast<const uint8_t*>(surfaces[i]);
    return tglSurfaceCommandSetBytes(views,di);
}
inline std::array<uint8_t,0x170> tglSurfaceCommandSet(
        const TglOwnedSurfaceInputs& inputs) noexcept {
    return tglSurfaceCommandSet(inputs.surfaces,inputs.di);
}
struct TglResourceStorageReference { std::array<uint8_t,0x148>& storage; };

// Native 52bf0 borrows surface+148 at descriptor+40. Both the surface and its
// backing owner must outlive every consumer of this descriptor.
struct TglNativeSurfaceConversionInvoker {
    int operator()(uintptr_t entry, const TglOwnedSurfaceStorage& surface,
                   std::array<uint8_t,0x48>& descriptor) const noexcept {
        using Convert = int (*)(const void*,void*);
        return reinterpret_cast<Convert>(entry)(&surface,descriptor.data());
    }
};

struct TglSurfaceConversionBinding {
    uintptr_t entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t image, Read read, Qualify qualify, Executable executable) {
        entry = 0;
        constexpr std::array<uint8_t,16> expected{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,
            0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0};
        std::array<uint8_t,16> actual{};
        if (!image || image > UINTPTR_MAX-0x52c00 || !qualify(image) ||
            !executable(image+0x52bf0,16) ||
            !read(image+0x52bf0,actual.data(),actual.size()) || actual != expected)
            return false;
        entry = image+0x52bf0;
        return true;
    }
};

// Gen12 AllocateResources finish calls FreeResources on ANY failure. This
// composes descriptor owners, not Linux object offsets or feature admission.
// Prepare handles per-resource initialization/metadata and preserves its error.
// Only a complete backed set is exposed; this proves neither GPU sync nor execution.
// SFC and app-owned LUT surfaces are separate owners, never adopted by this group.
template<class Backend, size_t Count> class TglOwnedResourceGroup {
    static_assert(Count > 0);
    using Resource = TglOwnedResource<Backend>;
    std::array<Resource,Count> resources;
    bool ready = false;
    bool bindingsValid = true;
    template<size_t... I>
    TglOwnedResourceGroup(Backend& backend, std::index_sequence<I...>) noexcept
        : resources{(static_cast<void>(I),Resource(backend))...} {}
    template<size_t... I>
    TglOwnedResourceGroup(std::array<Backend,Count>& backends, std::index_sequence<I...>) noexcept
        : resources{Resource(backends[I])...} {}
    template<size_t... I>
    TglOwnedResourceGroup(std::array<Backend,Count>& backends,
                         const std::array<TglResourceStorageReference,Count>& storage,
                         std::index_sequence<I...>) noexcept
        : resources{Resource(backends[I],storage[I].storage)...} {
        for (size_t i = 0; i < Count; ++i)
            for (size_t j = 0; j < i; ++j)
                if (&storage[i].storage == &storage[j].storage) bindingsValid = false;
    }
public:
    struct Request { bool required = false; TglResourceKey key{}; };
    explicit TglOwnedResourceGroup(Backend& backend) noexcept
        : TglOwnedResourceGroup(backend,std::make_index_sequence<Count>{}) {}
    // Surface and buffer allocators have different native resource types.
    // The complete backend array, and its borrowed OS context, outlive this group.
    explicit TglOwnedResourceGroup(std::array<Backend,Count>& backends) noexcept
        : TglOwnedResourceGroup(backends,std::make_index_sequence<Count>{}) {}
    TglOwnedResourceGroup(std::array<Backend,Count>& backends,
                         const std::array<TglResourceStorageReference,Count>& storage) noexcept
        : TglOwnedResourceGroup(backends,storage,std::make_index_sequence<Count>{}) {}
    TglOwnedResourceGroup(const TglOwnedResourceGroup&) = delete;
    TglOwnedResourceGroup& operator=(const TglOwnedResourceGroup&) = delete;
    template<class Prepare>
    int ensure(const std::array<Request,Count>& requests, Prepare prepare) noexcept {
        static_assert(noexcept(std::declval<Prepare&>()(size_t{},
            std::declval<const typename Resource::Storage&>(),bool{})),
            "resource initialization must report errors through status");
        ready = false;
        if (!bindingsValid) return 5;
        for (size_t i = 0; i < Count; ++i) {
            if (!requests[i].required) { resources[i].reset(); continue; }
            const auto result = resources[i].ensure(requests[i].key);
            if (result.status || result.state != Resource::State::Backed) {
                reset(); return result.status ? result.status : 5;
            }
            const int status = prepare(i,resources[i].storage(),result.changed);
            if (status) { reset(); return status; }
        }
        ready = true; return 0;
    }
    void reset() noexcept {
        ready = false;
        for (auto& resource : resources) resource.reset();
    }
    const typename Resource::Storage* storage(size_t i) const noexcept {
        return ready && i < Count && resources[i].state() == Resource::State::Backed
            ? &resources[i].storage() : nullptr;
    }
};

// Separate SFC owner: Darwin 1df210 allocates AVS then IEF and unwinds both
// on either failure. Backend must be configured for native resource type 0.
// TGL's buffer format is 3d (not ICL's 3e); linear tile is 4.
template<class Backend> class TglOwnedSfcLineBuffers {
    TglOwnedResourceGroup<Backend,2> owners;
public:
    explicit TglOwnedSfcLineBuffers(Backend& backend) noexcept : owners(backend) {}
    int ensure(uint32_t inputHeight, uint32_t scaledHeight) noexcept {
        TglOwnedSfcLineBufferSizes sizes;
        // Invalid dimensions do not retire an already valid allocation.
        if (!tglOwnedSfcLineBufferSizes(sizes,inputHeight,scaledHeight)) return 5;
        using Request=typename TglOwnedResourceGroup<Backend,2>::Request;
        const std::array<Request,2> requests{{
            {true,{sizes.avs,1,0x3d,4,0,false}},
            {true,{sizes.ief,1,0x3d,4,0,false}}
        }};
        return owners.ensure(requests,[](size_t,
            const typename TglOwnedResource<Backend>::Storage&,bool) noexcept { return 0; });
    }
    void reset() noexcept { owners.reset(); }
    const typename TglOwnedResource<Backend>::Storage* storage(size_t i) const noexcept {
        return owners.storage(i);
    }
};

// Gen12 statistics lifecycle, not a native fill ABI implementation. Backend
// must allocate a buffer; Fill must validate and initialize actual backing.
template<class Backend> class TglStatisticsResource {
public:
    explicit TglStatisticsResource(Backend &backend) noexcept : owner_(backend) {}
    template<class Fill>
    int ensure(uint32_t width, uint32_t height, Fill fill) noexcept {
        static_assert(noexcept(fill(owner_.storage(), uint32_t{})),
                      "initialization must report status without throwing");
        TglStatisticsAllocation candidate;
        const int shape = tglStatisticsAllocationSize(width, height, candidate);
        if (shape) return shape; // invalid request does not retire a valid resource
        const auto result = owner_.ensure({candidate.bytes, 1, 0x3e, 4, 0, false});
        if (result.status) { layout_ = {}; return result.status; }
        if (result.state != TglOwnedResource<Backend>::State::Backed) {
            reset(); return 5;
        }
        if (result.changed) {
            const int status = fill(owner_.storage(), candidate.bytes);
            if (status) { reset(); return status; }
        }
        layout_ = candidate;
        return 0;
    }
    void reset() noexcept { owner_.reset(); layout_ = {}; }
    const TglStatisticsAllocation &layout() const noexcept { return layout_; }
private:
    TglOwnedResource<Backend> owner_;
    TglStatisticsAllocation layout_{};
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
