// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - Linux platform implementation
#pragma once

#include <string>
#include <vector>

#include "core/event.h"
#include "capture/platform.h"

namespace ccap {

// Run the target on Linux and collect command creation events.
// Mechanism: LD_PRELOAD injects ccap_shim.so, which intercepts the execve
// family and the posix_spawn family.
CaptureResult runLinuxPreload(const RunOptions& opts,
                                 EventCollector* collector);

}  // namespace ccap
