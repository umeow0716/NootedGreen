//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_gen11.hpp"
#include "kern_guc_ring.hpp"
#include "kern_gpu_capabilities.hpp"
#include "kern_ggtt_bounds.hpp"
#include "kern_ggtt_rotation.hpp"
#include "kern_vf_irq_gate.hpp"
#include "kern_vf_context_shutdown.hpp"
#include "kern_vf_submission_gate.hpp"
#include "kern_context_pool.hpp"
#include "kern_context_descriptor.hpp"
#include "kern_workqueue_unwind.hpp"
#include "kern_binary_identity.hpp"
#include "AppleIntelParams.hpp"
#include <Headers/kern_api.hpp>
#include "kern_green.hpp"
#include "IntelDPLinkTraining.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread_call.h>
#include <kern/sched_prim.h>
#include <i386/machine_routines.h>

// ==== 6 kextInfos: ICL fallback + dual TGL identities (com.xxxxx and com.apple) from /Library/Extensions ====
//trivial
// ICL FB — com.apple (fallback path)
static const char *pathsICLFB[] = {
    "/System/Library/Extensions/AppleIntelICLLPGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelICLLPGraphicsFramebuffer",
};
static KernelPatcher::KextInfo kextG11FB {"com.apple.driver.AppleIntelICLLPGraphicsFramebuffer", pathsICLFB, 1, {}, {},
    KernelPatcher::KextInfo::Unloaded};

// ICL HW — com.apple (fallback path)
static const char *pathsICLHW[] = {
    "/System/Library/Extensions/AppleIntelICLGraphics.kext/Contents/MacOS/AppleIntelICLGraphics",
};
static KernelPatcher::KextInfo kextG11HW {"com.apple.driver.AppleIntelICLGraphics", pathsICLHW, 1, {}, {},
    KernelPatcher::KextInfo::Unloaded};

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
// Legacy relay helpers below remain inactive until per-GT IP/ABI discovery
// and lifetime handling are implemented; BAR size is not the discriminator.
constexpr uint32_t kVfGGTTPteBase = 0x800000;
constexpr uint32_t kVfGGTTPteBytes = 0x800000;
constexpr uint32_t kVfDirectBar0Bytes = kVfGGTTPteBase + kVfGGTTPteBytes;
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
constexpr uint32_t kGucActionMmioRelay = 0x5005;
constexpr uint32_t kGucActionHost2GucSelfCfg = 0x0508;
constexpr uint32_t kGucActionHost2GucControlCtb = 0x4509;
constexpr uint32_t kGucActionRegisterContext = 0x4502;
constexpr uint32_t kGucActionDeregisterContext = 0x4503;
constexpr uint32_t kGucActionDeregisterContextDone = 0x4600;
constexpr uint32_t kGucActionScheduleContext = 0x1000;
constexpr uint32_t kGucActionScheduleContextModeSet = 0x1001;
constexpr uint32_t kGucActionScheduleContextModeDone = 0x1002;
constexpr uint32_t kGucActionContextResetNotification = 0x1008;
constexpr uint32_t kGucActionEngineFailureNotification = 0x1009;
constexpr uint32_t kGucActionUpdateContextPolicies = 0x100B;
constexpr uint32_t kGucActionTlbInvalidation = 0x7000;
constexpr uint32_t kGucActionTlbInvalidationDone = 0x7001;
constexpr uint32_t kGucSelfCfgMemIrqStatusAddr = 0x0900;
constexpr uint32_t kGucSelfCfgMemIrqSourceAddr = 0x0901;
constexpr uint32_t kGucSelfCfgH2GCtbAddr = 0x0902;
constexpr uint32_t kGucSelfCfgH2GCtbDescAddr = 0x0903;
constexpr uint32_t kGucSelfCfgH2GCtbSize = 0x0904;
constexpr uint32_t kGucSelfCfgG2HCtbAddr = 0x0905;
constexpr uint32_t kGucSelfCfgG2HCtbDescAddr = 0x0906;
constexpr uint32_t kGucSelfCfgG2HCtbSize = 0x0907;
constexpr uint32_t kVf2PfUpdateGGTTOpcode = 0x02;
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
uint64_t *gVfGGTTShadow = nullptr;
uint64_t gVfGGTTBase = 0;
uint64_t gVfGGTTSize = 0;
uint32_t gVfContextCount = 0;
uint32_t gVfDoorbellCount = 0;
void *gVfGlobalPageTable = nullptr;
void *gVfHardwareGuc = nullptr;
bool gVfGGTTReady = false;
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
bool gVfDirectGGTT = false;
bool gVfBinderReady = false;
uint32_t gVfRelayFailureLogs = 0;
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
	if (!vfCanUseSleepingLock())
		return false;
	if (!guc || !request || requestLength == 0 || requestLength > 31 ||
	    (request[0] & (kGucOriginGuc | kGucTypeMask)))
		return false;
	const uint32_t action = request[0] & 0xFFFFU;
	const bool retirementAction =
		(action == kGucActionDeregisterContext) ||
		(action == kGucActionTlbInvalidation) ||
		(action == kGucActionScheduleContextModeSet &&
		 requestLength >= 3 && request[2] == kGucContextDisable);
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
	const uint32_t responseCredits = action == kGucActionScheduleContextModeSet ? 4U :
		(action == kGucActionDeregisterContext || action == kGucActionTlbInvalidation ? 3U : 0U);
	for (uint32_t retry = 0; retry < 8; retry++) {
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
		const uint32_t used = valid ?
			(tail >= head ? tail - head : size - head + tail) : size;
		if (valid && needed < size - used && vfReserveG2HCredits(responseCredits)) {
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
		IODelay(50U << retry);
	}
	return false;
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
			if (gVfRelayFailureLogs++ < 16)
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

bool vfBootstrapBinder()
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
	// mapping is a failure, not permission to try another generation's relay.
	auto *cb = NGreen::callback;
	if (!cb || !cb->setRMMIOIfNecessary())
		return false;
	const uint64_t bar0Length = cb->getRMMIOLength();
	if (cb->getRMMIOAddress() && bar0Length >= kVfDirectBar0Bytes) {
		gVfDirectGGTT = true;
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

bool vfRelayPTEs(uint32_t offset, uint32_t mode, uint32_t copies, uint64_t pte)
{
	if (!gVfBinderReady || copies > 1023)
		return false;
	uint32_t request[4] = {
		(0xFU << 24) | (kVf2PfUpdateGGTTOpcode << 16) | kGucActionMmioRelay,
		(offset << 12) | ((mode & 3U) << 10) | copies,
		static_cast<uint32_t>(pte),
		static_cast<uint32_t>(pte >> 32),
	};
	uint32_t response[4] = {};
	if (!vfGucSendMMIO(request, 4, response) ||
	    ((response[0] >> 24) & 0xFU) != 0xFU ||
	    (response[0] & 0xFFFFFFU) != copies + 1) {
		if (gVfRelayFailureLogs++ < 16)
			SYSLOG("ngreen", "V217: GGTT relay failed off=0x%x mode=%u copies=%u reply=0x%08x",
			       offset, mode, copies, response[0]);
		return false;
	}
	return true;
}

bool vfSyncShadowRange(const NGIGAddressRange &range)
{
	if (!gVfBinderReady || !gVfGGTTShadow || range.length == 0)
		return range.length == 0;
	if (!NGGgtt::contains(gVfGGTTBase, gVfGGTTSize, range.start, range.length)) {
		if (gVfRelayFailureLogs++ < 16)
			SYSLOG("ngreen", "V217: refusing GGTT range outside VF assignment [0x%llx,+0x%llx]",
			       static_cast<unsigned long long>(range.start),
			       static_cast<unsigned long long>(range.length));
		return false;
	}

	uint32_t globalPage = static_cast<uint32_t>(range.start >> 12);
	uint32_t relativePage = static_cast<uint32_t>((range.start - gVfGGTTBase) >> 12);
	uint32_t remaining = static_cast<uint32_t>(range.length >> 12);
	while (remaining) {
		const uint64_t first = gVfGGTTShadow[globalPage];
		uint32_t run = 1;
		uint32_t mode = 0; // duplicate
		if (remaining > 1) {
			const uint64_t next = gVfGGTTShadow[globalPage + 1];
			const uint64_t addressMask = 0x00007FFFFFF000ULL;
			if ((next & ~addressMask) == (first & ~addressMask) &&
			    (next & addressMask) == (first & addressMask) + 0x1000ULL)
				mode = 1; // replicate consecutive physical pages
		}
		while (run < remaining && run < 1024) {
			const uint64_t candidate = gVfGGTTShadow[globalPage + run];
			if (mode == 0) {
				if (candidate != first) break;
			} else {
				const uint64_t addressMask = 0x00007FFFFFF000ULL;
				if ((candidate & ~addressMask) != (first & ~addressMask) ||
				    (candidate & addressMask) != (first & addressMask) + 0x1000ULL * run)
					break;
			}
			run++;
		}
		if (!vfRelayPTEs(relativePage, mode, run - 1, first))
			return false;
		globalPage += run;
		relativePage += run;
		remaining -= run;
	}
	return true;
}
} // namespace

bool ngVfGGTTBinderActive()
{
	// Historical name retained for the shared Gen11 interface.  Callers use
	// this as the "do not touch physical GGTT/GMADR" predicate, which applies
	// to both the direct-PTE and GuC-relay VF transports.
	return gVfIdentity == VfIdentity::Virtual || gVfGGTTReady;
}

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

bool ngVfGGTTRead32(unsigned long reg, UInt32 &value)
{
	if (!gVfBinderReady || !gVfGGTTShadow || reg < kVfGGTTPteBase ||
	    reg >= kVfGGTTPteBase + kVfGGTTPteBytes)
		return false;
	const uint32_t byteOffset = static_cast<uint32_t>(reg - kVfGGTTPteBase);
	const uint64_t pte = gVfGGTTShadow[byteOffset >> 3];
	value = (byteOffset & 4U) ? static_cast<uint32_t>(pte >> 32) :
	                           static_cast<uint32_t>(pte);
	return true;
}

bool ngVfGGTTWrite32(unsigned long reg, UInt32 value)
{
	if (!gVfBinderReady || !gVfGGTTShadow || reg < kVfGGTTPteBase ||
	    reg >= kVfGGTTPteBase + kVfGGTTPteBytes)
		return false;
	const uint32_t byteOffset = static_cast<uint32_t>(reg - kVfGGTTPteBase);
	const uint32_t globalPage = byteOffset >> 3;
	uint64_t &pte = gVfGGTTShadow[globalPage];
	if (byteOffset & 4U) {
		pte = (pte & 0xFFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
		const uint64_t address = static_cast<uint64_t>(globalPage) << 12;
		if (address >= gVfGGTTBase && address - gVfGGTTBase < gVfGGTTSize)
			(void)vfRelayPTEs(static_cast<uint32_t>((address - gVfGGTTBase) >> 12),
			                  0, 0, pte);
	} else {
		pte = (pte & 0xFFFFFFFF00000000ULL) | value;
	}
	return true;
}

void Gen11::init() {
	callback = this;

	if (checkKernelArgument("-ngreentglfb") || checkKernelArgument("-ngreentglwithgfx")) {
		SYSLOG("ngreen", "Gen11::init: FB tier → TGL (ICL FB skipped)");
		lilu.onKextLoadForce(&kextG11FBT);
		lilu.onKextLoadForce(&kextG11FBTA);
		if (checkKernelArgument("-ngreentglwithgfx")) {
			SYSLOG("ngreen", "Gen11::init: HW tier → TGL (ICL HW skipped)");
			lilu.onKextLoadForce(&kextG11HWT);
			lilu.onKextLoadForce(&kextG11HWTA);
		}
	} else if (checkKernelArgument("-ngreentglgfx")) {
		SYSLOG("ngreen", "Gen11::init: HW tier → TGL (ICL HW skipped)");
		lilu.onKextLoadForce(&kextG11HWT);
		lilu.onKextLoadForce(&kextG11HWTA);
	} else if (checkKernelArgument("-ngreenicl")) {
		SYSLOG("ngreen", "Gen11::init: FB tier → ICL fallback");
		lilu.onKextLoadForce(&kextG11FB);
		SYSLOG("ngreen", "Gen11::init: HW tier → ICL fallback");
		lilu.onKextLoadForce(&kextG11HW);
	}
}

static bool isWEGCoexistMode() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngwegcoex", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngwegcoex");
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

static bool isV93PlaneGuardEnabled() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenv93", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngreenv93");
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
	const bool physicalFramebuffer = index == kextG11FB.loadIndex ||
		index == kextG11FBT.loadIndex || index == kextG11FBTA.loadIndex;
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
	if (kextG11FB.loadIndex == index) {
		if (this->tglFBLoaded) {
			DBGLOG("ngreen", "Skipping ICL FB — TGL FB already loaded");
			return true;
		}
		auto *activeKext = &kextG11FB;
		DBGLOG("ngreen", "init AppleIntelICLLPGraphicsFramebuffer!");
		//NGreen::callback->igfxGen = iGFXGen::Gen11;
		PANIC_COND(!NGreen::callback->setRMMIOIfNecessary(), "ngreen", "Cannot map ICL framebuffer BAR0");
		
		const bool wegCoexist = isWEGCoexistMode();
		if (wegCoexist) {
			SYSLOG("nblue", "WEG coexist mode enabled: skipping NootedBlue CDCLK route overlap");
		}

		if (wegCoexist) {
			SolveRequestPlus solveRequests[] = {
		 		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb", this->ohwConfigureCustomAUX},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "nblue",	"Failed to resolve symbols");
		} else {
			SolveRequestPlus solveRequests[] = {
		 		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb", this->ohwConfigureCustomAUX},
				{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv", this->orgProbeCDClockFrequency},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "nblue",	"Failed to resolve symbols");
		}
		
		if (wegCoexist) {
			RouteRequestPlus requests[] = {
				{"__ZN31AppleIntelFramebufferController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "nblue","Failed to route symbols");
		} else {
			RouteRequestPlus requests[] = {
				{"__ZN31AppleIntelFramebufferController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "nblue","Failed to route symbols");
		}

		//static const uint8_t f15[]= {0x00,0x02, 0x00, 0x5c, 0x8a};
		//static const uint8_t r15[]= {0x00,0x00, 0x00, 0x49, 0x9a};
		
		
		// AppleIntelFramebufferController::hwSetMode skip hwRegsNeedUpdate
		static const uint8_t f2[] = {0xE8, 0x31, 0xE5, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x3D};
		static const uint8_t r2[] = {0xE8, 0x31, 0xE5, 0xFF, 0xFF, 0x84, 0xC0, 0xEB, 0x3D};
		
		//sonoma
		static const uint8_t f2b[] = {0xE8, 0x54, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x5C};
		static const uint8_t r2b[] = {0xE8, 0x54, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0xeb, 0x5C};
		
		//sequoia
		static const uint8_t f2c[] = {0xE8, 0x74, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x5C};
		static const uint8_t r2c[] = {0xE8, 0x74, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0xeb, 0x5C};
		
		/*if (getKernelVersion() <= KernelVersion::Ventura) {
			KernelPatcher::LookupPatch patch { &kextG11FB, f2, r2, sizeof(f2), 1 };
			patcher.applyLookupPatch(&patch);
		}
		
		if (getKernelVersion() == KernelVersion::Sonoma) {
			KernelPatcher::LookupPatch patchb { &kextG11FB, f2b, r2b, sizeof(f2b), 1 };
			patcher.applyLookupPatch(&patchb);
		}
		
		if (getKernelVersion() >= KernelVersion::Sequoia) {
			KernelPatcher::LookupPatch patchc { &kextG11FB, f2c, r2c, sizeof(f2c), 1 };
			patcher.applyLookupPatch(&patchc);
		}*/
		
		

		// Variant-consistent remap for constructor entries:
		// B8 xx 00 5C 8A -> B8 xx 00 49 9A and exact C7 05 ... 02 00 5C 8A site.
		static const uint8_t kPatchPlatformRemapMovEaxFind0[] = {0xB8, 0x00, 0x00, 0x5C, 0x8A};
		static const uint8_t kPatchPlatformRemapMovEaxReplace0[] = {0xB8, 0x00, 0x00, 0x49, 0x9A};
		static const uint8_t kPatchPlatformRemapMovEaxFind1[] = {0xB8, 0x01, 0x00, 0x5C, 0x8A};
		static const uint8_t kPatchPlatformRemapMovEaxReplace1[] = {0xB8, 0x01, 0x00, 0x49, 0x9A};
		static const uint8_t kPatchPlatformRemapMovEaxFind2[] = {0xB8, 0x02, 0x00, 0x5C, 0x8A};
		static const uint8_t kPatchPlatformRemapMovEaxReplace2[] = {0xB8, 0x02, 0x00, 0x49, 0x9A};
		static const uint8_t kPatchPlatformRemapC705Find2[] = {0xC7, 0x05, 0xE9, 0x9B, 0x05, 0x00, 0x02, 0x00, 0x5C, 0x8A};
		static const uint8_t kPatchPlatformRemapC705Replace2[] = {0xC7, 0x05, 0xE9, 0x9B, 0x05, 0x00, 0x02, 0x00, 0x49, 0x9A};

		// hwSetMode: bypass hwRegsNeedUpdate result (CALL hwRegsNeedUpdate; TEST AL,AL: JE+0x62 → JMP+0x62)
		// Verified unique (1 match at 0x94055) in ICL LP le binary. Forces register reprogram unconditionally. [ICL-LP]
		static const uint8_t kPatchHwRegsNeedUpdateBypassFind[] = {0xe8, 0xe2, 0xcc, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x62};
		static const uint8_t kPatchHwRegsNeedUpdateBypassReplace[] = {0xe8, 0xe2, 0xcc, 0xff, 0xff, 0x84, 0xc0, 0xeb, 0x62};

		LookupPatchPlus const minPatches[] = {
			{&kextG11FB, kPatchPlatformRemapMovEaxFind0, kPatchPlatformRemapMovEaxReplace0, arrsize(kPatchPlatformRemapMovEaxFind0), 1},
			{&kextG11FB, kPatchPlatformRemapMovEaxFind1, kPatchPlatformRemapMovEaxReplace1, arrsize(kPatchPlatformRemapMovEaxFind1), 1},
			{&kextG11FB, kPatchPlatformRemapMovEaxFind2, kPatchPlatformRemapMovEaxReplace2, arrsize(kPatchPlatformRemapMovEaxFind2), 1},
			{&kextG11FB, kPatchPlatformRemapC705Find2, kPatchPlatformRemapC705Replace2, arrsize(kPatchPlatformRemapC705Find2), 1},
			{&kextG11FB, kPatchHwRegsNeedUpdateBypassFind, kPatchHwRegsNeedUpdateBypassReplace, arrsize(kPatchHwRegsNeedUpdateBypassFind), 1},  // hwSetMode always reprogram [ICL-LP]
		};
		
		PANIC_COND(!LookupPatchPlus::applyAll(patcher, minPatches , address, size), "ngreen", "kextG11FB Failed to apply patches!");
		//PANIC_COND
		
		DBGLOG("ngreen", "Loaded AppleIntelICLLPGraphicsFramebuffer!");
		return true;
		
		
	}	else if (kextG11FBT.loadIndex == index || kextG11FBTA.loadIndex == index) {
		this->tglFBLoaded = true;
		auto *activeKext = (kextG11FBTA.loadIndex == index) ? &kextG11FBTA : &kextG11FBT;
		PANIC_COND(!NGreen::callback->setRMMIOIfNecessary(), "ngreen", "Cannot map TGL framebuffer BAR0");
		SYSLOG("ngreen", "init AppleIntelTGLGraphicsFramebuffer");
		
		bool isprod=false;
		auto prod=patcher.solveSymbol(index, "__ZN24AppleIntelBaseController5startEP9IOService", address, size);
		if (!prod) isprod=true;
		
		if (isprod) {
			
			SolveRequestPlus solveRequests[] = {
				{"__ZN31AppleIntelFramebufferController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
				{"_gPlatformInformationList", this->gPlatformInformationList},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "ngreen",	"Failed to resolve symbols");
		}
		else
		{
			SolveRequestPlus solveRequests[] = {
				{"__ZN24AppleIntelBaseController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
				{"_gPlatformInformationList", this->gPlatformInformationList},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "ngreen",	"Failed to resolve symbols");
			
		}

		RouteRequestPlus requests[] = {
			// ...existing routes...
			//{"__ZN24AppleIntelBaseController17registerWithAICPMEPv", alwaysReturnSuccess, this->oalwaysReturnSuccess},
			// ...existing routes...
			// V204: keep the native constructor results, then install the controller's
			// captured register accessor only when that accessor is non-null.
			{"__ZN16AppleIntelScaler4initE10IGScalerID", AppleIntelScalerinit, this->oAppleIntelScalerinit},
			{"__ZN15AppleIntelPlane4initE9IGPlaneID",     AppleIntelPlaneinit,  this->oAppleIntelPlaneinit},
			// Later entry points repeat the non-null accessor repair in case construction
			// preceded framebuffer/controller publication.
			{"__ZN16AppleIntelScaler13disableScalerEb",disableScaler, this->odisableScaler},
			{"__ZN15AppleIntelPlane11enablePlaneEb",enablePlane, this->oenablePlane},
			{"__ZN16AppleIntelScaler17programPipeScalerEP21AppleIntelDisplayPath",programPipeScaler, this->oprogramPipeScaler},
			// V400: read-only logger for setupPipeScaler. Confirms whether Apple's
			// pipe scaler is downscaling (PIPE_SRCSZ vs PS_PS_WIN_SZ mismatch) — if
			// yes, that's a direct cause of the fragmented/repeated scanout.
			{"__ZN16AppleIntelScaler15setupPipeScalerEP21AppleIntelDisplayPathP10CRTCParams", setupPipeScaler, this->osetupPipeScaler},
			// V401: paramsSurfCompare — read-only. Fires per flip. Logs PLANE_CTL
			// tiling bits (mask 0xF800000), PLANE_STRIDE (+0x18), PLANE_SURF (+0x20)
			// for both old and new PLANEPARAMS, plus CRTCParams PIPE_SRCSZ.
			{"__ZN24AppleIntelBaseController17paramsSurfCompareEP10CRTCParamsS1_PN15AppleIntelPlane11PLANEPARAMSES4_", paramsSurfCompare, this->oparamsSurfCompare},
			// V402: setupDSCEngineParams — read-only. Logs entry of DSC config path.
			// Linux says DSC=off; this hook reveals whether Apple still goes through it.
			{"__ZN24AppleIntelBaseController20setupDSCEngineParamsEP21AppleIntelFramebufferP10CRTCParamsP21AppleIntelDisplayPathP29IODetailedTimingInformationV2", setupDSCEngineParams, this->osetupDSCEngineParams},
			// V403: SetupParams — master CRTCParams builder. Read-only logger of all
			// key fields after Apple populates the struct. Single chokepoint where
			// future origin-level overrides should land.
			{"__ZN24AppleIntelBaseController11SetupParamsEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsPK29IODetailedTimingInformationV2", setupParams, this->osetupParams},
			// V404: setupPipeWatermarks — runs INSIDE SetupParams before setupPipeScaler.
			// Per-pipe DBUF/watermark allocator. Prime suspect for setting PIPE_SEAM_EXCESS=0x1.
			{"__ZN24AppleIntelBaseController19setupPipeWatermarksEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParams", setupPipeWatermarks, this->osetupPipeWatermarks},
			// V405: AppleIntelPlane::configureColorPipeLine(FlipTransactionArgs*, bool)
			// Dispatches 8/10/12/12SEG gamma pipeline based on FlipTransactionArgs BPC mode.
			// Hook logs GAMMA_MODE (0x4A480) and PIPE_MISC (0x70030) before/after the call
			// to confirm Apple writes correct values for ADL-P Display 13 on our 8bpc eDP.
			{"__ZN15AppleIntelPlane22configureColorPipeLineEP19FlipTransactionArgsb", configureColorPipeLine, this->oConfigureColorPipeLine},
			// V406: AppleIntelPlane::configurePlane(FlipTransactionArgs*)
			// Patches FlipTransactionArgs+0x3c tiling enum to 2 (linear) so Apple builds
			// PLANE_CTL with bits[12:10]=000 and PLANE_STRIDE=pitch/512 natively.
			{"__ZN15AppleIntelPlane14configurePlaneEP19FlipTransactionArgs", configurePlane, this->oConfigurePlane},
			{"__ZN15AppleIntelPlane19updateRegisterCacheEv",AppleIntelPlaneupdateRegisterCache, this->oAppleIntelPlaneupdateRegisterCache},
			{"__ZN16AppleIntelScaler19updateRegisterCacheEv",AppleIntelScalerupdateRegisterCache, this->oAppleIntelScalerupdateRegisterCache},
			{"__ZN19AppleIntelPowerWell20disableDisplayEngineEv",disableDisplayEngine, this->odisableDisplayEngine},
			{"__ZN19AppleIntelPowerWell19enableDisplayEngineEv",enableDisplayEngine, this->oenableDisplayEngine},
			// V35: Removed ComboPhyEv hook — causes MCE on RPL/ADL. Firmware calibration sufficient.
			{"__ZN14AppleIntelPort16computeLaneCountEPK29IODetailedTimingInformationV2jjPj",computeLaneCount, this->ocomputeLaneCount},
			{"__ZN14AppleIntelPort21setupOptimalLaneCountEPK29IODetailedTimingInformationV2j",setupOptimalLaneCount, this->osetupOptimalLaneCount},
			// V97: Log AUX transactions to diagnose eDP link training failures on RPL
			{"__ZN14AppleIntelPort7readAUXEjPvj", wrapICLReadAUX, this->orgICLReadAUX},
			// V96: Force display online — WEG's getDisplayStatus hook (FOD) fails with
			// "err 2" on TGL kext because that symbol doesn't exist. TGL uses getOnlineInfo.
			{"__ZN21AppleIntelFramebuffer13getOnlineInfoEP21AppleIntelDisplayPathPhS2_", getOnlineInfo, this->ogetOnlineInfo},
			// Path B: Force aperture memory under dp0 mode to prevent WS=0x3 migration to
			// non-aperture, which leaves the display scanning empty pages and triggers
			// the 0x3→0x1 WS degradation.
			{"__ZN21AppleIntelFramebuffer24isApertureMemoryRequiredEv", wrapIsApertureMemoryRequired, this->oIsApertureMemoryRequired},
			// Path C: Coerce kIOWindowServerActiveAttribute=0x1 (WS degrade) to 0x3 (stay-active)
			// under dp0 mode so kernel-tracked fWSAAState never drops below 3 once WS goes active.
			{"__ZN21AppleIntelFramebuffer12setAttributeEjm", wrapSetAttribute, this->oSetAttribute},
			// V183: write-only ADL-P power well handler; no callthrough (TGL poll loop hangs on RPL).
			// Real TGL falls through to original via ohwSetPowerWellStatePGE.
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj", hwSetPowerWellStatePGE, this->ohwSetPowerWellStatePGE},
			{"__ZN19AppleIntelPowerWell22hwSetPowerWellStateAuxEbj",hwSetPowerWellStateAux, this->ohwSetPowerWellStateAux},
			{"__ZN19AppleIntelPowerWell22hwSetPowerWellStateDDIEbj",hwSetPowerWellStateDDI, this->ohwSetPowerWellStateDDI},
			{"__ZN31AppleIntelRegisterAccessManager19FastWriteRegister32Emj",FastWriteRegister32, this->oFastWriteRegister32},
			// V60: ReadRegister32 hooks DISABLED — V59 proved they cause 0-children regression
			// (display driver loops in forceWake power-well cycling, never completes init)
			{"__ZN31AppleIntelRegisterAccessManager15WriteRegister32Emj",raWriteRegister32, this->oraWriteRegister32},
			{"__ZN31AppleIntelRegisterAccessManager15WriteRegister32EPVvmj",raWriteRegister32b},
		};
		PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route dp symbols");
		
		if (isprod) {
			RouteRequestPlus requests[] = {
				{"__ZN21AppleIntelFramebuffer4initEP31AppleIntelFramebufferControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
				{"__ZN31AppleIntelFramebufferController23initPlatformWorkaroundsEv", initPlatformWorkarounds, this->oinitPlatformWorkarounds},
				{"__ZN31AppleIntelFramebufferController16getOSInformationEv", getOSInformation, this->ogetOSInformation},
				{"__ZN31AppleIntelFramebufferController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
				{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
				{"__ZN19AppleIntelPowerWell4initEP31AppleIntelFramebufferController",AppleIntelPowerWellinit, this->oAppleIntelPowerWellinit},
				{"__ZN31AppleIntelFramebufferController5startEP9IOService",AppleIntelBaseControllerstart, this->oAppleIntelBaseControllerstart},
				{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
				{"__ZN31AppleIntelFramebufferController14disableCDClockEv",disableCDClock,this->odisableCDClock},
				{"__ZN31AppleIntelFramebufferController16hwRegsNeedUpdateEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsPK29IODetailedTimingInformationV2PN16AppleIntelScaler12SCALERPARAMSE",hwRegsNeedUpdate, this->ohwRegsNeedUpdate},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route p symbols");
			
		} else
		{
			RouteRequestPlus requests[] = {
				{"__ZN21AppleIntelFramebuffer4initEP24AppleIntelBaseControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
				{"__ZN24AppleIntelBaseController23initPlatformWorkaroundsEv", initPlatformWorkarounds, this->oinitPlatformWorkarounds},
				{"__ZN24AppleIntelBaseController16getOSInformationEv", getOSInformation, this->ogetOSInformation},
				{"__ZN24AppleIntelBaseController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
				{"__ZN24AppleIntelBaseController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
				{"__ZN19AppleIntelPowerWell4initEP24AppleIntelBaseController",AppleIntelPowerWellinit, this->oAppleIntelPowerWellinit},
				{"__ZN24AppleIntelBaseController5startEP9IOService",AppleIntelBaseControllerstart, this->oAppleIntelBaseControllerstart},
				{"__ZN24AppleIntelBaseController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
				{"__ZN24AppleIntelBaseController14disableCDClockEv",disableCDClock,this->odisableCDClock},
				{"__ZN24AppleIntelBaseController16hwRegsNeedUpdateEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsPK29IODetailedTimingInformationV2PN16AppleIntelScaler12SCALERPARAMSE",hwRegsNeedUpdate, this->ohwRegsNeedUpdate},
				// V201 diagnostic: read scanout buffer right after hwSetupMemory returns to
				// detect what fills it (zeros vs wallpaper pixels). FB-only scope.
				{"__ZN24AppleIntelBaseController13hwSetupMemoryEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsb", wrapHwSetupMemory, this->ohwSetupMemory},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route d symbols");
			
		}
		
		//powerwell
		static const uint8_t f1[]= {0xe8, 0x99, 0x9f, 0xfd, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0x74, 0x78};
		static const uint8_t r1[]= {0xe8, 0x99, 0x9f, 0xfd, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0xeb, 0x78};
		
		static const uint8_t f1p[]= {0xe8, 0x66, 0xb0, 0xfe, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0x74, 0x45};
		static const uint8_t r1p[]= {0xe8, 0x66, 0xb0, 0xfe, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0xeb, 0x45};
		
		//osinfo
		/*fInfoHasLid                  : 1
		fInfoPipeCount               : 3
		fInfoPortCount               : 3
		fInfoFramebufferCount        : 3*/

		static const uint8_t f2[]= {0xc7, 0x05, 0x07, 0x81, 0x10, 0x00, 0x01, 0x03, 0x09, 0x03, 0xb8, 0x00, 0x00, 0x00, 0x04};
		static const uint8_t r2[]= {0xc7, 0x05, 0x07, 0x81, 0x10, 0x00, 0x01, 0x04, 0x03, 0x02, 0xb8, 0x00, 0x00, 0x00, 0x04};

		static const uint8_t f2p[]= {0xc7, 0x05, 0x57, 0xe5, 0x0b, 0x00, 0x01, 0x03, 0x09, 0x03};
		static const uint8_t r2p[]= {0xc7, 0x05, 0x57, 0xe5, 0x0b, 0x00, 0x01, 0x04, 0x03, 0x02};
		
		static const uint8_t f2b[]= {0x49, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x35, 0x17, 0x81, 0x10, 0x00, 0xb8, 0x08, 0x00, 0x00, 0x00};
		static const uint8_t r2b[]= {0x49, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x35, 0x17, 0x81, 0x10, 0x00, 0xb8, 0x08, 0x00, 0x00, 0x00};
		
		static const uint8_t f2c[]= {0x48, 0x89, 0x1d, 0xc0, 0x81, 0x10, 0x00, 0x4c, 0x89, 0x35, 0xc1, 0x81, 0x10, 0x00, 0x48, 0xb8, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
		static const uint8_t r2c[]= {0x48, 0x89, 0x1d, 0xc0, 0x81, 0x10, 0x00, 0x4c, 0x89, 0x35, 0xc1, 0x81, 0x10, 0x00, 0x48, 0xb8, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
		
		//mem
		static const uint8_t f2d[]= {0x0f, 0x94, 0xc0, 0xb9, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x80, 0x00};
		static const uint8_t r2d[]= {0x0f, 0x94, 0xc0, 0xb9, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x40, 0x00};
		
		//mem
		static const uint8_t f2dp[]= {0xb8, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x80, 0x00, 0x0f, 0x44, 0xd0, 0x0f, 0x94, 0xc1, 0x48, 0x01, 0x0d, 0xa2, 0xd0, 0x09, 0x00};
		static const uint8_t r2dp[]= {0xb8, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x40, 0x00, 0x0f, 0x44, 0xd0, 0x0f, 0x94, 0xc1, 0x48, 0x01, 0x0d, 0xa2, 0xd0, 0x09, 0x00};
		
		//cdclock
		static const uint8_t f2e[]= {0x48, 0xc7, 0x83, 0x60, 0x43, 0x00, 0x00, 0x00, 0x2d, 0x31, 0x01, 0x48, 0xc7, 0x83, 0x68, 0x43, 0x00, 0x00, 0x00, 0x54, 0xea, 0x2a, 0xc6, 0x83, 0xb4, 0x45, 0x00, 0x00, 0x00};
		static const uint8_t r2e[]= {0x48, 0xc7, 0x83, 0x60, 0x4a, 0x00, 0x00, 0x00, 0xa3, 0x02, 0x00, 0x48, 0xc7, 0x83, 0x68, 0x4a, 0x00, 0x00, 0x00, 0xf6, 0x09, 0x00, 0xc6, 0x83, 0xb4, 0x45, 0x00, 0x00, 0x00};
		


		//conn
		static const uint8_t f3[]= {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x03, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x04, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x05, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x06, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x07, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x08, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00};
		
		static const uint8_t r3[]= {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
		
		
		//lcd power reg
		static const uint8_t f4[]= {0x00, 0x72, 0x0c, 0x00};
		static const uint8_t r4[]= {0x00, 0x12, 0x06, 0x00};
		
		static const uint8_t f4a[]= {0x04, 0x72, 0x0c, 0x00};
		static const uint8_t r4a[]= {0x04, 0x12, 0x06, 0x00};
		
		static const uint8_t f4b[]= {0x08, 0x72, 0x0c, 0x00};
		static const uint8_t r4b[]= {0x08, 0x12, 0x06, 0x00};
		
		static const uint8_t f4c[]= {0x0c, 0x72, 0x0c, 0x00};
		static const uint8_t r4c[]= {0x0c, 0x12, 0x06, 0x00};
		
		
		//jalavoui
		static const uint8_t f6a[]= { 0xbe, 0x04, 0x00, 0x00, 0x00, 0x48, 0x89, 0xda, 0x31, 0xc9, 0xe8, 0x8c, 0xac, 0x04, 0x00};
		static const uint8_t r6a[]= { 0xbe, 0x04, 0x00, 0x00, 0x00, 0x48, 0x89, 0xda, 0x31, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		//ReadRegister64
		static const uint8_t f7[]= {0x83, 0xc0, 0xfc, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0xca, 0xf5, 0x0c, 0x00};
		static const uint8_t r7[]= {0x83, 0xc0, 0xf8, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0xca, 0xf5, 0x0c, 0x00};
		
		static const uint8_t f7p[]= {0x83, 0xc0, 0xfc, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0x84, 0x40, 0x08, 0x00};
		static const uint8_t r7p[]= {0x83, 0xc0, 0xf8, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0x84, 0x40, 0x08, 0x00};

		
		//hwreg
		static const uint8_t f10[]= {0xe8, 0xaf, 0xe2, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x5b};
		static const uint8_t r10[]= {0xe8, 0xaf, 0xe2, 0xff, 0xff, 0x84, 0xc0, 0xeb, 0x5b};
		
		static const uint8_t f10p[]= {0xe8, 0x9e, 0xf3, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x3d};
		static const uint8_t r10p[]= {0xe8, 0x9e, 0xf3, 0xff, 0xff, 0x84, 0xc0, 0xeb, 0x3d};
		
		//probeportmode
		static const uint8_t f13b[]= {0xff, 0x90, 0x90, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};
		static const uint8_t r13b[]= {0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};

		static const uint8_t f13[]= {0xff, 0x91, 0x90, 0x01, 0x00, 0x00, 0x83, 0xf8, 0x02, 0x0f, 0x84, 0xec, 0x00, 0x00, 0x00};
		static const uint8_t r13[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		static const uint8_t f13p[]= {0xff, 0x91, 0x78, 0x01, 0x00, 0x00, 0x83, 0xf8, 0x02, 0x74, 0x64};
		static const uint8_t r13p[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		static const uint8_t f13pb[]= {0xff, 0x90, 0x78, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};
		static const uint8_t r13pb[]= {0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};

		
		//getPathByPipe logs
		static const uint8_t f15[]= {0x74, 0x36, 0x48, 0xff, 0x05, 0x7e, 0x51, 0x08, 0x00, 0x44, 0x89, 0x3c, 0x24, 0x48, 0x8d, 0x15, 0x4d, 0x88, 0x03, 0x00, 0x4c, 0x8d, 0x05, 0x28, 0x8a, 0x03, 0x00};
		static const uint8_t r15[]= {0xeb, 0x36, 0x48, 0xff, 0x05, 0x7e, 0x51, 0x08, 0x00, 0x44, 0x89, 0x3c, 0x24, 0x48, 0x8d, 0x15, 0x4d, 0x88, 0x03, 0x00, 0x4c, 0x8d, 0x05, 0x28, 0x8a, 0x03, 0x00};
		
		//getBuiltInPor
		static const uint8_t f16[]= {0x48, 0x89, 0x05, 0xfc, 0x39, 0x12, 0x00, 0x48, 0x8b, 0x83, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x75, 0x0d};
		static const uint8_t r16[]= {0x48, 0x89, 0x05, 0xfc, 0x39, 0x12, 0x00, 0x48, 0x8b, 0x83, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x90, 0x90};
		
		static const uint8_t f16p[]= {0x48, 0x8b, 0x80, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x75, 0x0a};
		static const uint8_t r16p[]= {0x48, 0x8b, 0x80, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x90, 0x90};

		//getHPDState
		static const uint8_t f19[]= {0xbe, 0x70, 0x44, 0x04, 0x00};
		static const uint8_t r19[]= {0xbe, 0xa0, 0x38, 0x16, 0x00};
		
		//savenvram
		static const uint8_t f20[]= {0xff, 0x90, 0xf8, 0x09, 0x00, 0x00, 0x41, 0x89, 0xc6, 0x48, 0x85, 0xdb, 0x74, 0x17};
		static const uint8_t r20[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x85, 0xdb, 0x74, 0x17};
		
		static const uint8_t f20p[]= {0xff, 0x90, 0xf8, 0x09, 0x00, 0x00, 0x41, 0x89, 0xc6, 0x48, 0x85, 0xdb, 0x74, 0x17};
		static const uint8_t r20p[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x85, 0xdb, 0x74, 0x17};
		
		//SafeForceWake
		static const uint8_t f21[]= {0x0f, 0x84, 0x96, 0x00, 0x00, 0x00, 0x48, 0xff, 0x05, 0xbc, 0x05, 0x0f, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7};
		static const uint8_t r21[]= {0x48, 0xe9, 0x96, 0x00, 0x00, 0x00, 0x48, 0xff, 0x05, 0xbc, 0x05, 0x0f, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7};
		
		static const uint8_t f21p[]= {0x74, 0x3c, 0x48, 0xff, 0x05, 0x7a, 0x73, 0x09, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7, 0xe8, 0x97, 0x80, 0x01, 0x00};
		static const uint8_t r21p[]= {0xeb, 0x3c, 0x48, 0xff, 0x05, 0x7a, 0x73, 0x09, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7, 0xe8, 0x97, 0x80, 0x01, 0x00};
        
        //pixel hwcrt
        static const uint8_t f22[]= {0x48, 0x69, 0xc2, 0x50, 0xc3, 0x00, 0x00, 0x49, 0x89, 0x47, 0x28, 0xbf, 0x08, 0x00, 0x00, 0x00, 0xbe, 0x06, 0x00, 0x00, 0x00, 0xe8, 0x9e, 0x81, 0x01, 0x00, 0x84, 0xc0, 0x74, 0x3a};
        
        static const uint8_t r22[]= {0x48, 0xc7, 0xc0, 0xc0, 0x40, 0xd0, 0x2e, 0x49, 0x89, 0x47, 0x28, 0xbf, 0x08, 0x00, 0x00, 0x00, 0xbe, 0x06, 0x00, 0x00, 0x00, 0xe8, 0x9e, 0x81, 0x01, 0x00, 0x84, 0xc0, 0x90, 0x90};
		

		// force eDP panel detection regardless of pipe number (from NootedBlue)
		// NOPs two JE and one JNE that would skip eDP init when pipe != 1.
		// Required because our pinfo sets eDP on pipe=0, not pipe=1.
		static const uint8_t f6nb[]= {0x74, 0x2a, 0x83, 0xf8, 0x01, 0x74, 0x43, 0x85, 0xc0, 0x75, 0x60};
		static const uint8_t r6nb[]= {0x90, 0x90, 0x83, 0xf8, 0x01, 0x90, 0x90, 0x85, 0xc0, 0x90, 0x90};

		// fix register addresses if pipe=0 (jne→jmp to always use pipe-0 register offsets)
		static const uint8_t f24bp[]= {0x83, 0x78, 0x08, 0x00, 0x75, 0x0c};
		static const uint8_t r24bp[]= {0x83, 0x78, 0x08, 0x00, 0xeb, 0x0c};
		static const uint8_t f24cp[]= {0x00, 0x4c, 0x89, 0xea, 0x75, 0x12};
		static const uint8_t r24cp[]= {0x00, 0x4c, 0x89, 0xea, 0xeb, 0x12};
		static const uint8_t f24dp[]= {0x83, 0x78, 0x08, 0x00, 0x75, 0x0d};
		static const uint8_t r24dp[]= {0x83, 0x78, 0x08, 0x00, 0xeb, 0x0d};
		static const uint8_t f24b[]= {0x83, 0x78, 0x08, 0x00, 0x75, 0x0c};
		static const uint8_t r24b[]= {0x83, 0x78, 0x08, 0x00, 0xeb, 0x0c};
		static const uint8_t f24c[]= {0x48, 0x8b, 0x55, 0xd0, 0x75, 0x13};
		static const uint8_t r24c[]= {0x48, 0x8b, 0x55, 0xd0, 0xeb, 0x13};
		static const uint8_t f24d[]= {0x83, 0x78, 0x08, 0x00, 0x75, 0x0d};
		static const uint8_t r24d[]= {0x83, 0x78, 0x08, 0x00, 0xeb, 0x0d};
		// link training speed constant fix
		static const uint8_t f25[]= {0x77, 0x77, 0x00, 0x00};
		static const uint8_t r25[]= {0x33, 0x00, 0x00, 0x00};

		// Path E (TCON ID): guarded by -ngreentglwithgfx boot-arg (requires GFX kext present).
		// Rewrites CamelliaTcon2/BanksiaTcon DPCD ID comparisons so they match this panel's
		// DPCD bytes [14 1e c4 c1] → 0xc1c41e14. Also sets cameliav=2 in getOSInformation.
		// DO NOT enable on FB-only boot (no GFX): calls into AGDC services → hang.
		static const uint8_t f_tcon_camellia[]= {0x3d, 0x11, 0x0a, 0x84, 0x41};
		static const uint8_t r_tcon_camellia[]= {0x3d, 0x14, 0x1e, 0xc4, 0xc1};
		static const uint8_t f_tcon_banksia[] = {0x3d, 0x12, 0x14, 0xc4, 0x41};
		static const uint8_t r_tcon_banksia[] = {0x3d, 0x14, 0x1e, 0xc4, 0xc1};
		const bool enableTcon = checkKernelArgument("-ngreentglwithgfx");

		if (isprod){
			LookupPatchPlus const patchesp[] = {// tgl production kext

				// f1p (powerwell JZ→JMP) commented out — not present in NootedBlue's
				// working TGL FBT prod patch list. Was an extra NootedGreen accumulated.
				//{activeKext, f1p, r1p, arrsize(f1p),	1},
				{activeKext, f2p, r2p, arrsize(f2p),	1},
				{activeKext, f2dp, r2dp, arrsize(f2dp),	1},
				// f3 (connector data table rewrite) commented out — not in NootedBlue.
				//{activeKext, f3, r3, arrsize(f3),	1},
				// f4 family ("lcd power reg" 0x72→0x12, 4 variants ~27 binary mods) commented out
				// — NOT present in NootedBlue's working TGL FBT prod patch list. These rewrite
				// what appears to be LCD power register access patterns; on Display 13 hardware
				// the original Apple code paths may be correct without the rewrite.
				//{activeKext, f4, r4, arrsize(f4),	11},
				//{activeKext, f4a, r4a, arrsize(f4a),	11},
				//{activeKext, f4b, r4b, arrsize(f4b),	2},
				//{activeKext, f4c, r4c, arrsize(f4c),	2},
				{activeKext, f7p, r7p, arrsize(f7p),	1},
				// f10p ("hwreg" CALL+JZ→JMP bypass) commented out — NOT in NootedBlue.
				//{activeKext, f10p, r10p, arrsize(f10p),	1},
				// Keep native probe-port mode flow for compatibility.
				//{activeKext, f13p, r13p, arrsize(f13p),	1},
				//{activeKext, f13pb, r13pb, arrsize(f13pb),	1},
				//{activeKext, f16p, r16p, arrsize(f16p),	1},
				{activeKext, f6nb, r6nb, arrsize(f6nb),	1},
				{activeKext, f13p, r13p, arrsize(f13p),	1},
				{activeKext, f13pb, r13pb, arrsize(f13pb),	1},
				{activeKext, f19, r19, arrsize(f19),	1},
				{activeKext, f20p, r20p, arrsize(f20p),	1},
				{activeKext, f24bp, r24bp, arrsize(f24bp),	14},
				{activeKext, f24cp, r24cp, arrsize(f24cp),	1},
				{activeKext, f24dp, r24dp, arrsize(f24dp),	4},
				{activeKext, f25,  r25,  arrsize(f25),	6},
			};

			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesp , address, size), "ngreen", "kextG11FBT Failed to apply production patches!");
			if (enableTcon) {
				LookupPatchPlus const tconPatches[] = {
					{activeKext, f_tcon_camellia, r_tcon_camellia, arrsize(f_tcon_camellia), 1},
					{activeKext, f_tcon_banksia,  r_tcon_banksia,  arrsize(f_tcon_banksia),  1},
				};
				PANIC_COND(!LookupPatchPlus::applyAll(patcher, tconPatches, address, size),
					"ngreen", "Failed to apply production TCON patches");
				SYSLOG("ngreen", "Path E: TCON ID patches applied (prod)");
			}
		}
		else {
			LookupPatchPlus const patches[] = {// tgl debug kext
				// f1 (powerwell JZ→JMP) commented out — not present in NootedBlue's
				// working TGL FBT debug patch list. Was an extra NootedGreen accumulated.
				//{activeKext, f1, r1, arrsize(f1),	1},
				// f2/f2d: osinfo pipe/port/fb counts now set via getOSInformation hook — no binary patch needed.
				// f3 (connector data table rewrite) commented out — not in NootedBlue.
				//{activeKext, f3, r3, arrsize(f3),	1},
				// f4 family ("lcd power reg" 0x72→0x12, 4 variants ~27 binary mods) commented out
				// — NOT in NootedBlue's working TGL FBT debug patch list. Suspected contributor
				// to the fragmentation/repetition symptom: rewriting LCD power register access
				// patterns may corrupt panel-side state on Display 13 (ADL-P) hardware.
				//{activeKext, f4, r4, arrsize(f4),	12},
				//{activeKext, f4a, r4a, arrsize(f4a),	11},
				//{activeKext, f4b, r4b, arrsize(f4b),	2},
				//{activeKext, f4c, r4c, arrsize(f4c),	2},
		//		{activeKext, f6a, r6a, arrsize(f6a),	1},
		//		{activeKext, f7, r7, arrsize(f7),	1},
				// f10 ("hwreg" CALL+JZ→JMP bypass) commented out — NOT in NootedBlue.
				{activeKext, f10, r10, arrsize(f10),	1},
				// f13: mandatory. Without this port-probe bypass the spoofed TGL path freezes during boot.
				{activeKext, f13, r13, arrsize(f13),	1},
				// f13b: mandatory. Without this bypass AppleIntelPort::probePortStateEv hits
				// a pure-virtual call and panics in WindowServer during enableController.
		//		{activeKext, f13b, r13b, arrsize(f13b),	1},
				// f15: suppress getPathByPipe log flood at IGLogLevel=8.
				// On platform 0x9a490000 all paths are on pipe 0; every scan cycle logs
				// "pipe = 0" many times per second, flooding the log unreadably.
				// The je→jmp makes the branch unconditionally skip the IGFB log emit.
				// Purely cosmetic: no behavioral change, no display-pipe impact.
				{activeKext, f15, r15, arrsize(f15),	1},
				//{activeKext, f16, r16, arrsize(f16),	1},
				{activeKext, f19, r19, arrsize(f19),	1},
		//		{activeKext, f20, r20, arrsize(f20),	1},
				//{activeKext, f21, r21, arrsize(f21),	1},
				//{activeKext, f22, r22, arrsize(f22),    1},
		//		{activeKext, f6nb, r6nb, arrsize(f6nb),	1},
		//		{activeKext, f19, r19, arrsize(f19),	1},
		//		{activeKext, f20, r20, arrsize(f20),	1},
				{activeKext, f24b, r24b, arrsize(f24b),	11},
		//		{activeKext, f24c, r24c, arrsize(f24c),	1},
		//		{activeKext, f24d, r24d, arrsize(f24d),	6},
		//		{activeKext, f25,  r25,  arrsize(f25),	6},
				};

			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patches , address, size), "ngreen", "kextG11FBT Failed to apply dbg patches!");
			if (enableTcon) {
				LookupPatchPlus const tconPatches[] = {
					{activeKext, f_tcon_camellia, r_tcon_camellia, arrsize(f_tcon_camellia), 1},
					{activeKext, f_tcon_banksia,  r_tcon_banksia,  arrsize(f_tcon_banksia),  1},
				};
				PANIC_COND(!LookupPatchPlus::applyAll(patcher, tconPatches, address, size),
					"ngreen", "Failed to apply debug TCON patches");
				SYSLOG("ngreen", "Path E: TCON ID patches applied (dbg)");
			}
		}
		
		return true;
		
	}else if (kextG11HW.loadIndex == index) {
		if (this->tglHWLoaded) {
			DBGLOG("ngreen", "Skipping ICL HW — TGL HW already loaded");
			return true;
		}
		auto *activeKext = &kextG11HW;
		DBGLOG("ngreen", "init AppleIntelICLGraphics!");
		PANIC_COND(!NGreen::callback->setRMMIOIfNecessary(), "ngreen", "Cannot map ICL accelerator BAR0");
		const bool wegCoexist = isWEGCoexistMode();

		{
			// loadGuCBinary: always route — WEG's firmware path is Mojave-gated and dead on Sonoma.
			// Without this hook, no GuC binary loads at all in coexist mode → ring dead.
			RouteRequestPlus firmwareRoute[] = {
				{"__ZN13IGHardwareGuC13loadGuCBinaryEv", loadIclGuCBinary, this->oLoadIclGuCBinary},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, firmwareRoute, address, size), "ngreen", "Failed to route loadGuCBinary (ICL)");
		}

		if (!wegCoexist) {
			RouteRequestPlus gpuInfoRoute[] = {
				// getGPUInfo: override topology at ICL object offsets (different from TGL offsets)
				{"__ZN16IntelAccelerator10getGPUInfoEv", getGPUInfoICL, this->ogetGPUInfoICL},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, gpuInfoRoute, address, size), "ngreen", "Failed to route getGPUInfoICL");
		}
		
		// SKU gate 1+2: NOP JNZ/JA + XOR eax,eax (Sonoma AppleIntelICLGraphics, verified in KC)
		static const uint8_t fSKUGates12[] = {
			0x83, 0xF9, 0x01,
			0x0F, 0x85, 0x0B, 0x01, 0x00, 0x00,
			0xFF, 0xC8,
			0x83, 0xF8, 0x07,
			0x0F, 0x87, 0x00, 0x01, 0x00, 0x00,
			0x48, 0x8D, 0x0D, 0x77, 0x02, 0x00, 0x00, 0x48
		};
		static const uint8_t rSKUGates12[] = {
			0x83, 0xF9, 0x01,
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
			0x31, 0xC0,
			0x83, 0xF8, 0x07,
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
			0x48, 0x8D, 0x0D, 0x77, 0x02, 0x00, 0x00, 0x48
		};

		// SKU gate 3: NOP JNZ (Sonoma AppleIntelICLGraphics, verified in KC)
		static const uint8_t fSKUGate3[] = {
			0x83, 0xF8, 0x08, 0x0F, 0x85, 0xC2, 0x00, 0x00, 0x00, 0xC7
		};
		static const uint8_t rSKUGate3[] = {
			0x83, 0xF8, 0x08, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC7
		};

		// SKU bypass: TEST rax,rax; JZ->JMP (Sonoma, f2Long verified in KC at 0x14152bcd)
		static const uint8_t fSkuBypassLong[] = {
			0x48, 0x85, 0xC0, 0x74, 0x72, 0x48, 0x0F, 0xBC, 0xC0, 0x48, 0xFF, 0xC0, 0x48,
			0x8D, 0x15, 0x00, 0xCD, 0x0F, 0x00, 0x48, 0x8D, 0x48, 0xFF, 0x48, 0xF7, 0xC1,
			0xFD, 0xFF, 0xFF, 0xFF, 0x74, 0x27, 0x48, 0x6B, 0xC9, 0x79
		};
		static const uint8_t rSkuBypassLong[] = {
			0x48, 0x85, 0xC0, 0xEB, 0x72, 0x48, 0x0F, 0xBC, 0xC0, 0x48, 0xFF, 0xC0, 0x48,
			0x8D, 0x15, 0x00, 0xCD, 0x0F, 0x00, 0x48, 0x8D, 0x48, 0xFF, 0x48, 0xF7, 0xC1,
			0xFD, 0xFF, 0xFF, 0xFF, 0x74, 0x27, 0x48, 0x6B, 0xC9, 0x79
		};

		LookupPatchPlus const patches[] = {
			{&kextG11HW, fSKUGates12,    rSKUGates12,    arrsize(fSKUGates12),    1},
			{&kextG11HW, fSKUGate3,      rSKUGate3,      arrsize(fSKUGate3),      1},
			{&kextG11HW, fSkuBypassLong, rSkuBypassLong, arrsize(fSkuBypassLong), 1},
		};
		
		/*auto catalina = getKernelVersion() == KernelVersion::Catalina;
		if (catalina)
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesc , address, size), "ngreen", "cata Failed to apply patches!");
		else*/
		for (size_t i = 0; i < sizeof(patches)/sizeof(patches[0]); ++i) {
			//IOSleep(delay);
			PANIC_COND(!patches[i].apply(patcher, address, size), "ngreen", "kextG11HW Failed to apply patch %zu", i);
		}
		DBGLOG("ngreen", "Loaded AppleIntelICLGraphics!");
		injectAcceleratorPersonality(false);

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
		this->tglHWLoaded = true;
		auto *activeKext = (kextG11HWTA.loadIndex == index) ? &kextG11HWTA : &kextG11HWT;
		SYSLOG("ngreen", "init AppleIntelTGLGraphics (HW accelerator)");
		PANIC_COND(!NGreen::callback->setRMMIOIfNecessary(), "ngreen", "Cannot map TGL accelerator BAR0");
		SYSLOG("ngreen", "V165: setRMMIO done, starting symbol resolve");

		// V144: Resolve the Blit3D context params struct and the ExtendedContext initWithOptions.
		// Blit3DExtendedCtxParams is a static data symbol — address passed as param_2 to initWithOptions.
		// Without these, IGHardwareExtendedContextinitWithOptions is called with param_2=0
		// causing it to return 0 (ok=0) and leave ctx+0xb8=NULL → submitBlit crash at +0x28e.
		{
			SolveRequestPlus solveRequests[] = {
				{"__ZN23IGHardwareBlit3DContext17ExtendedCtxParamsE", this->Blit3DExtendedCtxParams},
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
		if (vfIdentifyDevice() == VfIdentity::Virtual) {
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
			LookupPatchPlus const vfDoorbellTopologyPatch {
				activeKext, vfDoorbellTopologyFind, vfDoorbellTopologyReplace, 1,
			};
			PANIC_COND(!vfDoorbellTopologyPatch.apply(patcher, address, size),
			           "ngreen", "Failed to replace VF DISTRDB read");
			SYSLOG("ngreen", "V229: replaced physical DISTRDB read before GuC routing");
		}

		const bool wegCoexist = isWEGCoexistMode();
		RouteRequestPlus requests[] = {
			// V217: Raptor Lake VFs use Wa_22018453856.  Query the PF-provisioned
			// GGTT range, replace Apple's zero/stolen-derived allocator ranges, keep
			// a software PTE shadow, and relay mutations to the PF through GuC.
			{"__ZN15IGMemoryManager12initSegmentsEv",
			 IGMemoryManagerInitSegments,
			 this->oIGMemoryManagerInitSegments},
			{"__ZN25IGHardwareGlobalPageTable15initWithOptionsEP16IntelAcceleratorRK14IGAddressRangePvyj",
			 IGHardwareGlobalPageTableInitWithOptions,
			 this->oIGHardwareGlobalPageTableInitWithOptions},
			{"__ZN25IGHardwareGlobalPageTable8mapRangeERK14IGAddressRangeyy",
			 IGHardwareGlobalPageTableMapRange,
			 this->oIGHardwareGlobalPageTableMapRange},
			{"__ZN25IGHardwareGlobalPageTable15mapRangeRotatedER33IGAddressRangeRotatedPageIteratorR25IGPhysicalSegmentIteratory",
			 IGHardwareGlobalPageTableMapRangeRotated,
			 this->oIGHardwareGlobalPageTableMapRangeRotated},
			{"__ZN25IGHardwareGlobalPageTable10unmapRangeERK14IGAddressRange",
			 IGHardwareGlobalPageTableUnmapRange,
			 this->oIGHardwareGlobalPageTableUnmapRange},
			{"__ZN25IGHardwareGlobalPageTable13mapRangeDummyERK14IGAddressRangey",
			 IGHardwareGlobalPageTableMapRangeDummy,
			 this->oIGHardwareGlobalPageTableMapRangeDummy},
			
			// V163: Hook startGraphicsEngine to clear PERCTX_PREEMPT_CTRL (FF_SLICE_CS_CHICKEN1 bit 14)
			// immediately after the TGL kext enables it. The TGL kext writes 0x40004000 to reg 0x20E0
			// (mask=bit14, value=bit14=1). On RPL this causes the EU thread dispatcher to freeze on
			// first context switch because hardware snapshots the register into the context image DMA
			// buffer — clearing it here, before any execlist context is created, prevents the bad
			// value from ever reaching the DMA buffer.
		 	{"__ZN16IntelAccelerator19startGraphicsEngineEv", startGraphicsEngine, this->ostartGraphicsEngine},
			{"__ZN16IntelAccelerator18stopGraphicsEngineEv",  stopGraphicsEngine,  this->ostopGraphicsEngine},

			// V212: Hook IGScheduler{4,5}::isGpuIdle — the bool watchdog query called post-startup.
			// startGraphicsEngine SUCCEEDS (returns non-zero = success path in decomp). But the GPU
			// watchdog polls isGpuIdle() after init; INSTDONE bit0 stuck at 0 makes it return false
			// → watchdog declares GPU hung → resets → startGraphicsEngine retry loop forever.
			// Fix: when INSTDONE == 0xfffffffe (RPL-P idle with stuck bit0), return true.
			{"__ZNK12IGScheduler59isGpuIdleEv", wrapIGScheduler5IsGpuIdle, this->oIGScheduler5IsGpuIdle},
			{"__ZNK12IGScheduler49isGpuIdleEv", wrapIGScheduler4IsGpuIdle, this->oIGScheduler4IsGpuIdle},

			// V164: Hook populateResetRegisterList which reads live MMIO values into the per-context
			// replay list (a batch of MI_LRI commands executed before every context switch).
			// startGraphicsEngine enables PERCTX_PREEMPT_CTRL then calls populateResetRegisterList,
			// which snapshots the live 0x4000 into the list. Hardware then replays 0x4000 back to
			// 0x20E0 on every context switch, overriding any post-hoc MMIO clear.
			// Fix: clear bit 14 BEFORE calling original so the snapshot captures 0x0000.
			{"__ZN16IntelAccelerator25populateResetRegisterListEv", populateResetRegisterList, this->opopulateResetRegisterList},


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
		SYSLOG("ngreen", "V165: routing %zu HW accelerator symbols", sizeof(requests)/sizeof(requests[0]));
		PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route symbols");
		SYSLOG("ngreen", "V165: HW accelerator symbols routed OK");

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

		{
			// loadGuCBinary: always route — WEG's firmware path is Mojave-gated and dead on Sonoma.
			// Without this hook, no GuC binary loads at all in coexist mode → ring dead.
			RouteRequestPlus firmwareRoute[] = {
				{"__ZN13IGHardwareGuC13loadGuCBinaryEv", loadGuCBinary, this->oloadGuCBinary},
				{"__ZN16IntelAccelerator17transferOwnershipEPK20IGSharedMappedBufferi",
				 vfTransferOwnership, this->oVfTransferOwnership},
				// V222: scheduler 4 uses the Gen11 reference GuC transport, but its
				// stock MMIO helper writes the legacy 0xc180 scratch registers.  A VF
				// is provisioned only for the Gen11 0x190240/0x1901f0 mailbox.
				{"__ZN13IGHardwareGuC19mmioHostToGuCActionEPKjjiPj",
				 vfMmioHostToGuCAction, this->oVfMmioHostToGuCAction},
				{"__ZN13IGHardwareGuC15hostToGuCActionEPKjjiPj",
				 vfLegacyHostToGuCAction, this->oVfLegacyHostToGuCAction},
				{"__ZN13IGHardwareGuC15createUkContextEy25UK_GEN11_CONTEXT_PRIORITY",
				 vfCreateUkContext, this->oVfCreateUkContext},
				{"__ZN13IGHardwareGuC14allocContextIdEyb",
				 vfAllocContextId, this->oVfAllocContextId},
				{"__ZN13IGHardwareGuC16releaseContextIdEj",
				 vfReleaseContextId, this->oVfReleaseContextId},
				// These routines touch raw physical doorbell registers BEFORE
				// calling hostToGuCAction; guarding the sender alone is too late.
				{"__ZN13IGHardwareGuC15acquireDoorbellEP35UK_GEN11_GUC_CONTEXT_DESCRIPTOR_RECb",
				 vfAcquireDoorbell, this->oVfAcquireDoorbell},
				{"__ZN13IGHardwareGuC15releaseDoorbellEP35UK_GEN11_GUC_CONTEXT_DESCRIPTOR_REC",
				 vfReleaseDoorbell, this->oVfReleaseDoorbell},
				{"__ZN13IGHardwareGuC15allocUkDoorbellEjb",
				 vfAllocUkDoorbell, this->oVfAllocUkDoorbell},
				{"__ZN13IGHardwareGuC17reacquireDoorbellEj",
				 vfReacquireDoorbell, this->oVfReacquireDoorbell},
				{"__ZN13IGHardwareGuC9isGuCIdleEv", vfIsGuCIdle, this->oVfIsGuCIdle},
				{"__ZN13IGHardwareGuC13isContextIdleEj", vfIsContextIdle, this->oVfIsContextIdle},
				{"__ZN13IGHardwareGuC16isKmdContextIdleERK21SGfxContextDescriptor",
				 vfIsKmdContextIdle, this->oVfIsKmdContextIdle},
				// V227: initDoorbells consumes DISTRDB before any submission.
				// Bypass the stock routine for a VF so an all-ones MMIO read
				// cannot turn into a 16 x 256 topology and corrupt the object.
				{"__ZN13IGHardwareGuC13initDoorbellsEv",
				 vfInitDoorbells, this->oVfInitDoorbells},
				// V226: DISTRDB (0xd08) is outside a VF's MMIO allowlist and
				// reads as all ones.  Feed Apple's allocator the real Gen12
				// eight-by-32 topology after validating the VF's GuC KLV quota.
				{"__ZN13IGHardwareGuC23readDoorbellSQIDIConfigEv",
				 vfReadDoorbellSQIDIConfig, this->oVfReadDoorbellSQIDIConfig},
				// V223: enlarge and re-layout Apple's legacy 1 KiB CTB rings before
				// translating their registration to the modern VF KLV ABI.
				{"__ZN21IGHardwareGuCCTBuffer19initWithAcceleratorEP22IOGraphicsAccelerator2",
				 vfCtbInitWithAccelerator, this->oVfCtbInitWithAccelerator},
				{"__ZN21IGHardwareGuCCTBuffer13ctChannelInitEv",
				 vfCtbChannelInit, this->oVfCtbChannelInit},
				{"__ZN21IGHardwareGuCCTBuffer15gucToHostActionEPj",
				 vfCtbGucToHostAction, this->oVfCtbGucToHostAction},
				{"__ZN13IGHardwareGuC32handleSoftwareGuCToHostInterruptEP22IOInterruptEventSourcei",
				 vfSoftwareGuCInterrupt, this->oVfSoftwareGuCInterrupt},
				// V237: 0xCEE8 is PF-owned.  A VF invalidates GuC translations
				// through the asynchronous v70 CT action instead.
				{"__ZN13IGHardwareGuC13invalidateTLBEv",
				 vfInvalidateTLB, this->oVfInvalidateTLB},
				// V236: replace the complete filter on a VF.  The stock filter
				// masks GFX_MSTR_IRQ, acquires physical force-wake and only then
				// calls readAndClearInterrupts; wrapping the latter alone cannot
				// make a VF interrupt safe.
				{"__ZN17IGInterruptBridge22interruptFilterHandlerEP28IOFilterInterruptEventSource",
				 vfInterruptFilterHandler, this->oVfInterruptFilterHandler},
				// processInterrupts() can also call readAndClearInterrupts outside
				// the hardware filter.  Keep that entry point VF-safe as well.
				{"__ZN17IGInterruptBridge22readAndClearInterruptsER8IGBitSetILm46EE",
				 vfReadAndClearInterrupts, this->oVfReadAndClearInterrupts},
				{"__ZN17IGInterruptBridge16enableInterruptsEv",
				 vfEnableInterrupts, this->oVfEnableInterrupts},
				{"__ZN17IGInterruptBridge17disableInterruptsEv",
				 vfDisableInterrupts, this->oVfDisableInterrupts},
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
		if (vfIdentifyDevice() == VfIdentity::Virtual) {
			static const uint8_t vfCtbTlbPollFind[] = {
				0xc7, 0x80, 0xe8, 0xce, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
				0x8b, 0x88, 0xe8, 0xce, 0x00, 0x00, 0xf6, 0xc1, 0x01, 0x75, 0xf5,
				0x49, 0x8b, 0x7e, 0x20
			};
			static const uint8_t vfCtbTlbPollReplace[] = {
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
				0x49, 0x8b, 0x7e, 0x20
			};
			LookupPatchPlus const vfCtbTlbPollPatch {
				activeKext, vfCtbTlbPollFind, vfCtbTlbPollReplace,
				sizeof(vfCtbTlbPollFind), 1
			};
			PANIC_COND(!vfCtbTlbPollPatch.apply(patcher, address, size), "ngreen",
			           "V222: failed to bypass VF CTB TLB poll");
			SYSLOG("ngreen", "V237: removed physical TLB access from VF CTB initialization");

			// V223: modern GuC descriptors count head/tail in dwords.  Apple's
			// legacy CTB implementation stores the same fields in bytes.  Remove
			// only those conversions; the ring-size field remains byte-sized and
			// retains its /4 conversion in both send and receive paths.
			static const uint8_t vfCtbSendReadFind[] = {
				0x44, 0x8b, 0x69, 0x0c,
				0x44, 0x8b, 0x71, 0x10, 0x41, 0xc1, 0xee, 0x02,
				0x48, 0x89, 0x4d, 0xb0,
				0x8b, 0x59, 0x14, 0x48, 0xc1, 0xeb, 0x02,
				0x41, 0xc1, 0xed, 0x02
			};
			static const uint8_t vfCtbSendReadReplace[] = {
				0x44, 0x8b, 0x69, 0x0c,
				0x44, 0x8b, 0x71, 0x10, 0x90, 0x90, 0x90, 0x90,
				0x48, 0x89, 0x4d, 0xb0,
				0x8b, 0x59, 0x14, 0x90, 0x90, 0x90, 0x90,
				0x41, 0xc1, 0xed, 0x02
			};
			static const uint8_t vfCtbSendStoreWaitFind[] = {
				0x48, 0x89, 0x0c, 0xf0, 0x41, 0xc1, 0xe6, 0x02,
				0x44, 0x89, 0x73, 0x14, 0x49, 0x8b, 0x45, 0x10
			};
			static const uint8_t vfCtbSendStoreWaitReplace[] = {
				0x48, 0x89, 0x0c, 0xf0, 0x90, 0x90, 0x90, 0x90,
				0x44, 0x89, 0x73, 0x14, 0x49, 0x8b, 0x45, 0x10
			};
			static const uint8_t vfCtbSendStoreNoWaitFind[] = {
				0x00, 0x41, 0xc1, 0xe6, 0x02, 0x4c, 0x89, 0xd3,
				0x45, 0x89, 0x72, 0x14, 0x4d, 0x89, 0xc5
			};
			static const uint8_t vfCtbSendStoreNoWaitReplace[] = {
				0x00, 0x90, 0x90, 0x90, 0x90, 0x4c, 0x89, 0xd3,
				0x45, 0x89, 0x72, 0x14, 0x4d, 0x89, 0xc5
			};
			static const uint8_t vfCtbRecvReadFind[] = {
				0x4d, 0x8b, 0x46, 0x58,
				0x41, 0x8b, 0x40, 0x10, 0x48, 0xc1, 0xe8, 0x02,
				0x41, 0x8b, 0x48, 0x14, 0xc1, 0xe9, 0x02
			};
			static const uint8_t vfCtbRecvReadReplace[] = {
				0x4d, 0x8b, 0x46, 0x58,
				0x41, 0x8b, 0x40, 0x10, 0x90, 0x90, 0x90, 0x90,
				0x41, 0x8b, 0x48, 0x14, 0x90, 0x90, 0x90
			};
			static const uint8_t vfCtbRecvStoreFind[] = {
				0x48, 0x39, 0xd9, 0x75, 0xe6, 0xc1, 0xe2, 0x02,
				0x41, 0x89, 0x50, 0x10
			};
			static const uint8_t vfCtbRecvStoreReplace[] = {
				0x48, 0x39, 0xd9, 0x75, 0xe6, 0x90, 0x90, 0x90,
				0x41, 0x89, 0x50, 0x10
			};
			LookupPatchPlus const vfCtbUnitPatches[] = {
				{activeKext, vfCtbSendReadFind, vfCtbSendReadReplace,
				 sizeof(vfCtbSendReadFind), 1},
				{activeKext, vfCtbSendStoreWaitFind, vfCtbSendStoreWaitReplace,
				 sizeof(vfCtbSendStoreWaitFind), 1},
				{activeKext, vfCtbSendStoreNoWaitFind, vfCtbSendStoreNoWaitReplace,
				 sizeof(vfCtbSendStoreNoWaitFind), 1},
				{activeKext, vfCtbRecvReadFind, vfCtbRecvReadReplace,
				 sizeof(vfCtbRecvReadFind), 1},
				{activeKext, vfCtbRecvStoreFind, vfCtbRecvStoreReplace,
				 sizeof(vfCtbRecvStoreFind), 1},
			};
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, vfCtbUnitPatches,
			                                      address, size),
			           "ngreen", "V223: failed to convert VF CTB head/tail units");
			SYSLOG("ngreen", "V223: converted VF CTB head/tail units to dwords");

			// V237: cover every remaining 0xCEE8 posting-read loop.  Zero the
			// destination register and remove the test/branch, so these legacy
			// routines perform no physical read at all.
			static const uint8_t vfTlbPollEcxFind[] = {
				0x8b, 0x88, 0xe8, 0xce, 0x00, 0x00,
				0xf6, 0xc1, 0x01, 0x75, 0xf5
			};
			static const uint8_t vfTlbPollEcxReplace[] = {
				0x31, 0xc9, 0x90, 0x90, 0x90, 0x90,
				0x90, 0x90, 0x90, 0x90, 0x90
			};
			static const uint8_t vfTlbPollEdxFind[] = {
				0x8b, 0x90, 0xe8, 0xce, 0x00, 0x00,
				0xf6, 0xc2, 0x01, 0x75, 0xf5
			};
			static const uint8_t vfTlbPollEdxReplace[] = {
				0x31, 0xd2, 0x90, 0x90, 0x90, 0x90,
				0x90, 0x90, 0x90, 0x90, 0x90
			};
			static const uint8_t vfTlbPollEsiFind[] = {
				0x8b, 0xb0, 0xe8, 0xce, 0x00, 0x00,
				0x40, 0xf6, 0xc6, 0x01, 0x75, 0xf4
			};
			static const uint8_t vfTlbPollEsiReplace[] = {
				0x31, 0xf6, 0x90, 0x90, 0x90, 0x90,
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90
			};
			static const uint8_t vfTlbPollMemoryFind[] = {
				0xf7, 0x80, 0xe8, 0xce, 0x00, 0x00,
				0x01, 0x00, 0x00, 0x00, 0x75, 0xf4
			};
			static const uint8_t vfTlbPollMemoryReplace[] = {
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90
			};
			LookupPatchPlus const vfTlbPollPatches[] = {
				{activeKext, vfTlbPollEcxFind, vfTlbPollEcxReplace,
				 sizeof(vfTlbPollEcxFind), 8},
				{activeKext, vfTlbPollEdxFind, vfTlbPollEdxReplace,
				 sizeof(vfTlbPollEdxFind), 1},
				{activeKext, vfTlbPollEsiFind, vfTlbPollEsiReplace,
				 sizeof(vfTlbPollEsiFind), 1},
				{activeKext, vfTlbPollMemoryFind, vfTlbPollMemoryReplace,
				 sizeof(vfTlbPollMemoryFind), 1},
			};
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, vfTlbPollPatches,
			                                      address, size),
			           "ngreen", "V237: failed to remove VF physical TLB reads");

			// Remove the corresponding writes as a separate exhaustive set.  The
			// immediate form appears ten times in the unmodified Tahoe binary; the
			// CTB occurrence was consumed by the contextual patch above, leaving nine.
			static const uint8_t vfTlbWriteImmFind[] = {
				0xc7, 0x80, 0xe8, 0xce, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
			};
			static const uint8_t vfTlbWriteImmReplace[] = {
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
			};
			static const uint8_t vfTlbWriteEcxFind[] = {
				0x89, 0x88, 0xe8, 0xce, 0x00, 0x00
			};
			static const uint8_t vfTlbWriteEcxReplace[] = {
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90
			};
			static const uint8_t vfTlbWriteR8Find[] = {
				0x44, 0x89, 0x80, 0xe8, 0xce, 0x00, 0x00
			};
			static const uint8_t vfTlbWriteR8Replace[] = {
				0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
			};
			LookupPatchPlus const vfTlbWritePatches[] = {
				{activeKext, vfTlbWriteImmFind, vfTlbWriteImmReplace,
				 sizeof(vfTlbWriteImmFind), 9},
				{activeKext, vfTlbWriteEcxFind, vfTlbWriteEcxReplace,
				 sizeof(vfTlbWriteEcxFind), 1},
				{activeKext, vfTlbWriteR8Find, vfTlbWriteR8Replace,
				 sizeof(vfTlbWriteR8Find), 1},
			};
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, vfTlbWritePatches,
			                                      address, size),
			           "ngreen", "V237: failed to remove VF physical TLB writes");
			SYSLOG("ngreen", "V237: removed all legacy 0xCEE8 reads and writes");

			// V224: Apple places the legacy action in CT header bits 31:16 and
			// the request fence in dw1.  Modern CTB/HXG uses exactly the same
			// message length, but places fence in CT header bits 31:16 and the
			// action in the HXG header at dw1.  Keep Apple's locking, waiting and
			// ring accounting intact while swapping only those two encodings.
			static const uint8_t vfCtbHxgHeaderFind[] = {
				0xc1, 0xe2, 0x10, 0x09, 0xf2,
				0x8d, 0x94, 0x17, 0x00, 0x01, 0x00, 0x00
			};
			static const uint8_t vfCtbHxgHeaderReplace[] = {
				0x44, 0x89, 0xe2,             // mov edx, r12d (fence)
				0xc1, 0xe2, 0x10,             // shl edx, 16
				0x09, 0xf2,                   // or edx, esi (HXG length)
				0x8b, 0x39,                   // mov edi, [rcx] (action)
				0x90, 0x90
			};
			static const uint8_t vfCtbHxgActionFind[] = {
				0x42, 0x89, 0x14, 0xb3, 0x31, 0xd2, 0x41, 0xf7, 0xf5,
				0x44, 0x89, 0x24, 0x93, 0x8d, 0x42, 0x01
			};
			static const uint8_t vfCtbHxgActionReplace[] = {
				0x42, 0x89, 0x14, 0xb3, 0x31, 0xd2, 0x41, 0xf7, 0xf5,
				0x89, 0x3c, 0x93, 0x90, 0x8d, 0x42, 0x01
			};
			// Modern responses always arrive through G2H.  Force Apple's caller
			// to use its pending-request path for every action, including 0x10;
			// the legacy descriptor response slots are not part of the modern ABI.
			static const uint8_t vfCtbWaitModeFind[] = {
				0x48, 0x85, 0xff, 0x74, 0x09, 0x45, 0x31, 0xc9,
				0x5d, 0xe9, 0xd4, 0xdd, 0xff, 0xff
			};
			static const uint8_t vfCtbWaitModeReplace[] = {
				0x48, 0x85, 0xff, 0x74, 0x09, 0x41, 0xb1, 0x01,
				0x5d, 0xe9, 0xd4, 0xdd, 0xff, 0xff
			};
			static const uint8_t vfCtbAllocateWaitFind[] = {
				0x83, 0xfa, 0x10, 0x41, 0x0f, 0x95, 0xc1,
				0x44, 0x22, 0x4d, 0xcc
			};
			static const uint8_t vfCtbAllocateWaitReplace[] = {
				0x83, 0xfa, 0xff, 0x41, 0x0f, 0x95, 0xc1,
				0x44, 0x22, 0x4d, 0xcc
			};
			LookupPatchPlus const vfCtbHxgPatches[] = {
				{activeKext, vfCtbHxgHeaderFind, vfCtbHxgHeaderReplace,
				 sizeof(vfCtbHxgHeaderFind), 1},
				{activeKext, vfCtbHxgActionFind, vfCtbHxgActionReplace,
				 sizeof(vfCtbHxgActionFind), 1},
				{activeKext, vfCtbWaitModeFind, vfCtbWaitModeReplace,
				 sizeof(vfCtbWaitModeFind), 1},
				{activeKext, vfCtbAllocateWaitFind, vfCtbAllocateWaitReplace,
				 sizeof(vfCtbAllocateWaitFind), 1},
			};
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, vfCtbHxgPatches,
			                                      address, size),
			           "ngreen", "V224: failed to encode VF CTB messages as HXG");
			SYSLOG("ngreen", "V224: converted VF CTB request framing to HXG");
		}

		// Keep IGAccelDevice::deviceStart native, including failure propagation.

		if (!wegCoexist || gVfIdentity == VfIdentity::Virtual) {
			RouteRequestPlus coexistOffRoutes[] = {
				// ForceWake: replace Apple's SafeForceWakeMultithreaded with i915-ported version.
				// Apple's code uses 90ms timeouts and no fallback; ours uses 50ms + reserve-bit fallback.
				// The original domain mapping was broken (d<<1 loop misaligned Apple's 3-bit dom bitmap).
				{"__ZN16IntelAccelerator26SafeForceWakeMultithreadedEbjj", forceWake, this->oforceWake},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, coexistOffRoutes, address, size), "ngreen", "Failed to route coexist-off symbols");
		}

		if (!wegCoexist) {
			RouteRequestPlus gpuInfoRoute[] = {
				{"__ZN16IntelAccelerator10getGPUInfoEv", getGPUInfo, this->ogetGPUInfo},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, gpuInfoRoute, address, size), "ngreen", "Failed to route getGPUInfo");
		}

		// SKU/device-ID panic bypass (verified @ 0x23c1d in LE binary)
		// Original: mov edi,[rsi]; cmp edi,0xDEAFBEEE; jg sentinel; cmp edi,0x9A408086; je ok; cmp edi,0x9A488086; je ok; call panic
		// Patch:    nop the sentinel-jg + change last "je ok" → "jmp ok" → always jumps to GT2 init path.
		// Necessary: our spoofed 0x9A498086 is not in the whitelist (0x9A408086 / 0x9A488086).
		static const uint8_t f3[] = {
			0x8b, 0x3e, 0x81, 0xff, 0xee, 0xbe, 0xaf, 0xde, 0x7f, 0x15, 0x81, 0xff, 0x86, 0x80, 0x40, 0x9a, 0x74, 0x2d
		};
		static const uint8_t r3[] = {
			0x8b, 0x3e, 0x81, 0xff, 0xee, 0xbe, 0xaf, 0xde, 0x90, 0x90, 0x81, 0xff, 0x86, 0x80, 0x40, 0x9a, 0xeb, 0x2d
		};
		// L3BankCount bypass (verified @ 0x28776 in LE binary)
		// Original: topology-gated conditionals (cmp slices/eu/threads) → only set L3BankCount=8 for a specific config.
		// Patch:    NOP all conditional branches → always store L3BankCount=8 @ IGAccelDevice+0x1164.
		static const uint8_t f3b[] = {// jmp L3BankCount
			0x74, 0x23, 0x83, 0xf9, 0x02, 0x0f, 0x85, 0x89, 0x01, 0x00, 0x00, 0x83, 0xfe, 0x01, 0x75, 0x59, 0x83, 0xfa, 0x0c, 0x75, 0x54, 0x41, 0xc7, 0x87, 0x64, 0x11, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00
		};
		static const uint8_t r3b[] = {
			0x90, 0x90, 0x83, 0xf9, 0x02, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x83, 0xfe, 0x01, 0x90, 0x90, 0x83, 0xfa, 0x0c, 0x90, 0x90, 0x41, 0xc7, 0x87, 0x64, 0x11, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00
		};
		
		// MaxEUPerSubSlice override (verified @ 0x28692 in LE binary)
		// Original: MaxEUPerSubSlice = 8 - popcount(EUDisableFuse)  → stores result at IGAccelDevice+0x116c
		// Patch:    hardcodes MaxEUPerSubSlice=8 for RPL 96EU (8 EU per traditional sub-slice).
		//           TGL binary counts sub-slices (SS), not dual sub-slices (DSS).
		//           Linux shows 16 EU/DSS = 8 EU/SS since each DSS has 2 SS.
		static const uint8_t f3bb[] = {//MaxEUPerSubSlice
			0xbe, 0x08, 0x00, 0x00, 0x00, 0x29, 0xde, 0x41, 0x89, 0xb7, 0x6c, 0x11, 0x00, 0x00, 0x41, 0x8b, 0x8f, 0x58, 0x11, 0x00, 0x00
		};
		static const uint8_t r3bb[] = {
			0xbe, 0x08, 0x00, 0x00, 0x00, 0x90, 0x90, 0x41, 0x89, 0xb7, 0x6c, 0x11, 0x00, 0x00, 0x41, 0x8b, 0x8f, 0x58, 0x11, 0x00, 0x00
		};
		
		// NumSubSlices override (verified @ 0x28654 in LE binary)
		// Original: mov ebx,[rbp-0x30]; popcnt esi,ebx; add esi,esi; mov [r15+0x1158],esi
		//           → NumSubSlices = popcount(subsliceMask) * 2  (hardware-detected)
		// Patch:    hardcodes NumSubSlices=8 for the target i7-13620H UHD 64EU
		//           (4 enabled DSS × 2 SS/DSS = 8 traditional SS).
		//           The host i915 topology query reports 1 slice, 4 DSS and 64 EUs.
		static const uint8_t f3bbb[] = {//NumSubSlices
			0x8b, 0x5d, 0xd0, 0xf3, 0x0f, 0xb8, 0xf3, 0x01, 0xf6, 0x41, 0x89, 0xb7, 0x58, 0x11, 0x00, 0x00
		};
		static const uint8_t r3bbb[] = {
			0x8b, 0x5d, 0xd0, 0xbe, 0x08, 0x00, 0x00, 0x00, 0x90, 0x41, 0x89, 0xb7, 0x58, 0x11, 0x00, 0x00
		};
		
		// V139: RPL-only mitigation for GP faults inside blit3d_submit_rectlist.
		// Some command-buffer pointers on spoofed paths are 8-byte aligned; Apple emits
		// aligned SSE stores (movaps [r9+...], xmmN), which faults on unaligned targets.
		// Convert the hot-path stores to movups to tolerate unaligned command pointers.
		static const uint8_t f_v139_movaps_10[] = {
			0x41, 0x0f, 0x29, 0x51, 0x10, 0x0f, 0x28, 0xd4
		};
		static const uint8_t r_v139_movaps_10[] = {
			0x41, 0x0f, 0x11, 0x51, 0x10, 0x0f, 0x28, 0xd4
		};
		static const uint8_t f_v139_movaps_30[] = {
			0x66, 0x0f, 0x3a, 0x21, 0xd4, 0x23, 0x41, 0x0f, 0x29, 0x51, 0x30
		};
		static const uint8_t r_v139_movaps_30[] = {
			0x66, 0x0f, 0x3a, 0x21, 0xd4, 0x23, 0x41, 0x0f, 0x11, 0x51, 0x30
		};
		static const uint8_t f_v139_movaps_50[] = {
			0x0f, 0x57, 0xd2, 0x0f, 0x16, 0xd4, 0x41, 0x0f, 0x29, 0x51, 0x50
		};
		static const uint8_t r_v139_movaps_50[] = {
			0x0f, 0x57, 0xd2, 0x0f, 0x16, 0xd4, 0x41, 0x0f, 0x11, 0x51, 0x50
		};
		static const uint8_t f_v139_movaps_00[] = {
			0x41, 0x0f, 0x29, 0x09
		};
		static const uint8_t r_v139_movaps_00[] = {
			0x41, 0x0f, 0x11, 0x09
		};
		static const uint8_t f_v139_movaps_20[] = {
			0x41, 0x0f, 0x29, 0x49, 0x20
		};
		static const uint8_t r_v139_movaps_20[] = {
			0x41, 0x0f, 0x11, 0x49, 0x20
		};
		static const uint8_t f_v139_movaps_40[] = {
			0x41, 0x0f, 0x29, 0x59, 0x40
		};
		static const uint8_t r_v139_movaps_40[] = {
			0x41, 0x0f, 0x11, 0x59, 0x40
		};
		{
			// V52: Split patches into always-apply and RPL-only groups.
			// Real TGL reads topology from fuses correctly; RPL must hardcode
			// because fuse layout differs and BCS ring doesn't start.
			LookupPatchPlus const patchesAlways[] = {
				// SKU/device-ID bypass — needed for both (0x9A49 not in whitelist)
				{activeKext, f3, r3, arrsize(f3),	1},
			};
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesAlways, address, size), "ngreen",
				"kextG11HWT Failed to apply base patches!");
			
			if (!NGreen::callback->isRealTGL) {
				// Legacy topology overrides; native BCS readiness remains mandatory.
				LookupPatchPlus const patchesRPL[] = {
					{activeKext, f3b, r3b, arrsize(f3b),	1},      // L3BankCount=8
					{activeKext, f3bb, r3bb, arrsize(f3bb),	1},    // MaxEU/SS=8
					{activeKext, f3bbb, r3bbb, arrsize(f3bbb),	1},// NumSubSlices=8
				};
				PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesRPL, address, size), "ngreen",
					"kextG11HWT Failed to apply RPL-specific patches!");


				// Every matching store in the pinned blit3d_submit_rectlist body is
				// converted as one mandatory group. A single count patches all matches
				// against the original image; sequential skip-based patches changed the
				// match set after every write and therefore targeted the wrong sites.
				LookupPatchPlus const unalignedStorePatches[] = {
					{activeKext, f_v139_movaps_10, r_v139_movaps_10,
					 arrsize(f_v139_movaps_10), 1},
					{activeKext, f_v139_movaps_30, r_v139_movaps_30,
					 arrsize(f_v139_movaps_30), 1},
					{activeKext, f_v139_movaps_50, r_v139_movaps_50,
					 arrsize(f_v139_movaps_50), 1},
					{activeKext, f_v139_movaps_00, r_v139_movaps_00,
					 arrsize(f_v139_movaps_00), 6},
					{activeKext, f_v139_movaps_20, r_v139_movaps_20,
					 arrsize(f_v139_movaps_20), 4},
					{activeKext, f_v139_movaps_40, r_v139_movaps_40,
					 arrsize(f_v139_movaps_40), 1},
				};
				PANIC_COND(!LookupPatchPlus::applyAll(patcher, unalignedStorePatches,
				                                      address, size),
				           "ngreen", "Failed to apply complete unaligned-store patch set");
				SYSLOG("ngreen", "V243: converted all 14 pinned blit3d aligned-store sites");

				SYSLOG("ngreen", "V52: Applied RPL-specific topology and unaligned-store patches");
			} else {
				SYSLOG("ngreen", "V52: Real TGL — skipping RPL compatibility patches");
			}
		}

		SYSLOG("ngreen", "Loaded AppleIntelTGLGraphics! %s",
			   NGreen::callback->isRealTGL ? "Real TGL — native topology" :
			   "RPL spoofed — slices=1 subslices=8(4DSS) maxEU/SS=8 totalEU=64 L3=8");
		// Adding a personality can start matching immediately. Publish only
		// after this payload's required routes and patches have been installed.
		injectAcceleratorPersonality(true);

		return true;
	}

    return false;
}

bool Gen11::IGMemoryManagerInitSegments(void *that)
{
	const auto identity = vfIdentifyDevice();
	if (identity == VfIdentity::Invalid ||
	    (identity == VfIdentity::Virtual && !vfBootstrapBinder()))
		return false;
	const bool original = FunctionCast(IGMemoryManagerInitSegments,
	                                   callback->oIGMemoryManagerInitSegments)(that);
	if (!original || identity == VfIdentity::Physical)
		return original;
	if (!vfBootstrapBinder()) {
		SYSLOG("ngreen", "V219: aborting VF memory-manager init without GGTT transport");
		return false;
	}

	// Tahoe TGL initSegments derives the first two 32-bit ranges from stolen
	// memory and BAR2.  A VF has neither.  Linux solves the same mismatch by
	// ballooning everything outside the PF-assigned interval; express that
	// interval directly in Apple's range fields.
	getMember<uint64_t>(that, 0xA0) = gVfGGTTBase;
	getMember<uint64_t>(that, 0xA8) = gVfGGTTSize;
	getMember<uint64_t>(that, 0xB0) = gVfGGTTBase;
	getMember<uint64_t>(that, 0xB8) = gVfGGTTSize;

	// Keep the driver's traditional 1 GiB lower guard for its unified 32-bit
	// allocator, but intersect the upper end with the VF allocation.
	const uint64_t vfEnd = gVfGGTTBase + gVfGGTTSize;
	const uint64_t unifiedStart = gVfGGTTBase > 0x40000000ULL ?
	                              gVfGGTTBase : 0x40000000ULL;
	const uint64_t unifiedEnd = vfEnd < 0xFE000000ULL ? vfEnd : 0xFE000000ULL;
	if (unifiedEnd > unifiedStart) {
		getMember<uint64_t>(that, 0xC0) = unifiedStart;
		getMember<uint64_t>(that, 0xC8) = unifiedEnd - unifiedStart;
	} else {
		// Leaving Apple's old range here would expose addresses outside the
		// PF-provisioned VF interval. This allocator requires a usable range.
		getMember<uint64_t>(that, 0xC0) = 0;
		getMember<uint64_t>(that, 0xC8) = 0;
		SYSLOG("ngreen", "V239: VF assignment cannot satisfy unified GGTT allocator");
		return false;
	}

	SYSLOG("ngreen", "V217: patched IGMemoryManager GGTT ranges global=[0x%llx,+0x%llx] unified=[0x%llx,+0x%llx]",
	       static_cast<unsigned long long>(gVfGGTTBase),
	       static_cast<unsigned long long>(gVfGGTTSize),
	       static_cast<unsigned long long>(getMember<uint64_t>(that, 0xC0)),
	       static_cast<unsigned long long>(getMember<uint64_t>(that, 0xC8)));
	return true;
}

bool Gen11::IGHardwareGlobalPageTableInitWithOptions(void *that,
                                                     void *accelerator,
                                                     const NGIGAddressRange &range,
                                                     void *mmioBase,
                                                     uint64_t dummyPage,
                                                     uint32_t options)
{
	if (vfIdentifyDevice() == VfIdentity::Physical)
		return FunctionCast(IGHardwareGlobalPageTableInitWithOptions,
		                    callback->oIGHardwareGlobalPageTableInitWithOptions)(that,
		                                                                         accelerator,
		                                                                         range,
		                                                                         mmioBase,
		                                                                         dummyPage,
		                                                                         options);
	if (!that || !accelerator || !NGGgtt::nativePhysicalRange(dummyPage, 0x1000)) {
		vfMarkProtocolFault("invalid VF GGTT receiver or truncated dummy DMA address");
		return false;
	}
	if (!vfBootstrapBinder())
		return false;

	// Native init's second loop clears from range.end to range.start + 4 GiB,
	// not to 4 GiB. A nonzero VF base would index beyond the 8 MiB PTE window.
	// Suppress both native loops for every VF transport (i915 nop_clear_range).
	// In this inspected payload the first loop is skipped by unsigned wrap,
	// and length == 4 GiB skips the second. No fabricated range is published.
	const NGIGAddressRange noDirectClear {UINT64_MAX, 0x100000000ULL};
	if (gVfDirectGGTT) {
		auto *cb = NGreen::callback;
		if (!cb || !cb->setRMMIOIfNecessary())
			return false;
		if (!cb->getRMMIOAddress() || cb->getRMMIOLength() < kVfDirectBar0Bytes) {
			SYSLOG("ngreen", "V219: full BAR0 mapping unavailable for direct GGTT");
			return false;
		}

		// Apple's accelerator-created mapping covered only the lower MMIO pages in
		// V216, so initWithOptions faulted when it reached BAR0+8 MiB.  Reuse the
		// complete IOPCIDevice BAR0 mapping owned by NootedGreen; this is the same
		// direct GGTT transport selected by Linux for this media-version-12 VF.
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

	// Preserve Apple's base-class/object setup while making both of its direct
	// PTE-clearing loops empty.  Linux intentionally installs nop_clear_range
	// for this VF: the PF owns clearing and the upper BAR0 PTE window is absent.
	const bool result = FunctionCast(IGHardwareGlobalPageTableInitWithOptions,
	                                 callback->oIGHardwareGlobalPageTableInitWithOptions)(that,
	                                                                                      accelerator,
	                                                                                      noDirectClear,
	                                                                                      mmioBase,
	                                                                                      dummyPage,
	                                                                                      options);
	if (!result)
		return false;

	getMember<uint64_t *>(that, 0x28) = gVfGGTTShadow;
	gVfGlobalPageTable = that;
	SYSLOG("ngreen", "V217: global GGTT shadow installed; Apple range=[0x%llx,+0x%llx] PF=[0x%llx,+0x%llx]",
	       static_cast<unsigned long long>(range.start),
	       static_cast<unsigned long long>(range.length),
	       static_cast<unsigned long long>(gVfGGTTBase),
	       static_cast<unsigned long long>(gVfGGTTSize));
	return true;
}

bool Gen11::IGHardwareGlobalPageTableMapRange(void *that,
                                              const NGIGAddressRange &range,
                                              uint64_t physical,
                                              uint64_t flags)
{
	// Native writes PTEs before returning. Validating only in the subsequent
	// relay sync is too late to protect either the shadow or the MMIO aperture.
	if (gVfIdentity != VfIdentity::Physical &&
	    (gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady ||
	     gVfSubmissionStopped || gVfProtocolFault ||
	     !that || that != gVfGlobalPageTable ||
	     !NGGgtt::contains(gVfGGTTBase, gVfGGTTSize, range.start, range.length) ||
	     !NGGgtt::nativePhysicalRange(physical, range.length))) {
		vfMarkProtocolFault("invalid VF GGTT map range or transport state");
		return false;
	}
	const bool result = FunctionCast(IGHardwareGlobalPageTableMapRange,
	                                 callback->oIGHardwareGlobalPageTableMapRange)(that,
	                                                                                range,
	                                                                                physical,
	                                                                                flags);
	if (!result || gVfIdentity != VfIdentity::Virtual ||
	    that != gVfGlobalPageTable || gVfDirectGGTT)
		return result;
	return vfSyncShadowRange(range);
}

bool Gen11::IGHardwareGlobalPageTableMapRangeRotated(void *that,
                                                     void *rangeIterator,
                                                     void *physicalIterator,
                                                     uint64_t flags)
{
	if (gVfIdentity == VfIdentity::Physical)
		return FunctionCast(IGHardwareGlobalPageTableMapRangeRotated,
		                                 callback->oIGHardwareGlobalPageTableMapRangeRotated)(that,
		                                                                                       rangeIterator,
		                                                                                       physicalIterator,
		                                                                                       flags);

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
	    that != gVfGlobalPageTable || !rangeIterator || !physicalIterator) {
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

	auto *pteBase = getMember<volatile uint64_t *>(that, 0x28);
	if (!pteBase || (!gVfDirectGGTT && pteBase != gVfGGTTShadow)) {
		vfMarkProtocolFault("VF rotated GGTT PTE aperture mismatch");
		return false;
	}
	if (gVfDirectGGTT) {
		auto *cb = NGreen::callback;
		if (!cb || !cb->getRMMIOAddress() ||
		    cb->getRMMIOLength() < kVfDirectBar0Bytes ||
		    pteBase != reinterpret_cast<volatile uint64_t *>(
		        const_cast<UInt32 *>(cb->getRMMIOAddress()) +
		        kVfGGTTPteBase / sizeof(UInt32))) {
			vfMarkProtocolFault("VF rotated GGTT direct aperture mismatch");
			return false;
		}
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

	const uint64_t pteFlags = flags & UINT64_C(0xFFFFFF8000000FFE);
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
				(physical & UINT64_C(0x7FFFFFF000)) | pteFlags | 1U;
			physical += 0x1000ULL;
			mappedPages++;
		}
		offset += length;
	}

	const uint64_t totalPages = spec.rangeLength >> 12;
	if (!validSegments || offset != segments->length || mappedPages != totalPages) {
		const uint64_t dummyPte =
			(getMember<uint64_t>(that, 0x38) & UINT64_C(0x7FFFFFF000)) | 1U;
		for (uint64_t source = 0; source < mappedPages; source++) {
			uint64_t destination = 0;
			if (NGGgttRotation::destination(spec, source, destination))
				pteBase[destination >> 12] = dummyPte;
		}
		OSSynchronizeIO();
		__asm__ volatile("sfence" ::: "memory");
		segments->memory->release();
		vfMarkProtocolFault("VF rotated physical segment changed during mapping");
		return false;
	}

	rotated->sourcePage = static_cast<uint32_t>(totalPages);
	rotated->cursor = spec.rangeStart + spec.rangeLength;
	OSSynchronizeIO();
	__asm__ volatile("sfence" ::: "memory");
	segments->memory->release();

	if (!gVfDirectGGTT && !vfSyncShadowRange(*rotated->range)) {
		vfMarkProtocolFault("failed to publish VF rotated GGTT shadow range");
		return false;
	}
	return true;
}

void Gen11::IGHardwareGlobalPageTableUnmapRange(void *that,
                                                const NGIGAddressRange &range)
{
	// A void unmap cannot tell its caller not to free/reuse DMA backing.
	// Refuse to continue teardown on invalid input or a faulted VF; silently
	// returning would turn a skipped PTE write into a use-after-free risk.
	PANIC_COND(gVfIdentity != VfIdentity::Physical &&
		(gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady ||
		 (gVfProtocolFault && !gVfDmaQuiesced) ||
		 !that || that != gVfGlobalPageTable ||
		 !NGGgtt::contains(gVfGGTTBase, gVfGGTTSize, range.start, range.length)),
		"ngreen", "Cannot safely complete VF GGTT unmap; refusing DMA backing release");
	const auto invalidation = NGGgtt::unmapInvalidation(
		gVfCtbEverEnabled != 0, gVfCtbEnabled != 0,
		gVfCtbStopped != 0, gVfProtocolFault != 0,
		gVfDmaQuiesced != 0);
	PANIC_COND(gVfIdentity == VfIdentity::Virtual &&
	           invalidation == NGGgtt::TlbInvalidation::Unsafe,
		"ngreen", "VF GGTT unmap started without a usable TLB transport");
	FunctionCast(IGHardwareGlobalPageTableUnmapRange,
	             callback->oIGHardwareGlobalPageTableUnmapRange)(that, range);
	if (gVfIdentity != VfIdentity::Virtual)
		return;

	// Apple releaseRange calls this virtual method, sets only a deferred flush
	// bit and can then return to a caller that releases the DMA mapping. Relay
	// (when needed) and a completed heavy GuC invalidation are therefore part of
	// the unmap transaction, not optional diagnostics. The sfence matches the
	// stock physical invalidator and drains direct BAR0 PTE stores first.
	PANIC_COND(!gVfDirectGGTT && !vfSyncShadowRange(range),
		"ngreen", "VF GGTT unmap relay failed before DMA release");
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
	if (gVfIdentity != VfIdentity::Physical &&
	    (gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady ||
	     gVfSubmissionStopped || gVfProtocolFault ||
	     !that || that != gVfGlobalPageTable ||
	     !NGGgtt::contains(gVfGGTTBase, gVfGGTTSize, range.start, range.length))) {
		vfMarkProtocolFault("invalid VF dummy GGTT range or transport state");
		return false;
	}
	const bool result = FunctionCast(IGHardwareGlobalPageTableMapRangeDummy,
	                                 callback->oIGHardwareGlobalPageTableMapRangeDummy)(that,
	                                                                                     range,
	                                                                                     flags);
	if (!result || gVfIdentity != VfIdentity::Virtual ||
	    that != gVfGlobalPageTable || gVfDirectGGTT)
		return result;
	return vfSyncShadowRange(range);
}

bool Gen11::IGAccelTaskIsKernelGPUTask(const void *that)
{
	const bool original = FunctionCast(IGAccelTaskIsKernelGPUTask,
	                                   callback->oIGAccelTaskIsKernelGPUTask)(that);
	if (original || NGreen::callback->isRealTGL || that == nullptr)
		return original;

	void *task = const_cast<void *>(that);
	void *accelerator = getMember<void *>(task, 0x10);
	if (accelerator == nullptr)
		return original;

	void *kernelTask = getMember<void *>(accelerator, 0x150);
	if (kernelTask == nullptr) {
		SYSLOG("ngreen", "V214: bootstrapping first VF task from Global GTT");
		return true;
	}

	return original;
}

void *ccont;
void *ccont2;

//FB Hooks

uint64_t Gen11::AppleIntelScalerinit(AppleIntel::AppleIntelScaler *that, uint32_t pipeIndex)
{
	auto ret = FunctionCast(AppleIntelScalerinit, callback->oAppleIntelScalerinit)(that, pipeIndex);
	if (ccont)
		that->fWriteAccessor = ccont;
	if (ccont2)
		that->fController = reinterpret_cast<AppleIntel::AppleIntelBaseController *>(ccont2);
	return ret;
}

uint64_t Gen11::AppleIntelPlaneinit(AppleIntel::AppleIntelPlane *that, uint32_t pipeIndex)
{
	auto ret = FunctionCast(AppleIntelPlaneinit, callback->oAppleIntelPlaneinit)(that, pipeIndex);
	if (ccont)
		getMember<void *>(that, 0x90) = ccont; // fWriteAccessor — ccont must NOT go to real fRegCache at +0x88

	return ret;
}

void Gen11::disableScaler(AppleIntel::AppleIntelScaler *that, bool disable)
{
	if (ccont)
		that->fWriteAccessor = ccont;
	FunctionCast(disableScaler, callback->odisableScaler)(that, disable);
}

void Gen11::enablePlane(AppleIntel::AppleIntelPlane *that, bool enable)
{
	if (ccont)
		getMember<void *>(that, 0x90) = ccont; // fWriteAccessor — ccont must NOT go to real fRegCache at +0x88
	FunctionCast(enablePlane, callback->oenablePlane)(that, enable);

}

void Gen11::programPipeScaler(AppleIntel::AppleIntelScaler *that, AppleIntel::AppleIntelDisplayPath *displayPath)
{
	if (ccont)
		that->fWriteAccessor = ccont;
	FunctionCast(programPipeScaler, callback->oprogramPipeScaler)(that, displayPath);
}

// V400: AppleIntelScaler::setupPipeScaler(AppleIntelDisplayPath *, CRTCParams *)
// ORIGIN-LEVEL FIX for seam-joining scaler wrongly enabled on single-pipe panels.
//
// Apple's setupPipeScaler enters the "Seam joining scaler enabled" branch when
// a byte gate is non-zero — disasm reveals:
//   rax = this->[+0x10]                     (AppleIntelScaler's controller ptr)
//   if [rax+0x3FD8] == 0xFFFFFFFF (device-tree absent on spoofed RPL)
//       rax += 0x1E5
//   else
//       rax += 0x1E3
//   if [rax] != 0:  enable seam joining
//   else:           skip seam joining
//
// Linux i915 on this exact hardware confirms `bigjoiner: no, pipes: 0x0` —
// dual-pipe joining must NOT be active for this single-pipe 2560x1600 eDP panel.
// Apple is doing it anyway, producing the per-row offset / fragmented pattern.
//
// Fix: before calling original, zero BOTH gate bytes (+0x1E3 and +0x1E5) at
// this->[+0x10]. Belt-and-suspenders covers both device-tree-present and absent
// branches. Origin-level — modifies Apple's own decision input, not MMIO. The
// rest of Apple's code paths run unchanged with seam joining naturally skipped.
//
// Gated on !isRealTGL since real TGL doesn't need this.
//
// Post-call still logs CRTCParams scaler fields to verify SEAM_EXCESS==0.
void Gen11::setupPipeScaler(AppleIntel::AppleIntelScaler *that, AppleIntel::AppleIntelDisplayPath *path, AppleIntel::CRTCParams *params)
{
	// ccont fixup (same as programPipeScaler) — needed because V204 init hooks
	// don't always populate ccont; original would crash on this=NULL ccont path.
	that->fWriteAccessor = ccont;


	// Pre-call snapshot — captures values BEFORE setupPipeScaler runs so we can
	// tell whether THIS function sets PIPE_SEAM_EXCESS=0x1 or whether something
	// upstream did. Also captures the gate bytes our zero-the-gate attempt targeted.
	uint32_t pre_seam = 0, pre_winsz = 0, pre_winpos = 0, pre_hphase = 0;
	uint8_t pre_gate1E3 = 0xFF, pre_gate1E5 = 0xFF;
	bool have_base = false;
	if (NGreen::callback != nullptr && !NGreen::callback->isRealTGL) {
		if (params != nullptr) {
			pre_seam   = params->PIPE_SEAM_EXCESS;
			pre_winsz  = params->PS_PS_WIN_SZ;
			pre_winpos = params->PS_PS_WIN_POS;
			pre_hphase = params->PS_HPHASE;
		}
		if (that != nullptr) {
			auto *base = reinterpret_cast<uint8_t *>(that->fController);
			if (base != nullptr) {
				have_base    = true;
				pre_gate1E3  = base[0x1E3];
				pre_gate1E5  = base[0x1E5];
				// Still try the gate clear — harmless if not the right gate.
				base[0x1E3]  = 0;
				base[0x1E5]  = 0;
			}
		}
	}

	FunctionCast(setupPipeScaler, callback->osetupPipeScaler)(that, path, params);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || params == nullptr)
		return;

	static int v400Count = 0;
	if (v400Count >= 12) return;
	++v400Count;

	// PIPE_SRCSZ per Intel spec: high 16 = horizontal-1, low 16 = vertical-1.
	const uint32_t src_w = ((params->PIPE_SRCSZ >> 16) & 0xFFFF) + 1;
	const uint32_t src_h = (params->PIPE_SRCSZ & 0xFFFF) + 1;
	SYSLOG("ngreen", "V400[%d]: setupPipeScaler %s gates[+0x1E3,+0x1E5]=(0x%02x,0x%02x) "
		   "PRE: SEAM=0x%x WINSZ=0x%x WINPOS=0x%x HPHASE=0x%x | "
		   "POST: SRC=%ux%u SEAM=0x%x WINSZ=0x%x WINPOS=0x%x HPHASE=0x%x "
		   "HTOTAL=0x%x VTOTAL=0x%x TRANS_CONF=0x%x",
		   v400Count, have_base ? "base-ok" : "NO-base",
		   pre_gate1E3, pre_gate1E5,
		   pre_seam, pre_winsz, pre_winpos, pre_hphase,
		   src_w, src_h, params->PIPE_SEAM_EXCESS, params->PS_PS_WIN_SZ, params->PS_PS_WIN_POS, params->PS_HPHASE,
		   params->TRANS_HTOTAL, params->TRANS_VTOTAL, params->TRANS_CONF);
}

// V401: AppleIntelBaseController::paramsSurfCompare — READ-ONLY logger.
// Fires per flip. Apple uses the return value to decide whether to fully reprogram
// the plane. We just log the inputs.
bool Gen11::paramsSurfCompare(AppleIntel::AppleIntelBaseController *that,
                              AppleIntel::CRTCParams *p1, AppleIntel::CRTCParams *p2,
                              AppleIntel::PLANEPARAMS *pl1, AppleIntel::PLANEPARAMS *pl2)
{
	// V408: force linear tiling in the NEW (pl2) PLANEPARAMS before the comparison.
	// hwRegsNeedUpdate has no PLANEPARAMS argument — it cannot fix tiling there.
	// paramsSurfCompare is the last point where pl2 can be patched before its values
	// trigger raWriteRegister32 writes for PLANE_CTL and PLANE_STRIDE.
	// Old (pl1/hardware) is already linear from UEFI; patching pl2 to match suppresses
	// the X-tiled commit without any MMIO intercept.
	/*if (NGreen::callback && !NGreen::callback->isRealTGL && pl2) {
		pl2->PLANE_CTL    = (pl2->PLANE_CTL & ~(0x7u << 10));  // bits[12:10] = 000 → linear
		pl2->PLANE_STRIDE = 0xa0;                               // 2560×4 / 64 = 160 = 0xa0
	}*/

	// Capture Apple's natural values BEFORE any force, for V401 logging.
	uint32_t nat_ctl_pl1    = pl1 ? pl1->PLANE_CTL    : 0;
	uint32_t nat_stride_pl1 = pl1 ? pl1->PLANE_STRIDE : 0;
	uint32_t nat_surf_pl1   = pl1 ? pl1->PLANE_SURF   : 0;
	uint32_t nat_ctl_pl2    = pl2 ? pl2->PLANE_CTL    : 0;
	uint32_t nat_stride_pl2 = pl2 ? pl2->PLANE_STRIDE : 0;
	uint32_t nat_surf_pl2   = pl2 ? pl2->PLANE_SURF   : 0;

	if (!NGreen::callback->isRealTGL && pl2) {
		pl2->PLANE_CTL    = (pl2->PLANE_CTL & ~(0x7u << 10));  // bits[12:10] = 000 → linear
		pl2->PLANE_STRIDE = 0xa0;                               // 2560×4 / 64 = 160 = 0xa0
	}

	bool ret = FunctionCast(paramsSurfCompare, callback->oparamsSurfCompare)(that, p1, p2, pl1, pl2);

	SYSLOG("ngreen", "V401, original ret [%d]", ret);
	// ADL-P: the ICL/TGL driver's SURF-only fast-path (used when paramsSurfCompare returns
	// false) does not work correctly under this spoof — PLANE_SURF never gets written on
	// subsequent flips.  Force a full reprogram whenever the surface address actually changes.
	if (!NGreen::callback->isRealTGL && !ret && pl1 && pl2) {
		if (pl1->PLANE_SURF != pl2->PLANE_SURF)
			ret = true;
	}

	// Real fix for seam scaler 2: Apple reads PIPE_SEAM_EXCESS and PS_PS_WIN_SZ from p2
	// AFTER paramsSurfCompare returns, to fill the GPU DSB buffer entry for PS_WIN_SZ_2_A
	// (0x68274). On a single-pipe ADL-P panel there is no seam scaler 2; if the DSB gets
	// 0x3fff04f there, the pipeline aborts the flip immediately (param2=0 at V405).
	// Zeroing here prevents any non-zero seam value from reaching the DSB.
	if (!NGreen::callback->isRealTGL && p2 != nullptr) {
		p2->PIPE_SEAM_EXCESS = 0;
		p2->PS_PS_WIN_SZ     = 0;
	}

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL) return ret;

	static int v401Count = 0;
	if (v401Count >= 20) return ret;
	++v401Count;

	uint32_t old_ctl    = pl1 ? pl1->PLANE_CTL    : 0;
	uint32_t new_ctl    = pl2 ? pl2->PLANE_CTL    : 0;
	uint32_t old_stride = pl1 ? pl1->PLANE_STRIDE : 0;
	uint32_t new_stride = pl2 ? pl2->PLANE_STRIDE : 0;
	uint32_t old_surf   = pl1 ? pl1->PLANE_SURF   : 0;
	uint32_t new_surf   = pl2 ? pl2->PLANE_SURF   : 0;
	uint32_t old_src    = p1  ? p1->PIPE_SRCSZ    : 0;
	uint32_t new_src    = p2  ? p2->PIPE_SRCSZ    : 0;

	// PLANE_CTL tiling is bits[12:10]: 000=linear, 001=X-tile, 100=Tile4.
	uint32_t old_tile     = (old_ctl     >> 10) & 0x7;
	uint32_t new_tile     = (new_ctl     >> 10) & 0x7;
	uint32_t nat_tile_pl1 = (nat_ctl_pl1 >> 10) & 0x7;
	uint32_t nat_tile_pl2 = (nat_ctl_pl2 >> 10) & 0x7;

	SYSLOG("ngreen", "V401[%d]: paramsSurfCompare ret=%d | "
		   "OLD: CTL=0x%x tile=%u STRIDE=0x%x SURF=0x%x SRC=0x%x | "
		   "NEW: CTL=0x%x tile=%u STRIDE=0x%x SURF=0x%x SRC=0x%x | "
		   "NAT plold: tile_pl1=%u stride_pl1=0x%x surf_pl1=0x%x | "
		   "NAT: plnew tile_pl2=%u stride_pl2=0x%x surf_pl2=0x%x",
		   v401Count, ret,
		   old_ctl, old_tile, old_stride, old_surf, old_src,
		   new_ctl, new_tile, new_stride, new_surf, new_src,
		   nat_tile_pl1, nat_stride_pl1, nat_surf_pl1,
		   nat_tile_pl2, nat_stride_pl2, nat_surf_pl2);

	return ret;
}

// V402: AppleIntelBaseController::setupDSCEngineParams — READ-ONLY logger.
// Logs entry of DSC config path. Linux says DSC=off on our panel; if Apple still
// goes through this with non-zero DSC bits, the call is the origin of any DSC
// corruption and disabling it here would replace V300's CRTCParams write-back.
void Gen11::setupDSCEngineParams(AppleIntel::AppleIntelBaseController *that,
                                 AppleIntel::AppleIntelFramebuffer *fb,
                                 AppleIntel::CRTCParams *params,
								 AppleIntel::AppleIntelDisplayPath *path,
                                 IODetailedTimingInformationV2 *timing)
{
	// Pre-call snapshot: did anyone set DSC fields before us?
	uint32_t pre_dsc_engine = 0, pre_dsc_joiner = 0, pre_pps0 = 0;
	if (NGreen::callback != nullptr && !NGreen::callback->isRealTGL && params != nullptr) {
		pre_dsc_engine = params->DSC_ENGINE_SEL;
		pre_dsc_joiner = params->DSC_JOINER_CTL;
		pre_pps0       = params->PPS_0;
	}

	FunctionCast(setupDSCEngineParams, callback->osetupDSCEngineParams)(that, fb, params, path, timing);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || params == nullptr) return;

	static int v402Count = 0;
	if (v402Count >= 6) return;
	++v402Count;

	const bool dsc_was_enabled = (params->DSC_ENGINE_SEL & 0xF0000000u) != 0;
	SYSLOG("ngreen", "V402[%d]: setupDSCEngineParams %s | "
		   "PRE: DSC_ENGINE=0x%x DSC_JOINER=0x%x PPS_0=0x%x | "
		   "POST: DSC_ENGINE=0x%x DSC_JOINER=0x%x PPS_0=0x%x PPS_16=0x%x",
		   v402Count,
		   dsc_was_enabled ? "*** DSC ENABLED (vs Linux=off) ***" : "(DSC stayed off)",
		   pre_dsc_engine, pre_dsc_joiner, pre_pps0,
		   params->DSC_ENGINE_SEL, params->DSC_JOINER_CTL, params->PPS_0, params->PPS_16);
}

// V403: AppleIntelBaseController::SetupParams — post-call.
// Logs full CRTCParams + causation test: force PIPE_SEAM_EXCESS=0 and PS_PS_WIN_SZ=0
// so any seam-join config Apple set up earlier in the modeset is wiped after the
// master builder finishes. If fragmentation disappears with these zeroed, seam was
// the cause. If unchanged, seam fields are irrelevant and we look elsewhere.
void Gen11::setupParams(AppleIntel::AppleIntelBaseController *that,
                        AppleIntel::AppleIntelFramebuffer *fb,
						AppleIntel::AppleIntelDisplayPath *path,
                        AppleIntel::CRTCParams *params,
                        const IODetailedTimingInformationV2 *timing)
{
	FunctionCast(setupParams, callback->osetupParams)(that, fb, path, params, timing);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || params == nullptr) return;

	// V403-zero: causation test. Wipe seam-join CRTCParams fields BEFORE any
	// post-SetupParams consumer reads them (paramsSurfCompare, MMIO emit etc).
	uint32_t pre_seam  = params->PIPE_SEAM_EXCESS;
	uint32_t pre_winsz = params->PS_PS_WIN_SZ;
	uint32_t pre_hphase = params->PS_HPHASE;
	if (pre_seam != 0 || pre_winsz != 0 || pre_hphase != 0) {
		params->PIPE_SEAM_EXCESS = 0;
		params->PS_PS_WIN_SZ     = 0;
		params->PS_HPHASE        = 0;
	}

	static int v403Count = 0;
	if (v403Count >= 6) return;
	++v403Count;

	SYSLOG("ngreen", "V403[%d]: SetupParams post-call CRTCParams: "
		   "CLK_SEL=0x%x DDI_FUNC_CTL=0x%x DDI_FUNC_CTL2=0x%x MSA_MISC=0x%x "
		   "HTOTAL=0x%x HBLANK=0x%x HSYNC=0x%x VTOTAL=0x%x VBLANK=0x%x VSYNC=0x%x "
		   "PIPE_SRCSZ=0x%x TRANS_CONF=0x%x | "
		   "PS_WIN_POS=0x%x | PRE: SEAM=0x%x WINSZ=0x%x HPHASE=0x%x -> NOW 0 | "
		   "DSC_ENGINE=0x%x DSC_JOINER=0x%x PPS_0=0x%x PPS_16=0x%x",
		   v403Count,
		   params->TRANS_CLK_SEL, params->TRANS_DDI_FUNC_CTL, params->TRANS_DDI_FUNC_CTL2, params->TRANS_MSA_MISC,
		   params->TRANS_HTOTAL, params->TRANS_HBLANK, params->TRANS_HSYNC,
		   params->TRANS_VTOTAL, params->TRANS_VBLANK, params->TRANS_VSYNC,
		   params->PIPE_SRCSZ, params->TRANS_CONF,
		   params->PS_PS_WIN_POS,
		   pre_seam, pre_winsz, pre_hphase,
		   params->DSC_ENGINE_SEL, params->DSC_JOINER_CTL, params->PPS_0, params->PPS_16);
}

// V404: AppleIntelBaseController::setupPipeWatermarks — READ-ONLY pre/post.
// Tells us if setupPipeWatermarks is what sets PIPE_SEAM_EXCESS=0x1. Called
// from inside SetupParams before setupPipeScaler — if pre=0 post=1 here, this
// is our seam-join origin (rather than setupPipeScaler).
void Gen11::setupPipeWatermarks(AppleIntel::AppleIntelBaseController *that,
                                AppleIntel::AppleIntelFramebuffer *fb,
								AppleIntel::AppleIntelDisplayPath *path,
                                AppleIntel::CRTCParams *params)
{
	uint32_t pre_seam = 0, pre_winsz = 0, pre_winpos = 0;
	if (NGreen::callback != nullptr && !NGreen::callback->isRealTGL && params != nullptr) {
		pre_seam   = params->PIPE_SEAM_EXCESS;
		pre_winsz  = params->PS_PS_WIN_SZ;
		pre_winpos = params->PS_PS_WIN_POS;
	}

	FunctionCast(setupPipeWatermarks, callback->osetupPipeWatermarks)(that, fb, path, params);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || params == nullptr) return;

	static int v404Count = 0;
	if (v404Count >= 4) return;
	++v404Count;

	SYSLOG("ngreen", "V404[%d]: setupPipeWatermarks | "
		   "PRE: SEAM=0x%x WINSZ=0x%x WINPOS=0x%x | "
		   "POST: SEAM=0x%x WINSZ=0x%x WINPOS=0x%x %s",
		   v404Count,
		   pre_seam, pre_winsz, pre_winpos,
		   params->PIPE_SEAM_EXCESS, params->PS_PS_WIN_SZ, params->PS_PS_WIN_POS,
		   (pre_seam == 0 && params->PIPE_SEAM_EXCESS != 0)
			   ? "*** SEAM SET HERE ***"
			   : (pre_seam != 0 ? "(seam pre-existed)" : "(no seam)"));
}

// V405: AppleIntelPlane::configureColorPipeLine(FlipTransactionArgs*, bool)
//
// Dispatches to configurePipePostCSCGamma_{8,10,12,12SEG}Bit based on a BPC/gamma
// mode selector field within FlipTransactionArgs.  The field is at byte offset 0x1C
// (IDA's "param_1[7].Tiling" — element 7 of a dword array inside the struct).
//
// For our 2560×1600 eDP at 8bpc this always hits case 0 → 8Bit, which writes:
//   GAMMA_MODE   (0x4A480, Pipe A) = 0x0  (LEGACY_8BIT)
//   PIPE_MISC    (0x70030, Pipe A) bits[5:3] = 0b000 (8 bpc)
//
// Display 13 (ADL-P) uses the same register addresses as Display 12 (TGL), so
// Apple's writes are structurally correct.  The hook logs pre/post snapshots so
// we can confirm the hardware actually sees the right values each flip cycle.
void Gen11::configureColorPipeLine(AppleIntel::AppleIntelPlane *that, AppleIntel::FlipTransactionArgs *flipArgs, bool param_2)
{
	static int v405Count = 0;

	uint32_t pre_gamma = 0, pre_misc = 0;
	uint8_t  sel = 0xFF;

	if (NGreen::callback != nullptr && !NGreen::callback->isRealTGL) {
		pre_gamma = NGreen::callback->readReg32(0x4A480);  // GAMMA_MODE Pipe A
		pre_misc  = NGreen::callback->readReg32(0x70030);  // PIPE_MISC  Pipe A
		if (flipArgs != nullptr)
			sel = static_cast<uint8_t>(NGUnaligned::readLe32(&flipArgs->flt_001C) >> 24);

		// DIAG: log the color pipeline bitmask (FB+0x4248) to identify which bit drives PIPE_MISC bit 23.
		// The bitmask at FB+0x4248 (FB = *(that+0x68)) selects which sub-functions run inside
		// configureColorPipeLine. Bit 10 = configurePipeHDRMode candidate. Log pre/post to verify.
		if (that != nullptr) {
			void *fb = getMember<void *>(that, 0x68);
			if (fb != nullptr) {
				uint32_t bm_before = getMember<uint32_t>(fb, 0x4248);
				getMember<uint32_t>(fb, 0x4248) &= ~(1u << 10);
				uint32_t bm_after  = getMember<uint32_t>(fb, 0x4248);
				SYSLOG("ngreen", "DIAG[BM]: fb=%p bitmask 0x%08x→0x%08x param2=%d",
				       fb, bm_before, bm_after, (int)param_2);
			}
		}
	}

	FunctionCast(configureColorPipeLine, callback->oConfigureColorPipeLine)(that, flipArgs, param_2);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL) return;

	if (v405Count >= 20) return;
	++v405Count;

	const uint32_t post_gamma = NGreen::callback->readReg32(0x4A480);
	const uint32_t post_misc  = NGreen::callback->readReg32(0x70030);
	// PIPE_MISC BPC field: bits[5:3].  0=8bpc, 1=10bpc, 2=6bpc, 3=12bpc.
	const uint8_t bpc_pre  = (pre_misc  >> 3) & 0x7u;
	const uint8_t bpc_post = (post_misc >> 3) & 0x7u;
	// GAMMA_MODE encoding: 0=legacy8bit, 1=prec10bit, 2=prec12interp, 3=split
	const char *gammaName[] = {"LEGACY_8BIT", "PREC_10BIT", "PREC_12INTERP", "SPLIT"};
	const char *gname = (post_gamma <= 3) ? gammaName[post_gamma] : "unknown";

	SYSLOG("ngreen", "V405[%d]: configureColorPipeLine sel=0x%02x param2=%d | "
		   "GAMMA_MODE: 0x%x→0x%x(%s) | PIPE_MISC: 0x%x→0x%x bpc[5:3]=%u→%u",
		   v405Count, sel, (int)param_2,
		   pre_gamma, post_gamma, gname,
		   pre_misc, post_misc, bpc_pre, bpc_post);
}

// V406: CORE origin-level plane tiling/stride manipulation for ADL-P/RPL spoofed as TGL.
// Physical framebuffer pages are CPU-written in linear order. Apple's IOSurface allocates
// with tiling enum=0 (X-tiled) → configurePlane ORs PLANE_CTL bit10 → display engine
// fetches X-tile geometry → black screen.
//
// Linux i915 on this exact ADL-P hardware: modifier=0x0 (LINEAR), stride=0x2800 bytes (0x14 units)
//
// FlipTransactionArgs+0x3c tiling enum (from disasm VA 0x59ad-0x59c3):
//   0x0 → X-tiled:  ORs 0x400 into PLANE_CTL bits[12:10] = 001
//   0x1 → Y-tiled:  ORs 0x1000 into bits[12:10] = 100
//   else → linear:  ANDs 0xffe7e7ff (clears bits[12:10] = 000)
//
// AppleIntelPlane shadow offsets (from configurePlane disasm):
//   +0x100 = PLANE_CTL shadow (tiling bits built here, read @ VA 0x5a41)
//   +0x104 = PLANE_STRIDE shadow (read @ VA 0x6867)
//   +0x154 = PLANE_COLOR_CTL shadow (post-OR write @ VA 0x6687)
//
// ============================================================================
// === CHOOSE ONE: Main approach (uncomment your choice below) ===
// ============================================================================

// ✓ APPROACH 1: PRE-CALL TILING PATCH (origin-level, Apple's natural path)
#define V406_APPROACH_TILING_PATCH 1

// Alternative approaches below (comment out APPROACH 1 if trying these):
// #define V406_APPROACH_POST_SHADOW_PATCH 1      // Post-call shadow field write
// #define V406_APPROACH_STRIDE_DIVISOR 1          // Stride divisor manipulation
// #define V406_APPROACH_DUAL_TILING_STRIDE 1      // Tiling + stride combined
// #define V406_APPROACH_DIAGNOSTIC_ONLY 1         // Pass-through with logging

// ============================================================================
// === TILING ENUM SELECTOR (active for approaches using pre-call patch) ===
// ============================================================================

// #define V406_TILING_VALUE 2       // linear — CPU BAR2 compositor path
// #define V406_TILING_VALUE 1       // Y-tiled
#define V406_TILING_VALUE 0          // X-tiled — matches Apple IOSurface allocator

// ============================================================================
// === STRIDE FORCE (active for stride-manipulation approaches) ===
// ============================================================================

#define V406_STRIDE_FORCE_ENABLE 0              // stride handled by paramsSurfCompare V408
#define V406_STRIDE_VALUE 0x14                  // X-tile: 2560×4 / 512 = 20 = 0x14

// ============================================================================
// === PLANE_CTL BITS MANIPULATION (advanced—leave as 0 unless testing) ===
// ============================================================================

#define V406_PLANE_CTL_CLEAR_MASK 0x0           // Bits to clear (0xFFE7E7FF = tiling bits)
#define V406_PLANE_CTL_SET_MASK 0x0             // Bits to set (0x400=X-tiled, 0x1000=Y-tiled)

// ============================================================================
// === SHADOW FIELD OFFSET OVERRIDES (for post-call patching) ===
// ============================================================================

#define V406_STRIDE_SHADOW_OFFSET 0x104         // AppleIntelPlane+0x104 = PLANE_STRIDE
#define V406_CTL_SHADOW_OFFSET 0x100            // AppleIntelPlane+0x100 = PLANE_CTL

// ============================================================================

void Gen11::configurePlane(AppleIntel::AppleIntelPlane *that, AppleIntel::FlipTransactionArgs *flipArgs)
{
	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || flipArgs == nullptr) {
		FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);
		return;
	}

	// Pre-call snapshot
	uint32_t incomingTiling = 0;
	if (flipArgs != nullptr)
		incomingTiling = flipArgs->TilingEnum;

	static int v406CallCount = 0;
	static int v406LogCount = 0;
	++v406CallCount;

#ifdef V406_APPROACH_TILING_PATCH
	// ===== APPROACH 1: PRE-CALL TILING PATCH =====
	// Patch FlipTransactionArgs+0x3c before calling original
	// Apple's configurePlane reads this, takes the corresponding disasm branch,
	// builds PLANE_CTL with correct tiling bits natively
	
	uint32_t *tilingField = &flipArgs->TilingEnum;
	const uint32_t savedTiling = *tilingField;
	*tilingField = V406_TILING_VALUE;

	if (v406LogCount < 20) {
		++v406LogCount;
		SYSLOG("ngreen", "V406[%d]: TILING_PATCH call#%d | "
			   "Incoming tiling=0x%x → Patched=0x%x (%s)",
			   v406LogCount, v406CallCount, incomingTiling, V406_TILING_VALUE,
			   (V406_TILING_VALUE == 0) ? "X-tiled" :
			   (V406_TILING_VALUE == 1) ? "Y-tiled" : "linear/else");
	}

	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	*tilingField = savedTiling;   // restore

#elif defined(V406_APPROACH_POST_SHADOW_PATCH)
	// ===== APPROACH 2: POST-CALL SHADOW PATCH =====
	// Call original, then modify AppleIntelPlane shadow fields directly
	
	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	if (that != nullptr) {
		// Patch PLANE_CTL shadow at +0x100 (or custom offset)
		uint32_t &ctlShadow = getMember<uint32_t>(that, V406_CTL_SHADOW_OFFSET);
		const uint32_t ctlBefore = ctlShadow;
		
		// Clear tiling bits if needed
		if (V406_PLANE_CTL_CLEAR_MASK != 0)
			ctlShadow &= ~V406_PLANE_CTL_CLEAR_MASK;
		
		// Set specific bits if needed
		if (V406_PLANE_CTL_SET_MASK != 0)
			ctlShadow |= V406_PLANE_CTL_SET_MASK;
		
		if (v406LogCount < 20) {
			++v406LogCount;
			SYSLOG("ngreen", "V406[%d]: POST_SHADOW_PATCH call#%d | "
				   "PLANE_CTL shadow @+0x%x: 0x%x → 0x%x (clear=0x%x set=0x%x)",
				   v406LogCount, v406CallCount, V406_CTL_SHADOW_OFFSET,
				   ctlBefore, ctlShadow, V406_PLANE_CTL_CLEAR_MASK, V406_PLANE_CTL_SET_MASK);
		}

		// Optionally patch STRIDE shadow
		if (V406_STRIDE_FORCE_ENABLE) {
			uint32_t &strideShadow = getMember<uint32_t>(that, V406_STRIDE_SHADOW_OFFSET);
			const uint32_t strideBefore = strideShadow;
			strideShadow = V406_STRIDE_VALUE;
			if (v406LogCount < 20) {
				SYSLOG("ngreen", "V406[%d]: STRIDE shadow @+0x%x: 0x%x → 0x%x",
					   v406LogCount, V406_STRIDE_SHADOW_OFFSET, strideBefore, V406_STRIDE_VALUE);
			}
		}
	}

#elif defined(V406_APPROACH_STRIDE_DIVISOR)
	// ===== APPROACH 3: STRIDE DIVISOR (experimental) =====
	// Patch stride divisor used in configurePlane's pitch/divisor calculation
	// WARNING: This requires knowing the exact location where stride divisor is stored
	// (likely in a local stack variable or controller object). This is highly experimental.
	
	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	if (v406LogCount < 20) {
		++v406LogCount;
		SYSLOG("ngreen", "V406[%d]: STRIDE_DIVISOR call#%d | "
			   "Attempting stride divisor override (experimental) | "
			   "Incoming tiling=0x%x",
			   v406LogCount, v406CallCount, incomingTiling);
	}

	// NOTE: Actual stride divisor patching would require identifying the controller
	// field or register that holds 0x200 (stride divisor for X-tiled).
	// This approach is a placeholder—override if you find the divisor location.

#elif defined(V406_APPROACH_DUAL_TILING_STRIDE)
	// ===== APPROACH 4: DUAL TILING + STRIDE =====
	// Combine pre-call tiling patch + post-call stride shadow patch
	
	uint32_t *tilingField = &flipArgs->TilingEnum;
	const uint32_t savedTiling = *tilingField;
	*tilingField = V406_TILING_VALUE;

	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	*tilingField = savedTiling;

	// Post-call: patch STRIDE shadow
	if (V406_STRIDE_FORCE_ENABLE && that != nullptr) {
		uint32_t &strideShadow = getMember<uint32_t>(that, V406_STRIDE_SHADOW_OFFSET);
		const uint32_t strideBefore = strideShadow;
		strideShadow = V406_STRIDE_VALUE;
		
		if (v406LogCount < 20) {
			++v406LogCount;
			SYSLOG("ngreen", "V406[%d]: DUAL_TILING_STRIDE call#%d | "
				   "Tiling=0x%x STRIDE shadow: 0x%x → 0x%x",
				   v406LogCount, v406CallCount, V406_TILING_VALUE,
				   strideBefore, V406_STRIDE_VALUE);
		}
	}

#else
	// ===== APPROACH 5: DIAGNOSTIC PASS-THROUGH =====
	// No changes—pure logging to see original behavior
	
	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	if (v406LogCount < 20) {
		++v406LogCount;
		SYSLOG("ngreen", "V406[%d]: DIAGNOSTIC_ONLY call#%d | "
			   "Incoming tiling=0x%x (unchanged)",
			   v406LogCount, v406CallCount, incomingTiling);
	}
#endif

	// Always log final state if STRIDE monitoring enabled
	if (V406_STRIDE_FORCE_ENABLE && that != nullptr && v406LogCount < 20) {
		uint32_t ctlFinal = getMember<uint32_t>(that, V406_CTL_SHADOW_OFFSET);
		uint32_t strideFinal = getMember<uint32_t>(that, V406_STRIDE_SHADOW_OFFSET);
		SYSLOG("ngreen", "V406-final[%d]: CTL@+0x%x=0x%x STRIDE@+0x%x=0x%x",
			   v406LogCount, V406_CTL_SHADOW_OFFSET, ctlFinal,
			   V406_STRIDE_SHADOW_OFFSET, strideFinal);
	}
}

void Gen11::AppleIntelPlaneupdateRegisterCache(AppleIntel::AppleIntelPlane *that)
{
	getMember<void *>(that, 0x90) = ccont; // fWriteAccessor — ccont must NOT go to real fRegCache at +0x88
	FunctionCast(AppleIntelPlaneupdateRegisterCache, callback->oAppleIntelPlaneupdateRegisterCache)(that);
}

void Gen11::AppleIntelScalerupdateRegisterCache(AppleIntel::AppleIntelScaler *that)
{
	that->fWriteAccessor = ccont;
	FunctionCast(AppleIntelScalerupdateRegisterCache, callback->oAppleIntelScalerupdateRegisterCache)(that);
}

void Gen11::disableDisplayEngine(AppleIntel::AppleIntelBaseController *that)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(disableDisplayEngine, callback->odisableDisplayEngine)(that );
}

void Gen11::enableDisplayEngine(AppleIntel::AppleIntelBaseController *that)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(enableDisplayEngine, callback->oenableDisplayEngine)(that );
}

void Gen11::computeLaneCount(AppleIntel::AppleIntelBaseController *that, const IODetailedTimingInformationV2 *timing, unsigned int linkRate, unsigned int bpp, unsigned int *laneCount) {
	if (!laneCount) return;

	// Call original first — handles all standard DP rates on both real TGL and spoofed paths.
	FunctionCast(computeLaneCount, callback->ocomputeLaneCount)(that, timing, linkRate, bpp, laneCount);

	// Real TGL: preserve Apple's result unchanged.
	if (NGreen::callback->isRealTGL)
		return;

	// For !isRealTGL (RPL spoofed): Apple's DPCD-based result (MAX_LANE_COUNT) reflects
	// the panel's capability, but UEFI/GOP may have trained the link at a different lane
	// count.  Read DDI_BUF_CTL_A to discover the actual HW-trained lane count and
	// override Apple's result if UEFI trained more lanes than Apple computed.
	//
	// DDI_BUF_CTL PORT_WIDTH field bits[3:1]:
	//   000 = x1 (1 lane),  001 = x2 (2 lanes),  011 = x4 (4 lanes)
	const uint32_t ddiA    = NGreen::callback->readReg32(0x64000);  // DDI_BUF_CTL_A
	const unsigned int width   = (ddiA >> 1) & 0x7u;
	const unsigned int hwLanes = (width >= 3) ? 4u : (width >= 1) ? 2u : 1u;

	static int v90L4Logs = 0;
	if (v90L4Logs < 10) {
		v90L4Logs++;
		SYSLOG("ngreen", "V90L4[%d]: linkRate=%u bpp=%u appleLC=%u DDI_BUF_CTL_A=0x%x hwLanes=%u",
			   v90L4Logs, linkRate, bpp, *laneCount, ddiA, hwLanes);
	}

	if (hwLanes > *laneCount)
		*laneCount = hwLanes;
}

// setupOptimalLaneCount is called by hwSetMode to pick the lane count that gets
// stored in the port's cached LinkConfig (and ultimately into TRANS_DDI_FUNC_CTL).
// Apple's implementation runs: optimal = computeLaneCount(...); then caps it to
// port->maxLaneCount (from DPCD MAX_LANE_COUNT, which is 2 on this panel).
// On !isRealTGL we instead snap the cached count to match DDI_BUF_CTL_A so that
// SetupParams builds TRANS_DDI_FUNC_CTL with the correct HW-trained lane field.
void Gen11::setupOptimalLaneCount(AppleIntel::AppleIntelBaseController *that, const IODetailedTimingInformationV2 *timing, unsigned int bpp) {
	// Always run Apple's original first to populate all other LinkConfig fields.
	FunctionCast(setupOptimalLaneCount, callback->osetupOptimalLaneCount)(that, timing, bpp);

	if (NGreen::callback->isRealTGL)
		return;

	// Read HW-trained lane count from DDI_BUF_CTL_A bits[3:1].
	// PORT_WIDTH: 0=x1, 1=x2, 3=x4.
	const uint32_t ddiA    = NGreen::callback->readReg32(0x64000);
	const unsigned int width   = (ddiA >> 1) & 0x7u;
	const unsigned int hwLanes = (width >= 3) ? 4u : (width >= 1) ? 2u : 1u;

	// The port object stores the cached optimal lane count at a known offset.
	// AppleIntelPort::setupOptimalLaneCount writes fOptimalLaneCount (confirmed
	// by IDA: str result into [x0 + offset] before returning).
	// We patch it post-call so the cap-to-DPCD logic is overridden.
	// Offset 0x148 is fOptimalLaneCount in AppleIntelPort on this kext version.
	unsigned int &cached = getMember<unsigned int>(that, 0x148);

	static int v90L5Logs = 0;
	if (v90L5Logs < 10) {
		v90L5Logs++;
		SYSLOG("ngreen", "V90L5[%d]: setupOptimalLC: was=%u DDI_BUF_CTL_A=0x%x hwLanes=%u",
			   v90L5Logs, cached, ddiA, hwLanes);
	}

	if (hwLanes > cached)
		cached = hwLanes;
}

IOReturn Gen11::wrapICLReadAUX(void *that, uint32_t address, void *buffer, uint32_t length) {

	IOReturn retVal = FunctionCast(wrapICLReadAUX, callback->orgICLReadAUX)(that, address, buffer, length);

	// V97AUX: log first ~40 AUX reads to diagnose eDP link training failures.
	static int auxLogCount = 0;
	if (auxLogCount < 40) {
		auxLogCount++;
		uint8_t *b = reinterpret_cast<uint8_t *>(buffer);
		if (length >= 2)
			SYSLOG("ngreen", "V97AUX[%d]: addr=0x%04x len=%u ret=0x%x [0]=0x%02x [1]=0x%02x",
				   auxLogCount, address, length, retVal, b ? b[0] : 0xFF, (b && length >= 2) ? b[1] : 0xFF);
		else
			SYSLOG("ngreen", "V97AUX[%d]: addr=0x%04x len=%u ret=0x%x",
				   auxLogCount, address, length, retVal);
	}

	// V98T removed: do NOT clamp DPCD[0x0100-0x0101] (LINK_BW_SET / LANE_COUNT_SET).
	// Capping these to HBR2/2-lanes caused Apple to train the link at HBR2×2 lanes.
	// V97P then wrote a 4-lane DDI_FUNC_CTL value to a 2-lane trained link → black screen.
	// UEFI already trained the link at HBR3×4 lanes; we must let Apple see those values
	// so it (re-)trains consistently and V97P's bit16-only correction stays coherent.
	if (NGreen::callback && !NGreen::callback->isRealTGL && address == 0x0100 && buffer && length >= 1) {
		auto *raw = reinterpret_cast<uint8_t *>(buffer);
		static int v98tLogs = 0;
		if (v98tLogs < 5) {
			v98tLogs++;
			if (length >= 2)
				SYSLOG("ngreen", "V98T[%d]: DPCD 0x0100 passthrough bw=0x%02x lanes=0x%02x",
					   v98tLogs, raw[0], raw[1]);
			else
				SYSLOG("ngreen", "V98T[%d]: DPCD 0x0100 passthrough bw=0x%02x (len=1)",
					   v98tLogs, raw[0]);
		}
	}

	// V99: Suppress spurious LINK_STATUS_UPDATED (DPCD[0x204] bit7) on RPL-P.
	// HDCP probing reads DPCD 0x6921d, which causes the eDP panel to assert IRQ_HPD,
	// setting LINK_STATUS_UPDATED=1. Apple's checkLinkStatus then sees
	// INTERLANE_ALIGN_DONE=0 and tears down the display (~10s after boot).
	// The physical link is healthy; only the IRQ flag is spurious.
	// Clearing bit7 of DPCD[0x204] prevents the driver from acting on the IRQ.
	if (NGreen::callback && !NGreen::callback->isRealTGL && address == 0x0202 && buffer && length >= 3) {
		auto *raw = reinterpret_cast<uint8_t *>(buffer);
		if (raw[2] & 0x80) {
			static int v99Logs = 0;
			if (v99Logs < 10) {
				v99Logs++;
				SYSLOG("ngreen", "V99[%d]: suppressed DPCD 0x204 LINK_STATUS_UPDATED "
					   "(was 0x%02x, lanes=[0x%02x 0x%02x])",
					   v99Logs, raw[2], raw[0], raw[1]);
			}
			raw[2] &= ~0x80u; // clear LINK_STATUS_UPDATED
		}
	}

	if (address != 0x0000 && address != 0x2200) return retVal;

	if (length < sizeof(DPCDCap16) || buffer == nullptr)
		return retVal;

	auto caps = reinterpret_cast<DPCDCap16 *>(buffer);

	if (NGreen::callback && !NGreen::callback->isRealTGL) {
		// V98: Do NOT cap maxLaneCount or maxLinkRate.
		// Previous versions capped maxLaneCount to 2, which caused Apple to train at
		// 2 lanes while V97P subsequently wrote a 4-lane DDI_FUNC_CTL value → black screen.
		// The hardware is trained at HBR3×4 lanes by UEFI; let Apple see those real caps
		// so its LightUpEDP (re-)trains consistently at HBR3×4 lanes.
		static int v98Logs = 0;
		if (v98Logs < 5) {
			v98Logs++;
			SYSLOG("ngreen", "V98[%d]: DPCD caps @0x%04x maxLinkRate=0x%02x maxLane=0x%02x (passthrough)",
				   v98Logs, address, caps->maxLinkRate, caps->maxLaneCount);
		}
	}

	if (caps->revision < 0x03) {
		caps->maxLinkRate = 0;
	}

	return retVal;
}

void Gen11::getOnlineInfo(AppleIntel::AppleIntelFramebuffer *that, AppleIntel::AppleIntelDisplayPath *displayPath, unsigned char *online, unsigned char *changed) {
	// V96 removed (was: force *online=1 for fbId==0). Confirmed no-op on this hardware:
	// baseline log shows Apple's getOnlineInfo natively reports orig=1 for FB0, so the
	// V96 forcing was already redundant. Keeping the wrapper as a logging shell so we
	// can observe original online behavior across boots — if any orig!=1 case shows up
	// we'll know V96 was masking a real status bug, not just being redundant.
	FunctionCast(getOnlineInfo, callback->ogetOnlineInfo)(that, displayPath, online, changed);
	uint32_t fbId = getMember<uint32_t>(that, 0x1DC);
	unsigned char origOnline = online ? *online : 0xFF;
	static int v96PassLogs = 0;
	if (v96PassLogs < 12) {
		v96PassLogs++;
		SYSLOG("ngreen", "V96p: fb%u getOnlineInfo: orig=%d passthrough (V96 hack removed)",
			   fbId, origOnline);
	}
}

// Path B: hook AppleIntelFramebuffer::isApertureMemoryRequired().
// On real TGL: pass-through (preserve native behavior).
// On RPL/ADL with -ngreendp0: force return true so setupScanoutMemory never migrates
// from aperture → non-aperture. setupScanoutMemory's logic is:
//   if (isApertureMemoryRequired() && nonAperSurf!=0) → migrate-TO-aperture
//   else if (!isApertureMemoryRequired() && nonAperSurf==0) → migrate-FROM-aperture (the bad path)
//   else → no migration; if nonAperSurf==0 → "Using aperture memory"
// Forcing true with nonAperSurf==0 (the boot state) keeps us on the aperture path forever,
// matching what V99S+V99G already do at the hardware level — but cleanly, at the driver level,
// so WS sees a coherent fWSAAState→memory mapping and shouldn't degrade 0x3→0x1.
bool Gen11::wrapIsApertureMemoryRequired(AppleIntel::AppleIntelFramebuffer *that) {
	bool orig = FunctionCast(wrapIsApertureMemoryRequired, callback->oIsApertureMemoryRequired)(that);
	const bool isRealTGL = NGreen::callback && NGreen::callback->isRealTGL;

	// V205 freeze probe + continuous PSR1 disable. PSR1 at 0x60800 was found re-enabled
	// post-V105 (V105 was writing wrong register 0x64800). Even with the V105 fix, Apple
	// or DMC may re-arm PSR1 later — keep stomping it on every call.
	if (NGreen::callback) {
		static uint32_t v205Calls = 0;
		v205Calls++;

		// V99Z dirty-rect test removed: wipe ran but visually no change because WS
		// rewrites the whole buffer every frame. The "frozen images" symptom is
		// actually the X-tile-as-linear scanout artifact appearing more pronounced
		// on low-frequency content (text, solid blocks) than high-frequency content
		// (wallpaper texture). Single root cause = scanout-vs-buffer tile mismatch.
		// (Wipe proved BAR2 writes reach scanout in earlier magenta test, but WS's
		// per-frame rewriting makes the wipe invisible.)

		// Continuous PSR1+PSR2 disable — every call. PSR enabled = panel refreshes from
		// its own cache and ignores new SURF arms → frozen frame even while pipe vsyncs.
		uint32_t psr1Now = NGreen::callback->readReg32(0x60800);
		if (psr1Now & 1) {
			NGreen::callback->writeReg32(0x60800, 0);
			static uint32_t v205PSR1Resets = 0;
			if (v205PSR1Resets < 8) {
				v205PSR1Resets++;
				SYSLOG("ngreen", "V205PSR1[%u]: re-disabled PSR1 (was 0x%x) at call=%u",
					   v205PSR1Resets, psr1Now, v205Calls);
			}
		}
		uint32_t psr2Now = NGreen::callback->readReg32(0x60A10);
		if (psr2Now & 1) {
			NGreen::callback->writeReg32(0x60A10, 0);
			static uint32_t v205PSR2Resets = 0;
			if (v205PSR2Resets < 8) {
				v205PSR2Resets++;
				SYSLOG("ngreen", "V205PSR2[%u]: re-disabled PSR2 (was 0x%x) at call=%u",
					   v205PSR2Resets, psr2Now, v205Calls);
			}
		}

		// Periodic state snapshot for freeze diagnosis. Reduced thresholds since
		// wrapIsApertureMemoryRequired only fires ~44 times per boot in FB-only mode.
		const bool sample = (v205Calls == 1  || v205Calls == 2  || v205Calls == 5  ||
							 v205Calls == 10 || v205Calls == 20 || v205Calls == 30 ||
							 v205Calls == 40 || v205Calls == 44);
		if (sample) {
			uint32_t frm   = NGreen::callback->readReg32(0x70040);
			uint32_t pstat = NGreen::callback->readReg32(0x70024);
			uint32_t pcfg  = NGreen::callback->readReg32(0x70008);
			uint32_t dcst  = NGreen::callback->readReg32(0x45504);
			// V211: also probe Plane 2 (overlay) and Plane 3 (sprite) on Pipe A.
			// The visible "frozen overlay over animating background" symptom suggests
			// these planes hold stale content because WS's dp0 path only writes Plane 1.
			uint32_t p1ctl = NGreen::callback->readReg32(0x70180);
			uint32_t p1surf = NGreen::callback->readReg32(0x7019C);
			uint32_t p1liv = NGreen::callback->readReg32(0x701AC);
			uint32_t p2ctl = NGreen::callback->readReg32(0x71180);
			uint32_t p2surf = NGreen::callback->readReg32(0x7119C);
			uint32_t p2liv = NGreen::callback->readReg32(0x711AC);
			uint32_t p3ctl = NGreen::callback->readReg32(0x72180);
			uint32_t p3surf = NGreen::callback->readReg32(0x7219C);
			uint32_t p3liv = NGreen::callback->readReg32(0x721AC);
			uint32_t curctl = NGreen::callback->readReg32(0x70080);
			uint32_t curbase = NGreen::callback->readReg32(0x70084);
			uint32_t curpos = NGreen::callback->readReg32(0x70088);
			SYSLOG("ngreen", "V205[c=%u]: FRM=%u STAT=%08x CONF=%08x DC=%08x | P1 CTL=%08x SURF=%08x LIVE=%08x | P2 CTL=%08x SURF=%08x LIVE=%08x | P3 CTL=%08x SURF=%08x LIVE=%08x | CUR CTL=%08x BASE=%08x POS=%08x | PSR1=%08x PSR2=%08x",
				   v205Calls, frm, pstat, pcfg, dcst,
				   p1ctl, p1surf, p1liv,
				   p2ctl, p2surf, p2liv,
				   p3ctl, p3surf, p3liv,
				   curctl, curbase, curpos,
				   NGreen::callback->readReg32(0x60800), NGreen::callback->readReg32(0x60A10));
		}
	}

	if (isRealTGL || !isDisplayPipeForceDisabled()) {
		return orig;
	}
	static int logCount = 0;
	if (logCount < 8 && orig != true) {
		logCount++;
		SYSLOG("ngreen", "PathB: isApertureMemoryRequired forced true (orig=%d) fb=%p", orig, that);
	}
	return true;
}

// Path C: hook AppleIntelFramebuffer::setAttribute(IOSelect, uintptr_t).
// We intercept exactly one path: kIOWindowServerActiveAttribute ('wsrv' = 0x77737276).
// When WindowServer writes 0x1 (degrade-from-active), coerce the value to 0x3 before
// calling the original — keeping the driver's fWSAAState pinned at the active value so
// it never takes the hwDeferFeatures / degraded path.  Real TGL is unaffected.
IOReturn Gen11::wrapSetAttribute(void *that, uint32_t attr, uintptr_t value) {
	const bool isRealTGL = NGreen::callback && NGreen::callback->isRealTGL;
	if (attr == 0x77737276u /* 'wsrv' */ && !isRealTGL && isDisplayPipeForceDisabled()) {
		static int logCount = 0;
		uintptr_t newValue = value;
		bool coerced = false;
		if ((value & 0xFFu) == 0x1u) {
			newValue = (value & ~uintptr_t(0xFF)) | 0x3u;
			coerced = true;
		}
		if (logCount < 16) {
			logCount++;
			SYSLOG("ngreen", "PathC: wsrv setAttribute fb=%p value=0x%llx%s",
				   that, (unsigned long long)value, coerced ? " → coerced 0x3" : "");
		}
		return FunctionCast(wrapSetAttribute, callback->oSetAttribute)(that, attr, newValue);
	}
	return FunctionCast(wrapSetAttribute, callback->oSetAttribute)(that, attr, value);
}

// V183: ADL-P/RPL write-only power well handler.
// HSW_PWR_WELL_CTL1 (0x45400): REQ bits = odd bits (mask 0xAA: bits 1,3,5,7,...);
//                               STATE bits = even bits (mask 0x55: bits 0,2,4,6,...).
// TGL original polls STATE bits after writing REQ — on ADL-P those ACKs
// never arrive within the 20-iteration timeout, spinning the CPU.
// Fix: write REQ bits directly to CTL1, skip polling entirely.
// Previously this wrote to CTL2 (0x45404) — wrong register. CTL2 controls DDI/AUX
// wells; PG display wells (PG1/PG2) are in CTL1. Writing to CTL2 left CTL1 at the
// DMC-reset value (0x405, no REQ bits) → hardware never enabled display power domains
// → vsync interrupts never delivered → GPU ring idle → framebuffer stayed black.
void Gen11::hwSetPowerWellStatePGE(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2)
{
	if (!NGreen::callback->isRealTGL) {
		uint32_t ctl1 = NGreen::callback->readReg32(0x45400);
		uint32_t newVal;
		if (param_1) {
			// Enable: clear all REQ bits then set the requested ones.
			newVal = (ctl1 & 0xFFFFFF55U) | (param_2 & 0xAAU);
		} else {
			// Disable: clear the requested REQ bits.
			newVal = ctl1 & ~(param_2 & 0xAAU);
		}
		SYSLOG("ngreen", "V183.PGE: en=%u mask=0x%x ctl1: 0x%x->0x%x",
			   (unsigned)param_1, param_2, ctl1, newVal);
		NGreen::callback->writeReg32(0x45400, newVal);
		return;
	}
	// Real TGL: use original with ccont fixup.
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStatePGE, callback->ohwSetPowerWellStatePGE)(that, param_1, param_2);
}

void Gen11::hwSetPowerWellStateAux(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStateAux, callback->ohwSetPowerWellStateAux)(that,param_1,param_2);
}

void Gen11::hwSetPowerWellStateDDI(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStateDDI, callback->ohwSetPowerWellStateDDI)(that,param_1,param_2);
}

void Gen11::FastWriteRegister32(AppleIntel::AppleIntelBaseController *that, unsigned long param_1, uint32_t param_2)
{
	// V99D: Diagnose — log all FastWrite calls near display engine range on first boot
	// to understand what addresses/values flow through this path.
	{
		static int v99DCount = 0;
		if (v99DCount < 30) {
			v99DCount++;
			SYSLOG("ngreen", "V99D[%d]: FastWrite addr=0x%lx val=0x%x",
				   v99DCount, param_1, param_2);
		}
	}

	// V72F removed (was: force RCS/BCS RING_EMR FastWrite path to 0xFFFFFFFF — same
	// blanket-mask hack as V72R/V72W, on the FastWriteRegister32 entry). Last of the
	// "raWriteRegister32-side" EMR mask trio. The V74 50ms enforcer still re-forces
	// EMR via direct writeReg32 from a polling thread, so this passthrough alone is
	// not yet a full "no blanket EMR mask" test — V74's EMR portion is stripped
	// separately in v71EmrEnforcer.
	if (param_1 == 0x20b4 || param_1 == 0x220b4) {
		static int v72FPassCount = 0;
		if (v72FPassCount < 6) {
			++v72FPassCount;
			SYSLOG("ngreen", "V72Fp[%d]: EMR @ 0x%lx val=0x%x passthrough (V72F hack removed)",
				   v72FPassCount, param_1, param_2);
		}
	}

	// V99F[S] removed (was: PLANE_STRIDE *= 8 — same X-tile-units → 64B-cacheline-units
	// rewrite as V99R[S], on the FastWriteRegister32 entry). Pair-mate to V99R[Sp].
	// V99S downstream still re-forces STRIDE=0xa0 at SURF arm via direct MMIO, so
	// this passthrough does not affect what scans out.
	if ((param_1 & 0xFFFFF) == 0x70188) {
		static int v99FSpCount = 0;
		if (v99FSpCount < 3) {
			++v99FSpCount;
			SYSLOG("ngreen", "V99F[Sp%d]: PLANE_STRIDE 0x%x passthrough (V99F[S] hack removed)",
				   v99FSpCount, param_2);
		}
	}
	// V99F[C] removed (was: PLANE_CTL X-tiled (001) → Y-tiled-legacy (100) rewrite,
	// FastWriteRegister32 twin of V99R[C]). V99S at SURF arm still re-forces CTL
	// downstream, so this is a no-op for actually-displayed values.
	if ((param_1 & 0xFFFFF) == 0x70180) {
		static int v99FCpCount = 0;
		if (v99FCpCount < 3) {
			++v99FCpCount;
			uint32_t tiling = (param_2 >> 10) & 0x7;
			SYSLOG("ngreen", "V99F[Cp%d]: PLANE_CTL 0x%x passthrough tiling=%d (V99F[C] hack removed)",
				   v99FCpCount, param_2, tiling);
		}
	}
	// V103F removed (was: FastWriteRegister32 twin of V103 — block DC_STATE_EN
	// non-zero writes). Pair-mate to V103/V103P; all three sites now passthrough.
	if ((param_1 & 0xFFFFF) == 0x45504 && NGreen::callback && NGreen::callback->dmcIsAdlp) {
		static int v103FpCount = 0;
		if (v103FpCount < 6) {
			++v103FpCount;
			SYSLOG("ngreen", "V103Fp[%d]: DC_STATE_EN FastWrite 0x%x passthrough (V103F hack removed)",
				   v103FpCount, param_2);
		}
	}

	// V195F removed (FastWriteRegister32 site of V195 — same hack pile).
	if ((param_1 & 0xFFFFF) == 0x45400 && NGreen::callback && !NGreen::callback->isRealTGL
		&& NGreen::callback->uefiCtl1 != 0) {
		static int v195FpCount = 0;
		if (v195FpCount < 6) {
			++v195FpCount;
			SYSLOG("ngreen", "V195Fp[%d]: CTL1 FastWrite 0x%x passthrough (V195F hack removed)",
				   v195FpCount, param_2);
		}
	}

	// V99F[SURF] removed (was: at SURF arm via FastWriteRegister32, force PLANE_STRIDE=0xa0
	// — FastWrite-path twin of V99S non-dp0 STRIDE=0xa0 force in raWriteRegister32). The
	// raWriteRegister32 V99S still re-forces STRIDE at SURF arm via direct MMIO, so this
	// passthrough doesn't change what scans out.
	if ((param_1 & 0xFFFFF) == 0x7019C && NGreen::callback) {
		uint32_t hwStride = NGreen::callback->readReg32(0x70188);
		static int v99FSurfPassCount = 0;
		if (v99FSurfPassCount < 3) {
			++v99FSurfPassCount;
			SYSLOG("ngreen", "V99F[SURFp%d]: SURF arm 0x%x STRIDE=0x%x passthrough (V99F[SURF] hack removed)",
				   v99FSurfPassCount, param_2, hwStride);
		}
	}

	return FunctionCast(FastWriteRegister32, callback->oFastWriteRegister32)(that,param_1,param_2 );
}

void Gen11::raWriteRegister32b(void *that,void *param_1,unsigned long param_2, UInt32 param_3)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return;
	//if (reinterpret_cast<volatile uint64_t*>(param_1)==nullptr) return;
	raWriteRegister32(that, reinterpret_cast<uint64_t>(param_1) + param_2,param_3);
};

void Gen11::raWriteRegister32(void *that,unsigned long param_1, UInt32 param_2)
{
	auto *green = NGreen::callback;
	if (!callback || !green)
		return;
	// V93: optional plane SURF zero-write guard.
	// Disabled by default due black-screen regressions; enable only with -ngreenv93.
	struct V93PlaneSurfState {
		uint32_t surfReg;
		uint32_t lastNonZeroSurf;
	};
	static V93PlaneSurfState v93States[8] {};
	static int v93LogCount = 0;

	auto getV93State = [](uint32_t surfReg) -> V93PlaneSurfState * {
		for (auto &state : v93States) {
			if (state.surfReg == surfReg)
				return &state;
		}
		for (auto &state : v93States) {
			if (state.surfReg == 0) {
				state.surfReg = surfReg;
				state.lastNonZeroSurf = 0;
				return &state;
			}
		}
		return nullptr;
	};

	if (NGreen::callback && isV93PlaneGuardEnabled()) {
		const uint32_t reg = static_cast<uint32_t>(param_1 & 0xFFFFF);
		const bool looksLikePlaneSurf =
			(reg >= 0x60000 && reg <= 0xBFFFF) &&
			((reg & 0xFFF) == 0x19C);

		if (looksLikePlaneSurf) {
			auto *state = getV93State(reg);
			if (param_2 != 0) {
				if (state)
					state->lastNonZeroSurf = param_2;
			} else {
				const uint32_t ctlReg = reg - 0x1C; // PLANE_CTL is SURF - 0x1C on Gen11 paths.
				uint32_t planCtl = NGreen::callback->readReg32(ctlReg);
				if (planCtl & 0x80000000u) {
					if (state && state->lastNonZeroSurf != 0) {
						if (v93LogCount < 32) {
							SYSLOG("ngreen", "V93: blocked zero SURF@0x%x while enabled; keeping last 0x%x", reg, state->lastNonZeroSurf);
							v93LogCount++;
						}
						param_2 = state->lastNonZeroSurf;
					} else {
						uint32_t currentSurf = NGreen::callback->readReg32(reg);
						if (currentSurf != 0) {
							if (state)
								state->lastNonZeroSurf = currentSurf;
							if (v93LogCount < 32) {
								SYSLOG("ngreen", "V93: blocked zero SURF@0x%x while enabled; keeping current 0x%x", reg, currentSurf);
								v93LogCount++;
							}
							param_2 = currentSurf;
						} else {
							// Last resort: disable plane before allowing SURF=0 write.
							NGreen::callback->writeReg32(ctlReg, planCtl & ~0x80000000u);
							if (v93LogCount < 32) {
								SYSLOG("ngreen", "V93: forced plane disable for SURF@0x%x before zero write", reg);
								v93LogCount++;
							}
						}
					}
				}
			}
		}
	}

	// V72R removed (was: force RING_EMR (0x20b4 / 0x220b4) writes to 0xFFFFFFFF —
	// blanket-mask-all-GT-engine-errors). Hack that hides root cause; Linux i915
	// doesn't do blanket EMR masking. Remove and observe what real engine errors
	// emerge so the actual cause can be fixed.
	// NOTE: V72W (in wrapWriteRegister32 helper) and V72F (in FastWriteRegister32)
	// and V74 50ms permanent enforcer still mask RING_EMR. To fully test "no EMR
	// blanket mask" they need to be disabled in subsequent steps.

	// V99R[S] removed (was: PLANE_STRIDE *= 8 — convert X-tiled tile-units 0x14 to
	// Y-tiled/linear cacheline-units 0xa0). Hack guessing the right scanout stride
	// without knowing the actual buffer layout. Linux i915 picks stride based on
	// the IOSurface/buffer's documented tiling, not by rewriting Apple's value.
	// Removing this lets Apple's natural PLANE_STRIDE (0x14) reach hardware. If
	// scanout shows X-tiled bytes correctly → buffer was X-tiled. If garbled →
	// real layer is buffer/renderer side, not register-write.
	// NOTE: V99S (SURF arm) and V99f (fast-write variant) and V99F (FastWrite) still
	// have their own PLANE_STRIDE rewrites — to fully test, those need removal too.
	// For now keep the address-match shell so future legitimate intercepts can use it.
	if ((param_1 & 0xFFFFF) == 0x70188) { // PLANE_STRIDE Pipe A Plane 1
		static int v99SPassCount = 0;
		if (v99SPassCount < 3) {
			++v99SPassCount;
			SYSLOG("ngreen", "V99R[Sp%d]: PLANE_STRIDE 0x%x passthrough (V99R hack removed)", v99SPassCount, param_2);
		}
	}
	// V99R[C] / V99R[Cdp] removed (was: PLANE_CTL tiling rewrites — X-tiled (001) →
	// Y-tiled legacy (100) on default path, force linear under -ngreendp0). Same
	// "guess the tiling" antipattern as V99R[S]; pairs with the stride-rewrite hack
	// just removed. Linux i915 picks PLANE_CTL tiling bits from the IOSurface/buffer's
	// declared tiling, never rewrites Apple's value. With V99R[S] passthrough, leaving
	// the corresponding CTL field rewritten is contradictory: STRIDE is now 0x14
	// (X-tiled tile-units) but CTL would still be forced to Y-tiled or linear, which
	// is guaranteed-wrong scanout. Removing both lets Apple's natural CTL reach HW so
	// we can observe what tiling Apple's allocator actually chose.
	if ((param_1 & 0xFFFFF) == 0x70180) { // PLANE_CTL Pipe A Plane 1
		static int v99CPassCount = 0;
		if (v99CPassCount < 3) {
			++v99CPassCount;
			uint32_t tiling = (param_2 >> 10) & 0x7;
			SYSLOG("ngreen", "V99R[Cp%d]: PLANE_CTL 0x%x passthrough tiling=%d (V99R hack removed)",
				   v99CPassCount, param_2, tiling);
		}
	}

	// V97 removed (was: clear bit[16] PORT_SYNC_MODE_MASTER_SELECT[0] from
	// TRANS_DDI_FUNC_CTL_A on !isRealTGL — Apple's SetupParams sets it but UEFI GOP
	// doesn't, allegedly disrupting trained eDP link → "InterLane Alignment is lost"
	// ~10s later). Symptom-hiding hack: real fix should match Linux i915's Display 13
	// transcoder programming, not strip a bit Apple's stack expects to set. Removing
	// reveals whether the InterLane loss still happens; if yes, look at Linux's
	// TRANS_DDI_FUNC_CTL field layout for ADL-P (Display 13) since PORT_SYNC fields
	// changed across display generations.
	if ((param_1 & 0xFFFFF) == 0x60400 && NGreen::callback && !NGreen::callback->isRealTGL) {
		static int v97RpCount = 0;
		if (v97RpCount < 6) {
			++v97RpCount;
			SYSLOG("ngreen", "V97Rp[%d]: TRANS_DDI_FUNC_CTL_A 0x%x passthrough bit16=%d (V97 hack removed)",
				   v97RpCount, param_2, !!(param_2 & (1u << 16)));
		}
	}

	// V103 removed (was: block all non-zero DC_STATE_EN writes when ADL-P DMC is
	// loaded — keeps the DMC from entering DC3/DC5/DC6 because Apple's ICL-targeted
	// driver has no ADL-P DC exit recovery, all eDP lanes drop ~70s after a write).
	// Symptom-hiding hack: Linux i915 has proper DC enter/exit for Display 13 via
	// the DMC. Permanently disabling DC states masks Apple's broken DC exit instead
	// of providing one. Removing reveals when DC_STATE_EN gets written and what
	// value Apple wants set; the fix is to ensure DC exit is properly handled (or
	// to align with what Linux ADL-P DMC expects).
	if ((param_1 & 0xFFFFF) == 0x45504 && NGreen::callback && NGreen::callback->dmcIsAdlp) {
		static int v103PassCount = 0;
		if (v103PassCount < 10) {
			++v103PassCount;
			SYSLOG("ngreen", "V103p[%d]: DC_STATE_EN 0x%x passthrough (V103 hack removed)",
				   v103PassCount, param_2);
		}
	}

	// V195 removed (was: same OR-in bits 14,12 hack as V195W, on raWriteRegister32).
	if ((param_1 & 0xFFFFF) == 0x45400 && NGreen::callback && !NGreen::callback->isRealTGL
		&& NGreen::callback->uefiCtl1 != 0) {
		static int v195pCount = 0;
		if (v195pCount < 6) {
			++v195pCount;
			SYSLOG("ngreen", "V195p[%d]: CTL1 ra 0x%x passthrough (V195 hack removed)",
				   v195pCount, param_2);
		}
	}

	// V99S: PLANE_SURF arm — force correct STRIDE/CTL immediately before latching.
	// raWriteRegister32/WriteRegister32 is a CACHE-ONLY update; hardware MMIO for
	// double-buffered PLANE_STRIDE is written via a volatile* path not caught by
	// FastWriteRegister32. Forcing writeReg32 here ensures the hardware shadow is
	// correct right before SURF arms the double-buffer flip.
	//
	// dp0 path: redirect non-aperture SURF to 0x0 AND remap GGTT[0..] to the same
	// physical pages (V99G).  setupScanoutMemory migrates SURF from aperture to
	// ≥0x10000000 (non-aperture stolen RAM, phys ~0x7f…) when WindowServer sets
	// kIOWindowServerActiveAttribute=3. After migration WS writes to the non-aperture
	// physical pages; PLANE_SURF must also scan them. V99G copies the GGTT PTEs from the
	// non-aperture range down to GGTT[0..3999] so SURF=0x0 scans the same pages.
	if ((param_1 & 0xFFFFF) == 0x7019C && NGreen::callback) {
		uint32_t hwStride = NGreen::callback->readReg32(0x70188);
		uint32_t hwCtl    = NGreen::callback->readReg32(0x70180);
		static int v99SCount = 0;
		if (v99SCount < 8) {
			++v99SCount;
			// V201B: read TOP-LEFT (gray border) AND CENTER (Apple logo / loading bar
			// area) of the buffer. Linear byte offset of pixel (x,y) = y*10240 + x*4.
			// Sample points (BGRA, 2560×1600):
			//  - (0,0)         offset 0          → top-left, gray bg
			//  - (1280,800)    offset 0x7D2800   → screen center, Apple logo
			//  - (1280,1000)   offset 0x9C7800   → loading-bar row
			//  - (640,800)     offset 0x7D1A00   → mid-left of logo area
			volatile uint32_t *aperture = nullptr;
			uint64_t apertureLen = 0;
			uint32_t tlCtr = 0xDEADBEEF, ctr = 0xDEADBEEF, bar = 0xDEADBEEF, mid = 0xDEADBEEF;
			if (NGreen::callback->getAperture(aperture, apertureLen) && apertureLen >= 0xA00000) {
				volatile uint32_t *fb32 = aperture;
				tlCtr = fb32[0];             // top-left
				ctr   = fb32[0x7D2800 / 4];  // center (Apple logo)
				bar   = fb32[0x9C7800 / 4];  // loading bar row
				mid   = fb32[0x7D1A00 / 4];  // mid-left of logo area
			}
			SYSLOG("ngreen", "V99S[%d]: SURF arm 0x%x STRIDE=0x%x CTL=0x%x | tl=%08x ctr(1280,800)=%08x bar(1280,1000)=%08x mid(640,800)=%08x",
				   v99SCount, (uint32_t)param_2, hwStride, hwCtl, tlCtr, ctr, bar, mid);

			// V203: on the LAST sampled SURF arm, dump every pipe/transcoder/DSC/scaler
			// register relevant to the duplicated-content symptom. We're hunting an
			// off-by-2 in stride / src-vs-active / DSC bpp / pipe-bpc / M-N.
			if (v99SCount == 8) {
				#define R(addr) NGreen::callback->readReg32(addr)
				SYSLOG("ngreen", "V203: --- SCANOUT REGISTER DUMP ---");
				// Plane A
				SYSLOG("ngreen", "V203 PLANE_A: CTL=%08x STRIDE=%08x POS=%08x SIZE=%08x OFFSET=%08x SURF=%08x SURFLIVE=%08x AUX_DIST=%08x AUX_OFFSET=%08x KEYVAL=%08x KEYMSK=%08x KEYMAX=%08x COLOR_CTL=%08x",
					   R(0x70180), R(0x70188), R(0x7018C), R(0x70190), R(0x701A4),
					   R(0x7019C), R(0x701AC), R(0x701C0), R(0x701C4),
					   R(0x70194), R(0x70198), R(0x701A0), R(0x701CC));
				// Pipe A general
				SYSLOG("ngreen", "V203 PIPE_A: SRCSZ=%08x CONF=%08x MISC=%08x MISC2=%08x STAT=%08x",
					   R(0x6001C), R(0x70008), R(0x70030), R(0x7002C), R(0x70024));
				// Transcoder A timings
				SYSLOG("ngreen", "V203 TRANS_A: HTOTAL=%08x HBLANK=%08x HSYNC=%08x VTOTAL=%08x VBLANK=%08x VSYNC=%08x VSYNCSHIFT=%08x",
					   R(0x60000), R(0x60004), R(0x60008), R(0x6000C),
					   R(0x60010), R(0x60014), R(0x60028));
				// DDI function control + MSA
				SYSLOG("ngreen", "V203 TRANS_A_DDI: DDI_FUNC_CTL=%08x DDI_FUNC_CTL2=%08x MSA_MISC=%08x CONF=%08x CLK_SEL=%08x",
					   R(0x60400), R(0x60404), R(0x60410), R(0x70008), R(0x46140));
				// DP M/N values for Pipe A
				SYSLOG("ngreen", "V203 TRANS_A_DPMN: DATAM1=%08x DATAN1=%08x DATAM2=%08x DATAN2=%08x LINKM1=%08x LINKN1=%08x LINKM2=%08x LINKN2=%08x",
					   R(0x60030), R(0x60034), R(0x60038), R(0x6003C),
					   R(0x60040), R(0x60044), R(0x60048), R(0x6004C));
				// Pipe A scaler 1
				SYSLOG("ngreen", "V203 PIPE_A_PS1: CTRL=%08x WIN_POS=%08x WIN_SZ=%08x VPHASE=%08x HPHASE=%08x",
					   R(0x68180), R(0x68170), R(0x68174), R(0x68188), R(0x68194));
				// DSC slice control / PPS for Pipe A (DSC_BASE_A around 0x6B200; varies by gen)
				SYSLOG("ngreen", "V203 DSC_A: PIC_RC=%08x PPS0=%08x PPS1=%08x PPS2=%08x PPS3=%08x PPS4=%08x",
					   R(0x6B200), R(0x6B210), R(0x6B214), R(0x6B218), R(0x6B21C), R(0x6B220));
				// DDI buf / port
				SYSLOG("ngreen", "V203 DDI_BUF: A_CTL=%08x B_CTL=%08x | DP_TP_CTL_A=%08x DP_TP_STATUS_A=%08x",
					   R(0x64000), R(0x64100), R(0x64040), R(0x64044));
				// Display Buffer programming
				SYSLOG("ngreen", "V203 DBUF: CTL_S0=%08x CTL_S1=%08x DBUF_BUF_CFG_A_PA=%08x A_PB=%08x",
					   R(0x44300), R(0x44304), R(0x70B80), R(0x70B84));
				#undef R
			}
		}
		/*.  ------ MAN IN THE MIDDLE CHIP HACK -------
		// V99R[P] + V99G + linear CTL/STRIDE forces — CORE scanout coherence (!isRealTGL).
		// Confirmed load-bearing for visible scanout on spoofed RPL/ADL-P in dp0, dp1, AND
		// without any -ngreendp* boot arg. Real TGL hardware programs these correctly via
		// Apple's native code path — gating the entire triad on !isRealTGL.
		//
		// Friend's architectural feedback (Visual Ehrmanntraut, NootedBlue lineage):
		// the right long-term fix is intercepting CRTCParams / PLANEPARAMS / SCALERPARAMS
		// at hwSetupMemory / paramsSurfCompare / hwRegsNeedUpdate (already partial via
		// V97P / V97C) instead of hooking MMIO writes after-the-fact. "Hacking regs only
		// won't work — leave WS alone, problem is in apple code."
		//
		//   1. SURF redirect: non-aperture writes (>=0x10000000) → 0, so the display engine
		//      always scans from the same GGTT page range (GGTT[0..3999]) no matter where
		//      Apple's setupScanoutMemory chose to migrate the surface.
		//   2. V99G: per-flip GGTT remap — copies the PTEs at the migrated surface pages
		//      down to GGTT[0..3999] so SURF=0 fetches the same physical memory WS's CPU
		//      compositor is writing into. Re-runs whenever Apple's SURF address changes
		//      (handles double/triple buffering — WS rotates between 2-3 IOSurfaces per flip).
		//   3. Linear CTL/STRIDE forces (tiling→0, STRIDE=0xa0): required for visible
		//      output. Removing them produces a black screen with no scanout activity,
		//      confirmed empirically. Side effect: produces the fragmented/repeated
		//      pattern when Apple's allocator stores buffer in non-linear physical layout
		//      — known cost of this path, removable only by struct-level fix.
		if (NGreen::callback && !NGreen::callback->isRealTGL && param_2 >= 0x10000000u) {
			static int v99PCount = 0;
			if (v99PCount < 8)
				SYSLOG("ngreen", "V99R[P%d]: SURF 0x%x->aperture (non-aperture redirect)",
					   ++v99PCount, (uint32_t)param_2);

			// ngreen-buf=N: 1=single, 2=double (default), 3=triple buffering.
			// Each buffer slot occupies 4000 GGTT pages = 0xFA0000 bytes of aperture:
			//   slot 0 → GGTT[0..3999],      SURF=0x0
			//   slot 1 → GGTT[4000..7999],   SURF=0xFA0000
			//   slot 2 → GGTT[8000..11999],  SURF=0x1F40000
			// Each unique non-aperture srcPage Apple presents is assigned a fixed slot.
			// The GGTT PTEs for that slot are remapped to point at the IOSurface's
			// physical pages. SURF is rewritten to the matching aperture slot address
			// so the display engine scans the correct physical memory.
			// With single buffering all flips always land on slot 0 (SURF=0x0).
			static int  bufCount       = -1;
			static uint32_t slotPages[3] = {0, 0, 0}; // srcPage assigned to each slot
			static int  slotCount      = 0;
			static int  v99GCount      = 0;

			if (bufCount < 0) {
				int val = 2;
				PE_parse_boot_argn("ngreen-buf", &val, sizeof(val));
				bufCount = (val >= 1 && val <= 3) ? val : 2;
				SYSLOG("ngreen", "V99G: ngreen-buf=%d (buffering slots)", bufCount);
			}

			uint32_t srcPage = (uint32_t)param_2 >> 12;

			// Find existing slot or assign a new one.
			int slot = -1;
			for (int s = 0; s < slotCount; s++) {
				if (slotPages[s] == srcPage) { slot = s; break; }
			}
			if (slot < 0) {
				if (slotCount < bufCount) {
					slot = slotCount++;
				} else {
					// All slots occupied — evict oldest (slot 0), shift down.
					for (int s = 0; s < bufCount - 1; s++) slotPages[s] = slotPages[s + 1];
					slot = bufCount - 1;
				}
				slotPages[slot] = srcPage;
			}

			// Remap GGTT[slot*4000 .. slot*4000+3999] from srcPage..srcPage+3999.
			{
				int base = slot * 4000;
				int remapped = 0, remapSkipped = 0;
				for (int i = 0; i < 4000; i++) {
					uint32_t lo = NGreen::callback->readReg32(GGTT_PTE_LO(srcPage + i));
					uint32_t hi = NGreen::callback->readReg32(GGTT_PTE_HI(srcPage + i));
					if (!(lo & 1)) { remapSkipped++; continue; }
					NGreen::callback->writeReg32(GGTT_PTE_LO(base + i), lo);
					NGreen::callback->writeReg32(GGTT_PTE_HI(base + i), hi);
					remapped++;
				}
				NGreen::callback->writeReg32(0x101008, 0x1); // flush GGTT TLB
				if (++v99GCount <= 8 || (v99GCount & 0x3F) == 0)
					SYSLOG("ngreen", "V99G[%d]: GGTT[%d..%d] <- srcPage=0x%x slot=%d remapped=%d skip=%d",
						   v99GCount, base, base + 3999, srcPage, slot, remapped, remapSkipped);
			}

			// Redirect SURF to the aperture address of the assigned slot.
			// slot 0 → 0x0, slot 1 → 0xFA0000, slot 2 → 0x1F40000.
			param_2 = (uint32_t)slot * 0xFA0000u;
		}
		// CTL/STRIDE forces — MATCH APPLE'S NATURAL INTENT.
		// V401 paramsSurfCompare logs prove Apple wants: CTL bits[12:10]=001 (X-tiled),
		// STRIDE=0x14 (20 X-tile units = 10240B/row = 2560*4bpp). Apple's IOSurface
		// allocator produces X-tiled physical buffers — Y-tile and linear forces both
		// scan wrong bytes from an X-tile buffer. Match Apple = same tile mode as the
		// buffer = correct scanout, IF the SURF address reaches the right pages
		// (V99R[P]+V99G handle the SURF redirect / GGTT remap unconditionally).
		//
		// Gated on !isRealTGL. Real TGL programs natively.
		if (NGreen::callback && !NGreen::callback->isRealTGL) {
			// force CTL linear and STRIDE=0xa0 (CPU compositor writes linearly via BAR2).
			uint32_t hwTiling = (hwCtl >> 10) & 0x7;
			if (hwTiling != 0)
				NGreen::callback->writeReg32(0x70180, hwCtl & ~(0x7u << 10));
			if (hwStride != 0xa0)
				NGreen::callback->writeReg32(0x70188, 0xa0);
		}*/
	}

	if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return green->writeReg32(param_1,param_2);
	if (!callback->oraWriteRegister32) return green->writeReg32(param_1,param_2);
	FunctionCast(raWriteRegister32, callback->oraWriteRegister32)( that,param_1,param_2);
};

uint32_t Gen11::AppleIntelFramebufferinit(AppleIntel::AppleIntelFramebuffer *frame,
                                          AppleIntel::AppleIntelBaseController *cont,
                                          uint32_t pipeIndex)
{
	if (cont) {
		callback->framecont = cont;
		ccont2 = cont;
		auto *accessor = getMember<void *>(cont, 0xC40);
		if (accessor)
			ccont = accessor;
	}
	// Offsets into the full IOFramebuffer subclass hierarchy (much larger than the
	// tail fields captured in AppleIntelParams::AppleIntelFramebuffer).
	if (ccont) {
		getMember<void *>(frame, 0x4a40) = ccont;
		getMember<void *>(frame, 0xc40) = ccont;
	}
	auto ret = FunctionCast(AppleIntelFramebufferinit, callback->oAppleIntelFramebufferinit)(frame, cont, pipeIndex);
	if (ccont) {
		getMember<void *>(frame, 0x4a40) = ccont;
		getMember<void *>(frame, 0xc40) = ccont;
	}
	return ret;
}



void Gen11::initPlatformWorkarounds(AppleIntel::AppleIntelBaseController *that)
{
	// Platform workaround flags for ADL-P (RPL-P) running under TGL driver.
	// flags_ig (+0xC58): boot info flags — checked by PowerWell::init to set fAlwaysOn.
	//   Must be set BEFORE PowerWell::init runs if we want Apple's native fAlwaysOn path.
	//   We also force fAlwaysOn=1 in our PowerWell::init hook as belt-and-suspenders.
	// fInfoFlags2 (+0xC5C): display feature flags.
	//   ADL-P uses PCH PWM for backlight (cnp_setup_backlight confirmed in Linux syslog).
	//   Do NOT set FB_FLAG_ENABLE_BACKLIGHT_REG_CONTROL (that forces CPU-register backlight).
	that->flags_ig    = FB_FLAG_BOOST_PIXEL_FREQUENCY_LIMIT;
	that->fInfoFlags2 =
		FB_FLAG_ALTERNATE_PWM_INCREMENT1 |
		FB_FLAG_ALTERNATE_PWM_INCREMENT2 |
		FB_FLAG_ENABLE_SLICE_FEATURES    |
		FB_FLAG_FORCE_POWER_ALWAYS_CONNECTED |
		FB_FLAG_AVOID_FAST_LINK_TRAINING;

	FunctionCast(initPlatformWorkarounds, callback->oinitPlatformWorkarounds)(that);

	// V212: ADL-P (Display 13) specific display workarounds, ported from Linux i915.
	// Apple's TGL kext targets Display 12 and doesn't apply these chicken bits / clock-
	// gating / error masks on the spoofed setup. Linux marks them as REQUIRED for
	// Display 13+; their absence can manifest as display engine misbehavior
	// (timing/underrun/error-recovery loops). Gated by !isRealTGL so genuine TGL HW
	// (Display 12) is unaffected.
	if (NGreen::callback && !NGreen::callback->isRealTGL) {
		// Wa_22011091694:adlp — DPCE_GATING_DIS = REG_BIT(17) in GEN9_CLKGATE_DIS_5 (0x46540)
		NGreen::callback->intel_de_rmw(0x46540, 0, 1u << 17);

		// Bspec/49189 ADL-P init — CLEAR DDI_CLOCK_REG_ACCESS = REG_BIT(7) in GEN8_CHICKEN_DCPR_1 (0x46430)
		NGreen::callback->intel_de_rmw(0x46430, 1u << 7, 0);

		// PIPE_CHICKEN Pipe A (0x70038):
		//   bit 30 = UNDERRUN_RECOVERY_DISABLE_ADLP — required on Display 13+
		//   bit 7  = PER_PIXEL_ALPHA_BYPASS_EN     — Display WA #1153
		//   bit 15 = PIXEL_ROUNDING_TRUNC_FB_PASSTHRU — Display WA #1605353570
		NGreen::callback->intel_de_rmw(0x70038, 0, (1u << 30) | (1u << 15) | (1u << 7));

		// XELPD_DISPLAY_ERR_FATAL_MASK (0x4421C) ← mask all fatal display errors on
		// Display 13 (per icl_display_core_init in Linux i915). Without this, fatal
		// error events can trigger pipeline restart loops.
		NGreen::callback->writeReg32(0x4421C, 0xFFFFFFFFu);

		uint32_t pipeChicken    = NGreen::callback->readReg32(0x70038);
		uint32_t clkGateDis5    = NGreen::callback->readReg32(0x46540);
		uint32_t chickenDcpr1   = NGreen::callback->readReg32(0x46430);
		uint32_t errFatalMask   = NGreen::callback->readReg32(0x4421C);
		SYSLOG("ngreen", "V212: ADL-P Display 13+ workarounds applied — PIPE_CHICKEN(A)=0x%x CLKGATE_DIS_5=0x%x CHICKEN_DCPR_1=0x%x ERR_FATAL_MASK=0x%x",
			   pipeChicken, clkGateDis5, chickenDcpr1, errFatalMask);
	}
}

uint64_t Gen11::getOSInformation(AppleIntel::AppleIntelBaseController *that)
{
	auto *pinfo = reinterpret_cast<PlatformInfo *>(callback->gPlatformInformationList);
	if (pinfo) {
		// Index 1 = the mobile TGL/ADL-P platform entry (0x9A490000 and variants).
		pinfo[1].fInfoFlags =
			FB_FLAG_DISABLE_PIPE_SCRAMBLE      |
			FB_FLAG_FRAMEBUFFER_COMPRESSION    |
			FB_FLAG_ALLOW_CONNECTOR_RECOVER    |
			FB_FLAG_FORCE_POWER_ALWAYS_CONNECTED |
			FB_FLAG_AVOID_FAST_LINK_TRAINING;

		// cameliav=2 (CamelliaTcon2) requires GFX kext present — gate on -ngreentglwithgfx.
		pinfo[1].cameliav = checkKernelArgument("-ngreentglwithgfx") ? 2 : 0;
		pinfo[1].fMobile  = 1;
		// 3/3/3 baseline restored. Multi-pipe reduction is whack-a-mole — every count
		// reduction reveals new cross-pipe NULL-deref sites in TGL FB internals
		// (enableController +0x1356, getFreeJoinablePathCount +0xa7, etc).
		// Best known config: 3/3/3 + -ngreendp0 → reaches login with banded display.
		pinfo[1].fPipeCount            = 3;
		pinfo[1].fInfoPortCount        = 3;
		pinfo[1].fInfoFramebufferCount = 3;
		pinfo[1].fSliceCount  = 1;
		pinfo[1].fmaxEuCount  = 8;
		pinfo[1].fsubslices   = 10;

		// Connector 0: built-in eDP (LVDS), DDI-A, pipe 0
		pinfo[1].connectors[0].index = 0;
		pinfo[1].connectors[0].busId = 0;
		pinfo[1].connectors[0].pipe  = 0;
		pinfo[1].connectors[0].pad   = 0;
		pinfo[1].connectors[0].type  = ConnectorLVDS;
		pinfo[1].connectors[0].flags = 0x8 | 0x10;

		// Connector 1: external USB-C/Thunderbolt DP (TC1/DDI-D), pipe 2
		pinfo[1].connectors[1].index = 1;
		pinfo[1].connectors[1].busId = 1;
		pinfo[1].connectors[1].pipe  = 2;
		pinfo[1].connectors[1].pad   = 0;
		pinfo[1].connectors[1].type  = ConnectorDP;
		pinfo[1].connectors[1].flags = 0x1 | 0x400;

		// Connectors 2-3: Dummy
		pinfo[1].connectors[2] = { 2, 2, 2, 0, ConnectorDummy, 0 };
		pinfo[1].connectors[3] = { 3, 3, 3, 0, ConnectorDummy, 0 };

		SYSLOG("ngreen", "getOSInformation: patched pinfo[1] for ADL-P (LVDS+HDMI, mobile)");
	}
	return FunctionCast(getOSInformation, callback->ogetOSInformation)(that);
}

void Gen11::hwInitializeCState(AppleIntel::AppleIntelBaseController *that)
{
	if (!that || !ngPhysicalGpuAccessAllowed()) {
		// Display DMC and physical power wells are not owned by a VF. The
		// original fallback also performs physical initialization: do not call
		// it merely because a DMC boot argument was absent.
		vfMarkProtocolFault("physical DMC initialization on VF/unknown device or null controller");
		return;
	}
	SYSLOG("ngreen", "NB-BUILD-V50-ALLOW-METAL");

	int origB48 = getMember<int>(that, 0xB48);
	int origCE4 = getMember<int>(that, 0xCE4);
	SYSLOG("ngreen", "hwInitCState B48=%d CE4=%d", origB48, origCE4);

	// Boot-arg "ngreen-dmc":
	//   not set or "skip" → safe fallback: passthrough original + AUX only (proven working)
	//   "tgl"             → load TGL DMC v2.12 blob + TGL display engine registers
	//                       + ICL/TGL combo PHY signal levels (PHY_A eDP, PHY_B DP)
	//   "adlp"            → load ADL-P DMC v2.16 blob + ADL-P display engine registers
	//                       + combo PHY signal levels (PHY_A eDP)
	//   "icl"             → passthrough original ICL DMC load + ICL combo PHY signal levels
	char dmcArg[16] = {};
	PE_parse_boot_argn("ngreen-dmc", dmcArg, sizeof(dmcArg));

	if (dmcArg[0] == 't' || dmcArg[0] == 'T') {
		// ── TGL DMC ──
		SYSLOG("ngreen", "hwInitCState: ngreen-dmc=tgl, loading TGL DMC v2.12 (%u dwords)", tgl_dmc_ver2_12_bin_s / 4);
		// Write TGL DMC blob to MMIO 0x80000+
		for (unsigned long off = 0; off < tgl_dmc_ver2_12_bin_s; off += 4)
			FastWriteRegister32(reinterpret_cast<AppleIntel::AppleIntelBaseController *>(ccont), off + 0x80000,
				tgl_dmc_ver2_12_bin[off / sizeof(uint32_t)]);

		// Disable DC states before touching display engine registers (same as ADL-P path).
		// DC_STATE_EN = 0x45504
		NGreen::callback->writeReg32(DC_STATE_EN, 0);

		// Use UEFI CTL1 as base (same pattern as ADL-P path).
		uint32_t tglUefiCtl1 = NGreen::callback->readReg32(0x45400);
		{
			uint32_t newCtl1 = tglUefiCtl1 | 0x00000401u;
			NGreen::callback->writeReg32(0x45400, newCtl1); // HSW_PWR_WELL_CTL1
			NGreen::callback->writeReg32(0x45404, 0x00000C03); // HSW_PWR_WELL_CTL2
			SYSLOG("ngreen", "V101T: PWR_WELL CTL1 uefi=0x%x->0x%x CTL2=0xc03", tglUefiCtl1, newCtl1);
		}
		NGreen::callback->writeReg32(0x45408, 0x40000000); // HSW_PWR_WELL_CTL3
		NGreen::callback->writeReg32(0x4540C, 0x00000401); // HSW_PWR_WELL_CTL4
		NGreen::callback->writeReg32(0x45440, 0x00000003); // ICL_PWR_WELL_CTL_AUX1 — AUX A
		NGreen::callback->writeReg32(0x45444, 0x00000003); // ICL_PWR_WELL_CTL_AUX2 — AUX B
		NGreen::callback->writeReg32(0x45450, 0x00000003); // ICL_PWR_WELL_CTL_DDI1 — DDI A
		NGreen::callback->writeReg32(0x45454, 0x00000003); // ICL_PWR_WELL_CTL_DDI2 — DDI B

		// TGL display engine registers (DMC trigger/context regs in 0x8Fxxx range)
		// Values from IDA of original hwInitializeCState (TGL-native values)
		NGreen::callback->writeReg32(0x8F074, 0x00006FC0);
		NGreen::callback->writeReg32(0x8F004, 0x00A40088);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);

		// Enable DMC — DC_STATE_DEBUG (0x45520) = 2
		NGreen::callback->writeReg32(0x45520, 2); // DC_STATE_DEBUG
		NGreen::callback->dmcIsAdlp = true;        // reuse flag: also protects TGL path via V103/V104P
		NGreen::callback->uefiCtl1  = tglUefiCtl1;
		SYSLOG("ngreen", "hwInitCState: TGL DMC loaded");
		// setSignalLevels block REMOVED for the same reason as the ADL-P branch:
		// Linux i915 (per /Volumes/EFI/syslog.txt drm trace) only calls setSignalLevels
		// during DP link training, where swing=0/0/0/0 is the negotiation STARTING point
		// before DPRX adjust-request bumps it to vswing=1/1/1/1. Calling it once at
		// hwInitCState with all-zero levels freezes the combo PHY at the lowest drive
		// strength forever → DP receivers read garbage bitRates, link silently fails.
		// {
		// 	uint8_t swing[4]   = {0, 0, 0, 0};
		// 	uint8_t preEmph[4] = {0, 0, 0, 0};
		// 	IntelDPLinkTraining::setSignalLevels(/*phy=*/0, /*lanes=*/4, /*isHBR2=*/false, /*isDP=*/true, swing, preEmph);
		// 	IntelDPLinkTraining::setSignalLevels(/*phy=*/1, /*lanes=*/4, /*isHBR2=*/false, /*isDP=*/true, swing, preEmph);
		// }
		// Let original run with B48=1 (ICL CSR blob loads to SRAM).
		// Same rationale as ADL-P: the TGL DMC firmware also causes lane drops because its
		// DC state management runs on ADL-P hardware; the ICL DMC is safer on this silicon.
		// TGL context regs (8Fxxx) are re-applied after so the display engine sees TGL values.
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
		// Re-apply TGL context regs overwritten by original's ICL blob load.
		NGreen::callback->writeReg32(0x8F074, 0x00006FC0);
		NGreen::callback->writeReg32(0x8F004, 0x00A40088);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);
		SYSLOG("ngreen", "V104T: TGL context regs re-applied after ICL blob load");
		// V102T: restore CTL1 after original (ICL DMC save/restore table may write 0x401).
		{
			uint32_t postCtl1 = NGreen::callback->readReg32(0x45400);
			uint32_t fixCtl1  = tglUefiCtl1 | 0x00000401u;
			if (postCtl1 != fixCtl1) {
				NGreen::callback->writeReg32(0x45400, fixCtl1);
				SYSLOG("ngreen", "V102T: restore PWR_WELL CTL1 0x%x->0x%x", postCtl1, fixCtl1);
			}
		}

	} else if (dmcArg[0] == 'a' || dmcArg[0] == 'A') {
		// ── ADL-P DMC ──
		SYSLOG("ngreen", "hwInitCState: ngreen-dmc=adlp, loading ADL-P DMC v2.16 (%u dwords)", adlp_dmc_ver2_16_bin_s / 4);
		// adlp_dmc_ver2_16_bin is the raw firmware payload extracted from the v3 blob
		// (adlp_dmc_ver2_16.bin: CSS+package/v3 headers stripped, main payload at file offset 0x310).
		// Write directly to SRAM starting at 0x80000.
		for (unsigned long off = 0; off < adlp_dmc_ver2_16_bin_s; off += 4)
			FastWriteRegister32(reinterpret_cast<AppleIntel::AppleIntelBaseController *>(ccont), off + 0x80000,
				adlp_dmc_ver2_16_bin[off / sizeof(uint32_t)]);

		// Disable DC states before touching display engine registers.
		// If DC5/DC6 is active when we write, the clock-gated blocks won't latch the writes.
		// DC_STATE_EN = 0x45504 (confirmed: Archive HIGH, linux display/intel_display_regs.h)
		NGreen::callback->writeReg32(DC_STATE_EN, 0);

		// Power wells — Gen12 ICL-style DDI + AUX power well enable.
		// HSW_PWR_WELL_CTL1/2 (0x45400/45404): PG1/PG2 enable+state — values read from
		// Linux intel_reg dump on same hardware (reg_dump.txt):
		//   CTL1=0x00000401 (PG1 req+enabled), CTL2=0x00000C03 (PG1+PG2 req+enabled)
		//   CTL3=0x40000000 (PG3 state only), CTL4=0x00000401
		// V100: preserve UEFI CTL1 state bits (bits 12,14 = display power wells enabled).
		// Writing the hardcoded 0x401 clears those bits, breaking vsync interrupt delivery
		// → WindowServer crash at ~60s ("Display not ready").
		// Fix: read UEFI CTL1 and OR in our minimum bits; keep CTL2 hardcoded at 0x0C03.
		// DO NOT OR CTL2 with UEFI: UEFI leaves 0xfc00 (TC port "state" bits) set in CTL2.
		// Asserting TC bits without IOM handshake disrupts the display domain during mode
		// change, preventing the WSA un-blank (stays at 0x1, no cursor, no desktop).
		uint32_t uefiCtl1 = NGreen::callback->readReg32(0x45400);
		{
			uint32_t newCtl1  = uefiCtl1 | 0x00000401u;  // ensure PG1 req+state bits set
			NGreen::callback->writeReg32(0x45400, newCtl1); // HSW_PWR_WELL_CTL1
			NGreen::callback->writeReg32(0x45404, 0x00000C03); // HSW_PWR_WELL_CTL2 — hardcoded, no TC bits
			SYSLOG("ngreen", "V101: PWR_WELL CTL1 uefi=0x%x->0x%x CTL2=0xc03",
				   uefiCtl1, newCtl1);
		}
		NGreen::callback->writeReg32(0x45408, 0x40000000); // HSW_PWR_WELL_CTL3
		NGreen::callback->writeReg32(0x4540C, 0x00000401); // HSW_PWR_WELL_CTL4
		// ICL_PWR_WELL_CTL_AUX1/2 (0x45440/45444): enable AUX power wells A+B
		// bit1=req, bit0=enabled per ICL DDI power well HW spec (intel_display_regs.h)
		NGreen::callback->writeReg32(0x45440, 0x00000003); // ICL_PWR_WELL_CTL_AUX1 — AUX A enabled
		NGreen::callback->writeReg32(0x45444, 0x00000003); // ICL_PWR_WELL_CTL_AUX2 — AUX B enabled
		// ICL_PWR_WELL_CTL_DDI1/2 (0x45450/45454): enable DDI power wells A+B
		NGreen::callback->writeReg32(0x45450, 0x00000003); // ICL_PWR_WELL_CTL_DDI1 — DDI A enabled
		NGreen::callback->writeReg32(0x45454, 0x00000003); // ICL_PWR_WELL_CTL_DDI2 — DDI B enabled

		// ADL-P / RPL-P display engine registers — exact MMIO init pairs from v3 blob header
		// (adlp_dmc_ver2_16.bin header @ 0x0210, MMIO[0..6], stepping='A' variant)
		NGreen::callback->writeReg32(0x8F074, 0x00086FC0);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);
		NGreen::callback->writeReg32(0x8F004, 0x01240108);
		NGreen::callback->writeReg32(0x8F038, 0xC003B200); // was missing
		NGreen::callback->writeReg32(0x8F008, 0x4FE44F98); // corrected from 0x512050D4
		NGreen::callback->writeReg32(0x8F03C, 0xC003B300);
		NGreen::callback->writeReg32(0x8F00C, 0x571056C0); // corrected from 0x584C57FC
		// DDI C-F (ADL-P TC port registers — DKL PHY, 0x5Fxxx range)
		NGreen::callback->writeReg32(0x5F074, 0x00096FC0);
		NGreen::callback->writeReg32(0x5F034, 0xC003DF00);
		NGreen::callback->writeReg32(0x5F004, 0x214C2114);
		NGreen::callback->writeReg32(0x5F038, 0xC003E000);
		NGreen::callback->writeReg32(0x5F008, 0x22402208);
		NGreen::callback->writeReg32(0x5F03C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5F00C, 0x241422FC);
		NGreen::callback->writeReg32(0x5F040, 0xC0033100);
		NGreen::callback->writeReg32(0x5F010, 0x26F826CC);
		NGreen::callback->writeReg32(0x5F474, 0x0009EFC0);
		NGreen::callback->writeReg32(0x5F434, 0xC003DF00);
		NGreen::callback->writeReg32(0x5F404, 0xA968A930);
		NGreen::callback->writeReg32(0x5F438, 0xC003E000);
		NGreen::callback->writeReg32(0x5F408, 0xAA5CAA24);
		NGreen::callback->writeReg32(0x5F43C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5F40C, 0xAC30AB18);
		NGreen::callback->writeReg32(0x5F440, 0xC0033100);
		NGreen::callback->writeReg32(0x5F410, 0xAF14AEE8);
		NGreen::callback->writeReg32(0x5F874, 0x00053FC0);
		NGreen::callback->writeReg32(0x5F83C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5F80C, 0x25202408);
		NGreen::callback->writeReg32(0x5F840, 0xC0033100);
		NGreen::callback->writeReg32(0x5F810, 0x280427D8);
		NGreen::callback->writeReg32(0x5FC3C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5FC0C, 0x95209408);
		NGreen::callback->writeReg32(0x5FC40, 0xC0033100);
		NGreen::callback->writeReg32(0x5FC10, 0x980497D8);

		// Transcoder A DDI function + timing registers (re-imported from EFI reference for safety).
		// Values from Linux intel_reg dump on this hardware (reg_dump.txt).
		//   0x60400 = TRANS_DDI_FUNC_CTL_A  — DDI A enabled, DP SST, 8bpc, 2 lanes
		//   0x60000 = TRANS_HTOTAL_A        — 2560 active, 2720 total
		//   0x60004 = TRANS_HBLANK_A        — same as HTOTAL on eDP
		//   0x60008 = TRANS_HSYNC_A         — sync positions
		//   0x6000C = TRANS_VTOTAL_A        — 1600 active, 1750 total
		//   0x60010 = TRANS_VBLANK_A
		//   0x60014 = TRANS_VSYNC_A
		//   0x60028 = TRANS_VSYNCSHIFT_A
		//   0x60030/34 TRANS_DATA_M1/N1_A   — DP M/N values (TU 64, link rate)
		//   0x60040/44 TRANS_LINK_M1/N1_A
		// 0x8A000106: bit31=enable, [27:24]=DDI_A, [19:16]=2lanes, [3:1]=DP_SST(0x01)
		// Pre-matches Apple's target so paramsFbCompare sees no lane-count change; changing
		// lane count in TRANS_DDI_FUNC_CTL while transcoder is live resets the DDI buffer
		// and drops the trained link.
		NGreen::callback->writeReg32(0x60400, 0x8A000106); // TRANS_DDI_FUNC_CTL_A
		NGreen::callback->writeReg32(0x60000, 0x0A9F09FF); // TRANS_HTOTAL_A
		NGreen::callback->writeReg32(0x60004, 0x0A9F09FF); // TRANS_HBLANK_A
		NGreen::callback->writeReg32(0x60008, 0x0A4F0A2F); // TRANS_HSYNC_A
		NGreen::callback->writeReg32(0x6000C, 0x06D5063F); // TRANS_VTOTAL_A
		NGreen::callback->writeReg32(0x60010, 0x06D50000); // TRANS_VBLANK_A
		NGreen::callback->writeReg32(0x60014, 0x06480642); // TRANS_VSYNC_A
		NGreen::callback->writeReg32(0x60028, 0x00000000); // TRANS_VSYNCSHIFT_A
		NGreen::callback->writeReg32(0x60030, 0x7E5D159E); // TRANS_DATA_M1_A  (TU 64, M=0x5d159e)
		NGreen::callback->writeReg32(0x60034, 0x00800000); // TRANS_DATA_N1_A  (N=0x800000)
		NGreen::callback->writeReg32(0x60040, 0x0007C1CD); // TRANS_LINK_M1_A  (M=0x7c1cd)
		NGreen::callback->writeReg32(0x60044, 0x00080000); // TRANS_LINK_N1_A  (N=0x80000)

		// Panel power sequencer (re-imported from EFI reference).
		// TGL/ADL-P both have PCH_SPLIT (ICP/TGP PCH) → intel_pps_setup sets
		// mmio_base = PCH_PPS_BASE = 0xC7200 (not 0x61200 which is BXT/APL).
		// Values from Linux intel_reg dump on this hardware:
		//   0xC7204 (PP_CONTROL)   = 0x00000067 (panel on, VDD on, power-on target)
		//   0xC7208 (PP_ON_DELAYS) = 0x07D00001 (T1=1, T3=2000ms power-on delays)
		NGreen::callback->writeReg32(0xC7204, 0x00000067); // PP_CONTROL
		NGreen::callback->writeReg32(0xC7208, 0x07D00001); // PP_ON_DELAYS

		// PIPE_CLK_SEL_A (0x46140 = 0x10000000) REMOVED after the EFI re-import caused
		// "CD Clock PLL is locked" line to disappear from the FB log and link bitRates to
		// turn into garbage (27/40/63 instead of 179/204/206). Empirical: writing this
		// value to 0x46140 here either selects a clock source that prevents CD PLL lock,
		// or 0x46140 isn't actually PIPE_CLK_SEL on Display 13 (ADL-P) — Linux i915
		// names it differently for ADL-P. Leave for Apple's later mode-setup to program.
		// NGreen::callback->writeReg32(0x46140, 0x10000000); // PIPE_CLK_SEL_A

		// Enable DMC — DC_STATE_DEBUG (0x45520) = 2
		NGreen::callback->writeReg32(0x45520, 2); // DC_STATE_DEBUG
		NGreen::callback->dmcIsAdlp = true;
		NGreen::callback->uefiCtl1  = uefiCtl1;  // save for V60 re-enforcement
		SYSLOG("ngreen", "hwInitCState: ADL-P DMC loaded");
		// setSignalLevelsADLP block REMOVED after the EFI re-import caused link bitRates
		// to turn into garbage (27/40/63 instead of 179/204/206). Likely cause: calling
		// it with swing[]=preEmph[]={0,0,0,0} BEFORE Apple's link training overwrites
		// the UEFI-trained combo PHY DW2/4/5/7 with zero levels → DP link reads back
		// nonsense bitrates. The setSignalLevels function should only be invoked DURING
		// link training when swing/preEmph are properly populated, not as init scaffolding.
		// Left as a comment so we can re-enable surgically once real swing values are known.
		// {
		// 	uint8_t swing[4]   = {0, 0, 0, 0};
		// 	uint8_t preEmph[4] = {0, 0, 0, 0};
		// 	IntelDPLinkTraining::setSignalLevelsADLP(/*phy=*/0, /*lanes=*/4, /*isHBR2=*/false, /*isEDP=*/true,  swing, preEmph);
		// 	IntelDPLinkTraining::setSignalLevelsADLP(/*phy=*/1, /*lanes=*/4, /*isHBR2=*/false, /*isEDP=*/false, swing, preEmph);
		// }
		// Let original run with B48=1 so the ICL CSR blob is loaded to SRAM.
		// The ADL-P DMC firmware (even correct binary) autonomously drops all eDP lanes
		// at ~10s because its DC state management code runs on ADL-P hardware and performs
		// link maintenance that the ICL driver can't recover from. The ICL DMC running on
		// ADL-P hardware does NOT cause lane drops (it doesn't know ADL-P DC3CO/PSR2).
		// Our ADL-P display engine context regs (8Fxxx) are re-applied after the original
		// so the display engine still sees ADL-P-correct values despite ICL SRAM content.
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
		// Re-apply ADL-P context regs overwritten by original's ICL blob load.
		NGreen::callback->writeReg32(0x8F074, 0x00086FC0);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);
		NGreen::callback->writeReg32(0x8F004, 0x01240108);
		NGreen::callback->writeReg32(0x8F038, 0xC003B200);
		NGreen::callback->writeReg32(0x8F008, 0x4FE44F98);
		NGreen::callback->writeReg32(0x8F03C, 0xC003B300);
		NGreen::callback->writeReg32(0x8F00C, 0x571056C0);
		SYSLOG("ngreen", "V104: ADL-P context regs re-applied after ICL blob load");
		// V102: CTL1 restore — ICL DMC save/restore table writes CTL1=0x401, clearing
		{
			uint32_t postCtl1 = NGreen::callback->readReg32(0x45400);
			uint32_t fixCtl1  = uefiCtl1 | 0x00000401u;
			if (postCtl1 != fixCtl1) {
				NGreen::callback->writeReg32(0x45400, fixCtl1);
				SYSLOG("ngreen", "V102: restore PWR_WELL CTL1 0x%x->0x%x", postCtl1, fixCtl1);
			}
		}
		// V105: disable PSR1+PSR2 — the ICL DMC initializer (original hwInitializeCState)
		// enables PSR2 (EDP_PSR2_CTL bit 0 = 1) for the eDP panel. On ADL-P hardware with
		// the ICL driver the PSR2 selective-update path is non-functional: the panel locks
		// into self-refresh showing the initial black frame; only the cursor plane (separate
		// SU path) updates. Clearing both registers before the first frame is displayed
		// restores normal scanout. Confirmed root cause via V76: PSR2_CTL=0x4811, V88
		// direct physical writes to SURF invisible despite valid GGTT PTEs.
		{
			// V105 ADDRESS FIX: PSR1 control on TGL is at 0x60800 (transcoder EDP space),
			// NOT 0x64800 (older Gen ICL/SKL layout). Pre-fix V105 was writing to a wrong
			// register; PSR1 stayed enabled at 0x60800 = 0x00100001, panel kept refreshing
			// from its own cache, screen frozen on first frame even while PIPE_FRMCOUNT
			// kept advancing. Probe V205 caught this. Now writes to both addresses for
			// safety (0x60800 = TGL, 0x64800 = legacy/ICL — covers both spoof paths).
			uint32_t psr2ctl     = NGreen::callback->readReg32(0x60A10);  // EDP_PSR2_CTL TGL
			uint32_t psr1ctlTgl  = NGreen::callback->readReg32(0x60800);  // EDP_PSR_CTL  TGL
			uint32_t psr1ctlIcl  = NGreen::callback->readReg32(0x64800);  // EDP_PSR_CTL  ICL
			uint32_t psr2ctlIcl  = NGreen::callback->readReg32(0x60900);  // EDP_PSR2_CTL ICL
			NGreen::callback->writeReg32(0x60A10, 0);  // PSR2 TGL
			NGreen::callback->writeReg32(0x60800, 0);  // PSR1 TGL  ← THE ACTUAL FIX
			NGreen::callback->writeReg32(0x60900, 0);  // PSR2 legacy
			NGreen::callback->writeReg32(0x64800, 0);  // PSR1 legacy
			SYSLOG("ngreen", "V105: PSR disabled — TGL(PSR1=0x%x PSR2=0x%x) legacy(PSR1=0x%x PSR2=0x%x)",
				   psr1ctlTgl, psr2ctl, psr1ctlIcl, psr2ctlIcl);
		}

	} else if (dmcArg[0] == 'i' || dmcArg[0] == 'I') {
		// ── ICL ──
		// ICL is the native target. PHY levels belong to actual link training;
		// identical register layout does not make TGL electrical tables valid
		// for ICL or justify forcing two unnegotiated four-lane HBR links here.
		SYSLOG("ngreen", "hwInitCState: ngreen-dmc=icl, native passthrough");
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
		SYSLOG("ngreen", "hwInitCState: ICL done");

	} else {
		// ── skip (default, safe fallback) ──
		// Proven working: just let original run + configure AUX.
		// No DMC blob, no display engine register writes.
		SYSLOG("ngreen", "hwInitCState: skip (safe fallback), passthrough original");
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
	}

	hwConfigureCustomAUX(that, true);
	SYSLOG("ngreen", "hwInitCState: done");
}

void NGreen::adlpDcExit(const char *caller) {
	if (!dmcIsAdlp) return;
	const uint32_t dcState = readReg32(0x45504); // DC_STATE_EN
	if (dcState == 0) return;
	static int adlpDcExitCount = 0;
	if (adlpDcExitCount < 48) {
		adlpDcExitCount++;
		SYSLOG("ngreen", "adlpDcExit[%s/%d]: DC_STATE_EN=0x%x — restoring ADL-P display state", caller, adlpDcExitCount, dcState);
	}
	// 1. Disable DC states so clock-gated blocks latch the writes below.
	writeReg32(0x45504, 0);
	// 2. Restore power wells (request enable on CTL1/2/3/4 + AUX A/B + DDI A/B).
	writeReg32(0x45400, uefiCtl1 | 0x401u);  // PWR_WELL_CTL1
	writeReg32(0x45404, 0x0C03u);             // PWR_WELL_CTL2
	writeReg32(0x45408, 0x40000000u);         // PWR_WELL_CTL3
	writeReg32(0x4540C, 0x401u);              // PWR_WELL_CTL4
	writeReg32(0x45440, 0x3u);               // ICL_PWR_WELL_CTL_AUX1 — AUX A
	writeReg32(0x45444, 0x3u);               // ICL_PWR_WELL_CTL_AUX2 — AUX B
	writeReg32(0x45450, 0x3u);               // ICL_PWR_WELL_CTL_DDI1 — DDI A
	writeReg32(0x45454, 0x3u);               // ICL_PWR_WELL_CTL_DDI2 — DDI B
	// 3. Restore ADL-P display engine context registers (saved by DMC on DC entry).
	writeReg32(0x8F074, 0x00086FC0u);
	writeReg32(0x8F034, 0xC003B400u);
	writeReg32(0x8F004, 0x01240108u);
	writeReg32(0x8F038, 0xC003B200u);
	writeReg32(0x8F008, 0x4FE44F98u);
	writeReg32(0x8F03C, 0xC003B300u);
	writeReg32(0x8F00C, 0x571056C0u);
	// 4. Restore panel power sequencer.
	writeReg32(0xC7204, 0x67u);  // PP_CONTROL
	// 5. Disable PSR — DMC may have re-enabled it on DC exit.
	writeReg32(0x60800, 0u);   // EDP_PSR_CTL (TGL)
	writeReg32(0x60A10, 0u);   // EDP_PSR2_CTL (TGL)
	if (adlpDcExitCount <= 48) {
		SYSLOG("ngreen", "adlpDcExit[%s]: restore complete", caller);
	}
}

void Gen11::AppleIntelPowerWellinit(AppleIntel::AppleIntelPowerWell *that, AppleIntel::AppleIntelBaseController *param_1)
{
	ccont = param_1->fRegCachePool;

	FunctionCast(AppleIntelPowerWellinit, callback->oAppleIntelPowerWellinit)(that, param_1);

	// After callthrough the south display domain is clocked; direct BAR read is safe.
	uint32_t pg  = NGreen::callback->readReg32(0x45404);
	uint32_t ddi = NGreen::callback->readReg32(0x45454);
	uint32_t aux = NGreen::callback->readReg32(0x45444);
	SYSLOG("ngreen", "PowerWell::init UEFI state — PG=0x%08x DDI=0x%08x AUX=0x%08x", pg, ddi, aux);

	// Apple's PowerWell::init checks fController->flags_ig & FB_FLAG_BOOST_PIXEL_FREQUENCY_LIMIT
	// (+0xC58) before setting fAlwaysOn=1. On RPL/ADL that flag isn't set when the kext first
	// calls PowerWell::init (initPlatformWorkarounds runs later), so fAlwaysOn stays 0 and
	// Apple can gate power wells off. We force fAlwaysOn=1 unconditionally on non-real-TGL.
	// Also stamp fMMIO in case Apple's TGL-path init skipped it on ADL-P hardware.
	SYSLOG("ngreen", "PowerWell::init — flags_ig=0x%x fAlwaysOn(before)=%u fMMIO=%p",
		   param_1->flags_ig, that->fAlwaysOn, that->fMMIO);
	if (!NGreen::callback->isRealTGL) {
		that->fAlwaysOn = 1;
		if (!that->fMMIO) that->fMMIO = reinterpret_cast<AppleIntel::AppleIntelMMIO *>(ccont);
		SYSLOG("ngreen", "PowerWell::init forced fAlwaysOn=1, fMMIO=%p fMMIOBase=%p",
			   that->fMMIO, that->fMMIO ? that->fMMIO->fMMIOBase : nullptr);
	}
	SYSLOG("ngreen", "PowerWell::init done — fAlwaysOn=%u fPGBase=%u PG1=%u PG2=%u PG3=%u PG4=%u",
		   that->fAlwaysOn, that->fPGBase, that->fPG1, that->fPG2, that->fPG3, that->fPG4);
	SYSLOG("ngreen", "PowerWell::init DDI — [0]=%u [1]=%u [2]=%u [3]=%u [4]=%u [5]=%u [6]=%u [7]=%u [8]=%u",
		   that->fDDI[0], that->fDDI[1], that->fDDI[2], that->fDDI[3], that->fDDI[4],
		   that->fDDI[5], that->fDDI[6], that->fDDI[7], that->fDDI[8]);
	SYSLOG("ngreen", "PowerWell::init AUX — [0]=%u [1]=%u [2]=%u [3]=%u [4]=%u [5]=%u [6]=%u [7]=%u [8]=%u",
		   that->fAUX[0], that->fAUX[1], that->fAUX[2], that->fAUX[3], that->fAUX[4],
		   that->fAUX[5], that->fAUX[6], that->fAUX[7], that->fAUX[8]);
}

bool Gen11::AppleIntelBaseControllerstart(AppleIntel::AppleIntelBaseController *that, IOService *param_1)
{
	if (!that || !ngPhysicalGpuAccessAllowed())
		return false;
	callback->framecont = that;
	ccont2 = that;
	if (auto *accessor = getMember<void *>(that, 0xC40))
		ccont = accessor;
	// V25: Display workarounds BEFORE start (no ForceWake needed for display regs 0x4xxxx+).
	// GT workarounds moved AFTER start (ForceWake must be held for GT regs 0x0-0x7FFF).
	
	SYSLOG("ngreen", "AppleIntelBaseControllerstart: applying display workarounds");

	// Disable DC states during init to prevent power domain conflicts
	NGreen::callback->writeReg32(DC_STATE_EN, 0);
	
	/* Wa_14011294188:ehl,jsl,tgl,rkl,adl-s */
	NGreen::callback->intel_de_rmw(SOUTH_DSPCLK_GATE_D, 0,
				PCH_DPMGUNIT_CLOCK_GATE_DISABLE);
	
	// PCH reset handshake
	NGreen::callback->intel_de_rmw(HSW_NDE_RSTWRN_OPT, RESET_PCH_HANDSHAKE_ENABLE,
				RESET_PCH_HANDSHAKE_ENABLE);
	
	/* Wa_14011508470:tgl,dg1,rkl,adl-s,adl-p,dg2 */
	NGreen::callback->intel_de_rmw(GEN11_CHICKEN_DCPR_2, 0,
				DCPR_CLEAR_MEMSTAT_DIS | DCPR_SEND_RESP_IMM |
				DCPR_MASK_LPMODE | DCPR_MASK_MAXLATENCY_MEMUP_CLR);
	
	/* Display WA #1185 WaDisableDARBFClkGating:glk,icl,ehl,tgl (Wa_14010480278) */
	NGreen::callback->intel_de_rmw(GEN9_CLKGATE_DIS_0, 0, DARBF_GATING_DIS);
	
	/* Wa_14013723622 */
	NGreen::callback->intel_de_rmw(CLKREQ_POLICY, CLKREQ_POLICY_MEM_UP_OVRD, 0);
	
	SYSLOG("ngreen", "AppleIntelBaseControllerstart: display workarounds applied");

	SYSLOG("ngreen", "FBController::start() entering...");
	auto ret=FunctionCast(AppleIntelBaseControllerstart, callback->oAppleIntelBaseControllerstart)(that,param_1 );
	SYSLOG("ngreen", "FBController::start() returned %d", ret);
	
	if (ret) {
		// The personality dict itself was registered in IOCatalogue from the HW-kext
		// processKext branch (see Gen11::injectAcceleratorPersonality). All this wrapper
		// does — and all an FB-tier route should do — is poke the FBController service
		// so IOKit re-runs matching now that the personality is present.
		auto *service = OSDynamicCast(IOService, reinterpret_cast<OSObject *>(that));
		if (service) {
			SYSLOG("ngreen", "FBController: calling registerService() to trigger accelerator matching");
			service->registerService();
		}
	}

	return ret;
}

uint32_t Gen11::wrapProbeCDClockFrequency(AppleIntel::AppleIntelBaseController *that) {

	// Sonoma probeCDClockFrequency checks reg 0x46070 (BXT_DE_PLL_ENABLE) bit 31 first.
	// If bit 31 is CLEAR it panics immediately: "Wrong CD clock frequency set by EFI".
	// EFI on Hackintosh may leave this clear, so we ensure it is set before calling the original.
	auto squash = NGreen::callback->readReg32(BXT_DE_PLL_ENABLE);  // byte addr OK — readReg32 divides internally
	if (!(squash & BXT_DE_PLL_PLL_ENABLE)) {
		DBGLOG("ngreen", "wrapProbeCDClockFrequency: BXT_DE_PLL_PLL_ENABLE (0x46070 bit31) was clear (0x%x), setting it", squash);
		NGreen::callback->writeReg32(BXT_DE_PLL_ENABLE, squash | BXT_DE_PLL_PLL_ENABLE);
	}

	// Always sanitize (disable + reprogram PLL) regardless of current CDCLK value.
	//
	// When BIOS leaves CDCLK already at >= 648 MHz (threshold), skipping sanitize causes
	// orgProbeCDClockFrequency to take the "cdclk already at target" path, which reads PCU
	// mailbox command 0x6 (PCODE_CDCLK_CONFIG verify). On ADL-P this returns 0x9b9b9b9b
	// (PCU not responding / command not supported), causing initCDClock to return an error.
	// start() then skips port allocation and boot display setup entirely — no hwUpdateCursorMemory,
	// no cursor GGTT entries → full black screen even when the display link is trained.
	//
	// Always calling sanitize takes the "frequency changed" success path in initCDClock,
	// which writes mailbox(7,2) ACK and skips the failing PCU 0x6 verify read.
	auto cdclk = NGreen::callback->readReg32(ICL_REG_CDCLK_CTL) & CDCLK_FREQ_DECIMAL_MASK;  // byte addr OK
	SYSLOG("ngreen", "wrapProbeCDClockFrequency: cdclk=0x%x, force-sanitizing to bypass PCU mailbox failure", cdclk);
	sanitizeCDClockFrequency(that);

	auto retVal = callback->orgProbeCDClockFrequency(that);
	return retVal;
}

void Gen11::sanitizeCDClockFrequency(AppleIntel::AppleIntelBaseController *that) {

	//auto referenceFrequency = callback->wrapReadRegister32(that, SKL_DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK;
	auto referenceFrequency =NGreen::callback->readReg32(ICL_REG_DSSM)>> 29;
	//auto referenceFrequency = callback->wrapReadRegister32(that, ICL_REG_DSSM) >> 29;
	uint32_t newPLLFrequency = 0;
	switch (referenceFrequency) {
		case ICL_REF_CLOCK_FREQ_19_2:
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_19_2;
			break;
			
		case ICL_REF_CLOCK_FREQ_24_0:
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_24_0;
			break;
			
		case ICL_REF_CLOCK_FREQ_38_4:
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_38_4;
			break;
			
		default:
			return;
	}

	DBGLOG("ngreen", "sanitizeCDClockFrequency: ref=%u targetPll=0x%x", referenceFrequency, newPLLFrequency);

	// Use solved original directly so sanitize remains safe even when disableCDClock route is toggled off.
	if (callback->orgDisableCDClock) {
		callback->orgDisableCDClock(that);
	} else {
		disableCDClock(that);
	}

	callback->orgSetCDClockFrequency(that, newPLLFrequency);

}

void Gen11::disableCDClock(AppleIntel::AppleIntelBaseController *that)
{
	FunctionCast(disableCDClock, callback->odisableCDClock)(that );
}

uint8_t Gen11::hwRegsNeedUpdate
		  (AppleIntel::AppleIntelBaseController *that,
		   AppleIntel::AppleIntelFramebuffer *param_1,
		   AppleIntel::AppleIntelDisplayPath *param_2,
		   AppleIntel::CRTCParams *param_3,
		   const IODetailedTimingInformationV2 *param_4,
		   AppleIntel::SCALERPARAMS *param_5)
{
	// ADL-P DC exit: restore power wells and context before Apple writes registers.
	if (NGreen::callback->dmcIsAdlp)
		NGreen::callback->adlpDcExit("hwRNU");

	// param_3 is the pending CRTCParams built by SetupParams.
	if (!NGreen::callback->isRealTGL && param_3) {
		auto *params = param_3;

		// V97P: clear bit[16] in TRANS_DDI_FUNC_CTL.
		// Apple's SetupParams sets bit16 as a port-type flag; UEFI/HW never sets it.
		// Without this the DDI FUNC_CTL compare fires and triggers a full modeset
		// that disrupts the already-trained 4-lane eDP link → black screen.
		if (params->TRANS_DDI_FUNC_CTL & (1u << 16)) {
			static int v97PCount = 0;
			if (v97PCount < 12) {
				v97PCount++;
				SYSLOG("ngreen", "V97P[%d]: CRTCParams TRANS_DDI_FUNC_CTL 0x%x -> 0x%x",
					   v97PCount, params->TRANS_DDI_FUNC_CTL,
					   params->TRANS_DDI_FUNC_CTL & ~(1u << 16));
			}
			params->TRANS_DDI_FUNC_CTL &= ~(1u << 16);
		}

		// V97C: align pending TRANS_CONF with the live HW value.
		// paramsFbCompare logs "TRANS_CONF 0xc0000000->0xc0000024": HW has bits[5,2]
		// clear (UEFI default), Apple wants to set them (interlace/depth config).
		// Writing those bits to an active pipe causes a transient signal disruption
		// that sets the panel's DPCD InterLane Alignment Lost bit (0x202=0x80),
		// which checkLinkStatus detects ~10 s later and tears down the display.
		// Fix: replace the pending TRANS_CONF with the current HW register value so
		// paramsFbCompare sees no change and the partial pipe update is suppressed.
		// PIPE_CONF_A (= TRANS_CONF in ICL+) = 0x70008.
		// NOTE: 0x60008 is TRANS_HSYNC_A (horizontal sync timing) — do NOT use that.
		const uint32_t hwTransConf = NGreen::callback->readReg32(0x70008);
		if (params->TRANS_CONF != hwTransConf) {
			static int v97CCount = 0;
			if (v97CCount < 12) {
				v97CCount++;
				SYSLOG("ngreen", "V97C[%d]: CRTCParams TRANS_CONF 0x%x -> HW 0x%x (suppressed pipe update)",
					   v97CCount, params->TRANS_CONF, hwTransConf);
			}
			params->TRANS_CONF = hwTransConf;
		}

		// V300 REVERTED — was a memory-write hack zeroing CRTCParams[+0xE8]/[+0xEC] to
		// kill DSC engine select bits. Per jalavoui's feedback ("stop hacking memory
		// writes, fix the origin not the destination"), DSC should be controlled at
		// getDPCDInfo / Info.plist FeatureControl level, not patched after-the-fact in
		// the CRTCParams struct. Our Info.plist already sets DSCSupport=0/DSCCapReporting=0
		// (confirmed by fb.log "DSC supported: 0" / "DSC Caps Reporting enabled: 0"),
		// so if Apple still calls setupDSCEngineParams the path is somewhere downstream
		// that ignores FeatureControl — needs investigation at the origin function, not
		// here. Linux on this hardware: VBT Port A DSC:0, link runs "DSC off" → DSC was
		// likely a wrong hypothesis for the fragmentation symptom in the first place.
	}

	static int CCount = 0;
	CCount++;
	SYSLOG("ngreen", "V97C[%d]: hwRegsNeedUpdate called", CCount);
	// Return the original result so that register reprogramming proceeds normally.
	// The lane count mismatch (4→2) that previously broke the display is now fixed
	// by the computeLaneCount hook forcing 4 lanes.  Without register updates, the
	// plane surface address and stride never get written, leaving the stale BIOS
	// framebuffer on screen (grey/black vertical bars with only cursor visible).
	return FunctionCast(hwRegsNeedUpdate, callback->ohwRegsNeedUpdate)(that, param_1, param_2, param_3, param_4, param_5);
}

int Gen11::wrapHwSetupMemory(AppleIntel::AppleIntelBaseController *that, AppleIntel::AppleIntelFramebuffer *fb, AppleIntel::AppleIntelDisplayPath *displayPath, AppleIntel::CRTCParams *params, bool isAperture)
{
	int ret = FunctionCast(wrapHwSetupMemory, callback->ohwSetupMemory)(that, fb, displayPath, params, isAperture);

	// V201: post-hwSetupMemory probe — read 32 bytes at the SURF GTT offset through BAR2.
	// SURF address is at fb+0x4330 (per hwSetupMemory decomp at offset 0x6E046).
	// If buffer is all zeros → freshly-allocated IOBufferMemoryDescriptor pages, nothing
	// has rendered into them yet. If non-zero → some path filled them (EFI GOP carry-over,
	// CoreDisplay handoff blit, or other).
	static int v201Count = 0;
	if (v201Count < 8 && NGreen::callback) {
		uint32_t surfAddr = getMember<uint32_t>(fb, 0x4330);
		uint32_t fbSize   = getMember<uint32_t>(fb, 0x4334);
		uint8_t  fbIdx    = getMember<uint8_t>(fb,  0x4288);
		uint8_t  yTileFlg = getMember<uint8_t>(fb,  0x4A18);

		volatile uint32_t *aperture = nullptr;
		uint64_t apertureLen = 0;
		// Sample top-left and screen-center for 2560×1600 BGRA layout.
		uint32_t tlCtr = 0xDEADBEEF, ctr = 0xDEADBEEF, bar = 0xDEADBEEF, mid = 0xDEADBEEF;
		if (NGreen::callback->getAperture(aperture, apertureLen) &&
			uint64_t(surfAddr) + 0xA00000 <= apertureLen) {
			volatile uint32_t *fb32 = aperture + (surfAddr / sizeof(uint32_t));
			tlCtr = fb32[0];
			ctr   = fb32[0x7D2800 / 4];  // (1280,800) Apple logo center
			bar   = fb32[0x9C7800 / 4];  // (1280,1000) loading-bar row
			mid   = fb32[0x7D1A00 / 4];  // (640,800) mid-left of logo area
		}
		// V201P: read GGTT PTE for the surface page — verifies display controller can access it.
		// GGTT PTE LO/HI at GEN8_GGTT_PTE_BASE(0x800000) + page*8. Valid: bit0=present, bit3=LLC.
		uint32_t pteLo = 0, pteHi = 0;
		if (surfAddr && NGreen::callback->mmioValid()) {
			uint32_t surfPage = surfAddr >> 12;
			pteLo = NGreen::callback->readReg32(GGTT_PTE_LO(surfPage));
			pteHi = NGreen::callback->readReg32(GGTT_PTE_HI(surfPage));
		}
		v201Count++;
		SYSLOG("ngreen", "V201[%d]: hwSetupMemory ret=0x%x fb=%p surf=0x%x size=0x%x idx=%u tile=%u | tl=%08x ctr=%08x bar=%08x mid=%08x | PTE=%08x:%08x(present=%d llc=%d)",
			   v201Count, ret, fb, surfAddr, fbSize, fbIdx, yTileFlg, tlCtr, ctr, bar, mid,
			   pteHi, pteLo, pteLo & 1, (pteLo >> 3) & 1);
	}

	return ret;
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
	if (vfActive && !vfBootstrapBinder()) {
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

void Gen11::getGPUInfoICL(void *that)
{
	FunctionCast(getGPUInfoICL, callback->ogetGPUInfoICL)(that);
	
	// --- GPU topology override for ICL HW binary ---
	// ICL object layout (verified from AppleIntelICLGraphics.sonoma.bin disassembly):
	//   0x1190 = NumSlices          0x12cc = NumSlices mirror
	//   0x1188 = NumSubSlices       0x12d0 = NumSubSlices mirror
	//   0x11a0 = MaxEUPerSubSlice
	//   0x1154 = ExecutionUnitCount (= MaxEUPerSubSlice × NumSubSlices)
	//   0x1198 = L3BankCount
	//   0x1150 = GPU Sku
	// ICL counts traditional sub-slices (same as TGL binary).
	// Use ICL GT2 LP config (1×8×8 = 64 EU) to stay within ICL-valid topology.
	unsigned int numSlices        = 1;
	unsigned int numSubSlices     = 8;   // ICL GT2 LP max (8 SS)
	unsigned int maxEUPerSubSlice = 8;
	unsigned int totalEU          = maxEUPerSubSlice * numSubSlices; // = 64
	
	getMember<UInt32>(that, 0x1190) = numSlices;
	getMember<UInt32>(that, 0x1188) = numSubSlices;
	getMember<UInt32>(that, 0x11a0) = maxEUPerSubSlice;
	getMember<UInt32>(that, 0x1154) = totalEU;
	getMember<UInt32>(that, 0x12cc) = numSlices;     // NumSlices mirror
	getMember<UInt32>(that, 0x12d0) = numSubSlices;  // NumSubSlices mirror
	getMember<UInt32>(that, 0x1198) = 8;             // L3BankCount
	
	SYSLOG("ngreen", "getGPUInfoICL: overridden topology → slices=%u subslices=%u maxEU/SS=%u totalEU=%u L3Banks=8",
		   numSlices, numSubSlices, maxEUPerSubSlice, totalEU);
}

void Gen11::getGPUInfo(void *that)
{

#define RPM_CONFIG0				(0xd00)
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT	3
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK	(1 << GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT)
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_19_2_MHZ	0
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_24_MHZ	1
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT	3
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK	(0x7 << GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT)
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_24_MHZ	0
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_19_2_MHZ	1
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_38_4_MHZ	2
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_25_MHZ	3
#define   GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT	1
#define   GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_MASK	(0x3 << GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT)
	
	FunctionCast(getGPUInfo, callback->ogetGPUInfo)(that);

	// --- GPU topology override for the target RPL i7-13620H UHD SR-IOV VF ---
	// A DRM_I915_QUERY_TOPOLOGY_INFO query on the PF reports:
	// 1 slice, 4 enabled DSS, 16 EU/DSS, 64 EU total.
	// TGL binary uses traditional sub-slices (SS), not dual sub-slices (DSS):
	//   4 DSS × 2 SS/DSS = 8 SS, 16 EU/DSS / 2 = 8 EU/SS, 8 × 8 = 64 EU.
	// Object layout (byte offsets from `this`, verified via disassembly):
	//   0x115c = NumSlices          0x0dd8 = NumSlices mirror
	//   0x1158 = NumSubSlices       0x0ddc = NumSubSlices mirror
	//   0x116c = MaxEUPerSubSlice
	//   0x1124 = ExecutionUnitCount (= MaxEUPerSubSlice × NumSubSlices)
	//   0x1150 = Frequency pair (low32=fMaxMHz, high32=fMinMHz)
	//   0x1164 = L3BankCount
	unsigned int numSlices        = 1;
	unsigned int numSubSlices     = 8;   // 4 DSS × 2 = 8 traditional SS
	unsigned int maxEUPerSubSlice = 8;   // 16 EU/DSS ÷ 2 SS/DSS = 8 EU/SS
	unsigned int totalEU          = maxEUPerSubSlice * numSubSlices; // = 64
	
	getMember<UInt32>(that, 0x115c) = numSlices;
	getMember<UInt32>(that, 0x1158) = numSubSlices;
	getMember<UInt32>(that, 0x116c) = maxEUPerSubSlice;
	getMember<UInt32>(that, 0x1124) = totalEU;
	getMember<UInt32>(that, 0x0dd8) = numSlices;
	getMember<UInt32>(that, 0x0ddc) = numSubSlices;
	getMember<UInt32>(that, 0x1164) = 8;  // L3BankCount (confirmed from InsanelyMac TGL logs)
	
	// Target PF sysfs reports RP0=1500 MHz and RPn=100 MHz.
	getMember<uint64_t>(that, 0x1150) = 0x064000005DCULL;
	
	SYSLOG("ngreen", "getGPUInfo: overridden topology → slices=%u subslices=%u maxEU/SS=%u totalEU=%u L3Banks=8",
		   numSlices, numSubSlices, maxEUPerSubSlice, totalEU);

}

// V164: Hook populateResetRegisterList to clear PERCTX_PREEMPT_CTRL before snapshot.
//
// IntelAccelerator::startGraphicsEngine writes 0x40004000 to 0x20E0, then calls
// populateResetRegisterList which reads the live MMIO value via [rax+20E0h] and stores
// it into a per-context replay list (a batch of MI_LRI commands replayed before every
// context switch). If 0x4000 is snapshotted into that list, hardware perpetually
// replays it back, overriding any post-hoc MMIO clear (V162/V163 arrive too late).
//
// Fix: clear bit 14 BEFORE calling original so the snapshot captures 0x0000,
// making the replay batch write 0x0000 to 0x20E0 on every context switch instead.
void Gen11::populateResetRegisterList(void *that)
{
	const auto identity = vfIdentifyDevice();
	if (identity == VfIdentity::Virtual) {
		// The VF has no guest-owned legacy reset registers; GuC restores its
		// direct-LRCA contexts from PF-managed state.
		return;
	}
	if (identity != VfIdentity::Physical)
		return;
	FunctionCast(populateResetRegisterList,
	             callback->opopulateResetRegisterList)(that);
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
	const auto identity = vfIdentifyDevice();
	if (identity == VfIdentity::Virtual) {
		// Ring power, reset and legacy execlist state belong to the PF. The
		// translated GuC scheduler brings VF contexts online independently.
		return 1;
	}
	if (identity != VfIdentity::Physical)
		return 0;
	return FunctionCast(startGraphicsEngine,
	                    callback->ostartGraphicsEngine)(that);
}

unsigned long Gen11::stopGraphicsEngine(void *that)
{
	const auto identity = vfIdentifyDevice();
	if (identity == VfIdentity::Virtual) {
		if (gVfDeviceStopping && gVfGGTTReady) {
			PANIC_COND(!vfQuiesceDeviceForShutdown(gVfHardwareGuc), "ngreen",
				"Cannot stop VF accelerator before every GuC context and DMA mapping is quiesced");
			SYSLOG("ngreen", "V242: VF final engine stop completed through GuC context retirement");
		}
		// Runtime reset and final ring stop are both PF/GuC-owned. The final
		// path above first closes every guest producer and waits for DMA.
		return 1;
	}
	if (identity != VfIdentity::Physical)
		return 0;
	return FunctionCast(stopGraphicsEngine,
	                    callback->ostopGraphicsEngine)(that);
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

unsigned long Gen11::loadIclGuCBinary(void *that) {
	// ICL and TGL may both be loaded. Never share their original-function
	// slot or interpret an ICL object using TGL's private scheduler layout.
	if (!that || vfIdentifyDevice() != VfIdentity::Physical) {
		vfMarkProtocolFault("ICL GuC firmware path is unavailable to a VF");
		return 0;
	}
	return FunctionCast(loadIclGuCBinary, callback->oLoadIclGuCBinary)(that);
}

unsigned long Gen11::loadGuCBinary(void *that) {
	// The PF already owns and runs GuC for an SR-IOV VF. Report firmware as
	// available so Apple's GuC scheduler initializes its submission transport,
	// but never try to replace the PF-owned image or WOPCM configuration.
	// ICL has a separate hook/original slot. The VF scheduler-data path is
	// implemented only for the UUID-pinned TGL payload.
	if (vfIdentifyDevice() != VfIdentity::Physical) {
		if (gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady || gVfProtocolFault)
			return 0;
		// The stock load routine initializes all scheduler-side allocations
		// before touching WOPCM and uploading firmware.  Skipping it outright
		// left contextCount at zero, so the very first createUkContext returned
		// the 0x400 invalid-context sentinel and tore the GuC object down.
		if (!that || !callback->orgInitSchedControl || !vfCanUseSleepingLock() ||
		    !getMember<void *>(that, 0x38) || !getMember<IOLock *>(that, 0x40) ||
		    !getMember<IOLock *>(that, 0xA08)) {
			vfMarkProtocolFault("missing VF scheduler initializer, owner or locks");
			return 0;
		}
		using InitSchedControl = bool (*)(void *);
		const bool initialized = reinterpret_cast<InitSchedControl>(
			callback->orgInitSchedControl)(that);
		// Native initSchedControl checks setupContextPool but discards failures
		// from setupLogBuffers and setupAdditionalDataStructs before returning
		// true. Do not admit partially constructed scheduler storage.
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

	// Firmware belongs to the actual GPU, not the host/guest CPUID model or
	// the spoofed device-id property. Other physical generations need their
	// own validated firmware path, not a successful return without a load.
	if (that && NGGpuCapabilities::isTigerLake(NGreen::callback->getOriginalDeviceId())) {
		SYSLOG("ngreen", "loadGuCBinary: real TGL — calling original for GuC firmware load");
		return FunctionCast(loadGuCBinary, callback->oloadGuCBinary)(that);
	}
	SYSLOG("ngreen", "loadGuCBinary: unsupported physical firmware path; refusing success");
	return 0;
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
	if (gVfIdentity == VfIdentity::Physical)
		return FunctionCast(vfCreateUkContext, callback->oVfCreateUkContext)(
			that, owner, priority);
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
	if (gVfIdentity == VfIdentity::Physical)
		return FunctionCast(vfAllocContextId, callback->oVfAllocContextId)(that, owner, clear);
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
	if (gVfIdentity == VfIdentity::Physical) {
		FunctionCast(vfReleaseContextId, callback->oVfReleaseContextId)(that, id);
		return;
	}
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
	if (gVfIdentity == VfIdentity::Physical)
		return FunctionCast(vfAcquireDoorbell, callback->oVfAcquireDoorbell)(that, descriptor, pin);
	vfMarkProtocolFault("legacy doorbell acquisition on direct-LRCA VF");
	return 0x100U; // native invalid-doorbell sentinel, not an allocated ID
}

void Gen11::vfReleaseDoorbell(void *that, void *descriptor) {
	if (gVfIdentity == VfIdentity::Physical) {
		FunctionCast(vfReleaseDoorbell, callback->oVfReleaseDoorbell)(that, descriptor);
		return;
	}
	// No legacy doorbell can have been acquired through the VF entry points.
	// Do not clear 0xfd4/0x1000 register banks or pretend hardware was released.
	vfMarkProtocolFault("legacy doorbell release on direct-LRCA VF");
}

bool Gen11::vfAllocUkDoorbell(void *that, uint32_t contextId, bool pin) {
	if (gVfIdentity == VfIdentity::Physical)
		return FunctionCast(vfAllocUkDoorbell, callback->oVfAllocUkDoorbell)(that, contextId, pin);
	vfMarkProtocolFault("legacy UK doorbell allocation on direct-LRCA VF");
	return false;
}

uint16_t Gen11::vfReacquireDoorbell(void *that, uint32_t contextId) {
	if (gVfIdentity == VfIdentity::Physical)
		return FunctionCast(vfReacquireDoorbell, callback->oVfReacquireDoorbell)(that, contextId);
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
	if (gVfIdentity == VfIdentity::Physical)
		return FunctionCast(vfIsGuCIdle, callback->oVfIsGuCIdle)(that);
	return vfKnownIdleSnapshot();
}

bool Gen11::vfIsContextIdle(void *that, uint32_t contextId) {
	if (gVfIdentity == VfIdentity::Physical)
		return FunctionCast(vfIsContextIdle, callback->oVfIsContextIdle)(that, contextId);
	// Legacy proxy ID cannot identify a single modern LRCA. All-idle is a
	// conservative sufficient condition; never inspect unused proxy counters.
	return vfKnownIdleSnapshot();
}

bool Gen11::vfIsKmdContextIdle(void *that, const uint32_t *descriptor) {
	if (gVfIdentity == VfIdentity::Physical)
		return FunctionCast(vfIsKmdContextIdle, callback->oVfIsKmdContextIdle)(that, descriptor);
	return descriptor && vfKnownIdleSnapshot(descriptor);
}

void Gen11::vfTransferOwnership(void *that, const void *backing, int owner) {
	if (gVfIdentity == VfIdentity::Physical) {
		FunctionCast(vfTransferOwnership, callback->oVfTransferOwnership)(that, backing, owner);
		return;
	}
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
	if (gVfIdentity == VfIdentity::Physical)
		return FunctionCast(vfLegacyHostToGuCAction,
		                    callback->oVfLegacyHostToGuCAction)(
			that, request, requestLength, timeout, response);
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
	if (gVfIdentity == VfIdentity::Physical) {
		return FunctionCast(vfMmioHostToGuCAction,
		                    callback->oVfMmioHostToGuCAction)(
			that, request, requestLength, timeout, response);
	}
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
	if (gVfIdentity == VfIdentity::Physical) {
		return FunctionCast(vfReadDoorbellSQIDIConfig,
		                    callback->oVfReadDoorbellSQIDIConfig)(that);
	}
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
	if (vfIdentifyDevice() == VfIdentity::Physical) {
		FunctionCast(vfInitDoorbells, callback->oVfInitDoorbells)(that);
		return;
	}

	// IntelAccelerator::start normally completed this before constructing the
	// scheduler.  Retrying here makes the ordering requirement explicit and
	// prevents a transiently-unready VF from falling through to DISTRDB.
	if (!gVfGGTTReady && !vfBootstrapBinder()) {
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
	if (gVfIdentity == VfIdentity::Physical) {
		return FunctionCast(vfAttachContextDesc,
		                    callback->oVfAttachContextDesc)(that, descriptor);
	}
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
	const uint32_t lrcaPage = descriptorLo & 0xFFFFF000U;
	const uint32_t rawClass = descriptorHi >> 29;
	const uint32_t engineInstance = (descriptorHi >> 16) & 0x3FU;
	static constexpr uint8_t engineClassMap[] = {0, 1, 2, 3, 5, 4};
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
	if (!poolValid || !contextBacking ||
	    !NGGgtt::contains(gVfGGTTBase, gVfGGTTSize, lrcaPage, contextBytes) ||
	    lrcaPage >= kGucGgttTop || contextBytes > kGucGgttTop - lrcaPage ||
	    rawClass >= arrsize(engineClassMap) || engineInstance >= 32 ||
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
				    entry.engineClass != engineClassMap[rawClass] ||
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
				entry.engineClass = engineClassMap[rawClass];
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
	const uint8_t engineClass = engineClassMap[rawClass];
	const uint32_t request[] = {
		kGucActionRegisterContext,
		kGucContextRegistrationFlagKmd,
		gucId,
		engineClass,
		1U << engineInstance,
		0, 0, // no work-queue descriptor for a single-LRC context
		0, 0, // no work queue
		0,
		descriptorLo,
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
	if (gVfIdentity == VfIdentity::Physical) {
		FunctionCast(vfDetachContextDesc,
		             callback->oVfDetachContextDesc)(that, descriptor);
		return;
	}
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
	if (gVfIdentity == VfIdentity::Physical) {
		return FunctionCast(vfSubmitWorkItem,
		                    callback->oVfSubmitWorkItem)(that, legacyContextId,
		                                                 descriptor, hwCsType,
		                                                 channelId, ringSequence,
		                                                 ringTail);
	}
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
	if (gVfIdentity == VfIdentity::Physical) {
		return FunctionCast(vfCtbInitWithAccelerator,
		                    callback->oVfCtbInitWithAccelerator)(that, accelerator);
	}
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
	if (gVfIdentity == VfIdentity::Physical) {
		FunctionCast(vfCtbChannelInit, callback->oVfCtbChannelInit)(that);
		return;
	}
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
	if (gVfIdentity == VfIdentity::Physical) {
		FunctionCast(vfInvalidateTLB, callback->oVfInvalidateTLB)(that);
		return;
	}
	(void)vfInvalidateTLBSync(that);
}

bool Gen11::vfCtbGucToHostAction(void *that, uint32_t *message) {
	if (gVfIdentity == VfIdentity::Physical)
		return FunctionCast(vfCtbGucToHostAction,
		                    callback->oVfCtbGucToHostAction)(that, message);
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
	if (!response && (hxg & kGucOriginGuc) != 0 &&
	    action == kGucActionTlbInvalidationDone && length >= 2) {
		const uint32_t seqno = message[2];
		if (gVfTlbWaitActive && seqno == gVfTlbWaitSeqno &&
		    OSCompareAndSwap(seqno - 1U, seqno, &gVfTlbDoneSeqno)) {
			vfReleaseG2HCredits(3);
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
		if (handled)
			vfReleaseG2HCredits(action == kGucActionScheduleContextModeDone ? 4U : 3U);
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
	if (gVfIdentity == VfIdentity::Physical) {
		return FunctionCast(vfInterruptFilterHandler,
		                    callback->oVfInterruptFilterHandler)(that,
		                                                          eventSource);
	}
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
	if (gVfIdentity == VfIdentity::Physical) {
		FunctionCast(vfSoftwareGuCInterrupt, callback->oVfSoftwareGuCInterrupt)(
			that, source, count);
		return;
	}
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
	if (gVfIdentity == VfIdentity::Physical) {
		FunctionCast(vfEnableInterrupts, callback->oVfEnableInterrupts)(that);
		return;
	}
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
	if (gVfIdentity == VfIdentity::Physical) {
		FunctionCast(vfDisableInterrupts, callback->oVfDisableInterrupts)(that);
		return;
	}
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
	if (!interrupts)
		return;
	if (gVfIdentity == VfIdentity::Physical) {
		FunctionCast(vfReadAndClearInterrupts,
		             callback->oVfReadAndClearInterrupts)(that, interrupts);
		return;
	}

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



uint32_t Gen11::wrapReadRegister32(void *controller, uint32_t address) {
	auto *green = NGreen::callback;
	if (!green)
		return 0xFFFFFFFFU;
	if (controller == nullptr)
		return green->readReg32(address);  // readReg32 now takes byte offsets

	// Mirror Apple bounds logic, but keep a fallback path for the 2D-only dropped window.
	auto partInfo = getMember<uint8_t *>(controller, 0xCF8);
	auto mmioBase = getMember<uint8_t *>(controller, 0x9B8);
	const int signedMmioSize = getMember<int>(controller, 0xC38);

	bool twoDOnlyPart = partInfo && ((partInfo[0xB2] & 0x1) != 0);
	bool dropped2DWindow = (address >= 0x2000U && address <= 0x23FFFFU);

	if (twoDOnlyPart && dropped2DWindow) {
		return green->readReg32(address);  // readReg32 now takes byte offsets
	}

	if (mmioBase && signedMmioSize >= 4 &&
	    address <= static_cast<uint32_t>(signedMmioSize) - 4U) {
		return *reinterpret_cast<volatile uint32_t *>(mmioBase + address);
	}

	if (callback && callback->owrapReadRegister32)
		return FunctionCast(wrapReadRegister32,
		                    callback->owrapReadRegister32)(controller, address);
	return green->readReg32(address);
}

void Gen11::wrapWriteRegister32(void *controller, uint32_t address, uint32_t value) {
	auto *green = NGreen::callback;
	if (!green)
		return;
	if (controller == nullptr) {
		green->writeReg32(address, value);
		return;
	}

	// V195W removed (was: OR-in bits 14,12 from UEFI's HSW_PWR_WELL_CTL1 onto every
	// 0x45400 write because the ICL DMC save/restore table allegedly clears them
	// periodically, supposedly stalling WindowServer's vsync). Pair-mate to V195 in
	// raWriteRegister32 + V195F in FastWriteRegister32 — all three sites stripped
	// together. The DMC save/restore behavior on Display 13 (ADL-P) differs from
	// Display 11 (ICL); enforcing UEFI's specific bits hides what Apple's stack
	// actually wants and what the ADL-P DMC table actually contains. If vsync stalls
	// re-emerge, fix should come from a correct DMC table or proper power-well init,
	// not bit OR-in.
	if (address == 0x45400 && !green->isRealTGL && green->uefiCtl1 != 0) {
		static int v195WpCount = 0;
		if (v195WpCount < 6) {
			++v195WpCount;
			SYSLOG("ngreen", "V195Wp[%d]: CTL1 0x%x passthrough uefiCtl1=0x%x (V195W hack removed)",
			       v195WpCount, value, green->uefiCtl1);
		}
	}

	// V72W removed (was: force RCS/BCS RING_EMR writes to 0xFFFFFFFF — same blanket
	// error-mask hack as V72R, on the wrapWriteRegister32 helper path). Pair-mate to
	// the V72R passthrough already in place. Keeping the address-match shell so a
	// future legitimate intercept can use this hook point.
	if (address == 0x20b4 || address == 0x220b4) {
		static int v72WPassCount = 0;
		if (v72WPassCount < 6) {
			++v72WPassCount;
			SYSLOG("ngreen", "V72Wp[%d]: EMR @ 0x%x val=0x%x passthrough (V72W hack removed)",
			       v72WPassCount, address, value);
		}
	}

	// ── V63: Broad RCS register write intercept (diagnostic only, no modification) ──
	// Catches ANY write to RCS control range: ELSP, EXECLIST, CTX_CTRL, CCID, TAIL, etc.
	// Rate-limited to first 100 writes to avoid flooding Lilu buffer.
	if (!green->isRealTGL) {
		static int v63WriteCount = 0;
		// RCS engine MMIO range: 0x2000-0x2FFF covers all ring control registers
		if (address >= 0x2000 && address <= 0x2FFF) {
			if (v63WriteCount < 100) {
				v63WriteCount++;
				SYSLOG("ngreen", "V63W[%d]: RCS reg 0x%x = 0x%x", v63WriteCount, address, value);
			} else if (v63WriteCount == 100) {
				v63WriteCount++;
				SYSLOG("ngreen", "V63W: rate limit reached (100 RCS writes logged)");
			}
		}
	}

	if (callback && callback->owrapWriteRegister32)
		FunctionCast(wrapWriteRegister32,
		             callback->owrapWriteRegister32)(controller, address, value);
	else
		green->writeReg32(address, value);
}

/**
 * Port of i915 force wake for Gen12 (TGL/ADL/RPL).
 * Replaces IntelAccelerator::SafeForceWakeMultithreaded.
 *
 * Differences from Apple's code:
 * 1. 50 ms ACK timeouts (Apple uses 90 ms) — https://patchwork.kernel.org/patch/7057561/
 * 2. Reserve-bit fallback on primary ACK timeout — https://patchwork.kernel.org/patch/10029821/
 * 3. Correct Gen9-style 3-domain iteration matching Apple's dom bitmask
 *    (dom: bit0=Render, bit1=Media, bit2=Blitter/GT)
 *
 * NOTE: The header's regForDom/ackForDom use Gen11+ domain bitmask IDs
 * (FORCEWAKE_RENDER=1=Render, FORCEWAKE_GT=2, FORCEWAKE_MEDIA=4) which does NOT
 * match Apple's 3-bit dom (1=Render, 2=Media, 4=Blitter). We use direct register
 * mapping here to match Apple's convention, same as WEG's ForceWakeWorkaround.
 */

// Map Apple's 3-bit domain to MMIO request register (Render + GT/Blitter same Gen9-Gen12)
static uint32_t fwReqReg(unsigned d) {
	if (d == DOM_RENDER)  return FORCEWAKE_RENDER_GEN9;   // 0xa278
	if (d == DOM_BLITTER) return FORCEWAKE_BLITTER_GEN9;  // 0xa188
	return 0;  // Media uses Gen11+ per-engine registers, handled separately
}

// Map Apple's 3-bit domain to MMIO ACK register (Render + GT/Blitter same Gen9-Gen12)
static uint32_t fwAckReg(unsigned d) {
	if (d == DOM_RENDER)  return FORCEWAKE_ACK_RENDER_GEN9;   // 0x0D84
	if (d == DOM_BLITTER) return FORCEWAKE_ACK_BLITTER_GEN9;  // 0x130044
	return 0;
}

// Gen12 Media ForceWake: per-engine register pairs (VDBOX + VEBOX)
struct FwMediaEngine {
	uint32_t req;
	uint32_t ack;
	const char *name;
};
static const FwMediaEngine fwMediaEngines[] = {
	{ FORCEWAKE_MEDIA_VDBOX_GEN11(0), FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(0), "VDBOX0" },  // 0xa540 / 0x0D50
	{ FORCEWAKE_MEDIA_VEBOX_GEN11(0), FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(0), "VEBOX0" },  // 0xa560 / 0x0D70
};

void Gen11::wrapSafeForceWake(void *that, bool set, uint32_t dom) {
	forceWake(that, set, dom, 1);  // forward to our forceWake with ctx=1 (normal, non-IRQ)
}

void Gen11::forceWake(void *that, bool set, uint32_t dom, uint8_t ctx) {
	// i915 does not create force-wake domains for a VF. Runtime engine power is
	// owned by the PF/GuC and these registers are intentionally inaccessible in
	// BAR0, so a successful no-op is the only valid guest-side behavior.
	if (gVfIdentity != VfIdentity::Physical) {
		// An unclassified/failed VF must not fall through to PF MMIO either.
		static bool loggedVfNoop = false;
		if (!loggedVfNoop) {
			loggedVfNoop = true;
			SYSLOG("ngreen", "V240: force-wake denied without physical GPU identity=%u",
			       static_cast<unsigned int>(gVfIdentity));
		}
		return;
	}

	// ctx 2: IRQ, ctx 1: normal
	uint32_t ack_exp = set << ctx;
	uint32_t mask = 1 << ctx;
	uint32_t wr = ack_exp | (1 << ctx << 16);

	for (unsigned d = DOM_FIRST; d <= DOM_LAST; d <<= 1) {
		if (!(dom & d)) continue;

		if (d == DOM_MEDIA) {
			// Gen12+: Media uses per-engine ForceWake (VDBOX + VEBOX), NOT Gen9 single register
			for (const auto &eng : fwMediaEngines) {
				wrapWriteRegister32(callback->framecont, eng.req, wr);
				IOPause(100);
				if (!pollRegister(eng.ack, ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS) &&
					!forceWakeWaitAckFallback(eng.req, eng.ack, ack_exp, mask) &&
					!pollRegister(eng.ack, ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS))
					SYSLOG("ngreen", "ForceWake timeout for %s (dom=0x%x), expected 0x%x", eng.name, dom, ack_exp);
				else
					DBGLOG("ngreen", "ForceWake OK %s set=%d", eng.name, set);
			}
		} else {
			wrapWriteRegister32(callback->framecont, fwReqReg(d), wr);
			IOPause(100);
			if (!pollRegister(fwAckReg(d), ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS) &&
				!forceWakeWaitAckFallback(fwReqReg(d), fwAckReg(d), ack_exp, mask) &&
				!pollRegister(fwAckReg(d), ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS))
				SYSLOG("ngreen", "ForceWake timeout for domain %s (dom=0x%x), expected 0x%x", strForDom(d), dom, ack_exp);
			else
				DBGLOG("ngreen", "ForceWake OK domain=%s set=%d ack=0x%x", strForDom(d), set, wrapReadRegister32(callback->framecont, fwAckReg(d)));
		}
	}
	// V61: silenced — see above
}

bool Gen11::pollRegister(uint32_t reg, uint32_t val, uint32_t mask, uint32_t timeout) {
    uint64_t now = 0, deadline = 0;
    clock_interval_to_deadline(timeout, kMillisecondScale, &deadline);
    for (clock_get_uptime(&now); now < deadline; clock_get_uptime(&now)) {
        auto rd = wrapReadRegister32(callback->framecont, reg);
        if ((rd & mask) == val)
            return true;
    }
    return false;
}

bool Gen11::forceWakeWaitAckFallback(uint32_t reqReg, uint32_t ackReg, uint32_t val, uint32_t mask) {
	unsigned pass = 1;
	bool ack = false;
	auto controller = callback->framecont;
	
	do {
		pollRegister(ackReg, 0, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_ACK_TIMEOUT_MS);
		wrapWriteRegister32(controller, reqReg, fw_set(FORCEWAKE_KERNEL_FALLBACK));
		
		IODelay(10 * pass);
		pollRegister(ackReg, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_ACK_TIMEOUT_MS);

		ack = (wrapReadRegister32(controller, ackReg) & mask) == val;

		wrapWriteRegister32(controller, reqReg, fw_clear(FORCEWAKE_KERNEL_FALLBACK));
	} while (!ack && pass++ < 10);
	
	return ack;
}

void Gen11::injectAcceleratorPersonality(bool useTglNames)
{
	if (this->acceleratorPersonalityInjected) {
		DBGLOG("ngreen", "injectAcceleratorPersonality: already injected, skipping");
		return;
	}

	SYSLOG("ngreen", "injectAcceleratorPersonality: registering IntelAccelerator (%s) into IOCatalogue",
	       useTglNames ? "TGL" : "ICL");

	auto *dict = OSDictionary::withCapacity(24);
	if (!dict) return;

	const char *bundleId = useTglNames ? "com.xxxxx.driver.AppleIntelTGLGraphics" : "com.apple.driver.AppleIntelICLGraphics";
	const char *mtlName  = useTglNames ? "AppleIntelTGLGraphicsMTLDriver"        : "AppleIntelICLGraphicsMTLDriver";
	const char *glName   = useTglNames ? "AppleIntelTGLGraphicsGLDriver"         : "AppleIntelICLGraphicsGLDriver";
	const char *vaName   = useTglNames ? "AppleIntelTGLGraphicsVADriver"         : "AppleIntelICLGraphicsVADriver";

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

void Gen11::hwConfigureCustomAUX(AppleIntel::AppleIntelBaseController *that, bool param_1)
{
	SYSLOG("ngreen", "hwAUX p1=%d CE4=%d",
		(int)param_1, getMember<int>(that, 0xCE4));

	// Pure passthrough — V12 showed that the native "Custom AUX enable" logic works
	// correctly on ADL-P hardware. The 0x863xx PHY writes added in V12 broke EDID
	// (56283 µs failure). Native-only: EDID succeeded in 3663 µs on same hardware.
	if (callback->ohwConfigureCustomAUX)
		FunctionCast(hwConfigureCustomAUX, callback->ohwConfigureCustomAUX)(that, param_1);
}

// V212: The GPU watchdog calls isGpuIdle() after engine init to decide if the GPU is healthy.
// On RPL-P, INSTDONE bit0 is permanently 0 → original returns false → watchdog resets the GPU
// → startGraphicsEngine retry loop. startGraphicsEngine itself SUCCEEDS (return value is non-zero
// = success path); the reset is triggered entirely by this watchdog query.
bool Gen11::wrapIGScheduler5IsGpuIdle(const void *that) {
	if (gVfIdentity == VfIdentity::Virtual)
		return vfKnownIdleSnapshot();
	if (gVfIdentity != VfIdentity::Physical)
		return false;
	return FunctionCast(wrapIGScheduler5IsGpuIdle,
	                    callback->oIGScheduler5IsGpuIdle)(that);
}

bool Gen11::wrapIGScheduler4IsGpuIdle(const void *that) {
	if (gVfIdentity == VfIdentity::Virtual)
		return vfKnownIdleSnapshot();
	if (gVfIdentity != VfIdentity::Physical)
		return false;
	return FunctionCast(wrapIGScheduler4IsGpuIdle,
	                    callback->oIGScheduler4IsGpuIdle)(that);
}

//SIGNATURES GFX
//ulong __thiscall IntelAccelerator::start(IntelAccelerator *this,IOService *param_1)
//undefined8 __thiscall IGAccelDevice::deviceStart(IGAccelDevice *this)
//void __thiscall IntelAccelerator::getGPUInfo(IntelAccelerator *this)
//void IntelAccelerator::getGPUInfo(void)
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
