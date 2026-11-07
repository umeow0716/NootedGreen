//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_vf_internal.hpp"

constexpr uint32_t kGen11SoftScratch0 = 0x190240;
constexpr uint32_t kGen11GucHostInterrupt = 0x1901f0;
constexpr uint32_t kGucSendTrigger = 1;

constexpr uint32_t kGucTypeBusy = 0x30000000U;

constexpr uint32_t kGucActionMatchVersion = 0x5500;
constexpr uint32_t kGucActionVfReset = 0x5507;
constexpr uint32_t kGucActionQuerySingleKlv = 0x5509;
constexpr uint32_t kGucActionHost2GucSelfCfg = 0x0508;
constexpr uint32_t kGucActionUpdateContextPolicies =
	NGVfGuCRequest::updateContextPolicies;
constexpr uint32_t kGucActionTlbInvalidation = NGVfGuCRequest::tlbInvalidation;
constexpr uint32_t kGucSelfCfgMemIrqStatusAddr = 0x0900;
constexpr uint32_t kGucSelfCfgMemIrqSourceAddr = 0x0901;
constexpr uint32_t kGucSelfCfgH2GCtbAddr = 0x0902;
constexpr uint32_t kGucSelfCfgH2GCtbDescAddr = 0x0903;
constexpr uint32_t kGucSelfCfgH2GCtbSize = 0x0904;
constexpr uint32_t kGucSelfCfgG2HCtbAddr = 0x0905;
constexpr uint32_t kGucSelfCfgG2HCtbDescAddr = 0x0906;
constexpr uint32_t kGucSelfCfgG2HCtbSize = 0x0907;
constexpr uint32_t kGucKlvGGTTStart = 0x0001;
constexpr uint32_t kGucKlvGGTTSize = 0x0002;
constexpr uint32_t kGucKlvNumContexts = 0x0004;
constexpr uint32_t kGucKlvNumDoorbells = 0x0006;
constexpr uint32_t kGucVfLatestMajor = 1;
constexpr uint32_t kGucVfLatestMinor = 1;

IOLock *gVfGucLock = nullptr;
IORecursiveLock *gVfPageTableUpdateLock = nullptr;
uint64_t gVfGGTTBase = 0;
uint64_t gVfGGTTSize = 0;
uint32_t gVfContextCount = 0;
uint32_t gVfDoorbellCount = 0;
void *gVfGlobalPageTable = nullptr;
void *gVfHardwareGuc = nullptr;
void *gVfAccelerator = nullptr;
// Tahoe's IntelAccelerator::stop releases and clears +0x1260 before invoking
// the inherited IOGraphicsAccelerator2::stop.  That inherited body then drops
// the kernel task, whose stamp/scratch mappings can still enter release_pte and
// resolve their IGMemoryManager through the now-cleared accelerator field.
// Keep exactly one retained manager across native stop and republish only for
// the synchronous inherited-stop extent.  These three fields are owned by the
// exactly-once accelerator-stop thread; the page-table recursive lock protects
// the temporary +0x1260 publication and every nested mapping/table destructor.
OSObject *gVfStopMemoryManager = nullptr;
void *gVfStopMemoryManagerAccelerator = nullptr;
IOThread gVfStopMemoryManagerThread = nullptr;
mach_vm_address_t gIOAccelCommandPoolInit = 0;
bool gVfGGTTReady = false;
NGVfRuntime::Registers gVfRuntimeRegisters = {};
NGVfRuntime::Topology gVfTopology = {};
volatile UInt32 gVfRuntimeReady = 0;
VfIdentity gVfIdentity = VfIdentity::Unknown;

VfIdentity vfIdentifyDevice()
{
	if (gVfIdentity != VfIdentity::Unknown)
		return gVfIdentity;
	auto *cb = NGreen::callback;
	if (!cb)
		return VfIdentity::Invalid;
	const auto capability = NGGpuCapabilities::sriov(cb->getOriginalDeviceId());
	if (capability == NGGpuCapabilities::Sriov::Absent) {
		gVfIdentity = VfIdentity::Physical;
		return gVfIdentity;
	}
	if (capability == NGGpuCapabilities::Sriov::Unknown) {
		// A not-yet-captured/unknown PCI ID is not evidence of a physical GPU.
		// Do not cache this, allowing identification after PCI discovery.
		return VfIdentity::Invalid;
	}
	if (!cb->setRMMIOIfNecessary())
		return VfIdentity::Invalid;
	constexpr uint32_t vfCap = 0x1901f8;
	if (!cb->getRMMIOAddress() || cb->getRMMIOLength() < vfCap + sizeof(uint32_t))
		return VfIdentity::Invalid;
	// Match i915 gen12_pci_capability_is_vf. Invalid BAR reads are neither
	// proof of a VF nor permission to run physical engine initialization.
	const uint32_t value = cb->getRMMIOAddress()[vfCap / sizeof(uint32_t)];
	if (value & ~1U)
		gVfIdentity = VfIdentity::Invalid;
	else
		gVfIdentity = value ? VfIdentity::Virtual : VfIdentity::Physical;
	SYSLOG("ngreen", "V239: VF_CAP=0x%08x identity=%u", value,
	       static_cast<unsigned int>(gVfIdentity));
	return gVfIdentity;
}
uint32_t gVfMmioFailureLogs = 0;
uint32_t gVfCtbGpuBase = 0;
uint8_t *gVfCtbCpuBase = nullptr;
OSObject *gVfCtbBacking = nullptr;
OSObject *gVfCtbObject = nullptr;
bool gVfCtbDisableConfirmed = false;
volatile UInt32 gVfCtbStopped = 0;
volatile UInt32 gVfCtbEnabled = 0;
volatile UInt32 gVfCtbEverEnabled = 0;
volatile UInt32 gVfSubmissionStopped = 0;
volatile UInt32 gVfIrqCallbackGate = 0;
volatile UInt32 gVfContextOperationGate = 0;
// Count every externally reachable VF resource producer before it can enter
// an inherited IOAccelerator mutex/wait scope.  The high bit closes admission;
// the low bits are invocation-lifetime leases, not GPU-completion references.
volatile UInt32 gVfExternalProducerGate = 0;
// Selector 3 (accelerator release) synchronously drops the last orphan-cache
// reference and therefore nests selector 4 (cache free). During the native
// finalize handler these are retirement callbacks, not new resource producers.
// Keep the dynamic extent thread-affine: a global depth allowed an unrelated
// callback thread to bypass the already-closed external-producer gate.
ThreadLocal<UInt32, 1> gVfDeviceCacheRetirementDepth {};
// Tahoe's finalize handler retires caches and can synchronously tail-call
// IOService::finalize, which re-enters the routed IntelAccelerator::stop. One
// atomic lifecycle owner coordinates that recursion and calls native stop once.
volatile UInt32 gVfAcceleratorStopPhase =
	NGVfAcceleratorStop::raw(NGVfAcceleratorStop::Phase::Idle);
volatile UInt32 gVfContextShutdownStarted = 0;
volatile UInt32 gVfContextShutdownComplete = 0;
volatile UInt32 gVfDmaQuiesced = 0;
volatile UInt32 gVfDeviceStopping = 0;
volatile UInt32 gVfSchedulerFirmwareReady = 0;
bool gVfUsesMemoryIrq = false;
bool gVfMemIrqConfigured = false;
volatile UInt32 gVfMemIrqRequested = 0;
volatile UInt32 gVfMmioIrqReady = 0;
volatile UInt32 gVfProtocolFault = 0;
bool gVfMmioPoisoned = false; // protected by gVfGucLock
// V312 diagnostic-only counter.  It bounds GGTT PTE round-trip logging across
// every direct-map entry point without changing the encoded value or retaining
// PF-owned ownership bits.  The diagnostic helper is used only for the first
// store of an operation; all remaining stores keep the established V219 path.
// Saturate after the bounded diagnostic window.  Nonzero is consequently a
// monotonic, non-wrapping proof that a routed direct GGTT store has occurred.
volatile UInt32 gVfGgttDiagnosticWrites = 0;
// V316 diagnostic-only task-construction tickets.  One ticket brackets one
// complete native substage; saturation keeps repeated task creation from
// turning a start-boundary probe into an unbounded kernel log source.
volatile UInt32 gVfTaskConstructionDiagnosticStages = 0;
volatile UInt32 gVfBootstrapStarted = 0;
volatile UInt32 gVfTlbNextSeqno = 0;
volatile UInt32 gVfTlbWaitActive = 0;
volatile UInt32 gVfTlbWaitSeqno = 0;
volatile UInt32 gVfTlbDoneSeqno = 0;
// Match i915: keep 1/4 of the 16 KiB receive ring for unsolicited events,
// plus its empty/full sentinel. Counts include both CT and HXG headers.
constexpr uint32_t kVfG2HCreditCapacity = (0x4000 - 0x1000) / 4 - 1;
volatile UInt32 gVfG2HCreditsUsed = 0;

extern "C" __attribute__((noinline))
bool ngVfCommandPoolInitHelper(void *pool, void *accelerator, void *channel,
	void *task, uint32_t maximum, uint32_t bytes, const uint64_t *tail)
{
	PANIC_COND(!gIOAccelCommandPoolInit || !tail, "ngreen",
	           "Missing native command-pool init trampoline or stack arguments");
	if (pool && gVfIdentity == VfIdentity::Virtual && gVfAccelerator &&
	    accelerator == gVfAccelerator)
		getMember<uint64_t>(pool, NGIOAccelCommandPool::recordOffset) = 0;
	using Init = bool (*)(void *, void *, void *, void *, uint32_t, uint32_t,
	                      uint32_t, uint32_t, uint32_t, uint32_t);
	return reinterpret_cast<Init>(gIOAccelCommandPoolInit)(
		pool, accelerator, channel, task, maximum, bytes,
		static_cast<uint32_t>(tail[0]), static_cast<uint32_t>(tail[1]),
		static_cast<uint32_t>(tail[2]), static_cast<uint32_t>(tail[3]));
}

// Nonstandard return contract consumed only by the UUID-pinned VF constructor
// patch: preserve native AL, set ZF from it and restore task in RDI. All normal
// ABI callee-saved registers and the incoming ten-argument stack are preserved.
extern "C" __attribute__((naked, noinline))
bool ngVfCommandPoolInitBridge(void *, void *, void *, void *, uint32_t,
	uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{
	__asm__ volatile(
		"pushq %rbx\n\t"
		"subq $8, %rsp\n\t"
		"movq %rcx, %rbx\n\t"
		"leaq 24(%rsp), %rax\n\t"
		"pushq %rax\n\t"
		"callq _ngVfCommandPoolInitHelper\n\t"
		"addq $16, %rsp\n\t"
		"movq %rbx, %rdi\n\t"
		"popq %rbx\n\t"
		"testb %al, %al\n\t"
		"retq\n\t");
}

bool vfInterruptTransportReady()
{
	return gVfUsesMemoryIrq ?
		gVfMemIrqConfigured && gVfMemIrqRequested != 0 :
		gVfMmioIrqReady != 0;
}

bool vfNativeGpuWorkReady()
{
	// This is transport readiness, not an external-user admission or owner-
	// lifetime lease. DeviceStopping deliberately does not close it: native
	// finishAllStamps may still need retirement submissions before engine stop.
	// The counted GuC operation gate covers attach/detach/submit only; it does
	// not drain native event-machine waiters or periodic timer callback owners.
	OSSynchronizeIO();
	return NGVfSubmission::ready({
		gVfIdentity == VfIdentity::Virtual,
		gVfGGTTReady,
		vfInterruptTransportReady(),
		gVfCtbCpuBase != nullptr,
		gVfCtbGpuBase != 0,
		gVfCtbEnabled != 0,
		gVfCtbStopped != 0,
		gVfSubmissionStopped != 0,
		gVfProtocolFault != 0,
	});
}

bool vfCtbConsumerReady(bool requireInterrupt)
{
	OSSynchronizeIO();
	return NGVfSubmission::consumerReady({
		gVfIdentity == VfIdentity::Virtual,
		gVfGGTTReady,
		vfInterruptTransportReady(),
		gVfCtbCpuBase != nullptr,
		gVfCtbGpuBase != 0,
		gVfCtbEnabled != 0,
		gVfCtbStopped != 0,
		gVfSubmissionStopped != 0,
		gVfProtocolFault != 0,
	}, requireInterrupt);
}

void vfMarkProtocolFault(const char *reason)
{
	// A terminal protocol fault must prevent any new user/control producer from
	// reaching native resource state.  Do not wait here: faults can be raised by
	// an admitted producer or interrupt callback which must first unwind its own
	// lease.  Final accelerator stop performs the bounded drain.
	for (;;) {
		OSSynchronizeIO();
		const UInt32 state = gVfExternalProducerGate;
		const UInt32 next = NGVfIrqGate::close(state);
		if (next == state ||
		    OSCompareAndSwap(state, next, &gVfExternalProducerGate))
			break;
	}
	OSSynchronizeIO();
	if (OSCompareAndSwap(0, 1, &gVfProtocolFault))
		SYSLOG("ngreen", "V237: VF protocol halted after %s", reason);
}

// Allocation/shape rejection while IGHardwareGuC::initWithOptions is still
// constructing scheduler-private state is not proof that CT transport is
// corrupt.  Stop every new producer, but deliberately keep the retirement
// transport usable: native init failure synchronously frees its unpublished
// GGTT mappings before returning to startGraphicsEngine(), which records the
// terminal protocol fault only after that teardown has completed.  Poisoning
// transport first would make the following void unmap unable to prove its TLB
// boundary and turn an ordinary bootstrap rejection into a guaranteed panic.
void vfAbortSchedulerBootstrap(const char *reason)
{
	const bool first = OSCompareAndSwap(0, 1, &gVfSubmissionStopped);
	OSSynchronizeIO();
	if (first)
		SYSLOG("ngreen", "V250: stopped VF scheduler bootstrap before safe native unwind: %s",
		       reason);
}

bool vfCanUseSleepingLock()
{
	if (ml_at_interrupt_context() || !preemption_enabled()) {
		vfMarkProtocolFault("blocking VF operation from non-preemptible context");
		return false;
	}
	return true;
}

bool vfReserveG2HCredits(uint32_t count)
{
	if (!count)
		return true;
	for (;;) {
		const UInt32 used = gVfG2HCreditsUsed;
		uint32_t next = 0;
		if (!NGGuCRing::reserveCredits(used, kVfG2HCreditCapacity, count, next))
			return false;
		if (OSCompareAndSwap(used, next, &gVfG2HCreditsUsed))
			return true;
	}
}

void vfReleaseG2HCredits(uint32_t count)
{
	for (;;) {
		const UInt32 used = gVfG2HCreditsUsed;
		uint32_t next = 0;
		if (!NGGuCRing::releaseCredits(used, kVfG2HCreditCapacity, count, next)) {
			vfMarkProtocolFault("unexpected G2H credit return");
			return;
		}
		if (OSCompareAndSwap(used, next, &gVfG2HCreditsUsed))
			return;
	}
}

// Tahoe's TGL binary assigns a GuC ID to a process-wide proxy work queue whose
// items contain changing LRCAs. A VF cannot use that legacy proxy: the PF-owned
// GuC accepts REGISTER_CONTEXT and schedules the registered LRCA directly.
// Keep a small lifecycle record keyed by context-image GGTT page and use the
// array index as the local GuC ID. The VF quota is a local zero-based namespace.
// A 16 KiB ring holds at most 819 five-dword stamp stores.
constexpr uint32_t kVfPendingStamps = 1024;

// V366: pending stamp storage for redirected contexts. With 32 entries per
// context, a command buffer of ~40+ encoders overflowed it, so semaphore
// waits on the lost stamps stayed on the shared slot, which only the CPU
// advances: the GPU stalled for seconds (or until a reset).
constexpr uint32_t kVfStampRedirectsMax = 128;
uint32_t gVfPendingStampPool[kVfStampRedirectsMax][kVfPendingStamps];
uint64_t gVfPendingStampPoolUsed[kVfStampRedirectsMax / 64];

// A retired or reused context stops being redirected (the merge drops it
// from gVfStampRedirects) and returns its pending stamp storage.
void vfEndStampRedirectLocked(VfGucContext &entry)
{
	entry.stampRedirected = false;
	if (entry.pendingStamps) {
		const auto pool = static_cast<uint32_t>((entry.pendingStamps - gVfPendingStampPool[0]) / kVfPendingStamps);
		gVfPendingStampPoolUsed[pool / 64] &= ~(1ULL << (pool % 64));
		entry.pendingStamps = nullptr;
	}
	entry.pendingStampHead = entry.pendingStampCount = 0;
	entry.pendingStampLost = false;
}

IOSimpleLock *gVfContextLock = nullptr;
VfGucContext *gVfContexts = nullptr;
uint32_t gVfContextCapacity = 0;
uint32_t gVfContextLifecycleLogs = 0;
NGVfTimeoutObservation::Reference gVfTimeoutReferences[NGVfTimeoutObservation::engineCount] {};

constexpr uint32_t kVfContextEventTimeoutMs = 1000;
constexpr uint32_t kVfCtbBackpressureTimeoutMs = 1000;
// MemoryInfo has a pinned two-second native busy-lock timeout.  Leave margin
// for a previously admitted caller to finish cleanup before teardown fails
// closed instead of releasing objects beneath it.
constexpr uint32_t kVfExternalProducerDrainTimeoutMs = 5000;
constexpr uint32_t kGucTypeFastRequest = 0x20000000U;
constexpr uint32_t kGucClientPriorityKmdNormal = 2;
constexpr uint32_t kGucPolicyExecutionQuantum = 0x2001;
constexpr uint32_t kGucPolicyPreemptionTimeout = 0x2002;
constexpr uint32_t kGucPolicySchedulingPriority = 0x2003;
constexpr uint32_t kGucPolicySlpmFrequency = 0x2005;

bool vfLegacyProxyPoolValid(void *that, mach_vm_address_t getter,
	                               uint32_t *usedOut,
	                               uint32_t *countOut) {
	if (!that || !getter ||
	    !getMember<IOLock *>(that, 0x40) || !getMember<void *>(that, 0x50))
		return false;
	auto *backing = getMember<void *>(that, 0x68);
	if (!backing)
		return false;
	using Getter = uint8_t *(*)(void *);
	auto *pool = reinterpret_cast<Getter>(getter)(backing);
	const uint32_t count = getMember<uint32_t>(that, 0x80);
	const uint32_t used = getMember<uint32_t>(that, 0x84);
	const uint32_t next = getMember<uint32_t>(that, 0x88);
	if (!NGContextPool::validStorage(pool,
	        getMember<uint64_t>(backing, kVfMappedBufferLengthOffset), count) ||
	    used > count || (next >= count && next != NGContextPool::invalidId))
		return false;
	if (usedOut)
		*usedOut = used;
	if (countOut)
		*countOut = count;
	return true;
}

uint32_t vfContextHash(uint32_t lrcaPage)
{
	return (lrcaPage >> 12) * 2654435761U;
}

int32_t vfFindContextLocked(uint32_t lrcaPage)
{
	if (!gVfContexts || !gVfContextCapacity)
		return -1;

	const uint32_t start = vfContextHash(lrcaPage) % gVfContextCapacity;
	for (uint32_t probe = 0; probe < gVfContextCapacity; probe++) {
		const uint32_t slot = (start + probe) % gVfContextCapacity;
		const auto &entry = gVfContexts[slot];
		if (entry.state == kVfGucContextEmpty)
			return -1;
		if ((entry.state != kVfGucContextTombstone || entry.task || entry.contextBacking ||
		     entry.ringBacking || entry.stampBacking || entry.scratchBacking ||
		     entry.submissionCoverage.owner) &&
		    entry.lrcaPage == lrcaPage)
			return static_cast<int32_t>(slot);
	}
	return -1;
}

int32_t vfReserveContextLocked(uint32_t lrcaPage)
{
	if (!gVfContexts || !gVfContextCapacity)
		return -1;

	const uint32_t start = vfContextHash(lrcaPage) % gVfContextCapacity;
	int32_t tombstone = -1;
	for (uint32_t probe = 0; probe < gVfContextCapacity; probe++) {
		const uint32_t slot = (start + probe) % gVfContextCapacity;
		const auto state = gVfContexts[slot].state;
		if (state == kVfGucContextTombstone &&
		    !gVfContexts[slot].task && !gVfContexts[slot].contextBacking &&
		    !gVfContexts[slot].ringBacking &&
		    !gVfContexts[slot].stampBacking && !gVfContexts[slot].scratchBacking &&
		    !gVfContexts[slot].submissionCoverage.owner && tombstone < 0)
			tombstone = static_cast<int32_t>(slot);
		if (state == kVfGucContextEmpty)
			return tombstone >= 0 ? tombstone : static_cast<int32_t>(slot);
	}
	return tombstone;
}

void vfReleaseRetiredContextBacking(uint16_t gucId)
{
	if (!gVfContextLock || !gVfContexts || gucId >= gVfContextCapacity)
		return;

	OSObject *backing = nullptr;
	OSObject *ringBacking = nullptr;
	OSObject *stampBacking = nullptr;
	OSObject *scratchBacking = nullptr;
	const IOInterruptState interruptState =
		IOSimpleLockLockDisableInterrupt(gVfContextLock);
	auto &entry = gVfContexts[gucId];
	if (!gVfProtocolFault && entry.state == kVfGucContextTombstone &&
	    entry.refCount == 0 && entry.submissionCoverage.resetForReuse()) {
		backing = entry.contextBacking;
		ringBacking = entry.ringBacking;
		stampBacking = entry.stampBacking;
		scratchBacking = entry.scratchBacking;
		entry.stampIndex = 0;
		vfEndStampRedirectLocked(entry);
		NGVfContextEvent::clearReleasedIdentity(entry);
	}
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
	if (ringBacking)
		ringBacking->release();
	if (stampBacking)
		stampBacking->release();
	if (scratchBacking)
		scratchBacking->release();
	if (backing)
		backing->release();
}

bool vfEnsureGucLock();
bool vfValidH2GMapping(const volatile uint32_t *descriptor,
                       const volatile uint32_t *buffer);

bool vfCaptureHardwareGuc(void *guc)
{
	if (!guc) {
		vfMarkProtocolFault("missing VF GuC object");
		return false;
	}
	if (!gVfHardwareGuc &&
	    !OSCompareAndSwapPtr(nullptr, guc, &gVfHardwareGuc)) {
		// Another caller published the object. Validate it below.
		OSSynchronizeIO();
	}
	if (gVfHardwareGuc != guc) {
		vfMarkProtocolFault("inconsistent VF GuC object identity");
		return false;
	}
	return true;
}

bool vfInitContextBridge()
{
	if (!gVfContextCount || gVfContextCount > 65535U)
		return false;
	if (!vfEnsureGucLock())
		return false;
	IOLockLock(gVfGucLock);
	if (gVfContexts && gVfContextLock && gVfContextCapacity) {
		IOLockUnlock(gVfGucLock);
		return true;
	}

	auto *lock = IOSimpleLockAlloc();
	auto *contexts = static_cast<VfGucContext *>(
		IOMallocZero(static_cast<size_t>(gVfContextCount) * sizeof(VfGucContext)));
	if (!lock || !contexts) {
		if (lock)
			IOSimpleLockFree(lock);
		if (contexts)
			IOFree(contexts,
			       static_cast<size_t>(gVfContextCount) * sizeof(VfGucContext));
		IOLockUnlock(gVfGucLock);
		return false;
	}

	gVfContextLock = lock;
	gVfContexts = contexts;
	gVfContextCapacity = gVfContextCount;
	for (auto &reference : gVfTimeoutReferences)
		reference = {};
	IOLockUnlock(gVfGucLock);
	SYSLOG("ngreen", "V230: initialized direct GuC context namespace with %u IDs",
	       gVfContextCapacity);
	return true;
}

bool vfSendCtbFastAction(void *guc, const uint32_t *request,
	                     uint32_t requestLength, uint32_t &transportFence,
	                     IOLock *alreadyHeldQueue,
	                     volatile uint32_t *contextTailField,
	                     uint32_t contextTail)
{
	transportFence = 0;
	if (!guc || !request || requestLength == 0 || requestLength > 31 ||
	    (request[0] & (kGucOriginGuc | kGucTypeMask)))
		return false;
	const auto attributes = NGVfGuCRequest::inspect(request, requestLength);
	if (!attributes.valid) {
		vfMarkProtocolFault("malformed or unsupported VF GuC FAST request");
		return false;
	}
	if (!vfCanUseSleepingLock())
		return false;
	const bool retirementAction = attributes.retirement;
	if (!gVfCtbGpuBase || !gVfCtbEnabled || gVfProtocolFault || gVfCtbStopped ||
	    (gVfSubmissionStopped && !retirementAction))
		return false;

	// Tahoe's legacy sender sleeps for a full timeout tick even when the G2H
	// response is already present and treats a successful non-zero HXG payload
	// as failure.  Modern i915 submits all context lifecycle actions as FAST
	// requests and relies on MODE_DONE/DEREGISTER_DONE for the operations that
	// complete asynchronously.  Write that native framing directly while using
	// Apple's H2G lock and fence counter so its remaining traffic stays ordered.
	auto *ctb = getMember<void *>(guc, 0xA10);
	if (!ctb)
		return false;
	auto *lock = getMember<IOLock *>(ctb, 0x18);
	auto *descriptor = getMember<volatile uint32_t *>(ctb, 0x48);
	auto *buffer = getMember<volatile uint32_t *>(ctb, 0x50);
	if (!lock || (alreadyHeldQueue && alreadyHeldQueue != lock) ||
	    !vfValidH2GMapping(descriptor, buffer)) {
		vfMarkProtocolFault("H2G CTB mapping mismatch");
		return false;
	}

	transportFence = 0;
	const uint32_t responseCredits = attributes.responseCredits;
	uint64_t deadline = 0;
	clock_interval_to_deadline(kVfCtbBackpressureTimeoutMs,
	                           kMillisecondScale, &deadline);
	for (;;) {
		if (!alreadyHeldQueue)
			IOLockLock(lock);
		OSSynchronizeIO();
		if (!gVfCtbEnabled || gVfCtbStopped || gVfProtocolFault ||
		    (gVfSubmissionStopped && !retirementAction)) {
			if (!alreadyHeldQueue)
				IOLockUnlock(lock);
			return false;
		}
		const uint32_t size = descriptor[3] / sizeof(uint32_t);
		const uint32_t head = descriptor[4];
		uint32_t tail = descriptor[5];
		const uint32_t status = descriptor[6];
		const uint32_t needed = requestLength + 1U;
		// The allocated H2G ring is exactly 4 KiB. Never let a corrupted
		// descriptor enlarge the bounds used for CPU writes.
		const bool valid = NGGuCRing::validDescriptor(descriptor[3], PAGE_SIZE,
		                                               head, tail, status) &&
		                   size > needed;
		const bool ringSpace = valid &&
			NGGuCRing::producerHasSpace(size, head, tail, needed);
		if (ringSpace && vfReserveG2HCredits(responseCredits)) {
			// A single-LRC tail must become visible in the context image at the
			// same publication point as its MODE_SET/SCHEDULE notification. Do not
			// expose it while waiting for ring space or on any failed enqueue.
			if (contextTailField) {
				*contextTailField = contextTail;
				OSSynchronizeIO();
			}
			const uint32_t fence = static_cast<uint32_t>(OSIncrementAtomic(
				reinterpret_cast<volatile SInt32 *>(
					reinterpret_cast<uint8_t *>(ctb) + 0x3C))) & 0xFFFFU;
			const uint32_t wireStart = tail;
			buffer[tail] = (fence << 16) | requestLength;
			tail = (tail + 1U) % size;
			buffer[tail] = kGucTypeFastRequest |
			               (request[0] & 0x0FFFFFFFU);
			tail = (tail + 1U) % size;
			for (uint32_t i = 1; i < requestLength; i++) {
				buffer[tail] = request[i];
				tail = (tail + 1U) % size;
			}
			OSSynchronizeIO();
			descriptor[5] = tail;
			OSSynchronizeIO();
			NGreen::callback->writeReg32(kGen11GucHostInterrupt,
			                              kGucSendTrigger);
			// The producer queue is still held, so another CPU producer cannot
			// reuse these slots during this bounded readback. GuC consumption,
			// PCIe delivery and GPU execution are NOT proved by CPU equality.
			// Only the already validated single-context MODE_SET/SCHEDULE path;
			// no extra MMIO reads, payload writes, retries or result changes.
			static uint32_t wireLogs = 0;
			if (contextTailField && (requestLength == 2U || requestLength == 3U) && wireLogs < 16U) {
				++wireLogs;
				const uint32_t raw0 = buffer[wireStart];
				const uint32_t raw1 = buffer[(wireStart + 1U) % size];
				const uint32_t raw2 = buffer[(wireStart + 2U) % size];
				const uint32_t raw3 = requestLength == 3U ? buffer[(wireStart + 3U) % size] : 0U;
				const bool matches = raw0 == ((fence << 16) | requestLength) &&
					raw1 == (kGucTypeFastRequest | (request[0] & 0x0FFFFFFFU)) &&
					raw2 == request[1] && (requestLength != 3U || raw3 == request[2]);
				SYSLOG("ngreen", "V380: H2G CPU readback fence=%u words=%u start=%u tail=%u lrc-tail=%x raw=%08x:%08x:%08x:%08x matches=%u gpu-proof=0",
				       fence,requestLength + 1U,wireStart,tail,contextTail,
				       raw0,raw1,raw2,raw3,matches?1U:0U);
			}
			if (!alreadyHeldQueue)
				IOLockUnlock(lock);
			transportFence = fence;
			return true;
		}
		if (!alreadyHeldQueue)
			IOLockUnlock(lock);
		if (!valid) {
			SYSLOG("ngreen", "V231: invalid H2G CTB size=%u head=%u tail=%u status=0x%x",
			       size, head, tail, status);
			vfMarkProtocolFault("invalid H2G CTB descriptor");
			return false;
		}
		uint64_t now = 0;
		clock_get_uptime(&now);
		if (now >= deadline) {
			SYSLOG("ngreen", "V244: H2G backpressure timeout size=%u head=%u tail=%u needed=%u ringSpace=%d credits=%u/%u",
			       size, head, tail, needed, ringSpace,
			       static_cast<unsigned int>(gVfG2HCreditsUsed),
			       kVfG2HCreditCapacity);
			vfMarkProtocolFault("H2G CTB backpressure timeout");
			return false;
		}
		// Tahoe's scheduler treats false as a fatal submit result. Unlike Linux
		// it has no tasklet retry path, so wait here while GuC consumes H2G and
		// the independent G2H callback returns reply credits. The entry guard
		// already proved this context may sleep.
		if (!Gen11::pollVfGuCToHost(guc))
			return false;
		IOSleep(1);
	}
}

bool vfSetContextPolicy(void *guc, uint16_t gucId, uint8_t engineClass,
	                    uint32_t &transportFence)
{
	// Normal KMD priority and a 1 ms execution quantum like i915. The engines
	// are shared with the host: a context that does not yield must be reset
	// by GuC within i915's default 640 ms (render batches are not always
	// preemptible within 50 ms), not after i915's
	// 7.5 s render timeout, which stalls the host long enough for a full GT
	// reset.
	(void)engineClass;
	const uint32_t preemptionTimeout = 640000U;
	const uint32_t request[] = {
		kGucActionUpdateContextPolicies,
		gucId,
		(kGucPolicySchedulingPriority << 16) | 1U,
		kGucClientPriorityKmdNormal,
		(kGucPolicyExecutionQuantum << 16) | 1U,
		1000U,
		(kGucPolicyPreemptionTimeout << 16) | 1U,
		preemptionTimeout,
		(kGucPolicySlpmFrequency << 16) | 1U,
		0U,
	};
	return vfSendCtbFastAction(guc, request, arrsize(request), transportFence);
}

bool vfCanWaitForGuc(void *guc)
{
	if (!vfCanUseSleepingLock())
		return false;
	// initWithOptions creates this independent workloop. Sleeping while its
	// gate is held prevents the software G2H event source from completing us.
	auto *loop = guc ? getMember<IOWorkLoop *>(guc, 0xA00) : nullptr;
	if (!loop || loop->onThread() || loop->inGate()) {
		vfMarkProtocolFault("synchronous GuC wait would block its completion workloop");
		return false;
	}
	return true;
}

bool vfEnterIrqCallback()
{
	for (;;) {
		OSSynchronizeIO();
		const UInt32 state = gVfIrqCallbackGate;
		UInt32 next = 0;
		if (!NGVfIrqGate::enter(state, next))
			return false;
		if (OSCompareAndSwap(state, next, &gVfIrqCallbackGate)) {
			OSSynchronizeIO();
			return true;
		}
	}
}

void vfLeaveIrqCallback()
{
	for (;;) {
		OSSynchronizeIO();
		const UInt32 state = gVfIrqCallbackGate;
		UInt32 next = 0;
		if (!NGVfIrqGate::leave(state, next)) {
			OSCompareAndSwap(0, 1, &gVfProtocolFault);
			OSSynchronizeIO();
			return;
		}
		if (OSCompareAndSwap(state, next, &gVfIrqCallbackGate)) {
			OSSynchronizeIO();
			return;
		}
	}
}

bool vfCloseIrqCallbackGateAndWait(void *guc)
{
	if (!vfCanWaitForGuc(guc))
		return false;
	for (;;) {
		OSSynchronizeIO();
		const UInt32 state = gVfIrqCallbackGate;
		const UInt32 next = NGVfIrqGate::close(state);
		if (next == state || OSCompareAndSwap(state, next, &gVfIrqCallbackGate))
			break;
	}
	OSSynchronizeIO();

	for (uint32_t waited = 0; waited < kVfContextEventTimeoutMs; waited++) {
		OSSynchronizeIO();
		if (NGVfIrqGate::drained(gVfIrqCallbackGate))
			return true;
		IOSleep(1);
	}
	vfMarkProtocolFault("timed out draining VF IRQ callbacks");
	return false;
}

bool vfEnterExternalProducer()
{
	for (;;) {
		OSSynchronizeIO();
		const UInt32 state = gVfExternalProducerGate;
		UInt32 next = 0;
		if (!NGVfIrqGate::enter(state, next)) {
			if (!NGVfIrqGate::closed(state))
				vfMarkProtocolFault("VF external-producer gate overflow");
			return false;
		}
		if (OSCompareAndSwap(state, next, &gVfExternalProducerGate)) {
			OSSynchronizeIO();
			return true;
		}
	}
}

void vfLeaveExternalProducer()
{
	for (;;) {
		OSSynchronizeIO();
		const UInt32 state = gVfExternalProducerGate;
		UInt32 next = 0;
		if (!NGVfIrqGate::leave(state, next)) {
			vfMarkProtocolFault("VF external-producer gate underflow");
			return;
		}
		if (OSCompareAndSwap(state, next, &gVfExternalProducerGate)) {
			OSSynchronizeIO();
			return;
		}
	}
}

bool vfOwnsDeviceCacheRetirement()
{
	auto *depth = gVfDeviceCacheRetirementDepth.get();
	return depth && *depth != 0;
}

NGVfAcceleratorStop::Phase vfAcceleratorStopPhase()
{
	OSSynchronizeIO();
	const auto phase = static_cast<NGVfAcceleratorStop::Phase>(
		gVfAcceleratorStopPhase);
	PANIC_COND(!NGVfAcceleratorStop::valid(phase), "ngreen",
		"Invalid VF accelerator stop phase %u", gVfAcceleratorStopPhase);
	return phase;
}

bool vfTransitionAcceleratorStop(
	NGVfAcceleratorStop::Phase expected,
	NGVfAcceleratorStop::Phase next)
{
	const bool changed = OSCompareAndSwap(
		NGVfAcceleratorStop::raw(expected),
		NGVfAcceleratorStop::raw(next), &gVfAcceleratorStopPhase);
	OSSynchronizeIO();
	return changed;
}

bool vfTargetsPublishedAccelerator(void *accelerator)
{
	OSSynchronizeIO();
	return gVfIdentity == VfIdentity::Virtual && accelerator &&
	       gVfAccelerator && accelerator == gVfAccelerator;
}

bool vfCloseExternalProducerGateAndWait()
{
	if (!vfCanUseSleepingLock())
		return false;
	for (;;) {
		OSSynchronizeIO();
		const UInt32 state = gVfExternalProducerGate;
		const UInt32 next = NGVfIrqGate::close(state);
		if (next == state ||
		    OSCompareAndSwap(state, next, &gVfExternalProducerGate))
			break;
	}
	OSSynchronizeIO();

	for (uint32_t waited = 0;
	     waited < kVfExternalProducerDrainTimeoutMs; waited++) {
		OSSynchronizeIO();
		if (NGVfIrqGate::drained(gVfExternalProducerGate))
			return true;
		IOSleep(1);
	}
	vfMarkProtocolFault("timed out draining VF external producers");
	return false;
}

bool vfEnterContextOperation()
{
	for (;;) {
		OSSynchronizeIO();
		const UInt32 state = gVfContextOperationGate;
		UInt32 next = 0;
		if (!NGVfIrqGate::enter(state, next))
			return false;
		if (OSCompareAndSwap(state, next, &gVfContextOperationGate)) {
			OSSynchronizeIO();
			return true;
		}
	}
}

void vfLeaveContextOperation()
{
	for (;;) {
		OSSynchronizeIO();
		const UInt32 state = gVfContextOperationGate;
		UInt32 next = 0;
		if (!NGVfIrqGate::leave(state, next)) {
			vfMarkProtocolFault("VF context-operation gate underflow");
			return;
		}
		if (OSCompareAndSwap(state, next, &gVfContextOperationGate)) {
			OSSynchronizeIO();
			return;
		}
	}
}

bool vfCloseContextOperationGateAndWait(void *guc)
{
	if (guc ? !vfCanWaitForGuc(guc) : !vfCanUseSleepingLock())
		return false;
	for (;;) {
		OSSynchronizeIO();
		const UInt32 state = gVfContextOperationGate;
		const UInt32 next = NGVfIrqGate::close(state);
		if (next == state ||
		    OSCompareAndSwap(state, next, &gVfContextOperationGate))
			break;
	}
	OSSynchronizeIO();

	for (uint32_t waited = 0; waited < kVfContextEventTimeoutMs; waited++) {
		OSSynchronizeIO();
		if (NGVfIrqGate::drained(gVfContextOperationGate))
			return true;
		IOSleep(1);
	}
	vfMarkProtocolFault("timed out draining VF context operations");
	return false;
}

bool vfWaitForContextShutdown(void *guc)
{
	OSSynchronizeIO();
	if (gVfContextShutdownComplete)
		return true;
	if (guc ? !vfCanWaitForGuc(guc) : !vfCanUseSleepingLock())
		return false;
	for (uint32_t waited = 0; waited < kVfContextEventTimeoutMs; waited++) {
		OSSynchronizeIO();
		if (gVfContextShutdownComplete)
			return true;
		IOSleep(1);
	}
	vfMarkProtocolFault("timed out waiting for VF context shutdown");
	return false;
}

volatile uint32_t gVfTlbInvalidations = 0;
bool vfInvalidateTLBSync(void *guc, NGVfGuCRequest::TlbTarget target)
{
	// GEN12_GUC_TLB_INV_CR (0xCEE8) belongs to the physical GT and is not in
	// a VF's runtime MMIO allowlist. i915 sends target GUC for internal/GGTT
	// translations and target ENGINES for the full engine/PPGTT barrier; both
	// use heavy mode with cache flush and wait for the matching G2H sequence.
	// Serialize requests so a single bounded waiter is sufficient and never
	// fall back to the physical register on failure.
	if (!vfCaptureHardwareGuc(guc))
		return false;
	if (!gVfGGTTReady || gVfProtocolFault || !vfInterruptTransportReady() ||
	    !gVfCtbGpuBase || !gVfCtbEnabled || gVfCtbStopped) {
		vfMarkProtocolFault("TLB invalidation before VF CTB/interrupt readiness");
		return false;
	}
	if (!vfCanWaitForGuc(guc))
		return false;
	if (!vfEnsureGucLock()) {
		vfMarkProtocolFault("TLB invalidation lock allocation failure");
		return false;
	}

	const uint64_t tlbStart = mach_absolute_time();
	vfSlowMark(">tlb");
	IOLockLock(gVfGucLock);
	const uint32_t seqno = static_cast<uint32_t>(OSIncrementAtomic(
		reinterpret_cast<volatile SInt32 *>(&gVfTlbNextSeqno)));
	gVfTlbDoneSeqno = seqno - 1U;
	gVfTlbWaitSeqno = seqno;
	gVfTlbWaitActive = 1;
	OSSynchronizeIO();

	const uint32_t request[] = {
		kGucActionTlbInvalidation,
		seqno,
		NGVfGuCRequest::tlbInvalidationControl(target),
	};
	uint32_t transportFence = 0;
	const bool sent = vfSendCtbFastAction(guc, request, arrsize(request),
	                                      transportFence);
	bool completed = false;
	if (sent) {
		// GuC answers within microseconds; spin for the first 2 ms before
		// sleeping, so frequent unmaps (video encode) do not pay a scheduler
		// tick per invalidation.
		for (uint32_t spins = 0, waited = 0; waited < kVfContextEventTimeoutMs;) {
			OSSynchronizeIO();
			if (gVfTlbDoneSeqno == seqno) {
				completed = true;
				break;
			}
			if (!Gen11::pollVfGuCToHost(guc))
				break;
			if (spins < 1000) {
				spins++;
				IODelay(2);
			} else {
				IOSleep(1);
				waited++;
			}
		}
	}
	gVfTlbWaitActive = 0;
	OSSynchronizeIO();
	IOLockUnlock(gVfGucLock);
	{
		uint64_t ns;
		absolutetime_to_nanoseconds(mach_absolute_time() - tlbStart, &ns);
		gVfTlbInvalidations++;
		vfSlowMark("tlb");
		static uint32_t logs = 0;
		if (ns >= 500000 && logs++ < 200)
			SYSLOG("ngreen", "V356: TLB invalidation target=%u took %llu us", static_cast<uint32_t>(target), ns / 1000);
	}

	if (!sent || !completed) {
		SYSLOG("ngreen", "V237: VF GuC TLB invalidate failed seq=%u target=%u sent=%d fence=%u",
		       seqno, static_cast<unsigned int>(target), sent, transportFence);
		vfMarkProtocolFault(sent ? "GuC TLB invalidation timeout" :
		                         "GuC TLB invalidation enqueue failure");
	}
	return sent && completed;
}

bool vfWaitForContextState(void *guc, uint16_t gucId, VfGucContextState wanted)
{
	if (!gVfContextLock || !gVfContexts || gucId >= gVfContextCapacity)
		return false;
	for (uint32_t waited = 0; waited < kVfContextEventTimeoutMs; waited++) {
		const IOInterruptState interruptState =
			IOSimpleLockLockDisableInterrupt(gVfContextLock);
		const auto state = gVfContexts[gucId].state;
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
		if (state == wanted)
			return true;
		if (!vfCanWaitForGuc(guc))
			return false;
		if (!Gen11::pollVfGuCToHost(guc))
			return false;
		IOSleep(1);
	}
	return false;
}

bool vfWaitForContextTransition(void *guc, uint16_t gucId, uint32_t lrcaPage,
	                            VfGucContextState previous)
{
	if (!gVfContextLock || !gVfContexts || gucId >= gVfContextCapacity)
		return false;
	for (uint32_t waited = 0; waited < kVfContextEventTimeoutMs; waited++) {
		const IOInterruptState interruptState =
			IOSimpleLockLockDisableInterrupt(gVfContextLock);
		const auto &entry = gVfContexts[gucId];
		const bool changed = entry.lrcaPage != lrcaPage ||
		                     entry.state != previous;
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
		if (changed)
			return true;
		if (!vfCanWaitForGuc(guc))
			return false;
		if (!Gen11::pollVfGuCToHost(guc))
			return false;
		IOSleep(1);
	}
	return false;
}

constexpr uint32_t kVfCtbModernDescOffset = 0x0010;
static_assert(kVfG2HCreditCapacity == (kVfCtbG2HBufferBytes * 3 / 4) / 4 - 1,
              "G2H reply credits must retain unsolicited-event headroom");
constexpr uint32_t kVfCtbUsedBytes = kVfCtbG2HBufferOffset + kVfCtbG2HBufferBytes;
constexpr uint32_t kVfMemIrqBytes = 0x1000;
constexpr uint32_t kVfMemIrqStatusOffset = NGVfMemIrq::statusOffset;
constexpr uint32_t kVfMemIrqSourceOffset = NGVfMemIrq::sourceOffset;
constexpr uint32_t kVfGucIrqOffset = NGVfMemIrq::gucIrqOffset;
static_assert(kVfMemIrqOffset == kVfCtbUsedBytes,
              "memory IRQ page must follow the CTB payload");
static_assert(kVfMemIrqOffset + kVfMemIrqBytes == kVfCtbBackingBytes,
              "memory IRQ page must fit the CTB allocation");
bool gVfCtbAllocationPending = false;
void *gVfCtbInitOwner = nullptr;
IOThread gVfCtbAllocationThread = nullptr;
OSObject *gVfCtbAllocatedBacking = nullptr; // borrowed during init only

bool vfValidH2GMapping(const volatile uint32_t *descriptor,
                       const volatile uint32_t *buffer)
{
	auto *base = gVfCtbCpuBase;
	return base && descriptor == reinterpret_cast<volatile uint32_t *>(
	                                  base + kVfCtbH2GDescOffset) &&
	       buffer == reinterpret_cast<volatile uint32_t *>(
	                     base + kVfCtbH2GBufferOffset);
}

bool vfValidG2HMapping(const volatile uint32_t *descriptor,
                       const volatile uint32_t *buffer)
{
	auto *base = gVfCtbCpuBase;
	return base && descriptor == reinterpret_cast<volatile uint32_t *>(
	                                  base + kVfCtbG2HDescOffset) &&
	       buffer == reinterpret_cast<volatile uint32_t *>(
	                     base + kVfCtbG2HBufferOffset);
}

bool vfStopSubmissionAndSealCtb(void *guc)
{
	if (!vfCanWaitForGuc(guc))
		return false;
	auto *ctb = guc ? getMember<void *>(guc, 0xA10) : nullptr;
	auto *h2gLock = ctb ? getMember<IOLock *>(ctb, 0x18) : nullptr;
	auto *g2hLock = ctb ? getMember<IOLock *>(ctb, 0x20) : nullptr;
	auto *h2gDescriptor = ctb ? getMember<volatile uint32_t *>(ctb, 0x48) : nullptr;
	auto *h2gBuffer = ctb ? getMember<volatile uint32_t *>(ctb, 0x50) : nullptr;
	auto *g2hDescriptor = ctb ? getMember<volatile uint32_t *>(ctb, 0x58) : nullptr;
	auto *g2hBuffer = ctb ? getMember<volatile uint32_t *>(ctb, 0x60) : nullptr;
	if (!h2gLock || !g2hLock || h2gLock == g2hLock ||
	    !vfValidH2GMapping(h2gDescriptor, h2gBuffer) ||
	    !vfValidG2HMapping(g2hDescriptor, g2hBuffer)) {
		vfMarkProtocolFault("cannot stop VF submission without both pinned CTB channels");
		return false;
	}

	// First reject new register/enable/schedule work. Disable, deregister and
	// TLB invalidation remain admissible so already-started teardown/unmap can
	// receive the G2H completions it needs before the transport is sealed.
	OSCompareAndSwap(0, 1, &gVfSubmissionStopped);
	OSSynchronizeIO();

	for (uint32_t waited = 0; waited < kVfContextEventTimeoutMs; waited++) {
		// Fixed H2G -> G2H order. The sender owns only H2G and the consumer
		// releases G2H before lifecycle handling, so no reverse edge exists.
		IOLockLock(h2gLock);
		IOLockLock(g2hLock);
		OSSynchronizeIO();
		const uint32_t h2gBytes = h2gDescriptor[3];
		const uint32_t h2gHead = h2gDescriptor[4];
		const uint32_t h2gTail = h2gDescriptor[5];
		const uint32_t h2gStatus = h2gDescriptor[6];
		const uint32_t g2hBytes = g2hDescriptor[3];
		const uint32_t g2hHead = g2hDescriptor[4];
		const uint32_t g2hTail = g2hDescriptor[5];
		const uint32_t g2hStatus = g2hDescriptor[6];
		const bool h2gValid = NGGuCRing::validDescriptor(
			h2gBytes, kVfCtbH2GBufferBytes, h2gHead, h2gTail, h2gStatus);
		const bool g2hValid = NGGuCRing::validDescriptor(
			g2hBytes, kVfCtbG2HBufferBytes, g2hHead, g2hTail, g2hStatus);
		const bool settled = h2gValid && g2hValid && h2gHead == h2gTail &&
			g2hHead == g2hTail &&
			gVfG2HCreditsUsed == 0 && gVfTlbWaitActive == 0;
		if (settled) {
			// Both native queue locks form the host-side linearization point:
			// no sender can publish and no already-queued G2H event can be hidden.
			OSCompareAndSwap(0, 1, &gVfCtbStopped);
			OSCompareAndSwap(1, 0, &gVfCtbEnabled);
			OSSynchronizeIO();
			IOLockUnlock(g2hLock);
			IOLockUnlock(h2gLock);
			return true;
		}
		IOLockUnlock(g2hLock);
		IOLockUnlock(h2gLock);

		if (!h2gValid || !g2hValid) {
			vfMarkProtocolFault("invalid CTB descriptor while draining VF transport");
			return false;
		}
		if (gVfProtocolFault)
			return false;
		if (!Gen11::pollVfGuCToHost(guc))
			return false;
		IOSleep(1);
	}

	vfMarkProtocolFault("timed out draining VF H2G producers/completions");
	return false;
}

bool vfRetireContextForShutdown(void *guc, uint16_t gucId)
{
	if (!gVfContextLock || !gVfContexts || gucId >= gVfContextCapacity)
		return false;

	// A slot needs at most MODE_DISABLE followed by DEREGISTER. Pending states
	// can add one wait-only pass when an operation admitted before gate closure
	// published its request immediately before shutdown.
	for (uint32_t pass = 0; pass < 6; pass++) {
		bool sendDisable = false;
		bool sendDeregister = false;
		bool waitOnly = false;
		uint32_t lrcaPage = 0;
		VfGucContextState state = kVfGucContextEmpty;

		const IOInterruptState interruptState =
			IOSimpleLockLockDisableInterrupt(gVfContextLock);
		auto &entry = gVfContexts[gucId];
		state = entry.state;
		lrcaPage = entry.lrcaPage;
		switch (NGVfContextShutdown::action(state)) {
			case NGVfContextShutdown::Action::Complete:
				break;
			case NGVfContextShutdown::Action::Deregister:
				entry.state = kVfGucContextPendingDeregister;
				sendDeregister = true;
				break;
			case NGVfContextShutdown::Action::Disable:
				entry.state = kVfGucContextPendingDisable;
				entry.disablePending = true;
				sendDisable = true;
				break;
			case NGVfContextShutdown::Action::Wait:
				waitOnly = true;
				break;
		}
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);

		if (state == kVfGucContextEmpty)
			return true;
		if (state == kVfGucContextTombstone) {
			vfReleaseRetiredContextBacking(gucId);
			return true;
		}

		uint32_t transportFence = 0;
		if (sendDisable) {
			const uint32_t disable[] = {
				kGucActionScheduleContextModeSet, gucId, kGucContextDisable,
			};
			if (!vfSendCtbFastAction(guc, disable, arrsize(disable),
			                         transportFence) ||
			    !vfWaitForContextState(guc, gucId,
			                           kVfGucContextDisabled)) {
				vfMarkProtocolFault("VF shutdown context-disable failure");
				return false;
			}
			continue;
		}
		if (sendDeregister) {
			const uint32_t deregister[] = {
				kGucActionDeregisterContext, gucId,
			};
			if (!vfSendCtbFastAction(guc, deregister,
			                         arrsize(deregister), transportFence) ||
			    !vfWaitForContextState(guc, gucId,
			                           kVfGucContextTombstone)) {
				vfMarkProtocolFault("VF shutdown context-deregister failure");
				return false;
			}
			continue;
		}
		if (waitOnly) {
			if (!vfWaitForContextTransition(guc, gucId, lrcaPage, state)) {
				vfMarkProtocolFault("VF shutdown context-transition timeout");
				return false;
			}
			continue;
		}
	}

	vfMarkProtocolFault("VF shutdown exceeded context transition bound");
	return false;
}

bool vfDirectContextTableUnowned()
{
	if (!gVfContextLock || !gVfContexts || !gVfContextCapacity)
		return false;
	// Callers close and drain the operation gate first. No table writer remains,
	// so avoid holding interrupts disabled across a quota-sized (57K here) scan.
	OSSynchronizeIO();
	for (uint32_t id = 0; id < gVfContextCapacity; id++) {
		const auto &entry = gVfContexts[id];
		if (entry.state != kVfGucContextEmpty || entry.task || entry.contextBacking || entry.ringBacking ||
		    entry.stampBacking || entry.scratchBacking ||
		    entry.refCount || entry.enablePending || entry.disablePending ||
		    entry.submissionCoverage.owner)
			return false;
	}
	return true;
}

bool vfRollbackFailedPostCtbBootstrap(void *guc)
{
	// A software allocation or its first synchronous GGTT invalidation can fail
	// after CTB enable but before any direct LRCA is registered with GuC. Once a
	// protocol fault is recorded, the normal CTB drain deliberately refuses to
	// trust ring state. The MMIO CONTROL_CTB acknowledgement is still an
	// independent firmware boundary: if the operation gate is drained and the
	// complete direct-context table is unowned, disabling CTB proves that the
	// only firmware-visible allocation (CTB, plus memory IRQ where supported)
	// has stopped DMA.
	// Apple-created proxy/workqueue buffers were never registered with GuC.
	OSSynchronizeIO();
	if (!guc || guc != gVfHardwareGuc || !gVfCtbEverEnabled ||
	    gVfSchedulerFirmwareReady ||
	    !gVfProtocolFault || gVfDmaQuiesced || gVfMmioPoisoned)
		return false;
	OSCompareAndSwap(0, 1, &gVfSubmissionStopped);
	OSSynchronizeIO();
	if (!vfCloseContextOperationGateAndWait(guc) ||
	    !vfDirectContextTableUnowned())
		return false;

	gVfMemIrqRequested = 0;
	auto *base = gVfCtbCpuBase;
	if (gVfUsesMemoryIrq && base) {
		*reinterpret_cast<volatile uint32_t *>(
			base + kVfMemIrqOffset + kVfMemIrqEnableOffset) = 0;
	}
	OSSynchronizeIO();
	if (!vfCloseIrqCallbackGateAndWait(guc))
		return false;

	uint32_t disable[4] = {kGucActionHost2GucControlCtb, 0, 0, 0};
	uint32_t reply[4] = {};
	if (!vfGucSendMMIO(disable, 2, reply) ||
	    !NGVfMmioResponse::noData(reply[0]))
		return false;

	gVfCtbDisableConfirmed = true;
	OSCompareAndSwap(0, 1, &gVfCtbStopped);
	OSCompareAndSwap(1, 0, &gVfCtbEnabled);
	OSCompareAndSwap(0, 1, &gVfDmaQuiesced);
	OSCompareAndSwap(0, 1, &gVfContextShutdownComplete);
	OSSynchronizeIO();
	SYSLOG("ngreen", "V248: safely disabled CTB after failed pre-submission VF bootstrap");
	return true;
}

bool vfQuiesceDeviceForShutdown(void *guc)
{
	OSSynchronizeIO();
	if (gVfContextShutdownComplete)
		return gVfDmaQuiesced != 0;
	if (!OSCompareAndSwap(0, 1, &gVfContextShutdownStarted))
		return vfWaitForContextShutdown(guc) && gVfDmaQuiesced;

	// Stop new Apple-facing producers, then close and drain the counted
	// attach/detach/submit gate. This makes the following context-table sweep a
	// complete snapshot rather than a best-effort scan of moving ownership.
	OSCompareAndSwap(0, 1, &gVfSubmissionStopped);
	OSSynchronizeIO();
	if (!vfCloseContextOperationGateAndWait(guc))
		return false;

	// A failed native start can call IntelAccelerator::stop before the GuC
	// scheduler has ever enabled CTB transport.  No GPU request or context
	// registration is admissible before that irreversible boundary, but prove
	// the context table is still completely unowned before declaring DMA idle.
	// This is the early-start rollback counterpart of the full disable /
	// deregister / TLB-invalidate sequence below; treating a missing GuC object
	// as an error here used to turn an ordinary start failure into a panic.
	if (!gVfCtbEverEnabled) {
		if (!gVfContextLock || !gVfContexts || !gVfContextCapacity) {
			vfMarkProtocolFault("pre-CTB rollback has no context ownership table");
			return false;
		}
		// The operation gate is closed and drained above. G2H is impossible
		// before CTB enable, so the shared complete-table proof is sufficient.
		if (!vfDirectContextTableUnowned()) {
			vfMarkProtocolFault("pre-CTB rollback found live context ownership");
			return false;
		}
		OSCompareAndSwap(0, 1, &gVfDmaQuiesced);
		OSSynchronizeIO();
		OSCompareAndSwap(0, 1, &gVfContextShutdownComplete);
		OSSynchronizeIO();
		SYSLOG("ngreen", "V246: quiesced early VF start rollback before CTB/GPU DMA admission");
		return true;
	}
	if (gVfProtocolFault)
		return false;

	for (uint32_t id = 0; id < gVfContextCapacity; id++) {
		if (!vfRetireContextForShutdown(guc, static_cast<uint16_t>(id)))
			return false;
	}

	// Once CTB has run, independently retire engine/PPGTT translations and the
	// GuC's internal/GGTT translations. All contexts are now deregistered and
	// the operation gate is closed, so both matching heavy-invalidation
	// completions form the device-wide DMA/translation boundary for every later
	// teardown unmap. Before CTB enable there cannot have been GPU work.
	if (gVfCtbEverEnabled &&
	    (!vfInvalidateTLBSync(guc, NGVfGuCRequest::TlbTarget::Engines) ||
	     !vfInvalidateTLBSync(guc, NGVfGuCRequest::TlbTarget::Guc)))
		return false;

	OSCompareAndSwap(0, 1, &gVfDmaQuiesced);
	OSSynchronizeIO();
	OSCompareAndSwap(0, 1, &gVfContextShutdownComplete);
	OSSynchronizeIO();
	SYSLOG("ngreen", "V242: VF contexts retired and DMA quiesced before CTB shutdown");
	return true;
}

// V324: a task's stamp slot is per channel and shared by all of the task's
// contexts on it. GuC runs each context's queued submissions back to back, so
// an older stamp can be stored after a newer one and the slot goes backwards:
// the newer event never completes (head == tail, stamp behind forever; an
// untimed finishEvent wait then wedges IOServiceTerminateThread and the whole
// session). Stamp stores are redirected to a per-context PPHWSP dword (the
// context's last completed stamp) and merged here: the shared slot advances to
// the newest completed stamp, but never past a busy context's own completed
// stamp, since that context's pending stamps are all newer than it.
constexpr uint32_t kVfPrivateStampOffset = 0xF00;
// Context ids with redirected stamps (the context table is GuC-id sized).
uint16_t gVfStampRedirects[kVfStampRedirectsMax];
uint32_t gVfStampRedirectCount = 0;
uint8_t *(*gVfStampGetter)(void *) = nullptr; // set with the first redirect

VfSlowSubmitThread gVfSlowSubmitThreads[32];

void vfSlowMark(const char *n) {
	const auto self = current_thread();
	for (auto &t : gVfSlowSubmitThreads)
		if (t.thread == self) {
			if (t.log)
				t.log->mark(n);
			return;
		}
}

// Native submit stages, marked into the running submission's slow log.
const char *const gVfSubmitProbeNames[][2] {{">pcb", "pcb"}, {">fifo", "fifo"}, {">coalesce", "coalesce"}};
mach_vm_address_t gVfSubmitProbeOrg[arrsize(gVfSubmitProbeNames)] {};

void vfMergeRedirectedStamps(bool dump) {
	auto getter = gVfStampGetter;
	if (!getter || !gVfContextLock || !gVfContexts)
		return;
	static struct { volatile uint32_t *shared; uint32_t done; bool busy; uint32_t limit, covered, head, tail, state, before; } c[arrsize(gVfStampRedirects)];
	const IOInterruptState saved = IOSimpleLockLockDisableInterrupt(gVfContextLock);
	uint32_t live = 0;
	for (uint32_t i = 0; i < gVfStampRedirectCount; i++) {
		const uint16_t id = gVfStampRedirects[i];
		const auto &entry = gVfContexts[id];
		if (!entry.stampRedirected)
			continue;
		gVfStampRedirects[live] = id;
		auto *stamps = entry.stampBacking ? getter(entry.stampBacking) : nullptr;
		auto *image = entry.contextBacking ? getter(entry.contextBacking) : nullptr;
		auto *own = image ? reinterpret_cast<volatile uint32_t *>(image + kVfPrivateStampOffset) : nullptr;
		if (own)
			__asm__ volatile("clflush (%0)\n\tmfence" :: "r"(own) : "memory");
		c[live].shared = stamps && own ? reinterpret_cast<volatile uint32_t *>(stamps + entry.stampIndex * 64U) : nullptr;
		c[live].done = own ? *own : 0;
		c[live].busy = entry.submissionCoverage.covered &&
			static_cast<int32_t>(entry.submissionCoverage.coveredStamp - c[live].done) > 0;
		// V346: everything below this context's oldest pending stamp is done,
		// whichever context it belonged to.
		auto &pending = gVfContexts[id];
		while (pending.pendingStampCount &&
		       static_cast<int32_t>(pending.pendingStamps[pending.pendingStampHead] - c[live].done) <= 0)
			pending.pendingStampHead = (pending.pendingStampHead + 1) % kVfPendingStamps, pending.pendingStampCount--;
		if (pending.pendingStampLost && static_cast<int32_t>(pending.pendingStampLostUpTo - c[live].done) <= 0)
			pending.pendingStampLost = false;
		c[live].limit = pending.pendingStampCount ? pending.pendingStamps[pending.pendingStampHead] - 1 : c[live].done;
		c[live].covered = entry.submissionCoverage.coveredStamp;
		c[live].head = image ? *reinterpret_cast<volatile uint32_t *>(image + 0x1014) : 0;
		c[live].tail = image ? *reinterpret_cast<volatile uint32_t *>(image + 0x101C) : 0;
		c[live].state = static_cast<uint32_t>(entry.state);
		c[live].before = c[live].shared ? *c[live].shared : 0;
		live++;
	}
	gVfStampRedirectCount = live;
	for (uint32_t i = 0; i < live; i++) {
		auto *shared = c[i].shared;
		bool first = shared != nullptr;
		for (uint32_t k = 0; first && k < i; k++)
			first = c[k].shared != shared;
		if (!first)
			continue;
		uint32_t newest = c[i].done, bound = 0;
		bool bounded = false;
		for (uint32_t k = i; k < live; k++) {
			if (c[k].shared != shared)
				continue;
			if (static_cast<int32_t>(c[k].done - newest) > 0)
				newest = c[k].done;
			if (c[k].busy && (!bounded || static_cast<int32_t>(bound - c[k].limit) > 0))
				bound = c[k].limit, bounded = true;
		}
		if (bounded && static_cast<int32_t>(newest - bound) > 0)
			newest = bound;
		if (static_cast<int32_t>(newest - *shared) > 0)
			*shared = newest;
	}
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, saved);
	// V345: who holds each shared slot back when an event times out.
	static uint32_t dumps = 0;
	if (dump && (++dumps <= 6 || (dumps % 8 == 0 && dumps < 512)))
		for (uint32_t i = 0; i < live; i++)
			if (c[i].shared && (c[i].busy || c[i].before != *c[i].shared || (c[i].head & 0x1FFFFCU) != (c[i].tail & 0x1FFFFCU)))
				SYSLOG("ngreen", "V345: #%u id=%u slot=%llx done=%x limit=%x covered=%x busy=%d shared %x->%x head=%x tail=%x state=%u",
				       dumps, gVfStampRedirects[i], reinterpret_cast<uint64_t>(c[i].shared), c[i].done, c[i].limit, c[i].covered,
				       c[i].busy, c[i].before, *c[i].shared, c[i].head, c[i].tail, c[i].state);
}

// Redirect the stamp stores (MI_FLUSH_DW or PIPE_CONTROL post-sync writes to
// the context's shared stamp slot) of newly written ring commands.
void vfRedirectStamps(uint32_t id, volatile uint32_t *ring, uint32_t ringSize, uint32_t from, uint32_t to,
                             uint8_t *(*getter)(void *), uint64_t (*getGpu)(void *)) {
	const uint32_t words = ringSize / 4;
	if (!getter || !getGpu || !ring || !words || (words & (words - 1)))
		return;
	gVfStampGetter = getter;
	auto &entry = gVfContexts[id];
	if (!entry.stampBacking || !entry.contextBacking)
		return;
	const uint64_t target = getGpu(entry.stampBacking) + entry.stampIndex * 64U;
	const uint32_t own = (entry.descriptorLo & ~0xFFFU) + kVfPrivateStampOffset;
	auto *image = getter(entry.contextBacking);
	auto *stamps = getter(entry.stampBacking);
	if (!target || !image || !stamps)
		return;
	if (!entry.stampRedirected) {
		const IOInterruptState saved = IOSimpleLockLockDisableInterrupt(gVfContextLock);
		uint32_t pool = 0;
		while (pool < kVfStampRedirectsMax && (gVfPendingStampPoolUsed[pool / 64] >> (pool % 64) & 1))
			pool++;
		if (gVfStampRedirectCount < kVfStampRedirectsMax && pool < kVfStampRedirectsMax) {
			gVfPendingStampPoolUsed[pool / 64] |= 1ULL << (pool % 64);
			entry.pendingStamps = gVfPendingStampPool[pool];
			auto *slot = reinterpret_cast<volatile uint32_t *>(image + kVfPrivateStampOffset);
			*slot = *reinterpret_cast<volatile uint32_t *>(stamps + entry.stampIndex * 64U);
			__asm__ volatile("clflush (%0)\n\tmfence" :: "r"(slot) : "memory");
			gVfStampRedirects[gVfStampRedirectCount++] = static_cast<uint16_t>(id);
			entry.stampRedirected = true;
		}
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, saved);
		if (!entry.stampRedirected)
			return;
	}
	static uint32_t logs[8] {};
	uint32_t values[104], count = 0; // the 512-dword scan holds at most 103 stores
	// V348: semaphore waits on one of this task's shared stamp slots.
	struct { uint32_t at, slot, value; } waits[16];
	uint32_t waitCount = 0;
	const uint64_t stampBase = target - entry.stampIndex * 64U;
	for (uint32_t i = from / 4 % words, end = to / 4 % words, guard = 0; i != end && guard < 512; i = (i + 1) & (words - 1), guard++) {
		auto at = [&](uint32_t k) -> volatile uint32_t & { return ring[(i + k) & (words - 1)]; };
		uint32_t length;
		if ((at(0) & 0xFF80C000U) == 0x13004000U && (at(1) | (static_cast<uint64_t>(at(2)) << 32)) == target) {
			at(1) = own | 4U; // MI_FLUSH_DW: GGTT
			at(2) = 0;
			values[count++] = at(3);
			length = 5;
		} else if ((at(0) & 0xFFFF00FFU) == 0x7A000004U && ((at(1) >> 14) & 3) == 1 && !(at(1) & (1U << 24)) &&
		           (at(2) | (static_cast<uint64_t>(at(3)) << 32)) == target) {
			at(1) |= 1U << 24; // PIPE_CONTROL: GGTT
			at(2) = own;
			at(3) = 0;
			values[count++] = at(4);
			length = 6;
		} else if ((at(0) & 0xFFBF7FFFU) == 0x0E001002U && waitCount < arrsize(waits)) {
			// MI_SEMAPHORE_WAIT, poll, SAD >= SDD, on a shared stamp slot.
			const uint64_t address = at(2) | (static_cast<uint64_t>(at(3)) << 32);
			if ((at(0) & (1U << 22)) || address < stampBase || address >= stampBase + 0x1000 || (address & 63))
				continue;
			waits[waitCount++] = {i, static_cast<uint32_t>((address - stampBase) / 64), at(1)};
			i = (i + 3) & (words - 1), guard += 3;
			continue;
		} else {
			continue;
		}
		for (uint32_t k = 1; k < 4; k++)
			__asm__ volatile("clflush (%0)" :: "r"(&at(k)) : "memory");
		__asm__ volatile("mfence" ::: "memory");
		if (logs[entry.engineClass & 7]++ < 3)
			SYSLOG("ngreen", "V324: id=%u class=%u stamp redirected to GGTT 0x%x", id, entry.engineClass, own);
		i = (i + length - 1) & (words - 1), guard += length - 1;
	}
	if (count) {
		const IOInterruptState saved = IOSimpleLockLockDisableInterrupt(gVfContextLock);
		constexpr uint32_t capacity = kVfPendingStamps;
		for (uint32_t k = 0; k < count; k++)
			if (!entry.pendingStampLost && entry.pendingStampCount < capacity)
				entry.pendingStamps[(entry.pendingStampHead + entry.pendingStampCount++) % capacity] = values[k];
			else
				entry.pendingStampLost = true, entry.pendingStampLostUpTo = values[k];
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, saved);
	}
	// V348: the shared slot only advances when the CPU merges, so a GPU waiting
	// on it can stall for as long as no merge happens. Stamps are monotonic
	// per context: wait on the private slot of the context that writes the
	// awaited stamp instead (or that already has).
	if (waitCount) {
		static uint32_t waitLogs = 0;
		const IOInterruptState saved = IOSimpleLockLockDisableInterrupt(gVfContextLock);
		for (uint32_t w = 0; w < waitCount; w++) {
			uint32_t producer = 0;
			bool found = false, lost = false;
			// The context that will write the awaited stamp; else, if nothing on
			// the slot lost track of its stamps, the stamp is already complete
			// and any context whose own stamp passed it can stand in.
			for (uint32_t pass = 0; pass < 2 && !found && !(pass && lost); pass++)
				for (uint32_t r = 0; r < gVfStampRedirectCount && !found; r++) {
					auto &other = gVfContexts[gVfStampRedirects[r]];
					if (!other.stampRedirected || other.stampBacking != entry.stampBacking ||
					    other.stampIndex != waits[w].slot || !other.contextBacking)
						continue;
					if (!pass) {
						lost |= other.pendingStampLost;
						const uint32_t first = other.pendingStampCount ? other.pendingStamps[other.pendingStampHead] : 0;
						const uint32_t last = other.pendingStampCount ?
							other.pendingStamps[(other.pendingStampHead + other.pendingStampCount - 1) % kVfPendingStamps] : 0;
						if (static_cast<int32_t>(waits[w].value - first) < 0 || static_cast<int32_t>(last - waits[w].value) < 0)
							continue;
						for (uint32_t q = 0; q < other.pendingStampCount && !found; q++)
							found = other.pendingStamps[(other.pendingStampHead + q) % kVfPendingStamps] == waits[w].value;
					} else {
						auto *otherImage = getter(other.contextBacking);
						found = otherImage && static_cast<int32_t>(*reinterpret_cast<volatile uint32_t *>(
							otherImage + kVfPrivateStampOffset) - waits[w].value) >= 0;
					}
					if (found)
						producer = (other.descriptorLo & ~0xFFFU) + kVfPrivateStampOffset;
				}
			waits[w].at |= found ? 0x80000000U : 0;
			waits[w].slot |= producer & ~0xFFFU;
			if (found) {
				auto at = [&](uint32_t k) -> volatile uint32_t & { return ring[((waits[w].at & 0xFFFFFFU) + k) & (words - 1)]; };
				at(2) = producer;
				at(3) = 0;
				at(0) |= 1U << 22; // GGTT
				for (uint32_t k = 0; k < 4; k++)
					__asm__ volatile("clflush (%0)" :: "r"(&at(k)) : "memory");
			}
		}
		__asm__ volatile("mfence" ::: "memory");
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, saved);
		for (uint32_t w = 0; w < waitCount && waitLogs < 32; w++, waitLogs++)
			SYSLOG("ngreen", "V348: id=%u waits slot %u >= %x -> %s 0x%x", id, waits[w].slot & 0xFFFU, waits[w].value,
			       (waits[w].at & 0x80000000U) ? "producer" : "shared", waits[w].slot & ~0xFFFU);
	}
}

// V330: after GuC resets a hung context nothing completes its submitted
// stamps, so every wait on that context (some untimed: finishEvent during
// teardown) blocks forever, and GuC replays the hung batch from the same head.
// Like i915's reset handling, move the ring head past the submitted work and
// complete its stamps; the hung work is dropped. GuC does not schedule the
// context again on its own, so the next submission re-enables it.
void vfSkipResetContext(uint32_t id, mach_vm_address_t getterAddress) {
	auto getter = reinterpret_cast<uint8_t *(*)(void *)>(getterAddress);
	if (!getter || !gVfContextLock || !gVfContexts || id >= gVfContextCapacity)
		return;
	uint32_t head = 0, tail = 0, stamp = 0;
	bool done = false;
	const IOInterruptState saved = IOSimpleLockLockDisableInterrupt(gVfContextLock);
	auto &entry = gVfContexts[id];
	auto *image = entry.contextBacking ? getter(entry.contextBacking) : nullptr;
	uint8_t *stamps = entry.stampBacking ? getter(entry.stampBacking) : nullptr;
	// V349: a context hung on its first run may have no coverage record yet;
	// skip it anyway (else it is never scheduled again and every wait on it
	// hangs), completing the newest stamp its redirected stores would write.
	if (image && stamps) {
		auto *ringHead = reinterpret_cast<volatile uint32_t *>(image + 0x1014);
		head = *ringHead, tail = *reinterpret_cast<volatile uint32_t *>(image + 0x101C);
		*ringHead = (head & ~0x1FFFFCU) | (tail & 0x1FFFFCU);
		auto *target = entry.stampRedirected ? reinterpret_cast<volatile uint32_t *>(image + kVfPrivateStampOffset) :
			reinterpret_cast<volatile uint32_t *>(stamps + entry.stampIndex * 64U);
		stamp = entry.submissionCoverage.covered ? entry.submissionCoverage.coveredStamp : *target;
		if (entry.pendingStampCount) {
			const uint32_t newest = entry.pendingStamps[(entry.pendingStampHead + entry.pendingStampCount - 1) % kVfPendingStamps];
			if (static_cast<int32_t>(newest - stamp) > 0)
				stamp = newest;
		}
		if (static_cast<int32_t>(stamp - *target) > 0)
			*target = stamp;
		__asm__ volatile("clflush (%0)\n\tclflush (%1)\n\tmfence" :: "r"(ringHead), "r"(target) : "memory");
		done = true;
		// GuC no longer schedules it: the next submission re-enables it.
		if (entry.state == kVfGucContextEnabled)
			entry.state = kVfGucContextDisabled;
	}
	const bool redirected = entry.stampRedirected;
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, saved);
	if (done)
		vfMergeRedirectedStamps();
	const uint32_t shared = stamps ? *reinterpret_cast<volatile uint32_t *>(stamps + entry.stampIndex * 64U) : 0;
	SYSLOG("ngreen", "V330: reset id=%u skipped head 0x%x -> tail 0x%x, stamp %x complete=%d redirected=%d shared %x",
	       id, head, tail, stamp, done, redirected, shared);
}

uint64_t vfConsumeMemoryInterrupts()
{
	auto *base = gVfCtbCpuBase;
	if (!gVfUsesMemoryIrq || !gVfMemIrqConfigured || !base ||
	    gVfCtbStopped || gVfProtocolFault)
		return 0;

	auto *page = reinterpret_cast<volatile uint8_t *>(
		base + kVfMemIrqOffset);
	auto *statusBase = page + kVfMemIrqStatusOffset;
	auto *sourceBase = page + kVfMemIrqSourceOffset;
	OSSynchronizeIO();

	uint64_t pending = 0;
	uint64_t sourceSnapshot = 0;
	for (uint32_t i = 0; i < 64; i++) {
		if (sourceBase[i])
			sourceSnapshot |= 1ULL << i;
	}

	// Match intel_iov_memirq_handler(): an engine source means that its user
	// interrupt is pending.  The status byte is diagnostic, not a second gate,
	// and Gen12 VF memory IRQs do not synthesize context-switch/error events.
	const auto consumeEngine = [&](const NGVfMemIrq::EngineRoute &route) {
		const uint32_t irqOffset = route.irqOffset;
		const uint8_t source = sourceBase[irqOffset];
		if (!source)
			return;
		sourceBase[irqOffset] = 0;
		auto *status = statusBase + irqOffset * NGVfMemIrq::statusStride;
		status[0] = 0;
		pending |= 1ULL << route.callbackBit;
	};

	// irq_offset values are the Gen11+ logical engine interrupt offsets used by
	// i915. The shared table binds them to Tahoe's corresponding callbacks.
	for (size_t i = 0; i < NGVfMemIrq::engineRouteCount; ++i)
		consumeEngine(NGVfMemIrq::engineRoutes[i]);

	const uint8_t gucSource = sourceBase[kVfGucIrqOffset];
	if (gucSource) {
		sourceBase[kVfGucIrqOffset] = 0;
		auto *gucStatus = statusBase +
			kVfGucIrqOffset * NGVfMemIrq::statusStride;
		// Linux dispatches G2H whenever the GuC source byte is asserted and
		// treats status[15] as a programming-note assertion only.
		gucStatus[15] = 0;
		pending |= 1ULL << 45;
		if (gucStatus[0]) {
			gucStatus[0] = 0;
			// Modern VF SW_INT_0 signals migration, not Apple's legacy GuC
			// software callback. Rebinding/restoring migrated state is not yet
			// implemented; stop submission rather than dispatching the wrong ABI.
			vfMarkProtocolFault("VF migration notification requires state reinitialization");
		}
	}
	OSSynchronizeIO();

	static uint32_t irqLogs = 0;
	if ((sourceSnapshot || pending) && irqLogs++ < 128) {
		SYSLOG("ngreen", "V236: VF memory IRQ source=0x%016llx pending=0x%016llx",
		       static_cast<unsigned long long>(sourceSnapshot),
		       static_cast<unsigned long long>(pending));
	}
	return pending;
}

bool vfG2HCtbPending(uint32_t &head)
{
	head = 0;
	auto *base = gVfCtbCpuBase;
	if (!base || gVfCtbStopped || gVfProtocolFault)
		return false;

	// Apple's +0x58 channel pointer is biased 0x10 bytes before the modern
	// descriptor.  Its legacy size/head/tail fields therefore alias the modern
	// descriptor window used by the runtime byte patches.
	auto *descriptor = reinterpret_cast<volatile uint32_t *>(
		base + kVfCtbG2HDescOffset);
	OSSynchronizeIO();
	const uint32_t bytes = descriptor[3];
	head = descriptor[4];
	const uint32_t tail = descriptor[5];
	const uint32_t status = descriptor[6];
	if (!NGGuCRing::validDescriptor(bytes, kVfCtbG2HBufferBytes, head, tail, status)) {
		vfMarkProtocolFault("invalid G2H descriptor while checking pending interrupts");
		return false;
	}
	return head != tail;
}

bool vfEnsureGucLock()
{
	if (!vfCanUseSleepingLock())
		return false;
	if (gVfGucLock)
		return true;
	auto *candidate = IOLockAlloc();
	if (!candidate)
		return false;
	// Publish one lifetime-long mailbox lock. Losing concurrent allocators
	// must use the published lock, not serialize on different instances.
	if (!OSCompareAndSwapPtr(nullptr, candidate, &gVfGucLock))
		IOLockFree(candidate);
	return true;
}

bool vfEnsurePageTableUpdateLock()
{
	if (!vfCanUseSleepingLock())
		return false;
	if (gVfPageTableUpdateLock)
		return true;
	auto *candidate = IORecursiveLockAlloc();
	if (!candidate)
		return false;
	// Page-table construction and failure unwind can re-enter task destruction,
	// while cache rollback and manager fan-out can re-enter lower table/pool
	// operations. Publish one lifetime-long recursive transaction lock so those
	// native ownership paths remain balanced without opening an interleaving
	// window to another task, table or PagePool operation.
	if (!OSCompareAndSwapPtr(nullptr, candidate, &gVfPageTableUpdateLock))
		IORecursiveLockFree(candidate);
	return true;
}

bool vfAdmitPagePoolTransactionOwner(void *pool)
{
	// The admitted Tahoe manager is the only direct PagePool factory caller and
	// passes options=0. Reject an unexpected threaded/callback-backed pool before
	// entering the sleepable transaction: its queued callback admission would
	// require a separate drain protocol around final free.
	return pool && gVfAccelerator &&
	       getMember<void *>(pool, 0x18) == gVfAccelerator &&
	       getMember<uint8_t>(pool, 0x64) == 0;
}

bool vfNativeCallbackBindingsReady(void *accelerator)
{
	if (!accelerator)
		return false;
	auto *workloop = getMember<IOWorkLoop *>(accelerator, 0xF0);
	auto *eventMachine = getMember<void *>(accelerator, 0x380);
	auto *scheduler = getMember<void *>(accelerator, 0x1250);
	auto *dpsmTimer = getMember<IOTimerEventSource *>(accelerator, 0x1460);
	if (!workloop || !eventMachine || !scheduler)
		return false;

	auto *fallback = getMember<IOInterruptEventSource *>(eventMachine, 0xD30);
	auto *periodicSet = getMember<OSSet *>(scheduler, 0x438);
	auto *periodicLock = getMember<IOLock *>(scheduler, 0x440);
	auto *periodicTimer = getMember<IOTimerEventSource *>(scheduler, 0x448);
	if (!fallback || !periodicSet || !periodicLock || !periodicTimer)
		return false;

	// No client can reach this unpublished accelerator yet, but native engine
	// bootstrap has already enabled its kernel-context stamps.  On the admitted
	// scheduler-4 path each active fallback stamp owns one bit in eventMachine
	// +0xd40..0xd7f, while the transition from zero to nonzero fallback users
	// registers the single +0xd30 source with the scheduler periodic set.  The
	// scheduler's unchecked +0x450 reference counter therefore equals the set
	// cardinality (zero or one) here; it does not equal the number of fallback
	// stamps.  The separate +0xd90 counter also covers hardware-backed stamp
	// paths and is valid while nonzero.  Empty collections are required only
	// after finishAllStamps() at the stop boundary below.
	IOLockLock(periodicLock);
	const auto fallbackUsers = getMember<uint64_t>(eventMachine, 0xD80);
	const auto secondaryStampUsers = getMember<uint64_t>(eventMachine, 0xD90);
	const auto periodicRefCount = getMember<uint64_t>(scheduler, 0x450);
	const auto periodicMemberCount = periodicSet->getCount();
	const bool fallbackRegistered = periodicSet->containsObject(fallback);
	uint64_t fallbackBitmapUsers = 0;
	for (size_t index = 0; index < 8; index++)
		fallbackBitmapUsers += __builtin_popcountll(
			getMember<uint64_t>(eventMachine, 0xD40 + index * sizeof(uint64_t)));
	IOLockUnlock(periodicLock);

	const bool fallbackActive = fallbackUsers != 0;
	const bool periodicOwnershipCoherent =
		fallbackUsers == fallbackBitmapUsers &&
		periodicRefCount == periodicMemberCount &&
		periodicRefCount <= 1 &&
		fallbackRegistered == fallbackActive &&
		periodicRefCount == (fallbackActive ? 1ULL : 0ULL);
	// Tahoe initializes +0x1460 to null, asks the power controller whether an
	// idle timer is needed, and branches directly past construction when that
	// answer is non-negative.  A null DPSM timer is therefore a complete native
	// state, not a missing callback owner.  If native start did create one, keep
	// requiring the exact accelerator workloop binding before publication.
	const bool dpsmBindingReady =
		!dpsmTimer || dpsmTimer->getWorkLoop() == workloop;
	const bool sourceBindingsReady =
		getMember<uint8_t>(eventMachine, 0xD88) == 1 &&
		fallback->getWorkLoop() == workloop &&
		periodicTimer->getWorkLoop() == workloop &&
		dpsmBindingReady;
	SYSLOG("ngreen", "V325: native callback ownership fallback=%llu bitmap=%llu secondary=%llu periodicRefs=%llu periodicMembers=%u registered=%d coherent=%d bindings=%d dpsm=%d",
	       fallbackUsers, fallbackBitmapUsers, secondaryStampUsers, periodicRefCount,
	       periodicMemberCount, fallbackRegistered, periodicOwnershipCoherent,
	       sourceBindingsReady, dpsmTimer != nullptr);
	return periodicOwnershipCoherent && sourceBindingsReady;
}

bool vfDetachNativeCallbackSourcesBeforeBaseStop(void *accelerator)
{
	if (!accelerator)
		return false;
	auto *workloop = getMember<IOWorkLoop *>(accelerator, 0xF0);
	auto *eventMachine = getMember<void *>(accelerator, 0x380);
	auto *scheduler = getMember<void *>(accelerator, 0x1250);
	if (!workloop || !eventMachine || !scheduler ||
	    getMember<IOTimerEventSource *>(accelerator, 0x1460) != nullptr)
		return false;

	auto *fallback = getMember<IOInterruptEventSource *>(eventMachine, 0xD30);
	auto *periodicSet = getMember<OSSet *>(scheduler, 0x438);
	auto *periodicLock = getMember<IOLock *>(scheduler, 0x440);
	auto *periodicTimer = getMember<IOTimerEventSource *>(scheduler, 0x448);
	if (!fallback || !periodicSet || !periodicLock || !periodicTimer)
		return false;

	// IntelAccelerator::stop has already executed finishAllStamps(43) at this
	// boundary.  Stabilize the periodic collection under its native mutex, but
	// never hold that mutex across removeEventSource(): passive timer callbacks
	// enter the workloop gate before acquiring scheduler+0x440.
	IOLockLock(periodicLock);
	const bool callbacksEmpty =
		getMember<uint64_t>(eventMachine, 0xD80) == 0 &&
		getMember<uint64_t>(eventMachine, 0xD90) == 0 &&
		getMember<uint64_t>(scheduler, 0x450) == 0 &&
		periodicSet->getCount() == 0;
	IOLockUnlock(periodicLock);
	if (!callbacksEmpty)
		return false;

	// Both removals synchronously enter the same workloop gate.  The timer's
	// setWorkLoop(nullptr) path first disables it and increments its generation,
	// so a queued passive callout can retain/release the source but cannot invoke
	// the raw scheduler owner after this function returns.  Keep both source
	// objects alive for their native owner free() routines; only detach them here
	// before IOGraphicsAccelerator2::stop clears accelerator+0xf0.
	const auto fallbackAttached = getMember<uint8_t>(eventMachine, 0xD88);
	if (fallbackAttached == 1) {
		if (fallback->getWorkLoop() != workloop)
			return false;
		fallback->disable();
		if (workloop->removeEventSource(fallback) != kIOReturnSuccess ||
		    fallback->getWorkLoop() != nullptr)
			return false;
		getMember<uint8_t>(eventMachine, 0xD88) = 0;
	} else if (fallbackAttached != 0 || fallback->getWorkLoop() != nullptr) {
		return false;
	}

	auto *periodicAttached = periodicTimer->getWorkLoop();
	if (periodicAttached) {
		if (periodicAttached != workloop)
			return false;
		periodicTimer->cancelTimeout();
		if (workloop->removeEventSource(periodicTimer) != kIOReturnSuccess ||
		    periodicTimer->getWorkLoop() != nullptr)
			return false;
	}

	// A callback that had already entered the workloop gate at the first
	// snapshot is allowed to finish before removeEventSource returns.  Recheck
	// its terminal state only after both raw-owner sources are detached.
	IOLockLock(periodicLock);
	const bool postDetachEmpty =
		getMember<uint64_t>(eventMachine, 0xD80) == 0 &&
		getMember<uint64_t>(eventMachine, 0xD90) == 0 &&
		getMember<uint64_t>(scheduler, 0x450) == 0 &&
		periodicSet->getCount() == 0;
	IOLockUnlock(periodicLock);
	return postDetachEmpty;
}

bool vfDetachPartialNativeCallbackSources(void *accelerator)
{
	if (!accelerator ||
	    getMember<IOTimerEventSource *>(accelerator, 0x1460) != nullptr)
		return false;
	auto *workloop = getMember<IOWorkLoop *>(accelerator, 0xF0);
	auto *eventMachine = getMember<void *>(accelerator, 0x380);
	auto *scheduler = getMember<void *>(accelerator, 0x1250);
	if (!eventMachine && !scheduler)
		return true;
	if (!workloop)
		return false;

	// IOGraphicsAccelerator2::start can call its own stop before returning
	// false to IntelAccelerator::start.  That edge precedes Intel's
	// enableSchedulerEvents, engine start and DPSM construction, but a virtual
	// factory may already have created a scheduler timer.  Intel's later
	// failure epilogue instead invokes IntelAccelerator::stop with a null
	// provider, which deliberately skips base stop.  In both cases only
	// zero-use partial bindings may survive to this helper.
	if (eventMachine) {
		auto *fallback = getMember<IOInterruptEventSource *>(eventMachine, 0xD30);
		if (getMember<uint64_t>(eventMachine, 0xD80) != 0 ||
		    getMember<uint64_t>(eventMachine, 0xD90) != 0)
			return false;
		if (getMember<uint8_t>(eventMachine, 0xD88) == 1) {
			if (!fallback || fallback->getWorkLoop() != workloop)
				return false;
			fallback->disable();
			if (workloop->removeEventSource(fallback) != kIOReturnSuccess ||
			    fallback->getWorkLoop() != nullptr)
				return false;
			getMember<uint8_t>(eventMachine, 0xD88) = 0;
		} else if (getMember<uint8_t>(eventMachine, 0xD88) != 0 ||
		           (fallback && fallback->getWorkLoop() != nullptr)) {
			return false;
		}
	}

	if (scheduler) {
		auto *periodicSet = getMember<OSSet *>(scheduler, 0x438);
		auto *periodicLock = getMember<IOLock *>(scheduler, 0x440);
		auto *periodicTimer = getMember<IOTimerEventSource *>(scheduler, 0x448);
		if (!periodicSet || !periodicLock || !periodicTimer)
			return false;
		IOLockLock(periodicLock);
		const bool callbacksEmpty =
			getMember<uint64_t>(scheduler, 0x450) == 0 &&
			periodicSet->getCount() == 0;
		IOLockUnlock(periodicLock);
		if (!callbacksEmpty)
			return false;
		auto *attached = periodicTimer->getWorkLoop();
		if (attached) {
			if (attached != workloop)
				return false;
			periodicTimer->cancelTimeout();
			if (workloop->removeEventSource(periodicTimer) != kIOReturnSuccess ||
			    periodicTimer->getWorkLoop() != nullptr)
				return false;
		}
	}

	if (scheduler) {
		auto *periodicSet = getMember<OSSet *>(scheduler, 0x438);
		auto *periodicLock = getMember<IOLock *>(scheduler, 0x440);
		IOLockLock(periodicLock);
		const bool postDetachEmpty =
			(!eventMachine ||
			 (getMember<uint64_t>(eventMachine, 0xD80) == 0 &&
			  getMember<uint64_t>(eventMachine, 0xD90) == 0)) &&
			getMember<uint64_t>(scheduler, 0x450) == 0 &&
			periodicSet->getCount() == 0;
		IOLockUnlock(periodicLock);
		return postDetachEmpty;
	}
	return !eventMachine ||
	       (getMember<uint64_t>(eventMachine, 0xD80) == 0 &&
	        getMember<uint64_t>(eventMachine, 0xD90) == 0);
}

static bool vfWaitMmioHeader(NGreen *cb, bool busyPhase, uint32_t &header)
{
	// intel_guc_send_mmio allows 10 ms for ownership, then up to 20 s BUSY
	// on a VF. Spin only for the fast-response window; yield for longer waits.
	uint64_t deadline = 0, now = 0;
	clock_interval_to_deadline(busyPhase ? 20000 : 10, kMillisecondScale, &deadline);
	uint32_t spins = 0;
	for (;;) {
		header = cb->readReg32(kGen11SoftScratch0);
		if (busyPhase ? ((header & kGucOriginGuc) == 0 ||
		                 (header & kGucTypeMask) != kGucTypeBusy) :
		                ((header & kGucOriginGuc) != 0))
			return true;
		clock_get_uptime(&now);
		if (now >= deadline)
			return false;
		if (spins++ < 10)
			IODelay(1);
		else
			IOSleep(1);
	}
}

bool vfGucSendMMIO(const uint32_t request[4], uint32_t requestLength,
                   uint32_t response[4])
{
	auto *cb = NGreen::callback;
	if (!cb || !request || !response || requestLength == 0 || requestLength > 4 ||
	    (request[0] & (kGucOriginGuc | kGucTypeMask)) != 0)
		return false;
	bzero(response, 4 * sizeof(*response));
	if (!vfCanUseSleepingLock())
		return false;
	if (!cb->setRMMIOIfNecessary())
		return false;
	if (!cb->getRMMIOAddress() ||
	    cb->getRMMIOLength() < kGen11SoftScratch0 + 4 * sizeof(uint32_t))
		return false;

	if (!vfEnsureGucLock())
		return false;

	IOLockLock(gVfGucLock);
	if (gVfMmioPoisoned) {
		IOLockUnlock(gVfGucLock);
		return false;
	}
	bool success = false;
	for (uint32_t retry = 0; retry < 4 && !success; retry++) {
		for (uint32_t i = 0; i < requestLength; i++)
			cb->writeReg32(kGen11SoftScratch0 + i * 4, request[i]);

		// Match intel_guc_send_mmio(): posting read, notify, then wait first for
		// GuC ownership and finally for a non-BUSY response.
		(void)cb->readReg32(kGen11SoftScratch0 + (requestLength - 1) * 4);
		cb->writeReg32(kGen11GucHostInterrupt, kGucSendTrigger);

		uint32_t header = 0;
		if (!vfWaitMmioHeader(cb, false, header)) {
			// A timeout does not transfer mailbox ownership back to the host.
			// Replaying RESET/configuration here could overwrite a live request.
			vfMarkProtocolFault("GuC MMIO ownership timeout");
			gVfMmioPoisoned = true;
			break;
		}

		if ((header & kGucTypeMask) == kGucTypeBusy)
			(void)vfWaitMmioHeader(cb, true, header);

		if ((header & kGucOriginGuc) == 0 ||
		    (header & kGucTypeMask) == kGucTypeBusy) {
			vfMarkProtocolFault("GuC MMIO busy timeout or invalid ownership");
			gVfMmioPoisoned = true;
			break;
		}
		if ((header & kGucTypeMask) == kGucTypeRetry)
			continue;
		if ((header & kGucTypeMask) == kGucTypeFailure) {
			if ((header & 0xFFFFU) == 0x107U) {
				// INTEL_GUC_RESPONSE_VF_MIGRATED. Migration recovery is not
				// implemented; the old client configuration cannot be reused.
				vfMarkProtocolFault("VF migrated during MMIO request");
				gVfMmioPoisoned = true;
			}
			if (gVfMmioFailureLogs++ < 16)
				SYSLOG("ngreen", "V217: GuC MMIO request 0x%08x failed: 0x%08x",
				       request[0], header);
			break;
		}
		if ((header & kGucTypeMask) != kGucTypeSuccess) {
			vfMarkProtocolFault("unexpected GuC MMIO response type");
			gVfMmioPoisoned = true;
			break;
		}

		response[0] = header;
		for (uint32_t i = 1; i < 4; i++)
			response[i] = cb->readReg32(kGen11SoftScratch0 + i * 4);
		success = true;
	}
	IOLockUnlock(gVfGucLock);
	return success;
}

bool vfGucRelay(const NGVfRuntime::RelayRequest &request, uint32_t response[4])
{
	return vfGucSendMMIO(request.words, 4, response) &&
	       NGVfRuntime::validRelayResponse(response, request.magic);
}

bool vfQueryPfRuntime()
{
	if (gVfRuntimeReady)
		return true;

	uint32_t response[4] = {};
	const auto handshake = NGVfRuntime::makeRelayRequest(
		NGVfRuntime::opcodeHandshake,
		(static_cast<uint32_t>(NGVfRuntime::iovMajor) << 16U) |
		 NGVfRuntime::iovMinor,
		0, 0);
	if (!vfGucRelay(handshake, response) ||
	    response[1] != ((static_cast<uint32_t>(NGVfRuntime::iovMajor) << 16U) |
	                    NGVfRuntime::iovMinor) ||
	    response[2] != 0 || response[3] != 0) {
		SYSLOG("ngreen", "V244: PF MMIO-relay ABI 1.0 handshake failed reply=[0x%08x,0x%08x,0x%08x,0x%08x]",
		       response[0], response[1], response[2], response[3]);
		return false;
	}

	NGVfRuntime::Registers registers = {};
	const auto coreFuses = NGVfRuntime::makeRelayRequest(
		NGVfRuntime::opcodeGetRuntime, NGVfRuntime::rpmConfig0,
		NGVfRuntime::mirrorFuse3, NGVfRuntime::euDisable);
	if (!vfGucRelay(coreFuses, response))
		return false;
	registers.rpmConfig0 = response[1];
	registers.mirrorFuse3 = response[2];
	registers.euDisable = response[3];

	const auto topologyFuses = NGVfRuntime::makeRelayRequest(
		NGVfRuntime::opcodeGetRuntime, NGVfRuntime::sliceEnable,
		NGVfRuntime::geometryDssEnable, NGVfRuntime::veboxVdboxDisable);
	if (!vfGucRelay(topologyFuses, response))
		return false;
	registers.sliceEnable = response[1];
	registers.geometryDssEnable = response[2];
	registers.veboxVdboxDisable = response[3];

	const auto timingAndFirmware = NGVfRuntime::makeRelayRequest(
		NGVfRuntime::opcodeGetRuntime, NGVfRuntime::ctcMode,
		NGVfRuntime::hucKernelLoadInfo, 0);
	if (!vfGucRelay(timingAndFirmware, response) || response[3] != 0)
		return false;
	registers.ctcMode = response[1];
	registers.hucKernelLoadInfo = response[2];

	NGVfRuntime::Topology topology = {};
	if (!NGVfRuntime::deriveTopology(registers, topology)) {
		SYSLOG("ngreen", "V244: rejected PF runtime fuses rpm=0x%08x l3=0x%08x eu=0x%08x slice=0x%08x dss=0x%08x media=0x%08x",
		       registers.rpmConfig0, registers.mirrorFuse3,
		       registers.euDisable, registers.sliceEnable,
		       registers.geometryDssEnable, registers.veboxVdboxDisable);
		return false;
	}

	gVfRuntimeRegisters = registers;
	gVfTopology = topology;
	OSSynchronizeIO();
	OSCompareAndSwap(0, 1, &gVfRuntimeReady);
	SYSLOG("ngreen", "V244: PF runtime topology slices=%u DSS=%u SS=%u EU/SS=%u EUs=%u L3=%u media=0x%05x",
	       topology.sliceCount, topology.traditionalSubsliceCount / 2U,
	       topology.traditionalSubsliceCount, topology.maxEusPerSubslice,
	       topology.euCount, topology.l3BankCount, topology.enabledMediaMask);
	return true;
}

bool vfGucSelfConfig(uint16_t key, uint16_t length, uint64_t value)
{
	uint32_t request[4] = {
		kGucActionHost2GucSelfCfg,
		(static_cast<uint32_t>(key) << 16) | length,
		static_cast<uint32_t>(value),
		static_cast<uint32_t>(value >> 32),
	};
	uint32_t response[4] = {};
	if (!vfGucSendMMIO(request, 4, response) ||
	    !NGVfMmioResponse::selfConfigAccepted(response[0])) {
		SYSLOG("ngreen", "V223: GuC self-config key=0x%04x value=0x%llx failed reply=0x%08x",
		       key, static_cast<unsigned long long>(value), response[0]);
		return false;
	}
	return true;
}

bool vfConfigureMemIrq()
{
	if (!gVfUsesMemoryIrq || gVfSubmissionStopped || gVfCtbStopped ||
	    gVfProtocolFault)
		return false;
	if (gVfMemIrqConfigured)
		return true;
	if (!gVfCtbGpuBase || !gVfCtbCpuBase)
		return false;

	const uint64_t page = static_cast<uint64_t>(gVfCtbGpuBase) +
	                      kVfMemIrqOffset;
	const uint64_t gucStatus = page + kVfMemIrqStatusOffset +
	                           kVfGucIrqOffset * 16U;
	const uint64_t gucSource = page + kVfMemIrqSourceOffset +
	                           kVfGucIrqOffset;
	if (!vfGucSelfConfig(kGucSelfCfgMemIrqSourceAddr, 2, gucSource) ||
	    !vfGucSelfConfig(kGucSelfCfgMemIrqStatusAddr, 2, gucStatus))
		return false;

	// i915 enables the low 16 engine-interrupt bits at install time.  Keep the
	// same ABI value; the LRCA loads it into GEN12_RING_INT_MASK on restore.
	auto *enable = reinterpret_cast<volatile uint32_t *>(
		gVfCtbCpuBase + kVfMemIrqOffset + kVfMemIrqEnableOffset);
	*enable = gVfMemIrqRequested ? 0xFFFFU : 0;
	OSSynchronizeIO();
	gVfMemIrqConfigured = true;
	SYSLOG("ngreen", "V234: configured VF memory IRQ page=0x%llx GuC status=0x%llx source=0x%llx",
	       static_cast<unsigned long long>(page),
	       static_cast<unsigned long long>(gucStatus),
	       static_cast<unsigned long long>(gucSource));
	return true;
}

bool vfConfigureModernCtb(bool g2h, uint32_t appleDescriptorAddress)
{
	if (gVfProtocolFault || gVfSubmissionStopped || gVfCtbStopped ||
	    !gVfCtbBacking || !gVfCtbCpuBase ||
	    (g2h && appleDescriptorAddress < 0x400U))
		return false;
	// registerCommandTransportBuffers sends G2H first at base+PAGE_SIZE/4,
	// followed by H2G at base.  The re-layout hook below intentionally keeps
	// those legacy request addresses stable so the allocation base is recoverable.
	const uint32_t base = g2h ? appleDescriptorAddress - 0x400U :
	                           appleDescriptorAddress;
	if (base != gVfCtbGpuBase || base < gVfGGTTBase ||
	    static_cast<uint64_t>(base) + kVfCtbBackingBytes > gVfGGTTBase + gVfGGTTSize) {
		SYSLOG("ngreen", "V223: rejected CTB base=0x%08x expected=0x%08x VF=[0x%llx,+0x%llx]",
		       base, gVfCtbGpuBase,
		       static_cast<unsigned long long>(gVfGGTTBase),
		       static_cast<unsigned long long>(gVfGGTTSize));
		return false;
	}

	const uint64_t descriptor = base + (g2h ? kVfCtbG2HDescOffset :
	                                           kVfCtbH2GDescOffset) +
	                            kVfCtbModernDescOffset;
	const uint64_t buffer = base + (g2h ? kVfCtbG2HBufferOffset :
	                                       kVfCtbH2GBufferOffset);
	const uint32_t bytes = g2h ? kVfCtbG2HBufferBytes : kVfCtbH2GBufferBytes;
	const uint16_t descriptorKey = g2h ? kGucSelfCfgG2HCtbDescAddr :
	                                         kGucSelfCfgH2GCtbDescAddr;
	const uint16_t bufferKey = g2h ? kGucSelfCfgG2HCtbAddr :
	                                     kGucSelfCfgH2GCtbAddr;
	const uint16_t sizeKey = g2h ? kGucSelfCfgG2HCtbSize :
	                                   kGucSelfCfgH2GCtbSize;

	if ((g2h && gVfUsesMemoryIrq && !vfConfigureMemIrq()) ||
	    !vfGucSelfConfig(descriptorKey, 2, descriptor) ||
	    !vfGucSelfConfig(bufferKey, 2, buffer) ||
	    !vfGucSelfConfig(sizeKey, 1, bytes))
		return false;

	if (!g2h) {
		uint32_t request[4] = {kGucActionHost2GucControlCtb, 1, 0, 0};
		uint32_t response[4] = {};
		if (!vfGucSendMMIO(request, 2, response) ||
		    !NGVfMmioResponse::noData(response[0])) {
			SYSLOG("ngreen", "V223: GuC CTB enable failed reply=0x%08x", response[0]);
			return false;
		}
		// Mappings and KLV registration alone do not establish a usable CTB.
		// Publish the irreversible lifecycle bit before opening submission. An
		// unmap racing this boundary must not mistake a once-active VF for a
		// pre-GPU initialization rollback.
		OSCompareAndSwap(0, 1, &gVfCtbEverEnabled);
		OSSynchronizeIO();
		OSCompareAndSwap(0, 1, &gVfCtbEnabled);
	}

	SYSLOG("ngreen", "V223: configured %s CTB desc=0x%llx buffer=0x%llx bytes=0x%x",
	       g2h ? "G2H" : "H2G",
	       static_cast<unsigned long long>(descriptor),
	       static_cast<unsigned long long>(buffer), bytes);
	return true;
}

bool vfQueryKLV64(uint32_t key, uint64_t &value)
{
	uint32_t request[4] = {kGucActionQuerySingleKlv, key, 0, 0};
	uint32_t response[4] = {};
	if (!vfGucSendMMIO(request, 2, response) ||
	    !NGVfMmioResponse::queryKlvLength(response[0], 2U))
		return false;
	value = static_cast<uint64_t>(response[1]) |
	        (static_cast<uint64_t>(response[2]) << 32);
	return true;
}

bool vfQueryKLV32(uint32_t key, uint32_t &value)
{
	uint32_t request[4] = {kGucActionQuerySingleKlv, key, 0, 0};
	uint32_t response[4] = {};
	if (!vfGucSendMMIO(request, 2, response) ||
	    !NGVfMmioResponse::queryKlvLength(response[0], 1U))
		return false;
	value = response[1];
	return true;
}

bool vfBootstrapDirectGgtt()
{
	if (vfIdentifyDevice() != VfIdentity::Virtual || gVfProtocolFault)
		return false;
	if (!NGGpuCapabilities::hasKnownDirectVfGgtt(NGreen::callback->getOriginalDeviceId())) {
		// MTL/ARL require per-GT IP discovery and media-13 binder handling.
		// Do not RESET or infer direct writes from a coincidental BAR size.
		vfMarkProtocolFault("VF generation requires unimplemented per-GT GGTT discovery");
		return false;
	}
	if (!NGreen::callback->getRMMIOAddress() ||
	    NGreen::callback->getRMMIOLength() < kVfDirectBar0Bytes) {
		vfMarkProtocolFault("incomplete direct GGTT mapping before VF reset");
		return false;
	}
	if (gVfGGTTReady)
		return true;
	// RESET is not an idempotent query. A concurrent caller or retry after a
	// partial bootstrap must not reset firmware beneath already pinned state.
	if (!OSCompareAndSwap(0, 1, &gVfBootstrapStarted)) {
		vfMarkProtocolFault("concurrent or repeated incomplete VF bootstrap");
		return false;
	}

	uint32_t request[4] = {kGucActionVfReset, 0, 0, 0};
	uint32_t response[4] = {};
	if (!vfGucSendMMIO(request, 1, response) ||
	    !NGVfMmioResponse::noData(response[0])) {
		SYSLOG("ngreen", "V217: GuC VF reset failed");
		return false;
	}

	// Ask for the i915-supported GuC VF ABI 1.1.  GuC is allowed to return a
	// newer minor revision (the host firmware currently reports 1.17.0); Linux
	// i915 deliberately rejects only a newer major revision here.
	request[0] = kGucActionMatchVersion;
	request[1] = (kGucVfLatestMajor << 16) | (kGucVfLatestMinor << 8);
	if (!vfGucSendMMIO(request, 2, response)) {
		SYSLOG("ngreen", "V218: GuC VF ABI handshake transport failed");
		return false;
	}
	const uint32_t gucBranch = (response[1] >> 24) & 0xFFU;
	const uint32_t gucMajor = (response[1] >> 16) & 0xFFU;
	const uint32_t gucMinor = (response[1] >> 8) & 0xFFU;
	const uint32_t gucPatch = response[1] & 0xFFU;
	if (!NGVfMmioResponse::noData(response[0]) ||
	    gucMajor > kGucVfLatestMajor) {
		SYSLOG("ngreen", "V218: unsupported GuC VF ABI %u.%u.%u.%u (header=0x%08x)",
		       gucBranch, gucMajor, gucMinor, gucPatch, response[0]);
		return false;
	}
	SYSLOG("ngreen", "V218: negotiated GuC VF ABI %u.%u.%u.%u",
	       gucBranch, gucMajor, gucMinor, gucPatch);
	// Before CTB exists, use Intel's official GuC-proxied MMIO relay to obtain
	// PF-owned fuses.  A VF cannot safely infer these values from 0xffffffff
	// direct reads, PCI IDs, or host CPU topology.
	if (!vfQueryPfRuntime()) {
		vfMarkProtocolFault("PF runtime-register query failed");
		return false;
	}

	if (!vfQueryKLV64(kGucKlvGGTTStart, gVfGGTTBase) ||
	    !vfQueryKLV64(kGucKlvGGTTSize, gVfGGTTSize) ||
	    gVfGGTTSize == 0 || gVfGGTTBase >= 0x100000000ULL ||
	    ((gVfGGTTBase | gVfGGTTSize) & (PAGE_SIZE - 1U)) != 0 ||
	    gVfGGTTSize > 0x100000000ULL - gVfGGTTBase) {
		SYSLOG("ngreen", "V217: invalid GuC VF GGTT assignment base=0x%llx size=0x%llx",
		       static_cast<unsigned long long>(gVfGGTTBase),
		       static_cast<unsigned long long>(gVfGGTTSize));
		return false;
	}

	// Match i915's VF query contract: GuC exposes the local context and
	// doorbell quotas, while the PF-owned global range bases stay private.
	if (!vfQueryKLV32(kGucKlvNumContexts, gVfContextCount) ||
	    !vfQueryKLV32(kGucKlvNumDoorbells, gVfDoorbellCount) ||
	    gVfContextCount == 0 || gVfDoorbellCount == 0 ||
	    gVfDoorbellCount > kGen12DoorbellCount) {
		SYSLOG("ngreen", "V226: invalid GuC VF submission quotas contexts=%u doorbells=%u",
		       gVfContextCount, gVfDoorbellCount);
		return false;
	}
	SYSLOG("ngreen", "V226: GuC VF submission quotas contexts=%u doorbells=%u",
	       gVfContextCount, gVfDoorbellCount);
	if (!vfInitContextBridge()) {
		SYSLOG("ngreen", "V230: failed to allocate direct GuC context namespace");
		return false;
	}

	// The original PCI ID establishes a known direct-GGTT media-12 platform.
	// BAR length only proves that the mapped PTE window is accessible; a short
	// mapping is a failure, not permission to infer another generation's ABI.
	auto *cb = NGreen::callback;
	if (!cb || !cb->setRMMIOIfNecessary())
		return false;
	const uint64_t bar0Length = cb->getRMMIOLength();
	if (cb->getRMMIOAddress() && bar0Length >= kVfDirectBar0Bytes) {
		gVfGGTTReady = true;
		SYSLOG("ngreen", "V219: direct VF GGTT selected, BAR0=%llu MiB range=[0x%llx,+0x%llx]",
		       static_cast<unsigned long long>(bar0Length >> 20),
		       static_cast<unsigned long long>(gVfGGTTBase),
		       static_cast<unsigned long long>(gVfGGTTSize));
		return true;
	}

	vfMarkProtocolFault("direct VF GGTT requires a complete BAR0 PTE mapping");
	return false;
}

bool ngPhysicalGpuAccessAllowed()
{
	return vfIdentifyDevice() == VfIdentity::Physical;
}

bool ngVirtualGpuAccessAllowed()
{
	return vfIdentifyDevice() == VfIdentity::Virtual;
}

bool ngGpuRegisterAccessAllowed(unsigned long reg)
{
	const auto identity = vfIdentifyDevice();
	if (identity == VfIdentity::Physical)
		return true;
	if (identity != VfIdentity::Virtual)
		return false;
	const bool allowed = reg <= 0xFFFFFFFFUL &&
		NGGpuCapabilities::isVfMmioRegister(static_cast<uint32_t>(reg));
	if (!allowed)
		vfMarkProtocolFault("direct VF access outside the fixed MMIO allowlist");
	return allowed;
}

bool vfQuiesceVirtualInterruptsBeforeMsi()
{
	// MTL/ARL use a separately negotiated memory-IRQ page. This register plan
	// is the Linux Gen11 virtual-MMIO reset used by the admitted TGL/ADL/RPL
	// media-12 path only.
	if (gVfUsesMemoryIrq)
		return true;
	auto *cb = NGreen::callback;
	if (!cb || !cb->setRMMIOIfNecessary()) {
		vfMarkProtocolFault("cannot map VF BAR0 for pre-MSI interrupt reset");
		return false;
	}

	// Master disable must be the first posted write, matching
	// i915::gen11_master_intr_disable(). Sample it before touching the source
	// banks so a failed BAR transaction cannot be mistaken for quiescence.
	cb->writeReg32(NGVfIrqGate::preMsiQuiescePlan[0].offset,
	               NGVfIrqGate::preMsiQuiescePlan[0].value);
	OSSynchronizeIO();
	const uint32_t firstMaster =
		cb->readReg32(NGVfIrqGate::masterRegister);
	if (!NGVfIrqGate::masterDisabled(firstMaster)) {
		SYSLOG("ngreen", "V310: VF master IRQ disable failed value=0x%08x",
		       firstMaster);
		vfMarkProtocolFault("VF master interrupt remained enabled before MSI");
		return false;
	}

	for (uint32_t index = 1;
	     index < NGVfIrqGate::preMsiQuiesceCount; index++) {
		const auto &write = NGVfIrqGate::preMsiQuiescePlan[index];
		cb->writeReg32(write.offset, write.value);
	}
	OSSynchronizeIO();
	const uint32_t finalMaster = cb->readReg32(NGVfIrqGate::masterRegister);
	if (!NGVfIrqGate::masterDisabled(finalMaster)) {
		SYSLOG("ngreen", "V310: VF master IRQ re-enabled during reset value=0x%08x",
		       finalMaster);
		vfMarkProtocolFault("VF interrupt reset did not preserve master disable");
		return false;
	}
	SYSLOG("ngreen", "V310: quiesced Gen11 VF IRQ sources before PCI MSI registration master=0x%08x",
	       finalMaster);
	return true;
}
