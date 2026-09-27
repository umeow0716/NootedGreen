//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

#include "kern_green.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>

static NGreen nb;

static const char *bootargDebug = "-NGreenDebug";


PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
	nullptr,
	0,
	&bootargDebug,
	1,
	nullptr,
	0,
	KernelVersion::Ventura,
	// Tahoe is intentionally enabled for experimental VM bring-up.  The
	// TGL driver payloads are still the known Sonoma binaries, so this only
	// relaxes Lilu's host-kernel gate; binary patches remain signature gated.
	KernelVersion::Tahoe,
	[]() { nb.init(); },
};
