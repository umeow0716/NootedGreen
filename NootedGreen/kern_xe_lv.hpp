#pragma once
#include <Headers/kern_patcher.hpp>

// Library validation exception for our own Metal driver shim. AMFI's verdict
// stands for every other file; only the exact shim path is admitted.
namespace XeLV {
void init();
void processKernel(KernelPatcher &patcher);
bool processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
}
