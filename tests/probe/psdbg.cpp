// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Minimal reproduction: locate the problem where the PowerShell static
// analysis drops "quoted command names"
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

static void extract(const std::string& seg) {
    std::string s = seg;
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    s = s.substr(b);
    printf("  after trim      : [%s]\n", s.c_str());

    for (int round = 0; round < 2; ++round) {
        if (s.empty()) break;
        printf("  round %d s[0]='%c'\n", round, s[0]);
        if (s[0] != '&' && s[0] != '.') break;
        s = s.substr(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s[0]))) {
            s = s.substr(1);
        }
    }
    printf("  after operators : [%s]\n", s.c_str());
}

int main() {
    printf("case A:\n");
    extract("\"abc\"");
    printf("case B:\n");
    extract("C:\\abc");
    return 0;
}
