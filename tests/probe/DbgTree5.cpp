// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Probe v5: verify the effect of the lpApplicationName + lpCommandLine
// combinations
// mode 0: lpApplicationName=exe, cmdline="exe args..."
// mode 1: lpApplicationName=null, cmdline="exe args..."
// mode 2: lpApplicationName=exe, cmdline with the exe wrapped in quotes
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: DbgTree5 <mode> <exe> [args...]\n"); return 1; }
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

    LPCWSTR appName = argv[2];
    if (mode == 1) appName = nullptr;

    // mode 2: the first token of the cmdline is quoted
    if (mode == 2) {
        cmd = L"\"";
        cmd += argv[2];
        cmd += L"\"";
        for (int i = 3; i < argc; ++i) {
            cmd += L" \"";
            cmd += argv[i];
            cmd += L"\"";
        }
    }

    BOOL ok = CreateProcessW(appName, &cmd[0], nullptr, nullptr, FALSE,
                              DEBUG_PROCESS, nullptr, nullptr, &si, &pi);
    if (!ok) { wprintf(L"CreateProcessW failed: %lu\n", GetLastError()); return 2; }
    CloseHandle(pi.hThread);
    wprintf(L"[root pid=%lu mode=%d cmd=%ls]\n", pi.dwProcessId, mode, cmd.c_str());

    int creates = 0, exits = 0;
    uint64_t last = GetTickCount64();

    for (;;) {
        DEBUG_EVENT ev{};
        BOOL got = WaitForDebugEvent(&ev, 100);
        if (!got) {
            if (GetTickCount64() - last > 5000) { wprintf(L"[timeout]\n"); break; }
            continue;
        }
        last = GetTickCount64();
        DWORD cont = DBG_CONTINUE;

        switch (ev.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT: {
                creates++;
                auto& ci = ev.u.CreateProcessInfo;
                if (ci.hFile) CloseHandle(ci.hFile);
                if (ci.hProcess) CloseHandle(ci.hProcess);
                if (ci.hThread) CloseHandle(ci.hThread);
                break;
            }
            case EXIT_PROCESS_DEBUG_EVENT:
                exits++;
                break;
            case EXCEPTION_DEBUG_EVENT: {
                DWORD code = ev.u.Exception.ExceptionRecord.ExceptionCode;
                if (code != STATUS_BREAKPOINT) cont = DBG_EXCEPTION_NOT_HANDLED;
                break;
            }
            case LOAD_DLL_DEBUG_EVENT: {
                auto& di = ev.u.LoadDll;
                if (di.hFile) CloseHandle(di.hFile);
                break;
            }
            default: break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }

    DWORD rootExit = 0;
    GetExitCodeProcess(pi.hProcess, &rootExit);
    wprintf(L"[SUMMARY mode=%d creates=%d exits=%d rootExit=%lu]\n", mode, creates,
            exits, rootExit);
    CloseHandle(pi.hProcess);
    return 0;
}
