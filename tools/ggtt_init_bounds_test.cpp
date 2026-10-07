// Offline model of the inspected TGL initWithOptions loop bounds.
// This is not a GPU emulation or a test of page-table coherency.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "../NootedGreen/kern_ggtt_bounds.hpp"
#include "../NootedGreen/kern_vf_ggtt_pte.hpp"

struct Bounds {
    uint64_t firstBegin, firstEnd;
    bool secondRuns;
    uint64_t secondBegin, secondEnd;
};

static Bounds nativeBounds(uint64_t start, uint64_t length) {
    return {start >> 12, (start + length - 1) >> 12,
            length != UINT64_C(0x100000000),
            (start + length) >> 12, (start + UINT64_C(0x100000000)) >> 12};
}

int main() {
	unsigned attributeCases = 0;
	for (uint64_t flags = 0; flags < 0x200; ++flags) {
		const bool expected = (flags & ~UINT64_C(0x9A)) == 0;
		assert(NGVfGgttPte::validAppleAttributes(flags) == expected);
		++attributeCases;
	}
	for (uint64_t bit = 9; bit < 64; ++bit)
		assert(!NGVfGgttPte::validAppleAttributes(UINT64_C(1) << bit));
	for (uint64_t physical : {UINT64_C(0), UINT64_C(0x1000),
	                          UINT64_C(0x12345000),
	                          (UINT64_C(1) << 39) - 0x1000}) {
		const uint64_t pte = NGVfGgttPte::encodeSystemMemory(physical);
		assert(pte == (physical | 1U));
		assert((pte & UINT64_C(0x1E)) == 0); // no LM or PF-owned VFID
		assert((pte & ~UINT64_C(0x7FFFFFF001)) == 0);
	}
	std::printf("PASS: %u Apple GGTT attribute masks and direct-VF PTE encoding\n",
	            attributeCases);

    using Invalidation = NGGgtt::TlbInvalidation;
    unsigned lifecycleCases = 0;
    for (unsigned ever = 0; ever < 2; ++ever)
    for (unsigned enabled = 0; enabled < 2; ++enabled)
    for (unsigned stopped = 0; stopped < 2; ++stopped)
    for (unsigned faulted = 0; faulted < 2; ++faulted)
    for (unsigned quiesced = 0; quiesced < 2; ++quiesced) {
        const auto actual = NGGgtt::unmapInvalidation(
            ever != 0, enabled != 0, stopped != 0, faulted != 0,
            quiesced != 0);
        const auto expected = (quiesced || !ever) ? Invalidation::NotRequired :
            (enabled && !stopped && !faulted ? Invalidation::Required :
                                               Invalidation::Unsafe);
        assert(actual == expected);
        ++lifecycleCases;
    }
    std::printf("PASS: %u GGTT unmap/transport lifecycle cases\n", lifecycleCases);

    const uint64_t values[] = {0, 1, 0xFFF, 0x1000, 0x2000, 0x100000,
        0x40000000, 0xFEE00000, 0xFFFFF000, UINT64_C(0x100000000),
        UINT64_C(0x100001000), UINT64_MAX - 0xFFF, UINT64_MAX};
    unsigned rangeCases = 0;
    for (auto base : values) for (auto size : values)
    for (auto start : values) for (auto length : values) {
        using Wide = unsigned __int128;
        const bool expected = ((base | size | start | length) & 0xFFF) == 0 &&
            size > 0 && Wide(base) + size <= (Wide(1) << 32) &&
            start >= base && Wide(start) + length <= Wide(base) + size;
        assert(NGGgtt::contains(base, size, start, length) == expected);
        ++rangeCases;
    }
    std::printf("PASS: %u GGTT range cases against 128-bit arithmetic oracle\n", rangeCases);

    unsigned segmentCases = 0;
    for (auto base : values) for (auto size : values) {
        using Wide = unsigned __int128;
        const bool assigned = ((base | size) & 0xFFF) == 0 && size > 0 &&
            base < (Wide(1) << 32) && Wide(base) + size <= (Wide(1) << 32);
        const Wide end = Wide(base) + size;
        const Wide lower = base > UINT64_C(0x40000000) ?
                           base : UINT64_C(0x40000000);
        const Wide upper = end < UINT64_C(0xFE000000) ?
                           end : UINT64_C(0xFE000000);
        const bool expectedValid = assigned && upper > lower;
        const auto plan = NGGgtt::vfSegmentPlan(base, size);
        assert(plan.valid == expectedValid);
        if (expectedValid) {
            assert(plan.globalStart == base);
            assert(plan.globalLength == size);
            assert(plan.unified32Start == lower);
            assert(plan.unified32Length == upper - lower);
            assert(NGGgtt::contains(base, size, plan.unified32Start,
                                    plan.unified32Length));
        } else {
            assert(plan.globalStart == 0 && plan.globalLength == 0);
            assert(plan.unified32Start == 0 && plan.unified32Length == 0);
        }
        ++segmentCases;
    }
    std::printf("PASS: %u VF segment-plan cases against 128-bit oracle\n", segmentCases);
    const uint64_t dmaValues[] = {0, 1, 0xFFF, 0x1000, 0x2000,
        (UINT64_C(1) << 39) - 0x1000, (UINT64_C(1) << 39) - 1,
        UINT64_C(1) << 39, (UINT64_C(1) << 39) + 0x1000,
        UINT64_MAX - 0xFFF, UINT64_MAX};
    unsigned dmaCases = 0;
    for (auto physical : dmaValues) for (auto length : dmaValues) {
        using Wide = unsigned __int128;
        const bool expected = physical != 0 &&
            ((physical | length) & 0xFFF) == 0 &&
            Wide(physical) + length <= (Wide(1) << 39);
        assert(NGGgtt::nativePhysicalRange(physical, length) == expected);
        ++dmaCases;
    }
    std::printf("PASS: %u native DMA address truncation/bounds cases\n", dmaCases);

    unsigned synchronizationCases = 0;
    for (bool present : {false, true})
    for (bool global : {false, true})
    for (uint64_t physical : {UINT64_C(0), UINT64_C(0x1000),
                              UINT64_C(0x12345000), UINT64_C(0x100000000000)})
    for (uint64_t flags : {UINT64_C(0), UINT64_C(0x5), UINT64_C(0x9A)}) {
        const auto entry = NGGgtt::classifySynchronizationEntry(
            present, global, physical, flags);
        const bool addressValid = physical != 0 && !(physical & 0xFFF) &&
            physical <= (UINT64_C(1) << 39) - 0x1000;
        const bool expectedMap = present && addressValid;
        const bool expectedValid = !present || (global && physical == 0) ||
            addressValid;
        assert(entry.valid == expectedValid);
        assert(entry.map == expectedMap);
        assert(entry.flags == (expectedMap ? (global ? 0 : flags) : 0));
        ++synchronizationCases;
    }
    std::printf("PASS: %u PF-owner placeholder/private-entry synchronization cases\n",
                synchronizationCases);
    const uint64_t windows[][2] = {{0x1000, 0x100000},
        {0xFED00000, 0x100000}, {0, UINT64_C(0x100000000)}};
    const uint64_t tops[] = {0, 0xFEE00000, UINT64_MAX};
    unsigned mappingCases = 0;
    for (auto cpu : values) for (auto allocated : values)
    for (auto required : values) for (auto gpu : values)
    for (const auto &window : windows) for (auto top : tops) {
        using Wide = unsigned __int128;
        const auto base = window[0], size = window[1];
        const bool expected = cpu && !(cpu & 0xFFF) && required &&
            allocated >= required && Wide(cpu) + required <= UINT64_MAX &&
            gpu && gpu < top && Wide(gpu) + required <= top &&
            ((base | size | gpu | required) & 0xFFF) == 0 && size &&
            Wide(base) + size <= (Wide(1) << 32) && gpu >= base &&
            Wide(gpu) + required <= Wide(base) + size;
        assert(NGGgtt::mappedBacking(cpu, allocated, required, gpu, base, size, top) == expected);
        ++mappingCases;
    }
    assert(NGGgtt::mappedBacking(0x1000, 0x8000, 0x8000, 0x100000,
                                 0x100000, 0x100000, 0xFEE00000));
    // Same low 32 bits as a valid mapping must not hide a high GPU address.
    assert(!NGGgtt::mappedBacking(0x1000, 0x8000, 0x8000, UINT64_C(0x100100000),
                                  0x100000, 0x100000, 0xFEE00000));
    std::printf("PASS: %u mapped CPU/GPU backing cases plus high-address alias regression\n", mappingCases);
    const auto suppressed = nativeBounds(UINT64_MAX, UINT64_C(0x100000000));
    assert(suppressed.firstBegin > suppressed.firstEnd);
    assert(!suppressed.secondRuns);
    // Regression: a valid nonzero VF assignment was incorrectly passed to
    // native init, whose clearing loop extends beyond the full PTE aperture.
    unsigned cases = 0;
    for (uint64_t base = 0x100000; base < 0xF0000000; base += 0x100000) {
        const auto unsafe = nativeBounds(base, 0x100000);
        assert(unsafe.secondRuns);
        assert(unsafe.secondEnd > 0x100000); // 8 MiB / 8-byte PTE
        ++cases;
    }
    std::printf("PASS: %u native GGTT init overflow models and suppressed loops\n", cases);
}
