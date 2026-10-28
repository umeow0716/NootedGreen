#include "kern_xe_lv.hpp"
#include <Headers/kern_api.hpp>
#include <IOKit/IORegistryEntry.h>

namespace {
const char *paths[] {"/System/Library/Extensions/AppleMobileFileIntegrity.kext/Contents/MacOS/AppleMobileFileIntegrity"};
KernelPatcher::KextInfo amfi {"com.apple.driver.AppleMobileFileIntegrity", paths, arrsize(paths), {true}, {},
    KernelPatcher::KextInfo::Unloaded};

// The UMD links @loader_path/impostor.dylib for its load-time constructor
// only, so our shim takes that name.
const char *const allowed[] {
    "/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/impostor.dylib",
    "/System/Volumes/Data/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/impostor.dylib",
};

void *(*fileVnode)(void *);
int (*getPath)(void *, char *, int *);

using CheckLV = int (*)(void *, void *, int64_t, int64_t, unsigned long);
CheckLV orgCheckLV;

int wrapCheckLV(void *proc, void *fg, int64_t offset, int64_t errorMessage, unsigned long errorSize) {
    const int result = orgCheckLV(proc, fg, offset, errorMessage, errorSize);
    if (!result || !fg) return result;
    auto vnode = fileVnode(fg);
    if (!vnode) return result;
    char path[1024];
    int length = sizeof(path);
    if (getPath(vnode, path, &length)) return result;
    for (auto p : allowed)
        if (!strcmp(path, p)) {
            static bool logged;
            if (!logged) {
                logged = true;
                SYSLOG("ngxe", "library validation admitted %s", path);
            }
            return 0;
        }
    return result;
}
}

void XeLV::init() {
    lilu.onKextLoadForce(&amfi);
}

void XeLV::processKernel(KernelPatcher &p) {
    fileVnode = reinterpret_cast<decltype(fileVnode)>(p.solveSymbol(KernelPatcher::KernelID, "_fg_get_vnode"));
    p.clearError();
    getPath = reinterpret_cast<decltype(getPath)>(p.solveSymbol(KernelPatcher::KernelID, "_vn_getpath"));
    p.clearError();
}

bool XeLV::processKext(KernelPatcher &p, size_t index, mach_vm_address_t address, size_t size) {
    if (index != amfi.loadIndex) return false;
    bool ok = fileVnode && getPath;
    if (ok) {
        KernelPatcher::RouteRequest route {"__Z30_file_check_library_validationP4procP8fileglobxxm", wrapCheckLV, orgCheckLV};
        ok = p.routeMultiple(index, &route, 1, address, size);
        p.clearError();
    }
    IORegistryEntry::getRegistryRoot()->setProperty("NGXeLibraryValidation", ok);
    SYSLOG("ngxe", "NGXeLibraryValidation=%d", ok);
    return true;
}
