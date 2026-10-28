// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

#include "capture/peb_reader.h"

#include <windows.h>

#include <cstring>

#include "core/command_line.h"

namespace ccap {

// Forward declaration (defined at the end of the file)
std::string wideToUtf8(const std::wstring& w);

namespace {

// PROCESS_BASIC_INFORMATION is not defined in the public SDK, so it is
// declared here.
// The struct layout is a stable ABI (the convention of ntdll's
// NtQueryInformationProcess).
typedef struct _CCAP_PROCESS_BASIC_INFORMATION {
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
} CCAP_PROCESS_BASIC_INFORMATION;

// Offsets in RTL_USER_PROCESS_PARAMETERS.
//
// Important: the **field offsets are the same** on x86 and x64
// (0x38 / 0x60 / 0x70),
// the difference is only in the **width of the pointers themselves** (4 bytes
// vs 8 bytes).
//
// The pitfall hit before: assuming x86 had a different set of offsets
// (0x24 / 0x38 / 0x40),
// which made the CommandLine read out as garbage when reading a 32-bit
// MSBuild,
// and ReadProcessMemory reported err=998 directly.
// Measured (raw PEB dump):
//   PEB+0x20 = ProcessParameters
//   PP+0x38 = CurrentDirectory
//   PP+0x60 = ImagePathName
//   PP+0x70 = CommandLine
constexpr size_t kOffCommandLine = 0x70;
constexpr size_t kOffCurrentDir = 0x38;
constexpr size_t kOffImagePath = 0x60;

// The offset of PEB->ProcessParameters: 0x20 on x64, 0x18 on x86.
// But measurement shows that a WOW64 target also yields the correct
// ProcessParameters with 0x20
// (NtQueryInformationProcess returns the PEB from the 64-bit perspective even
// under WOW64),
// so 0x20 is used uniformly.
constexpr size_t kOffPebProcessParameters = 0x20;

bool readRemote(HANDLE hProcess, uintptr_t address, void* buf, size_t size) {
    if (!address || size == 0) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), buf,
                             size, &got) && got == size;
}

// Read a pointer, interpreted with the target process's bit width
bool readRemotePtr(HANDLE hProcess, uintptr_t address, bool is64bit,
                     uintptr_t* out) {
    if (is64bit) {
        uint64_t v = 0;
        if (!readRemote(hProcess, address, &v, sizeof(v))) return false;
        *out = static_cast<uintptr_t>(v);
        return true;
    }
    uint32_t v = 0;
    if (!readRemote(hProcess, address, &v, sizeof(v))) return false;
    *out = static_cast<uintptr_t>(v);
    return true;
}

// Read a UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; }
//
// The struct width depends on the bit width of the target process:
//   x64: Length(2) MaxLen(2) pad(4) Buffer(8)  -> Buffer at offset 8
//   x86: Length(2) MaxLen(2) Buffer(4)          -> Buffer at offset 4
//
// **It must be read according to the process bit width; guessing by "does the
// value read look plausible" is not allowed.**
// Measured on 32-bit MSBuild: reading with the 16-byte layout yields
// len=7950, buf=0x685421CD4C01B821
// -- "plausible looking" garbage -- so the probing logic breaks and the
// subsequent ReadProcessMemory reports err=998.
//
// Pointer width: **always read as 8 bytes**.
// Measured in the RTL_USER_PROCESS_PARAMETERS of a WOW64 target (32-bit
// MSBuild),
// the UNICODE_STRING still has the 8-byte pointer layout (Length@0,
// Buffer@8),
// because NtQueryInformationProcess gives the PEB from the 64-bit
// perspective.
bool readUnicodeString(HANDLE hProcess, uintptr_t usAddr, std::wstring* out) {
    if (!usAddr) return false;

    unsigned char raw[16] = {0};
    if (!readRemote(hProcess, usAddr, raw, sizeof(raw))) return false;

    USHORT length = 0;
    uint64_t buffer64 = 0;
    std::memcpy(&length, raw, sizeof(length));
    std::memcpy(&buffer64, raw + 8, sizeof(buffer64));
    uintptr_t buffer = static_cast<uintptr_t>(buffer64);

    // Sanity check: a command line cannot be longer than 32K characters
    if (length == 0) {
        out->clear();
        return true;
    }
    if (length > 32768 || buffer == 0) return false;

    std::wstring buf(static_cast<size_t>(length) / sizeof(wchar_t), L'\0');
    if (!readRemote(hProcess, buffer, &buf[0], length)) return false;
    *out = std::move(buf);
    return true;
}

// Get the target PEB address + the parent process pid
//
// **WOW64 has two PEBs**, and this is where mistakes happen most easily:
//
//   NtQueryInformationProcess(ProcessBasicInformation)      -> PEB64
//   NtQueryInformationProcess(ProcessWOW64Information)      -> PEB32
//
// In a 64-bit debugger, for a 32-bit target process:
//   - its 32-bit code reads PEB32 (fs:[0x30])
//   - ProcessBasicInformation gives PEB64 (the WOW64 shadow PEB)
//
// Measured: ProcessBasicInformation gives 0x914000, ProcessWOW64Information
// gives 0x915000, while the PEB the target reports itself is 0x915000 -- a
// difference of 0x1000.
//
// Patching PEB64 has no effect on the target at all (BeingDebugged is still
// 1).
// The patch has to go to PEB32.
uintptr_t getPebAddress(HANDLE hProcess, DWORD pid, bool isWow64,
                          uint32_t* parentPid, uintptr_t* peb32Out) {
    typedef LONG (NTAPI * NtQueryInformationProcess)(
        HANDLE, ULONG, PVOID, ULONG, PULONG);
    static NtQueryInformationProcess fn =
        reinterpret_cast<NtQueryInformationProcess>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                           "NtQueryInformationProcess"));
    if (!fn) return 0;

    CCAP_PROCESS_BASIC_INFORMATION pbi{};
    NTSTATUS st = fn(hProcess, 0, &pbi, sizeof(pbi), nullptr);

    // InheritedFromUniqueProcessId is the parent process pid.
    // Do not guess at offsets inside the PEB -- measured, reading PEB+0x08
    // yields 0xFFFFFFFF,
    // because that field is not at 0x08 in the x64 PEB.
    if (parentPid && st >= 0) {
        *parentPid = static_cast<uint32_t>(pbi.InheritedFromUniqueProcessId);
    }

    if (isWow64) {
        // Get the 32-bit PEB (ProcessWOW64Information = 26)
        //
        // But it is **only used to patch BeingDebugged**.
        // The command line has to be read from PEB64 -- the
        // RTL_USER_PROCESS_PARAMETERS inside PEB32
        // uses 4-byte pointers throughout,
        // and its field offsets are completely different from the 8-byte
        // pointer x64 layout,
        // so copying the x64 offsets over yields garbage.
        //
        // So **two** addresses are returned here:
        //   peb64 -> read the command line / image path / working directory
        //   peb32 -> patch BeingDebugged (only this one field's offset is
        //   certain)
        ULONG_PTR peb32 = 0;
        NTSTATUS st32 = fn(hProcess, 26, &peb32, sizeof(peb32), nullptr);
        if (st32 >= 0 && peb32 && peb32Out) {
            *peb32Out = static_cast<uintptr_t>(peb32);
        }
    }

    if (st < 0) return 0;
    return reinterpret_cast<uintptr_t>(pbi.PebBaseAddress);
}

}  // namespace

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                   static_cast<int>(w.size()), nullptr, 0,
                                   nullptr, nullptr);
    if (need <= 0) return "";
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &out[0], need, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                   static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return L"";
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], need);
    return out;
}

std::vector<std::string> splitWideCommandLine(const std::wstring& w) {
    std::string ansi = wideToUtf8(w);
    return splitWindowsCommandLine(ansi);
}

PebInfo readPebInfo(HANDLE hProcess, DWORD pid, bool clearBeingDebugged) {
    PebInfo info;
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) return info;

    // IsWow64Process only makes sense on a 64-bit OS:
    //   false -> the target is 64-bit too
    //   true  -> the target is 32-bit (running on WOW64)
    bool is64bit = true;
    {
        BOOL wow = FALSE;
        if (IsWow64Process(hProcess, &wow)) {
            is64bit = (wow == FALSE);
        } else {
            // IsWow64Process failed -> conservatively treat it as 64-bit
            is64bit = true;
        }
    }

    uintptr_t peb32 = 0;
    uintptr_t peb = getPebAddress(hProcess, pid, !is64bit, &info.parentPid,
                                    &peb32);
    if (!peb) return info;

    // Clear the BeingDebugged flag.
    //
    // **It has to go to PEB32** (the one the WOW64 target actually uses).
    // There is also a BeingDebugged byte in PEB64, but 32-bit code reads
    // PEB32,
    // and patching PEB64 has no effect on the target at all.
    //
    // Offset: **0x02 on both x64 and x86**. Measured by scanning byte by byte:
    //   32-bit PEB: +0x00=00 +0x01=00 +0x02=01(BeingDebugged) +0x03=04 ...
    //   64-bit PEB: +0x00=00 +0x01=00 +0x02=01(BeingDebugged) +0x03=04 ...
    // The layout is the same on both sides.
    if (clearBeingDebugged) {
        constexpr size_t kOffBeingDebugged = 0x02;

        uintptr_t targetPeb = is64bit ? peb : (peb32 ? peb32 : peb);
        unsigned char zero = 0;
        if (WriteProcessMemory(hProcess,
                               reinterpret_cast<LPVOID>(targetPeb +
                                                       kOffBeingDebugged),
                               &zero, 1, nullptr)) {
            info.beingDebuggedCleared = true;
        } else if (getenv("CCAP_DEBUG_CMDLINE")) {
            fprintf(stderr,
                    "[ccap-peb] pid=%lu clear BeingDebugged FAILED err=%lu\n",
                    pid, GetLastError());
        }
    }

    // PEB->ProcessParameters:
    //   x64 -> PEB+0x20
    //   WOW64 also measures as 0x20, and the pointer width is still 8 bytes
    //   (NtQueryInformationProcess returns the PEB from the 64-bit perspective
    //   even under WOW64)
    //
    // So **8 bytes are always read here**; it must not vary with is_64bit.
    uintptr_t procParams = 0;
    {
        uint64_t v = 0;
        if (!readRemote(hProcess, peb + kOffPebProcessParameters, &v, sizeof(v))) {
            return info;
        }
        procParams = static_cast<uintptr_t>(v);
    }
    if (!procParams) return info;

    std::wstring cl;
    if (readUnicodeString(hProcess, procParams + kOffCommandLine, &cl)) {
        info.commandLine = cl;
        info.cmdLineUtf8 = wideToUtf8(cl);
    }

    std::wstring ip;
    if (readUnicodeString(hProcess, procParams + kOffImagePath, &ip)) {
        info.imagePath = ip;
    }

    // CURDIR { UNICODE_STRING DosPath; HANDLE Handle; }
    std::wstring cd;
    if (readUnicodeString(hProcess, procParams + kOffCurrentDir, &cd)) {
        info.currentDir = cd;
    }

    info.ok = !info.commandLine.empty() || !info.imagePath.empty();
    return info;
}

}  // namespace ccap
