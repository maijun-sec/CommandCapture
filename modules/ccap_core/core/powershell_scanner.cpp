// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

#include "core/powershell_scanner.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "core/command_line.h"
#include "core/normalize.h"

namespace ccap {

namespace {

// Common PowerShell aliases -> canonical names
const std::map<std::string, std::string>& aliasTable() {
    static const std::map<std::string, std::string> t = {
        // File system
        {"ls", "Get-ChildItem"},
        {"dir", "Get-ChildItem"},
        {"gci", "Get-ChildItem"},
        {"cat", "Get-Content"},
        {"gc", "Get-Content"},
        {"type", "Get-Content"},
        {"cp", "Copy-Item"},
        {"copy", "Copy-Item"},
        {"cpi", "Copy-Item"},
        {"mv", "Move-Item"},
        {"move", "Move-Item"},
        {"mi", "Move-Item"},
        {"rm", "Remove-Item"},
        {"del", "Remove-Item"},
        {"erase", "Remove-Item"},
        {"ri", "Remove-Item"},
        {"ren", "Rename-Item"},
        {"rename", "Rename-Item"},
        {"ni", "New-Item"},
        {"md", "New-Item"},
        {"mkdir", "New-Item"},
        {"rd", "Remove-Item"},
        {"rmdir", "Remove-Item"},
        {"echo", "Write-Output"},
        {"write", "Write-Output"},
        {"write-output", "Write-Output"},
        {"clc", "Clear-Host"},
        {"clear", "Clear-Host"},
        {"cls", "Clear-Host"},
        // Process
        {"ps", "Get-Process"},
        {"gps", "Get-Process"},
        {"kill", "Stop-Process"},
        {"spps", "Stop-Process"},
        {"start", "Start-Process"},
        {"saps", "Start-Process"},
        {"sleep", "Start-Sleep"},
        // Network
        {"curl", "Invoke-WebRequest"},
        {"wget", "Invoke-WebRequest"},
        {"irm", "Invoke-RestMethod"},
        // Miscellaneous
        {"cd", "Set-Location"},
        {"chdir", "Set-Location"},
        {"sl", "Set-Location"},
        {"pwd", "Get-Location"},
        {"gl", "Get-Location"},
        {"gm", "Get-Member"},
        {"gcm", "Get-Command"},
        {"help", "Get-Help"},
        {"man", "Get-Help"},
        {"history", "Get-History"},
        {"measure", "Measure-Object"},
        {"select", "Select-Object"},
        {"where", "Where-Object"},
        {"foreach", "ForEach-Object"},
        {"sort", "Sort-Object"},
        {"group", "Group-Object"},
        {"compare", "Compare-Object"},
        {"tee", "Tee-Object"},
        {"export", "Export-Csv"},
        {"import", "Import-Csv"},
        {"iex", "Invoke-Expression"},
        {"oh", "Out-Host"},
        {"gcm", "Get-Command"},
    };
    return t;
}

// The list of PowerShell "verbs". A cmdlet is always Verb-Noun.
//
// This table must be **complete** --- one missing verb causes the matching
// cmdlet to be misjudged as an external program, and the output loses a
// (builtin) marker:
//   Where-Object   -> verb "Where"   is not in the table -> treated as an
//                                           external program
//   ForEach-Object -> verb "ForEach" is not in the table -> treated as an
//                                           external program
// Such misjudgements make the question "which ones are builtin cmdlets?"
// come out with the wrong answer.
//
// Below is every verb returned by the official PowerShell Get-Verb
// (including alias verbs).
const std::set<std::string>& psVerbs() {
    static const std::set<std::string> t = {
        // Lifecycle
        "Add", "Clear", "Close", "Copy", "Enter", "Exit", "Find", "Format",
        "Get", "Hide", "Join", "Lock", "Move", "New", "Open", "Optimize",
        "Push", "Pop", "Redo", "Remove", "Rename", "Reset", "Resize", "Search",
        "Select", "Set", "Show", "Skip", "Split", "Step", "Switch", "Undo",
        "Unlock", "Watch", "Backup", "Checkpoint", "Compare", "Compress",
        "Convert", "ConvertFrom", "ConvertTo", "Dismount", "Edit", "Expand",
        "Export", "Group", "Import", "Initialize", "Limit", "Merge", "Mount",
        "Out", "Publish", "Restore", "Save", "Sync", "Unpublish", "Update",
        "Wait", "Invoke", "Register", "Unregister", "Request", "Start",
        "Stop", "Restart", "Suspend", "Resume", "Debug", "Measure", "Trace",
        "Write", "Read", "Block", "Grant", "Protect", "Revoke", "Unprotect",
        "Resolve", "Test", "Connect", "Disconnect", "Send", "Receive",
        "Enable", "Disable", "Assert", "Complete", "Confirm", "Deny",
        "Approve", "Submit", "Install", "Uninstall",
        // Data
        "Backup", "Checkpoint", "Compare", "Compress", "Convert",
        "ConvertFrom", "ConvertTo", "Dismount", "Edit", "Expand", "Export",
        "Group", "Import", "Initialize", "Limit", "Merge", "Mount", "Out",
        "Publish", "Restore", "Save", "Sync", "Unpublish", "Update",
        // Diagnostics
        "Debug", "Measure", "Ping", "Repair", "Resolve", "Test", "Trace",
        // Lifecycle (complete)
        "Approve", "Assert", "Build", "Complete", "Confirm", "Deny",
        "Disable", "Enable", "Install", "Invoke", "Register", "Request",
        "Restart", "Resume", "Start", "Stop", "Submit", "Suspend",
        "Uninstall", "Unregister", "Wait", "Request",
        // Security
        "Block", "Grant", "Protect", "Revoke", "Unblock", "Unprotect",
        // Other common ones
        "ForEach", "Where", "Group", "Sort", "Tee", "Measure",
        "Compare", "Convert", "Join", "Split",
        // Communication
        "Connect", "Disconnect", "Read", "Receive", "Send", "Write",
    };
    return t;
}

// Is it a PowerShell keyword / control structure
bool isPsKeyword(const std::string& w) {
    static const std::set<std::string> t = {
        "if", "elseif", "else", "switch", "foreach", "for", "while", "do",
        "until", "break", "continue", "return", "function", "filter", "param",
        "begin", "process", "end", "try", "catch", "finally", "throw", "trap",
        "class", "enum", "in", "data", "dynamicparam", "workflow", "inlinescript",
        "parallel", "sequence", "using", "namespace", "base", "configuration",
        "default", "hidden", "static", "public", "private",
    };
    return t.count(w) > 0;
}

// "Special" commands in PowerShell: not cmdlets, but language constructs
bool isPsStructural(const std::string& w) {
    static const std::set<std::string> t = {
        "Write-Output", "Write-Host", "Write-Error", "Write-Warning",
        "Write-Verbose", "Write-Debug", "Write-Progress", "Write-Information",
        "Out-Null", "Out-Default", "Out-Host", "Out-File", "Out-String",
        "Out-Buffer", "Out-GridView", "Out-Printer", "Out-Input",
    };
    return t.count(w) > 0;
}

// Is it a token that is "not a command"
//
// In PowerShell these are expressions / declarations / language
// constructs. Early versions reported them as commands (pure noise).
bool isPsNonCommand(const std::string& w) {
    if (w.empty()) return true;

    // Variable reference: $foo / $env:PATH / $_.Name / $script:x
    if (w[0] == '$') return true;

    // Parameter declaration: param(...)
    {
        std::string lower;
        for (char c : w) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "param") return true;
        if (lower == "dynamicparam") return true;
    }

    // Expression fragment starting with a comparison/logical operator:
    //   Where-Object { $_.Name -eq 'explorer' }
    // What gets split out is something like "-eq 'explorer'", which must not
    // be treated as a command.
    if (w[0] == '-') {
        static const std::set<std::string> ops = {
            "eq", "ne", "gt", "ge", "lt", "le", "like", "notlike",
            "match", "notmatch", "contains", "notcontains", "in", "notin",
            "and", "or", "not", "xor", "is", "isnot", "replace", "split",
        };
        std::string op;
        for (char c : w) {
            if (c == ' ') break;
            op += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ops.count(op)) return true;
    }

    return false;
}

// Is it a variable assignment: $name = value
bool isPsAssignment(const std::vector<std::string>& words) {
    if (words.size() < 2) return false;
    if (words[0].empty() || words[0][0] != '$') return false;
    return words[1] == "=";
}

// Is it a hashtable literal: @{ key = value; ... }
//
// Symptom when this is not recognised:
//   $params = @{ Path = "C:\temp" }
//              ^^^^^^^^^^ reported as a command named "Path"
//
// The value of a hashtable is **data**, not a command. But the keys and
// values can contain command substitutions, so the line still has to be
// scanned for those --- just not for the key itself.
bool isPsHashtableLiteral(const std::string& w) {
    return w.size() >= 2 && w[0] == '@' && w[1] == '{';
}

// Is it the start of a here-string: @" or @'
bool isPsHereStringStart(const std::string& w) {
    return w.size() >= 2 && w[0] == '@' &&
           (w[1] == '"' || w[1] == '\'');
}

// Is it a splatting operator: @name
//
// `Get-ChildItem @params` splats a hashtable into the parameters. The
// @params token is not a command.
bool isPsSplat(const std::string& w) {
    return w.size() >= 2 && w[0] == '@' && w[1] != '{' && w[1] != '"' &&
           w[1] != '\'';
}

// Is it Invoke-Expression (or its alias)?
//
// `Invoke-Expression "git status"` **runs** the string as a command. The
// command inside the string is therefore really executed, and has to be
// reported --- otherwise a whole command is silently missing from the
// capture.
//
// The alias is "iex".
bool isPsInvokeExpression(const std::string& w) {
    std::string lower;
    for (char c : w) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower == "invoke-expression" || lower == "iex";
}

// Extract the **literal string** argument of a command segment.
//
// Returns the string content (without the quotes), or "" when the argument
// is not a literal string.
//
// Why the raw segment and not the tokenized argv:
//   The tokenizer strips the quotes, so by the time we look at argv[1] the
//   difference between
//     Invoke-Expression "git status"   (a literal -- the command is known)
//     Invoke-Expression $cmd           (a variable -- the command is unknown)
//   is already gone. Treating the variable form as a literal would report
//   a command named "$cmd" --- a pure false positive.
//
// The literal is extracted from the raw text, so the quotes are still
// visible there.
std::string extractPsLiteralStringArg(const std::string& segment) {
    size_t i = 0;
    // Skip the command name.
    while (i < segment.size() &&
           std::isspace(static_cast<unsigned char>(segment[i]))) {
        ++i;
    }
    while (i < segment.size() &&
           !std::isspace(static_cast<unsigned char>(segment[i]))) {
        ++i;
    }
    // Skip the whitespace between the name and the argument.
    while (i < segment.size() &&
           std::isspace(static_cast<unsigned char>(segment[i]))) {
        ++i;
    }
    if (i >= segment.size()) return "";

    char q = segment[i];
    if (q != '"' && q != '\'') return "";   // not a literal string

    size_t end = i + 1;
    std::string content;
    while (end < segment.size()) {
        if (segment[end] == q) {
            // A doubled quote is an escaped quote.
            if (end + 1 < segment.size() && segment[end + 1] == q) {
                content += q;
                end += 2;
                continue;
            }
            break;
        }
        content += segment[end++];
    }
    if (end >= segment.size()) return "";   // unterminated
    return content;
}

// Strip PowerShell comments
//   # line comment
//   <# ... #> block comment
std::string stripPsComments(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        // Block comment
        if (i + 3 < n && text[i] == '<' && text[i + 1] == '#') {
            size_t end = text.find("#>", i + 2);
            if (end == std::string::npos) break;
            // Keep the newlines, otherwise the line numbers get misaligned
            for (size_t k = i; k < end + 2; ++k) {
                if (text[k] == '\n') out += '\n';
            }
            i = end + 2;
            continue;
        }
        // Line comment
        if (text[i] == '#') {
            while (i < n && text[i] != '\n') ++i;
            continue;
        }
        // Not a comment inside a string
        if (text[i] == '\'') {
            out += text[i++];
            while (i < n) {
                if (text[i] == '\'' && i + 1 < n && text[i + 1] == '\'') {
                    out += text[i++];
                    out += text[i++];
                    continue;
                }
                out += text[i];
                if (text[i] == '\'') { ++i; break; }
                ++i;
            }
            continue;
        }
        if (text[i] == '"') {
            out += text[i++];
            while (i < n) {
                // In PowerShell "" is an escaped double quote
                if (text[i] == '"' && i + 1 < n && text[i + 1] == '"') {
                    out += text[i++];
                    out += text[i++];
                    continue;
                }
                out += text[i];
                if (text[i] == '"') { ++i; break; }
                ++i;
            }
            continue;
        }
        if (text[i] == '`') {
            // The backtick is the escape character; keep it together with the
            // next character
            out += text[i++];
            if (i < n) out += text[i++];
            continue;
        }
        out += text[i++];
    }
    return out;
}

// Strip PowerShell here-strings.
//
// A here-string is:
//   @"text"@          (expandable: $variables are interpolated)
//   @'text'@          (literal: nothing is interpolated)
//
// The content spans multiple lines and ends with a line that starts with
// the terminator.
//
// Why strip it instead of parsing it:
//
//   The content is **text**, not code. A here-string like
//
//     $text = @"
//     npm install
//     "@
//
//   contains "npm install" --- which is NOT a command that runs. Reporting
//   it would be a false positive.
//
//   The earlier behaviour was worse than a false positive: the here-string
//   was not recognised at all, so the terminator line ("@) was tokenised
//   into a bare "@" token and reported as a command.
//
// The newlines are kept, so the line numbers of everything after it stay
// correct.
std::string stripPsHereStrings(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        // Not inside a string literal
        if (text[i] == '\'' || text[i] == '"') {
            char q = text[i++];
            out += q;
            while (i < n) {
                if (q == '"' && text[i] == '"' && i + 1 < n && text[i + 1] == '"') {
                    out += text[i++];
                    out += text[i++];
                    continue;
                }
                out += text[i];
                if (text[i] == q) { ++i; break; }
                ++i;
            }
            continue;
        }

        // Here-string start: @" or @' at a token boundary
        if (text[i] == '@' && i + 1 < n && (text[i + 1] == '"' || text[i + 1] == '\'')) {
            char q = text[i + 1];
            // The @ must start a token, otherwise it is part of a word
            // (e.g. an email address).
            bool atTokenStart = (i == 0) ||
                                std::isspace(static_cast<unsigned char>(text[i - 1]));
            if (atTokenStart) {
                i += 2;
                // Find the terminator: a line that starts with the quote
                // followed by @.
                std::string terminator;
                terminator += q;
                terminator += '@';
                while (i < n) {
                    size_t lineEnd = text.find('\n', i);
                    if (lineEnd == std::string::npos) lineEnd = n;
                    std::string line = text.substr(i, lineEnd - i);
                    // Trim the leading whitespace: the terminator may be
                    // indented.
                    size_t firstNonSpace = line.find_first_not_of(" \t");
                    if (firstNonSpace != std::string::npos &&
                        line.compare(firstNonSpace, terminator.size(), terminator) == 0) {
                        // Drop the content, keep the newline so the line
                        // numbers stay aligned.
                        for (size_t k = i; k < lineEnd; ++k) {
                            if (text[k] == '\n') out += '\n';
                        }
                        i = lineEnd;
                        break;
                    }
                    // Keep the newline count of the skipped line.
                    if (lineEnd < n) {
                        out += '\n';
                        i = lineEnd + 1;
                    } else {
                        i = n;
                        break;
                    }
                }
                continue;
            }
        }

        out += text[i++];
    }
    return out;
}

// Split a piece of PowerShell on the pipe |
//   The pipe character only counts when outside quotes and not part of a
//   :: static member access
std::vector<std::string> splitPsPipeline(const std::string& seg) {
    std::vector<std::string> out;
    std::string cur;
    bool inS = false, inD = false;
    int depthBrace = 0;

    for (size_t i = 0; i < seg.size(); ++i) {
        char c = seg[i];

        if (c == '\'' && !inD) { inS = !inS; cur += c; continue; }
        if (c == '"' && !inS) { inD = !inD; cur += c; continue; }
        if (c == '`') { cur += c; if (i + 1 < seg.size()) cur += seg[++i]; continue; }

        if (!inS && !inD) {
            if (c == '{') ++depthBrace;
            if (c == '}') --depthBrace;

            if (c == '|' && depthBrace == 0) {
                // || is logical or, not a pipe
                if (i + 1 < seg.size() && seg[i + 1] == '|') {
                    cur += c;
                    cur += seg[++i];
                    continue;
                }
                if (!cur.empty()) out.push_back(cur);
                cur.clear();
                continue;
            }
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Extract the command name and arguments from one pipeline segment
//   Handles the & call operator, executable paths and -Parameter arguments
void extractPsCommand(const std::string& seg,
                        std::vector<std::string>* argvOut,
                        bool* dotSourced = nullptr) {
    std::string s = seg;

    // Strip leading and trailing whitespace
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    s = s.substr(b);

    // Strip redirections (> file, 2>&1, 3> file)
    {
        std::string t;
        bool inS = false, inD = false;
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '\'' && !inD) { inS = !inS; t += c; continue; }
            if (c == '"' && !inS) { inD = !inD; t += c; continue; }
            if (!inS && !inD && (c == '>' || c == '<')) {
                // Truncate up to the next pipe
                size_t j = i;
                while (j < s.size() && s[j] != '|') ++j;
                s = t + s.substr(j);
                break;
            }
            t += c;
        }
    }

    // Call operators:
    //   & "C:\path\prog.exe" args   --- invoke a command
    //   . "C:\path\script.ps1"      --- run a script in the current scope
    //   & and . can be stacked (in practice .& is the same thing)
    //
    // Watch the order: strip & first, then . --- the other way round and
    // ". &x" gets stripped wrongly.
    //
    // A dot-source (the . operator) carries meaningful information: it
    // means a script is being invoked.
    bool usedDotOperator = false;
    for (int round = 0; round < 2; ++round) {
        if (s.empty()) break;
        if (s[0] != '&' && s[0] != '.') break;
        if (s[0] == '.') usedDotOperator = true;
        s = s.substr(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s[0]))) {
            s = s.substr(1);
        }
    }

    if (s.empty()) return;

    // Tokenize: PowerShell separates with spaces, quotes come in pairs
    std::vector<std::string> words;
    {
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i >= s.size()) break;

            std::string w;
            if (s[i] == '"' || s[i] == '\'') {
                char q = s[i++];
                while (i < s.size()) {
                    if (s[i] == q) {
                        if (i + 1 < s.size() && s[i + 1] == q) {
                            w += q;
                            i += 2;
                            continue;
                        }
                        ++i;
                        break;
                    }
                    w += s[i++];
                }
                // The quoted content is one complete token.
                // **It must be pushed here** --- an early version missed this
                // line, and as a result every quoted token (command name or
                // argument) was silently dropped:
                //   "C:\path\prog.exe"  ->  0 events
                //   Write-Output "hi"   ->  only Write-Output survives
                words.push_back(w);
            } else {
                while (i < s.size() &&
                       !std::isspace(static_cast<unsigned char>(s[i]))) {
                    w += s[i++];
                }

                // PowerShell allows the "command(arguments)" form.
                // Split the parenthesized part off so the command name stays
                // clean (otherwise "param($x)" is taken as a whole and slips
                // past the keyword check).
                size_t lp = w.find('(');
                bool headOk = false;
                if (lp != std::string::npos && lp > 0) {
                    headOk = true;
                    for (char c : w.substr(0, lp)) {
                        if (!(std::isalnum(static_cast<unsigned char>(c)) ||
                              c == '-' || c == '_' || c == '.' || c == '\\' ||
                              c == '/' || c == ':')) {
                            headOk = false;
                            break;
                        }
                    }
                }

                if (headOk) {
                    words.push_back(w.substr(0, lp));
                    std::string inner = w.substr(lp + 1);
                    size_t close = inner.rfind(')');
                    if (close != std::string::npos) inner = inner.substr(0, close);
                    size_t ib = 0;
                    while (ib < inner.size() &&
                           std::isspace(static_cast<unsigned char>(inner[ib]))) {
                        ++ib;
                    }
                    inner = inner.substr(ib);
                    if (!inner.empty()) words.push_back(inner);
                } else {
                    words.push_back(w);
                }
            }
        }
    }

    if (words.empty()) return;
    if (dotSourced) *dotSourced = usedDotOperator;
    *argvOut = words;
}

// Expand $var / ${var} so that path variables can resolve to a real path
std::string expandPsVars(const std::string& s) {
    if (s.find('$') == std::string::npos) return s;
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '$' && i + 1 < s.size()) {
            size_t j = i + 1;
            if (s[j] == '{') {
                size_t end = s.find('}', j);
                if (end != std::string::npos) {
                    out += "${" + s.substr(j + 1, end - j - 1) + "}";
                    i = end + 1;
                    continue;
                }
            }
            while (j < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[j])) ||
                    s[j] == '_' || s[j] == ':')) {
                ++j;
            }
            if (j > i + 1) {
                out += s.substr(i, j - i);
                i = j;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

void emitPsEvent(const std::string& scriptPath,
                   int lineNo,
                   const std::vector<std::string>& argv,
                   const std::string& cwd,
                   const PsScanOptions& opts,
                   EventCollector* collector,
                   bool isScript = false) {
    if (!collector || argv.empty()) return;

    std::string name = argv[0];
    if (name.empty()) return;

    // Expand aliases
    std::string canonical = name;
    if (opts.expandAliases) {
        std::string lower;
        for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto it = aliasTable().find(lower);
        if (it != aliasTable().end()) canonical = it->second;
    }

    // Decide whether it is a cmdlet or an external program
    bool isCmdlet = false;
    {
        size_t dash = canonical.find('-');
        if (dash != std::string::npos && dash > 0) {
            std::string verb = canonical.substr(0, dash);
            for (char& c : verb) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            static std::set<std::string> verbsLower = [] {
                std::set<std::string> s;
                for (const auto& v : psVerbs()) {
                    std::string l;
                    for (char c : v) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    s.insert(l);
                }
                return s;
            }();
            if (verbsLower.count(verb)) isCmdlet = true;
        }
    }

    // External program: the path contains a separator, or it is not a cmdlet
    bool looksExternal = false;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        looksExternal = true;
    }
    if (!isCmdlet && !looksExternal) {
        // Unknown command --- it might be an exe, or a misspelled cmdlet
        looksExternal = true;
    }

    std::string resolvedPath;
    bool resolved = false;
    if (looksExternal && opts.resolvePaths) {
        std::string expanded = expandPsVars(name);
        resolvedPath = resolveExecutable(expanded);
        if (!resolvedPath.empty()) resolved = true;
    }

    CommandEvent e;
    e.timestampMs = nowUnixMs();
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
    e.executable = name;
    e.argv = argv;
    e.cwd = cwd;
    e.scriptPath = scriptPath;
    e.scriptLine = lineNo;

    if (isScript) {
        // Dot-source: a script is invoked, not an external program
        e.kind = "script";
        e.isBuiltin = false;
    } else if (looksExternal) {
        e.kind = "command";
        e.isBuiltin = false;
    } else {
        e.kind = "cmdlet";
        e.isBuiltin = true;
    }
    e.resolved = resolved;
    if (resolved && !resolvedPath.empty()) {
        e.notes.push_back("resolved=" + resolvedPath);
    }
    if (canonical != name) {
        e.notes.push_back("alias_of=" + canonical);
    }
    if (isPsStructural(canonical)) {
        e.notes.push_back("output_stream");
    }
    collector->add(std::move(e));
}

// ─────────────────────────────────────────────────────────────────────────────
// Sub-expression extraction
//
// Why it has to be extracted separately:
//   In `Write-Output "$(Get-Date -Format o)"` the `Get-Date` **really is
//   invoked** --- but with pure lexical splitting the whole
//   `$(Get-Date -Format o)` becomes one ordinary argument token and the
//   command name is lost.
//
// After extraction the region is scanned recursively, so the real command no
// longer disappears.
// ─────────────────────────────────────────────────────────────────────────────

// Extract the content wrapped in brackets: $( ... ) / @( ... ) / { ... }
//
// Returns the inner content (without the outer brackets). Returns an empty
// string when they are not matched.
std::string extractPsSubexpression(const std::string& s, size_t openPos,
                                     char openCh, char closeCh,
                                     size_t* endPos) {
    if (openPos >= s.size() || s[openPos] != openCh) return "";
    if (endPos) *endPos = openPos;

    int depth = 0;
    bool inS = false, inD = false;
    for (size_t i = openPos; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\'' && !inD) { inS = !inS; continue; }
        if (c == '"' && !inS) { inD = !inD; continue; }
        if (inS || inD) continue;
        if (c == '`') { ++i; continue; }
        if (c == openCh) { ++depth; continue; }
        if (c == closeCh) {
            --depth;
            if (depth == 0) {
                if (endPos) *endPos = i;
                return s.substr(openPos + 1, i - openPos - 1);
            }
        }
    }
    // Not matched --- leave it alone
    if (endPos) *endPos = std::string::npos;
    return "";
}

}  // namespace

bool isPowershellBuiltin(const std::string& name) {
    size_t dash = name.find('-');
    if (dash == std::string::npos || dash == 0) return false;
    std::string verb = name.substr(0, dash);
    for (char& c : verb) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    static std::set<std::string> verbsLower = [] {
        std::set<std::string> s;
        for (const auto& v : psVerbs()) {
            std::string l;
            for (char c : v) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            s.insert(l);
        }
        return s;
    }();
    return verbsLower.count(verb) > 0;
}

std::string expandPsAlias(const std::string& name) {
    std::string lower;
    for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto it = aliasTable().find(lower);
    if (it != aliasTable().end()) return it->second;
    return name;
}

// ─────────────────────────────────────────────────────────────────────────────
// Recursively scan a piece of PowerShell text (used inside sub-expressions)
//
// Difference from scan_powershell:
//   - handles multiple lines at once (a sub-expression may span lines)
//   - no special handling of script blocks (the content of {} has already
//     been extracted separately)
//
// max_depth prevents infinite recursion: $( $( $( ...) ) ) is rare in real
// scripts, but a malicious or mistaken script could blow the stack.
// ─────────────────────────────────────────────────────────────────────────────
void scanPsFragment(const std::string& text,
                      const std::string& scriptPath,
                      int lineNo,
                      const std::string& cwd,
                      const PsScanOptions& opts,
                      EventCollector* collector,
                      int depth) {
    if (!collector || depth > 6) return;

    // Split by line (keeping the line number offset)
    std::vector<std::pair<int, std::string>> lines;
    {
        std::string cur;
        int ln = lineNo;
        for (char c : text) {
            if (c == '\n') {
                lines.emplace_back(ln, cur);
                cur.clear();
                ++ln;
                continue;
            }
            if (c != '\r') cur += c;
        }
        if (!cur.empty()) lines.emplace_back(ln, cur);
    }

    for (auto& lp : lines) {
        const std::string& line = lp.second;
        int thisLine = lp.first;
        if (line.empty()) continue;

        // Extract the sub-expressions ($() / @()) first --- recursively scan
        // the commands inside them
        {
            std::string rest;
            size_t i = 0;
            while (i < line.size()) {
                char c = line[i];
                // $() sub-expression
                if (c == '$' && i + 1 < line.size() && line[i + 1] == '(') {
                    size_t end = 0;
                    std::string inner = extractPsSubexpression(line, i + 1, '(', ')', &end);
                    if (!inner.empty()) {
                        scanPsFragment(inner, scriptPath, thisLine, cwd, opts,
                                         collector, depth + 1);
                        i = end + 1;
                        continue;
                    }
                }
                // @() array expression
                if (c == '@' && i + 1 < line.size() && line[i + 1] == '(') {
                    size_t end = 0;
                    std::string inner = extractPsSubexpression(line, i + 1, '(', ')', &end);
                    if (!inner.empty()) {
                        scanPsFragment(inner, scriptPath, thisLine, cwd, opts,
                                         collector, depth + 1);
                        i = end + 1;
                        continue;
                    }
                }
                rest += c;
                ++i;
            }
            if (rest.empty()) continue;
        }

        // Pipeline split + command extraction
        for (const auto& pipeSeg : splitPsPipeline(line)) {
            std::vector<std::string> argv;
            bool dotSourced = false;
            extractPsCommand(pipeSeg, &argv, &dotSourced);
            if (argv.empty()) continue;

            std::string first = argv[0];
            std::string lower;
            for (char c : first) {
                lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (isPsKeyword(lower)) continue;
            if (lower == "function" || lower == "filter") continue;
            if (dotSourced) {
                emitPsEvent(scriptPath, thisLine, argv, cwd, opts, collector,
                              /*is_script=*/true);
                continue;
            }
            if (isPsNonCommand(first)) continue;
            if (isPsAssignment(argv)) continue;
            if (isPsHashtableLiteral(first)) continue;
            if (isPsSplat(first)) continue;
            emitPsEvent(scriptPath, thisLine, argv, cwd, opts, collector);
        }
    }
}

void scanPowershell(const std::string& path,
                     const PsScanOptions& opts,
                     EventCollector* collector,
                     int depth) {
    std::string raw;
    if (!readFile(path, &raw)) return;

    std::string text = stripPsHereStrings(stripPsComments(raw));
    std::string cwd = dirName(path);

    // Process by line, but keep the line numbers
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : text) {
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else if (c != '\r') {
                cur += c;
            }
        }
        if (!cur.empty()) lines.push_back(cur);
    }

    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& line = lines[li];
        int lineNo = static_cast<int>(li) + 1;
        if (line.empty()) continue;

        // 0) Extract the $(...) / @(...) sub-expressions first and
        // recursively scan the commands inside them.
        //
        //    This has to happen **before** the statement split ---
        //    a sub-expression may contain ; and |, so splitting first would
        //    cut `$(Get-ChildItem; Get-Date)` in half,
        //    even though in PowerShell it is one expression.
        //
        //    Note: **the main flow still uses the original line**.
        //    If the sub-expression were cut out of the line before the main
        //    flow ran, the argument of
        //    `Write-Output "now is $(Get-Date)"` would lose a piece and the
        //    command the user sees would not match the script.
        //    The right approach is: tokenize the original line as usual
        //    (arguments stay complete) and report the commands inside the
        //    sub-expression **additionally** on their own.
        {
            size_t i = 0;
            while (i < line.size()) {
                char c = line[i];
                if ((c == '$' || c == '@') && i + 1 < line.size() &&
                    line[i + 1] == '(') {
                    size_t end = 0;
                    std::string inner =
                        extractPsSubexpression(line, i + 1, '(', ')', &end);
                    if (!inner.empty()) {
                        scanPsFragment(inner, path, lineNo, cwd, opts, collector,
                                         depth + 1);
                        i = end + 1;
                        continue;
                    }
                }
                ++i;
            }
        }

        // 1) Statement-level split: newlines outside ; and { }
        //    For simplicity: split on ;, then split each piece on the pipeline
        std::vector<std::string> statements;
        {
            std::string cur;
            bool inS = false, inD = false;
            for (size_t i = 0; i < line.size(); ++i) {
                char c = line[i];
                if (c == '\'' && !inD) { inS = !inS; cur += c; continue; }
                if (c == '"' && !inS) { inD = !inD; cur += c; continue; }
                if (!inS && !inD) {
                    if (c == ';') {
                        if (!cur.empty()) statements.push_back(cur);
                        cur.clear();
                        continue;
                    }
                    // A hashtable literal: @{ key = value; ... }
                    //
                    // The braces of a hashtable are **not** script block
                    // boundaries. Splitting there produced nonsense:
                    //   $params = @{ Path = "C:\temp" }
                    //                ^^^^^^^^^^ became a "command" named Path.
                    //
                    // So when we see "@{" we swallow the whole hashtable
                    // (balanced braces) and keep it as one statement.
                    if (c == '@' && i + 1 < line.size() && line[i + 1] == '{') {
                        size_t j = i + 2;
                        int depth = 1;
                        while (j < line.size() && depth > 0) {
                            if (line[j] == '{') ++depth;
                            else if (line[j] == '}') --depth;
                            if (depth == 0) break;
                            ++j;
                        }
                        cur += line.substr(i, (j < line.size() ? j + 1 : j) - i);
                        i = (j < line.size() ? j : line.size() - 1);
                        continue;
                    }
                    if (c == '{' || c == '}') {
                        // Script block boundary
                        if (!cur.empty()) statements.push_back(cur);
                        cur.clear();
                        // What follows '{' is the script block content and is
                        // handled on its own line
                        statements.push_back(std::string(1, c));
                        continue;
                    }
                }
                cur += c;
            }
            if (!cur.empty()) statements.push_back(cur);
        }

        for (const auto& stmt : statements) {
            std::string s = stmt;
            // Strip leading and trailing whitespace
            size_t b = 0;
            while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
            s = s.substr(b);
            if (s.empty()) continue;

            // A bare { or } --- a script block boundary marker
            if (s == "{" || s == "}") continue;

            // Script block content: the { Get-Process } form
            if (s.front() == '{') {
                std::string inner = s.substr(1);
                size_t close = inner.find('}');
                if (close != std::string::npos) inner = inner.substr(0, close);
                // Recursively handle the commands inside the script block
                std::vector<std::string> innerLines;
                {
                    std::string cur;
                    for (char c : inner) {
                        if (c == '\n' || c == ';') {
                            innerLines.push_back(cur);
                            cur.clear();
                        } else {
                            cur += c;
                        }
                    }
                    if (!cur.empty()) innerLines.push_back(cur);
                }
                for (const auto& il : innerLines) {
                    for (const auto& pipeSeg : splitPsPipeline(il)) {
                        std::vector<std::string> argv;
                        extractPsCommand(pipeSeg, &argv);
                        if (argv.empty()) continue;
                        std::string first = argv[0];
                        std::string lower;
                        for (char c : first) {
                            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        }
                        if (isPsKeyword(lower)) continue;
                        if (isPsNonCommand(first)) continue;
                        if (isPsAssignment(argv)) continue;
                        if (isPsHashtableLiteral(first)) continue;
                        if (isPsSplat(first)) continue;
                        emitPsEvent(path, lineNo, argv, cwd, opts, collector);
                    }
                }
                continue;
            }

            // 2) Pipeline split
            for (const auto& pipeSeg : splitPsPipeline(s)) {
                std::vector<std::string> argv;
                bool dotSourced = false;
                extractPsCommand(pipeSeg, &argv, &dotSourced);
                if (argv.empty()) continue;

                std::string first = argv[0];
                std::string lower;
                for (char c : first) {
                    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }

                // Keywords (if / foreach / function ...) are not commands
                if (isPsKeyword(lower)) continue;

                // function definition: function Foo { ... } --- the function
                // name is not a command
                if (lower == "function" || lower == "filter") continue;

                // A dot-source (the . operator) is invoking a script ---
                // even when the target is a variable ($PSScriptRoot\helper.ps1),
                // that is meaningful information.
                if (dotSourced) {
                    emitPsEvent(path, lineNo, argv, cwd, opts, collector,
                                  /*is_script=*/true);
                    continue;
                }

                // Variable references, variable assignments, expression
                // fragments --- none of them are commands
                if (isPsNonCommand(first)) continue;
                if (isPsAssignment(argv)) continue;

                // A hashtable literal is data, not a command.
                // (The keys and values inside it can still contain command
                // substitutions, but those are handled by the sub-expression
                // pass, which runs before this one.)
                if (isPsHashtableLiteral(first)) continue;

                // A splatting token (@params) is an argument, not a command.
                if (isPsSplat(first)) continue;

                // Invoke-Expression "git status" **runs** the string.
                //
                // The string argument is a command that really executes, so
                // it has to be reported --- otherwise the whole command is
                // missing from the capture.
                //
                // The Invoke-Expression cmdlet itself is reported too: it is
                // a real command that runs.
                //
                // Only a **literal** string can be resolved. A variable
                // (`Invoke-Expression $cmd`) holds an unknown value, and
                // guessing would produce false positives.
                //
                // The literal check is done on the **raw segment**, not on
                // the tokenized argv: the tokenizer strips the quotes, so by
                // the time we look at argv[1] the difference between
                //     Invoke-Expression "git status"   (a literal)
                //     Invoke-Expression $cmd           (a variable)
                // is already gone. Distinguishing them after the fact
                // would turn every variable invocation into a bogus command.
                if (isPsInvokeExpression(first) && argv.size() >= 2) {
                    std::string literal = extractPsLiteralStringArg(pipeSeg);
                    if (!literal.empty()) {
                        scanPsFragment(literal, path, lineNo, cwd, opts,
                                       collector, depth + 1);
                    }
                }

                emitPsEvent(path, lineNo, argv, cwd, opts, collector);
            }
        }
    }
}

}  // namespace ccap
