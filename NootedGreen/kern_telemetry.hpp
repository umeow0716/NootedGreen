// Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit
// License version 1.0. See LICENSE for details.

#pragma once

#include <Headers/kern_patcher.hpp>

// AppleIntelTGLGraphics imports two private kernel telemetry entry points.
// NootedGreen exports ABI-identical forwarding shims so the pinned payload can
// depend on this kext without granting it com.apple.kpi.private.
bool ngResolveKernelTelemetry(KernelPatcher &patcher);
