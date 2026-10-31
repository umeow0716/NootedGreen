#pragma once
#include <Headers/kern_patcher.hpp>

// The firmware (NDRV) framebuffer has 0 Hz modes and no vblank, so macOS falls
// back to fixed 30/60 Hz pacing. Report 144 Hz modes and run a 144 Hz vblank.
namespace NDRVRefresh {
void init();
bool processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
}
