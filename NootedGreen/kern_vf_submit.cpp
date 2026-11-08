//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_vf_internal.hpp"

// A failed, unpublished object has no process pointer. Use this otherwise
// invalid pointer value only to hand its proven cleanup state to virtual free;
// never dereference it or publish this object to the native caller.
bool TglAccel::vfWorkQueueInit(void *that, void *accelerator, uint32_t id, void *process) {
	// Installed only for the UUID-pinned VF payload. withOptions owns this
	// fresh object exclusively until init returns; successful queues unchanged.
	if (!that)
		return false;
	PANIC_COND(getMember<void *>(that, 0x10) || getMember<void *>(that, 0x20) ||
		getMember<void *>(that, 0x30) || getMember<void *>(that, 0x38), "ngreen",
		"Refusing reinitialization of a nonempty VF workqueue");
	if (!accelerator || !process || id >= NGContextPool::invalidId ||
	    !vfCanUseSleepingLock() ||
	    (accelerator && (getMember<uint8_t>(accelerator, 0x1190) &
	                     kVfLegacyPageOwnershipFlag))) {
		vfAbortSchedulerBootstrap(
			"invalid owner, process, context ID or execution state for VF workqueue");
		getMember<void *>(that, 0x38) = NGWorkQueue::failedInitMarker();
		return false;
	}
	const bool result = FunctionCast(vfWorkQueueInit, callback->oVfWorkQueueInit)(
		that, accelerator, id, process);
	if (result)
		return true;
	vfAbortSchedulerBootstrap("native VF workqueue initialization failure");
	struct Operations {
		void unlock(void *lock) { IOLockUnlock(static_cast<IOLock *>(lock)); }
		void freeLock(void *lock) { IOLockFree(static_cast<IOLock *>(lock)); }
		void release(void *object) { static_cast<OSObject *>(object)->release(); }
	} operations;
	PANIC_COND(!NGWorkQueue::unwindFailedInit(getMember<void *>(that, 0x10),
		getMember<void *>(that, 0x20), getMember<void *>(that, 0x30), operations),
		"ngreen", "Unexpected failed VF workqueue state; cannot safely unwind");
	PANIC_COND(!NGWorkQueue::markFailedInit(getMember<void *>(that, 0x10),
		getMember<void *>(that, 0x20), getMember<void *>(that, 0x30),
		getMember<void *>(that, 0x38)), "ngreen",
		"Failed VF workqueue unexpectedly retains resources or process");
	// Native withOptions releases this object next. Only the marked, fully
	// unwound object is eligible for the base-free route below.
	return false;
}

void TglAccel::vfWorkQueueFree(void *that) {
	PANIC_COND(!that, "ngreen", "Null VF workqueue in free");
	if (getMember<void *>(that, 0x38) == NGWorkQueue::failedInitMarker()) {
		PANIC_COND(!callback->vfOSObjectFree || !NGWorkQueue::consumeFailedInit(
			getMember<void *>(that, 0x10), getMember<void *>(that, 0x20),
			getMember<void *>(that, 0x30), getMember<void *>(that, 0x38)),
			"ngreen", "Cannot destroy incompletely unwound VF workqueue");
		using BaseFree = void (*)(void *);
		reinterpret_cast<BaseFree>(callback->vfOSObjectFree)(that);
		return; // object is deleted, no further access
	}
	// Direct-LRCA VF submission never publishes this legacy work queue to GuC.
	// Native free releases its mapped buffer and lock but omits the accelerator
	// retain and OSObject base destruction; complete those proven obligations.
	auto *accelerator = getMember<void *>(that, 0x10);
	PANIC_COND(!accelerator || !getMember<void *>(that, 0x20) ||
		!getMember<void *>(that, 0x30) || !getMember<void *>(that, 0x38) ||
		!callback->vfOSObjectFree, "ngreen",
		"Refusing incomplete published VF workqueue teardown");
	FunctionCast(vfWorkQueueFree, callback->oVfWorkQueueFree)(that);
	void *ownedAccelerator = nullptr;
	PANIC_COND(!NGWorkQueue::consumePublishedAfterNativeFree(
		getMember<void *>(that, 0x10), getMember<void *>(that, 0x20),
		getMember<void *>(that, 0x30), getMember<void *>(that, 0x38),
		ownedAccelerator), "ngreen",
		"Native VF workqueue teardown retained resources or lost ownership");
	static_cast<OSObject *>(ownedAccelerator)->release();
	using BaseFree = void (*)(void *);
	reinterpret_cast<BaseFree>(callback->vfOSObjectFree)(that);
}

// Render-context prologue. A VF context never sees the register state the PF
// programs through MMIO, and Apple's TGL driver (written for full parts) never
// programs pixel pipe hashing: with this part's DSS mask 0xf the pipes have
// 2/2/0 dual subslices and the default hash sends pixels to the missing third
// pipe. Every new RCS ring starts with the same PIPE_CONTROL; on a context's
// first submission it is replaced by a jump to this shared GGTT batch, which
// runs that PIPE_CONTROL, the WhateverXe workaround registers and Mesa's
// subslice hash table (all context-saved state), then returns to the ring.
static const uint32_t kRcsRingHead[6] {0x7A000004, 0x00144000, 0x40000000, 0, 0, 0};
static OSObject *gVfRcsPrologue;
static uint64_t gVfRcsPrologueGpu;

static void vfBuildRcsPrologue(uint32_t *b) {
	size_t n = 0;
	for (auto v : kRcsRingHead) b[n++] = v;
	// V316: the 3DSTATE commands below need the 3D pipeline. Pipeline selection
	// is context state, and a context whose first run inherits the engine's
	// state may sit in the media/GPGPU pipeline (FrameStatistics walkers, host
	// compute); its first batch then hung with only CS and SDE busy. Flush
	// (RT, depth, DC, CS stall) and select 3D as Apple's batches do.
	const uint32_t select[] {0x7A000004, 0x00101021, 0, 0, 0, 0, 0x69041310};
	for (auto v : select) b[n++] = v;
	// TGL object-level 3D preemption corrupts the pipeline (PRM Vol14 UP3
	// p53, WEX r135/r144). The host enables per-context control (0x20E0
	// bit 14) and VF contexts start with CS_CHICKEN1 bit 0 set; GuC
	// preempts VF contexts every quantum. Clear it (masked write).
	// V310: GPGPU/media granularity likewise defaults to mid-thread, which needs a
	// system routine (STATE_SIP) the VF never has: a media walker preempted
	// mid-thread (Apple's FrameStatistics kernel) never resumes. Command level.
	b[n++] = 0x11000001;
	b[n++] = 0x2580;
	b[n++] = 0x00070004;
	// CS_DEBUG_MODE2 bit 4 (CONSTANT_BUFFER address offset disable): Mesa sets
	// it in every context. A VF context whose first run does not restore its
	// image keeps the host's value, and then reads the UMD's push constant
	// offsets as absolute addresses: its draws write nothing (or hang) for
	// the context's whole life. The register is context-saved.
	b[n++] = 0x11000001;
	b[n++] = 0x20D8;
	b[n++] = 0x00100000;
	// COMMON_SLICE_CHICKEN3 (Wa_1409142259), FF_MODE2 (Wa_1608008084),
	// ROW_CHICKEN2 (Wa_1606931601)
	const uint32_t lri[] {0x11000005, 0x7304, 0x02000200, 0x6604, 0xE0040000, 0xE4F4, 0x40004000};
	for (auto v : lri) b[n++] = v;
	// More of i915's Gen12 RCS workarounds; the host applies them only to
	// its own contexts:
	//   ROW_CHICKEN2 push constant deref hold disable (Wa_1409804808)
	//   ROW_CHICKEN4 disable TDL push (Wa_14010229206)
	//   CS_DEBUG_MODE1 FF DOP clock gate disable (Wa_1606700617)
	//   SAMPLER_MODE small PL (Wa_1406941453)
	//   HIZ_CHICKEN depth test LE/GE optimization off (Wa_1806527549)
	//   COMMON_SLICE_CHICKEN4 TDC load balancing calc off (Wa_1606376872)
	//   COMMON_SLICE_CHICKEN1 RCC RHWO optimization off (Wa_1508744258, RCC hangs;
	//     Mesa sets it in every render context)
	const uint32_t wa[] {0x1100000D, 0xE4F4, 0x01000100, 0xE48C, 0x02000200, 0x20EC, 0x00020002,
	                     0xE18C, 0x80008000, 0x7018, 0x20002000, 0x7300, 0x00400040, 0x7010, 0x40004000};
	for (auto v : wa) b[n++] = v;
	// GT_MODE as Apple's batches load it (0xE43F0000). That LRI comes from a
	// non-privileged batch and 0x7008 is not in the host's whitelist, so it is
	// dropped; a new context then keeps the host's (Mesa's) value with binding
	// table alignment BTP_18_8, and every binding table pointer is read 8x too
	// far: render targets resolve to garbage surface state.
	b[n++] = 0x11000001;
	b[n++] = 0x7008;
	b[n++] = 0xE43F0000;
	// V315: L3 allocation (0xB134/0xB138) and 0x2084 as Apple's batches load them
	// (0x88000020, 0x40000040, masked 0x0f5e0346). Those LRIs come from
	// non-privileged batches and are dropped like GT_MODE's; a context whose
	// first run inherits another L3 partition (host 0xb0000040, default
	// 0xd0000020) hung its first draw (NotificationCenter, "bad boots").
	const uint32_t l3[] {0x11000005, 0xB134, 0x88000020, 0xB138, 0x40000040, 0x2084, 0x0F5E0346};
	for (auto v : l3) b[n++] = v;
	// 3DSTATE_SUBSLICE_HASH_TABLE, slice 0 on TABLE_0, two- and three-way
	// tables from intel_compute_pixel_hash_table_3way(8, 16, 2, 2, 0).
	b[n++] = 0x791F000C;
	b[n++] = 2;
	uint32_t *twoWay = b + n, *threeWay = b + n + 4;
	for (unsigned i = 0; i < 12; i++) b[n + i] = 0;
	for (unsigned e = 0; e < 128; e++) {
		const uint32_t pipe = ((e / 16) + (e % 16)) % 2;
		twoWay[e / 32] |= pipe << (e % 32);
		threeWay[e / 16] |= pipe << (2 * (e % 16));
	}
	n += 12;
	b[n++] = 0x791E0000; // 3DSTATE_3D_MODE: SubsliceHashingTableEnable
	b[n++] = 0x00200020;
	// 3DSTATE_BINDING_TABLE_POOL_ALLOC: disabled. The UMD never programs it
	// (binding tables are relative to Surface State Base); Mesa enables it in
	// every context, and a VF context whose first run inherits that reads its
	// binding tables from a host address.
	b[n++] = 0x79190002;
	b[n++] = 0;
	b[n++] = 0;
	b[n++] = 0;
	b[n++] = 0x05000000; // MI_BATCH_BUFFER_END
}

// Pixel hashing and the LRI workarounds are not kept across a VF context
// switch, so every submission (each starts with kRcsRingHead) runs it.
static void vfInjectRcsPrologue(volatile uint32_t *ring, uint32_t ringSize, uint32_t at) {
	const uint32_t words = ringSize / 4;
	if (!gVfRcsPrologueGpu || !ring || !words || (words & (words - 1)))
		return;
	auto w = [&](uint32_t i) -> volatile uint32_t & { return ring[(at / 4 + i) & (words - 1)]; };
	for (uint32_t i = 0; i < arrsize(kRcsRingHead); i++)
		if (w(i) != kRcsRingHead[i]) {
			static uint32_t logs = 0;
			if (logs++ < 16)
				SYSLOG("ngreen", "V269: RCS ring +%x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x differs; no prologue",
				       at, w(0), w(1), w(2), w(3), w(4), w(5), w(6), w(7), w(8), w(9), w(10), w(11));
			return;
		}
	w(1) = static_cast<uint32_t>(gVfRcsPrologueGpu);
	w(2) = static_cast<uint32_t>(gVfRcsPrologueGpu >> 32);
	w(3) = w(4) = w(5) = 0; // MI_NOOP
	w(0) = 0x18800001; // MI_BATCH_BUFFER_START, GGTT
	for (uint32_t i = 0; i < arrsize(kRcsRingHead); i++)
		__asm__ volatile("clflush (%0)" :: "r"(&w(i)) : "memory");
	__asm__ volatile("mfence" ::: "memory");
}

// V275: GGTT writes in newly submitted ring commands. A GGTT address below the
// VF's range lands in PF-owned PTEs, i.e. a host-domain DMA write.
static void vfScanRingGgttWrites(uint32_t id, uint32_t type, volatile uint32_t *ring, uint32_t ringSize,
                                 uint32_t from, uint32_t to) {
	static uint32_t sampled = 0;
	const uint32_t words = ringSize / 4;
	if (!ring || !words || (words & (words - 1)))
		return;
	for (uint32_t i = from / 4 % words, end = to / 4 % words, guard = 0; i != end && guard < 512; guard++) {
		const uint32_t w = ring[i], client = w >> 29, op = (w >> 23) & 0x3F;
		uint32_t length;
		if (client == 0)
			length = op < 0x10 ? 1 : (w & 0xFF) + 2;
		else if (client == 3)
			length = (w & 0xFF) + 2;
		else
			break;
		auto at = [&](uint32_t k) { return ring[(i + k) & (words - 1)]; };
		bool write = false, ggtt = false;
		uint64_t address = 0;
		if (client == 0 && op == 0x24) { // MI_STORE_REGISTER_MEM
			write = true, ggtt = w & (1U << 22), address = at(2) | (uint64_t(at(3)) << 32);
		} else if (client == 0 && op == 0x20) { // MI_STORE_DATA_IMM
			write = true, ggtt = w & (1U << 22), address = at(1) | (uint64_t(at(2)) << 32);
		} else if (client == 0 && op == 0x26 && ((w >> 14) & 3)) { // MI_FLUSH_DW post-sync
			write = true, ggtt = at(1) & 4, address = (at(1) & ~7U) | (uint64_t(at(2)) << 32);
		} else if (w >> 16 == 0x7A00 && ((at(1) >> 14) & 3)) { // PIPE_CONTROL post-sync
			write = true, ggtt = at(1) & (1U << 24), address = at(2) | (uint64_t(at(3)) << 32);
		} else if (client == 0 && (op == 0x2F || op == 0x2E)) { // MI_ATOMIC, MI_COPY_MEM_MEM (destination)
			write = true, ggtt = w & (1U << 22), address = at(1) | (uint64_t(at(2)) << 32);
		} else if (client == 0 && op == 0x31) { // MI_BATCH_BUFFER_START
			ggtt = !(w & (1U << 8)), address = at(1) | (uint64_t(at(2)) << 32);
		}
		// V352: a GGTT write through a non-present PTE goes out as PF address 0
		// (host DMAR "DMA Write [00:02.0] fault addr 0x0").
		if (ggtt && write) {
			static uint32_t holes = 0;
			auto *pte = vfDirectPteBase(gVfGlobalPageTable);
			const uint64_t entry = pte && address < (1ULL << 32) ? pte[address >> 12] : 1;
			// A PF-provisioned VF slot the guest never mapped is present with
			// address 0 (VFID only), which ends up at the same host address.
			const bool outside = address < gVfGGTTBase || address >= gVfGGTTBase + gVfGGTTSize;
			if ((outside || !(entry & 1) || !(entry & ~0xFFFULL)) && holes++ < 64)
				SYSLOG("ngreen", "V352: id=%u type=%u ring +%03x %08x %08x writes GGTT 0x%llx through PTE 0x%llx", id,
				       type, i * 4, w, at(1), static_cast<unsigned long long>(address),
				       static_cast<unsigned long long>(entry));
		}
		if (ggtt && (address < 0x10000000ULL || sampled < 24)) {
			sampled++;
			SYSLOG("ngreen", "V275: id=%u type=%u ring +%03x %08x %s GGTT 0x%llx", id, type, i * 4, w,
			       write ? "writes" : "starts batch at", static_cast<unsigned long long>(address));
		}
		i = (i + length) & (words - 1);
	}
}

uint32_t TglAccel::vfCreateUkContext(void *that, uint64_t owner, int priority) {
	if (!that || priority < 0 || priority > 3)
		return NGContextPool::invalidId;
	if (gVfIdentity != VfIdentity::Virtual || !gVfCtbEnabled ||
	    gVfSubmissionStopped || gVfCtbStopped ||
	    gVfProtocolFault || !vfCanUseSleepingLock() ||
	    !vfLegacyProxyPoolValid(that,
		callback->vfSharedMappedBufferGetVirtualAddress) ||
	    !callback->vfAllocContext || !callback->vfReleaseContext ||
	    !callback->vfSharedMappedBufferWithOptions ||
	    !callback->vfWorkQueueWithOptions ||
	    !callback->vfMappedBufferGetGPUVirtualAddress ||
	    !callback->oIGMappedBuffergetMemory ||
	    !callback->vfAccelSysMemoryGetPhysicalSegment) {
		// Apple's initWithOptions releases a failed CTB and continues toward
		// legacy MMIO context creation. Its createUkContext failure sentinel
		// is 0x400, which makes that initialization path return failure.
		vfAbortSchedulerBootstrap("legacy context creation without safe VF transport or pool lock");
		return NGContextPool::invalidId;
	}
	// initWithOptions creates these serially today, but keep the diagnostic
	// one-shot race-free rather than relying on that private implementation.
	static volatile UInt32 bootstrapContextTraced = 0;
	const bool traceBootstrap =
		OSCompareAndSwap(0, 1, &bootstrapContextTraced);

	using AllocContext = uint32_t (*)(void *, uint64_t, bool);
	using ReleaseContext = void (*)(void *, uint32_t);
	using SharedFactory = OSObject *(*)(void *, uint64_t, uint32_t, uint32_t);
	using WorkQueueFactory = OSObject *(*)(void *, uint32_t, void *);
	using GetVirtualAddress = uint8_t *(*)(void *);
	using GetGpuAddress = uint64_t (*)(void *);
	using GetMemory = void *(*)(void *);
	using GetPhysicalSegment = uint64_t (*)(void *, uint64_t, uint64_t *);
	auto *accelerator = getMember<void *>(that, 0x38);
	auto *task = accelerator ? getMember<void *>(accelerator, 0x150) : nullptr;
	if (!accelerator || !task) {
		vfAbortSchedulerBootstrap("VF proxy context has no accelerator task owner");
		return NGContextPool::invalidId;
	}
	if (getMember<uint8_t>(accelerator, 0x1190) &
	    kVfLegacyPageOwnershipFlag) {
		vfAbortSchedulerBootstrap("legacy page ownership requested for VF proxy context");
		return NGContextPool::invalidId;
	}

	auto releaseContext = reinterpret_cast<ReleaseContext>(callback->vfReleaseContext);
	auto rollback = [&](uint32_t id, OSObject *queue, OSObject *backing) {
		if (queue)
			queue->release();
		if (backing)
			backing->release();
		releaseContext(that, id);
	};
	const uint32_t id = reinterpret_cast<AllocContext>(callback->vfAllocContext)(
		that, owner, true);
	if (id == NGContextPool::invalidId) {
		vfAbortSchedulerBootstrap("VF proxy context ID allocation failure");
		return id;
	}
	if (traceBootstrap)
		SYSLOG("ngreen", "V248: allocated first VF proxy context id=%u", id);

	uint32_t poolCount = 0;
	PANIC_COND(!vfLegacyProxyPoolValid(that,
		callback->vfSharedMappedBufferGetVirtualAddress, nullptr, &poolCount) ||
		id >= poolCount, "ngreen",
		"New VF proxy context escaped its validated pool");
	auto getVirtual = reinterpret_cast<GetVirtualAddress>(
		callback->vfSharedMappedBufferGetVirtualAddress);
	auto getGpu = reinterpret_cast<GetGpuAddress>(
		callback->vfMappedBufferGetGPUVirtualAddress);

	auto *backing = reinterpret_cast<SharedFactory>(
		callback->vfSharedMappedBufferWithOptions)(task, PAGE_SIZE, 2, 0);
	if (!backing) {
		vfAbortSchedulerBootstrap("VF proxy process backing allocation failure");
		rollback(id, nullptr, nullptr);
		return NGContextPool::invalidId;
	}
	const uint64_t backingCpu = reinterpret_cast<uint64_t>(getVirtual(backing));
	const uint64_t backingGpu = getGpu(backing);
	const uint64_t backingBytes = getMember<uint64_t>(
		backing, kVfMappedBufferLengthOffset);
	if (!NGGgtt::mappedBacking(backingCpu, backingBytes, PAGE_SIZE, backingGpu,
	                          gVfGGTTBase, gVfGGTTSize, kGucGgttTop)) {
		vfAbortSchedulerBootstrap("invalid VF proxy process backing mapping");
		rollback(id, nullptr, backing);
		return NGContextPool::invalidId;
	}
	if (traceBootstrap)
		SYSLOG("ngreen", "V248: mapped first VF proxy process backing GGTT=0x%llx",
		       static_cast<unsigned long long>(backingGpu));
	if (!gVfRcsPrologue) {
		auto *prologue = reinterpret_cast<SharedFactory>(
			callback->vfSharedMappedBufferWithOptions)(task, PAGE_SIZE, 2, 0);
		auto *cpu = prologue ? reinterpret_cast<uint32_t *>(getVirtual(prologue)) : nullptr;
		const uint64_t gpu = prologue ? getGpu(prologue) : 0;
		if (cpu && NGGgtt::mappedBacking(reinterpret_cast<uint64_t>(cpu),
		        getMember<uint64_t>(prologue, kVfMappedBufferLengthOffset), PAGE_SIZE, gpu,
		        gVfGGTTBase, gVfGGTTSize, kGucGgttTop)) {
			bzero(cpu, PAGE_SIZE);
			vfBuildRcsPrologue(cpu);
			for (size_t i = 0; i < PAGE_SIZE; i += 64)
				__asm__ volatile("clflush (%0)" :: "r"(reinterpret_cast<uint8_t *>(cpu) + i) : "memory");
			__asm__ volatile("mfence" ::: "memory");
			gVfRcsPrologue = prologue;
			gVfRcsPrologueGpu = gpu;
			SYSLOG("ngreen", "V269: RCS prologue batch at GGTT 0x%llx", static_cast<unsigned long long>(gpu));
		} else {
			SYSLOG("ngreen", "V269: no RCS prologue batch");
			if (prologue) prologue->release();
		}
	}
	auto *memory = reinterpret_cast<GetMemory>(
		callback->oIGMappedBuffergetMemory)(backing);
	uint64_t segmentBytes = 0;
	// Pinned Tahoe createUkContext calls IGAccelMemory's virtual +0x158 with
	// exactly (offset, length).  Its concrete system-memory implementation is
	// IGAccelSysMemory::getPhysicalSegment(unsigned long long, unsigned long
	// long *), which in turn supplies MemoryManager+0x88 IOMapper options to the
	// owned IOMemoryDescriptor.  Calling IOMemoryDescriptor directly is a
	// different three-argument ABI and can also return the wrong (unmapped)
	// physical address. Resolve and invoke the exact UUID-admitted symbol.
	const bool systemMemory = memory &&
		static_cast<OSMetaClassBase *>(memory)->metaCast("IGAccelSysMemory");
	const uint64_t physical = systemMemory ?
		reinterpret_cast<GetPhysicalSegment>(
			callback->vfAccelSysMemoryGetPhysicalSegment)(
				memory, 0, &segmentBytes) : 0;
	if (!physical || segmentBytes < PAGE_SIZE ||
	    !NGGgtt::nativePhysicalRange(physical, PAGE_SIZE)) {
		vfAbortSchedulerBootstrap("invalid VF proxy process physical segment");
		rollback(id, nullptr, backing);
		return NGContextPool::invalidId;
	}
	if (traceBootstrap)
		SYSLOG("ngreen", "V248: validated first VF proxy physical segment bytes=0x%llx",
		       static_cast<unsigned long long>(segmentBytes));

	auto *process = reinterpret_cast<void *>(backingCpu + PAGE_SIZE / 2);
	const uint64_t processGpu = backingGpu + PAGE_SIZE / 2;
	auto *queue = reinterpret_cast<WorkQueueFactory>(
		callback->vfWorkQueueWithOptions)(accelerator, id, process);
	if (!queue) {
		vfAbortSchedulerBootstrap("VF proxy workqueue allocation failure");
		rollback(id, nullptr, backing);
		return NGContextPool::invalidId;
	}
	if (traceBootstrap)
		SYSLOG("ngreen", "V248: allocated first VF proxy work queue");
	auto *queueBacking = getMember<void *>(queue, 0x30);
	const uint64_t queueCpu = queueBacking ?
		reinterpret_cast<uint64_t>(getVirtual(queueBacking)) : 0;
	const uint64_t queueGpu = queueBacking ? getGpu(queueBacking) : 0;
	const uint64_t queueBytes = queueBacking ?
		getMember<uint64_t>(queueBacking, kVfMappedBufferLengthOffset) : 0;
	if (gVfProtocolFault || getMember<void *>(queue, 0x10) != accelerator ||
	    getMember<void *>(queue, 0x38) != process ||
	    getMember<uint64_t>(process, 0x20) != UINT64_C(0x100002000) ||
	    !NGGgtt::mappedBacking(queueCpu, queueBytes, 0x2000, queueGpu,
	                          gVfGGTTBase, gVfGGTTSize, kGucGgttTop)) {
		vfAbortSchedulerBootstrap("incomplete VF proxy workqueue mapping or ownership");
		rollback(id, queue, backing);
		return NGContextPool::invalidId;
	}

	auto *poolBacking = getMember<void *>(that, 0x68);
	auto *pool = getVirtual(poolBacking);
	auto *record = pool + static_cast<size_t>(id) * NGContextPool::stride;
	auto *metadata = getMember<uint8_t *>(that, 0x50) +
		static_cast<size_t>(id) * 0x20;
	PANIC_COND(!(record[NGContextPool::flagsOffset] & 1U), "ngreen",
		"New VF proxy context lost its allocation reservation");
	getMember<uint32_t>(record, 0x5A70) = static_cast<uint32_t>(priority);
	getMember<uint32_t>(record, 0x08) = id;
	getMember<uint32_t>(record, 0x24) = 0x100;
	getMember<uint32_t>(record, NGContextPool::flagsOffset) =
		(getMember<uint32_t>(record, NGContextPool::flagsOffset) & ~0x1EU) | 0xAU;
	getMember<uint32_t>(record, 0x18) = static_cast<uint32_t>(backingGpu);
	NGUnaligned::writeLe64(record + 0x1C, physical);
	getMember<uint32_t>(record, 0x5A7C) = static_cast<uint32_t>(processGpu);
	getMember<uint32_t>(record, 0x5A80) = static_cast<uint32_t>(queueGpu);
	getMember<uint32_t>(record, 0x5A84) = 0x2000;
	getMember<void *>(metadata, 0x00) = queue;
	getMember<void *>(metadata, 0x08) = backing;
	getMember<void *>(metadata, 0x10) = reinterpret_cast<void *>(backingCpu);
	getMember<void *>(metadata, 0x18) = process;
	NGUnaligned::writeLe32(process, id);
	NGUnaligned::writeLe64(static_cast<uint8_t *>(process) + 0x04, backingCpu);
	NGUnaligned::writeLe32(static_cast<uint8_t *>(process) + 0x28,
		static_cast<uint32_t>(priority));
	OSSynchronizeIO();
	if (traceBootstrap)
		SYSLOG("ngreen", "V248: completed first VF proxy context id=%u", id);
	return id;
}

uint32_t TglAccel::vfAllocContextId(void *that, uint64_t owner, bool clear) {
	if (!that || gVfIdentity != VfIdentity::Virtual || !gVfCtbEnabled ||
	    gVfSubmissionStopped || gVfCtbStopped || gVfProtocolFault ||
	    !callback->vfSharedMappedBufferGetVirtualAddress)
		return NGContextPool::invalidId;
	// Both native callers (allocContext and attachContextDesc) already own
	// GuC+0x40. Do not recursively acquire this non-recursive pool lock.
	auto *backing = getMember<void *>(that, 0x68);
	if (!backing || !getMember<void *>(that, 0x50)) {
		vfMarkProtocolFault("missing legacy proxy context pool");
		return NGContextPool::invalidId;
	}
	using Getter = void *(*)(void *);
	auto *pool = static_cast<uint8_t *>(reinterpret_cast<Getter>(
		callback->vfSharedMappedBufferGetVirtualAddress)(backing));
	uint32_t id = NGContextPool::invalidId;
	const auto result = NGContextPool::allocate(pool,
		getMember<uint64_t>(backing, kVfMappedBufferLengthOffset),
		getMember<uint32_t>(that, 0x80), getMember<uint32_t>(that, 0x84),
		getMember<uint32_t>(that, 0x88), clear, id);
	if (result == NGContextPool::Result::Invalid)
		vfMarkProtocolFault("invalid legacy proxy context pool bounds or occupancy");
	return id;
}

void TglAccel::vfReleaseContextId(void *that, uint32_t id) {
	// Both native callers already own GuC+0x40. Cleanup may follow CTB
	// shutdown/fault; do not require enabled transport or release DMA here.
	PANIC_COND(!that || gVfIdentity != VfIdentity::Virtual ||
		!callback->vfSharedMappedBufferGetVirtualAddress, "ngreen",
		"Cannot validate VF proxy context retirement");
	auto *backing = getMember<void *>(that, 0x68);
	auto *metadata = getMember<uint8_t *>(that, 0x50);
	PANIC_COND(!backing || !metadata, "ngreen", "Missing VF proxy context pool");
	using Getter = uint8_t *(*)(void *);
	auto *pool = reinterpret_cast<Getter>(callback->vfSharedMappedBufferGetVirtualAddress)(backing);
	PANIC_COND(!NGContextPool::release(pool,
		getMember<uint64_t>(backing, kVfMappedBufferLengthOffset),
		getMember<uint32_t>(that, 0x80), getMember<uint32_t>(that, 0x84), id),
		"ngreen", "Invalid or duplicate VF proxy context retirement");
	// setupContextPool allocates exactly count * 0x20 metadata bytes. Match
	// native retirement of the two owned-pointer slots; no object release here.
	auto *entry = metadata + static_cast<size_t>(id) * 0x20;
	getMember<void *>(entry, 0) = nullptr;
	getMember<void *>(entry, 8) = nullptr;
}

uint16_t TglAccel::vfAcquireDoorbell(void *that, void *descriptor, bool pin) {
	(void)that;
	(void)descriptor;
	(void)pin;
	vfMarkProtocolFault("legacy doorbell acquisition on direct-LRCA VF");
	return 0x100U; // native invalid-doorbell sentinel, not an allocated ID
}

void TglAccel::vfReleaseDoorbell(void *that, void *descriptor) {
	(void)that;
	(void)descriptor;
	// No legacy doorbell can have been acquired through the VF entry points.
	// Do not clear 0xfd4/0x1000 register banks or pretend hardware was released.
	vfMarkProtocolFault("legacy doorbell release on direct-LRCA VF");
}

bool TglAccel::vfAllocUkDoorbell(void *that, uint32_t contextId, bool pin) {
	(void)that;
	(void)contextId;
	(void)pin;
	vfMarkProtocolFault("legacy UK doorbell allocation on direct-LRCA VF");
	return false;
}

uint16_t TglAccel::vfReacquireDoorbell(void *that, uint32_t contextId) {
	(void)that;
	(void)contextId;
	// Reject before the original's acquire/sleep/retry loop.
	vfMarkProtocolFault("legacy doorbell retry on direct-LRCA VF");
	return 0x100U;
}

bool TglAccel::vfRejectLegacyGucMessage(void *that, const void *message,
	                                 unsigned int flags, void *completion) {
	(void)that;
	(void)message;
	(void)flags;
	(void)completion;
	vfMarkProtocolFault("legacy IGGuC host message reached a VF");
	PANIC_COND(true, "ngreen", "Refusing PF-owned legacy GuC MMIO on a VF");
	return false;
}

bool TglAccel::vfRejectLegacyGucDma(void *that, uint64_t address,
	                             unsigned int size, unsigned int offset,
	                             unsigned int dmaType, bool wait) {
	(void)that;
	(void)address;
	(void)size;
	(void)offset;
	(void)dmaType;
	(void)wait;
	vfMarkProtocolFault("legacy IGGuC DMA reached a VF");
	PANIC_COND(true, "ngreen", "Refusing PF-owned legacy GuC DMA on a VF");
	return false;
}

void TglAccel::vfRejectLegacyDoorbell(void *that, IGHwCsType hwCsType) {
	(void)that;
	(void)hwCsType;
	vfMarkProtocolFault("legacy IGGuC doorbell reached a VF");
	PANIC_COND(true, "ngreen", "Refusing PF-owned legacy GuC doorbell on a VF");
}

bool TglAccel::vfRejectNativeCtbAction(void *that, const uint32_t *request,
	                                unsigned int requestLength, int timeout,
	                                uint32_t *response, bool fence) {
	(void)that;
	(void)request;
	(void)requestLength;
	(void)timeout;
	(void)response;
	(void)fence;
	vfMarkProtocolFault("native legacy CTB producer reached a VF");
	PANIC_COND(true, "ngreen", "Refusing PF-owned native CTB producer on a VF");
	return false;
}

void TglAccel::vfRejectLegacyExecList(void *that, unsigned int tail) {
	(void)that;
	(void)tail;
	vfMarkProtocolFault("legacy execlist submission reached a VF");
	PANIC_COND(true, "ngreen", "Refusing PF-owned execlist submission on a VF");
}

// Modern contexts never update Apple's legacy proxy work-queue idle fields.
// A successful native marker is followed by MI_REPORT_HEAD in the same ring
// transaction. Require both independent GPU writes: the requested stamp (with
// Tahoe's signed wrap comparison) and the exact post-marker reported head.
// This is an ordinary completion snapshot, NOT a submission barrier or the
// device-wide DMA-stop proof used for teardown.
static bool vfContextKnownIdle(const VfGucContext &entry,
	                            bool softwareCompletionPossible,
	                            mach_vm_address_t mappedBufferGetter) {
	if (entry.enablePending || entry.disablePending ||
	    entry.submissionCoverage.owner)
		return false;
	switch (entry.state) {
		case kVfGucContextEmpty:
		case kVfGucContextTombstone:
			return true;
		case kVfGucContextRegistered:
		case kVfGucContextEnabled:
		case kVfGucContextDisabled:
			break;
		case kVfGucContextRegistering:
		case kVfGucContextPendingEnable:
		case kVfGucContextPendingDisable:
		case kVfGucContextPendingDeregister:
		default:
			return false;
	}

	// A newly registered context has no admitted GPU work. Once any CTB submit
	// has been published, an unmarked later submit deliberately removes this
	// shortcut until another exact marker covers it.
	if (!entry.submissionCoverage.hasSubmittedWork())
		return true;
	if (!mappedBufferGetter ||
	    !entry.contextBacking || !entry.stampBacking ||
	    !NGVfContextShutdown::validPacketBacking(
	        static_cast<int32_t>(entry.stampIndex),
	        getMember<uint64_t>(entry.stampBacking, kVfMappedBufferLengthOffset),
	        sizeof(uint64_t)) ||
	    getMember<uint64_t>(entry.contextBacking,
	                        kVfMappedBufferLengthOffset) < PAGE_SIZE)
		return false;

	using GetVirtualAddress = void *(*)(void *);
	auto getVirtualAddress = reinterpret_cast<GetVirtualAddress>(
		mappedBufferGetter);
	// The two admitted Tahoe payloads hash-pin this exact getter to a 10-byte
	// leaf that only loads backing +0x38. It cannot sleep, allocate, unlock the
	// mapping or recurse while the context spin lock protects this snapshot.
	auto *contextImage = static_cast<volatile uint8_t *>(
		getVirtualAddress(entry.contextBacking));
	auto *stamps = static_cast<volatile uint8_t *>(
		getVirtualAddress(entry.stampBacking));
	if (!contextImage || !stamps)
		return false;
	OSSynchronizeIO();
	const uint32_t observedHead = *reinterpret_cast<volatile uint32_t *>(
		contextImage + 0x10);
	const uint32_t observedStamp = *reinterpret_cast<volatile uint32_t *>(
		stamps + static_cast<size_t>(entry.stampIndex) * 64U);
	OSSynchronizeIO();
	return entry.submissionCoverage.gpuComplete(
		observedStamp, observedHead, softwareCompletionPossible);
}

bool vfKnownIdleSnapshot(mach_vm_address_t mappedBufferGetter,
	                            const uint32_t *descriptor) {
	if (gVfIdentity != VfIdentity::Virtual || !gVfCtbEnabled || gVfCtbStopped ||
	    gVfSubmissionStopped || gVfProtocolFault || gVfDeviceStopping ||
	    gVfContextShutdownStarted || !gVfAccelerator || !mappedBufferGetter ||
	    !gVfContextLock ||
	    !gVfContexts)
		return false;
	// IOAccelEventMachineFast2::deviceTerminatedUnlocked CPU-writes the stamp
	// slots after incrementing accelerator +0xdc8. Such values are software
	// completion and can never satisfy the GPU-idle contract.
	OSSynchronizeIO();
	const bool softwareCompletionPossible =
		getMember<volatile UInt32>(gVfAccelerator, gVfTerminatedOffset) != 0;
	if (softwareCompletionPossible)
		return false;
	const IOInterruptState saved = IOSimpleLockLockDisableInterrupt(gVfContextLock);
	bool idle = true;
	if (descriptor) {
		const auto value = NGContextDescriptor::read(descriptor);
		const int32_t slot = vfFindContextLocked(value.low & 0xFFFFF000U);
		idle = slot >= 0 && gVfContexts[slot].descriptorLo == value.low &&
		       vfContextKnownIdle(gVfContexts[slot],
		                          softwareCompletionPossible,
		                          mappedBufferGetter);
	} else {
		for (uint32_t i = 0; i < gVfContextCapacity; ++i) {
			if (!vfContextKnownIdle(gVfContexts[i],
			                        softwareCompletionPossible,
			                        mappedBufferGetter)) {
				idle = false;
				break;
			}
		}
	}
	// Termination publishes +0xdc8 before copying software completion values
	// into stamp slots. Sample the counter again after every stamp/head read:
	// an overlap can only turn this snapshot into busy, never GPU-complete.
	OSSynchronizeIO();
	const bool terminationRaced =
		getMember<volatile UInt32>(gVfAccelerator, gVfTerminatedOffset) != 0;
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, saved);
	return idle && !terminationRaced;
}

bool TglAccel::vfSchedulerPush(void *that, void *hardwareContext,
	                        unsigned int ringTail,
	                        unsigned int auxiliary,
	                        bool carriesStamp,
	                        bool hasPendingCommands) {
	// submitToRing's saved +0x48 marker exists only at this ABI. Keep one
	// invocation-scoped record until the synchronous native push returns; the
	// routed vfSubmitWorkItem below must claim the same owner/stamp/tail before
	// it can publish CTB work. Do not hold the H2G queue lock across the native
	// call because vfSubmitWorkItem acquires that lock itself.
	VfContextOperationGuard operationGuard;
	if (!operationGuard || !that || !hardwareContext || !callback ||
	    !callback->oVfSchedulerPush ||
	    !callback->vfSharedMappedBufferGetVirtualAddress ||
	    !gVfContextLock || !gVfContexts ||
	    gVfSubmissionStopped || gVfProtocolFault) {
		vfMarkProtocolFault("invalid Scheduler4 push admission");
		PANIC_COND(true, "ngreen", "VF Scheduler4 push lacks a safe admission boundary");
		return false;
	}

	auto *descriptor = reinterpret_cast<const uint32_t *>(
		reinterpret_cast<uint8_t *>(hardwareContext) + kVfContextDescriptorOffset);
	const auto descriptorValue = NGContextDescriptor::read(descriptor);
	const auto attributes = NGContextDescriptor::inspect(descriptorValue);
	auto *task = getMember<void *>(hardwareContext, kVfContextTaskOffset);
	auto *contextBacking = reinterpret_cast<OSObject *>(
		getMember<void *>(hardwareContext, kVfContextImageBufferOffset));
	auto *ringObject = getMember<void *>(hardwareContext, kVfContextRingObjectOffset);
	auto *ringBacking = ringObject ? reinterpret_cast<OSObject *>(
		getMember<void *>(ringObject, kVfRingMappedBufferOffset)) : nullptr;
	const int32_t nativeStampIndex = ringObject ?
		getMember<int32_t>(ringObject, kVfRingStampIndexOffset) : -1;
	const uint32_t ringSequence = ringObject ?
		getMember<uint32_t>(ringObject, 0x44) : 0;
	const uint32_t ringSize = ringObject ?
		getMember<uint32_t>(ringObject, kVfRingSizeOffset) : 0;
	const uint32_t ringMask = ringObject ?
		getMember<uint32_t>(ringObject, kVfRingMaskOffset) : 0;
	const uint64_t owner = static_cast<uint64_t>(
		reinterpret_cast<uintptr_t>(IOThreadSelf()));
	using GetVirtualAddress = void *(*)(void *);
	auto *ring = ringBacking ? static_cast<volatile uint8_t *>(
		reinterpret_cast<GetVirtualAddress>(
			callback->vfSharedMappedBufferGetVirtualAddress)(ringBacking)) : nullptr;
	uint32_t reportHead = 0;
	bool reportHeadValid = false;
	if (ring && ringSize >= 16 && ringMask == ringSize - 1U &&
	    getMember<uint64_t>(ringBacking, kVfMappedBufferLengthOffset) >= ringSize &&
	    ringTail < ringSize && (ringTail & (sizeof(uint64_t) - 1U)) == 0) {
		const uint32_t lastOffset =
			(ringTail - sizeof(uint32_t)) & ringMask;
		const uint32_t previousOffset =
			(ringTail - 2U * sizeof(uint32_t)) & ringMask;
		OSSynchronizeIO();
		const uint32_t lastDword = *reinterpret_cast<volatile uint32_t *>(
			ring + lastOffset);
		const uint32_t previousDword = *reinterpret_cast<volatile uint32_t *>(
			ring + previousOffset);
		reportHeadValid = NGVfSubmissionCoverage::reportHeadForTail(
			ringTail, ringSize, lastDword, previousDword, reportHead);
	}

	int32_t slot = -1;
	uint64_t token = 0;
	if (attributes.valid && task && contextBacking && ringBacking &&
	    nativeStampIndex >= 0 && owner && reportHeadValid) {
		const IOInterruptState saved =
			IOSimpleLockLockDisableInterrupt(gVfContextLock);
		slot = vfFindContextLocked(attributes.lrcaPage);
		if (slot >= 0) {
			auto &entry = gVfContexts[slot];
			const bool identity = entry.refCount && entry.task == task &&
				entry.contextBacking == contextBacking &&
				entry.ringBacking == ringBacking &&
				entry.stampIndex == static_cast<uint32_t>(nativeStampIndex) &&
				entry.descriptorLo == descriptorValue.low &&
				entry.descriptorHi == descriptorValue.high &&
				(entry.state == kVfGucContextRegistered ||
				 entry.state == kVfGucContextDisabled ||
				 entry.state == kVfGucContextEnabled);
			if (identity)
				token = entry.submissionCoverage.begin(
					owner, ringSequence, ringTail, reportHead, carriesStamp);
			if (!token)
				entry.submissionCoverage.invalidate();
		}
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, saved);
	}
	if (!token) {
		vfMarkProtocolFault("Scheduler4 push context/coverage identity mismatch");
		PANIC_COND(true, "ngreen", "VF Scheduler4 push cannot bind submission metadata");
		return false;
	}

	const bool accepted = FunctionCast(vfSchedulerPush,
		callback->oVfSchedulerPush)(that, hardwareContext, ringTail, auxiliary,
		                            carriesStamp, hasPendingCommands);
	bool finished = false;
	bool retired = false;
	const IOInterruptState saved =
		IOSimpleLockLockDisableInterrupt(gVfContextLock);
	const int32_t finalSlot = vfFindContextLocked(attributes.lrcaPage);
	if (finalSlot == slot && finalSlot >= 0) {
		auto &entry = gVfContexts[finalSlot];
		if (entry.descriptorLo == descriptorValue.low &&
		    entry.descriptorHi == descriptorValue.high) {
			finished = entry.submissionCoverage.finish(
				token, owner, accepted);
			retired = finished && entry.state == kVfGucContextTombstone &&
				entry.refCount == 0;
		}
	}
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, saved);
	if (!finished) {
		vfMarkProtocolFault("Scheduler4 push publication/result mismatch");
		PANIC_COND(true, "ngreen", "VF submission metadata did not match CTB publication");
		return false;
	}
	if (retired)
		vfReleaseRetiredContextBacking(static_cast<uint16_t>(finalSlot));
	return accepted;
}

bool TglAccel::vfIsGuCIdle(void *that) {
	(void)that;
	return vfKnownIdleSnapshot(callback ?
		callback->vfSharedMappedBufferGetVirtualAddress : 0);
}

bool TglAccel::vfIsContextIdle(void *that, uint32_t contextId) {
	(void)that;
	(void)contextId;
	// Legacy proxy ID cannot identify a single modern LRCA. All-idle is a
	// conservative sufficient condition; never inspect unused proxy counters.
	return vfKnownIdleSnapshot(callback ?
		callback->vfSharedMappedBufferGetVirtualAddress : 0);
}

bool TglAccel::vfIsKmdContextIdle(void *that, const uint32_t *descriptor) {
	(void)that;
	return descriptor && vfKnownIdleSnapshot(callback ?
		callback->vfSharedMappedBufferGetVirtualAddress : 0, descriptor);
}

void TglAccel::vfTransferOwnership(void *that, const void *backing, int owner) {
	(void)backing;
	(void)owner;
	// Native transferOwnership is itself a no-op without flag 0x20. With it,
	// it sends per-page commands through PCI config 0xf8/0xfc, not the Intel
	// SR-IOV protocol. This ABI is void, so merely recording a fault would let
	// its many native callers continue as if ownership had changed. Fail before
	// any caller can publish or release pages under that false assumption.
	PANIC_COND(!that || gVfIdentity != VfIdentity::Virtual ||
		(getMember<uint8_t>(that, 0x1190) & kVfLegacyPageOwnershipFlag), "ngreen",
		"Unsupported legacy page-ownership transfer on a VF");
}

bool TglAccel::vfLegacyHostToGuCAction(void *that, const uint32_t *request,
                                  unsigned int requestLength, int timeout,
                                  uint32_t *response) {
	(void)that;
	(void)timeout;
	// Audited direct callers issue legacy doorbell 0x10/0x20, log 0x30,
	// or sampling 0x3005 actions. None belongs to our direct-LRCA VF path.
	// Never allow the native sender to bypass framing/shutdown checks or to
	// fall back to a mailbox when CTB initialization has failed.
	if (response)
		*response = 1;
	SYSLOG("ngreen", "V239: rejected untranslated legacy GuC action=0x%08x len=%u",
	       request && requestLength ? request[0] : 0, requestLength);
	vfMarkProtocolFault("untranslated legacy GuC request on VF");
	return false;
}

bool TglAccel::vfMmioHostToGuCAction(void *that, const uint32_t *request,
	                              unsigned int requestLength, int timeout,
	                              uint32_t *response) {
	// All rejected requests leave an explicit failure result. Apple's audited
	// registration/deregistration callers pass null here, but the routed ABI
	// must not expose an uninitialized success value to any future caller.
	if (response)
		*response = 1;
	// Readiness is not device identity. A failed VF bootstrap must never
	// fall through to Apple's physical scratch/doorbell implementation.
	if (!gVfGGTTReady) {
		if (response)
			*response = 1;
		return false;
	}
	if (!vfCaptureHardwareGuc(that)) {
		if (response)
			*response = 1;
		return false;
	}

	if (!request || requestLength == 0 || requestLength > 4) {
		SYSLOG("ngreen", "V222: rejected malformed VF GuC MMIO request len=%u",
		       requestLength);
		return false;
	}

	// Apple's reference scheduler registers legacy CTBs with action 0x4505.
	// GuC VF firmware deliberately rejects that MMIO action: a VF must publish
	// the descriptor, buffer and size through six self-config KLVs, then enable
	// CTB transport with 0x4509.  Preserve Apple's call contract while translating
	// only the registration/teardown actions. Modern VF mailbox operations
	// use vfGucSendMMIO directly; arbitrary legacy requests are not forwarded.
	if (request[0] == 0x4505U) {
		if (!NGVfLegacyCtb::registration(request, requestLength,
		                                  gVfCtbGpuBase)) {
			// requestLength is attacker-controlled protocol input. Do not inspect
			// absent arguments merely to report a malformed request.
			SYSLOG("ngreen", "V223: rejected malformed legacy CTB registration len=%u",
			       requestLength);
			return false;
		}
		bool ok = vfConfigureModernCtb(request[3] == 1U, request[1]);
		if (ok && request[3] == 0U) {
			// Linux enables GuC interrupt delivery immediately around CT enable and
			// explicitly consumes messages that arrived across that boundary. The VF
			// bridge is already enabled by startGraphicsEngine before initFirmware,
			// but an MSI can still race publication of gVfCtbEnabled. Clear its
				// interrupt source (for memory-IRQ devices) and synchronously drain the
				// receive ring once so the
			// first GGTT/TLB transaction cannot wait on a lost edge. The native
			// consumer and this path serialize on the same G2H lock.
				if (gVfUsesMemoryIrq)
					(void)vfConsumeMemoryInterrupts();
			ok = pollVfGuCToHost(that) && gVfProtocolFault == 0;
			if (ok)
				SYSLOG("ngreen", "V248: drained the VF CTB enable boundary before firmware allocations");
		}
		if (response)
			*response = NGVfLegacyCtb::responseStatus(ok);
		return ok;
	}

	// Legacy teardown deregisters H2G and G2H separately with 0x4506.  The
	// modern VF ABI owns both channels under one 0x4509 control bit, so disable
	// once on Apple's first (type 0) request and acknowledge the second locally.
	if (request[0] == 0x4506U) {
		auto *ctb = getMember<void *>(that, 0xA10);
		const bool ownedCtb = ctb && ctb == gVfCtbObject;
		const uint32_t registrationToken = ownedCtb ?
			getMember<uint32_t>(ctb, 0x38) : 0U;
		if (!ownedCtb || !NGVfLegacyCtb::deregistration(
				request, requestLength, registrationToken)) {
			SYSLOG("ngreen", "V224: rejected malformed legacy CTB deregistration len=%u",
			       requestLength);
			return false;
		}
		bool ok = true;
		if (request[2] == 0U) {
			// GuC free can be reached through partial-init unwind without the
			// accelerator stop wrapper. Establish the context/DMA boundary here as
			// an idempotent last chance, while CTB and IRQ consumers are still live.
			bool dmaQuiesced = vfQuiesceDeviceForShutdown(that);
			const bool bootstrapRollback = !dmaQuiesced && gVfProtocolFault &&
				vfRollbackFailedPostCtbBootstrap(that);
			dmaQuiesced = dmaQuiesced || bootstrapRollback;
			// Stop ordinary producers first, but keep G2H/IRQ consumers alive long
			// enough to retire already-published MODE_DONE/DEREGISTER_DONE/TLB_DONE
			// replies. The final H2G lock check seals the transport only after the
			// ring is empty and all reserved reply credits have returned.
			const bool producersStopped = bootstrapRollback ||
				(dmaQuiesced && vfStopSubmissionAndSealCtb(that));

				// Once no reply is still required, mask future engine memory IRQs on
				// devices that implement that ABI. This is not a DMA-quiescence
				// acknowledgement; it only prevents a later enable path from reopening it.
				if (producersStopped && gVfUsesMemoryIrq) {
				gVfMemIrqRequested = 0;
				OSSynchronizeIO();
				auto *base = gVfCtbCpuBase;
				if (base) {
					*reinterpret_cast<volatile uint32_t *>(
						base + kVfMemIrqOffset + kVfMemIrqEnableOffset) = 0;
					OSSynchronizeIO();
				}
			}

			// Only now is it safe to close callback admission: expected G2H
			// completions are already drained, and callbacks admitted before closure
			// are counted until they leave the shared backing.
			const bool irqDrained = bootstrapRollback ||
				(producersStopped && vfCloseIrqCallbackGateAndWait(that));

			uint32_t reply[4] = {};
			bool disabled = bootstrapRollback;
			if (!bootstrapRollback) {
				gVfCtbDisableConfirmed = false;
				uint32_t disable[4] = {kGucActionHost2GucControlCtb, 0, 0, 0};
				disabled = producersStopped &&
					vfGucSendMMIO(disable, 2, reply) &&
					NGVfMmioResponse::noData(reply[0]);
			}
			gVfCtbDisableConfirmed = disabled;
			ok = producersStopped && irqDrained && disabled;
			SYSLOG("ngreen", "V224: disabled VF CTB transport ret=%d dma=%d producers=%d irqDrained=%d reply=0x%08x",
			       disabled, dmaQuiesced, producersStopped, irqDrained, reply[0]);
			if (!disabled)
				vfMarkProtocolFault("GuC CTB transport disable failure");
		} else {
			// The legacy ABI issues one request per channel, while 0x4509
			// disables both at once. Do not acknowledge the second legacy
			// request unless producer sealing, firmware disable and callback drain
			// are all still visible.
			OSSynchronizeIO();
			ok = gVfSubmissionStopped && gVfCtbStopped &&
			     gVfCtbDisableConfirmed &&
			     NGVfIrqGate::drained(gVfIrqCallbackGate);
			if (!ok)
				vfMarkProtocolFault("GuC CTB teardown without sealed producers/disable/IRQ drain");
		}
		if (response)
			*response = NGVfLegacyCtb::responseStatus(ok);
		if (request[2] == 1U && ok) {
			// CPU interrupt callbacks are drained, but CTB disable alone does not
				// prove engine or GuC DMA is quiescent. Keep our references and mappings
				// quarantined until
			// device-side shutdown has a verified completion boundary.
			gVfMemIrqConfigured = false;
			gVfCtbDisableConfirmed = false;
			PANIC_COND(!gVfDmaQuiesced, "ngreen",
				"CTB stopped without a completed VF DMA-quiescence boundary");
				SYSLOG("ngreen", "V242: CTB stopped after context/DMA quiescence; shared backing remains quarantined");
		}
		return ok;
	}

	return vfLegacyHostToGuCAction(that, request, requestLength, timeout, response);
}

bool TglAccel::vfReadDoorbellSQIDIConfig(void *that) {
	if (!gVfGGTTReady)
		return false;

	// Apple's Gen11 scheduler owns a fixed 256-entry ID-to-context table and
	// cannot represent a partial quota without translating every legacy
	// doorbell operation.  This host provisions its sole VF with all 256 IDs,
	// so its local ID space is exactly the native 8 x 32 topology.
	if (gVfDoorbellCount != kGen12DoorbellCount) {
		SYSLOG("ngreen", "V226: unsupported partial VF doorbell quota %u",
		       gVfDoorbellCount);
		return false;
	}

	getMember<uint16_t>(that, 0x9E0) = kGen12DoorbellsPerSQIDI;
	getMember<uint8_t>(that, 0x9E2) = kGen12SQIDICount;
	getMember<uint8_t>(that, 0x9E3) = kGen12SQIDICount;

	static bool logged = false;
	if (!logged) {
		logged = true;
		SYSLOG("ngreen", "V226: synthesized VF doorbell topology %u SQIDIs x %u = %u",
		       static_cast<unsigned int>(kGen12SQIDICount),
		       static_cast<unsigned int>(kGen12DoorbellsPerSQIDI),
		       kGen12DoorbellCount);
	}
	return true;
}

void TglAccel::vfInitDoorbells(void *that) {
	// IntelAccelerator::start normally completed this before constructing the
	// scheduler.  Retrying here makes the ordering requirement explicit and
	// prevents a transiently-unready VF from falling through to DISTRDB.
	if (!that) {
		vfMarkProtocolFault("null VF doorbell allocator object");
		return;
	}
	if (!gVfGGTTReady && !vfBootstrapDirectGgtt()) {
		SYSLOG("ngreen", "V227: refusing physical doorbell discovery without VF bootstrap");
		vfMarkProtocolFault("VF doorbell initialization before bootstrap");
		return;
	}
	if (gVfDoorbellCount != kGen12DoorbellCount) {
		SYSLOG("ngreen", "V227: unsupported VF doorbell quota %u during allocator init",
		       gVfDoorbellCount);
		// initDoorbells is void. Make the unsupported topology observable to the
		// following loadGuCBinary admission check instead of continuing with a
		// zeroed/partial allocator that cannot represent native Tahoe IDs.
		vfMarkProtocolFault("unsupported partial VF doorbell allocator topology");
		return;
	}

	// OSObject allocations start zeroed, but make the complete allocator state
	// deterministic in case the scheduler object is ever reinitialized.  The
	// upper bound deliberately stops before the topology fields at +0x9e0.
	bzero(reinterpret_cast<uint8_t *>(that) + kGucDoorbellAllocatorOffset,
	      kGucDoorbellTopologyOffset - kGucDoorbellAllocatorOffset);
	getMember<uint16_t>(that, 0x9E0) = kGen12DoorbellsPerSQIDI;
	getMember<uint8_t>(that, 0x9E2) = kGen12SQIDICount;
	getMember<uint8_t>(that, 0x9E3) = kGen12SQIDICount;

	SYSLOG("ngreen", "V227: initialized VF doorbell allocator %u SQIDIs x %u = %u",
	       static_cast<unsigned int>(kGen12SQIDICount),
	       static_cast<unsigned int>(kGen12DoorbellsPerSQIDI),
	       kGen12DoorbellCount);
}

// Submission and final retirement share the native H2G queue lock. Lock order
// is H2G -> context spinlock, never the reverse. No firmware-completion wait or
// original attach/detach call is allowed while this guard owns the queue.
class VfContextQueueGuard {
public:
	explicit VfContextQueueGuard(void *guc) {
		if (!vfCanUseSleepingLock())
			return;
		auto *ctb = guc ? getMember<void *>(guc, 0xA10) : nullptr;
		if (ctb && ctb == gVfCtbObject)
			lock = getMember<IOLock *>(ctb, 0x18);
		if (lock)
			IOLockLock(lock);
	}
	~VfContextQueueGuard() { unlock(); }
	VfContextQueueGuard(const VfContextQueueGuard &) = delete;
	VfContextQueueGuard &operator=(const VfContextQueueGuard &) = delete;
	IOLock *get() const { return lock; }
	void unlock() {
		if (lock) {
			IOLockUnlock(lock);
			lock = nullptr;
		}
	}
private:
	IOLock *lock = nullptr;
};

// Called only by the owner of a newly reserved, unregistered context slot.
// i915 __lrc_init_regs/init_vf_irq_reg_state initializes this batch for its first
// restore; subsequent GPU saves recreate it. Never rewrite it on each submit.
static bool vfPrepareContextMemoryIrq(OSObject *backing, mach_vm_address_t getter) {
	if (!gVfUsesMemoryIrq || !backing || !getter || !gVfMemIrqConfigured ||
	    !gVfCtbEnabled ||
	    gVfSubmissionStopped || gVfCtbStopped || gVfProtocolFault ||
	    getMember<uint64_t>(backing, kVfMappedBufferLengthOffset) <
	        kVfContextMinimumImageBytes)
		return false;
	using GetVirtualAddress = void *(*)(void *);
	auto *image = static_cast<uint8_t *>(
		reinterpret_cast<GetVirtualAddress>(getter)(backing));
	if (!image)
		return false;
	auto *regs = reinterpret_cast<volatile uint32_t *>(image + 0x1000);
	const uint32_t page = gVfCtbGpuBase + kVfMemIrqOffset;
	if (!NGVfMemIrq::prepareContextRegisters(
	        regs, NGVfMemIrq::contextRegisterDwords, page))
		return false;
	OSSynchronizeIO();
	return true;
}

bool TglAccel::vfAttachContextDesc(void *that, const uint32_t *descriptor) {
	VfContextOperationGuard operationGuard;
	if (!operationGuard)
		return false;
	if (gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady ||
	    gVfSubmissionStopped || gVfProtocolFault || !that || !descriptor ||
	    !callback->vfSharedMappedBufferGetVirtualAddress)
		return false;

	// The VF route owns the complete direct-LRCA lifecycle. Tahoe's native attach
	// allocates a legacy proxy slot and then inserts an LRCA hash node, but its
	// void hash add silently ignores allocation failure. None of that storage is
	// consumed by the routed submit/idle/detach paths, so do not enter it.
	const auto descriptorValue = NGContextDescriptor::read(descriptor);
	const uint32_t descriptorLo = descriptorValue.low;
	const uint32_t descriptorHi = descriptorValue.high;
	const auto descriptorAttributes = NGContextDescriptor::inspect(descriptorValue);
	const uint32_t lrcaPage = descriptorAttributes.lrcaPage;
	const uint32_t engineInstance = descriptorAttributes.engineInstance;
	auto *hardwareContext = const_cast<uint8_t *>(
		reinterpret_cast<const uint8_t *>(descriptor) -
		kVfContextDescriptorOffset);
	auto *contextBacking = reinterpret_cast<OSObject *>(
		getMember<void *>(hardwareContext, kVfContextImageBufferOffset));
	auto *ringObject = getMember<void *>(hardwareContext, kVfContextRingObjectOffset);
	auto *ringBacking = ringObject ? reinterpret_cast<OSObject *>(
		getMember<void *>(ringObject, kVfRingMappedBufferOffset)) : nullptr;
	auto *task = getMember<void *>(hardwareContext, kVfContextTaskOffset);
	auto *stampBacking = task ? reinterpret_cast<OSObject *>(
		getMember<void *>(task, kVfTaskStampBufferOffset)) : nullptr;
	auto *scratchBacking = task ? reinterpret_cast<OSObject *>(
		getMember<void *>(task, kVfTaskScratchBufferOffset)) : nullptr;
	const int32_t stampIndex = ringObject ?
		getMember<int32_t>(ringObject, kVfRingStampIndexOffset) : -1;
	const uint64_t stampBytes = stampBacking ?
		getMember<uint64_t>(stampBacking, kVfMappedBufferLengthOffset) : 0;
	const uint64_t scratchBytes = scratchBacking ?
		getMember<uint64_t>(scratchBacking, kVfMappedBufferLengthOffset) : 0;
	const uint64_t contextBytes = contextBacking ?
		getMember<uint64_t>(contextBacking, kVfMappedBufferLengthOffset) : 0;
	if (!descriptorAttributes.valid || !task || !contextBacking || !ringBacking ||
	    !stampBacking || !scratchBacking ||
	    !NGVfContextShutdown::validPacketBacking(stampIndex, stampBytes, scratchBytes) ||
	    !NGGgtt::contains(gVfGGTTBase, gVfGGTTSize, lrcaPage, contextBytes) ||
	    lrcaPage >= kGucGgttTop || contextBytes > kGucGgttTop - lrcaPage ||
	    contextBytes < kVfContextMinimumImageBytes) {
		SYSLOG("ngreen", "V241: rejected direct LRCA %08x:%08x bytes=0x%llx",
		       descriptorHi, descriptorLo,
		       static_cast<unsigned long long>(contextBytes));
		vfMarkProtocolFault("invalid VF direct context descriptor before attach");
		return false;
	}

	if (!vfInitContextBridge())
		return false;

	int32_t slot = -1;
	bool invalidCoverageReuse = false;
	IOInterruptState interruptState;
	for (;;) {
		bool waitForTransition = false;
		VfGucContextState previousState = kVfGucContextEmpty;
		interruptState = IOSimpleLockLockDisableInterrupt(gVfContextLock);
		slot = vfFindContextLocked(lrcaPage);
		if (slot >= 0) {
			auto &entry = gVfContexts[slot];
			previousState = entry.state;
			if (entry.refCount && (entry.state == kVfGucContextRegistered ||
			    entry.state == kVfGucContextPendingEnable ||
			    entry.state == kVfGucContextEnabled)) {
				if (entry.refCount == 0xFFFFU || entry.task != task ||
				    entry.ringBacking != ringBacking ||
				    entry.stampBacking != stampBacking || entry.scratchBacking != scratchBacking ||
				    entry.stampIndex != static_cast<uint32_t>(stampIndex) ||
				    !NGContextDescriptor::matchesRecord(descriptorValue,
				        descriptorAttributes, contextBacking,
				        {entry.descriptorLo, entry.descriptorHi}, entry.engineClass,
				        entry.engineInstance, entry.contextBacking)) {
					IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
					vfMarkProtocolFault("context reference overflow or LRCA identity mismatch");
					return false;
				}
				entry.refCount++;
				IOSimpleLockUnlockEnableInterrupt(gVfContextLock,
				                                      interruptState);
				return true;
			}
			// Registration and teardown own the slot.  Never attach a new
			// reference to an ID that GuC may still associate with its old LRCA.
			waitForTransition = true;
		} else {
			slot = vfReserveContextLocked(lrcaPage);
			if (slot >= 0) {
				auto &entry = gVfContexts[slot];
				if (!entry.submissionCoverage.resetForReuse()) {
					invalidCoverageReuse = true;
					slot = -1;
				} else {
					entry.lrcaPage = lrcaPage;
					entry.descriptorLo = descriptorLo;
					entry.descriptorHi = descriptorHi;
					entry.refCount = 1;
					entry.engineClass = descriptorAttributes.gucClass;
					entry.engineInstance = static_cast<uint8_t>(engineInstance);
					entry.enablePending = false;
					entry.disablePending = false;
					entry.task = task;
					contextBacking->retain();
					entry.contextBacking = contextBacking;
					// V264: native context free releases ring/FIFO before descriptor
					// cleanup. Keep the DMA ring buffer alive independently until GuC
					// deregistration and the final native reference are both retired.
					ringBacking->retain();
					entry.ringBacking = ringBacking;
					// V265: packet encoders reference task stamp and scratch buffers.
					// Pin buffers, not the task, to avoid a task/context reference cycle.
					stampBacking->retain();
					entry.stampBacking = stampBacking;
					scratchBacking->retain();
					entry.scratchBacking = scratchBacking;
					entry.stampIndex = static_cast<uint32_t>(stampIndex);
					vfEndStampRedirectLocked(entry);
					entry.state = kVfGucContextRegistering;
				}
			}
		}
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);

		if (!waitForTransition)
			break;
		if (!vfWaitForContextTransition(that, static_cast<uint16_t>(slot),
		                                lrcaPage, previousState)) {
			SYSLOG("ngreen", "V230: timed out waiting to reuse LRCA 0x%08x id=%u state=%u",
			       lrcaPage, static_cast<unsigned int>(slot),
			       static_cast<unsigned int>(previousState));
			vfMarkProtocolFault("GuC context-ID reuse timeout");
			return false;
		}
	}
	if (invalidCoverageReuse) {
		vfMarkProtocolFault("active submission metadata reached context-slot reuse");
		PANIC_COND(true, "ngreen", "VF context slot reused during an active submission");
		return false;
	}
	if (slot < 0) {
		SYSLOG("ngreen", "V230: exhausted %u direct GuC context IDs",
		       gVfContextCapacity);
		return false;
	}

	const uint16_t gucId = static_cast<uint16_t>(slot);
	const uint8_t engineClass = descriptorAttributes.gucClass;
	const uint32_t gucDescriptorLo = NGContextDescriptor::gucHwlrca(descriptorValue);
	const uint32_t request[] = {
		kGucActionRegisterContext,
		kGucContextRegistrationFlagKmd,
		gucId,
		engineClass,
		1U << engineInstance,
		0, 0, // no work-queue descriptor for a single-LRC context
		0, 0, // no work queue
		0,
		gucDescriptorLo,
		0,    // Gen12 LRCA is a 32-bit GGTT descriptor
	};
	uint32_t transportFence = 0;
	// i915 runs init_vf_irq_reg_state only when HAS_MEMORY_IRQ_STATUS.  The
	// legacy Gen11 virtual-MMIO VF path must retain Tahoe's native context image;
	// writing memory-IRQ registers there corrupts an unsupported protocol.
	const bool irqContextPrepared = !gVfUsesMemoryIrq ||
		vfPrepareContextMemoryIrq(contextBacking,
		                          callback->vfSharedMappedBufferGetVirtualAddress);
	const bool registered = irqContextPrepared &&
		vfSendCtbFastAction(that, request, arrsize(request), transportFence);
	const bool policySet = registered &&
		vfSetContextPolicy(that, gucId, engineClass, transportFence);

	interruptState = IOSimpleLockLockDisableInterrupt(gVfContextLock);
	auto &entry = gVfContexts[gucId];
	if (registered && policySet && entry.state == kVfGucContextRegistering)
		entry.state = kVfGucContextRegistered;
	else if (!registered)
		entry.state = kVfGucContextTombstone;
	else
		entry.state = kVfGucContextPendingDeregister;
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);

	if (!registered || !policySet) {
		SYSLOG("ngreen", "V231: context register/policy enqueue failed id=%u LRCA=0x%08x class=%u instance=%u fence=%u",
		       gucId, descriptorLo, engineClass, engineInstance, transportFence);
		bool deregisterSent = false;
		bool tombstoneReached = false;
		if (registered) {
			const uint32_t deregister[] = {
				kGucActionDeregisterContext, gucId,
			};
			deregisterSent = vfSendCtbFastAction(
				that, deregister, arrsize(deregister), transportFence);
			tombstoneReached = deregisterSent &&
				vfWaitForContextState(that, gucId, kVfGucContextTombstone);
		}
		if (!NGVfContextShutdown::registrationCleanupComplete(
		        registered, deregisterSent, tombstoneReached)) {
			vfMarkProtocolFault("failed to retire partially registered GuC context");
			PANIC_COND(true, "ngreen", "Partial VF context registration remains DMA-owned");
		}
		interruptState = IOSimpleLockLockDisableInterrupt(gVfContextLock);
		if (gVfContexts[gucId].state == kVfGucContextTombstone)
			gVfContexts[gucId].refCount = 0;
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
		vfReleaseRetiredContextBacking(gucId);
		return false;
	}

	if (gVfContextLifecycleLogs++ < 64) {
		SYSLOG("ngreen", "V230: registered GuC context id=%u LRCA=%08x:%08x class=%u instance=%u",
		       gucId, descriptorHi, descriptorLo, engineClass, engineInstance);
	}
	// V273: how the context's PPGTT translates the ring stamp page at
	// 0x40000000 (every RCS ring starts with a PIPE_CONTROL write there).
	static uint32_t walkDumps = 0;
	if (walkDumps < 16) {
		using Getter = uint8_t *(*)(void *);
		auto *image = reinterpret_cast<Getter>(callback->vfSharedMappedBufferGetVirtualAddress)(contextBacking);
		auto *r = image ? reinterpret_cast<volatile uint32_t *>(image + 0x1000) : nullptr;
		uint64_t root = 0;
		for (uint32_t i = 0; r && i + 1 < 0x100; i++) {
			if (r[i] == 0x2270)
				root |= r[i + 1];
			else if (r[i] == 0x2274)
				root |= static_cast<uint64_t>(r[i + 1]) << 32;
		}
		if (root)
			walkDumps++;
		static const uint64_t walkAddresses[] {0x40000000ULL, 0x40004000ULL, 0x40008000ULL};
		for (const uint64_t va : walkAddresses) {
		uint64_t table = root & 0x7FFFFFF000ULL;
		for (uint32_t level = 0; table && level < 4; level++) {
			const uint32_t index = (va >> (39 - 9 * level)) & 511;
			auto *desc = IOMemoryDescriptor::withPhysicalAddress(table, PAGE_SIZE, kIODirectionIn);
			auto *map = desc ? desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly) : nullptr;
			uint64_t entry = 0;
			if (map) {
				entry = reinterpret_cast<volatile uint64_t *>(map->getVirtualAddress())[index];
				map->release();
			}
			OSSafeReleaseNULL(desc);
			SYSLOG("ngreen", "V273: id=%u class=%u root=%llx va=%llx L%u[%u] @%llx = %016llx", gucId, engineClass,
			       static_cast<unsigned long long>(root), static_cast<unsigned long long>(va), level, index,
			       static_cast<unsigned long long>(table), static_cast<unsigned long long>(entry));
			table = (entry & 1) && !(level > 0 && (entry & 0x80)) ? (entry & 0x7FFFFFF000ULL) : 0;
		}
		}
	}
	// V271: register state of the first render contexts, to compare one that
	// never runs with ones that do.
	static uint32_t rcsDumps = 0, ccsDumps = 0;
	if ((engineClass == 0 && rcsDumps < 3) || (engineClass == 4 && ccsDumps < 2)) {
		(engineClass ? ccsDumps : rcsDumps)++;
		using Getter = uint8_t *(*)(void *);
		auto *image = reinterpret_cast<Getter>(callback->vfSharedMappedBufferGetVirtualAddress)(contextBacking);
		if (image) {
			auto *r = reinterpret_cast<volatile uint32_t *>(image + 0x1000);
			for (uint32_t i = 0; i < 0x60; i += 8)
				SYSLOG("ngreen", "V271: id=%u +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", gucId, i * 4,
				       r[i], r[i + 1], r[i + 2], r[i + 3], r[i + 4], r[i + 5], r[i + 6], r[i + 7]);
		}
	}
	return true;
}

void TglAccel::vfDetachContextDesc(void *that, const uint32_t *descriptor) {
	// V263/V264: native context free already released ring/FIFO objects, but
	// the direct GuC record pins ring, stamp and scratch DMA buffers. This hook precedes release
	// of additional mapped backing, context image and task/stamps. Any unproven
	// retirement must fail-stop
	// the guest before returning into that destructor; it is not DMA recovery.
	VfContextOperationGuard operationGuard;
	const bool postShutdown = !operationGuard;
	OSObject *contextBacking = nullptr;
	void *task = nullptr;
	if (descriptor) {
		auto *hardwareContext = const_cast<uint8_t *>(
			reinterpret_cast<const uint8_t *>(descriptor) -
			kVfContextDescriptorOffset);
		task = getMember<void *>(hardwareContext, kVfContextTaskOffset);
		contextBacking = reinterpret_cast<OSObject *>(
			getMember<void *>(hardwareContext, kVfContextImageBufferOffset));
	}
	if (!that || !descriptor || !gVfGGTTReady || !gVfContextLock || !gVfContexts) {
		// A void caller will proceed with hardware-context destruction. Even if
		// bookkeeping itself is missing, retain any discoverable image before
		// fail-stop; the image retain alone does not protect the other backing.
		if (contextBacking)
			contextBacking->retain();
		vfMarkProtocolFault("VF detach without valid context bookkeeping");
		PANIC_COND(true, "ngreen", "VF detach cannot prove DMA backing retirement");
		return;
	}
	PANIC_COND(postShutdown && !vfWaitForContextShutdown(that), "ngreen",
		"VF context detach raced an incomplete device shutdown");

	const auto descriptorValue = NGContextDescriptor::read(descriptor);
	const auto descriptorAttributes = NGContextDescriptor::inspect(descriptorValue);
	const uint32_t descriptorLo = descriptorValue.low;
	const uint32_t descriptorHi = descriptorValue.high;
	const uint32_t lrcaPage = descriptorAttributes.lrcaPage;
	const uint64_t contextBytes = contextBacking ?
		getMember<uint64_t>(contextBacking, kVfMappedBufferLengthOffset) : 0;
	if (!descriptorAttributes.valid || !task || !contextBacking ||
	    contextBytes < kVfContextMinimumImageBytes ||
	    !NGGgtt::contains(gVfGGTTBase, gVfGGTTSize, lrcaPage, contextBytes) ||
	    lrcaPage >= kGucGgttTop || contextBytes > kGucGgttTop - lrcaPage) {
		// Detach is void, so its caller cannot observe failure and will continue
		// destroying the hardware context. If any backing object is still
		// discoverable, quarantine it before returning: malformed identity must
		// never turn into a DMA use-after-free. Fail-stop also protects ring/task
		// backing that would otherwise be released by the native destructor.
		if (contextBacking)
			contextBacking->retain();
		vfMarkProtocolFault("invalid VF context identity before detach");
		PANIC_COND(true, "ngreen", "Invalid VF detach cannot release DMA backing");
		return;
	}
	if (postShutdown) {
		// Firmware ownership was retired by the shutdown sweep before CTB was
		// sealed. Late Apple object destruction only releases our direct record's
		// final reference; it must never attempt another H2G request through a
		// stopped transport or enter Tahoe's unused legacy proxy/hash path.
		int32_t shutdownSlot = -1;
		bool finalReference = false;
		bool invalidShutdownRecord = false;
		const IOInterruptState shutdownState =
			IOSimpleLockLockDisableInterrupt(gVfContextLock);
		shutdownSlot = vfFindContextLocked(lrcaPage);
		if (shutdownSlot >= 0) {
			auto &entry = gVfContexts[shutdownSlot];
			invalidShutdownRecord =
				entry.state != kVfGucContextTombstone || entry.refCount == 0 ||
				entry.task != task ||
				!NGContextDescriptor::matchesRecord(descriptorValue,
				    descriptorAttributes, contextBacking,
				    {entry.descriptorLo, entry.descriptorHi}, entry.engineClass,
				    entry.engineInstance, entry.contextBacking);
			if (!invalidShutdownRecord) {
				entry.refCount--;
				finalReference = entry.refCount == 0;
			}
		}
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, shutdownState);
		PANIC_COND(shutdownSlot < 0 || invalidShutdownRecord, "ngreen",
			"Late VF detach has no safely quiesced context record");
		if (finalReference)
			vfReleaseRetiredContextBacking(
				static_cast<uint16_t>(shutdownSlot));
		return;
	}
	int32_t slot = -1;
	VfGucContextState state = kVfGucContextEmpty;
	bool issueDisable = false;
	bool issueDeregister = false;
	bool identityMismatch = false;
	VfContextQueueGuard queue(that);
	if (!queue.get()) {
		// Without the queue lock no table lookup can prove that a retained record
		// belongs to this descriptor. Quarantine the caller-visible image before
		// the void native teardown releases it.
		contextBacking->retain();
		vfMarkProtocolFault("context retirement without pinned H2G queue");
		PANIC_COND(true, "ngreen", "VF detach has no pinned retirement transport");
		return;
	}
	IOInterruptState interruptState =
		IOSimpleLockLockDisableInterrupt(gVfContextLock);
	slot = vfFindContextLocked(lrcaPage);
	if (slot >= 0) {
		auto &entry = gVfContexts[slot];
		identityMismatch = entry.task != task ||
			!NGContextDescriptor::matchesRecord(descriptorValue,
			descriptorAttributes, contextBacking,
			{entry.descriptorLo, entry.descriptorHi}, entry.engineClass,
			entry.engineInstance, entry.contextBacking);
		if (identityMismatch) {
			// Leave the known GuC record and its retained backing untouched. The
			// supplied page may belong to another object, so it cannot identify the
			// direct record that is safe to retire.
		} else if (!entry.refCount) {
			IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
			vfMarkProtocolFault("duplicate final context detach");
			PANIC_COND(true, "ngreen", "Duplicate VF detach cannot prove DMA retirement");
			return;
		} else if (entry.refCount > 1) {
			entry.refCount--;
			IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
			queue.unlock();
			return;
		}
		if (!identityMismatch) {
			// One thread owns final retirement. Do not admit a new reference or
			// a second waiter while this owner releases locks to await firmware.
			entry.refCount = 0;
			state = entry.state;
			if (state == kVfGucContextEnabled) {
				entry.state = kVfGucContextPendingDisable;
				entry.disablePending = true;
				issueDisable = true;
			} else if (state == kVfGucContextRegistered ||
			           state == kVfGucContextDisabled) {
				entry.state = kVfGucContextPendingDeregister;
				issueDeregister = true;
			}
		}
	}
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
	queue.unlock();
	if (identityMismatch) {
		// The known record keeps its own backing alive, but a mismatching caller
		// may own a second object that native destruction will release next.
		// Quarantine that supplied object as well because the void ABI cannot
		// return a teardown failure to its owner.
		contextBacking->retain();
		vfMarkProtocolFault("VF detach descriptor/backing identity mismatch");
		PANIC_COND(true, "ngreen", "Mismatched VF detach cannot release DMA backing");
		return;
	}

	if (slot < 0) {
		// No fallback bookkeeping can prove that an untracked direct GuC
		// registration disappeared. Pin this backing for the rest of the boot
		// before fail-stopping caller destruction.
		contextBacking->retain();
		SYSLOG("ngreen", "V237: quarantined untracked detach LRCA=%08x:%08x backing=%p",
		       descriptorHi, descriptorLo, contextBacking);
		vfMarkProtocolFault("VF detach has no direct GuC context record");
		PANIC_COND(true, "ngreen", "Untracked VF detach cannot release DMA backing");
		return;
	}

	const uint16_t gucId = static_cast<uint16_t>(slot);
	uint32_t transportFence = 0;

	// A concurrent first submit owns the enable transition.  Wait for its
	// MODE_DONE before issuing disable; otherwise its late enable event could be
	// mistaken for our disable completion and the LRCA could be freed too early.
	if (state == kVfGucContextPendingEnable) {
		if (vfWaitForContextState(that, gucId, kVfGucContextEnabled)) {
			interruptState =
				IOSimpleLockLockDisableInterrupt(gVfContextLock);
			if (gVfContexts[gucId].state == kVfGucContextEnabled) {
				gVfContexts[gucId].state = kVfGucContextPendingDisable;
				gVfContexts[gucId].disablePending = true;
				issueDisable = true;
			}
			IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
		}
	}

	bool disabled = state == kVfGucContextRegistered ||
	                state == kVfGucContextDisabled ||
	                state == kVfGucContextPendingDeregister ||
	                state == kVfGucContextTombstone;
	if (issueDisable) {
		const uint32_t disable[] = {
			kGucActionScheduleContextModeSet, gucId, kGucContextDisable,
		};
		const bool disableSent =
			vfSendCtbFastAction(that, disable, arrsize(disable), transportFence);
		disabled = disableSent &&
		           vfWaitForContextState(that, gucId, kVfGucContextDisabled);
		if (!disableSent) {
			interruptState =
				IOSimpleLockLockDisableInterrupt(gVfContextLock);
			auto &entry = gVfContexts[gucId];
			if (entry.state == kVfGucContextPendingDisable) {
				entry.state = kVfGucContextEnabled;
				entry.disablePending = false;
			}
			IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
		}
	} else if (state == kVfGucContextPendingDisable) {
		disabled = vfWaitForContextState(that, gucId, kVfGucContextDisabled);
	}

	bool deregistered = state == kVfGucContextTombstone;
	if (disabled) {
		interruptState = IOSimpleLockLockDisableInterrupt(gVfContextLock);
		if (gVfContexts[gucId].state == kVfGucContextDisabled) {
			gVfContexts[gucId].state = kVfGucContextPendingDeregister;
			issueDeregister = true;
		} else if (gVfContexts[gucId].state == kVfGucContextTombstone) {
			deregistered = true;
		}
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
	}
	if (issueDeregister) {
		const uint32_t deregister[] = {
			kGucActionDeregisterContext, gucId,
		};
		deregistered =
			vfSendCtbFastAction(that, deregister, arrsize(deregister), transportFence) &&
			vfWaitForContextState(that, gucId, kVfGucContextTombstone);
	} else if (disabled && !deregistered) {
		deregistered =
			vfWaitForContextState(that, gucId, kVfGucContextTombstone);
	}

	if (!disabled || !deregistered) {
		SYSLOG("ngreen", "V231: context teardown incomplete id=%u disabled=%d deregistered=%d fence=%u",
		       gucId, disabled, deregistered, transportFence);
		vfMarkProtocolFault("GuC context teardown timeout");
		PANIC_COND(true, "ngreen", "VF context DMA retirement did not complete");
	} else if (gVfContextLifecycleLogs++ < 64) {
		SYSLOG("ngreen", "V230: deregistered GuC context id=%u LRCA=0x%08x",
		       gucId, lrcaPage);
	}
	// Failure above cannot return into native ring/image/task destruction.
	// The record retains image, ring, stamp and scratch until acknowledged teardown.
	// A completed deregistration is sufficient to
	// retire the direct record because no VF legacy proxy/hash entry exists.
	if (deregistered)
		vfReleaseRetiredContextBacking(gucId);
}

bool TglAccel::vfSubmitWorkItem(void *that, unsigned int legacyContextId,
	                         const uint32_t *descriptor, IGHwCsType hwCsType,
	                         unsigned int channelId, unsigned int ringSequence,
	                         unsigned int ringTail) {
	vfSlowMark(">work");
	VfContextOperationGuard operationGuard;
	if (!operationGuard)
		return false;
	if (!that || !gVfGGTTReady || !descriptor || !gVfContextLock || !gVfContexts ||
	    !callback->vfSharedMappedBufferGetVirtualAddress)
		return false;
	if (gVfSubmissionStopped || gVfProtocolFault) {
		static uint32_t haltedSubmitLogs = 0;
		if (haltedSubmitLogs++ < 16)
			SYSLOG("ngreen", "V237: rejected VF submit after protocol fault");
		return false;
	}
	const auto descriptorValue = NGContextDescriptor::read(descriptor);
	const uint32_t descriptorLo = descriptorValue.low;
	const uint32_t descriptorHi = descriptorValue.high;
	const auto descriptorAttributes =
		NGContextDescriptor::inspect(descriptorValue);
	auto *hardwareContext = const_cast<uint8_t *>(
		reinterpret_cast<const uint8_t *>(descriptor) -
		kVfContextDescriptorOffset);
	auto *task = getMember<void *>(hardwareContext, kVfContextTaskOffset);
	if (!descriptorAttributes.valid || !task ||
	    static_cast<uint32_t>(hwCsType) != descriptorAttributes.hwCsType) {
		vfMarkProtocolFault("submit descriptor/command-streamer identity mismatch");
		return false;
	}

	const bool interruptReady = vfInterruptTransportReady() && gVfCtbCpuBase;
	if (!interruptReady) {
		// A bootstrap stamp can precede CTB registration.  It cannot be safely
		// submitted because MODE_DONE and completion interrupts would be
		// unobservable. No command was submitted, so reporting success here
		// would fabricate progress and leave a completion stamp outstanding.
		static uint32_t bootstrapLogs = 0;
		if (bootstrapLogs++ < 16) {
				SYSLOG("ngreen", "V251: suppressed pre-interrupt VF bootstrap submit LRCA=0x%08x",
			       descriptorLo);
		}
		return false;
	}

	VfContextQueueGuard queue(that);
	if (!queue.get() || !gVfCtbEnabled || gVfSubmissionStopped ||
	    gVfCtbStopped || gVfProtocolFault)
		return false;
	// Check admission before touching the context image. A final detach must
	// acquire this queue before clearing refCount, so it cannot deregister or
	// release backing between this check, the tail write and CTB publication.
	const uint32_t lrcaPage = descriptorLo & 0xFFFFF000U;
	const uint64_t submissionOwner = static_cast<uint64_t>(
		reinterpret_cast<uintptr_t>(IOThreadSelf()));
	uint64_t submissionToken = 0;
	bool coverageClaimed = false;
	IOInterruptState admissionState = IOSimpleLockLockDisableInterrupt(gVfContextLock);
	const int32_t admittedSlot = vfFindContextLocked(lrcaPage);
	const bool identityAdmitted = admittedSlot >= 0 && gVfContexts[admittedSlot].refCount &&
		gVfContexts[admittedSlot].task == task &&
		NGContextDescriptor::matchesRecord(descriptorValue,
			descriptorAttributes,
			gVfContexts[admittedSlot].contextBacking,
			{gVfContexts[admittedSlot].descriptorLo,
			 gVfContexts[admittedSlot].descriptorHi},
			gVfContexts[admittedSlot].engineClass,
			gVfContexts[admittedSlot].engineInstance,
			gVfContexts[admittedSlot].contextBacking) &&
		(gVfContexts[admittedSlot].state == kVfGucContextRegistered ||
		 gVfContexts[admittedSlot].state == kVfGucContextDisabled ||
		 gVfContexts[admittedSlot].state == kVfGucContextEnabled);
	if (identityAdmitted && submissionOwner) {
		auto &coverage = gVfContexts[admittedSlot].submissionCoverage;
		submissionToken = coverage.serial;
		coverageClaimed = coverage.claim(
			submissionToken, submissionOwner, ringSequence, ringTail);
		if (!coverageClaimed)
			coverage.invalidate();
	}
	const bool admitted = identityAdmitted && coverageClaimed;
	OSObject *admittedBacking = admitted ? gVfContexts[admittedSlot].contextBacking : nullptr;
	OSObject *admittedRingBacking = admitted ? gVfContexts[admittedSlot].ringBacking : nullptr;
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, admissionState);
	vfSlowMark("admit");
	if (!admitted) {
		if (identityAdmitted) {
			vfMarkProtocolFault("GuC submit lacked matching Scheduler4 coverage metadata");
		}
		return false;
	}

	// Tahoe's legacy WQ encoder proves that the final argument is the byte
	// ring tail: it stores (arg >> 3) in WQ_RING_TAIL[28:18].  The previous
	// bridge mislabeled the first integer after hwCsType as ringTail and never
	// published the real value.  Modern single-LRC GuC submission has no WQ
	// item; i915 therefore writes CTX_RING_TAIL in the LRCA immediately before
	// MODE_SET/SCHED_CONTEXT (guc_set_lrc_tail()).
	//
	// descriptor is the packed member at IGHardwareContext+0x89.  Its context
	// image buffer is at +0x98, and Apple itself writes the tail at image+0x101c
	// in IGHardwareContext::updateRingTail.  Repeating that write here closes
	// the ordering gap between Apple's legacy producer and our direct CTB
	// notification while avoiding GGTT remaps or physical-address reads.
	constexpr size_t kContextRegisterStateOffset = 0x1000;
	constexpr size_t kContextRingTailOffset = 0x101C;
	constexpr size_t kContextRingControlOffset = 0x102C;
	constexpr uint32_t kRingControlPagesMask = 0x001FF000U;
	constexpr uint32_t kRingControlValid = 1U;
	auto *contextImageBuffer =
		getMember<void *>(hardwareContext, kVfContextImageBufferOffset);
	if (contextImageBuffer != admittedBacking) {
		vfMarkProtocolFault("submit context backing identity mismatch");
		return false;
	}
	using GetVirtualAddress = void *(*)(void *);
	auto getVirtualAddress = reinterpret_cast<GetVirtualAddress>(
		callback->vfSharedMappedBufferGetVirtualAddress);
	auto *contextImage = contextImageBuffer ?
		reinterpret_cast<uint8_t *>(getVirtualAddress(contextImageBuffer)) : nullptr;
	if (!contextImage ||
	    getMember<uint64_t>(contextImageBuffer, kVfMappedBufferLengthOffset) <
	        kVfContextMinimumImageBytes) {
		SYSLOG("ngreen", "V233: submit has no CPU context image LRCA=0x%08x legacy=%u",
		       descriptorLo, legacyContextId);
		return false;
	}
	auto *ringTailField = reinterpret_cast<volatile uint32_t *>(
		contextImage + kContextRingTailOffset);
	const uint32_t ringControl = *reinterpret_cast<volatile uint32_t *>(
		contextImage + kContextRingControlOffset);
	const uint32_t ringSize =
		(ringControl & kRingControlPagesMask) + PAGE_SIZE;
	// Queue ownership prevents final detach while inspecting the retained ring.
	// A valid control word alone cannot authorize a tail beyond real backing.
	auto *ringObject = getMember<void *>(hardwareContext, kVfContextRingObjectOffset);
	auto *ringBacking = ringObject ? reinterpret_cast<OSObject *>(
		getMember<void *>(ringObject, kVfRingMappedBufferOffset)) : nullptr;
	if (!admittedRingBacking || ringBacking != admittedRingBacking ||
	    getMember<uint64_t>(admittedRingBacking, kVfMappedBufferLengthOffset) < ringSize) {
		vfMarkProtocolFault("submit ring backing identity or extent mismatch");
		return false;
	}
	const uint32_t nativeRingSize = getMember<uint32_t>(ringObject, kVfRingSizeOffset);
	const uint32_t nativeRingMask = getMember<uint32_t>(ringObject, kVfRingMaskOffset);
	if (nativeRingSize != ringSize || (nativeRingSize & (nativeRingSize - 1U)) != 0 ||
	    nativeRingMask != nativeRingSize - 1U) {
		vfMarkProtocolFault("submit ring control and native geometry mismatch");
		return false;
	}
	if ((ringControl & kRingControlValid) == 0 ||
	    (ringTail & (sizeof(uint64_t) - 1U)) != 0 || ringTail >= ringSize) {
		SYSLOG("ngreen", "V233: rejected ring tail=0x%x ctl=0x%08x size=0x%x LRCA=0x%08x",
		       ringTail, ringControl, ringSize, descriptorLo);
		return false;
	}
	const uint32_t previousRingTail = *ringTailField;

	int32_t slot = -1;
	bool enable = false;
	VfGucContextState previousState = kVfGucContextEmpty;
	IOInterruptState interruptState =
		IOSimpleLockLockDisableInterrupt(gVfContextLock);
	slot = vfFindContextLocked(lrcaPage);
	if (slot >= 0) {
		auto &entry = gVfContexts[slot];
		if (entry.state == kVfGucContextRegistered ||
		    entry.state == kVfGucContextDisabled) {
			previousState = entry.state;
			entry.state = kVfGucContextPendingEnable;
			entry.enablePending = true;
			enable = true;
		} else if (entry.state != kVfGucContextEnabled) {
			slot = -1;
		}
	}
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
	if (slot < 0) {
		SYSLOG("ngreen", "V230: submit has no schedulable context LRCA=0x%08x legacy=%u",
		       lrcaPage, legacyContextId);
		return false;
	}

	if (static_cast<uint32_t>(hwCsType) == 0)
		vfInjectRcsPrologue(static_cast<volatile uint32_t *>(reinterpret_cast<void *>(
			reinterpret_cast<uint8_t *(*)(void *)>(callback->vfSharedMappedBufferGetVirtualAddress)(ringBacking))),
			ringSize, previousRingTail);

	if (gVfContexts[slot].engineClass == 1 && gVfContexts[slot].contextBacking) {
		auto getter = reinterpret_cast<uint8_t *(*)(void *)>(callback->vfSharedMappedBufferGetVirtualAddress);
		auto *image = getter(gVfContexts[slot].contextBacking);
		auto *regs = image ? reinterpret_cast<volatile uint32_t *>(image + 0x1000) : nullptr;
		uint64_t root = 0;
		for (uint32_t i = 0; regs && i + 1 < 0x100; i++) {
			if ((regs[i] & 0xFFF) == 0x270 && (regs[i] >> 12)) root |= regs[i + 1];
			else if ((regs[i] & 0xFFF) == 0x274 && (regs[i] >> 12)) root |= static_cast<uint64_t>(regs[i + 1]) << 32;
		}
		vfWakeVideoBatch(root & 0x7FFFFFF000ULL, reinterpret_cast<volatile uint32_t *>(getter(ringBacking)), ringSize,
		                 previousRingTail, ringTail);
	}
	{
		auto getter = reinterpret_cast<uint8_t *(*)(void *)>(callback->vfSharedMappedBufferGetVirtualAddress);
		vfRedirectStamps(static_cast<uint32_t>(slot), reinterpret_cast<volatile uint32_t *>(getter(ringBacking)),
		                 ringSize, previousRingTail, ringTail, getter,
		                 reinterpret_cast<uint64_t (*)(void *)>(callback->vfMappedBufferGetGPUVirtualAddress));
	}

	vfSlowMark("ring");
	const uint16_t gucId = static_cast<uint16_t>(slot);
	const uint32_t enableRequest[] = {
		kGucActionScheduleContextModeSet, gucId, kGucContextEnable,
	};
	const uint32_t scheduleRequest[] = {
		kGucActionScheduleContext, gucId,
	};
	uint32_t transportFence = 0;
	bool submitted = enable ?
		vfSendCtbFastAction(that, enableRequest, arrsize(enableRequest),
		                    transportFence, queue.get(), ringTailField, ringTail) :
		vfSendCtbFastAction(that, scheduleRequest, arrsize(scheduleRequest),
		                    transportFence, queue.get(), ringTailField, ringTail);
	vfSlowMark("ctb");
	if (submitted) {
		bool coveragePublished = false;
		interruptState = IOSimpleLockLockDisableInterrupt(gVfContextLock);
		const int32_t publishedSlot = vfFindContextLocked(lrcaPage);
		if (publishedSlot == admittedSlot && publishedSlot >= 0) {
			auto &entry = gVfContexts[publishedSlot];
			coveragePublished = entry.descriptorLo == descriptorLo &&
				entry.descriptorHi == descriptorHi &&
				entry.submissionCoverage.publish(
					submissionToken, submissionOwner, ringSequence, ringTail);
			if (coveragePublished && static_cast<size_t>(hwCsType) < NGVfTimeoutObservation::engineCount)
				gVfTimeoutReferences[static_cast<size_t>(hwCsType)] = {
					static_cast<uint32_t>(publishedSlot), descriptorLo, descriptorHi,
					entry.submissionCoverage.serial, true};
		}
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
		if (!coveragePublished) {
			vfMarkProtocolFault("CTB publication lost Scheduler4 coverage metadata");
			PANIC_COND(true, "ngreen", "Published VF work has no exact completion marker state");
			return false;
		}
	}

	if (enable) {
		if (submitted) {
			// Match i915: once MODE_SET is queued, single-LRC submission is
			// considered enabled immediately because KMD already published the
			// LRCA tail.  MODE_DONE only retires the pending G2H reference.
			interruptState =
				IOSimpleLockLockDisableInterrupt(gVfContextLock);
			auto &entry = gVfContexts[gucId];
			if (entry.state == kVfGucContextPendingEnable)
				entry.state = kVfGucContextEnabled;
			IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
		} else {
			interruptState =
				IOSimpleLockLockDisableInterrupt(gVfContextLock);
			auto &entry = gVfContexts[gucId];
			if (entry.state == kVfGucContextPendingEnable) {
				entry.state = previousState;
				entry.enablePending = false;
			}
			IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
		}
	}
	static uint32_t contextImageLogs = 0;
	if (submitted && contextImageLogs++ < 16) {
		const auto *state = reinterpret_cast<volatile uint32_t *>(
			contextImage + kContextRegisterStateOffset);
		SYSLOG("ngreen", "V234: LRCA image desc=%08x:%08x ctrl=%08x head=%08x tail=%08x start=%08x ctl=%08x",
		       descriptorHi, descriptorLo, state[3], state[5], state[7],
		       state[9], state[11]);
		// V374: bounded retained CPU image only, not a page walk or a GPU
		// completion claim. Native init copies its four roots into these exact
		// register/value pairs; include register selectors to detect a layout
		// mismatch rather than silently interpreting unrelated words as PDPs.
		// The existing minimum image length covers all reads through 0x33.
		SYSLOG("ngreen", "V374: LRCA roots desc=%08x:%08x p3=%08x:%08x/%08x:%08x p2=%08x:%08x/%08x:%08x p1=%08x:%08x/%08x:%08x p0=%08x:%08x/%08x:%08x",
		       descriptorHi, descriptorLo,
		       state[0x24], state[0x25], state[0x26], state[0x27],
		       state[0x28], state[0x29], state[0x2A], state[0x2B],
		       state[0x2C], state[0x2D], state[0x2E], state[0x2F],
		       state[0x30], state[0x31], state[0x32], state[0x33]);
	}

	queue.unlock();
	static uint32_t submitLogs = 0, mediaLogs[8] {};
	const uint32_t csType = static_cast<uint32_t>(hwCsType);
	const bool media = (csType == 1 || csType == 3) && mediaLogs[csType & 7]++ < 40;
	if (submitLogs++ < 600 || !submitted || media) {
		char process[24] {};
		proc_selfname(process, sizeof(process));
		SYSLOG("ngreen", "V233: direct submit id=%u LRCA=0x%08x type=%u tail=0x%x old=0x%x channel=%u seq=%u enable=%d ret=%d fence=%u desc-hi=%08x serial=%llx proc=%s",
		       gucId, descriptorLo, static_cast<unsigned int>(hwCsType),
		       ringTail, previousRingTail, channelId, ringSequence, enable,
		       submitted, transportFence, descriptorHi, submissionToken, process);
	}
	// V274: the new-work commands of the first submissions on non-render
	// channels (a blitter context's first submission precedes host DMA write
	// faults at IOVA 0).
	static uint32_t ringDumps = 0;
	if (hwCsType != 0 && ringDumps < 6 && ringTail > previousRingTail && ringTail - previousRingTail <= 0x100) {
		ringDumps++;
		auto *cpuRing = static_cast<volatile uint32_t *>(getVirtualAddress(admittedRingBacking));
		for (uint32_t i = previousRingTail / 4; cpuRing && i < ringTail / 4; i += 8)
			SYSLOG("ngreen", "V274: id=%u type=%u ring +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", gucId,
			       static_cast<unsigned int>(hwCsType), i * 4, cpuRing[i], cpuRing[i + 1], cpuRing[i + 2],
			       cpuRing[i + 3], cpuRing[i + 4], cpuRing[i + 5], cpuRing[i + 6], cpuRing[i + 7]);
	}
	// V277: loginwindow's first render batch faults or hangs on a VF; dump it
	// at submission so the fault watcher can still stop the VM immediately.
	static uint32_t loginDumps = 0;
	if (hwCsType == 0 && enable && loginDumps < 2) {
		char name[24] {};
		proc_selfname(name, sizeof(name));
		if (!strncmp(name, "loginwindow", sizeof(name))) {
			loginDumps++;
			vfDumpResetContext(gucId, callback->vfSharedMappedBufferGetVirtualAddress);
		}
	}
	vfScanRingGgttWrites(gucId, static_cast<uint32_t>(hwCsType),
		static_cast<volatile uint32_t *>(getVirtualAddress(admittedRingBacking)), ringSize,
		previousRingTail, ringTail);
	return submitted;
}

bool TglAccel::vfCtbInitWithAccelerator(void *that, void *accelerator) {
	if (!that)
		return false;
	PANIC_COND(getMember<void *>(that, 0x10) || getMember<void *>(that, 0x18) ||
		getMember<void *>(that, 0x20) || getMember<void *>(that, 0x28) ||
		getMember<void *>(that, 0x30) || getMember<void *>(that, 0x40) ||
		getMember<void *>(that, 0x90), "ngreen", "Refusing nonempty VF CTB reinitialization");
	if (gVfIdentity != VfIdentity::Virtual || !accelerator || !gVfGGTTReady ||
	    gVfProtocolFault || !vfCanUseSleepingLock()) {
		getMember<void *>(that, 0x90) = NGWorkQueue::failedInitMarker();
		return false;
	}

	if (gVfCtbBacking ||
	    !OSCompareAndSwapPtr(nullptr, that, &gVfCtbInitOwner)) {
		vfMarkProtocolFault("CTB reinitialization while old backing is quarantined");
		getMember<void *>(that, 0x90) = NGWorkQueue::failedInitMarker();
		return false;
	}

	gVfCtbDisableConfirmed = false;
	gVfCtbAllocatedBacking = nullptr;
	gVfCtbAllocationThread = IOThreadSelf();
	gVfCtbAllocationPending = true;
	const bool result = FunctionCast(vfCtbInitWithAccelerator,
	                                 callback->oVfCtbInitWithAccelerator)(that,
	                                                                        accelerator);
	gVfCtbAllocationPending = false;
	gVfCtbAllocationThread = nullptr;
	gVfCtbAllocatedBacking = nullptr;
	OSCompareAndSwapPtr(that, nullptr, &gVfCtbInitOwner);
	if (!result) {
		struct Operations {
			void unlock(void *lock) { IOLockUnlock(static_cast<IOLock *>(lock)); }
			void freeLock(void *lock) { IOLockFree(static_cast<IOLock *>(lock)); }
			void release(void *object) { static_cast<OSObject *>(object)->release(); }
		} operations;
		PANIC_COND(!NGWorkQueue::unwindFailedCtbInit(getMember<void *>(that, 0x10),
			getMember<void *>(that, 0x18), getMember<void *>(that, 0x20),
			getMember<void *>(that, 0x40), operations) ||
			!NGWorkQueue::markFailedInit(getMember<void *>(that, 0x10),
			getMember<void *>(that, 0x18), getMember<void *>(that, 0x20),
			getMember<void *>(that, 0x90)), "ngreen", "Cannot unwind failed VF CTB init");
		SYSLOG("ngreen", "V223: enlarged VF CTB initialization failed");
		return false;
	}

	if (!gVfCtbBacking || !gVfCtbCpuBase || gVfProtocolFault) {
		vfMarkProtocolFault("CTB initialized without a validated pinned layout");
		// Native init succeeded, so its locks are released but backing exists.
		// Do not run legacy free/transfer/unmap after a layout/protocol fault.
		// If channel init already pinned the object, no second retain is needed.
		if (gVfCtbObject != that)
			static_cast<OSObject *>(that)->retain(); // quarantine until guest reset
		return false;
	}
	return result;
}

void TglAccel::vfCtbFree(void *that) {
	PANIC_COND(!that, "ngreen", "Null VF CTB in free");
	if (getMember<void *>(that, 0x90) == NGWorkQueue::failedInitMarker()) {
		PANIC_COND(that == gVfCtbObject || !callback->vfOSObjectFree ||
			getMember<void *>(that, 0x28) || getMember<void *>(that, 0x30) ||
			getMember<void *>(that, 0x40) || !NGWorkQueue::consumeFailedInit(
			getMember<void *>(that, 0x10), getMember<void *>(that, 0x18),
			getMember<void *>(that, 0x20), getMember<void *>(that, 0x90)),
			"ngreen", "Refusing destruction of active or incompletely unwound VF CTB");
		using BaseFree = void (*)(void *);
		reinterpret_cast<BaseFree>(callback->vfOSObjectFree)(that);
		return;
	}
	FunctionCast(vfCtbFree, callback->oVfCtbFree)(that);
}

void *TglAccel::vfCtbMappedBufferWithOptions(void *accelTask, unsigned long size,
	                                      unsigned int type, unsigned int flags) {
	const bool ctbAllocation = gVfCtbAllocationPending && gVfGGTTReady &&
	    gVfCtbAllocationThread == IOThreadSelf() && gVfCtbInitOwner &&
	    size == PAGE_SIZE && type == 0 && flags == 0;
	if (ctbAllocation) {
		if (gVfCtbAllocatedBacking) {
			vfMarkProtocolFault("multiple backing allocations during CTB initialization");
			return nullptr;
		}
		SYSLOG("ngreen", "V223: expanding VF CTB backing from 0x%lx to 0x%x bytes",
		       size, kVfCtbBackingBytes);
		size = kVfCtbBackingBytes;
	}
	auto *result = FunctionCast(vfCtbMappedBufferWithOptions,
	                    callback->oVfCtbMappedBufferWithOptions)(accelTask, size,
	                                                              type, flags);
	if (ctbAllocation)
		gVfCtbAllocatedBacking = static_cast<OSObject *>(result);
	return result;
}

void TglAccel::vfCtbChannelInit(void *that) {
	// Validate the exact enlarged allocation before the native initializer or
	// our layout writes. A process-global "pending" flag alone could match an
	// unrelated thread's allocation or allow reinitializing a live ring.
	if (!that || !gVfGGTTReady || !gVfCtbAllocationPending ||
	    gVfCtbInitOwner != that || gVfCtbAllocationThread != IOThreadSelf() ||
	    !gVfCtbAllocatedBacking ||
	    getMember<uint64_t>(gVfCtbAllocatedBacking, kVfMappedBufferLengthOffset) <
	        kVfCtbBackingBytes ||
	    getMember<OSObject *>(that, 0x40) != gVfCtbAllocatedBacking || gVfCtbBacking) {
		vfMarkProtocolFault("CTB channel initialization without its enlarged allocation");
		return;
	}
	auto *backing = getMember<OSObject *>(that, 0x40);
	// Native ctChannelInit writes through its CPU mapping before checking it,
	// and truncates GPU addresses to 32 bits. Do not call it to discover either
	// address; use inspected accessors and validate before the first byte write.
	if (!callback->vfSharedMappedBufferGetVirtualAddress ||
	    !callback->vfMappedBufferGetGPUVirtualAddress ||
	    !getMember<void *>(backing, 0x30)) {
		vfMarkProtocolFault("CTB missing CPU/GPU mapping accessor or mapping object");
		return;
	}
	using CpuAddress = uint8_t *(*)(void *);
	using GpuAddress = uint64_t (*)(void *);
	auto *baseCpu = reinterpret_cast<CpuAddress>(
		callback->vfSharedMappedBufferGetVirtualAddress)(backing);
	const uint64_t base = reinterpret_cast<GpuAddress>(
		callback->vfMappedBufferGetGPUVirtualAddress)(backing);
	if (!NGGgtt::mappedBacking(reinterpret_cast<uintptr_t>(baseCpu),
		getMember<uint64_t>(backing, kVfMappedBufferLengthOffset), kVfCtbBackingBytes,
		base, gVfGGTTBase, gVfGGTTSize, kGucGgttTop)) {
		vfMarkProtocolFault("invalid or duplicate CTB backing range");
		return;
	}
	// Pin before publishing addresses, including failure paths inside Apple's
	// initWithAccelerator. Its later free() releases the original reference.
	// Also pin the CTB object: a sender can have captured its queue lock just
	// before shutdown. Backing retention alone does not keep that lock alive.
	// withOptions/init-failure and IGHardwareGuC::free both use release(), so
	// this prevents their CTB free/transferOwnership path during quarantine.
	auto *ctbObject = static_cast<OSObject *>(that);
	ctbObject->retain();
	gVfCtbObject = ctbObject;
	backing->retain();
	gVfCtbBacking = backing;
	gVfCtbGpuBase = static_cast<uint32_t>(base);
	gVfMemIrqConfigured = false;

	bzero(baseCpu, kVfCtbBackingBytes);
	auto *h2gDesc = reinterpret_cast<uint32_t *>(baseCpu + kVfCtbH2GDescOffset);
	auto *g2hDesc = reinterpret_cast<uint32_t *>(baseCpu + kVfCtbG2HDescOffset);
	h2gDesc[0] = gVfCtbGpuBase + kVfCtbH2GBufferOffset;
	h2gDesc[3] = kVfCtbH2GBufferBytes;
	g2hDesc[0] = gVfCtbGpuBase + kVfCtbG2HBufferOffset;
	g2hDesc[3] = kVfCtbG2HBufferBytes;

	getMember<uint8_t *>(that, 0x48) = reinterpret_cast<uint8_t *>(h2gDesc);
	getMember<uint8_t *>(that, 0x50) = baseCpu + kVfCtbH2GBufferOffset;
	getMember<uint8_t *>(that, 0x58) = reinterpret_cast<uint8_t *>(g2hDesc);
	getMember<uint8_t *>(that, 0x60) = baseCpu + kVfCtbG2HBufferOffset;
	// Only modern H2G/G2H are supported; never leave native aliases for the
	// unused channels pointing into the freshly repurposed backing.
	getMember<void *>(that, 0x68) = nullptr;
	getMember<void *>(that, 0x70) = nullptr;
	getMember<void *>(that, 0x78) = nullptr;
	getMember<void *>(that, 0x80) = nullptr;
	getMember<uint32_t>(that, 0x88) = getMember<uint32_t>(that, 0x38);
	// The filter may already be installed. Publish its CPU pointer only after
	// every descriptor and object channel pointer is fully initialized.
	OSSynchronizeIO();
	gVfCtbCpuBase = baseCpu;

	SYSLOG("ngreen", "V223: laid out VF CTB base=0x%08x H2G=0x%x G2H=0x%x backing=0x%x",
	       gVfCtbGpuBase, kVfCtbH2GBufferBytes, kVfCtbG2HBufferBytes,
	       kVfCtbBackingBytes);
}

void TglAccel::vfInvalidateTLB(void *that) {
	OSSynchronizeIO();
	if (!gVfCtbEverEnabled || gVfDmaQuiesced)
		return;
	// The native ABI is void and callers immediately continue updating or
	// retiring shared GPU data. A failed synchronous invalidation cannot be
	// reported, so returning would permit stale translations to outlive their
	// backing.
	PANIC_COND(!that || !vfInvalidateTLBSync(
		that, NGVfGuCRequest::TlbTarget::Guc), "ngreen",
		"Required IGHardwareGuC VF TLB invalidation did not complete");
}

void TglAccel::vfBaseInvalidateTLB(const void *that) {
	(void)that;
	OSSynchronizeIO();
	// i915's gen12vf_ggtt_invalidate skips the request until GuC is ready.
	// CTB-ever-enabled is our corresponding irreversible readiness point. Once
	// the shutdown sweep has deregistered every context and completed its final
	// heavy invalidation, later native object destruction may still free GGTT
	// bookkeeping. No device can consume those PTEs, and the CTB may already be
	// sealed, so a second request would manufacture a teardown protocol fault.
	if (!gVfCtbEverEnabled)
		return;
	if (gVfDmaQuiesced)
		return;
	PANIC_COND(!gVfHardwareGuc || !vfInvalidateTLBSync(
		gVfHardwareGuc, NGVfGuCRequest::TlbTarget::Guc),
		"ngreen", "Required IGGuC VF TLB invalidation did not complete");
}
