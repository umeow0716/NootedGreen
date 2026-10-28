#pragma once
#include <Headers/kern_patcher.hpp>

// Gen12 3D fixes for the 16.0.32 TGL donor, ported from WhateverXe
// (BSD-3-Clause, b00t0x). Offsets are relative to the donor's load address.
namespace XeRender {
void init();
bool processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
}
