// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_capture - C ABI bridge layer
//
// Turns the C++ event model into C callbacks.
//
// Why this is a separate file:
//   The C++ side interfaces (EventCollector / CommandEvent) evolve with the
//   internal implementation;
//   the C ABI is an **external promise** and must not change with them.
//   Keeping the conversion logic here means internal changes do not affect
//   the ABI.

#include "ccap_capture.h"

#include <cstring>
#include <string>
#include <vector>

#include "core/event.h"
#include "core/json.h"
#include "core/script_scanner.h"
#include "capture/platform.h"

namespace {

// The session object
//
// The name is **deliberately not** ccap_session --
// the C header already occupies that name with
// `typedef struct ccap_session ccap_session;`,
// and reusing it would create an ambiguity in C++ (struct tag vs typedef).
// SessionImpl is used here because the semantics are clearer too: it is the
// implementation behind the C ABI.
struct SessionImpl {
    ccap::EventCollector collector;
};

// JSON serialization of a single event
//
// Keeps the same field set as event_to_json in main.cpp --
// but this one is written separately, and **not shared**:
//   the main.cpp version keeps evolving (new fields, changed output),
//   while the C ABI JSON format is a contract once published.
//   Two independently stable implementations are safer than one shared
//   function.
std::string eventToJson(const ccap::CommandEvent& e) {
    std::string j = "{";
    j += ccap::jsonKvNum("seq", static_cast<int64_t>(e.seq));
    j += ",";
    j += ccap::jsonKvNum("timestamp_ms", e.timestampMs);
    j += ",";
    j += ccap::jsonKvStr("platform", ccap::platformName(e.platform));
    j += ",";
    j += ccap::jsonKvStr("source", ccap::sourceName(e.source));
    j += ",";
    j += ccap::jsonKvNum("pid", e.pid);
    j += ",";
    j += ccap::jsonKvNum("ppid", e.ppid);
    j += ",";
    j += ccap::jsonKvNum("depth", e.depth);
    j += ",";
    j += ccap::jsonKvStr("kind", e.kind);
    j += ",";
    j += ccap::jsonKvStr("executable", e.executable);
    j += ",";
    j += ccap::jsonKv("argv", ccap::jsonStringArray(e.argv));
    j += ",";
    j += ccap::jsonKvStr("command_line", e.rawCommandLine);
    j += ",";
    j += ccap::jsonKvStr("cwd", e.cwd);
    j += ",";
    j += ccap::jsonKvBool("is_builtin", e.isBuiltin);
    j += ",";
    j += ccap::jsonKvBool("resolved", e.resolved);
    j += ",";
    j += ccap::jsonKvBool("truncated", e.truncated);
    j += ",";
    j += ccap::jsonKvStr("script_path", e.scriptPath);
    j += ",";
    j += ccap::jsonKvNum("script_line", e.scriptLine);
    j += ",";
    j += ccap::jsonKvNum("exec_index", e.execIndex);
    j += ",";
    j += ccap::jsonKv("notes", ccap::jsonStringArray(e.notes));
    j += "}";
    return j;
}

}  // namespace

extern "C" {

void ccap_run_options_init(ccap_run_options* options) {
    if (!options) return;
    std::memset(options, 0, sizeof(*options));
    // Note: these are the **C ABI field names** (snake_case),
    // not the camelBack names on the C++ side -- the two sides deliberately
    // use different field names.
    options->scan_script = 1;
    options->script_recursive = 1;
    options->script_depth = 4;
    options->quiet_ms = 300;
}

ccap_session* ccap_session_create() {
    return reinterpret_cast<ccap_session*>(new (std::nothrow) SessionImpl());
}

void ccap_session_destroy(ccap_session* session) {
    delete reinterpret_cast<SessionImpl*>(session);
}

int ccap_session_run(ccap_session* session,
                     const char* target,
                     const char* const* args,
                     size_t argCount,
                     const ccap_run_options* options,
                     ccap_event_callback onEvent,
                     ccap_note_callback onNote,
                     void* user) {
    if (!session || !target || !*target) return 2;
    SessionImpl* impl = reinterpret_cast<SessionImpl*>(session);

    ccap::RunOptions opts;
    if (options) {
        opts.cwd = options->cwd ? options->cwd : "";
        // Left side is the C++ side (camelBack), right side is the C ABI
        // side (snake_case).
        opts.scanScript = options->scan_script != 0;
        opts.scriptRecursive = options->script_recursive != 0;
        opts.scriptDepth = options->script_depth;
        opts.timeoutMs = options->timeout_ms;
        opts.quietMs = options->quiet_ms;
        opts.verbose = options->verbose != 0;
        opts.clearBeingDebugged = options->clear_being_debugged != 0;
        opts.redirectOutputToStderr = options->redirect_output_to_stderr != 0;
    } else {
        opts.scanScript = true;
        opts.scriptRecursive = true;
        opts.scriptDepth = 4;
        opts.quietMs = 300;
    }
    opts.target = target;

    if (args && argCount > 0) {
        opts.args.reserve(argCount);
        for (size_t i = 0; i < argCount; ++i) {
            if (args[i]) opts.args.push_back(args[i]);
        }
    }

    ccap::RunResult rr = ccap::runCapture(opts, &impl->collector);

    // Events go out first, notes after --
    // the order carries meaning: events are the payload, notes are
    // supplementary explanations.
    auto events = impl->collector.deduped();
    if (onEvent) {
        for (const auto& e : events) {
            std::string line = eventToJson(e);
            onEvent(user, line.c_str(), line.size());
        }
    }
    if (onNote) {
        for (const auto& n : rr.notes) {
            onNote(user, n.c_str());
        }
    }

    if (!rr.ok) return 1;
    return 0;
}

int ccap_session_scan(ccap_session* session,
                      const char* scriptPath,
                      const ccap_run_options* options,
                      ccap_event_callback onEvent,
                      ccap_note_callback onNote,
                      void* user) {
    if (!session || !scriptPath || !*scriptPath) return 2;
    SessionImpl* impl = reinterpret_cast<SessionImpl*>(session);

    ccap::ScanOptions so;
    so.recursive = true;
    so.maxDepth = 4;
    if (options) {
        so.recursive = options->script_recursive != 0;
        so.maxDepth = options->script_depth > 0 ? options->script_depth : 4;
    }

    ccap::scanScript(scriptPath, so, &impl->collector, 0);

    auto events = impl->collector.deduped();
    if (onEvent) {
        for (const auto& e : events) {
            std::string line = eventToJson(e);
            onEvent(user, line.c_str(), line.size());
        }
    }
    if (onNote) {
        const char* note =
            "scan only: these are script builtins, no process was created";
        onNote(user, note);
    }
    return static_cast<int>(events.size());
}

ccap_platform ccap_current_platform(void) {
    switch (ccap::currentPlatform()) {
        case ccap::Platform::Windows: return CCAP_PLATFORM_WINDOWS;
        case ccap::Platform::Linux:   return CCAP_PLATFORM_LINUX;
        default:                        return CCAP_PLATFORM_UNKNOWN;
    }
}

const char* ccap_platform_name(ccap_platform platform) {
    switch (platform) {
        case CCAP_PLATFORM_WINDOWS: return "windows";
        case CCAP_PLATFORM_LINUX:   return "linux";
        default:                     return "unknown";
    }
}

const char* ccap_version(void) {
    return "1.0.0";
}

}  // extern "C"
