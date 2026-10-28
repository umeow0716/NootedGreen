// Gen12 render fixes ported from WhateverXe (Copyright (c) 2026 b00t0x,
// BSD-3-Clause). Each patch checks the donor bytes first and is skipped
// (reported in the registry) when they don't match.
#include "render.hpp"
#include <Headers/kern_api.hpp>
#include <IOKit/IORegistryEntry.h>

extern "C" uintptr_t ngxeCCSPSResume;
uintptr_t ngxeCCSPSResume;

// Donor 8D056..8D06D in resolve_ccs. FAST_CLEAR_0 (resolve type 2) must set
// PS bit 8 (Fast Clear Enable); the donor emits it clear.
extern "C" __attribute__((naked, used)) void ngxeCCSPSBridge() {
    asm volatile(
        "shll $6, %eax\n orl $2, %eax\n"
        "pushfq\n pushq %rax\n cmpl $0x82, %eax\n jne 1f\n orl $0x100, %eax\n"
        "1:\n movw %ax, 0xa0(%r15)\n popq %rax\n popfq\n"
        "movw $0x1f80, 0xa2(%r15)\n jmpq *_ngxeCCSPSResume(%rip)\n");
}

namespace {
const char *paths[] {"/Library/Extensions/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics"};
KernelPatcher::KextInfo tgl {"com.xxxxx.driver.AppleIntelTGLGraphics", paths, arrsize(paths), {}, {},
    KernelPatcher::KextInfo::Unloaded};

using SurfaceAux = void (*)(void *, void *, uint32_t *, uint32_t);
SurfaceAux orgSurfaceAux;

// CCS_E requires HALIGN16. Only single-level, single-layer 2D color surfaces
// whose level 0 layout doesn't change.
bool correctCCSAlignment(uint32_t *w) {
    const auto format = (w[0] >> 18) & 0x3FFU;
    if ((w[6] & 7U) != 5 || (w[0] >> 29) != 1 || ((w[0] >> 12) & 3U) != 3 ||
        ((w[0] >> 14) & 3U) != 1 || (format != 0xC0 && format != 0x80) ||
        (w[1] & 0x00F80000U) || (w[2] & 0x80000000U) ||
        (w[3] & 0xFFFC0000U) || w[4] || w[5] ||
        (((w[2] & 0x3FFFU) + 1) & 15U) || (((w[3] & 0x3FFFFU) + 1) & 127U)) return false;
    w[0] |= 0x8000U;
    return true;
}

void wrapSurfaceAux(void *resource, void *entry, uint32_t *surface, uint32_t intent) {
    orgSurfaceAux(resource, entry, surface, intent);
    correctCCSAlignment(surface);
}

using GPUInfo = uint64_t (*)(void *);
GPUInfo orgGPUInfo;

// This machine (RPL-P, 64 EU): eight native subslices, six L3 banks.
constexpr uint32_t urbBanks = 6;

uint64_t wrapGPUInfo(void *accelerator) {
    const auto result = orgGPUInfo(accelerator);
    auto banks = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(accelerator) + 0x1164);
    SYSLOG("ngxe", "URB banks %u", *banks);
    if (!*banks) *banks = urbBanks;
    return result;
}

// Context-saved registers that the donor sets through MMIO never reach a VF
// context. Load them from the per-context batch (BB_PER_CTX_PTR), which the
// hardware runs after every RCS context restore:
//   0x7304 COMMON_SLICE_CHICKEN3 disable CPS-aware color pipe (Wa_1409142259)
//   0x6604 FF_MODE2 GS224/TDS128 timers (Wa_1608008084)
//   0xE4F4 ROW_CHICKEN2 disable EU early read (Wa_1606931601)
//
// It also programs pixel pipe hashing. Gen12 has three pixel pipes of two DSS
// each; with this part's DSS mask 0xf the pipes have 2/2/0 subslices, and the
// default hash keeps sending pixels to the empty third pipe (draws produce
// nothing, then RCS hangs). Like Mesa, use a subslice hash table that
// alternates between pipes 0 and 1.
const uint32_t ctxLRI[] {0, 0x11000005, 0x7304, 0x02000200, 0x6604, 0xE0040000, 0xE4F4, 0x40004000};

void buildCtxBatch(uint32_t *b) {
    size_t n = 0;
    for (auto v : ctxLRI) b[n++] = v;
    // 3DSTATE_SUBSLICE_HASH_TABLE: slice 0 uses TABLE_0; 8x16 two-way and
    // three-way tables from intel_compute_pixel_hash_table_3way(8, 16, 2, 2, 0).
    b[n++] = 0x791F000C;
    b[n++] = 2;
    uint32_t *twoWay = b + n, *threeWay = b + n + 4;
    for (unsigned e = 0; e < 128; e++) {
        const uint32_t pipe = ((e / 16) + (e % 16)) % 2;
        twoWay[e / 32] |= pipe << (e % 32);
        threeWay[e / 16] |= pipe << (2 * (e % 16));
    }
    n += 12;
    // 3DSTATE_3D_MODE: SubsliceHashingTableEnable (+mask)
    b[n++] = 0x791E0000;
    b[n++] = 0x00200020;
    b[n++] = 0x05000000; // MI_BATCH_BUFFER_END
}

using Allocate = void *(*)(void *, size_t, unsigned, unsigned);
using CPUAddress = uint8_t *(*)(void *);
using GPUAddress = uint64_t (*)(void *);
Allocate sharedAllocate;
CPUAddress sharedCPU;
GPUAddress mappedGPU;
mach_vm_address_t restoreCaller;
mach_vm_address_t tglAddress;

using ContextInit = bool (*)(void *, void *, const void *, uint8_t);
using ContextRestore = bool (*)(void *);
ContextInit orgContextInit;
ContextRestore orgContextRestore;

void prepareCtxBatch(void *owner) {
    auto o = static_cast<uint8_t *>(owner);
    auto &slot = *reinterpret_cast<void **>(o + 0x13F8);
    auto task = *reinterpret_cast<void **>(o + 0x150);
    static bool logged;
    if (!logged) {
        logged = true;
        SYSLOG("ngxe", "context init owner=%p slot=%p task=%p", owner, slot, task);
    }
    if (slot || !task) return;
    auto buffer = sharedAllocate(task, 4096, 3, 0);
    if (!buffer) {
        SYSLOG("ngxe", "per-context batch allocation failed");
        return;
    }
    auto cpu = sharedCPU(buffer);
    const auto gpu = mappedGPU(buffer);
    if (!cpu || !gpu || (gpu & 4095) || gpu > 0xFFFFF000ULL) {
        SYSLOG("ngxe", "unusable per-context batch cpu=%p gpu=0x%llx", cpu, gpu);
        reinterpret_cast<OSObject *>(buffer)->release();
        return;
    }
    bzero(cpu, 4096);
    buildCtxBatch(reinterpret_cast<uint32_t *>(cpu));
    for (unsigned i = 0; i < 4096; i += 64) asm volatile("clflush (%0)" :: "r"(cpu + i) : "memory");
    asm volatile("mfence" ::: "memory");
    slot = buffer; // the donor's engine stop releases it
    SYSLOG("ngxe", "per-context batch at gpu 0x%llx", gpu);
}

bool wrapContextInit(void *context, void *task, const void *params, uint8_t priority) {
    const auto *pb = static_cast<const uint8_t *>(params);
    SYSLOG("ngxe", "context init type=%u flags=0x%02x%02x caller=0x%llx", pb[0], pb[3], pb[2],
        reinterpret_cast<uint64_t>(__builtin_return_address(0)) - tglAddress);
    auto owner = task ? *reinterpret_cast<void **>(static_cast<uint8_t *>(task) + 0x10) : nullptr;
    if (owner) prepareCtxBatch(owner);
    else SYSLOG("ngxe", "context init without owner task=%p", task);
    return orgContextInit(context, task, params, priority);
}

// Contexts cloned from the safe image skip the native pointer store.
bool wrapContextRestore(void *context) {
    const auto caller = reinterpret_cast<mach_vm_address_t>(__builtin_return_address(0));
    const bool result = orgContextRestore(context);
    if (!result || caller != restoreCaller) return result;
    auto c = static_cast<uint8_t *>(context);
    auto o = *reinterpret_cast<uint8_t **>(c + 0x50);
    auto image = *reinterpret_cast<void **>(c + 0x98);
    if (c[0x6C] || !o || !image) return result;
    // The owner's own batch: another owner's may already be freed.
    auto batch = *reinterpret_cast<void **>(o + 0x13F8);
    const auto gpu = batch ? mappedGPU(batch) : 0;
    auto cpu = sharedCPU(image);
    if (gpu && cpu && *reinterpret_cast<uint32_t *>(cpu + 0x1048) == 0x21C0) {
        *reinterpret_cast<uint32_t *>(cpu + 0x104C) = static_cast<uint32_t>(gpu) | 5;
        asm volatile("clflush (%0)" :: "r"(cpu + 0x104C) : "memory");
        asm volatile("mfence" ::: "memory");
    }
    return result;
}

bool bytesAt(mach_vm_address_t address, size_t offset, const uint8_t *bytes, size_t length) {
    return !memcmp(reinterpret_cast<const void *>(address + offset), bytes, length);
}

void report(const char *name, bool ok) {
    IORegistryEntry::getRegistryRoot()->setProperty(name, ok);
    SYSLOG("ngxe", "%s=%d", name, ok);
}
}

void XeRender::init() {
    lilu.onKextLoadForce(&tgl);
}

bool XeRender::processKext(KernelPatcher &p, size_t index, mach_vm_address_t address, size_t size) {
    if (index != tgl.loadIndex) return false;
    if (size <= 0x8D4E0) return true;

    // resolve_ccs places its state at cursor+4096-alignedSize but reserves only
    // two dwords, so after the init batch it overruns the 4 KiB buffer and
    // truncates binding pointers. Reserve the usable page instead.
    const uint8_t ccsFind[] {0xBE, 0x02, 0x00, 0x00, 0x00};
    const uint8_t ccsReplace[] {0xBE, 0xFE, 0x03, 0x00, 0x00};
    bool ccs = p.solveSymbol<mach_vm_address_t>(index,
        "__Z11resolve_ccsP25IOAccelCommandBufferPool2P14IGMappedBufferP22color_resolve_params_tbRK8IGVectorI11blit_rect_t25IGIOMallocAllocatorPolicyE",
        address, size) == address + 0x8CB2A && bytesAt(address, 0x8CB80, ccsFind, sizeof(ccsFind));
    p.clearError();
    if (ccs) {
        KernelPatcher::LookupPatch patch {&tgl, ccsFind, ccsReplace, sizeof(ccsFind), 1};
        p.applyLookupPatch(&patch, reinterpret_cast<uint8_t *>(address + 0x8CB80), sizeof(ccsFind) + 1);
        ccs = p.getError() == KernelPatcher::Error::NoError && bytesAt(address, 0x8CB80, ccsReplace, sizeof(ccsReplace));
        p.clearError();
    }
    report("NGXeCCSPageReservation", ccs);

    const uint8_t psBlock[] {0xC1, 0xE0, 0x06, 0x83, 0xC8, 0x02, 0x66, 0x41, 0x89, 0x87, 0xA0, 0, 0, 0,
        0x66, 0x41, 0xC7, 0x87, 0xA2, 0, 0, 0, 0x80, 0x1F};
    const uint8_t psType[] {0x8B, 0x42, 0x64, 0x83, 0xE0, 0x03};
    bool ps = ccs && bytesAt(address, 0x8D056, psBlock, sizeof(psBlock)) &&
        bytesAt(address, 0x8D02B, psType, sizeof(psType));
    if (ps) {
        ngxeCCSPSResume = address + 0x8D06E;
        uint8_t jump[14] {0xFF, 0x25, 0, 0, 0, 0};
        const auto target = reinterpret_cast<uintptr_t>(ngxeCCSPSBridge);
        memcpy(jump + 6, &target, sizeof(target));
        p.routeBlock(address + 0x8D056, jump, sizeof(jump));
        ps = p.getError() == KernelPatcher::Error::NoError && bytesAt(address, 0x8D056, jump, sizeof(jump));
        p.clearError();
    }
    report("NGXeCCSFastClear", ps);

    const char *auxState = "__ZN15IGAccelResource23fillSurfaceStateAuxInfoEPNS_17ResourceInfoEntryER22SGfxRenderSurfaceState6Intent";
    const uint8_t auxHead[] {0x55, 0x48, 0x89, 0xE5, 0xF6, 0x86, 0x91, 0, 0, 0, 0x02, 0x8B, 0x42, 0x18};
    bool aux = p.solveSymbol<mach_vm_address_t>(index, auxState, address, size) == address + 0x73FC2 &&
        bytesAt(address, 0x73FC2, auxHead, sizeof(auxHead));
    p.clearError();
    if (aux) {
        KernelPatcher::RouteRequest route {auxState, wrapSurfaceAux, orgSurfaceAux};
        aux = p.routeMultiple(index, &route, 1, address, size);
        p.clearError();
    }
    report("NGXeCCSAlignment", aux);

    // getGPUInfo only fills the URB bank count (this+0x1164) for one slice
    // with twelve native subslices; otherwise it stays zero and the Metal URB
    // allocator underflows. NootedGreen patches inside getGPUInfo, so wrap it
    // rather than editing its body.
    const char *gpuInfo = "__ZN16IntelAccelerator10getGPUInfoEv";
    bool urb = p.solveSymbol<mach_vm_address_t>(index, gpuInfo, address, size) == address + 0x2847E;
    p.clearError();
    if (urb) {
        KernelPatcher::RouteRequest route {gpuInfo, wrapGPUInfo, orgGPUInfo};
        urb = p.routeMultiple(index, &route, 1, address, size);
        p.clearError();
    }
    report("NGXeURBBanks", urb);

    const char *ctxInit = "__ZN17IGHardwareContext15initWithOptionsEP11IGAccelTaskRK23IGHardwareContextParamsh";
    const char *ctxRestore = "__ZN17IGHardwareContext20restoreFromSafeImageEv";
    const uint8_t restoreCall[] {0x4C, 0x89, 0xEF, 0xE8, 0x61, 0x02, 0x00, 0x00};
    const uint8_t validFind[] {0x83, 0xC8, 0x01, 0x41, 0x89, 0x87, 0x4C, 0x10};
    const uint8_t validReplace[] {0x83, 0xC8, 0x05, 0x41, 0x89, 0x87, 0x4C, 0x10};
    bool ctx = p.solveSymbol<mach_vm_address_t>(index, ctxInit, address, size) == address + 0x7B9D4 &&
        p.solveSymbol<mach_vm_address_t>(index, ctxRestore, address, size) == address + 0x7C392 &&
        bytesAt(address, 0x7C129, restoreCall, sizeof(restoreCall)) &&
        bytesAt(address, 0x7C21D, validFind, sizeof(validFind));
    p.clearError();
    if (ctx) {
        KernelPatcher::LookupPatch patch {&tgl, validFind, validReplace, sizeof(validFind), 1};
        p.applyLookupPatch(&patch, reinterpret_cast<uint8_t *>(address + 0x7C21D), sizeof(validFind) + 1);
        ctx = p.getError() == KernelPatcher::Error::NoError;
        p.clearError();
    }
    if (ctx) {
        sharedAllocate = reinterpret_cast<Allocate>(address + 0x10924);
        sharedCPU = reinterpret_cast<CPUAddress>(address + 0x10B60);
        mappedGPU = reinterpret_cast<GPUAddress>(address + 0x13E7C);
        restoreCaller = address + 0x7C131;
        tglAddress = address;
        KernelPatcher::RouteRequest routes[] {
            {ctxInit, wrapContextInit, orgContextInit},
            {ctxRestore, wrapContextRestore, orgContextRestore},
        };
        ctx = p.routeMultiple(index, routes, arrsize(routes), address, size);
        p.clearError();
    }
    report("NGXeContextBatch", ctx);

    // RCS completion and render-pass PIPE_CONTROLs: add HDC flush, state and
    // constant-cache invalidation (immediates of four movabs instructions).
    const uint32_t flushSites[] {0x84E5D, 0x85406, 0x51A20, 0x51A3A};
    const uint8_t flushFind[][10] {
        {0x48, 0xB8, 0x04, 0x00, 0x00, 0x7A, 0x21, 0x50, 0x10, 0x40},
        {0x48, 0xB9, 0x04, 0x00, 0x00, 0x7A, 0x21, 0x50, 0x10, 0x40},
        {0x48, 0xBF, 0x04, 0x00, 0x00, 0x7A, 0x21, 0x52, 0x10, 0x40},
        {0x48, 0xBB, 0x04, 0x00, 0x00, 0x7A, 0x98, 0x44, 0x10, 0x00}};
    const uint8_t flushReplace[][10] {
        {0x48, 0xB8, 0x04, 0x02, 0x00, 0x7A, 0x21, 0x70, 0x10, 0x40},
        {0x48, 0xB9, 0x04, 0x02, 0x00, 0x7A, 0x21, 0x70, 0x10, 0x40},
        {0x48, 0xBF, 0x04, 0x02, 0x00, 0x7A, 0x21, 0x72, 0x10, 0x50},
        {0x48, 0xBB, 0x04, 0x02, 0x00, 0x7A, 0x94, 0x44, 0x10, 0x20}};
    bool flush = p.solveSymbol<mach_vm_address_t>(index, "__ZN24IGHardwareRingBufferMain18commitStampCommandEjb",
        address, size) == address + 0x84DC8;
    p.clearError();
    for (size_t n = 0; flush && n < arrsize(flushSites); ++n)
        flush = bytesAt(address, flushSites[n], flushFind[n], sizeof(flushFind[n]));
    for (size_t n = 0; flush && n < arrsize(flushSites); ++n) {
        KernelPatcher::LookupPatch patch {&tgl, flushFind[n], flushReplace[n], sizeof(flushFind[n]), 1};
        p.applyLookupPatch(&patch, reinterpret_cast<uint8_t *>(address + flushSites[n]), sizeof(flushFind[n]) + 1);
        flush = p.getError() == KernelPatcher::Error::NoError;
        p.clearError();
    }
    report("NGXeRCSFlush", flush);

    // The shared device settings tell the UMD POSH=0 but PTBR=1, and PTBR
    // needs POSH. Publish PTBR=0 (mov edi,1 -> mov edi,0 for the second value).
    const uint8_t ptbrFind[] {0xBF, 0x01, 0x00, 0x00, 0x00, 0xBE, 0x10, 0x00, 0x00, 0x00};
    const uint8_t ptbrReplace[] {0xBF, 0x00, 0x00, 0x00, 0x00, 0xBE, 0x10, 0x00, 0x00, 0x00};
    bool ptbr = p.solveSymbol<mach_vm_address_t>(index, "__Z41intelCreateSharedDeviceSettingsDictionaryv",
        address, size) == address + 0x17928 && bytesAt(address, 0x1797B, ptbrFind, sizeof(ptbrFind));
    p.clearError();
    if (ptbr) {
        KernelPatcher::LookupPatch patch {&tgl, ptbrFind, ptbrReplace, sizeof(ptbrFind), 1};
        p.applyLookupPatch(&patch, reinterpret_cast<uint8_t *>(address + 0x1797B), sizeof(ptbrFind) + 1);
        ptbr = p.getError() == KernelPatcher::Error::NoError;
        p.clearError();
    }
    report("NGXePTBROff", ptbr);
    return true;
}
