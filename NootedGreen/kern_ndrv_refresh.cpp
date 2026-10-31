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
constexpr size_t kShmemOffset = 0x90;    // IOFramebuffer's StdFBShmem_t (see handleVBL)
constexpr size_t kVblTimeOffset = 0x40;  // StdFBShmem_t::vblTime
constexpr size_t kVblDeltaOffset = 0x48; // StdFBShmem_t::vblDelta

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

// V353: the firmware framebuffer has no cursor plane, so WindowServer draws the
// cursor into every frame and recomposites the whole display on each mouse
// move. Claim a hardware cursor that draws nothing: the Looking Glass client
// renders the cursor itself from the shape and position the guest host sends.
constexpr uint32_t kHardwareCursorAttribute = 'crsr';
constexpr size_t kHaveHardwareCursorOffset = 0x19e; // IOFramebuffer: crsr attribute cached by open()
bool hardwareCursor;
using GetAttribute = IOReturn (*)(void *, uint32_t, uintptr_t *);
GetAttribute orgGetAttribute;
IOReturn (*orgSetCursorImage)(void *, void *);
IOReturn (*orgSetCursorState)(void *, int32_t, int32_t, bool);

IOReturn wrapGetAttribute(void *that, uint32_t attribute, uintptr_t *value) {
    if (attribute == kHardwareCursorAttribute && value) {
        *value = 1;
        return kIOReturnSuccess;
    }
    return orgGetAttribute(that, attribute, value);
}

IOReturn wrapSetCursorImage(void *, void *) { return kIOReturnSuccess; }
IOReturn wrapSetCursorState(void *, int32_t, int32_t, bool) { return kIOReturnSuccess; }

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
    // handleVBL stamps the wakeup time and its distance to the previous one,
    // timer jitter included; a display's vblank is an exact grid.
    if (auto *shmem = getMember<uint8_t *>(v->fb, kShmemOffset)) {
        getMember<uint64_t>(shmem, kVblTimeOffset) = v->next;
        getMember<uint64_t>(shmem, kVblDeltaOffset) = period;
    }
    v->next += period;
    if (v->next <= now) v->next = now + period - (now - v->next) % period;
    // Default timer leeway lets it fire up to ~1 ms late, which shows as
    // vblank jitter and a stretched period (vblDelta is measured).
    uint64_t leeway;
    nanoseconds_to_absolutetime(20000, &leeway);
    v->timer->wakeAtTime(kIOTimeOptionsWithLeeway, v->next, leeway);
}

bool startVblank(void *, void *, IOService *fb, IONotifier *) {
    // Built by the first WindowServer, before the 144 Hz mode info above was in
    // place; the next WindowServer reuses it unless it is gone.
    fb->removeProperty("IOFBConfig");
    // open() ran at boot, before the cursor hooks; the next WindowServer's
    // cursor setup reads what it cached.
    if (hardwareCursor) getMember<uint8_t>(fb, kHaveHardwareCursorOffset) = 1;
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
    if (!checkKernelArgument("-ngnohwcursor")) {
        KernelPatcher::RouteRequest cursor[] {
            {"__ZN17IONDRVFramebuffer12getAttributeEjPm", wrapGetAttribute, orgGetAttribute},
            {"__ZN17IONDRVFramebuffer14setCursorImageEPv", wrapSetCursorImage, orgSetCursorImage},
            {"__ZN17IONDRVFramebuffer14setCursorStateEiib", wrapSetCursorState, orgSetCursorState},
        };
        hardwareCursor = p.routeMultiple(index, cursor, arrsize(cursor), address, size);
        SYSLOG("ngreen", "V353: NDRV hardware cursor: %d", hardwareCursor);
        p.clearError();
    }
    ndrvRouted = ok;
    maybeStart();
    return true;
}
