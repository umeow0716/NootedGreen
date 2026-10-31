#include "kern_ndrv_refresh.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

namespace {
const char *ndrvPaths[] {"/System/Library/Extensions/IONDRVSupport.kext/IONDRVSupport"};
const char *iofbPaths[] {"/System/Library/Extensions/IOGraphicsFamily.kext/IOGraphicsFamily"};
KernelPatcher::KextInfo kexts[] {
    {"com.apple.iokit.IOGraphicsFamily", iofbPaths, arrsize(iofbPaths), {true}, {}, KernelPatcher::KextInfo::Unloaded},
    {"com.apple.iokit.IONDRVSupport", ndrvPaths, arrsize(ndrvPaths), {true}, {}, KernelPatcher::KextInfo::Unloaded},
};

constexpr uint32_t kRefreshHz = 144;
constexpr uint64_t kPeriodNs = 1000000000ULL / kRefreshHz;
constexpr size_t kRefreshRateOffset = 8; // IODisplayModeInformation::refreshRate (16.16)

using GetInfo = int (*)(void *, int32_t, void *);
GetInfo orgGetInfo;
using HandleVBL = void (*)(IOService *, void *);
HandleVBL handleVBL;

int wrapGetInfo(void *that, int32_t mode, void *info) {
    const int result = orgGetInfo(that, mode, info);
    if (!result && info) {
        auto &rate = *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(info) + kRefreshRateOffset);
        if (!rate) rate = kRefreshHz << 16;
    }
    return result;
}

// The firmware framebuffer has no vblank interrupt, so drive IOFramebuffer's
// vblank handler from a timer: it stamps vblTime/vblDelta in the shared
// cursor memory that CoreDisplay paces WindowServer and the display link by.
struct Vblank {
    IOService *fb;
    IOTimerEventSource *timer;
    uint64_t next;
};

void vblankFire(Vblank *v) {
    handleVBL(v->fb, nullptr);
    uint64_t now = mach_absolute_time(), period;
    nanoseconds_to_absolutetime(kPeriodNs, &period);
    v->next += period;
    if (v->next <= now) v->next = now + period - (now - v->next) % period;
    // Default timer leeway lets it fire up to ~1 ms late, which shows as
    // vblank jitter and a stretched period (vblDelta is measured).
    uint64_t leeway;
    nanoseconds_to_absolutetime(20000, &leeway);
    v->timer->wakeAtTime(IOTimerEventSource::kIOTimeOptionsWithLeeway, v->next, leeway);
}

bool startVblank(void *, void *, IOService *fb, IONotifier *) {
    // Built by the first WindowServer, before the 144 Hz mode info above was in
    // place; the next WindowServer reuses it unless it is gone.
    fb->removeProperty("IOFBConfig");
    auto *wl = fb->getWorkLoop();
    if (!handleVBL || !wl) return true;
    auto *v = new Vblank {fb, nullptr, mach_absolute_time()};
    v->timer = IOTimerEventSource::timerEventSource(fb, [](OSObject *, IOTimerEventSource *t) {
        // owner is the framebuffer; the state rides in the timer's refCon.
        vblankFire(static_cast<Vblank *>(t->getRefcon()));
    });
    if (!v->timer || wl->addEventSource(v->timer) != kIOReturnSuccess) {
        SYSLOG("ngreen", "V351: vblank timer for %s failed", fb->getName());
        return true;
    }
    v->timer->setRefcon(v);
    v->timer->setTimeoutUS(1);
    SYSLOG("ngreen", "V351: %u Hz vblank for %s", kRefreshHz, fb->getName());
    return true;
}

bool ndrvRouted, graphicsSeen;

// IOGraphicsFamily (vblank handler) and IONDRVSupport (mode info) come in
// either order; start the vblank once both are in.
void maybeStart() {
    if (graphicsSeen && ndrvRouted)
        IOService::addMatchingNotification(gIOFirstPublishNotification, IOService::serviceMatching("IONDRVFramebuffer"),
            startVblank, nullptr);
}
} // namespace

void NDRVRefresh::init() {
    if (!checkKernelArgument("-ngndrv0hz")) lilu.onKextLoadForce(kexts, arrsize(kexts));
}

bool NDRVRefresh::processKext(KernelPatcher &p, size_t index, mach_vm_address_t address, size_t size) {
    if (index == kexts[0].loadIndex) {
        handleVBL = p.solveSymbol<HandleVBL>(index, "__ZN13IOFramebuffer9handleVBLEPS_Pv", address, size);
        p.clearError();
        SYSLOG("ngreen", "V351: vblank handler %d", handleVBL != nullptr);
        graphicsSeen = true;
        maybeStart();
        return true;
    }
    if (index != kexts[1].loadIndex) return false;
    KernelPatcher::RouteRequest route {"__ZN17IONDRVFramebuffer28getInformationForDisplayModeEiP24IODisplayModeInformation",
        wrapGetInfo, orgGetInfo};
    const bool ok = p.routeMultiple(index, &route, 1, address, size);
    p.clearError();
    SYSLOG("ngreen", "V351: NDRV modes report %u Hz: %d", kRefreshHz, ok);
    ndrvRouted = ok;
    maybeStart();
    return true;
}
