// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

#include "core/normalize.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ccap {

namespace {

inline bool isSep(char c) {
    return c == '/' || c == '\\';
}

inline char lowerC(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

}  // namespace

bool fileExists(const std::string& path) {
#if defined(_WIN32)
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (path.empty()) return false;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISREG(st.st_mode);
#endif
}

bool dirExists(const std::string& path) {
#if defined(_WIN32)
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (path.empty()) return false;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
#endif
}

bool isAbsolutePath(const std::string& path) {
    if (path.empty()) return false;
    if (isSep(path[0])) return true;
#if defined(_WIN32)
    // C:\foo or C:/foo
    if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':' && isSep(path[2])) {
        return true;
    }
#endif
    return false;
}

std::string normalizePath(const std::string& path) {
    if (path.empty()) return path;

    std::string s = path;
    for (char& c : s) {
        if (c == '\\') c = '/';
    }

    // Keep the Windows drive letter prefix
    std::string prefix;
    size_t start = 0;
#if defined(_WIN32)
    if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') {
        prefix = s.substr(0, 2);
        start = 2;
    }
#endif

    bool absolute = (start < s.size() && s[start] == '/');
    if (absolute && !prefix.empty()) {
        prefix += '/';
        ++start;
    }

    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = start; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '/') {
            if (cur.empty() || cur == ".") {
                // skip
            } else if (cur == "..") {
                if (!parts.empty() && parts.back() != "..") {
                    parts.pop_back();
                } else if (!absolute) {
                    parts.push_back("..");
                }
                // when absolute, ".." above the root is dropped
            } else {
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur += s[i];
        }
    }

    std::string out = prefix;
    if (absolute && (prefix.empty() || prefix.back() != '/')) out += '/';

    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }

    if (out.empty()) out = absolute ? "/" : ".";
    return out;
}

std::string normalizePathNative(const std::string& path) {
    std::string s = normalizePath(path);
#if defined(_WIN32)
    // The native separator on Windows is '\'
    for (char& c : s) {
        if (c == '/') c = '\\';
    }
#endif
    return s;
}

std::string baseName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

std::string extensionOf(const std::string& path) {
    std::string base = baseName(path);
    size_t pos = base.find_last_of('.');
    if (pos == std::string::npos || pos == 0) return "";
    std::string ext = base.substr(pos);
    for (char& c : ext) c = lowerC(c);
    return ext;
}

std::string dirName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return "";
    return path.substr(0, pos);
}

std::string getEnv(const std::string& name) {
    const char* v = std::getenv(name.c_str());
    return v ? std::string(v) : std::string();
}

std::string currentDir() {
#if defined(_WIN32)
    char buf[MAX_PATH * 4];
    DWORD n = GetCurrentDirectoryA(sizeof(buf), buf);
    if (n == 0 || n >= sizeof(buf)) return "";
    return std::string(buf, n);
#else
    char buf[4096];
    if (getcwd(buf, sizeof(buf)) == nullptr) return "";
    return std::string(buf);
#endif
}

bool readFile(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();

    // ── Strip the UTF-8 BOM ───────────────────────────────────────────
    //
    // Why this has to happen here (rather than in each parser separately):
    //
    //   Editors on Windows and PowerShell 5.1's
    //   `Set-Content -Encoding UTF8` both write a BOM. A BOM is not
    //   "content", it is an encoding marker --- leaving it in place makes
    //   **every** prefix-matching parser fail silently:
    //
    //     build log:  "EXECUTING 'g++ ...'"  ->  rfind("EXECUTING ", 0) != 0
    //     -> not a single command gets parsed, and **no error is reported**
    //
    //   The symptom is "0 commands parsed", not "parse failed" --- the very
    //   worst class of bug.
    //
    // Putting it in read_file solves it for every consumer at once
    // (build logs / JSON configuration / compilation database / the
    // captured event stream).
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    if (out) *out = content;
    return true;
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

std::string findInPath(const std::string& name,
                         const std::vector<std::string>& extraDirs) {
    if (name.empty()) return "";

    // Contains a separator -> resolve relative to the current directory
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        std::string p = normalizePath(name);
        if (fileExists(p)) return p;
        return "";
    }

    std::vector<std::string> dirs = extraDirs;
    std::string pathEnv = getEnv("PATH");
    if (!pathEnv.empty()) {
        // PATH separator: ';' on Windows, ':' on POSIX
#if defined(_WIN32)
        const char pathSep = ';';
#else
        const char pathSep = ':';
#endif
        std::string cur;
        for (size_t i = 0; i <= pathEnv.size(); ++i) {
            if (i == pathEnv.size() || pathEnv[i] == pathSep) {
                if (!cur.empty()) dirs.push_back(cur);
                cur.clear();
            } else {
                cur += pathEnv[i];
            }
        }
    }

#if defined(_WIN32)
    std::vector<std::string> exts;
    std::string pathext = getEnv("PATHEXT");
    if (pathext.empty()) pathext = ".COM;.EXE;.BAT;.CMD";
    {
        std::string cur;
        for (size_t i = 0; i <= pathext.size(); ++i) {
            if (i == pathext.size() || pathext[i] == ';') {
                if (!cur.empty()) {
                    for (char& c : cur) c = lowerC(c);
                    exts.push_back(cur);
                }
                cur.clear();
            } else {
                cur += pathext[i];
            }
        }
    }
    // The original name is tried first (so foo.bat hits directly)
    for (const auto& d : dirs) {
        std::string cand = normalizePath(d + "/" + name);
        if (fileExists(cand)) return cand;
    }
    for (const auto& d : dirs) {
        for (const auto& e : exts) {
            std::string cand = normalizePath(d + "/" + name + e);
            if (fileExists(cand)) return cand;
        }
    }
#else
    for (const auto& d : dirs) {
        std::string cand = normalizePath(d + "/" + name);
        if (fileExists(cand)) return cand;
    }
#endif

    return "";
}

std::string resolveExecutable(const std::string& name) {
    if (name.empty()) return "";

    std::string n = stripEnvPrefix(name);
    if (n.empty()) return "";

    if (isAbsolutePath(n)) {
        std::string p = normalizePath(n);
        if (fileExists(p)) return p;
        return "";
    }

    if (n.find('/') != std::string::npos || n.find('\\') != std::string::npos) {
        std::string p = normalizePath(currentDir() + "/" + n);
        if (fileExists(p)) return p;
        return "";
    }

    return findInPath(n);
}

std::string resolveExecutableNative(const std::string& name) {
    std::string p = resolveExecutable(name);
    if (p.empty()) return "";
    return normalizePathNative(p);
}

std::string stripEnvPrefix(const std::string& s) {
    if (s.empty()) return s;

#if defined(_WIN32)
    // %VAR%
    if (s.size() >= 2 && s.front() == '%' && s.back() == '%') {
        std::string name = s.substr(1, s.size() - 2);
        if (name.find('%') == std::string::npos && !name.empty()) {
            std::string v = getEnv(name);
            return v.empty() ? s : v;
        }
    }
#else
    // ${VAR}
    if (s.size() >= 3 && s[0] == '$' && s[1] == '{' && s.back() == '}') {
        std::string name = s.substr(2, s.size() - 3);
        std::string v = getEnv(name);
        return v.empty() ? s : v;
    }
    // $VAR
    if (s.size() >= 2 && s[0] == '$') {
        size_t end = 1;
        while (end < s.size() &&
               (std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '_')) {
            ++end;
        }
        if (end > 1) {
            std::string name = s.substr(1, end - 1);
            std::string v = getEnv(name);
            return v.empty() ? s : v;
        }
    }
#endif

    return s;
}

std::string expandEnvVars(const std::string& s) {
    if (s.empty()) return s;
    std::string out = s;

#if defined(_WIN32)
    // %VAR%
    for (int guard = 0; guard < 16; ++guard) {
        size_t open = out.find('%');
        if (open == std::string::npos) break;
        size_t close = out.find('%', open + 1);
        if (close == std::string::npos) break;
        std::string name = out.substr(open + 1, close - open - 1);
        if (name.empty()) break;
        std::string value = getEnv(name);
        out = out.substr(0, open) + value + out.substr(close + 1);
    }
#else
    // $VAR / ${VAR}
    for (int guard = 0; guard < 16; ++guard) {
        size_t open = out.find("${");
        if (open != std::string::npos) {
            size_t close = out.find('}', open + 2);
            if (close == std::string::npos) break;
            std::string name = out.substr(open + 2, close - open - 2);
            out = out.substr(0, open) + getEnv(name) + out.substr(close + 1);
            continue;
        }
        open = out.find('$');
        if (open == std::string::npos) break;
        size_t nameStart = open + 1;
        size_t nameEnd = nameStart;
        while (nameEnd < out.size() &&
               (std::isalnum(static_cast<unsigned char>(out[nameEnd])) ||
                out[nameEnd] == '_')) {
            ++nameEnd;
        }
        if (nameEnd == nameStart) break;
        std::string name = out.substr(nameStart, nameEnd - nameStart);
        out = out.substr(0, open) + getEnv(name) + out.substr(nameEnd);
    }
#endif

    return out;
}

}  // namespace ccap
