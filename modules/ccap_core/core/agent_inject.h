// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Injecting the JVM capture agent into a process tree.
//
// WHY THIS EXISTS
//
//   The agent only captures a compile if it is attached to the JVM that
//   runs the compiler. That is a build configuration problem, and the
//   configuration differs per build tool.
//
//   The obvious way to attach an agent is to put "-javaagent:..." on the
//   command line. Measured, that does not work for most of the commands
//   that matter:
//
//     * `javac` is a program, not a JVM launcher. Its own option parser
//       rejects "-javaagent" with "invalid flag".
//
//     * `mvn` and `gradle` do not pass a "-javaagent" they were given to
//       the JVM that does the compiling.
//
//   JAVA_TOOL_OPTIONS has neither problem. The JVM reads it before it
//   starts, so its position in the command line cannot matter, and every
//   java command reads it -- java, javac, javap, jar, javadoc, jlink,
//   jshell, jcmd and jpackage, all measured.
//
// WHY THE ENVIRONMENT, ON WINDOWS TOO
//
//   On Windows a child process's command line cannot be changed after the
//   fact: by the time the debugger sees CREATE_PROCESS_DEBUG_EVENT the
//   PEB is already mapped and the command line has been handed to the C
//   runtime. So the argument cannot be rewritten the way it can be on
//   Linux, where the execve hook owns the argv.
//
//   The environment has no such problem, because it is inherited: a
//   process that has JAVA_TOOL_OPTIONS passes it to every process it
//   starts. Measured: mvn -> java -> javac and gradle -> daemon -> javac
//   both carry it through.
//
//   So injecting it once, into the environment of the root process,
//   covers the whole process tree.
#pragma once

#include <string>

namespace ccap {

// The environment variable the JVM reads for extra options.
extern const char* const kJavaToolOptionsEnv;

// Build the value for JAVA_TOOL_OPTIONS that attaches the agent.
//
// The agent is **appended** to whatever was there, so a value that was
// already set -- by the user, or by another tool -- keeps working.
//
// @param existing the value already in the environment, or empty
// @param agentJar the agent jar
// @param captureDir the directory the agent writes its captures into
// @return the value to set, or empty when there is no agent to attach
std::string buildJavaToolOptions(const std::string& existing,
                                 const std::string& agentJar,
                                 const std::string& captureDir);

}  // namespace ccap
