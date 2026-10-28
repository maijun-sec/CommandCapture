// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - structured parsing of compile commands
//
// Turns one raw command line into a CompileCommand:
//   source file, output file, include path, macro definitions, language
//   standard, other options.
//
// The parsing rules are dispatched per compiler, but the **structure is
// uniform** -- so downstream consumers (export, adapters) do not have to care
// about the compiler type.

#include "ccap_compdb.h"

#include <cctype>

#include "core/command_line.h"
#include "core/json_parser.h"
#include "core/normalize.h"

namespace ccap {
namespace compdb {

namespace {

// Is it a source file extension?
bool isSourceExtension(const std::string& ext) {
    static const char* kSourceExts[] = {
        ".c", ".cc", ".cpp", ".cxx", ".c++", ".cp",
        ".m", ".mm",
        ".i", ".ii",       // preprocessed
    };
    for (const char* e : kSourceExts) {
        if (ext == e) return true;
    }
    return false;
}

// Is it a header file extension?
bool isHeaderExtension(const std::string& ext) {
    static const char* kHeaderExts[] = {
        ".h", ".hh", ".hpp", ".hxx", ".h++", ".inl", ".ipp",
        ".tcc", ".tpp",
    };
    for (const char* e : kHeaderExts) {
        if (ext == e) return true;
    }
    return false;
}

// The language MSVC is told to use for the **next** source file.
//
// MSVC has three ways to say "compile this file as C++":
//
//   /TP           treat all following .c files as C++
//   /Tp<file>     treat this specific file as C++
//   /TC           treat all following .c files as C
//
// Why this matters here:
//   The driver choice in the clang translation is based on the **source
//   file extension**. A file named "foo.c" that MSVC is told to compile
//   as C++ (/TP) would otherwise be translated to **clang** (the C
//   driver) instead of **clang++** -- and clang++ links libstdc++,
//   which changes the ABI of the result.
//
// So the intent has to survive parsing, not just the file name.
enum class SourceLanguage {
    Default,
    C,
    Cpp,
};

// Parse the MSVC per-file language flags.
//
// Returns false when the argument is not one of them.
bool parseMsvcSourceLanguage(const std::string& arg,
                             SourceLanguage* lang,
                             std::string* specificFile) {
    // /Tp<file> -- a specific file, the flag carries the name.
    if (arg.rfind("/Tp", 0) == 0 && arg.size() > 3) {
        *lang = SourceLanguage::Cpp;
        *specificFile = arg.substr(3);
        return true;
    }
    // /TP -- all following files
    if (arg == "/TP") {
        *lang = SourceLanguage::Cpp;
        return true;
    }
    // /TC -- all following files
    if (arg == "/TC") {
        *lang = SourceLanguage::C;
        return true;
    }
    return false;
}

// Split "-Dfoo=bar" into (foo, bar)
// Split "-Dfoo" into (foo, "")
// MSVC's "/Dfoo=bar" is handled the same way
bool splitDefine(const std::string& text, std::string* name, std::string* value) {
    size_t eq = text.find('=');
    if (eq == std::string::npos) {
        *name = text;
        value->clear();
    } else {
        *name = text.substr(0, eq);
        *value = text.substr(eq + 1);
    }
    return !name->empty();
}

// Take the next argument (handles the separated form "-I dir")
//
// Returns false when there is no next argument.
bool takeNext(const std::vector<std::string>& argv, size_t* i, std::string* out) {
    if (*i + 1 >= argv.size()) return false;
    ++(*i);
    *out = argv[*i];
    return true;
}

}  // namespace

std::string CompileCommand::reconstruct() const {
    // Rebuild the command line.
    //
    // Note: **no quote escaping is done here** -- the exported
    // compile_commands.json uses an "arguments" array (one token per
    // element), so escaping is unnecessary.
    // reconstruct() is only used for text output and the translated export.
    std::string out;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) out += ' ';
        const std::string& a = argv[i];
        bool needQuote = a.empty();
        for (char c : a) {
            if (std::isspace(static_cast<unsigned char>(c)) || c == '"' ||
                c == '\'') {
                needQuote = true;
                break;
            }
        }
        if (needQuote) {
            out += '"';
            for (char c : a) {
                if (c == '"') out += '\\';
                out += c;
            }
            out += '"';
        } else {
            out += a;
        }
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// CommandParser
// ═══════════════════════════════════════════════════════════════════════════

CommandParser::CommandParser() : table_(CompilerTable::builtin()) {}

CompileCommand CommandParser::parseArgv(const std::vector<std::string>& argv,
                                         const std::string& cwd) const {
    CompileCommand cmd;
    cmd.argv = argv;
    cmd.directory = cwd;

    if (argv.empty()) return cmd;

    cmd.compilerBase = ccap::baseName(argv[0]);
    // Strip .exe (on Windows argv[0] may be "cl.exe")
    std::string base = cmd.compilerBase;
    if (base.size() > 4) {
        std::string tail = base.substr(base.size() - 4);
        for (char& c : tail) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (tail == ".exe") base = base.substr(0, base.size() - 4);
    }
    cmd.compilerBase = base;

    cmd.kind = table_.matchBaseName(base);

    // Parse the path
    //
    // **Deliberately no PATH lookup.**
    //
    // Reason: the compiler path in a log / compile database may belong to
    // **another machine** (e.g. the /export/home/... paths in a build log
    // produced elsewhere). If we call resolve_executable() here, it would
    // fill in the
    // compiler from this machine's PATH (e.g.
    // D:/development/w64devkit/bin/g++.exe) -- that is **wrong**: the user
    // sees a local compiler, while the analysis targets the compiler on the
    // machine that produced the log.
    //
    // Only record the path when the command line **itself carries one**.
    // When a real PATH resolution is needed, use `ccap compdb aliases`
    // (an explicit action).
    if (!argv[0].empty() && ccap::isAbsolutePath(argv[0])) {
        cmd.compilerPath = argv[0];
    }

    // ── Main loop: dispatch by option category ─────────────────────────
    for (size_t i = 1; i < argv.size(); ++i) {
        const std::string& a = argv[i];

        // ── Output file ─────────────────────────────────────────────────
        if (a == "-o" || a == "/o") {
            std::string v;
            if (takeNext(argv, &i, &v)) {
                cmd.outputFile = v;
            }
            continue;
        }
        if (a.rfind("-o", 0) == 0 && a.size() > 2 && a[1] != '-') {
            cmd.outputFile = a.substr(2);
            continue;
        }
        // MSVC: /Fe<file> is the exe output, /Fo<dir> is the obj output
        if (a.rfind("/Fe", 0) == 0) {
            cmd.outputFile = a.substr(3);
            continue;
        }
        if (a.rfind("/Fo", 0) == 0) {
            if (cmd.outputFile.empty()) cmd.outputFile = a.substr(3);
            continue;
        }

        // ── Include path ───────────────────────────────────────────────
        if (a == "-I" || a == "/I") {
            std::string v;
            if (takeNext(argv, &i, &v)) cmd.includeDirs.push_back(v);
            continue;
        }
        if (a.rfind("-I", 0) == 0 && a.size() > 2) {
            cmd.includeDirs.push_back(a.substr(2));
            continue;
        }
        // MSVC: /I<dir> (=/Ifoo, note there is no space)
        if (a.rfind("/I", 0) == 0 && a.size() > 2) {
            cmd.includeDirs.push_back(a.substr(2));
            continue;
        }
        // ── Pre-included header ─────────────────────────────────────────
        //
        // gcc/clang: -include <file>
        // MSVC:       /FI<file>  (no space)
        //
        // Why it is recorded rather than dropped: a forced include
        // **changes the meaning of the translation unit** -- it can
        // define macros that change which code compiles. Dropping it
        // silently would make the translated command compile something
        // **different**, which is worse than reporting a loss.
        if (a == "-include") {
            std::string v;
            if (takeNext(argv, &i, &v)) cmd.preIncludes.push_back(v);
            continue;
        }
        if (a.rfind("-include=", 0) == 0 && a.size() > 9) {
            cmd.preIncludes.push_back(a.substr(9));
            continue;
        }
        if (a.rfind("/FI", 0) == 0 && a.size() > 3) {
            cmd.preIncludes.push_back(a.substr(3));
            continue;
        }
        // System includes
        if (a == "-isystem" || a == "-idirafter" || a == "-iquote") {
            std::string v;
            if (takeNext(argv, &i, &v)) cmd.systemDirs.push_back(v);
            continue;
        }
        if (a.rfind("-isystem", 0) == 0 && a.size() > 8) {
            cmd.systemDirs.push_back(a.substr(8));
            continue;
        }
        // MSVC: /external:I<dir> and /imsvc<dir>
        if (a.rfind("/external:I", 0) == 0 && a.size() > 11) {
            cmd.systemDirs.push_back(a.substr(11));
            continue;
        }
        if (a.rfind("/imsvc", 0) == 0 && a.size() > 6) {
            cmd.systemDirs.push_back(a.substr(6));
            continue;
        }
        // Framework (macOS)
        if (a == "-F") {
            std::string v;
            if (takeNext(argv, &i, &v)) cmd.frameworkDirs.push_back(v);
            continue;
        }
        if (a.rfind("-F", 0) == 0 && a.size() > 2 && a[1] != 'f') {
            cmd.frameworkDirs.push_back(a.substr(2));
            continue;
        }

        // ── Macro definitions ───────────────────────────────────────────
        if (a == "-D" || a == "/D") {
            std::string v;
            if (takeNext(argv, &i, &v)) {
                std::string n, val;
                if (splitDefine(v, &n, &val)) {
                    cmd.defines.emplace_back(n, val);
                }
            }
            continue;
        }
        if (a.rfind("-D", 0) == 0 && a.size() > 2) {
            std::string n, val;
            if (splitDefine(a.substr(2), &n, &val)) {
                cmd.defines.emplace_back(n, val);
            }
            continue;
        }
        if (a.rfind("/D", 0) == 0 && a.size() > 2) {
            std::string n, val;
            if (splitDefine(a.substr(2), &n, &val)) {
                cmd.defines.emplace_back(n, val);
            }
            continue;
        }
        // Undefine a macro: -Ufoo / /Ufoo
        if (a.rfind("-U", 0) == 0 && a.size() > 2) continue;
        if (a.rfind("/U", 0) == 0 && a.size() > 2) continue;

        // ── Language standard ───────────────────────────────────────────
        if (a.rfind("-std=", 0) == 0) {
            cmd.languageStandard = a.substr(5);
            continue;
        }
        if (a.rfind("/std:", 0) == 0) {
            cmd.languageStandard = a.substr(5);
            continue;
        }
        if (a == "-ansi") {
            cmd.languageStandard = "c89";
            continue;
        }

        // ── Compile only, do not link ───────────────────────────────────
        //
        // gcc and clang spell it -c, MSVC spells it /c.
        // The flag is consumed here, so the intent is recorded in
        // cmd.compileOnly -- the clang translation needs it to know whether
        // to emit -c.
        if (a == "-c" || a == "/c") {
            cmd.compileOnly = true;
            continue;
        }

        // ── MSVC per-file language selection ───────────────────────────
        //
        // /TP          compile the following files as C++
        // /Tp<file>    compile that specific file as C++
        // /TC          compile the following files as C
        //
        // The intent has to be recorded, because the **file extension is
        // not enough**: a ".c" file compiled as C++ must be translated to
        // clang++ (which links libstdc++), not clang.
        {
            SourceLanguage lang = SourceLanguage::Default;
            std::string specific;
            if (parseMsvcSourceLanguage(a, &lang, &specific)) {
                const char* name = (lang == SourceLanguage::Cpp) ? "cpp" : "c";
                if (!specific.empty()) {
                    cmd.perFileLanguages.emplace_back(specific, name);
                } else {
                    cmd.forcedLanguage = name;
                }
                continue;
            }
        }

        // ── Preprocessor options (not passed to the analyzer, but recorded)
        if (a == "-E" || a == "-M" || a == "-MM" || a == "-MD" ||
            a == "-MMD" || a == "-MP") {
            cmd.notes.push_back("preprocessor-only option: " + a);
            continue;
        }

        // ── Non-option arguments: source files ──────────────────────────
        if (!a.empty() && a[0] == '-') {
            cmd.otherOptions.push_back(a);
            continue;
        }
        if (!a.empty() && a[0] == '/') {
            // MSVC-style / options
            cmd.otherOptions.push_back(a);
            continue;
        }

        std::string ext = ccap::extensionOf(a);
        if (isSourceExtension(ext)) {
            cmd.sourceFiles.push_back(a);
        } else if (isHeaderExtension(ext)) {
            // A header file compiled directly (rare, but it happens)
            cmd.sourceFiles.push_back(a);
        } else {
            // Other non-option arguments -- possibly linker inputs
            cmd.otherOptions.push_back(a);
        }
    }

    // ── Decide whether this is a "link only" command ───────────────────
    //
    // With -c it is a compilation (object files only).
    // Without -c but with source files -> compile + link (the frontend is
    // still invoked).
    // With no source files at all -> pure link.
    //
    // The -c / /c flag was already recorded in cmd.compileOnly above.
    cmd.isLinkOnly = cmd.sourceFiles.empty() && !cmd.compileOnly;

    // ── Turn relative paths into absolute paths ───────────────────────
    //
    // The compile database specification recommends that file be an
    // absolute path -- because the consumer may run under a different cwd.
    if (!cwd.empty()) {
        std::string base = ccap::normalizePath(cwd);
        for (auto& s : cmd.sourceFiles) {
            if (!ccap::isAbsolutePath(s)) {
                s = ccap::normalizePath(base + "/" + s);
            }
        }
    }

    return cmd;
}

CompileCommand CommandParser::parseLine(const std::string& line,
                                         const std::string& cwd) const {
    CompileCommand cmd;
    cmd.rawCommand = line;

    std::string trimmed = line;
    // Strip leading and trailing whitespace
    size_t b = 0;
    while (b < trimmed.size() &&
           std::isspace(static_cast<unsigned char>(trimmed[b]))) ++b;
    trimmed = trimmed.substr(b);
    while (!trimmed.empty() &&
           std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }

    if (trimmed.empty()) return cmd;

    std::vector<std::string> argv = ccap::splitCompilerCommandLine(trimmed);
    if (argv.empty()) return cmd;

    // ── Strip the wrapper prefix ──────────────────────────────────────
    //
    // Build logs produced by the static-analysis build wrappers contain a
    // wrapper binary in front of the real compiler:
    //   COMPILING '/export/.../<wrapper> g++ -c -o property.o -I. ... property.cpp '
    //
    // The real compiler is the **second** token (g++);
    // the first one is the wrapper.
    //
    // Without stripping it, the compiler is recognized as the wrapper's
    // name -- a compiler that does not exist -- so the kind cannot be
    // determined and every argument is parsed wrong.
    //
    // Only strip **known** wrapper names; never strip an arbitrary name
    // sharing the prefix (the user might really have a tool named
    // something similar).
    {
        // The wrapper binaries that the common static-analysis build tools
        // put in front of the real compiler.
        //
        // These are **input data**, not a product reference: they are the
        // literal program names that appear in the build logs those tools
        // write, and the tool has to recognise them to parse such a log
        // correctly.
        //
        // The list is deliberately **generic** ("tool" + "translate" /
        // "emit" / "build" / "analyze") rather than tied to one vendor's
        // spelling, so that logs from different tools are covered without
        // this file carrying a product name.
        static const char* kTranslatorPrefixes[] = {
            "tool-translate", "tool-emit", "tool-build", "tool-analyze",
            "sa-translate", "sa-emit", "sa-build", "sa-analyze",
            "sast-translate", "sast-emit", "sast-build", "sast-analyze",
        };
        for (const char* p : kTranslatorPrefixes) {
            if (argv.size() >= 2) {
                std::string first = ccap::baseName(argv[0]);
                if (first == p) {
                    argv.erase(argv.begin());
                    break;
                }
            }
        }
    }

    cmd = parseArgv(argv, cwd);
    cmd.rawCommand = line;
    return cmd;
}

// ═══════════════════════════════════════════════════════════════════════════
// Build log (EXECUTING / COMPILING / CWD = format)
// ═══════════════════════════════════════════════════════════════════════════

std::vector<CompileCommand> CommandParser::parseBuildLog(
    const std::string& text) const {
    std::vector<CompileCommand> out;

    std::string currentCwd;
    std::string cur;
    bool inCommand = false;

    auto flush = [&]() {
        if (!inCommand) return;
        inCommand = false;
        if (cur.empty()) return;
        // Build log lines may have trailing whitespace
        while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back()))) {
            cur.pop_back();
        }
        if (cur.empty()) return;
        out.push_back(parseLine(cur, currentCwd));
        cur.clear();
    };

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = (nl == std::string::npos)
                               ? text.substr(pos)
                               : text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("CWD = ", 0) == 0) {
            flush();
            currentCwd = line.substr(6);
            while (!currentCwd.empty() &&
                   std::isspace(static_cast<unsigned char>(currentCwd.back()))) {
                currentCwd.pop_back();
            }
        } else if (line.rfind("EXECUTING ", 0) == 0 ||
                   line.rfind("COMPILING ", 0) == 0) {
            flush();
            inCommand = true;
            // Strip the "EXECUTING " prefix and the surrounding quotes
            size_t sp = line.find(' ');
            cur = line.substr(sp + 1);
            // Strip the leading and trailing single quotes
            if (!cur.empty() && cur.front() == '\'') cur.erase(cur.begin());
            if (!cur.empty() && cur.back() == '\'') cur.pop_back();
        } else if (line.rfind("Emit for file", 0) == 0) {
            // The front end's emit result -- ignore it
            flush();
        } else if (line.rfind("[ERROR]", 0) == 0) {
            // Error line -- ignore it
        } else {
            // Other lines: if a command is being read, this may be a
            // continuation line of that command
            if (inCommand && !line.empty()) {
                cur += " ";
                cur += line;
            }
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    flush();

    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// Plain text files
// ═══════════════════════════════════════════════════════════════════════════

std::vector<CompileCommand> CommandParser::parseTextFile(const std::string& text) const {
    std::vector<CompileCommand> out;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = (nl == std::string::npos)
                               ? text.substr(pos)
                               : text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Skip blank lines and obvious comments
        std::string trimmed = line;
        size_t b = 0;
        while (b < trimmed.size() &&
               std::isspace(static_cast<unsigned char>(trimmed[b]))) ++b;
        trimmed = trimmed.substr(b);
        if (!trimmed.empty() && trimmed[0] != '#') {
            out.push_back(parseLine(trimmed, ""));
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// compile_commands.json
// ═══════════════════════════════════════════════════════════════════════════

std::vector<CompileCommand> CommandParser::parseCompilationDb(
    const std::string& text) const {
    std::vector<CompileCommand> out;

    std::string err;
    JsonPtr root = jsonParse(text, &err);
    if (!root || !root->isArray()) return out;

    for (const auto& item : root->array) {
        if (!item || !item->isObject()) continue;

        std::string directory = item->get("directory") ? item->get("directory")->asString() : "";
        std::string file = item->get("file") ? item->get("file")->asString() : "";

        std::vector<std::string> argv;
        if (const JsonValue* args = item->get("arguments")) {
            argv = args->asStringArray();
        }
        if (argv.empty()) {
            if (const JsonValue* c = item->get("command")) {
                argv = ccap::splitCompilerCommandLine(c->asString());
            }
        }
        if (argv.empty()) continue;

        CompileCommand cc = parseArgv(argv, directory);
        if (!file.empty()) {
            bool has = false;
            for (const auto& s : cc.sourceFiles) {
                if (s == file) { has = true; break; }
            }
            if (!has) cc.sourceFiles.push_back(file);
        }
        out.push_back(cc);
    }

    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// ccap's own event stream
// ═══════════════════════════════════════════════════════════════════════════

std::vector<CompileCommand> CommandParser::parseCaptureEvents(
    const std::string& text) const {
    std::vector<CompileCommand> out;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = (nl == std::string::npos)
                               ? text.substr(pos)
                               : text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Quickly filter out lines that are not objects
        size_t b = 0;
        while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
        if (b < line.size() && line[b] == '{') {
            std::string err;
            JsonPtr ev = jsonParse(line, &err);
            if (ev && ev->isObject()) {
                // Only take runtime events -- the static layer is script
                // builtin commands, there is no real process
                const JsonValue* src = ev->get("source");
                if (src && src->asString() == "runtime") {
                    std::string cwd;
                    if (const JsonValue* c = ev->get("cwd")) cwd = c->asString();
                    std::string commandLine;
                    if (const JsonValue* c = ev->get("command_line")) {
                        commandLine = c->asString();
                    }
                    std::string executable;
                    if (const JsonValue* c = ev->get("executable")) {
                        executable = c->asString();
                    }

                    std::vector<std::string> useArgv;
                    if (const JsonValue* a = ev->get("argv")) {
                        useArgv = a->asStringArray();
                    }
                    if (useArgv.empty() && !commandLine.empty()) {
                        useArgv = ccap::splitCompilerCommandLine(commandLine);
                    }
                    if (useArgv.empty() && !executable.empty()) {
                        useArgv.push_back(executable);
                    }

                    if (!useArgv.empty()) {
                        CompileCommand cc = parseArgv(useArgv, cwd);
                        cc.rawCommand = commandLine;
                        out.push_back(cc);
                    }
                }
            }
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    return out;
}

}  // namespace compdb
}  // namespace ccap
