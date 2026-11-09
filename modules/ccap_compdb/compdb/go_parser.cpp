// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - the Go toolchain
//
// See ccap_compdb_go.h for why the Go toolchain is a case of its own.

#include "ccap_compdb_go.h"

#include <cctype>

#include "core/normalize.h"

namespace ccap {
namespace compdb {

namespace {

// Resolve a path against the working directory of the command.
//
// The command line may carry a relative path (the Go toolchain writes
// the manifest into the build's temporary directory and names it
// relative to the working directory), and the file has to be found
// where the command actually ran.
std::string resolveAgainst(const std::string& path,
                           const std::string& directory) {
    if (path.empty()) return path;
    if (ccap::isAbsolutePath(path)) return ccap::normalizePath(path);
    if (directory.empty()) return path;
    return ccap::normalizePath(directory + "/" + path);
}

// Trim the whitespace around a line of the manifest.
std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// The import configuration manifest
// ═══════════════════════════════════════════════════════════════════════════

GoImportConfig readGoImportConfig(const std::string& path,
                                  const std::string& directory) {
    GoImportConfig out;

    if (path.empty()) {
        out.error = "no import configuration was named";
        return out;
    }

    const std::string resolved = resolveAgainst(path, directory);

    std::string content;
    if (!ccap::readFile(resolved, &content)) {
        // A manifest that is gone is the **normal** case when a build
        // log is parsed after the build: the manifest lives in the
        // build's temporary directory, and the build deletes it.
        //
        // So this is reported, not fatal -- the path is what a consumer
        // needs, and the path is kept either way.
        out.error = "the file could not be read (it is normally deleted "
                    "when the build ends)";
        return out;
    }

    out.read = true;

    size_t pos = 0;
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = (nl == std::string::npos)
                               ? content.substr(pos)
                               : content.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? content.size() + 1 : nl + 1;

        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        // "packagefile <import path>=<export data file>"
        static const std::string kPackageFile = "packagefile ";
        if (line.compare(0, kPackageFile.size(), kPackageFile) != 0) {
            out.otherDirectives.push_back(line);
            continue;
        }

        const std::string rest = line.substr(kPackageFile.size());

        // The import path is the key, so the split is on the **first**
        // '=' -- a file name may contain one, an import path may not.
        const size_t eq = rest.find('=');
        if (eq == std::string::npos || eq == 0) {
            out.otherDirectives.push_back(line);
            continue;
        }

        const std::string importPath = rest.substr(0, eq);
        const std::string archive = rest.substr(eq + 1);
        if (archive.empty()) {
            out.otherDirectives.push_back(line);
            continue;
        }

        out.packageFiles[importPath] = archive;
    }

    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// The command line
// ═══════════════════════════════════════════════════════════════════════════

bool isGoSourceFile(const std::string& path) {
    // The extension is lowercased by extensionOf, so this also
    // accepts the ".GO" spelling that a Windows build might produce.
    return ccap::extensionOf(path) == ".go";
}

void parseGoArgv(const std::vector<std::string>& argv, CompileCommand* cmd) {
    if (!cmd) return;

    // Whether the operands have started.
    //
    // Go's flag package stops parsing at the first operand, and the
    // operands of a Go tool command are the source files. So once a
    // source file has been seen, everything after it is an operand
    // too.
    //
    // WHY THE STOP IS TIED TO THE SOURCE FILES
    //
    //   The value of a flag this code does not model ("-trimpath
    //   <path>", "-goversion <version>") looks like an operand as
    //   well, and it comes **before** the sources. Stopping there
    //   would lose every flag after it -- and with it the package
    //   path, the language version and the import configuration.
    //
    //   So the operands begin at the first *source file*, which is
    //   where the operands of the command really begin.
    bool sawSource = false;

    for (size_t i = 1; i < argv.size(); ++i) {
        const std::string& a = argv[i];

        if (sawSource || a.empty() || a[0] != '-') {
            if (isGoSourceFile(a)) {
                cmd->sourceFiles.push_back(a);
                sawSource = true;
            } else {
                cmd->otherOptions.push_back(a);
            }
            continue;
        }

        // The three spellings of a flag:
        //   "-name"           a boolean flag, no value
        //   "-name=value"     the value is attached
        //   "-name" <next>    the value is the next argument
        std::string name = a.substr(1);
        std::string value;
        bool hasValue = false;

        const size_t eq = name.find('=');
        if (eq != std::string::npos) {
            value = name.substr(eq + 1);
            name = name.substr(0, eq);
            hasValue = true;
        }

        // The flags that take a value. This list is short, and it is
        // the reason the Go options are parsed here rather than in the
        // generic loop: only a flag that is **known** to take a value
        // may consume the next argument.
        //
        // Guessing wrong would swallow a source file: the generic loop
        // cannot know whether "-complete" or "-trimpath" wants a value,
        // and an unknown flag that ate the next argument would take a
        // ".go" file with it.
        if (name == "p" || name == "lang" || name == "importcfg" ||
            name == "o") {
            if (!hasValue && i + 1 < argv.size()) {
                value = argv[++i];
                hasValue = true;
            }
            if (hasValue) {
                if (name == "p") {
                    cmd->packageImportPath = value;
                } else if (name == "lang") {
                    cmd->languageStandard = value;
                } else if (name == "importcfg") {
                    // Kept **verbatim**: the downstream reads this file,
                    // and the build may well have deleted it by now.
                    cmd->importConfigPath = value;
                } else {
                    cmd->outputFile = value;
                }
            }
            continue;
        }

        // A flag this code does not model. It is kept as it is, so
        // that the record of the command stays complete.
        cmd->otherOptions.push_back(a);
    }

    // The link step links the archives the compile steps produced, so
    // it has no sources of its own.
    cmd->isLinkOnly = cmd->sourceFiles.empty() && !cmd->compileOnly;

    // The sources are made absolute against the working directory,
    // like the native parser does: the consumer may run under a
    // different cwd.
    if (!cmd->directory.empty()) {
        const std::string base = ccap::normalizePath(cmd->directory);
        for (auto& s : cmd->sourceFiles) {
            if (!ccap::isAbsolutePath(s)) {
                s = ccap::normalizePath(base + "/" + s);
            }
        }
    }

    // ── The import configuration ───────────────────────────────────────
    //
    // The manifest is the dependency list of the package. It is read
    // on a best-effort basis: the file usually does not exist any
    // more by the time the command line is parsed, and the path is
    // kept either way.
    if (!cmd->importConfigPath.empty()) {
        const GoImportConfig cfg =
            readGoImportConfig(cmd->importConfigPath, cmd->directory);
        cmd->packageFiles = cfg.packageFiles;

        // The two generic slots are filled from the manifest, in the
        // same order, so that the i-th import path and the i-th
        // export data file belong together.
        //
        //   includeDirs -- the import paths: the Go counterpart of an
        //                  include path is the set of imported
        //                  packages
        //   systemDirs  -- the export data files: where the types of
        //                  those packages come from
        for (const auto& kv : cfg.packageFiles) {
            cmd->includeDirs.push_back(kv.first);
            cmd->systemDirs.push_back(kv.second);
        }

        if (cfg.read) {
            cmd->notes.push_back(
                "go: the import configuration lists " +
                std::to_string(cfg.packageFiles.size()) + " package(s)");
        } else {
            cmd->notes.push_back(
                "go: the import configuration '" + cmd->importConfigPath +
                "' could not be read (" + cfg.error +
                "); the dependency list is not available");
        }
    }

    // The link step is not just another compile: its import
    // configuration is the **authoritative** statement of which
    // packages are in the binary, which the compile steps cannot
    // give. That is why the link command must not be filtered away.
    if (cmd->compilerBase == "link") {
        cmd->notes.push_back(
            "go: this is the link step -- its import configuration is the "
            "authoritative list of the packages in the binary");
    }
}

}  // namespace compdb
}  // namespace ccap
