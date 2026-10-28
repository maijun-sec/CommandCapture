// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// Probe v8: scan the x86 RTL_USER_PROCESS_PARAMETERS to find the correct
// offset of CommandLine
// The approach: in the range 16..0x80 read every offset with the 8-byte
// UNICODE_STRING layout and pick the one whose Length is plausible and
// whose Buffer points at readable memory.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>

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

// Try to read a wchar string at a candidate address
static bool try_string(HANDLE h, uintptr_t bufAddr, size_t len, std::wstring* out) {
    if (!bufAddr || len == 0 || len > 32768) return false;
    std::wstring s(len / 2, 0);
    if (!read_remote(h, bufAddr, &s[0], len)) return false;
    *out = std::move(s);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: DbgTree8 <exe> [args...]\n"); return 1; }

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

    uint64_t last = GetTickCount64();
    int done = 0;

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
                if (done++ > 1) break;   // only look at the first two
                auto& ci = ev.u.CreateProcessInfo;

                static NtQIP fn = (NtQIP)GetProcAddress(
                    GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
                PBI pbi{};
                fn(ci.hProcess, 0, &pbi, sizeof(pbi), nullptr);

                BOOL wow = FALSE;
                IsWow64Process(ci.hProcess, &wow);
                printf("\n[pid=%lu wow64=%d peb=%p]\n", ev.dwProcessId,
                       wow ? 1 : 0, pbi.PebBaseAddress);

                size_t pp_off = wow ? 0x10 : 0x20;
                uintptr_t pp = 0;
                if (!read_remote(ci.hProcess, (uintptr_t)pbi.PebBaseAddress + pp_off,
                                 &pp, wow ? 4 : 8)) {
                    printf("  read ProcessParameters FAILED\n");
                    break;
                }
                printf("  ProcessParameters=%p\n", (void*)pp);

                // First dump the PEB itself to see the real x86 PEB layout
                {
                    unsigned char pebraw[64] = {0};
                    size_t got = 0;
                    if (ReadProcessMemory(ci.hProcess,
                                          (LPCVOID)pbi.PebBaseAddress,
                                          pebraw, sizeof(pebraw), &got)) {                        printf("  --- PEB raw (%zu bytes) ---\n", got);
                        for (size_t i = 0; i < got; i += 16) {
                            printf("  PEB+0x%02X:", (unsigned)i);
                            for (size_t j = 0; j < 16 && i + j < got; ++j) {
                                printf(" %02X", pebraw[i + j]);
                            }
                            printf("\n");
                        }
                        // Print every dword of the first 0x40 bytes of the PEB
                        // as a pointer
                        printf("  --- PEB dwords as ptr32 ---\n");
                        for (size_t i = 0; i < 0x40; i += 4) {
                            uint32_t v = 0;
                            std::memcpy(&v, pebraw + i, 4);
                            if (!v) continue;
                            unsigned char sig[4] = {0};
                            BOOL rd = ReadProcessMemory(ci.hProcess,
                                                       (LPCVOID)(uintptr_t)v,
                                                       sig, 2, &got);
                            const char* tag = "";
                            if (rd && sig[0] == 'M' && sig[1] == 'Z') {
                                tag = "<= MZ image";
                            } else {
                                // Try it as ProcessParameters and look for a
                                // UNICODE_STRING
                                for (size_t o = 0x18; o <= 0x60; o += 4) {
                                    unsigned char raw2[16] = {0};
                                    if (!ReadProcessMemory(ci.hProcess,
                                                           (LPCVOID)((uintptr_t)v + o),
                                                           raw2, 16, &got)) {
                                        continue;
                                    }
                                    USHORT L = 0; uint32_t B = 0;
                                    std::memcpy(&L, raw2, 2);
                                    std::memcpy(&B, raw2 + 4, 4);
                                    if (!L || L > 2048 || !B) continue;
                                    std::wstring s;
                                    if (!try_string(ci.hProcess, B, L, &s)) continue;
                                    if (s.find(L'\\') == std::wstring::npos &&
                                        s.find(L'/') == std::wstring::npos &&
                                        s.find(L'.') == std::wstring::npos) {
                                        continue;
                                    }
                                    tag = "<= PP? cmdline at +0x??";
                                    break;
                                }
                            }
                            printf("   PEB+0x%02X = %08X %s\n", (unsigned)i, v, tag);
                        }
                    }
                }

                // Try PEB+0x20 as the ProcessParameters (64-bit layout)
                {
                    uintptr_t pp64 = 0;
                    if (read_remote(ci.hProcess,
                                    (uintptr_t)pbi.PebBaseAddress + 0x20,
                                    &pp64, 8)) {
                        printf("  PEB+0x20 = %p\n", (void*)pp64);
                        for (size_t o = 0x10; o <= 0x80; o += 8) {
                            unsigned char raw2[16] = {0};
                            if (!read_remote(ci.hProcess, pp64 + o, raw2, 16)) continue;
                            USHORT L = 0; uint64_t B = 0;
                            std::memcpy(&L, raw2, 2);
                            std::memcpy(&B, raw2 + 8, 8);
                            if (!L || L > 4096 || !B) continue;
                            std::wstring s;
                            if (!try_string(ci.hProcess, (uintptr_t)B, L, &s)) continue;
                            printf("     PP+0x%02X len=%u -> [%ls]\n", (unsigned)o, L,
                                   s.c_str());
                        }
                    }
                }

                // Scan
                for (size_t off = 0x10; off <= 0x90; off += 4) {
                    unsigned char raw[16] = {0};
                    if (!read_remote(ci.hProcess, pp + off, raw, sizeof(raw))) continue;

                    USHORT len16 = 0;
                    uint32_t buf32 = 0;
                    uint64_t buf64 = 0;
                    std::memcpy(&len16, raw, 2);
                    std::memcpy(&buf32, raw + 4, 4);
                    std::memcpy(&buf64, raw + 8, 8);

                    uintptr_t buf = wow ? (uintptr_t)buf32 : (uintptr_t)buf64;
                    if (!buf) continue;

                    std::wstring s;
                    if (!try_string(ci.hProcess, buf, len16, &s)) continue;
                    if (len16 == 0) continue;
                    // Keep only the ones that look like a path or a command
                    // line
                    if (s.find(L'\\') == std::wstring::npos &&
                        s.find(L'/') == std::wstring::npos &&
                        s.find(L'.') == std::wstring::npos) {
                        continue;
                    }
                    printf("  ** off=0x%02X len=%u buf=%p -> [%ls]\n",
                           (unsigned)off, len16, (void*)buf, s.c_str());
                }

                // Raw byte dump
                {
                    unsigned char blob[0x100] = {0};
                    size_t got = 0;
                    if (ReadProcessMemory(ci.hProcess, (LPCVOID)pp, blob,
                                          sizeof(blob), &got)) {
                        printf("  --- raw dump of ProcessParameters (%zu bytes) ---\n", got);
                        for (size_t i = 0; i < got; i += 16) {
                            printf("  +0x%0302X:", (unsigned)i);
                            for (size_t j = 0; j < 16 && i + j < got; ++j) {
                                printf(" %02X", blob[i + j]);
                            }
                            printf("\n");
                        }
                    } else {
                        printf("  raw dump FAILED\n");
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
