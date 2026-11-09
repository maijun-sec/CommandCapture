// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - the Go toolchain tests
//
// The Go toolchain is the first kind in this repository that is
// recognized by its **path** rather than by its name, so most of these
// tests are about one question: "is this the Go tool, or is it
// something else that happens to be called 'compile' or 'link'?".
//
// That question matters more than it looks. A program called "link"
// that has nothing to do with Go would be handed to the Go parser,
// where "-o" and "-p" mean something completely different, and the
// record of the build would be wrong in a way nothing would show.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "ccap_compdb.h"
#include "ccap_compdb_go.h"

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

std::string fixturePath() {
    return std::string(CCAP_TEST_FIXTURES_DIR) + "/go/importcfg.txt";
}

// The toolchain directory of a Go installation.
const char* kToolDir = "/usr/local/go/pkg/tool/linux_amd64";

// ═══════════════════════════════════════════════════════════════════════════
// 1. Recognition -- the path shape
// ═══════════════════════════════════════════════════════════════════════════

void testRecognition() {
    std::printf("\n=== 1. Recognition ===\n");
    CompilerTable t = CompilerTable::builtin();

    // The two tools that matter.
    check("the compile tool is recognized",
          t.matchPath(std::string(kToolDir) + "/compile") == CompilerKind::Go);
    check("the link tool is recognized",
          t.matchPath(std::string(kToolDir) + "/link") == CompilerKind::Go);

    // The same tools on Windows.
    check("the windows compile tool is recognized",
          t.matchPath("C:\\Go\\pkg\\tool\\windows_amd64\\compile.exe") ==
              CompilerKind::Go);
    check("the windows link tool is recognized",
          t.matchPath("C:\\Go\\pkg\\tool\\windows_amd64\\link.exe") ==
              CompilerKind::Go);

    // The toolchain directory is "<goos>_<goarch>".
    check("the darwin toolchain is recognized",
          t.matchPath("/usr/local/go/pkg/tool/darwin_arm64/compile") ==
              CompilerKind::Go);
    check("the js toolchain is recognized",
          t.matchPath("/usr/local/go/pkg/tool/js_wasm/compile") ==
              CompilerKind::Go);

    // ── The names alone are not enough ─────────────────────────────────
    //
    // This is the whole point of the path shape: an unrelated program
    // called "compile" or "link" must not be mistaken for the Go
    // tool.
    check("a program called compile is not the Go tool",
          t.matchPath("/usr/bin/compile") != CompilerKind::Go);
    check("a program called link is not the Go tool",
          t.matchPath("/opt/mytool/link") != CompilerKind::Go);
    check("a program called compile.exe is not the Go tool",
          t.matchPath("C:\\tools\\compile.exe") != CompilerKind::Go);
    check("the bare name compile is not the Go tool",
          t.matchPath("compile") != CompilerKind::Go);
    check("the bare name link is not the Go tool",
          t.matchPath("link") != CompilerKind::Go);

    // The directory shape has to be complete: "pkg/tool" and a
    // "<goos>_<goarch>" directory, and the segments have to be
    // consecutive.
    check("a toolchain directory without the underscore is not the Go tool",
          t.matchPath("/usr/local/go/pkg/tool/linux/compile") !=
              CompilerKind::Go);
    check("a missing pkg/tool part is not the Go tool",
          t.matchPath("/usr/local/go/bin/linux_amd64/compile") !=
              CompilerKind::Go);
    check("a broken segment sequence is not the Go tool",
          t.matchPath("/usr/local/go/pkg/tool/amd64/linux/compile") !=
              CompilerKind::Go);

    // The other tools of the toolchain are deliberately not recognized:
    // the assembler is not a compilation of the project's sources, and
    // the default filter is supposed to drop it.
    check("the assembler is not the Go compile tool",
          t.matchPath(std::string(kToolDir) + "/asm") != CompilerKind::Go);
    check("the cgo tool is not the Go compile tool",
          t.matchPath(std::string(kToolDir) + "/cgo") != CompilerKind::Go);

    // The name-based rules still work -- the path shape only adds a
    // way to recognize a compiler, it does not replace the others.
    check("gcc is still gcc",
          t.matchPath("/usr/bin/gcc") == CompilerKind::Gcc);
    check("cl is still msvc",
          t.matchPath("C:\\msys64\\mingw64\\bin\\cl.exe") ==
              CompilerKind::Msvc);

    // The kind names.
    check("the kind is named go",
          std::string(compilerKindName(CompilerKind::Go)) == "go");
    CompilerKind k = CompilerKind::Unknown;
    check("the name go maps back to the kind",
          compilerKindFromName("go", &k) && k == CompilerKind::Go);
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. Parsing -- the compile command
// ═══════════════════════════════════════════════════════════════════════════

void testCompileParsing() {
    std::printf("\n=== 2. The compile command ===\n");
    CommandParser parser;

    const std::string tool = std::string(kToolDir) + "/compile";
    const std::string importcfg = fixturePath();

    CompileCommand cmd = parser.parseArgv(
        {tool,
         "-o", "/tmp/go-build/b001/_pkg_.a",
         "-trimpath", "/tmp/go-build/b001=>",
         "-p", "github.com/example/project/internal/store",
         "-lang=go1.23",
         "-complete",
         "-importcfg", importcfg,
         "./store.go", "./store_test.go"},
        "/build");

    check("the command is a Go command", cmd.kind == CompilerKind::Go,
          compilerKindName(cmd.kind));
    check("the package import path is extracted",
          cmd.packageImportPath == "github.com/example/project/internal/store",
          cmd.packageImportPath);
    check("the language version is extracted",
          cmd.languageStandard == "go1.23", cmd.languageStandard);
    check("the import configuration path is extracted",
          cmd.importConfigPath == importcfg, cmd.importConfigPath);
    check("the output is extracted",
          cmd.outputFile == "/tmp/go-build/b001/_pkg_.a", cmd.outputFile);
    check("the sources are extracted",
          cmd.sourceFiles.size() == 2 &&
              cmd.sourceFiles[0] == "/build/store.go" &&
              cmd.sourceFiles[1] == "/build/store_test.go",
          join(cmd.sourceFiles, ","));
    check("a compile is not a link", !cmd.isLinkOnly);
    check("the flags that were not modelled are kept",
          contains(cmd.otherOptions, "-complete") &&
              contains(cmd.otherOptions, "-trimpath"),
          join(cmd.otherOptions, ","));

    // ── The other spelling of the value ────────────────────────────────
    //
    // Go's flag package accepts both "-lang=go1.23" and "-lang go1.23",
    // and both occur in real command lines.
    {
        CompileCommand c2 = parser.parseArgv(
            {tool, "-lang", "go1.22", "-p=github.com/x/y", "-importcfg=" +
             importcfg, "./a.go"},
            "/build");
        check("the separated form of -lang is recognized",
              c2.languageStandard == "go1.22", c2.languageStandard);
        check("the attached form of -p is recognized",
              c2.packageImportPath == "github.com/x/y",
              c2.packageImportPath);
        check("the attached form of -importcfg is recognized",
              c2.importConfigPath == importcfg, c2.importConfigPath);
    }

    // ── The flags that stop at the first positional argument ──────────
    //
    // That is the Go flag package's rule. A source file is never a
    // flag's value, and a flag after a source file is not a flag.
    {
        CompileCommand c3 = parser.parseArgv(
            {tool, "-p", "main", "./main.go", "-not-a-flag.go"},
            "/build");
        check("the sources after the first positional are still sources",
              c3.sourceFiles.size() == 2 &&
                  c3.sourceFiles[0] == "/build/main.go" &&
                  c3.sourceFiles[1] == "/build/-not-a-flag.go",
              join(c3.sourceFiles, ","));
    }

    // ── An unknown flag must not swallow a source file ────────────────
    //
    // The generic parser cannot know whether an unknown flag takes a
    // value. Guessing wrong would take a ".go" file with it, and the
    // package would lose a source.
    {
        CompileCommand c4 = parser.parseArgv(
            {tool, "-complete", "-goversion", "go1.23.0", "./a.go"},
            "/build");
        check("an unknown flag does not swallow a source file",
              c4.sourceFiles.size() == 1 &&
                  c4.sourceFiles[0] == "/build/a.go",
              join(c4.sourceFiles, ","));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. The import configuration
// ═══════════════════════════════════════════════════════════════════════════

void testImportConfig() {
    std::printf("\n=== 3. The import configuration ===\n");

    GoImportConfig cfg = readGoImportConfig(fixturePath(), "/build");

    check("the manifest is read", cfg.read, cfg.error);
    check("every package of the manifest is read",
          cfg.packageFiles.size() == 4,
          std::to_string(cfg.packageFiles.size()));

    const std::map<std::string, std::string>::const_iterator it =
        cfg.packageFiles.find("github.com/example/project/internal/store");
    check("the export data of a package is found",
          it != cfg.packageFiles.end() &&
              it->second ==
                  "/tmp/go-build314159/b001/_pkg_.a",
          it == cfg.packageFiles.end() ? "not found" : it->second);

    check("a standard library package is found",
          cfg.packageFiles.count("fmt") == 1 &&
              cfg.packageFiles.at("fmt") ==
                  "/usr/local/go/pkg/linux_amd64/fmt.a");

    // The directive that is not a "packagefile" one is kept, so that a
    // consumer can report what was in the manifest.
    check("the directive that is not a packagefile is kept",
          cfg.otherDirectives.size() == 1 &&
              cfg.otherDirectives[0].compare(0, 13, "packageshlib ") == 0,
          join(cfg.otherDirectives, ","));

    // ── A manifest that is gone ────────────────────────────────────────
    //
    // This is the normal case when a build log is parsed after the
    // build: the manifest lives in the build's temporary directory, and
    // the build deletes it. The path has to survive that.
    {
        GoImportConfig gone =
            readGoImportConfig("/tmp/go-build-gone/b001/importcfg", "/build");
        check("a manifest that cannot be read says so",
              !gone.read && !gone.error.empty(), gone.error);
        check("a manifest that cannot be read has no packages",
              gone.packageFiles.empty());
    }

    // ── The command carries the mapping ────────────────────────────────
    {
        CommandParser parser;
        CompileCommand cmd = parser.parseArgv(
            {std::string(kToolDir) + "/compile",
             "-p", "github.com/example/project/internal/store",
             "-importcfg", fixturePath(),
             "./store.go"},
            "/build");

        check("the command carries the package mapping",
              cmd.packageFiles.size() == 4,
              std::to_string(cmd.packageFiles.size()));
        check("the export data is in the system-include slot",
              cmd.systemDirs.size() == 4 &&
                  contains(cmd.systemDirs,
                           "/usr/local/go/pkg/linux_amd64/fmt.a"),
              join(cmd.systemDirs, ","));
        check("the import paths are in the include slot",
              cmd.includeDirs.size() == 4 &&
                  contains(cmd.includeDirs, "fmt"),
              join(cmd.includeDirs, ","));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. The link step
// ═══════════════════════════════════════════════════════════════════════════

void testLinkStep() {
    std::printf("\n=== 4. The link step ===\n");
    CommandParser parser;

    const std::string importcfg = fixturePath();
    CompileCommand link = parser.parseArgv(
        {std::string(kToolDir) + "/link",
         "-o", "/tmp/go-build/b001/exe/project",
         "-importcfg", importcfg,
         "-buildmode=exe",
         "-s",
         "-w",
         "/tmp/go-build/b001/_pkg_.a"},
        "/build");

    check("the link step is a Go command", link.kind == CompilerKind::Go,
          compilerKindName(link.kind));
    check("the link step has no sources", link.sourceFiles.empty());
    check("the link step is a link", link.isLinkOnly);
    check("the link step carries the import configuration",
          link.importConfigPath == importcfg, link.importConfigPath);
    check("the link step carries the output",
          link.outputFile == "/tmp/go-build/b001/exe/project",
          link.outputFile);
    check("the link step carries the package set",
          link.packageFiles.size() == 4,
          std::to_string(link.packageFiles.size()));

    // ── The filter ─────────────────────────────────────────────────────
    //
    // "link" is in the default exclusion list, because it is usually
    // the C/C++ linker. The Go link step is not that: its import
    // configuration is the authoritative statement of which packages
    // are in the binary, and it exists nowhere else.
    FilterRules rules(FilterConfig::builtin());

    check("the Go link step is kept by the default filter",
          rules.keep(link));
    check("the reason for keeping it is stated",
          rules.explain(link).find("link step") != std::string::npos,
          rules.explain(link));

    // The C/C++ linker is still dropped -- the change is about the Go
    // toolchain, not about linkers in general.
    {
        CompileCommand cLink = parser.parseArgv(
            {"link", "-o", "project.exe", "main.obj"}, "/build");
        check("an ordinary linker is still dropped",
              !rules.keep(cLink));
        check("the reason for dropping it is unchanged",
              rules.explain(cLink) == "dropped: excluded (link)",
              rules.explain(cLink));
    }

    // The Go compile step is kept as well.
    {
        CompileCommand cCompile = parser.parseArgv(
            {std::string(kToolDir) + "/compile", "-p", "main",
             "./main.go"},
            "/build");
        check("the Go compile step is kept", rules.keep(cCompile));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. The export
// ═══════════════════════════════════════════════════════════════════════════

void testExport() {
    std::printf("\n=== 5. The export ===\n");
    CommandParser parser;
    const std::string importcfg = fixturePath();

    CompileCommand compile = parser.parseArgv(
        {std::string(kToolDir) + "/compile",
         "-o", "/tmp/go-build/b001/_pkg_.a",
         "-p", "github.com/example/project/internal/store",
         "-lang=go1.23",
         "-importcfg", importcfg,
         "./store.go"},
        "/build");

    CompileCommand link = parser.parseArgv(
        {std::string(kToolDir) + "/link",
         "-o", "/tmp/go-build/b001/exe/project",
         "-importcfg", importcfg},
        "/build");

    // ── The analyzer's own vocabulary ──────────────────────────────────
    {
        AdapterRegistry registry;
        AdapterContext ctx;
        const std::string out =
            exportTranslatedCommands({compile, link}, registry, ctx);

        check("the translated output has a line per command",
              out.find('\n') != std::string::npos &&
                  std::count(out.begin(), out.end(), '\n') == 2,
              out);

        check("the export data is a system include",
              out.find("--sys_include /usr/local/go/pkg/linux_amd64/fmt.a") !=
                  std::string::npos,
              out);
        check("the import configuration path is kept",
              out.find("--importcfg " + importcfg) != std::string::npos,
              out);
        check("the package import path is kept",
              out.find("--import_path "
                       "github.com/example/project/internal/store") !=
                  std::string::npos,
              out);
        check("the language version is a define",
              out.find("-Dgo.lang.version=go1.23") != std::string::npos,
              out);
        check("the sources are the trailing arguments",
              out.find("/build/store.go") != std::string::npos, out);

        // The link step is in there too: its import configuration is
        // the authoritative package list of the binary.
        check("the link step is translated as well",
              out.find("/tmp/go-build/b001/exe/project") != std::string::npos,
              out);
    }

    // ── The compilation database keeps the argv ───────────────────────
    //
    // The clang format is a faithful record of the build: the original
    // arguments, untouched. That holds for the Go commands too.
    {
        const std::string db = exportClangCompilationDb({compile});

        check("the database keeps the original arguments",
              db.find("-lang=go1.23") != std::string::npos &&
                  db.find("\"-p\", \"github.com/example/project/internal/"
                          "store\"") != std::string::npos,
              db);
        check("the database keeps the original import configuration",
              db.find(importcfg) != std::string::npos, db);
        check("the database names the compilation unit",
              db.find("\"file\":") != std::string::npos &&
                  db.find("/build/store.go") != std::string::npos,
              db);
    }
}

}  // namespace

int main() {
    std::printf("=== ccap_compdb Go toolchain tests ===\n");

    testRecognition();
    testCompileParsing();
    testImportConfig();
    testLinkStep();
    testExport();

    std::printf("\n===================================\n");
    std::printf("PASS: %d   FAIL: %d\n", gPass, gFail);
    std::printf("===================================\n");
    return gFail == 0 ? 0 : 1;
}
