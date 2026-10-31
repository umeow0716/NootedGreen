#pragma once
#include <Headers/kern_patcher.hpp>

// The firmware (NDRV) framebuffer reports a 0 Hz mode, for which macOS paces
// the display link, and so animation, at 30 Hz. Report 60 Hz instead.
namespace NDRVRefresh {
void init();
bool processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
}
