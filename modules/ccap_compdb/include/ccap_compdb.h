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
enum class CompilerKind {
    Unknown = 0,
    Gcc,        // gcc / g++ / cc / c++ / <target>-gcc ...
    Clang,      // clang / clang++ / clang-<ver>
    Msvc,       // MSVC cl.exe
    Iar,        // IAR EW
    Generic,    // fallback: generic extraction of -I/-D only
};

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
struct CompilerRule {
    CompilerKind kind = CompilerKind::Unknown;
    std::string id;                       // rule identifier (referenced by it in the config)
    std::vector<std::string> exactNames; // exact executable names
    std::vector<std::string> prefixRules;// prefixes (ending with '-' is clearer)
    std::vector<std::string> suffixRules;// suffixes
    bool enabled = true;

    // Can this rule match this executable name (without the path)?
    bool matchesBaseName(const std::string& base) const;
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

    // Match by full path (take the basename and match that)
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

    // Set the compiler table (affects kind determination)
    void setCompilerTable(const CompilerTable& table) { table_ = table; }

private:
    CompilerTable table_;
};

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
}  // namespace adapters

// ═══════════════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════════════

// The configuration file (JSON)
//
// See section 5.3 of docs/DESIGN-MODULES-2026-10-27.md for the structure.
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

    // Output
    std::string outputPath = "compile_commands.json";
    std::string outputFormat = "clang";

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
