// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb unit tests
//
// Covers:
//   1. Compiler identification (full table)
//   2. Aliases (symlinks recognize each other)
//   3. Command parsing (various option syntaxes)
//   4. Filtering (scripts must be dropped)
//   5. Header file / macro extraction
//   6. clang compilation database export
//   7. Compiler adapters (mutually isolated)
//   8. Config parsing
//
// Usage: test_compdb

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ccap_compdb.h"
#include "core/json_parser.h"
#include "core/normalize.h"

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

std::string join(const std::vector<std::string>& v, const char* sep = " ") {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) {
        if (x == s) return true;
    }
    return false;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// 1. Compiler identification
// ═══════════════════════════════════════════════════════════════════════════

void testCompilerTable() {
    std::printf("\n=== 1. Compiler identification ===\n");
    CompilerTable t = CompilerTable::builtin();

    check("gcc -> Gcc", t.matchBaseName("gcc") == CompilerKind::Gcc);
    check("g++ -> Gcc", t.matchBaseName("g++") == CompilerKind::Gcc);
    check("cc -> Gcc", t.matchBaseName("cc") == CompilerKind::Gcc);
    check("c++ -> Gcc", t.matchBaseName("c++") == CompilerKind::Gcc);
    check("clang -> Clang", t.matchBaseName("clang") == CompilerKind::Clang);
    check("clang++ -> Clang", t.matchBaseName("clang++") == CompilerKind::Clang);
    check("cl -> Msvc", t.matchBaseName("cl") == CompilerKind::Msvc);
    check("cl.exe -> Msvc", t.matchBaseName("cl.exe") == CompilerKind::Msvc);

    // Version suffix
    check("gcc-11 -> Gcc", t.matchBaseName("gcc-11") == CompilerKind::Gcc);
    check("g++-12 -> Gcc", t.matchBaseName("g++-12") == CompilerKind::Gcc);
    check("clang-15 -> Clang", t.matchBaseName("clang-15") == CompilerKind::Clang);
    check("clang++-17 -> Clang",
          t.matchBaseName("clang++-17") == CompilerKind::Clang);

    // Not a compiler
    check("sh -> Unknown", t.matchBaseName("sh") == CompilerKind::Unknown);
    check("make -> Unknown", t.matchBaseName("make") == CompilerKind::Unknown);
    check("ld -> Unknown", t.matchBaseName("ld") == CompilerKind::Unknown);
    check("as -> Unknown", t.matchBaseName("as") == CompilerKind::Unknown);

    // Full path
    check("/usr/bin/gcc -> Gcc",
          t.matchPath("/usr/bin/gcc") == CompilerKind::Gcc);
    check("C:\\msys64\\mingw64\\bin\\gcc.exe -> Gcc",
          t.matchPath("C:\\msys64\\mingw64\\bin\\gcc.exe") == CompilerKind::Gcc);

    // Table is not empty
    check("builtin table is not empty", !t.rules().empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. Aliases
// ═══════════════════════════════════════════════════════════════════════════

void testAliases() {
    std::printf("\n=== 2. Compiler aliases (symlinks recognize each other) ===\n");

    // Create two symlinks pointing at the same file
    const char* dir = std::getenv("TMPDIR");
    std::string tmp = dir ? dir : "/tmp";
    std::string real = tmp + "/ccap_test_real_gcc";
    std::string alias = tmp + "/ccap_test_alias_cc";

    // Write a fake executable file
    {
        FILE* f = std::fopen(real.c_str(), "wb");
        if (f) {
            std::fputs("#!/bin/sh\n", f);
            std::fclose(f);
        }
    }
    // Remove the old symlink
    std::remove(alias.c_str());
    bool made = (std::fopen(real.c_str(), "rb") != nullptr);
    if (!made) {
        std::printf("skip symlink test (cannot create the test file)\n");
    }

    AliasResolver resolver;
    resolver.add(real);

    // The real path should be normalized
    std::string resolved = AliasResolver::resolveRealPath(real);
    check("real path resolved successfully", !resolved.empty());

    if (made) {
        // Create the symlink: alias -> real
        //
        // Note that we **must not** rename(real, alias) first ——
        // that would move real away, so realpath(real) would fail,
        // the symlink would become a dangling link and realpath(alias)
        // would fail too, so both sides degrade to normalize_path
        // and never match.
        // The symptom is "the alias list is empty", which looks like
        // a product bug but is really the test deleting its own data.
        std::string cmd = "ln -sf '" + real + "' '" + alias + "'";
        int rc = std::system(cmd.c_str());
        if (rc == 0) {
            resolver.add(alias);
            const auto& al = resolver.aliasesOf(real);
            check("symlink recognized as an alias",
                  contains(al, alias),
                  "aliases=" + join(al, ","));

            // Mutual recognition: the alias side must also see real
            const auto& al2 = resolver.aliasesOf(alias);
            check("aliases recognize each other (both directions)",
                  contains(al2, real),
                  "aliases=" + join(al2, ","));
        } else {
            std::printf("skip symlink test (ln failed)\n");
        }
        std::remove(alias.c_str());
    }
    std::remove(real.c_str());

    // Compiler identity
    CompilerTable t = CompilerTable::builtin();
    AliasResolver r2;
    r2.add("/usr/bin/gcc");
    auto id = r2.identify("/usr/bin/gcc", t);
    check("identify recognizes gcc", id.kind == CompilerKind::Gcc,
          std::string("kind=") + compilerKindName(id.kind));
    check("identify reports the real path", !id.realPath.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. Command parsing
// ═══════════════════════════════════════════════════════════════════════════

void testParsing() {
    std::printf("\n=== 3. Command parsing ===\n");
    CommandParser p;

    // GCC style
    {
        auto cmd = p.parseLine(
            "g++ -I../include -DFOO=1 -DBAR -std=c++17 -c src/main.cpp -o build/main.o",
            "/home/user/proj");
        check("GCC kind is correct", cmd.kind == CompilerKind::Gcc);
        check("source file identified", cmd.sourceFiles.size() == 1 &&
                            cmd.sourceFiles[0].find("main.cpp") != std::string::npos,
              join(cmd.sourceFiles, ","));
        check("source file turned into an absolute path",
              cmd.sourceFiles[0] == "/home/user/proj/src/main.cpp",
              cmd.sourceFiles[0]);
        check("include path", cmd.includeDirs.size() == 1 &&
                              cmd.includeDirs[0] == "../include",
              join(cmd.includeDirs, ","));
        check("macro FOO=1", cmd.defines.size() == 2 &&
                           cmd.defines[0].first == "FOO" &&
                           cmd.defines[0].second == "1",
              cmd.defines.empty() ? "" : cmd.defines[0].first);
        check("macro BAR (no value)",
              cmd.defines.size() == 2 && cmd.defines[1].first == "BAR" &&
                  cmd.defines[1].second.empty());
        check("language standard", cmd.languageStandard == "c++17",
              cmd.languageStandard);
        check("output file", cmd.outputFile == "build/main.o", cmd.outputFile);
        check("not link-only", !cmd.isLinkOnly);
    }

    // MSVC style
    {
        auto cmd = p.parseLine(
            "cl.exe /I..\\include /DFOO=2 /std:c++17 /c src\\main.cpp /Fomain.obj",
            "C:\\proj");
        check("MSVC kind is correct", cmd.kind == CompilerKind::Msvc);
        check("MSVC include", cmd.includeDirs.size() == 1 &&
                                 cmd.includeDirs[0] == "..\\include",
              join(cmd.includeDirs, ","));
        check("MSVC macro", cmd.defines.size() == 1 &&
                            cmd.defines[0].first == "FOO" &&
                            cmd.defines[0].second == "2");
        check("MSVC standard", cmd.languageStandard == "c++17",
              cmd.languageStandard);
        check("MSVC source file",
              cmd.sourceFiles.size() == 1 &&
                  cmd.sourceFiles[0].find("main.cpp") != std::string::npos,
              join(cmd.sourceFiles, ","));
    }

    // -isystem
    {
        auto cmd = p.parseLine(
            "gcc -isystem /usr/include/c++/11 -c a.c", "/tmp");
        check("-isystem lands in system_dirs", cmd.systemDirs.size() == 1 &&
                                               cmd.systemDirs[0] == "/usr/include/c++/11",
              join(cmd.systemDirs, ","));
    }

    // Link only
    {
        auto cmd = p.parseLine("g++ build/a.o build/b.o -o app", "/tmp");
        check("pure link recognized", cmd.isLinkOnly);
        check("link has no source files", cmd.sourceFiles.empty());
    }

    // Quoted path (Windows style)
    {
        auto cmd = p.parseLine(
            "cl.exe /I\"C:\\Program Files\\dep\\include\" /c a.cpp", "C:\\p");
        check("quoted include",
              cmd.includeDirs.size() == 1 &&
                  cmd.includeDirs[0] == "C:\\Program Files\\dep\\include",
              cmd.includeDirs.empty() ? "" : cmd.includeDirs[0]);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. Build log parsing
// ═══════════════════════════════════════════════════════════════════════════

void testBuildLog() {
    std::printf("\n=== 4. build log parsing ===\n");

    // Real format (taken from a build log produced by a static-analysis
    // build wrapper)
    std::string log =
        "EXECUTING 'make all '\n"
        "EXECUTING '/bin/sh -c cd qmake && make '\n"
        "CWD = /export/home/acc/test-packages/qt-x11-free-3.3.2/qmake\n"
        "COMPILING '/export/home/acc/prevent/bin/sa-translate g++ -c -o property.o "
        "-I. -DQT_NO_TEXTCODEC property.cpp '\n"
        "EXECUTING '/usr/ccs/bin/as -Qy -s -o property.o /var/tmp/cc2Wo7sG.s '\n";

    CommandParser p;
    auto cmds = p.parseBuildLog(log);
    check("parsed 4 commands", cmds.size() == 4, "actual=" + std::to_string(cmds.size()));

    if (cmds.size() >= 4) {
        check("command 1 is make",
              cmds[0].compilerBase == "make", cmds[0].compilerBase);
        check("command 2 is sh", cmds[1].compilerBase == "sh",
              cmds[1].compilerBase);
        check("command 3 picked up the CWD",
              cmds[2].directory ==
                  "/export/home/acc/test-packages/qt-x11-free-3.3.2/qmake",
              cmds[2].directory);
        // Key point: the first token of the COMPILING line is the
        // wrapper and the real compiler is the second one ——
        // the prefix must be stripped.
        check("wrapper prefix stripped (compiler is g++)",
              cmds[2].compilerBase == "g++", cmds[2].compilerBase);
        check("kind is correct after stripping the prefix",
              cmds[2].kind == CompilerKind::Gcc,
              compilerKindName(cmds[2].kind));
        check("macros of command 3 extracted",
              cmds[2].defines.size() == 1 &&
                  cmds[2].defines[0].first == "QT_NO_TEXTCODEC",
              cmds[2].defines.empty() ? "" : cmds[2].defines[0].first);
        check("source file of command 3",
              cmds[2].sourceFiles.size() == 1 &&
                  cmds[2].sourceFiles[0].find("property.cpp") != std::string::npos,
              join(cmds[2].sourceFiles, ","));
        check("command 4 is as", cmds[3].compilerBase == "as",
              cmds[3].compilerBase);
    }

    // ── the wrapper prefix list is generic, not vendor-specific ─────────
    //
    // The parser has to recognise "a wrapper sits in front of the real
    // compiler" -- a shape -- rather than one vendor's spelling. The list
    // therefore uses tool-neutral names.
    //
    // These assertions exist so the stripping cannot silently regress:
    // if the list is emptied or renamed without updating the test, the
    // compiler would be recognised as the wrapper and every argument would
    // be parsed wrong.
    {
        CommandParser p;
        struct Case {
            const char* wrapper;
            const char* expectedCompiler;
        };
        const Case kCases[] = {
            {"sa-translate", "g++"},
            {"sa-emit", "g++"},
            {"sa-build", "g++"},
            {"sa-analyze", "g++"},
            {"tool-translate", "g++"},
            {"tool-emit", "gcc"},
        };
        bool allOk = true;
        for (const auto& c : kCases) {
            std::string line = std::string("/opt/bin/") + c.wrapper +
                               " " + c.expectedCompiler + " -c main.cpp";
            auto cmd = p.parseLine(line, "/work");
            if (cmd.compilerBase != c.expectedCompiler) {
                allOk = false;
            }
        }
        check("every known wrapper prefix is stripped", allOk);
    }

    // ── an unknown wrapper is NOT stripped ─────────────────────────────
    //
    // Only known names may be stripped: the user might really have a tool
    // whose name merely looks similar, and silently swallowing it would
    // hide a real compiler.
    {
        CommandParser p;
        auto cmd = p.parseLine("/opt/bin/some-other-wrapper g++ -c main.cpp",
                               "/work");
        check("an unknown wrapper is left alone",
              cmd.compilerBase == "some-other-wrapper", cmd.compilerBase);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. Filtering
// ═══════════════════════════════════════════════════════════════════════════

void testFilter() {
    std::printf("\n=== 5. Filtering (scripts must be dropped) ===\n");
    CommandParser p;
    FilterRules rules(FilterConfig::builtin());

    struct Case {
        const char* cmd;
        bool keep;
        const char* why;
    };
    Case cases[] = {
        {"g++ -c main.cpp", true, "compilers are kept"},
        {"gcc -c a.c", true, "compilers are kept"},
        {"clang++ -c a.cpp", true, "compilers are kept"},
        {"cl.exe /c a.cpp", true, "MSVC is kept"},
        {"sh -c 'g++ -c a.cpp'", false, "scripts are dropped"},
        {"bash build.sh", false, "scripts are dropped"},
        {"cmd.exe /C build.bat", false, "scripts are dropped"},
        {"make -j8", false, "build tools are dropped"},
        {"cmake --build .", false, "build tools are dropped"},
        {"ld -o app a.o", false, "linkers are dropped"},
        {"as -o a.o a.s", false, "assemblers are dropped"},
        {"ar rcs lib.a a.o", false, "archivers are dropped"},
        {"python setup.py build", false, "scripts are dropped"},
        {"collect2 -o app a.o", false, "linkers are dropped"},
    };

    for (const auto& c : cases) {
        auto cmd = p.parseLine(c.cmd, "/tmp");
        bool got = rules.keep(cmd);
        check(c.why, got == c.keep,
              std::string(c.cmd) + " -> " + rules.explain(cmd));
    }

    // Allowlist
    {
        FilterConfig cfg = FilterConfig::builtin();
        cfg.includeNames = {"g++"};
        FilterRules r2(cfg);
        auto gxx = p.parseLine("g++ -c a.cpp", "/tmp");
        auto gcc = p.parseLine("gcc -c a.c", "/tmp");
        check("kept when inside the allowlist", r2.keep(gxx));
        check("dropped when outside the allowlist", !r2.keep(gcc));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 6. clang compilation database export
// ═══════════════════════════════════════════════════════════════════════════

void testExport() {
    std::printf("\n=== 6. clang compilation database export ===\n");
    CommandParser p;

    std::vector<CompileCommand> cmds;
    cmds.push_back(p.parseLine(
        "g++ -I../include -DFOO=1 -c src/main.cpp -o main.o", "/home/u/proj"));
    cmds.push_back(p.parseLine(
        "gcc -c src/util.c -o util.o", "/home/u/proj"));

    std::string out = exportClangCompilationDb(cmds);
    check("output starts with [", !out.empty() && out[0] == '[', out.substr(0, 20));
    check("contains the directory field", out.find("\"directory\"") != std::string::npos);
    check("contains the arguments field", out.find("\"arguments\"") != std::string::npos);
    check("contains the file field", out.find("\"file\"") != std::string::npos);
    check("file is an absolute path",
          out.find("/home/u/proj/src/main.cpp") != std::string::npos);

    // Must be valid JSON
    std::string err;
    auto root = jsonParse(out, &err);
    check("output is valid JSON", root != nullptr, err);
    if (root && root->isArray()) {
        check("has 2 entries", root->array.size() == 2,
              "actual=" + std::to_string(root->array.size()));
        if (root->array.size() == 2) {
            const JsonValue* a = root->array[0].get();
            check("entry has an arguments array",
                  a->get("arguments") && a->get("arguments")->isArray());
        }
    }

    // Reverse parsing (round trip)
    auto back = p.parseCompilationDb(out);
    check("can be parsed back", back.size() == 2,
          "actual=" + std::to_string(back.size()));
    if (back.size() == 2) {
        check("include survives the round trip", back[0].includeDirs.size() == 1,
              join(back[0].includeDirs, ","));
        check("macros survive the round trip", back[0].defines.size() == 1,
              back[0].defines.empty() ? "" : back[0].defines[0].first);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 7. Compiler adapters
// ═══════════════════════════════════════════════════════════════════════════

void testAdapters() {
    std::printf("\n=== 7. Compiler adapters (mutually isolated) ===\n");
    CommandParser p;
    AdapterRegistry reg;

    // GCC
    {
        auto cmd = p.parseLine(
            "g++ -I../inc -DA=1 -isystem /usr/include/c++/11 -std=c++17 -c main.cpp",
            "/tmp");
        const CompilerAdapter* a = reg.find(cmd);
        check("GCC command selects the gcc adapter",
              a && a->kind() == CompilerKind::Gcc,
              a ? a->name() : "(null)");
        if (a) {
            AdapterContext ctx;
            auto args = a->translate(cmd, ctx);
            check("GCC adapter emits --sys_include",
                  join(args, " ").find("--sys_include") != std::string::npos,
                  join(args, " "));
            check("GCC adapter emits -I",
                  join(args, " ").find("-I") != std::string::npos);
            check("GCC adapter emits -D",
                  join(args, " ").find("-DA=1") != std::string::npos);
            check("GCC adapter never emits /D (MSVC syntax)",
                  join(args, " ").find("/D") == std::string::npos,
                  join(args, " "));
        }
    }

    // MSVC
    {
        auto cmd = p.parseLine(
            "cl.exe /I..\\inc /DA=1 /std:c++17 /c main.cpp", "C:\\p");
        const CompilerAdapter* a = reg.find(cmd);
        check("MSVC command selects the msvc adapter",
              a && a->kind() == CompilerKind::Msvc,
              a ? a->name() : "(null)");
        if (a) {
            AdapterContext ctx;
            auto args = a->translate(cmd, ctx);
            check("MSVC adapter uses /D", join(args, " ").find("/DA=1") != std::string::npos,
                  join(args, " "));
            check("MSVC adapter uses /I", join(args, " ").find("/I") != std::string::npos,
                  join(args, " "));
            check("MSVC adapter never emits -D (GCC syntax)",
                  join(args, " ").find("-D") == std::string::npos,
                  join(args, " "));
        }
    }

    // Clang
    {
        auto cmd = p.parseLine("clang++ -c a.cpp", "/tmp");
        const CompilerAdapter* a = reg.find(cmd);
        check("Clang command selects the clang adapter",
              a && a->kind() == CompilerKind::Clang,
              a ? a->name() : "(null)");
    }

    // Unknown compiler -> generic fallback
    {
        auto cmd = p.parseLine("my-custom-cc -c a.c", "/tmp");
        const CompilerAdapter* a = reg.find(cmd);
        check("unknown compiler falls back to generic",
              a && a->kind() == CompilerKind::Generic,
              a ? a->name() : "(null)");
    }

    // Adapter count
    check("registry has 4 adapters", reg.all().size() == 4,
          "actual=" + std::to_string(reg.all().size()));
}

// ═══════════════════════════════════════════════════════════════════════════
// 8. Config
// ═══════════════════════════════════════════════════════════════════════════

void testConfig() {
    std::printf("\n=== 8. Config parsing ===\n");

    std::string json = R"({
      "capture": {
        "compilers": [
          { "kind": "gcc", "id": "arm-gcc",
            "paths": ["/opt/tc/bin/arm-none-eabi-gcc"],
            "prefixes": ["arm-none-eabi-"],
            "enabled": true }
        ],
        "exclude": ["my-wrapper"],
        "include": []
      },
      "system_includes": {
        "gcc": ["/usr/lib/gcc/x86_64-linux-gnu/11/include"]
      },
      "output": { "path": "out.json", "format": "clang" }
    })";

    CompdbConfig cfg;
    std::string err;
    bool ok = CompdbConfig::load(json, &cfg, &err);
    check("config parsed successfully", ok, err);
    if (ok) {
        check("1 compiler entry", cfg.compilers.size() == 1);
        if (!cfg.compilers.empty()) {
            check("kind is correct", cfg.compilers[0].kind == "gcc");
            check("id is correct", cfg.compilers[0].id == "arm-gcc");
            check("paths are correct",
                  cfg.compilers[0].paths.size() == 1 &&
                      cfg.compilers[0].paths[0].find("arm-none-eabi") !=
                          std::string::npos);
            check("prefixes are correct", cfg.compilers[0].prefixes.size() == 1);
        }
        check("exclude is correct",
              cfg.filter.excludeNames.size() == 1 &&
                  cfg.filter.excludeNames[0] == "my-wrapper");
        check("systemIncludes is correct", cfg.systemIncludes.size() == 1);
        check("output path is correct", cfg.outputPath == "out.json",
              cfg.outputPath);
    }

    // Bad JSON
    {
        CompdbConfig bad;
        std::string e2;
        check("bad JSON is rejected", !CompdbConfig::load("{invalid", &bad, &e2));
        check("error message is not empty", !e2.empty());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 9. Header file / macro information
// ═══════════════════════════════════════════════════════════════════════════

void testHeadersAndMacros() {
    std::printf("\n=== 9. Header file / macro extraction ===\n");
    CommandParser p;

    auto cmd = p.parseLine(
        "g++ -I/usr/local/include -I/opt/inc "
        "-DFEATURE_X=1 -DPLAIN -DNAME=\"value\" "
        "-isystem /usr/include/c++/11 -UOLD_MACRO "
        "-c src/main.cpp",
        "/proj");

    check("2 include paths", cmd.includeDirs.size() == 2,
          join(cmd.includeDirs, ","));
    check("include path contents are correct",
          cmd.includeDirs.size() == 2 &&
              cmd.includeDirs[0] == "/usr/local/include" &&
              cmd.includeDirs[1] == "/opt/inc",
          join(cmd.includeDirs, ","));

    check("3 macros", cmd.defines.size() == 3, "actual=" + std::to_string(cmd.defines.size()));
    if (cmd.defines.size() == 3) {
        check("macro FEATURE_X=1",
              cmd.defines[0].first == "FEATURE_X" &&
                  cmd.defines[0].second == "1");
        check("macro PLAIN (no value)",
              cmd.defines[1].first == "PLAIN" &&
                  cmd.defines[1].second.empty());
        check("macro NAME with a quoted value",
              cmd.defines[2].first == "NAME" &&
                  cmd.defines[2].second == "\"value\"",
              cmd.defines[2].second);
    }

    check("1 system include", cmd.systemDirs.size() == 1,
          join(cmd.systemDirs, ","));
    check("-U does not enter the macro table",
          cmd.defines.size() == 3 &&
              !contains({cmd.defines[0].first, cmd.defines[1].first,
                         cmd.defines[2].first},
                        "OLD_MACRO"));
}

// ═══════════════════════════════════════════════════════════════════════════
// 10. ccap event stream parsing
// ═══════════════════════════════════════════════════════════════════════════

void testCaptureEvents() {
    std::printf("\n=== 10. ccap event stream parsing ===\n");

    std::string ndjson =
        R"({"seq":1,"platform":"linux","source":"runtime","pid":100,"ppid":99,"depth":1,"kind":"process","executable":"c++","argv":["c++","-I../inc","-DFOO","-c","main.cpp"],"commandLine":"c++ -I../inc -DFOO -c main.cpp","cwd":"/build","isBuiltin":false,"resolved":true,"truncated":false,"scriptPath":"","scriptLine":0,"execIndex":0,"notes":[]})"
        "\n"
        R"({"seq":2,"platform":"linux","source":"static","pid":0,"ppid":0,"depth":0,"kind":"builtin","executable":"echo","argv":["echo","hi"],"commandLine":"","cwd":"/build","isBuiltin":true,"resolved":false,"truncated":false,"scriptPath":"a.sh","scriptLine":1,"execIndex":0,"notes":[]})"
        "\n"
        R"({"seq":3,"platform":"linux","source":"runtime","pid":101,"ppid":100,"depth":2,"kind":"process","executable":"as","argv":["as","-o","a.o","a.s"],"commandLine":"as -o a.o a.s","cwd":"/build","isBuiltin":false,"resolved":true,"truncated":false,"scriptPath":"","scriptLine":0,"execIndex":0,"notes":[]})"
        "\n";

    CommandParser p;
    auto cmds = p.parseCaptureEvents(ndjson);
    check("parsed 2 runtime commands", cmds.size() == 2,
          "actual=" + std::to_string(cmds.size()));
    if (cmds.size() == 2) {
        check("command 1 is c++", cmds[0].compilerBase == "c++",
              cmds[0].compilerBase);
        check("cwd of command 1", cmds[0].directory == "/build",
              cmds[0].directory);
        check("include of command 1", cmds[0].includeDirs.size() == 1 &&
                                     cmds[0].includeDirs[0] == "../inc",
              join(cmds[0].includeDirs, ","));
        check("macros of command 1", cmds[0].defines.size() == 1 &&
                               cmds[0].defines[0].first == "FOO");
        check("static events are excluded", cmds.size() == 2);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 12. UTF-8 BOM
//
// Background: editors on Windows and PowerShell 5.1's
// `Set-Content -Encoding UTF8` both write a BOM. The BOM is not content, it
// is an encoding marker —— keeping it makes every **prefix-matching** parser
// fail silently:
//
//     "EXECUTING 'g++ ...'"  ->  rfind("EXECUTING ", 0) != 0
//     -> 0 commands parsed, and **no error reported at all**
//
// The symptom is "nothing at all", not "parsing failed" —— the worst kind
// of bug.
//
// The fix lives in read_file, so one change covers every consumer.
// ═══════════════════════════════════════════════════════════════════════════

void testBom() {
    std::printf("\n=== 12. UTF-8 BOM handling ===\n");

    const char* kBom = "\xEF\xBB\xBF";

    // ── read_file strips the BOM ─────────────────────────────────────────
    {
        const char* path = "ccap_test_bom.txt";
        {
            std::FILE* f = std::fopen(path, "wb");
            if (!f) {
                check("can create the test file", false, path);
            } else {
                std::fputs(kBom, f);
                std::fputs("EXECUTING 'g++ -c foo.cpp '\n", f);
                std::fclose(f);

                std::string content;
                bool ok = readFile(path, &content);
                check("read_file succeeded", ok, path);
                check("BOM was stripped", !content.empty() &&
                                        content.compare(0, 3, kBom) != 0,
                      content.substr(0, 6));
                check("content starts with EXECUTING",
                      content.rfind("EXECUTING ", 0) == 0, content.substr(0, 20));

                // Key point: once the BOM is gone the build log parser
                // recognizes the log.
                CommandParser p;
                auto cmds = p.parseBuildLog(content);
                check("log with BOM parses into 1 command", cmds.size() == 1,
                      "actual=" + std::to_string(cmds.size()));
                if (!cmds.empty()) {
                    check("command content is correct", cmds[0].compilerBase == "g++",
                          cmds[0].compilerBase);
                }
                std::remove(path);
            }
        }
    }

    // ── No BOM means no change ───────────────────────────────────────────
    {
        const char* path = "ccap_test_nobom.txt";
        {
            std::FILE* f = std::fopen(path, "wb");
            if (f) {
                std::fputs("EXECUTING 'g++ -c foo.cpp '\n", f);
                std::fclose(f);

                std::string content;
                readFile(path, &content);
                check("content is unchanged without a BOM",
                      content.rfind("EXECUTING ", 0) == 0,
                      content.substr(0, 20));

                CommandParser p;
                auto cmds = p.parseBuildLog(content);
                check("parses into 1 command without a BOM too", cmds.size() == 1,
                      "actual=" + std::to_string(cmds.size()));
                std::remove(path);
            }
        }
    }

    // ── A JSON config with a BOM must work as well ───────────────────────
    {
        const char* path = "ccap_test_bom.json";
        {
            std::FILE* f = std::fopen(path, "wb");
            if (f) {
                std::fputs(kBom, f);
                std::fputs("{\"output\":{\"path\":\"out.json\"}}", f);
                std::fclose(f);

                CompdbConfig cfg;
                std::string err;
                bool ok = CompdbConfig::loadFile(path, &cfg, &err);
                check("config with BOM loads", ok, err);
                if (ok) {
                    check("config content is correct", cfg.outputPath == "out.json",
                          cfg.outputPath);
                }
                std::remove(path);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// clang command translation
// ═══════════════════════════════════════════════════════════════════════════
void testClangTranslation() {
    std::printf("\n=== 12. clang command translation ===\n");

    CommandParser parser;

    // ── MSVC -> clang ──────────────────────────────────────────────────
    {
        const char* cmdline =
            "cl.exe /c /I\"..\\inc\" /DWIN32 /DVERSION=2 /std:c++17 "
            "/EHsc /Zi /O2 /Fo\"x64\\main.obj\" /MD main.cpp";
        auto cmd = parser.parseLine(cmdline, "/work");
        check("MSVC command parsed", cmd.kind == CompilerKind::Msvc);

        auto args = toClangCommand(cmd);
        std::string joined;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) joined += " ";
            joined += args[i];
        }

        check("driver is clang++ for a C++ source",
              !args.empty() && args[0] == "clang++", joined);
        check("include path translated to -I",
              joined.find("-I ..\\inc") != std::string::npos, joined);
        check("define without value translated",
              joined.find("-DWIN32") != std::string::npos, joined);
        check("define with value translated",
              joined.find("-DVERSION=2") != std::string::npos, joined);
        check("standard translated",
              joined.find("-std=c++17") != std::string::npos, joined);
        check("debug info translated to -g",
              joined.find("-g") != std::string::npos, joined);
        check("optimisation translated to -O2",
              joined.find("-O2") != std::string::npos, joined);
        check("output translated to -o",
              joined.find("-o x64\\main.obj") != std::string::npos, joined);
        check("compile-only preserved as -c",
              joined.find("-c") != std::string::npos, joined);
        // No MSVC-style option may survive the translation.
        //
        // A path may legitimately start with '/', so the test cannot simply
        // look for a leading slash. It looks for the *option spellings* that
        // MSVC uses: a slash followed by a letter, and not a path separator
        // followed by a path component.
        bool leaked = false;
        for (const auto& a : args) {
            if (a.size() < 2 || a[0] != '/') continue;
            if (!std::isalpha(static_cast<unsigned char>(a[1]))) continue;
            // "/work/main.cpp" is a path, "/D" is an option. A path segment
            // that looks like a single letter followed by more path is a
            // path; an option is a short token.
            if (a.find('/') != a.rfind('/') && a.find('\\') == std::string::npos &&
                a.size() > 3) {
                continue;   // looks like a path
            }
            leaked = true;
        }
        check("no MSVC option leaked through", !leaked, joined);

        auto losses = clangTranslationLosses(cmd);
        bool sawEhsc = false;
        bool sawRuntime = false;
        for (const auto& l : losses) {
            if (l.find("/EHsc") != std::string::npos) sawEhsc = true;
            if (l.find("runtime library") != std::string::npos) sawRuntime = true;
        }
        check("lossy option /EHsc is reported", sawEhsc);
        check("runtime library loss is reported", sawRuntime);
    }

    // ── gcc -> clang ───────────────────────────────────────────────────
    {
        const char* cmdline =
            "g++ -c -I. -DFOO=1 -std=c++17 -o main.o main.cpp";
        auto cmd = parser.parseLine(cmdline, "/work");
        check("gcc command parsed", cmd.kind == CompilerKind::Gcc);

        auto args = toClangCommand(cmd);
        std::string joined;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) joined += " ";
            joined += args[i];
        }
        check("gcc driver becomes clang++",
              !args.empty() && args[0] == "clang++", joined);
        check("gcc -c preserved", joined.find("-c") != std::string::npos, joined);
        check("gcc output preserved", joined.find("-o main.o") != std::string::npos,
              joined);
        check("gcc command has no losses",
              clangTranslationLosses(cmd).empty());
    }

    // ── C source -> clang (not clang++) ────────────────────────────────
    {
        auto cmd = parser.parseLine("gcc -c -o main.o main.c", "/work");
        auto args = toClangCommand(cmd);
        check("driver is clang for a C source",
              !args.empty() && args[0] == "clang");
    }

    // ── the compile-only flag survives the parser ──────────────────────
    {
        auto msvc = parser.parseLine("cl.exe /c main.cpp", "/work");
        check("MSVC /c sets compileOnly", msvc.compileOnly);
        auto gcc = parser.parseLine("g++ -c main.cpp", "/work");
        check("gcc -c sets compileOnly", gcc.compileOnly);
        auto link = parser.parseLine("g++ main.cpp -o app", "/work");
        check("a link command does not set compileOnly", !link.compileOnly);
    }

    // ── the exported text form ─────────────────────────────────────────
    {
        std::vector<CompileCommand> cmds;
        cmds.push_back(parser.parseLine(
            "cl.exe /c /I. /DDEBUG main.cpp", "/work"));
        std::string out = exportClangCommands(cmds);
        check("clang command export is not empty", !out.empty());
        check("clang command export starts with the driver",
              out.rfind("clang++", 0) == 0, out);
        check("clang command export has one line",
              out.find('\n') == out.size() - 1, out);
    }

    // ── MSVC /TP: a .c file compiled as C++ ───────────────────────────
    //
    // The extension says "C", but the compiler was told "C++".
    // The driver must be clang++, because clang++ links libstdc++ and
    // clang does not -- picking by extension alone would silently change
    // the ABI of the produced object.
    {
        auto cmd = parser.parseLine("cl.exe /c /TP foo.c", "/work");
        check("/TP sets the forced language",
              cmd.forcedLanguage == "cpp", cmd.forcedLanguage);
        auto args = toClangCommand(cmd);
        check("/TP selects clang++ for a .c file",
              !args.empty() && args[0] == "clang++",
              args.empty() ? "" : args[0]);
    }

    // ── MSVC /Tp<file>: one specific file ─────────────────────────────
    {
        auto cmd = parser.parseLine("cl.exe /c /Tpbar.cpp baz.c", "/work");
        check("/Tp records a per-file language",
              cmd.perFileLanguages.size() == 1 &&
                  cmd.perFileLanguages[0].first == "bar.cpp" &&
                  cmd.perFileLanguages[0].second == "cpp");
        check("/Tp does not set the global forced language",
              cmd.forcedLanguage.empty(), cmd.forcedLanguage);
    }

    // ── MSVC /TC: force C ─────────────────────────────────────────────
    {
        auto cmd = parser.parseLine("cl.exe /c /TC foo.cpp", "/work");
        check("/TC sets the forced language",
              cmd.forcedLanguage == "c", cmd.forcedLanguage);
        auto args = toClangCommand(cmd);
        check("/TC selects clang for a .cpp file",
              !args.empty() && args[0] == "clang",
              args.empty() ? "" : args[0]);
    }

    // ── the per-file override beats the global one ─────────────────────
    {
        auto cmd = parser.parseLine("cl.exe /c /Tpbar.cpp /TP baz.c", "/work");
        check("the global language is recorded",
              cmd.forcedLanguage == "cpp", cmd.forcedLanguage);
        check("the per-file language is recorded too",
              cmd.perFileLanguages.size() == 1);
        // bar.cpp is C++ by name, baz.c is C++ by /TP -> clang++ either way.
        auto args = toClangCommand(cmd);
        check("a C++ command uses clang++",
              !args.empty() && args[0] == "clang++",
              args.empty() ? "" : args[0]);
    }

    // ── /TP and friends do not leak into the raw option list ───────────
    //
    // If they stayed in otherOptions they would show up as "unknown
    // options" in the show output, and the loss report would complain
    // about options that were actually understood.
    {
        auto cmd = parser.parseLine("cl.exe /c /TP /TC /Tpfoo.cpp main.cpp",
                                    "/work");
        bool leaked = false;
        for (const auto& opt : cmd.otherOptions) {
            if (opt == "/TP" || opt == "/TC" || opt.rfind("/Tp", 0) == 0) {
                leaked = true;
            }
        }
        check("the language flags are consumed by the parser", !leaked);
    }

    // ── force-included headers ─────────────────────────────────────────
    //
    // A forced include changes the meaning of the translation unit, so
    // dropping it would make the translated command compile something
    // **different** -- worse than reporting a loss.
    {
        auto msvc = parser.parseLine("cl.exe /c /FIprelude.h main.cpp", "/work");
        check("MSVC /FI is recorded",
              msvc.preIncludes.size() == 1 && msvc.preIncludes[0] == "prelude.h",
              msvc.preIncludes.empty() ? "" : msvc.preIncludes[0]);
        auto args = toClangCommand(msvc);
        bool found = false;
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "-include" && args[i + 1] == "prelude.h") found = true;
        }
        check("the clang command keeps the forced include", found);
    }
    {
        auto gcc = parser.parseLine("g++ -c -include prelude.h main.cpp", "/work");
        check("gcc -include is recorded",
              gcc.preIncludes.size() == 1 && gcc.preIncludes[0] == "prelude.h",
              gcc.preIncludes.empty() ? "" : gcc.preIncludes[0]);
    }
    {
        // The analyzer front end gets the preinclude too.
        auto cmd = parser.parseLine("g++ -c -include prelude.h main.cpp", "/work");
        auto* adapter = adapters::makeGccAdapter();
        AdapterContext ctx;
        auto args = adapter->translate(cmd, ctx);
        bool found = false;
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "--preinclude" && args[i + 1] == "prelude.h") {
                found = true;
            }
        }
        check("the analyzer arguments keep the forced include", found);
    }
}

}  // namespace

int main() {
    std::printf("=== ccap_compdb unit tests ===\n");

    testCompilerTable();
    testAliases();
    testParsing();
    testBuildLog();
    testFilter();
    testExport();
    testAdapters();
    testConfig();
    testHeadersAndMacros();
    testCaptureEvents();
    testBom();
    testClangTranslation();

    std::printf("\n===================================\n");
    std::printf("PASS: %d   FAIL: %d\n", gPass, gFail);
    std::printf("===================================\n");
    return gFail == 0 ? 0 : 1;
}
