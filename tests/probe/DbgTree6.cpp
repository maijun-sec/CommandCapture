// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Probe v6: verify item by item the differences between ccap and the
// probe
// envmode: 0=pass nullptr, 1=pass the GetEnvironmentStringsW buffer as is,
//           2=pass a hand-built block
// showmode: 0=do not set STARTF_USESHOWWINDOW, 1=set SW_HIDE
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: DbgTree6 <envmode> <showmode> <exe> [args...]\n"); return 1; }
    int envmode = _wtoi(argv[1]);
    int showmode = _wtoi(argv[2]);

    std::wstring cmd;
    for (int i = 3; i < argc; ++i) {
        if (i > 3) cmd += L" ";
        cmd += L"\"";
        cmd += argv[i];
        cmd += L"\"";
    }

    STARTUPINFOW si{}; si.cb = sizeof(si);
    if (showmode == 1) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION pi{};

    DWORD flags = DEBUG_PROCESS;
    LPVOID env = nullptr;
    std::wstring envbuf;

    if (envmode == 1) {
        // Pass the GetEnvironmentStringsW buffer as is
        env = (LPVOID)GetEnvironmentStringsW();
        flags |= CREATE_UNICODE_ENVIRONMENT;
    } else if (envmode == 2) {
        // Hand-built: append \0 after every entry, and one more \0 at
        // the end
        LPWCH e = GetEnvironmentStringsW();
        if (e) {
            for (LPWCH p = e; *p; p += wcslen(p) + 1) {
                envbuf += p;
                envbuf.push_back(L'\0');
            }
            FreeEnvironmentStringsW(e);
        }
        env = envbuf.empty() ? nullptr : (LPVOID)&envbuf[0];
        flags |= CREATE_UNICODE_ENVIRONMENT;
    }

    BOOL ok = CreateProcessW(argv[3], &cmd[0], nullptr, nullptr, FALSE,
                              flags, env, nullptr, &si, &pi);
    if (!ok) { wprintf(L"CreateProcessW failed: %lu\n", GetLastError()); return 2; }
    CloseHandle(pi.hThread);
    wprintf(L"[root pid=%lu env=%d show=%d]\n", pi.dwProcessId, envmode, showmode);

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
    wprintf(L"[SUMMARY env=%d show=%d creates=%d exits=%d rootExit=%lu]\n", envmode,
            showmode, creates, exits, rootExit);
    CloseHandle(pi.hProcess);
    return 0;
}
