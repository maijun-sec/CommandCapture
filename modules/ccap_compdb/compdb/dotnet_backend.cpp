// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - the .NET backend
//
// See ccap_compdb_dotnet.h for the design rationale.

#include "ccap_compdb_dotnet.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/normalize.h"

namespace ccap {
namespace compdb {

namespace {

// ── The encoding ─────────────────────────────────────────────────────────
//
// MSBuild writes the diagnostic log as UTF-16LE (with a BOM) when its
// stdout is redirected, and as UTF-8 when it goes to a console.
//
// Reading it as the wrong one does not raise an error: it silently
// produces garbage, and the ASCII runs can be lost entirely. The
// symptom is "the log contains no compile command", which is a
// completely wrong conclusion -- and it is silent, so nothing tells
// you to look at the encoding.
//
// So the encoding is detected from the BOM, and a log without a BOM is
// read as UTF-8.
std::string readLogText(const std::string& path, bool* ok) {
    *ok = false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();

    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string raw = buffer.str();
    if (raw.empty()) return std::string();

    // UTF-16LE BOM.
    if (raw.size() >= 2 &&
        static_cast<unsigned char>(raw[0]) == 0xFF &&
        static_cast<unsigned char>(raw[1]) == 0xFE) {
        // The bytes come in pairs; a trailing odd byte is dropped.
        std::string out;
        out.reserve(raw.size() / 2);
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            const unsigned char lo = static_cast<unsigned char>(raw[i]);
            const unsigned char hi = static_cast<unsigned char>(raw[i + 1]);
            out.push_back(static_cast<char>(lo | (hi << 8)));
        }
        *ok = true;
        return out;
    }

    // UTF-16BE BOM.
    if (raw.size() >= 2 &&
        static_cast<unsigned char>(raw[0]) == 0xFE &&
        static_cast<unsigned char>(raw[1]) == 0xFF) {
        std::string out;
        out.reserve(raw.size() / 2);
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            const unsigned char hi = static_cast<unsigned char>(raw[i]);
            const unsigned char lo = static_cast<unsigned char>(raw[i + 1]);
            out.push_back(static_cast<char>(lo | (hi << 8)));
        }
        *ok = true;
        return out;
    }

    // UTF-8 BOM.
    if (raw.size() >= 3 &&
        static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
        *ok = true;
        return raw.substr(3);
    }

    *ok = true;
    return raw;
}

// ── The log line ─────────────────────────────────────────────────────────
//
// The diagnostic log line looks like:
//
//   11:07:33.438   1:7>C:\Program Files\dotnet\dotnet.exe exec "..."
//
// So there is a timestamp prefix, a "verbosity> " marker, the command
// line, and -- at the very end -- an annotation:
//
//   ... (任务 ID: 104)
//
// The annotation is MSBuild's, not the command's, and it has to go.
//
// WHY THE TIMESTAMP PREFIX MATTERS
//
// The prefix is "11:07:33.438   1:7>" -- the ">" is what separates it
// from the command. A line that starts with a ">" would otherwise be
// mistaken for a quote.
bool stripLogPrefix(const std::string& line, std::string* out) {
    const size_t gt = line.find('>');
    if (gt == std::string::npos) {
        *out = line;
        return false;
    }

    // The prefix is only a prefix if everything before the ">" is
    // digits, dots and colons -- that is what a timestamp and the
    // verbosity counter look like. Anything else means the ">" belongs
    // to the command line itself.
    const std::string head = line.substr(0, gt);
    for (char c : head) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != ':' &&
            c != ' ' && c != '\t') {
            *out = line;
            return false;
        }
    }
    if (head.empty()) {
        *out = line;
        return false;
    }

    *out = line.substr(gt + 1);
    return true;
}

// Strip the trailing MSBuild annotation.
//
// The annotation is a parenthesised group at the end of the line that
// contains "ID:" -- in the English build it is "(Task ID: 104)" and in
// the Chinese one it is "(任务 ID: 104)". The "ID:" is the stable part
// across the localisations, so it is what is looked for.
//
// WHY IT MATTERS
//
// Left in, it becomes part of the last argument, and the last
// argument of a Csc command line is usually a **source file**. The
// compiler would then be asked to compile a file whose name ends with
// "104)" -- which does not exist.
std::string stripTaskAnnotation(const std::string& line) {
    // The annotation is at the end. Walk back to the matching '('.
    //
    // The trim has to include the **carriage return**: the log uses
    // CRLF line endings, so the line that reaches here ends with
    // "104)\r". Trimming only spaces and tabs would leave the '\r' as
    // the last character, the ')' check would fail, and the annotation
    // would stay on the command line -- where it becomes part of the
    // last source file's name.
    size_t end = line.size();
    while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                       line[end - 1] == '\r')) {
        --end;
    }
    if (end == 0 || line[end - 1] != ')') return line;

    // The search has to be **bounded**.
    //
    // find_last_of(ch, pos) searches BACKWARDS from `pos`, which is
    // what is wanted here. (rfind would not do: rfind(sub, start)
    // searches from `start` to the END of the string, so it would
    // only look at the last character and would report "not found"
    // even though the '(' is right there, a few characters back.)
    const size_t open = line.find_last_of('(', end - 1);
    if (open == std::string::npos) return line;

    const std::string inner = line.substr(open + 1, end - open - 2);
    if (inner.find("ID:") == std::string::npos) return line;

    // Keep the text before the annotation, without the trailing space.
    std::string head = line.substr(0, open);
    while (!head.empty() && (head.back() == ' ' || head.back() == '\t' ||
                             head.back() == '\r')) {
        head.pop_back();
    }
    return head;
}

// Case-insensitive ASCII compare.
//
// The compiler dll name has to be matched case-insensitively: the file
// on disk is "csc.dll", but the log may spell it differently on a
// case-sensitive file system.
bool iequalsAscii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// The language a compiler dll implements.
struct DriverInfo {
    const char* dll;        // the file name of the compiler
    const char* language;   // the language it compiles
    CompilerKind kind;
};

const DriverInfo kDrivers[] = {
    { "csc.dll", "C#",           CompilerKind::Csharp },
    { "vbc.dll", "Visual Basic", CompilerKind::VbNet  },
    { "fsc.dll", "F#",           CompilerKind::FSharp },
};

// Find the driver in an argv.
//
// The shape is always "dotnet exec <path>/csc.dll ...".
const DriverInfo* findDriver(const std::vector<std::string>& argv) {
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        for (const auto& d : kDrivers) {
            const std::string b = baseName(a);
            if (b.size() == std::strlen(d.dll) && iequalsAscii(b, d.dll)) {
                return &d;
            }
        }
    }
    return nullptr;
}

// The options whose value is a PATH.
//
// These need a special treatment when the command is handed to the
// compiler: csc splits an **unquoted** argument on a comma, and the
// .NET assembly attribute file names always contain one:
//
//     .NETCoreApp,Version=v9.0.AssemblyAttributes.cs
//
// So the option and the path have to be given to csc as TWO argv
// tokens, and the comma never reaches csc's own splitter.
//
// WHY ONLY THESE
//
// The options that take a comma-separated LIST -- /warnaserror+ and
// /nowarn -- must stay ONE token, because splitting them breaks the
// list into separate arguments. So the distinction is not "has a
// comma", it is "the value is a path".
const char* kPathValueOptions[] = {
    "/embed:",
    "/analyzer:",
    "/reference:",
    "/r:",
};

// Does this argument carry a path value?
const char* pathValueOption(const std::string& arg) {
    for (const char* opt : kPathValueOptions) {
        const size_t n = std::strlen(opt);
        if (arg.size() > n && arg.compare(0, n, opt) == 0) return opt;
    }
    return nullptr;
}

// The output file the task produced.
std::string outputOf(const std::vector<std::string>& argv) {
    for (const auto& a : argv) {
        // "/out:" is five characters.
        if (a.size() > 5 && a.compare(0, 5, "/out:") == 0) {
            return a.substr(5);
        }
    }
    return std::string();
}

// The source files the task compiled.
//
// The Csc task passes them with /embed:<file>. The Visual Basic and F#
// drivers spell it the same way.
std::vector<std::string> sourcesOf(const std::vector<std::string>& argv) {
    std::vector<std::string> out;
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& a = argv[i];

        // "/embed:" is seven characters.
        if (a.size() > 7 && a.compare(0, 7, "/embed:") == 0) {
            out.push_back(a.substr(7));
            continue;
        }

        // "/embed:" followed by the file in the next token.
        if (a == "/embed:" && i + 1 < argv.size()) {
            out.push_back(argv[++i]);
        }
    }
    return out;
}

// The directory the compile ran in.
//
// The Csc command line uses **relative** paths for the sources and the
// output, so the directory is not optional: without it, "Hello.cs"
// cannot be resolved.
//
// The MSBuild log records it as the "MSBuildProjectDirectory"
// property, and that is the authoritative answer -- it is the project's
// own directory, not the directory the build was started from.
//
// WHY NOT THE BUILD'S WORKING DIRECTORY
//
//   Because they are different. A build started at the repository root
//   compiles a project in a subdirectory, and the compiler runs with
//   the **project** directory as its working directory. Using the
//   build's directory would put every source file in the wrong place.
//
//   Measured: the build ran from the repository root and the compile
//   was of ".trash/poly/cs/Hello.cs"; resolving it against the build's
//   directory gave ".../CommandCapture/Hello.cs", which does not
//   exist.
std::string projectDirectoryOf(const std::string& logText) {
    const std::string key = "MSBuildProjectDirectory = ";
    size_t at = logText.find(key);
    if (at == std::string::npos) return std::string();

    at += key.size();
    // The value runs to the end of the line.
    size_t end = logText.find('\n', at);
    if (end == std::string::npos) end = logText.size();
    std::string value = logText.substr(at, end - at);

    // Trim the whitespace the log puts around the value.
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

}  // namespace

// ── The Windows command line split ───────────────────────────────────────

std::vector<std::string> splitWindowsCommandLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inQuotes = false;
    // Whether the current argument was **started** -- by a quote or by
    // a character.
    //
    // It matters for the empty argument: "" is a real argument (an
    // empty path), and dropping it would change what the command means.
    bool started = false;
    size_t i = 0;
    const size_t n = line.size();

    while (i < n) {
        const char c = line[i];

        if (c == '\\') {
            // Count the backslash run.
            const size_t start = i;
            while (i < n && line[i] == '\\') ++i;
            const size_t run = i - start;

            if (i < n && line[i] == '"') {
                // The run of backslashes before a quote:
                //
                //   * an EVEN run (2, 4, ...) -- every backslash is
                //     literal and the quote is a DELIMITER;
                //   * an ODD run (1, 3, ...) -- the backslashes pair up
                //     and the last one escapes the quote, so the quote
                //     is LITERAL.
                //
                // So the number of literal backslashes is the whole run
                // in the even case, and half of it in the odd case.
                // Getting this wrong turns a path like "C:\\dir\\file"
                // into a different path.
                started = true;
                if (run % 2 == 0) {
                    cur.append(run, '\\');
                    inQuotes = !inQuotes;   // the quote is a delimiter
                } else {
                    cur.append(run / 2, '\\');
                    cur.push_back('"');      // the quote is literal
                }
                ++i;
            } else {
                cur.append(run, '\\');
                started = true;
            }
            continue;
        }

        if (c == '"') {
            if (inQuotes && i + 1 < n && line[i + 1] == '"') {
                cur.push_back('"');
                started = true;
                i += 2;
                continue;
            }
            // The quote opens or closes a quoted run; either way it
            // **starts** an argument, so "" produces an empty one.
            started = true;
            inQuotes = !inQuotes;
            ++i;
            continue;
        }

        if (!inQuotes && (c == ' ' || c == '\t')) {
            if (started) {
                out.push_back(cur);
                cur.clear();
                started = false;
            }
            ++i;
            continue;
        }

        cur.push_back(c);
        started = true;
        ++i;
    }

    if (started) out.push_back(cur);
    return out;
}

// ── The extraction ───────────────────────────────────────────────────────

bool extractDotnetCompiles(const std::string& logPath,
                           std::vector<DotnetCompile>* out,
                           std::string* error) {
    if (!out) return false;
    out->clear();

    bool readOk = false;
    const std::string text = readLogText(logPath, &readOk);
    if (!readOk) {
        if (error) *error = "cannot read the diagnostic log: " + logPath;
        return false;
    }

    std::istringstream stream(text);
    std::string line;
    bool sawAnyTask = false;

    // The directory the compile ran in.
    //
    // It is read once, from the log, and applied to every compile the
    // log contains: a build log is one build, so one directory.
    const std::string projectDir = projectDirectoryOf(text);

    while (std::getline(stream, line)) {
        if (line.find("csc.dll") == std::string::npos &&
            line.find("vbc.dll") == std::string::npos &&
            line.find("fsc.dll") == std::string::npos) {
            continue;
        }
        if (line.find("/noconfig") == std::string::npos) {
            continue;
        }

        // The log prefix, then the task annotation.
        std::string body;
        stripLogPrefix(line, &body);
        body = stripTaskAnnotation(body);

        std::vector<std::string> argv = splitWindowsCommandLine(body);
        if (argv.empty()) continue;

        // The executable path is logged **unquoted**, even when it
        // contains a space:
        //
        //     C:\Program Files\dotnet\dotnet.exe exec "..."
        //
        // So the first two tokens have to be joined back together.
        // The test is "does the joined name exist" -- the path is
        // known to be a real file, so the joined form is the right one.
        if (argv.size() >= 2 && !fileExists(argv[0])) {
            const std::string joined = argv[0] + " " + argv[1];
            if (fileExists(joined)) {
                argv.erase(argv.begin());
                argv[0] = joined;
            }
        }

        const DriverInfo* driver = findDriver(argv);
        if (!driver) continue;

        sawAnyTask = true;

        DotnetCompile c;
        c.argv = argv;
        c.language = driver->language;
        c.directory = projectDir;
        for (const auto& a : argv) {
            if (baseName(a).size() == std::strlen(driver->dll) &&
                iequalsAscii(baseName(a), driver->dll)) {
                c.compilerPath = a;
                break;
            }
        }
        out->push_back(c);
    }

    if (!sawAnyTask) {
        if (error) {
            *error = "no compiler task found in the diagnostic log";
        }
        return false;
    }
    return true;
}

// ── The conversion ───────────────────────────────────────────────────────

std::vector<CompileCommand> toCompileCommands(
    const std::vector<DotnetCompile>& compiles) {
    std::vector<CompileCommand> out;

    for (const auto& c : compiles) {
        CompileCommand cmd;

        // The compiler path and its base name.
        cmd.compilerPath = c.compilerPath;
        cmd.compilerBase = baseName(c.compilerPath);

        for (const auto& a : c.argv) {
            const std::string b = baseName(a);
            if (b == "csc.dll" || b == "vbc.dll" || b == "fsc.dll") {
                cmd.kind = b == "csc.dll" ? CompilerKind::Csharp
                         : b == "vbc.dll" ? CompilerKind::VbNet
                                          : CompilerKind::FSharp;
                break;
            }
        }

        cmd.argv = c.argv;
        cmd.directory = c.directory;

        // The sources.
        //
        // They are **relative** in the command line ("Hello.cs"), so
        // they are made absolute against the compile's directory.
        //
        // WHY
        //
        //   The compilation database format wants absolute paths: a
        //   consumer reads the file from anywhere, and the relative
        //   path only means something in the directory the build ran
        //   in.
        cmd.sourceFiles = sourcesOf(c.argv);
        for (auto& f : cmd.sourceFiles) {
            if (!f.empty() && !isAbsolutePath(f) && !c.directory.empty()) {
                f = normalizePath(c.directory + "/" + f);
            }
        }

        // The output.
        cmd.outputFile = outputOf(c.argv);

        // The references and the analyzers.
        //
        // The two are different things and they go to different places:
        // a reference is an assembly the compiler reads types from, an
        // analyzer is a source generator that runs alongside the
        // compile. A consumer that has to look a type up in an
        // assembly cannot afford to have the two mixed.
        for (size_t i = 0; i < c.argv.size(); ++i) {
            const std::string& a = c.argv[i];

            // The option and its value, either joined ("/r:x") or
            // separate ("/r:" "x").
            std::string value;
            bool isReference = false;
            bool isAnalyzer = false;

            // The prefix test is a **length-safe** one.
            //
            // Comparing a fixed number of characters against the option
            // string is a trap: "/reference:" is ELEVEN characters and
            // "/analyzer:" is TEN, so a test written for one of them
            // silently never matches the other. Nothing reports that --
            // the option is just "not recognized", and the reference
            // set comes out empty.
            auto startsWith = [&a](const char* prefix) -> bool {
                const size_t n = std::strlen(prefix);
                return a.size() > n && a.compare(0, n, prefix) == 0;
            };
            auto isExactly = [&a](const char* text) -> bool {
                return a == text;
            };

            if (startsWith("/reference:")) {
                value = a.substr(11);
                isReference = true;
            } else if (startsWith("/r:")) {
                value = a.substr(3);
                isReference = true;
            } else if (isExactly("/reference:") || isExactly("/r:")) {
                isReference = true;
                if (i + 1 < c.argv.size()) value = c.argv[++i];
            } else if (startsWith("/analyzer:")) {
                value = a.substr(10);
                isAnalyzer = true;
            } else if (isExactly("/analyzer:")) {
                isAnalyzer = true;
                if (i + 1 < c.argv.size()) value = c.argv[++i];
            } else {
                continue;
            }

            if (value.empty()) continue;

            // The log quotes the value when it contains a space; the
            // quotes are not part of the path.
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            if (value.empty()) continue;

            if (isReference) {
                cmd.classpathDirs.push_back(value);
            } else if (isAnalyzer) {
                cmd.processorDirs.push_back(value);
            }
        }

        out.push_back(cmd);
    }

    return out;
}

// ── The command recognition ──────────────────────────────────────────────

bool isMsBuildCommand(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;

    const std::string base = baseName(argv[0]);
    if (base == "dotnet" || base == "dotnet.exe") return true;
    if (base == "msbuild" || base == "msbuild.exe") return true;
    if (base == "devenv" || base == "devenv.exe") return true;

    // The "dotnet" host may be spelled with a path.
    for (const auto& a : argv) {
        const std::string b = baseName(a);
        if (b == "dotnet" || b == "dotnet.exe") return true;
    }
    return false;
}

}  // namespace compdb
}  // namespace ccap
