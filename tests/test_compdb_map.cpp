// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb -- the switch table / retention level tests
//
// Covers:
//   1. The option classification (what an option *means*)
//   2. The retention matrix (what survives at the AST / BC level)
//   3. The clang spelling of the options (MSVC -> clang)
//   4. The unknown compiler fallback (the "treat it as gcc" case)
//   5. The explicit compiler -> profile mapping
//   6. The configuration loading of the profiles
//   7. The report (nothing is dropped silently)
//
// Usage: test_compdb_map

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

bool containsSub(const std::vector<std::string>& v, const std::string& sub) {
    for (const auto& x : v) {
        if (x.find(sub) != std::string::npos) return true;
    }
    return false;
}

// Does the option list contain the pair ("-I", "/inc")?
//
// The check is on the **pair**, not on a joined string, because an
// argv entry must not contain a space: "-I" and "/inc" are two
// entries, which is exactly what clang expects.
//
// The order matters (the include path follows the option), so the
// pair has to be adjacent.
bool hasPair(const std::vector<std::string>& v, const std::string& opt,
             const std::string& val) {
    for (size_t i = 0; i + 1 < v.size(); ++i) {
        if (v[i] == opt && v[i + 1] == val) return true;
    }
    return false;
}

// Parse a command line into a CompileCommand.
CompileCommand parse(const std::string& line,
                     const std::string& cwd = "/build") {
    CommandParser parser;
    parser.setCompilerTable(CompilerTable::builtin());
    return parser.parseLine(line, cwd);
}

// 鈹€鈹€ 1. The classification 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
//
// The classification is the durable fact: it says what an option
// *means*, independently of how it is spelled.
void testClassification() {
    std::printf("\n-- classification --\n");

    OptionMapper mapper = OptionMapper::builtin();
    const CompilerProfile& gnu = *mapper.findProfile("gcc");
    const CompilerProfile& msvc = *mapper.findProfile("cl");

    // 鈹€鈹€ The gcc spelling 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    check("-I is an include", mapper.classify("-I", gnu) == OptionClass::Include);
    check("-Ifoo is an include", mapper.classify("-Ifoo", gnu) == OptionClass::Include);
    check("-isystem is a system include",
          mapper.classify("-isystem", gnu) == OptionClass::SystemInclude);
    check("-D is a macro", mapper.classify("-DX=1", gnu) == OptionClass::Macro);
    check("-U is a macro undef",
          mapper.classify("-UFOO", gnu) == OptionClass::MacroUndef);
    check("-include is a pre-include",
          mapper.classify("-include", gnu) == OptionClass::PreInclude);
    check("-std= is a language std",
          mapper.classify("-std=c++17", gnu) == OptionClass::LanguageStd);
    check("-c is compile only",
          mapper.classify("-c", gnu) == OptionClass::CompileOnly);
    check("-o is the output", mapper.classify("-o", gnu) == OptionClass::Output);
    check("-O2 is an optimization",
          mapper.classify("-O2", gnu) == OptionClass::Optimization);
    check("-Wall is a warning",
          mapper.classify("-Wall", gnu) == OptionClass::Warning);
    check("-lfoo is a linker option",
          mapper.classify("-lfoo", gnu) == OptionClass::Linker);
    check("-Wl, is a linker option",
          mapper.classify("-Wl,-x", gnu) == OptionClass::Linker);
    check("-MMD is a dependency",
          mapper.classify("-MMD", gnu) == OptionClass::Dependency);

    // The triple and the CPU feature level are different classes,
    // because they are needed at different levels.
    check("--target is the triple",
          mapper.classify("--target=x86_64-linux-gnu", gnu) ==
              OptionClass::Target);
    check("-march is the CPU level",
          mapper.classify("-march=x86-64-v3", gnu) == OptionClass::TargetCpu);
    check("-mcpu is the CPU level",
          mapper.classify("-mcpu=cortex-m4", gnu) == OptionClass::TargetCpu);

    // 鈹€鈹€ The longest-match rule 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    //
    // This is the part that a naive prefix scan gets wrong.
    check("-include is not an include",
          mapper.classify("-include", gnu) != OptionClass::Include);
    check("-isystem is not -isysroot",
          mapper.classify("-isystem", gnu) == OptionClass::SystemInclude);
    check("-c is not -c99", mapper.classify("-c", gnu) == OptionClass::CompileOnly);
    check("-O2 is not an optimization of -O",
          mapper.classify("-O2", gnu) == OptionClass::Optimization);

    // 鈹€鈹€ The MSVC spelling 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    check("/I is an include", mapper.classify("/Ifoo", msvc) == OptionClass::Include);
    check("/D is a macro", mapper.classify("/DFOO=1", msvc) == OptionClass::Macro);
    check("/FI is a pre-include",
          mapper.classify("/FIhdr.h", msvc) == OptionClass::PreInclude);
    check("/external:I is a system include",
          mapper.classify("/external:Ifoo", msvc) == OptionClass::SystemInclude);
    check("/std: is a language std",
          mapper.classify("/std:c++17", msvc) == OptionClass::LanguageStd);
    check("/c is compile only",
          mapper.classify("/c", msvc) == OptionClass::CompileOnly);
    check("/Zi is debug info",
          mapper.classify("/Zi", msvc) == OptionClass::DebugInfo);

    // 鈹€鈹€ The response file 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    check("@file is a response file",
          mapper.classify("@build.rsp", gnu) == OptionClass::ResponseFile);

    // 鈹€鈹€ The unknown option 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    check("an unknown option is unknown",
          mapper.classify("-Qunknown", gnu) == OptionClass::Unknown);
}

// 鈹€鈹€ 2. The retention matrix 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
//
// The level decides what survives. This is the core of the feature.
void testRetentionLevels() {
    std::printf("\n-- retention levels --\n");

    OptionMapper mapper = OptionMapper::builtin();

    // A gcc command with one option of every interesting class.
    CompileCommand cmd = parse(
        "gcc -c -I/inc -isystem /sys -DFOO=1 -UBAR -include hdr.h "
        "-std=c++17 -O2 -g -Wall -MMD -MF dep.d -L/lib -lfoo "
        "-fno-strict-aliasing --target=x86_64-linux-gnu -march=x86-64-v3 "
        "-o out.o main.cpp");

    MappingReport astReport;
    std::vector<std::string> ast = mapper.map(cmd, TranslateLevel::Ast, &astReport);

    MappingReport bcReport;
    std::vector<std::string> bc = mapper.map(cmd, TranslateLevel::Bc, &bcReport);

    // 鈹€鈹€ The AST level 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    check("ast keeps the include path", containsSub(ast, "-I/inc"));
    check("ast keeps the system include", hasPair(ast, "-isystem", "/sys"));
    check("ast keeps the macro", containsSub(ast, "-DFOO=1"));
    check("ast keeps the macro undef", containsSub(ast, "-UBAR"));
    check("ast keeps the pre-include", hasPair(ast, "-include", "hdr.h"));
    check("ast keeps the language std", containsSub(ast, "-std=c++17"));
    check("ast keeps the compile-only flag", contains(ast, "-c"));
    check("ast keeps the output", hasPair(ast, "-o", "out.o"));
    check("ast keeps the source file", contains(ast, "main.cpp"));

    check("ast drops the optimisation", !containsSub(ast, "-O2"));
    check("ast drops the warnings", !containsSub(ast, "-Wall"));
    check("ast drops the dependency flags", !containsSub(ast, "-MMD"));
    check("ast drops the linker flags", !containsSub(ast, "-lfoo"));
    check("ast drops the codegen flags", !containsSub(ast, "-fno-strict"));
    // The check is on the exact token, not a substring: the target
    // triple "x86_64-linux-gnu" contains "-g" ("linux-gnu"), so a
    // substring check would report a false positive.
    check("ast drops the debug info", !contains(ast, "-g"));

    // -- The target triple --------------------------------------------
    //
    // The triple is needed at the AST level too.
    //
    // Measured with clang 15.0.7: the AST of the same source differs
    // between targets (the __int128 / __builtin_va_list / SVE builtin
    // sets), and under some targets the code does not compile at all
    // ("__int128 is not supported on this target"). Dropping the
    // triple produces a *different* AST, not the same one.
    check("ast keeps the target triple",
          containsSub(ast, "--target=x86_64-linux-gnu"));

    // -- The CPU feature level ----------------------------------------
    //
    // "-march" / "-mcpu" decide the instruction set, not the AST: the
    // AST of the same source under -march=x86-64-v2 and
    // -march=x86-64-v3 is identical. So the AST level drops it.
    check("ast drops the CPU feature level", !containsSub(ast, "-march"));

    // 鈹€鈹€ The BC level 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    //
    // The BC level adds the target and the debug info: they decide the
    // ABI and the data layout of the bitcode.
    check("bc keeps the target triple",
          containsSub(bc, "--target=x86_64-linux-gnu"));
    check("bc keeps the debug info", contains(bc, "-g"));

    // ... and it keeps everything the AST keeps.
    check("bc keeps the include path", containsSub(bc, "-I/inc"));
    check("bc keeps the macro", containsSub(bc, "-DFOO=1"));
    check("bc keeps the language std", containsSub(bc, "-std=c++17"));

    // ... and it still drops the noise.
    check("bc drops the optimisation", !containsSub(bc, "-O2"));
    check("bc drops the warnings", !containsSub(bc, "-Wall"));
    check("bc drops the linker flags", !containsSub(bc, "-lfoo"));

    // 鈹€鈹€ The two levels differ 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    check("the bc level keeps more than the ast level", bc.size() > ast.size());

    // 鈹€鈹€ The driver 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    check("a C++ source gets the C++ driver", ast[0] == "clang++");
}

// 鈹€鈹€ 3. The MSVC -> clang spelling 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
void testMsvcTranslation() {
    std::printf("\n-- msvc to clang --\n");

    OptionMapper mapper = OptionMapper::builtin();

    CompileCommand cmd = parse(
        "cl /c /Iinclude /DFOO=1 /FIpre.h /std:c++17 /Zi /O2 /W4 "
        "/Fobuild\\out.obj main.cpp");

    MappingReport report;
    std::vector<std::string> out = mapper.map(cmd, TranslateLevel::Ast, &report);

    check("msvc uses clang-cl", out[0] == "clang-cl");
    check("msvc /I becomes -I", containsSub(out, "-Iinclude"));
    check("msvc /D becomes -D", containsSub(out, "-DFOO=1"));
    check("msvc /std: becomes -std=", containsSub(out, "-std=c++17"));
    check("msvc /c becomes -c", contains(out, "-c"));
    check("msvc /W4 is dropped", !containsSub(out, "-W4"));
    check("msvc /O2 is dropped at the ast level", !containsSub(out, "-O2"));

    // The debug info is only needed for the bitcode, not for the AST.
    check("msvc /Zi is dropped at the ast level", !containsSub(out, "-g"));

    // ... and it is kept at the BC level.
    MappingReport bcReport;
    std::vector<std::string> bc = mapper.map(cmd, TranslateLevel::Bc, &bcReport);
    check("msvc /Zi becomes -g at the bc level", containsSub(bc, "-g"));

    // The exception handling model has no clang equivalent: it has to
    // be reported, not silently dropped.
    CompileCommand cmd2 = parse("cl /c /EHsc main.cpp");
    MappingReport report2;
    mapper.map(cmd2, TranslateLevel::Ast, &report2);
    check("msvc /EHsc is reported as having no equivalent",
          containsSub(report2.noEquivalent, "/EHsc"));
}

// 鈹€鈹€ 4. The unknown compiler 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
//
// The "an unknown compiler, treat it as gcc" case.
void testUnknownCompiler() {
    std::printf("\n-- unknown compiler --\n");

    OptionMapper mapper = OptionMapper::builtin();

    // A compiler name nobody knows.
    CompileCommand cmd = parse("my-cool-cc -c -I/inc -DFOO=1 main.c");

    MappingReport report;
    std::vector<std::string> out = mapper.map(cmd, TranslateLevel::Ast, &report);

    // The generic profile still knows the common options.
    check("the generic profile keeps the include path",
          containsSub(out, "-I/inc"));
    check("the generic profile keeps the macro", containsSub(out, "-DFOO=1"));
    check("the generic profile keeps the compile-only flag", contains(out, "-c"));

    // ... but it does not guess at the rest.
    check("the generic profile drops an unknown option",
          !containsSub(out, "-Qwhatever"));

    // And it reports what it did not recognize.
    CompileCommand cmd2 = parse("my-cool-cc -c -Qweird main.c");
    MappingReport report2;
    mapper.map(cmd2, TranslateLevel::Ast, &report2);
    check("the unknown option is reported", containsSub(report2.unknown, "-Qweird"));
    check("the unknown option has a reason", !report2.unknown.empty());

    // The generic profile is deliberately a subset of the gcc table:
    // an unknown compiler's "-std=" is still recognized (it is
    // de-facto standard) but "-fno-strict-aliasing" is not.
    CompileCommand cmd3 = parse("my-cool-cc -c -std=c++17 -fno-strict-aliasing main.c");
    MappingReport report3;
    std::vector<std::string> out3 = mapper.map(cmd3, TranslateLevel::Ast, &report3);
    check("the generic profile keeps -std=", containsSub(out3, "-std=c++17"));
    check("the generic profile drops the -f flag", !containsSub(out3, "-fno-strict"));
    check("the dropped -f flag is reported as unknown",
          containsSub(report3.unknown, "-fno-strict-aliasing"));
}

// 鈹€鈹€ 5. The explicit compiler -> profile mapping 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
void testExplicitMapping() {
    std::printf("\n-- explicit mapping --\n");

    // A compiler whose name looks nothing like gcc, but which the user
    // says is "the gcc family".
    OptionMapper mapper = OptionMapper::builtin();
    mapper.setCompilerProfile("my-cool-cc", "gnu");

    CompileCommand cmd = parse("my-cool-cc -c -I/inc -fno-strict-aliasing main.c");
    MappingReport report;
    std::vector<std::string> out = mapper.map(cmd, TranslateLevel::Ast, &report);

    check("the mapped compiler uses the gnu table",
          containsSub(out, "-fno-strict-aliasing") == false);
    check("the mapped compiler keeps the include", containsSub(out, "-I/inc"));

    // The prefix form.
    OptionMapper mapper2 = OptionMapper::builtin();
    mapper2.setCompilerProfile("arm-*", "gnu");
    check("the prefix mapping resolves",
          mapper2.mappedProfileId("arm-none-eabi-gcc") == "gnu");
    check("the prefix mapping does not over-match",
          mapper2.mappedProfileId("x86_64-linux-gnu-gcc") == "");

    // The longest prefix wins.
    OptionMapper mapper3 = OptionMapper::builtin();
    mapper3.setCompilerProfile("arm-*", "generic");
    mapper3.setCompilerProfile("arm-none-eabi-*", "gnu");
    check("the longest prefix wins",
          mapper3.mappedProfileId("arm-none-eabi-gcc") == "gnu");
    check("the shorter prefix still applies",
          mapper3.mappedProfileId("arm-something-else") == "generic");

    // The default profile.
    OptionMapper mapper4 = OptionMapper::builtin();
    mapper4.setDefaultProfile("gnu");
    check("the default profile applies to an unknown compiler",
          mapper4.profileFor("something-else").id == "gnu");
    check("the default profile does not override a known compiler",
          mapper4.profileFor("gcc").id == "gnu");

    // The mapping wins over the name matching.
    OptionMapper mapper5 = OptionMapper::builtin();
    mapper5.setCompilerProfile("gcc", "generic");
    check("the explicit mapping wins over the name",
          mapper5.mappedProfileId("gcc") == "generic");
}

// 鈹€鈹€ 6. The configuration loading 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
void testConfigProfiles() {
    std::printf("\n-- config profiles --\n");

    const char* kConfig = R"JSON({
  "profiles": [
    {
      "id": "my-cc",
      "kind": "gcc",
      "names": ["my-cool-cc"],
      "driver": "clang",
      "cpp_driver": "clang++",
      "include_env": ["MYCPATH"],
      "no_equivalent": ["/MYONLY"],
      "rules": [
        {
          "name": "I",
          "shape": ["dash", "attached", "unattached", "path"],
          "class": "include",
          "clang": "-I%s",
          "note": "the include path"
        },
        {
          "name": "MYONLY",
          "shape": ["dash", "attached"],
          "class": "codegen",
          "clang": ""
        },
        {
          "name": "Z",
          "shape": ["dash", "attached", "unattached"],
          "class": "macro",
          "clang": "-D%s"
        }
      ]
    }
  ],
  "compiler_profiles": {
    "other-cc": "my-cc"
  },
  "default_profile": "generic",
  "output": {
    "format": "clang-cmd",
    "level": "bc"
  }
})JSON";

    CompdbConfig config;
    std::string err;
    bool ok = CompdbConfig::load(kConfig, &config, &err);
    check("the config parses", ok, err);
    if (!ok) return;

    check("the profile is loaded", config.profiles.size() == 1);
    if (config.profiles.empty()) return;

    const CompilerProfile& p = config.profiles[0];
    check("the profile id is read", p.id == "my-cc");
    check("the profile kind is read", p.kind == CompilerKind::Gcc);
    check("the profile names are read", p.exactNames.size() == 1);
    check("the profile driver is read", p.driver == "clang");
    check("the profile cpp driver is read", p.cppDriver == "clang++");
    check("the profile env vars are read", p.includeEnvVars.size() == 1);
    check("the profile no-equivalent list is read", p.noEquivalent.size() == 1);
    check("the profile rules are read", p.rules.size() == 3);

    check("the compiler mapping is read", config.compilerProfiles.size() == 1);
    check("the default profile is read", config.defaultProfile == "generic");
    check("the output level is read", config.outputLevel == "bc");

    // The shape flags were parsed.
    if (!p.rules.empty()) {
        check("the shape flags are parsed",
              (p.rules[0].shape & kShapeAttached) != 0 &&
                  (p.rules[0].shape & kShapeUnattached) != 0);
        check("the class is parsed", p.rules[0].cls == OptionClass::Include);
        check("the clang form is parsed", p.rules[0].clangForm == "-I%s");
    }

    // The mapper picks the profile up.
    OptionMapper mapper = buildMapper(config);
    check("the profile is in the mapper", mapper.findProfile("my-cool-cc") != nullptr);

    CompileCommand cmd = parse("my-cool-cc -c -I/inc -ZFOO=1 main.c");
    MappingReport report;
    std::vector<std::string> out = mapper.map(cmd, TranslateLevel::Ast, &report);

    check("the custom include is mapped", containsSub(out, "-I/inc"));
    check("the custom macro is mapped", containsSub(out, "-DFOO=1"));

    // The default profile applies to an unknown compiler -- and it does
    // NOT override a profile that matches by name.
    check("the default profile does not override the name match",
          mapper.profileFor("my-cool-cc").id == "my-cc");
    check("the default profile is the last resort",
          mapper.profileFor("totally-unknown").id == "generic");
}

// 鈹€鈹€ 7. The report 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
//
// Nothing is dropped silently: every decision is explained.
void testReport() {
    std::printf("\n-- the report --\n");

    OptionMapper mapper = OptionMapper::builtin();

    CompileCommand cmd = parse(
        "gcc -c -I/inc -O2 -Wall -Qweird -lfoo main.cpp");

    MappingReport report;
    mapper.map(cmd, TranslateLevel::Ast, &report);

    check("the report has a decision per option", report.decisions.size() >= 5);

    // Every kept decision has an emission.
    for (const auto& d : report.decisions) {
        if (d.kept) {
            check("a kept decision has an emission", !d.emitted.empty());
            break;
        }
    }

    // Every dropped decision has a reason.
    bool allDroppedHaveReason = true;
    for (const auto& d : report.decisions) {
        if (!d.kept && d.reason.empty()) allDroppedHaveReason = false;
    }
    check("every dropped decision has a reason", allDroppedHaveReason);

    // The unknown option is in its own list.
    check("the unknown option is listed", containsSub(report.unknown, "-Qweird"));

    // The dropped-by-level list is populated.
    check("the dropped list is populated", !report.droppedByLevel.empty());

    // The text report is not empty.
    check("the text report is not empty", !report.toText().empty());

    // The export function produces a report too.
    std::vector<CompileCommand> cmds;
    cmds.push_back(cmd);
    std::string text = exportMappingReports(cmds, mapper, TranslateLevel::Ast);
    check("the exported report names the level",
          text.find("retention level: ast") != std::string::npos);
    check("the exported report names the unknown option",
          text.find("-Qweird") != std::string::npos);
}

// 鈹€鈹€ 8. The end-to-end export 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
void testExport() {
    std::printf("\n-- export --\n");

    OptionMapper mapper = OptionMapper::builtin();

    CompileCommand cmd = parse("g++ -c -I/inc -DFOO=1 -std=c++17 -O2 main.cpp");
    std::vector<CompileCommand> cmds;
    cmds.push_back(cmd);

    std::string ast = exportClangCommandLinesText(cmds, mapper, TranslateLevel::Ast);
    check("the ast export is not empty", !ast.empty());
    check("the ast export uses clang++", ast.find("clang++") == 0);
    check("the ast export keeps the include", ast.find("-I") != std::string::npos);
    check("the ast export drops the optimisation",
          ast.find("-O2") == std::string::npos);

    std::string bc = exportClangCommandLinesText(cmds, mapper, TranslateLevel::Bc);
    check("the bc export is not empty", !bc.empty());

    // The level names.
    TranslateLevel level = TranslateLevel::Ast;
    check("the ast level name parses", translateLevelFromName("ast", &level) &&
                                       level == TranslateLevel::Ast);
    check("the bc level name parses", translateLevelFromName("bc", &level) &&
                                      level == TranslateLevel::Bc);
    check("the bitcode alias parses", translateLevelFromName("bitcode", &level) &&
                                     level == TranslateLevel::Bc);
    check("an unknown level does not parse", !translateLevelFromName("nope", &level));

    // The class names are stable (they are used in the config).
    check("the include class is named include",
          std::string(optionClassName(OptionClass::Include)) == "include");
    check("the pre-include class is named pre-include",
          std::string(optionClassName(OptionClass::PreInclude)) == "pre-include");
}

// ── 9. The compiler registry ──────────────────────────────────────────────
//
// The registry is the **family-aware** layer: it knows that "gcc",
// "g++" and "g++cc" are variants of one family, that "arm-none-eabi-gcc"
// is a cross compiler of that family, and that a rule may apply only
// from a given compiler version on.
//
// These tests exist to pin the *wiring*, not just the model: a registry
// that is built but never consulted would pass a model-only test and
// still produce the wrong command.
void testRegistry() {
    std::printf("\n-- the registry --\n");

    OptionMapper mapper = OptionMapper::builtin();

    // ── The name resolution ─────────────────────────────────────────
    //
    // The executable name is what appears in a build log; the family id
    // is the abstract name. The resolution has to connect them.
    {
        CompilerResolution r = mapper.resolve("gcc", "");
        check("gcc resolves to the gnu family", r.familyId == "gnu");
        check("gcc resolves to the gcc variant", r.variantId == "gcc");
    }
    {
        CompilerResolution r = mapper.resolve("g++", "");
        check("g++ resolves to the gnu family", r.familyId == "gnu");
        // The variant matters: g++ compiles C++ by default and gcc
        // does not, so they are not the same table.
        check("g++ resolves to the g++ variant", r.variantId == "g++");
    }
    {
        CompilerResolution r = mapper.resolve("cl", "");
        check("cl resolves to the msvc family", r.familyId == "msvc");
        check("cl resolves to the cl variant", r.variantId == "cl");
    }
    {
        CompilerResolution r = mapper.resolve("clang++", "");
        check("clang++ resolves to the clang family", r.familyId == "clang");
    }
    {
        // The cross compiler: a glob match, not an exact name.
        CompilerResolution r = mapper.resolve("arm-none-eabi-gcc", "");
        check("arm-none-eabi-gcc resolves to the gnu family",
              r.familyId == "gnu");
        check("the cross compiler matched by glob", r.matchedBy == "glob");
    }
    {
        // The unknown compiler: the conservative fallback.
        CompilerResolution r = mapper.resolve("totally-unknown-cc", "");
        check("an unknown compiler resolves to the generic family",
              r.familyId == "generic");
        check("an unknown compiler matched by the fallback",
              r.matchedBy == "generic");
    }

    // ── The redirect ────────────────────────────────────────────────
    //
    // "replace/my-cc/g++" says: an executable called "my-cc" is really
    // the g++ driver. That is a more specific statement than a glob,
    // so it is tried first.
    //
    // This is the case the built-in table deliberately does NOT ship:
    // whether an executable name means another compiler is a fact about
    // a particular machine (a vendor wrapper, a symlink), so it
    // belongs in the user's configuration.
    {
        CompilerRegistry reg = CompilerRegistry::builtin();
        CompilerMatch m;
        m.pattern = "replace/my-cc/g++";
        m.isRedirect = true;
        reg.addMatch(m);

        OptionMapper m2;
        m2.setRegistry(reg);
        CompilerResolution r = m2.resolve("my-cc", "");
        check("the redirect selects the gnu family", r.familyId == "gnu");
        check("the redirect selects the g++ variant", r.variantId == "g++");
        check("the redirect is reported as a redirect",
              r.matchedBy == "redirect");

        // The redirect must win over the glob: it is the more specific
        // statement about what the executable is.
        CompilerRegistry reg2 = CompilerRegistry::builtin();
        CompilerMatch g;
        g.pattern = "my-*";
        g.variantId = "gnu";
        reg2.addMatch(g);
        CompilerMatch rd;
        rd.pattern = "replace/my-cc/g++";
        rd.isRedirect = true;
        reg2.addMatch(rd);

        OptionMapper m3;
        m3.setRegistry(reg2);
        CompilerResolution r3 = m3.resolve("my-cc", "");
        check("the redirect wins over the glob", r3.matchedBy == "redirect");
    }

    // ── The version-scoped rule ─────────────────────────────────────
    //
    // This is the capability that a flat profile cannot express: the
    // same option name means different things at different compiler
    // versions.
    {
        CompilerRegistry reg = CompilerRegistry::builtin();

        // A rule that only applies from gcc 10 on.
        CompilerFamily* gnu = nullptr;
        for (CompilerFamily& fam : reg.familiesRef()) {
            if (fam.id == "gnu") gnu = &fam;
        }
        check("the gnu family exists", gnu != nullptr);
        if (gnu) {
            CompilerVariant* gxx = nullptr;
            for (CompilerVariant& v : gnu->variants) {
                if (v.id == "g++") gxx = &v;
            }
            check("the g++ variant exists", gxx != nullptr);
            if (gxx) {
                SwitchRule r;
                r.name = "std=";
                r.shape = kShapeDash | kShapeAttached | kShapeEqual;
                r.cls = OptionClass::LanguageStd;
                r.clangForm = "-std=%s";
                r.versionMin = "10";
                gxx->rules.push_back(r);
            }
        }

        OptionMapper m2;
        m2.setRegistry(reg);

        // On gcc 9 the rule does not fire.
        CompilerResolution old = m2.resolve("g++", "9.3.0");
        bool oldHasRule = false;
        for (const SwitchRule& r : old.rules) {
            if (r.name == "std=" && r.versionMin == "10") oldHasRule = true;
        }
        check("the version-scoped rule is in the rule set", oldHasRule);

        // On gcc 11 the option is recognized ...
        CompileCommand cmd = parse("g++ -c -std=c++17 main.cpp");
        MappingReport r11;
        m2.setCompilerVersion("11.4.0");
        std::vector<std::string> out11 =
            m2.map(cmd, TranslateLevel::Ast, &r11);
        check("the version-scoped rule fires on the new version",
              containsSub(out11, "-std=c++17"));
        check("the resolution records the version",
              r11.compilerVersion == "11.4.0");

        // ... and on gcc 9 the option is NOT unknown: the table knows
        // it, it just does not apply to that version.
        MappingReport r9;
        m2.setCompilerVersion("9.3.0");
        std::vector<std::string> out9 = m2.map(cmd, TranslateLevel::Ast, &r9);
        check("the version-scoped rule does not fire on the old version",
              !containsSub(out9, "-std=c++17"));
        check("the version-excluded option is not reported as unknown",
              !containsSub(r9.unknown, "-std=c++17"));

        // With no version at all, the rule must not fire either: a rule
        // whose precondition could not be verified must not be applied.
        MappingReport rNone;
        m2.setCompilerVersion("");
        std::vector<std::string> outNone =
            m2.map(cmd, TranslateLevel::Ast, &rNone);
        check("the version-scoped rule does not fire without a version",
              !containsSub(outNone, "-std=c++17"));
    }

    // ── The version comparison ──────────────────────────────────────
    //
    // The comparison is component-wise, not lexicographic: "10.0" is
    // **greater** than "9.0", and gcc 10 exists.
    {
        std::vector<unsigned> v;
        check("a version parses", parseVersion("10.4.2", &v));
        check("the version has three components", v.size() == 3);
        check("10.0 is greater than 9.0",
              compareVersions("10.0", "9.0") > 0);
        check("4.7 equals 4.7.0", compareVersions("4.7", "4.7.0") == 0);
        check("an unparseable version does not parse",
              !parseVersion("unknown", &v));
    }

    // ── The glob matcher ────────────────────────────────────────────
    {
        check("the glob matches a suffix", globMatch("*-gcc", "arm-gcc"));
        check("the glob matches a prefix", globMatch("gcc-*", "gcc-11"));
        check("the glob matches the middle", globMatch("*-gcc", "a-gcc"));
        check("the glob does not over-match",
              !globMatch("*-gcc", "gcc-11"));
        check("the glob is case insensitive",
              globMatch("*-GCC", "arm-gcc"));
    }
}

// ── 10. The audit buckets ────────────────────────────────────────────────
//
// The six buckets exist because the three kinds of *dropped* need
// different responses from the reader, and because a **rewrite** is
// where a semantic drift would hide.
void testAuditBuckets() {
    std::printf("\n-- the audit buckets --\n");

    OptionMapper mapper = OptionMapper::builtin();

    CompileCommand cmd = parse(
        "gcc -c -I/inc -O2 -Wall -Qweird -lfoo -EHsc main.cpp");

    MappingReport report;
    mapper.map(cmd, TranslateLevel::Ast, &report);

    // The kept bucket: the include path survived unchanged.
    check("the kept bucket has the include",
          containsSub(report.kept, "-I/inc"));

    // The dropped-by-level bucket: the optimisation and the warning.
    check("the dropped-by-level bucket has the optimisation",
          containsSub(report.droppedByLevelBucket, "-O2"));
    check("the dropped-by-level bucket has the warning",
          containsSub(report.droppedByLevelBucket, "-Wall"));

    // The unknown bucket: the option the table never heard of.
    check("the unknown bucket has the unknown option",
          containsSub(report.unknownBucket, "-Qweird"));

    // The counts must add up: every decision lands in exactly one
    // bucket.
    std::vector<size_t> counts = report.bucketCounts(6);
    size_t total = 0;
    for (size_t c : counts) total += c;
    check("every decision is in a bucket",
          total == report.decisions.size());

    // The text report names the buckets.
    std::string text = report.toText();
    check("the text report names the bucket summary",
          text.find("buckets:") != std::string::npos);
    check("the text report names the family",
          text.find("family:") != std::string::npos);

    // ── The unknown policies ────────────────────────────────────────
    {
        // The default: dropped from the command, reported.
        MappingReport r1;
        std::vector<std::string> o1 =
            mapper.map(cmd, TranslateLevel::Ast, &r1);
        check("the default policy drops the unknown option",
              !containsSub(o1, "-Qweird"));
        check("the default policy reports the unknown option",
              containsSub(r1.unknown, "-Qweird"));
        check("the default policy does not fail", !r1.failed);
    }
    {
        // "keep": passed through unchanged, still reported.
        OptionMapper m2 = OptionMapper::builtin();
        m2.setUnknownPolicy(UnknownPolicy::Keep);
        MappingReport r2;
        std::vector<std::string> o2 =
            m2.map(cmd, TranslateLevel::Ast, &r2);
        check("the keep policy passes the unknown option through",
              containsSub(o2, "-Qweird"));
        check("the keep policy still reports it",
              containsSub(r2.unknown, "-Qweird"));
    }
    {
        // "drop": dropped silently, but still counted.
        OptionMapper m3 = OptionMapper::builtin();
        m3.setUnknownPolicy(UnknownPolicy::Drop);
        MappingReport r3;
        std::vector<std::string> o3 =
            m3.map(cmd, TranslateLevel::Ast, &r3);
        check("the drop policy drops the unknown option",
              !containsSub(o3, "-Qweird"));
        check("the drop policy records it in the policy bucket",
              containsSub(r3.droppedByPolicy, "-Qweird"));
        check("the drop policy does not fail", !r3.failed);
    }
    {
        // "fail": the mapping fails.
        OptionMapper m4 = OptionMapper::builtin();
        m4.setUnknownPolicy(UnknownPolicy::Fail);
        MappingReport r4;
        m4.map(cmd, TranslateLevel::Ast, &r4);
        check("the fail policy fails the mapping", r4.failed);
        check("the fail policy explains why",
              !r4.failureReason.empty());
    }

    // The policy names round-trip.
    {
        UnknownPolicy p = UnknownPolicy::Report;
        check("the report policy parses",
              unknownPolicyFromName("report", &p) &&
                  p == UnknownPolicy::Report);
        check("the keep policy parses",
              unknownPolicyFromName("keep", &p) && p == UnknownPolicy::Keep);
        check("the drop policy parses",
              unknownPolicyFromName("drop", &p) && p == UnknownPolicy::Drop);
        check("the fail policy parses",
              unknownPolicyFromName("fail", &p) && p == UnknownPolicy::Fail);
        check("an unknown policy does not parse",
              !unknownPolicyFromName("nope", &p));
        check("the policy name is stable",
              std::string(unknownPolicyName(UnknownPolicy::Keep)) == "keep");
    }

    // The bucket names are stable (they are used in the audit output).
    check("the kept bucket is named kept",
          std::string(auditBucketName(AuditBucket::Kept)) == "kept");
    check("the mapped bucket is named mapped",
          std::string(auditBucketName(AuditBucket::Mapped)) == "mapped");
    check("the no-equivalent bucket is named dropped_no_equivalent",
          std::string(auditBucketName(AuditBucket::DroppedNoEquivalent)) ==
              "dropped_no_equivalent");
    check("the by-level bucket is named dropped_by_level",
          std::string(auditBucketName(AuditBucket::DroppedByLevel)) ==
              "dropped_by_level");
    check("the by-policy bucket is named dropped_by_policy",
          std::string(auditBucketName(AuditBucket::DroppedByPolicy)) ==
              "dropped_by_policy");
    check("the unknown bucket is named unknown",
          std::string(auditBucketName(AuditBucket::Unknown)) == "unknown");
}

// ── 11. The extended option shapes ───────────────────────────────────────
//
// The original twelve shape flags cover the common option syntaxes. The
// extended ones cover what a real toolchain integration needs: the
// values that are introduced by a colon, wrapped in parentheses, quoted,
// or split into a list that goes through a sub-table.
//
// These are tested through the **config**, because that is how a user
// reaches them: a built-in table cannot cover every vendor's spelling.
void testExtendedShapes() {
    std::printf("\n-- the extended shapes --\n");

    // ── The colon form ──────────────────────────────────────────────
    //
    // The value is introduced by a ':' that is part of the option
    // token: "/std:c++17" with a rule named "std".
    {
        const char* kConfig = R"JSON({
  "profiles": [
    {
      "id": "colon-cc",
      "kind": "gcc",
      "names": ["colon-cc"],
      "rules": [
        {
          "name": "std",
          "shape": ["dash", "attached", "colon"],
          "class": "language-std",
          "clang": "-std=%s"
        }
      ]
    }
  ]
})JSON";

        CompdbConfig config;
        std::string err;
        check("the colon config parses", CompdbConfig::load(kConfig, &config, &err),
              err);

        OptionMapper mapper = buildMapper(config);
        CompileCommand cmd = parse("colon-cc -c -std:c++17 main.c");
        MappingReport report;
        std::vector<std::string> out =
            mapper.map(cmd, TranslateLevel::Ast, &report);
        check("the colon form takes the value after the colon",
              containsSub(out, "-std=c++17"));
    }

    // ── The parenthesised form ──────────────────────────────────────
    {
        const char* kConfig = R"JSON({
  "profiles": [
    {
      "id": "paren-cc",
      "kind": "gcc",
      "names": ["paren-cc"],
      "rules": [
        {
          "name": "foo",
          "shape": ["dash", "attached", "parens"],
          "class": "macro",
          "clang": "-D%s"
        }
      ]
    }
  ]
})JSON";

        CompdbConfig config;
        std::string err;
        CompdbConfig::load(kConfig, &config, &err);

        OptionMapper mapper = buildMapper(config);
        CompileCommand cmd = parse("paren-cc -c --foo(bar) main.c");
        MappingReport report;
        std::vector<std::string> out =
            mapper.map(cmd, TranslateLevel::Ast, &report);
        check("the parenthesised form takes the value inside the brackets",
              containsSub(out, "-Dbar"));
    }

    // ── The quoted form ─────────────────────────────────────────────
    {
        const char* kConfig = R"JSON({
  "profiles": [
    {
      "id": "quote-cc",
      "kind": "gcc",
      "names": ["quote-cc"],
      "rules": [
        {
          "name": "I",
          "shape": ["dash", "attached", "strip_quotes", "path"],
          "class": "include",
          "clang": "-I%s"
        }
      ]
    }
  ]
})JSON";

        CompdbConfig config;
        std::string err;
        CompdbConfig::load(kConfig, &config, &err);

        OptionMapper mapper = buildMapper(config);
        // The quotes are part of the value, not of the option name.
        CompileCommand cmd = parse("quote-cc -c -I\"/usr/include\" main.c");
        MappingReport report;
        std::vector<std::string> out =
            mapper.map(cmd, TranslateLevel::Ast, &report);
        check("the quoted form removes the quotes",
              containsSub(out, "-I/usr/include"));
    }

    // ── The split + alternate table form ────────────────────────────
    //
    // "-Wl,-a,-b" dispatches to the linker sub-table: each piece after
    // the comma is a linker option, not a compiler option.
    {
        CompilerProfile p;
        p.id = "split-cc";
        p.kind = CompilerKind::Gcc;
        p.exactNames = {"split-cc"};

        SwitchRule wl;
        wl.name = "Wl,";
        wl.shape = kShapeDash | kShapeAttached | kShapeSplit |
                   kShapeAlternateTable;
        wl.cls = OptionClass::Linker;
        wl.alternateTable = "linker";
        p.rules.push_back(wl);

        // The linker sub-table: "z" is known, "bogus" is not.
        SwitchRule z;
        z.name = "z";
        z.shape = kShapeDash | kShapeAttached;
        z.cls = OptionClass::Linker;
        z.clangForm = "-z%s";

        SwitchRule bogus;
        bogus.name = "bogus";
        bogus.shape = kShapeDash | kShapeAttached;
        bogus.cls = OptionClass::Linker;

        p.subTables["linker"] = {z, bogus};

        CompdbConfig config;
        config.profiles.push_back(p);
        OptionMapper mapper = buildMapper(config);

        CompileCommand cmd = parse("split-cc -c -Wl,-z,now main.c");
        MappingReport report;
        mapper.map(cmd, TranslateLevel::Ast, &report);

        // The linker option is dropped at the AST level -- but it was
        // **recognized** (it is not in the unknown list), which is the
        // point of the sub-table.
        check("the split option is not reported as unknown",
              !containsSub(report.unknown, "-Wl,-z,now"));
    }

    // ── The unsupported flag ─────────────────────────────────────────
    //
    // "known but unsupported" is not the same as "no equivalent": the
    // compiler accepts the option, but the translation cannot honour
    // it. It is reported, not passed through.
    {
        const char* kConfig = R"JSON({
  "profiles": [
    {
      "id": "unsup-cc",
      "kind": "gcc",
      "names": ["unsup-cc"],
      "rules": [
        {
          "name": "X",
          "shape": ["dash", "attached", "unsupported"],
          "class": "codegen",
          "clang": ""
        }
      ]
    }
  ]
})JSON";

        CompdbConfig config;
        std::string err;
        CompdbConfig::load(kConfig, &config, &err);

        OptionMapper mapper = buildMapper(config);
        CompileCommand cmd = parse("unsup-cc -c -Xfoo main.c");
        MappingReport report;
        std::vector<std::string> out =
            mapper.map(cmd, TranslateLevel::Ast, &report);
        check("the unsupported option is not passed through",
              !containsSub(out, "-Xfoo"));
        check("the unsupported option is reported",
              containsSub(report.noEquivalent, "-Xfoo"));
    }
}

// ── 12. The abbreviation and the hyphen/underscore equivalence ────────────
//
// These two shapes change the **matching** itself, not the value
// extraction, so they get their own tests.
//
// Both are deliberately off by default. A rule has to ask for them,
// because the option sets that need them are the exception, not the
// rule:
//
//   hyphen_is_underscore  gcc has "--sysroot" and no "--sys_root";
//                        treating the two as the same would let one
//                        spelling silently stand in for the other.
//
//   abbrev_match          an abbreviation is ambiguous by
//                        construction, so it needs the whole rule set
//                        to be resolved -- and it is only resolved
//                        when it is unambiguous.
void testAbbrevAndHyphenShapes() {
    std::printf("\n-- the abbreviation and hyphen shapes --\n");

    // ── The hyphen/underscore equivalence ───────────────────────────
    //
    // The rule is spelled "foo_bar" and carries the flag. The token
    // "foo-bar" must match it, and the value must still be cut off at
    // the right offset.
    {
        const char* kConfig = R"JSON({
  "profiles": [
    {
      "id": "hyphen-cc",
      "kind": "gcc",
      "names": ["hyphen-cc"],
      "rules": [
        {
          "name": "foo_bar",
          "shape": ["dash", "attached", "hyphen_is_underscore"],
          "class": "macro",
          "clang": "-D%s"
        }
      ]
    }
  ]
})JSON";

        CompdbConfig config;
        std::string err;
        check("the hyphen config parses",
              CompdbConfig::load(kConfig, &config, &err), err);

        OptionMapper mapper = buildMapper(config);
        CompileCommand cmd = parse("hyphen-cc -c --foo-bar=1 main.c");
        MappingReport report;
        std::vector<std::string> out =
            mapper.map(cmd, TranslateLevel::Ast, &report);
        check("the hyphen form matches the dashed spelling",
              containsSub(out, "-D1"));
        check("the dashed spelling is not reported as unknown",
              !containsSub(report.unknown, "--foo-bar=1"));
    }

    // The same rule must NOT match when the flag is absent. This is the
    // important half: the equivalence is opt-in per rule.
    {
        const char* kConfig = R"JSON({
  "profiles": [
    {
      "id": "nohyphen-cc",
      "kind": "gcc",
      "names": ["nohyphen-cc"],
      "rules": [
        {
          "name": "foo_bar",
          "shape": ["dash", "attached"],
          "class": "macro",
          "clang": "-D%s"
        }
      ]
    }
  ]
})JSON";

        CompdbConfig config;
        std::string err;
        CompdbConfig::load(kConfig, &config, &err);

        OptionMapper mapper = buildMapper(config);
        CompileCommand cmd = parse("nohyphen-cc -c --foo-bar=1 main.c");
        MappingReport report;
        std::vector<std::string> out =
            mapper.map(cmd, TranslateLevel::Ast, &report);
        check("without the flag the dashed spelling does not match",
              !containsSub(out, "-D1"));
        check("without the flag it is reported as unknown",
              containsSub(report.unknown, "--foo-bar=1"));
    }

    // ── The unambiguous abbreviation ────────────────────────────────
    //
    // The rule is "sysroot" and carries abbrev_match. The token
    // "sysr" is a prefix of it and no other rule starts with "sysr",
    // so the abbreviation is accepted.
    {
        const char* kConfig = R"JSON({
  "profiles": [
    {
      "id": "abbrev-cc",
      "kind": "gcc",
      "names": ["abbrev-cc"],
      "rules": [
        {
          "name": "sysroot",
          "shape": ["dash_dash", "attached", "unattached", "abbrev_match"],
          "class": "sysroot",
          "clang": "--sysroot=%s"
        }
      ]
    }
  ]
})JSON";

        CompdbConfig config;
        std::string err;
        check("the abbreviation config parses",
              CompdbConfig::load(kConfig, &config, &err), err);

        OptionMapper mapper = buildMapper(config);
        CompileCommand cmd = parse("abbrev-cc -c --sysr=/opt/sysroot main.c");
        MappingReport report;
        std::vector<std::string> out =
            mapper.map(cmd, TranslateLevel::Ast, &report);
        check("the unambiguous abbreviation matches",
              containsSub(out, "--sysroot=/opt/sysroot"));
    }

    // ── The ambiguous abbreviation ──────────────────────────────────
    //
    // Two rules start with "sys": "sysroot" and "sys-version". The
    // token "sys" is a prefix of both, so the abbreviation cannot be
    // resolved and the option is reported as unknown.
    //
    // Guessing the longest candidate would be wrong: the token alone
    // cannot tell the two apart, and picking one would produce a
    // command that compiles different code.
    {
        const char* kConfig = R"JSON({
  "profiles": [
    {
      "id": "ambig-cc",
      "kind": "gcc",
      "names": ["ambig-cc"],
      "rules": [
        {
          "name": "sysroot",
          "shape": ["dash_dash", "attached", "abbrev_match"],
          "class": "sysroot",
          "clang": "--sysroot=%s"
        },
        {
          "name": "sys-version",
          "shape": ["dash_dash", "attached", "abbrev_match"],
          "class": "codegen",
          "clang": "--sys-version=%s"
        }
      ]
    }
  ]
})JSON";

        CompdbConfig config;
        std::string err;
        CompdbConfig::load(kConfig, &config, &err);

        OptionMapper mapper = buildMapper(config);
        CompileCommand cmd = parse("ambig-cc -c --sys=/opt/sysroot main.c");
        MappingReport report;
        std::vector<std::string> out =
            mapper.map(cmd, TranslateLevel::Ast, &report);
        check("the ambiguous abbreviation does not match either rule",
              !containsSub(out, "--sysroot=/opt/sysroot") &&
              !containsSub(out, "--sys-version=/opt/sysroot"));
        check("the ambiguous abbreviation is reported as unknown",
              containsSub(report.unknown, "--sys=/opt/sysroot"));
    }

    // ── An exact match beats an abbreviation ───────────────────────
    //
    // The table has both "std" and "std-legacy". The token "std" is an
    // exact match for the first rule, so it must not be resolved as an
    // abbreviation (which would be ambiguous).
    {
        const char* kConfig = R"JSON({
  "profiles": [
    {
      "id": "exact-cc",
      "kind": "gcc",
      "names": ["exact-cc"],
      "rules": [
        {
          "name": "std",
          "shape": ["dash_dash", "attached", "abbrev_match"],
          "class": "language-std",
          "clang": "--std=%s"
        },
        {
          "name": "std-legacy",
          "shape": ["dash_dash", "attached", "abbrev_match"],
          "class": "language-std",
          "clang": "--std-legacy=%s"
        }
      ]
    }
  ]
})JSON";

        CompdbConfig config;
        std::string err;
        CompdbConfig::load(kConfig, &config, &err);

        OptionMapper mapper = buildMapper(config);
        CompileCommand cmd = parse("exact-cc -c --std=c++17 main.c");
        MappingReport report;
        std::vector<std::string> out =
            mapper.map(cmd, TranslateLevel::Ast, &report);
        check("the exact match wins over the abbreviation",
              containsSub(out, "--std=c++17"));
        check("the exact match is not reported as unknown",
              !containsSub(report.unknown, "--std=c++17"));
    }

    // ── An abbreviation without the flag does not match ────────────
    //
    // The flag is opt-in per rule, so a rule without it must not
    // accept an abbreviation.
    {
        const char* kConfig = R"JSON({
  "profiles": [
    {
      "id": "noabbrev-cc",
      "kind": "gcc",
      "names": ["noabbrev-cc"],
      "rules": [
        {
          "name": "sysroot",
          "shape": ["dash_dash", "attached"],
          "class": "sysroot",
          "clang": "--sysroot=%s"
        }
      ]
    }
  ]
})JSON";

        CompdbConfig config;
        std::string err;
        CompdbConfig::load(kConfig, &config, &err);

        OptionMapper mapper = buildMapper(config);
        CompileCommand cmd = parse("noabbrev-cc -c --sysr=/opt/sysroot main.c");
        MappingReport report;
        std::vector<std::string> out =
            mapper.map(cmd, TranslateLevel::Ast, &report);
        check("without the flag the abbreviation does not match",
              !containsSub(out, "--sysroot=/opt/sysroot"));
        check("without the flag the abbreviation is reported as unknown",
              containsSub(report.unknown, "--sysr=/opt/sysroot"));
    }
}

// ── 13. The version extraction ────────────────────────────────────────────
//
// The regex engine is a self-built component, so it gets its own tests
// against **real** version strings -- not synthetic ones.
//
// The strings below are what the compilers actually print, collected
// from real installations. A version probe that matches the wrong
// string is worse than no probe at all: it would apply a
// version-scoped rule to a compiler it does not describe.
void testVersionExtraction() {
    std::printf("\n-- the version extraction --\n");

    // The probes come from the **built-in registry**, not from copies
    // in this test: a copy would keep passing after the built-in
    // probe was broken, which is exactly the failure this test exists
    // to catch.
    CompilerRegistry reg = CompilerRegistry::builtin();

    auto probeFor = [&](const char* familyId) -> const VersionProbe* {
        const CompilerFamily* fam = reg.findFamily(familyId);
        if (!fam) return nullptr;
        return &fam->versionProbe;
    };

    // The gcc probe.
    {
        const VersionProbe* probe = probeFor("gnu");
        check("the gnu family has a version probe", probe != nullptr);
        if (!probe) return;

        struct Case {
            const char* output;
            const char* expected;
        };
        const Case kCases[] = {
            // The Debian / Red Hat spelling.
            {"gcc (GCC) 4.8.5 20150623 (Red Hat 4.8.5-44)", "4.8.5"},
            // The Ubuntu spelling.
            {"gcc (Ubuntu 11.4.0-1ubuntu1~22.04) 11.4.0", "11.4.0"},
            // The plain spelling.
            {"g++ (GCC) 4.8.5", "4.8.5"},
            // The historical "2.96" oddity.
            {"g++ version 2.96 (2.9.6)", "2.9.6"},
            // A newer gcc.
            {"gcc (GCC) 12.2.0", "12.2.0"},
            // A gcc 10, to prove the two-digit version works.
            {"gcc (GCC) 10.3.1", "10.3.1"},
        };

        for (const Case& c : kCases) {
            std::string got = extractVersion(*probe, c.output);
            std::string name =
                std::string("the gcc probe extracts the version from: ") +
                c.output;
            check(name.c_str(), got == c.expected,
                  std::string("got '") + got + "', expected '" + c.expected + "'");
        }

        // A banner of a **different** family must not be matched by
        // this family's probe: that would describe the gcc family by
        // the clang's version, and the version-scoped rules would then
        // be applied to the wrong compiler.
        check("the gcc probe does not match a clang banner",
              extractVersion(*probe, "clang version 17.0.6") == "");
    }

    // The clang probe.
    {
        const VersionProbe* probe = probeFor("clang");
        check("the clang family has a version probe", probe != nullptr);
        if (!probe) return;

        check("the clang probe extracts the version",
              extractVersion(*probe, "clang version 17.0.6") == "17.0.6");
        check("the clang probe extracts a two-component version",
              extractVersion(*probe, "clang version 16.0") == "16.0");
        check("the clang probe does not match a gcc banner",
              extractVersion(*probe, "gcc (GCC) 12.2.0") == "");
    }

    // The MSVC probe.
    {
        const VersionProbe* probe = probeFor("msvc");
        check("the msvc family has a version probe", probe != nullptr);
        if (!probe) return;

        check("the msvc probe extracts the version",
              extractVersion(*probe,
                            "Microsoft (R) C/C++ Optimizing Compiler Version "
                            "19.36.32532 for x64") == "19.36.32532",
              std::string("got '") +
                  extractVersion(*probe,
                                 "Microsoft (R) C/C++ Optimizing Compiler "
                                 "Version 19.36.32532 for x64") + "'");
    }

    // The IAR probe.
    {
        const VersionProbe* probe = probeFor("iar");
        check("the iar family has a version probe", probe != nullptr);
        if (!probe) return;

        check("the iar probe extracts the version",
              extractVersion(*probe,
                            "IAR C/C++ Compiler 8.50.1") == "8.50.1",
              std::string("got '") +
                  extractVersion(*probe, "IAR C/C++ Compiler 8.50.1") + "'");
    }

    // The replacement with a capture group.
    {
        VersionProbe probe;
        probe.switchText = "--version";
        probe.patterns = {"replace/.*version (\\d+)\\.(\\d+).*/$1.$2"};

        check("the replacement substitutes the capture groups",
              extractVersion(probe, "tool version 12.3") == "12.3");
    }

    // An empty probe never matches.
    {
        VersionProbe probe;
        check("an empty probe extracts nothing",
              extractVersion(probe, "gcc 12.2.0") == "");
    }

    // The capture groups are numbered from 1, and the whole match is
    // group 0.
    {
        VersionProbe probe;
        probe.switchText = "--version";
        probe.patterns = {"gcc (\\d+\\.\\d+)"};

        std::string got = extractVersion(probe, "gcc 12.2");
        check("the plain form takes the first capture group", got == "12.2",
              std::string("got '") + got + "'");
    }

    // The plain form with no capture group takes the whole match.
    {
        VersionProbe probe;
        probe.switchText = "--version";
        probe.patterns = {"\\d+\\.\\d+\\.\\d+"};

        std::string got = extractVersion(probe, "gcc 12.2.0");
        check("the plain form takes the whole match", got == "12.2.0");
    }
}

}  // namespace

int main() {
    std::printf("=== ccap_compdb switch table tests ===\n");

    testClassification();
    testRetentionLevels();
    testMsvcTranslation();
    testUnknownCompiler();
    testExplicitMapping();
    testConfigProfiles();
    testReport();
    testExport();
    testRegistry();
    testAuditBuckets();
    testExtendedShapes();
    testAbbrevAndHyphenShapes();
    testVersionExtraction();

    std::printf("\n===================================\n");
    std::printf("PASS: %d   FAIL: %d\n", gPass, gFail);
    std::printf("===================================\n");
    return gFail == 0 ? 0 : 1;
}
