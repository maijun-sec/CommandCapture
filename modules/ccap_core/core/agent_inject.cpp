// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the repository LICENSE file for the
// full text.

#include "core/agent_inject.h"

namespace ccap {

const char* const kJavaToolOptionsEnv = "JAVA_TOOL_OPTIONS";

std::string buildJavaToolOptions(const std::string& existing,
                                 const std::string& agentJar,
                                 const std::string& captureDir) {
    if (agentJar.empty()) {
        return std::string();
    }

    // The capture directory travels as the agent's own argument, because
    // that is the only channel that reaches the compiler's JVM: a system
    // property set on the launcher would not be inherited by a process
    // the launcher starts.
    std::string option = "-javaagent:" + agentJar;
    if (!captureDir.empty()) {
        option += "=ccap.capture.dir=" + captureDir;
    }

    if (existing.empty()) {
        return option;
    }

    // An existing value is kept: it may hold settings of its own.
    bool onlySpace = true;
    for (char c : existing) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            onlySpace = false;
            break;
        }
    }
    if (onlySpace) {
        return option;
    }

    return existing + " " + option;
}

}  // namespace ccap
