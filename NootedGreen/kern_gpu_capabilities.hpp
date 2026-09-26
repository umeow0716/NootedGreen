/*
 * PCI identifiers derived from Intel's include/drm/intel/pciids.h.
 * Copyright 2013 Intel Corporation. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#ifndef NGREEN_GPU_CAPABILITIES_HPP
#define NGREEN_GPU_CAPABILITIES_HPP
#include <stdint.h>

namespace NGGpuCapabilities {
// Capability to inspect VF_CAP, NOT proof of working driver support. Unknown
// devices must not be probed using another generation's register layout.
enum class Sriov : uint8_t { Unknown, Absent, Present };

inline bool isTigerLake(uint32_t device) {
    switch (device) {
        case 0x9A60: case 0x9A68: case 0x9A70: case 0x9A40: case 0x9A49:
        case 0x9A59: case 0x9A78: case 0x9AC0: case 0x9AC9: case 0x9AD9: case 0x9AF8:
            return true;
        default:
            return false;
    }
}

// Source: i915-sriov-dkms-2026.03.05.7 pciids.h plus i915_pci.c has_sriov.
// Exact IDs intentionally avoid broad family masks, CPU model and BAR size.
inline Sriov sriov(uint32_t device) {
    switch (device) {
        // ICL, EHL, JSL
        case 0x8A50: case 0x8A51: case 0x8A52: case 0x8A53: case 0x8A54:
        case 0x8A56: case 0x8A57: case 0x8A58: case 0x8A59: case 0x8A5A:
        case 0x8A5B: case 0x8A5C: case 0x8A5D: case 0x8A70: case 0x8A71:
        case 0x4541: case 0x4551: case 0x4555: case 0x4557: case 0x4570: case 0x4571:
        case 0x4E51: case 0x4E55: case 0x4E57: case 0x4E61: case 0x4E71:
        // RKL, DG1
        case 0x4C80: case 0x4C8A: case 0x4C8B: case 0x4C8C: case 0x4C90: case 0x4C9A:
        case 0x4905: case 0x4906: case 0x4907: case 0x4908: case 0x4909:
        // DG2 and ATS-M (no has_sriov in this i915 device table)
        case 0x56A0: case 0x56A1: case 0x56A2: case 0x56BE: case 0x56BF:
        case 0x5690: case 0x5691: case 0x5692: case 0x56A5: case 0x56A6:
        case 0x56B0: case 0x56B1: case 0x56BA: case 0x56BB: case 0x56BC: case 0x56BD:
        case 0x5693: case 0x5694: case 0x5695: case 0x56A3: case 0x56A4:
        case 0x56B2: case 0x56B3: case 0x5696: case 0x5697:
        case 0x56C0: case 0x56C2: case 0x56C1:
            return Sriov::Absent;
        // TGL
        case 0x9A60: case 0x9A68: case 0x9A70: case 0x9A40: case 0x9A49:
        case 0x9A59: case 0x9A78: case 0x9AC0: case 0x9AC9: case 0x9AD9: case 0x9AF8:
        // ADL-S, ADL-P, ADL-N
        case 0x4680: case 0x4682: case 0x4688: case 0x468A: case 0x468B:
        case 0x4690: case 0x4692: case 0x4693:
        case 0x46A0: case 0x46A1: case 0x46A2: case 0x46A3: case 0x46A6:
        case 0x46A8: case 0x46AA: case 0x462A: case 0x4626: case 0x4628:
        case 0x46B0: case 0x46B1: case 0x46B2: case 0x46B3:
        case 0x46C0: case 0x46C1: case 0x46C2: case 0x46C3:
        case 0x46D0: case 0x46D1: case 0x46D2: case 0x46D3: case 0x46D4:
        // RPL-S, RPL-U, RPL-P
        case 0xA780: case 0xA781: case 0xA782: case 0xA783:
        case 0xA788: case 0xA789: case 0xA78A: case 0xA78B:
        case 0xA721: case 0xA7A1: case 0xA7A9: case 0xA7AC: case 0xA7AD:
        case 0xA720: case 0xA7A0: case 0xA7A8: case 0xA7AA: case 0xA7AB:
        // MTL, ARL
        case 0x7D40: case 0x7D45: case 0x7D55: case 0x7D60: case 0x7DD5:
        case 0x7D51: case 0x7DD1: case 0x7D41: case 0x7D67: case 0xB640:
            return Sriov::Present;
        default:
            return Sriov::Unknown;
    }
}
}
#endif
