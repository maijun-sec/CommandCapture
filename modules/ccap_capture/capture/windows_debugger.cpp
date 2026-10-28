// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

#include "capture/windows_debugger.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/command_line.h"
#include "core/normalize.h"
#include "capture/peb_reader.h"

namespace ccap {

namespace {

constexpr DWORD kDbgContinue = 0x00010002;      // DBG_CONTINUE
constexpr DWORD kDbgExceptionNotHandled = 0x80010001;

struct ProcInfo {
    DWORD pid = 0;
    DWORD ppid = 0;
    std::string exe;
    std::string cmdline;
    std::vector<std::string> argv;
    std::string cwd;
    int depth = 0;
    int64_t startMs = 0;
    bool exited = false;
};

// Safely trim an incomplete trailing character off a possibly truncated UTF-8
// string
std::string trimToValidUtf8(const std::string& s) {
    size_t end = s.size();
    // At most back up 4 bytes
    for (int i = 0; i < 4 && end > 0; ++i) {
        unsigned char c = static_cast<unsigned char>(s[end - 1]);
        if ((c & 0xC0) != 0x80) {
            // Check how many continuation bytes this start byte declares
            size_t need = 1;
            if ((c & 0xF8) == 0xF0) need = 4;
            else if ((c & 0xF0) == 0xE0) need = 3;
            else if ((c & 0xE0) == 0xC0) need = 2;
            if (end >= need) return s.substr(0, end);
            --end;
            continue;
        }
        --end;
    }
    return s.substr(0, end);
}

// Replace the backslashes in a path with forward slashes (the Windows API
// accepts them everywhere)
std::string forwardSlash(const std::string& p) {
    std::string s = p;
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

// Encode one token into the command line following the Windows CRT rules.
//
// The key point: a backslash inside quotes, when it comes before the closing
// quote, must be escaped in pairs.
// For example, writing the path C:\a\b as "C:\a\b" is **wrong** -- while
// parsing, the \ of \b escapes the
// closing quote, and the path ends up truncated to C:\a\b" --
// the correct spelling is "C:\a\\b" (the trailing backslash is doubled).
//
// The safest approach: always turn paths inside arguments into forward
// slashes, which the Windows API accepts completely,
// and that sidesteps the classic "backslash + quote" pitfall.
std::string encodeArg(const std::string& a) {
    bool needQuote = a.empty();
    for (char c : a) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '"' ||
            c == '\t') {
            needQuote = true;
            break;
        }
    }
    if (!needQuote) return a;

    std::string out;
    out += '"';
    for (size_t i = 0; i < a.size(); ++i) {
        char c = a[i];
        if (c == '"') {
            out += '\\';
            out += '"';
        } else if (c == '\\') {
            // Count the number of consecutive backslashes that follow, to
            // decide whether to double them
            size_t j = i;
            while (j < a.size() && a[j] == '\\') ++j;
            size_t run = j - i;
            if (j < a.size() && a[j] == '"') {
                // 2n backslashes + " => n backslashes + a literal "
                out.append(run * 2, '\\');
                out += '"';
                ++j;
            } else {
                // Not followed by a quote, keep them as they are
                out.append(run, '\\');
            }
            i = j - 1;
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

// Build the command line passed to the target process.
// The Windows rules: the first token must be the program name (optionally
// quoted), the whole thing is joined with spaces,
// and arguments that need quotes get quotes.
//
// Note: **the executable file path must keep its backslashes**.
// Measured (C:/WINDOWS/system32/cmd.exe + /c script.bat) makes cmd.exe
// fail when it re-parses the image path itself, and it directly reports
// "The syntax of the command is incorrect",
// and it creates only 1 process before exiting.
// Forward slashes are safe for script arguments, but the exe path must not
// be converted.
std::string buildCommandLine(const std::string& exe,
                               const std::vector<std::string>& args) {
    std::string cmd = encodeArg(exe);
    for (const auto& a : args) {
        cmd += ' ';
        cmd += encodeArg(a);
    }
    return cmd;
}

// Build the full command line, argv[0] is the exe
std::string buildFullCommandLine(const std::string& exe,
                                    const std::vector<std::string>& argv) {
    std::vector<std::string> rest(argv.begin() + (argv.empty() ? 0 : 1),
                                  argv.end());
    return buildCommandLine(exe, rest);
}

// Find the basename of the program name token, used to set the executable in
// the event
std::string exeFromCmdline(const std::string& cmdline) {
    std::vector<std::string> parts = splitWindowsCommandLine(cmdline);
    if (parts.empty()) return "";
    return baseName(parts[0]);
}

}  // namespace

CaptureResult runWindowsDebugger(const CaptureOptions& opts,
                                    EventCollector* collector) {
    CaptureResult result;

    if (opts.target.empty()) {
        result.error = "empty target";
        return result;
    }

    // Resolve the target: it may be a script (.bat/.cmd/.ps1), it may be an
    // executable file
    std::string target = opts.target;
    std::string ext = extensionOf(target);

    // Scripts have to be started through an interpreter
    std::string interpreter;
    if (ext == ".bat" || ext == ".cmd") {
        interpreter = "cmd.exe";
        target = "/c";
    } else if (ext == ".ps1") {
        interpreter = "powershell.exe";
        target = "-NoProfile";
    } else if (ext == ".py") {
        interpreter = "python";
        target = "";
    } else if (ext == ".sh") {
        interpreter = "bash";
        target = "";
    }

    // Assemble argv
    std::vector<std::string> argv;
    std::string exePath;

    if (!interpreter.empty()) {
        // The interpreter path must use the native separator (see the
        // comment in build_command_line)
        std::string interpPath = resolveExecutableNative(interpreter);
        exePath = interpPath.empty() ? interpreter : interpPath;
        argv.push_back(baseName(exePath));
        if (!target.empty()) argv.push_back(target);
        // Script paths uniformly use forward slashes: both cmd.exe and
        // powershell accept them,
        // and it avoids the pitfall where the CRT treats "backslash + closing
        // quote" as an escape character.
        argv.push_back(forwardSlash(opts.target));
        for (const auto& a : opts.args) argv.push_back(a);
    } else {
        exePath = resolveExecutableNative(opts.target);
        if (exePath.empty()) {
            // Try a relative path
            std::string cand = normalizePathNative(currentDir() + "/" + opts.target);
            if (fileExists(cand)) exePath = cand;
        }
        if (exePath.empty()) {
            result.error = "cannot resolve target: " + opts.target;
            return result;
        }
        argv.push_back(baseName(exePath));
        for (const auto& a : opts.args) argv.push_back(a);
    }

    std::string cmdline = buildFullCommandLine(exePath, argv);

    // Set up the startup info
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    // The target's standard handles.
    //
    // By default they inherit our stdout/stderr -- the user's build output
    // belongs on the terminal.
    // But if redirectOutputToStderr is set, stdout is pointed at stderr,
    // so that ccap's own stdout contains only the capture data (the JSON can
    // be parsed directly).
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hStdErr = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (opts.redirectOutputToStderr) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = hStdErr;
        si.hStdError = hStdErr;
        si.hStdInput = hStdIn;
    }

    PROCESS_INFORMATION pi{};

    DWORD flags = DEBUG_PROCESS | CREATE_UNICODE_ENVIRONMENT;
    if (opts.redirectOutputToStderr) {
        // If STARTF_USESTDHANDLES is passed, bInheritHandles must be TRUE,
        // otherwise the child process does not get these handles.
        flags |= CREATE_NO_WINDOW;
    }

    // Build the environment block
    std::wstring envBlock;
    {
        LPWCH env = GetEnvironmentStringsW();
        if (env) {
            for (LPWCH p = env; *p; p += wcslen(p) + 1) {
                envBlock += p;
                envBlock.push_back(L'\0');
            }
            FreeEnvironmentStringsW(env);
        }
    }

    std::wstring wExe = utf8ToWide(exePath);
    std::wstring wCmd = utf8ToWide(cmdline);
    std::wstring wCwd = utf8ToWide(opts.cwd);

    BOOL ok = CreateProcessW(
        wExe.c_str(),
        &wCmd[0],
        nullptr, nullptr,
        opts.redirectOutputToStderr ? TRUE : FALSE,
        flags,
        envBlock.empty() ? nullptr : &envBlock[0],
        wCwd.empty() ? nullptr : wCwd.c_str(),
        &si, &pi);

    if (!ok) {
        DWORD err = GetLastError();
        result.error = "CreateProcessW failed: " + std::to_string(err);
        // Degrade gracefully: try once more without DEBUG_PROCESS, so the user
        // at least sees the real error
        STARTUPINFOW si2{};
        si2.cb = sizeof(si2);
        PROCESS_INFORMATION pi2{};
        BOOL ok2 = CreateProcessW(wExe.c_str(), &wCmd[0], nullptr, nullptr,
                                  FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr,
                                  wCwd.empty() ? nullptr : wCwd.c_str(),
                                  &si2, &pi2);
        if (ok2) {
            WaitForSingleObject(pi2.hProcess, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(pi2.hProcess, &code);
            CloseHandle(pi2.hThread);
            CloseHandle(pi2.hProcess);
            result.exitCode = static_cast<int>(code);
            result.error += " (ran without capture)";
        }
        return result;
    }

    CloseHandle(pi.hThread);

    result.rootPid = static_cast<int>(pi.dwProcessId);

    // Process table
    std::unordered_map<DWORD, ProcInfo> procs;
    std::unordered_map<DWORD, DWORD> parentOf;
    std::vector<DWORD> creationOrder;   // record pids in creation order
    std::set<DWORD> live;

    // Root process
    {
        ProcInfo root;
        root.pid = pi.dwProcessId;
        root.ppid = 0;
        root.exe = baseName(exePath);
        root.cmdline = cmdline;
        root.argv = argv;
        root.cwd = opts.cwd.empty() ? currentDir() : opts.cwd;
        root.depth = 0;
        root.startMs = nowUnixMs();
        procs[root.pid] = root;
        live.insert(root.pid);
    }

    // Event loop
    int64_t lastEventMs = nowUnixMs();
    int64_t startMs = lastEventMs;
    bool rootExited = false;

    // The set of thread ids that have already seen an exception.
    // It is used to tell "the loader's initial breakpoint" apart from "a
    // breakpoint thrown by the program itself".
    std::set<DWORD> threadSeenException;

    for (;;) {
        if (opts.timeoutMs > 0 && nowUnixMs() - startMs > opts.timeoutMs) {
            result.timedOut = true;
            // Kill the whole tree
            for (DWORD pid : live) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                if (h) {
                    TerminateProcess(h, 1);
                    CloseHandle(h);
                }
            }
            break;
        }

        DEBUG_EVENT ev{};
        DWORD timeoutMs = 100;
        BOOL got = WaitForDebugEvent(&ev, timeoutMs);

        if (!got) {
            // Timeout tick.
            //
            // The key point: the end **must not** be decided by "no events for
            // a while".
            // While cmd.exe runs a builtin command (for example dir /b over
            // 100k files),
            // there is not a single debug event, but the script is still
            // running and the child processes have not been created yet.
            // Returning early would lose the whole subtree.
            //
            // The correct criterion: the root process has exited **and** all
            // child processes have exited.
            // The live set only drops a pid on EXIT_PROCESS_DEBUG_EVENT,
            // so live.empty() is equivalent to "the whole tree is over".
            if (live.empty()) {
                break;
            }

            // Safety net: the root process has exited, but some child
            // processes are hanging around.
            // Give them a quiet period and force the end on timeout, so the
            // whole capture tool cannot hang.
            if (rootExited && nowUnixMs() - lastEventMs > opts.quietMs) {
                result.notes.push_back(
                    "dangling child processes after root exit; forcing stop");
                for (DWORD pid : live) {
                    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                    if (h) {
                        TerminateProcess(h, 1);
                        CloseHandle(h);
                    }
                }
                break;
            }
            continue;
        }

        lastEventMs = nowUnixMs();
        DWORD pid = ev.dwProcessId;

        switch (ev.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT: {
                auto& ci = ev.u.CreateProcessInfo;
                HANDLE hProcess = ci.hProcess;

                // The file handle must be closed, otherwise the target
                // process may not get some resources
                if (ci.hFile) CloseHandle(ci.hFile);

                std::string cl;
                std::string image;
                std::string cwd;
                std::vector<std::string> args;
                // The image path comes from CREATE_PROCESS_DEBUG_INFO
                // (lpImageName may be NULL,
                // which is common on Windows, so it is only a fallback)
                if (ci.lpImageName) {
                    if (ci.fUnicode) {
                        image = wideToUtf8(
                            reinterpret_cast<const wchar_t*>(ci.lpImageName));
                    } else {
                        image = reinterpret_cast<const char*>(ci.lpImageName);
                    }
                }

                // The command line can only be read from the debuggee's PEB
                // -- the CREATE_PROCESS_DEBUG_INFO
                // struct has no lpCommandLine field at all.
                PebInfo peb = readPebInfo(hProcess, pid,
                                           opts.clearBeingDebugged);
                if (peb.ok) {
                    cl = peb.cmdLineUtf8;
                    if (peb.imagePath.size()) {
                        image = wideToUtf8(peb.imagePath);
                    }
                    cwd = wideToUtf8(peb.currentDir);
                    if (!cl.empty()) {
                        args = splitWindowsCommandLine(cl);
                    }
                }

                std::string exe;
                if (!args.empty()) {
                    exe = baseName(args[0]);
                } else if (!image.empty()) {
                    exe = baseName(image);
                }
                if (exe.empty()) {
                    exe = exeFromCmdline(cl);
                }

                // Parent process pid: it can only be obtained from the PEB's
                // InheritedFromUniqueProcessId.
                // CREATE_PROCESS_DEBUG_EVENT carries no parent process
                // information.
                DWORD ppid = peb.parentPid;
                if (ppid == 0) {
                    // Fallback: debug events arrive in creation order, so the
                    // parent is always recorded before the child,
                    // and therefore "the most recently recorded, still alive
                    // process" is most likely the parent.
                    for (auto it = creationOrder.rbegin();
                         it != creationOrder.rend(); ++it) {
                        if (*it != pid) {
                            ppid = *it;
                            break;
                        }
                    }
                }

                // depth = parent depth + 1
                int depth = 0;
                {
                    auto it = procs.find(ppid);
                    if (it != procs.end()) depth = it->second.depth + 1;
                }

                ProcInfo piNew;
                piNew.pid = pid;
                piNew.ppid = ppid;
                piNew.exe = exe;
                piNew.cmdline = cl;
                piNew.argv = args;
                piNew.cwd = cwd;
                piNew.depth = depth;
                piNew.startMs = nowUnixMs();
                procs[pid] = piNew;
                parentOf[pid] = ppid;
                creationOrder.push_back(pid);
                live.insert(pid);

                // Record the event
                if (collector) {
                    CommandEvent e;
                    e.timestampMs = piNew.startMs;
                    e.platform = Platform::Windows;
                    e.source = EventSource::Runtime;
                    e.pid = pid;
                    e.ppid = ppid;
                    e.executable = exe;
                    e.argv = args;
                    e.rawCommandLine = cl;
                    e.cwd = cwd;
                    e.kind = "process";
                    e.depth = depth;
                    e.resolved = !image.empty();
                    if (!image.empty()) e.notes.push_back("image=" + image);
                    if (args.empty() && !cl.empty()) {
                        e.notes.push_back("argv_parse_failed");
                    }
                    collector->add(std::move(e));
                }

                // Continue right away, so the target does not get stuck at the
                // initial breakpoint
                ContinueDebugEvent(pid, ev.dwThreadId, kDbgContinue);

                // The process/thread handles in the debug event are the
                // debugger's responsibility to close.
                // Not closing them exhausts the handle count after a few
                // thousand processes.
                if (ci.hProcess) CloseHandle(ci.hProcess);
                if (ci.hThread) CloseHandle(ci.hThread);
                break;
            }

            case EXIT_PROCESS_DEBUG_EVENT: {
                auto it = procs.find(pid);
                if (it != procs.end()) {
                    it->second.exited = true;
                }
                live.erase(pid);
                if (pid == static_cast<DWORD>(result.rootPid)) {
                    rootExited = true;
                    DWORD code = 0;
                    GetExitCodeProcess(pi.hProcess, &code);
                    result.exitCode = static_cast<int>(code);
                }
                ContinueDebugEvent(pid, ev.dwThreadId, kDbgContinue);
                break;
            }

            case EXCEPTION_DEBUG_EVENT: {
                // Exception handling policy:
                //
                // 1. The first exception of every new thread is the initial
                //    breakpoint placed by the loader
                //    (STATUS_BREAKPOINT). It **must** be released with
                //    DBG_CONTINUE,
                //    otherwise the target gets stuck at the initial
                //    breakpoint and only 1 process is created.
                //
                // 2. Every exception thrown by the program itself afterwards
                //    uses DBG_EXCEPTION_NOT_HANDLED,
                //    handed back to the target's own exception handling logic.
                //    Early versions used DBG_CONTINUE for all
                //    STATUS_BREAKPOINTs,
                //    which swallowed the program's own breakpoints (debugger
                //    assertions, Delphi/C++ __debugbreak),
                //    changing the behaviour of the observed program.
                //
                // 3. Single-step exceptions (STATUS_SINGLE_STEP) are handed
                //    back as well.
                DWORD code = ev.u.Exception.ExceptionRecord.ExceptionCode;
                bool firstInThread = (ev.u.Exception.dwFirstChance != 0);

                // Record "this thread has seen an exception"
                auto& seen = threadSeenException;
                bool isFirstForThread = seen.insert(ev.dwThreadId).second;

                if (code == STATUS_BREAKPOINT && isFirstForThread &&
                    firstInThread) {
                    // The loader's initial breakpoint
                    ContinueDebugEvent(pid, ev.dwThreadId, kDbgContinue);
                } else {
                    // The program's own exception: do not interfere
                    ContinueDebugEvent(pid, ev.dwThreadId, kDbgExceptionNotHandled);
                }
                break;
            }

            case EXIT_THREAD_DEBUG_EVENT:
            case CREATE_THREAD_DEBUG_EVENT:
            case LOAD_DLL_DEBUG_EVENT: {
                if (ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
                    auto& di = ev.u.LoadDll;
                    if (di.hFile) CloseHandle(di.hFile);
                } else if (ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
                    // New thread: its first exception is also the loader's
                    // initial breakpoint.
                    // Record it here so the exception handling logic knows
                    // this thread is "new".
                    threadSeenException.erase(ev.dwThreadId);
                } else if (ev.dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT) {
                    threadSeenException.erase(ev.dwThreadId);
                }
                ContinueDebugEvent(pid, ev.dwThreadId, kDbgContinue);
                break;
            }

            default:
                ContinueDebugEvent(pid, ev.dwThreadId, kDbgContinue);
                break;
        }
    }

    // Wrap up: if the root process is still running, wait for it to finish
    if (!rootExited) {
        // On the timeout path we have already called TerminateProcess -- the
        // process is about to die,
        // so there is no need to wait 5 seconds.
        //
        // This used to wait unconditionally for 5000ms, which made
        // --timeout 2000 actually take 7 seconds
        // (2 seconds of timeout + 5 seconds of idle waiting). The user sets a
        // 2 second timeout but waits 7 seconds;
        // that is not "a bit slow", it means **the timeout parameter is not
        // respected**.
        //
        // The timeout path uses 500ms as a fallback (TerminateProcess
        // completes almost immediately),
        // the non-timeout path keeps 5000ms -- at that point the process may
        // indeed be finishing up normally.
        DWORD waitMs = result.timedOut ? 500 : 5000;
        WaitForSingleObject(pi.hProcess, waitMs);
        DWORD code = 0;
        if (GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE) {
            result.exitCode = static_cast<int>(code);
        }
    }

    CloseHandle(pi.hProcess);

    // The depth was already computed in CREATE_PROCESS_DEBUG_EVENT from the
    // parent process relationship
    // (the parent pid comes from PEB->InheritedFromUniqueProcessId).
    // There is no need to backfill it here -- collector.events() returns a
    // const reference,
    // and the earlier backfill loop wrote to a temporary object, which was an
    // ineffective no-op.

    return result;
}

}  // namespace ccap
