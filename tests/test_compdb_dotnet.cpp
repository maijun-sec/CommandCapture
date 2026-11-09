// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - the .NET backend tests
//
// The .NET backend reads a build log, so its tests are built on a **real**
// log -- one captured from an actual "dotnet build" -- rather than on a
// synthetic line. A synthetic line would test the parser against the
// input the author imagined, and the three things that actually broke
// during development (the UTF-16 encoding, the unquoted executable path,
// the task-id annotation) are all properties of the **real** log that a
// synthetic one would not have.
//
// The fixture is a real MSBuild diagnostic log, trimmed to the Csc task
// line. It is checked in as UTF-16LE with a BOM, which is how MSBuild
// writes it when its stdout is redirected.

#include "ccap_compdb_build.h"
#include "ccap_compdb_dotnet.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "ccap_compdb.h"

namespace {

int gPass = 0;
int gFail = 0;

void check(const char* name, bool ok, const std::string& detail = "") {
    if (ok) {
        ++gPass;
        std::printf("ok   %s\n", name);
    } else {
        ++gFail;
        std::printf("FAIL %s%s%s\n", name, detail.empty() ? "" : " -- ",
                    detail.c_str());
    }
}

// The fixture path.
std::string fixturePath() {
    return std::string(CCAP_TEST_FIXTURES_DIR) + "/dotnet/msbuild-csc-line.txt";
}

// A scratch file in the temp directory.
//
// The fixture directory is checked in, so the test must not write into
// it: the stray files would look like part of the test data, and they
// would show up in every "what changed" review.
std::string scratchPath(const char* name) {
#if defined(_WIN32)
    char base[MAX_PATH + 1] = {0};
    const DWORD got = ::GetTempPathA(MAX_PATH, base);
    std::string dir = (got == 0) ? std::string(".") : std::string(base);
    if (!dir.empty() && dir.back() == '\\') dir.pop_back();
    return dir + "\\ccap-test-" + name;
#else
    return std::string("/tmp/ccap-test-") + name;
#endif
}

// Write a text to a file, with the given encoding.
//
// The "utf16" mode writes UTF-16LE with a BOM -- the way MSBuild does.
bool writeFixture(const std::string& path, const std::string& text,
                  bool utf16) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    if (!utf16) {
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return true;
    }
    const char bom[2] = { static_cast<char>(0xFF), static_cast<char>(0xFE) };
    out.write(bom, 2);
    for (char c : text) {
        const char pair[2] = { c, 0 };
        out.write(pair, 2);
    }
    return true;
}

// ── The Windows command line split ───────────────────────────────────────

void testSplit() {
    std::printf("\n-- the windows command line split --\n");

    // The plain case.
    {
        auto v = ccap::compdb::splitWindowsCommandLine("a b c");
        check("the plain case splits into three",
              v.size() == 3 && v[0] == "a" && v[2] == "c");
    }

    // A quoted path with a space stays ONE argument.
    {
        auto v = ccap::compdb::splitWindowsCommandLine(
            "\"C:\\Program Files\\dotnet\\dotnet.exe\" exec \"x.dll\"");
        check("a quoted path stays one argument",
              v.size() == 3 && v[0] == "C:\\Program Files\\dotnet\\dotnet.exe",
              v.empty() ? "" : v[0]);
        check("the second argument is the verb",
              v.size() == 3 && v[1] == "exec");
    }

    // The backslash-before-quote rule: an odd run escapes the quote, an
    // even run does not.
    {
        auto v = ccap::compdb::splitWindowsCommandLine("a\\\"b");
        check("an odd backslash run escapes the quote",
              v.size() == 1 && v[0] == "a\"b", v.empty() ? "" : v[0]);
    }
    {
        // "a\\\\" -- a run of two backslashes -- then a quote, then b,
        // then a quote.
        //
        // The run is even, so both backslashes are literal and the
        // quote OPENS a quoted section. The "b" is inside it, and the
        // second quote closes it. A quote toggles the state rather
        // than pairing up, so the whole thing is ONE argument:
        // a + \\ + b.
        auto v = ccap::compdb::splitWindowsCommandLine("a\\\\\"b\"");
        std::string got;
        for (const auto& s : v) got += "[" + s + "]";
        check("an even backslash run keeps both backslashes and the quote "
              "opens a quoted section",
              v.size() == 1 && v[0] == "a\\\\b", got);
    }
    {
        // The same run, but the quoted section is EMPTY and there is a
        // space after it. Now the quote really does separate: the
        // space is outside the quotes.
        auto v = ccap::compdb::splitWindowsCommandLine("a\\\\\"\" x");
        std::string got;
        for (const auto& s : v) got += "[" + s + "]";
        check("an empty quoted section ends at the closing quote",
              v.size() == 2 && v[0] == "a\\\\" && v[1] == "x", got);
    }

    // A doubled quote inside a quoted run is a literal quote.
    {
        auto v = ccap::compdb::splitWindowsCommandLine("\"a\"\"b\"");
        check("a doubled quote is a literal quote",
              v.size() == 1 && v[0] == "a\"b", v.empty() ? "" : v[0]);
    }

    // An empty quoted argument.
    //
    // The Windows parser produces an EMPTY argument here, so the split
    // has to keep it. A path that is an empty string is meaningful
    // (it is the current directory), so dropping it would change what
    // the command means.
    {
        auto v = ccap::compdb::splitWindowsCommandLine("\"\" x");
        check("an empty quoted argument is kept as an empty token",
              v.size() == 2 && v[0].empty() && v[1] == "x",
              "size=" + std::to_string(v.size()));
    }
}

// ── The extraction, against a REAL log ───────────────────────────────────

void testExtractRealLog() {
    std::printf("\n-- the extraction from a real log --\n");

    const std::string path = fixturePath();

    std::vector<ccap::compdb::DotnetCompile> compiles;
    std::string err;
    const bool ok = ccap::compdb::extractDotnetCompiles(path, &compiles, &err);

    check("the real diagnostic log yields a compile", ok, err);
    if (!ok) return;

    check("exactly one compile was extracted", compiles.size() == 1,
          "got " + std::to_string(compiles.size()));

    if (compiles.empty()) return;
    const auto& c = compiles[0];

    // The host, the verb, the compiler dll.
    check("the argv starts with the host",
          !c.argv.empty() && c.argv[0].find("dotnet.exe") != std::string::npos,
          c.argv.empty() ? "" : c.argv[0]);
    check("the second argv is the verb",
          c.argv.size() > 1 && c.argv[1] == "exec");
    check("the third argv is the compiler dll",
          c.argv.size() > 2 && c.argv[2].find("csc.dll") != std::string::npos,
          c.argv.size() > 2 ? c.argv[2] : "");
    check("the compiler path was recorded",
          c.compilerPath.find("csc.dll") != std::string::npos);
    check("the language was recorded", c.language == "C#", c.language);

    // The response file must NOT be there: the log has it expanded.
    bool hasRsp = false;
    for (const auto& a : c.argv) {
        if (!a.empty() && a[0] == '@') hasRsp = true;
    }
    check("the log has the response file already expanded", !hasRsp);

    // The executable path was logged unquoted, so it had to be repaired.
    check("the executable path was repaired into one argument",
          c.argv[0].find(" ") != std::string::npos, c.argv[0]);
    check("the executable path is not split in two",
          c.argv[0].find("Program Files\\dotnet\\dotnet.exe") != std::string::npos,
          c.argv[0]);

    // The task-id annotation must be gone.
    bool hasAnnotation = false;
    for (const auto& a : c.argv) {
        if (a.find("ID:") != std::string::npos) hasAnnotation = true;
    }
    check("the task-id annotation was stripped", !hasAnnotation);

    // The sources and the output.
    auto cmds = ccap::compdb::toCompileCommands(compiles);
    check("the conversion produced a command", cmds.size() == 1);
    if (cmds.empty()) return;

    check("the compiler kind is csharp",
          cmds[0].kind == ccap::compdb::CompilerKind::Csharp);
    check("the source file was found",
          !cmds[0].sourceFiles.empty() &&
              cmds[0].sourceFiles[0].find("Hello.cs") != std::string::npos,
          cmds[0].sourceFiles.empty() ? "" : cmds[0].sourceFiles[0]);
    check("the output file was found",
          cmds[0].outputFile.find("Hello.dll") != std::string::npos,
          cmds[0].outputFile);
    check("the references were found", !cmds[0].classpathDirs.empty());
    check("the analyzers were found", !cmds[0].processorDirs.empty());
}

// ── The C# adapter ───────────────────────────────────────────────────────
//
// The adapter reports the compile in the analyser's own vocabulary.
//
// The analyzers matter as much as the references: a source generator
// rewrites the source before it is compiled, and an analyzer reports
// diagnostics the plain compiler would not. A consumer that reproduces
// the compile has to have them -- and they are captured, so dropping them
// in the adapter would be a silent loss.

void testCsharpAdapter() {
    std::printf("\n-- the csharp adapter --\n");

    ccap::compdb::CompileCommand cmd;
    cmd.kind = ccap::compdb::CompilerKind::Csharp;
    cmd.sourceFiles.push_back("Hello.cs");
    cmd.classpathDirs.push_back("ref/net9.0/System.dll");
    cmd.processorDirs.push_back("analyzers/NetAnalyzers.dll");
    cmd.outputFile = "obj/Debug/net9.0/Hello.dll";

    ccap::compdb::AdapterRegistry registry;
    const ccap::compdb::CompilerAdapter* adapter = registry.find(cmd);
    check("the csharp adapter was found", adapter != nullptr);
    if (!adapter) return;

    ccap::compdb::AdapterContext ctx;
    const std::vector<std::string> out = adapter->translate(cmd, ctx);

    auto has = [&out](const char* name) {
        for (const auto& a : out) {
            if (a == name) return true;
        }
        return false;
    };
    auto hasValue = [&out](const char* name, const std::string& value) {
        for (size_t i = 0; i + 1 < out.size(); ++i) {
            if (out[i] == name && out[i + 1] == value) return true;
        }
        return false;
    };

    check("the reference assembly is a system include",
          hasValue("--sys_include", "ref/net9.0/System.dll"));
    check("the analyzer is kept", hasValue("--analyzer",
                                           "analyzers/NetAnalyzers.dll"));
    check("the source file is a positional argument", has("Hello.cs"));
}

// ── The encoding ─────────────────────────────────────────────────────────
//
// The same content, written two ways. The UTF-16 one is how MSBuild
// writes it; the UTF-8 one is what a console would show. Both must
// give the same result -- and the UTF-16 one is the case that used to
// silently produce nothing.

void testEncoding() {
    std::printf("\n-- the log encoding --\n");

    // The scratch files go to the **temp** directory, not next to the
    // fixture.
    //
    // The fixture directory is checked in, so writing into it would
    // leave three stray files behind after every test run -- and they
    // would look like they belong to the test data.
    const std::string utf16 = scratchPath("tmp-utf16.txt");
    const std::string utf8 = scratchPath("tmp-utf8.txt");

    // A minimal but realistic Csc task line.
    const std::string body =
        "11:07:33.438   1:7>C:\\Program Files\\dotnet\\dotnet.exe exec "
        "\"C:\\sdk\\Roslyn\\bincore\\csc.dll\" /noconfig /target:library "
        "/out:obj\\Debug\\net9.0\\Hello.dll /embed:Hello.cs (任务 ID: 104)";

    writeFixture(utf16, body, true);
    writeFixture(utf8, body, false);

    {
        std::vector<ccap::compdb::DotnetCompile> compiles;
        std::string err;
        const bool ok =
            ccap::compdb::extractDotnetCompiles(utf16, &compiles, &err);
        check("the utf-16 log is read", ok, err);
        check("the utf-16 log yields one compile", compiles.size() == 1);
    }
    {
        std::vector<ccap::compdb::DotnetCompile> compiles;
        std::string err;
        const bool ok =
            ccap::compdb::extractDotnetCompiles(utf8, &compiles, &err);
        check("the utf-8 log is read", ok, err);
        check("the utf-8 log yields one compile", compiles.size() == 1);
    }

    // A file that is not a diagnostic log at all.
    {
        const std::string plain = scratchPath("tmp-plain.txt");
        writeFixture(plain, "just some build output\nwith no compiler in it\n",
                     false);

        std::vector<ccap::compdb::DotnetCompile> compiles;
        std::string err;
        const bool ok =
            ccap::compdb::extractDotnetCompiles(plain, &compiles, &err);
        check("a log without a compiler task is reported as such",
              !ok && !err.empty(), err);
        check("a log without a compiler task yields nothing",
              compiles.empty());
    }

    // A log that does not exist at all is a different failure from "the
    // log has no compile in it": the first is an error, the second is
    // an answer.
    {
        std::vector<ccap::compdb::DotnetCompile> compiles;
        std::string err;
        const bool ok = ccap::compdb::extractDotnetCompiles(
            std::string(CCAP_TEST_FIXTURES_DIR) + "/dotnet/no-such-file.txt",
            &compiles, &err);
        check("a missing log is an error", !ok && !err.empty());
    }
}

// ── The command recognition ──────────────────────────────────────────────

void testRecognition() {
    std::printf("\n-- the msbuild command recognition --\n");

    check("dotnet is recognized",
          ccap::compdb::isMsBuildCommand({"dotnet", "build"}));
    check("dotnet.exe is recognized",
          ccap::compdb::isMsBuildCommand({"C:\\Program Files\\dotnet\\dotnet.exe",
                                           "build"}));
    check("msbuild is recognized",
          ccap::compdb::isMsBuildCommand({"msbuild", "MySolution.sln"}));
    check("make is not recognized",
          !ccap::compdb::isMsBuildCommand({"make"}));
    check("gcc is not recognized",
          !ccap::compdb::isMsBuildCommand({"gcc", "-c", "a.c"}));
    check("an empty command is not recognized",
          !ccap::compdb::isMsBuildCommand({}));
}


// ── The build plan ───────────────────────────────────────────────────────
//
// The plan is the part of the unified entry that can be tested without
// starting a build: which backends will run, and what the build command
// looks like after ccap has adjusted it.
//
// The adjustments are the interesting part. They are what makes the
// .NET backend work at all, and each one has a measured reason.

void testBuildPlan() {
    std::printf("\n-- the build plan --\n");

    ccap::compdb::BuildOptions opts;

    // A build that runs a compiler directly cannot have a .NET part, so
    // it gets no log and no stdout hijack.
    {
        auto plan = ccap::compdb::planBuild("gcc", {"-c", "a.c"}, opts);
        check("a bare compiler driver has no msbuild log",
              plan.msbuildLog.empty());
        check("a bare compiler driver does not write a log",
              !plan.writesMsbuildLog);
        check("a bare compiler driver is not an msbuild build",
              !plan.isMsBuild);
    }

    // A compiler driver gets no MSBuild switches, because it would
    // reject them.
    //
    // Measured: "cmake --build -v:diag" fails with "Unknown argument
    // -v:diag". The same is true of "--no-incremental".
    {
        auto plan = ccap::compdb::planBuild("gcc", {"-c", "a.c"}, opts);
        for (const auto& a : plan.args) {
            check("a compiler driver gets no msbuild switch",
                  a != "-v:diag" && a != "--no-incremental", a);
        }
    }

    // A build tool that is not a compiler gets no MSBuild switches
    // either -- "cmake --build" rejects them.
    {
        auto plan = ccap::compdb::planBuild("cmake", {"--build", "b"}, opts);
        bool hasMsbuildSwitch = false;
        for (const auto& a : plan.args) {
            if (a == "-v:diag" || a == "--no-incremental") {
                hasMsbuildSwitch = true;
            }
        }
        check("a build tool gets no msbuild switch", !hasMsbuildSwitch);
    }

    // A dotnet build: the verbosity, the incremental switch and the
    // file logger are added, and a log file is planned.
    {
        auto plan = ccap::compdb::planBuild("dotnet", {"build", "x.csproj"}, opts);
        check("a dotnet build is an msbuild build", plan.isMsBuild);
        check("a dotnet build gets a log file", !plan.msbuildLog.empty());
        check("the log file is unique per run",
              plan.msbuildLog.find("ccap-msbuild") != std::string::npos);

        bool hasNoInc = false;
        bool hasFlp = false;
        bool hasDiag = false;
        for (const auto& a : plan.args) {
            if (a == "-v:diag") hasDiag = true;
            if (a == "--no-incremental") hasNoInc = true;
            if (a.rfind("-flp:logfile=", 0) == 0) hasFlp = true;
        }
        check("the incremental switch is forced off", hasNoInc);
        check("the file logger is requested", hasFlp);
        check("the file logger writes the log",
              plan.logWrittenByLogger);
        check("the diagnostic verbosity is on the logger, not the build",
              !hasDiag);
        {
            bool flpPointsAtLog = false;
            for (const auto& a : plan.args) {
                if (a.rfind("-flp:logfile=", 0) == 0 &&
                    a.find(plan.msbuildLog) != std::string::npos) {
                    flpPointsAtLog = true;
                }
            }
            check("the file logger points at the planned log", flpPointsAtLog);
        }
    }

    // A user's own verbosity is kept.
    {
        auto plan = ccap::compdb::planBuild("dotnet", {"build", "x.csproj",
                                                    "-v:minimal"},
                                         opts);
        bool hasDiag = false;
        for (const auto& a : plan.args) {
            if (a == "-v:diag") hasDiag = true;
        }
        check("the user's own verbosity is kept", !hasDiag);
    }

    // The .NET backend can be turned off.
    {
        ccap::compdb::BuildOptions off;
        off.noDotnet = true;
        auto plan = ccap::compdb::planBuild("dotnet", {"build", "x.csproj"}, off);
        check("the .NET backend can be turned off",
              plan.msbuildLog.empty() && !plan.isMsBuild);
    }

    // A given log is used as it is.
    {
        ccap::compdb::BuildOptions given;
        given.msbuildLog = "somewhere/build.log";
        auto plan = ccap::compdb::planBuild("dotnet", {"build", "x.csproj"},
                                         given);
        check("a given log is used as it is",
              plan.msbuildLog == "somewhere/build.log");
        check("a given log is not written by the build",
              !plan.writesMsbuildLog);
        check("a given log is not written by the file logger",
              !plan.logWrittenByLogger);
    }

    // A wrapper script may call "dotnet build" inside it, so it gets
    // the log even though it is not an MSBuild build itself.
    //
    // It does NOT get the MSBuild switches: the wrapper has its own
    // argument syntax and may reject them.
    {
        auto plan = ccap::compdb::planBuild("cmd", {"/c", "build.bat"}, opts);
        check("a wrapper script is not an msbuild build", !plan.isMsBuild);
        check("a wrapper script still gets the log",
              !plan.msbuildLog.empty());
        check("a wrapper script does not use the file logger",
              !plan.logWrittenByLogger);
        for (const auto& a : plan.args) {
            check("a wrapper script gets no msbuild switch",
                  a != "-v:diag" && a != "--no-incremental", a);
        }
    }

    // The user can say what a wrapper accepts, and then the switches go
    // to the wrapper's own command line -- where a script that forwards
    // its arguments passes them on.
    {
        ccap::compdb::BuildOptions given;
        given.msbuildArgs = "-v:diag -flp:logfile=build.log;verbosity=diag";
        auto plan = ccap::compdb::planBuild("cmd", {"/c", "build.bat"}, given);

        bool hasDiag = false;
        bool hasFlp = false;
        for (const auto& a : plan.args) {
            if (a == "-v:diag") hasDiag = true;
            if (a.rfind("-flp:logfile=", 0) == 0) hasFlp = true;
        }
        check("the user's msbuild switches reach the wrapper", hasDiag);
        check("the user's file logger reaches the wrapper", hasFlp);
        check("the wrapper is marked as needing the switches",
              plan.wrapperNeedsMsbuildArgs);
    }

    // The switches are split on spaces, and a quoted part stays one
    // argument -- the file logger's path may have spaces in it.
    {
        ccap::compdb::BuildOptions given;
        given.msbuildArgs =
            "-flp:logfile=\"C:\\my dir\\build.log\";verbosity=diag";
        auto plan = ccap::compdb::planBuild("cmd", {"/c", "build.bat"}, given);
        check("the quoted path stays one argument",
              plan.args.size() == 4 &&
                  plan.args[2] ==
                      "-flp:logfile=C:\\my dir\\build.log;verbosity=diag",
              plan.args.size() > 2 ? plan.args[2] : "");
    }

    // The compiler driver is recognized by its base name, so an
    // absolute path still counts as a native compiler.
    {
        auto plan = ccap::compdb::planBuild(
            "C:\\Program Files\\LLVM\\bin\\clang-cl.exe", {"/c", "a.c"}, opts);
        check("an absolute compiler path gets no log",
              plan.msbuildLog.empty());
    }

    // A command this code has never heard of gets the log: the cost of
    // a wrongly redirected stdout is lower than the cost of a silently
    // empty .NET capture.
    {
        auto plan = ccap::compdb::planBuild("some-build-tool", {"x"}, opts);
        check("an unknown build tool gets the log",
              !plan.msbuildLog.empty());
    }

    // "dotnet" on its own is not a build: "dotnet test" and "dotnet
    // exec" are not MSBuild builds, and they must not get the
    // switches.
    //
    // The plan still says "isMsBuild" for them, because "dotnet test"
    // DOES build the project underneath -- the flag only says "this
    // command may run an MSBuild build", not "this command is a
    // build".
    {
        auto plan = ccap::compdb::planBuild("dotnet", {"test"}, opts);
        for (const auto& a : plan.args) {
            check("dotnet test gets no msbuild switch",
                  a != "-v:diag" && a != "--no-incremental", a);
        }
        check("dotnet test does not use the file logger",
              !plan.logWrittenByLogger);
    }

}

}  // namespace

int main() {
    std::printf("=== ccap_compdb .NET backend tests ===\n");

    testSplit();
    testExtractRealLog();
    testCsharpAdapter();
    testEncoding();
    testRecognition();
    testBuildPlan();

    std::printf("\n===================================\n");
    std::printf("PASS: %d   FAIL: %d\n", gPass, gFail);
    std::printf("===================================\n");
    return gFail == 0 ? 0 : 1;
}
