//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_vf_internal.hpp"

void vfDumpResetContext(uint32_t id, mach_vm_address_t getterAddress) {
	if (!gVfContexts || id >= gVfContextCapacity || !getterAddress)
		return;
	using Getter = uint8_t *(*)(void *);
	auto getter = reinterpret_cast<Getter>(getterAddress);
	const auto &entry = gVfContexts[id];
	auto *image = entry.contextBacking ? getter(entry.contextBacking) : nullptr;
	auto *ring = entry.ringBacking ? reinterpret_cast<volatile uint32_t *>(getter(entry.ringBacking)) : nullptr;
	if (!image)
		return;
	auto *regs = reinterpret_cast<volatile uint32_t *>(image + 0x1000);
	const uint32_t head = regs[5], tail = regs[7];
	SYSLOG("ngreen", "V270: reset id=%u head=0x%x tail=0x%x", id, head, tail);
	// V293: non-render context images (layout differs from RCS): dump the
	// start of the register state once per context class.
	static uint32_t xcsImageDumps = 0;
	if (entry.engineClass == 1 && xcsImageDumps++ < 2)
		for (uint32_t i = 0; i < 0x60; i += 8)
			SYSLOG("ngreen", "V293: id=%u class=%u +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", id,
			       entry.engineClass, i * 4, regs[i], regs[i + 1], regs[i + 2], regs[i + 3], regs[i + 4],
			       regs[i + 5], regs[i + 6], regs[i + 7]);
	// Saved register pairs: BB address/state and the active head (MMIO offsets
	// relative to the engine base, so match the low 12 bits).
	for (uint32_t i = 0; i + 1 < 0x400 / 4; i++) {
		const uint32_t r = regs[i] & 0xFFF;
		if ((regs[i] >> 12) && (r == 0x140 || r == 0x168 || r == 0x110 || r == 0x11C || r == 0x1C0 || r == 0x1C4))
			SYSLOG("ngreen", "V270: reg %05x = %08x", regs[i], regs[i + 1]);
	}
	// The batch being executed: translate BB_ADDR through the context's PPGTT
	// and dump the commands around it.
	uint64_t root = 0, bb = 0;
	for (uint32_t i = 0; i + 1 < 0x100; i++) {
		if ((regs[i] & 0xFFF) == 0x270 && (regs[i] >> 12)) root |= regs[i + 1];
		else if ((regs[i] & 0xFFF) == 0x274 && (regs[i] >> 12)) root |= static_cast<uint64_t>(regs[i + 1]) << 32;
		else if ((regs[i] & 0xFFF) == 0x140 && (regs[i] >> 12)) bb |= regs[i + 1];
		else if ((regs[i] & 0xFFF) == 0x168 && (regs[i] >> 12)) bb |= static_cast<uint64_t>(regs[i + 1] & 0xFFFF) << 32;
	}
	// GuC does not save a reset context, so BB_ADDR may be stale. List the
	// ring's batch starts, take the first one at or after the saved head (the
	// submission that hung) and dump its first four pages.
	// V293: video batches: report referenced 0x4xxxxxxx buffers missing from
	// the context's PPGTT.
	static uint32_t videoScans = 0;
	for (uint32_t i = 0; entry.engineClass == 1 && videoScans < 2 && ring && i + 2 < (tail & 0x3FFFC) / 4; i++) {
		if (ring[i] != 0x18800101)
			continue;
		const uint64_t start = ring[i + 1] | (static_cast<uint64_t>(ring[i + 2] & 0xFFFF) << 32);
		uint32_t reported = 0, scanned = 0;
		static uint32_t words[1024];
		for (uint32_t pageIndex = 0; pageIndex < 8 && reported < 40; pageIndex++) {
			const uint64_t va = (start & ~0xFFFULL) + pageIndex * 0x1000ULL;
			if (!vfPpgttRead(root & 0x7FFFFFF000ULL, va, words, 1024))
				break;
			scanned++;
			for (uint32_t k = 0; k + 1 < 1024 && reported < 40; k++) {
				const uint64_t ref = words[k] | (static_cast<uint64_t>(words[k + 1]) << 32);
				if ((words[k] >> 28) != 4 || words[k + 1] > 0xFFFF || (words[k] & 0xFFF) > 0x40)
					continue;
				// Walk the buffer forward: how many pages are mapped before the first hole?
				static uint64_t seen[64];
				static uint32_t seenCount;
				bool known = false;
				for (uint32_t q = 0; q < seenCount; q++)
					known |= seen[q] == (ref & ~0xFFFULL);
				if (known)
					continue;
				if (seenCount < 64)
					seen[seenCount++] = ref & ~0xFFFULL;
				uint32_t mapped = 0;
				while (mapped < 1024 && vfPpgttLeaf(root & 0x7FFFFFF000ULL, (ref & ~0xFFFULL) + mapped * 0x1000ULL))
					mapped++;
				SYSLOG("ngreen", "V293: id=%u batch %llx +%04x buffer %llx: %u pages mapped before a hole, pte %llx", id,
				       static_cast<unsigned long long>(start), pageIndex * 0x1000 + k * 4,
				       static_cast<unsigned long long>(ref), mapped,
				       static_cast<unsigned long long>(vfPpgttLeaf(root & 0x7FFFFFF000ULL, ref & ~0xFFFULL)));
				reported++;
			}
		}
		SYSLOG("ngreen", "V293: id=%u batch %llx scanned %u pages, %u buffers", id,
		       static_cast<unsigned long long>(start), scanned, reported);
	}
	if (entry.engineClass == 1)
		videoScans++;
	static uint32_t batchDumps = 0;
	if (bb & ~3ULL)
		SYSLOG("ngreen", "V276: id=%u saved bb=%llx", id, static_cast<unsigned long long>(bb));
	bb = 0;
	uint64_t lastBefore = 0;
	for (uint32_t i = 0; ring && i + 2 < (tail & 0x3FFFC) / 4; i++)
		if (ring[i] == 0x18800101) {
			const uint64_t start = ring[i + 1] | (static_cast<uint64_t>(ring[i + 2] & 0xFFFF) << 32);
			SYSLOG("ngreen", "V276: id=%u ring +%03x starts batch %llx", id, i * 4, static_cast<unsigned long long>(start));
			if (!bb && i * 4 >= (head & 0x3FFFC))
				bb = start;
			else if (!bb)
				lastBefore = start;
		}
	if (!bb)
		bb = lastBefore;
	const uint64_t hungBatch = bb;
	if (batchDumps >= 6)
		bb = 0;
	else if (bb)
		batchDumps++;
	uint64_t surfaceBase = 0;
	for (uint32_t pageIndex = 0; bb && pageIndex < 4; pageIndex++) {
		const uint64_t va = (bb & ~0xFFFULL) + pageIndex * 0x1000ULL;
		uint64_t table = root & 0x7FFFFFF000ULL;
		for (uint32_t level = 0; table && level < 5; level++) {
			auto *desc = IOMemoryDescriptor::withPhysicalAddress(table, PAGE_SIZE, kIODirectionIn);
			auto *map = desc ? desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly) : nullptr;
			if (!map) {
				OSSafeReleaseNULL(desc);
				break;
			}
			auto *page = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
			if (level == 4) {
				vfDumpPsSurfaces(id, root, page, surfaceBase);
				// V278: is the depth/stencil surface this batch binds fully mapped?
				for (uint32_t i = 0; i + 5 < 1024; i++)
					if (page[i] == 0x78060006 || page[i] == 0x78050006) {
						const bool stencil = page[i] == 0x78060006;
						const uint64_t base = page[i + 2] | (static_cast<uint64_t>(page[i + 3] & 0xFFFF) << 32);
						const uint32_t pitch = (page[i + 1] & 0x1FFFF) + 1;
						const uint32_t height = stencil ? ((page[i + 4] >> 16) & 0x3FFF) + 1 : ((page[i + 4] >> 17) & 0x3FFF) + 1;
						if (!base)
							continue;
						const uint32_t pages = (pitch * height + 0xFFF) / 0x1000;
						uint32_t present = 0, missing = 0;
						uint64_t firstMissing = 0;
						for (uint32_t k = 0; k < pages && k < 256; k++) {
							if (vfPpgttLeaf(root, base + k * 0x1000ULL))
								present++;
							else if (!missing++)
								firstMissing = base + k * 0x1000ULL;
						}
						SYSLOG("ngreen", "V278: id=%u %s buffer %llx pitch=%u height=%u pages=%u present=%u missing=%u first-missing=%llx pte=%llx",
						       id, stencil ? "stencil" : "depth", static_cast<unsigned long long>(base), pitch, height, pages,
						       present, missing, static_cast<unsigned long long>(firstMissing),
						       static_cast<unsigned long long>(vfPpgttLeaf(root, base)));
					}
				for (uint32_t i = pageIndex ? 0 : (bb & 0xFE0) / 4; i + 8 <= 1024; i += 8)
					if (page[i] | page[i + 1] | page[i + 2] | page[i + 3] | page[i + 4] | page[i + 5] | page[i + 6] | page[i + 7])
						SYSLOG("ngreen", "V276: id=%u va=%llx: %08x %08x %08x %08x %08x %08x %08x %08x", id,
						       static_cast<unsigned long long>(va + i * 4), page[i], page[i + 1], page[i + 2], page[i + 3],
						       page[i + 4], page[i + 5], page[i + 6], page[i + 7]);
				table = 0;
			} else {
				const uint64_t entry = reinterpret_cast<volatile uint64_t *>(page)[(va >> (39 - 9 * level)) & 511];
				table = (entry & 1) ? (entry & 0x7FFFFFF000ULL) : 0;
				if ((level == 1 || level == 2) && (entry & 0x80))
					table = 0;
				if (!table)
					SYSLOG("ngreen", "V276: id=%u va=%llx L%u entry=%016llx", id, static_cast<unsigned long long>(va),
					       level, static_cast<unsigned long long>(entry));
			}
			map->release();
			OSSafeReleaseNULL(desc);
		}
	}
	// V342: the first batch of a new render context starts by chaining to
	// another buffer (MI_BATCH_BUFFER_START); dump the first pages there too.
	static uint32_t chainDumps = 0;
	uint32_t chain[3];
	if (entry.engineClass == 0 && hungBatch && chainDumps < 2 && vfPpgttRead(root & 0x7FFFFFF000ULL, hungBatch, chain, 3) &&
	    (chain[0] & 0xFF8000FF) == 0x18800001) {
		chainDumps++;
		const uint64_t target = chain[1] | (static_cast<uint64_t>(chain[2] & 0xFFFF) << 32);
		SYSLOG("ngreen", "V342: id=%u batch %llx chains to %llx (%08x)", id, static_cast<unsigned long long>(hungBatch),
		       static_cast<unsigned long long>(target), chain[0]);
		static uint32_t words[1024];
		for (uint32_t pageIndex = 0; pageIndex < 3; pageIndex++) {
			const uint64_t va = (target & ~0xFFFULL) + pageIndex * 0x1000ULL;
			if (!vfPpgttRead(root & 0x7FFFFFF000ULL, va, words, 1024))
				break;
			for (uint32_t i = pageIndex ? 0 : (target & 0xFE0) / 4; i + 8 <= 1024; i += 8)
				if (words[i] | words[i + 1] | words[i + 2] | words[i + 3] | words[i + 4] | words[i + 5] | words[i + 6] | words[i + 7])
					SYSLOG("ngreen", "V342: id=%u va=%llx: %08x %08x %08x %08x %08x %08x %08x %08x", id,
					       static_cast<unsigned long long>(va + i * 4), words[i], words[i + 1], words[i + 2], words[i + 3],
					       words[i + 4], words[i + 5], words[i + 6], words[i + 7]);
		}
	}
	// V295: a hung media walker: dump its interface descriptor and the first
	// instructions of its kernel (instruction base + kernel start pointer).
	static uint32_t kernelDumps = 0;
	uint32_t media[256];
	if (entry.engineClass == 0 && hungBatch && kernelDumps < 4 && vfPpgttRead(root, hungBatch, media, 256)) {
		uint64_t dynamicBase = 0, instructionBase = 0, surfaceBase = 0;
		for (uint32_t i = 0; i + 4 < 256; i++) {
			if ((media[i] & 0xFFFF0000) == 0x61010000 && i + 12 < 256) {
				surfaceBase = (media[i + 4] | (static_cast<uint64_t>(media[i + 5]) << 32)) & ~0xFFFULL;
				dynamicBase = (media[i + 6] | (static_cast<uint64_t>(media[i + 7]) << 32)) & ~0xFFFULL;
				instructionBase = (media[i + 10] | (static_cast<uint64_t>(media[i + 11]) << 32)) & ~0xFFFULL;
				SYSLOG("ngreen", "V295: id=%u SBA len=%u dynamic=%llx instruction=%llx", id, media[i] & 0xFF,
				       static_cast<unsigned long long>(dynamicBase), static_cast<unsigned long long>(instructionBase));
			} else if (media[i] == 0x70020002 && dynamicBase && instructionBase) {
				uint32_t idt[8], isa[64];
				if (!vfPpgttRead(root, dynamicBase + media[i + 3], idt, 8))
					break;
				SYSLOG("ngreen", "V295: id=%u IDT %08x %08x %08x %08x %08x %08x %08x %08x", id, idt[0], idt[1], idt[2],
				       idt[3], idt[4], idt[5], idt[6], idt[7]);
				// V308: the walker's binding table: each surface and how many of its pages are mapped.
				uint32_t table[8] {};
				if (surfaceBase && vfPpgttRead(root, surfaceBase + (idt[4] & 0xFFE0), table, 8))
					for (uint32_t e = 0; e < 8; e++) {
						uint32_t ss[16] {};
						if (!table[e] || !vfPpgttRead(root, surfaceBase + (table[e] & ~0x3FU), ss, 16))
							continue;
						const uint64_t surface = ss[8] | (static_cast<uint64_t>(ss[9] & 0xFFFF) << 32);
						const uint32_t height = ((ss[2] >> 16) & 0x3FFF) + 1, pitch = (ss[3] & 0x3FFFF) + 1;
						const uint64_t bytes = static_cast<uint64_t>(pitch) * (((ss[0] >> 12) & 3) == 3 ? (height + 31) & ~31U : height);
						uint32_t pages = 0, holes = 0;
						uint64_t firstHole = 0;
						for (uint64_t at = surface & ~0xFFFULL; at < surface + bytes && pages < 4096; at += PAGE_SIZE, pages++)
							if (!(vfPpgttLeaf(root, at) & 0x7FFFFFF000ULL) && !holes++) firstHole = at; // absent or scratch-at-0
						SYSLOG("ngreen", "V308: id=%u bt[%u] ss %08x %08x %08x %08x surface %llx pages=%u unmapped=%u first=%llx", id, e,
						       ss[0], ss[1], ss[2], ss[3], static_cast<unsigned long long>(surface), pages, holes,
						       static_cast<unsigned long long>(firstHole));
					}
				if (!vfPpgttRead(root, instructionBase + (idt[0] & ~0x3FU), isa, 64))
					break;
				kernelDumps++;
				for (uint32_t k = 0; k < 64; k += 8)
					SYSLOG("ngreen", "V295: id=%u isa+%03x: %08x %08x %08x %08x %08x %08x %08x %08x", id, k * 4, isa[k],
					       isa[k + 1], isa[k + 2], isa[k + 3], isa[k + 4], isa[k + 5], isa[k + 6], isa[k + 7]);
				break;
			}
		}
	}
	// V296: a hung video batch: dump its first commands.
	static uint32_t videoDumps = 0;
	if (entry.engineClass == 1 && hungBatch && videoDumps < 2 && vfPpgttRead(root, hungBatch, media, 256)) {
		videoDumps++;
		static uint32_t page[1024];
		const bool whole = videoDumps == 1 && vfPpgttRead(root, hungBatch, page, 1024);
		for (uint32_t k = 0; k < (whole ? 1024U : 64U); k += 8) {
			const uint32_t *w = whole ? page : media;
			SYSLOG("ngreen", "V296: id=%u bb+%03x: %08x %08x %08x %08x %08x %08x %08x %08x", id, k * 4, w[k],
			       w[k + 1], w[k + 2], w[k + 3], w[k + 4], w[k + 5], w[k + 6], w[k + 7]);
		}
		// V298: the PAK's chained batches (MB code written by the RCS MbEnc
		// kernels): what do they hold by the time of the reset?
		static uint32_t words[1024];
		uint32_t chained = 0;
		if (vfPpgttRead(root, hungBatch, words, 1024))
			for (uint32_t k = 5; k + 2 < 1024 && chained < 3; k++) {
				if ((words[k] & 0xFF800000) != 0x18800000 || (words[k] & 0xFF) != 1)
					continue;
				const uint64_t target = words[k + 1] | (static_cast<uint64_t>(words[k + 2] & 0xFFFF) << 32);
				uint32_t t[16];
				if (!vfPpgttRead(root, target, t, 16))
					continue;
				chained++;
				SYSLOG("ngreen", "V298: id=%u bb+%03x %08x -> %llx: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
				       id, k * 4, words[k], static_cast<unsigned long long>(target), t[0], t[1], t[2], t[3], t[4], t[5],
				       t[6], t[7], t[8], t[9], t[10], t[11]);
			}
	}
	if (ring && (head & ~7U) < 0x4000) {
		const uint32_t at = (head & 0x3FFFC) / 4;
		const uint32_t from = at >= 8 ? at - 8 : 0;
		for (uint32_t i = from; i < from + 16; i += 8)
			SYSLOG("ngreen", "V270: ring +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", i * 4,
			       ring[i], ring[i + 1], ring[i + 2], ring[i + 3], ring[i + 4], ring[i + 5], ring[i + 6], ring[i + 7]);
	}
}

bool Gen11::vfCtbGucToHostAction(void *that, uint32_t *message) {
	if (!that || !message || !vfCtbConsumerReady(false))
		return false;
	if (!vfCanUseSleepingLock())
		return false;

	// Apple's consumer masks length to five bits and trusts descriptor size.
	// Its software-interrupt caller supplies only 32 dwords of stack space.
	// Validate the modern frame before copying, with a fixed allocation bound.
	auto *lock = getMember<IOLock *>(that, 0x20);
	auto *descriptor = getMember<volatile uint32_t *>(that, 0x58);
	auto *buffer = getMember<volatile uint32_t *>(that, 0x60);
	if (!lock || !vfValidG2HMapping(descriptor, buffer)) {
		vfMarkProtocolFault("G2H CTB mapping mismatch");
		return false;
	}
	// Serialize frame removal through event application. IRQ and synchronous
	// poll consumers can otherwise dequeue in order but apply MODE_DONE in
	// reverse order after releasing the native G2H lock.
	struct ConsumerTransaction {
		IOLock *lock;
		explicit ConsumerTransaction(IOLock *value) : lock(value) { IOLockLock(lock); }
		~ConsumerTransaction() { IOLockUnlock(lock); }
		ConsumerTransaction(const ConsumerTransaction &) = delete;
		ConsumerTransaction &operator=(const ConsumerTransaction &) = delete;
	} consumerTransaction(lock);
	// A queued consumer may have passed admission before its predecessor
	// quarantined the transport. Do not remove another frame in that case.
	if (!vfCtbConsumerReady(false))
		return false;
	OSSynchronizeIO();
	constexpr uint32_t ringDwords = kVfCtbG2HBufferBytes / sizeof(uint32_t);
	uint32_t head = descriptor[4];
	const uint32_t tail = descriptor[5];
	if (!NGGuCRing::validDescriptor(descriptor[3], kVfCtbG2HBufferBytes,
	                                head, tail, descriptor[6])) {
		vfMarkProtocolFault("invalid G2H CTB descriptor");
		return false;
	}
	if (head == tail) {
		return false;
	}
	if (!NGGuCRing::readFrame(buffer, ringDwords, head, tail, message, 32)) {
		vfMarkProtocolFault("invalid or oversized G2H CTB frame");
		return false;
	}
	OSSynchronizeIO();
	descriptor[4] = head;
	OSSynchronizeIO();

	// The bounded reader returns [modern CT header, HXG header, payload...].
	// Only event frames are admitted on this FAST-only VF transport.
	const uint32_t transport = message[0];
	const uint32_t hxg = message[1];
	const uint32_t length = transport & 0xFFU;
	const uint32_t type = hxg & kGucTypeMask;
	const bool response = (hxg & kGucOriginGuc) != 0 &&
	                      (type == kGucTypeRetry ||
	                       type == kGucTypeFailure || type == kGucTypeSuccess);
	// BUSY is a mailbox ownership transition, not a terminal CT response.
	// Match intel_guc_ct.c:ct_handle_hxg; never retire an Apple fence on BUSY.
	if (!(hxg & kGucOriginGuc) || (!response && type != 0x10000000U)) {
		vfMarkProtocolFault("invalid G2H HXG origin or message type");
		return false;
	}
	// This VF bridge sends FAST requests only. The sole direct native CTB
	// sender caller is hostToGuCAction, which our VF route rejects. A response
	// therefore cannot satisfy a legacy waiter; in particular a FAST failure
	// must not leave a context appearing successfully registered/scheduled.
	if (response) {
		vfMarkProtocolFault("unexpected response on FAST-only VF transport");
		return false;
	}

	// The FAST-only bridge implements exactly three asynchronous completions.
	// It cannot service relay, log, capture or other GuC events, so accepting
	// and dropping one would desynchronize firmware state. Match i915's
	// unsupported-event failure policy instead of forwarding it to Tahoe's
	// legacy dispatcher, which has no modern-v70 event implementation.
	const auto event = NGVfGuCEvent::inspect(hxg, length);
	const uint32_t action = hxg & 0xFFFFU;
	// V331: memory CAT error (a context's page fault). i915 only logs it; the
	// GuC reset notification for the faulting context follows and is handled.
	if (action == 0x6000U && length >= 2) {
		SYSLOG("ngreen", "V331: GuC memory CAT error for context id=%u", message[2]);
		return true;
	}
	if (!event.valid()) {
		SYSLOG("ngreen", "V238: unsupported or malformed GuC event action=0x%04x len=%u hxg=0x%08x payload=%08x:%08x:%08x",
		       action, length, hxg, length > 1 ? message[2] : 0, length > 2 ? message[3] : 0,
		       length > 3 ? message[4] : 0);
		vfMarkProtocolFault("unsupported or malformed GuC G2H event");
		return false;
	}
	// Match intel_guc_ct.c:ct_handle_event: return reserved receive space as
	// soon as a structurally valid completion leaves G2H, before interpreting
	// its lifecycle payload. A late/stale event must not leak credits and block
	// transport drain forever; an unsolicited event still trips the accounting
	// underflow guard and is then rejected by the state checks below.
	if (event.responseCredits)
		vfReleaseG2HCredits(event.responseCredits);
	if (gVfProtocolFault)
		return false;
	if (event.kind == NGVfGuCEvent::Kind::TlbInvalidationDone) {
		const uint32_t seqno = message[2];
		OSSynchronizeIO();
		if (seqno & 0x80000000U) {
			// -ngtlbsubmit completions; nobody waits for them.
		} else if (NGVfGuCEvent::expectedTlbCompletion(
		        gVfTlbWaitActive != 0, gVfTlbWaitSeqno,
		        gVfTlbDoneSeqno, seqno) &&
		    OSCompareAndSwap(seqno - 1U, seqno, &gVfTlbDoneSeqno)) {
			OSSynchronizeIO();
		} else {
			static uint32_t staleTlbLogs = 0;
			if (staleTlbLogs++ < 16) {
				SYSLOG("ngreen", "V237: stale VF TLB completion seq=%u waiting=%u active=%u",
				       seqno, gVfTlbWaitSeqno, gVfTlbWaitActive);
			}
			vfMarkProtocolFault("stale or duplicate GuC TLB completion");
			return false;
		}
	}
	if (event.fatal()) {
		const uint32_t payload0 = message[2];
		const uint32_t payload1 = length >= 3 ? message[3] : 0;
		const uint32_t payload2 = length >= 4 ? message[4] : 0;
		SYSLOG("ngreen", "V234: GuC failure event action=0x%04x len=%u payload=%08x:%08x:%08x",
		       action, length, payload0, payload1, payload2);
		// GuC already reset the engine and dropped only the hung context; keep
		// the other contexts running.
		if (event.kind == NGVfGuCEvent::Kind::ContextReset) {
			vfDumpResetContext(payload0, callback->vfSharedMappedBufferGetVirtualAddress);
			vfSkipResetContext(payload0, callback->vfSharedMappedBufferGetVirtualAddress);
			return true;
		}
		vfMarkProtocolFault("GuC engine failure notification");
		return false;
	}
	if (gVfContextLock && gVfContexts &&
	    (event.kind == NGVfGuCEvent::Kind::ScheduleContextModeDone ||
	     event.kind == NGVfGuCEvent::Kind::DeregisterContextDone)) {
		const uint32_t gucId = message[2];
		bool handled = false;
		VfGucContextState newState = kVfGucContextEmpty;
		if (gucId < gVfContextCapacity) {
			const IOInterruptState interruptState =
				IOSimpleLockLockDisableInterrupt(gVfContextLock);
			auto &entry = gVfContexts[gucId];
			if (event.kind == NGVfGuCEvent::Kind::ScheduleContextModeDone) {
				const auto completion = NGVfContextEvent::scheduleDone(
					entry.state, entry.enablePending, entry.disablePending,
					message[3]);
				if (completion.handled) {
					entry.state = completion.state;
					entry.enablePending = completion.enablePending;
					entry.disablePending = completion.disablePending;
					handled = true;
				}
			} else if (NGVfContextEvent::deregisterDone(entry)) {
				// Leave LRCA/backing associated until the retirement owner has
				// also finished every native detach; attach must still wait. Normal
				// final detach already set refCount to zero, while the device-wide
				// shutdown sweep deliberately preserves outstanding native owners.
				handled = true;
			}
			newState = entry.state;
			IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
		}
		if (handled && gVfContextLifecycleLogs++ < 64) {
			SYSLOG("ngreen", "V230: GuC lifecycle event action=0x%04x id=%u state=%u",
			       action, gucId, static_cast<unsigned int>(newState));
		}
		if (!handled) {
			SYSLOG("ngreen", "V237: unexpected GuC lifecycle event action=0x%04x id=%u state=%u",
			       action, gucId, static_cast<unsigned int>(newState));
			vfMarkProtocolFault("unexpected GuC context lifecycle event");
			return false;
		}
	}

	static uint32_t logCount = 0;
	if (logCount++ < 32) {
		SYSLOG("ngreen", "V224: VF G2H fence=%u len=%u hxg=0x%08x response=%d",
		       transport >> 16, length, hxg, response);
	}

	// A small header with bit 8 clear makes Apple's handler return message[1].
	// The VF software dispatcher ignores that legacy action return value.
	message[0] = length & 0x1FU;
	message[1] = hxg;
	return true;
}

void Gen11::vfSignalStampUpdate(void *that, const void *stamps) {
	VfSlowSubmitLog slow("stamp update");
	vfMergeRedirectedStamps();
	FunctionCast(vfSignalStampUpdate, callback->oVfSignalStampUpdate)(that, stamps);
}

bool Gen11::vfInterruptFilterHandler(void *that, void *eventSource) {
	(void)eventSource;
	if (!gVfUsesMemoryIrq)
		return false;
	// Identity is established before enabling interrupts. Never probe/map BARs
	// from a filter, or interpret an unready VF as a physical device.
	if (!vfCtbConsumerReady(true))
		return false;
	VfIrqCallbackGuard irqGuard;
	if (!irqGuard)
		return false;

	// A PCI VF receives a dedicated MSI whose state lives entirely in the
	// PF-provisioned memory-IRQ page.  Never enter Tahoe's stock filter here: it
	// acquires physical force-wake and masks/unmasks GFX_MSTR_IRQ around every
	// interrupt. i915's memory-IRQ VF path does neither. GFX_MSTR_IRQ is in
	// the VF-accessible MMIO list; accessibility alone does not select the
	// correct interrupt protocol for this device.
	uint64_t pending = vfConsumeMemoryInterrupts();
	uint32_t g2hHead = 0;
	if (!pending && vfG2HCtbPending(g2hHead))
		pending = 1ULL << 45;
	if (!pending)
		return false;

	if (!callback->vfServiceInterrupts) {
		vfMarkProtocolFault("missing VF interrupt dispatcher");
		return false;
	}

	using ServiceInterrupts = void (*)(void *, const uint64_t *);
	auto service = reinterpret_cast<ServiceInterrupts>(
		callback->vfServiceInterrupts);
	service(that, &pending);

	// Hardware callbacks only signal their software event sources. CTB draining
	// belongs to vfSoftwareGuCInterrupt, never to this hard-interrupt filter.
	return false;
}

void Gen11::vfRequestEnableCallback(void *that, OSObject *requestor,
	                                void (*action)(OSObject *, ...)) {
	if (!that || !requestor || !action || !callback ||
	    !callback->oVfRequestEnableCallback) {
		vfMarkProtocolFault("invalid VF interrupt enable callback registration");
		return;
	}

	// IGInterruptBridge::enable() sets +0x8a8 and services its callback list
	// exactly once. The VF must enable the bridge before GuC construction so
	// CTB-backed allocations can receive completions; initInterrupts() therefore
	// registers the GuC callback after the native one-shot list was cleared. Run
	// such late callbacks synchronously, matching the already-enabled state and
	// avoiding an orphan list entry/retain. Before enable, preserve the native
	// list ownership and ordering unchanged.
	if (getMember<uint8_t>(that, 0x8A8) != 0) {
		action(requestor);
		static uint32_t immediateCallbackLogs = 0;
		if (immediateCallbackLogs++ < 16)
			SYSLOG("ngreen", "V249: registered late VF GuC interrupt callback immediately");
		return;
	}

	using RequestEnableCallback = void (*)(
		void *, OSObject *, void (*)(OSObject *, ...));
	reinterpret_cast<RequestEnableCallback>(
		callback->oVfRequestEnableCallback)(that, requestor, action);
}

bool Gen11::vfDrainGuCToHost(void *that, IOInterruptEventSource *source,
	                         bool synchronousPoll) {
	VfSlowSubmitLog slow("G2H drain");
	if (!that || !vfCtbConsumerReady(!synchronousPoll))
		return false;
	VfIrqCallbackGuard irqGuard;
	if (!irqGuard || !gVfCtbCpuBase || gVfCtbStopped)
		return false;
	auto *ctb = getMember<void *>(that, 0xA10);
	if (!ctb || !callback->vfCtbSoftwareInterrupt) {
		vfMarkProtocolFault("missing VF software CTB dispatcher");
		return false;
	}
	using CtbInterrupt = uint32_t (*)(void *);
	const auto consume = reinterpret_cast<CtbInterrupt>(callback->vfCtbSoftwareInterrupt);
	for (uint32_t drained = 0; drained < 256; drained++) {
		uint32_t before = 0;
		if (!vfG2HCtbPending(before))
			return gVfProtocolFault == 0;
		// Retain the native consumer call boundary, but do not interpret the
		// returned modern HXG action as legacy log-flush status bits.
		(void)consume(ctb);
		// The routed parser consumes a malformed/fatal frame before setting the
		// quarantine flag. Stop immediately; never feed later frames through a
		// transport whose firmware/host state is already unknown.
		if (gVfProtocolFault)
			return false;
		uint32_t after = 0;
		if (vfG2HCtbPending(after) && after == before) {
			static uint32_t stalledLogs = 0;
			if (stalledLogs++ < 16) {
				SYSLOG("ngreen", "V236: stopped stalled VF G2H drain at head=%u",
				       after);
			}
			vfMarkProtocolFault("stalled GuC G2H descriptor");
			return false;
		}
	}
	uint32_t head = 0;
	if (vfG2HCtbPending(head)) {
		if (source && !synchronousPoll)
			source->interruptOccurred(nullptr, nullptr, 0);
		else if (!synchronousPoll)
			vfMarkProtocolFault("G2H drain needs a software event source");
	}
	return gVfProtocolFault == 0;
}

bool Gen11::pollVfGuCToHost(void *that) {
	// Every synchronous waiter has already proved it is outside the GuC
	// workloop. A bounded direct drain closes the MSI lost-edge window without
	// spinning: if more than one slice remains, the caller sleeps and polls the
	// next slice on its following bounded wait iteration.
	return vfDrainGuCToHost(that, nullptr, true);
}

void Gen11::vfSoftwareGuCInterrupt(void *that, IOInterruptEventSource *source, int count) {
	(void)count;
	(void)vfDrainGuCToHost(that, source, false);
}

void Gen11::vfEnableInterrupts(void *that) {
	(void)that;
	if (!gVfUsesMemoryIrq || gVfIdentity != VfIdentity::Virtual || gVfProtocolFault ||
	    gVfSubmissionStopped || gVfCtbStopped)
		return;
	gVfMemIrqRequested = 1;
	OSSynchronizeIO();
	auto *base = gVfCtbCpuBase;
	if (gVfMemIrqConfigured && base) {
		*reinterpret_cast<volatile uint32_t *>(base + kVfMemIrqOffset +
		                                      kVfMemIrqEnableOffset) = 0xFFFFU;
		OSSynchronizeIO();
	}
}

void Gen11::vfDisableInterrupts(void *that) {
	(void)that;
	if (!gVfUsesMemoryIrq)
		return;
	gVfMemIrqRequested = 0;
	OSSynchronizeIO();
	auto *base = gVfCtbCpuBase;
	if (base) {
		// Mirrors intel_iov_memirq_reset. This mask update is not a DMA
		// quiescence acknowledgement and does not permit freeing the page.
		*reinterpret_cast<volatile uint32_t *>(base + kVfMemIrqOffset +
		                                      kVfMemIrqEnableOffset) = 0;
		OSSynchronizeIO();
	}
}

void Gen11::vfReadAndClearInterrupts(void *that, void *interrupts) {
	(void)that;
	if (!interrupts)
		return;

	// processInterrupts() reaches this entry without the hardware filter. A VF
	// must still remain memory-only and must never fall back to physical IIRs.
	// Clear the caller-visible snapshot before admission so a closed gate cannot
	// expose stale interrupt bits.
	auto *snapshot = reinterpret_cast<uint64_t *>(interrupts);
	*snapshot = 0;
	if (!gVfUsesMemoryIrq)
		return;
	VfIrqCallbackGuard irqGuard;
	if (!irqGuard)
		return;
	*snapshot = vfConsumeMemoryInterrupts();
}

void Gen11::vfSuppressPhysicalErrorInterrupts(void *that) {
	(void)that;
	// Scheduler 4's outer enable/disable methods still register the logical
	// scheduler callback with IGInterruptBridge.  Only their tail-called error
	// helpers are replaced: those iterate command streamers and write physical
	// RING_* interrupt registers that no VF owns.
}

// Tahoe's timeout recovery assumes ownership of physical engine stop/start
// registers. The PF owns those registers for an SR-IOV VF. Keep these lower
// boundaries inert even though the actual event-timeout root now fail-stops.
void Gen11::vfSuppressTimeoutHardwareAction(void *that, uint32_t engine) {
	(void)that;
	(void)engine;
}

// Native debug capture is not observational on Gen11: it takes force-wake,
// reads physical GuC/RING state and repeatedly writes the INSTDONE selector.
void Gen11::vfSuppressPhysicalDebugCapture(void *that, uint32_t reason) {
	(void)that;
	(void)reason;
}

struct VfTimeoutContextFrame {
	uint32_t id = 0, flags = 0, state = 0;
	uint32_t expectedStamp = 0, observedStamp = 0;
	uint32_t expectedHead = 0, reportedHead = 0, ringHead = 0, ringTail = 0;
	uint32_t windowFlags = 0, windowBytes = 0;
	char windowBase64[257] {};
};
static_assert(sizeof(VfTimeoutContextFrame) == 304, "six bounded CPU frames with short ring prefix");

// Ring contents of every timed-out engine, logged before the timeout panic.
static uint32_t gVfTimeoutRing[NGVfTimeoutObservation::engineCount][128];
static uint32_t gVfTimeoutRingWords[NGVfTimeoutObservation::engineCount];

static uint32_t vfCaptureTimeoutFrames(mach_vm_address_t getter,
	VfTimeoutContextFrame (&frames)[NGVfTimeoutObservation::engineCount], uint32_t prefixEngine) {
	VfContextOperationGuard operation;
	if (!operation || gVfIdentity != VfIdentity::Virtual || !getter ||
	    !gVfAccelerator || !gVfContextLock || !gVfContexts ||
	    gVfContextShutdownStarted || gVfDeviceStopping)
		return 0;
	using Getter = void *(*)(void *);
	const IOInterruptState saved = IOSimpleLockLockDisableInterrupt(gVfContextLock);
	OSSynchronizeIO();
	const bool softwareBefore = getMember<volatile UInt32>(gVfAccelerator, 0xDC8) != 0;
	for (uint32_t engine = 0; engine < NGVfTimeoutObservation::engineCount; ++engine) {
		const auto &reference = gVfTimeoutReferences[engine];
		auto &frame = frames[engine];
		if (!reference.valid || reference.id >= gVfContextCapacity)
			continue;
		frame.id = reference.id;
		frame.flags = 1;
		const auto &entry = gVfContexts[reference.id];
		if (!reference.matches(reference.id, entry.descriptorLo, entry.descriptorHi,
		                       entry.submissionCoverage.serial) ||
		    entry.state == kVfGucContextEmpty || entry.state == kVfGucContextTombstone)
			continue;
		frame.flags |= 2;
		frame.state = static_cast<uint32_t>(entry.state);
		frame.expectedStamp = entry.submissionCoverage.coveredStamp;
		frame.expectedHead = entry.submissionCoverage.coveredHead;
		if (entry.submissionCoverage.hasMarkerCoverage()) frame.flags |= 4;
		if (!entry.contextBacking || !entry.stampBacking ||
		    getMember<uint64_t>(entry.contextBacking, kVfMappedBufferLengthOffset) < 0x1030 ||
		    !NGVfContextShutdown::validPacketBacking(static_cast<int32_t>(entry.stampIndex),
		        getMember<uint64_t>(entry.stampBacking, kVfMappedBufferLengthOffset), sizeof(uint64_t)))
			continue;
		// Same UUID-pinned ten-byte leaf and retained CPU mappings as the
		// existing completion query. No MMIO or native physical capture;
		// the diagnostic-only short CPU ring prefix is separately bounded below.
		auto *image = static_cast<volatile uint8_t *>(reinterpret_cast<Getter>(getter)(entry.contextBacking));
		auto *stamps = static_cast<volatile uint8_t *>(reinterpret_cast<Getter>(getter)(entry.stampBacking));
		if (!image || !stamps) continue;
		frame.flags |= 8;
		{
			// V305: is the CPU reading a stale cached stamp the GPU already wrote?
			auto *stamp = reinterpret_cast<volatile uint32_t *>(stamps + static_cast<size_t>(entry.stampIndex) * 64U);
			const uint32_t cached = *stamp;
			__asm__ volatile("clflush (%0)\n\tmfence" :: "r"(stamp) : "memory");
			frame.observedStamp = *stamp;
			static uint32_t staleLogs = 0;
			if (frame.observedStamp != cached && staleLogs++ < 16)
				SYSLOG("ngreen", "V305: id=%x stamp %x cached, %x after clflush", frame.id, cached, frame.observedStamp);
		}
		frame.reportedHead = *reinterpret_cast<volatile uint32_t *>(image + 0x10);
		frame.ringHead = *reinterpret_cast<volatile uint32_t *>(image + 0x1014);
		frame.ringTail = *reinterpret_cast<volatile uint32_t *>(image + 0x101C);
		gVfTimeoutRingWords[engine] = 0;
		if (entry.ringBacking) {
			auto *ring = static_cast<volatile uint32_t *>(reinterpret_cast<Getter>(getter)(entry.ringBacking));
			const uint64_t length = getMember<uint64_t>(entry.ringBacking, kVfMappedBufferLengthOffset);
			uint32_t words = frame.ringTail / 4U;
			if (words > 128) words = 128;
			if (ring && words * 4ULL <= length) {
				for (uint32_t i = 0; i < words; ++i) gVfTimeoutRing[engine][i] = ring[i];
				gVfTimeoutRingWords[engine] = words;
			}
		}
		// V379: select the proven timeout channel, not VCS0 unconditionally.
		// No MMIO or batch traversal. Saved LRCA head is NOT a live instruction pointer.
		if (engine == prefixEngine && entry.submissionCoverage.hasMarkerCoverage() && entry.ringBacking) {
			const uint32_t control = *reinterpret_cast<volatile uint32_t *>(image + 0x102C);
			const uint32_t bytes = (control & 0x001FF000U) + PAGE_SIZE;
			if ((control & 1U) && frame.ringTail == entry.submissionCoverage.coveredTail &&
			    NGVfTimeoutObservation::ringPrefixAdmitted(bytes,
			        getMember<uint64_t>(entry.ringBacking, kVfMappedBufferLengthOffset), frame.ringTail, frame.ringHead)) {
				auto *ring = static_cast<volatile uint8_t *>(reinterpret_cast<Getter>(getter)(entry.ringBacking));
				if (ring) {
					uint32_t words[48] {};
					for (uint32_t i = 0; i < frame.ringTail / 4U; ++i)
						words[i] = *reinterpret_cast<volatile uint32_t *>(ring + i * 4U);
					NGVfTimeoutObservation::encodeRingPrefix(words, frame.windowBase64);
					frame.windowBytes = bytes;
					frame.windowFlags = 1;
				}
			}
		}
		if (entry.submissionCoverage.gpuComplete(frame.observedStamp, frame.reportedHead, softwareBefore))
			frame.flags |= 16;
	}
	OSSynchronizeIO();
	const bool softwareAfter = getMember<volatile UInt32>(gVfAccelerator, 0xDC8) != 0;
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, saved);
	if (softwareAfter)
		for (auto &frame : frames) frame.flags &= ~16U;
	return 1U | (softwareBefore ? 2U : 0U) | (softwareAfter ? 4U : 0U);
}

#define NG_TIMEOUT_FRAME_FORMAT "%x/%x/%x/%x/%x/%x/%x/%x/%x"
#define NG_TIMEOUT_FRAME_ARGS(f) (f).id,(f).flags,(f).state,(f).expectedStamp,(f).observedStamp,(f).expectedHead,(f).reportedHead,(f).ringHead,(f).ringTail
#define NG_TIMEOUT_WINDOW_FORMAT "%x/%x/%.256s"
#define NG_TIMEOUT_WINDOW_ARGS(f) (f).windowFlags,(f).windowBytes,(f).windowBase64

void *Gen11::vfRejectEventTimeout(void *that, int32_t channel) {
	(void)that;
	// V267: inherited restart clears its software error and restarts the timer
	// even when eventTimeout returns null. Do not return into that retry or
	// fabricate completion while a direct GuC context may still DMA. Guest
	// fail-stop is not PF containment; the independent host watcher is required.
	vfMergeRedirectedStamps(true);
	VfTimeoutContextFrame frames[NGVfTimeoutObservation::engineCount] {};
	const uint32_t prefixEngine = NGVfTimeoutObservation::prefixEngineForChannel(channel);
	const uint32_t capture = vfCaptureTimeoutFrames(callback ? callback->vfSharedMappedBufferGetVirtualAddress : 0, frames, prefixEngine);
	static uint32_t ringDumps = 0;
	for (uint32_t engine=0;ringDumps < 4 && engine<NGVfTimeoutObservation::engineCount;++engine) {
		const auto *w=gVfTimeoutRing[engine];
		for (uint32_t i=0;i<gVfTimeoutRingWords[engine];i+=8)
			SYSLOG("ngreen", "V268: ring engine=%u id=%x +%03x: %08x %08x %08x %08x %08x %08x %08x %08x",
				engine,frames[engine].id,i*4,w[i],w[i+1],w[i+2],w[i+3],w[i+4],w[i+5],w[i+6],w[i+7]);
	}
	ringDumps++;
	// GuC already reset the hung context, so nothing of it can still DMA; keep
	// the guest alive and let the channel time out again.
	// Log and dump each newly stalled context set, not just the first 16 timeouts of a boot.
	static uint32_t logs = 0, lastIds = 0;
	const uint32_t ids = frames[0].id ^ (frames[2].id << 16) ^ (frames[3].id << 8);
	if (ids != lastIds) { lastIds = ids; if (logs > 3) logs = 0; }
	if (logs++ < 16)
		SYSLOG("ngreen", "V267: event timeout channel=%d capture=%x T0=" NG_TIMEOUT_FRAME_FORMAT
			" T2=" NG_TIMEOUT_FRAME_FORMAT " T3=" NG_TIMEOUT_FRAME_FORMAT "; continuing", channel, capture,
			NG_TIMEOUT_FRAME_ARGS(frames[0]), NG_TIMEOUT_FRAME_ARGS(frames[2]), NG_TIMEOUT_FRAME_ARGS(frames[3]));
	// Where the stalled contexts sit: ring around the saved head and the batch.
	if (logs <= 3 && callback)
		for (uint32_t engine = 0; engine < NGVfTimeoutObservation::engineCount; ++engine)
			if ((frames[engine].flags & 8) && !(frames[engine].flags & 16))
				vfDumpResetContext(frames[engine].id, callback->vfSharedMappedBufferGetVirtualAddress);
	return nullptr;
}

#undef NG_TIMEOUT_FRAME_ARGS
#undef NG_TIMEOUT_FRAME_FORMAT
#undef NG_TIMEOUT_WINDOW_ARGS
#undef NG_TIMEOUT_WINDOW_FORMAT

// A zero signature is the native no-diagnosis result consumed by
// IGAccelFIFOChannel::getHardwareDiagnosisReport.
uint32_t Gen11::vfSuppressHangAnalysis(void *that) {
	(void)that;
	return 0;
}

void Gen11::vfSuppressHangDump(void *that) {
	(void)that;
}

void Gen11::vfRejectHardwareResetReplay(void *that) {
	(void)that;
	vfMarkProtocolFault("physical engine reset/replay requested on VF");
}

bool Gen11::vfRejectPhysicalEngineReset(void *that, void *context) {
	(void)that;
	(void)context;
	vfMarkProtocolFault("physical engine reset requested on VF");
	return false;
}
