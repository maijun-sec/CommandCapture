// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Debugging probe v2: tell apart several ContinueDebugEvent combinations
// Usage: DbgTree2 <mode> <exe> [args...]
//   mode=0: breakpoints use DBG_CONTINUE (0x00010002)
//   mode=1: breakpoints use DBG_EXCEPTION_NOT_HANDLED (0x80010001)
//   mode=2: no special handling of breakpoints, always NOT_HANDLED
//   mode=3: use DEBUG_ONLY_THIS_PROCESS for comparison
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: DbgTree2 <mode> <exe> [args...]\n"); return 1; }
    int mode = _wtoi(argv[1]);

    std::wstring cmd;
    for (int i = 2; i < argc; ++i) {
        if (i > 2) cmd += L" ";
        cmd += L"\"";
        cmd += argv[i];
        cmd += L"\"";
    }

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    DWORD flags = DEBUG_PROCESS;
    if (mode == 3) flags = DEBUG_ONLY_THIS_PROCESS;

    BOOL ok = CreateProcessW(argv[2], &cmd[0], nullptr, nullptr, FALSE,
                              flags, nullptr, nullptr, &si, &pi);
    if (!ok) { wprintf(L"CreateProcessW failed: %lu\n", GetLastError()); return 2; }
    CloseHandle(pi.hThread);
    wprintf(L"[root pid=%lu mode=%d]\n", pi.dwProcessId, mode);

    int creates = 0, exits = 0, excs = 0;
    uint64_t last = GetTickCount64();

    for (;;) {
        DEBUG_EVENT ev{};
        BOOL got = WaitForDebugEvent(&ev, 100);
        if (!got) {
            if (GetTickCount64() - last > 3000) { wprintf(L"[timeout]\n"); break; }
            continue;
        }
        last = GetTickCount64();

        DWORD cont = DBG_EXCEPTION_NOT_HANDLED;

        switch (ev.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT: {
                creates++;
                auto& ci = ev.u.CreateProcessInfo;
                wprintf(L"[CREATE pid=%lu]\n", ev.dwProcessId);
                if (ci.hFile) CloseHandle(ci.hFile);
                if (ci.hProcess) CloseHandle(ci.hProcess);
                if (ci.hThread) CloseHandle(ci.hThread);
                cont = DBG_CONTINUE;
                break;
            }
            case EXIT_PROCESS_DEBUG_EVENT:
                exits++;
                wprintf(L"[EXIT  pid=%lu]\n", ev.dwProcessId);
                cont = DBG_CONTINUE;
                break;
            case EXCEPTION_DEBUG_EVENT: {
                DWORD code = ev.u.Exception.ExceptionRecord.ExceptionCode;
                excs++;
                wprintf(L"[EXC   pid=%lu code=%08lX first=%lu]\n", ev.dwProcessId,
                        code, ev.u.Exception.dwFirstChance);
                if (code == STATUS_BREAKPOINT) {
                    if (mode == 0) cont = DBG_CONTINUE;
                    else if (mode == 1) cont = DBG_CONTINUE;  // same as mode0
                    else cont = DBG_EXCEPTION_NOT_HANDLED;
                } else {
                    cont = DBG_EXCEPTION_NOT_HANDLED;
                }
                break;
            }
            case LOAD_DLL_DEBUG_EVENT: {
                auto& di = ev.u.LoadDll;
                if (di.hFile) CloseHandle(di.hFile);
                cont = DBG_CONTINUE;
                break;
            }
            default:
                cont = DBG_CONTINUE;
                break;
        }

        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }

    DWORD rootExit = 0;
    GetExitCodeProcess(pi.hProcess, &rootExit);
    wprintf(L"[SUMMARY mode=%d creates=%d exits=%d excs=%d rootExit=%lu]\n",
            mode, creates, exits, excs, rootExit);
    CloseHandle(pi.hProcess);
    return 0;
}
