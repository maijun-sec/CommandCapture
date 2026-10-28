// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Probe v7: diagnose the exact step at which the PEB read fails
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>

typedef LONG (NTAPI * NtQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);
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
    BOOL ok = ReadProcessMemory(h, (LPCVOID)addr, buf, size, &got);
    if (!ok) {
        printf("      ReadProcessMemory FAILED err=%lu addr=%p size=%zu\n",
               GetLastError(), (void*)addr, size);
        return false;
    }
    if (got != size) {
        printf("      ReadProcessMemory PARTIAL got=%zu want=%zu\n", got, size);
        return false;
    }
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: DbgTree7 <exe> [args...]\n"); return 1; }

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

    int creates = 0;
    uint64_t last = GetTickCount64();

    for (;;) {
        DEBUG_EVENT ev{};
        BOOL got = WaitForDebugEvent(&ev, 100);
        if (!got) {
            if (GetTickCount64() - last > 8000) break;
            continue;
        }
        last = GetTickCount64();
        DWORD cont = DBG_CONTINUE;

        switch (ev.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT: {
                creates++;
                auto& ci = ev.u.CreateProcessInfo;
                printf("[CREATE pid=%lu]\n", ev.dwProcessId);

                static NtQIP fn = (NtQIP)GetProcAddress(
                    GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
                PBI pbi{};
                NTSTATUS st = fn(ci.hProcess, 0, &pbi, sizeof(pbi), nullptr);
                printf("   NtQIP status=%08lX peb=%p ppid=%llu\n",
                       (unsigned long)st, pbi.PebBaseAddress,
                       (unsigned long long)pbi.InheritedFromUniqueProcessId);

                BOOL wow = FALSE;
                IsWow64Process(ci.hProcess, &wow);
                printf("   IsWow64=%d\n", wow ? 1 : 0);

                if (pbi.PebBaseAddress) {
                    size_t pp_off = wow ? 0x10 : 0x20;
                    uintptr_t pp = 0;
                    printf("   reading PEB+0x%02X -> ProcessParameters\n", pp_off);
                    if (read_remote(ci.hProcess, (uintptr_t)pbi.PebBaseAddress + pp_off,
                                    &pp, sizeof(pp))) {
                        printf("   ProcessParameters=%p\n", (void*)pp);
                        // CommandLine offset
                        size_t cl_off = wow ? 0x40 : 0x70;
                        unsigned char raw[16] = {0};
                        printf("   reading PP+0x%02X -> UNICODE_STRING\n", cl_off);
                        if (read_remote(ci.hProcess, pp + cl_off, raw, 16)) {
                            USHORT len = 0; uintptr_t buf = 0;
                            memcpy(&len, raw, 2);
                            memcpy(&buf, raw + 8, 8);
                            printf("   UNICODE_STRING len=%u buf=%p\n", len, (void*)buf);
                            if (len && buf) {
                                std::wstring s(len/2, 0);
                                if (read_remote(ci.hProcess, buf, &s[0], len)) {
                                    printf("   CommandLine=[%ls]\n", s.c_str());
                                }
                            }
                        }
                    }
                }

                if (ci.hFile) CloseHandle(ci.hFile);
                if (ci.hProcess) CloseHandle(ci.hProcess);
                if (ci.hThread) CloseHandle(ci.hThread);
                break;
            }
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
    printf("[SUMMARY creates=%d]\n", creates);
    CloseHandle(pi.hProcess);
    return 0;
}
