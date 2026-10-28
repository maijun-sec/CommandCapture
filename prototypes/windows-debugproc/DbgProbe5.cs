// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

class DbgProbe5 {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public IntPtr lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [StructLayout(LayoutKind.Sequential)]
    struct DEBUG_EVENT {
        public uint dwDebugEventCode, dwProcessId, dwThreadId;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 160)] public byte[] u;
    }

    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern bool CreateProcessW(string app, string cmd, IntPtr pa, IntPtr ta, bool inherit,
        uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool WaitForDebugEvent(out DEBUG_EVENT ev, uint ms);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool ContinueDebugEvent(uint pid, uint tid, uint status);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int size, out IntPtr read);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool GetExitCodeProcess(IntPtr h, out uint code);

    const uint DEBUG_PROCESS = 0x00000001;
    const uint CREATE_PROCESS_DEBUG_EVENT = 3;
    const uint EXIT_PROCESS_DEBUG_EVENT = 5;
    const uint DBG_CONTINUE = 0x00010002;
    const uint DBG_EXCEPTION_NOT_HANDLED = 0x80010001;

    static IntPtr Ptr(byte[] b, int off) { return new IntPtr(BitConverter.ToInt64(b, off)); }

    static int Main(string[] args) {
        Console.WriteLine("=== DEBUG_PROCESS full-tree probe (debugger API mechanism) ===");
        // Child spawns a grandchild that writes a marker file, so we can verify execution.
        string marker = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "cc_dbg_marker.txt");
        string cmd = "cmd.exe /c \"cmd.exe /c echo GRANDCHILD_RAN > \"" + marker + "\" & whoami.exe\"";

        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        PROCESS_INFORMATION pi;
        bool ok = CreateProcessW(null, cmd, IntPtr.Zero, IntPtr.Zero, false, DEBUG_PROCESS,
                                IntPtr.Zero, null, ref si, out pi);
        Console.WriteLine("[create] ok=" + ok + " err=" + Marshal.GetLastWin32Error() + " root=" + pi.dwProcessId);
        if (!ok) return 1;

        int created = 0;
        var sw = Stopwatch.StartNew();
        int quiet = 0;
        while (sw.ElapsedMilliseconds < 8000 && quiet < 8) {
            DEBUG_EVENT ev;
            if (!WaitForDebugEvent(out ev, 200)) { quiet++; continue; }
            quiet = 0;
            if (ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
                created++;
                IntPtr childProc = Ptr(ev.u, 8);
                IntPtr namePtr  = Ptr(ev.u, 40);
                string img = "<none>";
                if (namePtr != IntPtr.Zero) {
                    IntPtr read;
                    byte[] lb = new byte[2];
                    if (ReadProcessMemory(childProc, namePtr, lb, 2, out read)) {
                        ushort len = BitConverter.ToUInt16(lb, 0);
                        if (len > 0 && len < 32768) {
                            byte[] buf = new byte[len * 2];
                            if (ReadProcessMemory(childProc, namePtr, buf, len * 2, out read))
                                img = Encoding.Unicode.GetString(buf).TrimEnd('\0');
                        }
                    }
                }
                Console.WriteLine("  [CREATE] pid=" + ev.dwProcessId + " image=" + img);
            } else if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
                Console.WriteLine("  [EXIT] pid=" + ev.dwProcessId);
            } else {
                Console.WriteLine("  [event " + ev.dwDebugEventCode + "] pid=" + ev.dwProcessId);
            }
            // IMPORTANT: DBG_CONTINUE, and for exceptions use DBG_EXCEPTION_NOT_HANDLED
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
        }
        uint exit = 0; GetExitCodeProcess(pi.hProcess, out exit);
        Console.WriteLine("[total] create-events=" + created + "  rootExitCode=0x" + exit.ToString("X8"));
        Console.WriteLine("[marker file exists] = " + System.IO.File.Exists(marker));
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 0;
    }
}
