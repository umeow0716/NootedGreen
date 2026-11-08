//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_vf_internal.hpp"

// After a GuC context reset the image holds the saved state: log where the
// ring stopped and which batch it was in.
// The leaf PTE of a GPU virtual address in a 4-level PPGTT (0 if unmapped).
uint64_t vfPpgttLeaf(uint64_t root, uint64_t va) {
	uint64_t table = root & 0x7FFFFFF000ULL, entry = 0;
	for (uint32_t level = 0; table && level < 4; level++) {
		auto *desc = IOMemoryDescriptor::withPhysicalAddress(table, PAGE_SIZE, kIODirectionIn);
		auto *map = desc ? desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly) : nullptr;
		entry = map ? reinterpret_cast<volatile uint64_t *>(map->getVirtualAddress())[(va >> (39 - 9 * level)) & 511] : 0;
		OSSafeReleaseNULL(map);
		OSSafeReleaseNULL(desc);
		if (!(entry & 1))
			return 0;
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

// V297: Apple's H.264 PAK batches (VCS) start with an empty MI_FLUSH_DW and no
// MI_FORCE_WAKEUP; with VDBOX power gating the MFX never takes the state
// commands. Turn that flush into MI_FORCE_WAKEUP(MFX power well) + MI_NOOPs.
// V300: Apple's AVC VDEnc emits Gen9 (KBL) VDENC commands. Rewrite the batch
// in place with NgreenAvcGen12::produce (Gen12 lengths, MFX_WAITs, surface
// formats, MOCS and MFC status registers), when the result fits the page.
static void vfVdencToGen12(volatile uint32_t *b, uint32_t room, uint64_t bb) {
	static uint32_t in[1024], out[1536];
	uint32_t end = 0, width = 0, height = 0;
	for (uint32_t i = 0; i < room && i < 1024; i++) {
		in[i] = b[i];
		if (in[i] == 0x05000000) { end = i; break; }
	}
	for (uint32_t i = 0; i + 2 < end; i++)
		if (in[i] == 0x70810004) { width = ((in[i + 2] >> 4) & 0x3FFF) + 1; height = (in[i + 2] >> 18) + 1; break; }
	if (!end || !width) {
		static uint32_t skips = 0;
		if (skips++ < 16)
			SYSLOG("ngreen", "V300: video batch %llx not converted: %s (room %u dwords)", static_cast<unsigned long long>(bb),
			       end ? "no VDENC_SRC" : "no BB_END in page", room);
		return;
	}
	const auto r = NgreenAvcGen12::produce(in, end, width, height, out, 1536);
	static uint32_t logs = 0, failures = 0;
	if (!r.ok || r.words + 1 > room) {
		if (failures++ < 32)
			SYSLOG("ngreen", "V300: video batch %llx %ux%u not converted: %s", static_cast<unsigned long long>(bb), width, height, r.reason);
		// The first rejected batch in full, to extend the profile offline.
		for (uint32_t i = 0; failures == 1 && i < end; i += 8)
			SYSLOG("ngreen", "V300: rejected +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", i * 4, in[i], in[i + 1], in[i + 2],
			       in[i + 3], in[i + 4], in[i + 5], in[i + 6], in[i + 7]);
		return;
	}
	// Apple leaves the MFX stream-out (DW10), MB status (DW52) and slice size
	// stream-out (DW65) addresses 0, but Gen12 VDEnc writes them (Intel points
	// them at its PAK statistics buffers): a page fault at 0. Use the unused
	// VDEnc stream-in buffer (stream-in is disabled) as their scratch.
	uint32_t mfxBuf = 0, streamIn = 0, streamInHigh = 0;
	for (uint32_t k = 0; k < r.words; k++) {
		if (out[k] == 0x70020042 && !mfxBuf) mfxBuf = k;
		if (out[k] == 0x70840045 && k + 14 < r.words) { streamIn = out[k + 13]; streamInHigh = out[k + 14]; break; }
	}
	if (mfxBuf && streamIn) {
		static const uint8_t slots[] = {10, 52, 65};
		static const uint32_t offsets[] = {0x10000, 0x20000, 0};
		for (uint32_t s = 0; s < 3; s++) {
			uint32_t *dw = &out[mfxBuf + slots[s]];
			if (!dw[0] && !dw[1]) { dw[0] = streamIn + offsets[s]; dw[1] = streamInHigh; }
		}
	}
	// V304: log PAK-BSE (bitstream output) buffer sizes as they change.
	for (uint32_t k = 0; k + 25 < r.words; k++) {
		if (out[k] != 0x70030018) continue;
		static uint32_t lastSize = 0, sizeLogs = 0;
		const uint32_t size = out[k + 24] - out[k + 21];
		if (size != lastSize && sizeLogs++ < 16)
			SYSLOG("ngreen", "V304: video batch %llx %ux%u PAK-BSE %08x size %x", static_cast<unsigned long long>(bb), width, height, out[k + 21], size);
		lastSize = size;
		break;
	}
	// Gen12 VDENC_IMG_STATE takes the frame QP in DW14 (KBL had no such field,
	// Apple leaves it 0): VDEnc then quantizes at QP 0 and PAK codes mb_qp_delta
	// back to the slice QP, so rate control never takes effect. Use the first
	// MFX_AVC_SLICE_STATE's slice QP like media-driver.
	for (uint32_t k = 0, image = 0; k + 14 < r.words; k++) {
		if (out[k] == 0x70850021) image = k;
		if (out[k] == 0x71030009 && image) { out[image + 14] = (out[image + 14] & ~0xFFU) | ((out[k + 3] >> 16) & 0x3F); break; }
	}
	// MFX_AVC_IMG_STATE DW4 from Apple has MbaffFrameFlag and ChromaFormatIdc 3
	// set; media-driver Gen12 programs frame-only 4:2:0 (0x159c for this case).
	for (uint32_t k = 0; k + 4 < r.words; k++)
		if (out[k] == 0x71000013) {
			out[k + 4] = (out[k + 4] & ~0xC02U) | 0x400U;
			// DW6: Apple caps inter MBs at the intra limit (0xa8c bits); media-driver
			// leaves InterMbMaxSz at 0xfff. Busy inter MBs past the cap fail the frame.
			out[k + 6] = (out[k + 6] & 0xFFFFU) | 0x0FFF0000U;
			break;
		}
	for (uint32_t k = 0; k < r.words; k++)
		b[k] = out[k];
	b[r.words] = 0x05000000;
	for (uint32_t k = 0; k <= r.words; k += 16)
		__asm__ volatile("clflush (%0)" :: "r"(&b[k]) : "memory");
	__asm__ volatile("clflush (%0)" :: "r"(&b[r.words]) : "memory");
	__asm__ volatile("mfence" ::: "memory");
	if (logs++ < 4)
		SYSLOG("ngreen", "V300: video batch %llx %ux%u VDEnc to Gen12, %u -> %u dwords", static_cast<unsigned long long>(bb), width, height, end, static_cast<uint32_t>(r.words));
}

// Write back CPU caches over a GPU virtual range of a 4-level PPGTT.
static void vfFlushGpuRange(uint64_t root, uint64_t va, uint64_t length) {
	for (uint64_t at = va & ~63ULL, end = va + length; at < end;) {
		const uint64_t leaf = vfPpgttLeaf(root, at), next = (at | 0xFFF) + 1;
		auto *desc = leaf ? IOMemoryDescriptor::withPhysicalAddress(leaf & 0x7FFFFFF000ULL, PAGE_SIZE, kIODirectionInOut) : nullptr;
		auto *map = desc ? desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere) : nullptr;
		if (map) {
			const auto page = map->getVirtualAddress();
			for (; at < next && at < end; at += 64)
				__asm__ volatile("clflush (%0)" :: "r"(page + (at & 0xFFF)) : "memory");
			map->release();
		}
		OSSafeReleaseNULL(desc);
		at = next;
	}
	__asm__ volatile("mfence" ::: "memory");
}

// V301: the GPU reads the AVC bitstream without snooping; AppleGVA's copy of
// it may still sit in CPU caches. Flush every MFD_AVC_BSD_OBJECT's slice data
// (MFX_IND_OBJ_BASE_ADDR_STATE base + start, length) before submission.
static void vfFlushDecodeBitstream(uint64_t root, volatile uint32_t *b, uint32_t room, uint64_t bb) {
	uint64_t base = 0;
	uint32_t attr = 0;
	static uint32_t logs = 0;
	for (uint32_t i = 0; i + 3 < room; i++) {
		if (b[i] == 0x05000000)
			break;
		if (b[i] == 0x70000003U && (b[i + 1] & 0x40010U) == 0x40000U) {
			// V312: decoder MFX_PIPE_MODE_SELECT has Extended Stream-Out Enable (bit 18) set
			// with no stream-out buffer: the MFX writes to GPU address 0 (host DMAR faults).
			b[i + 1] &= ~0x40000U;
			__asm__ volatile("clflush (%0)" :: "r"(&b[i + 1]) : "memory");
		} else if (b[i] == 0x70010004U && (b[i + 3] & 0xF8000000U) == 0x08000000U && i + 5 < room) {
			// V303: AppleGVA's decoder MFX_SURFACE_STATE has format 0 (YCRCB_NORMAL),
			// reserved bit 20 and a Y-tiled pitch one short of 128-byte aligned, and no
			// Cr offset: program NV12 like media-driver (PLANAR_420_8, aligned pitch).
			const uint32_t pitch = (((b[i + 3] >> 3) & 0x1FFFF) + 1 + 127) & ~127U;
			const uint32_t dw3 = 0x48000000U | ((pitch - 1) << 3) | (b[i + 3] & 7);
			if (logs < 8)
				SYSLOG("ngreen", "V303: batch %llx MFX_SURFACE dw3 %08x -> %08x dw5 %08x -> %08x", static_cast<unsigned long long>(bb),
				       b[i + 3], dw3, b[i + 5], b[i + 5] ? b[i + 5] : b[i + 4]);
			b[i + 3] = dw3;
			if (!b[i + 5]) b[i + 5] = b[i + 4];
			__asm__ volatile("clflush (%0)" :: "r"(&b[i + 3]) : "memory");
			__asm__ volatile("clflush (%0)" :: "r"(&b[i + 5]) : "memory");
		} else if ((b[i] & 0xFFFF0000U) == 0x70030000U) {
			base = b[i + 1] | (static_cast<uint64_t>(b[i + 2] & 0xFFFF) << 32);
			attr = b[i + 3];
		} else if ((b[i] & 0xFFFF0000U) == 0x71280000U && base) {
			const uint32_t length = b[i + 1], start = b[i + 2] & 0x1FFFFFFF;
			vfFlushGpuRange(root, base + start, length);
			if (logs++ < 8)
				SYSLOG("ngreen", "V301: batch %llx bitstream %llx+%x len %x attr %08x flushed", static_cast<unsigned long long>(bb),
				       static_cast<unsigned long long>(base), start, length, attr);
			// V302: the first decode batch in full, to diff against media-driver's.
			for (uint32_t k = 0; logs == 1 && k < room; k += 8) {
				SYSLOG("ngreen", "V302: +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", k * 4, b[k], k + 1 < room ? b[k + 1] : 0,
				       k + 2 < room ? b[k + 2] : 0, k + 3 < room ? b[k + 3] : 0, k + 4 < room ? b[k + 4] : 0,
				       k + 5 < room ? b[k + 5] : 0, k + 6 < room ? b[k + 6] : 0, k + 7 < room ? b[k + 7] : 0);
				bool end = false;
				for (uint32_t e = k; e < k + 8 && e < room; e++) end |= b[e] == 0x05000000;
				if (end) break;
			}
			// First slice: each page's PTE and its first dwords as the CPU sees them.
			for (uint32_t o = 0; logs == 1 && o < length && o < 0x10000; o += 0x1000) {
				uint32_t d[4] {};
				vfPpgttRead(root, base + start + o, d, 4);
				SYSLOG("ngreen", "V301: +%05x pte %llx: %08x %08x %08x %08x", start + o,
				       static_cast<unsigned long long>(vfPpgttLeaf(root, base + start + o)), d[0], d[1], d[2], d[3]);
			}
		}
	}
}

// V307: AppleGVA reads MFX status (byte counts, completion markers) that MI_STORE_REGISTER_MEM and
// MI_STORE_DATA_IMM write without snooping. The driver clears those slots right before submission,
// so a dirty CPU line can hide or later overwrite the GPU's value (coded size 0). Write the lines
// back and drop them now, so the read after completion comes from memory.
static void vfFlushStatusTargets(uint64_t root, volatile uint32_t *b, uint32_t room) {
	for (uint32_t i = 0; i + 3 < room && b[i] != 0x05000000; i++) {
		const uint32_t op = b[i] >> 23;
		if (b[i] >> 29 == 0 && (op == 0x24 || op == 0x20) && (b[i] & 0xFF) <= 3) {
			const uint64_t at = b[i + 2 - (op == 0x20)] | (static_cast<uint64_t>(b[i + 3 - (op == 0x20)] & 0xFFFF) << 32);
			if (at && !(b[i] & (1U << 22))) vfFlushGpuRange(root, at, 8); // PPGTT targets only
			i += (b[i] & 0xFF) + 1;
		}
	}
}

// V374: opt-in Tahoe ICL VADriver; its Gen11 decode batches need these Gen12 fields.
static void vfIclDecodeToGen12(volatile uint32_t *b, uint32_t room) {
	if (b[0] == 0x13000082U && !b[1] && !b[2] && !b[3]) {
		b[0] = 0x0E800000U;
		b[1] = 0x03000200U;
	}
	for (uint32_t i = 0; i + 1 < room && b[i] != 0x05000000U;) {
		const uint32_t d = b[i];
		uint32_t n = 1;
		if (d >> 29 == 0) {
			const uint32_t op = (d >> 23) & 0x3F;
			if (op == 0x20 || op == 0x24) {
				b[i] = d | 0x00400000U;
				n = (op == 0x20 ? (d & 0x3FF) : (d & 0xFF)) + 2;
			} else if (op == 0x26) {
				n = (d & 0x3F) + 2;
			} else if (op == 0x1D) {
				n = 2;
			}
		} else if (d >> 29 == 3) {
			n = (d >> 16) == 0x6800 ? 1 : (d & 0xFFF) + 2;
			if (d == 0x70020042U && i + 52 < room) {
				b[i + 6] &= ~0x400U;
				if (b[i + 13] == 0x4000U) b[i + 13] = 0x8000U;
				if (b[i + 16] == 0x6000U) b[i + 16] = 0xC000U;
				b[i + 51] = 0x1AU;
			} else if (d == 0x70040008U && i + 4 < room && b[i + 4] == 0x2000U) {
				b[i + 4] = 0x4000U;
			}
		}
		i += n;
	}
	for (uint32_t i = 0; i < room; i += 16)
		__asm__ volatile("clflush (%0)" :: "r"(&b[i]) : "memory");
	__asm__ volatile("mfence" ::: "memory");
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
			// V299: the first dwords of the first video batches.
			static uint32_t heads = 0;
			if (heads++ < 6)
				SYSLOG("ngreen", "V299: video batch %llx: %08x %08x %08x %08x %08x %08x %08x %08x",
				       static_cast<unsigned long long>(bb), b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
			static uint32_t iclBatches = 0;
			if (checkKernelArgument("-ngvadump") && b[5] != 0x05000000 && iclBatches++ < 3)
				for (uint32_t k = 0; k + 7 < (PAGE_SIZE - (bb & 0xFFF)) / 4 && k < 512; k += 8) {
					SYSLOG("ngreen", "V374: batch %llx +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", static_cast<unsigned long long>(bb),
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
						// V384: the slice commands of the first HEVC frames (second-level batch).
						static uint32_t slices = 0;
						for (uint32_t s = k; s + 2 < room && b[s] != 0x05000000 && slices < 3; s++) {
							if (b[s] != 0x18C00101)
								continue;
							slices++;
							const uint64_t sb = b[s + 1] | (static_cast<uint64_t>(b[s + 2] & 0xFFFF) << 32);
							uint32_t w[48] {};
							vfFlushGpuRange(root, sb, sizeof(w));
							if (vfPpgttRead(root, sb, w, 48))
								for (uint32_t r = 0; r < 48; r += 8)
									SYSLOG("ngreen", "V384: slice batch %llx +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", static_cast<unsigned long long>(sb),
									       r * 4, w[r], w[r + 1], w[r + 2], w[r + 3], w[r + 4], w[r + 5], w[r + 6], w[r + 7]);
							break;
						}
						// V386: the first VP9 frames' bitstream head and probability
						// buffer (PIPE_BUF_ADDR dw83) as the engine reads them.
						static uint32_t vp9Dumps = 0;
						uint32_t pba = 0, ind = 0;
						bool vp9 = false;
						for (uint32_t s = k; vp9Dumps < 2 && s < room && b[s] != 0x05000000; s++) {
							if (b[s] == 0x73820077) pba = s;
							else if (b[s] == 0x7383001B) ind = s;
							else if (b[s] == 0x73B0000A) vp9 = true;
						}
						if (vp9 && pba && ind && pba + 85 < room && ind + 3 < room) {
							vp9Dumps++;
							static uint32_t w[512];
							const struct { const char *what; uint64_t va; uint32_t count; } dumps[] = {
								{"bitstream", b[ind + 1] | (static_cast<uint64_t>(b[ind + 2] & 0xFFFF) << 32), 16},
								{"probabilities", b[pba + 83] | (static_cast<uint64_t>(b[pba + 84] & 0xFFFF) << 32), 512}};
							for (const auto &d : dumps) {
								if (!vfPpgttRead(root, d.va, w, d.count)) {
									SYSLOG("ngreen", "V386: VP9 %s %llx unmapped", d.what, static_cast<unsigned long long>(d.va));
									continue;
								}
								for (uint32_t r = 0; r < d.count; r += 8)
									SYSLOG("ngreen", "V386: VP9 %s %llx +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", d.what,
									       static_cast<unsigned long long>(d.va), r * 4, w[r], w[r + 1], w[r + 2], w[r + 3], w[r + 4], w[r + 5],
									       w[r + 6], w[r + 7]);
							}
						}
						break;
					}
					if (drops++ < 8)
						SYSLOG("ngreen", "V383: dropped HEVC batch %llx (%08x at +%03x)", static_cast<unsigned long long>(bb), b[k], k * 4);
					b[0] = 0x05000000;
					__asm__ volatile("clflush (%0)\n\tmfence" :: "r"(&b[0]) : "memory");
					break;
				}
			}
			if (checkKernelArgument("-ngiclva") && b[5] != 0x05000000)
				vfIclDecodeToGen12(b, (PAGE_SIZE - (bb & 0xFFF)) / 4);
			if (b[0] == 0x13000083 && !b[1] && !b[2] && !b[3] && !b[4]) {
				vfVdencToGen12(b, (PAGE_SIZE - (bb & 0xFFF)) / 4, bb);
				// V309: the previous encode's MFC status block (byte count at +0x18), from memory.
				static uint64_t previousStatus = 0;
				static uint32_t statusLogs = 0, encodes = 0;
				if (previousStatus) {
					uint32_t st[12] {};
					vfFlushGpuRange(root, previousStatus, 0x30);
					if (vfPpgttRead(root, previousStatus, st, 12) && (!st[6] || (encodes & 127) == 0) && statusLogs++ < 24)
						SYSLOG("ngreen", "V309: encode %u status %llx: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", encodes,
						       static_cast<unsigned long long>(previousStatus), st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7],
						       st[8], st[9], st[10], st[11]);
				}
				encodes++;
				for (uint32_t i = 0; i + 3 < (PAGE_SIZE - (bb & 0xFFF)) / 4 && b[i] != 0x05000000; i++)
					if ((b[i] >> 23) == 0x24 && b[i + 1] == 0x8a0) { previousStatus = (b[i + 2] | (static_cast<uint64_t>(b[i + 3] & 0xFFFF) << 32)) - 0x18; break; }
				b[0] = 0x0E800000; // MI_FORCE_WAKEUP
				b[1] = 0x03000200; // MFX power well on, HEVC off (masks bits 9, 8), as media-driver G12
				for (uint32_t k = 0; k < 2; k++)
					__asm__ volatile("clflush (%0)" :: "r"(&b[k]) : "memory");
				__asm__ volatile("mfence" ::: "memory");
				static uint32_t logs = 0;
				if (logs++ < 4)
					SYSLOG("ngreen", "V297: force wakeup in video batch %llx", static_cast<unsigned long long>(bb));
			} else {
				vfFlushDecodeBitstream(root, b, (PAGE_SIZE - (bb & 0xFFF)) / 4, bb);
			}
			vfFlushStatusTargets(root, b, (PAGE_SIZE - (bb & 0xFFF)) / 4);
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
