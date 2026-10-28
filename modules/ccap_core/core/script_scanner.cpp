// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

#include "core/script_scanner.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

#include "core/command_line.h"
#include "core/normalize.h"
#include "core/powershell_scanner.h"

namespace ccap {

namespace {

// cmd.exe command separators: & | && ||
// sh separators: ; & && || |  and newline
// PowerShell: ;  and newline

bool isShellSeparator(char c) {
    return c == ';' || c == '\n';
}

bool isRedirectChar(char c) {
    return c == '>' || c == '<' || c == '|';
}

// Strip a trailing comment
// - shell: a leading # (outside quotes)
// - cmd: rem / ::
std::string stripComment(const std::string& line, bool cmdStyle) {
    bool inS = false, inD = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\\' && i + 1 < line.size()) { ++i; continue; }
        if (c == '\'' && !inD) { inS = !inS; continue; }
        if (c == '"' && !inS) { inD = !inD; continue; }
        if (inS || inD) continue;

        if (cmdStyle) {
            if (c == ':' && i + 1 < line.size() && line[i + 1] == ':') {
                return line.substr(0, i);
            }
        } else {
            if (c == '#') {
                // A # only starts a comment when it follows whitespace
                if (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1]))) {
                    return line.substr(0, i);
                }
            }
        }
    }
    return line;
}

// cmd REM comment detection
//
// REM is only a comment at the **start of the line (after skipping leading
// whitespace)**, and:
//   1. it is **case-insensitive** --- REM / Rem / rem all count as a
//      comment
//   2. it must be followed by a separator (whitespace / & / | / < / > / =
//      etc.) or the end of the line
//      otherwise `remote` is a real command and must not be treated as a
//      comment
//
// Implementation note:
//   Previously this was decided *during* the character-by-character scan,
//   with the condition written as
//   "the start position after skipping leading whitespace == 0" ---
//   but that position is already 3 by the time the third character has been
//   scanned, so the condition **never held** and REM comments were never
//   recognized.
//
//   The right approach is to **judge the whole line independently**: find
//   the first non-whitespace character of the line and check whether it is
//   rem.
bool isCmdRemComment(const std::string& line) {
    size_t start = 0;
    while (start < line.size() &&
           std::isspace(static_cast<unsigned char>(line[start]))) {
        ++start;
    }
    if (start + 3 > line.size()) return false;

    if (std::tolower(static_cast<unsigned char>(line[start])) != 'r') return false;
    if (std::tolower(static_cast<unsigned char>(line[start + 1])) != 'e') return false;
    if (std::tolower(static_cast<unsigned char>(line[start + 2])) != 'm') return false;

    // It must be followed by a separator or the end of the line
    if (start + 3 == line.size()) return true;
    char next = line[start + 3];
    if (std::isspace(static_cast<unsigned char>(next))) return true;
    // cmd metacharacters count as separators too
    switch (next) {
        case '&': case '|': case '<': case '>': case '=':
        case '(': case ')': case ',': case ';':
            return true;
        default:
            break;
    }
    return false;
}

// Split a line on separators, but keep connectors like & | as the boundary
// of a standalone token
// Returns a number of "command segments"
std::vector<std::string> splitCommandSegments(const std::string& line,
                                                bool cmdStyle) {
    std::vector<std::string> segs;
    std::string cur;
    bool inS = false, inD = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];

        if (c == '\\' && !cmdStyle && i + 1 < line.size()) {
            cur += c;
            cur += line[i + 1];
            ++i;
            continue;
        }
        if (c == '^' && cmdStyle && i + 1 < line.size()) {
            // cmd escape character
            cur += line[i + 1];
            ++i;
            continue;
        }
        if (c == '\'' && !inD) { inS = !inS; cur += c; continue; }
        if (c == '"' && !inS) { inD = !inD; cur += c; continue; }

        if (!inS && !inD) {
            if (isShellSeparator(c)) {
                if (!cur.empty()) segs.push_back(cur);
                cur.clear();
                continue;
            }
            if (cmdStyle && (c == '&' || c == '|')) {
                if (!cur.empty()) segs.push_back(cur);
                cur.clear();
                // Skip && or ||
                if (i + 1 < line.size() && line[i + 1] == c) ++i;
                continue;
            }
            if (!cmdStyle && c == '&' && i + 1 < line.size() && line[i + 1] == '&') {
                if (!cur.empty()) segs.push_back(cur);
                cur.clear();
                ++i;
                continue;
            }
        }
        cur += c;
    }
    if (!cur.empty()) segs.push_back(cur);
    return segs;
}

// Strip the redirection part (> file, < file, 2>&1, >> file)
std::string stripRedirections(const std::string& seg) {
    std::string out;
    bool inS = false, inD = false;
    for (size_t i = 0; i < seg.size(); ++i) {
        char c = seg[i];
        if (c == '\'' && !inD) { inS = !inS; out += c; continue; }
        if (c == '"' && !inS) { inD = !inD; out += c; continue; }
        if (!inS && !inD && isRedirectChar(c)) {
            // Truncate up to the next separator
            size_t j = i;
            while (j < seg.size() && !isShellSeparator(seg[j])) ++j;
            std::string tail = seg.substr(j);
            out += tail;
            return out;
        }
        out += c;
    }
    return out;
}

// Decide whether a segment is a control structure (if / for / while / goto
// / label / function definition)
// These do not produce a command execution
// Does the segment start with the given keyword (case insensitive)?
//
// The keyword has to be a **whole word**: "forx" is not "for".
bool startsWithKeyword(const std::string& seg, const char* keyword) {
    size_t i = 0;
    while (i < seg.size() && std::isspace(static_cast<unsigned char>(seg[i]))) ++i;
    size_t len = std::strlen(keyword);
    if (seg.size() < i + len) return false;
    for (size_t k = 0; k < len; ++k) {
        if (std::tolower(static_cast<unsigned char>(seg[i + k])) !=
            std::tolower(static_cast<unsigned char>(keyword[k]))) {
            return false;
        }
    }
    // The next character must be a separator (or end of string).
    if (i + len == seg.size()) return true;
    char next = seg[i + len];
    return std::isspace(static_cast<unsigned char>(next)) || next == '(' ||
           next == '%';
}

// The body of a cmd "for" loop: the part after "do".
//
//     for %i in (a b) do echo loop   ->  "echo loop"
//     for %%i in (x) do @call sub    ->  "@call sub"
//
// Returns "" when there is no "do" (a malformed or multi-line loop).
std::string cmdForBody(const std::string& seg) {
    // Find the "do" keyword.
    //
    // It has to be found **outside** the parentheses: the iteration list
    // may legitimately contain the letters "do" ("for %i in (do) do echo x").
    size_t depth = 0;
    bool inS = false, inD = false;
    for (size_t i = 0; i + 1 < seg.size(); ++i) {
        char c = seg[i];
        if (c == '\'' && !inD) { inS = !inS; continue; }
        if (c == '"' && !inS) { inD = !inD; continue; }
        if (inS || inD) continue;
        if (c == '(') { ++depth; continue; }
        if (c == ')') { if (depth > 0) --depth; continue; }
        if (depth != 0) continue;
        // A "do" at a word boundary.
        //
        // "do" is **two** letters: d + o. The check has to be
        //   seg[i]   == 'd'
        //   seg[i+1] == 'o'
        //   seg[i+2] is a separator (or end of string)
        //
        // Getting that off by one (looking for a third 'd') makes the
        // whole thing silently never match.
        if ((c == 'd' || c == 'D') &&
            (i == 0 || std::isspace(static_cast<unsigned char>(seg[i - 1])))) {
            size_t after = i + 2;
            if (after < seg.size() &&
                std::tolower(static_cast<unsigned char>(seg[i + 1])) == 'o' &&
                (after + 1 == seg.size() ||
                 std::isspace(static_cast<unsigned char>(seg[after])))) {
                std::string body = seg.substr(after);
                size_t b = 0;
                while (b < body.size() &&
                       std::isspace(static_cast<unsigned char>(body[b]))) {
                    ++b;
                }
                return body.substr(b);
            }
        }
    }
    return "";
}

bool isControlStructure(const std::string& seg, bool cmdStyle) {
    std::string s = seg;
    // Strip leading whitespace
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    s = s.substr(i);
    if (s.empty()) return true;

    static const std::set<std::string> shControl = {
        "if", "then", "else", "elif", "fi", "for", "while", "until", "do",
        "done", "case", "esac", "in", "function", "select", "time", "{", "}",
        "!", "[[", "]]",
    };
    static const std::set<std::string> cmdControl = {
        "if", "else", "for", "in", "do", "goto", "call", "exit",
    };

    std::string first;
    {
        size_t sp = s.find_first_of(" \t");
        first = (sp == std::string::npos) ? s : s.substr(0, sp);
    }
    std::string lower;
    for (char c : first) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (cmdStyle) {
        if (cmdControl.count(lower)) return true;
        // label:  :label
        if (!s.empty() && s[0] == ':') return true;
    } else {
        if (shControl.count(lower)) return true;
    }
    return false;
}

// Extract the command name and arguments from a segment
void extractCommand(const std::string& seg,
                     bool cmdStyle,
                     std::vector<std::string>* argvOut) {
    std::string s = seg;

    // Strip leading whitespace
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    s = s.substr(i);

    // cmd: strip the "call " prefix but keep it (call creates a new
    // context)
    // sh: handle the "env VAR=1 cmd" prefix
    if (!cmdStyle) {
        // Skip prefixes such as env / command / exec / nohup / sudo
        for (int guard = 0; guard < 8; ++guard) {
            std::string lower;
            for (char c : s) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            static const std::set<std::string> prefixes = {
                "env", "command", "exec", "nohup", "time", "nice", "ionice",
                "stdbuf", "setsid",
            };
            size_t sp = s.find_first_of(" \t");
            std::string w = (sp == std::string::npos) ? s : s.substr(0, sp);
            std::string wl;
            for (char c : w) wl += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!prefixes.count(wl)) break;
            s = s.substr(sp == std::string::npos ? s.size() : sp + 1);
            // Skip the VAR=1 assignments that follow env
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s[0]))) s.erase(0, 1);
            while (!s.empty() && s.find('=') != std::string::npos &&
                   s.find('=') < s.find_first_of(" \t") &&
                   s.find('=') != std::string::npos) {
                size_t eq = s.find('=');
                size_t sp2 = s.find_first_of(" \t", eq);
                if (sp2 == std::string::npos) { s.clear(); break; }
                s = s.substr(sp2 + 1);
            }
            if (s.empty()) break;
        }
    }

    // Strip redirections
    s = stripRedirections(s);

    if (cmdStyle) {
        // cmd: handle the @ prefix (no echo)
        size_t k = 0;
        while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
        if (k < s.size() && s[k] == '@') {
            s = s.substr(k + 1);
        }
    }

    std::vector<std::string> parts = cmdStyle
        ? splitWindowsCommandLine(s)
        : splitShellLine(s);

    if (parts.empty()) return;

    // Strip the assignment prefix (VAR=value cmd)
    if (!cmdStyle) {
        size_t idx = 0;
        while (idx < parts.size() && parts[idx].find('=') != std::string::npos &&
               parts[idx].find('=') > 0) {
            ++idx;
        }
        if (idx < parts.size()) {
            parts.erase(parts.begin(), parts.begin() + static_cast<long>(idx));
        }
    }

    if (parts.empty()) return;
    *argvOut = parts;
}

// Decide whether a segment references a script (used for recursive
// scanning)
// The first argument is taken as the script path
bool extractScriptRef(const std::vector<std::string>& argv,
                        std::string* out) {
    if (argv.size() < 2) return false;
    std::string cand = argv[1];
    if (cand.empty()) return false;
    std::string ext = extensionOf(cand);
    static const std::set<std::string> scriptExts = {
        ".bat", ".cmd", ".ps1", ".sh", ".bash", ".zsh", ".py", ".pl", ".rb",
    };
    if (!scriptExts.count(ext)) return false;
    *out = cand;
    return true;
}

void emitEvent(const std::string& scriptPath,
                int lineNo,
                const std::vector<std::string>& argv,
                const std::string& cwd,
                const ScanOptions& opts,
                EventCollector* collector) {
    if (!collector) return;
    if (argv.empty()) return;

    std::string exe = argv[0];
    bool builtin = looksLikeShellBuiltin(exe);

    std::string resolvedPath;
    bool resolved = false;
    // **Builtin commands are not path-resolved.**
    //
    // `echo` in cmd is a builtin command and does not create a process at
    // all. But there may really be an echo.exe on PATH (w64devkit / MSYS
    // both ship one), and resolving it would produce
    // "resolved=D:/.../echo.exe" --- making it look as if echo were an
    // external program.
    //
    // For a builtin command `resolved` is always false: there is no such
    // thing as an "executable file path" for it.
    if (opts.resolvePaths && !builtin) {
        resolvedPath = resolveExecutable(exe);
        if (!resolvedPath.empty()) {
            resolved = true;
            // Keep the original name, but record the resolution result
        }
    }

    CommandEvent e;
    e.timestampMs = nowUnixMs();
    // A command found by static analysis still belongs to the current
    // platform --- it used to be Unknown, which made downstream filtering
    // by platform lose the whole layer.
#if defined(_WIN32)
    e.platform = Platform::Windows;
#elif defined(__linux__)
    e.platform = Platform::Linux;
#else
    e.platform = Platform::Unknown;
#endif
    e.source = EventSource::Static;
    e.pid = 0;
    e.ppid = 0;
    e.executable = exe;
    e.argv = argv;
    e.cwd = cwd;
    e.scriptPath = scriptPath;
    e.scriptLine = lineNo;
    e.kind = builtin ? "builtin" : "command";
    e.isBuiltin = builtin;
    e.resolved = resolved;
    if (resolved && !resolvedPath.empty()) {
        e.notes.push_back("resolved=" + resolvedPath);
    }
    collector->add(std::move(e));
}

}  // namespace

void scanScriptFile(const std::string& path,
                      const ScanOptions& opts,
                      EventCollector* collector,
                      int depth) {
    if (depth > opts.maxDepth) return;

    std::string content;
    if (!readFile(path, &content)) return;
    if (static_cast<int>(content.size()) > opts.maxFileSize) return;

    std::string ext = extensionOf(path);
    bool cmdStyle = (ext == ".bat" || ext == ".cmd");
    bool psStyle = (ext == ".ps1");
    std::string cwd = dirName(path);

    // Process line by line, accumulating logical lines (cmd's ^ continuation,
    // sh's \ continuation)
    std::string logical;
    int logicalStartLine = 1;
    int lineNo = 0;
    std::set<std::string> visited;

    auto flush = [&](const std::string& line, int startLine) {
        // Skip a REM comment line entirely.
        // This has to be checked **before** strip_comment ---
        // because strip_comment only handles a trailing :: and #,
        // whereas REM means "the whole line is a comment".
        if (cmdStyle && isCmdRemComment(line)) return;

        std::string stripped = stripComment(line, cmdStyle);
        // PowerShell uses # comments
        if (psStyle) {
            stripped = stripComment(line, false);
        }
        if (stripped.empty()) return;

        std::vector<std::string> segs = splitCommandSegments(stripped, cmdStyle);
        for (const auto& seg : segs) {
            if (seg.empty()) continue;
            if (isControlStructure(seg, cmdStyle)) {
                // A control structure may contain commands, e.g.:
                // if x (echo hi)
                // cmd: the parenthesized part holds commands too
                //
                // **But not for "for".** In
                //     for %i in (a b) do echo loop
                // the parentheses hold the **iteration list**, not commands.
                // Extracting them as a command would report "a b" as a
                // command and lose the actual command ("echo loop").
                //
                // So a "for" segment is handled by its own path below.
                if (cmdStyle && !startsWithKeyword(seg, "for")) {
                    size_t lp = seg.find('(');
                    if (lp != std::string::npos) {
                        size_t rp = seg.rfind(')');
                        if (rp > lp) {
                            std::string inner = seg.substr(lp + 1, rp - lp - 1);
                            std::vector<std::string> iargv;
                            extractCommand(inner, cmdStyle, &iargv);
                            if (!iargv.empty()) {
                                emitEvent(path, startLine, iargv, cwd, opts, collector);
                            }
                        }
                    }
                }
                // cmd "for": the command is the part **after "do"**.
                //   for %i in (a b) do echo loop   ->  echo loop
                //   for %%i in (x) do @call sub    ->  call sub
                if (cmdStyle) {
                    std::string doPart = cmdForBody(seg);
                    if (!doPart.empty()) {
                        std::vector<std::string> dargv;
                        extractCommand(doPart, cmdStyle, &dargv);
                        if (!dargv.empty()) {
                            emitEvent(path, startLine, dargv, cwd, opts, collector);
                        }
                    }
                }
                continue;
            }

            std::vector<std::string> argv;
            extractCommand(seg, cmdStyle, &argv);
            if (argv.empty()) continue;

            emitEvent(path, startLine, argv, cwd, opts, collector);

            // Recursion: a script invokes another script
            if (opts.recursive) {
                std::string ref;
                if (extractScriptRef(argv, &ref)) {
                    std::string target = ref;
                    if (!isAbsolutePath(target)) {
                        std::string cand = normalizePath(cwd + "/" + target);
                        if (fileExists(cand)) {
                            target = cand;
                        } else {
                            target = resolveExecutable(target);
                        }
                    }
                    if (!target.empty() && visited.insert(target).second) {
                        scanScriptFile(target, opts, collector, depth + 1);
                    }
                }
            }
        }
    };

    // Split into lines
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : content) {
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else if (c != '\r') {
                cur += c;
            }
        }
        if (!cur.empty()) lines.push_back(cur);
    }

    for (const auto& raw : lines) {
        ++lineNo;
        std::string line = raw;

        bool continued = false;
        if (!line.empty()) {
            char last = line.back();
            if (cmdStyle && last == '^') {
                line.pop_back();
                continued = true;
            } else if (!cmdStyle && last == '\\') {
                line.pop_back();
                continued = true;
            }
        }

        if (!logical.empty()) {
            logical += ' ';
            logical += line;
        } else {
            logical = line;
            logicalStartLine = lineNo;
        }

        if (!continued) {
            flush(logical, logicalStartLine);
            logical.clear();
        }
    }
    if (!logical.empty()) {
        flush(logical, logicalStartLine);
    }
}

void scanScript(const std::string& path,
                const ScanOptions& opts,
                EventCollector* collector,
                int depth) {
    std::string ext = extensionOf(path);

    // PowerShell has its own syntax (cmdlets, aliases, script blocks,
    // pipelines), so it goes through the dedicated scanner.
    if (ext == ".ps1" || ext == ".psm1" || ext == ".psd1") {
        PsScanOptions po;
        po.resolvePaths = opts.resolvePaths;
        scanPowershell(path, po, collector, depth);
        return;
    }

    if (ext == ".bat" || ext == ".cmd" || ext == ".sh" ||
        ext == ".bash" || ext == ".zsh") {
        scanScriptFile(path, opts, collector, depth);
    }
}

}  // namespace ccap
