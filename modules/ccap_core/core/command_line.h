// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - command line / argument parsing
//
// Provides two flavours of semantics:
//   1) Windows CRT style command line splitting (equivalent to
//      CommandLineToArgvW, but implemented in pure C++ so that static
//      analysis and unit tests can run on Linux too).
//   2) POSIX shell style splitting, handling '...' and "..." plus backslash
//      escapes.
#pragma once

#include <string>
#include <vector>

namespace ccap {

// Split a command line using the Windows CRT rules.
// The returned argv[0] is the program name, the rest are the arguments.
// Supported escapes: \" \\ \t \n \v \r, plus any character inside "xxx".
std::vector<std::string> splitWindowsCommandLine(const std::string& cmdline);

// Split one line using POSIX shell rules (used for .sh static analysis).
// Everything inside single quotes is literal; inside double quotes \" \\ \$ \`
// can be escaped; a backslash escapes the next character.
std::vector<std::string> splitShellLine(const std::string& line);

// Detect the flavour automatically and split a command line.
//
// Why this is needed:
//   POSIX rules treat `\` as an escape character, Windows rules treat it as an
//   ordinary character (unless it precedes a quote). Splitting the same line
//   with the wrong rule *silently loses content*:
//
//     cl.exe /I"..\inc" /c a.cpp
//     POSIX rules   -> the \i in /I"..\inc" is eaten -> "..inc"
//     Windows rules -> "..\inc" is preserved intact
//
// Detection strategy (most reliable first):
//   1. **Contains Windows-specific syntax** -> Windows rules
//      (starts with /I /D /std: /Fo /Fe /external:I, or has a .exe suffix)
//   2. **Contains a backslash and no POSIX escape signature** -> Windows rules
//   3. Otherwise -> POSIX rules
//
// The "POSIX escape signature" in rule 2 means: a backslash followed by a
// shell metacharacter (" ' ` $ \ space newline). In `C:\path\file` the
// backslash is followed by a letter, so it is a Windows path; `\"` is a POSIX
// escape.
std::vector<std::string> splitCommandLineAuto(const std::string& line);

// Split a *compiler* command line (auto detection plus macro quote handling).
//
// Why this needs dedicated handling:
//   In `-DNAME="value"` the quotes are **part of the macro value**, not
//   grouping delimiters. GCC / Clang / MSVC all share this semantics --
//   after expansion the macro is the string literal "value".
//
//   A generic splitter treats the quotes as grouping delimiters and strips
//   them, yielding `-DNAME=value` -- the macro value changes from a string
//   literal into an identifier. That is a **semantic change** and it is not
//   recoverable.
//
//   The rule: after an option starting with `-D` or `/D`, the quotes in the
//   immediately following token are **kept literally**.
//
//   Note: the separated form `-D NAME="value"` is supported too -- the whole
//   next token is handled literally.
std::vector<std::string> splitCompilerCommandLine(const std::string& line);

// Strip one layer of outer quotes (if the whole token is wrapped in a
// matching pair of " or ').
std::string stripOuterQuotes(const std::string& s);

// Decide whether a string looks like a known shell/cmd builtin or a common
// external tool alias. Used to classify static analysis results as
// builtin / external.
bool looksLikeShellBuiltin(const std::string& cmd);

}  // namespace ccap
