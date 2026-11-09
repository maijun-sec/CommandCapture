// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - the Go adapter
//
// WHY A GO COMPILE IS NOT TRANSLATED TO CLANG
//
// A Go compile does not produce an object file that clang could read,
// and there is no clang front end for Go. So this adapter does not
// translate into a clang command: it **reports** the compile in the
// analyzer's own vocabulary, which is what a consumer of a Go analysis
// actually needs:
//
//   --sys_include <file>   the export data of a dependency: where the
//                           types of the imported packages come from
//   --importcfg <file>     the import configuration manifest: the
//                           authoritative map of "import path -> export
//                           data"
//   --import_path <path>   the import path of the package itself
//   -Dgo.lang.version=<v>  the Go language version ("-lang=go1.23")
//   -o <file>              the output (the archive, or the binary)
//   <file>.go              the sources
//
// WHY THE EXPORT DATA IS THE "SYSTEM INCLUDE"
//
// A Go package does not include headers. It imports **packages**, and
// the compiler reads their **export data** -- the archive the compile
// step produced, which carries the types without the source.
//
// The lookup rules are nothing like "search this directory for a
// header": the file is named after nothing in particular, and the
// thing that identifies it is the **import path** it belongs to. So
// the export data goes into the "--sys_include" slot -- the slot for
// "where do the types come from" -- the same way the .NET reference
// assemblies do.
//
// WHY THE MANIFEST PATH SURVIVES THE TRANSLATION
//
// The manifest is the only place where the import paths and their
// export data are paired. The "--sys_include" list carries the files
// but not the pairing, and the pairing is what a consumer needs to
// resolve "fmt.Println" to a type.
//
// So the path of the manifest is kept, verbatim, in the translated
// command. The consumer opens it and gets the pairing back.
//
// (The manifest usually does not exist any more by then -- it lives in
// the build's temporary directory, which the build deletes. That is
// why the path is kept even when the file could not be read: it is
// the one thing that lets a consumer find the manifest while it
// exists.)

#include "ccap_compdb.h"

namespace ccap {
namespace compdb {
namespace adapters {

class GoAdapter : public CompilerAdapter {
public:
    CompilerKind kind() const override { return CompilerKind::Go; }

    const char* name() const override { return "go"; }

    bool matches(const CompileCommand& cmd) const override {
        return cmd.kind == CompilerKind::Go;
    }

    std::vector<std::string> translate(const CompileCommand& cmd,
                                       const AdapterContext& ctx) const override {
        std::vector<std::string> out;

        // ── The export data of the dependencies ─────────────────────────
        //
        // This is the Go counterpart of the system include path: the
        // places the compiler reads the types of the imported packages
        // from.
        for (const auto& kv : cmd.packageFiles) {
            out.push_back("--sys_include");
            out.push_back(kv.second);
        }

        // ── The import configuration ───────────────────────────────────
        //
        // The path is emitted **verbatim** -- it is the original path
        // of the manifest, and it is what the consumer reads to get
        // the "import path -> export data" pairing back.
        if (!cmd.importConfigPath.empty()) {
            out.push_back("--importcfg");
            out.push_back(cmd.importConfigPath);
        }

        // ── The package import path ────────────────────────────────────
        //
        // The identity of the package being compiled: it is what the
        // sources of that package are called, and it is how a
        // consumer tells two packages of the same project apart.
        if (!cmd.packageImportPath.empty()) {
            out.push_back("--import_path");
            out.push_back(cmd.packageImportPath);
        }

        // ── The language version ───────────────────────────────────────
        //
        // Go's "-lang" is the language version of the package ("go1.23"),
        // and it is carried by a define rather than by a "-std=": the
        // analyzer's vocabulary has no other spelling for a language
        // version of a non-C language, and a define is the one form
        // every front end understands.
        //
        // The name is namespaced, because "go.lang.version" is not a
        // preprocessor symbol of the package -- it is a fact about the
        // language the package is compiled in, and it must not be
        // confused with one.
        if (!cmd.languageStandard.empty()) {
            out.push_back("-Dgo.lang.version=" + cmd.languageStandard);
        }

        // ── The output ─────────────────────────────────────────────────
        //
        // The archive for a compile, the binary for a link.
        if (!cmd.outputFile.empty()) {
            out.push_back("-o");
            out.push_back(cmd.outputFile);
        }

        // ── The sources ────────────────────────────────────────────────
        for (const auto& s : cmd.sourceFiles) {
            out.push_back(s);
        }

        return out;
    }
};

const CompilerAdapter* makeGoAdapter() {
    static const GoAdapter instance;
    return &instance;
}

}  // namespace adapters
}  // namespace compdb
}  // namespace ccap
