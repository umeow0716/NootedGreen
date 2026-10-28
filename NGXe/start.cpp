// WhateverXe ports for NootedGreen: the MTLCompilerService sandbox exception
// for the Gen12 compiler adapter and Gen12 render fixes for the TGL donor.
#include "access.hpp"
#include "render.hpp"
#include "lv.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>

static const char *bootargDebug = "-ngxedbg";

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowSafeMode,
    nullptr, 0,
    &bootargDebug, 1,
    nullptr, 0,
    KernelVersion::Sonoma,
    KernelVersion::Tahoe,
    []() {
        XeDataAccess::init();
        XeRender::init();
        XeLV::init();
        lilu.onPatcherLoadForce([](void *, KernelPatcher &p) {
            XeDataAccess::processKernel(p);
            XeLV::processKernel(p);
        });
        lilu.onKextLoadForce(nullptr, 0,
            [](void *, KernelPatcher &p, size_t index, mach_vm_address_t address, size_t size) {
                XeDataAccess::processKext(p, index, address, size);
                XeRender::processKext(p, index, address, size);
                XeLV::processKext(p, index, address, size);
            });
    },
};
