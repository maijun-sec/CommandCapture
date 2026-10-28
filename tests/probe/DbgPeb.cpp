// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Probe v10: confirm which PEB NtQueryInformationProcess returns for a
// WOW64 target
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
    return ReadProcessMemory(h, (LPCVOID)addr, buf, size, &got) && got == size;
}
static bool write_remote(HANDLE h, uintptr_t addr, const void* buf, size_t size) {
    SIZE_T put = 0;
    return WriteProcessMemory(h, (LPVOID)addr, buf, size, &put) && put == size;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: DbgPeb <exe> [args...]\n"); return 1; }

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

    static NtQIP fn = (NtQIP)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");

    uint64_t last = GetTickCount64();

    for (;;) {
        DEBUG_EVENT ev{};
        BOOL got = WaitForDebugEvent(&ev, 100);
        if (!got) {
            if (GetTickCount64() - last > 5000) break;
            continue;
        }
        last = GetTickCount64();
        DWORD cont = DBG_CONTINUE;

        switch (ev.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT: {
                auto& ci = ev.u.CreateProcessInfo;

                BOOL wow = FALSE;
                IsWow64Process(ci.hProcess, &wow);

                PBI pbi{};
                NTSTATUS st = fn(ci.hProcess, 0, &pbi, sizeof(pbi), nullptr);

                printf("pid=%lu wow64=%d NtQIP=%08lX peb=%p\n",
                       ev.dwProcessId, wow ? 1 : 0, (unsigned long)st,
                       pbi.PebBaseAddress);

                if (pbi.PebBaseAddress) {
                    // dump the first 0x20 bytes to find BeingDebugged
                    // (offset 0x02)
                    unsigned char raw[0x20] = {0};
                    if (read_remote(ci.hProcess,
                                    (uintptr_t)pbi.PebBaseAddress, raw,
                                    sizeof(raw))) {
                        printf("  PEB+0x00: ");
                        for (int i = 0; i < 0x20; ++i) {
                            printf("%02X ", raw[i]);
                        }
                        printf("\n  byte@0x02 = %d\n", raw[0x02]);
                        printf("  byte@0x08 = %d\n", raw[0x08]);
                        printf("  byte@0x0A = %d\n", raw[0x0A]);

                        // Try writing 0x02 and 0x08 separately to see which
                        // one really takes effect
                        unsigned char zero = 0;
                        BOOL w2 = write_remote(ci.hProcess,
                                               (uintptr_t)pbi.PebBaseAddress + 0x02,
                                               &zero, 1);
                        unsigned char c2 = 0xFF;
                        read_remote(ci.hProcess,
                                    (uintptr_t)pbi.PebBaseAddress + 0x02,
                                    &c2, 1);
                        printf("  write@0x02 ok=%d now=%d\n", w2 ? 1 : 0, c2);

                        BOOL w8 = write_remote(ci.hProcess,
                                               (uintptr_t)pbi.PebBaseAddress + 0x08,
                                               &zero, 1);
                        unsigned char c8 = 0xFF;
                        read_remote(ci.hProcess,
                                    (uintptr_t)pbi.PebBaseAddress + 0x08,
                                    &c8, 1);
                        printf("  write@0x08 ok=%d now=%d\n", w8 ? 1 : 0, c8);
                    }
                }

                // WOW64: the target process reads a different PEB, one in the
                // 32-bit address space.
                // Use ProcessWOW64Information to get the 32-bit PEB address.
                if (wow) {
                    typedef LONG (NTAPI * NtQIP2)(HANDLE, ULONG, PVOID,
                                                 ULONG, PULONG);
                    static NtQIP2 fn2 = (NtQIP2)GetProcAddress(
                        GetModuleHandleW(L"ntdll.dll"),
                        "NtQueryInformationProcess");
                    // ProcessWOW64Information = 26
                    ULONG_PTR peb32 = 0;
                    NTSTATUS st2 = fn2(ci.hProcess, 26, &peb32,
                                       sizeof(peb32), nullptr);
                    printf("  ProcessWOW64Information status=%08lX peb32=%p\n",
                           (unsigned long)st2, (void*)peb32);
                    if (st2 >= 0 && peb32) {
                        unsigned char raw32[0x20] = {0};
                        if (read_remote(ci.hProcess, (uintptr_t)peb32, raw32,
                                        sizeof(raw32))) {
                            printf("  PEB32+0x00: ");
                            for (int i = 0; i < 0x20; ++i) {
                                printf("%02X ", raw32[i]);
                            }
                            printf("\n  PEB32 byte@0x02=%d byte@0x08=%d\n",
                                   raw32[0x02], raw32[0x08]);

                            // Try writing byte by byte in the PEB32 to see which
                            // byte affects BeingDebugged
                            for (size_t bo = 0x00; bo <= 0x0C; ++bo) {
                                unsigned char orig = 0;
                                read_remote(ci.hProcess,
                                            (uintptr_t)peb32 + bo, &orig, 1);
                                unsigned char z = 0;
                                write_remote(ci.hProcess, (uintptr_t)peb32 + bo,
                                             &z, 1);
                                unsigned char now = 0xFF;
                                read_remote(ci.hProcess,
                                            (uintptr_t)peb32 + bo, &now, 1);
                                printf("    PEB32+0x%02X: orig=%02X -> now=%02X\n",
                                       (unsigned)bo, orig, now);
                            }

                            unsigned char zero = 0;
                            write_remote(ci.hProcess, (uintptr_t)peb32 + 0x08,
                                         &zero, 1);
                            unsigned char ck = 0xFF;
                            read_remote(ci.hProcess, (uintptr_t)peb32 + 0x08,
                                        &ck, 1);
                            printf("  PEB32 write@0x08 -> %d\n", ck);
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
    CloseHandle(pi.hProcess);
    return 0;
}
