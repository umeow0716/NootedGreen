//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_gen11.hpp"
#include "kern_guc_ring.hpp"
#include "kern_gpu_capabilities.hpp"
#include "kern_ggtt_bounds.hpp"
#include "kern_ggtt_rotation.hpp"
#include "kern_vf_ggtt_pte.hpp"
#include "kern_vf_irq_gate.hpp"
#include "kern_vf_context_shutdown.hpp"
#include "kern_vf_submission_gate.hpp"
#include "kern_vf_runtime.hpp"
#include "kern_vf_runtime_patch.hpp"
#include "kern_vf_tlb_patch.hpp"
#include "kern_unaligned_patch.hpp"
#include "kern_framebuffer_patch.hpp"
#include "kern_vf_guc_request.hpp"
#include "kern_context_pool.hpp"
#include "kern_context_descriptor.hpp"
#include "kern_workqueue_unwind.hpp"
#include "kern_binary_identity.hpp"
#include <Headers/kern_api.hpp>
#include "kern_green.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread_call.h>
#include <kern/sched_prim.h>
#include <i386/machine_routines.h>

// TGL FB — com.xxxxx (loaded from /Library/Extensions/)
static const char *pathsTGLFB[] = {
    "/Library/Extensions/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer",
};
static KernelPatcher::KextInfo kextG11FBT {"com.xxxxx.driver.AppleIntelTGLGraphicsFramebuffer", pathsTGLFB, 1,
    {false, false, false, true}, {},
    KernelPatcher::KextInfo::Unloaded};

// TGL FB — com.apple (loaded from /Library/Extensions/)
static KernelPatcher::KextInfo kextG11FBTA {"com.apple.driver.AppleIntelTGLGraphicsFramebuffer", pathsTGLFB, 1,
	{false, false, false, true}, {},
	KernelPatcher::KextInfo::Unloaded};

// TGL HW — com.xxxxx (loaded from /Library/Extensions/)
static const char *pathsTGLHW[] = {
    "/Library/Extensions/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics",
};
static KernelPatcher::KextInfo kextG11HWT {"com.xxxxx.driver.AppleIntelTGLGraphics", pathsTGLHW, 1,
    {false, false, false, true}, {},
    KernelPatcher::KextInfo::Unloaded};

// TGL HW — com.apple (loaded from /Library/Extensions/)
static KernelPatcher::KextInfo kextG11HWTA {"com.apple.driver.AppleIntelTGLGraphics", pathsTGLHW, 1,
	{false, false, false, true}, {},
	KernelPatcher::KextInfo::Unloaded};

// IOAcceleratorFamily2 symbols are resolved inside NGreen::processKext via
// kextIOAcceleratorFamily2 (kern_green.cpp) which already has the valid path.
// No separate kextIOAF2 registration needed here.

Gen11 *Gen11::callback = nullptr;

namespace {
// Current admission covers known media-12 direct VF GGTT platforms only.
// Media-13 requires its own per-GT IP/ABI discovery; BAR size alone is not an
// admission discriminator and no speculative relay fallback is retained.
constexpr uint32_t kVfGGTTPteBase = 0x800000;
constexpr uint32_t kVfDirectBar0Bytes = kVfGGTTPteBase * 2;
constexpr uint32_t kGen11SoftScratch0 = 0x190240;
constexpr uint32_t kGen11GucHostInterrupt = 0x1901f0;
constexpr uint32_t kGucSendTrigger = 1;
// intel_guc.h: addresses at/above this limit bypass GGTT translation.
constexpr uint64_t kGucGgttTop = 0xFEE00000ULL;

constexpr uint32_t kGucOriginGuc = 0x80000000U;
constexpr uint32_t kGucTypeMask = 0x70000000U;
constexpr uint32_t kGucTypeBusy = 0x30000000U;
constexpr uint32_t kGucTypeRetry = 0x50000000U;
constexpr uint32_t kGucTypeFailure = 0x60000000U;
constexpr uint32_t kGucTypeSuccess = 0x70000000U;

constexpr uint32_t kGucActionMatchVersion = 0x5500;
constexpr uint32_t kGucActionVfReset = 0x5507;
constexpr uint32_t kGucActionQuerySingleKlv = 0x5509;
constexpr uint32_t kGucActionHost2GucSelfCfg = 0x0508;
constexpr uint32_t kGucActionHost2GucControlCtb = 0x4509;
constexpr uint32_t kGucActionRegisterContext = NGVfGuCRequest::registerContext;
constexpr uint32_t kGucActionDeregisterContext = NGVfGuCRequest::deregisterContext;
constexpr uint32_t kGucActionDeregisterContextDone = 0x4600;
constexpr uint32_t kGucActionScheduleContext = NGVfGuCRequest::scheduleContext;
constexpr uint32_t kGucActionScheduleContextModeSet =
	NGVfGuCRequest::scheduleContextModeSet;
constexpr uint32_t kGucActionScheduleContextModeDone = 0x1002;
constexpr uint32_t kGucActionContextResetNotification = 0x1008;
constexpr uint32_t kGucActionEngineFailureNotification = 0x1009;
constexpr uint32_t kGucActionUpdateContextPolicies =
	NGVfGuCRequest::updateContextPolicies;
constexpr uint32_t kGucActionTlbInvalidation = NGVfGuCRequest::tlbInvalidation;
constexpr uint32_t kGucActionTlbInvalidationDone = 0x7001;
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

// Gen12 has 256 doorbells arranged as eight 32-doorbell SQIDI groups.  The
// physical DISTRDB register (0xd08) is intentionally not visible to a VF, but
// Apple's TGL scheduler still uses that topology to index its fixed 256-entry
// bookkeeping arrays.
constexpr uint32_t kGen12DoorbellCount = 256;
constexpr uint16_t kGen12DoorbellsPerSQIDI = 32;
constexpr uint8_t kGen12SQIDICount = 8;

// Tahoe's IGHardwareGuC layout.  The two 8 x 256-bit allocator maps occupy
// [0xdc, 0x1dc), followed by the steal cursor and the fixed 256-entry owner
// table.  Keep these bounds explicit: initDoorbells() trusts DISTRDB and will
// otherwise clear far beyond the 0xa58-byte object when a VF reads 0xffffffff.
constexpr size_t kGucDoorbellAllocatorOffset = 0xDC;
constexpr size_t kGucDoorbellTopologyOffset = 0x9E0;

IOLock *gVfGucLock = nullptr;
uint64_t gVfGGTTBase = 0;
uint64_t gVfGGTTSize = 0;
uint32_t gVfContextCount = 0;
uint32_t gVfDoorbellCount = 0;
void *gVfGlobalPageTable = nullptr;
void *gVfHardwareGuc = nullptr;
bool gVfGGTTReady = false;
NGVfRuntime::Registers gVfRuntimeRegisters = {};
NGVfRuntime::Topology gVfTopology = {};
volatile UInt32 gVfRuntimeReady = 0;
enum class VfIdentity : uint8_t { Unknown, Physical, Virtual, Invalid };
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
volatile UInt32 gVfContextShutdownStarted = 0;
volatile UInt32 gVfContextShutdownComplete = 0;
volatile UInt32 gVfDmaQuiesced = 0;
volatile UInt32 gVfDeviceStopping = 0;
bool gVfMemIrqConfigured = false;
volatile UInt32 gVfMemIrqRequested = 0;
volatile UInt32 gVfProtocolFault = 0;
bool gVfMmioPoisoned = false; // protected by gVfGucLock
volatile UInt32 gVfBootstrapStarted = 0;
volatile UInt32 gVfTlbNextSeqno = 0;
volatile UInt32 gVfTlbWaitActive = 0;
volatile UInt32 gVfTlbWaitSeqno = 0;
volatile UInt32 gVfTlbDoneSeqno = 0;
// Match i915: keep 1/4 of the 16 KiB receive ring for unsolicited events,
// plus its empty/full sentinel. Counts include both CT and HXG headers.
constexpr uint32_t kVfG2HCreditCapacity = (0x4000 - 0x1000) / 4 - 1;
volatile UInt32 gVfG2HCreditsUsed = 0;

static bool vfNativeGpuWorkReady()
{
	OSSynchronizeIO();
	return NGVfSubmission::ready({
		gVfIdentity == VfIdentity::Virtual,
		gVfGGTTReady,
		gVfMemIrqConfigured,
		gVfCtbCpuBase != nullptr,
		gVfCtbGpuBase != 0,
		gVfCtbEnabled != 0,
		gVfCtbStopped != 0,
		gVfSubmissionStopped != 0,
		gVfProtocolFault != 0,
	});
}

void vfMarkProtocolFault(const char *reason)
{
	if (OSCompareAndSwap(0, 1, &gVfProtocolFault))
		SYSLOG("ngreen", "V237: VF protocol halted after %s", reason);
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
struct VfGucContext {
	uint32_t lrcaPage;
	uint32_t descriptorLo;
	uint16_t refCount;
	uint8_t engineClass;
	uint8_t engineInstance;
	VfGucContextState state;
	bool enablePending;
	bool disablePending;
	OSObject *contextBacking;
};

IOSimpleLock *gVfContextLock = nullptr;
VfGucContext *gVfContexts = nullptr;
uint32_t gVfContextCapacity = 0;
uint32_t gVfContextLifecycleLogs = 0;

constexpr uint32_t kVfContextEventTimeoutMs = 1000;
constexpr uint32_t kVfCtbBackpressureTimeoutMs = 1000;
constexpr size_t kVfContextDescriptorOffset = 0x89;
constexpr size_t kVfContextImageBufferOffset = 0x98;
// Tahoe IGMappedBuffer::initWithOptions stores the requested byte length at
// +0x20 (0x13c48); fillIfRequested consumes the same field as a byte bound.
constexpr size_t kVfMappedBufferLengthOffset = 0x20;
constexpr uint64_t kVfContextMinimumImageBytes = 0x1000 + 0x5A * sizeof(uint32_t);
constexpr uint32_t kGucTypeFastRequest = 0x20000000U;
constexpr uint32_t kGucContextRegistrationFlagKmd = 1;
constexpr uint32_t kGucContextDisable = 0;
constexpr uint32_t kGucContextEnable = 1;
constexpr uint32_t kGucClientPriorityKmdNormal = 2;
constexpr uint32_t kGucPolicyExecutionQuantum = 0x2001;
constexpr uint32_t kGucPolicyPreemptionTimeout = 0x2002;
constexpr uint32_t kGucPolicySchedulingPriority = 0x2003;
constexpr uint32_t kGucPolicySlpmFrequency = 0x2005;

static bool vfLegacyProxyPoolValid(void *that, mach_vm_address_t getter,
	                               uint32_t *usedOut = nullptr,
	                               uint32_t *countOut = nullptr) {
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
		if ((entry.state != kVfGucContextTombstone || entry.contextBacking) &&
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
		    !gVfContexts[slot].contextBacking && tombstone < 0)
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
	const IOInterruptState interruptState =
		IOSimpleLockLockDisableInterrupt(gVfContextLock);
	auto &entry = gVfContexts[gucId];
	if (!gVfProtocolFault && entry.state == kVfGucContextTombstone &&
	    entry.refCount == 0) {
		backing = entry.contextBacking;
		entry.contextBacking = nullptr;
		entry.lrcaPage = 0;
		entry.descriptorLo = 0;
	}
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
	if (backing)
		backing->release();
}

bool vfEnsureGucLock();
bool vfValidH2GMapping(const volatile uint32_t *descriptor,
                       const volatile uint32_t *buffer);
bool vfInvalidateTLBSync(void *guc);
bool vfQuiesceDeviceForShutdown(void *guc);

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
	IOLockUnlock(gVfGucLock);
	SYSLOG("ngreen", "V230: initialized direct GuC context namespace with %u IDs",
	       gVfContextCapacity);
	return true;
}

bool vfSendCtbFastAction(void *guc, const uint32_t *request,
	                     uint32_t requestLength, uint32_t &transportFence,
	                     IOLock *alreadyHeldQueue = nullptr)
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
			const uint32_t fence = static_cast<uint32_t>(OSIncrementAtomic(
				reinterpret_cast<volatile SInt32 *>(
					reinterpret_cast<uint8_t *>(ctb) + 0x3C))) & 0xFFFFU;
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
		IOSleep(1);
	}
}

bool vfSetContextPolicy(void *guc, uint16_t gucId, uint8_t engineClass,
	                    uint32_t &transportFence)
{
	// Match i915's v70 defaults: normal KMD priority, a 1 ms execution
	// quantum, and the platform's longer render/compute preemption timeout.
	const uint32_t preemptionTimeout =
		(engineClass == 0 || engineClass == 4) ? 7500000U : 640000U;
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

class VfIrqCallbackGuard {
public:
	VfIrqCallbackGuard() : admitted(vfEnterIrqCallback()) {}
	~VfIrqCallbackGuard() {
		if (admitted)
			vfLeaveIrqCallback();
	}
	VfIrqCallbackGuard(const VfIrqCallbackGuard &) = delete;
	VfIrqCallbackGuard &operator=(const VfIrqCallbackGuard &) = delete;
	explicit operator bool() const { return admitted; }

private:
	bool admitted;
};

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

class VfContextOperationGuard {
public:
	VfContextOperationGuard() : admitted(vfEnterContextOperation()) {}
	~VfContextOperationGuard() {
		if (admitted)
			vfLeaveContextOperation();
	}
	VfContextOperationGuard(const VfContextOperationGuard &) = delete;
	VfContextOperationGuard &operator=(const VfContextOperationGuard &) = delete;
	explicit operator bool() const { return admitted; }

private:
	bool admitted;
};

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

bool vfInvalidateTLBSync(void *guc)
{
	// GEN12_GUC_TLB_INV_CR (0xCEE8) belongs to the physical GT and is not in
	// a VF's runtime MMIO allowlist. i915's gen12vf_ggtt_invalidate() sends a
	// GuC-internal, heavy invalidation with cache flush and waits for its G2H
	// sequence completion. Serialize requests so a single bounded waiter is
	// sufficient and never fall back to the physical register on failure.
	if (!vfCaptureHardwareGuc(guc))
		return false;
	if (!gVfGGTTReady || gVfProtocolFault || !gVfMemIrqConfigured ||
	    !gVfCtbGpuBase || !gVfCtbEnabled || gVfCtbStopped) {
		vfMarkProtocolFault("TLB invalidation before VF CTB/memory IRQ readiness");
		return false;
	}
	if (!vfCanWaitForGuc(guc))
		return false;
	if (!vfEnsureGucLock()) {
		vfMarkProtocolFault("TLB invalidation lock allocation failure");
		return false;
	}

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
		0x80000003U, // FLUSH_CACHE | HEAVY | GUC internal translations
	};
	uint32_t transportFence = 0;
	const bool sent = vfSendCtbFastAction(guc, request, arrsize(request),
	                                      transportFence);
	bool completed = false;
	if (sent) {
		for (uint32_t waited = 0; waited < kVfContextEventTimeoutMs; waited++) {
			OSSynchronizeIO();
			if (gVfTlbDoneSeqno == seqno) {
				completed = true;
				break;
			}
			IOSleep(1);
		}
	}
	gVfTlbWaitActive = 0;
	OSSynchronizeIO();
	IOLockUnlock(gVfGucLock);

	if (!sent || !completed) {
		SYSLOG("ngreen", "V237: VF GuC TLB invalidate failed seq=%u sent=%d fence=%u",
		       seqno, sent, transportFence);
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
		IOSleep(1);
	}
	return false;
}

// V223 CTB layout.  Apple's legacy reference scheduler packs two 1 KiB rings
// into a single 4 KiB mapping.  GuC VF self-config requires page-sized rings,
// and i915 uses a larger receive ring to avoid event starvation.  Keep Apple's
// 64-byte legacy descriptors as software bookkeeping, while presenting the
// modern descriptor at legacy+0x10 where Apple's head/tail fields already live.
constexpr uint32_t kVfCtbBackingBytes = 0x8000;
// Bias each Apple descriptor by 0x10 so its head/tail/status window begins on
// the same 2 KiB boundaries used by i915's modern descriptors.
constexpr uint32_t kVfCtbH2GDescOffset = 0x07f0;
constexpr uint32_t kVfCtbG2HDescOffset = 0x0ff0;
constexpr uint32_t kVfCtbModernDescOffset = 0x0010;
constexpr uint32_t kVfCtbH2GBufferOffset = 0x2000;
constexpr uint32_t kVfCtbH2GBufferBytes = 0x1000;
constexpr uint32_t kVfCtbG2HBufferOffset = 0x3000;
constexpr uint32_t kVfCtbG2HBufferBytes = 0x4000;
static_assert(kVfG2HCreditCapacity == (kVfCtbG2HBufferBytes * 3 / 4) / 4 - 1,
              "G2H reply credits must retain unsolicited-event headroom");
constexpr uint32_t kVfCtbUsedBytes = kVfCtbG2HBufferOffset + kVfCtbG2HBufferBytes;
// Intel's VF ABI requires a memory-interrupt page in GGTT.  Reuse the final,
// page-aligned 4 KiB of the enlarged CTB allocation so CT transport and IRQ
// state share one mapping and no extra IGMappedBuffer allocation is needed.
constexpr uint32_t kVfMemIrqOffset = 0x7000;
constexpr uint32_t kVfMemIrqBytes = 0x1000;
constexpr uint32_t kVfMemIrqStatusOffset = 0x000;
constexpr uint32_t kVfMemIrqSourceOffset = 0x400;
constexpr uint32_t kVfMemIrqEnableOffset = 0x440;
constexpr uint32_t kVfGucIrqOffset = 25;
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

bool vfStopSubmissionAndSealCtb(void *guc)
{
	if (!vfCanWaitForGuc(guc))
		return false;
	auto *ctb = guc ? getMember<void *>(guc, 0xA10) : nullptr;
	auto *lock = ctb ? getMember<IOLock *>(ctb, 0x18) : nullptr;
	auto *descriptor = ctb ? getMember<volatile uint32_t *>(ctb, 0x48) : nullptr;
	auto *buffer = ctb ? getMember<volatile uint32_t *>(ctb, 0x50) : nullptr;
	if (!lock || !vfValidH2GMapping(descriptor, buffer)) {
		vfMarkProtocolFault("cannot stop VF submission without pinned H2G transport");
		return false;
	}

	// First reject new register/enable/schedule work. Disable, deregister and
	// TLB invalidation remain admissible so already-started teardown/unmap can
	// receive the G2H completions it needs before the transport is sealed.
	OSCompareAndSwap(0, 1, &gVfSubmissionStopped);
	OSSynchronizeIO();

	for (uint32_t waited = 0; waited < kVfContextEventTimeoutMs; waited++) {
		IOLockLock(lock);
		OSSynchronizeIO();
		const uint32_t bytes = descriptor[3];
		const uint32_t head = descriptor[4];
		const uint32_t tail = descriptor[5];
		const uint32_t status = descriptor[6];
		const bool valid = NGGuCRing::validDescriptor(
			bytes, kVfCtbH2GBufferBytes, head, tail, status);
		const bool settled = valid && head == tail &&
			gVfG2HCreditsUsed == 0 && gVfTlbWaitActive == 0;
		if (settled) {
			// This lock is the linearization point shared by every H2G sender.
			// Once stopped becomes visible, no later sender can publish a frame.
			OSCompareAndSwap(0, 1, &gVfCtbStopped);
			OSCompareAndSwap(1, 0, &gVfCtbEnabled);
			OSSynchronizeIO();
			IOLockUnlock(lock);
			return true;
		}
		IOLockUnlock(lock);

		if (!valid) {
			vfMarkProtocolFault("invalid H2G descriptor while draining VF transport");
			return false;
		}
		if (gVfProtocolFault)
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
	if (gVfProtocolFault)
		return false;

	for (uint32_t id = 0; id < gVfContextCapacity; id++) {
		if (!vfRetireContextForShutdown(guc, static_cast<uint16_t>(id)))
			return false;
	}

	// Once CTB has run, finish with the same heavy GuC invalidation used for a
	// live unmap. All contexts are now deregistered and the operation gate is
	// closed, so completion is a device-wide DMA/translation boundary for every
	// later teardown unmap. Before CTB enable there cannot have been GPU work.
	if (gVfCtbEverEnabled && !vfInvalidateTLBSync(guc))
		return false;

	OSCompareAndSwap(0, 1, &gVfDmaQuiesced);
	OSSynchronizeIO();
	OSCompareAndSwap(0, 1, &gVfContextShutdownComplete);
	OSSynchronizeIO();
	SYSLOG("ngreen", "V242: VF contexts retired and DMA quiesced before CTB shutdown");
	return true;
}

uint64_t vfConsumeMemoryInterrupts()
{
	auto *base = gVfCtbCpuBase;
	if (!gVfMemIrqConfigured || !base || gVfCtbStopped)
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
	const auto consumeEngine = [&](uint32_t irqOffset, uint32_t userBit) {
		const uint8_t source = sourceBase[irqOffset];
		if (!source)
			return;
		sourceBase[irqOffset] = 0;
		auto *status = statusBase + irqOffset * 16U;
		status[0] = 0;
		pending |= 1ULL << userBit;
	};

	// irq_offset values are the Gen11+ logical engine interrupt offsets used by
	// i915.  The IGBitSet positions are Tahoe's corresponding user callbacks.
	consumeEngine(0, 0);   // RCS0
	consumeEngine(4, 1);   // CCS0
	consumeEngine(15, 2);  // BCS0
	consumeEngine(32, 3);  // VCS0
	consumeEngine(33, 4);  // VCS1 / Tahoe's second VCS callback
	consumeEngine(63, 5);  // VECS0

	const uint8_t gucSource = sourceBase[kVfGucIrqOffset];
	if (gucSource) {
		sourceBase[kVfGucIrqOffset] = 0;
		auto *gucStatus = statusBase + kVfGucIrqOffset * 16U;
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
	if (!base || gVfCtbStopped)
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
	    (response[0] & 0x0FFFFFFFU) != 1) {
		SYSLOG("ngreen", "V223: GuC self-config key=0x%04x value=0x%llx failed reply=0x%08x",
		       key, static_cast<unsigned long long>(value), response[0]);
		return false;
	}
	return true;
}

bool vfConfigureMemIrq()
{
	if (gVfSubmissionStopped || gVfCtbStopped || gVfProtocolFault)
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

	if ((g2h && !vfConfigureMemIrq()) ||
	    !vfGucSelfConfig(descriptorKey, 2, descriptor) ||
	    !vfGucSelfConfig(bufferKey, 2, buffer) ||
	    !vfGucSelfConfig(sizeKey, 1, bytes))
		return false;

	if (!g2h) {
		uint32_t request[4] = {kGucActionHost2GucControlCtb, 1, 0, 0};
		uint32_t response[4] = {};
		if (!vfGucSendMMIO(request, 2, response) ||
		    (response[0] & 0x0FFFFFFFU) != 0) {
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
	if (!vfGucSendMMIO(request, 2, response) || (response[0] & 0xFFFFU) != 2)
		return false;
	value = static_cast<uint64_t>(response[1]) |
	        (static_cast<uint64_t>(response[2]) << 32);
	return true;
}

bool vfQueryKLV32(uint32_t key, uint32_t &value)
{
	uint32_t request[4] = {kGucActionQuerySingleKlv, key, 0, 0};
	uint32_t response[4] = {};
	if (!vfGucSendMMIO(request, 2, response) || (response[0] & 0xFFFFU) != 1)
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
	if (!vfGucSendMMIO(request, 1, response)) {
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
	if ((response[0] & 0x0FFFFFFFU) != 0 || gucMajor > kGucVfLatestMajor) {
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
} // namespace

bool ngPhysicalGpuAccessAllowed()
{
	return vfIdentifyDevice() == VfIdentity::Physical;
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

void Gen11::init() {
	callback = this;

	if (checkKernelArgument("-ngreentglfb") || checkKernelArgument("-ngreentglwithgfx")) {
		SYSLOG("ngreen", "Gen11::init: FB tier → TGL");
		lilu.onKextLoadForce(&kextG11FBT);
		lilu.onKextLoadForce(&kextG11FBTA);
		if (checkKernelArgument("-ngreentglwithgfx")) {
			SYSLOG("ngreen", "Gen11::init: HW tier → TGL");
			lilu.onKextLoadForce(&kextG11HWT);
			lilu.onKextLoadForce(&kextG11HWTA);
		}
	} else if (checkKernelArgument("-ngreentglgfx")) {
		SYSLOG("ngreen", "Gen11::init: HW tier → TGL");
		lilu.onKextLoadForce(&kextG11HWT);
		lilu.onKextLoadForce(&kextG11HWTA);
	}
}

static bool isDisplayPipeForceDisabled() {
	// NOTE: isLegacyOwnershipModeEnabled() (V80 plane-linearization) is intentionally
	// NOT checked here. V80 writes are self-limiting to the first 3 ticks (≤150ms) and
	// must not prevent WindowServer from opening the display pipe after that window.
	// Coupling these two features caused DisplayPipeSupported=0 → black screen forever.

	// Stage-3 baseline now has DYLD-side NULL guards for DisplayPipe path.
	// Keep native DisplayPipe ON by default and use ngreendp0 only as an explicit
	// fallback switch when troubleshooting.
	int nativeDisplayPipe = 0;
	if (PE_parse_boot_argn("ngreendp1", &nativeDisplayPipe, sizeof(nativeDisplayPipe))) {
		SYSLOG("ngreen", "V78A: parsed ngreendp1=%d", nativeDisplayPipe);
		if (nativeDisplayPipe != 0)
			return false;
	}

	if (checkKernelArgument("-ngreendp1")) {
		SYSLOG("ngreen", "V78A: detected -ngreendp1");
		return false;
	}

	int enabled = 0;
	if (PE_parse_boot_argn("ngreendp0", &enabled, sizeof(enabled))) {
		SYSLOG("ngreen", "V78A: parsed ngreendp0=%d", enabled);
		return enabled != 0;
	}

	if (checkKernelArgument("-ngreendp0")) {
		SYSLOG("ngreen", "V78A: detected -ngreendp0");
		return true;
	}

	static bool v78aLogged = false;
	if (!v78aLogged) {
		v78aLogged = true;
		SYSLOG("ngreen", "V78A (capped): default native DisplayPipeSupported ON");
	}
	return false;
}

static uint32_t getV65Tier1WantBits() {
	// Every admitted native producer may use both render and blitter contexts.
	// Keeping BCS masked while calling native submitBlit/barrierSubmission loses
	// completions and can deadlock dependent work. VF interrupts use the separate
	// memory-IRQ bridge and never reach this physical-register policy.
	return (1u << GEN11_RCS0) | (1u << GEN11_BCS);
}

static void *vfRejectPhysicalFramebufferProbe(void *, void *, int *) {
	return nullptr;
}

static bool vfRejectPhysicalFramebufferStart(void *, void *) {
	return false;
}

bool Gen11::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	const bool physicalFramebuffer = index == kextG11FBT.loadIndex ||
		index == kextG11FBTA.loadIndex;
	if (physicalFramebuffer && !ngPhysicalGpuAccessAllowed()) {
		// A VF has no physical display controller. Refusing only DMC or MMIO
		// helpers is too late: native probe/start have their own raw accesses.
		// This is admission rejection, not a virtual framebuffer implementation.
		RouteRequestPlus reject[] = {
			{"__ZN24AppleIntelBaseController5probeEP9IOServicePi", vfRejectPhysicalFramebufferProbe},
			{"__ZN31AppleIntelFramebufferController5startEP9IOService", vfRejectPhysicalFramebufferStart},
		};
		PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, reject, address, size),
			"ngreen", "Cannot contain physical framebuffer on VF/unknown device");
		SYSLOG("ngreen", "Physical framebuffer probe/start rejected for VF/unknown device");
		return true;
	}
	if (kextG11FBT.loadIndex == index || kextG11FBTA.loadIndex == index) {
		const auto *image = reinterpret_cast<const uint8_t *>(address);
		const bool isprod = NGBinaryIdentity::matchesKextUuid(
			image, size, NGBinaryIdentity::tglFramebufferProductionUuid);
		const bool isdebug = NGBinaryIdentity::matchesKextUuid(
			image, size, NGBinaryIdentity::tglFramebufferDebugUuid);
		PANIC_COND(!isprod && !isdebug, "ngreen",
			"Unsupported TGL framebuffer payload ABI; refusing private-layout routes");
		auto *activeKext = (kextG11FBTA.loadIndex == index) ? &kextG11FBTA : &kextG11FBT;
		PANIC_COND(!NGreen::callback->setRMMIOIfNecessary(), "ngreen", "Cannot map TGL framebuffer BAR0");
		SYSLOG("ngreen", "init AppleIntelTGLGraphicsFramebuffer");
		
		// Both admitted variants checked a 64-bit read against length-4.
		// Require the full eight-byte operand before dereferencing it.
		mach_vm_address_t read64Start = 0, read64End = 0;
		SolveRequestPlus read64SymbolBounds[] = {
			{"__ZN31AppleIntelRegisterAccessManager14ReadRegister64Em",
			 read64Start},
			{"__ZN31AppleIntelRegisterAccessManager14ReadRegister64EPVvm",
			 read64End},
		};
		PANIC_COND(!SolveRequestPlus::solveAll(
		               patcher, index, read64SymbolBounds, address, size) ||
		           read64End <= read64Start || read64End - read64Start > 0x100,
		           "ngreen", "Invalid ReadRegister64 patch bounds");
		LookupPatchPlus const read64Bounds = isprod ?
			LookupPatchPlus {activeKext, NGFramebufferPatch::productionFind,
			 NGFramebufferPatch::productionReplace, 1} :
			LookupPatchPlus {activeKext, NGFramebufferPatch::debugFind,
			 NGFramebufferPatch::debugReplace, 1};
		PANIC_COND(!read64Bounds.apply(
		               patcher, read64Start, read64End - read64Start), "ngreen",
			"Failed to apply UUID-pinned ReadRegister64 bounds patch");

		return true;
		
	} else if (kextG11HWT.loadIndex == index || kextG11HWTA.loadIndex == index) {
		// Every route below depends on private Tahoe object offsets, symbol ABIs or
		// exact instruction sequences. PF ownership does not make an unknown
		// payload layout safe, so use the same fail-closed identity gate for both
		// physical and virtual devices.
		PANIC_COND(!NGBinaryIdentity::matchesKextUuid(
			reinterpret_cast<const uint8_t *>(address), size,
			NGBinaryIdentity::tglVfPayloadUuid), "ngreen",
			"Unsupported TGL accelerator payload ABI; refusing private-layout routes");
		auto *activeKext = (kextG11HWTA.loadIndex == index) ? &kextG11HWTA : &kextG11HWT;
		SYSLOG("ngreen", "init AppleIntelTGLGraphics (HW accelerator)");
		PANIC_COND(!NGreen::callback->setRMMIOIfNecessary(), "ngreen", "Cannot map TGL accelerator BAR0");
		const auto identity = vfIdentifyDevice();
		PANIC_COND(identity == VfIdentity::Invalid, "ngreen",
			"Cannot classify TGL accelerator as a physical function or VF");
		const bool vfActive = identity == VfIdentity::Virtual;
		const bool tglGeneration = NGGpuCapabilities::isTigerLake(
			NGreen::callback->getOriginalDeviceId());
		// The payload is patched before its personality is published. Complete the
		// one-shot VF bootstrap here so every embedded fuse immediate comes from
		// the same PF snapshot that owns this VF's GuC/GGTT assignment.
		PANIC_COND(vfActive && !vfBootstrapDirectGgtt(), "ngreen",
			"Cannot bootstrap VF runtime/GGTT before accelerator publication");
		SYSLOG("ngreen", "V165: setRMMIO done, starting symbol resolve");

		if (vfActive) {
			SolveRequestPlus solveRequests[] = {
				{"__ZN13IGHardwareGuC16initSchedControlEv", this->orgInitSchedControl},
				{"__ZN21IGHardwareGuCCTBuffer32handleSoftwareGuCToHostInterruptEv",
				 this->vfCtbSoftwareInterrupt},
				// V233: single-LRC GuC submission must publish the new tail in
				// the context image before scheduling it.  Resolve the accessor
				// used by IGHardwareContext::updateRingTail so the bridge can do
				// the same update without touching physical memory.
				{"__ZNK20IGSharedMappedBuffer17getVirtualAddressEv",
				 this->vfSharedMappedBufferGetVirtualAddress},
				// V236: a VF has a dedicated MSI backed by the memory-IRQ page.
				// Resolve the callback dispatcher so the replacement filter never
				// has to enter Apple's physical GT interrupt hierarchy.
				{"__ZN17IGInterruptBridge17serviceInterruptsERK8IGBitSetILm46EE",
				 this->vfServiceInterrupts},
				// V216: The TGL driver assigns the old value returned by
				// OSAddAtomic64 to IGAccelTask+0x258.  Failed accelerator start
				// candidates consume value 0 without restoring this global, so a
				// later real kernel task is misclassified as a user task throughout
				// initWithOptions.  Resolve the counter so it can be repaired before
				// the next bootstrap allocation starts.
				{"__ZN11IGAccelTask12fTaskCounterE", this->igAccelTaskCounter},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "ngreen",
			           "Failed to resolve mandatory TGL accelerator bootstrap symbols");
		}

		// V229: apply this byte patch before routing readDoorbellSQIDIConfig.
		// routeFunction overwrites the function entry and therefore overlaps the
		// exact sequence below (which starts at entry + 4).  Patching afterwards
		// made V228 panic before the driver could start.  Calls emitted as local
		// rel32 targets now remain safe even when they bypass the routed entry.
		// 0x001f00ff encodes SQIDI mask 0xff and 32 doorbells per SQIDI.
		if (vfActive) {
			SolveRequestPlus bufferAccessors[] = {
				{"__ZNK20IGSharedMappedBuffer17getVirtualAddressEv",
				 this->vfSharedMappedBufferGetVirtualAddress},
				{"__ZNK14IGMappedBuffer20getGPUVirtualAddressEv",
				 this->vfMappedBufferGetGPUVirtualAddress},
				{"__ZNK14IGMappedBuffer9getMemoryEv",
				 this->oIGMappedBuffergetMemory},
				{"__ZN13IGHardwareGuC12allocContextEyb",
				 this->vfAllocContext},
				{"__ZN13IGHardwareGuC14releaseContextEj",
				 this->vfReleaseContext},
				{"__ZN20IGSharedMappedBuffer11withOptionsEP11IGAccelTaskmjj",
				 this->vfSharedMappedBufferWithOptions},
				{"__ZN22IGHardwareGuCWorkQueue11withOptionsEP22IOGraphicsAccelerator2jP37UK_GEN11_SCHED_PROCESS_DESCRIPTOR_REC",
				 this->vfWorkQueueWithOptions},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, bufferAccessors, address, size),
			           "ngreen", "Cannot resolve VF buffer accessors or proxy-context lifecycle");
			this->vfOSObjectFree = patcher.solveSymbol(KernelPatcher::KernelID,
				"__ZN8OSObject4freeEv");
			PANIC_COND(!this->vfOSObjectFree, "ngreen",
			           "Cannot resolve base destructor for failed VF workqueues");
			RouteRequestPlus workQueueInitRoute[] = {
				{"__ZN22IGHardwareGuCWorkQueue19initWithAcceleratorEP22IOGraphicsAccelerator2jP37UK_GEN11_SCHED_PROCESS_DESCRIPTOR_REC",
				 vfWorkQueueInit, this->oVfWorkQueueInit},
				{"__ZN22IGHardwareGuCWorkQueue4freeEv",
				 vfWorkQueueFree, this->oVfWorkQueueFree},
				{"__ZN21IGHardwareGuCCTBuffer4freeEv",
				 vfCtbFree, this->oVfCtbFree},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, workQueueInitRoute, address, size),
			           "ngreen", "Failed to install VF workqueue allocation unwind");
			// Preserve Apple's event-source and callback lifecycle but remove the
			// direct GFX_MSTR_IRQ accesses surrounding enable/disableInterrupts.
			// Bound each patch by adjacent symbols, never by the entire image.
			mach_vm_address_t irqEnable = 0, irqEnableRegs = 0;
			mach_vm_address_t irqDisable = 0, irqDisableRegs = 0;
			SolveRequestPlus irqBounds[] = {
				{"__ZN17IGInterruptBridge6enableEv", irqEnable},
				{"__ZN17IGInterruptBridge16enableInterruptsEv", irqEnableRegs},
				{"__ZN17IGInterruptBridge7disableEv", irqDisable},
				{"__ZN17IGInterruptBridge17disableInterruptsEv", irqDisableRegs},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, irqBounds, address, size) ||
			           irqEnableRegs <= irqEnable || irqDisableRegs <= irqDisable ||
			           irqEnableRegs - irqEnable > 0x400 || irqDisableRegs - irqDisable > 0x400,
			           "ngreen", "Invalid VF IRQ lifecycle patch bounds");
			static const uint8_t masterDisable[] = {
				0x81, 0xa0, 0x10, 0x00, 0x19, 0x00, 0xff, 0xff, 0xff, 0x7f};
			static const uint8_t masterEnable[] = {
				0x81, 0x88, 0x10, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x80};
			static const uint8_t noMasterAccess[] = {
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
			LookupPatchPlus const masterPatches[] = {
				{activeKext, masterDisable, noMasterAccess, 1},
				{activeKext, masterEnable, noMasterAccess, 1},
			};
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, masterPatches, irqEnable,
			                                      irqEnableRegs - irqEnable) ||
			           !LookupPatchPlus::applyAll(patcher, masterPatches, irqDisable,
			                                      irqDisableRegs - irqDisable),
			           "ngreen", "Failed to isolate VF IRQ lifecycle from GFX_MSTR_IRQ");
			// These entry points can call framebuffer force-wake instead of the
			// multithreaded accelerator route. A VF has no guest-owned domains.
			RouteRequestPlus vfWakeRoutes[] = {
				{"__ZN16IntelAccelerator13SafeForceWakeEbj", wrapSafeForceWake},
				{"__ZN16IntelAccelerator22SafeForceWakeInterruptEbj", wrapSafeForceWake},
				{"__ZN16IntelAccelerator26SafeForceWakeMultithreadedEbjj", forceWake},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, vfWakeRoutes, address, size),
			           "ngreen", "Failed to isolate VF force-wake entry points");
			static const uint8_t vfDoorbellTopologyFind[] = {
				0x48, 0x8b, 0x47, 0x38,
				0x48, 0x8b, 0x80, 0x40, 0x12, 0x00, 0x00,
				0x8b, 0x80, 0x08, 0x0d, 0x00, 0x00,
				0xc6, 0x87, 0xe2, 0x09, 0x00, 0x00, 0x00,
			};
			static const uint8_t vfDoorbellTopologyReplace[] = {
				0x48, 0x8b, 0x47, 0x38,
				0x48, 0x8b, 0x80, 0x40, 0x12, 0x00, 0x00,
				0xb8, 0xff, 0x00, 0x1f, 0x00, 0x90,
				0xc6, 0x87, 0xe2, 0x09, 0x00, 0x00, 0x00,
			};
			mach_vm_address_t doorbellReadStart = 0, doorbellReadEnd = 0;
			SolveRequestPlus doorbellReadBounds[] = {
				{"__ZN13IGHardwareGuC23readDoorbellSQIDIConfigEv",
				 doorbellReadStart},
				{"__ZN13IGHardwareGuC15acquireDoorbellEP35UK_GEN11_GUC_CONTEXT_DESCRIPTOR_RECb",
				 doorbellReadEnd},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(
			               patcher, index, doorbellReadBounds, address, size) ||
			           doorbellReadEnd <= doorbellReadStart ||
			           doorbellReadEnd - doorbellReadStart > 0x100,
			           "ngreen", "Invalid VF DISTRDB patch bounds");
			LookupPatchPlus const vfDoorbellTopologyPatch {
				activeKext, vfDoorbellTopologyFind, vfDoorbellTopologyReplace, 1,
			};
			PANIC_COND(!vfDoorbellTopologyPatch.apply(
			               patcher, doorbellReadStart,
			               doorbellReadEnd - doorbellReadStart),
			           "ngreen", "Failed to replace VF DISTRDB read");
			SYSLOG("ngreen", "V229: replaced physical DISTRDB read before GuC routing");
		}

		if (vfActive) {
			RouteRequestPlus requests[] = {
			// V217: Query the media-12 PF-provisioned GGTT range, replace Apple's
			// zero/stolen-derived allocator ranges, and validate direct BAR0 PTE
			// mappings plus their required GuC TLB invalidation lifecycle.
			{"__ZN15IGMemoryManager12initSegmentsEv",
			 IGMemoryManagerInitSegments},
			{"__ZN25IGHardwareGlobalPageTable15initWithOptionsEP16IntelAcceleratorRK14IGAddressRangePvyj",
			 IGHardwareGlobalPageTableInitWithOptions,
			 this->oIGHardwareGlobalPageTableInitWithOptions},
			{"__ZN25IGHardwareGlobalPageTable8mapRangeERK14IGAddressRangeyy",
			 IGHardwareGlobalPageTableMapRange},
			{"__ZN25IGHardwareGlobalPageTable15mapRangeRotatedER33IGAddressRangeRotatedPageIteratorR25IGPhysicalSegmentIteratory",
			 IGHardwareGlobalPageTableMapRangeRotated},
			{"__ZN25IGHardwareGlobalPageTable10unmapRangeERK14IGAddressRange",
			 IGHardwareGlobalPageTableUnmapRange},
			{"__ZN25IGHardwareGlobalPageTable13mapRangeDummyERK14IGAddressRangey",
			 IGHardwareGlobalPageTableMapRangeDummy},
			
			// A VF owns neither engine power/reset nor legacy execlist rings. Keep
			// Apple's lifecycle calls away from PF-owned registers; final stop still
			// performs explicit direct-LRCA and DMA quiescence below.
			{"__ZN16IntelAccelerator19startGraphicsEngineEv", startGraphicsEngine},
			{"__ZN16IntelAccelerator18stopGraphicsEngineEv",  stopGraphicsEngine},

			// A VF has no guest-owned INSTDONE state. Derive the watchdog result
			// solely from the tracked GuC context lifecycle.
			{"__ZNK12IGScheduler59isGpuIdleEv", wrapIGScheduler5IsGpuIdle},
			{"__ZNK12IGScheduler49isGpuIdleEv", wrapIGScheduler4IsGpuIdle},

			// Legacy reset-register replay is physical-engine state and is not
			// constructed for a direct-LRCA VF.
			{"__ZN16IntelAccelerator25populateResetRegisterListEv", populateResetRegisterList},


			 // Keep the bootstrap task identity coherent before native allocation.
			 // Allocation failures propagate; no borrowed kernel task is substituted.
			 {"__ZN11IGAccelTask11withOptionsEP16IntelAccelerator", igAccelTaskWithOptions, this->oigAccelTaskWithOptions},
			 // V214: During IOAccel bootstrap IntelAccelerator+0x150 is still null.  The
			 // TGL driver otherwise takes the non-kernel branch in newPageTableForTask
			 // and dereferences that null task at +0x260.  Treat only this first VF task
			 // as the kernel task so it synchronizes from Global GTT; once +0x150 is
			 // populated, preserve Apple's classification for every later task.
			 {"__ZNK11IGAccelTask15isKernelGPUTaskEv", IGAccelTaskIsKernelGPUTask, this->oIGAccelTaskIsKernelGPUTask},
			 // Route the four native context producers so incomplete allocation is
			 // rejected before Tahoe dereferences each context's FIFO at +0xb8.
			 {"__ZN11IGAccelTask16getBlit3DContextEb", getBlit3DContext, this->ogetBlit3DContext},
		
			 // Preserve native submit work, but validate the borrowed task and every
			 // context object before the pinned Tahoe body dereferences its FIFO.
			 {"__ZN16IntelAccelerator10submitBlitEP15blit3d_params_tRK8IGVectorI11rect_pair_t25IGIOMallocAllocatorPolicyEP11IGAccelTaskb", submitBlit, this->osubmitBlit},

			 // barrierSubmission must keep its event/FIFO side effects. The wrapper
			 // admits the original only after all contexts used by that body exist.
			 {"__Z17barrierSubmissionR19IGAccelCommandQueueR16IntelAcceleratorR24IGAccelCommandDescriptorR12IOAccelEventtPKt", barrierSubmission, this->obarrierSubmission},
			 
			 // V117: Hook getBlit2DContext for null-guarding only.
			 // submitBlit expects a real Blit2D context layout and unconditionally reads
			 // [ctx+0xb8], so we must never substitute depth/color/3D context objects here.
			 {"__ZN11IGAccelTask16getBlit2DContextEb", getBlit2DContext, this->ogetBlit2DContext},

			 // Resolve-context getters are used by barrierSubmission and are generally safe.
			 {"__ZN11IGAccelTask22getDepthResolveContextEb", getDepthResolveContext, this->ogetDepthResolveContext},
			 {"__ZN11IGAccelTask22getColorResolveContextEb", getColorResolveContext, this->ogetColorResolveContext},
			 
			};
			SYSLOG("ngreen", "V165: routing %zu VF accelerator symbols", sizeof(requests)/sizeof(requests[0]));
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size),
				"ngreen", "Failed to route VF accelerator symbols");
			SYSLOG("ngreen", "V165: VF accelerator symbols routed OK");
		}

		{
			RouteRequestPlus startRoute[] = {
				{"__ZN16IntelAccelerator5startEP9IOService", start, this->ostart},
				{"__ZN16IntelAccelerator4stopEP9IOService", acceleratorStop,
				 this->oAcceleratorStop},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, startRoute, address, size),
			           "ngreen", "Cannot admit pinned accelerator without lifecycle routes");
			SYSLOG("ngreen", "V242: Hooked IntelAccelerator start/stop lifecycle");
		}

		if (vfActive) {
			// The PF already owns GuC firmware. Replace only the VF transport and
			// lifecycle entry points; a physical GPU keeps Apple's native dispatch.
			RouteRequestPlus firmwareRoute[] = {
				{"__ZN13IGHardwareGuC13loadGuCBinaryEv", loadGuCBinary},
				{"__ZN16IntelAccelerator17transferOwnershipEPK20IGSharedMappedBufferi",
				 vfTransferOwnership},
				// V222: scheduler 4 uses the Gen11 reference GuC transport, but its
				// stock MMIO helper writes the legacy 0xc180 scratch registers.  A VF
				// is provisioned only for the Gen11 0x190240/0x1901f0 mailbox.
				{"__ZN13IGHardwareGuC19mmioHostToGuCActionEPKjjiPj",
				 vfMmioHostToGuCAction},
				{"__ZN13IGHardwareGuC15hostToGuCActionEPKjjiPj",
				 vfLegacyHostToGuCAction},
				{"__ZN13IGHardwareGuC15createUkContextEy25UK_GEN11_CONTEXT_PRIORITY",
				 vfCreateUkContext},
				{"__ZN13IGHardwareGuC14allocContextIdEyb",
				 vfAllocContextId},
				{"__ZN13IGHardwareGuC16releaseContextIdEj",
				 vfReleaseContextId},
				// These routines touch raw physical doorbell registers BEFORE
				// calling hostToGuCAction; guarding the sender alone is too late.
				{"__ZN13IGHardwareGuC15acquireDoorbellEP35UK_GEN11_GUC_CONTEXT_DESCRIPTOR_RECb",
				 vfAcquireDoorbell},
				{"__ZN13IGHardwareGuC15releaseDoorbellEP35UK_GEN11_GUC_CONTEXT_DESCRIPTOR_REC",
				 vfReleaseDoorbell},
				{"__ZN13IGHardwareGuC15allocUkDoorbellEjb",
				 vfAllocUkDoorbell},
				{"__ZN13IGHardwareGuC17reacquireDoorbellEj",
				 vfReacquireDoorbell},
				{"__ZN13IGHardwareGuC9isGuCIdleEv", vfIsGuCIdle},
				{"__ZN13IGHardwareGuC13isContextIdleEj", vfIsContextIdle},
				{"__ZN13IGHardwareGuC16isKmdContextIdleERK21SGfxContextDescriptor",
				 vfIsKmdContextIdle},
				// V227: initDoorbells consumes DISTRDB before any submission.
				// Bypass the stock routine for a VF so an all-ones MMIO read
				// cannot turn into a 16 x 256 topology and corrupt the object.
				{"__ZN13IGHardwareGuC13initDoorbellsEv",
				 vfInitDoorbells},
				// V226: DISTRDB (0xd08) is outside a VF's MMIO allowlist and
				// reads as all ones.  Feed Apple's allocator the real Gen12
				// eight-by-32 topology after validating the VF's GuC KLV quota.
				{"__ZN13IGHardwareGuC23readDoorbellSQIDIConfigEv",
				 vfReadDoorbellSQIDIConfig},
				// V223: enlarge and re-layout Apple's legacy 1 KiB CTB rings before
				// translating their registration to the modern VF KLV ABI.
				{"__ZN21IGHardwareGuCCTBuffer19initWithAcceleratorEP22IOGraphicsAccelerator2",
				 vfCtbInitWithAccelerator, this->oVfCtbInitWithAccelerator},
				{"__ZN21IGHardwareGuCCTBuffer13ctChannelInitEv",
				 vfCtbChannelInit},
				{"__ZN21IGHardwareGuCCTBuffer15gucToHostActionEPj",
				 vfCtbGucToHostAction},
				{"__ZN13IGHardwareGuC32handleSoftwareGuCToHostInterruptEP22IOInterruptEventSourcei",
				 vfSoftwareGuCInterrupt},
				// V237: 0xCEE8 is PF-owned.  A VF invalidates GuC translations
				// through the asynchronous v70 CT action instead.
				{"__ZN13IGHardwareGuC13invalidateTLBEv",
				 vfInvalidateTLB},
				// IGGuC owns context-private GGTT updates and has no CTB pointer.
				// Route its invalidator separately: use the captured hardware GuC once
				// transport is ready, and match i915's pre-ready no-op behavior.
				{"__ZNK5IGGuC13invalidateTLBEv",
				 vfBaseInvalidateTLB},
				// V236: replace the complete filter on a VF.  The stock filter
				// masks GFX_MSTR_IRQ, acquires physical force-wake and only then
				// calls readAndClearInterrupts; wrapping the latter alone cannot
				// make a VF interrupt safe.
				{"__ZN17IGInterruptBridge22interruptFilterHandlerEP28IOFilterInterruptEventSource",
				 vfInterruptFilterHandler},
				// processInterrupts() can also call readAndClearInterrupts outside
				// the hardware filter.  Keep that entry point VF-safe as well.
				{"__ZN17IGInterruptBridge22readAndClearInterruptsER8IGBitSetILm46EE",
				 vfReadAndClearInterrupts},
				{"__ZN17IGInterruptBridge16enableInterruptsEv",
				 vfEnableInterrupts},
				{"__ZN17IGInterruptBridge17disableInterruptsEv",
				 vfDisableInterrupts},
				{"__ZN20IGSharedMappedBuffer11withOptionsEP11IGAccelTaskmjj",
				 vfCtbMappedBufferWithOptions, this->oVfCtbMappedBufferWithOptions},
				// V230: translate Tahoe's legacy process-wide proxy submission
				// into the one-GuC-ID-per-LRCA lifecycle required by v70 and VFs.
				{"__ZN13IGHardwareGuC29AttachContextDescToGucContextERK21SGfxContextDescriptor",
				 vfAttachContextDesc, this->oVfAttachContextDesc},
				{"__ZN13IGHardwareGuC31DetachContextDescFromGucContextERK21SGfxContextDescriptor",
				 vfDetachContextDesc, this->oVfDetachContextDesc},
				{"__ZN13IGHardwareGuC14submitWorkItemEjRK21SGfxContextDescriptor10IGHwCsTypejjj",
				 vfSubmitWorkItem, this->oVfSubmitWorkItem},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, firmwareRoute, address, size), "ngreen", "Failed to route VF GuC firmware transport");
		}

		// V237: IGHardwareGuCCTBuffer::initWithAccelerator directly accesses the
		// PF-owned 0xCEE8 register.  Remove the complete write/read/poll sequence
		// only on a verified VF. Physical hardware retains its native CT ABI.
		if (vfActive) {
			// Resolve every native body that a VF still enters, including bodies
			// captured as originals by a wrapper. Fully replaced routines are not
			// mutated: their entry routes are the isolation boundary. Each live
			// patch below is confined by adjacent symbols and has one pinned anchor.
			mach_vm_address_t workQueueInit = 0, workQueueInitEnd = 0;
			mach_vm_address_t workQueueFree = 0, workQueueFreeEnd = 0;
			mach_vm_address_t ctbInit = 0, ctbInitEnd = 0;
			mach_vm_address_t ctbFree = 0, ctbFreeEnd = 0;
			mach_vm_address_t releaseUkContext = 0, releaseUkContextEnd = 0;
			mach_vm_address_t attachContext = 0, attachContextEnd = 0;
			mach_vm_address_t detachContext = 0, detachContextEnd = 0;
			SolveRequestPlus tlbPatchBounds[] = {
				{"__ZN22IGHardwareGuCWorkQueue19initWithAcceleratorEP22IOGraphicsAccelerator2jP37UK_GEN11_SCHED_PROCESS_DESCRIPTOR_REC",
				 workQueueInit},
				{"__ZN22IGHardwareGuCWorkQueue9lockQueueEv", workQueueInitEnd},
				{"__ZN22IGHardwareGuCWorkQueue4freeEv", workQueueFree},
				{"__ZN22IGHardwareGuCWorkQueue18calculateFreeSpaceEv", workQueueFreeEnd},
				{"__ZN21IGHardwareGuCCTBuffer19initWithAcceleratorEP22IOGraphicsAccelerator2",
				 ctbInit},
				{"__ZN21IGHardwareGuCCTBuffer9lockQueueE34UK_GEN11_CMD_TRANSPORT_BUFFER_TYPE",
				 ctbInitEnd},
				{"__ZN21IGHardwareGuCCTBuffer4freeEv", ctbFree},
				{"__ZN21IGHardwareGuCCTBuffer15hostToGuCActionEPKjjiPjb", ctbFreeEnd},
				{"__ZN13IGHardwareGuC16releaseUkContextEj", releaseUkContext},
				{"__ZN13IGHardwareGuC13isContextIdleEj", releaseUkContextEnd},
				{"__ZN13IGHardwareGuC29AttachContextDescToGucContextERK21SGfxContextDescriptor",
				 attachContext},
				{"__ZN13IGHardwareGuC31DetachContextDescFromGucContextERK21SGfxContextDescriptor",
				 attachContextEnd},
				{"__ZN13IGHardwareGuC31DetachContextDescFromGucContextERK21SGfxContextDescriptor",
				 detachContext},
				{"__ZN13IGHardwareGuC13inSubmitQueueE10IGHwCsTypePV36SCHED_CONTEXT_ENGINE_PRESENCE_STRUCT",
				 detachContextEnd},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, tlbPatchBounds,
			                                      address, size) ||
			           workQueueInitEnd <= workQueueInit ||
			           workQueueInitEnd - workQueueInit > 0x200 ||
			           workQueueFreeEnd <= workQueueFree ||
			           workQueueFreeEnd - workQueueFree > 0x100 ||
			           ctbInitEnd <= ctbInit || ctbInitEnd - ctbInit > 0x200 ||
			           ctbFreeEnd <= ctbFree || ctbFreeEnd - ctbFree > 0x100 ||
			           releaseUkContextEnd <= releaseUkContext ||
			           releaseUkContextEnd - releaseUkContext > 0x200 ||
			           attachContextEnd <= attachContext ||
			           attachContextEnd - attachContext > 0x400 ||
			           detachContextEnd <= detachContext ||
			           detachContextEnd - detachContext > 0x400,
			           "ngreen", "Invalid VF physical-TLB patch bounds");
			LookupPatchPlus const vfCtbTlbPollPatch {
				activeKext, NGVfTlbPatch::ctbInitFind,
				NGVfTlbPatch::ctbInitReplace,
				sizeof(NGVfTlbPatch::ctbInitFind), 1
			};
			PANIC_COND(!vfCtbTlbPollPatch.apply(
			               patcher, ctbInit, ctbInitEnd - ctbInit), "ngreen",
			           "V222: failed to bypass VF CTB TLB poll");
			SYSLOG("ngreen", "V237: removed physical TLB access from VF CTB initialization");

			// Remove 0xCEE8 only from native bodies that are still executed. The
			// five fully routed routines retain their untouched original bytes and
			// cannot be reached through their public entries on a VF.
			LookupPatchPlus const immediateEcxPatches[] = {
				{activeKext, NGVfTlbPatch::writeImmediateFind,
				 NGVfTlbPatch::writeImmediateReplace, 1},
				{activeKext, NGVfTlbPatch::pollEcxFind,
				 NGVfTlbPatch::pollEcxReplace, 1},
			};
			LookupPatchPlus const immediateMemoryPatches[] = {
				{activeKext, NGVfTlbPatch::writeImmediateFind,
				 NGVfTlbPatch::writeImmediateReplace, 1},
				{activeKext, NGVfTlbPatch::pollMemoryFind,
				 NGVfTlbPatch::pollMemoryReplace, 1},
			};
			LookupPatchPlus const r8EcxPatches[] = {
				{activeKext, NGVfTlbPatch::writeR8Find,
				 NGVfTlbPatch::writeR8Replace, 1},
				{activeKext, NGVfTlbPatch::pollEcxFind,
				 NGVfTlbPatch::pollEcxReplace, 1},
			};
			PANIC_COND(
				!LookupPatchPlus::applyAll(patcher, immediateEcxPatches,
				 workQueueInit, workQueueInitEnd - workQueueInit) ||
				!LookupPatchPlus::applyAll(patcher, immediateEcxPatches,
				 workQueueFree, workQueueFreeEnd - workQueueFree) ||
				!LookupPatchPlus::applyAll(patcher, immediateMemoryPatches,
				 ctbFree, ctbFreeEnd - ctbFree) ||
				!LookupPatchPlus::applyAll(patcher, immediateEcxPatches,
				 releaseUkContext, releaseUkContextEnd - releaseUkContext) ||
				!LookupPatchPlus::applyAll(patcher, r8EcxPatches,
				 attachContext, attachContextEnd - attachContext) ||
				!LookupPatchPlus::applyAll(patcher, immediateEcxPatches,
				 detachContext, detachContextEnd - detachContext),
				"ngreen", "V237: failed to isolate a live VF physical-TLB caller");
			SYSLOG("ngreen", "V237: isolated all live native 0xCEE8 callers");

		}

		// Keep IGAccelDevice::deviceStart native, including failure propagation.

		// Admit exactly the 0x9a49 compatibility identity. The old patch removed
		// two branches and made every ID reach GT2 initialization; changing the
		// existing 0x9a40 compare immediate preserves Apple's sentinel and failure
		// paths while accepting the one device ID published by our PCI hook.
		static const uint8_t r3[] = {
			0x8b, 0x3e, 0x81, 0xff, 0xee, 0xbe, 0xaf, 0xde, 0x7f, 0x15,
			0x81, 0xff, 0x86, 0x80, 0x49, 0x9a, 0x74, 0x2d,
		};
		// Apple reads these PF-owned runtime registers through raw BAR0 loads in
		// getGPUInfo(). They are not VF-visible. Replace only those five loads with
		// the values returned by Intel's early MMIO relay, then leave Apple's native
		// population-count and timestamp calculations intact.
		uint8_t vfSliceFuseReplace[] = {
			0x41, 0xbc, 0, 0, 0, 0, 0x90
		};
		uint8_t vfDssFuseReplace[] = {
			0xb9, 0, 0, 0, 0, 0x89, 0x4d, 0xd0, 0x90
		};
		uint8_t vfEuFuseReplace[] = {
			0xb9, 0, 0, 0, 0, 0x89, 0x4d, 0xcc, 0x90
		};
		uint8_t vfMediaFuseReplace[] = {
			0x41, 0xbe, 0, 0, 0, 0, 0x90
		};
		uint8_t vfRpmConfigReplace[] = {
			0xb8, 0, 0, 0, 0, 0x90
		};

		// L3BankCount is selected by branches tied to Apple's original SKU table.
		// Force the single store, but populate its immediate from MIRROR_FUSE3
		// instead of assuming the target machine always has eight banks.
		uint8_t r3b[] = {
			0x90, 0x90, 0x83, 0xf9, 0x02, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x83, 0xfe, 0x01, 0x90, 0x90, 0x83, 0xfa, 0x0c, 0x90, 0x90, 0x41, 0xc7, 0x87, 0x64, 0x11, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00
		};
		
		// V139: RPL-only mitigation for GP faults inside blit3d_submit_rectlist.
		// Some command-buffer pointers on spoofed paths are 8-byte aligned; Apple emits
		// aligned SSE stores (movaps [r9+...], xmmN), which faults on unaligned targets.
		// Convert the hot-path stores to movups to tolerate unaligned command pointers.
		{
			// V52: Split patches into always-apply and RPL-only groups.
			// TGL reads its native fuse layout correctly on either PF or VF;
			// the current later-generation compatibility path must hardcode
			// because fuse layout differs and BCS ring doesn't start.
			mach_vm_address_t gpuInfoStart = 0, gpuInfoEnd = 0;
			SolveRequestPlus gpuInfoBounds[] = {
				{"__ZN16IntelAccelerator10getGPUInfoEv", gpuInfoStart},
				{"__ZN16IntelAccelerator14teardownDeviceEP11IOPCIDevice", gpuInfoEnd},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, gpuInfoBounds,
			                                      address, size) ||
			           gpuInfoEnd <= gpuInfoStart ||
			           gpuInfoEnd - gpuInfoStart > 0x1000,
			           "ngreen", "Invalid getGPUInfo patch bounds");
			LookupPatchPlus const patchesAlways[] = {
				{activeKext, NGVfRuntimePatch::spoofedSkuFind, r3,
				 arrsize(NGVfRuntimePatch::spoofedSkuFind), 1},
			};
			PANIC_COND(!LookupPatchPlus::applyAll(
			               patcher, patchesAlways, gpuInfoStart,
			               gpuInfoEnd - gpuInfoStart), "ngreen",
				"kextG11HWT Failed to apply base patches!");

			if (vfActive) {
				PANIC_COND(!gVfRuntimeReady, "ngreen",
					"VF runtime fuses were not published before getGPUInfo patching");
				NGVfRuntime::writeImmediate32(vfSliceFuseReplace + 2,
					gVfRuntimeRegisters.sliceEnable & 0xFFU);
				NGVfRuntime::writeImmediate32(vfDssFuseReplace + 1,
					gVfTopology.geometryDssMask);
				NGVfRuntime::writeImmediate32(vfEuFuseReplace + 1,
					gVfRuntimeRegisters.euDisable & 0xFFU);
				NGVfRuntime::writeImmediate32(vfMediaFuseReplace + 2,
					gVfRuntimeRegisters.veboxVdboxDisable);
				NGVfRuntime::writeImmediate32(vfRpmConfigReplace + 1,
					gVfRuntimeRegisters.rpmConfig0);
				NGVfRuntime::writeImmediate32(r3b + 28,
					gVfTopology.l3BankCount);

				LookupPatchPlus const vfRuntimePatches[] = {
					{activeKext, NGVfRuntimePatch::sliceFuseFind, vfSliceFuseReplace,
					 arrsize(NGVfRuntimePatch::sliceFuseFind), 1},
					{activeKext, NGVfRuntimePatch::dssFuseFind, vfDssFuseReplace,
					 arrsize(NGVfRuntimePatch::dssFuseFind), 1},
					{activeKext, NGVfRuntimePatch::euFuseFind, vfEuFuseReplace,
					 arrsize(NGVfRuntimePatch::euFuseFind), 1},
					{activeKext, NGVfRuntimePatch::mediaFuseFind, vfMediaFuseReplace,
					 arrsize(NGVfRuntimePatch::mediaFuseFind), 1},
					{activeKext, NGVfRuntimePatch::rpmConfigFind, vfRpmConfigReplace,
					 arrsize(NGVfRuntimePatch::rpmConfigFind), 1},
					{activeKext, NGVfRuntimePatch::l3BranchFind, r3b,
					 arrsize(NGVfRuntimePatch::l3BranchFind), 1},
				};
				PANIC_COND(!LookupPatchPlus::applyAll(
					patcher, vfRuntimePatches, gpuInfoStart, gpuInfoEnd - gpuInfoStart),
					"ngreen", "Failed to inject PF runtime fuses into VF getGPUInfo");
				SYSLOG("ngreen", "V244: injected relayed VF runtime fuses into pinned getGPUInfo");
			}

			if (!tglGeneration) {
				mach_vm_address_t rectListStart = 0, rectListEnd = 0;
				SolveRequestPlus rectListBounds[] = {
					{"__ZL22blit3d_submit_rectlistP23IGHardwareBlit3DContextP15blit3d_params_tPK8IGVectorI11rect_pair_t25IGIOMallocAllocatorPolicyE",
					 rectListStart},
					{"__ZL19IsSurfaceCompressedj", rectListEnd},
				};
				PANIC_COND(!SolveRequestPlus::solveAll(
				               patcher, index, rectListBounds, address, size) ||
				           rectListEnd <= rectListStart ||
				           rectListEnd - rectListStart > 0x3000,
				           "ngreen", "Invalid blit3d rect-list patch bounds");
				// Every matching store in the pinned blit3d_submit_rectlist body is
				// converted as one mandatory group. A single count patches all matches
				// against the original image; sequential skip-based patches changed the
				// match set after every write and therefore targeted the wrong sites.
				LookupPatchPlus const unalignedStorePatches[] = {
					{activeKext, NGUnalignedPatch::movaps10Find,
					 NGUnalignedPatch::movups10Replace, 1},
					{activeKext, NGUnalignedPatch::movaps30Find,
					 NGUnalignedPatch::movups30Replace, 1},
					{activeKext, NGUnalignedPatch::movaps50Find,
					 NGUnalignedPatch::movups50Replace, 1},
					{activeKext, NGUnalignedPatch::movaps00Find,
					 NGUnalignedPatch::movups00Replace, 6},
					{activeKext, NGUnalignedPatch::movaps20Find,
					 NGUnalignedPatch::movups20Replace, 4},
					{activeKext, NGUnalignedPatch::movaps40Find,
					 NGUnalignedPatch::movups40Replace, 1},
				};
				PANIC_COND(!LookupPatchPlus::applyAll(
				               patcher, unalignedStorePatches, rectListStart,
				               rectListEnd - rectListStart),
				           "ngreen", "Failed to apply complete unaligned-store patch set");
				SYSLOG("ngreen", "V243: converted all 14 pinned blit3d aligned-store sites");

				SYSLOG("ngreen", "V52: applied later-gen unaligned-store compatibility patches");
			} else {
				SYSLOG("ngreen", "V52: TGL generation — skipping later-gen unaligned-store patches");
			}
		}

		SYSLOG("ngreen", "Loaded AppleIntelTGLGraphics! path=%s generation=%s",
			   vfActive ? "VF" : "physical",
			   tglGeneration ? "TGL native topology" :
			   "later-gen compatibility topology");
		// Adding a personality can start matching immediately. Publish only
		// after this payload's required routes and patches have been installed.
		injectAcceleratorPersonality();

		return true;
	}

    return false;
}

bool Gen11::IGMemoryManagerInitSegments(void *that)
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

bool Gen11::IGHardwareGlobalPageTableInitWithOptions(void *that,
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
	if (result)
		gVfGlobalPageTable = that;
	SYSLOG("ngreen", "V219: direct global GGTT init ret=%d AppleMMIO=%p fullBAR0=%p len=0x%llx range=[0x%llx,+0x%llx]",
	       result, mmioBase, fullBar0,
	       static_cast<unsigned long long>(cb->getRMMIOLength()),
	       static_cast<unsigned long long>(range.start),
	       static_cast<unsigned long long>(range.length));
	return result;
}

static volatile uint64_t *vfDirectPteBase(void *that)
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

static bool vfCanCompleteGgttUpdate()
{
	OSSynchronizeIO();
	if (!gVfCtbEverEnabled)
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
	if (!gVfCtbEverEnabled)
		return true;
	if (!gVfHardwareGuc || !vfInvalidateTLBSync(gVfHardwareGuc)) {
		vfMarkProtocolFault("VF GGTT update did not complete TLB invalidation");
		return false;
	}
	return true;
}

bool Gen11::IGHardwareGlobalPageTableMapRange(void *that,
                                              const NGIGAddressRange &range,
                                              uint64_t physical,
                                              uint64_t flags)
{
	// Validate the complete operation before touching the direct PTE aperture.
	// Do not pass Apple's physical-driver cache attributes through: on Gen12
	// bits 4:2 are PF-owned VFID and bit 1 is local memory, neither of which a
	// media-12 integrated direct VF may encode.
	auto *pteBase = vfDirectPteBase(that);
	if (gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady ||
	    gVfSubmissionStopped || gVfProtocolFault ||
	    !that || that != gVfGlobalPageTable ||
	    !NGGgtt::contains(gVfGGTTBase, gVfGGTTSize, range.start, range.length) ||
	    !NGGgtt::nativePhysicalRange(physical, range.length) || !pteBase ||
	    !NGVfGgttPte::validAppleAttributes(flags) ||
	    !vfCanCompleteGgttUpdate()) {
		vfMarkProtocolFault("invalid VF GGTT map range or transport state");
		return false;
	}
	const uint64_t end = range.start + range.length;
	for (uint64_t gpu = range.start; gpu < end; gpu += 0x1000ULL) {
		pteBase[gpu >> 12] = NGVfGgttPte::encodeSystemMemory(physical);
		physical += 0x1000ULL;
	}
	return vfCompleteGgttUpdate();
}

bool Gen11::IGHardwareGlobalPageTableMapRangeRotated(void *that,
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
	    gVfSubmissionStopped || gVfProtocolFault || !that ||
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
			pteBase[destination >> 12] =
				NGVfGgttPte::encodeSystemMemory(physical);
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
			if (NGGgttRotation::destination(spec, source, destination))
				pteBase[destination >> 12] = dummyPte;
		}
		(void)vfCompleteGgttUpdate();
		segments->memory->release();
		vfMarkProtocolFault("VF rotated physical segment changed during mapping");
		return false;
	}

	rotated->sourcePage = static_cast<uint32_t>(totalPages);
	rotated->cursor = spec.rangeStart + spec.rangeLength;
	const bool completed = vfCompleteGgttUpdate();
	segments->memory->release();

	return completed;
}

void Gen11::IGHardwareGlobalPageTableUnmapRange(void *that,
                                                const NGIGAddressRange &range)
{
	// A void unmap cannot tell its caller not to free/reuse DMA backing.
	// Refuse to continue teardown on invalid input or a faulted VF; silently
	// returning would turn a skipped PTE write into a use-after-free risk.
	PANIC_COND(gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady ||
		(gVfProtocolFault && !gVfDmaQuiesced) ||
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
	for (uint64_t gpu = range.start; gpu < end; gpu += 0x1000ULL)
		pteBase[gpu >> 12] = dummyPte;

	// Apple releaseRange calls this virtual method, sets only a deferred flush
	// bit and can then return to a caller that releases the DMA mapping. A
	// completed heavy GuC invalidation is therefore part of the unmap
	// transaction, not an optional diagnostic. The sfence matches the stock
	// physical invalidator and drains direct BAR0 PTE stores first.
	__asm__ volatile("sfence" ::: "memory");
	if (invalidation == NGGgtt::TlbInvalidation::NotRequired)
		return;
	PANIC_COND(!vfInvalidateTLBSync(gVfHardwareGuc),
		"ngreen", "VF GGTT unmap could not quiesce translations before DMA release");
}

bool Gen11::IGHardwareGlobalPageTableMapRangeDummy(void *that,
                                                   const NGIGAddressRange &range,
                                                   uint64_t flags)
{
	if (gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady ||
	    gVfSubmissionStopped || gVfProtocolFault ||
	    !that || that != gVfGlobalPageTable ||
	    !NGGgtt::contains(gVfGGTTBase, gVfGGTTSize, range.start, range.length) ||
	    !NGVfGgttPte::validAppleAttributes(flags) ||
	    !vfCanCompleteGgttUpdate()) {
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
	for (uint64_t gpu = range.start; gpu < end; gpu += 0x1000ULL)
		pteBase[gpu >> 12] = dummyPte;
	return vfCompleteGgttUpdate();
}

bool Gen11::IGAccelTaskIsKernelGPUTask(const void *that)
{
	const bool original = FunctionCast(IGAccelTaskIsKernelGPUTask,
	                                   callback->oIGAccelTaskIsKernelGPUTask)(that);
	const bool virtualDevice = vfIdentifyDevice() == VfIdentity::Virtual;
	if (original || !virtualDevice || that == nullptr)
		return original;

	void *task = const_cast<void *>(that);
	void *accelerator = getMember<void *>(task, 0x10);
	if (accelerator == nullptr)
		return original;

	void *kernelTask = getMember<void *>(accelerator, 0x150);
	if (NGVfSubmission::bootstrapKernelTask(original, virtualDevice, true, true,
	                                       kernelTask != nullptr)) {
		SYSLOG("ngreen", "V214: bootstrapping first VF task from Global GTT");
		return true;
	}

	return original;
}

unsigned long Gen11::start(void *that, void *provider)
{
	// An SR-IOV VF owns neither force-wake nor legacy execlist MMIO. Establish
	// the GuC VF ABI and its assigned GGTT range before native scheduler setup.
	const auto identity = vfIdentifyDevice();
	if (identity == VfIdentity::Invalid) {
		SYSLOG("ngreen", "V239: refusing accelerator start with invalid PCI identity");
		return 0;
	}
	const bool vfActive = identity == VfIdentity::Virtual;
	if (vfActive && !vfBootstrapDirectGgtt()) {
		vfMarkProtocolFault("VF bootstrap failed before native accelerator start");
		return 0;
	}

	auto *service = static_cast<IOService *>(that);
	int scheduler = vfActive ? 4 : (NGreen::callback->isRealTGL ? 3 : 5);
	int requestedScheduler = 0;
	if (PE_parse_boot_argn("ngreenSched", &requestedScheduler,
	                       sizeof(requestedScheduler)) &&
	    requestedScheduler >= 3 && requestedScheduler <= 5) {
		scheduler = requestedScheduler;
	} else {
		auto *providerService =
			OSDynamicCast(IOService, reinterpret_cast<OSObject *>(provider));
		auto *configured = providerService ?
			OSDynamicCast(OSNumber, providerService->getProperty("SchedulerType")) :
			nullptr;
		if (configured) {
			const int value = static_cast<int>(configured->unsigned32BitValue());
			if (value >= 3 && value <= 5)
				scheduler = value;
		}
	}

	// A VF supports only the translated reference GuC scheduler. Host fallback
	// would enter PF-owned execlist and power-management register paths.
	if (vfActive)
		scheduler = 4;
	auto *schedulerNumber =
		OSNumber::withNumber(static_cast<unsigned long long>(scheduler), 32);
	const bool schedulerPublished =
		schedulerNumber && service->setProperty("GraphicsSchedulerSelect",
		                                       schedulerNumber);
	OSSafeReleaseNULL(schedulerNumber);
	if (vfActive && !schedulerPublished) {
		vfMarkProtocolFault("failed to publish mandatory VF scheduler selection");
		return 0;
	}
	SYSLOG("ngreen", "Accelerator scheduler=%d path=%s", scheduler,
	       vfActive ? "VF GuC" : "physical");

	if (vfActive) {
		auto *zero = OSNumber::withNumber(0ULL, 32);
		const bool pmDisabled =
			zero && service->setProperty("SchedPmNotifyEnable", zero);
		const bool fallbackDisabled =
			zero && service->setProperty("SchedulerFallbackOnFirmwareFail", zero);
		OSSafeReleaseNULL(zero);
		if (!pmDisabled || !fallbackDisabled) {
			vfMarkProtocolFault("failed to disable PF-owned VF scheduler fallbacks");
			return 0;
		}
	} else if (!NGreen::callback->isRealTGL) {
		// Later physical generations require the accelerator-local force-wake
		// path. Keep this as a property selection; do not program GT registers
		// before Apple's native start has established engine ownership.
		auto *current =
			OSDynamicCast(OSDictionary, service->getProperty("Development"));
		auto *development = current ?
			OSDictionary::withDictionary(current) :
			OSDictionary::withCapacity(1);
		auto *enabled = OSNumber::withNumber(1ULL, 32);
		if (development && enabled) {
			development->setObject("MultiForceWakeSelect", enabled);
			service->setProperty("Development", development);
		}
		OSSafeReleaseNULL(enabled);
		OSSafeReleaseNULL(development);
	}

	const auto result = FunctionCast(start, callback->ostart)(that, provider);
	if (!result) {
		if (vfActive)
			vfMarkProtocolFault("native accelerator start failed after VF bootstrap");
		return result;
	}

	// The accelerator personality is injected before this wrapper runs. Publish
	// only after native start succeeds so matching cannot observe partial state.
	service->registerService(kIOServiceAsynchronous);
	return result;
}

void Gen11::acceleratorStop(void *that, void *provider)
{
	// Mark final service teardown before Apple's stop sequence. Its initial
	// finishAllStamps call remains free to submit/drain work; stopGraphicsEngine
	// is the later boundary where the VF operation gate is closed and every GuC
	// context is retired while CTB and memory IRQ delivery are still available.
	if (gVfIdentity == VfIdentity::Virtual) {
		OSCompareAndSwap(0, 1, &gVfDeviceStopping);
		OSSynchronizeIO();
		SYSLOG("ngreen", "V242: VF accelerator stop requested; deferring quiescence until post-stamp engine stop");
	}
	FunctionCast(acceleratorStop, callback->oAcceleratorStop)(that, provider);
}

void Gen11::populateResetRegisterList(void *that)
{
	(void)that;
	// This replacement is installed only for a classified VF. It has no
	// guest-owned legacy reset registers; GuC restores direct-LRCA contexts
	// from PF-managed state.
}

void *Gen11::igAccelTaskWithOptions(void *that)
{
	// V216: IOGraphicsAccelerator2 stores IntelAccelerator+0x150 only after
	// IGAccelTask::withOptions returns.  The first task therefore has to receive
	// counter value 0; that value controls its address-mode flags, managed page
	// table list, Global-GTT synchronization, and stamp/scratch allocation.
	//
	// Earlier failed start candidates increment Apple's process-wide counter but
	// IGAccelTask::free never decrements it (only IGAccelTask::stop resets it).
	// Reset the stale value before the next unassigned accelerator constructs its
	// kernel task.  Changing the per-object identity later in initialization is
	// unsafe because address mode and page-table state have already been chosen.
	if (vfIdentifyDevice() == VfIdentity::Virtual && that != nullptr &&
	    getMember<void *>(that, 0x150) == nullptr) {
		if (callback->igAccelTaskCounter == 0) {
			SYSLOG("ngreen", "V216: bootstrap task counter symbol is unavailable; refusing unsafe allocation");
			return nullptr;
		}

		auto *counter = reinterpret_cast<volatile UInt64 *>(callback->igAccelTaskCounter);
		UInt64 stale = *counter;
		if (stale != 0) {
			bool repaired = false;
			for (unsigned attempt = 0; attempt < 8 && stale != 0; attempt++) {
				if (OSCompareAndSwap64(stale, 0, counter)) {
					repaired = true;
					break;
				}
				stale = *counter;
			}

			if (repaired) {
				SYSLOG("ngreen",
				       "V216: reset stale VF bootstrap task counter from %llu before allocation",
				       static_cast<unsigned long long>(stale));
			} else if (*counter != 0) {
				SYSLOG("ngreen",
				       "V216: could not stabilize VF bootstrap task counter (current=%llu); refusing unsafe allocation",
				       static_cast<unsigned long long>(*counter));
				return nullptr;
			}
		}
	}

	// Preserve native ownership: a failed factory returns null. Never write an
	// unrelated IOAccelTask base-class field or substitute a borrowed task.
	return FunctionCast(igAccelTaskWithOptions,
	                    callback->oigAccelTaskWithOptions)(that);
}

void *Gen11::getBlit3DContext(void *that, bool create)
{
	if (!that || !callback->ogetBlit3DContext)
		return nullptr;
	if (gVfIdentity == VfIdentity::Virtual && !vfNativeGpuWorkReady())
		return nullptr;
	return FunctionCast(getBlit3DContext,
	                    callback->ogetBlit3DContext)(that, create);
}

unsigned long Gen11::startGraphicsEngine(void *that)
{
	(void)that;
	// This replacement is installed only for a classified VF. Ring power,
	// reset and legacy execlist state belong to the PF; the translated GuC
	// scheduler brings VF contexts online independently.
	return 1;
}

unsigned long Gen11::stopGraphicsEngine(void *that)
{
	(void)that;
	if (gVfDeviceStopping && gVfGGTTReady) {
		PANIC_COND(!vfQuiesceDeviceForShutdown(gVfHardwareGuc), "ngreen",
			"Cannot stop VF accelerator before every GuC context and DMA mapping is quiesced");
		SYSLOG("ngreen", "V242: VF final engine stop completed through GuC context retirement");
	}
	// Runtime reset and final ring stop are both PF/GuC-owned. The final path
	// above first closes every guest producer and waits for DMA.
	return 1;
}

bool Gen11::submitBlit(void *that, void *param_1, void *param_2, void *param_3, bool param_4) {
	// Native returns a boolean in AL, not an IOReturn. Every rejected/no-op
	// path below returns false; success is propagated only from actual native work.
	// Caller paths that ignore this result still need separate failure handling.
	if (!that || !param_2 || !callback->osubmitBlit)
		return false;

	// The native implementation returns true immediately for an empty vector.
	const uint64_t count = *reinterpret_cast<const uint64_t *>(param_2);
	if (count == 0)
		return FunctionCast(submitBlit, callback->osubmitBlit)(
			that, param_1, param_2, param_3, param_4);
	if (!param_1 || !param_3 || !getMember<void *>(param_3, 0x0))
		return false;

	if (gVfIdentity == VfIdentity::Virtual && !vfNativeGpuWorkReady())
		return false;

	// Tahoe may dereference either native blit-context FIFO depending on its
	// format/route selection. Preallocate both through the original factories, but do not write
	// private task slots or substitute a task with unrelated ownership.
	void *blit2D = getBlit2DContext(param_3, true);
	void *blit3D = getBlit3DContext(param_3, true);
	if (!blit2D || !blit3D || !getMember<void *>(blit2D, 0xb8) ||
	    !getMember<void *>(blit3D, 0xb8)) {
		SYSLOG("ngreen", "V243: rejected blit with incomplete native contexts task=%p 2D=%p 3D=%p",
		       param_3, blit2D, blit3D);
		return false;
	}

	// On a later-generation physical GPU, routeSel=3 executes TGL-compiled EU
	// shader payloads which previously stalled RCS. The VF path deliberately
	// exercises it through GuC for controlled validation; PF remains explicit
	// unsupported containment until generation-specific payloads exist.
	// routeSel=3 routes to blit3d_submit_commands which executes TGL-compiled EU shaders
	if (!NGreen::callback->isRealTGL &&
	    gVfIdentity != VfIdentity::Virtual) {
		auto *blit193 = reinterpret_cast<uint8_t *>(param_1);
		const uint32_t routeSel193 = (blit193[0xA2] >> 3U) & 0x3U;
		if (routeSel193 == 3) {
			return false;
		}
	}

	return FunctionCast(submitBlit, callback->osubmitBlit)(that, param_1, param_2, param_3, param_4);
}

uint8_t Gen11::barrierSubmission(void *queue, void *accelerator, void *cmdDesc,
								 void *event, uint16_t count, const uint16_t *list) {
	if (gVfIdentity == VfIdentity::Virtual) {
		if (!vfNativeGpuWorkReady() || !queue || !accelerator || !cmdDesc ||
		    !event || (count && !list) || !callback->obarrierSubmission)
			return 0;

		// The Tahoe body unconditionally dereferences 2D, 3D and depth FIFO
		// pointers and may dereference color as well. Prime every native context
		// first so a failed VF allocation becomes a truthful barrier failure,
		// never a kernel null dereference or fabricated completion.
		void *task = getMember<void *>(cmdDesc, 0x8);
		void *blit2D = task ? getBlit2DContext(task, true) : nullptr;
		void *blit3D = task ? getBlit3DContext(task, true) : nullptr;
		void *depth = task ? getDepthResolveContext(task, true) : nullptr;
		void *color = task ? getColorResolveContext(task, true) : nullptr;
		if (!blit2D || !blit3D || !depth || !color ||
		    !getMember<void *>(blit2D, 0xb8) ||
		    !getMember<void *>(blit3D, 0xb8) ||
		    !getMember<void *>(depth, 0xb8) ||
		    !getMember<void *>(color, 0xb8)) {
			SYSLOG("ngreen", "V243: rejected VF barrier with incomplete contexts task=%p 2D=%p 3D=%p depth=%p color=%p",
			       task, blit2D, blit3D, depth, color);
			return 0;
		}
		return FunctionCast(barrierSubmission, callback->obarrierSubmission)(
			queue, accelerator, cmdDesc, event, count, list);
	}

	if (!queue || !accelerator || !cmdDesc || !event ||
	    (count && !list) || !callback->obarrierSubmission)
		return 0;

	if (!NGreen::callback->isRealTGL) {
		// The same UUID-pinned native body has unconditional context/FIFO
		// dereferences on a later-generation PF. Reject incomplete allocation,
		// but never replace the barrier with a constant-success response.
		void *task = getMember<void *>(cmdDesc, 0x8);
		void *blit2D = task ? getBlit2DContext(task, true) : nullptr;
		void *blit3D = task ? getBlit3DContext(task, true) : nullptr;
		void *depth = task ? getDepthResolveContext(task, true) : nullptr;
		void *color = task ? getColorResolveContext(task, true) : nullptr;
		if (!blit2D || !blit3D || !depth || !color ||
		    !getMember<void *>(blit2D, 0xb8) ||
		    !getMember<void *>(blit3D, 0xb8) ||
		    !getMember<void *>(depth, 0xb8) ||
		    !getMember<void *>(color, 0xb8))
			return 0;
	}
	return FunctionCast(barrierSubmission, callback->obarrierSubmission)(queue, accelerator,
																		 cmdDesc, event,
																		 count, list);
}

void *Gen11::getBlit2DContext(void *that, bool create)
{
	if (!that || !callback->ogetBlit2DContext)
		return nullptr;
	if (gVfIdentity == VfIdentity::Virtual && !vfNativeGpuWorkReady())
		return nullptr;
	return FunctionCast(getBlit2DContext,
	                    callback->ogetBlit2DContext)(that, create);
}

void *Gen11::getDepthResolveContext(void *that, bool create)
{
	if (!that || !callback->ogetDepthResolveContext)
		return nullptr;
	if (gVfIdentity == VfIdentity::Virtual && !vfNativeGpuWorkReady())
		return nullptr;
	return FunctionCast(getDepthResolveContext,
	                    callback->ogetDepthResolveContext)(that, create);
}

void *Gen11::getColorResolveContext(void *that, bool create)
{
	if (!that || !callback->ogetColorResolveContext)
		return nullptr;
	if (gVfIdentity == VfIdentity::Virtual && !vfNativeGpuWorkReady())
		return nullptr;
	return FunctionCast(getColorResolveContext,
	                    callback->ogetColorResolveContext)(that, create);
}

unsigned long Gen11::loadGuCBinary(void *that) {
	// The PF already owns and runs GuC for an SR-IOV VF. Report firmware as
	// available so Apple's GuC scheduler initializes its submission transport,
	// but never try to replace the PF-owned image or WOPCM configuration.
	// This route is installed only for the UUID-pinned VF payload.
	if (gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady || gVfProtocolFault)
		return 0;
	// The stock load routine initializes all scheduler-side allocations before
	// touching WOPCM and uploading firmware. Skipping it outright left
	// contextCount at zero, so the first createUkContext returned the 0x400
	// invalid-context sentinel and tore the GuC object down.
	if (!that || !callback->orgInitSchedControl || !vfCanUseSleepingLock() ||
	    !getMember<void *>(that, 0x38) || !getMember<IOLock *>(that, 0x40) ||
	    !getMember<IOLock *>(that, 0xA08)) {
		vfMarkProtocolFault("missing VF scheduler initializer, owner or locks");
		return 0;
	}
	using InitSchedControl = bool (*)(void *);
	const bool initialized = reinterpret_cast<InitSchedControl>(
		callback->orgInitSchedControl)(that);
	// Native initSchedControl checks setupContextPool but discards failures from
	// setupLogBuffers and setupAdditionalDataStructs before returning true. Do
	// not admit partially constructed scheduler storage.
	const bool storageReady = initialized && getMember<void *>(that, 0x50) &&
		getMember<void *>(that, 0x68) && getMember<void *>(that, 0x60) &&
		getMember<void *>(that, 0x70) && getMember<void *>(that, 0x78) &&
		getMember<void *>(that, 0x9E8);
	if (!storageReady) {
		vfMarkProtocolFault("incomplete native VF scheduler storage allocation");
		return 0;
	}
	SYSLOG("ngreen", "V224: VF GuC firmware is PF-owned; scheduler data init=%d",
	       initialized);
	return initialized && !gVfProtocolFault;
}

// A failed, unpublished object has no process pointer. Use this otherwise
// invalid pointer value only to hand its proven cleanup state to virtual free;
// never dereference it or publish this object to the native caller.
bool Gen11::vfWorkQueueInit(void *that, void *accelerator, uint32_t id, void *process) {
	// Installed only for the UUID-pinned VF payload. withOptions owns this
	// fresh object exclusively until init returns; successful queues unchanged.
	if (!that)
		return false;
	PANIC_COND(getMember<void *>(that, 0x10) || getMember<void *>(that, 0x20) ||
		getMember<void *>(that, 0x30) || getMember<void *>(that, 0x38), "ngreen",
		"Refusing reinitialization of a nonempty VF workqueue");
	if (!accelerator || !process || id >= NGContextPool::invalidId ||
	    !vfCanUseSleepingLock() ||
	    (accelerator && (getMember<uint8_t>(accelerator, 0x1190) & 0x20U))) {
		if (accelerator && (getMember<uint8_t>(accelerator, 0x1190) & 0x20U))
			vfMarkProtocolFault("legacy page ownership requested for VF workqueue");
		getMember<void *>(that, 0x38) = NGWorkQueue::failedInitMarker();
		return false;
	}
	const bool result = FunctionCast(vfWorkQueueInit, callback->oVfWorkQueueInit)(
		that, accelerator, id, process);
	if (result)
		return true;
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

void Gen11::vfWorkQueueFree(void *that) {
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

uint32_t Gen11::vfCreateUkContext(void *that, uint64_t owner, int priority) {
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
	    !callback->oIGMappedBuffergetMemory) {
		// Apple's initWithOptions releases a failed CTB and continues toward
		// legacy MMIO context creation. Its createUkContext failure sentinel
		// is 0x400, which makes that initialization path return failure.
		vfMarkProtocolFault("legacy context creation without safe VF transport or pool lock");
		return NGContextPool::invalidId;
	}

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
		vfMarkProtocolFault("VF proxy context has no accelerator task owner");
		return NGContextPool::invalidId;
	}
	if (getMember<uint8_t>(accelerator, 0x1190) & 0x20U) {
		vfMarkProtocolFault("legacy page ownership requested for VF proxy context");
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
	if (id == NGContextPool::invalidId)
		return id;

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
		rollback(id, nullptr, nullptr);
		return NGContextPool::invalidId;
	}
	const uint64_t backingCpu = reinterpret_cast<uint64_t>(getVirtual(backing));
	const uint64_t backingGpu = getGpu(backing);
	const uint64_t backingBytes = getMember<uint64_t>(
		backing, kVfMappedBufferLengthOffset);
	if (!NGGgtt::mappedBacking(backingCpu, backingBytes, PAGE_SIZE, backingGpu,
	                          gVfGGTTBase, gVfGGTTSize, kGucGgttTop)) {
		vfMarkProtocolFault("invalid VF proxy process backing mapping");
		rollback(id, nullptr, backing);
		return NGContextPool::invalidId;
	}
	auto *memory = reinterpret_cast<GetMemory>(callback->oIGMappedBuffergetMemory)(backing);
	auto **vtable = memory ? *reinterpret_cast<void ***>(memory) : nullptr;
	auto segment = vtable ? reinterpret_cast<GetPhysicalSegment>(
		vtable[0x158 / sizeof(void *)]) : nullptr;
	uint64_t segmentBytes = 0;
	const uint64_t physical = segment ? segment(memory, 0, &segmentBytes) : 0;
	if (!physical || segmentBytes < PAGE_SIZE ||
	    !NGGgtt::nativePhysicalRange(physical, PAGE_SIZE)) {
		vfMarkProtocolFault("invalid VF proxy process physical segment");
		rollback(id, nullptr, backing);
		return NGContextPool::invalidId;
	}

	auto *process = reinterpret_cast<void *>(backingCpu + PAGE_SIZE / 2);
	const uint64_t processGpu = backingGpu + PAGE_SIZE / 2;
	auto *queue = reinterpret_cast<WorkQueueFactory>(
		callback->vfWorkQueueWithOptions)(accelerator, id, process);
	if (!queue) {
		rollback(id, nullptr, backing);
		return NGContextPool::invalidId;
	}
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
		vfMarkProtocolFault("incomplete VF proxy workqueue mapping or ownership");
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
	return id;
}

uint32_t Gen11::vfAllocContextId(void *that, uint64_t owner, bool clear) {
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

void Gen11::vfReleaseContextId(void *that, uint32_t id) {
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

uint16_t Gen11::vfAcquireDoorbell(void *that, void *descriptor, bool pin) {
	(void)that;
	(void)descriptor;
	(void)pin;
	vfMarkProtocolFault("legacy doorbell acquisition on direct-LRCA VF");
	return 0x100U; // native invalid-doorbell sentinel, not an allocated ID
}

void Gen11::vfReleaseDoorbell(void *that, void *descriptor) {
	(void)that;
	(void)descriptor;
	// No legacy doorbell can have been acquired through the VF entry points.
	// Do not clear 0xfd4/0x1000 register banks or pretend hardware was released.
	vfMarkProtocolFault("legacy doorbell release on direct-LRCA VF");
}

bool Gen11::vfAllocUkDoorbell(void *that, uint32_t contextId, bool pin) {
	(void)that;
	(void)contextId;
	(void)pin;
	vfMarkProtocolFault("legacy UK doorbell allocation on direct-LRCA VF");
	return false;
}

uint16_t Gen11::vfReacquireDoorbell(void *that, uint32_t contextId) {
	(void)that;
	(void)contextId;
	// Reject before the original's acquire/sleep/retry loop.
	vfMarkProtocolFault("legacy doorbell retry on direct-LRCA VF");
	return 0x100U;
}

// Modern contexts never update Apple's legacy proxy work-queue idle fields.
// Only states that cannot currently execute provide an idle snapshot. Enabled
// contexts remain conservatively busy until a real completion/idle mechanism
// is implemented. This is NOT a submission barrier or device-DMA-stop proof.
static bool vfContextKnownIdle(const VfGucContext &entry) {
	return !entry.enablePending && !entry.disablePending &&
		(entry.state == kVfGucContextEmpty || entry.state == kVfGucContextTombstone ||
		 entry.state == kVfGucContextRegistered || entry.state == kVfGucContextDisabled);
}

static bool vfKnownIdleSnapshot(const uint32_t *descriptor = nullptr) {
	if (gVfIdentity != VfIdentity::Virtual || !gVfCtbEnabled || gVfCtbStopped ||
	    gVfProtocolFault || !gVfContextLock || !gVfContexts)
		return false;
	const IOInterruptState saved = IOSimpleLockLockDisableInterrupt(gVfContextLock);
	bool idle = true;
	if (descriptor) {
		const auto value = NGContextDescriptor::read(descriptor);
		const int32_t slot = vfFindContextLocked(value.low & 0xFFFFF000U);
		idle = slot >= 0 && gVfContexts[slot].descriptorLo == value.low &&
		       vfContextKnownIdle(gVfContexts[slot]);
	} else {
		for (uint32_t i = 0; i < gVfContextCapacity; ++i) {
			if (!vfContextKnownIdle(gVfContexts[i])) {
				idle = false;
				break;
			}
		}
	}
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, saved);
	return idle;
}

bool Gen11::vfIsGuCIdle(void *that) {
	(void)that;
	return vfKnownIdleSnapshot();
}

bool Gen11::vfIsContextIdle(void *that, uint32_t contextId) {
	(void)that;
	(void)contextId;
	// Legacy proxy ID cannot identify a single modern LRCA. All-idle is a
	// conservative sufficient condition; never inspect unused proxy counters.
	return vfKnownIdleSnapshot();
}

bool Gen11::vfIsKmdContextIdle(void *that, const uint32_t *descriptor) {
	(void)that;
	return descriptor && vfKnownIdleSnapshot(descriptor);
}

void Gen11::vfTransferOwnership(void *that, const void *backing, int owner) {
	(void)backing;
	(void)owner;
	// Native transferOwnership is itself a no-op without flag 0x20. With it,
	// it sends per-page commands through PCI config 0xf8/0xfc, not the Intel
	// SR-IOV protocol. Never let a legacy ownership mechanism act on VF pages.
	if (!that || gVfIdentity != VfIdentity::Virtual ||
	    (getMember<uint8_t>(that, 0x1190) & 0x20U))
		vfMarkProtocolFault("unsupported legacy VF page-ownership transfer");
}

bool Gen11::vfLegacyHostToGuCAction(void *that, const uint32_t *request,
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

bool Gen11::vfMmioHostToGuCAction(void *that, const uint32_t *request,
	                              unsigned int requestLength, int timeout,
	                              uint32_t *response) {
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
		if (requestLength != 4 || request[2] != 0x40U || request[3] > 1U) {
			SYSLOG("ngreen", "V223: rejected malformed legacy CTB registration len=%u args=%08x/%08x/%08x",
			       requestLength, request[1], request[2], request[3]);
			return false;
		}
		const bool ok = vfConfigureModernCtb(request[3] == 1U, request[1]);
		if (response)
			*response = 0;
		return ok;
	}

	// Legacy teardown deregisters H2G and G2H separately with 0x4506.  The
	// modern VF ABI owns both channels under one 0x4509 control bit, so disable
	// once on Apple's first (type 0) request and acknowledge the second locally.
	if (request[0] == 0x4506U) {
		if (requestLength != 3 || request[2] > 1U) {
			SYSLOG("ngreen", "V224: rejected malformed legacy CTB deregistration len=%u",
			       requestLength);
			return false;
		}
		bool ok = true;
		if (request[2] == 0U) {
			// GuC free can be reached through partial-init unwind without the
			// accelerator stop wrapper. Establish the context/DMA boundary here as
			// an idempotent last chance, while CTB and IRQ consumers are still live.
			const bool dmaQuiesced = vfQuiesceDeviceForShutdown(that);
			// Stop ordinary producers first, but keep G2H/IRQ consumers alive long
			// enough to retire already-published MODE_DONE/DEREGISTER_DONE/TLB_DONE
			// replies. The final H2G lock check seals the transport only after the
			// ring is empty and all reserved reply credits have returned.
			const bool producersStopped = dmaQuiesced &&
				vfStopSubmissionAndSealCtb(that);

			// Once no reply is still required, mask future engine memory IRQs.
			// This is still not proof that GuC/device DMA has stopped touching the
			// page; it only prevents a later enable path from reopening interrupts.
			if (producersStopped) {
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
			const bool irqDrained =
				producersStopped && vfCloseIrqCallbackGateAndWait(that);

			gVfCtbDisableConfirmed = false;
			uint32_t disable[4] = {kGucActionHost2GucControlCtb, 0, 0, 0};
			uint32_t reply[4] = {};
			const bool disabled = producersStopped &&
				vfGucSendMMIO(disable, 2, reply) &&
				(reply[0] & 0x0FFFFFFFU) == 0;
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
			*response = ok ? 0 : 1;
		if (request[2] == 1U && ok) {
			// CPU interrupt callbacks are drained, but CTB disable alone does not
			// prove engine or GuC memory-IRQ DMA is quiescent. Both users share
			// this backing, so keep our references and mappings quarantined until
			// device-side shutdown has a verified completion boundary.
			gVfMemIrqConfigured = false;
			gVfCtbDisableConfirmed = false;
			PANIC_COND(!gVfDmaQuiesced, "ngreen",
				"CTB stopped without a completed VF DMA-quiescence boundary");
			SYSLOG("ngreen", "V242: CTB stopped after context/DMA quiescence; shared memory-IRQ backing remains quarantined");
		}
		return ok;
	}

	return vfLegacyHostToGuCAction(that, request, requestLength, timeout, response);
}

bool Gen11::vfReadDoorbellSQIDIConfig(void *that) {
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

void Gen11::vfInitDoorbells(void *that) {
	// IntelAccelerator::start normally completed this before constructing the
	// scheduler.  Retrying here makes the ordering requirement explicit and
	// prevents a transiently-unready VF from falling through to DISTRDB.
	if (!gVfGGTTReady && !vfBootstrapDirectGgtt()) {
		SYSLOG("ngreen", "V227: refusing physical doorbell discovery without VF bootstrap");
		return;
	}
	if (gVfDoorbellCount != kGen12DoorbellCount) {
		SYSLOG("ngreen", "V227: unsupported VF doorbell quota %u during allocator init",
		       gVfDoorbellCount);
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
	if (!backing || !getter || !gVfMemIrqConfigured || !gVfCtbEnabled ||
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
	regs[0x50] = 0x14C80002U; // MI_LRM, global GGTT, engine-relative MMIO
	regs[0x51] = 0xA8;       // GEN12_RING_INT_MASK
	regs[0x52] = page + kVfMemIrqEnableOffset;
	regs[0x53] = 0;
	regs[0x55] = 0x11081003U; // MI_LRI(2), posted, engine-relative MMIO
	regs[0x56] = 0xAC;       // GEN12_RING_INT_STATUS
	regs[0x57] = page + kVfMemIrqStatusOffset;
	regs[0x58] = 0xA4;       // GEN12_RING_INT_SRC
	regs[0x59] = page + kVfMemIrqSourceOffset;
	OSSynchronizeIO();
	return true;
}

bool Gen11::vfAttachContextDesc(void *that, const uint32_t *descriptor) {
	VfContextOperationGuard operationGuard;
	if (!operationGuard)
		return false;
	if (gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady ||
	    gVfSubmissionStopped || gVfProtocolFault || !that || !descriptor ||
	    !callback->vfSharedMappedBufferGetVirtualAddress)
		return false;

	// Native attach touches the legacy pool, LRCA record and descriptor before
	// it returns. Validate every interval it will index before giving it control;
	// post-call rejection would be too late to prevent an invalid kernel access.
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
	const uint64_t contextBytes = contextBacking ?
		getMember<uint64_t>(contextBacking, kVfMappedBufferLengthOffset) : 0;
	uint32_t poolUsed = 0, poolCount = 0;
	const bool poolValid = vfLegacyProxyPoolValid(
		that, callback->vfSharedMappedBufferGetVirtualAddress, &poolUsed, &poolCount);
	if (!descriptorAttributes.valid || !poolValid || !contextBacking ||
	    !NGGgtt::contains(gVfGGTTBase, gVfGGTTSize, lrcaPage, contextBytes) ||
	    lrcaPage >= kGucGgttTop || contextBytes > kGucGgttTop - lrcaPage ||
	    contextBytes < kVfContextMinimumImageBytes) {
		SYSLOG("ngreen", "V241: rejected pre-native LRCA %08x:%08x bytes=0x%llx pool=%u/%u",
		       descriptorHi, descriptorLo,
		       static_cast<unsigned long long>(contextBytes), poolUsed, poolCount);
		vfMarkProtocolFault("invalid VF context descriptor or native proxy pool before attach");
		return false;
	}

	const bool attached = FunctionCast(vfAttachContextDesc,
	                                   callback->oVfAttachContextDesc)(that,
	                                                                    descriptor);
	if (!attached)
		return false;
	if (!vfInitContextBridge()) {
		FunctionCast(vfDetachContextDesc,
		             callback->oVfDetachContextDesc)(that, descriptor);
		return false;
	}

	int32_t slot = -1;
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
				if (entry.refCount == 0xFFFFU || entry.contextBacking != contextBacking ||
				    entry.descriptorLo != descriptorLo ||
				    entry.engineClass != descriptorAttributes.gucClass ||
				    entry.engineInstance != engineInstance) {
					IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
					vfMarkProtocolFault("context reference overflow or LRCA identity mismatch");
					FunctionCast(vfDetachContextDesc, callback->oVfDetachContextDesc)(
						that, descriptor);
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
				entry.lrcaPage = lrcaPage;
				entry.descriptorLo = descriptorLo;
				entry.refCount = 1;
				entry.engineClass = descriptorAttributes.gucClass;
				entry.engineInstance = static_cast<uint8_t>(engineInstance);
				entry.enablePending = false;
				entry.disablePending = false;
				contextBacking->retain();
				entry.contextBacking = contextBacking;
				entry.state = kVfGucContextRegistering;
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
			FunctionCast(vfDetachContextDesc,
			             callback->oVfDetachContextDesc)(that, descriptor);
			return false;
		}
	}
	if (slot < 0) {
		SYSLOG("ngreen", "V230: exhausted %u direct GuC context IDs",
		       gVfContextCapacity);
		FunctionCast(vfDetachContextDesc,
		             callback->oVfDetachContextDesc)(that, descriptor);
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
	const bool registered =
		vfPrepareContextMemoryIrq(contextBacking,
		                          callback->vfSharedMappedBufferGetVirtualAddress) &&
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
		if (registered) {
			const uint32_t deregister[] = {
				kGucActionDeregisterContext, gucId,
			};
			(void)vfSendCtbFastAction(that, deregister, arrsize(deregister), transportFence);
			(void)vfWaitForContextState(that, gucId, kVfGucContextTombstone);
		}
		interruptState = IOSimpleLockLockDisableInterrupt(gVfContextLock);
		if (gVfContexts[gucId].state == kVfGucContextTombstone)
			gVfContexts[gucId].refCount = 0;
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
		FunctionCast(vfDetachContextDesc,
		             callback->oVfDetachContextDesc)(that, descriptor);
		vfReleaseRetiredContextBacking(gucId);
		return false;
	}

	if (gVfContextLifecycleLogs++ < 64) {
		SYSLOG("ngreen", "V230: registered GuC context id=%u LRCA=%08x:%08x class=%u instance=%u",
		       gucId, descriptorHi, descriptorLo, engineClass, engineInstance);
	}
	return true;
}

void Gen11::vfDetachContextDesc(void *that, const uint32_t *descriptor) {
	VfContextOperationGuard operationGuard;
	const bool postShutdown = !operationGuard;
	if (!that || !descriptor || !gVfGGTTReady || !gVfContextLock || !gVfContexts) {
		vfMarkProtocolFault("VF detach without valid context bookkeeping");
		return;
	}
	PANIC_COND(postShutdown && !vfWaitForContextShutdown(that), "ngreen",
		"VF context detach raced an incomplete device shutdown");
	// Native detach indexes its private pool before its releaseContextId call.
	// A void teardown cannot safely skip that bookkeeping and let DMA backing
	// disappear, so fail-stop instead of returning on a corrupted pool snapshot.
	PANIC_COND(!vfLegacyProxyPoolValid(
		that, callback->vfSharedMappedBufferGetVirtualAddress), "ngreen",
		"Cannot safely retire VF context through invalid native proxy pool");

	const auto descriptorValue = NGContextDescriptor::read(descriptor);
	const uint32_t descriptorLo = descriptorValue.low;
	const uint32_t lrcaPage = descriptorLo & 0xFFFFF000U;
	if (postShutdown) {
		// Firmware ownership was retired by the shutdown sweep before CTB was
		// sealed. Late Apple object destruction therefore performs only native
		// proxy bookkeeping and releases our pin on the final native reference;
		// it must never attempt another H2G request through a stopped transport.
		int32_t shutdownSlot = -1;
		bool finalReference = false;
		bool invalidShutdownRecord = false;
		const IOInterruptState shutdownState =
			IOSimpleLockLockDisableInterrupt(gVfContextLock);
		shutdownSlot = vfFindContextLocked(lrcaPage);
		if (shutdownSlot >= 0) {
			auto &entry = gVfContexts[shutdownSlot];
			invalidShutdownRecord =
				entry.state != kVfGucContextTombstone || entry.refCount == 0;
			if (!invalidShutdownRecord) {
				entry.refCount--;
				finalReference = entry.refCount == 0;
			}
		}
		IOSimpleLockUnlockEnableInterrupt(gVfContextLock, shutdownState);
		PANIC_COND(shutdownSlot < 0 || invalidShutdownRecord, "ngreen",
			"Late VF detach has no safely quiesced context record");
		FunctionCast(vfDetachContextDesc,
		             callback->oVfDetachContextDesc)(that, descriptor);
		if (finalReference)
			vfReleaseRetiredContextBacking(
				static_cast<uint16_t>(shutdownSlot));
		return;
	}
	int32_t slot = -1;
	VfGucContextState state = kVfGucContextEmpty;
	bool issueDisable = false;
	bool issueDeregister = false;
	VfContextQueueGuard queue(that);
	if (!queue.get()) {
		vfMarkProtocolFault("context retirement without pinned H2G queue");
		return;
	}
	IOInterruptState interruptState =
		IOSimpleLockLockDisableInterrupt(gVfContextLock);
	slot = vfFindContextLocked(lrcaPage);
	if (slot >= 0) {
		auto &entry = gVfContexts[slot];
		if (!entry.refCount) {
			IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
			vfMarkProtocolFault("duplicate final context detach");
			return;
		}
		if (entry.refCount > 1) {
			entry.refCount--;
			IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
			queue.unlock();
			FunctionCast(vfDetachContextDesc,
			             callback->oVfDetachContextDesc)(that, descriptor);
			return;
		}
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
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, interruptState);
	queue.unlock();

	if (slot < 0) {
		SYSLOG("ngreen", "V230: detach could not find LRCA 0x%08x", lrcaPage);
		FunctionCast(vfDetachContextDesc,
		             callback->oVfDetachContextDesc)(that, descriptor);
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
	} else if (gVfContextLifecycleLogs++ < 64) {
		SYSLOG("ngreen", "V230: deregistered GuC context id=%u LRCA=0x%08x",
		       gucId, lrcaPage);
	}
	// The retained IGMappedBuffer reference above is the safety boundary: if
	// GuC teardown timed out, Apple's bookkeeping may be detached but the LRCA
	// pages and GGTT mapping remain pinned and this GuC ID is quarantined.
	FunctionCast(vfDetachContextDesc,
	             callback->oVfDetachContextDesc)(that, descriptor);
	// Keep the tombstone's LRCA/backing visible to attach waiters until the
	// native proxy bookkeeping is retired, not merely until firmware replies.
	if (deregistered)
		vfReleaseRetiredContextBacking(gucId);
}

bool Gen11::vfSubmitWorkItem(void *that, unsigned int legacyContextId,
	                         const uint32_t *descriptor, IGHwCsType hwCsType,
	                         unsigned int channelId, unsigned int ringSequence,
	                         unsigned int ringTail) {
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

	const bool memIrqReady = gVfMemIrqConfigured && gVfCtbCpuBase;
	if (!memIrqReady) {
		// A bootstrap stamp can precede CTB registration.  It cannot be safely
		// submitted because MODE_DONE and completion interrupts would be
		// unobservable. No command was submitted, so reporting success here
		// would fabricate progress and leave a completion stamp outstanding.
		static uint32_t bootstrapLogs = 0;
		if (bootstrapLogs++ < 16) {
			SYSLOG("ngreen", "V237: suppressed pre-memory-IRQ VF bootstrap submit LRCA=0x%08x",
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
	IOInterruptState admissionState = IOSimpleLockLockDisableInterrupt(gVfContextLock);
	const int32_t admittedSlot = vfFindContextLocked(lrcaPage);
	const bool admitted = admittedSlot >= 0 && gVfContexts[admittedSlot].refCount &&
		gVfContexts[admittedSlot].descriptorLo == descriptorLo &&
		(gVfContexts[admittedSlot].state == kVfGucContextRegistered ||
		 gVfContexts[admittedSlot].state == kVfGucContextDisabled ||
		 gVfContexts[admittedSlot].state == kVfGucContextEnabled);
	OSObject *admittedBacking = admitted ? gVfContexts[admittedSlot].contextBacking : nullptr;
	IOSimpleLockUnlockEnableInterrupt(gVfContextLock, admissionState);
	if (!admitted)
		return false;

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
	auto *hardwareContext = const_cast<uint8_t *>(
		reinterpret_cast<const uint8_t *>(descriptor) -
		kVfContextDescriptorOffset);
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
	if ((ringControl & kRingControlValid) == 0 ||
	    (ringTail & (sizeof(uint64_t) - 1U)) != 0 || ringTail >= ringSize) {
		SYSLOG("ngreen", "V233: rejected ring tail=0x%x ctl=0x%08x size=0x%x LRCA=0x%08x",
		       ringTail, ringControl, ringSize, descriptorLo);
		return false;
	}
	const uint32_t previousRingTail = *ringTailField;
	*ringTailField = ringTail;
	OSSynchronizeIO();

	static uint32_t contextImageLogs = 0;
	if (contextImageLogs++ < 16) {
		const auto *state = reinterpret_cast<volatile uint32_t *>(
			contextImage + kContextRegisterStateOffset);
		SYSLOG("ngreen", "V234: LRCA image desc=%08x:%08x ctrl=%08x head=%08x tail=%08x start=%08x ctl=%08x",
		       descriptorHi, descriptorLo, state[3], state[5], state[7],
		       state[9], state[11]);
	}

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

	const uint16_t gucId = static_cast<uint16_t>(slot);
	const uint32_t enableRequest[] = {
		kGucActionScheduleContextModeSet, gucId, kGucContextEnable,
	};
	const uint32_t scheduleRequest[] = {
		kGucActionScheduleContext, gucId,
	};
	uint32_t transportFence = 0;
	bool submitted = enable ?
		vfSendCtbFastAction(that, enableRequest, arrsize(enableRequest), transportFence, queue.get()) :
		vfSendCtbFastAction(that, scheduleRequest, arrsize(scheduleRequest), transportFence, queue.get());

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

	queue.unlock();
	static uint32_t submitLogs = 0;
	if (submitLogs++ < 64 || !submitted) {
		SYSLOG("ngreen", "V233: direct submit id=%u LRCA=0x%08x type=%u tail=0x%x old=0x%x channel=%u seq=%u enable=%d ret=%d fence=%u",
		       gucId, descriptorLo, static_cast<unsigned int>(hwCsType),
		       ringTail, previousRingTail, channelId, ringSequence, enable,
		       submitted, transportFence);
	}
	return submitted;
}

bool Gen11::vfCtbInitWithAccelerator(void *that, void *accelerator) {
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

void Gen11::vfCtbFree(void *that) {
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

void *Gen11::vfCtbMappedBufferWithOptions(void *accelTask, unsigned long size,
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

void Gen11::vfCtbChannelInit(void *that) {
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

void Gen11::vfInvalidateTLB(void *that) {
	(void)vfInvalidateTLBSync(that);
}

void Gen11::vfBaseInvalidateTLB(const void *that) {
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
	if (!gVfHardwareGuc || !vfInvalidateTLBSync(gVfHardwareGuc))
		vfMarkProtocolFault("IGGuC GGTT invalidation did not complete");
}

bool Gen11::vfCtbGucToHostAction(void *that, uint32_t *message) {
	if (!that || !message || !gVfGGTTReady || !gVfCtbCpuBase || gVfCtbStopped)
		return false;
	if (!vfCanUseSleepingLock())
		return false;

	// Apple's consumer masks length to five bits and trusts descriptor size.
	// Its software-interrupt caller supplies only 32 dwords of stack space.
	// Validate the modern frame before copying, with a fixed allocation bound.
	auto *lock = getMember<IOLock *>(that, 0x20);
	auto *descriptor = getMember<volatile uint32_t *>(that, 0x58);
	auto *buffer = getMember<volatile uint32_t *>(that, 0x60);
	if (!lock || descriptor != reinterpret_cast<volatile uint32_t *>(
	        gVfCtbCpuBase + kVfCtbG2HDescOffset) ||
	    buffer != reinterpret_cast<volatile uint32_t *>(
	        gVfCtbCpuBase + kVfCtbG2HBufferOffset)) {
		vfMarkProtocolFault("G2H CTB mapping mismatch");
		return false;
	}
	IOLockLock(lock);
	OSSynchronizeIO();
	constexpr uint32_t ringDwords = kVfCtbG2HBufferBytes / sizeof(uint32_t);
	uint32_t head = descriptor[4];
	const uint32_t tail = descriptor[5];
	if (!NGGuCRing::validDescriptor(descriptor[3], kVfCtbG2HBufferBytes,
	                                head, tail, descriptor[6])) {
		IOLockUnlock(lock);
		vfMarkProtocolFault("invalid G2H CTB descriptor");
		return false;
	}
	if (head == tail) {
		IOLockUnlock(lock);
		return false;
	}
	if (!NGGuCRing::readFrame(buffer, ringDwords, head, tail, message, 32)) {
		IOLockUnlock(lock);
		vfMarkProtocolFault("invalid or oversized G2H CTB frame");
		return false;
	}
	OSSynchronizeIO();
	descriptor[4] = head;
	OSSynchronizeIO();
	IOLockUnlock(lock);

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

	// MODE_SET and DEREGISTER complete asynchronously.  Consume their v70
	// lifecycle payload before handing the event to Apple's legacy interrupt
	// parser, which has no knowledge of either action.  This is the point that
	// makes ID reuse safe: a slot remains unavailable until GuC confirms that it
	// no longer references the old LRCA.
	const uint32_t action = hxg & 0xFFFFU;
	if (!response && (hxg & kGucOriginGuc) != 0 &&
	    ((action == kGucActionTlbInvalidationDone && length != 2) ||
	     (action == kGucActionScheduleContextModeDone && length != 3) ||
	     (action == kGucActionDeregisterContextDone && length != 2))) {
		SYSLOG("ngreen", "V238: malformed GuC completion action=0x%04x len=%u",
		       action, length);
		vfMarkProtocolFault("malformed GuC lifecycle completion");
		return false;
	}
	// Match intel_guc_ct.c:ct_handle_event: return reserved receive space as
	// soon as a structurally valid completion leaves G2H, before interpreting
	// its lifecycle payload. A late/stale event must not leak credits and block
	// transport drain forever; an unsolicited event still trips the accounting
	// underflow guard and is then rejected by the state checks below.
	if (action == kGucActionTlbInvalidationDone ||
	    action == kGucActionScheduleContextModeDone ||
	    action == kGucActionDeregisterContextDone)
		vfReleaseG2HCredits(length + 1U); // include the CT transport header
	if (!response && (hxg & kGucOriginGuc) != 0 &&
	    action == kGucActionTlbInvalidationDone && length >= 2) {
		const uint32_t seqno = message[2];
		if (gVfTlbWaitActive && seqno == gVfTlbWaitSeqno &&
		    OSCompareAndSwap(seqno - 1U, seqno, &gVfTlbDoneSeqno)) {
			OSSynchronizeIO();
		} else {
			static uint32_t staleTlbLogs = 0;
			if (staleTlbLogs++ < 16) {
				SYSLOG("ngreen", "V237: stale VF TLB completion seq=%u waiting=%u active=%u",
				       seqno, gVfTlbWaitSeqno, gVfTlbWaitActive);
			}
		}
	}
	if (!response && (hxg & kGucOriginGuc) != 0 &&
	    (action == kGucActionContextResetNotification ||
	     action == kGucActionEngineFailureNotification)) {
		const uint32_t payload0 = length >= 2 ? message[2] : 0;
		const uint32_t payload1 = length >= 3 ? message[3] : 0;
		const uint32_t payload2 = length >= 4 ? message[4] : 0;
		SYSLOG("ngreen", "V234: GuC failure event action=0x%04x len=%u payload=%08x:%08x:%08x",
		       action, length, payload0, payload1, payload2);
		vfMarkProtocolFault(action == kGucActionContextResetNotification ?
		                         "GuC context reset notification" :
		                         "GuC engine failure notification");
	}
	if (!response && (hxg & kGucOriginGuc) != 0 && gVfContextLock &&
	    gVfContexts &&
	    ((action == kGucActionScheduleContextModeDone && length >= 3) ||
	     (action == kGucActionDeregisterContextDone && length >= 2))) {
		const uint32_t gucId = message[2];
		bool handled = false;
		VfGucContextState newState = kVfGucContextEmpty;
		if (gucId < gVfContextCapacity) {
			const IOInterruptState interruptState =
				IOSimpleLockLockDisableInterrupt(gVfContextLock);
			auto &entry = gVfContexts[gucId];
			if (action == kGucActionScheduleContextModeDone) {
				// MODE_SET completions are ordered.  An enable can already be
				// followed by a disable when teardown races the first submission;
				// retire the enable token first instead of mistaking its MODE_DONE
				// for completion of the later disable.
				if (entry.enablePending) {
					entry.enablePending = false;
					if (entry.state == kVfGucContextPendingEnable)
						entry.state = kVfGucContextEnabled;
					handled = true;
				} else if (entry.disablePending) {
					entry.disablePending = false;
					entry.state = kVfGucContextDisabled;
					handled = true;
				}
			} else if (entry.state == kVfGucContextPendingDeregister) {
				// Leave LRCA/backing associated until the retirement owner has
				// also finished every native detach; attach must still wait. Normal
				// final detach already set refCount to zero, while the device-wide
				// shutdown sweep deliberately preserves outstanding native owners.
				entry.engineClass = 0;
				entry.engineInstance = 0;
				entry.enablePending = false;
				entry.disablePending = false;
				entry.state = kVfGucContextTombstone;
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

bool Gen11::vfInterruptFilterHandler(void *that, void *eventSource) {
	(void)eventSource;
	// Identity is established before enabling interrupts. Never probe/map BARs
	// from a filter, or interpret an unready VF as a physical device.
	if (!gVfGGTTReady)
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

void Gen11::vfSoftwareGuCInterrupt(void *that, IOInterruptEventSource *source, int count) {
	(void)count;
	if (!that || !gVfGGTTReady)
		return;
	VfIrqCallbackGuard irqGuard;
	if (!irqGuard || !gVfCtbCpuBase || gVfCtbStopped)
		return;
	auto *ctb = getMember<void *>(that, 0xA10);
	if (!ctb || !callback->vfCtbSoftwareInterrupt) {
		vfMarkProtocolFault("missing VF software CTB dispatcher");
		return;
	}
	using CtbInterrupt = uint32_t (*)(void *);
	const auto consume = reinterpret_cast<CtbInterrupt>(callback->vfCtbSoftwareInterrupt);
	for (uint32_t drained = 0; drained < 256; drained++) {
		uint32_t before = 0;
		if (!vfG2HCtbPending(before))
			return;
		// Retain the native consumer call boundary, but do not interpret the
		// returned modern HXG action as legacy log-flush status bits.
		(void)consume(ctb);
		uint32_t after = 0;
		if (vfG2HCtbPending(after) && after == before) {
			static uint32_t stalledLogs = 0;
			if (stalledLogs++ < 16) {
				SYSLOG("ngreen", "V236: stopped stalled VF G2H drain at head=%u",
				       after);
			}
			vfMarkProtocolFault("stalled GuC G2H descriptor");
			return;
		}
	}
	uint32_t head = 0;
	if (vfG2HCtbPending(head)) {
		if (source)
			source->interruptOccurred(nullptr, nullptr, 0);
		else
			vfMarkProtocolFault("G2H drain needs a software event source");
	}
}

void Gen11::vfEnableInterrupts(void *that) {
	(void)that;
	if (gVfIdentity != VfIdentity::Virtual || gVfProtocolFault ||
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
	VfIrqCallbackGuard irqGuard;
	if (!irqGuard)
		return;
	*snapshot = vfConsumeMemoryInterrupts();
}



void Gen11::wrapSafeForceWake(void *that, bool set, uint32_t dom)
{
	forceWake(that, set, dom, 0);
}

void Gen11::forceWake(void *that, bool set, uint32_t dom, uint8_t ctx)
{
	(void)that;
	(void)set;
	(void)dom;
	(void)ctx;
	// i915 creates no force-wake domains for a VF. Power ownership remains
	// with the PF/GuC, so every routed VF entry point is an intentional no-op.
	static bool logged = false;
	if (!logged) {
		logged = true;
		SYSLOG("ngreen", "V240: guest force-wake suppressed for SR-IOV VF");
	}
}
void Gen11::injectAcceleratorPersonality()
{
	if (this->acceleratorPersonalityInjected) {
		DBGLOG("ngreen", "injectAcceleratorPersonality: already injected, skipping");
		return;
	}

	SYSLOG("ngreen", "injectAcceleratorPersonality: registering TGL IntelAccelerator into IOCatalogue");

	auto *dict = OSDictionary::withCapacity(24);
	if (!dict) return;

	const char *bundleId = "com.xxxxx.driver.AppleIntelTGLGraphics";
	const char *mtlName  = "AppleIntelTGLGraphicsMTLDriver";
	const char *glName   = "AppleIntelTGLGraphicsGLDriver";
	const char *vaName   = "AppleIntelTGLGraphicsVADriver";

	// Basic matching properties
	auto *bi  = OSString::withCString(bundleId);
	auto *cls = OSString::withCString("IntelAccelerator");
	auto *mc  = OSString::withCString("IOAccelerator");
	auto *pv  = OSString::withCString("IOPCIDevice");
	auto *pcm = OSString::withCString("0x03000000&0xff000000");
	auto *pm  = OSString::withCString("0x9a498086");
	auto *ps  = OSNumber::withNumber(static_cast<unsigned long long>(1000), 32);

	dict->setObject("CFBundleIdentifier", bi);
	dict->setObject("IOClass", cls);
	dict->setObject("IOMatchCategory", mc);
	dict->setObject("IOProviderClass", pv);
	dict->setObject("IOPCIClassMatch", pcm);
	dict->setObject("IOPCIPrimaryMatch", pm);
	dict->setObject("IOProbeScore", ps);

	OSSafeReleaseNULL(bi);
	OSSafeReleaseNULL(cls);
	OSSafeReleaseNULL(mc);
	OSSafeReleaseNULL(pv);
	OSSafeReleaseNULL(pcm);
	OSSafeReleaseNULL(pm);
	OSSafeReleaseNULL(ps);

	// V44: GPU driver bundle names — required by IOAcceleratorFamily2 and WindowServer
	auto *mtl = OSString::withCString(mtlName);
	auto *gl  = OSString::withCString(glName);
	auto *dvd = OSString::withCString(vaName);
	auto *src = OSString::withCString("0.0.0.0.0");
	auto *vaCodec  = OSString::withCString("Gen10");
	auto *vaScaler = OSString::withCString("Gen10");
	auto *vaBGRA   = OSString::withCString("Gen10");
	auto *vaRendID = OSNumber::withNumber(static_cast<unsigned long long>(17301568), 32); // 0x1084000

	dict->setObject("MetalPluginName", mtl);
	dict->setObject("IOGLBundleName", gl);
	dict->setObject("IODVDBundleName", dvd);
	dict->setObject("IOSourceVersion", src);
	dict->setObject("IOGVACodec", vaCodec);
	dict->setObject("IOGVAScaler", vaScaler);
	dict->setObject("IOGVABGRAEnc", vaBGRA);
	dict->setObject("IOVARendererID", vaRendID);

	OSSafeReleaseNULL(mtl);
	OSSafeReleaseNULL(gl);
	OSSafeReleaseNULL(dvd);
	OSSafeReleaseNULL(src);
	OSSafeReleaseNULL(vaCodec);
	OSSafeReleaseNULL(vaScaler);
	OSSafeReleaseNULL(vaBGRA);
	OSSafeReleaseNULL(vaRendID);

	// IOAccelerator2D plugin type (required for 2D acceleration matching)
	auto *pluginDict = OSDictionary::withCapacity(1);
	if (pluginDict) {
		auto *pluginName = OSString::withCString("IOAccelerator2D.plugin");
		pluginDict->setObject("ACCF0000-0000-0000-0000-000a2789904e", pluginName);
		OSSafeReleaseNULL(pluginName);
		dict->setObject("IOCFPlugInTypes", pluginDict);
		pluginDict->release();
	}

	// V77: Display pipe capabilities forced to 0 to prevent WS GPU compositing crash.
	auto *dpCaps = OSDictionary::withCapacity(2);
	if (dpCaps) {
		auto *dpSupp = OSNumber::withNumber(static_cast<unsigned long long>(0), 32);
		auto *trSupp = OSNumber::withNumber(static_cast<unsigned long long>(0), 32);
		dpCaps->setObject("DisplayPipeSupported", dpSupp);
		dpCaps->setObject("TransactionsSupported", trSupp);
		OSSafeReleaseNULL(dpSupp);
		OSSafeReleaseNULL(trSupp);
		dict->setObject("IOAccelDisplayPipeCapabilities", dpCaps);
		dpCaps->release();
	}

	SYSLOG("ngreen", "injectAcceleratorPersonality: personality has %u properties", dict->getCount());

	auto *array = OSArray::withCapacity(1);
	if (array) {
		array->setObject(dict);
		if (gIOCatalogue) {
			bool ok = gIOCatalogue->addDrivers(array, true);
			SYSLOG("ngreen", "injectAcceleratorPersonality: addDrivers returned %d", ok);
			this->acceleratorPersonalityInjected = ok;
		} else {
			SYSLOG("ngreen", "injectAcceleratorPersonality: gIOCatalogue is null!");
		}
		array->release();
	}
	dict->release();
}

// The VF has no guest-owned INSTDONE register. Its watchdog query is true only
// after every tracked direct-LRCA context has reached an idle lifecycle state.
bool Gen11::wrapIGScheduler5IsGpuIdle(const void *that) {
	(void)that;
	return vfKnownIdleSnapshot();
}

bool Gen11::wrapIGScheduler4IsGpuIdle(const void *that) {
	(void)that;
	return vfKnownIdleSnapshot();
}

//SIGNATURES GFX
//ulong __thiscall IntelAccelerator::start(IntelAccelerator *this,IOService *param_1)
//undefined8 __thiscall IGAccelDevice::deviceStart(IGAccelDevice *this)
//void __thiscall IntelAccelerator::populateResetRegisterList(IntelAccelerator *this)
//IGAccelTask * IGAccelTask::withOptions(IntelAccelerator *param_1)
//IGHardwareExtendedContext * __thiscall IGAccelTask::getBlit3DContext(IGAccelTask *this,bool param_1)
//undefined8 __thiscall IGHardwareExtendedContext::initWithOptions (IGHardwareExtendedContext *this,IGAccelTask *param_1, IGHardwareExtendedContextParams *param_2)
//undefined8 blit3d_init_ctx(IGHardwareBlit3DContext *param_1)
//void blit3d_initialize_scratch_space(IGAccelSysMemory *param_1)
//void __thiscall IGHardwareBlit3DContext::initialize(IGHardwareBlit3DContext *this)
//ulong __thiscall IntelAccelerator::startGraphicsEngine(IntelAccelerator *this)
//undefined8 __thiscall IntelAccelerator::stopGraphicsEngine(IntelAccelerator *this)
//void __thiscall IGAccelSegmentResourceList::initBlitUsage(IGAccelSegmentResourceList *this)
//ulong IntelAccelerator::submitBlit (blit3d_params_t *param_1,IGVector *param_2,IGAccelTask *param_3,bool param_4)
