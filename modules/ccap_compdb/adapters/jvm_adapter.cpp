// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - the JVM language adapter
//
// Handles javac, ECJ, Kotlin, Scala 3 and Scala 2.
//
// **Why these are not the native adapters.** A native compile is described
// by its include paths and its macros. A JVM compile is described by its
// **classpath**: the set of jars and class directories the compiler reads
// types from. There is no `-I` and no `-D` in the same sense.
//
// So the translation is a different shape: the classpath, the source path
// and the module path, spelled the way an analyser front end expects.
//
// **Why one adapter and not five.** The five compilers spell the options
// they share the same way (`-classpath` / `--class-path`, `-d`, `-sourcepath`)
// and differ only in the spelling of the ones they do not share. The
// per-compiler *parsing* already happened on the Java side -- in the
// capture agent, which knows each compiler's option table -- so what
// arrives here is already structured.
//
// **Why the compiler kind still matters.** The output directory, the
// module path and the processor path are spelled differently, and a
// consumer that wants to distinguish a Kotlin compile from a Scala one
// needs to be able to.
//
// **Fully isolated from the other adapters** -- it references no other
// adapter, and no other adapter references it.

#include "ccap_compdb.h"

namespace ccap {
namespace compdb {
namespace adapters {

namespace {

// The classpath, in the spelling an analyser front end expects.
//
// The JVM compilers all accept `-classpath`; ECJ also accepts
// `--class-path`. The capture records them under one name, so the
// translation uses one spelling too.
void appendClasspath(std::vector<std::string>& out,
                     const std::vector<std::string>& entries) {
    for (const auto& e : entries) {
        out.push_back("--classpath");
        out.push_back(e);
    }
}

// The directories the compiler reads sources from.
void appendSourcepath(std::vector<std::string>& out,
                      const std::vector<std::string>& entries) {
    for (const auto& e : entries) {
        out.push_back("--sourcepath");
        out.push_back(e);
    }
}

// The module path.
void appendModulePath(std::vector<std::string>& out,
                      const std::vector<std::string>& entries) {
    for (const auto& e : entries) {
        out.push_back("--module-path");
        out.push_back(e);
    }
}

// The annotation processor path.
void appendProcessorPath(std::vector<std::string>& out,
                         const std::vector<std::string>& entries) {
    for (const auto& e : entries) {
        out.push_back("--processor-path");
        out.push_back(e);
    }
}

}  // namespace

class JvmAdapter : public CompilerAdapter {
public:
    explicit JvmAdapter(CompilerKind k, const char* n) : kind_(k), name_(n) {}

    CompilerKind kind() const override { return kind_; }

    const char* name() const override { return name_; }

    bool matches(const CompileCommand& cmd) const override {
        return cmd.kind == kind_;
    }

    std::vector<std::string> translate(const CompileCommand& cmd,
                                       const AdapterContext& ctx) const override {
        std::vector<std::string> out;

        // ── The classpath ───────────────────────────────────────────────
        //
        // This is the JVM equivalent of the include path, and it is the
        // one thing a consumer cannot do without: without the classpath
        // the types of every referenced library are unknown.
        //
        // It is read from classpath_dirs, which is where the JVM capture
        // parser puts it. The native include_dirs are **not** used: they
        // mean something else entirely (-I, a header search path), and
        // confusing the two would make a consumer search a jar for a
        // header.
        appendClasspath(out, cmd.classpathDirs);

        // ── The source path ─────────────────────────────────────────────
        appendSourcepath(out, cmd.systemDirs);

        // ── The module path ─────────────────────────────────────────────
        appendModulePath(out, cmd.moduleDirs);

        // ── The annotation processor path ───────────────────────────────
        appendProcessorPath(out, cmd.processorDirs);

        // ── The language level ──────────────────────────────────────────
        //
        // The JVM compilers spell it differently -- javac uses --release,
        // Kotlin uses -language-version, Scala uses -source -- but the
        // capture records it under one name, so the translation uses one
        // name too.
        if (!cmd.languageStandard.empty()) {
            out.push_back("--language-level");
            out.push_back(cmd.languageStandard);
        }

        // ── The output directory ────────────────────────────────────────
        if (!cmd.outputFile.empty()) {
            out.push_back("--output-dir");
            out.push_back(cmd.outputFile);
        }

        // ── The sources ─────────────────────────────────────────────────
        //
        // Last, because they are the positional arguments.
        for (const auto& s : cmd.sourceFiles) {
            out.push_back(s);
        }

        return out;
    }

private:
    CompilerKind kind_;
    const char* name_;
};

const CompilerAdapter* makeJavacAdapter() {
    static JvmAdapter inst(CompilerKind::Javac, "javac");
    return &inst;
}

const CompilerAdapter* makeEcjAdapter() {
    static JvmAdapter inst(CompilerKind::Ecj, "ecj");
    return &inst;
}

const CompilerAdapter* makeKotlinAdapter() {
    static JvmAdapter inst(CompilerKind::Kotlin, "kotlin");
    return &inst;
}

const CompilerAdapter* makeScala3Adapter() {
    static JvmAdapter inst(CompilerKind::Scala3, "scala3");
    return &inst;
}

const CompilerAdapter* makeScala2Adapter() {
    static JvmAdapter inst(CompilerKind::Scala2, "scala2");
    return &inst;
}

}  // namespace adapters
}  // namespace compdb
}  // namespace ccap
