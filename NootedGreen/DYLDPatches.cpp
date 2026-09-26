//! Copyright © 2022-2023 ChefKiss Inc. Licensed under the Thou Shalt Not Profit License version 1.5.
//! See LICENSE for details.

#include "DYLDPatches.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/OSAtomic.h>

DYLDPatches *DYLDPatches::callback = nullptr;

void DYLDPatches::init() {
	callback = this;
}

void DYLDPatches::processPatcher(KernelPatcher &patcher) {
	auto *entry = IORegistryEntry::fromPath("/", gIODTPlane);
	if (entry) {
		SYSLOG_COND(!entry->setProperty("hwgva-id", const_cast<char *>(kHwGvaId), arrsize(kHwGvaId)),
			"DYLD", "Failed to publish hwgva-id");
		entry->release();
	}

	KernelPatcher::RouteRequest request {"_cs_validate_page", wrapCsValidatePage, this->orgCsValidatePage};
	SYSLOG_COND(!patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1), "DYLD",
		"Failed to route _cs_validate_page; optional media model patches are disabled");
}

void DYLDPatches::wrapCsValidatePage(vnode *vp, memory_object_t pager, memory_object_offset_t page_offset,
	const void *data, int *validated_p, int *tainted_p, int *nx_p) {
	auto *instance = callback;
	PANIC_COND(!instance || !instance->orgCsValidatePage, "DYLD", "Invalid _cs_validate_page route state");
	FunctionCast(wrapCsValidatePage, instance->orgCsValidatePage)(vp, pager, page_offset, data, validated_p,
		tainted_p, nx_p);
	if (!vp || !data)
		return;

	char path[PATH_MAX] {};
	int pathlen = PATH_MAX;
	if (vn_getpath(vp, path, &pathlen) != 0 || pathlen <= 0 || pathlen > PATH_MAX)
		return;

	// Diagnostic only. cs_validate_page may run concurrently, so claim each log
	// with an atomic gate instead of racing on a plain static bool.
	static volatile UInt32 loggedTglMtl = 0;
	static volatile UInt32 loggedTglGl = 0;
	static volatile UInt32 loggedTglVa = 0;
	if (strstr(path, "AppleIntelTGLGraphicsMTLDriver.bundle") &&
		OSCompareAndSwap(0, 1, &loggedTglMtl))
		SYSLOG("DYLD", "MTL_BUNDLE_SEEN: %s", path);
	if (strstr(path, "AppleIntelTGLGraphicsGLDriver.bundle") &&
		OSCompareAndSwap(0, 1, &loggedTglGl))
		SYSLOG("DYLD", "GL_BUNDLE_SEEN: %s", path);
	if (strstr(path, "AppleIntelTGLGraphicsVADriver.bundle") &&
		OSCompareAndSwap(0, 1, &loggedTglVa))
		SYSLOG("DYLD", "VA_BUNDLE_SEEN: %s", path);

	// The retired CoreLSKD and Sonoma 14.7.1 hardcoded control-flow patches
	// had no exact binary admission. Only patch composite semantic strings in
	// shared-cache pages; ordinary executables and GPU bundles pass untouched.
	if (!UserPatcher::matchSharedCachePath(path))
		return;

	auto &deviceInfo = BaseDeviceInfo::get();
	if (deviceInfo.modelIdentifier[0] &&
		KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE,
			kVideoToolboxDRMModelOriginal, arrsize(kVideoToolboxDRMModelOriginal),
			deviceInfo.modelIdentifier, 20))
		DBGLOG("DYLD", "Applied 'VideoToolbox DRM model check' patch");

	const DYLDPatch patches[] = {
		{kAGVABoardIdOriginal, kAGVABoardIdPatched, "iMacPro1,1 spoof (AppleGVA)"},
		{kHEVCEncBoardIdOriginal, kHEVCEncBoardIdPatched, "iMacPro1,1 spoof (AppleGVAHEVCEncoder)"},
	};
	DYLDPatch::applyAll(patches, const_cast<void *>(data), PAGE_SIZE);
}
