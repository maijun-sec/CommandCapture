// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - compiler alias resolution (symlinks mutually recognized)
//
// Real-world compiler aliases:
//   /usr/bin/gcc   -> /usr/bin/gcc-11      (version symlink)
//   /usr/bin/cc    -> /usr/bin/gcc        (traditional alias)
//   /usr/bin/c++   -> /usr/bin/g++
//   /opt/tc/bin/arm-none-eabi-g++ -> /opt/tc/bin/arm-none-eabi-gcc
//
// Requirement (explicitly stated by the user):
//   "a symlink records an alias; the real command and the symlinked command
//    are aliases of each other"
//
// Implementation:
//   1. Resolve the **real path** (Linux: realpath; Windows: GetFinalPathNameByHandle)
//   2. All names sharing the same real path are **aliases of each other**
//   3. If any member of the alias set matches a kind, the whole group counts
//      as that kind
//
// Why this is more reliable than exact-name matching:
//   The classic compiler-configuration tools identify compilers with
//   **exact string matching** (case-sensitive).
//   So configuring the compiler as /usr/bin/gcc configures gcc,
//   and a build that invokes `cc` is **not recognized** -- unless you
//   configure cc as well.
//
//   But "cc and gcc are the same file" is a **certain fact at the
//   filesystem level**, not a guess. So we use it directly.

#include "ccap_compdb.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>

#include "core/normalize.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#  include <limits.h>
#  include <stdlib.h>
#  include <unistd.h>
#endif

namespace ccap {
namespace compdb {

#if defined(_WIN32)

std::string AliasResolver::resolveRealPath(const std::string& path) {
    if (path.empty()) return path;

    // GetFinalPathNameByHandle needs a handle it can open.
    // Use FILE_READ_ATTRIBUTES rather than GENERIC_READ --
    // for many exes reading the attributes is enough to get the final path.
    HANDLE h = CreateFileA(path.c_str(), FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // Fall back to path normalization when it cannot be obtained
        return ccap::normalizePath(path);
    }

    DWORD need = GetFinalPathNameByHandleA(h, nullptr, 0, FILE_NAME_NORMALIZED);
    if (need == 0) {
        CloseHandle(h);
        return ccap::normalizePath(path);
    }

    std::vector<char> buf(need + 1, '\0');
    DWORD got = GetFinalPathNameByHandleA(h, buf.data(), need, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (got == 0 || got > need) {
        return ccap::normalizePath(path);
    }

    std::string result(buf.data(), got);
    // Strip the "\\?\" prefix
    if (result.rfind("\\\\?\\", 0) == 0) {
        result = result.substr(4);
    }
    // Normalize the separator
    for (char& c : result) {
        if (c == '\\') c = '/';
    }
    return result;
}

#else  // Linux

std::string AliasResolver::resolveRealPath(const std::string& path) {
    if (path.empty()) return path;

    // realpath requires the target to exist.
    // For a path that does not exist (e.g. not yet generated in the build
    // directory), fall back to normalization.
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf) != nullptr) {
        return std::string(buf);
    }
    return ccap::normalizePath(path);
}

#endif

void AliasResolver::add(const std::string& path) {
    if (path.empty()) return;

    std::string real = resolveRealPath(path);
    if (real.empty()) return;

    // Record path -> real
    realOf_[path] = real;

    // Add path to the name list of real
    auto& names = byReal_[real];
    if (std::find(names.begin(), names.end(), path) == names.end()) {
        names.push_back(path);
    }
}

const std::vector<std::string>& AliasResolver::aliasesOf(const std::string& path) const {
    auto it = realOf_.find(path);
    if (it == realOf_.end()) return empty_;

    auto rit = byReal_.find(it->second);
    if (rit == byReal_.end()) return empty_;

    // Exclude the path itself
    static thread_local std::vector<std::string> scratch;
    scratch.clear();
    for (const auto& n : rit->second) {
        if (n != path) scratch.push_back(n);
    }
    return scratch;
}

const std::vector<std::string>& AliasResolver::namesOfReal(const std::string& path) const {
    auto it = realOf_.find(path);
    if (it == realOf_.end()) {
        // Maybe the caller passed in the real path itself
        auto rit = byReal_.find(path);
        if (rit != byReal_.end()) return rit->second;
        return empty_;
    }
    auto rit = byReal_.find(it->second);
    if (rit == byReal_.end()) return empty_;
    return rit->second;
}

AliasResolver::Identity AliasResolver::identify(const std::string& path,
                                                const CompilerTable& table) const {
    Identity id;
    id.realPath = resolveRealPath(path);

    // First match with the **path itself**
    CompilerKind kind = table.matchPath(path);

    // If that fails, match with any member of the alias group.
    //
    // This is the core of "the real command and the symlinked command are
    // aliases of each other":
    //   If /usr/bin/gcc is configured (a symlink to gcc-11),
    //   a build that uses cc (which also points to gcc-11) is recognized
    //   just the same.
    if (kind == CompilerKind::Unknown) {
        auto rit = byReal_.find(id.realPath);
        if (rit != byReal_.end()) {
            for (const auto& name : rit->second) {
                CompilerKind k = table.matchPath(name);
                if (k != CompilerKind::Unknown) {
                    kind = k;
                    if (const CompilerRule* r = table.findRule(
                            ccap::baseName(name))) {
                        id.ruleId = r->id;
                    }
                    break;
                }
            }
        }
    } else {
        if (const CompilerRule* r = table.findRule(ccap::baseName(path))) {
            id.ruleId = r->id;
        }
    }

    id.kind = kind;
    return id;
}

void AliasResolver::scanPathAliases(const std::vector<std::string>& names) {
    // Only scan the given names -- no full-filesystem PATH scan.
    //
    // Reason: a full scan can be slow on CI machines (PATH holds hundreds of
    // directories), while what the user really cares about is "which aliases
    // do the compilers I configured have".

    // Step 1: resolve each input into an actual path
    //
    // The input may be:
    //   - a full path (/usr/bin/gcc)  -> use as is
    //   - a bare name (gcc)           -> look it up in PATH
    std::vector<std::string> resolvedPaths;
    for (const auto& name : names) {
        std::string full;
        if (name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos) {
            // Full path (or relative path)
            full = name;
        } else {
            full = ccap::findInPath(name);
        }
        if (!full.empty()) {
            add(full);
            resolvedPaths.push_back(full);
        }
    }

    // Step 2: for each known real path, find its "siblings"
    //
    // Sibling = another name in the same directory that points to the same
    // real path. How to find them? Scan every entry in that directory,
    // resolve each real path, and compare.
    //
    // The scope is limited to **the directories of the known compilers** --
    // not the whole filesystem.
    std::set<std::string> dirs;
    for (const auto& p : resolvedPaths) {
        std::string d = ccap::dirName(p);
        if (!d.empty()) dirs.insert(d);
    }

    for (const auto& dir : dirs) {
        // List the directory contents
        // Use opendir/readdir (POSIX)
#if !defined(_WIN32)
        DIR* dp = opendir(dir.c_str());
        if (!dp) continue;
        struct dirent* ent;
        while ((ent = readdir(dp)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            std::string candidate = dir + "/" + name;
            std::string real = resolveRealPath(candidate);
            // Compare against the known real paths
            for (const auto& known : resolvedPaths) {
                if (resolveRealPath(known) == real) {
                    add(candidate);
                    break;
                }
            }
        }
        closedir(dp);
#endif
    }
}

}  // namespace compdb
}  // namespace ccap
