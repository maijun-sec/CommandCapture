// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb_families - the built-in compiler families
//
// See ccap_compdb_registry.h for the design rationale.
//
// This file assembles the built-in registry: the families, their
// variant chains, their match patterns and their version probes.
//
// THE FAMILIES
//
//   gnu      gcc, g++, g++cc          -- the gcc family and its chain
//   clang    clang, clang++           -- the clang family
//   msvc     cl, clang-cl             -- the MSVC family
//   iar      icc, iccarm              -- IAR EW
//   generic  (no variants)            -- the conservative fallback
//
// WHY THE VARIANT CHAIN MATTERS
//
// gcc and g++ share almost every option, but not every one:
//
//   - "g++" compiles C++ by default; "gcc" does not. A translation
//     that used "gcc" to compile a .cpp file has to say "-x c++",
//     and one that used "g++" does not.
//   - "g++" pulls in the C++ standard library search paths.
//
// So the variants are not cosmetic. The g++ variant carries the
// rule that forces the C++ language mode when the source is C++.
//
// WHY THE VERSION PROBE MATTERS
//
// The gcc version decides several option semantics:
//
//   - "-std=c++11" and later exist from gcc 4.8 on.
//   - "-fno-common" became the default in gcc 10 (the opposite of
//     what it did before).
//   - The __int128 type exists from gcc 4.6 on.
//
// The probe is a **switch** ("--version") plus an **ordered** list of
// patterns. The order matters: a greedy pattern would swallow the
// version of a different compiler family. The patterns below are
// ordered least-likely-to-mis-match first, exactly as the reference
// config does.
//
// THE PROBE IS OPTIONAL
//
// The mapping engine never runs the compiler by itself. The version
// is used only when the caller supplies it (from a build log that
// recorded it, or from a configuration the user wrote). The rules
// that are version-scoped simply do not apply when the version is
// unknown -- which is the safe direction, because a rule whose
// precondition could not be verified must not fire.

#include "ccap_compdb_registry.h"

namespace ccap {
namespace compdb {

namespace {

// The gnu family.
CompilerFamily makeGnuFamilyEntry() {
    CompilerFamily fam;
    fam.id = "gnu";
    fam.kind = CompilerKind::Gcc;
    fam.profile = makeGnuFamilyProfile();

    // The variant chain: gcc -> g++ -> g++cc.
    //
    // "g++cc" is gcc driven as g++ but compiling C (the
    // "g++ -x c" arrangement). The same thing is modelled with
    // its own `g++cc` translator.
    CompilerVariant gcc;
    gcc.id = "gcc";
    gcc.language = "C";
    gcc.familyHead = true;
    gcc.nextVariantId = "g++";

    CompilerVariant gxx;
    gxx.id = "g++";
    gxx.language = "C++";
    gxx.nextVariantId = "g++cc";

    CompilerVariant gxxcc;
    gxxcc.id = "g++cc";
    gxxcc.language = "C";

    fam.variants = {gcc, gxx, gxxcc};

    // The version probe.
    //
    // The patterns are ordered least-likely-to-mis-match first, which
    // is the established rule ("Order Version Regex least likely to
    // incorrectly match first").
    //
    // The first pattern handles the "2.96 is really 2.9.6" oddity,
    // which is a real historical gcc version string.
    fam.versionProbe.switchText = "--version";
    fam.versionProbe.patterns = {
        // The historical "2.96" spelling.
        //
        // It comes **first** because it is the only pattern that
        // recognises a version that is not written the normal way:
        // "g++ version 2.96 (2.9.6)" is really 2.9.6, but the banner
        // also contains "2.96", and the normal patterns would pick
        // that up first.
        //
        // The pattern matches the "2.96" directly rather than trying
        // to allow for a suffix: a greedy "2\.9.*" would swallow the
        // " (2.9.6)" that the rest of the pattern needs.
        "replace/.*g[c+][c+] version 2\\.96 \\(2\\.9[^)]*\\).*/2.9.6",

        // "gcc (GCC) 4.8.5 20150623 (Red Hat 4.8.5-44)"
        "replace/.*g[c+][c+][^\\s]* [^\\s]+ (\\d+\\.\\d+(\\.\\d+)?) \\(.*\\).*/$1",
        // "gcc (Ubuntu 11.4.0-1ubuntu1~22.04) 11.4.0"
        "replace/.*g[c+][c+][^\\s]* \\([^)]*\\) (\\d+\\.\\d+(\\.\\d+)?).*/$1",
        // "g++ (GCC) 4.8.5" -- the version right after the word.
        "replace/.*g[c+][c+][^\\s]* [^\\s]+ (\\d+\\.\\d+(\\.\\d+)?).*/$1",
        // "gcc (GCC) 12.2.0" -- the version in the parentheses.
        "replace/.*g[c+][c+][^\\s]* \\(GCC\\) (\\d+\\.\\d+(\\.\\d+)?).*/$1",

        // The last resort: the first dotted number in the output.
        //
        // It is **family-scoped** on purpose. An unscoped fallback
        // ("the first dotted number anywhere") would match a clang
        // banner when the gcc probe runs on it, and the gcc family
        // would then be described by the clang's version -- which
        // would apply the wrong version-scoped rules.
        //
        // The family marker has to be present, and the version has to
        // follow it.
        "replace/.*g[c+][c+][^\\s]*[^\\d]*(\\d+\\.\\d+(\\.\\d+)?).*/$1",
    };

    return fam;
}

// The clang family.
CompilerFamily makeClangFamilyEntry() {
    CompilerFamily fam;
    fam.id = "clang";
    fam.kind = CompilerKind::Clang;
    fam.profile = makeClangFamilyProfile();

    CompilerVariant clang;
    clang.id = "clang";
    clang.language = "C";
    clang.familyHead = true;
    clang.nextVariantId = "clang++";

    CompilerVariant clangxx;
    clangxx.id = "clang++";
    clangxx.language = "C++";

    fam.variants = {clang, clangxx};

    fam.versionProbe.switchText = "--version";
    fam.versionProbe.patterns = {
        // "clang version 17.0.6 (...)"
        "replace/.*clang version (\\d+\\.\\d+(\\.\\d+)?).*/$1",
        // The family-scoped last resort: the first dotted number that
        // follows the family marker.
        //
        // The scoping matters: an unscoped fallback would match a gcc
        // banner, and the clang family would then be described by the
        // gcc's version.
        "replace/.*clang[^\\s]*[^\\d]*(\\d+\\.\\d+(\\.\\d+)?).*/$1",
    };

    return fam;
}

// The MSVC family.
CompilerFamily makeMsvcFamilyEntry() {
    CompilerFamily fam;
    fam.id = "msvc";
    fam.kind = CompilerKind::Msvc;
    fam.profile = makeMsvcFamilyProfile();

    CompilerVariant cl;
    cl.id = "cl";
    cl.language = "C";
    cl.familyHead = true;
    cl.nextVariantId = "clang-cl";

    // clang-cl accepts the MSVC spelling and is the driver we
    // actually emit, so the "clang-cl" variant is the one that
    // matters for the output.
    CompilerVariant clangcl;
    clangcl.id = "clang-cl";
    clangcl.language = "C";

    fam.variants = {cl, clangcl};

    fam.versionProbe.switchText = "";
    fam.versionProbe.patterns = {
        // The MSVC banner is "Microsoft (R) C/C++ Optimizing Compiler
        // Version 19.36.32532 for x64".
        "replace/.*Version (\\d+\\.\\d+(\\.\\d+)?).*/$1",
    };

    return fam;
}

// The IAR family.
CompilerFamily makeIarFamilyEntry() {
    CompilerFamily fam;
    fam.id = "iar";
    fam.kind = CompilerKind::Iar;
    fam.profile = makeIarFamilyProfile();

    CompilerVariant icc;
    icc.id = "icc";
    icc.language = "C";
    icc.familyHead = true;
    icc.nextVariantId = "iccarm";

    CompilerVariant iccarm;
    iccarm.id = "iccarm";
    iccarm.language = "C";

    fam.variants = {icc, iccarm};

    fam.versionProbe.switchText = "--version";
    fam.versionProbe.patterns = {
        // The banner is "IAR C/C++ Compiler 8.50.1".
        //
        // The "C/C++" is written as "C.C\\+\\+" rather than "C/C\\+\\+"
        // on purpose: the pattern is separated from its replacement by
        // a '/', and the separator is the **last** slash in the
        // pattern. A literal slash inside the regex would therefore
        // move the separator, and the regex would be cut in the wrong
        // place.
        //
        // The '.' matches the '/', and it also matches the space in
        // "IAR C/C++ Compiler" -- which is the intent: the pattern
        // does not care about the exact spelling of the banner, only
        // about the version that follows it.
        "replace/.*IAR C.C\\+\\+ Compiler (\\d+\\.\\d+(\\.\\d+)?).*/$1",
    };

    return fam;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// The built-in registry
// ═══════════════════════════════════════════════════════════════════════════

CompilerRegistry CompilerRegistry::builtin() {
    CompilerRegistry reg;

    reg.upsertFamily(makeGnuFamilyEntry());
    reg.upsertFamily(makeClangFamilyEntry());
    reg.upsertFamily(makeMsvcFamilyEntry());
    reg.upsertFamily(makeIarFamilyEntry());

    // The generic family: the conservative fallback.
    {
        CompilerFamily generic;
        generic.id = "generic";
        generic.kind = CompilerKind::Generic;
        generic.profile = makeGenericFamilyProfile();
        generic.profile.builtin = true;
        reg.upsertFamily(generic);
    }

    // ── The match patterns ──────────────────────────────────────────
    //
    // The patterns are the "how does an executable name map onto a
    // variant" table. The `extra_comp` mechanism does the same thing
    // with `gcc-*` and `*-gcc` entries.
    //
    // NOTE ON THE REDIRECT
    //
    // There is deliberately **no** built-in redirect.
    //
    // The tempting one is "replace/gcc/g++" -- the idea that an
    // executable called "gcc" is really the g++ driver. That is
    // **wrong**, and it is wrong in a way that produces a silently
    // incorrect artifact:
    //
    //   "gcc"  is the C driver. It compiles .c as C and .cpp as C++
    //          (unlinked), and it does not link libstdc++.
    //   "g++"  is the C++ driver. It compiles .cpp as C++ and links
    //          libstdc++.
    //
    // They are two different executables, not one executable under two
    // names. Redirecting "gcc" to the g++ variant would make every C
    // translation unit in the build claim to be C++ -- and the only
    // symptom would be a link error, or worse, a link that succeeds
    // with the wrong standard library.
    //
    // A redirect is for the case where an executable name really does
    // mean another compiler: a vendor wrapper, a "cc" that is
    // actually a cross compiler, a symlink. Those are facts about a
    // particular machine, so they belong in the **user's**
    // configuration, not in the built-in table.
    //
    // The variant is chosen by the executable name instead (gcc -> the
    // gcc variant, g++ -> the g++ variant), which is the correct
    // statement about the same distinction.

    // The cross compilers.
    const char* kGnuGlobs[] = {
        "*-gcc", "*-g++", "gcc-*", "g++-*", "*gcc", "*g++",
    };
    for (const char* g : kGnuGlobs) {
        CompilerMatch m;
        m.pattern = g;
        m.variantId = "gnu";
        reg.addMatch(m);
    }

    // The clang family.
    const char* kClangGlobs[] = {"clang-*", "clang++-*", "*-clang", "*-clang++"};
    for (const char* g : kClangGlobs) {
        CompilerMatch m;
        m.pattern = g;
        m.variantId = "clang";
        reg.addMatch(m);
    }

    // The MSVC family.
    const char* kMsvcGlobs[] = {"cl-*", "*-cl", "clang-cl"};
    for (const char* g : kMsvcGlobs) {
        CompilerMatch m;
        m.pattern = g;
        m.variantId = "msvc";
        reg.addMatch(m);
    }

    // The IAR family.
    const char* kIarGlobs[] = {"iccarm*", "icc*", "icpc*"};
    for (const char* g : kIarGlobs) {
        CompilerMatch m;
        m.pattern = g;
        m.variantId = "iar";
        reg.addMatch(m);
    }

    return reg;
}

}  // namespace compdb
}  // namespace ccap
