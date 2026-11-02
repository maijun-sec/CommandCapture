// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Tests for the JVM agent injection and the merge of the two views of a
// JVM compile.
//
// Covers:
//   1. Building the JAVA_TOOL_OPTIONS value
//   2. Merging ccap's command lines with the agent's captures
//
// Usage: test_agent_inject

#include <cstdio>
#include <string>
#include <vector>

#include "ccap_compdb.h"
#include "core/agent_inject.h"

using namespace ccap;
using namespace ccap::compdb;

namespace {

int gPass = 0;
int gFail = 0;

void check(const char* name, bool ok, const std::string& detail = "") {
    if (ok) {
        std::printf("ok   %s\n", name);
        ++gPass;
    } else {
        std::printf("FAIL %s  %s\n", name, detail.c_str());
        ++gFail;
    }
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// ── The JAVA_TOOL_OPTIONS value ───────────────────────────────────────────

void testToolOptions() {
    // No agent, no value. Silently adding one would attach an agent the
    // user did not ask for.
    check("no agent means no tool options",
          buildJavaToolOptions("", "", "/tmp/cap").empty());
    check("no agent means no tool options -- even with an existing value",
          buildJavaToolOptions("-Xmx512m", "", "/tmp/cap").empty());

    // The basic shape: the agent, and the capture directory as the agent's
    // own argument.
    std::string v = buildJavaToolOptions("", "/opt/agent.jar", "/tmp/cap");
    check("the agent goes in", contains(v, "-javaagent:/opt/agent.jar"),
          v);
    check("the capture directory goes in",
          contains(v, "ccap.capture.dir=/tmp/cap"), v);

    // An existing value is kept, not replaced. The user may have had
    // settings of their own there.
    std::string kept =
        buildJavaToolOptions("-Xmx512m -Dfoo=bar", "/opt/agent.jar", "/tmp/cap");
    check("an existing value is kept", contains(kept, "-Xmx512m"), kept);
    check("an existing value is kept -- the agent is appended",
          contains(kept, "-javaagent:/opt/agent.jar"), kept);
    check("the existing value comes first",
          kept.find("-Xmx512m") < kept.find("-javaagent:"), kept);

    // An empty (or blank) value is treated as absent.
    std::string blank = buildJavaToolOptions("   ", "/opt/agent.jar", "/tmp/cap");
    check("a blank value is treated as absent",
          blank.find("-javaagent:") == 0, blank);

    // No capture directory: the agent is still attached, without one.
    std::string noDir = buildJavaToolOptions("", "/opt/agent.jar", "");
    check("the agent is attached without a capture directory",
          contains(noDir, "-javaagent:/opt/agent.jar") &&
              !contains(noDir, "ccap.capture.dir="),
          noDir);
}

// ── The merge ────────────────────────────────────────────────────────────

// A command as ccap sees it: the command line on the shell.
CompileCommand ccapView(const std::string& outDir,
                         const std::vector<std::string>& sources) {
    CompileCommand c;
    c.rawCommand = "javac -d " + outDir + " Foo.java";
    c.argv = {"javac", "-d", outDir, "Foo.java"};
    c.compilerBase = "javac";
    c.compilerPath = "C:/jdk/bin/javac.exe";
    c.directory = "C:/work";
    c.outputFile = outDir;
    c.sourceFiles = sources;
    c.kind = CompilerKind::Javac;
    return c;
}

// The same compile as the agent sees it: the ground truth.
CompileCommand agentView(const std::string& outDir,
                          const std::vector<std::string>& sources) {
    CompileCommand c;
    c.rawCommand = "javac -classpath lib.jar -d " + outDir + " Foo.java";
    c.argv = {"javac", "-classpath", "lib.jar", "-d", outDir, "Foo.java"};
    c.compilerBase = "javac";
    c.compilerPath = "javac";
    c.directory = "C:/work";
    c.outputFile = outDir;
    c.sourceFiles = sources;
    c.classpathDirs = {"lib.jar"};
    c.languageStandard = "8";
    c.kind = CompilerKind::Javac;
    return c;
}

void testMerge() {
    // Both saw the same compile: the agent's version wins, because it
    // carries the classpath the compiler really used.
    {
        std::vector<CompileCommand> fromCcap = {ccapView("out", {"Foo.java"})};
        std::vector<CompileCommand> fromAgent = {agentView("out", {"Foo.java"})};
        auto merged = mergeJvmCommands(fromCcap, fromAgent);

        check("the merge keeps one entry", merged.size() == 1,
              "size=" + std::to_string(merged.size()));
        if (merged.size() == 1) {
            check("the agent's version wins -- it has the classpath",
                  !merged[0].classpathDirs.empty(), merged[0].rawCommand);
            check("the agent's version wins -- it has the language level",
                  merged[0].languageStandard == "8", merged[0].languageStandard);
            // What only ccap knows is filled in.
            check("the compiler path comes from ccap",
                  merged[0].compilerPath == "C:/jdk/bin/javac.exe",
                  merged[0].compilerPath);
        }
    }

    // Only ccap saw it: the command line alone is still worth keeping.
    {
        std::vector<CompileCommand> fromCcap = {ccapView("out", {"Foo.java"})};
        std::vector<CompileCommand> fromAgent;
        auto merged = mergeJvmCommands(fromCcap, fromAgent);

        check("a compile only ccap saw is kept", merged.size() == 1,
              "size=" + std::to_string(merged.size()));
        if (merged.size() == 1) {
            check("it keeps ccap's command line",
                  merged[0].rawCommand.find("javac -d") == 0, merged[0].rawCommand);
        }
    }

    // Only the agent saw it: ccap did not see the compile at all. This is
    // the API path -- the compile happens in process, so there is no
    // command line for ccap to see.
    {
        std::vector<CompileCommand> fromCcap;
        std::vector<CompileCommand> fromAgent = {agentView("out", {"Foo.java"})};
        auto merged = mergeJvmCommands(fromCcap, fromAgent);

        check("a compile only the agent saw is kept", merged.size() == 1,
              "size=" + std::to_string(merged.size()));
    }

    // Different compiles: neither is dropped.
    {
        std::vector<CompileCommand> fromCcap = {ccapView("out1", {"A.java"})};
        std::vector<CompileCommand> fromAgent = {agentView("out2", {"B.java"})};
        auto merged = mergeJvmCommands(fromCcap, fromAgent);

        check("two different compiles are both kept", merged.size() == 2,
              "size=" + std::to_string(merged.size()));
    }

    // The same output directory but different sources: two compiles, not
    // one. The sources are part of the identity.
    {
        std::vector<CompileCommand> fromCcap = {ccapView("out", {"A.java"})};
        std::vector<CompileCommand> fromAgent = {agentView("out", {"B.java"})};
        auto merged = mergeJvmCommands(fromCcap, fromAgent);

        check("the same output directory with different sources is two compiles",
              merged.size() == 2, "size=" + std::to_string(merged.size()));
    }

    // The order of the sources does not change which compile it is.
    {
        std::vector<CompileCommand> fromCcap = {ccapView("out", {"A.java", "B.java"})};
        std::vector<CompileCommand> fromAgent = {agentView("out", {"B.java", "A.java"})};
        auto merged = mergeJvmCommands(fromCcap, fromAgent);

        check("the source order does not affect the match", merged.size() == 1,
              "size=" + std::to_string(merged.size()));
    }

    // The two sides spell the same path differently.
    //
    // The agent reports the path the compiler was handed ("D:\\out"), and
    // ccap reports the path it saw on the command line, normalized to
    // forward slashes ("D:/out"). Those are the same directory, and the
    // merge has to say so -- otherwise the same compile is recorded
    // twice: once from the command line, once from the agent.
    {
        CompileCommand fromShell = ccapView("D:/out", {"D:/src/Foo.java"});
        CompileCommand fromAgentSide = agentView("D:\\out", {"D:\\src\\Foo.java"});
        auto merged = mergeJvmCommands({fromShell}, {fromAgentSide});

        check("the same path in two spellings is one compile",
              merged.size() == 1, "size=" + std::to_string(merged.size()));
    }

    // A trailing separator is not part of the directory's identity.
    {
        CompileCommand fromShell = ccapView("D:/out", {"D:/src/Foo.java"});
        CompileCommand fromAgentSide = agentView("D:\\out\\", {"D:\\src\\Foo.java"});
        auto merged = mergeJvmCommands({fromShell}, {fromAgentSide});

        check("a trailing separator does not break the match",
              merged.size() == 1, "size=" + std::to_string(merged.size()));
    }
}

// ── The command line side ─────────────────────────────────────────────────

// The parser has to know the JVM source extensions and the JVM output
// directory option.
//
// Why this matters: without them a javac command line ends up with no
// source files at all, and then
//
//   * it is treated as a link-only command and dropped from the compile
//     database, and
//   * the merge cannot match it against the agent's capture, because the
//     merge keys on the output directory plus the source set -- one side
//     has sources and the other does not.
//
// The result would be the same compile recorded twice.
void testJvmCommandLineParsing() {
    CommandParser parser;

    {
        CompileCommand c = parser.parseArgv(
            {"javac", "-d", "out", "src/Foo.java"}, "C:/work");
        check("a javac command line has its source files",
              c.sourceFiles.size() == 1, "size=" + std::to_string(c.sourceFiles.size()));
        check("a javac command line has its output directory",
              c.outputFile == "out", c.outputFile);
        check("a javac command line is not link-only", !c.isLinkOnly);
    }

    // The "-d<dir>" spelling, with no space.
    {
        CompileCommand c = parser.parseArgv({"javac", "-dout", "Foo.java"}, "C:/work");
        check("a javac command line accepts the attached output directory",
              c.outputFile == "out", c.outputFile);
    }

    {
        CompileCommand c = parser.parseArgv(
            {"kotlinc", "-d", "out", "src/Foo.kt"}, "C:/work");
        check("a kotlinc command line has its source files",
              c.sourceFiles.size() == 1, "size=" + std::to_string(c.sourceFiles.size()));
    }

    {
        CompileCommand c = parser.parseArgv(
            {"scalac", "-d", "out", "src/Foo.scala"}, "C:/work");
        check("a scalac command line has its source files",
              c.sourceFiles.size() == 1, "size=" + std::to_string(c.sourceFiles.size()));
    }

    // A native compiler keeps its own behaviour: "-d" is not an output
    // directory there, and a ".java" file is not a source of a C
    // compiler.
    {
        CompileCommand c = parser.parseArgv(
            {"g++", "-c", "main.cpp", "-o", "main.o"}, "C:/work");
        check("a native compiler still finds its source files",
              c.sourceFiles.size() == 1, "size=" + std::to_string(c.sourceFiles.size()));
        check("a native compiler keeps its output file",
              c.outputFile == "main.o", c.outputFile);
    }

    // The whole point: the command line view and the agent's view have to
    // describe the same compile, so that the merge recognizes them as one.
    {
        CompileCommand fromShell = parser.parseArgv(
            {"javac", "-d", "D:/out", "D:/src/Foo.java"}, "C:/work");

        CompileCommand fromAgentSide;
        fromAgentSide.kind = CompilerKind::Javac;
        fromAgentSide.compilerBase = "javac";
        fromAgentSide.directory = "C:/work";
        fromAgentSide.outputFile = "D:\\out";
        fromAgentSide.sourceFiles = {"D:\\src\\Foo.java"};

        auto merged = mergeJvmCommands({fromShell}, {fromAgentSide});
        check("the command line and the agent describe one compile",
              merged.size() == 1, "size=" + std::to_string(merged.size()));
    }
}

}  // namespace

int main() {
    std::printf("=== agent injection and merge tests ===\n");

    testToolOptions();
    testMerge();
    testJvmCommandLineParsing();

    std::printf("\n===================================\n");
    std::printf("PASS: %d   FAIL: %d\n", gPass, gFail);
    std::printf("===================================\n");
    return gFail == 0 ? 0 : 1;
}
