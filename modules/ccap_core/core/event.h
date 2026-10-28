// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - unified event model
//
// Every command record produced by any platform or source (runtime observation
// or static script analysis) is normalized into this struct so that downstream
// consumers can treat them uniformly.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ccap {

enum class EventSource {
    Runtime,   // a real process creation observed at runtime
    Static,    // a builtin command recovered by static script analysis
};

enum class Platform {
    Windows,
    Linux,
    Unknown,
};

struct CommandEvent {
    // Monotonically increasing sequence number, used for ordering and dedup.
    uint64_t seq = 0;

    // Timestamp (Unix milliseconds)
    int64_t timestampMs = 0;

    Platform platform = Platform::Unknown;
    EventSource source = EventSource::Runtime;

    // Process identity
    uint32_t pid = 0;
    uint32_t ppid = 0;

    // Executable (may be a resolved absolute path, or a bare script name)
    std::string executable;

    // Argument vector. argv[0] is by convention the executable name
    // (empty when unknown).
    std::vector<std::string> argv;

    // Raw command line string (native on Windows; a join on Linux)
    std::string rawCommandLine;

    // Working directory
    std::string cwd;

    // Only meaningful when source == Static
    std::string scriptPath;
    int scriptLine = 0;

    // Confidence / classification hints
    std::string kind;      // "process" | "builtin" | "script"
    bool     isBuiltin = false;
    bool     resolved     = false;  // executable resolved to an absolute path
    bool     truncated    = false;  // command line was truncated

    // Remarks / warnings
    std::vector<std::string> notes;

    // Depth (level relative to the root process), maintained by the collector.
    int depth = 0;

    // Event identity, used for exact deduplication.
    //
    // A static event is identified by (script path, line number) -- the same
    // command on the same line is the same command, but the *same* command on
    // *different* lines is two distinct commands and both must be kept.
    //
    // A runtime event is identified by (pid, executable, full argv, occurrence
    // index). The last component cannot be dropped: on Linux exec does not
    // change the pid, so one pid exec'ing the same command N times is N
    // independent executions and must not be merged into one.
    int64_t execIndex = 0;
};

// Event collector: thread-safe append and query.
class EventCollector {
public:
    void add(CommandEvent ev);
    const std::vector<CommandEvent>& events() const { return events_; }
    std::vector<CommandEvent> take();

    // The deduplicated event sequence.
    //
    // Static events are deduplicated by (script path, line number, argv) --
    // identical commands on different lines are different commands and must
    // all be kept.
    //
    // Runtime events are deduplicated by (pid, executable, full argv, exec
    // index). The exec index is assigned in add() in order of arrival, so the
    // 1st and 2nd occurrence of the same command under the same pid are not
    // merged.
    std::vector<CommandEvent> deduped() const;

    size_t size() const { return events_.size(); }

private:
    std::vector<CommandEvent> events_;
    uint64_t nextSeq_ = 1;

    // Exec index assignment table for runtime events:
    //   key   = (pid, executable, full argv)
    //   value = highest index assigned so far
    std::map<std::string, int64_t> execCounter_;
};


// Helpers
int64_t nowUnixMs();
std::string platformName(Platform p);
std::string sourceName(EventSource s);

}  // namespace ccap
