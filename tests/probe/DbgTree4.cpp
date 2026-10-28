// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Debugging probe v4: verify whether the different ways of getting the PEB
// affect the child process tracing
// Usage: DbgTree4 <mode> <exe> [args...]
//   mode=0: do not read the PEB (baseline)
//   mode=1: NtQueryInformationProcess (known to break the tracing)
//   mode=2: read TEB->ProcessEnvironmentBlock (using NtQueryInformationThread)
//   mode=3: ContinueDebugEvent first, then read the PEB asynchronously
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>

typedef LONG (NTAPI * NtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef LONG (NTAPI * NtQueryInformationThread)(HANDLE, ULONG, PVOID, ULONG, PULONG);

typedef struct _PBI {
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
} PBI;

// The TEB structure (x64 prefix)
typedef struct _TEB64 {
    NTSTATUS ExceptionCode;
    PVOID StackBase;
    PVOID StackLimit;
    PVOID SubSystemTib;
    ULONG_PTR FiberData;
    ULONG_PTR ArbitraryUserPointer;
    PVOID Self;
    PVOID PEB;   // offset 0x60 on x64
} TEB64;

// THREAD_BASIC_INFORMATION is not defined in the public SDK
// Note: the TebBaseAddress field sits at offset 8 on x64
typedef struct _CLIENT_ID {
    ULONG_PTR UniqueProcess;
    ULONG_PTR UniqueThread;
} CLIENT_ID;

typedef struct _TBI {
    NTSTATUS ExitStatus;
    PVOID TebBaseAddress;
    CLIENT_ID ClientId;
    ULONG_PTR AffinityMask;
    ULONG Priority;
    ULONG BasePriority;
} TBI;

static bool read_remote(HANDLE h, uintptr_t addr, void* buf, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(h, (LPCVOID)addr, buf, size, &got) && got == size;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: DbgTree4 <mode> <exe> [args...]\n"); return 1; }
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

    BOOL ok = CreateProcessW(argv[2], &cmd[0], nullptr, nullptr, FALSE,
                              DEBUG_PROCESS, nullptr, nullptr, &si, &pi);
    if (!ok) { wprintf(L"CreateProcessW failed: %lu\n", GetLastError()); return 2; }
    CloseHandle(pi.hThread);
    wprintf(L"[root pid=%lu mode=%d]\n", pi.dwProcessId, mode);

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

                if (mode == 1) {
                    static NtQueryInformationProcess fn =
                        (NtQueryInformationProcess)GetProcAddress(
                            GetModuleHandleW(L"ntdll.dll"),
                            "NtQueryInformationProcess");
                    PBI pbi{};
                    fn(ci.hProcess, 0, &pbi, sizeof(pbi), nullptr);
                    wprintf(L"   [peb-nt] %p\n", pbi.PebBaseAddress);
                } else if (mode == 2) {
                    // Get the TEB via NtQueryInformationThread, then take
                    // TEB->PEB from it
                    static NtQueryInformationThread fn =
                        (NtQueryInformationThread)GetProcAddress(
                            GetModuleHandleW(L"ntdll.dll"),
                            "NtQueryInformationThread");
                    TBI tbi{};
                    NTSTATUS st = fn(ci.hThread, 0, &tbi, sizeof(tbi), nullptr);
                    wprintf(L"   [teb] status=%08lX teb=%p\n", (unsigned long)st,
                            tbi.TebBaseAddress);
                    if (tbi.TebBaseAddress) {
                        uintptr_t peb = 0;
                        if (read_remote(ci.hProcess, (uintptr_t)tbi.TebBaseAddress + 0x60,
                                        &peb, sizeof(peb))) {
                            wprintf(L"   [teb] peb=%p\n", (void*)peb);
                        } else {
                            wprintf(L"   [teb] read TEB->PEB FAILED\n");
                        }
                    }
                }

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
    wprintf(L"[SUMMARY mode=%d creates=%d exits=%d rootExit=%lu]\n", mode, creates,
            exits, rootExit);
    CloseHandle(pi.hProcess);
    return 0;
}
