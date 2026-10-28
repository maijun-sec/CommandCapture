// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Debugging probe v3: add a PEB read in the CREATE event and verify that
// it does not break the tracing
// Usage: DbgTree3 <exe> [args...]
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>

typedef LONG (NTAPI * NtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);

typedef struct _PBI {
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
} PBI;

static bool read_remote(HANDLE h, uintptr_t addr, void* buf, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(h, (LPCVOID)addr, buf, size, &got) && got == size;
}

static void try_peb(HANDLE hProcess, DWORD pid) {
    static NtQueryInformationProcess fn =
        (NtQueryInformationProcess)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                 "NtQueryInformationProcess");
    if (!fn) { wprintf(L"   [peb] no ntdll fn\n"); return; }
    PBI pbi{};
    NTSTATUS st = fn(hProcess, 0, &pbi, sizeof(pbi), nullptr);
    wprintf(L"   [peb] status=%08lX peb=%p\n", (unsigned long)st, pbi.PebBaseAddress);
    if (!pbi.PebBaseAddress) return;
    uintptr_t pp = 0;
    if (!read_remote(hProcess, (uintptr_t)pbi.PebBaseAddress + 0x20, &pp, sizeof(pp))) {
        wprintf(L"   [peb] read ProcessParameters FAILED\n");
        return;
    }
    wprintf(L"   [peb] ProcessParameters=%p\n", (void*)pp);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: DbgTree3 <exe> [args...]\n"); return 1; }

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

    int creates = 0, exits = 0;
    uint64_t last = GetTickCount64();

    for (;;) {
        DEBUG_EVENT ev{};
        BOOL got = WaitForDebugEvent(&ev, 100);
        if (!got) {
            if (GetTickCount64() - last > 3000) { wprintf(L"[timeout]\n"); break; }
            continue;
        }
        last = GetTickCount64();
        DWORD cont = DBG_CONTINUE;

        switch (ev.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT: {
                creates++;
                auto& ci = ev.u.CreateProcessInfo;
                wprintf(L"[CREATE pid=%lu]\n", ev.dwProcessId);
                // Key point: the PEB is read here
                try_peb(ci.hProcess, ev.dwProcessId);
                if (ci.hFile) CloseHandle(ci.hFile);
                if (ci.hProcess) CloseHandle(ci.hProcess);
                if (ci.hThread) CloseHandle(ci.hThread);
                break;
            }
            case EXIT_PROCESS_DEBUG_EVENT:
                exits++;
                wprintf(L"[EXIT  pid=%lu]\n", ev.dwProcessId);
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
    wprintf(L"[SUMMARY creates=%d exits=%d rootExit=%lu]\n", creates, exits, rootExit);
    CloseHandle(pi.hProcess);
    return 0;
}
