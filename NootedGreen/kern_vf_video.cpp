//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_vf_internal.hpp"

// After a GuC context reset the image holds the saved state: log where the
// ring stopped and which batch it was in.
// The leaf PTE of a GPU virtual address in a 4-level PPGTT (0 if unmapped).
// V386: 1 GB / 2 MB entries (PS, bit 7) end the walk; the result is then the
// 4 KB frame's address within the large page. chain gets each level's entry.
uint64_t vfPpgttLeaf(uint64_t root, uint64_t va, uint64_t *chain) {
	uint64_t table = root & 0x7FFFFFF000ULL, entry = 0;
	for (uint32_t level = 0; table && level < 4; level++) {
		auto *desc = IOMemoryDescriptor::withPhysicalAddress(table, PAGE_SIZE, kIODirectionIn);
		auto *map = desc ? desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly) : nullptr;
		entry = map ? reinterpret_cast<volatile uint64_t *>(map->getVirtualAddress())[(va >> (39 - 9 * level)) & 511] : 0;
		OSSafeReleaseNULL(map);
		OSSafeReleaseNULL(desc);
		if (chain)
			chain[level] = entry;
		if (!(entry & 1))
			return 0;
		if ((level == 1 || level == 2) && (entry & 0x80)) {
			const uint64_t size = 1ULL << (39 - 9 * level);
			return ((entry & 0x7FFFFFF000ULL & ~(size - 1)) + (va & (size - 1) & ~0xFFFULL)) | (entry & 0xFFF);
		}
		table = level < 3 ? (entry & 0x7FFFFFF000ULL) : 0;
	}
	return entry;
}

// Copy dwords at a GPU virtual address of a 4-level PPGTT (false if unmapped).
bool vfPpgttRead(uint64_t root, uint64_t va, uint32_t *out, uint32_t count) {
	for (uint32_t i = 0; i < count;) {
		const uint64_t leaf = vfPpgttLeaf(root, va + i * 4);
		if (!leaf)
			return false;
		auto *desc = IOMemoryDescriptor::withPhysicalAddress(leaf & 0x7FFFFFF000ULL, PAGE_SIZE, kIODirectionIn);
		auto *map = desc ? desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly) : nullptr;
		if (!map) {
			OSSafeReleaseNULL(desc);
			return false;
		}
		auto *page = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
		for (uint32_t k = ((va + i * 4) & 0xFFF) / 4; k < 1024 && i < count; k++)
			out[i++] = page[k];
		map->release();
		OSSafeReleaseNULL(desc);
	}
	return true;
}

void vfWakeVideoBatch(uint64_t root, volatile uint32_t *ring, uint32_t ringSize, uint32_t from, uint32_t to) {
	const uint32_t words = ringSize / 4;
	if (!root || !ring || !words || (words & (words - 1)))
		return;
	for (uint32_t i = from / 4 % words, end = to / 4 % words, guard = 0; i != end && guard < 256; i = (i + 1) & (words - 1), guard++) {
		if (ring[i] != 0x18800101)
			continue;
		const uint64_t bb = ring[(i + 1) & (words - 1)] | (static_cast<uint64_t>(ring[(i + 2) & (words - 1)] & 0xFFFF) << 32);
		const uint64_t leaf = vfPpgttLeaf(root, bb);
		if (!leaf || (bb & 0xFFF) > 0xFEC)
			continue;
		auto *desc = IOMemoryDescriptor::withPhysicalAddress(leaf & 0x7FFFFFF000ULL, PAGE_SIZE, kIODirectionInOut);
		auto *map = desc ? desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere) : nullptr;
		if (map) {
			auto *b = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress() + (bb & 0xFFF));
			static uint32_t dumpedBatches = 0;
			if (checkKernelArgument("-ngvadump") && b[5] != 0x05000000 && dumpedBatches++ < 3)
				for (uint32_t k = 0; k + 7 < (PAGE_SIZE - (bb & 0xFFF)) / 4 && k < 512; k += 8) {
					SYSLOG("ngreen", "V299: batch %llx +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", static_cast<unsigned long long>(bb),
					       k * 4, b[k], b[k + 1], b[k + 2], b[k + 3], b[k + 4], b[k + 5], b[k + 6], b[k + 7]);
					bool end = false;
					for (uint32_t e = k; e < k + 8; e++) end |= b[e] == 0x05000000;
					if (end) break;
				}
			// V383: HEVC batches (HCP or HuC pipe mode select) run only when ngva
			// fixed them and marked them (MI_NOOP ID 0x4e47); the rest end at
			// their first dword, so the video engine runs nothing.
			if (b[0] != 0x05000000) {
				const uint32_t room = (PAGE_SIZE - (bb & 0xFFF)) / 4;
				bool marked = false;
				for (uint32_t k = 0; k < room && b[k] != 0x05000000; k++) {
					marked |= b[k] == 0x00404E47;
					if ((b[k] & 0xFFFF0000) != 0x73800000 && (b[k] & 0xFFFF0000) != 0x75800000)
						continue;
					static uint32_t runs = 0, drops = 0;
					if (marked) {
						if (runs++ < 4)
							SYSLOG("ngreen", "V383: HEVC batch %llx marked by ngva", static_cast<unsigned long long>(bb));
						break;
					}
					if (drops++ < 8)
						SYSLOG("ngreen", "V383: dropped HEVC batch %llx (%08x at +%03x)", static_cast<unsigned long long>(bb), b[k], k * 4);
					b[0] = 0x05000000;
					__asm__ volatile("clflush (%0)\n\tmfence" :: "r"(&b[0]) : "memory");
					break;
				}
			}
			map->release();
		}
		OSSafeReleaseNULL(desc);
	}
}

// V281: the surface states a hung batch binds to the pixel shader. Takes the
// surface state base from STATE_BASE_ADDRESS and every PS binding table
// pointer in the batch page, and dumps the first entries' RENDER_SURFACE_STATE.
void vfDumpPsSurfaces(uint32_t id, uint64_t root, volatile uint32_t *page, uint64_t &surfaceBase) {
	static uint32_t dumps = 0;
	for (uint32_t i = 0; i + 4 < 1024 && dumps < 48; i++) {
		if (page[i] == 0x61010014 && (page[i + 4] & 1))
			surfaceBase = (page[i + 4] & ~0xFFFU) | (static_cast<uint64_t>(page[i + 5] & 0xFFFF) << 32);
		if (page[i] != 0x782A0000 || !surfaceBase)
			continue;
		uint32_t table[4] {};
		const uint64_t tableVa = surfaceBase + (page[i + 1] & 0xFFFE0);
		if (!vfPpgttRead(root, tableVa, table, 4)) {
			SYSLOG("ngreen", "V281: id=%u binding table %llx unmapped", id, static_cast<unsigned long long>(tableVa));
			continue;
		}
		for (uint32_t e = 0; e < 4 && dumps < 48; e++) {
			uint32_t ss[16] {};
			if (!table[e] || !vfPpgttRead(root, surfaceBase + (table[e] & ~0x3FU), ss, 16))
				continue;
			dumps++;
			const uint64_t surface = ss[8] | (static_cast<uint64_t>(ss[9] & 0xFFFF) << 32);
			// V306: every page the surface spans (pitch x height, Y-tiled rows padded to 32).
			const uint32_t height = ((ss[2] >> 16) & 0x3FFF) + 1, pitch = (ss[3] & 0x3FFFF) + 1;
			const uint64_t bytes = static_cast<uint64_t>(pitch) * (((ss[0] >> 12) & 3) == 3 ? (height + 31) & ~31U : height);
			uint32_t pages = 0, holes = 0;
			uint64_t firstHole = 0;
			for (uint64_t at = surface & ~0xFFFULL; at < surface + bytes && pages < 4096; at += PAGE_SIZE, pages++)
				if (!(vfPpgttLeaf(root, at) & 0x7FFFFFF000ULL) && !holes++) firstHole = at; // absent or scratch-at-0
			SYSLOG("ngreen", "V281: id=%u surface %llx pte=%llx pages=%u unmapped=%u first=%llx", id, static_cast<unsigned long long>(surface),
			       static_cast<unsigned long long>(vfPpgttLeaf(root, surface)), pages, holes, static_cast<unsigned long long>(firstHole));
			SYSLOG("ngreen", "V281: id=%u bt %llx[%u] ss %llx: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
			       id, static_cast<unsigned long long>(tableVa), e,
			       static_cast<unsigned long long>(surfaceBase + (table[e] & ~0x3FU)), ss[0], ss[1], ss[2], ss[3],
			       ss[4], ss[5], ss[6], ss[7], ss[8], ss[9], ss[10], ss[11], ss[12], ss[13], ss[14], ss[15]);
		}
	}
}
