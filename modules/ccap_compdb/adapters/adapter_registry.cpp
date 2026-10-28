// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - adapter registry
//
// Responsible for mapping "one compile command" to "the adapter that should
// handle it".
//
// Dispatch strategy:
//   1. First match the kind exactly (gcc -> gcc adapter)
//   2. If the kind is unknown -> ask each adapter's matches()
//   3. If none match -> the generic adapter (fallback)
//
// Note the registry does **not** know any concrete compiler -- it only knows
// CompilerKind. The isolation between the individual adapters is guaranteed
// by "each adapter only handles its own kind".

#include "ccap_compdb.h"

namespace ccap {
namespace compdb {

AdapterRegistry::AdapterRegistry() {
    // The order = the priority.
    // generic goes last -- it is the fallback, its matches() is the most
    // permissive.
    //
    // The factories return **static instances** that stay valid for the
    // lifetime of the process, so raw pointers are stored here.
    adapters_.push_back(adapters::makeGccAdapter());
    adapters_.push_back(adapters::makeClangAdapter());
    adapters_.push_back(adapters::makeMsvcAdapter());
    adapters_.push_back(adapters::makeGenericAdapter());
}

const CompilerAdapter* AdapterRegistry::byKind(CompilerKind kind) const {
    for (const auto* a : adapters_) {
        if (a->kind() == kind) return a;
    }
    return nullptr;
}

const CompilerAdapter* AdapterRegistry::find(const CompileCommand& cmd) const {
    // 1) Exact kind match
    if (cmd.kind != CompilerKind::Unknown) {
        if (const CompilerAdapter* a = byKind(cmd.kind)) return a;
    }

    // 2) Let each adapter judge for itself
    for (const auto* a : adapters_) {
        if (a->kind() == CompilerKind::Generic) continue;   // fallback goes last
        if (a->matches(cmd)) return a;
    }

    // 3) Fallback
    if (const CompilerAdapter* a = byKind(CompilerKind::Generic)) return a;
    return nullptr;
}

}  // namespace compdb
}  // namespace ccap
