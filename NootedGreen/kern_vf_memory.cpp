//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_vf_internal.hpp"

bool TglAccel::vfAllocMoreCommandBuffers(void *pool)
{
	VfSlowSubmitLog slow("command pool growth");
	PANIC_COND(!callback || !callback->oIOAccelAllocMoreCommandBuffers,
	           "ngreen", "Missing native command-pool growth trampoline");
	const bool target = pool && gVfIdentity == VfIdentity::Virtual &&
		gVfAccelerator &&
		getMember<void *>(pool, NGIOAccelCommandPool::acceleratorOffset) ==
			gVfAccelerator;
	const uint16_t maximum = target ?
		getMember<uint16_t>(pool, NGIOAccelCommandPool::maximumOffset) : 0;
	const uint16_t previousCount = target ?
		getMember<uint16_t>(pool, NGIOAccelCommandPool::countOffset) : 0;
	const bool nativeSuccess = FunctionCast(
		vfAllocMoreCommandBuffers,
		callback->oIOAccelAllocMoreCommandBuffers)(pool);
	if (!target || !nativeSuccess)
		return nativeSuccess;

	const uint16_t publishedCount =
		getMember<uint16_t>(pool, NGIOAccelCommandPool::countOffset);
	const int16_t current =
		getMember<int16_t>(pool, NGIOAccelCommandPool::currentOffset);
	if (previousCount >= NGIOAccelCommandPool::slotCapacity)
		return false;
	const size_t slot = NGIOAccelCommandPool::slotsOffset +
		static_cast<size_t>(previousCount) * NGIOAccelCommandPool::slotStride;
	const uintptr_t memory = reinterpret_cast<uintptr_t>(
		getMember<void *>(pool, slot));
	const uintptr_t gpuMapping = reinterpret_cast<uintptr_t>(
		getMember<void *>(pool, slot + sizeof(uintptr_t)));
	const uintptr_t cpuMapping = reinterpret_cast<uintptr_t>(
		getMember<void *>(pool, slot + 2 * sizeof(uintptr_t)));
	const bool complete = NGIOAccelCommandPool::completedGrowth(
		nativeSuccess, maximum, previousCount, publishedCount, current,
		memory, gpuMapping, cpuMapping);
	if (!complete)
		SYSLOG("ngreen", "V268: rejecting incomplete VF command-pool growth old=%u new=%u current=%d",
		       previousCount, publishedCount, current);
	return complete;
}

// V367: the 256-slot walk is for refusal diagnostics; the getter runs for
// every command write, so its pre-call snapshot skips it.
static NGIOAccelCommandPool::ControlFrame vfCommandPoolFrame(void *pool, bool slots = true)
{
	NGIOAccelCommandPool::ControlFrame frame {};
	frame.maximum = getMember<uint16_t>(pool, NGIOAccelCommandPool::maximumOffset);
	frame.count = getMember<uint16_t>(pool, NGIOAccelCommandPool::countOffset);
	frame.current = getMember<int16_t>(pool, NGIOAccelCommandPool::currentOffset);
	frame.bytes = getMember<uint32_t>(pool, NGIOAccelCommandPool::bufferBytesOffset);
	frame.reserved = getMember<uint32_t>(pool, NGIOAccelCommandPool::reservedBytesOffset);
	frame.allocationOptions = getMember<uint32_t>(pool, NGIOAccelCommandPool::allocationOptionsOffset);
	frame.mappingOptions = getMember<uint32_t>(pool, NGIOAccelCommandPool::mappingOptionsOffset);
	frame.alignment = getMember<uint64_t>(pool, NGIOAccelCommandPool::alignmentOffset);
	frame.start = getMember<uintptr_t>(pool, NGIOAccelCommandPool::startOffset);
	frame.end = getMember<uintptr_t>(pool, NGIOAccelCommandPool::endOffset);
	frame.cursor = getMember<uintptr_t>(pool, NGIOAccelCommandPool::cursorOffset);
	frame.links = (getMember<void *>(pool, 0x10) != nullptr ? 1U : 0U) |
		(getMember<void *>(pool, 0x18) != nullptr ? 2U : 0U) |
		(getMember<void *>(pool, 0x20) != nullptr ? 4U : 0U) |
		(getMember<void *>(pool, 0x28) != nullptr ? 8U : 0U) |
		(getMember<void *>(pool, NGIOAccelCommandPool::recordOffset) != nullptr ? 16U : 0U);
	for (size_t index = 0; slots && index < NGIOAccelCommandPool::slotCapacity; ++index) {
		const size_t slot = NGIOAccelCommandPool::slotsOffset + index * NGIOAccelCommandPool::slotStride;
		frame.slot(index, getMember<void *>(pool, slot) != nullptr,
			getMember<void *>(pool, slot + sizeof(uintptr_t)) != nullptr,
			getMember<void *>(pool, slot + 2 * sizeof(uintptr_t)) != nullptr);
	}
	return frame;
}

void *TglAccel::vfGetCommandBufferPtrNoInc(void *pool, uint32_t dwords)
{
	PANIC_COND(!callback || !callback->oIOAccelGetCommandBufferPtrNoInc,
	           "ngreen", "Missing native command-pool getter trampoline");
	const bool target = pool && gVfIdentity == VfIdentity::Virtual &&
		gVfAccelerator &&
		getMember<void *>(pool, NGIOAccelCommandPool::acceleratorOffset) ==
			gVfAccelerator;
	vfSlowMark(">getptr");
	const NGIOAccelCommandPool::ControlFrame before = target ?
		vfCommandPoolFrame(pool, false) : NGIOAccelCommandPool::ControlFrame {};
	vfSlowMark("getptr pre");
	void *const result = FunctionCast(
		vfGetCommandBufferPtrNoInc,
		callback->oIOAccelGetCommandBufferPtrNoInc)(pool, dwords);
	vfSlowMark("getptr native");
	if (!target)
		return result;

	const uint16_t maximum =
		getMember<uint16_t>(pool, NGIOAccelCommandPool::maximumOffset);
	const uint16_t count =
		getMember<uint16_t>(pool, NGIOAccelCommandPool::countOffset);
	const int16_t current =
		getMember<int16_t>(pool, NGIOAccelCommandPool::currentOffset);
	uintptr_t memory = 0, gpuMapping = 0, cpuMapping = 0;
	if (current >= 0 && static_cast<uint16_t>(current) < count &&
	    static_cast<uint16_t>(current) < NGIOAccelCommandPool::slotCapacity) {
		const size_t slot = NGIOAccelCommandPool::slotsOffset +
			static_cast<size_t>(current) * NGIOAccelCommandPool::slotStride;
		memory = reinterpret_cast<uintptr_t>(getMember<void *>(pool, slot));
		gpuMapping = reinterpret_cast<uintptr_t>(
			getMember<void *>(pool, slot + sizeof(uintptr_t)));
		cpuMapping = reinterpret_cast<uintptr_t>(
			getMember<void *>(pool, slot + 2 * sizeof(uintptr_t)));
	}
	const uintptr_t start = reinterpret_cast<uintptr_t>(
		getMember<void *>(pool, NGIOAccelCommandPool::startOffset));
	const uintptr_t end = reinterpret_cast<uintptr_t>(
		getMember<void *>(pool, NGIOAccelCommandPool::endOffset));
	const uintptr_t cursor = reinterpret_cast<uintptr_t>(
		getMember<void *>(pool, NGIOAccelCommandPool::cursorOffset));
	const bool complete = NGIOAccelCommandPool::hasReturnedCapacity(
		maximum, count, current, memory, gpuMapping, cpuMapping,
		start, end, cursor, reinterpret_cast<uintptr_t>(result), dwords,
		getMember<uint32_t>(pool, NGIOAccelCommandPool::bufferBytesOffset),
		getMember<uint32_t>(pool, NGIOAccelCommandPool::reservedBytesOffset));
	if (!complete) {
		const NGIOAccelCommandPool::ControlFrame after = vfCommandPoolFrame(pool);
		const char *const refusal = NGIOAccelCommandPool::returnedCapacityRefusal(
			maximum, count, current, memory, gpuMapping, cpuMapping,
			start, end, cursor, reinterpret_cast<uintptr_t>(result), dwords,
			getMember<uint32_t>(pool, NGIOAccelCommandPool::bufferBytesOffset),
			getMember<uint32_t>(pool, NGIOAccelCommandPool::reservedBytesOffset));
		const uintptr_t returned = reinterpret_cast<uintptr_t>(result);
		const uint64_t remaining = returned >= start && returned <= end ?
			static_cast<uint64_t>(end - returned) : 0;
		vfMarkProtocolFault("VF command-pool getter returned inadequate capacity");
		PANIC_COND(true, "ngreen",
		           "V272: refusing inadequate VF command buffer request=%u current=%d count=%u"
		           " V362: reason=%s remaining=%llu BEFORE" NG_POOL_FRAME_FORMAT
		           " AFTER" NG_POOL_FRAME_FORMAT,
		           dwords, current, count, refusal, remaining,
		           NG_POOL_FRAME_ARGS(before), NG_POOL_FRAME_ARGS(after));
	}
	vfSlowMark("getptr post");
	return result;
}

#undef NG_POOL_FRAME_ARGS
#undef NG_POOL_FRAME_FORMAT
#undef NG_POOL_MASK_ARGS
#undef NG_POOL_MASK_FORMAT

static bool vfEventVectorGrowChecked(void *vector, size_t requested,
	bool (*native)(void *, size_t))
{
	PANIC_COND(!vector || !native, "ngreen",
	           "Missing VF event-vector growth state or trampoline");
	if (gVfIdentity == VfIdentity::Virtual) {
		const size_t vectorSize =
			getMember<size_t>(vector, NGEventVector::sizeOffset);
		const size_t capacity =
			getMember<size_t>(vector, NGEventVector::capacityOffset);
		const uintptr_t storage = reinterpret_cast<uintptr_t>(
			getMember<void *>(vector, NGEventVector::storageOffset));
		if (!NGEventVector::hasConsistentState(vectorSize, capacity, storage) ||
		    !NGEventVector::hasRepresentableRequest(requested)) {
			vfMarkProtocolFault("VF event-vector growth received unsafe pre-state");
			PANIC_COND(true, "ngreen",
			           "V273: refusing unsafe VF event vector size=%llu capacity=%llu request=%llu",
			           static_cast<uint64_t>(vectorSize), static_cast<uint64_t>(capacity),
			           static_cast<uint64_t>(requested));
		}
	}
	const bool nativeResult = native(vector, requested);
	if (gVfIdentity != VfIdentity::Virtual)
		return nativeResult;
	const size_t vectorSize =
		getMember<size_t>(vector, NGEventVector::sizeOffset);
	const size_t capacity =
		getMember<size_t>(vector, NGEventVector::capacityOffset);
	const uintptr_t storage = reinterpret_cast<uintptr_t>(
		getMember<void *>(vector, NGEventVector::storageOffset));
	if (!NGEventVector::hasCapacity(
	        vectorSize, capacity, storage, requested)) {
		vfMarkProtocolFault("VF event-vector growth did not publish requested capacity");
		PANIC_COND(true, "ngreen",
		           "V273: refusing incomplete VF event vector size=%llu capacity=%llu request=%llu",
		           static_cast<uint64_t>(vectorSize), static_cast<uint64_t>(capacity),
		           static_cast<uint64_t>(requested));
	}
	return nativeResult;
}

bool TglAccel::vf2DEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVf2DEventVectorGrow,
	           "ngreen", "Missing 2D event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVf2DEventVectorGrow));
}

bool TglAccel::vfAcceleratorEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfAcceleratorEventVectorGrow,
	           "ngreen", "Missing accelerator event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfAcceleratorEventVectorGrow));
}

bool TglAccel::vfRenderEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfRenderEventVectorGrow,
	           "ngreen", "Missing render event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfRenderEventVectorGrow));
}

bool TglAccel::vfBlitEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfBlitEventVectorGrow,
	           "ngreen", "Missing blit event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfBlitEventVectorGrow));
}

bool TglAccel::vfGLEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfGLEventVectorGrow,
	           "ngreen", "Missing GL event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfGLEventVectorGrow));
}

bool TglAccel::vfResourceEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfResourceEventVectorGrow,
	           "ngreen", "Missing resource event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfResourceEventVectorGrow));
}

bool TglAccel::vfSharedEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfSharedEventVectorGrow,
	           "ngreen", "Missing shared event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfSharedEventVectorGrow));
}

bool TglAccel::vfSurfaceEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfSurfaceEventVectorGrow,
	           "ngreen", "Missing surface event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfSurfaceEventVectorGrow));
}

bool TglAccel::IGMemoryManagerInitSegments(void *that)
{
	if (gVfIdentity != VfIdentity::Virtual || !that || !vfBootstrapDirectGgtt())
		return false;

	// The pinned native body only writes four IGAddressRange fields, but obtains
	// the first two from physical stolen memory and BAR2. A VF owns neither.
	// IGMemoryManager::init also ignores this method's return value, so an
	// unusable plan must fault the protocol and force the later global-page-table
	// initializer to fail instead of merely returning false here.
	const auto plan = NGGgtt::vfSegmentPlan(gVfGGTTBase, gVfGGTTSize);
	if (!plan.valid) {
		getMember<uint64_t>(that, 0xA0) = 0;
		getMember<uint64_t>(that, 0xA8) = 0;
		getMember<uint64_t>(that, 0xB0) = 0;
		getMember<uint64_t>(that, 0xB8) = 0;
		getMember<uint64_t>(that, 0xC0) = 0;
		getMember<uint64_t>(that, 0xC8) = 0;
		getMember<uint64_t>(that, 0xD0) = 0;
		getMember<uint64_t>(that, 0xD8) = 0;
		vfMarkProtocolFault("VF assignment cannot satisfy memory-manager segments");
		return false;
	}

	getMember<uint64_t>(that, 0xA0) = plan.globalStart;
	getMember<uint64_t>(that, 0xA8) = plan.globalLength;
	getMember<uint64_t>(that, 0xB0) = plan.globalStart;
	getMember<uint64_t>(that, 0xB8) = plan.globalLength;
	getMember<uint64_t>(that, 0xC0) = plan.unified32Start;
	getMember<uint64_t>(that, 0xC8) = plan.unified32Length;
	// This is the native 48-bit canonical PPGTT range. It is virtual address
	// space and must not be clipped to the VF's GGTT aperture.
	getMember<uint64_t>(that, 0xD0) = 0x40000000ULL;
	getMember<uint64_t>(that, 0xD8) = 0xFFFFFFFFC0000000ULL;

	SYSLOG("ngreen", "V217: patched IGMemoryManager GGTT ranges global=[0x%llx,+0x%llx] unified=[0x%llx,+0x%llx]",
	       static_cast<unsigned long long>(plan.globalStart),
	       static_cast<unsigned long long>(plan.globalLength),
	       static_cast<unsigned long long>(plan.unified32Start),
	       static_cast<unsigned long long>(plan.unified32Length));
	return true;
}

// V365: the PF provisions every VF GGTT slot as present with address 0 (VFID
// only), and native init's clear is suppressed, so a GPU write through a slot
// the guest never mapped would land on guest physical page 0. Point those
// slots at a private page and report anything that lands there. (The host's
// "[00:02.0] write fault addr 0x0" DMAR faults do not: nothing has so far.)
static IOBufferMemoryDescriptor *gVfGgttTrap;
static void vfWatchGgttTrap(void *, wait_result_t) {
	auto *page = static_cast<volatile uint64_t *>(gVfGgttTrap->getBytesNoCopy());
	uint32_t logs = 0;
	for (;;) {
		IOSleep(1000);
		for (uint32_t i = 0; i < 512; i++) {
			if (!page[i])
				continue;
			if (logs < 200) {
				logs++;
				SYSLOG("ngreen", "V365: GGTT trap +%03x: %016llx %016llx %016llx %016llx", i * 8, page[i],
				       i + 1 < 512 ? page[i + 1] : 0, i + 2 < 512 ? page[i + 2] : 0, i + 3 < 512 ? page[i + 3] : 0);
			}
			page[i] = 0;
		}
	}
}

static void vfTrapUnmappedGgtt(void *table) {
	auto *pte = vfDirectPteBase(table);
	if (!pte || !gVfGGTTSize)
		return;
	gVfGgttTrap = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		kIODirectionInOut | kIOMemoryPhysicallyContiguous, 0x1000, 0xFFFFFFFFF000ULL);
	if (!gVfGgttTrap)
		return;
	bzero(gVfGgttTrap->getBytesNoCopy(), 0x1000);
	const uint64_t trap = NGVfGgttPte::encodeSystemMemory(gVfGgttTrap->getPhysicalAddress());
	uint32_t count = 0;
	for (uint64_t gpu = gVfGGTTBase; gpu < gVfGGTTBase + gVfGGTTSize; gpu += 0x1000)
		if (!(pte[gpu >> 12] & ~0xFFFULL))
			pte[gpu >> 12] = trap, count++;
	OSSynchronizeIO();
	thread_t thread = nullptr;
	if (kernel_thread_start(vfWatchGgttTrap, nullptr, &thread) == KERN_SUCCESS)
		thread_deallocate(thread);
	SYSLOG("ngreen", "V365: GGTT trap page 0x%llx behind %u unmapped VF GGTT slots",
	       static_cast<unsigned long long>(gVfGgttTrap->getPhysicalAddress()), count);
}

bool TglAccel::IGHardwareGlobalPageTableInitWithOptions(void *that,
                                                     void *accelerator,
                                                     const NGIGAddressRange &range,
                                                     void *mmioBase,
                                                     uint64_t dummyPage,
                                                     uint32_t options)
{
	if (gVfIdentity != VfIdentity::Virtual || !that || !accelerator ||
	    !NGGgtt::nativePhysicalRange(dummyPage, 0x1000)) {
		vfMarkProtocolFault("invalid VF GGTT receiver or truncated dummy DMA address");
		return false;
	}
	if (!vfBootstrapDirectGgtt())
		return false;

	// Native init's second loop clears from range.end to range.start + 4 GiB,
	// not to 4 GiB. A nonzero VF base would index beyond the 8 MiB PTE window.
	// Suppress both native loops for the admitted direct VF transport
	// (i915 nop_clear_range).
	// In this inspected payload the first loop is skipped by unsigned wrap,
	// and length == 4 GiB skips the second. No fabricated range is published.
	const NGIGAddressRange noDirectClear {UINT64_MAX, 0x100000000ULL};
	auto *cb = NGreen::callback;
	if (!cb || !cb->setRMMIOIfNecessary())
		return false;
	if (!cb->getRMMIOAddress() || cb->getRMMIOLength() < kVfDirectBar0Bytes) {
		SYSLOG("ngreen", "V219: full BAR0 mapping unavailable for direct GGTT");
		return false;
	}

	// Apple's accelerator-created mapping covered only the lower MMIO pages in
	// V216, so initWithOptions faulted when it reached BAR0+8 MiB. Reuse the
	// complete IOPCIDevice BAR0 mapping owned by NootedGreen; this is the direct
	// GGTT transport selected by Linux for this media-version-12 VF.
	void *fullBar0 = const_cast<UInt32 *>(cb->getRMMIOAddress());
	const bool result = FunctionCast(IGHardwareGlobalPageTableInitWithOptions,
	                                 callback->oIGHardwareGlobalPageTableInitWithOptions)(that,
	                                                                                      accelerator,
	                                                                                      noDirectClear,
	                                                                                      fullBar0,
	                                                                                      dummyPage,
	                                                                                      options);
	if (result) {
		gVfGlobalPageTable = that;
		vfTrapUnmappedGgtt(that);
	}
	SYSLOG("ngreen", "V219: direct global GGTT init ret=%d AppleMMIO=%p fullBAR0=%p len=0x%llx range=[0x%llx,+0x%llx]",
	       result, mmioBase, fullBar0,
	       static_cast<unsigned long long>(cb->getRMMIOLength()),
	       static_cast<unsigned long long>(range.start),
	       static_cast<unsigned long long>(range.length));
	return result;
}

volatile uint64_t *vfDirectPteBase(void *that)
{
	auto *pteBase = that ? getMember<volatile uint64_t *>(that, 0x28) : nullptr;
	auto *cb = NGreen::callback;
	if (!pteBase || !cb || !cb->getRMMIOAddress() ||
	    cb->getRMMIOLength() < kVfDirectBar0Bytes ||
	    pteBase != reinterpret_cast<volatile uint64_t *>(
	        const_cast<UInt32 *>(cb->getRMMIOAddress()) +
	        kVfGGTTPteBase / sizeof(UInt32)))
		return nullptr;
	return pteBase;
}

enum class VfGgttDiagnosticOperation : uint32_t {
	Map = 1,
	RotatedMap = 2,
	RotatedRollback = 3,
	Unmap = 4,
	DummyMap = 5,
};

static UInt32 vfStoreFirstPteWithDiagnostic(volatile uint64_t *pteBase,
	                                         uint64_t gpu, uint64_t physical,
	                                         uint64_t intended,
	                                         VfGgttDiagnosticOperation operation,
	                                         uint64_t operationLength)
{
	UInt32 sample = gVfGgttDiagnosticWrites;
	while (sample < 33) {
		const UInt32 next = sample + 1;
		if (OSCompareAndSwap(sample, next, &gVfGgttDiagnosticWrites)) {
			sample = next;
			break;
		}
		sample = gVfGgttDiagnosticWrites;
	}
	if (sample == 0 || sample > 32) {
		pteBase[gpu >> 12] = intended;
		return 0;
	}

	// Read the VF-visible raw entry before and after the exact pre-existing
	// encoded store.  Do not merge `before` into `intended`: ownership bits are
	// still a hypothesis and V312 must distinguish hardware preservation from a
	// guest write that clears them.  A completed readback also places the record
	// in the log before any later autonomous transaction can trip containment.
	OSSynchronizeIO();
	const uint64_t before = pteBase[gpu >> 12];
	pteBase[gpu >> 12] = intended;
	__asm__ volatile("sfence" ::: "memory");
	OSSynchronizeIO();
	const uint64_t after = pteBase[gpu >> 12];
	SYSLOG("ngreen", "V312: GGTT sample=%d op=%u gpu=0x%llx length=0x%llx physical=0x%llx before=0x%016llx intended=0x%016llx after=0x%016llx",
	       static_cast<int>(sample), static_cast<unsigned int>(operation),
	       static_cast<unsigned long long>(gpu),
	       static_cast<unsigned long long>(operationLength),
	       static_cast<unsigned long long>(physical),
	       static_cast<unsigned long long>(before),
	       static_cast<unsigned long long>(intended),
	       static_cast<unsigned long long>(after));
	return sample;
}

// V369: processes still create resources while the system shuts down (a
// Blit3D context at exit). Once every context is retired and translations are
// invalidated (gVfDmaQuiesced), no GPU work can consume a GGTT entry, so later
// maps need no invalidation; refusing them faulted the VF and the next
// mapping panicked.
static bool vfCanCompleteGgttUpdate()
{
	OSSynchronizeIO();
	if (!gVfCtbEverEnabled || gVfDmaQuiesced)
		return true;
	if (!gVfHardwareGuc || !gVfCtbEnabled || gVfCtbStopped ||
	    gVfProtocolFault || !vfCanWaitForGuc(gVfHardwareGuc)) {
		vfMarkProtocolFault("VF GGTT update has no synchronous TLB transport");
		return false;
	}
	return true;
}

static bool vfCompleteGgttUpdate()
{
	// An aligned volatile 64-bit store replaces Apple's split high/low PTE
	// writes. Drain the WC/MMIO aperture before publishing or invalidating it.
	OSSynchronizeIO();
	__asm__ volatile("sfence" ::: "memory");
	OSSynchronizeIO();
	if (!gVfCtbEverEnabled || gVfDmaQuiesced)
		return true;
	if (!gVfHardwareGuc || !vfInvalidateTLBSync(
		gVfHardwareGuc, NGVfGuCRequest::TlbTarget::Guc)) {
		vfMarkProtocolFault("VF GGTT update did not complete TLB invalidation");
		return false;
	}
	return true;
}

// V358: page-table commits map every resource segment separately; a GuC TLB
// round trip per segment (470 per Activity Monitor refresh, 16k for a
// wallpaper video) held the accelerator for up to 230 ms. Inside a commit or
// update transaction the maps only take effect once it returns, so retire
// them with one invalidation at its end.
static uint32_t gVfGgttDeferDepth = 0;
static bool gVfGgttDeferred = false;

static void vfRequireCompletedGgttUpdate()
{
	// Native commit failure does not publish the installed-PTE flag. Its
	// cleanup can therefore skip release_pte and drop the backing even though
	// our direct PTE stores have already happened. Never return into that
	// cleanup after a failed post-write invalidation. A Guest panic is NOT
	// proof that Host DMA has stopped; runtime containment remains mandatory.
	PANIC_COND(!vfCompleteGgttUpdate(), "ngreen",
		"VF GGTT stores lack confirmed invalidation; refusing native backing cleanup");
}

// Map paths: defer inside this thread's commit/update transaction.
static void vfRequireCompletedGgttMap()
{
	if (gVfGgttDeferDepth && gVfPageTableUpdateLock && IORecursiveLockHaveLock(gVfPageTableUpdateLock)) {
		gVfGgttDeferred = true;
		return;
	}
	vfRequireCompletedGgttUpdate();
}

// Enter/leave with gVfPageTableUpdateLock held.
static void vfDeferGgttMaps() { gVfGgttDeferDepth++; }
static void vfFlushGgttMaps()
{
	if (--gVfGgttDeferDepth || !gVfGgttDeferred)
		return;
	gVfGgttDeferred = false;
	vfRequireCompletedGgttUpdate();
}

// Native GGTT range loops store one PTE per page while gpu < start + length,
// so a sub-page length (e.g. a 0x240-byte dummy map) still covers its page.
static NGIGAddressRange vfPageRange(const NGIGAddressRange &range)
{
	return {range.start & ~0xFFFULL, (range.length + 0xFFFULL) & ~0xFFFULL};
}

bool TglAccel::IGHardwareGlobalPageTableMapRange(void *that,
                                              const NGIGAddressRange &nativeRange,
                                              uint64_t physical,
                                              uint64_t flags)
{
	const NGIGAddressRange range = vfPageRange(nativeRange);
	// Validate the complete operation before touching the direct PTE aperture.
	// Do not pass Apple's physical-driver cache attributes through: on Gen12
	// bits 4:2 are PF-owned VFID and bit 1 is local memory, neither of which a
	// media-12 integrated direct VF may encode.
	auto *pteBase = vfDirectPteBase(that);
	if (gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady ||
	    gVfProtocolFault ||
	    !that || that != gVfGlobalPageTable ||
	    !NGGgtt::contains(gVfGGTTBase, gVfGGTTSize, range.start, range.length) ||
	    !NGGgtt::nativePhysicalRange(physical, range.length) || !pteBase ||
	    !NGVfGgttPte::validAppleAttributes(flags) ||
	    !vfCanCompleteGgttUpdate()) {
		vfMarkProtocolFault("invalid VF GGTT map range or transport state");
		return false;
	}
	const uint64_t end = range.start + range.length;
	UInt32 diagnosticSample = 0;
	for (uint64_t gpu = range.start; gpu < end; gpu += 0x1000ULL) {
		const uint64_t intended = NGVfGgttPte::encodeSystemMemory(physical);
		if (gpu == range.start)
			diagnosticSample = vfStoreFirstPteWithDiagnostic(
				pteBase, gpu, physical, intended,
				VfGgttDiagnosticOperation::Map, range.length);
		else
			pteBase[gpu >> 12] = intended;
		physical += 0x1000ULL;
	}
	vfRequireCompletedGgttMap();
	if (diagnosticSample != 0)
		SYSLOG("ngreen", "V316: GGTT complete sample=%d gpu=0x%llx length=0x%llx",
		       static_cast<int>(diagnosticSample),
		       static_cast<unsigned long long>(range.start),
		       static_cast<unsigned long long>(range.length));
	return true;
}

bool TglAccel::IGHardwareGlobalPageTableMapRangeRotated(void *that,
                                                     void *rangeIterator,
                                                     void *physicalIterator,
                                                     uint64_t flags)
{
	// Reconstructed from Tahoe's commitRange (0x14090) and global
	// mapRangeRotated (0x103a0). Native performs its first PTE store before
	// validating divisor/cursor bounds and can store at the exclusive end. Keep
	// the private ABI local and replace only the VF implementation.
	struct RotatedRangeIterator {
		const NGIGAddressRange *range;
		uint32_t sourcePage;
		uint32_t widthPages;
		uint32_t heightPages;
		uint32_t reserved;
		uint64_t cursor;
	};
	struct PhysicalSegmentIterator {
		IOMemoryDescriptor *memory;
		uint64_t length;
		uint32_t options;
		uint32_t reserved;
	};
	static_assert(sizeof(RotatedRangeIterator) == 0x20,
	              "Tahoe rotated-range iterator ABI drift");
	static_assert(offsetof(RotatedRangeIterator, cursor) == 0x18,
	              "Tahoe rotated-range cursor ABI drift");
	static_assert(offsetof(PhysicalSegmentIterator, options) == 0x10,
	              "Tahoe physical iterator ABI drift");

	if (gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady ||
	    gVfProtocolFault || !that ||
	    that != gVfGlobalPageTable || !rangeIterator || !physicalIterator ||
	    !NGVfGgttPte::validAppleAttributes(flags) ||
	    !vfCanCompleteGgttUpdate()) {
		vfMarkProtocolFault("invalid VF rotated GGTT mapping state");
		return false;
	}
	auto *rotated = static_cast<RotatedRangeIterator *>(rangeIterator);
	auto *segments = static_cast<PhysicalSegmentIterator *>(physicalIterator);
	if (!rotated->range || !segments->memory) {
		vfMarkProtocolFault("missing VF rotated GGTT iterator backing");
		return false;
	}

	const uint64_t descriptorLength = segments->memory->getLength();
	if (!descriptorLength || descriptorLength > UINT64_MAX - 0xFFFULL) {
		vfMarkProtocolFault("invalid VF rotated physical descriptor length");
		return false;
	}
	const uint64_t alignedDescriptorLength =
		(descriptorLength + 0xFFFULL) & ~0xFFFULL;
	const NGGgttRotation::Spec spec {
		gVfGGTTBase, gVfGGTTSize,
		rotated->range->start, rotated->range->length,
		segments->length, rotated->cursor, rotated->sourcePage,
		rotated->widthPages, rotated->heightPages,
	};
	if (segments->length != alignedDescriptorLength ||
	    !NGGgttRotation::valid(spec)) {
		vfMarkProtocolFault("malformed VF rotated GGTT iterator geometry");
		return false;
	}

	auto *pteBase = vfDirectPteBase(that);
	const uint64_t dummyPage = getMember<uint64_t>(that, 0x38);
	if (!pteBase || !NGGgtt::nativePhysicalRange(dummyPage, 0x1000)) {
		vfMarkProtocolFault("VF rotated GGTT PTE aperture mismatch");
		return false;
	}

	// Preflight the entire descriptor before the first PTE write. The retained,
	// prepared descriptor is walked again for mapping; every segment is still
	// revalidated, and a rare second-pass failure rolls all written destinations
	// back to the pinned dummy page before returning failure.
	segments->memory->retain();
	bool validSegments = true;
	uint64_t offset = 0;
	while (offset < segments->length) {
		IOByteCount rawLength = 0;
		const uint64_t physical = segments->memory->getPhysicalSegment(
			offset, &rawLength, segments->options);
		if (!rawLength || rawLength > UINT64_MAX - 0xFFFULL) {
			validSegments = false;
			break;
		}
		const uint64_t length = (rawLength + 0xFFFULL) & ~0xFFFULL;
		if (length > segments->length - offset ||
		    !NGGgtt::nativePhysicalRange(physical, length)) {
			validSegments = false;
			break;
		}
		offset += length;
	}
	if (!validSegments || offset != segments->length) {
		segments->memory->release();
		vfMarkProtocolFault("invalid VF rotated physical segment sequence");
		return false;
	}

	uint64_t mappedPages = 0;
	offset = 0;
	while (offset < segments->length && validSegments) {
		IOByteCount rawLength = 0;
		uint64_t physical = segments->memory->getPhysicalSegment(
			offset, &rawLength, segments->options);
		if (!rawLength || rawLength > UINT64_MAX - 0xFFFULL) {
			validSegments = false;
			break;
		}
		const uint64_t length = (rawLength + 0xFFFULL) & ~0xFFFULL;
		if (length > segments->length - offset ||
		    !NGGgtt::nativePhysicalRange(physical, length)) {
			validSegments = false;
			break;
		}
		for (uint64_t consumed = 0; consumed < length; consumed += 0x1000ULL) {
			uint64_t destination = 0;
			if (!NGGgttRotation::destination(spec, mappedPages, destination)) {
				validSegments = false;
				break;
			}
			const uint64_t intended = NGVfGgttPte::encodeSystemMemory(physical);
			if (mappedPages == 0)
				vfStoreFirstPteWithDiagnostic(
					pteBase, destination, physical, intended,
					VfGgttDiagnosticOperation::RotatedMap,
					spec.rangeLength);
			else
				pteBase[destination >> 12] = intended;
			physical += 0x1000ULL;
			mappedPages++;
		}
		offset += length;
	}

	const uint64_t totalPages = spec.rangeLength >> 12;
	if (!validSegments || offset != segments->length || mappedPages != totalPages) {
		const uint64_t dummyPte = NGVfGgttPte::encodeSystemMemory(dummyPage);
		for (uint64_t source = 0; source < mappedPages; source++) {
			uint64_t destination = 0;
			if (NGGgttRotation::destination(spec, source, destination)) {
				if (source == 0)
					vfStoreFirstPteWithDiagnostic(
						pteBase, destination, dummyPage, dummyPte,
						VfGgttDiagnosticOperation::RotatedRollback,
						mappedPages * 0x1000ULL);
				else
					pteBase[destination >> 12] = dummyPte;
			}
		}
		vfRequireCompletedGgttUpdate();
		segments->memory->release();
		vfMarkProtocolFault("VF rotated physical segment changed during mapping");
		return false;
	}

	rotated->sourcePage = static_cast<uint32_t>(totalPages);
	rotated->cursor = spec.rangeStart + spec.rangeLength;
	vfRequireCompletedGgttMap();
	segments->memory->release();

	return true;
}

void TglAccel::IGHardwareGlobalPageTableUnmapRange(void *that,
                                                const NGIGAddressRange &nativeRange)
{
	const NGIGAddressRange range = vfPageRange(nativeRange);
	// A void unmap cannot tell its caller not to free/reuse DMA backing.
	// Tahoe IOAccelMemoryMap::release_pte also ignores the Intel mapping
	// release bool, so returning false further up cannot preserve backing.
	// Refuse to continue teardown on invalid input or a faulted VF; silently
	// returning would turn a skipped PTE write into a use-after-free risk.
	PANIC_COND(gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady ||
		(gVfCtbEverEnabled && gVfProtocolFault && !gVfDmaQuiesced) ||
		!that || that != gVfGlobalPageTable ||
		!NGGgtt::contains(gVfGGTTBase, gVfGGTTSize, range.start, range.length),
		"ngreen", "Cannot safely complete VF GGTT unmap; refusing DMA backing release");
	const auto invalidation = NGGgtt::unmapInvalidation(
		gVfCtbEverEnabled != 0, gVfCtbEnabled != 0,
		gVfCtbStopped != 0, gVfProtocolFault != 0,
		gVfDmaQuiesced != 0);
	auto *pteBase = vfDirectPteBase(that);
	const uint64_t dummyPage = getMember<uint64_t>(that, 0x38);
	PANIC_COND(invalidation == NGGgtt::TlbInvalidation::Unsafe,
		"ngreen", "VF GGTT unmap started without a usable TLB transport");
	PANIC_COND(!pteBase || !NGGgtt::nativePhysicalRange(dummyPage, 0x1000),
		"ngreen", "VF GGTT unmap has no valid direct PTE or dummy page");
	const uint64_t dummyPte = NGVfGgttPte::encodeSystemMemory(dummyPage);
	const uint64_t end = range.start + range.length;
	for (uint64_t gpu = range.start; gpu < end; gpu += 0x1000ULL) {
		if (gpu == range.start)
			vfStoreFirstPteWithDiagnostic(
				pteBase, gpu, dummyPage, dummyPte,
				VfGgttDiagnosticOperation::Unmap, range.length);
		else
			pteBase[gpu >> 12] = dummyPte;
	}

	// Apple releaseRange calls this virtual method, sets only a deferred flush
	// bit and can then return to a caller that releases the DMA mapping. A
	// completed heavy GuC invalidation is therefore part of the unmap
	// transaction, not an optional diagnostic. The sfence matches the stock
	// physical invalidator and drains direct BAR0 PTE stores first.
	__asm__ volatile("sfence" ::: "memory");
	if (invalidation == NGGgtt::TlbInvalidation::NotRequired)
		return;
	PANIC_COND(!vfInvalidateTLBSync(
		gVfHardwareGuc, NGVfGuCRequest::TlbTarget::Guc),
		"ngreen", "VF GGTT unmap could not quiesce translations before DMA release");
}

bool TglAccel::IGHardwareGlobalPageTableMapRangeDummy(void *that,
                                                   const NGIGAddressRange &nativeRange,
                                                   uint64_t flags)
{
	const NGIGAddressRange range = vfPageRange(nativeRange);
	if (gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady ||
	    gVfProtocolFault ||
	    !that || that != gVfGlobalPageTable ||
	    !NGGgtt::contains(gVfGGTTBase, gVfGGTTSize, range.start, range.length) ||
	    !NGVfGgttPte::validAppleAttributes(flags) ||
	    !vfCanCompleteGgttUpdate()) {
		SYSLOG("ngreen", "V370: dummy map rejected id=%u ready=%u fault=%u table=%u range=%llx+%llx ggtt=%llx+%llx flags=%llx",
		       gVfIdentity == VfIdentity::Virtual, gVfGGTTReady ? 1U : 0U, gVfProtocolFault ? 1U : 0U,
		       that && that == gVfGlobalPageTable, range.start, range.length,
		       static_cast<unsigned long long>(gVfGGTTBase), static_cast<unsigned long long>(gVfGGTTSize), flags);
		vfMarkProtocolFault("invalid VF dummy GGTT range or transport state");
		return false;
	}
	auto *pteBase = vfDirectPteBase(that);
	const uint64_t dummyPage = getMember<uint64_t>(that, 0x38);
	if (!pteBase || !NGGgtt::nativePhysicalRange(dummyPage, 0x1000)) {
		vfMarkProtocolFault("invalid VF dummy GGTT PTE aperture or DMA address");
		return false;
	}
	const uint64_t dummyPte = NGVfGgttPte::encodeSystemMemory(dummyPage);
	const uint64_t end = range.start + range.length;
	for (uint64_t gpu = range.start; gpu < end; gpu += 0x1000ULL) {
		if (gpu == range.start)
			vfStoreFirstPteWithDiagnostic(
				pteBase, gpu, dummyPage, dummyPte,
				VfGgttDiagnosticOperation::DummyMap, range.length);
		else
			pteBase[gpu >> 12] = dummyPte;
	}
	vfRequireCompletedGgttUpdate();
	return true;
}

static void vfRequireCompletedPpgttUpdate()
{
	// Before CTB enable no GPU work could have consumed a translation. After
	// device-wide shutdown all contexts and both TLB targets are already
	// retired. Every live interval in between requires the engine target, not
	// the GuC-internal target used for direct GGTT updates.
	OSSynchronizeIO();
	if (!gVfCtbEverEnabled || gVfDmaQuiesced)
		return;
	PANIC_COND(!gVfHardwareGuc || !vfInvalidateTLBSync(
		gVfHardwareGuc, NGVfGuCRequest::TlbTarget::Engines),
		"ngreen", "VF PPGTT update did not retire engine translations");
}

void TglAccel::vfPpgtt32UnmapRange(void *that,
	                             const NGIGAddressRange &range)
{
	PANIC_COND(!that || !callback->oVfPpgtt32UnmapRange,
		"ngreen", "Missing native VF 32-bit PPGTT unmap boundary");
	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF 32-bit PPGTT unmap cannot enter the page-table transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	FunctionCast(vfPpgtt32UnmapRange,
	             callback->oVfPpgtt32UnmapRange)(that, range);
	// This native body only installs dummy leaf PTEs. Confirm their engine-wide
	// visibility before releaseRange can return and free the mapped backing.
	vfRequireCompletedPpgttUpdate();
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
}

void TglAccel::vfPpgtt64UnmapRange(void *that,
	                             const NGIGAddressRange &range)
{
	PANIC_COND(!that || !callback->oVfPpgtt64UnmapRange,
		"ngreen", "Missing native VF 64-bit PPGTT unmap boundary");
	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF 64-bit PPGTT unmap cannot enter the page-table transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	// The original tail enters the separately routed shrink boundary. Keeping
	// this outer recursive hold prevents another mapper/task/pool operation from
	// observing the dummy-PTE prefix before pruning and retirement complete.
	FunctionCast(vfPpgtt64UnmapRange,
	             callback->oVfPpgtt64UnmapRange)(that, range);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
}

void TglAccel::vfPpgtt64ShrinkRange(void *that,
	                              const NGIGAddressRange &range)
{
	PANIC_COND(!that || !callback->oVfPpgtt64ShrinkRange,
		"ngreen", "Missing native VF 64-bit PPGTT shrink boundary");
	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF 64-bit PPGTT shrink cannot enter the page-table transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	// Every pinned caller has already finished its hardware-entry writes. The
	// native shrink helpers may release a descriptor into PagePool, whose final
	// return zeroes the CPU page before taking the optional pool lock. Retire
	// engine translations first, then allow that irreversible page return.
	vfRequireCompletedPpgttUpdate();
	FunctionCast(vfPpgtt64ShrinkRange,
	             callback->oVfPpgtt64ShrinkRange)(that, range);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
}

bool TglAccel::vfCommitPageTablesForTask(void *that, void *task, void *mapping)
{
	VfSlowSubmitLog slow("page-table commit");
	PANIC_COND(!that || !task || !mapping ||
	           !callback->oVfCommitPageTablesForTask ||
	           !callback->oVfReleasePageTablesForTask,
		"ngreen", "Missing VF page-table commit/rollback boundary");
	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF page-table commit cannot enter the page-table transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	vfDeferGgttMaps();
	const bool committed = FunctionCast(
		vfCommitPageTablesForTask,
		callback->oVfCommitPageTablesForTask)(that, task, mapping);
	vfFlushGgttMaps();
	if (committed) {
		IORecursiveLockUnlock(gVfPageTableUpdateLock);
		return true;
	}

	// The native manager ANDs every table result without rolling back an
	// earlier successful table or segment. Its false result prevents the
	// inherited mapping from publishing installed-PTE ownership, so returning
	// directly would let later backing cleanup skip release_pte. Use the paired
	// native release fan-out before leaving this still-owned call frame. The
	// routed GGTT and PPGTT unmap boundaries synchronously retire the applicable
	// translation target before any native descriptor/backing release.
	const bool rolledBack = FunctionCast(
		vfCommitPageTablesForTask,
		callback->oVfReleasePageTablesForTask)(that, task, mapping);
	PANIC_COND(!rolledBack, "ngreen",
		"VF partial page-table commit could not be rolled back safely");
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
	return false;
}

bool TglAccel::vfReleasePageTablesForTask(void *that, void *task, void *mapping)
{
	PANIC_COND(!that || !task || !mapping ||
	           !callback->oVfReleasePageTablesForTask,
		"ngreen", "Missing native VF page-table release boundary");
	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF page-table release cannot enter the page-table transaction");
	const uint64_t start = mach_absolute_time();
	const uint32_t invalidations = gVfTlbInvalidations;
	IORecursiveLockLock(gVfPageTableUpdateLock);
	const bool released = FunctionCast(
		vfReleasePageTablesForTask,
		callback->oVfReleasePageTablesForTask)(that, task, mapping);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	static uint32_t logs = 0;
	if (ns >= 1000000 && logs++ < 200)
		SYSLOG("ngreen", "V356: page-table release took %llu us, %u TLB invalidations", ns / 1000,
		       gVfTlbInvalidations - invalidations);
	return released;
}

bool TglAccel::vfUpdatePageTablesForTask(void *that, void *task, void *mapping)
{
	VfSlowSubmitLog slow("page-table update");
	PANIC_COND(!that || !task || !mapping ||
	           !callback->oVfUpdatePageTablesForTask,
		"ngreen", "Missing native VF page-table update boundary");
	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF page-table update cannot enter the page-table transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	vfDeferGgttMaps();
	const bool updated = FunctionCast(
		vfUpdatePageTablesForTask,
		callback->oVfUpdatePageTablesForTask)(that, task, mapping);
	vfFlushGgttMaps();
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
	return updated;
}

void *TglAccel::vfNewPageTableForTask(void *that, void *task)
{
	PANIC_COND(!that || !task || !callback ||
	           !callback->oVfNewPageTableForTask ||
	           !callback->vfGetHardwareContextAddressMode ||
	           !callback->vfPpgtt32WithOptions ||
	           !callback->vfPpgtt64WithOptions ||
	           !callback->vfFlushHardwareAfterGttUpdate,
		"ngreen", "Missing VF initial page-table synchronization boundary");
	if (gVfIdentity != VfIdentity::Virtual)
		return FunctionCast(vfNewPageTableForTask,
		                    callback->oVfNewPageTableForTask)(that, task);
	SYSLOG("ngreen", "V312: entering initial VF page-table factory task=%p manager=%p",
	       task, that);

	if (!vfEnsurePageTableUpdateLock()) {
		SYSLOG("ngreen", "V282: cannot serialize initial VF page-table synchronization");
		return nullptr;
	}
	IORecursiveLockLock(gVfPageTableUpdateLock);

	auto *accelerator = getMember<void *>(that, 0x10);
	using GetAddressMode = uint32_t (*)(const void *);
	const uint32_t addressMode = reinterpret_cast<GetAddressMode>(
		callback->vfGetHardwareContextAddressMode)(task);
	using WithOptions = void *(*)(void *, void *);
	void *pageTable = nullptr;
	if (accelerator && addressMode == 1)
		pageTable = reinterpret_cast<WithOptions>(
			callback->vfPpgtt32WithOptions)(accelerator, task);
	else if (accelerator && addressMode == 3)
		pageTable = reinterpret_cast<WithOptions>(
			callback->vfPpgtt64WithOptions)(accelerator, task);
	SYSLOG("ngreen", "V312: initial VF page-table factory returned table=%p mode=%u",
	       pageTable, addressMode);
	if (!pageTable) {
		IORecursiveLockUnlock(gVfPageTableUpdateLock);
		return nullptr;
	}

	void *source = nullptr;
	const bool kernelTask = IGAccelTaskIsKernelGPUTask(task);
	if (kernelTask) {
		source = getMember<void *>(that, 0x98);
	} else if (accelerator) {
		auto *bootstrapTask = getMember<void *>(accelerator, 0x150);
		if (bootstrapTask)
			source = getMember<void *>(bootstrapTask, 0x260);
	}

	auto *destinationVtable =
		*reinterpret_cast<mach_vm_address_t **>(pageTable);
	auto *sourceVtable = source ?
		*reinterpret_cast<mach_vm_address_t **>(source) : nullptr;
	const NGIGAddressRange managerRange =
		getMember<NGIGAddressRange>(that, 0xA0);
	const bool sourceIsGlobal = source && source == gVfGlobalPageTable;
	const bool sourceOwnerValid = kernelTask ? sourceIsGlobal :
		(source && !sourceIsGlobal);
	const bool assignedRangeExact = managerRange.start == gVfGGTTBase &&
		managerRange.length == gVfGGTTSize && gVfGGTTSize != 0;
	const bool rangeValid =
		(managerRange.start & (PAGE_SIZE - 1U)) == 0 &&
		(managerRange.length & (PAGE_SIZE - 1U)) == 0 &&
		managerRange.length <= UINT64_MAX - managerRange.start;
	bool synchronized = destinationVtable && sourceVtable &&
		sourceOwnerValid && rangeValid;

	const bool destinationUsesDescriptors =
		(getMember<uint32_t>(pageTable, 0x28) & 1U) != 0;
	const bool sourceUsesDescriptors = source &&
		(getMember<uint32_t>(source, 0x28) & 1U) != 0;
	const bool destinationRootEmpty =
		addressMode == 3 &&
		getMember<uint64_t>(pageTable, 0x40) == 0 &&
		getMember<uint64_t>(pageTable, 0x48) == 0 &&
		getMember<uint64_t>(pageTable, 0x50) == 0 &&
		getMember<uint64_t>(pageTable, 0x58) == 0;
	const bool emptyKernelBootstrap = synchronized &&
		NGGgtt::emptyKernelPpgttBootstrap(
			kernelTask, sourceIsGlobal, assignedRangeExact, addressMode,
			destinationRootEmpty,
			gVfGgttDiagnosticWrites != 0);
	if (kernelTask) {
		// Native Tahoe copies every page in managerRange from the global GGTT.
		// On this VF that is almost one million uncached MMIO reads over PF owner
		// tombstones.  The exact factory has returned a rootless 64-bit table and
		// no direct GGTT operation has occurred, so the correct initial PPGTT is
		// empty.  Later real mappings use routed commit/all-task transactions.
		synchronized = emptyKernelBootstrap;
		if (synchronized)
			SYSLOG("ngreen", "V315: admitted empty initial kernel PPGTT before any direct GGTT mutation");
	} else if (synchronized && destinationUsesDescriptors &&
	    sourceUsesDescriptors) {
		// Match the native 64-bit-to-64-bit clone window exactly. A missing
		// source descriptor is a sparse success; a present descriptor must be
		// accepted by the destination or the unpublished table is discarded.
		const NGIGAddressRange descriptorRange {0, 0x40000000ULL};
		void *descriptor = nullptr;
		using ReadDescriptor = bool (*)(const void *,
		                                const NGIGAddressRange &, void **);
		using MapDescriptor = bool (*)(void *, const NGIGAddressRange &, void *);
		auto readDescriptor = reinterpret_cast<ReadDescriptor>(
			sourceVtable[0x168 / sizeof(mach_vm_address_t)]);
		auto mapDescriptor = reinterpret_cast<MapDescriptor>(
			destinationVtable[0x158 / sizeof(mach_vm_address_t)]);
		synchronized = readDescriptor && mapDescriptor;
		const bool descriptorRead = synchronized &&
			readDescriptor(source, descriptorRange, &descriptor);
		if (descriptorRead && descriptor)
			synchronized =
				mapDescriptor(pageTable, descriptorRange, descriptor);
		if (synchronized && !descriptor)
			SYSLOG("ngreen", "V326: admitted sparse initial private PPGTT descriptor read=%d",
			       descriptorRead);
	} else if (synchronized) {
		using ReadEntry = bool (*)(const void *, uint64_t, uint64_t &, uint64_t &);
		using MapEntry = bool (*)(void *, const NGIGAddressRange &,
		                         uint64_t, uint64_t);
		auto readEntry = reinterpret_cast<ReadEntry>(
			sourceVtable[0x140 / sizeof(mach_vm_address_t)]);
		auto mapEntry = reinterpret_cast<MapEntry>(
			destinationVtable[0x118 / sizeof(mach_vm_address_t)]);
		synchronized = readEntry && mapEntry;
		uint64_t mappedEntries = 0;
		const uint64_t end = managerRange.start + managerRange.length;
		for (uint64_t address = managerRange.start;
		     synchronized && address != end; address += PAGE_SIZE) {
			uint64_t physical = 0;
			uint64_t flags = 0;
			const bool present = readEntry(source, address, physical, flags);
			// A non-kernel task is admitted only with its private bootstrap table.
			// Address zero is corruption here; PF GGTT owner tombstones are handled
			// solely by the later common synchronizer.
			const auto entry = NGGgtt::classifySynchronizationEntry(
				present, false, physical, flags);
			if (!entry.valid) {
				synchronized = false;
				break;
			}
			if (!entry.map)
				continue;
			const NGIGAddressRange pageRange {address, PAGE_SIZE};
			synchronized = mapEntry(pageTable, pageRange, physical, entry.flags);
			if (synchronized)
				mappedEntries++;
		}
		// Native synchronizeEachEntry always records the deferred flush,
		// including empty and failed ranges. Preserve that ordering before an
		// unpublished partial hierarchy can be released.
		using FlushGtt = void (*)(void *);
		reinterpret_cast<FlushGtt>(
			callback->vfFlushHardwareAfterGttUpdate)(accelerator);
		SYSLOG("ngreen", "V315: initial private page-table entry copy mapped=%llu synchronized=%d",
		       static_cast<unsigned long long>(mappedEntries), synchronized);
	}

	if (!synchronized) {
		using Release = void (*)(void *);
		auto release = destinationVtable ?
			reinterpret_cast<Release>(
				destinationVtable[0x28 / sizeof(mach_vm_address_t)]) : nullptr;
		PANIC_COND(!release, "ngreen",
			"Cannot release failed unpublished VF page table");
		release(pageTable);
		pageTable = nullptr;
		SYSLOG("ngreen", "V282: rejected partial VF page-table synchronization");
	}

	SYSLOG("ngreen", "V312: initial VF page-table synchronization returned table=%p kernel=%d synchronized=%d",
	       pageTable, kernelTask, synchronized);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
	return pageTable;
}

void TglAccel::vfSynchronizeAllTasks(void *that)
{
	PANIC_COND(!that || !callback->oVfSynchronizeAllTasks ||
	           getMember<void *>(that, 0x10) != gVfAccelerator,
		"ngreen", "Missing or foreign VF all-task synchronization owner");
	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF all-task synchronization cannot enter the page-table transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	// The native iterator borrows raw tasks from accelerator +0xc48. Task
	// construction and final free now use this same recursive domain, so every
	// +0x260 table remains published for the complete native walk.
	FunctionCast(vfSynchronizeAllTasks,
	             callback->oVfSynchronizeAllTasks)(that);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
}

void TglAccel::vfSynchronizeEachEntry(void *that, const void *source,
	                                const NGIGAddressRange &range,
	                                bool remap)
{
	PANIC_COND(!that || !source || !callback->vfFlushHardwareAfterGttUpdate,
		"ngreen", "Missing VF entry synchronization owner");
	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF entry synchronization cannot enter the page-table transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);

	auto *destinationVtable = *reinterpret_cast<mach_vm_address_t **>(that);
	auto *sourceVtable = *reinterpret_cast<mach_vm_address_t *const *>(source);
	auto *accelerator = getMember<void *>(that, 0x10);
	const bool rangeValid =
		(range.start & (PAGE_SIZE - 1U)) == 0 &&
		(range.length & (PAGE_SIZE - 1U)) == 0 &&
		range.length <= UINT64_MAX - range.start;
	bool synchronized = destinationVtable && sourceVtable && rangeValid &&
		accelerator == gVfAccelerator &&
		getMember<void *>(const_cast<void *>(source), 0x10) == accelerator;

	using ReadEntry = bool (*)(const void *, uint64_t, uint64_t &, uint64_t &);
	using WriteEntry = bool (*)(void *, const NGIGAddressRange &,
	                           uint64_t, uint64_t);
	auto readEntry = synchronized ? reinterpret_cast<ReadEntry>(
		sourceVtable[0x140 / sizeof(mach_vm_address_t)]) : nullptr;
	auto mapEntry = synchronized ? reinterpret_cast<WriteEntry>(
		destinationVtable[0x118 / sizeof(mach_vm_address_t)]) : nullptr;
	auto remapEntry = synchronized && remap ? reinterpret_cast<WriteEntry>(
		destinationVtable[0x128 / sizeof(mach_vm_address_t)]) : nullptr;
	synchronized = readEntry && mapEntry && (!remap || remapEntry);
	const bool sourceIsGlobal = source == gVfGlobalPageTable;

	const uint64_t end = range.start + range.length;
	for (uint64_t address = range.start;
	     synchronized && address != end; address += PAGE_SIZE) {
		uint64_t physical = 0;
		uint64_t flags = 0;
		const bool present = readEntry(source, address, physical, flags);
		const auto entry = NGGgtt::classifySynchronizationEntry(
			present, sourceIsGlobal, physical, flags);
		if (!entry.valid) {
			synchronized = false;
			break;
		}
		if (!entry.map)
			continue;
		const NGIGAddressRange pageRange {address, PAGE_SIZE};
		// A one-page remap can fail only before its store when the destination
		// hierarchy is absent. Build that missing hierarchy with mapRange rather
		// than letting the native void helper publish a silent prefix.
		synchronized = remap && remapEntry ?
			remapEntry(that, pageRange, physical, entry.flags) :
			mapEntry(that, pageRange, physical, entry.flags);
		if (!synchronized && remap)
			synchronized = mapEntry(that, pageRange, physical, entry.flags);
	}

	using FlushGtt = void (*)(void *);
	reinterpret_cast<FlushGtt>(
		callback->vfFlushHardwareAfterGttUpdate)(accelerator);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
	if (!synchronized) {
		vfMarkProtocolFault("VF entry synchronization failed inside void ABI");
		PANIC_COND(true, "ngreen",
			"Refusing partially synchronized VF page tables");
	}
}

bool TglAccel::vfPpgtt64RemapDescriptor(void *that,
	                                  const NGIGAddressRange &range,
	                                  void *descriptor)
{
	PANIC_COND(!that || !descriptor || !callback->oVfPpgtt64RemapDescriptor ||
	           !callback->vfPageDescriptorRetain ||
	           !callback->vfPageDescriptorRelease,
		"ngreen", "Missing VF shared-descriptor remap boundary");
	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF shared-descriptor remap cannot enter the page-table transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);

	auto *vtable = *reinterpret_cast<mach_vm_address_t **>(that);
	using ReadDescriptor = bool (*)(const void *,
	                               const NGIGAddressRange &, void **);
	auto readDescriptor = vtable ? reinterpret_cast<ReadDescriptor>(
		vtable[0x168 / sizeof(mach_vm_address_t)]) : nullptr;
	void *oldDescriptor = nullptr;
	const bool rangeValid = range.length == 0x40000000ULL &&
		(range.start & (range.length - 1U)) == 0 &&
		range.length <= UINT64_MAX - range.start;
	const bool oldPresent = rangeValid && readDescriptor &&
		readDescriptor(that, range, &oldDescriptor) && oldDescriptor;
	PANIC_COND(!oldPresent, "ngreen",
		"VF descriptor remap has no stable old shared descriptor");

	using DescriptorRef = void (*)(void *);
	reinterpret_cast<DescriptorRef>(
		callback->vfPageDescriptorRetain)(oldDescriptor);
	const bool remapped = FunctionCast(
		vfPpgtt64RemapDescriptor,
		callback->oVfPpgtt64RemapDescriptor)(that, range, descriptor);
	if (!remapped) {
		// The admitted native body is pinned as an unconditional store/success.
		// Preserve the retained old backing if that contract is ever violated.
		vfMarkProtocolFault("VF shared-descriptor remap violated its pinned ABI");
		PANIC_COND(true, "ngreen",
			"Cannot recover an unknown shared-descriptor remap state");
	}

	// The extra reference keeps the old descriptor and its CPU page out of
	// PagePool while native code releases the destination reference first. The
	// new parent entry is already stored here; retire all prior engine walks
	// before the retained old page is allowed to reach zero/reuse.
	vfRequireCompletedPpgttUpdate();
	reinterpret_cast<DescriptorRef>(
		callback->vfPageDescriptorRelease)(oldDescriptor);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
	return true;
}

void *TglAccel::vfPagePoolAllocatePage(void *that)
{
	PANIC_COND(!vfAdmitPagePoolTransactionOwner(that) ||
	           !callback->oVfPagePoolAllocatePage ||
	           !vfEnsurePageTableUpdateLock(), "ngreen",
		"Missing VF PagePool allocation transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	auto *descriptor = FunctionCast(
		vfPagePoolAllocatePage, callback->oVfPagePoolAllocatePage)(that);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
	return descriptor;
}

void TglAccel::vfPagePoolReleasePage(void *that, const void *descriptor)
{
	PANIC_COND(!vfAdmitPagePoolTransactionOwner(that) || !descriptor ||
	           !callback->oVfPagePoolReleasePage ||
	           !vfEnsurePageTableUpdateLock(), "ngreen",
		"Missing VF PagePool return transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	FunctionCast(vfPagePoolReleasePage,
	             callback->oVfPagePoolReleasePage)(that, descriptor);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
}

void TglAccel::vfPagePoolPrune(void *that, uint32_t age)
{
	PANIC_COND(!vfAdmitPagePoolTransactionOwner(that) ||
	           !callback->oVfPagePoolPrune ||
	           !vfEnsurePageTableUpdateLock(), "ngreen",
		"Missing VF PagePool prune transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	FunctionCast(vfPagePoolPrune,
	             callback->oVfPagePoolPrune)(that, age);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
}

void TglAccel::vfPagePoolFree(void *that)
{
	PANIC_COND(!vfAdmitPagePoolTransactionOwner(that) ||
	           !callback->oVfPagePoolFree ||
	           !vfEnsurePageTableUpdateLock(), "ngreen",
		"Missing VF PagePool final-free transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	FunctionCast(vfPagePoolFree,
	             callback->oVfPagePoolFree)(that);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
}

void TglAccel::vfReleasePagePool(void *that)
{
	PANIC_COND(!that || !callback->oVfReleasePagePool ||
	           !vfEnsurePageTableUpdateLock(), "ngreen",
		"Missing VF manager PagePool teardown transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	FunctionCast(vfReleasePagePool,
	             callback->oVfReleasePagePool)(that);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
}

void TglAccel::vfMemoryManagerFree(void *that)
{
	PANIC_COND(!that || !callback->oVfMemoryManagerFree ||
	           !vfEnsurePageTableUpdateLock(), "ngreen",
		"Missing VF memory-manager final-free transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	// The native destructor clears the kernel/global table, segment ranges and
	// device memory before delegating to OSObject::free(). Holding the outermost
	// boundary prevents a display-mode synchronization or task transaction from
	// observing those fields halfway through teardown.
	FunctionCast(vfMemoryManagerFree,
	             callback->oVfMemoryManagerFree)(that);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
}

void TglAccel::vfUpdateMappingCacheType(void *that, uint32_t requestedType)
{
	VfSlowSubmitLog slow("mapping cache type");
	PANIC_COND(!that || !callback || !callback->oVfUpdateMappingCacheType,
		"ngreen", "Missing VF cache-type transaction boundary");
	if (gVfIdentity != VfIdentity::Virtual) {
		FunctionCast(vfUpdateMappingCacheType,
		             callback->oVfUpdateMappingCacheType)(that, requestedType);
		return;
	}

	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF cache-type update cannot acquire a sleepable transaction lock");
	IORecursiveLockLock(gVfPageTableUpdateLock);

	constexpr uint32_t cacheFlagMask = 3U << 25;
	const uint32_t oldResourceFlags = getMember<uint32_t>(that, 0x108);
	const uint32_t newResourceFlags =
		(oldResourceFlags & ~cacheFlagMask) |
		((requestedType & 3U) << 25);
	auto *mapping = getMember<void *>(that, 0x40);
	if (!mapping) {
		// Preserve native behavior for a resource whose mapping has not yet
		// been created: only its advertised low two cache bits change.
		getMember<uint32_t>(that, 0x108) = newResourceFlags;
		IORecursiveLockUnlock(gVfPageTableUpdateLock);
		return;
	}

	const uint32_t oldMappingType = getMember<uint32_t>(mapping, 0x114);
	if (oldMappingType == requestedType) {
		getMember<uint32_t>(that, 0x108) = newResourceFlags;
		IORecursiveLockUnlock(gVfPageTableUpdateLock);
		return;
	}
	const bool installed = (getMember<uint32_t>(mapping, 0x10) & 4U) != 0;
	if (!installed) {
		getMember<uint32_t>(that, 0x108) = newResourceFlags;
		getMember<uint32_t>(mapping, 0x114) = requestedType;
		IORecursiveLockUnlock(gVfPageTableUpdateLock);
		return;
	}

	auto *vtable = *reinterpret_cast<mach_vm_address_t **>(mapping);
	using UpdateGPUPageTable = bool (*)(void *);
	auto updateGPUPageTable = vtable ?
		reinterpret_cast<UpdateGPUPageTable>(vtable[0x180 / sizeof(mach_vm_address_t)]) :
		nullptr;
	const bool admitted = updateGPUPageTable && gVfGGTTReady &&
		!gVfSubmissionStopped && !gVfProtocolFault &&
		gVfAccelerator && getMember<void *>(mapping, 0x88) == gVfAccelerator &&
		(!gVfCtbEverEnabled || vfNativeGpuWorkReady());
	if (!admitted) {
		IORecursiveLockUnlock(gVfPageTableUpdateLock);
		vfMarkProtocolFault("installed VF cache-type update lacks stable owners or transport");
		PANIC_COND(true, "ngreen",
			"Refusing installed VF cache-type update before page-table mutation");
		return;
	}

	// Match the native order: software state changes before the installed
	// mapping is fanned out across every task page table.
	getMember<uint32_t>(that, 0x108) = newResourceFlags;
	getMember<uint32_t>(mapping, 0x114) = requestedType;
	if (updateGPUPageTable(mapping)) {
		IORecursiveLockUnlock(gVfPageTableUpdateLock);
		return;
	}

	// updatePageTableForTask and updateRange both preserve successful prefixes.
	// Under the native caller's existing ownership scope, replaying the same
	// range with the old type restores every entry written before the pinned
	// pre-write page-walk failure. Neither backing nor installed ownership may
	// be released by this void operation.
	getMember<uint32_t>(mapping, 0x114) = oldMappingType;
	getMember<uint32_t>(that, 0x108) = oldResourceFlags;
	const bool replayedOldType = updateGPUPageTable(mapping);
	OSSynchronizeIO();
	bool enginesRetired = !gVfCtbEverEnabled || gVfDmaQuiesced;
	bool gucRetired = enginesRetired;
	if (!enginesRetired && !gVfProtocolFault) {
		enginesRetired = gVfHardwareGuc && vfInvalidateTLBSync(
			gVfHardwareGuc, NGVfGuCRequest::TlbTarget::Engines);
		gucRetired = enginesRetired && vfInvalidateTLBSync(
			gVfHardwareGuc, NGVfGuCRequest::TlbTarget::Guc);
	}
	IORecursiveLockUnlock(gVfPageTableUpdateLock);

	SYSLOG("ngreen", "V281: cache-type update failed; old state replay=%d engines=%d guc=%d",
	       replayedOldType, enginesRetired, gucRetired);
	vfMarkProtocolFault("VF cache-type page-table update failed after rollback");
	PANIC_COND(true, "ngreen",
		"VF cache-type update failed; restored state cannot return through void ABI");
}

static void *vfRingVirtual(void *ring, size_t byteOffset)
{
	if (!ring || (byteOffset % sizeof(mach_vm_address_t)) != 0)
		return nullptr;
	auto *vtable = *reinterpret_cast<mach_vm_address_t **>(ring);
	if (!vtable)
		return nullptr;
	return reinterpret_cast<void *>(vtable[byteOffset / sizeof(mach_vm_address_t)]);
}

bool TglAccel::vfWaitForRingSpace(void *that, uint32_t requestedDwords)
{
	vfSlowMark(">rspace");
	PANIC_COND(!that || !callback->oVfWaitForRingSpace ||
	           gVfIdentity != VfIdentity::Virtual || gVfSubmissionStopped ||
	           gVfProtocolFault || !vfCanUseSleepingLock(),
		"ngreen", "Invalid VF ring reservation admission");
	auto *accelerator = getMember<void *>(that, 0x10);
	const uint8_t engine = getMember<uint8_t>(that, 0x40);
	const uint32_t cursor = getMember<uint32_t>(that, 0x64);
	const uint32_t ringBytes = getMember<uint32_t>(that, 0x8C);
	const uint32_t ringMask = getMember<uint32_t>(that, 0x90);
	PANIC_COND(!accelerator || !getMember<void *>(that, 0x18) ||
	           !getMember<void *>(that, 0x80) || engine >= 6 ||
	           ringBytes < 16 || ringMask != ringBytes - 1U ||
	           getMember<uint8_t>(that, 0x6D) != 0 ||
	           getMember<uint8_t>(that, 0x6E) != 0,
		"ngreen", "Malformed VF ring reservation state");

	const uint64_t engineMask = 1ULL << engine;
	const bool tlbPending =
		(getMember<uint64_t>(accelerator, 0x1340) & engineMask) != 0;
	const bool auxPending =
		(getMember<uint64_t>(accelerator, 0x1380) & engineMask) != 0;
	uint32_t flushTlbDwords = 0;
	if (tlbPending) {
		using GetFlushTLBSpace = uint32_t (*)(void *);
		auto getFlushTLBSpace = reinterpret_cast<GetFlushTLBSpace>(
			vfRingVirtual(that, 0x150));
		PANIC_COND(!getFlushTLBSpace, "ngreen",
			"Missing VF ring TLB reservation virtual");
		flushTlbDwords = getFlushTLBSpace(that);
		PANIC_COND(flushTlbDwords != 5 && flushTlbDwords != 6 &&
		           flushTlbDwords != 10 && flushTlbDwords != 12,
			"ngreen", "Unexpected VF ring TLB reservation size");
	}
	const bool extendedRenderTrailer = engine == 0 &&
		(getMember<uint64_t>(accelerator, 0x1190) & (1ULL << 14)) != 0;
	const auto callerRequest =
		NGVfSubmission::guardedRingCallerRequest(requestedDwords);
	PANIC_COND(!callerRequest.valid, "ngreen",
		"Overflowing VF ring caller reservation");
	const auto reservation = NGVfSubmission::ringReservation(
		callerRequest.dwords, ringBytes, cursor, extendedRenderTrailer,
		tlbPending, flushTlbDwords, auxPending);
	PANIC_COND(!reservation.valid, "ngreen",
		"Oversized or malformed VF ring reservation");

	// A hung render context (recovered only when GuC resets it, seconds
	// later) stops the engine; a busy client such as WindowServer then fills
	// its ring and Apple's wait gives up. Wait again a few times, then report
	// the failure to Apple's caller instead of panicking.
	bool nativeResult = false;
	vfSlowMark("rspace pre");
	for (uint32_t attempt = 0; attempt < 5 && !nativeResult; attempt++)
		nativeResult = FunctionCast(
			vfWaitForRingSpace,
			callback->oVfWaitForRingSpace)(that, callerRequest.dwords);
	vfSlowMark("rspace native");
	const uint32_t available = getMember<uint32_t>(that, 0x88);
	const uint32_t committedCursor = getMember<uint32_t>(that, 0x64);
	// Only Apple's own accounting counts: the modelled reservation above can
	// exceed what Apple reserves (it asked for 69 dwords with exactly 276
	// bytes free and was right to proceed).
	if (!nativeResult) {
		static uint32_t logs = 0;
		if (logs++ < 32)
			SYSLOG("ngreen", "V344: engine %u ring wait failed native=%d available=%u requested=%u cursor=0x%x",
			       engine, nativeResult, available, callerRequest.dwords, committedCursor);
		return false;
	}
	PANIC_COND(getMember<void *>(that, 0x10) != accelerator ||
	           getMember<uint8_t>(that, 0x40) != engine ||
	           getMember<uint32_t>(that, 0x8C) != ringBytes ||
	           getMember<uint32_t>(that, 0x90) != ringMask ||
	           committedCursor >= ringBytes || (committedCursor & 3U) != 0 ||
	           getMember<uint8_t>(that, 0x6D) != (tlbPending ? 1U : 0U) ||
	           getMember<uint8_t>(that, 0x6E) != (auxPending ? 1U : 0U),
		"ngreen", "VF ring reservation returned without proven capacity");
	vfSlowMark("rspace post");
	return true;
}

static bool vfPpgttTaskHasNoDirectContexts(void *task)
{
	if (!task)
		return false;
	if (!gVfContextLock && !gVfContexts && !gVfContextCapacity)
		return true;
	if (!gVfContextLock || !gVfContexts || !gVfContextCapacity)
		return false;

	bool empty = true;
	const IOInterruptState interruptState =
		IOSimpleLockLockDisableInterrupt(gVfContextLock);
	for (uint32_t id = 0; id < gVfContextCapacity; id++) {
		if (gVfContexts[id].task == task) {
			empty = false;
			break;
		}
	}
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
	return empty;
}

void TglAccel::vfAccelTaskFree(void *that)
{
	PANIC_COND(!that || !callback->oVfAccelTaskFree,
		"ngreen", "Missing native VF task-free boundary");
	if (gVfIdentity != VfIdentity::Virtual) {
		FunctionCast(vfAccelTaskFree, callback->oVfAccelTaskFree)(that);
		return;
	}
	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF task free cannot enter the page-table transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	// Native final free releases the Aux table and private PPGTT before its
	// inherited owner. Ordinary hardware contexts retain the task; the special
	// task-owned contexts are released by IGAccelTask::release before free can
	// run. Our direct record independently proves that no GuC context still
	// names this task. With no producer left, the engine completion cannot be
	// followed by a refill from the soon-to-be-freed tables.
	OSSynchronizeIO();
	const bool ownsPageTable = getMember<void *>(that, 0x260) ||
	                           getMember<void *>(that, 0x278);
	if (ownsPageTable && gVfCtbEverEnabled && !gVfDmaQuiesced) {
		PANIC_COND(!vfPpgttTaskHasNoDirectContexts(that), "ngreen",
			"VF task reached page-table free with a live GuC context");
		vfRequireCompletedPpgttUpdate();
	}
	FunctionCast(vfAccelTaskFree, callback->oVfAccelTaskFree)(that);
	// The original Intel body delegates to inherited IOAccelTask::free before
	// returning, so accelerator-list unlink is complete before this transaction
	// is opened to synchronizeAllTasks or another task factory.
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
}
