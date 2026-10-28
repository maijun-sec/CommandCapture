// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Unit tests: Windows CRT command line splitting
#include <cstdio>
#include <string>
#include <vector>

#include "core/command_line.h"

static int gFail = 0;

static void check(const std::string& input,
                  const std::vector<std::string>& expect,
                  const char* label) {
    auto got = ccap::splitWindowsCommandLine(input);
    bool ok = got == expect;
    if (!ok) {
        gFail++;
        std::printf("FAIL %s\n  input: [%s]\n", label, input.c_str());
        std::printf("  expect:");
        for (auto& s : expect) std::printf(" [%s]", s.c_str());
        std::printf("\n  got   :");
        for (auto& s : got) std::printf(" [%s]", s.c_str());
        std::printf("\n");
    } else {
        std::printf("ok   %s\n", label);
    }
}

int main() {
    check("C:\\WINDOWS\\system32\\cmd.exe /c script.bat",
          {"C:\\WINDOWS\\system32\\cmd.exe", "/c", "script.bat"},
          "plain path with backslashes");

    check("\"C:\\WINDOWS\\system32\\cmd.exe\" /c \"D:/a b/c.bat\"",
          {"C:\\WINDOWS\\system32\\cmd.exe", "/c", "D:/a b/c.bat"},
          "quoted exe + quoted path with space");

    check("whoami",
          {"whoami"},
          "single token");

    check("whoami  ",
          {"whoami"},
          "trailing spaces");

    // CRT rule: 0 backslashes + " => toggles the quote state,
    // so /c" echo piped " is parsed as /c plus one token holding a space.
    check("cmd  /S /D /c\" echo piped \"",
          {"cmd", "/S", "/D", "/c echo piped "},
          "cmd pipe form");

    check("a\\b c",
          {"a\\b", "c"},
          "backslash before letter is literal");

    check("a\\\\b",
          {"a\\\\b"},
          "double backslash literal");

    check("\"a\\\\\"",
          {"a\\"},
          "trailing escaped backslash inside quotes");

    check("\"a b\" c",
          {"a b", "c"},
          "quoted with space");

    check("\"\"",
          {""},
          "empty quoted token");

    check("prog \"a\"\"b\"",
          {"prog", "ab"},
          "adjacent quoted segments");

    std::printf("\n%s (%d failures)\n", gFail ? "FAILED" : "ALL PASSED", gFail);
    return gFail ? 1 : 0;
}
