// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

#include "core/event.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

namespace ccap {

namespace {

std::mutex gMutex;

// Identity of a runtime event: pid + executable file + full argv + exec index
std::string runtimeIdentity(const CommandEvent& e) {
    std::ostringstream os;
    os << "R\x1f" << e.pid << '\x1f' << e.executable << '\x1f';
    for (size_t i = 0; i < e.argv.size(); ++i) {
        if (i) os << '\x1f';
        os << e.argv[i];
    }
    os << '\x1f' << e.execIndex;
    return os.str();
}

// Identity of a static event: script path + line number + full argv
//
// Why static events carry a line number:
//   Two lines of the same script that both say `echo same` are **two
//   different commands**. Early versions used only (pid, exe, argv) as the
//   key, and pid is always 0, so the second one was treated as a duplicate
//   and dropped --- silently losing data.
std::string staticIdentity(const CommandEvent& e) {
    std::ostringstream os;
    os << "S\x1f" << e.scriptPath << '\x1f' << e.scriptLine << '\x1f';
    for (size_t i = 0; i < e.argv.size(); ++i) {
        if (i) os << '\x1f';
        os << e.argv[i];
    }
    return os.str();
}

}  // namespace

int64_t nowUnixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string platformName(Platform p) {
    switch (p) {
        case Platform::Windows: return "windows";
        case Platform::Linux:   return "linux";
        default:                return "unknown";
    }
}

std::string sourceName(EventSource s) {
    switch (s) {
        case EventSource::Runtime: return "runtime";
        case EventSource::Static:  return "static";
        default:                    return "unknown";
    }
}

void EventCollector::add(CommandEvent ev) {
    std::lock_guard<std::mutex> lock(gMutex);
    ev.seq = nextSeq_++;

    // Runtime events: assign the exec index per (pid, exe, argv).
    // On Linux, exec does not change the pid; if the same pid execs the
    // same command N times in a row, those are N independent executions ---
    // they must be told apart by the index, otherwise they get merged by
    // mistake.
    if (ev.source == EventSource::Runtime && ev.pid) {
        std::string key = runtimeIdentity(ev);
        int64_t& n = execCounter_[key];
        ev.execIndex = n++;
    }

    events_.push_back(std::move(ev));
}

std::vector<CommandEvent> EventCollector::take() {
    std::lock_guard<std::mutex> lock(gMutex);
    std::vector<CommandEvent> out;
    out.swap(events_);
    execCounter_.clear();
    return out;
}

std::vector<CommandEvent> EventCollector::deduped() const {
    std::lock_guard<std::mutex> lock(gMutex);

    std::set<std::string> seen;
    std::vector<CommandEvent> out;
    out.reserve(events_.size());

    for (const auto& e : events_) {
        std::string key = (e.source == EventSource::Static)
                              ? staticIdentity(e)
                              : runtimeIdentity(e);
        if (seen.insert(key).second) {
            out.push_back(e);
        }
    }
    return out;
}

}  // namespace ccap
