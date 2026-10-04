//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_gen11.hpp"
#include <IOKit/IOTimerEventSource.h>
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
#include "kern_vf_mmio_response.hpp"
#include "kern_vf_legacy_ctb.hpp"
#include "kern_vf_tlb_patch.hpp"
#include "kern_vf_standalone_patch.hpp"
#include "kern_vf_guc_factory_patch.hpp"
#include "kern_vf_blit3d_scratch_patch.hpp"
#include "kern_unaligned_patch.hpp"
#include "kern_framebuffer_patch.hpp"
#include "kern_vf_guc_event.hpp"
#include "kern_vf_guc_request.hpp"
#include "kern_vf_memirq.hpp"
#include "kern_context_pool.hpp"
#include "kern_context_descriptor.hpp"
#include "kern_workqueue_unwind.hpp"
#include "kern_binary_identity.hpp"
#include "kern_ioaccel_command_pool.hpp"
#include "kern_event_vector.hpp"
#include <Headers/kern_api.hpp>
#include "kern_green.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
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

// Resolve the native accelerator lifecycle API from the running Tahoe
// IOAcceleratorFamily2 image.  The VF wrapper calls these named methods instead
// of writing IOGraphicsAccelerator2 private state at hardcoded offsets.
static const char *pathsIOAcceleratorFamily2[] = {
	"/System/Library/Extensions/IOAcceleratorFamily2.kext/Contents/MacOS/IOAcceleratorFamily2",
};
static KernelPatcher::KextInfo kextIOAcceleratorFamily2 {
	"com.apple.iokit.IOAcceleratorFamily2", pathsIOAcceleratorFamily2, 1,
	{true, false, false, false, true}, {},
	KernelPatcher::KextInfo::Unloaded
};

// Resolve the Tahoe PCI interrupt allocator from its owning fileset.  Do not
// search KernelPatcher::KernelID: IOPCIDevice lives in IOPCIFamily even when
// both images are linked into the same BootKC container.
static const char *pathsIOPCIFamily[] = {
	"/System/Library/Extensions/IOPCIFamily.kext/Contents/MacOS/IOPCIFamily",
};
static KernelPatcher::KextInfo kextIOPCIFamily {
	"com.apple.iokit.IOPCIFamily", pathsIOPCIFamily, 1,
	{true, false, false, false, true}, {},
	KernelPatcher::KextInfo::Unloaded
};

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
constexpr uint32_t kGucActionScheduleContext = NGVfGuCRequest::scheduleContext;
constexpr uint32_t kGucActionScheduleContextModeSet =
	NGVfGuCRequest::scheduleContextModeSet;
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
IORecursiveLock *gVfPageTableUpdateLock = nullptr;
uint64_t gVfGGTTBase = 0;
uint64_t gVfGGTTSize = 0;
uint32_t gVfContextCount = 0;
uint32_t gVfDoorbellCount = 0;
void *gVfGlobalPageTable = nullptr;
void *gVfHardwareGuc = nullptr;
void *gVfAccelerator = nullptr;
mach_vm_address_t gIOAccelCommandPoolInit = 0;
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
volatile UInt32 gVfSchedulerFirmwareReady = 0;
bool gVfUsesMemoryIrq = false;
bool gVfMemIrqConfigured = false;
volatile UInt32 gVfMemIrqRequested = 0;
volatile UInt32 gVfMmioIrqReady = 0;
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

static bool vfInterruptTransportReady()
{
	return gVfUsesMemoryIrq ?
		gVfMemIrqConfigured && gVfMemIrqRequested != 0 :
		gVfMmioIrqReady != 0;
}

static bool vfNativeGpuWorkReady()
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

static bool vfCtbConsumerReady(bool requireInterrupt)
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
static void vfAbortSchedulerBootstrap(const char *reason)
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
struct VfGucContext {
	uint32_t lrcaPage;
	uint32_t descriptorLo;
	uint32_t descriptorHi;
	uint16_t refCount;
	uint8_t engineClass;
	uint8_t engineInstance;
	VfGucContextState state;
	bool enablePending;
	bool disablePending;
	void *task;
	OSObject *contextBacking;
	OSObject *ringBacking;
	OSObject *stampBacking;
	OSObject *scratchBacking;
};

IOSimpleLock *gVfContextLock = nullptr;
VfGucContext *gVfContexts = nullptr;
uint32_t gVfContextCapacity = 0;
uint32_t gVfContextLifecycleLogs = 0;

constexpr uint32_t kVfContextEventTimeoutMs = 1000;
constexpr uint32_t kVfCtbBackpressureTimeoutMs = 1000;
constexpr size_t kVfContextDescriptorOffset = 0x89;
constexpr size_t kVfContextImageBufferOffset = 0x98;
constexpr size_t kVfContextRingObjectOffset = 0xB0;
constexpr size_t kVfRingMappedBufferOffset = 0x80;
constexpr size_t kVfRingSizeOffset = 0x8C;
constexpr size_t kVfRingMaskOffset = 0x90;
constexpr size_t kVfRingStampIndexOffset = 0x38;
constexpr size_t kVfContextTaskOffset = 0x58;
constexpr size_t kVfTaskScratchBufferOffset = 0x280;
constexpr size_t kVfTaskStampBufferOffset = 0x288;
// Tahoe IGMappedBuffer::initWithOptions stores the requested byte length at
// +0x20 (0x13c48); fillIfRequested consumes the same field as a byte bound.
constexpr size_t kVfMappedBufferLengthOffset = 0x20;
// Accelerator feature byte +0x1190 bit 5 selects Tahoe's legacy page-
// ownership and async-slice programming path. That path acquires physical
// force-wake and writes MMIO register 0xA204; an SR-IOV VF must reject it
// before native start rather than reaching a later ownership-route failure.
constexpr uint8_t kVfLegacyPageOwnershipFlag = 0x20U;
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
		if ((entry.state != kVfGucContextTombstone || entry.task || entry.contextBacking ||
		     entry.ringBacking || entry.stampBacking || entry.scratchBacking) &&
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
		    !gVfContexts[slot].stampBacking && !gVfContexts[slot].scratchBacking && tombstone < 0)
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
	    entry.refCount == 0) {
		backing = entry.contextBacking;
		ringBacking = entry.ringBacking;
		stampBacking = entry.stampBacking;
		scratchBacking = entry.scratchBacking;
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
bool vfEnsurePageTableUpdateLock();
bool vfValidH2GMapping(const volatile uint32_t *descriptor,
                       const volatile uint32_t *buffer);
bool vfValidG2HMapping(const volatile uint32_t *descriptor,
                       const volatile uint32_t *buffer);
bool vfInvalidateTLBSync(void *guc, NGVfGuCRequest::TlbTarget target);
bool vfQuiesceDeviceForShutdown(void *guc);
bool vfGucSendMMIO(const uint32_t request[4], uint32_t requestLength,
                   uint32_t response[4]);

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
	                     IOLock *alreadyHeldQueue = nullptr,
	                     volatile uint32_t *contextTailField = nullptr,
	                     uint32_t contextTail = 0)
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
		if (!Gen11::pollVfGuCToHost(guc))
			return false;
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
		for (uint32_t waited = 0; waited < kVfContextEventTimeoutMs; waited++) {
			OSSynchronizeIO();
			if (gVfTlbDoneSeqno == seqno) {
				completed = true;
				break;
			}
			if (!Gen11::pollVfGuCToHost(guc))
				break;
			IOSleep(1);
		}
	}
	gVfTlbWaitActive = 0;
	OSSynchronizeIO();
	IOLockUnlock(gVfGucLock);

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
// MTL/ARL's VF ABI requires a memory-interrupt page in GGTT. Reuse the final,
// page-aligned 4 KiB of the enlarged CTB allocation so CT transport and IRQ
// state share one mapping and no extra IGMappedBuffer allocation is needed.
// TGL/ADL/RPL use virtual interrupt MMIO and never publish or modify this page;
// retaining one allocation shape keeps the private Tahoe CTB ABI deterministic.
constexpr uint32_t kVfMemIrqOffset = 0x7000;
constexpr uint32_t kVfMemIrqBytes = 0x1000;
constexpr uint32_t kVfMemIrqStatusOffset = NGVfMemIrq::statusOffset;
constexpr uint32_t kVfMemIrqSourceOffset = NGVfMemIrq::sourceOffset;
constexpr uint32_t kVfMemIrqEnableOffset = NGVfMemIrq::enableOffset;
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
		    entry.refCount || entry.enablePending || entry.disablePending)
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
	const bool tglRequested = checkKernelArgument("-ngreentglfb") ||
		checkKernelArgument("-ngreentglwithgfx") ||
		checkKernelArgument("-ngreentglgfx");
	if (tglRequested) {
		lilu.onKextLoadForce(&kextIOAcceleratorFamily2);
		lilu.onKextLoadForce(&kextIOPCIFamily);
	}

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

static void *vfRejectPhysicalFramebufferProbe(void *, void *, int *) {
	return nullptr;
}

static bool vfRejectPhysicalFramebufferStart(void *, void *) {
	return false;
}

bool Gen11::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (index == kextIOPCIFamily.loadIndex) {
		this->ioPciConfigureInterrupts = patcher.solveSymbol(
			index, "__ZN11IOPCIDevice19configureInterruptsEjjjj",
			address, size);
		PANIC_COND(!this->ioPciConfigureInterrupts, "ngreen",
			"Cannot resolve the exported Tahoe PCI MSI configurator");
		SYSLOG("ngreen", "V246: resolved Tahoe IOPCIFamily PCI interrupt allocator");
		return true;
	}

	if (index == kextIOAcceleratorFamily2.loadIndex) {
		const auto *ioAccelImage = reinterpret_cast<const uint8_t *>(address);
		PANIC_COND(!NGBinaryIdentity::matchesKextUuid(
		               ioAccelImage, size,
		               NGBinaryIdentity::ioAcceleratorTahoe25G229Uuid),
		           "ngreen", "Unsupported Tahoe IOAcceleratorFamily2 private ABI");
		KernelPatcher::SolveRequest lifecycle[] = {
			{"__ZN22IOGraphicsAccelerator217enableAcceleratorEv",
			 this->ioGraphicsEnableAccelerator},
			{"__ZN22IOGraphicsAccelerator218disableAcceleratorEv",
			 this->ioGraphicsDisableAccelerator},
			{"__ZN24IOAccelEventMachineFast29initEventEP12IOAccelEvent",
			 this->ioAccelEventMachineInitEvent},
		};
		PANIC_COND(!patcher.solveMultiple(index, lifecycle, address, size),
			"ngreen", "Cannot resolve native IOAccelerator lifecycle API");
		mach_vm_address_t poolInitStart = 0, growthStart = 0, growthEnd = 0;
		mach_vm_address_t getterStart = 0, getterEnd = 0;
		KernelPatcher::SolveRequest commandPoolBounds[] = {
			{"__ZN25IOAccelCommandBufferPool24initEP22IOGraphicsAccelerator2P15IOAccelChannel2P11IOAccelTaskiijjjj",
			 poolInitStart},
			{"__ZN25IOAccelCommandBufferPool223allocMoreCommandBuffersEv", growthStart},
			{"__ZN25IOAccelCommandBufferPool24freeEv", growthEnd},
			{"__ZN25IOAccelCommandBufferPool217getBufferPtrNoIncEj", getterStart},
			{"__ZN25IOAccelCommandBufferPool212submitBufferEv", getterEnd},
		};
		PANIC_COND(!patcher.solveMultiple(index, commandPoolBounds, address, size) ||
		           growthStart <= poolInitStart ||
		           growthStart - poolInitStart != 0x182 ||
		           growthEnd <= growthStart ||
		           !NGIOAccelCommandPool::hasReviewedGrowthContract(
		               reinterpret_cast<const uint8_t *>(growthStart),
		               growthEnd - growthStart) ||
		           getterEnd <= getterStart ||
		           !NGIOAccelCommandPool::hasReviewedGetBufferContract(
		               reinterpret_cast<const uint8_t *>(getterStart),
		               getterEnd - getterStart),
		           "ngreen", "Changed IOAccelerator command-pool contract");
		KernelPatcher::RouteRequest commandPoolRoutes[] = {
			{"__ZN25IOAccelCommandBufferPool24initEP22IOGraphicsAccelerator2P15IOAccelChannel2P11IOAccelTaskiijjjj",
			 ngVfCommandPoolInitBridge, gIOAccelCommandPoolInit},
			{"__ZN25IOAccelCommandBufferPool223allocMoreCommandBuffersEv",
			 vfAllocMoreCommandBuffers, this->oIOAccelAllocMoreCommandBuffers},
			{"__ZN25IOAccelCommandBufferPool217getBufferPtrNoIncEj",
			 vfGetCommandBufferPtrNoInc, this->oIOAccelGetCommandBufferPtrNoInc},
		};
		PANIC_COND(!patcher.routeMultiple(index, commandPoolRoutes, address, size),
		           "ngreen", "Cannot route VF command-pool postconditions");
		SYSLOG("ngreen", "V272: guarded VF command-pool growth and returned capacity");
		return true;
	}

	const bool physicalFramebuffer = index == kextG11FBT.loadIndex ||
		index == kextG11FBTA.loadIndex;
	const auto *image = reinterpret_cast<const uint8_t *>(address);
	const bool framebufferProduction = physicalFramebuffer &&
		NGBinaryIdentity::matchesKextUuid(
			image, size, NGBinaryIdentity::tglFramebufferProductionUuid);
	const bool framebufferDebug = physicalFramebuffer &&
		NGBinaryIdentity::matchesKextUuid(
			image, size, NGBinaryIdentity::tglFramebufferDebugUuid);
	PANIC_COND(physicalFramebuffer && !framebufferProduction && !framebufferDebug,
		"ngreen", "Unsupported TGL framebuffer payload ABI; refusing private-layout routes");
	if (physicalFramebuffer &&
	    (!NGreen::callback || !NGreen::callback->isRealTGL)) {
		// A VF has no physical display controller, and a later-generation PF is
		// not permission to run a UUID-pinned TGL display ABI against different
		// registers. Refusing only DMC or MMIO
		// helpers is too late: native probe/start have their own raw accesses. The
		// production payload overrides probe in AppleIntelFramebufferController,
		// while the debug payload inherits its AppleIntelBaseController override;
		// install the UUID-specific entry instead of requiring a symbol absent from
		// the production binary. This is admission rejection, not a virtual
		// framebuffer implementation.
		if (framebufferProduction) {
			KernelPatcher::RouteRequest reject[] = {
				{"__ZN31AppleIntelFramebufferController5probeEP9IOServicePi", vfRejectPhysicalFramebufferProbe},
				{"__ZN31AppleIntelFramebufferController5startEP9IOService", vfRejectPhysicalFramebufferStart},
			};
			PANIC_COND(!patcher.routeMultiple(index, reject, address, size),
				"ngreen", "Cannot contain production TGL framebuffer on unsupported GPU");
		} else {
			KernelPatcher::RouteRequest reject[] = {
				{"__ZN24AppleIntelBaseController5probeEP9IOServicePi", vfRejectPhysicalFramebufferProbe},
				{"__ZN31AppleIntelFramebufferController5startEP9IOService", vfRejectPhysicalFramebufferStart},
			};
			PANIC_COND(!patcher.routeMultiple(index, reject, address, size),
				"ngreen", "Cannot contain debug TGL framebuffer on unsupported GPU");
		}
		SYSLOG("ngreen", "TGL framebuffer probe/start rejected for VF/non-TGL/unknown device");
		return true;
	}
	if (kextG11FBT.loadIndex == index || kextG11FBTA.loadIndex == index) {
		auto *activeKext = (kextG11FBTA.loadIndex == index) ? &kextG11FBTA : &kextG11FBT;
		PANIC_COND(!NGreen::callback->setRMMIOIfNecessary(), "ngreen", "Cannot map TGL framebuffer BAR0");
		SYSLOG("ngreen", "init AppleIntelTGLGraphicsFramebuffer");
		
		// Both admitted variants checked a 64-bit read against length-4.
		// Require the full eight-byte operand before dereferencing it.
		mach_vm_address_t read64Start = 0, read64End = 0;
		KernelPatcher::SolveRequest read64SymbolBounds[] = {
			{"__ZN31AppleIntelRegisterAccessManager14ReadRegister64Em",
			 read64Start},
			{"__ZN31AppleIntelRegisterAccessManager14ReadRegister64EPVvm",
			 read64End},
		};
		PANIC_COND(!patcher.solveMultiple(
		               index, read64SymbolBounds, address, size) ||
		           read64End <= read64Start || read64End - read64Start > 0x100,
		           "ngreen", "Invalid ReadRegister64 patch bounds");
		LookupPatchPlus const read64Bounds = framebufferProduction ?
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
		const uint32_t gpuDevice = NGreen::callback->getOriginalDeviceId();
		gVfUsesMemoryIrq = vfActive &&
			NGGpuCapabilities::hasIovMemoryIrq(gpuDevice);
		const bool tglGeneration = NGGpuCapabilities::isTigerLake(gpuDevice);
		PANIC_COND(!NGGpuCapabilities::supportsPinnedTigerLakePayload(
			gpuDevice, vfActive), "ngreen",
			"Refusing TGL native accelerator payload on a non-TGL physical function");
		if (vfActive) {
			SYSLOG("ngreen", "V251: VF interrupt transport=%s device=%04x",
			       gVfUsesMemoryIrq ? "memory" : "Gen11 virtual MMIO",
			       gpuDevice);
		}
		// The payload is patched before its personality is published. Complete the
		// one-shot VF bootstrap here so every embedded fuse immediate comes from
		// the same PF snapshot that owns this VF's GuC/GGTT assignment.
		PANIC_COND(vfActive && !vfBootstrapDirectGgtt(), "ngreen",
			"Cannot bootstrap VF runtime/GGTT before accelerator publication");
		SYSLOG("ngreen", "V165: setRMMIO done, starting symbol resolve");

		if (vfActive) {
			PANIC_COND(!this->ioPciConfigureInterrupts, "ngreen",
				"Cannot resolve the exported Tahoe PCI MSI configurator");
			// Tahoe's telemetry manager and per-stamp usage objects are created
			// before startGraphicsEngine(). TelemetryDisable only gates the trace
			// stream and context-image patchers; native manager initialization,
			// IOReport, sysctl and user-client OA paths still access PF-owned MMIO.
			// Keep Apple's object/lifetime shape, but make every externally reachable
			// hardware telemetry edge fail closed for a VF.
			KernelPatcher::RouteRequest telemetryRoutes[] = {
				{"__ZN18IGTelemetryManager14printDashboardEy",
				 vfTelemetryPrintDashboard},
				{"__ZN18IGTelemetryManager19initWithAcceleratorEP16IntelAcceleratorj",
				 vfTelemetryInitWithAccelerator},
				{"__ZN18IGTelemetryManager15telemetryRetainEv",
				 vfTelemetryRetain},
				{"__ZN18IGTelemetryManager16telemetryReleaseEv",
				 vfTelemetryRelease},
				{"__ZN18IGTelemetryManager15calcGlobalUsageEy",
				 vfTelemetryCalcGlobalUsage},
				{"__ZN18IGTelemetryManager9operationEyxP18TelemetryOperationP19TelemetryConnectionP4task",
				 vfTelemetryOperation},
				{"__ZN18IGTelemetryManager20patchContextImageRCSEP27SGfxHardwareContextImageRCS",
				 vfTelemetryPatchContextImage},
				{"__ZN18IGTelemetryManager16onConnectionStopER19TelemetryConnectionP4task",
				 vfTelemetryOnConnectionStop},
				{"__ZN18IGTelemetryManager21telemetryInitOaBufferER19TelemetryConnectionP19MDAPIInitOABufferOpS3_yPy",
				 vfTelemetryInitOaBuffer},
				{"__ZN18IGTelemetryManager21telemetryReadOaBufferEP21MDAPIReadOABufferOpInP22MDAPIReadOABufferOpOutPyR19TelemetryConnection",
				 vfTelemetryReadOaBuffer},
				{"__ZN18IGTelemetryManager26telemetryMapOaBufferMemoryEP27IntelDeviceMapStatsMemInOutS1_yPyP4task",
				 vfTelemetryMapOaBufferMemory},
				{"__ZN16IGTelemetryUsage13allocUsageMemEv",
				 vfTelemetryUsageAlloc},
				{"__ZN16IGTelemetryUsage8logUsageEP17IGHardwareContexty",
				 vfTelemetryUsageLog},
				{"__ZN16IGTelemetryUsage17reportGlobalUsageEv",
				 vfTelemetryUsageReportGlobal},
				{"__ZN16IGTelemetryUsage11startSampleEP20IGHardwareRingBufferjjyyyy",
				 vfTelemetryUsageStartSample},
				{"__ZN16IGTelemetryUsage10stopSampleEP20IGHardwareRingBufferjj",
				 vfTelemetryUsageStopSample},
				{"__ZN16IGTelemetryUsage16frameCalcGPUBusyEP16IntelAcceleratorj",
				 vfTelemetryUsageFrameCalc},
				// TelemetryDisable gates trace bodies, but native initSysctl has
				// already registered 55 writable debug OIDs and published this
				// accelerator to the global idvar control plane before checking it.
				// Suppress registration and its paired teardown together on a VF.
				{"__ZN16IntelAccelerator10initSysctlEv",
				 vfDisableDebugSysctl},
				{"__ZN16IntelAccelerator16unregisterSysctlEv",
				 vfDisableDebugSysctl},
			};
			PANIC_COND(!patcher.routeMultiple(
			               index, telemetryRoutes, address, size), "ngreen",
			           "Failed to isolate VF telemetry and OA hardware paths");
			SYSLOG("ngreen", "V260: disabled PF-owned telemetry/OA and debug-sysctl paths for VF");
			KernelPatcher::SolveRequest solveRequests[] = {
				{"__ZN13IGHardwareGuC16initSchedControlEv", this->orgInitSchedControl},
				{"__ZN11IGScheduler12initFirmwareEv", this->vfSchedulerInitFirmware},
				{"__ZN21IGHardwareGuCCTBuffer32handleSoftwareGuCToHostInterruptEv",
				 this->vfCtbSoftwareInterrupt},
				// V233: single-LRC GuC submission must publish the new tail in
				// the context image before scheduling it.  Resolve the accessor
				// used by IGHardwareContext::updateRingTail so the bridge can do
				// the same update without touching physical memory.
				{"__ZNK20IGSharedMappedBuffer17getVirtualAddressEv",
				 this->vfSharedMappedBufferGetVirtualAddress},
				// Resolve the callback dispatcher used by the MTL/ARL memory-IRQ
				// replacement. TGL/ADL/RPL retain the native virtual-MMIO filter.
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
			PANIC_COND(!patcher.solveMultiple(index, solveRequests, address, size), "ngreen",
			           "Failed to resolve mandatory TGL accelerator bootstrap symbols");
		}

		// V229: apply this byte patch before routing readDoorbellSQIDIConfig.
		// routeFunction overwrites the function entry and therefore overlaps the
		// exact sequence below (which starts at entry + 4).  Patching afterwards
		// made V228 panic before the driver could start.  Calls emitted as local
		// rel32 targets now remain safe even when they bypass the routed entry.
		// 0x001f00ff encodes SQIDI mask 0xff and 32 doorbells per SQIDI.
		if (vfActive) {
			KernelPatcher::SolveRequest bufferAccessors[] = {
				{"__ZNK20IGSharedMappedBuffer17getVirtualAddressEv",
				 this->vfSharedMappedBufferGetVirtualAddress},
				{"__ZNK14IGMappedBuffer20getGPUVirtualAddressEv",
				 this->vfMappedBufferGetGPUVirtualAddress},
				{"__ZNK14IGMappedBuffer9getMemoryEv",
				 this->oIGMappedBuffergetMemory},
				// getMemory() returns the private IGAccelMemory owner, not an
				// IOMemoryDescriptor.  The exact system-memory subclass method
				// applies the same IOMapper options as Tahoe's GGTT commit path.
				{"__ZN16IGAccelSysMemory18getPhysicalSegmentEyPy",
				 this->vfAccelSysMemoryGetPhysicalSegment},
				{"__ZN13IGHardwareGuC12allocContextEyb",
				 this->vfAllocContext},
				{"__ZN13IGHardwareGuC14releaseContextEj",
				 this->vfReleaseContext},
				{"__ZN20IGSharedMappedBuffer11withOptionsEP11IGAccelTaskmjj",
				 this->vfSharedMappedBufferWithOptions},
				{"__ZN22IGHardwareGuCWorkQueue11withOptionsEP22IOGraphicsAccelerator2jP37UK_GEN11_SCHED_PROCESS_DESCRIPTOR_REC",
				 this->vfWorkQueueWithOptions},
			};
			PANIC_COND(!patcher.solveMultiple(index, bufferAccessors, address, size),
			           "ngreen", "Cannot resolve VF buffer accessors or proxy-context lifecycle");
			this->vfOSObjectFree = patcher.solveSymbol(KernelPatcher::KernelID,
				"__ZN8OSObject4freeEv");
			PANIC_COND(!this->vfOSObjectFree, "ngreen",
			           "Cannot resolve base destructor for failed VF workqueues");

			// Pool init returns bool, but this UUID-pinned constructor discarded it
			// before backing allocation and base setup. The routed System-KC bridge
			// clears cleanup-sensitive record state, preserves AL in ZF and restores
			// the task in RDI; this same-size patch branches false to the constructor's
			// existing epilogue without a code cave.
			mach_vm_address_t extendedInit = 0, extendedFree = 0;
			KernelPatcher::SolveRequest extendedBounds[] = {
				{"__ZN25IGHardwareExtendedContext15initWithOptionsEP11IGAccelTaskRK31IGHardwareExtendedContextParams",
				 extendedInit},
				{"__ZN25IGHardwareExtendedContext4freeEv", extendedFree},
			};
			PANIC_COND(!patcher.solveMultiple(index, extendedBounds, address, size) ||
			           extendedFree <= extendedInit ||
			           !NGIOAccelCommandPool::hasReviewedExtendedInitContract(
			               reinterpret_cast<const uint8_t *>(extendedInit),
			               extendedFree - extendedInit),
			           "ngreen", "Changed VF extended-context pool-init contract");
			LookupPatchPlus const extendedInitPatch {
				activeKext, NGIOAccelCommandPool::extendedInitFind,
				NGIOAccelCommandPool::extendedInitReplace, 1,
			};
			PANIC_COND(!extendedInitPatch.apply(
			               patcher, extendedInit, extendedFree - extendedInit),
			           "ngreen", "Failed to propagate VF command-pool init failure");
			SYSLOG("ngreen", "V269: guarded VF extended-context pool construction");

			// The rect-list request is 64-byte aligned, but native admitted exactly
			// 64 KiB while this pool reserves its final eight bytes. Bound only this
			// producer to the configured usable capacity; PF remains byte-identical.
			mach_vm_address_t rectListCapacityStart = 0, rectListCapacityEnd = 0;
			KernelPatcher::SolveRequest rectListCapacityBounds[] = {
				{"__ZL22blit3d_submit_rectlistP23IGHardwareBlit3DContextP15blit3d_params_tPK8IGVectorI11rect_pair_t25IGIOMallocAllocatorPolicyE",
				 rectListCapacityStart},
				{"__ZL19IsSurfaceCompressedj", rectListCapacityEnd},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, rectListCapacityBounds, address, size) ||
			           rectListCapacityEnd <= rectListCapacityStart ||
			           !NGIOAccelCommandPool::hasReviewedRectListCapacity(
			               reinterpret_cast<const uint8_t *>(rectListCapacityStart),
			               rectListCapacityEnd - rectListCapacityStart),
			           "ngreen", "Changed VF rect-list command-pool capacity contract");
			LookupPatchPlus const rectListCapacityPatch {
				activeKext, NGIOAccelCommandPool::rectListCapacityFind,
				NGIOAccelCommandPool::rectListCapacityReplace, 1,
			};
			PANIC_COND(!rectListCapacityPatch.apply(
			               patcher, rectListCapacityStart,
			               rectListCapacityEnd - rectListCapacityStart),
			           "ngreen", "Failed to reserve the VF rect-list pool tail");
			SYSLOG("ngreen", "V270: bounded VF rect-list command requests to 0xfff8 bytes");

			// HIZ assembly uses a 4 KiB window relative to the pointer returned by
			// getBufferPtrNoInc(). Its native dynamic request always fit the current
			// tail and therefore failed to rotate the 0x9b8-byte initialization prefix.
			// Require one complete configured usable region so the native getter
			// submits a partial slot before any HIZ command is written.
			mach_vm_address_t resolveHizStart = 0, resolveHizEnd = 0;
			KernelPatcher::SolveRequest resolveHizBounds[] = {
				{"__Z14resolve_hiz_g7P25IOAccelCommandBufferPool2P14IGMappedBufferP22depth_resolve_params_tR15resolve_phase_tRjS7_yb",
				 resolveHizStart},
				{"__ZL25hiz_first_instr_optimizedP24g8_hiz_resolve_cmd_buf_tPK22depth_resolve_params_tbjjjjR22SGfx3dStateDepthBufferR26SGfx3dStateHierDepthBuffery",
				 resolveHizEnd},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, resolveHizBounds, address, size) ||
			           resolveHizEnd <= resolveHizStart ||
			           !NGIOAccelCommandPool::hasReviewedResolveHizCapacity(
			               reinterpret_cast<const uint8_t *>(resolveHizStart),
			               resolveHizEnd - resolveHizStart),
			           "ngreen", "Changed VF resolve-HIZ command-pool capacity contract");
			LookupPatchPlus const resolveHizCapacityPatch {
				activeKext, NGIOAccelCommandPool::resolveHizCapacityFind,
				NGIOAccelCommandPool::resolveHizCapacityReplace, 1,
			};
			PANIC_COND(!resolveHizCapacityPatch.apply(
			               patcher, resolveHizStart, resolveHizEnd - resolveHizStart),
			           "ngreen", "Failed to require a fresh VF resolve-HIZ command slot");
			SYSLOG("ngreen", "V271: require full usable VF resolve-HIZ command capacity");

			// Tahoe's GuC factory releases the object after initWithOptions()
			// already called virtual free() on the same failure path. XNU's
			// OSObject::free() deletes the instance, so the second virtual dispatch
			// dereferences freed storage. Keep the native cleanup and null return,
			// but remove only that second dispatch in the UUID-pinned factory.
			mach_vm_address_t gucFactory = 0, gucInit = 0;
			KernelPatcher::SolveRequest gucFactoryBounds[] = {
				{"__ZN13IGHardwareGuC11withOptionsEP16IntelAccelerator",
				 gucFactory},
				{"__ZN13IGHardwareGuC15initWithOptionsEP16IntelAccelerator",
				 gucInit},
			};
			PANIC_COND(!patcher.solveMultiple(index, gucFactoryBounds, address, size) ||
			           gucInit <= gucFactory || gucInit - gucFactory > 0x100,
			           "ngreen", "Invalid VF GuC factory patch bounds");
			LookupPatchPlus const gucFactoryPatch {
				activeKext, NGVfGuCFactoryPatch::releaseAfterFailedInitFind,
				NGVfGuCFactoryPatch::releaseAfterFailedInitReplace, 1,
			};
			PANIC_COND(!gucFactoryPatch.apply(
			               patcher, gucFactory, gucInit - gucFactory), "ngreen",
			           "Failed to remove VF GuC factory double destruction");
			mach_vm_address_t schedulerInit = 0, schedulerWait = 0;
			// Pools here are newly initialized and have no allocated GPU pages.
			// Preserve native release/free cleanup, but visit only the created
			// prefix instead of walking forward past the failed factory slot.
			mach_vm_address_t poolInit = 0, managerFree = 0;
			KernelPatcher::SolveRequest poolBounds[] = {
				{"__ZN15IGMemoryManager12initPagePoolEv", poolInit},
				{"__ZN15IGMemoryManager4freeEv", managerFree},
			};
			PANIC_COND(!patcher.solveMultiple(index, poolBounds, address, size) ||
			           managerFree <= poolInit || managerFree - poolInit != 0xcc,
			           "ngreen", "Invalid VF page-pool init patch bounds");
			PANIC_COND(!NGVfPagePoolPatch::prefixUnwindPreflight(
			               reinterpret_cast<const uint8_t *>(poolInit), managerFree - poolInit),
			           "ngreen", "Changed or ambiguous VF page-pool unwind");
			LookupPatchPlus const poolUnwindPatch {
				activeKext, NGVfPagePoolPatch::prefixUnwindFind,
				NGVfPagePoolPatch::prefixUnwindReplace, 1,
			};
			PANIC_COND(!poolUnwindPatch.apply(patcher, poolInit, managerFree - poolInit),
			           "ngreen", "Failed to repair VF page-pool prefix unwind");
			KernelPatcher::SolveRequest schedulerBounds[] = {
				{"__ZN12IGScheduler419initWithAcceleratorEP22IOGraphicsAccelerator2", schedulerInit},
				{"__ZN12IGScheduler414waitForGpuIdleEv", schedulerWait},
			};
			PANIC_COND(!patcher.solveMultiple(index, schedulerBounds, address, size) ||
			           schedulerWait <= schedulerInit || schedulerWait - schedulerInit != 0xc2,
			           "ngreen", "Invalid VF scheduler failed-init patch bounds");
			PANIC_COND(!NGVfGuCFactoryPatch::schedulerInitPreflight(
			               reinterpret_cast<const uint8_t *>(schedulerInit), 0xaf),
			           "ngreen", "Ambiguous or changed VF scheduler failed-init patch site");
			LookupPatchPlus const schedulerInitPatch {
				activeKext, NGVfGuCFactoryPatch::schedulerInitFreeFind,
				NGVfGuCFactoryPatch::schedulerInitFreeReplace, 1,
			};
			PANIC_COND(!schedulerInitPatch.apply(patcher, schedulerInit, 0xaf),
			           "ngreen", "Failed to preserve single-owner VF scheduler init cleanup");
			// Metal's first Blit3D context fills scratch blend states through
			// offset 0xd20f, but a VF-only shared-buffer mapping stops at
			// 0xd000. Keep the native initializer and publish a page-rounded
			// allocation size instead; the private factory only reads the
			// recorded length back from this same global. Bound the rewrite by
			// stable exported neighbours enclosing the private global constructor
			// and require the exact UUID-pinned anchor, including its const-table
			// RIP target.
			mach_vm_address_t blit3dBoundsStart = 0, blit3dBoundsEnd = 0;
			KernelPatcher::SolveRequest blit3dScratchBounds[] = {
				{"__ZN23IGHardwareBlit2DContext10initializeEv", blit3dBoundsStart},
				{"__ZN21IGAccelDisplayMachine9MetaClassC1Ev", blit3dBoundsEnd},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, blit3dScratchBounds, address, size) ||
			           blit3dBoundsEnd <= blit3dBoundsStart ||
			           blit3dBoundsEnd - blit3dBoundsStart > 0x400,
			           "ngreen", "Invalid Blit3D scratch patch bounds");
			LookupPatchPlus const blit3dScratchPatch {
				activeKext, NGVfBlit3dScratchPatch::scratchSizeFind,
				NGVfBlit3dScratchPatch::scratchSizeReplace, 1,
			};
			PANIC_COND(!blit3dScratchPatch.apply(
			               patcher, blit3dBoundsStart,
			               blit3dBoundsEnd - blit3dBoundsStart), "ngreen",
			           "Failed to enlarge Blit3D scratch allocation");
			SYSLOG("ngreen", "V250: enlarged Blit3D scratch allocation 0xd240→0xe000");
			mach_vm_address_t ccsStart = 0, ccsEnd = 0;
			KernelPatcher::SolveRequest ccsBounds[] = {
				{"__ZN15IGAccelResource16submitCCSResolveEPNS_17ResourceInfoEntryER16IntelAcceleratorP11IGAccelTask20EIntelCCSResolveTypehh", ccsStart},
				{"__ZN15IGAccelResource24disableRenderCompressionEPNS_17ResourceInfoEntryE", ccsEnd},
			};
			PANIC_COND(!patcher.solveMultiple(index, ccsBounds, address, size) ||
			           ccsEnd <= ccsStart || ccsEnd - ccsStart != 0x554,
			           "ngreen", "Invalid CCS allocation repair bounds");
			PANIC_COND(!NGVfBlit3dScratchPatch::ccsAllocationPreflight(
			               reinterpret_cast<const uint8_t *>(ccsStart), ccsEnd - ccsStart),
			           "ngreen", "Changed or ambiguous CCS allocation repair anchors");
			LookupPatchPlus const ccsAllocationPatch {
				activeKext, NGVfBlit3dScratchPatch::ccsAllocationFind,
				NGVfBlit3dScratchPatch::ccsAllocationReplace, 1,
			};
			PANIC_COND(!ccsAllocationPatch.apply(patcher, ccsStart, ccsEnd - ccsStart),
			           "ngreen", "Failed to preserve CCS allocation-failure cleanup");

			// All eight event-pointer vector grow copies return false when no growth
			// was needed as well as on allocation failure. Lilu cannot range-select
			// duplicate symbols, so derive each exact target from unique owner anchors
			// in the admitted payload and validate every body before the first route.
			mach_vm_address_t twoDEventOwnerStart = 0, twoDEventOwnerEnd = 0;
			mach_vm_address_t acceleratorEventOwnerStart = 0, acceleratorEventOwnerEnd = 0;
			mach_vm_address_t renderEventOwnerStart = 0, renderEventOwnerEnd = 0;
			mach_vm_address_t blitEventOwnerStart = 0, blitEventOwnerEnd = 0;
			mach_vm_address_t glEventOwnerStart = 0, glEventOwnerEnd = 0;
			mach_vm_address_t resourceEventOwnerStart = 0, resourceEventOwnerEnd = 0;
			mach_vm_address_t sharedEventOwnerStart = 0, sharedEventOwnerEnd = 0;
			mach_vm_address_t surfaceEventOwnerStart = 0, surfaceEventOwnerEnd = 0;
			KernelPatcher::SolveRequest eventOwnerBounds[] = {
				{"__ZN16IGAccel2DContext8blitCopyEP12IOAccelEventP16IOAccelResource2S3_P22IOAccel2DBlitRectStrucj", twoDEventOwnerStart},
				{"__GLOBAL__sub_I_IGAccel2DContext.cpp", twoDEventOwnerEnd},
				{"__ZN19IGBlockFenceManagerD0Ev", acceleratorEventOwnerStart},
				{"__ZL18kDisplayVar_sysctlP10sysctl_oidPviP10sysctl_req", acceleratorEventOwnerEnd},
				{"__Z25process_MSAABufferResolveR26IGAccelSegmentResourceListR24IGAccelCommandDescriptorR22IOGraphicsAccelerator2RKN14IntelMTLRender20sResolveResourceDescES8_NS5_14eResolveFilterE10BufferTypeR12IOAccelEvent", renderEventOwnerStart},
				{"__Z40surfaceStateFillMemoryObjectControlStateR22SGfxRenderSurfaceStateP15IGAccelResourceP15IGMemoryManager", renderEventOwnerEnd},
				{"__ZN21IntelMTLBlitFunctions7executeEN12IntelMTLBlit7eTokensER19IGAccelCommandQueueR26IGAccelSegmentResourceListRK20IOAccelKernelCommandR24IGAccelCommandDescriptorR13IGHeapsAccessR22IOGraphicsAccelerator2R17IGHardwareContextR12IOAccelEvent", blitEventOwnerStart},
				{"__ZN14IGTelemetryKMD4initEP16IntelAcceleratory", blitEventOwnerEnd},
				{"__ZN16IGAccelGLContext32process_token_ResolveDepthBufferER24IOAccelCommandStreamInfo", glEventOwnerStart},
				{"__GLOBAL__sub_I_IGAccelGLContext.cpp", glEventOwnerEnd},
				{"__ZN15IGAccelResource22updateMappingCacheTypeEj", resourceEventOwnerStart},
				{"__GLOBAL__sub_I_IGAccelResource.cpp", resourceEventOwnerEnd},
				{"__ZN23IGAccelSharedUserClient9MetaClassD0Ev", sharedEventOwnerStart},
				{"__GLOBAL__sub_I_IGAccelSharedUserClient.cpp", sharedEventOwnerEnd},
				{"__ZN14IGAccelSurface9MetaClassD0Ev", surfaceEventOwnerStart},
				{"__GLOBAL__sub_I_IGAccelSurface.cpp", surfaceEventOwnerEnd},
			};
			PANIC_COND(!patcher.solveMultiple(index, eventOwnerBounds, address, size),
			           "ngreen", "Invalid VF event-vector owner bounds");
			const mach_vm_address_t twoDEventGrow = NGEventVector::locateReviewedGrow(
				twoDEventOwnerStart, twoDEventOwnerEnd, 0xA74);
			const mach_vm_address_t acceleratorEventGrow = NGEventVector::locateReviewedGrow(
				acceleratorEventOwnerStart, acceleratorEventOwnerEnd, 0x244);
			const mach_vm_address_t renderEventGrow = NGEventVector::locateReviewedGrow(
				renderEventOwnerStart, renderEventOwnerEnd, 0x764);
			const mach_vm_address_t blitEventGrow = NGEventVector::locateReviewedGrow(
				blitEventOwnerStart, blitEventOwnerEnd, 0x16C8);
			const mach_vm_address_t glEventGrow = NGEventVector::locateReviewedGrow(
				glEventOwnerStart, glEventOwnerEnd, 0x4AC8);
			const mach_vm_address_t resourceEventGrow = NGEventVector::locateReviewedGrow(
				resourceEventOwnerStart, resourceEventOwnerEnd, 0x60C);
			const mach_vm_address_t sharedEventGrow = NGEventVector::locateReviewedGrow(
				sharedEventOwnerStart, sharedEventOwnerEnd, 0xA);
			const mach_vm_address_t surfaceEventGrow = NGEventVector::locateReviewedGrow(
				surfaceEventOwnerStart, surfaceEventOwnerEnd, 0xA);
			PANIC_COND(!twoDEventGrow || !acceleratorEventGrow || !renderEventGrow ||
			           !blitEventGrow || !glEventGrow || !resourceEventGrow ||
			           !sharedEventGrow || !surfaceEventGrow,
			           "ngreen", "Changed VF event-vector growth contracts");
			ExactRouteRequest eventGrowRoutes[] = {
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 twoDEventGrow, vf2DEventVectorGrow, this->oVf2DEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 acceleratorEventGrow, vfAcceleratorEventVectorGrow,
				 this->oVfAcceleratorEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 renderEventGrow, vfRenderEventVectorGrow, this->oVfRenderEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 blitEventGrow, vfBlitEventVectorGrow, this->oVfBlitEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 glEventGrow, vfGLEventVectorGrow, this->oVfGLEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 resourceEventGrow, vfResourceEventVectorGrow,
				 this->oVfResourceEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 sharedEventGrow, vfSharedEventVectorGrow, this->oVfSharedEventVectorGrow},
				{"__ZN8IGVectorIP12IOAccelEvent25IGIOMallocAllocatorPolicyE4growEm",
				 surfaceEventGrow, vfSurfaceEventVectorGrow, this->oVfSurfaceEventVectorGrow},
			};
			PANIC_COND(!routeExactMultiple(patcher, eventGrowRoutes),
			           "ngreen", "Failed to guard VF event-vector growth");
			SYSLOG("ngreen", "V274: guarded all eight VF event-vector growth copies");
			KernelPatcher::RouteRequest workQueueInitRoute[] = {
				{"__ZN22IGHardwareGuCWorkQueue19initWithAcceleratorEP22IOGraphicsAccelerator2jP37UK_GEN11_SCHED_PROCESS_DESCRIPTOR_REC",
				 vfWorkQueueInit, this->oVfWorkQueueInit},
				{"__ZN22IGHardwareGuCWorkQueue4freeEv",
				 vfWorkQueueFree, this->oVfWorkQueueFree},
				{"__ZN21IGHardwareGuCCTBuffer4freeEv",
				 vfCtbFree, this->oVfCtbFree},
			};
			PANIC_COND(!patcher.routeMultiple(index, workQueueInitRoute, address, size),
			           "ngreen", "Failed to install VF workqueue allocation unwind");
			// Preserve Apple's complete event-source and callback lifecycle.  Its
			// Gen11 virtual-MMIO path is the native TGL/ADL/RPL VF protocol; only
			// MTL/ARL memory-IRQ VFs must suppress the surrounding master accesses.
			mach_vm_address_t irqEnable = 0, irqDisable = 0;
			KernelPatcher::SolveRequest irqLifecycle[] = {
				{"__ZN17IGInterruptBridge6enableEv", irqEnable},
				{"__ZN17IGInterruptBridge7disableEv", irqDisable},
			};
			PANIC_COND(!patcher.solveMultiple(index, irqLifecycle, address, size),
			           "ngreen", "Cannot resolve VF IRQ lifecycle");
			this->vfInterruptBridgeEnable = irqEnable;
			this->vfInterruptBridgeDisable = irqDisable;
			if (gVfUsesMemoryIrq) {
				mach_vm_address_t irqEnableRegs = 0, irqDisableRegs = 0;
				KernelPatcher::SolveRequest irqBounds[] = {
					{"__ZN17IGInterruptBridge16enableInterruptsEv", irqEnableRegs},
					{"__ZN17IGInterruptBridge17disableInterruptsEv", irqDisableRegs},
				};
				PANIC_COND(!patcher.solveMultiple(index, irqBounds, address, size) ||
				           irqEnableRegs <= irqEnable || irqDisableRegs <= irqDisable ||
				           irqEnableRegs - irqEnable > 0x400 ||
				           irqDisableRegs - irqDisable > 0x400,
				           "ngreen", "Invalid memory-IRQ lifecycle patch bounds");
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
				PANIC_COND(!LookupPatchPlus::applyAll(
				               patcher, masterPatches, irqEnable,
				               irqEnableRegs - irqEnable) ||
				           !LookupPatchPlus::applyAll(
				               patcher, masterPatches, irqDisable,
				               irqDisableRegs - irqDisable),
				           "ngreen", "Failed to isolate memory-IRQ lifecycle from GFX_MSTR_IRQ");
			}
			// These entry points can call framebuffer force-wake instead of the
			// multithreaded accelerator route. A VF has no guest-owned domains.
			KernelPatcher::RouteRequest vfWakeRoutes[] = {
				{"__ZN16IntelAccelerator13SafeForceWakeEbj", wrapSafeForceWake},
				{"__ZN16IntelAccelerator22SafeForceWakeInterruptEbj", wrapSafeForceWake},
				{"__ZN16IntelAccelerator26SafeForceWakeMultithreadedEbjj", forceWake},
			};
			PANIC_COND(!patcher.routeMultiple(index, vfWakeRoutes, address, size),
			           "ngreen", "Failed to isolate VF force-wake entry points");

			// The VF has no physical framebuffer by design. Keep Apple's complete
			// standalone fallback, but make its waitForMatchingService lookup
			// nonblocking instead of stalling accelerator start for 30 seconds.
			mach_vm_address_t fbRegistration = 0, fbRegistrationEnd = 0;
			KernelPatcher::SolveRequest fbRegistrationBounds[] = {
				{"__ZN16IntelAccelerator33registerWithFramebufferControllerEv",
				 fbRegistration},
				{"__ZN16IntelAccelerator23initHardwareWorkaroundsEv",
				 fbRegistrationEnd},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, fbRegistrationBounds, address, size) ||
			           fbRegistrationEnd <= fbRegistration ||
			           fbRegistrationEnd - fbRegistration > 0x300,
			           "ngreen", "Invalid VF framebuffer-registration patch bounds");
			LookupPatchPlus const vfFramebufferWait {
				activeKext, NGVfStandalonePatch::waitFind,
				NGVfStandalonePatch::waitReplace, 1,
			};
			PANIC_COND(!vfFramebufferWait.apply(
			               patcher, fbRegistration,
			               fbRegistrationEnd - fbRegistration), "ngreen",
			           "Failed to remove VF physical-framebuffer wait");
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
			KernelPatcher::SolveRequest doorbellReadBounds[] = {
				{"__ZN13IGHardwareGuC23readDoorbellSQIDIConfigEv",
				 doorbellReadStart},
				{"__ZN13IGHardwareGuC15acquireDoorbellEP35UK_GEN11_GUC_CONTEXT_DESCRIPTOR_RECb",
				 doorbellReadEnd},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, doorbellReadBounds, address, size) ||
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
			KernelPatcher::SolveRequest pageTableRollback[] = {
				{"__ZNK11IGAccelTask29getHardwareContextAddressModeEv",
				 this->vfGetHardwareContextAddressMode},
				{"__ZN31IGHardwarePerProcessPageTable3211withOptionsEP16IntelAcceleratorP11IGAccelTask",
				 this->vfPpgtt32WithOptions},
				{"__ZN31IGHardwarePerProcessPageTable6411withOptionsEP16IntelAcceleratorP11IGAccelTask",
				 this->vfPpgtt64WithOptions},
				{"__ZN16IntelAccelerator27flushHardwareAfterGttUpdateEv",
				 this->vfFlushHardwareAfterGttUpdate},
				{"__ZN10IGPagePool14PageDescriptor6retainEv",
				 this->vfPageDescriptorRetain},
				{"__ZN10IGPagePool14PageDescriptor7releaseEv",
				 this->vfPageDescriptorRelease},
			};
			PANIC_COND(!patcher.solveMultiple(
			               index, pageTableRollback, address, size), "ngreen",
			           "Cannot resolve VF page-table rollback boundary");
			KernelPatcher::RouteRequest requests[] = {
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
			// The 32-bit unmapper does not prune table pages, so retire engine
			// translations after its PTE stores. The 64-bit unmapper tail-calls
			// shrinkRange, which can return table pages to the pool; intercept that
			// boundary and retire translations before native pruning/zeroing.
			{"__ZN31IGHardwarePerProcessPageTable3210unmapRangeERK14IGAddressRange",
			 vfPpgtt32UnmapRange, this->oVfPpgtt32UnmapRange},
			{"__ZN31IGHardwarePerProcessPageTable6410unmapRangeERK14IGAddressRange",
			 vfPpgtt64UnmapRange, this->oVfPpgtt64UnmapRange},
			{"__ZN31IGHardwarePerProcessPageTable6411shrinkRangeERK14IGAddressRange",
			 vfPpgtt64ShrinkRange, this->oVfPpgtt64ShrinkRange},
			// Native commit fan-out preserves successful segment/address-space
			// prefixes when a later table fails. Roll every table back while the
			// task, mapping and native caller's ownership scope are still intact.
			{"__ZN15IGMemoryManager26commitIntoPageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap",
			 vfCommitPageTablesForTask, this->oVfCommitPageTablesForTask},
			{"__ZN15IGMemoryManager27releaseFromPageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap",
			 vfReleasePageTablesForTask, this->oVfReleasePageTablesForTask},
			{"__ZN15IGMemoryManager22updatePageTableForTaskEP11IGAccelTaskP16IGAccelMemoryMap",
			 vfUpdatePageTablesForTask, this->oVfUpdatePageTablesForTask},
			// Native synchronization has a void ABI: both the per-entry and the
			// descriptor path can leave a partially populated PPGTT and still return
			// it to the task. Rebuild that one factory transaction with observable
			// map results and release the unpublished table on any failure.
			{"__ZN15IGMemoryManager19newPageTableForTaskEP11IGAccelTask",
			 vfNewPageTableForTask, this->oVfNewPageTableForTask},
			// Display-mode changes synchronize the kernel table and every published
			// task through void helpers. Serialize the complete iterator, make entry
			// failures fail closed, and retain replaced shared descriptors until an
			// acknowledged engine invalidation has completed.
			{"__ZN15IGMemoryManager19synchronizeAllTasksEv",
			 vfSynchronizeAllTasks, this->oVfSynchronizeAllTasks},
			{"__ZN29IGHardwarePerProcessPageTable20synchronizeEachEntryEPK19IGHardwarePageTableRK14IGAddressRangeb",
			 vfSynchronizeEachEntry},
			{"__ZN31IGHardwarePerProcessPageTable6423remapDescriptorForRangeERK14IGAddressRangePN10IGPagePool14PageDescriptorE",
			 vfPpgtt64RemapDescriptor, this->oVfPpgtt64RemapDescriptor},
			// Tahoe constructs the manager pools without their optional native lock.
			// Put allocation, zero-before-return, prune and final teardown in the
			// same recursive ownership domain as every task/table transaction.
			{"__ZN10IGPagePool12allocatePageEv",
			 vfPagePoolAllocatePage, this->oVfPagePoolAllocatePage},
			{"__ZN10IGPagePool11releasePageEPKNS_14PageDescriptorE",
			 vfPagePoolReleasePage, this->oVfPagePoolReleasePage},
			{"__ZN10IGPagePool5pruneEj",
			 vfPagePoolPrune, this->oVfPagePoolPrune},
			{"__ZN10IGPagePool4freeEv",
			 vfPagePoolFree, this->oVfPagePoolFree},
			{"__ZN15IGMemoryManager15releasePagePoolEv",
			 vfReleasePagePool, this->oVfReleasePagePool},
			// Normal manager destruction independently releases dummy/global page
			// tables and clears every published segment before its base free. Keep
			// that complete owner teardown in the transaction too; locking only the
			// pool helpers would still allow synchronization to borrow a cleared
			// +0x98 table or range while final free was in progress.
			{"__ZN15IGMemoryManager4freeEv",
			 vfMemoryManagerFree, this->oVfMemoryManagerFree},
			// Cache-type requests from Metal, GL, blit and media all converge here.
			// Keep resource flags and every installed page table in one recoverable
			// transaction; the native void tail otherwise hides a partial failure.
			{"__ZN15IGAccelResource22updateMappingCacheTypeEj",
			 vfUpdateMappingCacheType, this->oVfUpdateMappingCacheType},
			// Several native callers ignore a false reservation, while timeout
			// recovery can return true without rechecking capacity. Validate the
			// complete reservation before any common ring writer can run.
			{"__ZN20IGHardwareRingBuffer12waitForSpaceEj",
			 vfWaitForRingSpace, this->oVfWaitForRingSpace},

			// Tahoe's aperture-resource path allocates a legacy hardware fence whose
			// constructor and destructor both write the PF-owned 0x100000 fence
			// register bank under physical force-wake. A VF owns no such registers.
			// Reject at the allocation boundary before a fence object or slot exists;
			// the native callers already unwind a null result transactionally.
			{"__ZN16IGFenceAllocator8allocateERK14IGAddressRangem19GFX3DSTATE_TILEMODE",
			 vfRejectPhysicalFence},

			// IGMemoryManager::init invokes this TGL virtual before engine start.
			// Native detection reads and may program physical eDRAM registers even
			// after force-wake itself has been suppressed. A VF has no eDRAM aperture;
			// retain the base initializer's false capability state without MMIO.
			{"__ZN21IntelTGLMemoryManager11detectEDRAMEv",
			 vfDisableEdramProbe},

			// The independent PAVP callback reaches physical force-wake and the
			// PF-owned 0x1082c0/0x320fc register pair before protected-media ring
			// submission. A VF cannot implement that protocol, and fabricated success
			// would falsely acknowledge DRM state. Fail at the callback root while a
			// physical function retains Apple's complete native implementation.
			{"__ZN16IntelAccelerator19PAVPCommandCallbackE22PAVPSessionCommandID_tjPjb",
			 vfRejectPavpCommandCallback},
			// Telemetry is intentionally unavailable on a VF. recognizeFlip has no
			// in-image caller but is exported and tail-calls IGTelemetryKMD::sample,
			// which can submit a main-ring command. Remove that conservative producer
			// root rather than admitting a trace operation which cannot produce data.
			{"__ZN25IGAccelTraceStreamManager13recognizeFlipEv",
			 vfIgnoreTraceRecognizeFlip},
			
			// A VF owns neither engine power/reset nor legacy execlist rings. Keep
			// Apple's lifecycle calls away from PF-owned registers; final stop still
			// performs explicit direct-LRCA and DMA quiescence below.
			{"__ZN16IntelAccelerator19startGraphicsEngineEv", startGraphicsEngine},
			{"__ZN11IGScheduler6createEP16IntelAccelerator", vfCreateScheduler,
			 this->originalSchedulerCreate},
			{"__ZN11IGScheduler15initWithOptionsEjyP22IOGraphicsAccelerator2", vfInitScheduler,
			 this->originalSchedulerInit},
			{"__ZN16IntelAccelerator18stopGraphicsEngineEv",  stopGraphicsEngine},

			// A VF has no guest-owned INSTDONE state. Derive the watchdog result
			// solely from the tracked GuC context lifecycle.
			{"__ZNK12IGScheduler59isGpuIdleEv", wrapIGScheduler5IsGpuIdle},
			{"__ZNK12IGScheduler49isGpuIdleEv", wrapIGScheduler4IsGpuIdle},

			// Native timeout diagnostics bypass the virtual GuC transport: they
			// halt/resume a physical RING_MI_MODE register, capture the legacy GuC
			// scratch bank and write the physical INSTDONE selector. The ring-buffer
			// diagnosis entry points additionally dump a broad PF-owned MMIO
			// registers. A true event timeout must not return to inherited restart
			// and wait retry with an unproven hardware state. Diagnostics remain
			// inert; the separately verified timeout root fail-stops the guest.
			{"__ZN19IGAccelEventMachine12eventTimeoutEi", vfRejectEventTimeout},
			{"__ZN11IGScheduler19haltCommandStreamerE10IGHwCsType",
			 vfSuppressTimeoutHardwareAction},
			{"__ZN11IGScheduler21resumeCommandStreamerE10IGHwCsType",
			 vfSuppressTimeoutHardwareAction},
			{"__ZN16IntelAccelerator15encodeDebugInfoE15IGTimeoutReason",
			 vfSuppressPhysicalDebugCapture},
			{"__ZN20IGHardwareRingBuffer14doHangAnalysisEv",
			 vfSuppressHangAnalysis},
			{"__ZN20IGHardwareRingBuffer16dumpHangAnalysisEv",
			 vfSuppressHangDump},
			// FIFO recovery dispatches through a ring-buffer virtual that performs
			// a complete physical engine reset, then replays the timed-out work.
			// A VF can neither reset its PF-owned engine nor safely claim that the
			// replay boundary succeeded. Quarantine the root request and make the
			// lower physical primitive fail closed for every caller.
			{"__ZN18IGAccelFIFOChannel22resetHardwareAndReplayEv",
			 vfRejectHardwareResetReplay},
			{"__ZN20IGHardwareRingBuffer19resetGraphicsEngineEP17IGHardwareContext",
			 vfRejectPhysicalEngineReset},

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
			 // Final task destruction releases Aux/PPGTT objects before inherited
			 // cleanup. Retire translations while their owners are still intact.
			 {"__ZN11IGAccelTask4freeEv", vfAccelTaskFree,
			  this->oVfAccelTaskFree},
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
			PANIC_COND(!patcher.routeMultiple(index, requests, address, size),
				"ngreen", "Failed to route VF accelerator symbols");
			SYSLOG("ngreen", "V165: VF accelerator symbols routed OK");
		}

		{
			KernelPatcher::RouteRequest startRoute[] = {
				{"__ZN16IntelAccelerator5startEP9IOService", start, this->ostart},
				{"__ZN16IntelAccelerator4stopEP9IOService", acceleratorStop,
				 this->oAcceleratorStop},
			};
			PANIC_COND(!patcher.routeMultiple(index, startRoute, address, size),
			           "ngreen", "Cannot admit pinned accelerator without lifecycle routes");
			SYSLOG("ngreen", "V242: Hooked IntelAccelerator start/stop lifecycle");
		}

		if (vfActive) {
			// The PF already owns GuC firmware. Replace only the VF transport and
			// lifecycle entry points; a physical GPU keeps Apple's native dispatch.
			KernelPatcher::RouteRequest firmwareRoute[] = {
				{"__ZN13IGHardwareGuC13loadGuCBinaryEv", loadGuCBinary},
				// V284: these legacy/base producers can bypass the modern
				// IGHardwareGuC transport entirely.  Reject at their common entry
				// points before any PF-owned DMA, CTB, doorbell or execlist MMIO.
				{"__ZN5IGGuC20sendHostToGucMessageEPK18IGHostToGucMessagejU13block_pointerFvvE",
				 vfRejectLegacyGucMessage},
				{"__ZN5IGGuC12dmaHostToGuCEyjjNS_12IGGucDmaTypeEb",
				 vfRejectLegacyGucDma},
				{"__ZN5IGGuC12ringDoorbellE10IGHwCsType",
				 vfRejectLegacyDoorbell},
				{"__ZN21IGHardwareGuCCTBuffer15hostToGuCActionEPKjjiPjb",
				 vfRejectNativeCtbAction},
				{"__ZN26IGHardwareCommandStreamer514submitExecListEj",
				 vfRejectLegacyExecList},
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
				// startGraphicsEngine enables the bridge before GuC construction so
				// CTB-backed allocations can wait for completions. Tahoe's native
				// request method only appends to a list that enable() services once;
				// immediately run callbacks registered after that one-shot boundary.
				{"__ZN17IGInterruptBridge21requestEnableCallbackEP8OSObjectPFvS1_zE",
				 vfRequestEnableCallback, this->oVfRequestEnableCallback},
				// V237: 0xCEE8 is PF-owned.  A VF invalidates GuC translations
				// through the asynchronous v70 CT action instead.
				{"__ZN13IGHardwareGuC13invalidateTLBEv",
				 vfInvalidateTLB},
				// IGGuC owns context-private GGTT updates and has no CTB pointer.
				// Route its invalidator separately: use the captured hardware GuC once
				// transport is ready, and match i915's pre-ready no-op behavior.
				{"__ZNK5IGGuC13invalidateTLBEv",
				 vfBaseInvalidateTLB},
				{"__ZN20IGSharedMappedBuffer11withOptionsEP11IGAccelTaskmjj",
				 vfCtbMappedBufferWithOptions, this->oVfCtbMappedBufferWithOptions},
				// V230: translate Tahoe's legacy process-wide proxy submission
				// into the one-GuC-ID-per-LRCA lifecycle required by v70 and VFs.
				{"__ZN13IGHardwareGuC29AttachContextDescToGucContextERK21SGfxContextDescriptor",
				 vfAttachContextDesc},
				{"__ZN13IGHardwareGuC31DetachContextDescFromGucContextERK21SGfxContextDescriptor",
				 vfDetachContextDesc},
				{"__ZN13IGHardwareGuC14submitWorkItemEjRK21SGfxContextDescriptor10IGHwCsTypejjj",
				 vfSubmitWorkItem},
			};
			PANIC_COND(!patcher.routeMultiple(index, firmwareRoute, address, size), "ngreen", "Failed to route VF GuC firmware transport");

			if (gVfUsesMemoryIrq) {
				// Only mtl_info-class VFs use the PF-provisioned memory-IRQ page.
				// Their filter, reader and bridge/scheduler programming must stay
				// entirely memory-backed.
				KernelPatcher::RouteRequest memoryIrqRoutes[] = {
					{"__ZN17IGInterruptBridge22interruptFilterHandlerEP28IOFilterInterruptEventSource",
					 vfInterruptFilterHandler},
					{"__ZN17IGInterruptBridge22readAndClearInterruptsER8IGBitSetILm46EE",
					 vfReadAndClearInterrupts},
					{"__ZN17IGInterruptBridge16enableInterruptsEv",
					 vfEnableInterrupts},
					{"__ZN17IGInterruptBridge17disableInterruptsEv",
					 vfDisableInterrupts},
					{"__ZN12IGScheduler416enableInterruptsEv",
					 vfEnableInterrupts},
					{"__ZN12IGScheduler417disableInterruptsEv",
					 vfDisableInterrupts},
				};
				PANIC_COND(!patcher.routeMultiple(
				               index, memoryIrqRoutes, address, size), "ngreen",
				           "Failed to route VF memory-IRQ transport");
			} else {
				// TGL/ADL/RPL implement the same Gen11 virtual interrupt MMIO
				// protocol consumed by Tahoe's native bridge. Keep its filter,
				// read/clear, masks and logical scheduler callback intact. Only the
				// nested per-engine error helpers touch physical RING_* registers.
				KernelPatcher::RouteRequest virtualMmioIrqRoutes[] = {
					{"__ZN12IGScheduler421enableErrorInterruptsEv",
					 vfSuppressPhysicalErrorInterrupts},
					{"__ZN12IGScheduler422disableErrorInterruptsEv",
					 vfSuppressPhysicalErrorInterrupts},
				};
				PANIC_COND(!patcher.routeMultiple(
				               index, virtualMmioIrqRoutes, address, size), "ngreen",
				           "Failed to isolate VF physical engine error interrupts");
			}
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
			KernelPatcher::SolveRequest tlbPatchBounds[] = {
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
			};
			PANIC_COND(!patcher.solveMultiple(index, tlbPatchBounds,
			                                      address, size) ||
			           workQueueInitEnd <= workQueueInit ||
			           workQueueInitEnd - workQueueInit > 0x200 ||
			           workQueueFreeEnd <= workQueueFree ||
			           workQueueFreeEnd - workQueueFree > 0x100 ||
			           ctbInitEnd <= ctbInit || ctbInitEnd - ctbInit > 0x200 ||
			           ctbFreeEnd <= ctbFree || ctbFreeEnd - ctbFree > 0x100 ||
			           releaseUkContextEnd <= releaseUkContext ||
			           releaseUkContextEnd - releaseUkContext > 0x200,
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
			// seven fully routed routines retain their untouched original bytes and
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
			PANIC_COND(
				!LookupPatchPlus::applyAll(patcher, immediateEcxPatches,
				 workQueueInit, workQueueInitEnd - workQueueInit) ||
				!LookupPatchPlus::applyAll(patcher, immediateEcxPatches,
				 workQueueFree, workQueueFreeEnd - workQueueFree) ||
				!LookupPatchPlus::applyAll(patcher, immediateMemoryPatches,
				 ctbFree, ctbFreeEnd - ctbFree) ||
				!LookupPatchPlus::applyAll(patcher, immediateEcxPatches,
				 releaseUkContext, releaseUkContextEnd - releaseUkContext),
				"ngreen", "V237: failed to isolate a live VF physical-TLB caller");
			SYSLOG("ngreen", "V237: isolated all live native 0xCEE8 callers");

		}

		// Keep IGAccelDevice::deviceStart native, including failure propagation.

		// Admit exactly the 0x9a49 compatibility identity. The old patch removed
		// two branches and made every ID reach GT2 initialization; changing the
		// existing 0x9a40 compare immediate preserves Apple's sentinel and failure
		// paths while accepting the one device ID published by our PCI hook. This
		// comparison belongs to IntelAccelerator::probe(), not getGPUInfo(). Keep
		// its patch bounded to the owning symbol so a valid anchor elsewhere can
		// never conceal a wrong function-range assumption.
		static const uint8_t r3[] = {
			0x8b, 0x3e, 0x81, 0xff, 0xee, 0xbe, 0xaf, 0xde, 0x7f, 0x15,
			0x81, 0xff, 0x86, 0x80, 0x49, 0x9a, 0x74, 0x2d,
		};
		mach_vm_address_t probeStart = 0, probeEnd = 0;
		KernelPatcher::SolveRequest probeBounds[] = {
			{"__ZN16IntelAccelerator5probeEP9IOServicePi", probeStart},
			{"__ZN16IntelAccelerator18encodeFailureStackE15IGFailureReason", probeEnd},
		};
		PANIC_COND(!patcher.solveMultiple(index, probeBounds, address, size) ||
		           probeEnd <= probeStart || probeEnd - probeStart > 0x800,
		           "ngreen", "Invalid IntelAccelerator::probe patch bounds");
		LookupPatchPlus const patchesAlways[] = {
			{activeKext, NGVfRuntimePatch::spoofedSkuFind, r3,
			 arrsize(NGVfRuntimePatch::spoofedSkuFind), 1},
		};
		PANIC_COND(!LookupPatchPlus::applyAll(
		               patcher, patchesAlways, probeStart, probeEnd - probeStart),
		           "ngreen", "Failed to patch pinned accelerator probe admission");

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
			KernelPatcher::SolveRequest gpuInfoBounds[] = {
				{"__ZN16IntelAccelerator10getGPUInfoEv", gpuInfoStart},
				{"__ZN16IntelAccelerator14teardownDeviceEP11IOPCIDevice", gpuInfoEnd},
			};
			PANIC_COND(!patcher.solveMultiple(index, gpuInfoBounds,
			                                      address, size) ||
			           gpuInfoEnd <= gpuInfoStart ||
			           gpuInfoEnd - gpuInfoStart > 0x1000,
			           "ngreen", "Invalid getGPUInfo patch bounds");
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
					{activeKext, NGVfRuntimePatch::sliceFuseFind, vfSliceFuseReplace, 1},
					{activeKext, NGVfRuntimePatch::dssFuseFind, vfDssFuseReplace, 1},
					{activeKext, NGVfRuntimePatch::euFuseFind, vfEuFuseReplace, 1},
					{activeKext, NGVfRuntimePatch::mediaFuseFind, vfMediaFuseReplace, 1},
					{activeKext, NGVfRuntimePatch::rpmConfigFind, vfRpmConfigReplace, 1},
					{activeKext, NGVfRuntimePatch::l3BranchFind, r3b, 1},
				};
				PANIC_COND(!LookupPatchPlus::applyAll(
					patcher, vfRuntimePatches, gpuInfoStart, gpuInfoEnd - gpuInfoStart),
					"ngreen", "Failed to inject PF runtime fuses into VF getGPUInfo");
				SYSLOG("ngreen", "V244: injected relayed VF runtime fuses into pinned getGPUInfo");
			}

			if (!tglGeneration) {
				mach_vm_address_t rectListStart = 0, rectListEnd = 0;
				KernelPatcher::SolveRequest rectListBounds[] = {
					{"__ZL22blit3d_submit_rectlistP23IGHardwareBlit3DContextP15blit3d_params_tPK8IGVectorI11rect_pair_t25IGIOMallocAllocatorPolicyE",
					 rectListStart},
					{"__ZL19IsSurfaceCompressedj", rectListEnd},
				};
				PANIC_COND(!patcher.solveMultiple(
				               index, rectListBounds, address, size) ||
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
		const char *bundleId = activeKext == &kextG11HWTA ?
			"com.apple.driver.AppleIntelTGLGraphics" :
			"com.xxxxx.driver.AppleIntelTGLGraphics";
		PANIC_COND(!injectAcceleratorPersonality(bundleId), "ngreen",
			"Cannot publish complete TGL accelerator personality");

		return true;
	}

    return false;
}

bool Gen11::vfAllocMoreCommandBuffers(void *pool)
{
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

void *Gen11::vfGetCommandBufferPtrNoInc(void *pool, uint32_t dwords)
{
	PANIC_COND(!callback || !callback->oIOAccelGetCommandBufferPtrNoInc,
	           "ngreen", "Missing native command-pool getter trampoline");
	const bool target = pool && gVfIdentity == VfIdentity::Virtual &&
		gVfAccelerator &&
		getMember<void *>(pool, NGIOAccelCommandPool::acceleratorOffset) ==
			gVfAccelerator;
	void *const result = FunctionCast(
		vfGetCommandBufferPtrNoInc,
		callback->oIOAccelGetCommandBufferPtrNoInc)(pool, dwords);
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
		start, end, cursor, reinterpret_cast<uintptr_t>(result), dwords);
	if (!complete) {
		vfMarkProtocolFault("VF command-pool getter returned inadequate capacity");
		PANIC_COND(true, "ngreen",
		           "V272: refusing inadequate VF command buffer request=%u current=%d count=%u",
		           dwords, current, count);
	}
	return result;
}

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

bool Gen11::vf2DEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVf2DEventVectorGrow,
	           "ngreen", "Missing 2D event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVf2DEventVectorGrow));
}

bool Gen11::vfAcceleratorEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfAcceleratorEventVectorGrow,
	           "ngreen", "Missing accelerator event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfAcceleratorEventVectorGrow));
}

bool Gen11::vfRenderEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfRenderEventVectorGrow,
	           "ngreen", "Missing render event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfRenderEventVectorGrow));
}

bool Gen11::vfBlitEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfBlitEventVectorGrow,
	           "ngreen", "Missing blit event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfBlitEventVectorGrow));
}

bool Gen11::vfGLEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfGLEventVectorGrow,
	           "ngreen", "Missing GL event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfGLEventVectorGrow));
}

bool Gen11::vfResourceEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfResourceEventVectorGrow,
	           "ngreen", "Missing resource event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfResourceEventVectorGrow));
}

bool Gen11::vfSharedEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfSharedEventVectorGrow,
	           "ngreen", "Missing shared event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfSharedEventVectorGrow));
}

bool Gen11::vfSurfaceEventVectorGrow(void *vector, size_t requested)
{
	PANIC_COND(!callback || !callback->oVfSurfaceEventVectorGrow,
	           "ngreen", "Missing surface event-vector growth trampoline");
	using Grow = bool (*)(void *, size_t);
	return vfEventVectorGrowChecked(vector, requested,
		reinterpret_cast<Grow>(callback->oVfSurfaceEventVectorGrow));
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
	if (!gVfHardwareGuc || !vfInvalidateTLBSync(
		gVfHardwareGuc, NGVfGuCRequest::TlbTarget::Guc)) {
		vfMarkProtocolFault("VF GGTT update did not complete TLB invalidation");
		return false;
	}
	return true;
}

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
	vfRequireCompletedGgttUpdate();
	return true;
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
		vfRequireCompletedGgttUpdate();
		segments->memory->release();
		vfMarkProtocolFault("VF rotated physical segment changed during mapping");
		return false;
	}

	rotated->sourcePage = static_cast<uint32_t>(totalPages);
	rotated->cursor = spec.rangeStart + spec.rangeLength;
	vfRequireCompletedGgttUpdate();
	segments->memory->release();

	return true;
}

void Gen11::IGHardwareGlobalPageTableUnmapRange(void *that,
                                                const NGIGAddressRange &range)
{
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
	PANIC_COND(!vfInvalidateTLBSync(
		gVfHardwareGuc, NGVfGuCRequest::TlbTarget::Guc),
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

void Gen11::vfPpgtt32UnmapRange(void *that,
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

void Gen11::vfPpgtt64UnmapRange(void *that,
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

void Gen11::vfPpgtt64ShrinkRange(void *that,
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

bool Gen11::vfCommitPageTablesForTask(void *that, void *task, void *mapping)
{
	PANIC_COND(!that || !task || !mapping ||
	           !callback->oVfCommitPageTablesForTask ||
	           !callback->oVfReleasePageTablesForTask,
		"ngreen", "Missing VF page-table commit/rollback boundary");
	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF page-table commit cannot enter the page-table transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	const bool committed = FunctionCast(
		vfCommitPageTablesForTask,
		callback->oVfCommitPageTablesForTask)(that, task, mapping);
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

bool Gen11::vfReleasePageTablesForTask(void *that, void *task, void *mapping)
{
	PANIC_COND(!that || !task || !mapping ||
	           !callback->oVfReleasePageTablesForTask,
		"ngreen", "Missing native VF page-table release boundary");
	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF page-table release cannot enter the page-table transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	const bool released = FunctionCast(
		vfReleasePageTablesForTask,
		callback->oVfReleasePageTablesForTask)(that, task, mapping);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
	return released;
}

bool Gen11::vfUpdatePageTablesForTask(void *that, void *task, void *mapping)
{
	PANIC_COND(!that || !task || !mapping ||
	           !callback->oVfUpdatePageTablesForTask,
		"ngreen", "Missing native VF page-table update boundary");
	PANIC_COND(!vfEnsurePageTableUpdateLock(), "ngreen",
		"VF page-table update cannot enter the page-table transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	const bool updated = FunctionCast(
		vfUpdatePageTablesForTask,
		callback->oVfUpdatePageTablesForTask)(that, task, mapping);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
	return updated;
}

void *Gen11::vfNewPageTableForTask(void *that, void *task)
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
	const bool rangeValid =
		(managerRange.start & (PAGE_SIZE - 1U)) == 0 &&
		(managerRange.length & (PAGE_SIZE - 1U)) == 0 &&
		managerRange.length <= UINT64_MAX - managerRange.start;
	bool synchronized = destinationVtable && sourceVtable && rangeValid;

	const bool destinationUsesDescriptors =
		(getMember<uint32_t>(pageTable, 0x28) & 1U) != 0;
	const bool sourceUsesDescriptors = source &&
		(getMember<uint32_t>(source, 0x28) & 1U) != 0;
	if (synchronized && !kernelTask && destinationUsesDescriptors &&
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
		if (synchronized && readDescriptor(source, descriptorRange, &descriptor))
			synchronized = descriptor &&
				mapDescriptor(pageTable, descriptorRange, descriptor);
	} else if (synchronized) {
		using ReadEntry = bool (*)(const void *, uint64_t, uint64_t &, uint64_t &);
		using MapEntry = bool (*)(void *, const NGIGAddressRange &,
		                         uint64_t, uint64_t);
		auto readEntry = reinterpret_cast<ReadEntry>(
			sourceVtable[0x140 / sizeof(mach_vm_address_t)]);
		auto mapEntry = reinterpret_cast<MapEntry>(
			destinationVtable[0x118 / sizeof(mach_vm_address_t)]);
		synchronized = readEntry && mapEntry;
		const uint64_t end = managerRange.start + managerRange.length;
		for (uint64_t address = managerRange.start;
		     synchronized && address != end; address += PAGE_SIZE) {
			uint64_t physical = 0;
			uint64_t flags = 0;
			if (!readEntry(source, address, physical, flags))
				continue;
			const NGIGAddressRange pageRange {address, PAGE_SIZE};
			synchronized = mapEntry(pageTable, pageRange, physical, flags);
		}
		// Native synchronizeEachEntry always records the deferred flush,
		// including empty and failed ranges. Preserve that ordering before an
		// unpublished partial hierarchy can be released.
		using FlushGtt = void (*)(void *);
		reinterpret_cast<FlushGtt>(
			callback->vfFlushHardwareAfterGttUpdate)(accelerator);
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

	IORecursiveLockUnlock(gVfPageTableUpdateLock);
	return pageTable;
}

void Gen11::vfSynchronizeAllTasks(void *that)
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

void Gen11::vfSynchronizeEachEntry(void *that, const void *source,
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

	const uint64_t end = range.start + range.length;
	for (uint64_t address = range.start;
	     synchronized && address != end; address += PAGE_SIZE) {
		uint64_t physical = 0;
		uint64_t flags = 0;
		if (!readEntry(source, address, physical, flags))
			continue;
		const NGIGAddressRange pageRange {address, PAGE_SIZE};
		// A one-page remap can fail only before its store when the destination
		// hierarchy is absent. Build that missing hierarchy with mapRange rather
		// than letting the native void helper publish a silent prefix.
		synchronized = remap && remapEntry ?
			remapEntry(that, pageRange, physical, flags) :
			mapEntry(that, pageRange, physical, flags);
		if (!synchronized && remap)
			synchronized = mapEntry(that, pageRange, physical, flags);
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

bool Gen11::vfPpgtt64RemapDescriptor(void *that,
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

void *Gen11::vfPagePoolAllocatePage(void *that)
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

void Gen11::vfPagePoolReleasePage(void *that, const void *descriptor)
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

void Gen11::vfPagePoolPrune(void *that, uint32_t age)
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

void Gen11::vfPagePoolFree(void *that)
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

void Gen11::vfReleasePagePool(void *that)
{
	PANIC_COND(!that || !callback->oVfReleasePagePool ||
	           !vfEnsurePageTableUpdateLock(), "ngreen",
		"Missing VF manager PagePool teardown transaction");
	IORecursiveLockLock(gVfPageTableUpdateLock);
	FunctionCast(vfReleasePagePool,
	             callback->oVfReleasePagePool)(that);
	IORecursiveLockUnlock(gVfPageTableUpdateLock);
}

void Gen11::vfMemoryManagerFree(void *that)
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

void Gen11::vfUpdateMappingCacheType(void *that, uint32_t requestedType)
{
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

bool Gen11::vfWaitForRingSpace(void *that, uint32_t requestedDwords)
{
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

	const bool nativeResult = FunctionCast(
		vfWaitForRingSpace,
		callback->oVfWaitForRingSpace)(that, callerRequest.dwords);
	const uint32_t available = getMember<uint32_t>(that, 0x88);
	const uint32_t committedCursor = getMember<uint32_t>(that, 0x64);
	PANIC_COND(!nativeResult ||
	           getMember<void *>(that, 0x10) != accelerator ||
	           getMember<uint8_t>(that, 0x40) != engine ||
	           getMember<uint32_t>(that, 0x8C) != ringBytes ||
	           getMember<uint32_t>(that, 0x90) != ringMask ||
	           committedCursor >= ringBytes || (committedCursor & 3U) != 0 ||
	           getMember<uint8_t>(that, 0x6D) != (tlbPending ? 1U : 0U) ||
	           getMember<uint8_t>(that, 0x6E) != (auxPending ? 1U : 0U) ||
	           !NGVfSubmission::ringReservationSatisfied(
	               reservation, available, ringBytes),
		"ngreen", "VF ring reservation returned without proven capacity");
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

void Gen11::vfAccelTaskFree(void *that)
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

bool Gen11::start(void *that, void *provider)
{
	if (!that || !provider || !callback || !callback->ostart) {
		SYSLOG("ngreen", "V239: refusing accelerator start with incomplete IOService ABI");
		return false;
	}
	// An SR-IOV VF owns neither force-wake nor legacy execlist MMIO. Establish
	// the GuC VF ABI and its assigned GGTT range before native scheduler setup.
	const auto identity = vfIdentifyDevice();
	if (identity == VfIdentity::Invalid) {
		SYSLOG("ngreen", "V239: refusing accelerator start with invalid PCI identity");
		return false;
	}
	const bool vfActive = identity == VfIdentity::Virtual;
	if (vfActive) {
		// start() is the first stable point where the exact IntelAccelerator
		// instance is available. Publish it before native context/pool creation;
		// the System-KC wrapper compares this owner and leaves every other pool
		// on the native PF path.
		gVfAccelerator = that;
		OSSynchronizeIO();
	}
	if (vfActive) {
		// utilGetProperty<unsigned> lets this OSData override the mandatory
		// accelerator property after publication. Do not invoke its string
		// parser or mutate global options to resolve a VF conflict.
		auto *options = IORegistryEntry::fromPath("IODeviceTree:/options");
		auto *overrideProperty = options ?
			options->copyProperty("GraphicsSchedulerSelect") : nullptr;
		const bool hasSchedulerOverride = OSDynamicCast(OSData, overrideProperty);
		OSSafeReleaseNULL(overrideProperty);
		OSSafeReleaseNULL(options);
		if (hasSchedulerOverride) {
			vfMarkProtocolFault("VF rejects late device-tree scheduler override");
			return false;
		}
	}
	// Native start overrides GraphicsSchedulerSelect with scheduler 5 when
	// this boot argument is present. A VF cannot enter that physical path.
	int firmwareDisableArgument = 0;
	if (vfActive && PE_parse_boot_argn("-disablegfxfirmware",
	                                  &firmwareDisableArgument,
	                                  sizeof(firmwareDisableArgument))) {
		vfMarkProtocolFault("VF cannot disable mandatory GuC firmware scheduling");
		return false;
	}
	if (vfActive && (getMember<uint8_t>(that, 0x1190) &
	                 kVfLegacyPageOwnershipFlag)) {
		vfMarkProtocolFault(
			"VF exposes unsupported legacy page-ownership/async-slice mode");
		return false;
	}
	if (vfActive && !vfBootstrapDirectGgtt()) {
		vfMarkProtocolFault("VF bootstrap failed before native accelerator start");
		return false;
	}
	if (vfActive) {
		// The UUID-pinned TGL driver predates IOPCIDevice::configureInterrupts
		// and asks getInterruptType() to lazily resolve a local filter source.
		// Tahoe exposes the VF's one-vector, 64-bit MSI capability, but no legacy
		// INTx route; explicitly allocate that MSI before the old driver creates
		// its IOFilterInterruptEventSource.  This is a public IOPCIFamily ABI in
		// Tahoe (and an exported symbol), not a fabricated interrupt property.
		if (!callback->ioPciConfigureInterrupts) {
			vfMarkProtocolFault("missing exported PCI MSI configurator");
			return false;
		}
		auto *pciDevice = OSDynamicCast(
			IOPCIDevice, reinterpret_cast<OSObject *>(provider));
		if (!pciDevice) {
			vfMarkProtocolFault("accelerator provider is not an IOPCIDevice");
			return false;
		}
		using ConfigureInterrupts = IOReturn (*)(IOPCIDevice *, UInt32,
			UInt32, UInt32, IOOptionBits);
		const IOReturn interruptResult =
			reinterpret_cast<ConfigureInterrupts>(
				callback->ioPciConfigureInterrupts)(
					pciDevice, kIOInterruptTypePCIMessaged, 1, 1, 0);
		if (interruptResult != kIOReturnSuccess) {
			SYSLOG("ngreen", "V246: VF MSI allocation failed ret=0x%x",
			       interruptResult);
			vfMarkProtocolFault("VF MSI allocation failed before native start");
			return false;
		}
		SYSLOG("ngreen", "V246: allocated the VF PCI MSI before local interrupt-bridge creation");
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
		return false;
	}
	SYSLOG("ngreen", "Accelerator scheduler=%d path=%s", scheduler,
	       vfActive ? "VF GuC" : "physical");

	if (vfActive) {
		auto *zero = OSNumber::withNumber(0ULL, 32);
		auto *one = OSNumber::withNumber(1ULL, 32);
		const bool pmDisabled =
			zero && service->setProperty("SchedPmNotifyEnable", zero);
		const bool fallbackDisabled =
			zero && service->setProperty("SchedulerFallbackOnFirmwareFail", zero);
		const bool telemetryDisabled =
			one && service->setProperty("TelemetryDisable", one);
		OSSafeReleaseNULL(zero);
		OSSafeReleaseNULL(one);
		if (!pmDisabled || !fallbackDisabled || !telemetryDisabled) {
			vfMarkProtocolFault(
				"failed to disable PF-owned VF scheduler/telemetry paths");
			return false;
		}
	}

	const auto result = FunctionCast(start, callback->ostart)(that, provider);
	if (!result) {
		if (vfActive) {
			// The pinned Tahoe start body normally routes failures after
			// startGraphicsEngine() through its virtual stop entry.  Its 0x215
			// DPSM-timer allocation failure is the exception: that edge returns
			// false after the VF GuC/CTB and accelerator lifecycle are live, but
			// never calls stop.  Close that exact transactional hole here while
			// retirement transport is still trusted.  acceleratorStop() sets the
			// device-stopping boundary before native finishAllStamps and reaches
			// our stopGraphicsEngine() quiescence route; use a null provider just
			// like Tahoe's other start-failure cleanup edge.
			OSSynchronizeIO();
			if (gVfSchedulerFirmwareReady && !gVfDeviceStopping) {
				SYSLOG("ngreen", "V252: rolling back live VF engine after native accelerator start failure");
				acceleratorStop(that, nullptr);
				OSSynchronizeIO();
				PANIC_COND(!gVfDeviceStopping || !gVfDmaQuiesced, "ngreen",
					"Native VF start failure escaped without a DMA-quiesced stop");
			}
			vfMarkProtocolFault("native accelerator start failed after VF bootstrap");
		}
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
	// context is retired while CTB and the selected IRQ transport remain live.
	if (gVfIdentity == VfIdentity::Virtual) {
		OSCompareAndSwap(0, 1, &gVfDeviceStopping);
		OSSynchronizeIO();
		SYSLOG("ngreen", "V242: VF accelerator stop requested; deferring quiescence until post-stamp engine stop");
	}
	FunctionCast(acceleratorStop, callback->oAcceleratorStop)(that, provider);
}

uint32_t Gen11::vfTelemetryPrintDashboard(void *that, uint64_t options)
{
	(void)that;
	(void)options;
	return 0;
}

int Gen11::vfTelemetryInitWithAccelerator(void *that, void *accelerator,
	                                      uint32_t options)
{
	if (!that || !accelerator)
		return static_cast<int>(kIOReturnUnsupported);

	// Exact Tahoe IGTelemetryManager layout. telemetryCreateManager has already
	// constructed the hash table and zeroed the embedded IGSupportMDAPI object.
	// Publish only the software ownership fields consumed by teardown/statistics;
	// leave both OA reference counters and all mapped-buffer pointers zero. The
	// native destructor's finalizeOaBuffer() zero-counter fast path then performs
	// no register access and no release of fabricated storage.
	getMember<void *>(that, 0x258) = accelerator;
	getMember<void *>(that, 0x2D8) = accelerator;
	getMember<void *>(that, 0x228) = getMember<void *>(accelerator, 0xE00);
	getMember<uint32_t>(that, 0x2A0) = 0;
	getMember<uint32_t>(that, 0x10) = 1;
	getMember<uint32_t>(that, 0x0C) = options;
	getMember<uint64_t>(that, 0x230) = 1;
	getMember<uint32_t>(that, 0x30C) = 0;
	getMember<uint32_t>(that, 0x310) = 0;
	return 0;
}

void Gen11::vfTelemetryRetain(void *that)
{
	(void)that;
}

void Gen11::vfTelemetryRelease(void *that)
{
	(void)that;
}

uint64_t Gen11::vfTelemetryCalcGlobalUsage(void *that, uint64_t timestamp)
{
	(void)that;
	(void)timestamp;
	return 0;
}

int64_t Gen11::vfTelemetryOperation(void *that, uint64_t selector,
	                                int64_t value, void *operation,
	                                void *connection, void *task)
{
	(void)that;
	(void)selector;
	(void)value;
	(void)operation;
	(void)connection;
	(void)task;
	return static_cast<int64_t>(static_cast<int32_t>(kIOReturnUnsupported));
}

void Gen11::vfTelemetryPatchContextImage(void *that, void *contextImage)
{
	(void)that;
	(void)contextImage;
}

void Gen11::vfTelemetryOnConnectionStop(void *that, void *connection,
	                                     void *task)
{
	(void)that;
	(void)task;
	if (!connection)
		return;
	auto *flags = static_cast<uint8_t *>(connection);
	flags[0x08] = 0;
	flags[0x09] = 0;
	flags[0x0A] = 0;
}

IOReturn Gen11::vfTelemetryInitOaBuffer(void *that, void *connection,
	                                    void *input, void *output,
	                                    uint64_t inputSize,
	                                    uint64_t *outputSize)
{
	(void)that;
	(void)connection;
	(void)input;
	(void)output;
	(void)inputSize;
	(void)outputSize;
	return kIOReturnUnsupported;
}

IOReturn Gen11::vfTelemetryReadOaBuffer(void *that, void *input,
	                                    void *output, uint64_t *outputSize,
	                                    void *connection)
{
	(void)that;
	(void)input;
	(void)output;
	(void)outputSize;
	(void)connection;
	return kIOReturnUnsupported;
}

IOReturn Gen11::vfTelemetryMapOaBufferMemory(void *that, void *input,
	                                         void *output, uint64_t inputSize,
	                                         uint64_t *outputSize, void *task)
{
	(void)that;
	(void)input;
	(void)output;
	(void)inputSize;
	(void)outputSize;
	(void)task;
	return kIOReturnUnsupported;
}

void Gen11::vfTelemetryUsageAlloc(void *that)
{
	if (!that)
		return;
	// Preserve initWithAcceleratorAndStampIndex()'s supported allocation-failure
	// state without paying for 64 per-stamp GPU and metadata buffers on a VF.
	getMember<void *>(that, 0x68) = nullptr;
	getMember<void *>(that, 0x70) = nullptr;
	getMember<uint32_t>(that, 0x78) = 0;
	getMember<uint64_t>(that, 0x80) = 0;
	getMember<void *>(that, 0x88) = nullptr;
	getMember<uint32_t>(that, 0x90) = 0;
}

void Gen11::vfTelemetryUsageLog(void *that, void *context, uint64_t timestamp)
{
	(void)that;
	(void)context;
	(void)timestamp;
}

void Gen11::vfTelemetryUsageReportGlobal(void *that)
{
	(void)that;
}

bool Gen11::vfTelemetryUsageStartSample(void *that, void *ring,
	                                     uint32_t stamp, uint32_t engine,
	                                     uint64_t commandId,
	                                     uint64_t submitTime,
	                                     uint64_t startTime,
	                                     uint64_t endTime)
{
	(void)that;
	(void)ring;
	(void)stamp;
	(void)engine;
	(void)commandId;
	(void)submitTime;
	(void)startTime;
	(void)endTime;
	return false;
}

void Gen11::vfTelemetryUsageStopSample(void *that, void *ring,
	                                   uint32_t stamp, uint32_t engine)
{
	(void)that;
	(void)ring;
	(void)stamp;
	(void)engine;
}

void Gen11::vfTelemetryUsageFrameCalc(void *that, void *accelerator,
	                                   uint32_t frame)
{
	(void)that;
	(void)accelerator;
	(void)frame;
}

void Gen11::vfDisableDebugSysctl(void *that)
{
	(void)that;
}

IOReturn Gen11::vfRejectPavpCommandCallback(void *that, uint32_t command,
	                                         uint32_t session, uint32_t *data,
	                                         bool recovery)
{
	(void)that;
	(void)command;
	(void)session;
	(void)data;
	(void)recovery;
	return kIOReturnUnsupported;
}

void Gen11::vfIgnoreTraceRecognizeFlip(void *that)
{
	(void)that;
}

void *Gen11::vfRejectPhysicalFence(void *that,
	                               const NGIGAddressRange &range,
	                               uint64_t pitch, uint32_t tileMode)
{
	(void)that;
	(void)range;
	(void)pitch;
	(void)tileMode;
	return nullptr;
}

void Gen11::vfDisableEdramProbe(void *that)
{
	if (!that)
		return;
	getMember<uint8_t>(that, 0x20) = 0;
	getMember<uint8_t>(that, 0x21) = 0;
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
	const bool vfTask = vfIdentifyDevice() == VfIdentity::Virtual && that != nullptr;
	if (vfTask && !vfEnsurePageTableUpdateLock()) {
		SYSLOG("ngreen", "V283: cannot serialize VF task construction");
		return nullptr;
	}
	if (vfTask)
		IORecursiveLockLock(gVfPageTableUpdateLock);
	if (vfTask && getMember<void *>(that, 0x150) == nullptr) {
		if (callback->igAccelTaskCounter == 0) {
			SYSLOG("ngreen", "V216: bootstrap task counter symbol is unavailable; refusing unsafe allocation");
			IORecursiveLockUnlock(gVfPageTableUpdateLock);
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
				IORecursiveLockUnlock(gVfPageTableUpdateLock);
				return nullptr;
			}
		}
	}

	// Preserve native ownership: a failed factory returns null. Never write an
	// unrelated IOAccelTask base-class field or substitute a borrowed task. The
	// recursive domain spans inherited early list publication, private-table
	// construction and failed-init destruction/list unlink.
	auto *task = FunctionCast(igAccelTaskWithOptions,
	                         callback->oigAccelTaskWithOptions)(that);
	if (vfTask)
		IORecursiveLockUnlock(gVfPageTableUpdateLock);
	return task;
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

bool Gen11::vfInitScheduler(void *scheduler, uint32_t options,
	                        uint64_t privateSize, void *accelerator)
{
	if (!scheduler || !accelerator || !callback || !callback->originalSchedulerInit)
		return false;
	if (!FunctionCast(vfInitScheduler, callback->originalSchedulerInit)(
			scheduler, options, privateSize, accelerator))
		return false;
	// Native base init ignores addEventSource's result. The pinned default
	// timer constructor and effective getter let us verify actual attachment.
	auto *timer = getMember<IOTimerEventSource *>(scheduler, 0x448);
	auto *expected = getMember<IOWorkLoop *>(accelerator, 0xf0);
	if (!timer || !expected) {
		vfMarkProtocolFault("VF native scheduler success has incomplete timer ownership");
		PANIC("ngreen", "Cannot unwind incomplete VF scheduler timer ownership");
	}
	auto *attached = timer->getWorkLoop();
	if (attached && attached != expected) {
		vfMarkProtocolFault("VF scheduler timer bound to unexpected workloop");
		PANIC("ngreen", "Cannot release VF scheduler with foreign timer binding");
	}
	if (attached != expected) {
		vfMarkProtocolFault("VF scheduler timer failed workloop attachment");
		return false;
	}
	// Do not manually free: the failed outer factory owns final cleanup.
	return true;
}

void *Gen11::vfCreateScheduler(void *accelerator)
{
	// Installed only on the VF route table. Validate the final native bits
	// before create can tail-dispatch into any physical scheduler factory.
	if (!accelerator || !callback || !callback->originalSchedulerCreate)
		return nullptr;
	const uint32_t schedulerType =
		(getMember<uint32_t>(accelerator, 0x1190) >> 23) & 7U;
	if (schedulerType != 4U) {
		vfMarkProtocolFault("VF final native scheduler selection is not GuC type 4");
		return nullptr;
	}
	return FunctionCast(vfCreateScheduler, callback->originalSchedulerCreate)(accelerator);
}

bool Gen11::startGraphicsEngine(void *that)
{
	// This replacement is installed only for a classified VF. Ring power,
	// reset and legacy execlist state belong to the PF, but the native tail of
	// IntelAccelerator::startGraphicsEngine first enters the scheduler firmware
	// boundary and then performs two software lifecycle transitions:
	// IGInterruptBridge::enable() and
	// IOGraphicsAccelerator2::enableAccelerator().  Omitting those transitions
	// leaves IOAccel resource creation asleep in acceleratorWaitEnabled().
	if (!that || !callback || !callback->vfSchedulerInitFirmware ||
	    !callback->vfInterruptBridgeEnable ||
	    !callback->vfInterruptBridgeDisable ||
	    !callback->ioGraphicsEnableAccelerator ||
	    !callback->ioAccelEventMachineInitEvent) {
		vfMarkProtocolFault("VF engine start before accelerator lifecycle is ready");
		return false;
	}

	// In the pinned native routine this call follows IGMemoryManager::initCache.
	// That cache method directly programs force-wake, MOCS, L3 and other
	// PF-owned MMIO, so a VF must omit it. IGScheduler::initFirmware is the next
	// independent named boundary: for scheduler 4 it constructs GuC and
	// registers the translated modern CTB. Requiring complete transport readiness
	// before this call created an impossible circular precondition and made every
	// otherwise-valid VF start return failure 0x214.
	auto *scheduler = getMember<void *>(that, 0x1250);
	if (!scheduler) {
		vfMarkProtocolFault("missing VF scheduler before firmware initialization");
		return false;
	}
	auto *interruptBridge = getMember<void *>(that, 0x1248);
	if (!interruptBridge) {
		vfMarkProtocolFault("missing VF interrupt bridge during engine start");
		return false;
	}
	using LifecycleMethod = void (*)(void *);
	// A VF has to admit its MSI consumer before CTB enable. Unlike the physical
	// Tahoe sequence, scheduler initialization allocates GGTT-backed proxy state
	// after enabling CTB, and every post-CTB map requires a synchronous GuC TLB
	// completion. Linux follows the same dependency: CT is enabled, interrupt
	// delivery is enabled immediately, then messages crossing that boundary are
	// consumed. The UUID-pinned bridge body retains the event-source lifecycle;
	// force-wake is isolated. TGL/ADL/RPL retain its native Gen11 virtual-MMIO
	// programming while MTL/ARL route the bridge to their memory-IRQ page.
	reinterpret_cast<LifecycleMethod>(callback->vfInterruptBridgeEnable)(
		interruptBridge);
	if (getMember<uint8_t>(interruptBridge, 0x8A8) == 0) {
		vfMarkProtocolFault("VF interrupt bridge did not enter enabled state");
		return false;
	}
	if (!gVfUsesMemoryIrq) {
		OSCompareAndSwap(0, 1, &gVfMmioIrqReady);
		OSSynchronizeIO();
	}
	SYSLOG("ngreen", "V251: enabled VF %s MSI consumer before scheduler firmware initialization",
	       gVfUsesMemoryIrq ? "memory-IRQ" : "virtual-MMIO");

	using InitFirmware = IOReturn (*)(void *);
	const IOReturn firmwareResult =
		reinterpret_cast<InitFirmware>(callback->vfSchedulerInitFirmware)(scheduler);
	if (firmwareResult != kIOReturnSuccess) {
		SYSLOG("ngreen", "V247: VF scheduler firmware initialization failed ret=0x%x",
		       firmwareResult);
		reinterpret_cast<LifecycleMethod>(callback->vfInterruptBridgeDisable)(
			interruptBridge);
		OSCompareAndSwap(1, 0, &gVfMmioIrqReady);
		OSSynchronizeIO();
		vfMarkProtocolFault("VF scheduler firmware initialization failure");
		return false;
	}
	OSCompareAndSwap(0, 1, &gVfSchedulerFirmwareReady);
	OSSynchronizeIO();
	if (!vfNativeGpuWorkReady()) {
		SYSLOG("ngreen",
		       "V251: incomplete post-firmware state ggtt=%d irq=%d mode=%s ctbCpu=%d ctbGpu=0x%x enabled=%d stopped=%d submissionStopped=%d fault=%d",
		       gVfGGTTReady, vfInterruptTransportReady(),
		       gVfUsesMemoryIrq ? "memory" : "MMIO", gVfCtbCpuBase != nullptr,
		       gVfCtbGpuBase, gVfCtbEnabled != 0, gVfCtbStopped != 0,
		       gVfSubmissionStopped != 0, gVfProtocolFault != 0);
		vfMarkProtocolFault("VF transport incomplete after scheduler firmware initialization");
		return false;
	}
	SYSLOG("ngreen", "V247: VF scheduler firmware and GuC transport initialized before accelerator enable");

	reinterpret_cast<LifecycleMethod>(callback->ioGraphicsEnableAccelerator)(that);

	// The next two calls in the UUID-pinned native tail attach the accelerator's
	// embedded completion events to IOAccelEventMachineFast2.  They are software
	// lifecycle, not engine programming, and omitting them leaves later stamp and
	// teardown paths operating on events that were never admitted by the event
	// machine.
	auto *eventMachine = getMember<void *>(that, 0x380);
	if (!eventMachine) {
		vfMarkProtocolFault("missing VF event machine during engine start");
		return false;
	}
	using InitEventMethod = void (*)(void *, void *);
	auto initEvent = reinterpret_cast<InitEventMethod>(
		callback->ioAccelEventMachineInitEvent);
	initEvent(eventMachine, reinterpret_cast<uint8_t *>(that) + 0x11A8);
	initEvent(eventMachine, reinterpret_cast<uint8_t *>(that) + 0x11E8);
	SYSLOG("ngreen", "V245: enabled VF bridge/IOAccelerator and initialized completion events");
	return true;
}

bool Gen11::stopGraphicsEngine(void *that)
{
	if (!that || !callback || !callback->vfInterruptBridgeDisable ||
	    !callback->ioGraphicsDisableAccelerator) {
		vfMarkProtocolFault("VF engine stop without accelerator lifecycle API");
		return false;
	}
	// Preserve the native software timer cancellation before shutdown. The
	// pinned constructor stores a base IOTimerEventSource at +0x1460.
	// This invalidates scheduled work; it does not drain callbacks or prove
	// DMA quiescence. Do not copy native +0x1458=1 (a reported idle state).
	auto *dpsmTimer = getMember<IOTimerEventSource *>(that, 0x1460);
	if (dpsmTimer)
		dpsmTimer->cancelTimeout();
	if (gVfDeviceStopping && gVfGGTTReady) {
		PANIC_COND(!vfQuiesceDeviceForShutdown(gVfHardwareGuc), "ngreen",
			"Cannot stop VF accelerator before every GuC context and DMA mapping is quiesced");
		SYSLOG("ngreen", "V242: VF final engine stop completed through GuC context retirement");
	}

	auto *interruptBridge = getMember<void *>(that, 0x1248);
	if (!interruptBridge) {
		vfMarkProtocolFault("missing VF interrupt bridge during engine stop");
		return false;
	}
	using LifecycleMethod = void (*)(void *);
	// Preserve the native order after any final VF DMA quiescence.  On
	// TGL/ADL/RPL this also disables the VF-owned virtual-MMIO interrupt block;
	// on MTL/ARL the routed bridge masks the memory-IRQ page.
	reinterpret_cast<LifecycleMethod>(callback->vfInterruptBridgeDisable)(
		interruptBridge);
	OSCompareAndSwap(1, 0, &gVfMmioIrqReady);
	OSSynchronizeIO();
	reinterpret_cast<LifecycleMethod>(callback->ioGraphicsDisableAccelerator)(that);
	SYSLOG("ngreen", "V244: disabled VF interrupt bridge and IOAccelerator lifecycle");
	return true;
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

	return FunctionCast(submitBlit, callback->osubmitBlit)(that, param_1, param_2, param_3, param_4);
}

void Gen11::barrierSubmission(void *queue, void *accelerator, void *cmdDesc,
								void *event, uint16_t count, const uint16_t *list) {
	// Both direct callers in the pinned Tahoe payload ignore RAX, and the native
	// body has return paths that do not establish a value: this is a void ABI.
	// Therefore no wrapper rejection can be reported to the caller. Fail-stop
	// before it continues without the mandatory barrier side effects.
	PANIC_COND(!queue || !accelerator || !cmdDesc || !event ||
		(count && !list) || !callback || !callback->obarrierSubmission,
		"ngreen", "Invalid Tahoe barrier submission arguments or trampoline");
	if (gVfIdentity == VfIdentity::Virtual) {
		PANIC_COND(!vfNativeGpuWorkReady(), "ngreen",
			"VF barrier reached without an admitted GPU transport");

		// The Tahoe body unconditionally dereferences 2D, 3D and depth FIFO
		// pointers and may dereference color as well. Prime every native context;
		// if any allocation is missing, the void caller cannot recover or observe
		// an error, so continuing would fabricate successful barrier progress.
		void *task = getMember<void *>(cmdDesc, 0x8);
		void *blit2D = task ? getBlit2DContext(task, true) : nullptr;
		void *blit3D = task ? getBlit3DContext(task, true) : nullptr;
		void *depth = task ? getDepthResolveContext(task, true) : nullptr;
		void *color = task ? getColorResolveContext(task, true) : nullptr;
		const bool incomplete = !blit2D || !blit3D || !depth || !color ||
		    !getMember<void *>(blit2D, 0xb8) ||
		    !getMember<void *>(blit3D, 0xb8) ||
		    !getMember<void *>(depth, 0xb8) ||
		    !getMember<void *>(color, 0xb8);
		if (incomplete) {
			SYSLOG("ngreen", "V243: rejected VF barrier with incomplete contexts task=%p 2D=%p 3D=%p depth=%p color=%p",
			       task, blit2D, blit3D, depth, color);
		}
		PANIC_COND(incomplete, "ngreen",
			"VF barrier cannot continue with incomplete native contexts");
	}
	FunctionCast(barrierSubmission, callback->obarrierSubmission)(
		queue, accelerator, cmdDesc, event, count, list);
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

bool Gen11::loadGuCBinary(void *that) {
	// The PF already owns and runs GuC for an SR-IOV VF. Report firmware as
	// available so Apple's GuC scheduler initializes its submission transport,
	// but never try to replace the PF-owned image or WOPCM configuration.
	// This route is installed only for the UUID-pinned VF payload.
	if (gVfIdentity != VfIdentity::Virtual || !gVfGGTTReady || gVfProtocolFault)
		return false;
	// The stock load routine initializes all scheduler-side allocations before
	// touching WOPCM and uploading firmware. Skipping it outright left
	// contextCount at zero, so the first createUkContext returned the 0x400
	// invalid-context sentinel and tore the GuC object down.
	if (!that || !callback->orgInitSchedControl || !vfCanUseSleepingLock() ||
	    !getMember<void *>(that, 0x38) || !getMember<IOLock *>(that, 0x40) ||
	    !getMember<IOLock *>(that, 0xA08)) {
		vfAbortSchedulerBootstrap("missing VF scheduler initializer, owner or locks");
		return false;
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
		vfAbortSchedulerBootstrap("incomplete native VF scheduler storage allocation");
		return false;
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

bool Gen11::vfRejectLegacyGucMessage(void *that, const void *message,
	                                 unsigned int flags, void *completion) {
	(void)that;
	(void)message;
	(void)flags;
	(void)completion;
	vfMarkProtocolFault("legacy IGGuC host message reached a VF");
	PANIC_COND(true, "ngreen", "Refusing PF-owned legacy GuC MMIO on a VF");
	return false;
}

bool Gen11::vfRejectLegacyGucDma(void *that, uint64_t address,
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

void Gen11::vfRejectLegacyDoorbell(void *that, IGHwCsType hwCsType) {
	(void)that;
	(void)hwCsType;
	vfMarkProtocolFault("legacy IGGuC doorbell reached a VF");
	PANIC_COND(true, "ngreen", "Refusing PF-owned legacy GuC doorbell on a VF");
}

bool Gen11::vfRejectNativeCtbAction(void *that, const uint32_t *request,
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

void Gen11::vfRejectLegacyExecList(void *that, unsigned int tail) {
	(void)that;
	(void)tail;
	vfMarkProtocolFault("legacy execlist submission reached a VF");
	PANIC_COND(true, "ngreen", "Refusing PF-owned execlist submission on a VF");
}

// Modern contexts never update Apple's legacy proxy work-queue idle fields.
// Only states that cannot currently execute provide an idle snapshot. Enabled
// contexts remain conservatively busy until a real completion/idle mechanism
// is implemented. This is NOT a submission barrier or device-DMA-stop proof.
// Tahoe IOAccel's Fast2 termination callback CPU-writes software values into
// the same stamp storage. A future stamp-based completion mechanism must
// exclude termination/restart/fault paths; stamp advancement alone is not GPU
// completion evidence (see the hash-pinned local KC contract).
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
	// SR-IOV protocol. This ABI is void, so merely recording a fault would let
	// its many native callers continue as if ownership had changed. Fail before
	// any caller can publish or release pages under that false assumption.
	PANIC_COND(!that || gVfIdentity != VfIdentity::Virtual ||
		(getMember<uint8_t>(that, 0x1190) & kVfLegacyPageOwnershipFlag), "ngreen",
		"Unsupported legacy page-ownership transfer on a VF");
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

bool Gen11::vfAttachContextDesc(void *that, const uint32_t *descriptor) {
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
			return false;
		}
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
	return true;
}

void Gen11::vfDetachContextDesc(void *that, const uint32_t *descriptor) {
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
	IOInterruptState admissionState = IOSimpleLockLockDisableInterrupt(gVfContextLock);
	const int32_t admittedSlot = vfFindContextLocked(lrcaPage);
	const bool admitted = admittedSlot >= 0 && gVfContexts[admittedSlot].refCount &&
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
	OSObject *admittedBacking = admitted ? gVfContexts[admittedSlot].contextBacking : nullptr;
	OSObject *admittedRingBacking = admitted ? gVfContexts[admittedSlot].ringBacking : nullptr;
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
	PANIC_COND(!gVfHardwareGuc || !vfInvalidateTLBSync(
		gVfHardwareGuc, NGVfGuCRequest::TlbTarget::Guc),
		"ngreen", "Required IGGuC VF TLB invalidation did not complete");
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
	if (!event.valid()) {
		SYSLOG("ngreen", "V238: unsupported or malformed GuC event action=0x%04x len=%u hxg=0x%08x",
		       action, length, hxg);
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
		if (NGVfGuCEvent::expectedTlbCompletion(
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
		vfMarkProtocolFault(event.kind == NGVfGuCEvent::Kind::ContextReset ?
		                         "GuC context reset notification" :
		                         "GuC engine failure notification");
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



void Gen11::wrapSafeForceWake(void *that, bool set, uint32_t dom)
{
	forceWake(that, set, dom, 0);
}

void Gen11::forceWake(void *that, bool set, uint32_t dom, uint32_t ctx)
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
bool Gen11::injectAcceleratorPersonality(const char *bundleId)
{
	if (this->acceleratorPersonalityInjected) {
		DBGLOG("ngreen", "injectAcceleratorPersonality: already injected, skipping");
		return true;
	}

	if (!bundleId || !gIOCatalogue)
		return false;

	// Clone the complete personality already admitted with this exact payload.
	// Reconstructing it by hand had silently dropped Development, Debug and both
	// HEVC capability dictionaries, making display and media behaviour diverge
	// from the bundled Tahoe driver.
	auto *matching = OSDictionary::withCapacity(1);
	auto *bundle = OSString::withCString(bundleId);
	if (!matching || !bundle) {
		OSSafeReleaseNULL(bundle);
		OSSafeReleaseNULL(matching);
		return false;
	}
	const bool matchingReady = matching->setObject("CFBundleIdentifier", bundle);
	bundle->release();
	if (!matchingReady) {
		matching->release();
		return false;
	}

	SInt32 generation = 0;
	auto *drivers = gIOCatalogue->findDrivers(matching, &generation);
	matching->release();
	if (!drivers || drivers->getCount() != 1) {
		SYSLOG("ngreen", "injectAcceleratorPersonality: expected one %s personality, found %u",
			bundleId, drivers ? drivers->getCount() : 0);
		OSSafeReleaseNULL(drivers);
		return false;
	}

	auto *source = OSDynamicCast(OSDictionary, drivers->getObject(0));
	const bool complete = source &&
		OSDynamicCast(OSDictionary, source->getObject("Development")) &&
		OSDynamicCast(OSDictionary, source->getObject("Debug")) &&
		OSDynamicCast(OSDictionary, source->getObject("IOAccelDisplayPipeCapabilities")) &&
		OSDynamicCast(OSDictionary, source->getObject("IOGVAHEVCDecodeCapabilities")) &&
		OSDynamicCast(OSDictionary, source->getObject("IOGVAHEVCEncodeCapabilities"));
	auto *dict = complete ? OSDictionary::withDictionary(source) : nullptr;
	drivers->release();
	if (!dict) {
		SYSLOG("ngreen", "injectAcceleratorPersonality: native personality is incomplete");
		return false;
	}

	// OSDictionary::withDictionary is shallow. Clone Development separately so
	// forcing the VF-only telemetry policy cannot mutate the admitted native PF
	// personality retained by the catalogue.
	auto *sourceDevelopment =
		OSDynamicCast(OSDictionary, dict->getObject("Development"));
	auto *development = sourceDevelopment ?
		OSDictionary::withDictionary(sourceDevelopment) : nullptr;
	auto *telemetryDisabled = OSNumber::withNumber(1ULL, 32);
	const bool telemetryReady = development && telemetryDisabled &&
		development->setObject("TelemetryDisable", telemetryDisabled) &&
		dict->setObject("Development", development);
	OSSafeReleaseNULL(telemetryDisabled);
	OSSafeReleaseNULL(development);
	if (!telemetryReady) {
		dict->release();
		return false;
	}

	auto *primaryMatch = OSString::withCString("0x9a498086");
	const bool matchReady = primaryMatch &&
		dict->setObject("IOPCIPrimaryMatch", primaryMatch);
	OSSafeReleaseNULL(primaryMatch);
	if (!matchReady) {
		dict->release();
		return false;
	}

	auto *array = OSArray::withCapacity(1);
	const bool arrayReady = array && array->setObject(dict);
	dict->release();
	if (!arrayReady) {
		OSSafeReleaseNULL(array);
		return false;
	}

	SYSLOG("ngreen", "injectAcceleratorPersonality: publishing complete %s personality",
		bundleId);
	const bool ok = gIOCatalogue->addDrivers(array, true);
	array->release();
	this->acceleratorPersonalityInjected = ok;
	return ok;
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

void *Gen11::vfRejectEventTimeout(void *that, int32_t channel) {
	(void)that;
	// V267: inherited restart clears its software error and restarts the timer
	// even when eventTimeout returns null. Do not return into that retry or
	// fabricate completion while a direct GuC context may still DMA. Guest
	// fail-stop is not PF containment; the independent host watcher is required.
	vfMarkProtocolFault("VF native event timeout without verified GPU quiescence");
	PANIC_COND(true, "ngreen", "V267: refusing VF event timeout recovery channel=%d", channel);
	return nullptr;
}

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
