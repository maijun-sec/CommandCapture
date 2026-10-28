// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

#include "core/command_line.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace ccap {

namespace {

// Test whether the character is whitespace under the CRT rules (space or tab)
inline bool isCrtSpace(char c) {
    return c == ' ' || c == '\t';
}

}  // namespace

std::vector<std::string> splitWindowsCommandLine(const std::string& cmdline) {
    std::vector<std::string> args;
    if (cmdline.empty()) return args;

    size_t i = 0;
    const size_t n = cmdline.size();

    while (i < n) {
        // Skip leading whitespace
        while (i < n && isCrtSpace(cmdline[i])) ++i;
        if (i >= n) break;

        std::string cur;
        bool inQuotes = false;
        bool hasToken = false;

        while (i < n) {
            char c = cmdline[i];

            if (!inQuotes && isCrtSpace(c)) break;

            if (c == '\\') {
                // Backslash: count the consecutive run
                //
                // CRT rule (only the parity of the backslash count matters):
                //   2k   backslashes + "  => k backslashes + toggle quote state
                //   2k+1 backslashes + "  => k backslashes + a literal "
                //   2k   backslashes + other => k backslashes, the character
                //                                   itself is handled normally
                //   2k+1 backslashes + other => k+1 backslashes, the character
                //                                     itself is handled normally
                //
                // Note the last rule: **a single backslash in front of a
                // non-quote character is a literal**. Get this wrong and
                // "C:\Windows\x.exe" turns into "C:Windowsx.exe".
                size_t backslashes = 0;
                while (i < n && cmdline[i] == '\\') {
                    ++backslashes;
                    ++i;
                }

                if (i < n && cmdline[i] == '"') {
                    cur.append(backslashes / 2, '\\');
                    if (backslashes % 2 == 1) {
                        // Escaped quote: literal, does not toggle the quote state
                        cur += '"';
                        ++i;
                    } else {
                        inQuotes = !inQuotes;
                        ++i;
                    }
                } else {
                    // No quote follows: keep the backslashes as-is (one extra
                    // when the count is odd)
                    cur.append(backslashes, '\\');
                }
                hasToken = true;
                continue;
            }

            if (c == '"') {
                inQuotes = !inQuotes;
                ++i;
                hasToken = true;
                continue;
            }

            cur += c;
            ++i;
            hasToken = true;
        }

        if (hasToken || !cur.empty()) {
            args.push_back(cur);
        }
    }

    return args;
}

std::vector<std::string> splitShellLine(const std::string& line) {
    std::vector<std::string> args;
    size_t i = 0;
    const size_t n = line.size();

    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= n) break;

        std::string cur;
        bool hasToken = false;

        while (i < n && !std::isspace(static_cast<unsigned char>(line[i]))) {
            char c = line[i];

            if (c == '\'') {
                ++i;
                while (i < n && line[i] != '\'') {
                    cur += line[i];
                    ++i;
                }
                if (i < n) ++i;  // consume the closing single quote
                hasToken = true;
                continue;
            }

            if (c == '"') {
                ++i;
                while (i < n && line[i] != '"') {
                    if (line[i] == '\\' && i + 1 < n &&
                        (line[i + 1] == '"' || line[i + 1] == '\\' ||
                         line[i + 1] == '$' || line[i + 1] == '`')) {
                        cur += line[i + 1];
                        i += 2;
                    } else {
                        cur += line[i];
                        ++i;
                    }
                }
                if (i < n) ++i;  // consume the closing double quote
                hasToken = true;
                continue;
            }

            if (c == '\\' && i + 1 < n) {
                cur += line[i + 1];
                i += 2;
                hasToken = true;
                continue;
            }

            cur += c;
            ++i;
            hasToken = true;
        }

        if (hasToken || !cur.empty()) {
            args.push_back(cur);
        }
    }

    return args;
}

std::vector<std::string> splitCommandLineAuto(const std::string& line) {
    // ── Discriminate: Windows rules or POSIX rules? ─────────────────
    //
    // Using the wrong rules **silently drops content**:
    //   cl.exe /I"..\inc" /c a.cpp
    //   POSIX rules -> \i is treated as an escape -> "..inc"  (both the
    //                  backslash and the i are gone)
    //   Windows rules -> "..\inc" is preserved intact
    //
    // Discriminators (ordered by reliability):

    // 1) Windows-specific syntax
    bool hasWindowsSyntax = false;
    {
        // /I /D /U /std: /Fo /Fe /external:I /imsvc /openmp
        static const char* kWinFlags[] = {
            "/I", "/D", "/U", "/std:", "/Fo", "/Fe", "/external:",
            "/imsvc", "/openmp", "/Zp", "/GS", "/RTC",
        };
        for (const char* f : kWinFlags) {
            size_t n = std::char_traits<char>::length(f);
            // Find the token start
            size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() &&
                       std::isspace(static_cast<unsigned char>(line[i]))) ++i;
                if (i >= line.size()) break;
                size_t start = i;
                while (i < line.size() &&
                       !std::isspace(static_cast<unsigned char>(line[i]))) {
                    // Spaces inside quotes are not separators
                    if (line[i] == '"') {
                        ++i;
                        while (i < line.size() && line[i] != '"') ++i;
                    }
                    if (i < line.size()) ++i;
                }
                if (i - start >= n && line.compare(start, n, f) == 0) {
                    hasWindowsSyntax = true;
                    break;
                }
            }
            if (hasWindowsSyntax) break;
        }
    }

    // 2) There are backslashes, but they are **not** POSIX escape patterns
    //
    //    POSIX escape: backslash followed by " ' ` $ \ or space
    //    Windows path: C:\dir\file --- backslash followed by a letter
    bool hasBackslash = false;
    bool hasPosixEscape = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '\\') continue;
        hasBackslash = true;
        if (i + 1 < line.size()) {
            char nxt = line[i + 1];
            if (nxt == '"' || nxt == '\'' || nxt == '`' || nxt == '$' ||
                nxt == '\\' || std::isspace(static_cast<unsigned char>(nxt))) {
                hasPosixEscape = true;
            }
        }
    }

    // 3) .exe suffix (a strong signal for Windows tooling)
    bool hasExe = line.find(".exe") != std::string::npos;

    bool useWindows = hasWindowsSyntax || hasExe ||
                       (hasBackslash && !hasPosixEscape);

    if (useWindows) {
        return splitWindowsCommandLine(line);
    }
    return splitShellLine(line);
}

std::vector<std::string> splitCompilerCommandLine(const std::string& line) {
    // ─────────────────────────────────────────────────────────────────
    // Splitting rules for compiler command lines
    //
    // Difference from generic splitting: **quotes inside a macro
    // definition are literals**.
    //
    //   -DNAME="value"
    //
    // The quotes there are **part of the macro value** (they expand to the
    // string literal "value"), not grouping markers. The generic splitter
    // strips them and yields -DNAME=value --- the macro's value changed
    // from a string into an identifier, so **the semantics changed**, and
    // the change is not recoverable.
    //
    // Implementation: scan it ourselves instead of using the generic
    // splitter. Once the line has been split, the quote information is
    // already lost.
    //
    // Both Windows and POSIX separator styles are handled:
    //   - whitespace separation (identical in both)
    //   - quote grouping (different semantics; here treated uniformly as
    //     "literal quotes")
    //   - backslash escaping (on Windows a \ before an ordinary character
    //     is a literal)
    // ─────────────────────────────────────────────────────────────────

    std::vector<std::string> args;
    const size_t n = line.size();
    size_t i = 0;

    // Is the previous token -D / /D? (the separated form: -D NAME="v")
    bool prevIsDefineOpt = false;

    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= n) break;

        std::string cur;
        bool hasToken = false;
        bool inQuotes = false;

        // Is this token **as a whole** a macro definition?
        //
        //   -DNAME="value"   -> yes (the quotes are part of the macro value)
        //   -D "value"       -> no  (option and value are separated, the
        //                          next token is the value)
        //
        // Both cases must be handled: in the first the whole token is
        // literal, in the second the next token is literal.
        bool tokenIsDefine = false;
        {
            size_t j = i;
            if (j < n && (line[j] == '-' || line[j] == '/')) {
                ++j;
                if (j < n && (line[j] == 'D' || line[j] == 'd')) {
                    ++j;
                    // "-D" must be followed directly by content (not
                    // whitespace) for this to be the inline form
                    if (j < n && !std::isspace(static_cast<unsigned char>(line[j]))) {
                        tokenIsDefine = true;
                    }
                }
            }
        }

        while (i < n) {
            char c = line[i];

            if (!inQuotes && std::isspace(static_cast<unsigned char>(c))) break;

            if (c == '\\' && i + 1 < n) {
                // Backslash: on Windows an ordinary character after it is a
                // literal; on POSIX " ' ` $ \ escape the next character
                char nxt = line[i + 1];
                if (nxt == '"' || nxt == '\'' || nxt == '`' || nxt == '$' ||
                    nxt == '\\' || std::isspace(static_cast<unsigned char>(nxt))) {
                    cur += nxt;
                    i += 2;
                } else {
                    cur += c;
                    ++i;
                }
                hasToken = true;
                continue;
            }

            if (c == '"' || c == '\'') {
                if (prevIsDefineOpt || tokenIsDefine) {
                    // Macro definition value: the quote is a **literal**
                    cur += c;
                    ++i;
                    hasToken = true;
                    continue;
                }
                // Ordinary quote: a grouping marker, always in pairs
                if (!inQuotes) {
                    inQuotes = true;
                } else {
                    inQuotes = false;
                }
                ++i;
                hasToken = true;
                continue;
            }

            cur += c;
            ++i;
            hasToken = true;
        }

        if (hasToken || !cur.empty()) {
            // Decide whether this token is a macro definition option
            bool isDefineOpt = (cur == "-D" || cur == "/D");
            // the -DNAME=... / /DNAME=... form
            bool isDefineInline = false;
            if (!isDefineOpt && cur.size() > 2) {
                if (cur.compare(0, 2, "-D") == 0 ||
                    cur.compare(0, 2, "/D") == 0) {
                    // But "-D" must not be followed directly by a path
                    // separator (that would be a different option)
                    isDefineInline = true;
                }
            }

            args.push_back(cur);
            prevIsDefineOpt = isDefineOpt;
            (void)isDefineInline;
        }
    }

    return args;
}

std::string stripOuterQuotes(const std::string& s) {
    if (s.size() >= 2) {        char f = s.front();
        char b = s.back();
        if ((f == '"' && b == '"') || (f == '\'' && b == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

bool looksLikeShellBuiltin(const std::string& cmd) {
    if (cmd.empty()) return false;

    // Take the basename (handles both / and \)
    std::string base = cmd;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);

    // cmd.exe builtin commands
    static const std::set<std::string> cmdBuiltins = {
        "assoc", "break", "cd", "chdir", "cls", "color", "copy", "date",
        "del", "dir", "echo", "endlocal", "erase", "exit", "for", "ftype",
        "goto", "if", "md", "mkdir", "mklink", "move", "path", "pause",
        "popd", "prompt", "pushd", "rd", "ren", "rename", "rmdir", "set",
        "setlocal", "shift", "start", "time", "title", "type", "ver",
        "verify", "vol", "rem", "call", "doskey", "choice",
    };

    // POSIX shell builtin commands
    static const std::set<std::string> shBuiltins = {
        ".", ":", "alias", "bg", "break", "builtin", "caller", "case",
        "cd", "command", "compgen", "complete", "continue", "declare",
        "dirs", "disown", "echo", "enable", "eval", "exec", "exit", "export",
        "false", "fc", "fg", "getopts", "hash", "help", "history", "if",
        "jobs", "kill", "let", "local", "logout", "popd", "printf", "pushd",
        "pwd", "read", "readonly", "return", "set", "shift", "shopt",
        "source", "suspend", "test", "times", "trap", "true", "type",
        "typeset", "ulimit", "umask", "unalias", "unset", "wait",
    };

    std::string lower;
    lower.reserve(base.size());
    for (char c : base) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (cmdBuiltins.count(lower)) return true;
    if (shBuiltins.count(lower)) return true;
    return false;
}

}  // namespace ccap
