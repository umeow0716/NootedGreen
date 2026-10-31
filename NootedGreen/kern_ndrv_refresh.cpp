#include "kern_ndrv_refresh.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>

namespace {
const char *paths[] {"/System/Library/Extensions/IONDRVSupport.kext/IONDRVSupport"};
KernelPatcher::KextInfo ndrv {"com.apple.iokit.IONDRVSupport", paths, arrsize(paths), {true}, {},
    KernelPatcher::KextInfo::Unloaded};

constexpr size_t kRefreshRateOffset = 8; // IODisplayModeInformation::refreshRate (16.16)
constexpr uint32_t kRefresh60Hz = 60U << 16;

using GetInfo = int (*)(void *, int32_t, void *);
GetInfo orgGetInfo;

int wrapGetInfo(void *that, int32_t mode, void *info) {
    const int result = orgGetInfo(that, mode, info);
    uint32_t before = 0;
    if (!result && info) {
        auto &rate = *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(info) + kRefreshRateOffset);
        before = rate;
        if (!rate) rate = kRefresh60Hz;
    }
    static uint32_t logs;
    if (logs++ < 8)
        SYSLOG("ngreen", "V350: mode %d info result=0x%x refresh 0x%x", mode, result, before);
    return result;
}
}

void NDRVRefresh::init() {
    if (!checkKernelArgument("-ngndrv0hz")) lilu.onKextLoadForce(&ndrv);
}

bool NDRVRefresh::processKext(KernelPatcher &p, size_t index, mach_vm_address_t address, size_t size) {
    if (index != ndrv.loadIndex) return false;
    KernelPatcher::RouteRequest route {"__ZN17IONDRVFramebuffer28getInformationForDisplayModeEiP24IODisplayModeInformation",
        wrapGetInfo, orgGetInfo};
    const bool ok = p.routeMultiple(index, &route, 1, address, size);
    p.clearError();
    SYSLOG("ngreen", "V350: NDRV 0 Hz modes report 60 Hz: %d", ok);
    return true;
}
