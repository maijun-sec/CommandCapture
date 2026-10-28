// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Debugging probe: verify that DEBUG_PROCESS really receives the child
// process events
// Usage: DbgTree <exe> [args...]
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: DbgTree <exe> [args]\n"); return 1; }

    std::wstring cmd;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) cmd += L" ";
        cmd += L"\"";
        cmd += argv[i];
        cmd += L"\"";
    }

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(argv[1], &cmd[0], nullptr, nullptr, FALSE,
                              DEBUG_PROCESS, nullptr, nullptr, &si, &pi);
    if (!ok) { wprintf(L"CreateProcessW failed: %lu\n", GetLastError()); return 2; }
    CloseHandle(pi.hThread);
    wprintf(L"[root pid=%lu]\n", pi.dwProcessId);

    int creates = 0, exits = 0, others = 0;
    int64_t last = GetTickCount64();
    DWORD rootExit = 0;

    for (;;) {
        DEBUG_EVENT ev{};
        BOOL got = WaitForDebugEvent(&ev, 100);
        if (!got) {
            if (GetTickCount64() - last > 2000) { wprintf(L"[timeout]\n"); break; }
            continue;
        }
        last = GetTickCount64();

        switch (ev.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT: {
                creates++;
                auto& ci = ev.u.CreateProcessInfo;
                wprintf(L"[CREATE pid=%lu exe=%ls]\n", ev.dwProcessId,
                        ci.lpImageName ? (LPCWSTR)ci.lpImageName : L"(null)");
                if (ci.hFile) CloseHandle(ci.hFile);
                if (ci.hProcess) CloseHandle(ci.hProcess);
                if (ci.hThread) CloseHandle(ci.hThread);
                ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, 0x00010002);
                break;
            }
            case EXIT_PROCESS_DEBUG_EVENT:
                exits++;
                wprintf(L"[EXIT  pid=%lu]\n", ev.dwProcessId);
                if (ev.dwProcessId == pi.dwProcessId) {
                    GetExitCodeProcess(pi.hProcess, &rootExit);
                }
                ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, 0x00010002);
                break;
            case EXCEPTION_DEBUG_EVENT: {
                DWORD code = ev.u.Exception.ExceptionRecord.ExceptionCode;
                wprintf(L"[EXC   pid=%lu code=%08lX first=%lu]\n", ev.dwProcessId,
                        code, ev.u.Exception.dwFirstChance);
                if (code == 0x80000003) ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, 0x00010002);
                else ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, 0x80010001);
                break;
            }
            case LOAD_DLL_DEBUG_EVENT: {
                auto& di = ev.u.LoadDll;
                if (di.hFile) CloseHandle(di.hFile);
                others++;
                ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, 0x00010002);
                break;
            }
            default:
                others++;
                ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, 0x00010002);
                break;
        }
    }

    wprintf(L"[SUMMARY creates=%d exits=%d others=%d rootExit=%lu]\n",
            creates, exits, others, rootExit);
    CloseHandle(pi.hProcess);
    return 0;
}
