// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - path normalization and executable resolution
#pragma once

#include <string>
#include <vector>

namespace ccap {

// Unify the separator to '/' (also on Windows) and drop redundant '.',
// resolving '..'
std::string normalizePath(const std::string& path);

// Normalize but keep the platform-native separators ('\' on Windows).
//
// Purpose: an executable path must be passed to CreateProcessW with native
// separators --- writing C:\Windows\System32\cmd.exe as
// C:/Windows/System32/cmd.exe makes cmd.exe fail to resolve its own image
// path.
std::string normalizePathNative(const std::string& path);

// Get the basename
std::string baseName(const std::string& path);

// Get the extension (including the dot, lowercase)
std::string extensionOf(const std::string& path);

// Get the containing directory
std::string dirName(const std::string& path);

// Test whether it is an absolute path
bool isAbsolutePath(const std::string& path);

// Read an environment variable (cross-platform)
std::string getEnv(const std::string& name);

// The current working directory
std::string currentDir();

// Read the whole file; returns true on success
bool readFile(const std::string& path, std::string* out);

// Whether the file exists (and is a regular file)
bool fileExists(const std::string& path);

// Whether the directory exists
bool dirExists(const std::string& path);

// Write a file
bool writeFile(const std::string& path, const std::string& content);

// Look up an executable in PATH; returns the absolute path when found.
// Windows tries every extension in PATHEXT; Linux looks up the name as-is.
std::string findInPath(const std::string& name,
                         const std::vector<std::string>& extraDirs = {});

// Resolve an executable:
//   - already an absolute path and it exists -> return it directly
//   - contains a path separator -> resolve relative to the cwd
//   - otherwise -> look it up in PATH
// An empty string means "not found".
std::string resolveExecutable(const std::string& name);

// Same as above, but returns the path with platform-native separators ('\' on
// Windows).
// The lpApplicationName passed to CreateProcessW must use this form.
std::string resolveExecutableNative(const std::string& name);

// Expand environment variable references:
//   Windows: %VAR%
//   POSIX:   $VAR / ${VAR}
std::string expandEnvVars(const std::string& s);

// Strip the path modifier prefixing a command name: %VAR% on Windows,
// $VAR on POSIX
std::string stripEnvPrefix(const std::string& s);

}  // namespace ccap
