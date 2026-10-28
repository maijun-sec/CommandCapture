// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Probe v9: measure the real impact of BeingDebugged on the observed
// program.
//
// The problem: DEBUG_PROCESS makes the target's PEB.BeingDebugged = 1.
// That makes the anti-debug logic behave differently —— but how big is
// the impact really?
//
// This probe:
//   1. reads the target's own BeingDebugged
//   2. tries to **write back** PEB.BeingDebugged = 0 from the debugger
//   3. lets the target read it again to verify that the write took effect
//
// Conclusion (measured): the write does take effect, but only while "the
// target has not reached its own code yet" —— because the loader's initial
// breakpoint is before main, so the debugger gets a chance to change it
// at that moment.
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

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (need <= 0) return L"";
    std::wstring out(need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], need);
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: DbgFlag <target.exe> [args...]\n");
        return 1;
    }

    // Convert to a wide character command line
    std::wstring wcmd;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) wcmd += L" ";
        wcmd += L"\"";
        wcmd += utf8_to_wide(argv[i]);
        wcmd += L"\"";
    }
    std::wstring wexe = utf8_to_wide(argv[1]);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(wexe.c_str(), &wcmd[0], nullptr, nullptr, FALSE,
                              DEBUG_PROCESS, nullptr, nullptr, &si, &pi);
    if (!ok) { printf("CreateProcessW failed: %lu\n", GetLastError()); return 2; }
    CloseHandle(pi.hThread);

    static NtQIP fn = (NtQIP)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");

    uint64_t last = GetTickCount64();
    int patched = 0;

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
                PBI pbi{};
                fn(ci.hProcess, 0, &pbi, sizeof(pbi), nullptr);
                if (pbi.PebBaseAddress) {
                    unsigned char flag = 0;
                    if (read_remote(ci.hProcess,
                                    (uintptr_t)pbi.PebBaseAddress + 0x02,
                                    &flag, 1)) {
                        printf("[pid=%lu] BeingDebugged before patch = %d\n",
                               ev.dwProcessId, flag);
                        if (flag) {
                            unsigned char zero = 0;
                            if (write_remote(ci.hProcess,
                                             (uintptr_t)pbi.PebBaseAddress + 0x02,
                                             &zero, 1)) {
                                unsigned char check = 0;
                                read_remote(ci.hProcess,
                                            (uintptr_t)pbi.PebBaseAddress + 0x02,
                                            &check, 1);
                                printf("[pid=%lu] patched -> %d (%s)\n",
                                       ev.dwProcessId, check,
                                       check == 0 ? "OK" : "FAILED");
                                patched++;
                            } else {
                                printf("[pid=%lu] WriteProcessMemory FAILED err=%lu\n",
                                       ev.dwProcessId, GetLastError());
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

    DWORD rootExit = 0;
    GetExitCodeProcess(pi.hProcess, &rootExit);
    printf("[SUMMARY patched=%d rootExit=%lu]\n", patched, rootExit);
    CloseHandle(pi.hProcess);
    return 0;
}
