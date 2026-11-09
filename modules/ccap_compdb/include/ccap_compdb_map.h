// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb_map - the compiler switch mapping table
//
// WHY THIS EXISTS
//
// The existing translation (clang_translator.cpp) converts a compile
// command into a clang command line by looking at a **fixed, hard-coded
// list** of options. That works for the options it happens to know, but
// it cannot answer the questions a real toolchain integration needs:
//
//   1. Which options must be **kept** for a clang AST to be accurate?
//   2. Which options must be **kept** for a clang LLVM BC file to be
//      accurate?
//   3. Which options are **noise** (linker flags, warning levels, code
//      generation tuning) and can be dropped?
//   4. Which options **change the meaning** of the translation unit and
//      therefore must never be dropped silently?
//
// The answers are **per-target**, not per-compiler. A static-analysis
// front end wants the preprocessor state and nothing else. A bitcode
// generator wants the same, plus the language mode and the target
// triple, but not the optimisation level. A human reading the command
// wants all of it.
//
// So the model here is two-dimensional:
//
//   AXIS 1 -- the option table
//     A table of *switch rules*, keyed by the option name, that says
//     how an option is spelled (attached / unattached / equal),
//     what it means (include path, macro, language standard, ...),
//     and what it maps to in clang.
//
//   AXIS 2 -- the retention level
//     Each option has a **retention class**, and each level (AST / BC)
//     says which classes it keeps.
//
// The two axes are independent, which is the whole point: adding a new
// compiler family means adding rules to the table, not rewriting the
// level logic.
//
// THE TWO-ARTEFACT SPLIT, AND WHY WE KEEP IT
//
// Static-analysis toolchains solve exactly this problem, and the
// established answer is two artefacts per compiler family:
//
//   <family>_config.xml    -- the compiler identity, the env vars that
//                              feed includes, the special switches
//                              (-c, -E, -o), the "how do I find out your
//                              version" regex, the macro candidates.
//
//   <family>_switches.dat  -- the option table: one line per switch,
//                              with flags describing the shape of the
//                              option and how to translate it.
//
// That split is good, and we adopt it. The two files answer different
// questions and change for different reasons:
//
//   * the config changes when the *toolchain* changes (a new version,
//     a new env var, a new include search path)
//   * the switches file changes when the *option set* changes
//
// We keep the same separation, but we put it in one JSON file with two
// sections, because a single file is easier to review, easier to
// distribute, and easier to keep consistent than a pair of files with
// an implicit coupling.
//
// THE SWITCH SHAPE FLAGS
//
// The *shape* of an option is encoded in a bitmask. We do the same,
// because the shape is what decides whether the value is part of the
// option token or the next argv entry:
//
//   oa_dash        the option is spelled with a single dash
//   oa_dash_dash   the option is spelled with a double dash
//   oa_attached    the value may be attached:  -Ifoo
//   oa_unattached  the value may be separate:   -I foo
//   oa_equal       the value may be given with '=': -I=foo
//   oa_optional    the value may be omitted entirely: -O
//   oa_required    the option is only valid with a value
//   oa_copy        pass the option through to the next stage
//   oa_skip_arg    the option is not a compile option at all
//   (drop it, but keep any attached value)
//   oa_map         rewrite the option into another one
//   oa_split       split the value on a separator and translate each
//   oa_custom      handled by a named custom translation function
//   oa_path        the value is a filesystem path
//
// We use these names because they are the established vocabulary for
// this problem: the naming is shared across the toolchains that model
// compiler option sets, so a reader who knows the format reads ours.
//
// THE CLASSIFICATION VOCABULARY
//
// Instead of "what do I do with this switch", we record
// **what the option means**. That is the durable fact, and it is what
// decides retention:
//
//   OptionClass::Unknown          -- we do not know what it does
//   OptionClass::Include          -- -I / /I
//   OptionClass::SystemInclude    -- -isystem / -idirafter / /external:I
//   OptionClass::Framework        -- -F
//   OptionClass::Macro            -- -D / /D
//   OptionClass::MacroUndef      -- -U
//   OptionClass::PreInclude       -- -include / /FI
//   OptionClass::LanguageStd      -- -std= / /std:
//   OptionClass::Language          -- -x / -xc++ (force the language)
//   OptionClass::Target           -- --target / -march / -mcpu
//   OptionClass::Sysroot          -- --sysroot
//   OptionClass::DebugInfo        -- -g / /Zi
//   OptionClass::Optimization     -- -O2
//   OptionClass::Output           -- -o / /Fo
//   OptionClass::CompileOnly      -- -c / /c
//   OptionClass::PreprocessOnly   -- -E / -M
//   OptionClass::Warning          -- -W... / /W4
//   OptionClass::Dependency       -- -MD / -MF / -MT
//   OptionClass::Linker           -- -Wl,... / -l / .o files
//   OptionClass::Codegen          -- -f... (code generation tuning)
//   OptionClass::StaticAnalysis   -- --analyze / -fanalyzer
//   OptionClass::Coverage         -- --coverage / -fprofile-arcs
//   OptionClass::Sanitizer        -- -fsanitize=...
//   OptionClass::ResponseFile     -- @file
//   OptionClass::Verbosity        -- -v / --verbose
//   OptionClass::Pass             -- -fno-... (a -f flag)
//
// RETENTION LEVELS
//
// Two levels, because those are the two the user asked for and they
// are genuinely different:
//
//   Level::Ast   -- the arguments needed for clang to parse the
//                   translation unit into a correct AST.
//   Level::Bc    -- the arguments needed for clang to emit an LLVM
//                   bitcode file.
//
// The difference is not arbitrary. The AST level needs the *source
// semantics*: the preprocessor state, the language, the headers.
// The BC level additionally needs the *target* semantics: the target
// triple, the data layout, the CPU. A BC file built for the wrong
// target is a silently wrong artifact, while an AST built for the
// wrong target is usually still correct (it just describes the wrong
// machine).
//
// So the retention matrix is roughly:
//
//   AST:  include, system include, macro, macro-undef, pre-include,
//         language std, language, sysroot, framework, target,
//         compile-only, output, response file
//   BC:   all of AST, plus debug info (so the BC has debug metadata)
//         and the source file (so the BC can name its source)
//
// And these are **dropped at both levels**:
//   linker, dependency, warning, optimization, codegen, verbosity,
//   coverage, sanitizer, static-analysis, preprocess-only
//
// Dropping them is not "losing information" -- it is removing
// arguments that a clang front end either rejects or that change
// nothing about the AST/BC shape.
//
// WHAT IS NEVER DROPPED SILENTLY
//
// Some options change the *meaning* of the translation unit. If we
// drop them, the resulting AST is a lie: it looks like it describes
// the code, but the code was actually compiled under different
// conditions. The three that matter most:
//
//   -U<macro>      undoes a -D; keeping only the -D set changes the
//                  meaning of every macro
//   -include       forces a header in; the header can define macros
//   -x / -std=     selects the language and the language mode
//
// So the mapping engine records, for every option it drops, *why* it
// was dropped. A caller that wants to be sure can read the report.

#pragma once

#include <map>
#include <string>
#include <vector>

namespace ccap {
namespace compdb {

// ═══════════════════════════════════════════════════════════════════════════
// Compiler kinds
// ═══════════════════════════════════════════════════════════════════════════

// The "kind" of a compiler
//
// Each kind corresponds to one adapter implementation.
// Adding a new compiler = adding one adapter file + one enum value here,
// **without touching any other file**.
//
// (The enum lives in this header rather than in ccap_compdb.h because the
// compiler profile -- the switch table -- has to refer to it, and this
// header is included first.)
enum class CompilerKind {
    Unknown = 0,
    Gcc,        // gcc / g++ / cc / c++ / <target>-gcc ...
    Clang,      // clang / clang++ / clang-<ver>
    Msvc,       // MSVC cl.exe
    Iar,        // IAR EW
    Generic,    // fallback: generic extraction of -I/-D only

    // ── The JVM languages ─────────────────────────────────────────────
    //
    // These are the compilers the JVM capture agent hooks. They are
    // **not** native compilers: they produce class files, not object
    // files, and their options are their own.
    //
    // They are separate kinds rather than one "jvm" kind because each
    // compiler has its own option syntax, and a consumer that knows the
    // difference between a Kotlin compile and a Scala compile can do
    // more with the capture. The adapter for each kind is isolated, just
    // like the native ones.
    Javac,      // javac (and sjavac, which drives javac)
    Ecj,        // Eclipse's compiler
    Kotlin,     // kotlinc / K2
    Scala3,     // Scala 3 (dotc)
    Scala2,     // Scala 2 (nsc)

    // ── The .NET languages ─────────────────────────────────────────────
    //
    // These are the compilers the .NET backend reaches. They are **not**
    // native compilers and they are **not** JVM compilers: they run on
    // the CLR, they produce assemblies rather than object files, and
    // their option syntax is their own.
    //
    // The C# one is the Roslyn command-line compiler (csc). The Visual
    // Basic and F# ones share the same driver shape and the same option
    // spelling, so they are one kind with different variants rather
    // than three kinds.
    Csharp,     // Roslyn csc (C#)
    VbNet,      // Roslyn vbc (Visual Basic)
    FSharp,     // Roslyn fsc (F#)

    // ── The Go language ────────────────────────────────────────────────
    //
    // Go has no "compiler" executable. The build drives the **tools**
    // of the toolchain, and the two that matter are:
    //
    //   compile   one package  -> one archive
    //   link      the archives -> the binary
    //
    // They are recognized by their **path**, never by their name:
    // "compile" and "link" are far too common as program names, and
    // matching on them would turn an unrelated tool into a Go
    // compile. See CompilerRule::pathShapes.
    Go,
};

// ═══════════════════════════════════════════════════════════════════════════
// The retention level
// ═══════════════════════════════════════════════════════════════════════════

// What the translated command is going to be used for.
//
// This is the second axis of the model. The first axis is the option
// table (what does this option mean); this one is the retention level
// (what does the consumer care about).
enum class TranslateLevel {
    // A clang AST. The translation unit must parse to the same tree
    // the original build produced.
    Ast = 0,

    // An LLVM bitcode file. The AST plus the target description.
    Bc = 1,
};

// ── WHY THE TWO LEVELS DIFFER, AND WHERE THE LINE REALLY IS ──────────────
//
// The obvious split is "the AST is about the source, the bitcode is
// about the machine" -- so the target would be a bitcode-only thing.
//
// **That is wrong**, and the measurement below is why.
//
//   Measured with clang 15.0.7 (the -ast-dump of the same source
//   under different targets, with the pointer addresses normalized):
//
//     target            __int128   __builtin_va_list   SVE types
//     ----------------  ---------  ------------------  ---------
//     x86_64-linux-gnu  yes        __va_list_tag[1]    no
//     i386-linux-gnu    NO         __va_list           no
//     aarch64-linux-gnu yes        __va_list_tag[1]    YES (41)
//     armv7-linux-gnu   NO         __va_list           no
//
//   So the target triple **changes the AST**: it decides which
//   builtin types exist. And it changes more than the shape -- it
//   changes whether the code compiles at all:
//
//     $ clang -target i386-linux-gnu -fsyntax-only t.c
//     error: __int128 is not supported on this target
//
//   Dropping the target from an AST command therefore does not
//   produce "the same AST"; it produces a *different* AST, or a
//   compile error.
//
// THE LINE THAT ACTUALLY MATTERS
//
// The real split is **triple vs CPU feature level**, not AST vs BC:
//
//   the triple    (--target=...)  decides the builtin types and the
//                                  ABI           -> needed at BOTH levels
//   the CPU level (-march/-mcpu)   decides the instruction set
//                                  -> AST is identical with or without it
//
//   Measured: the AST of the same source under -march=x86-64,
//   -march=x86-64-v2 and -march=x86-64-v3 is **identical**.
//
// So the retention rule is:
//
//   AST  keeps: the triple, the sysroot, the preprocessor state,
//              the language
//   BC   adds: the CPU feature level and the debug info
//
// and the thing that is dropped at both levels is the *code
// generation tuning* (-O2, -ffast-math, -fno-strict-aliasing ...),
// which changes the generated code but not the AST.

const char* translateLevelName(TranslateLevel level);

// Parse a level name ("ast" / "bc"). Returns false when unknown.
bool translateLevelFromName(const std::string& name, TranslateLevel* out);

// ═══════════════════════════════════════════════════════════════════════════
// The option classification
// ═══════════════════════════════════════════════════════════════════════════

// What an option *means*.
//
// The classification is what decides retention, so it must be about
// the meaning and not about the syntax. (-I and /I are the same
// meaning; -include and /FI are the same meaning.)
enum class OptionClass {
    Unknown = 0,
    Include,
    SystemInclude,
    Framework,
    Macro,
    MacroUndef,
    PreInclude,
    LanguageStd,
    Language,
    // The target **triple**: "--target=x86_64-linux-gnu", "-arch".
    //
    // It decides the builtin types and the ABI, so it is needed at
    // **both** levels -- see the measurement above.
    Target,
    // The target **CPU feature level**: "-march=...", "-mcpu=...".
    //
    // It decides the instruction set, not the AST, so it is only
    // needed at the bitcode level.
    TargetCpu,
    Sysroot,
    DebugInfo,
    Optimization,
    Output,
    CompileOnly,
    PreprocessOnly,
    Warning,
    Dependency,
    Linker,
    Codegen,
    StaticAnalysis,
    Coverage,
    Sanitizer,
    ResponseFile,
    Verbosity,
    Pass,

    // ── The .NET languages ─────────────────────────────────────────────
    //
    // A .NET compile is described by its **reference** set, not by an
    // include path: the compiler reads types from reference assemblies
    // and from the project's own sources, and there is no "search
    // directory for headers" notion at all.
    //
    // So the .NET concepts get their own classes rather than being
    // folded into Include/SystemInclude. A consumer that has to look
    // a type up in a reference **assembly** (a .dll with a metadata
    // table) rather than in a directory of headers cannot afford to
    // have the two confused -- they are different things with
    // different lookup rules.
    //
    //   Reference      -- /r:<assembly>   a reference assembly
    //   AnalyzedFile   -- /analyzer:<dll> a source generator / analyzer
    //   SourceFile     -- a .cs file to compile
    //   Nullable       -- /nullable:<mode>
    //   LangVersion    -- /langversion:<version>
    //   Unsafe         -- /unsafe, /unsafe-
    //   PlatformTarget -- /platform:<target>
    //   Optimize       -- /optimize, /optimize-
    //   DebugSymbols   -- /debug, /debug:<mode>
    //   Define         -- /define:<sym>[;...]
    Reference,
    AnalyzedFile,
    SourceFile,
    Nullable,
    LangVersion,
    Unsafe,
    PlatformTarget,
    Optimize,
    DebugSymbols,
    Define,
};

const char* optionClassName(OptionClass cls);

// The retention matrix: is an option of this class kept at this level?
//
// This is the default. A rule may override it (see
// SwitchRule::keepAtLevels), and the override wins.
//
// The matrix is the second axis of the whole model, and it is written
// as one predicate per level rather than a class-by-class table so
// that it reads as a decision rather than as data:
//
//   AST  keeps: the triple, the sysroot, the preprocessor state, the
//               language, the include paths
//   BC   adds:  the CPU feature level and the debug info
//
// Both levels drop: the linker options, the dependency options, the
// warning levels, the optimisation level and the code-generation
// tuning.
//
// WHY THE TRIPLE IS AT BOTH LEVELS
//
// The obvious split is "the AST is about the source, the bitcode is
// about the machine" -- so the target would be a bitcode-only thing.
//
// **That is wrong**, and the measurement below is why.
//
//   Measured with clang 15.0.7 (the -ast-dump of the same source
//   under different targets, with the pointer addresses normalized):
//
//     target            __int128   __builtin_va_list   SVE types
//     ----------------  ---------  ------------------  ---------
//     x86_64-linux-gnu  yes        __va_list_tag[1]    no
//     i386-linux-gnu    NO         __va_list           no
//     aarch64-linux-gnu yes        __va_list_tag[1]    YES (41)
//     armv7-linux-gnu   NO         __va_list           no
//
//   So the target triple **changes the AST**: it decides which
//   builtin types exist. And it changes more than the shape -- it
//   changes whether the code compiles at all:
//
//     $ clang -target i386-linux-gnu -fsyntax-only t.c
//     error: __int128 is not supported on this target
//
//   Dropping the target from an AST command therefore does not
//   produce "the same AST"; it produces a *different* AST, or a
//   compile error.
//
// THE LINE THAT ACTUALLY MATTERS
//
// The real split is **triple vs CPU feature level**, not AST vs BC:
//
//   the triple    (--target=...)  decides the builtin types and the
//                                  ABI           -> needed at BOTH levels
//   the CPU level (-march/-mcpu)   decides the instruction set
//                                  -> AST is identical with or without it
//
//   Measured: the AST of the same source under -march=x86-64,
//   -march=x86-64-v2 and -march=x86-64-v3 is **identical**.
bool keptAtLevel(OptionClass cls, TranslateLevel level);

// The human-readable reason an option of this class is dropped at
// this level.
//
// Every dropped option carries a reason, because "silently dropped"
// is the one outcome a mapping engine must never produce.
std::string dropReason(OptionClass cls, TranslateLevel level);

// ═══════════════════════════════════════════════════════════════════════════
// The switch shape flags
// ═══════════════════════════════════════════════════════════════════════════

// How an option is spelled.
//
// These mirror the industry-standard vocabulary (the one the
// compiler-configuration tools use), because the shape is a property
// of the *compiler family*, not of any one tool.
enum OptionShape : unsigned int {
    kShapeNone = 0,

    // The option is spelled with a single dash ("-I").
    kShapeDash = 1u << 0,
    // The option is spelled with a double dash ("--sysroot").
    kShapeDashDash = 1u << 1,
    // The option may appear as "-Ifoo" (value attached).
    kShapeAttached = 1u << 2,
    // The option may appear as "-I foo" (value in the next argv).
    kShapeUnattached = 1u << 3,
    // The option may appear as "-I=foo" (value after '=').
    kShapeEqual = 1u << 4,
    // The value may be omitted entirely ("-O", "-O2").
    kShapeOptionalValue = 1u << 5,
    // The value is required.
    kShapeRequiredValue = 1u << 6,
    // The value is a filesystem path.
    kShapePath = 1u << 7,
    // The option is not a compile option: drop it (and any attached
    // value) without complaint.
    kShapeSkip = 1u << 8,
    // The option is a prefix that selects a *sub-table* of options
    // ("-Wl," -> the linker table).
    kShapePrefix = 1u << 9,
    // The value is a comma-separated list ("-Wl,-a,-b").
    kShapeList = 1u << 10,
    // The option may carry a "next argument" that must be consumed
    // with it ("-o" "file.o").
    kShapeTakesNext = 1u << 11,

    // ── The extended vocabulary ────────────────────────────────────
    //
    // The flags below are the ones the established attribute set has
    // that the original twelve did not cover, and that a real
    // toolchain integration actually needs. They are here because
    // the *shape* of an option is a property of the compiler family,
    // and a family that cannot express "this option dispatches to a
    // sub-table" cannot be described by the table at all.
    //
    // See docs/*/COMPILER-CONFIG-GAP-ANALYSIS-*.md for the full
    // comparison.

    // The option selects a **sub-table**: the value is translated by
    // a different table.
    //
    // The canonical case is gcc's "-Wl,-a,-b": the "-Wl," prefix is
    // split on commas and each piece is translated by the *linker*
    // table, not the compiler table.
    //
    // Without this flag we either drop "-Wl," wholesale (losing real
    // information) or pass it to clang (which rejects it).
    kShapeAlternateTable = 1u << 12,

    // The option may be written as an **unambiguous prefix**.
    //
    // "--sysstd" matches "--sysroot" only when no other option starts
    // with "--sysstd". This is the `oa_abbrev_match` attribute.
    //
    // IMPLEMENTED as a second pass in the rule lookup: the ordinary
    // exact/prefix match runs first and always wins, and the
    // abbreviation pass only runs when it found nothing. An
    // abbreviation that more than one rule could satisfy is
    // **rejected** (the option is reported as unknown) rather than
    // resolved by guessing -- the token alone cannot tell the
    // candidates apart.
    //
    // The token is the **complete** option name in its abbreviated
    // form, so the value comes from an '=' or from the next argv
    // entry, never from the tail of the token.
    kShapeAbbrevMatch = 1u << 13,

    // '_' and '-' are interchangeable in the option name.
    //
    // "--foo-bar" and "--foo_bar" are the same option. This is
    // the `oa_hyphen_is_underscore` attribute.
    //
    // The comparison is done without rewriting the token, so the
    // attached value is still cut off at the right offset.
    //
    // This is **not** the default, because the option sets that need
    // it are the exception: gcc has "--sysroot" and no "--sys_root",
    // and treating the two as the same would let one spelling
    // silently stand in for the other.
    kShapeHyphenIsUnderscore = 1u << 14,

    // The value is introduced by a colon (MSVC's "/std:c++17",
    // clang-cl's "/clang:--target=...").
    kShapeColon = 1u << 15,

    // The value may be wrapped in parentheses ("--foo(bar)").
    kShapeParens = 1u << 16,

    // The value may be quoted, and the outer quote pair is removed
    // when the value is used.
    kShapeStripQuotes = 1u << 17,

    // The value is a **list** separated by commas, and each element is
    // translated separately.
    //
    // This is what makes "-Wl,-a,-b" work: the list is split, and
    // each piece goes through the sub-table.
    kShapeSplit = 1u << 18,

    // The option is **known but unsupported**: the compiler accepts
    // it, but the translation cannot honour it.
    //
    // This is different from "no equivalent": "unsupported" means
    // "we could translate this if we had the feature", while "no
    // equivalent" means "clang has no such concept". The
    // `oa_unsupported` attribute errors out by default.
    kShapeUnsupported = 1u << 19,

    // The option is **required**: a command line that contains it
    // cannot be translated by the generic path and needs a
    // specialised configuration.
    //
    // This is the `oa_required` attribute.
    kShapeRequired = 1u << 20,
};

// ═══════════════════════════════════════════════════════════════════════════
// One switch rule
// ═══════════════════════════════════════════════════════════════════════════

// The rule for one option.
//
// A rule is matched by **name** (without the leading dash). The shape
// flags then decide how the value is extracted.
struct SwitchRule {
    // The option name, without the leading dash(es).
    // "I", "std=", "sysroot", "include", "Fo" ...
    //
    // A trailing '=' in the name means "the value is always given
    // after an '='" (the "=" is part of the name, so "-std=c++17" is
    // matched by the name "std=").
    std::string name;

    // Match the name case-insensitively.
    //
    // False (the default) for the unix-style options, because gcc's
    // "-o" (the output file) and "-O2" (the optimisation level) are
    // **different options** that differ only in case.
    //
    // True for the MSVC options, whose spelling is conventionally
    // uppercase and which a user may write in either case on Windows.
    bool caseInsensitive = false;

    // The shape flags (see OptionShape).
    unsigned int shape = kShapeNone;

    // What the option means.
    OptionClass cls = OptionClass::Unknown;

    // The clang spelling of this option, when it is not the same.
    //
    // Empty means "keep the original spelling" -- which is the common
    // case for gcc/clang options, because clang *is* a gcc-compatible
    // driver for most of them.
    //
    // The value is used as a template: "%s" is replaced by the value.
    // So the MSVC rule for "/I" is {"I", ..., OptionClass::Include,
    // "-I%s"} and the rule for "/std:c++17" is {"std:", ...,
    // OptionClass::LanguageStd, "-std=%s"}.
    //
    // A value that does not contain "%s" is a fixed replacement (the
    // value is dropped): the MSVC rule for "/Zi" is {"Zi", ...,
    // OptionClass::DebugInfo, "-g"}.
    std::string clangForm;

    // The level at which the option is dropped, when it must not be
    // carried over.
    //
    // This is the escape hatch for the options that are gcc-specific
    // but harmless: "-mavx2" is a codegen option that clang accepts
    // only if the target supports it, so it is dropped at both levels
    // but *recorded* as such.
    //
    // Empty means "no explicit drop; use the class default".
    std::string dropAt;   // "" | "ast" | "bc" | "both"

    // Why this option exists -- a human-readable note.
    //
    // Used in the report and in the generated documentation, so the
    // table stays reviewable.
    std::string note;

    // The version range this rule applies to.
    //
    // Empty means "every version". This is the escape hatch for the
    // cases that need a per-version translator: the rule says "from
    // version 4.7, the option is spelled this way".
    //
    // The comparison is component-wise, so "10.0" > "9.0".
    //
    // See ccap_compdb_registry.h for VersionRange.
    std::string versionMin;
    std::string versionMax;

    // The retention levels this rule is kept at, when the user has
    // overridden the default matrix for this rule.
    //
    // Only meaningful when `hasLevelOverride` is true. The list names
    // the levels at which the option is **kept**; an empty list means
    // "dropped at every level".
    //
    // The flag exists because an empty list and "no override" are two
    // different things: the first is a decision, the second is the
    // absence of one. Without the flag, "drop -W4 everywhere" would be
    // indistinguishable from "use the class default".
    //
    // This is what makes the retention matrix user-editable per rule,
    // which is the axis that matters in practice: "keep -W4 at the AST
    // level for this project" is a one-line configuration, and it does
    // not require editing a table.
    bool hasLevelOverride = false;
    std::vector<std::string> keepAtLevels;

    // Does this rule apply to the given compiler version?
    bool appliesToVersion(const std::string& version) const;

    // Is this rule kept at the given level?
    //
    // The user override wins over the class default when it is set.
    bool keptAt(TranslateLevel level, OptionClass cls) const;

    // The sub-table this rule dispatches to.
    //
    // Only meaningful when the rule carries kShapeAlternateTable.
    //
    // The name refers to one of the profile's sub-tables ("linker").
    // The value of a kShapeSplit + kShapeAlternateTable rule is split
    // on commas, and each piece is translated by that sub-table.
    std::string alternateTable;
};

// ═══════════════════════════════════════════════════════════════════════════
// The compiler profile
// ═══════════════════════════════════════════════════════════════════════════

// The complete description of one compiler family.
//
// A profile is the pairing of:
//   * an identity (which executables are this family)
//   * a switch table (how this family spells its options)
//   * a set of "family level" facts (the driver name, the C/C++
//     distinction, the env vars that feed includes)
struct CompilerProfile {
    // The identifier ("gcc", "msvc", "clang", "my-cross-cc").
    std::string id;

    // Which compiler kind this profile belongs to.
    //
    // A profile is always an instance of a kind. "arm-none-eabi-gcc"
    // is the Gcc kind with a different profile: the option syntax is
    // the same, but the target and the sysroot are not.
    CompilerKind kind = CompilerKind::Unknown;

    // The executable names that identify this family.
    std::vector<std::string> exactNames;

    // Name prefixes that identify this family ("arm-none-eabi-").
    std::vector<std::string> prefixRules;

    // The driver to use when this family is the source.
    //
    // "clang" / "clang++" for a native family.
    // Empty means "decide from the source extension".
    std::string driver;

    // The C++ driver, when it differs from the C driver.
    std::string cppDriver;

    // The environment variables that contribute include paths.
    //
    // gcc:   CPATH, C_INCLUDE_PATH, CPLUS_INCLUDE_PATH
    // MSVC:  INCLUDE, LIB
    //
    // The distinction that matters: gcc's CPATH is a **user** include
    // path (like -I), while MSVC's INCLUDE is a **system** include
    // path (like /external:I). The order of the vector decides which
    // one it is: the first entry is a user path, the rest are system
    // paths.
    std::vector<std::string> includeEnvVars;

    // The switch table.
    std::vector<SwitchRule> rules;

    // The options that are known to have no clang equivalent, so the
    // report can say so explicitly instead of silently dropping them.
    std::vector<std::string> noEquivalent;

    // True when the profile was matched by name and the user did not
    // have to configure anything.
    bool builtin = false;

    // ── The sub-tables ───────────────────────────────────────────────
    //
    // A sub-table is a named set of rules that a rule can dispatch to.
    //
    // The canonical case is gcc's "-Wl,-a,-b": the "-Wl," prefix is
    // split on commas and each piece is translated by the **linker**
    // table, not by the compiler table.
    //
    // Without this, the option is either dropped wholesale (losing
    // real information) or passed to clang (which rejects it).
    //
    // The key is the sub-table's name ("linker"); the value is its
    // rules. The rule that dispatches to it carries the
    // kShapeAlternateTable flag and names the table in its
    // `alternateTable` field.
    std::map<std::string, std::vector<SwitchRule>> subTables;
};

// ═══════════════════════════════════════════════════════════════════════════
// The version predicate
// ═══════════════════════════════════════════════════════════════════════════

// A version range, as a predicate on a compiler version string.
//
// The version string is the conventional "a.b.c" form. The comparison
// is **component-wise**, not lexicographic, because "10.0" < "9.0"
// lexicographically and "10.0" > "9.0" numerically -- and gcc 10
// exists.
//
// An empty bound means "unbounded on that side".
struct VersionRange {
    // The lowest version this applies to (inclusive). Empty = no lower
    // bound.
    std::string minVersion;

    // The highest version this applies to (inclusive). Empty = no upper
    // bound.
    std::string maxVersion;

    // True when the range constrains anything at all.
    bool empty() const { return minVersion.empty() && maxVersion.empty(); }

    // Does the given version fall in this range?
    //
    // A version that cannot be parsed matches **nothing** unless the
    // range is empty. That is deliberate: a rule scoped to "gcc >= 4.7"
    // must not fire on a compiler whose version we failed to detect,
    // because firing it would apply a rule whose precondition we
    // could not verify.
    bool matches(const std::string& version) const;
};

// Parse a version string into its numeric components.
//
// Returns false when the string is not a version ("unknown", "1.2.3rc1"
// is accepted as 1.2.3 because the trailing junk is ignored).
bool parseVersion(const std::string& version, std::vector<unsigned>* out);

// Compare two versions component-wise.
//
// Returns <0, 0 or >0. A version with fewer components is treated as
// having zeros in the missing positions ("4.7" == "4.7.0").
int compareVersions(const std::string& a, const std::string& b);

// ═══════════════════════════════════════════════════════════════════════════
// The version detection
// ═══════════════════════════════════════════════════════════════════════════

// How to find out the version of a compiler.
//
// An `ident_switch` plus an ordered `version_regex` list. The
// **order matters**: the established rule says "Order Version Regex
// least likely to incorrectly match first", because a greedy regex
// would swallow the version of a *different* compiler family.
struct VersionProbe {
    // The switch that makes the compiler print its version
    // ("--version", "-v", "--vsn").
    std::string switchText;

    // The ordered list of patterns.
    //
    // Each pattern is either:
    //
    //   "replace/<regex>/<replacement>"  -- the replace form. The
    //        regex is applied to the whole probe output; the
    //        replacement is what the version becomes. "$1" refers to a
    //        capture group.
    //
    //   "<regex>"  -- the plain form. The first capture group is the
    //        version. If there is no capture group, the whole match is
    //        the version.
    //
    // The **first** pattern that matches wins. Later patterns are not
    // tried.
    std::vector<std::string> patterns;

    // True when this probe is usable.
    //
    // Only the **patterns** matter. The switch text is how the version
    // would be *obtained*, and it may legitimately be empty: the MSVC
    // compiler has no "--version" switch (its banner comes from
    // "cl" with no arguments, and the version is printed on stderr),
    // so the probe's patterns are applied to a banner the caller
    // supplies.
    //
    // An empty pattern list, on the other hand, means there is nothing
    // to apply.
    bool empty() const { return patterns.empty(); }
};

// Apply a probe's patterns to the probe output and extract the version.
//
// Returns an empty string when nothing matched.
std::string extractVersion(const VersionProbe& probe, const std::string& output);

// ═══════════════════════════════════════════════════════════════════════════
// The compiler match patterns
// ═══════════════════════════════════════════════════════════════════════════

// How a compiler executable name is matched to a variant.
//
// Two forms:
//
//   "gcc-*"        -- glob. '*' matches any run of characters, and it
//                     may appear anywhere ("*-gcc" matches
//                     "arm-none-eabi-gcc").
//
//   "replace/gcc/g++"
//                  -- redirect. The executable is *called* gcc but
//                     actually *is* g++ (the classic "g++ is gcc with
//                     a different driver" case). The part before the
//                     slash is the name as it appears, the part after
//                     is the compiler it really is.
struct CompilerMatch {
    // The pattern.
    std::string pattern;

    // The variant this pattern selects.
    std::string variantId;

    // True when this is a redirect rather than a glob.
    //
    // A redirect takes priority over a plain glob for the same
    // executable name: it is a more specific statement about what the
    // executable is.
    bool isRedirect = false;

    // Parse a "replace/a/b" pattern.
    //
    // Returns false when the pattern is not a redirect.
    static bool parseRedirect(const std::string& pattern,
                              std::string* from,
                              std::string* to);
};

// Match a compiler base name against a glob pattern.
//
// '*' matches any run of characters (including none). '?' matches a
// single character. The comparison is case-insensitive, because
// Windows file names are case-insensitive and a user may write
// "GCC-ARM-NONE-EABI".
bool globMatch(const std::string& pattern, const std::string& text);

// ═══════════════════════════════════════════════════════════════════════════
// The variants
// ═══════════════════════════════════════════════════════════════════════════

// One variant of a compiler family.
//
// A "variant" is one compiler type in the family: gcc, g++, g++cc,
// msvc_cxx, clangcxx. The variants of one family form a **chain**:
// gcc -> g++ -> g++cc, because the C++ driver of a family is a
// variant of the same family, not a separate family.
struct CompilerVariant {
    // The variant id ("gcc", "g++", "g++cc").
    std::string id;

    // The language this variant compiles.
    //
    // "C", "C++", "ObjC", "ObjC++".
    std::string language;

    // The id of the next variant in the chain ("" = none).
    std::string nextVariantId;

    // True for the head of the chain (the family's root variant).
    bool familyHead = false;

    // The rules that apply **only** to this variant.
    std::vector<SwitchRule> rules;

    // The id of the variant whose rules this variant inherits.
    //
    // When empty, the variant inherits from the family base. The
    // derived-from attribute expresses the same thing (hexagon,
    // keilarm, xtensa_clang use it).
    std::string derivedFrom;
};

// ═══════════════════════════════════════════════════════════════════════════
// The rule group
// ═══════════════════════════════════════════════════════════════════════════

// A group of rules that applies to a **subset** of the variants.
//
// This is the `<option_group><applies_to>` attribute. It exists
// because
// the same option name means different things in different members of
// a family, and because a family's "common" options are not always
// common.
//
// The scope is a list of "family:variant" selectors:
//
//   "gnu:gcc"      the gcc variant of the gnu family
//   "gnu:*"        every variant of the gnu family
//   "*"            every variant of every family
//   "gcc"          a bare variant id, matched against any family
struct RuleGroup {
    // The selectors this group applies to.
    std::vector<std::string> appliesTo;

    // The rules in the group.
    std::vector<SwitchRule> rules;

    // Does this group apply to the given family/variant?
    bool appliesToVariant(const std::string& familyId,
                          const std::string& variantId) const;
};

// ═══════════════════════════════════════════════════════════════════════════
// The user override
// ═══════════════════════════════════════════════════════════════════════════

// What the user wants to change about one rule.
struct RuleOverride {
    // The rule name this override applies to.
    std::string name;

    // Set to true to remove the rule entirely.
    bool disable = false;

    // When set, the rule's clang spelling is replaced.
    //
    // "%s" is the value placeholder. A form without "%s" is a fixed
    // replacement.
    bool hasClangForm = false;
    std::string clangForm;

    // When set, the rule's class is replaced.
    bool hasClass = false;
    OptionClass cls = OptionClass::Unknown;

    // When set, the rule's retention levels are overridden.
    //
    // The list names the levels at which the option is **kept**:
    // "ast", "bc". An empty list means "dropped at every level".
    bool hasLevelOverride = false;
    std::vector<TranslateLevel> keepAt;

    // When set, the rule's shape flags are replaced.
    bool hasShape = false;
    unsigned int shape = kShapeNone;
};

// An override scoped to a profile (and optionally a variant).
struct ProfileOverride {
    // The profile id ("gnu", "msvc"). Empty = every profile.
    std::string profile;

    // The variant id ("gcc"). Empty = every variant.
    std::string variant;

    // The rules to remove.
    std::vector<std::string> disable;

    // The rules to replace or add.
    std::vector<RuleOverride> overrides;

    // The rules to add that do not exist in any table.
    std::vector<SwitchRule> add;
};

// ═══════════════════════════════════════════════════════════════════════════
// The unknown-option policy
// ═══════════════════════════════════════════════════════════════════════════

// What to do with an option the switch table does not know.
//
// The default is `Report`: drop it from the generated command (an
// unknown option would make clang fail to start) but record it in the
// audit report, because an unknown option may well be one that changes
// the meaning of the translation unit.
enum class UnknownPolicy {
    // Drop it, but report it. (The default.)
    Report = 0,
    // Keep it unchanged and pass it through to clang.
    Keep,
    // Drop it silently. (Discouraged: it is the one mode that can
    // produce a wrong artifact without telling anybody.)
    Drop,
    // Treat it as an error: the mapping fails.
    Fail,
};

const char* unknownPolicyName(UnknownPolicy policy);
bool unknownPolicyFromName(const std::string& name, UnknownPolicy* out);

// ═══════════════════════════════════════════════════════════════════════════
// The audit bucket
// ═══════════════════════════════════════════════════════════════════════════

// The fate of one option.
//
// Every option lands in exactly one of these. The point of having six
// buckets instead of "kept / dropped" is that the three kinds of
// *dropped* need different responses from the reader:
//
//   dropped_no_equivalent -- "clang has no such concept". Fix it by
//                             understanding the compiler. There is no
//                             config change that helps.
//   dropped_by_level      -- "not needed for this consumer". Fine.
//   dropped_by_policy     -- "you asked for this". Fine, and it is
//                             recorded so that a surprising result can
//                             be traced to a config file.
//
// and the two kinds of *kept* need different treatment:
//
//   kept                 -- carried through unchanged.
//   mapped               -- carried through, but rewritten. This one
//                           deserves attention, because a rewrite is
//                           where a semantic drift would hide.
enum class AuditBucket {
    Kept = 0,
    Mapped,
    DroppedNoEquivalent,
    DroppedByLevel,
    DroppedByPolicy,
    Unknown,
};

const char* auditBucketName(AuditBucket bucket);

// ═══════════════════════════════════════════════════════════════════════════
// The compiler family
// ═══════════════════════════════════════════════════════════════════════════

// A compiler family: one profile plus a chain of variants.
//
// WHY A FAMILY AND NOT JUST A PROFILE
//
// A "profile" answers "how does this compiler spell its options". A
// "family" additionally answers:
//
//   - which **variants** (gcc / g++ / g++cc) belong to it, and how
//     they chain;
//   - which variants have their **own** rules;
//   - how an **executable name** maps onto a variant.
//
// The family-head attribute marks the root of
// the chain, `comp_next_type` links one variant to the next, and
// `extra_comp` maps executable names onto variants.
//
// The variant chain matters for a practical reason: gcc and g++ have
// *almost* the same options, but not the same. g++ compiles C++ by
// default; gcc does not. A toolchain that flattens them into one
// table either loses that or has to special-case it by hand.
struct CompilerFamily {
    // The family id ("gnu", "msvc", "clang", "iar").
    std::string id;

    // The profile that holds the family's base rules.
    CompilerProfile profile;

    // The variants, in chain order.
    std::vector<CompilerVariant> variants;

    // The rule groups: rules that apply to a subset of the variants.
    std::vector<RuleGroup> groups;

    // How to detect this compiler's version.
    VersionProbe versionProbe;

    // The compiler kind of the family.
    CompilerKind kind = CompilerKind::Unknown;

    // The variant with this id (nullptr when absent).
    const CompilerVariant* findVariant(const std::string& variantId) const;

    // The variant that follows the given one in the chain.
    const CompilerVariant* nextVariant(const std::string& variantId) const;
};

// ═══════════════════════════════════════════════════════════════════════════
// The registry
// ═══════════════════════════════════════════════════════════════════════════

// The compiler registry: the families, and the rules for resolving one
// executable name to a profile + variant.
class CompilerRegistry {
public:
    CompilerRegistry();

    // The built-in registry: the gnu, msvc, clang and generic families.
    static CompilerRegistry builtin();

    // Add a family. A family with an id that already exists is
    // **replaced** (not merged): the user's family is the whole
    // family, because the stock rules underneath would otherwise still
    // win on the longer option names.
    void upsertFamily(const CompilerFamily& family);

    // The family with this id (nullptr when absent).
    const CompilerFamily* findFamily(const std::string& id) const;

    // The families, in registration order.
    const std::vector<CompilerFamily>& families() const { return families_; }

    // The families, writable.
    //
    // This is for the in-place edits that are not a replacement: adding
    // a variant to an existing family, adding a rule group, attaching a
    // version probe. The replacement (a whole new family under the same
    // id) goes through upsertFamily().
    std::vector<CompilerFamily>& familiesRef() { return families_; }

    // Add a match pattern.
    void addMatch(const CompilerMatch& match);

    // The matches, in registration order.
    const std::vector<CompilerMatch>& matches() const { return matches_; }

    // The user overrides, in registration order.
    void addOverride(const ProfileOverride& override);
    const std::vector<ProfileOverride>& overrides() const { return overrides_; }

private:
    std::vector<CompilerFamily> families_;
    std::vector<CompilerMatch> matches_;
    std::vector<ProfileOverride> overrides_;
};

// ═══════════════════════════════════════════════════════════════════════════
// The resolution result
// ═══════════════════════════════════════════════════════════════════════════

// The outcome of resolving one compiler executable name.
struct CompilerResolution {
    // The family id ("gnu").
    std::string familyId;

    // The variant id ("gcc").
    std::string variantId;

    // The profile that was selected (the family's profile, possibly
    // overridden by the user).
    const CompilerProfile* profile = nullptr;

    // The effective rule set, already merged with the inheritance
    // chain, the groups and the user overrides.
    std::vector<SwitchRule> rules;

    // The version of the compiler, when it was detected.
    std::string version;

    // How the variant was chosen:
    //   "redirect"     -- a replace/ pattern matched
    //   "glob"         -- a glob pattern matched
    //   "exact"        -- an exact name matched
    //   "explicit"     -- the user's explicit compiler mapping
    //   "default"      -- the user's default profile
    //   "generic"      -- the generic fallback
    std::string matchedBy;

    // The pattern that matched (empty for exact/default/generic).
    std::string matchedPattern;
};

// Resolve a compiler executable name to a family, a variant and the
// effective rule set.
//
// The resolution order is documented in resolution.cpp and is:
//
//   1. the explicit user mapping
//   2. a user redirect
//   3. an exact family name
//   4. a glob match (longest pattern wins)
//   5. a version-suffixed name
//   6. the default profile
//   7. the generic profile
//
// `explicitProfileId` is the already-resolved user mapping (the
// caller resolves the exact-vs-glob question, because it owns the
// user's mapping list). `defaultProfileId` is the user's default.
//
// The result never fails: the generic profile is the last resort.
CompilerResolution resolveCompiler(const CompilerRegistry& registry,
                                   const std::string& compilerPath,
                                   const std::string& version,
                                   const std::string& explicitProfileId,
                                   const std::string& defaultProfileId,
                                   const CompilerProfile& genericProfile);

// The forward declaration of the compile command.
//
// The mapping engine needs the CompileCommand, but the compile command
// is defined in ccap_compdb.h, which includes this header. So the
// mapping functions take the command by reference to an incomplete type
// here, and the definition lives in the .cpp (where the complete type is
// visible).
struct CompileCommand;

// The forward declaration of the configuration.
//
// Same reason as CompileCommand: the configuration is defined in
// ccap_compdb.h, which includes this header.
struct CompdbConfig;

}  // namespace compdb
}  // namespace ccap

namespace ccap {
namespace compdb {

// ═══════════════════════════════════════════════════════════════════════════
// The mapping engine
// ═══════════════════════════════════════════════════════════════════════════

// What the engine decided about one option.
struct OptionDecision {
    // The original token.
    std::string original;

    // What the option means.
    OptionClass cls = OptionClass::Unknown;

    // The clang spelling that was emitted (empty when dropped).
    std::string emitted;

    // Why it was dropped (empty when kept).
    std::string reason;

    // True when the option was kept.
    bool kept = false;

    // Which audit bucket this decision landed in.
    //
    // The bucket is the *classification* of the decision; the `kept`
    // flag is the *action*. They are not the same thing: an option in
    // the Unknown bucket may have been kept (the "keep unknown
    // options" policy passes it through) or dropped (the default
    // policy drops it but reports it).
    AuditBucket bucket = AuditBucket::Unknown;
};

// The result of mapping one compile command.
struct MappingReport {
    // The decisions, in the order the options appeared.
    std::vector<OptionDecision> decisions;

    // The options that were dropped because they have no clang
    // equivalent at all (not merely "not needed at this level").
    std::vector<std::string> noEquivalent;

    // The options that were dropped because they are not needed at
    // this level. The reason says which level.
    std::vector<std::string> droppedByLevel;

    // The options the table did not know about.
    //
    // This list matters: an unknown option is **not** silently
    // dropped, it is reported, because an unknown option may well be
    // one that changes the meaning of the translation unit.
    std::vector<std::string> unknown;

    // ── The six audit buckets ───────────────────────────────────────
    //
    // Every option lands in exactly one of these. The three kinds of
    // *dropped* need different responses from the reader, so they are
    // kept apart:
    //
    //   dropped_no_equivalent -- "clang has no such concept". No
    //                             config change helps; the compiler has
    //                             to be understood.
    //   dropped_by_level      -- "not needed for this consumer". Fine.
    //   dropped_by_policy     -- "you asked for this". Fine, and it is
    //                             recorded so a surprising result can
    //                             be traced to a config file.
    //
    // and the two kinds of *kept*:
    //
    //   kept                 -- carried through unchanged.
    //   mapped               -- carried through, but rewritten. This one
    //                           deserves attention, because a rewrite
    //                           is where a semantic drift would hide.
    //
    // The sixth is unknown: the table did not know the option at all.
    std::vector<std::string> kept;
    std::vector<std::string> mapped;
    std::vector<std::string> droppedNoEquivalent;
    std::vector<std::string> droppedByLevelBucket;
    std::vector<std::string> droppedByPolicy;
    std::vector<std::string> unknownBucket;

    // The count per bucket, for a summary line.
    //
    // The index is the AuditBucket value. A vector rather than a map
    // because the bucket set is fixed and small, and a vector keeps
    // the audit output allocation-free.
    std::vector<size_t> bucketCounts(size_t bucketCount) const;

    // True when the mapping **failed** -- which only happens under the
    // "fail" unknown-option policy.
    //
    // The command line is still produced (it is the best effort that
    // was possible), but the caller is expected to stop rather than
    // consume a command that is known to be wrong.
    bool failed = false;

    // Why it failed (empty when it did not).
    std::string failureReason;

    // The family-aware resolution this report was produced with.
    //
    // Empty when the report was produced without a registry.
    std::string familyId;
    std::string variantId;
    std::string matchedBy;
    std::string matchedPattern;
    std::string compilerVersion;

    // A one-line summary per decision, for the human-readable report.
    std::string toText() const;
};

// The mapping engine.
//
// It owns the switch table and applies it to a parsed compile command.
class OptionMapper {
public:
    OptionMapper();

    // The built-in table: gcc, clang, msvc, iar, generic.
    static OptionMapper builtin();

    // Add a profile. Later profiles win over earlier ones for the
    // same option name, so a user profile can override a built-in one.
    void addProfile(const CompilerProfile& profile);

    // The profile that matches this compiler base name.
    //
    // Returns nullptr when nothing matches -- the caller then falls
    // back to the generic profile, or to the user's explicit mapping.
    const CompilerProfile* findProfile(const std::string& compilerBase) const;

    // The profile for this compiler base name, or the generic one.
    //
    // The generic profile is the answer to "an unknown compiler, but
    // treat it as the gcc family": it recognizes the **common** option
    // spelling (the -I / -D / -c style that practically every unix
    // compiler uses) and nothing else. Anything beyond that is
    // reported as unknown rather than guessed at.
    const CompilerProfile& profileFor(const std::string& compilerBase) const;

    // The generic profile (the "treat it as gcc" fallback).
    static const CompilerProfile& genericProfile();

    // Map one compile command into a clang command line.
    //
    // `level` decides what is kept.
    // The report explains every decision.
    std::vector<std::string> map(const CompileCommand& cmd,
                                  TranslateLevel level,
                                  MappingReport* report) const;

    // Classify one option token.
    //
    // Public because the CLI's "show" subcommand wants to explain a
    // single option without a full command.
    OptionClass classify(const std::string& token,
                          const CompilerProfile& profile) const;

    // ── The explicit unknown-compiler mapping ────────────────────────
    //
    // This is the "the user says: treat this compiler as the gcc
    // family" case.
    //
    // The mapping is keyed by the compiler's base name. A key that ends
    // with '*' is a **prefix** match: "arm-*" matches "arm-none-eabi-gcc".
    //
    // The lookup order (the first hit wins):
    //   1. an exact base-name match
    //   2. the longest prefix match
    //   3. the default profile
    //
    // The value is the id of the profile to use. The special value
    // "generic" means the conservative generic table.
    void setCompilerProfile(const std::string& namePattern,
                            const std::string& profileId);
    void setDefaultProfile(const std::string& profileId);

    // The profile id for a compiler base name, after the **explicit**
    // mapping. Empty when nothing matched explicitly.
    //
    // The default profile is not included here: it is the last resort
    // in profileFor(), after the name matching. Use profileFor() to
    // get the profile that will actually be used.
    std::string mappedProfileId(const std::string& compilerBase) const;

    // The default profile id (empty when none was set).
    const std::string& defaultProfile() const { return defaultProfile_; }

    // The profiles, in the order they are searched.
    const std::vector<CompilerProfile>& profiles() const { return profiles_; }

    // Replace the profile with this id, or add it when it is not
    // there.
    //
    // This is what "the user overrides the gcc table" means: the
    // profile is **replaced**, not merged, because the stock rules
    // underneath would otherwise still win on the longer names.
    void upsertProfile(const CompilerProfile& profile);

    // ═══════════════════════════════════════════════════════════════════
    // The registry
    // ═══════════════════════════════════════════════════════════════════
    //
    // The registry is the **family-aware** layer: it knows that "gcc",
    // "g++" and "g++cc" are variants of one family, that "arm-none-eabi-gcc"
    // is a cross compiler of that family, and that a rule may apply only
    // from a given compiler version on.
    //
    // The mapper owns it, so a mapper carries its whole world with it --
    // no external registry to keep in sync, no lifetime surprises.

    // The registry (read-only).
    const CompilerRegistry& registry() const { return registry_; }

    // The registry (writable): add families, matches and overrides.
    CompilerRegistry& registry() { return registry_; }

    // Replace the whole registry.
    void setRegistry(const CompilerRegistry& registry) { registry_ = registry; }

    // The resolution for one compiler, at one version.
    //
    // This is the **full** answer: the family, the variant, the merged
    // rule set and how the match was made. profileFor() is the
    // convenient shortcut that throws away everything but the profile.
    //
    // The version is the compiler's version ("11.4.0"). An empty version
    // means "not known", and the version-scoped rules then do not fire --
    // a rule whose precondition could not be verified must not be
    // applied.
    CompilerResolution resolve(const std::string& compilerBase,
                               const std::string& version) const;

    // The version used for the version-scoped rules.
    //
    // The mapper never runs the compiler by itself (see
    // ccap_compdb_registry.h for why), so the version comes from the
    // caller: a build log that recorded it, a configuration the user
    // wrote, or the command line.
    void setCompilerVersion(const std::string& version) {
        compilerVersion_ = version;
    }
    const std::string& compilerVersion() const { return compilerVersion_; }

    // ── The unknown-option policy ────────────────────────────────────
    //
    // What to do with an option the switch table does not know.
    //
    // The default is "report": drop it from the generated command (clang
    // would refuse to start) but record it in the audit report, because
    // an unknown option may well be one that changes the meaning of the
    // translation unit.
    void setUnknownPolicy(UnknownPolicy policy) { unknownPolicy_ = policy; }
    UnknownPolicy unknownPolicy() const { return unknownPolicy_; }

private:
    std::vector<CompilerProfile> profiles_;

    // The explicit mappings: pattern -> profile id.
    //
    // The vector (rather than a map) is because the prefix matches
    // have to be tried **longest first**, and a map would not preserve
    // the insertion order.
    std::vector<std::pair<std::string, std::string>> compilerProfiles_;

    std::string defaultProfile_;

    // The family-aware layer.
    CompilerRegistry registry_;

    // The compiler version for the version-scoped rules.
    std::string compilerVersion_;

    // What to do with an unknown option.
    UnknownPolicy unknownPolicy_ = UnknownPolicy::Report;
};

// ═══════════════════════════════════════════════════════════════════════════
// Building the mapper from a configuration
// ═══════════════════════════════════════════════════════════════════════════

// Build the mapper for a configuration.
//
// The built-in profiles are the base; the user's profiles are added on
// top. A user profile with the same id as a built-in one **replaces**
// it, because that is the intent of "override the gcc table".
//
// The explicit compiler->profile mapping is applied on top of that.
OptionMapper buildMapper(const CompdbConfig& config);

}  // namespace compdb
}  // namespace ccap
