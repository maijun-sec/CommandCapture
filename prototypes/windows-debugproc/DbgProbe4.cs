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

class DbgProbe4 {
    [StructLayout(LayoutKind.Sequential)]
    struct DEBUG_EVENT {
        public uint dwDebugEventCode, dwProcessId, dwThreadId;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 160)] public byte[] u;
    }
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool WaitForDebugEvent(out DEBUG_EVENT ev, uint ms);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool ContinueDebugEvent(uint pid, uint tid, uint status);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DebugActiveProcess(int pid);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int size, out IntPtr read);

    const uint CREATE_PROCESS_DEBUG_EVENT = 3;
    const uint EXIT_PROCESS_DEBUG_EVENT = 5;

    static IntPtr Ptr(byte[] b, int off) { return new IntPtr(BitConverter.ToInt64(b, off)); }

    static int Main(string[] args) {
        // Strategy: start a long-lived child (cmd that sleeps), attach as debugger,
        // then have IT spawn a grandchild. We watch for CREATE_PROCESS of the grandchild.
        // This proves: (a) attach works unelevated, (b) children of the debuggee are reported.
        Console.WriteLine("=== DebugActiveProcess: does it report CHILDREN of the debuggee? ===");

        var psi = new ProcessStartInfo("cmd.exe", "/c \"start /b cmd.exe /c ping -n 6 127.0.0.1 > nul & whoami.exe\"");
        psi.UseShellExecute = false;
        var p = new Process { StartInfo = psi };
        p.Start();
        Console.WriteLine("[setup] child pid=" + p.Id);

        bool attached = DebugActiveProcess(p.Id);
        Console.WriteLine("[attach] DebugActiveProcess => " + attached + " err=" + Marshal.GetLastWin32Error());
        if (!attached) { Console.WriteLine("ATTACH FAILED"); return 1; }

        int created = 0;
        var sw = Stopwatch.StartNew();
        int quiet = 0;
        while (sw.ElapsedMilliseconds < 5000 && quiet < 5) {
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
            }
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, 0);
        }
        Console.WriteLine("[total] create-events=" + created);
        try { p.WaitForExit(2000); } catch {}
        return 0;
    }
}
