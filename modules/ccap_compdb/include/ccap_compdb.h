// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - compile command recognition and parsing
//
// Responsibilities (**only** compile commands, not where they come from):
//   1. Recognize which commands are compile commands (the full compiler
//      table + symlink aliases mutually recognized)
//   2. Parse compile commands (from runtime capture / text files / a
//      build log / an existing compile_commands.json)
//   3. Filter out scripts, linkers and other non-compile commands
//   4. Extract structured information: header files, macros, source files
//   5. Export a clang compilation database
//   6. Dispatch to the per-compiler-type adapters for argument translation
//
// Key design: **no dependency on ccap_capture**.
// It only consumes "a list of command lines + a working directory",
// so it can consume compile commands produced by CMake / ninja
// without having to run the capture itself.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ccap_compdb_map.h"

namespace ccap {
namespace compdb {

// ═══════════════════════════════════════════════════════════════════════════
// Compiler kinds
// ═══════════════════════════════════════════════════════════════════════════
//
// The CompilerKind enum lives in ccap_compdb_map.h (which is included
// above), because the compiler profile -- the switch table -- has to
// refer to it, and that header is included first.

const char* compilerKindName(CompilerKind kind);
bool compilerKindFromName(const std::string& name, CompilerKind* out);

// One compiler rule
//
// Matching logic (in order):
//   1. exact match in exact_names (case-sensitive -- exact matching is
//      what the classic compiler-configuration tools do)
//   2. prefix match in prefix_rules (for cross compilers, e.g.
//      "arm-none-eabi-")
//   3. suffix match in suffix_rules (for version suffixes, e.g. "-11")
//
// Why prefix rules are needed:
//   the **base name** of the cross compiler `arm-none-eabi-g++` is g++,
//   but it is a **different compiler** from the local g++ and the two must
//   not be conflated.
//   Prefix rules group a whole toolchain family together, while each
//   concrete path is still configured independently.
//
// ── The path-shape rules ───────────────────────────────────────────────
//
// The three rules above all match on the **base name**. That is enough
// for a compiler whose name identifies it ("gcc", "cl", "javac"), but it
// is not enough for Go's toolchain:
//
//     <GOROOT>/pkg/tool/<goos>_<goarch>/compile
//     <GOROOT>/pkg/tool/<goos>_<goarch>/link
//
// WHY THE NAME IS NOT ENOUGH
//
//   "compile" and "link" are ordinary program names. There are build
//   tools called "compile" and "link" that have nothing to do with Go,
//   and a name-only rule would hand them to the Go parser -- where the
//   option syntax means something completely different. That is not a
//   cosmetic mistake: the command would be read as a Go compile, its
//   arguments would be interpreted as Go options, and the result would
//   be a wrong record of the build.
//
//   The Go tool binaries, on the other hand, are always in one place:
//   the "pkg/tool/<goos>_<goarch>" directory of the toolchain. That
//   directory shape IS the identity, so it is what the rule matches.
//
// A path shape is a slash-separated pattern:
//
//     pkg/tool/?*_*?/compile
//
//   * the last segment is the executable name;
//   * the segments before it are directory segments that must appear,
//     **consecutively**, somewhere in the path;
//   * inside a segment, '*' matches any run of characters and '?'
//     matches exactly one -- a segment is the unit, so a pattern can
//     never match across a directory boundary.
//
// The shape above says: the file is named "compile", and it sits in a
// "pkg/tool" directory whose last subdirectory is named
// "<something>_<something>".
struct CompilerRule {
    CompilerKind kind = CompilerKind::Unknown;
    std::string id;                       // rule identifier (referenced by it in the config)
    std::vector<std::string> exactNames; // exact executable names
    std::vector<std::string> prefixRules;// prefixes (ending with '-' is clearer)
    std::vector<std::string> suffixRules;// suffixes

    // The path-shape rules (see above). Empty for every rule that is
    // identified by its name.
    std::vector<std::string> pathShapes;

    bool enabled = true;

    // Can this rule match this executable name (without the path)?
    bool matchesBaseName(const std::string& base) const;

    // Can this rule match this executable **path**?
    //
    // Only the path-shape rules can: the name rules cannot tell two
    // executables with the same name apart.
    bool matchesPath(const std::string& path) const;
};

// The full compiler table
//
// The established pattern in compiler-configuration tools is a table of
// compiler types, each with its own matching rules and argument
// translation.
// We keep the same idea: one **extensible** table, instead of if-else
// branches scattered through the code.
class CompilerTable {
public:
    CompilerTable();

    // Built-in rules (GCC / Clang / MSVC / ...)
    static CompilerTable builtin();

    // Add a custom rule
    void addRule(const CompilerRule& rule);

    // Match a kind by executable name (without the path)
    CompilerKind matchBaseName(const std::string& base) const;

    // Match by full path.
    //
    // The path-shape rules are tried first, because they are the only
    // ones that can tell two executables with the same name apart
    // (see CompilerRule::pathShapes). When no path shape matches, the
    // base name decides -- which is what this always did.
    CompilerKind matchPath(const std::string& path) const;

    // Match the rule itself (nullptr when there is none)
    const CompilerRule* findRule(const std::string& base) const;

    // All rules
    const std::vector<CompilerRule>& rules() const { return rules_; }

private:
    std::vector<CompilerRule> rules_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Compiler aliases (symlinks mutually recognized)
// ═══════════════════════════════════════════════════════════════════════════

// The alias group of one compiler
//
// Real-world situations:
//   /usr/bin/gcc   -> /usr/bin/gcc-11   (symlink)
//   /usr/bin/cc    -> /usr/bin/gcc
//   /usr/bin/c++   -> /usr/bin/g++
//
// Rules:
//   1. Resolve the **real path**
//   2. All names sharing the same real path are **aliases of each other**
//   3. If any member of the alias set matches a compiler kind, the whole
//      group counts as that kind
//
// This is a certain fact at the filesystem level, not a guess -- so it is
// stricter and more reliable than exact string matching of compiler
// names.
class AliasResolver {
public:
    // Resolve the real path of an executable path (following symlinks)
    // Returns the original path (normalized) on failure.
    static std::string resolveRealPath(const std::string& path);

    // Record a path (and its real path) in the alias table
    void add(const std::string& path);

    // Look up the alias group of a path (excluding itself). Empty if the
    // path was never recorded.
    const std::vector<std::string>& aliasesOf(const std::string& path) const;

    // All known names for a given real path
    const std::vector<std::string>& namesOfReal(const std::string& path) const;

    // Resolve the "compiler identity" a path belongs to
    //
    // Identity = real path + the kind derived from it.
    // Purpose: to tell "are cc and gcc the same compiler?".
    struct Identity {
        std::string realPath;
        CompilerKind kind = CompilerKind::Unknown;
        std::string ruleId;
    };
    Identity identify(const std::string& path, const CompilerTable& table) const;

    // Scan PATH for all aliases that belong to the same real path
    // (only for the given names, to avoid a full-filesystem scan)
    void scanPathAliases(const std::vector<std::string>& names);

    size_t size() const { return byReal_.size(); }

private:
    std::map<std::string, std::vector<std::string>> byReal_;  // real path -> list of names
    std::map<std::string, std::string> realOf_;             // path -> real path
    std::vector<std::string> empty_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Compile commands
// ═══════════════════════════════════════════════════════════════════════════

// One compile command -- the structured result
struct CompileCommand {
    // The raw command line (not split)
    std::string rawCommand;
    // The split arguments (argv[0] is the compiler)
    std::vector<std::string> argv;

    // The working directory (the cwd of the compile command)
    std::string directory;

    // The compiler
    std::string compilerPath;        // full path (when it can be resolved)
    std::string compilerBase;        // basename
    CompilerKind kind = CompilerKind::Unknown;

    // Source files (absolute paths)
    std::vector<std::string> sourceFiles;
    // Output file
    std::string outputFile;

    // Header search paths
    std::vector<std::string> includeDirs;   // -I
    std::vector<std::string> systemDirs;     // -isystem / /external:I
    std::vector<std::string> frameworkDirs; // -F (macOS)

    // ── The JVM languages ─────────────────────────────────────────────
    //
    // A JVM compile is described by its classpath, not by its include
    // path. The native fields above are reused for the native compilers;
    // these three are the JVM ones, and they are only filled in by the
    // JVM capture parser.
    //
    //   classpathDirs   -- the jars and class directories the compiler
    //                      reads types from
    //   moduleDirs      -- the module path
    //   processorDirs   -- the annotation processor path
    //
    // They are separate from includeDirs because the two are genuinely
    // different things: a consumer that has to pick the right one -- for
    // example to search a jar for a class rather than a directory for a
    // header -- cannot afford to have them confused.
    std::vector<std::string> classpathDirs;
    std::vector<std::string> moduleDirs;
    std::vector<std::string> processorDirs;

    // ── The Go language ────────────────────────────────────────────────
    //
    // A Go compile is not described by an include path and a set of
    // macros. It is described by the **package** it belongs to and by
    // the **export data** of the packages it depends on:
    //
    //   packageImportPath -- the "-p" value: the import path of the
    //                       package being compiled ("github.com/x/y/pkg",
    //                       or "command-line-arguments" for the package
    //                       "go build" compiles from the command line)
    //   importConfigPath  -- the "-importcfg" value: the manifest that
    //                       lists the export data of every dependency
    //   packageFiles      -- that manifest, read: import path -> the
    //                       export data file of that package
    //
    // The language version ("-lang=go1.23") goes into languageStandard:
    // it is the language standard of the package, in the same sense that
    // "-std=c++17" is the language standard of a C++ translation unit.
    //
    // WHY THE MANIFEST PATH IS KEPT ALONGSIDE THE MAPPING
    //
    //   The manifest lives in the build's temporary directory, and the
    //   build deletes that directory when it is done. So the mapping may
    //   well be empty when a command line is parsed after the fact --
    //   and the path is then the only thing that still lets a consumer
    //   find the manifest while it exists.
    //
    //   That is also why the path survives the translation into the
    //   analyzer's own vocabulary: it is the one piece of the command
    //   that cannot be reconstructed from the rest.
    std::string packageImportPath;
    std::string importConfigPath;
    std::map<std::string, std::string> packageFiles;

    // Force-included headers: -include <file> / /FI<file>.
    //
    // A forced include changes the **meaning** of the translation unit
    // (it can define macros that change which code compiles), so it must
    // not be dropped silently.
    std::vector<std::string> preIncludes;

    // Macro definitions: name -> value (value may be empty)
    // Order is preserved: because the value may be empty, a vector<pair> is
    // used instead of a map
    std::vector<std::pair<std::string, std::string>> defines;

    // Language standard
    std::string languageStandard;

    // Other options (raw tokens)
    std::vector<std::string> otherOptions;

    // Whether this command is a "link" rather than a compilation
    bool isLinkOnly = false;

    // Whether the command asked for "compile only, do not link".
    //
    // gcc spells it -c, MSVC spells it /c, and clang spells it -c.
    // The flag is consumed during parsing, so it has to be recorded here --
    // otherwise the clang translation cannot know whether to emit -c.
    bool compileOnly = false;

    // The language the compiler was told to use, when it differs from the
    // file extension.
    //
    // MSVC can say "compile this .c file as C++" with /TP or /Tp<file>.
    // The extension alone would say "C" and pick the **clang** driver
    // instead of **clang++** -- and clang++ links libstdc++, which changes
    // the ABI of the produced object.
    //
    // So the intent has to survive parsing, not just the file name.
    //
    // Empty means "no explicit override" -- the extension decides.
    // "c" or "cpp" means the compiler was told explicitly.
    std::string forcedLanguage;

    // Per-file language overrides: file name -> "c" / "cpp".
    //
    // Needed for /Tp<file>, which names the file in the flag itself.
    std::vector<std::pair<std::string, std::string>> perFileLanguages;

    // Diagnostics
    std::vector<std::string> notes;

    // Rebuild the command line (used for export)
    std::string reconstruct() const;
};

// ═══════════════════════════════════════════════════════════════════════════
// Command parsing
// ═══════════════════════════════════════════════════════════════════════════

// Where a command came from
enum class CommandSource {
    RuntimeCapture,   // captured by ccap itself (JSON event stream)
    TextFile,         // plain text, one command per line
    BuildLog,  // a build log in the EXECUTING / COMPILING / CWD = format
    CompilationDb,    // an existing compile_commands.json
};

// The parser
class CommandParser {
public:
    CommandParser();

    // Parse one command line (already split into argv)
    // cwd is the working directory of this command.
    CompileCommand parseArgv(const std::vector<std::string>& argv,
                              const std::string& cwd) const;

    // Parse one raw command line string (splitting it ourselves)
    CompileCommand parseLine(const std::string& line,
                              const std::string& cwd) const;

    // Parse a whole build log
    //
    // The format (as produced by the common static-analysis build wrappers):
    //   EXECUTING 'make all '
    //   CWD = /path/to/dir
    //   COMPILING '/path/<wrapper> g++ -c ... '
    //
    // The wrapper binary in front of the real compiler is stripped; see
    // the kTranslatorPrefixes list in command_parser.cpp.
    //
    // All lines after CWD belong to that directory.
    std::vector<CompileCommand> parseBuildLog(const std::string& text) const;

    // Parse a plain text file (one command per line)
    std::vector<CompileCommand> parseTextFile(const std::string& text) const;

    // Parse compile_commands.json
    //
    // No third-party JSON library is pulled in -- core/json.h is a minimal
    // serializer, not a parser.
    // Here we write a **minimal parser supporting only this format**:
    // the input is either generated by us or generated by clang/CMake, so
    // the structure is fixed.
    std::vector<CompileCommand> parseCompilationDb(const std::string& text) const;

    // Parse ccap's own JSON event stream (ndjson) and extract the compile
    // commands from it
    //
    // ccap's event format (one JSON per line):
    //   {"seq":1,"platform":"linux","source":"runtime","pid":123,
    //    "executable":"c++","argv":["c++","-c","main.cpp"],
    //    "command_line":"c++ -c main.cpp","cwd":"/build",...}
    //
    // Only events with source == "runtime" are taken (the static layer is
    // script builtin commands, there is no process).
    std::vector<CompileCommand> parseCaptureEvents(const std::string& text) const;

    // Parse the JVM capture agent's JSON files
    //
    // The agent writes **one JSON object per file**, not a stream: each
    // file is one capture, and a directory of them is the capture set.
    // The text is therefore a concatenation of JSON objects, and this
    // parses them all.
    //
    // The format is the one the capture agent writes:
    //   {
    //     "api": "cli",                  // or "api"
    //     "compiler": "javac",           // javac/ecj/sjavac/kotlin/scala3/scala2
    //     "language": "java",            // java/kotlin/scala
    //     "argv": ["-d", "out", "Hello.java"],
    //     "classpath": ["lib.jar"],
    //     "sourcePath": [],
    //     "modulePath": [],
    //     "processorPath": [],
    //     "outputDirectory": "out",
    //     "sourceFiles": ["Hello.java"],
    //     "languageLevel": "17",
    //     "compilation": true,
    //     "workingDirectory": "/build"
    //   }
    //
    // **Why the fields are used as they are.** The argv is kept as the
    // raw command, because that is what a consumer that wants to replay
    // the compile needs. But the *effective* compile -- the classpath the
    // compiler actually used, the sources it actually compiled -- comes
    // from the structured fields, because the argv alone does not say
    // them: the API path has no argv at all, and the classpath may have
    // come from the environment.
    std::vector<CompileCommand> parseJvmCaptures(const std::string& text) const;

    // Parse the JVM capture files in a directory
    //
    // The files are read in name order, so the result is deterministic.
    // A file that cannot be read or parsed is skipped rather than
    // failing the whole import: one broken capture must not lose the
    // others.
    std::vector<CompileCommand> parseJvmCaptureDirectory(
        const std::string& directory) const;

    // Set the compiler table (affects kind determination)
    void setCompilerTable(const CompilerTable& table) { table_ = table; }

private:
    CompilerTable table_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Merging the two views of a JVM compile
// ═══════════════════════════════════════════════════════════════════════════

// Merge the compile commands ccap saw with the ones the JVM agent captured.
//
// WHY BOTH
//
//   The two sources answer different questions, and neither is complete on
//   its own:
//
//     * ccap sees the **command line** the compiler was started with. That
//       is always there -- but it does not say what the compiler really
//       did: a glob is still unexpanded, the classpath may have been
//       assembled by a plugin, and the API path (javax.tools) has no argv
//       at all.
//
//     * the agent sees what the compiler **actually did**: the sources it
//       really compiled, the classpath it really used. But it only sees
//       compiles that went through a compiler it could hook.
//
//   So the agent's version wins where both have an entry -- it carries the
//   ground truth -- and ccap's version fills in the compiles the agent
//   missed.
//
// HOW THEY ARE MATCHED
//
//   By the **output directory and the source file set**, not by the
//   command line. The command line differs between the two views (the
//   agent's argv is what the compiler was handed, which is not what ccap
//   saw on the command line), while the output directory and the sources
//   identify the same compile.
//
// @param fromCcap the commands ccap recorded
// @param fromAgent the commands the agent captured
// @return the merged list
std::vector<CompileCommand> mergeJvmCommands(const std::vector<CompileCommand>& fromCcap,
                                             const std::vector<CompileCommand>& fromAgent);

// ═══════════════════════════════════════════════════════════════════════════
// Filter rules
// ═══════════════════════════════════════════════════════════════════════════

// Filter configuration
struct FilterConfig {
    // Executable names to exclude (scripts, linkers, assemblers, ...)
    std::vector<std::string> excludeNames;
    // Name prefixes to exclude (for toolchain wrappers such as "ccache",
    // "sccache", "distcc")
    std::vector<std::string> excludePrefixes;
    // Allowlist (when non-empty, only these are kept)
    std::vector<std::string> includeNames;

    // **Keep script commands** (default false -- the user explicitly asked
    // for "filter out scripts")
    //
    // Independent of exclude_names: whether something is a script is
    // determined by "is this name a shell interpreter",
    // not by "is it in the exclude list". The two are different mechanisms.
    bool keepScripts = false;

    // Is it a script (always excluded unless keep_scripts)
    static bool isScript(const std::string& baseName);

    // The default exclusion table
    static FilterConfig builtin();
};

class FilterRules {
public:
    explicit FilterRules(const FilterConfig& config);

    // Should this command be kept?
    bool keep(const CompileCommand& cmd) const;

    // An explanation of the decision basis (for diagnostics)
    std::string explain(const CompileCommand& cmd) const;

    const FilterConfig& config() const { return config_; }

private:
    FilterConfig config_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Compiler adapters
// ═══════════════════════════════════════════════════════════════════════════

// The adapter's context
struct AdapterContext {
    // Extra system header search paths (from the configuration)
    std::vector<std::string> extraSystemIncludes;
    // The output directory (the base for resolving relative paths)
    std::string outputDir;
    // Whether to keep the original arguments such as -I / -D
    bool keepIncludeDirs = true;
    bool keepDefines = true;
};

// The adapter interface
//
// **One implementation per compiler family, fully isolated from each
// other**:
//   - each adapter only knows its own option syntax
//   - adapters never call each other
//   - adding a new compiler = adding a new file, nothing else changes
class CompilerAdapter {
public:
    virtual ~CompilerAdapter() = default;

    virtual CompilerKind kind() const = 0;

    // Can this adapter handle this command?
    virtual bool matches(const CompileCommand& cmd) const = 0;

    // Translate a native compile command into "the arguments for the
    // analyzer"
    //
    // Style modelled on the invocation used by static-analysis front ends:
    //   --sys_include <dir>       system headers
    //   --preinclude <file>       pre-included header
    //   -D<name>[=<val>]          macro
    //   -I<dir>                   include path
    virtual std::vector<std::string> translate(const CompileCommand& cmd,
                                               const AdapterContext& ctx) const = 0;

    // The adapter name (for diagnostics)
    virtual const char* name() const = 0;
};

// The adapter registry
class AdapterRegistry {
public:
    AdapterRegistry();

    // Find the adapter that can handle this command
    const CompilerAdapter* find(const CompileCommand& cmd) const;

    // Look up directly by kind
    const CompilerAdapter* byKind(CompilerKind kind) const;

    // All adapters
    const std::vector<const CompilerAdapter*>& all() const { return adapters_; }

private:
    // Note: the adapter factories return **static instances** (alive for the
    // lifetime of the process), so raw pointers are stored here and no
    // unique_ptr management is needed.
    //
    // Using unique_ptr would fail to compile -- the factory returns a const
    // pointer, which cannot be converted to unique_ptr<CompilerAdapter>.
    std::vector<const CompilerAdapter*> adapters_;
};

// The factory function of each adapter
//
// **One file and one factory function per adapter** --
// so adding a new compiler requires no changes in any other file.
namespace adapters {
const CompilerAdapter* makeGccAdapter();
const CompilerAdapter* makeClangAdapter();
const CompilerAdapter* makeMsvcAdapter();
const CompilerAdapter* makeGenericAdapter();

// The JVM language adapters.
//
// One per compiler kind, because the option syntax differs and the
// adapters are isolated from each other by design.
const CompilerAdapter* makeJavacAdapter();
const CompilerAdapter* makeEcjAdapter();
const CompilerAdapter* makeKotlinAdapter();
const CompilerAdapter* makeScala3Adapter();
const CompilerAdapter* makeScala2Adapter();

// The .NET language adapters.
//
// One per compiler kind, for the same reason as the JVM ones: the
// option syntax differs, and the adapters are isolated from each other.
const CompilerAdapter* makeCsharpAdapter();

// The Go adapter.
//
// The Go toolchain is one kind, not several: "compile" and "link"
// take the same options, and the difference between them is which
// step of the build they are -- a fact the model records, not a fact
// about the option syntax.
const CompilerAdapter* makeGoAdapter();
}  // namespace adapters

// ═══════════════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════════════

// The configuration file (JSON)
//
// See section 5.3 of docs/en/DESIGN-MODULES-2026-10-27.md for the structure.
struct CompdbConfig {
    // The enabled compilers
    struct CompilerEntry {
        std::string kind;              // "gcc" / "clang" / "msvc" ...
        std::string id;                // identifier
        std::vector<std::string> paths;// full paths
        std::vector<std::string> names;// executable names
        std::vector<std::string> prefixes;
        bool enabled = true;
    };

    std::vector<CompilerEntry> compilers;

    // Exclude / include
    FilterConfig filter;

    // The system header paths per kind
    std::map<std::string, std::vector<std::string>> systemIncludes;

    // ── The compiler profiles ─────────────────────────────────────────
    //
    // A profile is the **switch table** of one compiler family: how it
    // spells its options, what they mean, and how they map to clang.
    //
    // The built-in profiles cover gcc, clang, msvc and the generic
    // fallback. A user profile **adds** to those -- it does not
    // replace them -- so configuring an unknown cross compiler never
    // costs you the built-in gcc behaviour.
    //
    // See ccap_compdb_map.h for the design.
    std::vector<CompilerProfile> profiles;

    // The mapping of an **unknown** compiler to a profile.
    //
    // This is the "the user says: treat it as the gcc family" case.
    //
    // The key is the compiler's base name (or a prefix of it); the
    // value is the id of the profile to use for it. The special value
    // "generic" means "use the conservative generic table" -- the
    // intersection of the common option spellings.
    //
    // The lookup is tried in this order:
    //   1. an exact base-name match
    //   2. a prefix match (longest prefix wins)
    //   3. the default below
    //
    // The default is empty, which means "an unknown compiler falls back
    // to the generic profile". Setting it to "gnu" makes every unknown
    // compiler be treated as the gcc family.
    std::map<std::string, std::string> compilerProfiles;

    // The fallback profile for a compiler that is not in the table and
    // has no explicit mapping.
    //
    // Empty means the generic profile.
    std::string defaultProfile;

    // ═══════════════════════════════════════════════════════════════════
    // The compiler registry
    // ═══════════════════════════════════════════════════════════════════
    //
    // The profiles above answer "how does this compiler spell its
    // options". The registry answers the questions that a flat profile
    // cannot:
    //
    //   - which **variants** belong to a family (gcc -> g++ -> g++cc),
    //     and how an executable name selects one;
    //   - which rules apply to a **subset** of the variants;
    //   - how to detect the **version**, and which rules are scoped to
    //     a version range.
    //
    // A user family with the same id as a built-in one **replaces** it:
    // the intent of "override the gcc table" is "my gcc does not
    // behave like the stock one", and a merge would leave the stock
    // rules underneath -- which still win on the longer option names.
    std::vector<CompilerFamily> families;

    // The executable-name match patterns.
    //
    // Two forms:
    //
    //   "*-gcc"          a glob. '*' matches any run of characters and
    //                    may appear anywhere; '?' matches one.
    //   "replace/a/b"    a redirect: the executable called "a" is
    //                    really the compiler "b".
    //
    // The patterns are tried after the explicit user mapping and
    // before the default profile, and the **longest** glob wins.
    std::vector<CompilerMatch> compilerMatches;

    // The user's rule overrides.
    //
    // The scope is (profile, variant): an empty profile applies to
    // every profile, an empty variant to every variant of it.
    std::vector<ProfileOverride> profileOverrides;

    // What to do with an option the switch table does not know.
    //
    // "report" (the default) drops it from the generated command but
    // records it in the audit report. "keep" passes it through
    // unchanged. "drop" drops it silently -- the one mode that can
    // produce a wrong artifact without telling anybody. "fail" makes
    // the mapping fail.
    //
    // Empty means "report".
    std::string unknownPolicy;

    // The compiler version, for the version-scoped rules.
    //
    // The tool never runs the compiler by itself (a build log from
    // another machine has no toolchain here), so the version comes from
    // the caller. Empty means "not known", and the version-scoped rules
    // then do not fire -- a rule whose precondition could not be
    // verified must not be applied.
    std::string compilerVersion;

    // Output
    std::string outputPath = "compile_commands.json";
    std::string outputFormat = "clang";

    // The retention level of the generated clang commands.
    //
    // "ast" -- the arguments needed for a correct clang AST.
    // "bc"  -- the arguments needed for a correct LLVM bitcode file.
    //
    // Only used by the "clang-cmd" output format.
    std::string outputLevel = "ast";

    // Parse the JSON configuration; on failure returns false and fills in err
    static bool load(const std::string& text, CompdbConfig* out, std::string* err);
    static bool loadFile(const std::string& path, CompdbConfig* out, std::string* err);
};

// ═══════════════════════════════════════════════════════════════════════════
// Export
// ═══════════════════════════════════════════════════════════════════════════

// Export as a clang compilation database (compile_commands.json)
//
// Specification: https://clang.llvm.org/docs/JSONCompilationDatabase.html
//
// Uses "arguments" rather than "command" -- the former needs no
// command-line escaping, and consumers (clang tooling) support both, but
// arguments is less error-prone.
std::string exportClangCompilationDb(const std::vector<CompileCommand>& commands);

// Export as analyzer-style translated commands (one per line)
//
// Shape:
//   g++ --sys_include /usr/include -DDEBUG -c main.cpp
//
// The command is emitted without any wrapper prefix: the caller decides
// which front end to feed it to.
std::string exportTranslatedCommands(const std::vector<CompileCommand>& commands,
                                      const AdapterRegistry& registry,
                                      const AdapterContext& ctx);

// Export as real clang commands (one per line)
//
// Shape:
//   clang++ -I. -DDEBUG -std=c++17 -c main.cpp -o main.o
//
// This is the **translated** form: the driver is clang / clang++ and the
// options use clang's spelling, regardless of which compiler the original
// command used.
//
// It is different from exportClangCompilationDb(), which keeps the original
// argv untouched -- that one is a faithful record of the build, this one is
// a clang replay of it.
std::string exportClangCommands(const std::vector<CompileCommand>& commands);

// ═══════════════════════════════════════════════════════════════════════════
// The level-aware clang command export
// ═══════════════════════════════════════════════════════════════════════════

// The clang commands for a retention level.
//
// This is the table-driven version of exportClangCommands(): the
// options are mapped through the switch table, and the level decides
// which of them are kept.
//
// The difference between the two is not cosmetic:
//   exportClangCommands()  -- a best-effort translation of what it
//                             knows, with the losses reported
//                             separately
//   exportClangCommandLines() -- the full table-driven mapping, with
//                             every decision explained
//
// The lines are returned separately (not as one blob) so the caller can
// report on them individually.
std::vector<std::string> exportClangCommandLines(
    const std::vector<CompileCommand>& commands,
    const OptionMapper& mapper,
    TranslateLevel level,
    std::vector<MappingReport>* reports = nullptr);

// The same, as one text blob (one command per line).
std::string exportClangCommandLinesText(
    const std::vector<CompileCommand>& commands,
    const OptionMapper& mapper,
    TranslateLevel level);

// The human-readable report: what was kept, what was dropped and why.
std::string exportMappingReports(const std::vector<CompileCommand>& commands,
                                  const OptionMapper& mapper,
                                  TranslateLevel level);

// ═══════════════════════════════════════════════════════════════════════════
// clang command translation
// ═══════════════════════════════════════════════════════════════════════════

// Convert a native compile command into a **real clang command line**.
//
// This is different from the compiler adapters above: those translate into
// the argument set a static-analysis front end expects (--sys_include,
// --preinclude, ...), which is not a compiler command line.
//
// This answers: "if I want to compile this translation unit with clang
// instead of the original compiler, what is the command line?"
//
// The MSVC options are translated to their clang equivalents:
//   /I<dir>    -> -I<dir>
//   /D<name>   -> -D<name>
//   /std:c++17 -> -std=c++17
//   /Zi        -> -g
//   /O2        -> -O2
//   /Fo<file>  -> -o <file>
//
// The driver is "clang++" when the sources are C++ and "clang" otherwise,
// because linking a C++ translation unit needs the C++ driver.
//
// Options with no clang equivalent are NOT silently dropped -- see
// clangTranslationLosses() below.
std::vector<std::string> toClangCommand(const CompileCommand& cmd);

// The options of a command that have no clang equivalent.
//
// The translation is lossy for some MSVC options (the exception handling
// model, the runtime library selection, ...). The runtime library in
// particular changes the ABI, not just the flags, so the caller should know
// about it rather than discover it as a link error later.
std::vector<std::string> clangTranslationLosses(const CompileCommand& cmd);

}  // namespace compdb
}  // namespace ccap
