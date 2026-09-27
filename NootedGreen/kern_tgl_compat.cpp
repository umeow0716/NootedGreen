// Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit
// License version 1.0. See LICENSE for details.

#include "kern_tgl_compat.hpp"
#include "kern_green.hpp"

#include <Headers/kern_util.hpp>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOReturn.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include <mach/vm_param.h>
#include <string.h>

namespace {
constexpr size_t kDsbGpuOffsetOffset = 0xC50;
constexpr size_t kGgttPointerOffset = 0xCA0;
constexpr size_t kGgttLengthOffset = 0xCEC;
constexpr size_t kDsbSizeOffset = 0xDCC;
constexpr size_t kPortConfigurationOffset = 0x548;
constexpr size_t kDsbPageCount = NGTglCompat::dsbBytes / NGTglCompat::pageBytes;

volatile UInt32 dsbState {0};
void *dsbOwner {nullptr};
IOBufferMemoryDescriptor *dsbBuffer {nullptr};

template <typename T>
T objectField(const void *object, size_t offset)
{
	T value {};
	memcpy(&value, reinterpret_cast<const uint8_t *>(object) + offset,
	       sizeof(value));
	return value;
}

bool framebufferAccessAllowed()
{
	auto *callback = NGreen::callback;
	return callback && callback->nativeTigerLakePath() &&
		ngPhysicalGpuAccessAllowed() && callback->setRMMIOIfNecessary();
}

uint32_t readRegister32(unsigned long offset)
{
	return framebufferAccessAllowed() ? NGreen::callback->readReg32(offset) : UINT32_MAX;
}

void writeRegister32(unsigned long offset, uint32_t value)
{
	if (framebufferAccessAllowed())
		NGreen::callback->writeReg32(offset, value);
}

volatile uint64_t *checkedGgttEntry(void *owner, volatile void *requestedBase,
	                                unsigned long byteOffset)
{
	if (!framebufferAccessAllowed() || !owner || !requestedBase ||
	    (byteOffset & (sizeof(uint64_t) - 1U)) != 0)
		return nullptr;
	auto *base = objectField<volatile uint8_t *>(owner, kGgttPointerOffset);
	const uint32_t bytes = objectField<uint32_t>(owner, kGgttLengthOffset);
	if (requestedBase != base ||
	    (bytes != NGTglCompat::ggttSmallBytes &&
	     bytes != NGTglCompat::ggttLargeBytes) ||
	    byteOffset > bytes - sizeof(uint64_t))
		return nullptr;
	return reinterpret_cast<volatile uint64_t *>(base + byteOffset);
}

uint64_t readRegister64(void *owner, volatile void *base,
	                    unsigned long offset)
{
	auto *entry = checkedGgttEntry(owner, base, offset);
	return entry ? *entry : 0;
}

void writeRegister64(void *owner, volatile void *base, unsigned long offset,
	                 uint64_t value)
{
	auto *entry = checkedGgttEntry(owner, base, offset);
	if (!entry)
		return;
	*entry = value;
	__sync_synchronize();
	(void)*entry; // posting read: Gen11+ GGTT is mapped uncached by this payload.
}

uint64_t getPmtNow()
{
	uint64_t uptime = 0;
	uint64_t nanoseconds = 0;
	clock_get_uptime(&uptime);
	absolutetime_to_nanoseconds(uptime, &nanoseconds);
	return nanoseconds;
}

void releaseFailedDsbBuffer(IOBufferMemoryDescriptor *buffer)
{
	if (buffer)
		buffer->release();
	dsbOwner = nullptr;
	__sync_synchronize();
	(void)OSCompareAndSwap(1, 0, &dsbState);
}

IOReturn setupDsbMemory(void *owner)
{
	if (!framebufferAccessAllowed() || !owner)
		return kIOReturnUnsupported;

	const uint32_t size = objectField<uint32_t>(owner, kDsbSizeOffset);
	const uint64_t gpuOffset = objectField<uint64_t>(owner, kDsbGpuOffsetOffset);
	const uint32_t ggttBytes = objectField<uint32_t>(owner, kGgttLengthOffset);
	auto *gtt = objectField<volatile uint8_t *>(owner, kGgttPointerOffset);
	if (!gtt || !NGTglCompat::validDsbLayout(size, gpuOffset, ggttBytes))
		return kIOReturnBadArgument;

	if (!OSCompareAndSwap(0, 1, &dsbState)) {
		__sync_synchronize();
		return dsbState == 2 && dsbOwner == owner ?
			kIOReturnSuccess : kIOReturnExclusiveAccess;
	}
	dsbOwner = owner;
	__sync_synchronize();

	auto *buffer = IOBufferMemoryDescriptor::withOptions(
		kIODirectionInOut | kIOMemoryPhysicallyContiguous |
			kIOMemoryHostPhysicallyContiguous,
		size, PAGE_SIZE);
	if (!buffer) {
		releaseFailedDsbBuffer(nullptr);
		return kIOReturnNoMemory;
	}
	auto *bytes = buffer->getBytesNoCopy();
	if (!bytes) {
		releaseFailedDsbBuffer(buffer);
		return kIOReturnNoMemory;
	}
	bzero(bytes, size);

	IOByteCount segmentBytes = 0;
	const uint64_t physical = buffer->getPhysicalSegment(
		0, &segmentBytes, kIOMemoryMapperNone);
	uint64_t firstPte = 0;
	if (!physical || segmentBytes < size ||
	    !NGTglCompat::encodeGgttPte(physical, firstPte)) {
		releaseFailedDsbBuffer(buffer);
		return kIOReturnNoSpace;
	}

	uint64_t oldPtes[kDsbPageCount] {};
	volatile uint64_t *entries[kDsbPageCount] {};
	for (size_t page = 0; page < kDsbPageCount; page++) {
		const uint64_t pteOffset =
			(gpuOffset + page * NGTglCompat::pageBytes) >> 9U;
		entries[page] = reinterpret_cast<volatile uint64_t *>(gtt + pteOffset);
		oldPtes[page] = *entries[page];
	}

	for (size_t page = 0; page < kDsbPageCount; page++) {
		uint64_t pte = 0;
		if (!NGTglCompat::encodeGgttPte(
			    physical + page * NGTglCompat::pageBytes, pte)) {
			for (size_t restore = 0; restore < page; restore++)
				*entries[restore] = oldPtes[restore];
			releaseFailedDsbBuffer(buffer);
			return kIOReturnNoSpace;
		}
		*entries[page] = pte;
	}
	__sync_synchronize();
	for (size_t page = 0; page < kDsbPageCount; page++) {
		uint64_t expected = 0;
		(void)NGTglCompat::encodeGgttPte(
			physical + page * NGTglCompat::pageBytes, expected);
		if (*entries[page] != expected) {
			for (size_t restore = 0; restore < kDsbPageCount; restore++)
				*entries[restore] = oldPtes[restore];
			__sync_synchronize();
			(void)*entries[0];
			releaseFailedDsbBuffer(buffer);
			return kIOReturnNotResponding;
		}
	}

	// The framebuffer creates and maps an IODeviceMemory subrange immediately
	// after this returns. Retain the physical backing for the kext lifetime;
	// no guessed private field is written into the Apple controller object.
	dsbBuffer = buffer;
	__sync_synchronize();
	(void)OSCompareAndSwap(1, 2, &dsbState);
	return dsbState == 2 ? kIOReturnSuccess : kIOReturnNotReady;
}

uint32_t probePortMode(void *port)
{
	if (!framebufferAccessAllowed() || !port)
		return 0;
	auto *configuration = objectField<const uint32_t *>(
		port, kPortConfigurationOffset);
	if (!configuration)
		return 0;
	const uint32_t value = NGreen::callback->readReg32(
		NGTglCompat::portTxDflexDpsp1);
	return NGTglCompat::portMode(*configuration, value, value != UINT32_MAX);
}
}

class AppleIntelBaseController {
public:
	EXPORT void WriteRegister32(unsigned long, uint32_t);
	EXPORT uint32_t ReadRegister32(unsigned long);
	EXPORT void WriteRegister64(volatile void *, unsigned long, uint64_t);
	EXPORT uint64_t ReadRegister64(volatile void *, unsigned long);
	EXPORT uint64_t getPMTNow();
	EXPORT IOReturn hwSetupDSBMemory();
};

class AppleIntelFramebufferController {
public:
	EXPORT void WriteRegister32(unsigned long, uint32_t);
	EXPORT uint32_t ReadRegister32(unsigned long);
	EXPORT void WriteRegister64(volatile void *, unsigned long, uint64_t);
	EXPORT uint64_t ReadRegister64(volatile void *, unsigned long);
	EXPORT uint64_t getPMTNow();
	EXPORT IOReturn hwSetupDSBMemory();
};

class AppleIntelPortHAL {
public:
	EXPORT uint32_t probePortMode();
};

class AppleIntelDisplayPath {
public:
	EXPORT static uint32_t DSBEngineBusyStatus[3];
};

void AppleIntelBaseController::WriteRegister32(unsigned long offset, uint32_t value)
{
	writeRegister32(offset, value);
}

uint32_t AppleIntelBaseController::ReadRegister32(unsigned long offset)
{
	return readRegister32(offset);
}

void AppleIntelBaseController::WriteRegister64(volatile void *base,
	                                           unsigned long offset,
	                                           uint64_t value)
{
	writeRegister64(this, base, offset, value);
}

uint64_t AppleIntelBaseController::ReadRegister64(volatile void *base,
	                                              unsigned long offset)
{
	return readRegister64(this, base, offset);
}

uint64_t AppleIntelBaseController::getPMTNow()
{
	return getPmtNow();
}

IOReturn AppleIntelBaseController::hwSetupDSBMemory()
{
	return setupDsbMemory(this);
}

void AppleIntelFramebufferController::WriteRegister32(unsigned long offset,
	                                                   uint32_t value)
{
	writeRegister32(offset, value);
}

uint32_t AppleIntelFramebufferController::ReadRegister32(unsigned long offset)
{
	return readRegister32(offset);
}

void AppleIntelFramebufferController::WriteRegister64(volatile void *base,
	                                                    unsigned long offset,
	                                                    uint64_t value)
{
	writeRegister64(this, base, offset, value);
}

uint64_t AppleIntelFramebufferController::ReadRegister64(volatile void *base,
	                                                     unsigned long offset)
{
	return readRegister64(this, base, offset);
}

uint64_t AppleIntelFramebufferController::getPMTNow()
{
	return getPmtNow();
}

IOReturn AppleIntelFramebufferController::hwSetupDSBMemory()
{
	return setupDsbMemory(this);
}

uint32_t AppleIntelPortHAL::probePortMode()
{
	return ::probePortMode(this);
}

EXPORT uint32_t AppleIntelDisplayPath::DSBEngineBusyStatus[3] {};

extern "C" EXPORT char *strnstr(char *string, const char *find, size_t length)
{
	const char first = *find++;
	if (first == '\0')
		return string;
	const size_t tailLength = strlen(find);
	while (length != 0) {
		const char current = *string++;
		length--;
		if (current == '\0')
			return nullptr;
		if (current == first) {
			if (tailLength > length)
				return nullptr;
			if (strncmp(string, find, tailLength) == 0)
				return string - 1;
		}
	}
	return nullptr;
}
