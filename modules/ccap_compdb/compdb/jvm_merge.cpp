// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Merging the two views of a JVM compile.
//
// WHY MERGE AT ALL
//
//   The same compile is seen twice, and the two views answer different
//   questions:
//
//     * ccap sees the **command line** the compiler was started with.
//       That is always there. But it does not say what the compiler really
//       did: a glob is still unexpanded, the classpath may have been
//       assembled by a plugin, and the javax.tools path has no argv at all.
//
//     * the agent sees what the compiler **actually did**: the sources it
//       really compiled, the classpath it really used, the language level
//       it really ran at.
//
//   So the agent's version is the better one, and it wins where both have
//   an entry. ccap's version is still worth keeping: it covers the
//   compiles the agent could not hook, and it carries the compiler path
//   that the agent does not know.
//
// HOW THE TWO ARE MATCHED
//
//   By the **output directory and the source file set**.
//
//   Not by the command line: the two views rarely agree on it. ccap sees
//   the command line on the shell; the agent sees the argv the compiler
//   was handed, which is a different thing (the API path has none at
//   all, and a glob is expanded by the compiler, not by the shell).
//
//   The output directory and the source files, on the other hand, identify
//   the same compile from both sides: they are what the compile produced.

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "ccap_compdb.h"
#include "core/normalize.h"

namespace ccap {
namespace compdb {

namespace {

// The key a compile is identified by.
//
// The output directory alone is not enough -- a compiler can be run twice
// into the same directory -- so the source set is part of the key. The
// sources are sorted, because the order in which a compiler is handed its
// files does not change which compile it is.
//
// The paths are normalized before they go into the key, because the two
// sides spell the same path differently:
//
//   * the agent reports the path the compiler was handed ("D:\\out"), and
//   * ccap reports the path it saw on the command line, normalized to
//     forward slashes ("D:/out").
//
// Without normalizing, the same compile would not match itself, and the
// result would be the compile recorded twice -- once from the command line
// and once from the agent.
std::string mergeKeyPath(const std::string& path) {
    if (path.empty()) return std::string();

    std::string out = ccap::normalizePath(path);

    // Windows paths are case-insensitive, so "D:\OUT" and "D:\out" are the
    // same directory. On a case-sensitive filesystem they are not, so the
    // comparison stays case-sensitive there.
#if defined(_WIN32)
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
#endif

    // A trailing separator is not part of the directory's identity.
    while (out.size() > 1 && (out.back() == '/' || out.back() == '\\')) {
        out.pop_back();
    }
    return out;
}

std::string compileKey(const CompileCommand& c) {
    std::vector<std::string> sources;
    sources.reserve(c.sourceFiles.size());
    for (const auto& s : c.sourceFiles) {
        sources.push_back(mergeKeyPath(s));
    }
    std::sort(sources.begin(), sources.end());

    std::string key = mergeKeyPath(c.outputFile);
    for (const auto& s : sources) {
        key += "\x1f";
        key += s;
    }
    return key;
}

// Fill in what only ccap knows.
//
// The agent does not know which executable the compiler was -- it hooks
// the compiler's classes, not its launcher -- so the compiler path comes
// from ccap's side. It is the same compiler, so the two can be combined.
void fillFromCcap(CompileCommand* agent, const CompileCommand& ccapCmd) {
    if (!agent->compilerPath.empty() && agent->compilerPath != ccapCmd.compilerPath) {
        // The agent's value is a bare name; ccap's is a real path. The
        // real path is the more useful one.
        agent->compilerPath = ccapCmd.compilerPath;
    }
    if (agent->compilerBase.empty()) {
        agent->compilerBase = ccapCmd.compilerBase;
    }
    if (agent->directory.empty()) {
        agent->directory = ccapCmd.directory;
    }
}

}  // namespace

std::vector<CompileCommand> mergeJvmCommands(const std::vector<CompileCommand>& fromCcap,
                                             const std::vector<CompileCommand>& fromAgent) {
    // The agent's captures, keyed by what they compiled.
    std::map<std::string, size_t> agentIndex;
    for (size_t i = 0; i < fromAgent.size(); ++i) {
        agentIndex.emplace(compileKey(fromAgent[i]), i);
    }

    std::vector<CompileCommand> merged;
    std::set<size_t> usedAgent;

    // First the compiles ccap saw.
    for (const auto& c : fromCcap) {
        auto it = agentIndex.find(compileKey(c));
        if (it == agentIndex.end()) {
            // The agent did not see this compile. The command line alone is
            // still a usable compile command, so it is kept.
            merged.push_back(c);
            continue;
        }
        // Both saw it. The agent's version carries the ground truth, so it
        // is the one that is kept -- with what only ccap knows filled in.
        CompileCommand combined = fromAgent[it->second];
        fillFromCcap(&combined, c);
        merged.push_back(combined);
        usedAgent.insert(it->second);
    }

    // Then the compiles only the agent saw. They would have been missed
    // entirely -- the command line may not have looked like a compile at
    // all, which is exactly the case the API path is.
    for (size_t i = 0; i < fromAgent.size(); ++i) {
        if (usedAgent.count(i)) continue;
        merged.push_back(fromAgent[i]);
    }

    return merged;
}

}  // namespace compdb
}  // namespace ccap
