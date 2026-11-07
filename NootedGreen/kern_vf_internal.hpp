//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

// State and helpers shared by the VF sources (kern_gen11.cpp, kern_vf_*.cpp).

#ifndef kern_vf_internal_hpp
#define kern_vf_internal_hpp

#include "kern_gen11.hpp"
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <sys/proc.h>
#include <kern/thread.h>
#include "kern_guc_ring.hpp"
#include "kern_vf_cpu_range.hpp"
#include "kern_gpu_capabilities.hpp"
#include "kern_ggtt_bounds.hpp"
#include "kern_ggtt_rotation.hpp"
#include "kern_vf_ggtt_pte.hpp"
#include "kern_vf_irq_gate.hpp"
#include "kern_vf_accelerator_stop.hpp"
#include "kern_vf_context_shutdown.hpp"
#include "kern_vf_submission_gate.hpp"
#include "kern_avc_gen12_commands.hpp"
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
#include <Headers/kern_util.hpp>
#include "kern_green.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSSet.h>
#include <kern/thread_call.h>
#include <kern/sched_prim.h>
#include <i386/machine_routines.h>

// Current admission covers known media-12 direct VF GGTT platforms only.
// Media-13 requires its own per-GT IP/ABI discovery; BAR size alone is not an
// admission discriminator and no speculative relay fallback is retained.
constexpr uint32_t kVfGGTTPteBase = 0x800000;

constexpr uint32_t kVfDirectBar0Bytes = kVfGGTTPteBase * 2;

// intel_guc.h: addresses at/above this limit bypass GGTT translation.
constexpr uint64_t kGucGgttTop = 0xFEE00000ULL;

constexpr uint32_t kGucOriginGuc = 0x80000000U;

constexpr uint32_t kGucTypeMask = 0x70000000U;

constexpr uint32_t kGucTypeRetry = 0x50000000U;

constexpr uint32_t kGucTypeFailure = 0x60000000U;

constexpr uint32_t kGucTypeSuccess = 0x70000000U;

constexpr uint32_t kGucActionHost2GucControlCtb = 0x4509;

constexpr uint32_t kGucActionRegisterContext = NGVfGuCRequest::registerContext;

constexpr uint32_t kGucActionDeregisterContext = NGVfGuCRequest::deregisterContext;

constexpr uint32_t kGucActionScheduleContext = NGVfGuCRequest::scheduleContext;

constexpr uint32_t kGucActionScheduleContextModeSet =
	NGVfGuCRequest::scheduleContextModeSet;

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

extern IORecursiveLock *gVfPageTableUpdateLock;

extern uint64_t gVfGGTTBase;

extern uint64_t gVfGGTTSize;

extern uint32_t gVfDoorbellCount;

extern void *gVfGlobalPageTable;

extern void *gVfHardwareGuc;

extern void *gVfAccelerator;

extern OSObject *gVfStopMemoryManager;

extern void *gVfStopMemoryManagerAccelerator;

extern IOThread gVfStopMemoryManagerThread;

extern mach_vm_address_t gIOAccelCommandPoolInit;

extern bool gVfGGTTReady;

extern NGVfRuntime::Registers gVfRuntimeRegisters;

extern NGVfRuntime::Topology gVfTopology;

extern volatile UInt32 gVfRuntimeReady;

enum class VfIdentity : uint8_t { Unknown, Physical, Virtual, Invalid };

extern VfIdentity gVfIdentity;

VfIdentity vfIdentifyDevice();

extern uint32_t gVfCtbGpuBase;

extern uint8_t *gVfCtbCpuBase;

extern OSObject *gVfCtbBacking;

extern OSObject *gVfCtbObject;

extern bool gVfCtbDisableConfirmed;

extern volatile UInt32 gVfCtbStopped;

extern volatile UInt32 gVfCtbEnabled;

extern volatile UInt32 gVfCtbEverEnabled;

extern volatile UInt32 gVfSubmissionStopped;

extern volatile UInt32 gVfIrqCallbackGate;

extern volatile UInt32 gVfExternalProducerGate;

extern ThreadLocal<UInt32, 1> gVfDeviceCacheRetirementDepth;

extern volatile UInt32 gVfContextShutdownStarted;

extern volatile UInt32 gVfDmaQuiesced;

extern volatile UInt32 gVfDeviceStopping;

extern volatile UInt32 gVfSchedulerFirmwareReady;

extern bool gVfUsesMemoryIrq;

extern bool gVfMemIrqConfigured;

extern volatile UInt32 gVfMemIrqRequested;

extern volatile UInt32 gVfMmioIrqReady;

extern volatile UInt32 gVfProtocolFault;

extern volatile UInt32 gVfGgttDiagnosticWrites;

extern volatile UInt32 gVfTaskConstructionDiagnosticStages;

extern volatile UInt32 gVfTlbWaitActive;

extern volatile UInt32 gVfTlbWaitSeqno;

extern volatile UInt32 gVfTlbDoneSeqno;

bool vfInterruptTransportReady();

bool vfNativeGpuWorkReady();

bool vfCtbConsumerReady(bool requireInterrupt);

void vfMarkProtocolFault(const char *reason);

void vfAbortSchedulerBootstrap(const char *reason);

bool vfCanUseSleepingLock();

void vfReleaseG2HCredits(uint32_t count);

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
	uint32_t stampIndex;
	bool stampRedirected;
	// V346: stamp values this context's redirected stores will write, oldest
	// first. When full, recording stops until the newest unrecorded stamp is
	// done, so the front stays exact. Storage comes from gVfPendingStampPool
	// while the context is redirected.
	uint16_t pendingStampHead, pendingStampCount;
	bool pendingStampLost;
	uint32_t pendingStampLostUpTo;
	uint32_t *pendingStamps;
	NGVfSubmissionCoverage::Tracker submissionCoverage;
};

void vfEndStampRedirectLocked(VfGucContext &entry);

extern IOSimpleLock *gVfContextLock;

extern VfGucContext *gVfContexts;

extern uint32_t gVfContextCapacity;

extern uint32_t gVfContextLifecycleLogs;

extern NGVfTimeoutObservation::Reference gVfTimeoutReferences[NGVfTimeoutObservation::engineCount];

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

constexpr uint32_t kGucContextRegistrationFlagKmd = 1;

constexpr uint32_t kGucContextDisable = 0;

constexpr uint32_t kGucContextEnable = 1;

bool vfLegacyProxyPoolValid(void *that, mach_vm_address_t getter, uint32_t *usedOut = nullptr, uint32_t *countOut = nullptr);

int32_t vfFindContextLocked(uint32_t lrcaPage);

int32_t vfReserveContextLocked(uint32_t lrcaPage);

void vfReleaseRetiredContextBacking(uint16_t gucId);

bool vfEnsurePageTableUpdateLock();

bool vfValidG2HMapping(const volatile uint32_t *descriptor,
                       const volatile uint32_t *buffer);

bool vfInvalidateTLBSync(void *guc, NGVfGuCRequest::TlbTarget target);

bool vfQuiesceDeviceForShutdown(void *guc);

bool vfGucSendMMIO(const uint32_t request[4], uint32_t requestLength,
                   uint32_t response[4]);

bool vfCaptureHardwareGuc(void *guc);

bool vfInitContextBridge();

bool vfSendCtbFastAction(void *guc, const uint32_t *request, uint32_t requestLength, uint32_t &transportFence, IOLock *alreadyHeldQueue = nullptr, volatile uint32_t *contextTailField = nullptr, uint32_t contextTail = 0);

bool vfSetContextPolicy(void *guc, uint16_t gucId, uint8_t engineClass, uint32_t &transportFence);

bool vfCanWaitForGuc(void *guc);

bool vfEnterIrqCallback();

void vfLeaveIrqCallback();

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

bool vfCloseIrqCallbackGateAndWait(void *guc);

bool vfEnterExternalProducer();

void vfLeaveExternalProducer();

class VfExternalProducerGuard {
public:
	explicit VfExternalProducerGuard(void *accelerator, bool route = true) :
		tracked(false), admitted(true) {
		OSSynchronizeIO();
		tracked = route && gVfIdentity == VfIdentity::Virtual &&
		          accelerator && gVfAccelerator &&
		          accelerator == gVfAccelerator;
		admitted = !tracked || vfEnterExternalProducer();
	}

	~VfExternalProducerGuard() {
		if (tracked && admitted)
			vfLeaveExternalProducer();
	}

	VfExternalProducerGuard(const VfExternalProducerGuard &) = delete;
	VfExternalProducerGuard &operator=(const VfExternalProducerGuard &) = delete;
	explicit operator bool() const { return admitted; }

private:
	bool tracked;
	bool admitted;
};

class VfDeviceCacheRetirementScope {
public:
	explicit VfDeviceCacheRetirementScope(void *accelerator) : tracked(
		gVfIdentity == VfIdentity::Virtual && accelerator &&
		gVfAccelerator && accelerator == gVfAccelerator) {
		if (tracked) {
			auto *depth = gVfDeviceCacheRetirementDepth.get();
			if (depth) {
				PANIC_COND(*depth == UINT32_MAX, "ngreen",
					"VF device-cache retirement scope overflow");
				++*depth;
			} else {
				PANIC_COND(!gVfDeviceCacheRetirementDepth.set(1), "ngreen",
					"Concurrent VF device-cache retirement owner");
			}
			OSSynchronizeIO();
		}
	}

	~VfDeviceCacheRetirementScope() {
		if (tracked) {
			OSSynchronizeIO();
			auto *depth = gVfDeviceCacheRetirementDepth.get();
			PANIC_COND(!depth || *depth == 0, "ngreen",
				"Unbalanced VF device-cache retirement scope");
			const bool outermost = *depth == 1;
			--*depth;
			if (outermost)
				PANIC_COND(!gVfDeviceCacheRetirementDepth.erase(), "ngreen",
					"Cannot release VF device-cache retirement owner");
		}
	}

	VfDeviceCacheRetirementScope(const VfDeviceCacheRetirementScope &) = delete;
	VfDeviceCacheRetirementScope &operator=(const VfDeviceCacheRetirementScope &) = delete;

private:
	bool tracked;
};

bool vfOwnsDeviceCacheRetirement();

NGVfAcceleratorStop::Phase vfAcceleratorStopPhase();

bool vfTransitionAcceleratorStop( NGVfAcceleratorStop::Phase expected, NGVfAcceleratorStop::Phase next);

bool vfTargetsPublishedAccelerator(void *accelerator);

bool vfCloseExternalProducerGateAndWait();

bool vfEnterContextOperation();

void vfLeaveContextOperation();

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

bool vfWaitForContextShutdown(void *guc);

void vfSlowMark(const char *n);

extern volatile uint32_t gVfTlbInvalidations;

bool vfInvalidateTLBSync(void *guc, NGVfGuCRequest::TlbTarget target);

bool vfWaitForContextState(void *guc, uint16_t gucId, VfGucContextState wanted);

bool vfWaitForContextTransition(void *guc, uint16_t gucId, uint32_t lrcaPage, VfGucContextState previous);

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

constexpr uint32_t kVfCtbH2GBufferOffset = 0x2000;

constexpr uint32_t kVfCtbH2GBufferBytes = 0x1000;

constexpr uint32_t kVfCtbG2HBufferOffset = 0x3000;

constexpr uint32_t kVfCtbG2HBufferBytes = 0x4000;

// MTL/ARL's VF ABI requires a memory-interrupt page in GGTT. Reuse the final,
// page-aligned 4 KiB of the enlarged CTB allocation so CT transport and IRQ
// state share one mapping and no extra IGMappedBuffer allocation is needed.
// TGL/ADL/RPL use virtual interrupt MMIO and never publish or modify this page;
// retaining one allocation shape keeps the private Tahoe CTB ABI deterministic.
constexpr uint32_t kVfMemIrqOffset = 0x7000;

constexpr uint32_t kVfMemIrqEnableOffset = NGVfMemIrq::enableOffset;

extern bool gVfCtbAllocationPending;

extern void *gVfCtbInitOwner;

extern IOThread gVfCtbAllocationThread;

extern OSObject *gVfCtbAllocatedBacking;

bool vfValidG2HMapping(const volatile uint32_t *descriptor, const volatile uint32_t *buffer);

bool vfStopSubmissionAndSealCtb(void *guc);

bool vfRollbackFailedPostCtbBootstrap(void *guc);

bool vfQuiesceDeviceForShutdown(void *guc);

extern uint8_t *(*gVfStampGetter)(void *);

// V354: submissions that hold up the caller (WindowServer blocks in
// IOAccelCommandQueueSubmitCommandBuffers; 3-12 ms stalls drop frames).
// Nested hooks on the same thread add marks through vfSlowMark.
struct VfSlowSubmitLog;

struct VfSlowSubmitThread {
	thread_t thread;
	VfSlowSubmitLog *log;
};
extern VfSlowSubmitThread gVfSlowSubmitThreads[32];

void vfSlowMark(const char *n);

struct VfSlowSubmitLog {
	// Time between consecutive marks, summed per mark name.
	static constexpr uint32_t kNames = 40;
	const char *what;
	uint64_t start = mach_absolute_time(), last = start;
	uint64_t total[kNames] {};
	uint32_t count[kNames] {};
	const char *name[kNames] {};
	uint32_t names = 0;
	VfSlowSubmitThread *slot = nullptr;
	explicit VfSlowSubmitLog(const char *w) : what(w) {
		const auto self = current_thread();
		for (auto &t : gVfSlowSubmitThreads)
			if (OSCompareAndSwapPtr(nullptr, self, &t.thread)) {
				t.log = this, slot = &t;
				break;
			}
	}
	void mark(const char *n) {
		const uint64_t now = mach_absolute_time();
		uint32_t i = 0;
		while (i < names && name[i] != n)
			i++;
		if (i == names && names < kNames)
			name[names++] = n;
		if (i < names)
			total[i] += now - last, count[i]++;
		last = now;
	}
	~VfSlowSubmitLog() {
		if (slot)
			slot->log = nullptr, OSCompareAndSwapPtr(slot->thread, nullptr, &slot->thread);
		const uint64_t end = mach_absolute_time();
		uint64_t ns;
		absolutetime_to_nanoseconds(end - start, &ns);
		static volatile UInt32 logs = 0;
		if (ns < 3000000 || logs >= 300)
			return;
		OSIncrementAtomic(&logs);
		char line[1024], process[20] {};
		proc_selfname(process, sizeof(process));
		int used = snprintf(line, sizeof(line), "V354: %s by %s took %llu us:", what, process, ns / 1000);
		for (uint32_t i = 0; i < names && used > 0 && used < static_cast<int>(sizeof(line)); i++) {
			uint64_t part;
			absolutetime_to_nanoseconds(total[i], &part);
			used += snprintf(line + used, sizeof(line) - used, " %s %llu/%u", name[i], part / 1000, count[i]);
		}
		uint64_t rest;
		absolutetime_to_nanoseconds(end - last, &rest);
		if (used > 0 && used < static_cast<int>(sizeof(line)))
			snprintf(line + used, sizeof(line) - used, " rest %llu", rest / 1000);
		SYSLOG("ngreen", "%s", line);
	}
};

void vfSlowMark(const char *n);

extern const char *const gVfSubmitProbeNames[3][2];

extern mach_vm_address_t gVfSubmitProbeOrg[3];

template <size_t N>
uint64_t vfSubmitProbe(void *a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
	vfSlowMark(gVfSubmitProbeNames[N][0]);
	const uint64_t result = reinterpret_cast<uint64_t (*)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)>(
		gVfSubmitProbeOrg[N])(a, b, c, d, e, f);
	vfSlowMark(gVfSubmitProbeNames[N][1]);
	return result;
}

void vfMergeRedirectedStamps(bool dump = false);

void vfRedirectStamps(uint32_t id, volatile uint32_t *ring, uint32_t ringSize, uint32_t from, uint32_t to, uint8_t *(*getter)(void *), uint64_t (*getGpu)(void *));

void vfSkipResetContext(uint32_t id, mach_vm_address_t getterAddress);

uint64_t vfConsumeMemoryInterrupts();

bool vfG2HCtbPending(uint32_t &head);

bool vfEnsurePageTableUpdateLock();

bool vfAdmitPagePoolTransactionOwner(void *pool);

bool vfNativeCallbackBindingsReady(void *accelerator);

bool vfDetachNativeCallbackSourcesBeforeBaseStop(void *accelerator);

bool vfDetachPartialNativeCallbackSources(void *accelerator);

bool vfGucSendMMIO(const uint32_t request[4], uint32_t requestLength, uint32_t response[4]);

bool vfConfigureModernCtb(bool g2h, uint32_t appleDescriptorAddress);

bool vfBootstrapDirectGgtt();

bool vfQuiesceVirtualInterruptsBeforeMsi();

volatile uint64_t *vfDirectPteBase(void *that);

volatile uint64_t *vfDirectPteBase(void *that);

void vfDumpResetContext(uint32_t id, mach_vm_address_t getterAddress);

void vfWakeVideoBatch(uint64_t root, volatile uint32_t *ring, uint32_t ringSize, uint32_t from, uint32_t to);

bool vfKnownIdleSnapshot(mach_vm_address_t mappedBufferGetter, const uint32_t *descriptor = nullptr);

uint64_t vfPpgttLeaf(uint64_t root, uint64_t va);

bool vfPpgttRead(uint64_t root, uint64_t va, uint32_t *out, uint32_t count);

void vfWakeVideoBatch(uint64_t root, volatile uint32_t *ring, uint32_t ringSize, uint32_t from, uint32_t to);

void vfDumpPsSurfaces(uint32_t id, uint64_t root, volatile uint32_t *page, uint64_t &surfaceBase);

void vfDumpResetContext(uint32_t id, mach_vm_address_t getterAddress);

extern "C" bool ngVfCommandPoolInitBridge(void *, void *, void *, void *, uint32_t,
	uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

namespace V373 {
void install(KernelPatcher &patcher);
}

#endif
