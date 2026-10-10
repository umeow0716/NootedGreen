#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include "tgl_capability_adapter.hpp"
#include "descriptor_bridge.hpp"
#include "owned_resource.hpp"

static void testTrackedCpuOwner() {
    // Exercise the real function-pointer ABI only against owned test functions.
    static std::array<unsigned char,0x2a8> abiStorage{};
    static unsigned abiAllocations = 0, abiReleases = 0;
    auto allocateAbi = +[](size_t bytes) -> void* {
        assert(bytes == abiStorage.size()); ++abiAllocations;
        return abiStorage.data();
    };
    auto releaseAbi = +[](void* pointer) {
        assert(pointer == abiStorage.data()); ++abiReleases;
    };
    TglNativeTrackedCpuInvoker abi;
    assert(!abi.allocate(0,0x2a8));
    assert(!abi.allocate(reinterpret_cast<uintptr_t>(allocateAbi),0));
    abi.release(0,reinterpret_cast<uintptr_t>(abiStorage.data()));
    abi.release(reinterpret_cast<uintptr_t>(releaseAbi),0);
    assert(abiAllocations == 0 && abiReleases == 0);
    auto pointer = abi.allocate(reinterpret_cast<uintptr_t>(allocateAbi),0x2a8);
    assert(pointer == reinterpret_cast<uintptr_t>(abiStorage.data()) && abiAllocations == 1);
    abi.release(reinterpret_cast<uintptr_t>(releaseAbi),pointer);
    assert(abiReleases == 1);
    struct Backend {
        std::array<unsigned char,0x2a8> bytes{};
        unsigned allocations = 0, releases = 0, live = 0;
        bool fail = false;
        uintptr_t allocate(size_t n) {
            ++allocations; assert(n == bytes.size());
            if (fail) return 0;
            bytes.fill(0); ++live; return reinterpret_cast<uintptr_t>(bytes.data());
        }
        void release(uintptr_t p) {
            assert(p == reinterpret_cast<uintptr_t>(bytes.data()) && live == 1);
            ++releases; --live;
        }
    } backend;
    {
        TglTrackedCpuOwner<Backend> owner(backend);
        assert(!owner.allocate(0) && backend.allocations == 0);
        backend.fail = true; assert(!owner.allocate(0x2a8));
        assert(!owner.get() && !owner.size() && !backend.live && !backend.releases);
        backend.fail = false; assert(owner.allocate(0x2a8));
        assert(owner.get() && owner.size() == 0x2a8 && backend.live == 1);
        const auto attempts = backend.allocations;
        assert(!owner.allocate(0x2a8) && backend.allocations == attempts);
        for (auto byte : backend.bytes) assert(byte == 0);
        owner.reset(); owner.reset();
        assert(backend.releases == 1 && !backend.live && !owner.get() && !owner.size());
        assert(owner.allocate(0x2a8));
    }
    assert(backend.releases == 2 && backend.live == 0);
    constexpr uintptr_t image = 0x1000000;
    std::array<uint8_t,16> alloc{0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,
        0x48,0x89,0x7d,0xf8,0x48,0x8b,0x7d,0xf8};
    auto freed = alloc; freed[13] = 0x83;
    bool identity = true, rx = true;
    auto read = [&](uintptr_t p, void* out, size_t n) {
        assert(n == 16);
        if (p != image+0x437e0 && p != image+0x436d0) return false;
        std::memcpy(out,p == image+0x437e0 ? alloc.data() : freed.data(),n); return true;
    };
    auto qualify = [&](uintptr_t p) { return identity && p == image; };
    auto executable = [&](uintptr_t, size_t) { return rx; };
    TglTrackedCpuBinding binding;
    assert(binding.resolve(image,read,qualify,executable));
    alloc[0] = 0; assert(!binding.resolve(image,read,qualify,executable)); alloc[0] = 0x55;
    freed[0] = 0; assert(!binding.resolve(image,read,qualify,executable)); freed[0] = 0x55;
    identity = false; assert(!binding.resolve(image,read,qualify,executable)); identity = true;
    rx = false; assert(!binding.resolve(image,read,qualify,executable));
    assert(!binding.image && !binding.allocate && !binding.release);
    struct Invoke {
        Backend* backend;
        uintptr_t allocate(uintptr_t entry, size_t n) {
            assert(entry == 0x1000000+0x437e0); return backend->allocate(n);
        }
        void release(uintptr_t entry, uintptr_t p) {
            assert(entry == 0x1000000+0x436d0); backend->release(p);
        }
    };
    TglTrackedCpuBackend<Invoke> denied(image,read,qualify,executable,Invoke{&backend});
    assert(!denied.bound());
    const auto attempts = backend.allocations;
    assert(!denied.allocate(0x2a8) && backend.allocations == attempts);
    rx = true;
    TglTrackedCpuBackend<Invoke> native(image,read,qualify,executable,Invoke{&backend});
    assert(native.bound());
    {
        TglTrackedCpuOwner<TglTrackedCpuBackend<Invoke>> owner(native);
        assert(owner.allocate(0x2a8) && backend.live == 1);
    }
    assert(backend.live == 0 && backend.releases == 3);
}

static void testOwnedVeboxVtable() {
    constexpr uintptr_t image = 0x1000000;
    std::array<uintptr_t,TglOwnedVeboxVtable::count> original{};
    for (size_t i = 0; i < original.size(); ++i) original[i] = image+0x2000+i*8;
    for (size_t offset : TglOwnedVeboxVtable::missing) original[offset/8] = 0;
    original[0] = image+0x12e240; original[1] = image+0x12e250;
    original[0x1b8/8] = image+0x71e80;
    std::array<uintptr_t,20> hooks{};
    for (size_t i = 0; i < hooks.size(); ++i) hooks[i] = 0x2000+i*16;
    bool identity = true, rx = true;
    auto read = [&](uintptr_t p, void* out, size_t n) {
        assert(p == image+0x757c98 && n == 0x1e8); // never next table metadata
        std::memcpy(out,original.data(),n); return true;
    };
    auto qualify = [&](uintptr_t p) { return identity && p == image; };
    auto executable = [&](uintptr_t, size_t n) { assert(n == 1); return rx; };
    TglOwnedVeboxVtable table;
    auto build = [&] { return table.build(image,hooks,read,qualify,executable); };
    assert(build());
    for (size_t i = 0; i < original.size(); ++i) {
        auto expected = original[i];
        for (size_t j = 0; j < hooks.size(); ++j)
            if (TglOwnedVeboxVtable::required[j]/8 == i) expected = hooks[j];
        assert(table.entries[i] == expected);
    }
    for (size_t i = 0; i < hooks.size(); ++i) {
        auto saved = hooks[i]; hooks[i] = 0;
        assert(!build());
        for (auto value : table.entries) assert(value == 0);
        hooks[i] = saved;
    }
    hooks[0] = image+0x12e240; assert(!build()); hooks[0] = 0x2000;
    original[0x28/8] = image+0x1000; assert(!build()); original[0x28/8] = 0;
    original[0x48/8] = 0; assert(!build()); original[0x48/8] = image+0x2048;
    original[0x48/8] = 0x4000; assert(!build()); original[0x48/8] = image+0x2048;
    rx = false; assert(!build()); rx = true;
    identity = false; assert(!build()); identity = true;
    assert(build());
}

static void testNativeCscBinding() {
    constexpr uintptr_t image = 0x1000000, child = 0x2000, vt = 0x3000;
    std::array<uint8_t,16> code{0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,
        0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0};
    std::array<std::pair<uintptr_t,uintptr_t>,11> memory{{
        {child,vt},{vt+0x48,image+0x12ea30},{vt+0x90,0x4000},{vt+0x1b8,0x5000},
        {child+0x88,0x6000},{0x6000,image+0x757e90},{0x6d38,0x7000},
        {0x6d40,0x8000},{0x7000,image+0x757f78},{0x8000,image+0x757fb8},{0x9000,0xb}}};
    auto read = [&](uintptr_t p, void* out, size_t n) {
        if (p == image+0x12cfc0 && n == 16) { std::memcpy(out,code.data(),n); return true; }
        for (const auto& entry : memory) if (p == entry.first && n <= 8) {
            std::memcpy(out,&entry.second,n); return true;
        }
        if (p == 0xa000 && n == 4) { uint32_t cs = 3; std::memcpy(out,&cs,4); return true; }
        return false;
    };
    bool identity = true, rx = true;
    auto qualify = [&](uintptr_t p) { return identity && p == image; };
    auto executable = [&](uintptr_t, size_t) { return rx; };
    TglTwoPassCscBinding binding;
    assert(binding.resolve(image,read,qualify,executable));
    unsigned calls = 0; bool nativeResult = false, needed = true;
    auto invoke = [&](uintptr_t e,uintptr_t c,uintptr_t s,uintptr_t t) {
        assert(e == image+0x12cfc0 && c == child && s == 0x9000 && t == 0xa000);
        ++calls; return nativeResult;
    };
    auto query = [&] {
        return tglQueryNativeTwoPassCsc(binding,child,0x9000,0xa000,vt,0x4000,0x5000,
            needed,read,qualify,executable,invoke);
    };
    assert(query() && !needed && calls == 1);
    nativeResult = true; assert(query() && needed && calls == 2);
    for (size_t index : {size_t(0),size_t(1),size_t(2),size_t(3),size_t(4),size_t(5),size_t(8)}) {
        const auto original = memory[index].second; memory[index].second = 0;
        assert(!query() && needed && calls == 2); memory[index].second = original;
    }
    code[0] = 0; assert(!query() && needed && calls == 2); code[0] = 0x55;
    memory[3].second = image+0x71e80; // inherited base FALSE cannot masquerade as Gen12 hook
    assert(!query() && needed && calls == 2); memory[3].second = 0x5000;
    rx = false; assert(!query() && needed && calls == 2); rx = true;
    identity = false; assert(!query() && needed && calls == 2); identity = true;
    binding.entry++; assert(!query() && needed && calls == 2); binding.entry--;
    assert(!tglQueryNativeTwoPassCsc(binding,child,UINTPTR_MAX-0x132,0xa000,
        vt,0x4000,0x5000,needed,read,qualify,executable,invoke) && needed && calls == 2);
    assert(!tglQueryNativeTwoPassCsc(binding,child,0x9000,UINTPTR_MAX-2,
        vt,0x4000,0x5000,needed,read,qualify,executable,invoke) && needed && calls == 2);
    assert(!binding.resolve(0,read,qualify,executable) && !binding.image && !binding.entry);
}

static void testDirectVeboxFeasibility() {
    std::array<unsigned char, 0xf8> params{};
    std::array<unsigned char, 0x294> source{};
    std::array<unsigned char, 0x134> target{};
    std::array<unsigned char, 8> alpha{};
    const uint32_t one = 1, nv12 = 0x19;
    const uintptr_t targetAddress = 0x3000;
    const TglSurfaceRect rect{0,0,1920,1080};
    std::memcpy(params.data()+0x90, &one, 4);
    std::memcpy(params.data()+0x98, &targetAddress, 8);
    for (size_t offset : {0x30u,0x40u,0x50u}) std::memcpy(source.data()+offset, &rect, 16);
    std::memcpy(target.data()+0x40, &rect, 16);
    std::memcpy(source.data()+0x130, &nv12, 4);
    std::memcpy(target.data()+0x130, &nv12, 4);
    uintptr_t failed = 0;
    std::array<uintptr_t,32> reads{};
    size_t count = 0;
    auto read = [&](uintptr_t p, void* out, size_t n) {
        if (count < reads.size()) reads[count++] = p;
        if (p == failed) return false;
        auto copy = [&](uintptr_t base, const auto& bytes) {
            if (p < base || p-base > bytes.size() || n > bytes.size()-(p-base)) return false;
            std::memcpy(out, bytes.data()+p-base, n); return true;
        };
        return copy(0x1000,params) || copy(0x2000,source) || copy(0x3000,target) || copy(0x4000,alpha);
    };
    unsigned queries = 0;
    bool queryOk = true, twoPass = false;
    auto query = [&](uintptr_t s, uintptr_t t, bool& needed) {
        assert(s == 0x2000 && t == 0x3000); ++queries;
        needed = twoPass; return queryOk;
    };
    auto feasible = [&] { return tglDirectVeboxFeasible(0x1000,0x2000,0x3000,1,read,query); };
    assert(feasible() && queries == 1);
    const auto baselineReads = reads; const auto baselineCount = count;
    for (size_t i = 0; i < baselineCount; ++i) {
        failed = baselineReads[i]; count = 0;
        assert(!feasible());
    }
    failed = 0; queries = 0; count = 0;
    assert(!tglDirectVeboxFeasible(0x1000,0x2000,0x3000,0,read,query));
    assert(count == 0 && queries == 0);
    for (size_t offset : {0x78u,0x138u,0x290u,0x83u}) {
        source[offset] = 1; assert(!feasible()); source[offset] = 0;
    }
    queryOk = false; assert(!feasible()); queryOk = true;
    twoPass = true; assert(!feasible()); twoPass = false;
    const uintptr_t alphaAddress = 0x4000;
    std::memcpy(params.data()+0xf0, &alphaAddress, 8);
    assert(feasible());
    alpha[4] = 2; assert(!feasible()); alpha[4] = 1; assert(feasible());
    failed = 0x4004; assert(!feasible()); failed = 0;
    const uintptr_t overflowingAlpha = UINTPTR_MAX - 6;
    std::memcpy(params.data()+0xf0, &overflowingAlpha, 8);
    assert(!feasible());
    std::memcpy(params.data()+0xf0, &alphaAddress, 8);
    source[0x130] = 0; queries = 0;
    assert(!feasible() && queries == 0); source[0x130] = 0x19;
    target[0x130] = 0; queries = 0;
    assert(!feasible() && queries == 0); target[0x130] = 0x19;
    params[0x98] = 1; assert(!feasible()); // caller cannot substitute a different target
}

static void testVeboxHardwareBinding() {
    const TglSurfaceRect full{0,0,1920,1080};
    const TglSurfaceRect larger{-1,-1,1921,1081};
    assert(tglDirectVeboxGeometry(full, full, full, full));
    assert(tglDirectVeboxGeometry(full, full, larger, full)); // containment, not ICL equality
    assert(!tglDirectVeboxGeometry(full, full, {1,0,1921,1080}, full));
    assert(!tglDirectVeboxGeometry(full, {0,0,1280,720}, larger, full));
    assert(!tglDirectVeboxGeometry(full, full, larger, {0,0,1280,720}));
    assert(!tglDirectVeboxGeometry({1,0,1921,1080}, full, larger, full));
    assert(!tglDirectVeboxGeometry(full, {1,0,1921,1080}, larger, full));
    const TglSurfaceRect fractional{0,0,1920.5f,1080.5f};
    assert(tglDirectVeboxGeometry(fractional, fractional, larger, fractional));
    uintptr_t failedRect = 0;
    unsigned rectReads = 0;
    auto rectRead = [&](uintptr_t p, void* out, size_t n) {
        ++rectReads;
        assert(n == 16 && (p == 0xa030 || p == 0xa040 || p == 0xa050 || p == 0xb040));
        if (p == failedRect) return false;
        std::memcpy(out, p == 0xa050 ? &larger : &full, n); return true;
    };
    assert(tglDirectVeboxSurfaceGeometry(0xa000, 0xb000, rectRead) && rectReads == 4);
    for (uintptr_t p : {0xa030u,0xa040u,0xa050u,0xb040u}) {
        failedRect = p;
        assert(!tglDirectVeboxSurfaceGeometry(0xa000, 0xb000, rectRead));
    }
    rectReads = 0;
    assert(!tglDirectVeboxSurfaceGeometry(0, 0xb000, rectRead));
    assert(!tglDirectVeboxSurfaceGeometry(UINTPTR_MAX - 0x5e, 0xb000, rectRead));
    assert(!tglDirectVeboxSurfaceGeometry(0xa000, UINTPTR_MAX - 0x4e, rectRead));
    assert(rectReads == 0);
    for (float bad : {std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()}) {
        assert(!tglDirectVeboxGeometry(full, full, {0,0,bad,1080}, full));
        assert(!tglDirectVeboxGeometry({0,0,bad,1080}, full, larger, full));
        assert(!tglDirectVeboxGeometry(full, {0,0,bad,1080}, larger, full));
        assert(!tglDirectVeboxGeometry(full, full, larger, {0,0,bad,1080}));
    }
    constexpr uint32_t genericFormats[]{0x0d,0x0e,0x0f,0x10,0x11,0x12,0x13,
        0x14,0x15,0x17,0x19,0x52,0x53,0x58,0x59,0x4a,0x4c,0x4d,uint32_t(-7)};
    for (uint32_t format : genericFormats) assert(tglVeboxFormatSupported(format));
    assert(!tglVeboxFormatSupported(0x16)); // AUYV is not in Gen12 input rule
    assert(!tglVeboxFormatSupported(0) && !tglVeboxFormatSupported(UINT32_MAX));
    assert(!tglVeboxFormatSupported(0x10019));
    // DI predicate alone permits formats that generic admission must reject.
    assert(tglVeboxDiFormatSupported(UINT32_MAX));
    assert(!(tglVeboxFormatSupported(UINT32_MAX) && tglVeboxDiFormatSupported(UINT32_MAX)));
    constexpr uint32_t dnFormats[]{0x0e,0x11,0x0f,0x10,0x0d,0x4a,0x19,
        0x15,0x12,0x13,0x14,0x17,0x59,0x53,0x52,0x03,0x05};
    unsigned supported = 0;
    for (uint32_t format = 0; format < 256; ++format)
        supported += tglVeboxDnFormatSupported(format);
    assert(supported == 17);
    for (uint32_t format : dnFormats) assert(tglVeboxDnFormatSupported(format));
    assert(!tglVeboxDnFormatSupported(UINT32_MAX));
    assert(!tglVeboxDnFormatSupported(0x10019)); // no enum truncation to NV12
    unsigned formatReads = 0;
    uint32_t sourceFormat = 0x19;
    bool formatReadable = true;
    auto formatRead = [&](uintptr_t p, void* out, size_t n) {
        ++formatReads;
        assert(p == 0x5130 && n == 4);
        if (!formatReadable) return false;
        std::memcpy(out, &sourceFormat, n); return true;
    };
    assert(!tglVeboxDnSurfaceSupported(0, formatRead));
    assert(!tglVeboxDnSurfaceSupported(UINTPTR_MAX - 0x132, formatRead));
    assert(formatReads == 0);
    assert(tglVeboxDnSurfaceSupported(0x5000, formatRead) && formatReads == 1);
    assert(tglVeboxSurfaceSupported(0x5000, formatRead) && formatReads == 2);
    assert(!tglVeboxSurfaceSupported(0, formatRead) && formatReads == 2);
    assert(!tglVeboxSurfaceSupported(UINTPTR_MAX - 0x132, formatRead) && formatReads == 2);
    unsigned predicateCalls = 0;
    auto countedPredicate = [&](uint32_t) { ++predicateCalls; return true; };
    formatReadable = false;
    assert(!tglVeboxSurfaceFormatPredicate(0x5000, formatRead, countedPredicate));
    assert(predicateCalls == 0); // unreadable data must never reach format policy
    formatReadable = true;
    assert(tglVeboxSurfaceFormatPredicate(0x5000, formatRead, countedPredicate));
    assert(predicateCalls == 1);
    sourceFormat = UINT32_MAX;
    assert(!tglVeboxDnSurfaceSupported(0x5000, formatRead));
    assert(!tglVeboxSurfaceSupported(0x5000, formatRead));
    for (uint32_t src : {0x52u, 0x53u}) {
        for (uint32_t dst : {3u, 4u, 0x50u}) {
            assert(tglVeboxRtFormatSupported(0x0b, src, dst));
            assert(!tglVeboxRtFormatSupported(0, src, dst));
        }
    }
    for (uint32_t dst : {1u, 2u, 5u, 0x51u, UINT32_MAX})
        assert(!tglVeboxRtFormatSupported(0x0b, 0x53, dst));
    assert(!tglVeboxRtFormatSupported(0x0b, 0x19, 3));
    unsigned rtReads = 0;
    bool rtReadable = true;
    uint32_t rtFormat = 3;
    auto rtRead = [&](uintptr_t p, void* out, size_t n) {
        ++rtReads;
        assert(n == 4);
        uint32_t value = p == 0x6130 ? rtFormat : p == 0x5000 ? 0x0b : 0x53;
        assert(p == 0x6130 || p == 0x5000 || p == 0x5130);
        if (!rtReadable) return false;
        std::memcpy(out, &value, n); return true;
    };
    assert(!tglVeboxRtSurfaceSupported(0, 0x6000, rtRead));
    assert(!tglVeboxRtSurfaceSupported(0x5000, UINTPTR_MAX - 0x132, rtRead));
    assert(rtReads == 0);
    assert(tglVeboxRtSurfaceSupported(0x5000, 0x6000, rtRead) && rtReads == 3);
    rtFormat = 0x19; rtReads = 0;
    assert(tglVeboxRtSurfaceSupported(0x5000, 0x6000, rtRead) && rtReads == 1);
    rtReadable = false;
    assert(!tglVeboxRtSurfaceSupported(0x5000, 0x6000, rtRead));
    rtFormat = 3; rtReadable = true;
    for (uintptr_t failed : {uintptr_t(0x5000), uintptr_t(0x5130)}) {
        auto failSourceRead = [&](uintptr_t p, void* out, size_t n) {
            return p != failed && rtRead(p, out, n);
        };
        assert(!tglVeboxRtSurfaceSupported(0x5000, 0x6000, failSourceRead));
    }
    constexpr uint32_t diExcluded[]{0x15,0x14,0x17,0x03,0x01,0x51,0x50,0x05,0x06};
    for (uint32_t format : diExcluded) assert(!tglVeboxDiFormatSupported(format));
    assert(tglVeboxDiFormatSupported(0x19)); // NV12
    assert(tglVeboxDiFormatSupported(0x52) && tglVeboxDiFormatSupported(0x53));
    assert(!tglVeboxDiSurfaceSupported(0, formatRead));
    sourceFormat = 0x19; formatReadable = true;
    assert(tglVeboxDiSurfaceSupported(0x5000, formatRead));
    sourceFormat = 0x17; assert(!tglVeboxDiSurfaceSupported(0x5000, formatRead));
    formatReadable = false; assert(!tglVeboxDiSurfaceSupported(0x5000, formatRead));
    sourceFormat = 0x19; formatReadable = false;
    assert(!tglVeboxDnSurfaceSupported(0x5000, formatRead));
    std::array<unsigned char, 0xe0> pipeParams{};
    std::array<unsigned char, 0x83> pipeSurface{};
    uint32_t oneSource = 1;
    std::memcpy(pipeParams.data(), &oneSource, 4);
    std::memcpy(pipeParams.data() + 0x90, &oneSource, 4);
    uint8_t compNeeded = 0;
    uintptr_t failedPipeRead = 0;
    unsigned pipeReads = 0;
    auto pipeRead = [&](uintptr_t p, void* out, size_t n) {
        ++pipeReads;
        if (p == failedPipeRead) return false;
        if (p == 0x9000 && n == 1) { std::memcpy(out, &compNeeded, 1); return true; }
        if (p >= 0x7000 && p - 0x7000 + n <= pipeParams.size()) {
            std::memcpy(out, pipeParams.data() + p - 0x7000, n); return true;
        }
        if (p >= 0x8000 && p - 0x8000 + n <= pipeSurface.size()) {
            std::memcpy(out, pipeSurface.data() + p - 0x8000, n); return true;
        }
        assert(false); return false;
    };
    assert(tglCompositionBypassFeasible(0x7000, 0x8000, 0x9000, pipeRead));
    assert(pipeReads == 8);
    for (uintptr_t p : {0x9000u,0x7000u,0x7090u,0x8060u,0x8081u,0x8082u,0x8068u,0x70d8u}) {
        failedPipeRead = p;
        assert(!tglCompositionBypassFeasible(0x7000, 0x8000, 0x9000, pipeRead));
    }
    failedPipeRead = 0;
    for (size_t offset : {size_t(0x60),size_t(0x81),size_t(0x82),size_t(0x68)}) {
        pipeSurface[offset] = 1;
        assert(!tglCompositionBypassFeasible(0x7000, 0x8000, 0x9000, pipeRead));
        pipeSurface[offset] = 0;
    }
    for (size_t offset : {size_t(0),size_t(0x90),size_t(0xd8)}) {
        auto original = pipeParams[offset]; pipeParams[offset] = offset == 0xd8 ? 1 : 2;
        assert(!tglCompositionBypassFeasible(0x7000, 0x8000, 0x9000, pipeRead));
        pipeParams[offset] = original;
    }
    compNeeded = 1;
    assert(!tglCompositionBypassFeasible(0x7000, 0x8000, 0x9000, pipeRead));
    compNeeded = 2; pipeSurface[0x81] = 2; pipeSurface[0x82] = 2;
    assert(tglCompositionBypassFeasible(0x7000, 0x8000, 0x9000, pipeRead)); // native bit0
    pipeReads = 0;
    assert(!tglCompositionBypassFeasible(UINTPTR_MAX - 0xde, 0x8000, 0x9000, pipeRead));
    assert(!tglCompositionBypassFeasible(0x7000, 0, 0x9000, pipeRead));
    assert(!tglCompositionBypassFeasible(0x7000, UINTPTR_MAX - 0x81, 0x9000, pipeRead));
    assert(!tglCompositionBypassFeasible(0x7000, 0x8000, 0, pipeRead));
    assert(pipeReads == 0);
    constexpr uintptr_t image = 0x1000000, mhw = 0x2000, os = 0x3000;
    uintptr_t vt = image + 0x759520, actualOs = os, heap = 0x4000;
    bool identity = true, readable = true;
    auto read = [&](uintptr_t p, void* out, size_t n) {
        const auto value = p == mhw ? &vt : p == mhw + 0x18 ? &actualOs :
                           p == mhw + 0x20 ? &heap : nullptr;
        if (!readable || !value || n != 8) return false;
        std::memcpy(out, value, n); return true;
    };
    auto qualify = [&](uintptr_t p) { return identity && p == image; };
    TglVeboxHardwareBinding b;
    auto resolve = [&] { return b.resolve(mhw, os, image, read, qualify); };
    assert(resolve() && b.interface == mhw && b.os == os && b.heap == heap);
    auto empty = [&] { assert(!b.interface && !b.os && !b.heap); };
    ++vt; assert(!resolve()); empty(); --vt;
    ++actualOs; assert(!resolve()); empty(); --actualOs;
    heap = 0; assert(!resolve()); empty(); heap = 0x4000;
    identity = false; assert(!resolve()); empty(); identity = true;
    readable = false; assert(!resolve()); empty(); readable = true;
    assert(!b.resolve(UINTPTR_MAX, os, image, read, qualify)); empty();
    assert(!b.resolve(mhw, os, UINTPTR_MAX, read, qualify)); empty();
    assert(!b.resolve(mhw, 0, image, read, qualify)); empty();
    assert(resolve());
    std::array<uint8_t, 16> assignAnchor{
        0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,
        0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8};
    bool rx = true;
    unsigned calls = 0;
    uint32_t nativeResult = 0;
    uint32_t stateCount = 4, nextState = 0;
    auto assignRead = [&](uintptr_t p, void* out, size_t n) {
        if (n == 4 && (p == mhw + 0x28 || p == heap + 4)) {
            std::memcpy(out, p == mhw + 0x28 ? &stateCount : &nextState, 4); return true;
        }
        if (p == image + 0xfefa0 && n == assignAnchor.size()) {
            std::memcpy(out, assignAnchor.data(), n); return true;
        }
        return read(p, out, n);
    };
    auto executable = [&](uintptr_t p, size_t n) {
        return rx && p == image + 0xfefa0 && n == assignAnchor.size();
    };
    auto invoke = [&](uintptr_t p, uintptr_t object) {
        assert(p == image + 0xfefa0 && object == mhw); ++calls; return nativeResult;
    };
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 0 && calls == 1);
    nativeResult = 31;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 31 && calls == 2);
    rx = false;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 5 && calls == 2);
    rx = true; assignAnchor[0] ^= 1;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 5 && calls == 2);
    assignAnchor[0] ^= 1; ++heap;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 5 && calls == 2);
    --heap;
    stateCount = 0;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 5 && calls == 2);
    stateCount = 4; nextState = 4;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 5 && calls == 2);
    nextState = 0; stateCount = UINT32_MAX;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 5 && calls == 2);
    stateCount = 4;
    uint32_t instanceBytes = 4096, current = 3;
    auto readHeap = [&](uintptr_t p, void* out, size_t n) {
        const auto value = p == heap ? &current : p == heap + 0x2c ? &instanceBytes : nullptr;
        if (!value || n != 4) return false;
        std::memcpy(out, value, 4); return true;
    };
    TglVeboxHardwareBinding::StateResource state;
    assert(b.selectStateResource(false, 0, false, readHeap, state));
    assert(state.resource == heap + 0x40 && state.instanceOffset == 12288);
    assert(b.selectStateResource(false, 0, true, readHeap, state));
    assert(state.resource == heap + 0x188 && state.instanceOffset == 12288);
    current = 0;
    assert(b.selectStateResource(false, 0, false, readHeap, state));
    assert(state.resource == heap + 0x40 && state.instanceOffset == 0);
    current = 3;
    assert(b.selectStateResource(true, 0x9000, true, readHeap, state));
    assert(state.resource == 0x9000 && state.instanceOffset == 0);
    assert(!b.selectStateResource(true, 0, false, readHeap, state));
    assert(!state.resource && !state.instanceOffset);
    instanceBytes = UINT32_MAX; current = 2;
    assert(!b.selectStateResource(false, 0, false, readHeap, state));
    assert(!state.resource && !state.instanceOffset);
    current = 1;
    assert(b.selectStateResource(false, 0, false, readHeap, state));
    assert(state.instanceOffset == UINT32_MAX);
    auto deny = [](uintptr_t, void*, size_t) { return false; };
    assert(!b.selectStateResource(false, 0, false, deny, state));
    assert(!state.resource && !state.instanceOffset);
    uint32_t capacity = 16384, type = 0;
    instanceBytes = 4096;
    uintptr_t handle = 7, address = 0;
    state = {heap + 0x40, 12288};
    auto resourceRead = [&](uintptr_t p, void* out, size_t n) {
        if (p == heap + 0x2c && n == 4) {
            std::memcpy(out, &instanceBytes, 4); return true;
        }
        if (n == 4 && (p == state.resource + 0x10 || p == state.resource + 0x14)) {
            std::memcpy(out, p == state.resource + 0x10 ? &capacity : &type, 4); return true;
        }
        if (n == 8 && (p == state.resource + 0x20 || p == state.resource + 0x50)) {
            std::memcpy(out, p == state.resource + 0x20 ? &handle : &address, 8); return true;
        }
        return false;
    };
    assert(b.heapStateRange(state, 4096, resourceRead));
    assert(!b.heapStateRange(state, 4097, resourceRead));
    // Native174cba adds the heap region offset before clearing 0x800 bytes.
    assert(b.heapStateRange(state, 0x800, resourceRead, 0x800));
    assert(!b.heapStateRange(state, 0x800, resourceRead, 0x801));
    assert(!b.heapStateRange(state, 0x800, resourceRead, UINT32_MAX));
    state.instanceOffset = 0; // plenty of total capacity, but not this instance
    assert(b.heapStateRange(state, 4096, resourceRead));
    assert(!b.heapStateRange(state, 4097, resourceRead));
    state.instanceOffset = 1;
    assert(!b.heapStateRange(state, 1, resourceRead));
    state.instanceOffset = 12288;
    instanceBytes = 0; assert(!b.heapStateRange(state, 1, resourceRead));
    instanceBytes = 4096;
    handle = 0; assert(!b.heapStateRange(state, 4096, resourceRead));
    address = 0x9000; assert(b.heapStateRange(state, 4096, resourceRead));
    type = 1; assert(!b.heapStateRange(state, 4096, resourceRead)); type = 0;
    assert(!b.heapStateRange(state, 0, resourceRead));
    assert(!b.heapStateRange(state, 4096, deny));
    state = {heap + 0x188, 12288};
    assert(b.heapStateRange(state, 4096, resourceRead));
    // Every required descriptor read must succeed; no partial backing proof.
    for (uintptr_t field : {uintptr_t(0x10), uintptr_t(0x14),
                            uintptr_t(0x20), uintptr_t(0x50)}) {
        auto failField = [&](uintptr_t p, void* out, size_t n) {
            return p != state.resource + field && resourceRead(p, out, n);
        };
        assert(!b.heapStateRange(state, 4096, failField));
    }
    state.instanceOffset = UINT32_MAX;
    assert(!b.heapStateRange(state, 4096, resourceRead));
    state = {0x9000, 0}; // external CM surface is explicitly not this ABI
    assert(!b.heapStateRange(state, 4096, resourceRead));
    auto composedRead = [&](uintptr_t p, void* out, size_t n) {
        if (readHeap(p, out, n)) return true;
        for (uintptr_t offset : {uintptr_t(0x40), uintptr_t(0x188)}) {
            const uintptr_t resource = heap + offset;
            if (n == 4 && (p == resource + 0x10 || p == resource + 0x14)) {
                std::memcpy(out, p == resource + 0x10 ? &capacity : &type, n); return true;
            }
            if (n == 8 && (p == resource + 0x20 || p == resource + 0x50)) {
                std::memcpy(out, p == resource + 0x20 ? &handle : &address, n); return true;
            }
        }
        return assignRead(p, out, n);
    };
    auto assign = [&](uintptr_t entry, uintptr_t object) {
        current = 1; // simulate native publishing a new current instance
        return invoke(entry, object);
    };
    nativeResult = 0; current = 3;
    const unsigned beforeInvalidSize = calls;
    assert(b.prepareHeapState(image, false, 0, 0, composedRead,
        qualify, executable, assign, state) == 5);
    assert(b.prepareHeapState(image, false, 0x800, UINT32_MAX, composedRead,
        qualify, executable, assign, state) == 5);
    assert(calls == beforeInvalidSize && current == 3 && !state.resource);
    assert(b.prepareHeapState(image, false, 0x800, 0x800, composedRead,
        qualify, executable, assign, state) == 0);
    assert(state.resource == heap + 0x40 && state.instanceOffset == 4096);
    nativeResult = 31;
    assert(b.prepareHeapState(image, true, 0x800, 0, composedRead,
        qualify, executable, assign, state) == 31);
    assert(!state.resource && !state.instanceOffset);
    nativeResult = 0; capacity = 4096;
    assert(b.prepareHeapState(image, false, 0x800, 0, composedRead,
        qualify, executable, assign, state) == 5);
    assert(!state.resource && !state.instanceOffset);
}

static void testExecutionBinding() {
    constexpr uintptr_t base = 0x1000000;
    constexpr std::array<uintptr_t, 3> offsets{0x12ebc0, 0x12e420, 0x12e3f0};
    std::array<std::array<uint8_t, 16>, 3> bytes{{
        {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,0x48,0x8b,0x35,0x59,0x54,0x62,0,0xbf},
        {0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xb0,9,0,0,0x48,0x8b,5,0x56,0x5c},
        {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8}
    }};
    int denied = -1;
    bool identity = true, rx = true;
    auto read = [&](uintptr_t p, void* out, size_t n) {
        for (size_t i = 0; i < offsets.size(); ++i)
            if (p == base + offsets[i] && n == 16 && int(i) != denied) {
                std::memcpy(out, bytes[i].data(), n); return true;
            }
        return false;
    };
    auto qualify = [&](uintptr_t p) { return identity && p == base; };
    auto executable = [&](uintptr_t, size_t n) { return rx && n == 16; };
    TglExecutionBinding b;
    assert(b.resolve(base, read, qualify, executable));
    assert(b.image == base && b.create == base + offsets[0] &&
           b.initialize == base + offsets[1] && b.destroy == base + offsets[2]);
    for (int i = 0; i < 3; ++i) {
        denied = i; assert(!b.resolve(base, read, qualify, executable));
        assert(!b.image && !b.create && !b.initialize && !b.destroy); denied = -1;
        bytes[i][15] ^= 1; assert(!b.resolve(base, read, qualify, executable)); bytes[i][15] ^= 1;
    }
    identity = false; assert(!b.resolve(base, read, qualify, executable)); identity = true;
    rx = false; assert(!b.resolve(base, read, qualify, executable)); rx = true;
    assert(!b.resolve(UINTPTR_MAX, read, qualify, executable));
    assert(!b.resolve(0, read, qualify, executable));
}

static void testExecutionShape() {
    constexpr uintptr_t image = 0x1000000, object = 0x2000;
    uintptr_t vt = image + 0x757e90, first = 0x4000, second = 0x5000;
    uintptr_t av = image + 0x757f78, bv = image + 0x757fb8;
    bool allowed = true, qualified = true;
    bool codeValid = true, codeRx = true;
    std::array<std::array<uint8_t, 16>, 3> code{{
        {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,0x48,0x8b,0x35,0x59,0x54,0x62,0,0xbf},
        {0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xb0,9,0,0,0x48,0x8b,5,0x56,0x5c},
        {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8}
    }};
    auto read = [&](uintptr_t p, void* out, size_t n) {
        if (n == 16 && codeValid) {
            const std::array<uintptr_t, 3> offsets{0x12ebc0, 0x12e420, 0x12e3f0};
            for (size_t i = 0; i < offsets.size(); ++i)
                if (p == image + offsets[i]) {
                    std::memcpy(out, code[i].data(), n); return true;
                }
        }
        if (!allowed || n != 8) return false;
        const uintptr_t* value = p == object ? &vt : p == object + 0xd38 ? &first :
            p == object + 0xd40 ? &second : p == 0x4000 ? &av : p == 0x5000 ? &bv : nullptr;
        if (!value) return false;
        std::memcpy(out, value, n); return true;
    };
    auto qualify = [&](uintptr_t p) { return qualified && p == image; };
    assert(tglExecutionShape(object, image, read, qualify));
    qualified = false; assert(!tglExecutionShape(object, image, read, qualify)); qualified = true;
    allowed = false; assert(!tglExecutionShape(object, image, read, qualify)); allowed = true;
    first = 0; assert(!tglExecutionShape(object, image, read, qualify)); first = second;
    assert(!tglExecutionShape(object, image, read, qualify)); first = 0x4000;
    ++av; assert(!tglExecutionShape(object, image, read, qualify)); --av;
    ++bv; assert(!tglExecutionShape(object, image, read, qualify)); --bv;
    ++vt; assert(!tglExecutionShape(object, image, read, qualify)); --vt;
    assert(!tglExecutionShape(UINTPTR_MAX, image, read, qualify));
    assert(!tglExecutionShape(object, UINTPTR_MAX, read, qualify));
    struct Counts { int creates = 0, initializes = 0, destroys = 0, result = 0; } calls;
    struct Invoker {
        Counts* calls;
        uintptr_t create(uintptr_t entry) noexcept {
            assert(entry == 0x1000000 + 0x12ebc0); ++calls->creates; return 0x2000;
        }
        int initialize(uintptr_t entry, uintptr_t p) noexcept {
            assert(entry == 0x1000000 + 0x12e420 && p == 0x2000);
            ++calls->initializes; return calls->result;
        }
        void destroy(uintptr_t entry, uintptr_t p) noexcept {
            assert(entry == 0x1000000 + 0x12e3f0 && p == 0x2000); ++calls->destroys;
        }
    } invoke{&calls};
    TglExecutionBinding binding{image, image + 0x12ebc0, image + 0x12e420, image + 0x12e3f0};
    auto executable = [&](uintptr_t, size_t n) { return codeRx && n == 16; };
    TglExecutionBackend backend(binding, read, qualify, invoke, executable);
    {
        TglExecutionOwner owner(backend);
        calls.result = 31;
        assert(owner.ensure() == 31 && owner.get() == 0 && calls.destroys == 1);
        calls.result = 0; second = 0;
        assert(owner.ensure() == 5 && owner.get() == 0 && calls.destroys == 2);
        second = 0x5000;
        assert(owner.ensure() == 0 && owner.get() == object);
        assert(owner.ensure() == 0 && calls.creates == 3);
    }
    assert(calls.destroys == 3 && calls.initializes == 3);
    ++binding.destroy;
    TglExecutionBackend invalid(binding, read, qualify, invoke, executable);
    assert(invalid.create() == 0 && calls.creates == 3);
    --binding.destroy;
    codeValid = false;
    TglExecutionBackend unreadable(binding, read, qualify, invoke, executable);
    assert(unreadable.create() == 0 && calls.creates == 3);
    codeValid = true; codeRx = false;
    TglExecutionBackend nonExecutable(binding, read, qualify, invoke, executable);
    assert(nonExecutable.create() == 0 && calls.creates == 3);
    codeRx = true;
    for (auto& anchor : code) {
        anchor[15] ^= 1;
        TglExecutionBackend changedCode(binding, read, qualify, invoke, executable);
        assert(changedCode.create() == 0 && calls.creates == 3);
        anchor[15] ^= 1;
    }
}

static void testExecutionOwner() {
    struct Backend {
        bool allocation = true, valid = true, invalidateOnInit = false;
        int result = 0, creates = 0, initializes = 0, destroys = 0;
        uintptr_t create() noexcept { ++creates; return allocation ? 7 : 0; }
        int initialize(uintptr_t p) noexcept {
            assert(p == 7); ++initializes;
            if (invalidateOnInit) valid = false;
            return result;
        }
        bool validate(uintptr_t p) noexcept { assert(p == 7); return valid; }
        void destroy(uintptr_t p) noexcept { assert(p == 7); ++destroys; }
    } b;
    {
        TglExecutionOwner<Backend> owner(b);
        b.allocation = false;
        assert(owner.ensure() == 1 && owner.get() == 0 && b.destroys == 0);
        b.allocation = true; b.result = 31;
        assert(owner.ensure() == 31 && owner.get() == 0 && b.destroys == 1);
        b.result = 0; b.valid = false;
        assert(owner.ensure() == 5 && owner.get() == 0 && b.destroys == 2);
        b.valid = true;
        assert(owner.ensure() == 0 && owner.get() == 7);
        const auto calls = b.initializes;
        assert(owner.ensure() == 0 && b.initializes == calls);
        b.valid = false;
        assert(owner.ensure() == 5 && owner.get() == 0 && b.destroys == 3);
        b.valid = true;
        assert(owner.ensure() == 0);
        owner.reset(); owner.reset();
        assert(owner.get() == 0 && b.destroys == 4);
        assert(owner.ensure() == 0);
    }
    assert(b.destroys == 5 && b.creates == 6 && b.initializes == 5);
    Backend pass;
    {
        TglExecutionOwner<Backend> owner(pass);
        assert(owner.preparePass() == 0 && pass.initializes == 1);
        assert(owner.preparePass() == 0 && pass.initializes == 2 && pass.creates == 1);
        pass.result = 31;
        assert(owner.preparePass() == 31 && owner.get() == 0 && pass.destroys == 1);
        pass.result = 0;
        assert(owner.preparePass() == 0 && pass.creates == 2);
        pass.valid = false;
        const auto initialized = pass.initializes;
        assert(owner.preparePass() == 5 && owner.get() == 0);
        assert(pass.initializes == initialized && pass.destroys == 2);
        pass.valid = true;
        assert(owner.preparePass() == 0);
        pass.invalidateOnInit = true;
        assert(owner.preparePass() == 5 && owner.get() == 0 && pass.destroys == 3);
        owner.reset();
        assert(pass.destroys == 3); // failed post-Init shape already unwound
    }
}

static void testVeboxReport() {
    TglVeboxReportState storage;
    for (uint32_t mode : {0u, 1u, 2u, UINT32_MAX}) {
        for (unsigned flag = 0; flag < 256; ++flag) {
            storage.bytes.fill(0xa5);
            auto expected = storage.bytes;
            std::memcpy(expected.data() + 0xc, &mode, 4);
            expected[0x24] = (flag ^ 1) & 1;
            expected[0x25] = flag & 1;
            tglFinalizeVeboxReport(storage, mode, uint8_t(flag), uint8_t(flag));
            assert(storage.bytes == expected);
            std::array<uint8_t, 0x26> report{};
            tglExportVeboxReport(storage, 0, report);
            uint32_t exportedMode = 0;
            std::memcpy(&exportedMode, report.data() + 0xc, 4);
            assert(exportedMode == mode && report[0x24] == expected[0x24] &&
                   report[0x25] == expected[0x25]);
        }
    }
    for (unsigned flags = 0; flags < 32; ++flags) {
        std::array<uint8_t, 0x15> execution{};
        execution[0xf] = uint8_t(0xfe | ((flags >> 0) & 1));
        execution[0xb] = uint8_t(0xfe | ((flags >> 1) & 1));
        execution[0x13] = uint8_t(0xfe | ((flags >> 2) & 1));
        execution[0x14] = uint8_t(0xfe | ((flags >> 3) & 1));
        execution[8] = uint8_t(0xfe | ((flags >> 4) & 1));
        storage.bytes.fill(0xa5);
        auto expected = storage.bytes;
        expected[0] = flags & 1; expected[2] = (flags >> 1) & 1;
        if (flags & 4) {
            const uint32_t value = (flags & 8) && !(flags & 16) ? 2 : 3;
            std::memcpy(expected.data() + 4, &value, 4);
        }
        tglUpdateVeboxReport(storage, execution);
        assert(storage.bytes == expected);
    }
    storage.bytes.fill(0xa5);
    storage.reset();
    for (size_t i = 0; i < storage.bytes.size(); ++i)
        assert(storage.bytes[i] == ((i <= 0x1c || (i >= 0x20 && i <= 0x25)) ? 0 : 0xa5));
    std::array<uint8_t, 0x26> state{}, report{}, expected{};
    for (size_t i = 0; i < state.size(); ++i) state[i] = uint8_t(0x80 + i);
    report.fill(0xa5); expected = report;
    for (auto i : {0, 2, 0x13, 0x15, 0x17, 0x19, 0x24, 0x25}) expected[i] = state[i] & 1;
    for (auto i : {4, 5, 6, 7, 0xc, 0xd, 0xe, 0xf, 0x14, 0x16, 0x18, 0x1a})
        expected[i] = state[i];
    expected[0x10] = 1;
    tglExportVeboxReport(state, 0xff, report);
    assert(report == expected);
    std::memcpy(storage.bytes.data(), state.data(), state.size());
    report.fill(0xa5);
    tglExportVeboxReport(storage, 0xff, report);
    assert(report == expected);
    auto alias = state;
    expected = state;
    tglExportVeboxReport(state, 2, expected);
    tglExportVeboxReport(alias, 2, alias);
    assert(alias == expected && alias[0x10] == 0);
}

static void testVeboxPrefix() {
    TglVeboxPrefix prefix;
    prefix.vtable = 7; prefix.os = 9; prefix.slot40 = 11;
    uintptr_t surface = 0x2000;
    uint32_t kind = 5, value = 0x12345678;
    uint8_t flag = 0xfe;
    bool allowed = true;
    const auto read = [&](uintptr_t p, void *out, size_t n) {
        if (!allowed) return false;
        const void *data = nullptr;
        if (p == 0x1098 && n == 8) data = &surface;
        if (p == 0x2134 && n == 4) data = &kind;
        if (p == 0x1110 && n == 1) data = &flag;
        if (p == 0x111c && n == 4) data = &value;
        if (!data) return false;
        std::memcpy(out, data, n); return true;
    };
    assert(prefix.configure(0x3000, 0x1000, read));
    assert(prefix.flag48 == 0 && prefix.flag49 == 1 && prefix.parentData == 0x39f0 &&
           prefix.sourceValue == value);
    assert(prefix.vtable == 7 && prefix.os == 9 && prefix.slot40 == 11);
    const auto saved = prefix;
    allowed = false; assert(!prefix.configure(0x3000, 0x1000, read)); allowed = true;
    surface = UINTPTR_MAX; assert(!prefix.configure(0x3000, 0x1000, read)); surface = 0x2000;
    assert(!prefix.configure(UINTPTR_MAX, 0x1000, read));
    assert(!prefix.configure(0x3000, UINTPTR_MAX, read));
    assert(prefix.flag48 == saved.flag48 && prefix.flag49 == saved.flag49 &&
           prefix.parentData == saved.parentData && prefix.sourceValue == saved.sourceValue);
    flag = 0xff; kind = 4;
    assert(prefix.configure(0x3000, 0x1000, read) && prefix.flag48 == 1 && prefix.flag49 == 0);
}

static void testComposedStatisticsBuffer() {
    struct Fixture {
        std::array<uint8_t, 8192> bytes{};
        unsigned allocations = 0, releases = 0, fills = 0;
        bool writable = true;
    } fixture;
    const auto allocate = +[](void *ctx, const void *params, void *output) noexcept {
        auto &f = *static_cast<Fixture *>(ctx);
        const auto *p = static_cast<const uint8_t *>(params);
        uint32_t size = 0, type = 1;
        std::memcpy(&size, p + 0x14, 4); std::memcpy(&type, p, 4);
        assert(type == 0 && size <= f.bytes.size());
        ++f.allocations; f.bytes.fill(0xa5);
        auto *s = static_cast<uint8_t *>(output);
        const uintptr_t address = reinterpret_cast<uintptr_t>(f.bytes.data()), handle = 7;
        std::memcpy(s + 0x10, &size, 4);
        std::memcpy(s + 0x20, &handle, 8);
        std::memcpy(s + 0x50, &address, 8);
        return 0;
    };
    const auto release = +[](void *ctx, void *output) noexcept {
        ++static_cast<Fixture *>(ctx)->releases;
        std::memset(output, 0, 0x148);
    };
    TglNativeResourceBackend backend(&fixture, allocate, release, 0, 4096);
    const auto initialize = [&](const std::array<uint8_t, 0x148> &resource,
                                uint32_t bytes) noexcept {
        const auto writable = [&](uintptr_t address, uint32_t size) noexcept {
            return fixture.writable && address == reinterpret_cast<uintptr_t>(fixture.bytes.data())
                && size <= fixture.bytes.size();
        };
        const auto fill = [&](uintptr_t, uint32_t size, uint8_t value) noexcept {
            ++fixture.fills;
            std::memset(fixture.bytes.data(), value, size);
            return 0;
        };
        return tglFillBuffer(resource, bytes, 0, writable, fill);
    };
    {
        TglStatisticsResource<TglNativeResourceBackend> statistics(backend);
        assert(statistics.ensure(64, 16, initialize) == 0);
        assert(statistics.layout().bytes == 1280 && fixture.fills == 1);
        for (size_t i = 0; i < fixture.bytes.size(); ++i)
            assert(fixture.bytes[i] == (i < 1280 ? 0 : 0xa5));
        assert(statistics.ensure(64, 16, initialize) == 0 && fixture.fills == 1);
        fixture.writable = false;
        assert(statistics.ensure(128, 16, initialize) == 5);
        assert(statistics.layout().bytes == 0 && fixture.allocations == fixture.releases);
        fixture.writable = true;
        assert(statistics.ensure(128, 16, initialize) == 0 && fixture.fills == 2);
    }
    assert(fixture.allocations == 3 && fixture.releases == 3);
}

static void testBufferFill() {
    std::array<uint8_t, 0x148> resource{};
    std::array<uint8_t, 32> bytes{};
    const auto put = [&](size_t offset, auto value) {
        std::memcpy(resource.data() + offset, &value, sizeof(value));
    };
    const uintptr_t address = reinterpret_cast<uintptr_t>(bytes.data());
    put(0x10, uint32_t{32}); put(0x20, uintptr_t{7}); put(0x50, address);
    unsigned writes = 0;
    bool allowed = true;
    int status = 0;
    const auto writable = [&](uintptr_t p, uint32_t n) noexcept {
        return allowed && p == address && n <= bytes.size();
    };
    const auto fill = [&](uintptr_t p, uint32_t n, uint8_t value) noexcept {
        ++writes; assert(p == address);
        if (!status) std::memset(bytes.data(), value, n);
        return status;
    };
    assert(tglFillBuffer(resource, 16, 0x80, writable, fill) == 0 && writes == 1);
    for (size_t i = 0; i < bytes.size(); ++i) assert(bytes[i] == (i < 16 ? 0x80 : 0));
    allowed = false; assert(tglFillBuffer(resource, 16, 0, writable, fill) == 5);
    allowed = true;
    for (uint32_t n : {0u, 33u, UINT32_MAX})
        assert(tglFillBuffer(resource, n, 0, writable, fill) == 5);
    put(0x14, uint32_t{1}); assert(tglFillBuffer(resource, 16, 0, writable, fill) == 5);
    put(0x14, uint32_t{0}); put(0x20, uintptr_t{0});
    assert(tglFillBuffer(resource, 16, 0, writable, fill) == 5);
    put(0x20, uintptr_t{7}); put(0x50, UINTPTR_MAX);
    assert(tglFillBuffer(resource, 16, 0, writable, fill) == 5 && writes == 1);
    put(0x50, address); status = 31;
    assert(tglFillBuffer(resource, 16, 0, writable, fill) == 31 && writes == 2);
#if defined(__APPLE__)
    // Owned CPU memory only; fixture handle is not a GPU resource claim.
    assert(tglFillBufferNative(resource, 16, 0x55) == 0);
    for (size_t i = 0; i < 16; ++i) assert(bytes[i] == 0x55);
    assert(!tglNativeWritableRange(0, 16));
    mach_vm_address_t ro = 0;
    assert(mach_vm_allocate(mach_task_self(), &ro, 4096, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    assert(mach_vm_protect(mach_task_self(), ro, 4096, false, VM_PROT_READ) == KERN_SUCCESS);
    assert(!tglNativeWritableRange(ro, 16));
    put(0x50, static_cast<uintptr_t>(ro));
    assert(tglFillBufferNative(resource, 16, 0) == 5);
    assert(mach_vm_deallocate(mach_task_self(), ro, 4096) == KERN_SUCCESS);
#endif
}

static void testStatisticsLifecycle() {
    struct Backend {
        int allocations = 0, releases = 0, status = 0;
        bool backing = true;
        int allocate(std::array<uint8_t, 0x148> &s, const TglResourceKey &key) noexcept {
            ++allocations;
            assert(key.format == 0x3e && key.height == 1 && !key.compressed);
            s[0] = 1;
            return status;
        }
        bool backed(const std::array<uint8_t, 0x148> &) noexcept { return backing; }
        void release(std::array<uint8_t, 0x148> &) noexcept { ++releases; }
    } backend;
    unsigned fills = 0;
    int fillStatus = 0;
    const auto fill = [&](const std::array<uint8_t, 0x148> &s, uint32_t bytes) noexcept {
        ++fills; assert(s[0] == 1 && bytes != 0); return fillStatus;
    };
    {
        TglStatisticsResource<Backend> resource(backend);
        assert(resource.ensure(1920, 1080, fill) == 0 && fills == 1);
        assert(resource.layout().bytes == 520320);
        assert(resource.ensure(1920, 1080, fill) == 0 && fills == 1);
        assert(resource.ensure(0, 1080, fill) == 5 && resource.layout().bytes == 520320);
        fillStatus = 31;
        assert(resource.ensure(1280, 720, fill) == 31 && resource.layout().bytes == 0);
        assert(backend.allocations == 2 && backend.releases == 2);
        fillStatus = 0; backend.backing = false;
        assert(resource.ensure(1280, 720, fill) == 5 && fills == 2);
        backend.backing = true; backend.status = 25;
        assert(resource.ensure(1280, 720, fill) == 25 && resource.layout().bytes == 0);
        backend.status = 0;
        assert(resource.ensure(1280, 720, fill) == 0 && fills == 3);
    }
    assert(backend.allocations == 5 && backend.releases == 5);
}

static void testSurfaceBoundary() {
    uint32_t w = 0, h = 0;
    for (uint32_t format = 0; format <= 0x33; ++format)
        for (bool di : {false, true}) for (bool align64 : {false, true}) {
            assert(tglSurfaceBoundary(format, 101, 103, 200, 200, di, align64, w, h) == 0);
            const bool two = format == 0x19 || (format >= 0xd && format <= 0x15);
            assert(w == ((di && align64) ? 128u : two ? 102u : 101u));
            assert(h == (format == 0x19 ? 104u :
                   (di && format >= 0xd && format <= 0x13) ? 104u : 103u));
        }
    assert(tglSurfaceBoundary(0, 1920, 1080, 0, 0, false, false, w, h) == 0);
    assert(w == 64 && h == 16);
    assert(tglSurfaceBoundary(0, 32, 8, 0, 0, false, false, w, h) == 0);
    assert(w == 32 && h == 8);
    for (float bad : {-1.0f, INFINITY, NAN, 4294967296.0f}) {
        w = 7; h = 9;
        assert(tglSurfaceBoundary(0, 100, 100, bad, 100, false, false, w, h) == 5);
        assert(w == 7 && h == 9);
        assert(tglSurfaceBoundary(0, 100, 100, 100, bad, false, false, w, h) == 5);
        assert(w == 7 && h == 9);
    }
}

static void testStatisticsOffsets() {
    uint32_t queried = 17;
    for (uint32_t selector = 0; selector < 8; ++selector) {
        queried = 17;
        const int result = tglStatisticsQuery(selector, queried);
        const bool supported = selector == 0 || selector == 2 || selector == 3 || selector == 5;
        assert(result == (supported ? 0 : 31));
        if (!supported) assert(queried == 17);
        else assert(queried == (selector == 0 ? 0u : selector == 2 ? 44u :
                               selector == 3 ? 68u : 128u));
    }
    TglStatisticsAllocation allocation;
    assert(tglStatisticsAllocationSize(1920, 1080, allocation) == 0);
    assert(allocation.rowBytes == 1920 && allocation.blockRows == 270 &&
           allocation.totalRows == 271 && allocation.bytes == 520320);
    assert(tglStatisticsAllocationSize(1, 1, allocation) == 0);
    assert(allocation.rowBytes == 64 && allocation.blockRows == 1 &&
           allocation.totalRows == 17 && allocation.bytes == 1088);
    for (const auto dims : {std::array<uint32_t, 2>{0, 1}, {1, 0},
                            {UINT32_MAX, 1}, {1920, UINT32_MAX}}) {
        assert(tglStatisticsAllocationSize(dims[0], dims[1], allocation) == 5);
        assert(allocation.bytes == 1088 && allocation.totalRows == 17);
    }
    unsigned calls = 0;
    int status = 0;
    const auto query = [&](uint32_t selector, uint32_t &size) noexcept {
        ++calls;
        assert(selector == 5);
        size = 128; // fixture only, not a TGL platform constant
        return status;
    };
    uint32_t first = 0, second = 0;
    for (uint8_t a : {uint8_t{0}, uint8_t{1}, uint8_t{0xfe}, uint8_t{0xff}})
        for (uint8_t b : {uint8_t{0}, uint8_t{1}, uint8_t{0xfe}, uint8_t{0xff}}) {
            const auto before = calls;
            assert(tglStatisticsOffsets(64, 16, a, b, first, second, query) == 0);
            assert(calls == before + 1);
            assert(first == ((b & 1) ? 1152u : (a & 1) ? 1024u : 0u));
            assert(second == ((b & 1) ? 1408u : (a & 1) ? 1152u : 128u));
        }
    status = 31; first = 7; second = 9;
    assert(tglStatisticsOffsets(64, 16, 1, 1, first, second, query) == 31);
    assert(first == 7 && second == 9);
    status = 0;
    assert(tglStatisticsOffsets(UINT32_MAX, 2, 1, 1, first, second, query) == 0);
    assert(first == 126 && second == 382); // exact native wrap, not size validation
}

static void testSurfaceDescriptor() {
    std::array<uint8_t, 0x2a8> shell{};
    const auto put = [&](size_t offset, uint32_t value) {
        std::memcpy(shell.data() + offset, &value, sizeof(value));
    };
    put(0x130, 0x19); put(0xd8, 1920); put(0xdc, 1080);
    put(0xe0, 2048); put(0x13c, 7); put(0xe4, 2);
    put(0xf0, 4096); put(0x100, 10240); put(0x108, 11);
    for (size_t i = 0; i < 16; ++i) shell[0x50 + i] = static_cast<uint8_t>(i + 1);
    shell[0x29a] = 0xfe;
    std::array<uint8_t, 0x48> expected{};
    const auto expect = [&](size_t offset, uint32_t value) {
        std::memcpy(expected.data() + offset, &value, sizeof(value));
    };
    expect(0, 1); expect(8, 0x19); expect(0xc, 1920); expect(0x10, 1080);
    expect(0x14, 2048); expect(0x18, 7); expect(0x24, 14); expect(0x28, 2);
    std::memcpy(expected.data() + 0x2c, shell.data() + 0x50, 16);
    const uintptr_t resource = reinterpret_cast<uintptr_t>(shell.data() + 0x148);
    std::memcpy(expected.data() + 0x40, &resource, sizeof(resource));
    assert(tglSurfaceDescriptor(shell) == expected);
    // All optional-slot combinations; only slots 0/1 copy +f8 metadata.
    put(0xf8, 0x12345678);
    for (unsigned mask = 0; mask < 32; ++mask) {
        std::array<const std::array<uint8_t, 0x2a8> *, 5> slots{};
        std::array<uint8_t, 0x170> command{};
        for (size_t i = 0; i < slots.size(); ++i) if (mask & (1u << i)) {
            slots[i] = &shell;
            std::memcpy(command.data() + i * 0x48, expected.data(), expected.size());
            if (i < 2) std::memcpy(command.data() + i * 0x48 + 0x20,
                                   shell.data() + 0xf8, 4);
        }
        command[0x168] = 1;
        command[0x169] = (mask & 2) ? 1 : 0;
        assert(tglSurfaceCommandSet(slots, 0xff) == command);
    }
    shell[0x29a] = 0xff; expected[4] = 1;
    put(0xe0, 0); expect(0x14, 0); expect(0x24, 0);
    assert(tglSurfaceDescriptor(shell) == expected); // zero pitch never divides
    put(0xe0, 1); put(0xf0, 1); put(0x100, 0); put(0x108, 2);
    expect(0x14, 1); expect(0x24, 1); // native unsigned subtraction/addition wrap
    assert(tglSurfaceDescriptor(shell) == expected);
}

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
    testTrackedCpuOwner();
    testOwnedVeboxVtable();
    testNativeCscBinding();
    testDirectVeboxFeasibility();
    testVeboxHardwareBinding();
    testExecutionBinding();
    testExecutionShape();
    testExecutionOwner();
    testVeboxReport();
    testVeboxPrefix();
    testComposedStatisticsBuffer();
    testBufferFill();
    testStatisticsLifecycle();
    testSurfaceBoundary();
    testStatisticsOffsets();
    testSurfaceDescriptor();
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
