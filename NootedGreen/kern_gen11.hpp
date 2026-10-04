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
	static bool vfRejectLegacyGucMessage(void *that, const void *message,
	                                     unsigned int flags, void *completion);
	static bool vfRejectLegacyGucDma(void *that, uint64_t address,
	                                 unsigned int size, unsigned int offset,
	                                 unsigned int dmaType, bool wait);
	static void vfRejectLegacyDoorbell(void *that, IGHwCsType hwCsType);
	static bool vfRejectNativeCtbAction(void *that, const uint32_t *request,
	                                    unsigned int requestLength, int timeout,
	                                    uint32_t *response, bool fence);
	static void vfRejectLegacyExecList(void *that, unsigned int tail);
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
	static bool vfSchedulerPush(void *that, void *hardwareContext,
	                            unsigned int ringTail,
	                            unsigned int auxiliary,
	                            bool carriesStamp,
	                            bool hasPendingCommands);
	mach_vm_address_t oVfSchedulerPush {};
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
	static bool vfDrainGuCToHost(void *that,
	                            IOInterruptEventSource *source,
	                            bool synchronousPoll);
	mach_vm_address_t vfCtbSoftwareInterrupt {};
	static void vfRequestEnableCallback(void *that, OSObject *requestor,
	                                    void (*action)(OSObject *, ...));
	mach_vm_address_t oVfRequestEnableCallback {};
	static void vfInvalidateTLB(void *that);
	static void vfBaseInvalidateTLB(const void *that);
	static bool vfInterruptFilterHandler(void *that, void *eventSource);
	mach_vm_address_t vfServiceInterrupts {};
	static void vfReadAndClearInterrupts(void *that, void *interrupts);
	static void vfEnableInterrupts(void *that);
	static void vfDisableInterrupts(void *that);
	static void vfSuppressPhysicalErrorInterrupts(void *that);
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
	mach_vm_address_t vfAccelSysMemoryGetPhysicalSegment {};

	static bool start(void *that, void *provider);
	static void acceleratorStop(void *that, void *provider);
	mach_vm_address_t ostart {};
	mach_vm_address_t oAcceleratorStop {};
	static void vfBaseAcceleratorStop(void *that, void *provider);
	mach_vm_address_t oVfBaseAcceleratorStop {};
	mach_vm_address_t ioPciConfigureInterrupts {};
	static uint32_t vfTelemetryPrintDashboard(void *that, uint64_t options);
	static int vfTelemetryInitWithAccelerator(void *that, void *accelerator,
	                                         uint32_t options);
	static void vfTelemetryRetain(void *that);
	static void vfTelemetryRelease(void *that);
	static uint64_t vfTelemetryCalcGlobalUsage(void *that, uint64_t timestamp);
	static int64_t vfTelemetryOperation(void *that, uint64_t selector,
	                                   int64_t value, void *operation,
	                                   void *connection, void *task);
	static void vfTelemetryPatchContextImage(void *that, void *contextImage);
	static void vfTelemetryOnConnectionStop(void *that, void *connection,
	                                        void *task);
	static IOReturn vfTelemetryInitOaBuffer(void *that, void *connection,
	                                       void *input, void *output,
	                                       uint64_t inputSize,
	                                       uint64_t *outputSize);
	static IOReturn vfTelemetryReadOaBuffer(void *that, void *input,
	                                       void *output, uint64_t *outputSize,
	                                       void *connection);
	static IOReturn vfTelemetryMapOaBufferMemory(void *that, void *input,
	                                            void *output,
	                                            uint64_t inputSize,
	                                            uint64_t *outputSize,
	                                            void *task);
	static void vfTelemetryUsageAlloc(void *that);
	static void vfTelemetryUsageLog(void *that, void *context,
	                                uint64_t timestamp);
	static void vfTelemetryUsageReportGlobal(void *that);
	static bool vfTelemetryUsageStartSample(void *that, void *ring,
	                                        uint32_t stamp, uint32_t engine,
	                                        uint64_t commandId,
	                                        uint64_t submitTime,
	                                        uint64_t startTime,
	                                        uint64_t endTime);
	static void vfTelemetryUsageStopSample(void *that, void *ring,
	                                      uint32_t stamp, uint32_t engine);
	static void vfTelemetryUsageFrameCalc(void *that, void *accelerator,
	                                      uint32_t frame);
	static void vfDisableDebugSysctl(void *that);
	static IOReturn vfRejectPavpCommandCallback(void *that, uint32_t command,
	                                            uint32_t session,
	                                            uint32_t *data, bool recovery);
	static void vfIgnoreTraceRecognizeFlip(void *that);
	static void *vfRejectPhysicalFence(void *that,
	                                   const NGIGAddressRange &range,
	                                   uint64_t pitch, uint32_t tileMode);
	static void vfDisableEdramProbe(void *that);
	static void *igAccelTaskWithOptions(void *that);
	mach_vm_address_t oigAccelTaskWithOptions {};
	mach_vm_address_t igAccelTaskCounter {};
	static bool IGAccelTaskIsKernelGPUTask(const void *that);
	mach_vm_address_t oIGAccelTaskIsKernelGPUTask {};
	static bool submitBlit(void *that, void *params, void *rects, void *task,
	                       bool synchronous);
	mach_vm_address_t osubmitBlit {};
	static bool vfSubmitCCSResolve(void *that, void *entry, void *accelerator,
	                              void *task, uint32_t resolveType,
	                              uint8_t plane, uint8_t level);
	mach_vm_address_t oVfSubmitCCSResolve {};
	static bool vfSubmitDepthResolve(void *that, void *entry,
	                                void *accelerator, void *task,
	                                uint32_t resolveType, uint8_t plane,
	                                uint8_t level, uint16_t width,
	                                uint16_t height);
	mach_vm_address_t oVfSubmitDepthResolve {};
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
	static void vfPpgtt32UnmapRange(
		void *that, const NGIGAddressRange &range);
	mach_vm_address_t oVfPpgtt32UnmapRange {};
	static void vfPpgtt64UnmapRange(
		void *that, const NGIGAddressRange &range);
	mach_vm_address_t oVfPpgtt64UnmapRange {};
	static void vfPpgtt64ShrinkRange(
		void *that, const NGIGAddressRange &range);
	mach_vm_address_t oVfPpgtt64ShrinkRange {};
	static bool vfCommitPageTablesForTask(
		void *that, void *task, void *mapping);
	mach_vm_address_t oVfCommitPageTablesForTask {};
	static bool vfReleasePageTablesForTask(
		void *that, void *task, void *mapping);
	mach_vm_address_t oVfReleasePageTablesForTask {};
	static bool vfUpdatePageTablesForTask(
		void *that, void *task, void *mapping);
	mach_vm_address_t oVfUpdatePageTablesForTask {};
	static void *vfNewPageTableForTask(void *that, void *task);
	mach_vm_address_t oVfNewPageTableForTask {};
	static void vfSynchronizeAllTasks(void *that);
	mach_vm_address_t oVfSynchronizeAllTasks {};
	static void vfSynchronizeEachEntry(
		void *that, const void *source, const NGIGAddressRange &range,
		bool remap);
	static bool vfPpgtt64RemapDescriptor(
		void *that, const NGIGAddressRange &range, void *descriptor);
	mach_vm_address_t oVfPpgtt64RemapDescriptor {};
	mach_vm_address_t vfPageDescriptorRetain {};
	mach_vm_address_t vfPageDescriptorRelease {};
	mach_vm_address_t vfGetHardwareContextAddressMode {};
	mach_vm_address_t vfPpgtt32WithOptions {};
	mach_vm_address_t vfPpgtt64WithOptions {};
	mach_vm_address_t vfFlushHardwareAfterGttUpdate {};
	static void *vfPagePoolAllocatePage(void *that);
	mach_vm_address_t oVfPagePoolAllocatePage {};
	static void vfPagePoolReleasePage(void *that, const void *descriptor);
	mach_vm_address_t oVfPagePoolReleasePage {};
	static void vfPagePoolPrune(void *that, uint32_t age);
	mach_vm_address_t oVfPagePoolPrune {};
	static void vfPagePoolFree(void *that);
	mach_vm_address_t oVfPagePoolFree {};
	static void vfReleasePagePool(void *that);
	mach_vm_address_t oVfReleasePagePool {};
	static void vfMemoryManagerFree(void *that);
	mach_vm_address_t oVfMemoryManagerFree {};
	static void vfUpdateMappingCacheType(void *that, uint32_t requestedType);
	mach_vm_address_t oVfUpdateMappingCacheType {};
	static bool vfWaitForRingSpace(void *that, uint32_t requestedDwords);
	mach_vm_address_t oVfWaitForRingSpace {};
	static void vfAccelTaskFree(void *that);
	mach_vm_address_t oVfAccelTaskFree {};

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
	static IOReturn vfCommandQueueSubmit(void *that, const void *arguments);
	mach_vm_address_t oVfCommandQueueSubmit {};
	static IOReturn vfContextSubmit(void *that, void *input, void *output,
	                               uint64_t options, uint64_t *stamp);
	mach_vm_address_t oVfContextSubmit {};
	static IOReturn vf2DSetSurface(void *that, uint32_t surface,
	                              uint32_t mode);
	mach_vm_address_t oVf2DSetSurface {};
	static IOReturn vf2DFinish(void *that, uint32_t options);
	mach_vm_address_t oVf2DFinish {};
	static IOReturn vf2DBlit(void *that, void *command, uint64_t options);
	mach_vm_address_t oVf2DBlit {};
	static IOReturn vfSurfaceExternalMethod(void *that, uint32_t selector,
	                                      void *arguments, void *dispatch,
	                                      void *target, void *reference);
	mach_vm_address_t oVfSurfaceExternalMethod {};
	static IOReturn vfSharedExternalMethod(void *that, uint32_t selector,
	                                     void *arguments, void *dispatch,
	                                     void *target, void *reference);
	mach_vm_address_t oVfSharedExternalMethod {};
	static IOReturn vfGLContextExternalMethod(void *that, uint32_t selector,
	                                        void *arguments, void *dispatch,
	                                        void *target, void *reference);
	mach_vm_address_t oVfGLContextExternalMethod {};
	static IOReturn vfGLDrawableExternalMethod(void *that, uint32_t selector,
	                                         void *arguments, void *dispatch,
	                                         void *target, void *reference);
	mach_vm_address_t oVfGLDrawableExternalMethod {};
	static IOReturn vfSurfaceMtlExternalMethod(void *that, uint32_t selector,
	                                         void *arguments, void *dispatch,
	                                         void *target, void *reference);
	mach_vm_address_t oVfSurfaceMtlExternalMethod {};
	static IOReturn vfMemoryInfoExternalMethod(void *that, uint32_t selector,
	                                         void *arguments, void *dispatch,
	                                         void *target, void *reference);
	mach_vm_address_t oVfMemoryInfoExternalMethod {};
	static IOReturn vfDisplayPipeExternalMethod(void *that, uint32_t selector,
	                                          void *arguments, void *dispatch,
	                                          void *target, void *reference);
	mach_vm_address_t oVfDisplayPipeExternalMethod {};
	static IOReturn vfDisplayChangeHandler(void *that, void *reference,
	                                     void *framebuffer, int event,
	                                     void *argument);
	mach_vm_address_t oVfDisplayChangeHandler {};
	static void vfGartCollector(void *that, IOInterruptEventSource *source,
	                           int count);
	mach_vm_address_t oVfGartCollector {};
	static void vfFinalizeInterrupt(void *that,
	                               IOInterruptEventSource *source,
	                               int count);
	mach_vm_address_t oVfFinalizeInterrupt {};
	static void vfDeviceCacheControl(void *that, void *cache,
	                                uint32_t selector, uint64_t argument0,
	                                uint64_t argument1);
	mach_vm_address_t oVfDeviceCacheControl {};
	static void vfEmitFirstFlushEvents(void *that);
	mach_vm_address_t oVfEmitFirstFlushEvents {};
	static IOReturn vfDisplaySleepCallback(void *that, uint32_t command,
	                                     uint32_t argument0,
	                                     uint32_t argument1);
	mach_vm_address_t oVfDisplaySleepCallback {};
	static bool vfAllocMoreCommandBuffers(void *pool);
	mach_vm_address_t oIOAccelAllocMoreCommandBuffers {};
	static void *vfGetCommandBufferPtrNoInc(void *pool, uint32_t dwords);
	mach_vm_address_t oIOAccelGetCommandBufferPtrNoInc {};
	static bool vf2DEventVectorGrow(void *vector, size_t requested);
	mach_vm_address_t oVf2DEventVectorGrow {};
	static bool vfAcceleratorEventVectorGrow(void *vector, size_t requested);
	mach_vm_address_t oVfAcceleratorEventVectorGrow {};
	static bool vfRenderEventVectorGrow(void *vector, size_t requested);
	mach_vm_address_t oVfRenderEventVectorGrow {};
	static bool vfBlitEventVectorGrow(void *vector, size_t requested);
	mach_vm_address_t oVfBlitEventVectorGrow {};
	static bool vfGLEventVectorGrow(void *vector, size_t requested);
	mach_vm_address_t oVfGLEventVectorGrow {};
	static bool vfResourceEventVectorGrow(void *vector, size_t requested);
	mach_vm_address_t oVfResourceEventVectorGrow {};
	static bool vfSharedEventVectorGrow(void *vector, size_t requested);
	mach_vm_address_t oVfSharedEventVectorGrow {};
	static bool vfSurfaceEventVectorGrow(void *vector, size_t requested);
	mach_vm_address_t oVfSurfaceEventVectorGrow {};
	mach_vm_address_t vfInterruptBridgeEnable {};
	mach_vm_address_t vfInterruptBridgeDisable {};
	static bool stopGraphicsEngine(void *that);
	static bool startGraphicsEngine(void *that);
	mach_vm_address_t vfSchedulerInitFirmware {};
	mach_vm_address_t originalSchedulerCreate {};
	static void *vfCreateScheduler(void *accelerator);
	mach_vm_address_t originalSchedulerInit {};
	static bool vfInitScheduler(void *scheduler, uint32_t options,
	                            uint64_t privateSize, void *accelerator);
	static void populateResetRegisterList(void *that);
	static bool wrapIGScheduler5IsGpuIdle(const void *that);
	static bool wrapIGScheduler4IsGpuIdle(const void *that);
	static void vfSuppressTimeoutHardwareAction(void *that, uint32_t engine);
	static void vfSuppressPhysicalDebugCapture(void *that, uint32_t reason);
	static void *vfRejectEventTimeout(void *that, int32_t channel);
	static uint32_t vfSuppressHangAnalysis(void *that);
	static void vfSuppressHangDump(void *that);
	static void vfRejectHardwareResetReplay(void *that);
	static bool vfRejectPhysicalEngineReset(void *that, void *context);
	static void barrierSubmission(void *queue, void *accelerator,
	                              void *commandDescriptor, void *event,
	                              uint16_t count, const uint16_t *list);
	mach_vm_address_t obarrierSubmission {};

public:
	void init();
	static Gen11 *callback;
	static bool pollVfGuCToHost(void *that);
	bool processKext(KernelPatcher &patcher, size_t index,
	                 mach_vm_address_t address, size_t size);
};

#endif
