// Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit
// License version 1.0. See LICENSE for details.
#ifndef kern_gen11_hpp
#define kern_gen11_hpp

#include "kern_green.hpp"
#include "kern_patcherplus.hpp"
#include <IOKit/IOInterruptEventSource.h>

// AppleIntelTGLGraphics' two-qword virtual-address range.
struct NGIGAddressRange {
	uint64_t start;
	uint64_t length;
};

// Exact six-entry IGHwCsType jump table in the admitted Tahoe TGL payload.
enum IGHwCsType {
	kIGHwCsTypeRCS,
	kIGHwCsTypeCCS,
	kIGHwCsTypeBCS,
	kIGHwCsTypeVCS0,
	kIGHwCsTypeVCS2,
	kIGHwCsTypeVECS0,
};

class Gen11 {
private:
	static bool loadGuCBinary(void *that);
	static bool vfMmioHostToGuCAction(void *that, const uint32_t *request,
	                                  unsigned int requestLength, int timeout,
	                                  uint32_t *response);
	static bool vfLegacyHostToGuCAction(void *that, const uint32_t *request,
	                                    unsigned int requestLength, int timeout,
	                                    uint32_t *response);
	static uint32_t vfCreateUkContext(void *that, uint64_t owner, int priority);
	mach_vm_address_t vfAllocContext {};
	mach_vm_address_t vfReleaseContext {};
	mach_vm_address_t vfSharedMappedBufferWithOptions {};
	mach_vm_address_t vfWorkQueueWithOptions {};
	static bool vfWorkQueueInit(void *that, void *accelerator, uint32_t id,
	                            void *process);
	mach_vm_address_t oVfWorkQueueInit {};
	static void vfWorkQueueFree(void *that);
	mach_vm_address_t oVfWorkQueueFree {};
	mach_vm_address_t vfOSObjectFree {};
	static void vfCtbFree(void *that);
	mach_vm_address_t oVfCtbFree {};
	static uint32_t vfAllocContextId(void *that, uint64_t owner, bool clear);
	static void vfReleaseContextId(void *that, uint32_t id);
	static uint16_t vfAcquireDoorbell(void *that, void *descriptor, bool pin);
	static void vfReleaseDoorbell(void *that, void *descriptor);
	static bool vfAllocUkDoorbell(void *that, uint32_t contextId, bool pin);
	static uint16_t vfReacquireDoorbell(void *that, uint32_t contextId);
	static bool vfIsGuCIdle(void *that);
	static bool vfIsContextIdle(void *that, uint32_t contextId);
	static bool vfIsKmdContextIdle(void *that, const uint32_t *descriptor);
	static void vfTransferOwnership(void *that, const void *backing, int owner);
	static void vfInitDoorbells(void *that);
	static bool vfReadDoorbellSQIDIConfig(void *that);
	static bool vfCtbInitWithAccelerator(void *that, void *accelerator);
	mach_vm_address_t oVfCtbInitWithAccelerator {};
	static void vfCtbChannelInit(void *that);
	static bool vfCtbGucToHostAction(void *that, uint32_t *message);
	static void vfSoftwareGuCInterrupt(void *that,
	                                   IOInterruptEventSource *source,
	                                   int count);
	mach_vm_address_t vfCtbSoftwareInterrupt {};
	static void vfInvalidateTLB(void *that);
	static void vfBaseInvalidateTLB(const void *that);
	static bool vfInterruptFilterHandler(void *that, void *eventSource);
	mach_vm_address_t vfServiceInterrupts {};
	static void vfReadAndClearInterrupts(void *that, void *interrupts);
	static void vfEnableInterrupts(void *that);
	static void vfDisableInterrupts(void *that);
	static void *vfCtbMappedBufferWithOptions(void *accelTask,
	                                          unsigned long size,
	                                          unsigned int type,
	                                          unsigned int flags);
	mach_vm_address_t oVfCtbMappedBufferWithOptions {};
	static bool vfAttachContextDesc(void *that, const uint32_t *descriptor);
	static void vfDetachContextDesc(void *that, const uint32_t *descriptor);
	static bool vfSubmitWorkItem(void *that, unsigned int legacyContextId,
	                             const uint32_t *descriptor,
	                             IGHwCsType hwCsType,
	                             unsigned int channelId,
	                             unsigned int ringSequence,
	                             unsigned int ringTail);
	mach_vm_address_t vfSharedMappedBufferGetVirtualAddress {};
	mach_vm_address_t vfMappedBufferGetGPUVirtualAddress {};

	static bool start(void *that, void *provider);
	static void acceleratorStop(void *that, void *provider);
	mach_vm_address_t ostart {};
	mach_vm_address_t oAcceleratorStop {};
	static void *igAccelTaskWithOptions(void *that);
	mach_vm_address_t oigAccelTaskWithOptions {};
	mach_vm_address_t igAccelTaskCounter {};
	static bool IGAccelTaskIsKernelGPUTask(const void *that);
	mach_vm_address_t oIGAccelTaskIsKernelGPUTask {};
	static bool submitBlit(void *that, void *params, void *rects, void *task,
	                       bool synchronous);
	mach_vm_address_t osubmitBlit {};
	static void forceWake(void *that, bool set, uint32_t domain, uint32_t context);
	static void wrapSafeForceWake(void *that, bool set, uint32_t domain);
	mach_vm_address_t orgInitSchedControl {};

	static bool IGHardwareGlobalPageTableInitWithOptions(
		void *that, void *accelerator, const NGIGAddressRange &range,
		void *mmioBase, uint64_t dummyPage, uint32_t options);
	mach_vm_address_t oIGHardwareGlobalPageTableInitWithOptions {};
	static bool IGMemoryManagerInitSegments(void *that);
	static bool IGHardwareGlobalPageTableMapRange(
		void *that, const NGIGAddressRange &range, uint64_t physical,
		uint64_t flags);
	static bool IGHardwareGlobalPageTableMapRangeRotated(
		void *that, void *rangeIterator, void *physicalIterator, uint64_t flags);
	static void IGHardwareGlobalPageTableUnmapRange(
		void *that, const NGIGAddressRange &range);
	static bool IGHardwareGlobalPageTableMapRangeDummy(
		void *that, const NGIGAddressRange &range, uint64_t flags);

	mach_vm_address_t oIGMappedBuffergetMemory {};
	static void *getBlit2DContext(void *that, bool create);
	mach_vm_address_t ogetBlit2DContext {};
	static void *getDepthResolveContext(void *that, bool create);
	mach_vm_address_t ogetDepthResolveContext {};
	static void *getColorResolveContext(void *that, bool create);
	mach_vm_address_t ogetColorResolveContext {};
	static void *getBlit3DContext(void *that, bool create);
	mach_vm_address_t ogetBlit3DContext {};

	bool injectAcceleratorPersonality(const char *bundleId);
	bool acceleratorPersonalityInjected {false};
	mach_vm_address_t ioGraphicsEnableAccelerator {};
	mach_vm_address_t ioGraphicsDisableAccelerator {};
	mach_vm_address_t ioAccelEventMachineInitEvent {};
	mach_vm_address_t vfInterruptBridgeEnable {};
	mach_vm_address_t vfInterruptBridgeDisable {};
	static bool stopGraphicsEngine(void *that);
	static bool startGraphicsEngine(void *that);
	static void populateResetRegisterList(void *that);
	static bool wrapIGScheduler5IsGpuIdle(const void *that);
	static bool wrapIGScheduler4IsGpuIdle(const void *that);
	static void barrierSubmission(void *queue, void *accelerator,
	                              void *commandDescriptor, void *event,
	                              uint16_t count, const uint16_t *list);
	mach_vm_address_t obarrierSubmission {};

public:
	void init();
	static Gen11 *callback;
	bool processKext(KernelPatcher &patcher, size_t index,
	                 mach_vm_address_t address, size_t size);
};

#endif
