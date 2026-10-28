// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - helper implementations for CompileCommand
//
// Mostly implementations like reconstruct() that are shared across files.
// Keeping it in a separate file lets command_parser.cpp focus on "parsing".

#include "ccap_compdb.h"

namespace ccap {
namespace compdb {

// The methods of CompileCommand are all implemented in command_parser.cpp.
// This file is kept as a place for shared helper functions in the future.
//
// (It is empty for now, but it is listed in CMakeLists -- keeping the file
//   list stable avoids having to touch the build script whenever a file is
//   added or removed.)

}  // namespace compdb
}  // namespace ccap
