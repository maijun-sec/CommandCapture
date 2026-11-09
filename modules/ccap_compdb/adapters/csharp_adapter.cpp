// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - the .NET adapter
//
// WHY A .NET COMPILE IS NOT TRANSLATED TO CLANG
//
// A C# compile does not produce an object file or a bitcode file: it
// produces an **assembly**. There is no clang front end for it, and
// nothing to map it onto.
//
// So this adapter does not translate. It **reports** the compile in the
// analyzer's own vocabulary, which is what a consumer of a .NET
// analysis actually needs:
//
//   --sys_include <assembly>   the reference assemblies the compiler
//                               read types from
//   --analyzer <path>          the analyzers and source generators the
//                               compiler was given
//   -D<name>[=<value>]         the preprocessor symbols
//   <source file>              the files that were compiled
//
// WHY THE REFERENCES AND NOT THE INCLUDES
//
// A C# compile has no include path. The compiler reads types from
// **reference assemblies** (.dll files with a metadata table), and the
// lookup rules are completely different from "search this directory
// for a header". A consumer that has to find a type has to open an
// assembly, not walk a directory.
//
// So the references go into the system-include slot -- it is the slot
// for "where do the types come from" -- and the distinction is kept by
// the adapter rather than by the shared model.

#include "ccap_compdb.h"

namespace ccap {
namespace compdb {
namespace adapters {

class CsharpAdapter : public CompilerAdapter {
public:
    CompilerKind kind() const override { return CompilerKind::Csharp; }

    const char* name() const override { return "csharp"; }

    bool matches(const CompileCommand& cmd) const override {
        return cmd.kind == CompilerKind::Csharp;
    }

    std::vector<std::string> translate(const CompileCommand& cmd,
                                       const AdapterContext& ctx) const override {
        std::vector<std::string> out;

        // The reference assemblies.
        //
        // They are the .NET counterpart of the system include paths:
        // the places the compiler reads types from.
        for (const auto& d : cmd.classpathDirs) {
            out.push_back("--sys_include");
            out.push_back(d);
        }
        for (const auto& d : ctx.extraSystemIncludes) {
            out.push_back("--sys_include");
            out.push_back(d);
        }

        // The analyzers and the source generators.
        //
        // They are the .NET counterpart of the annotation processors, and
        // they live in the same slot -- the capture puts them in
        // processorDirs, and the JVM adapter reads the same slot for the
        // annotation processor path.
        //
        // They are kept because an analyzer **changes what the compiler
        // sees**: a source generator rewrites the source before it is
        // compiled, and an analyzer reports diagnostics the plain
        // compiler would not. A consumer that reproduces the compile has
        // to have them.
        for (const auto& a : cmd.processorDirs) {
            out.push_back("--analyzer");
            out.push_back(a);
        }

        // The source files.
        //
        // They come first in the argument list for the C/C++ adapters,
        // and the position does not matter here: the analyzer reads
        // the whole command line.
        for (const auto& f : cmd.sourceFiles) {
            out.push_back(f);
        }

        return out;
    }
};

const CompilerAdapter* makeCsharpAdapter() {
    static const CsharpAdapter instance;
    return &instance;
}

}  // namespace adapters
}  // namespace compdb
}  // namespace ccap
