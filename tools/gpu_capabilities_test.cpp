#include "../NootedGreen/kern_gpu_capabilities.hpp"
#include <cassert>
#include <cstdio>
#ifdef NGREEN_REFERENCE_PCIIDS
#include NGREEN_REFERENCE_PCIIDS
#endif

int main() {
    using namespace NGGpuCapabilities;
    assert(sriov(0xA7A8) == Sriov::Present); // host's RPL-P
    assert(sriov(0x9A49) == Sriov::Present); // native TGL also supports VF_CAP
    assert(sriov(0x8A52) == Sriov::Absent);  // ICL: do not read Gen12 VF_CAP
    assert(sriov(0x4C80) == Sriov::Absent);  // RKL != ADL
    assert(sriov(0x7D55) == Sriov::Present);
    assert(sriov(0) == Sriov::Unknown);
    assert(sriov(0xFFFF) == Sriov::Unknown);
    assert(sriov(0x1A7A8U) == Sriov::Unknown);
    assert(sriov(0xA7FF) == Sriov::Unknown); // no broad family-mask guesses

    assert(!isTigerLake(0x19A49U));
    unsigned present = 0, absent = 0, tigerLake = 0;
#ifdef NGREEN_REFERENCE_PCIIDS
    // Expand the primary-source ID macros, independently of our switch.
    // Membership follows has_sriov in i915_pci.c from the same source tree.
#define ID(value, ...) value
    const uint32_t tglIds[] = { INTEL_TGL_IDS(ID) };
    const uint32_t withSriov[] = {
        INTEL_TGL_IDS(ID), INTEL_ADLS_IDS(ID), INTEL_ADLP_IDS(ID),
        INTEL_ADLN_IDS(ID), INTEL_RPLS_IDS(ID), INTEL_RPLU_IDS(ID),
        INTEL_RPLP_IDS(ID), INTEL_MTL_IDS(ID), INTEL_ARL_IDS(ID),
    };
    const uint32_t withoutSriov[] = {
        INTEL_ICL_IDS(ID), INTEL_EHL_IDS(ID), INTEL_JSL_IDS(ID),
        INTEL_RKL_IDS(ID), INTEL_DG1_IDS(ID), INTEL_DG2_IDS(ID), INTEL_ATS_M_IDS(ID),
    };
#undef ID
#endif
    for (uint32_t id = 0; id <= 0xFFFF; ++id) {
        const auto actual = sriov(id);
        present += actual == Sriov::Present;
        absent += actual == Sriov::Absent;
        tigerLake += isTigerLake(id);
#ifdef NGREEN_REFERENCE_PCIIDS
        bool expectedTgl = false;
        for (auto known : tglIds) expectedTgl |= known == id;
        assert(isTigerLake(id) == expectedTgl);
        Sriov expected = Sriov::Unknown;
        for (auto known : withSriov)
            if (known == id) { assert(expected == Sriov::Unknown); expected = Sriov::Present; }
        for (auto known : withoutSriov)
            if (known == id) { assert(expected == Sriov::Unknown); expected = Sriov::Absent; }
        assert(actual == expected);
#endif
    }
    assert(present == 70 && absent == 65 && tigerLake == 11);
    std::printf("PASS: 65536 PCI IDs (%u VF_CAP-capable, %u without SR-IOV)%s\n",
                present, absent,
#ifdef NGREEN_REFERENCE_PCIIDS
                "; compared with primary-source PCI macros"
#else
                ""
#endif
    );
}
