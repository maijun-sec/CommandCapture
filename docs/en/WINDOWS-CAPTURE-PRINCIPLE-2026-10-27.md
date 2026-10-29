# Windows Capture Principle: Why the Debugger API Instead of Detours

<!-- i18n:begin -->
中文 | [**English**](../zh/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md)
<!-- i18n:end -->

**Date**: 2026-10-27
**Status**: Implemented and empirically verified

This document explains **how** CommandCapture captures a process tree on Windows, and why
the "`CreateProcessW` + `DEBUG_PROCESS`" path was chosen over Detours / ETW / WMI.

---

## 1. The Conclusion First

| Approach | Requires admin? | Sees the whole tree? | Detected by the target? | Conclusion |
|---|---|---|---|---|
| **`DEBUG_PROCESS` debugger API** | ❌ No | ✅ Kernel automatically tracks all descendants | ⚠️ `PEB.BeingDebugged == 1` | **Main path** |
| Detours (API hook) | ❌ No | ⚠️ Depends on injection, does not cover system services | ⚠️ A detour DLL shows up in the module list | Optional enhancement only |
| ETW (kernel session) | ✅ **Yes** | ✅ | ❌ | Unavailable in the current environment |
| WMI event subscription | ❌ No | ⚠️ Only notifies when the process ends | ❌ | Cannot obtain the command line |
| `DebugActiveProcess` | ❌ No | ❌ **Does not track child processes** | ⚠️ | Can only attach to a single process |

In one sentence: **the debugger API is the only combination of "non-admin + whole tree + full command line".**

---

## 2. Mechanism

### 2.1 Startup

```
CreateProcessW(target,..., DEBUG_PROCESS)
```

The meaning of the `DEBUG_PROCESS` flag is: **let the Windows kernel bring all descendant
processes of this process under debugging as well**. We do not need to do anything ourselves -- no need to hook `CreateProcess`, no need to walk the child process list, the kernel tracks it on its own.

```
ccap.exe
 └─ cmd.exe /c build.bat ← debug event: CREATE_PROCESS
 └─ whoami.exe ← debug event: CREATE_PROCESS
 └─ findstr.exe ← debug event: CREATE_PROCESS
```

### 2.2 Event Loop

```
WaitForDebugEvent loop
 ├─ CREATE_PROCESS_DEBUG_EVENT → record event
 ├─ EXCEPTION_DEBUG_EVENT → decide DBG_CONTINUE or DBG_EXCEPTION_NOT_HANDLED
 ├─ EXIT_PROCESS_DEBUG_EVENT → remove from the live set
 └─ all debugged processes exited → finish
```

**The termination criterion is "all debugged processes have exited", not "no events for a while".**

> **Why a quiet period cannot be used.**
> While `cmd.exe` runs `dir /b` across 100k files, no debug events occur --
> but the script is still running. Returning early on a quiet period misses the entire subtree.
> The correct criterion is: the root process has exited **and** all child processes have exited.

### 2.3 Where the Command Line Comes From

This is the easiest place to get tripped up:

> `CREATE_PROCESS_DEBUG_INFO` **has no `lpCommandLine` field**.

So the command line must be read from the debuggee's **PEB**:

```
PEB
 └─ ProcessParameters (PEB+0x20)
 ├─ CurrentDirectory (PP+0x38) working directory
 ├─ ImagePathName (PP+0x60) image path
 └─ CommandLine (PP+0x70) ★ command line
```

### 2.4 Where the Parent PID Comes From

`CREATE_PROCESS_DEBUG_INFO` also **contains no parent process information**.

The parent PID comes from `NtQueryInformationProcess`:

```
NtQueryInformationProcess(hProcess, ProcessBasicInformation,...)
 → PROCESS_BASIC_INFORMATION.InheritedFromUniqueProcessId
```

> **Do not guess at PEB+0x08.** Reading that location yields `0xFFFFFFFF` --
> on 64-bit it is not the parent PID.

### 2.5 32-bit Targets: WOW64 Has Two PEBs

This is the most insidious pitfall for 32-bit targets. Inside a 64-bit debugger, a 32-bit target process **has two PEBs at the same time**:

| Source | What it is |
|---|---|
| `NtQueryInformationProcess(ProcessBasicInformation)` | PEB64 (the WOW64 shadow PEB) |
| `NtQueryInformationProcess(ProcessWOW64Information)` | PEB32 ← **the one 32-bit code actually uses** |

Measured (`tests/probe/DbgPeb.cpp`):

```
ProcessBasicInformation -> peb = 0x914000
ProcessWOW64Information -> peb = 0x915000
target's own fs:[0x30] -> 0x915000
```

The difference is `0x1000`. **Patching PEB64 has no effect whatsoever on the target.**

Moreover, the `RTL_USER_PROCESS_PARAMETERS` layout differs between the two PEBs
(in PEB32 everything is a 4-byte pointer, and the field offsets do not line up with the 8-byte pointer layout of x64), therefore:

- Command line / image path / working directory → read from **PEB64**
- `BeingDebugged` patch → applied to **PEB32**

### 2.6 Handles in Debug Events Must Be Closed

The `hProcess` / `hThread` / `hFile` in `CREATE_PROCESS_DEBUG_INFO`
are all the debugger's responsibility to close.

If they are not closed, running a few thousand processes exhausts the handle table -- the symptom is that `CreateProcessW` suddenly fails halfway through, and the cause appears to have nothing to do with handles.

### 2.7 The First Breakpoint Must Use `DBG_CONTINUE`

```
DBG_CONTINUE (0x00010002) ← the initial loader breakpoint
DBG_EXCEPTION_NOT_HANDLED (0x80010001) ← exceptions thrown by the program itself afterwards
```

Handling the initial breakpoint with `DBG_EXCEPTION_NOT_HANDLED` makes the target **hang at the initial breakpoint**,
stalling after creating only 1 process.

Subsequent exceptions must be **handed back to the target**: track per thread whether the exception has already been seen, use `DBG_CONTINUE` only for the **first** loader breakpoint of each thread, and use `DBG_EXCEPTION_NOT_HANDLED` for breakpoints/exceptions thrown by the program itself.

---

## 3. Why Not Detours

Detours is Microsoft's own API hooking library. What it does is **rewrite function entry points inside the target process**,
redirecting the entry of `CreateProcessW` to our trampoline.

There is exactly one thing it does better than the debugger API: **it can hook API calls inside the target process**,
rather than only at the instant "a process is created".

But the problem we need to solve is already solved by the debugger API, and at a far lower cost.

### 3.1 Detours Cannot Solve Our Core Problem

What we want is "**which commands did this script launch**".

- Script built-in commands (`echo` / `cd` / `for`) **do not create a process at all** --
 neither Detours nor the debugger API can see them.
- The static analysis layer already covers this category, so **no injection is needed**.
- For the ones that really do create a process, the debugger API misses none.

So Detours brings **no incremental value** here.

### 3.2 The Actual Cost of Detours

| Problem | Explanation |
|---|---|
| **Injection race** | DLL injection happens some time after process startup. During that window the process's API calls are **not hooked**. `DetourCreateProcessWithDllEx` can move the injection earlier into the process initialization phase, but a window still exists. |
| **System service boundary** | Services, Session 0 processes, and certain system components enforce integrity level restrictions; a normal user cannot inject into them. |
| **API Set forwarding** | On Windows 10+, most exports in `kernel32.dll` are `ApiSet` forwards; the hooked target address and the actually executed address may not be the same, and the hook **silently fails**. |
| **Integrity level (IL) checks** | A high-IL process rejects injection from a low-IL process. |
| **EDR / anti-cheat flags** | An unfamiliar DLL appearing in the target process's module list will make many EDR products alert immediately. |
| **Must handle both 32-bit and 64-bit** | A 32-bit target needs the 32-bit version of the DLL, and both the path and the loading method differ. |
| **Cannot cover already running processes** | It can only hook processes started **afterwards**. |

### 3.3 The Debugger API Has None of These Problems

| Aspect | Debugger API | Detours |
|---|---|---|
| Permissions | A normal user is enough | A normal user is enough (but the target may refuse) |
| Covers already existing processes | ✅ `DebugActiveProcess` | ❌ |
| Covers all descendant processes | ✅ Automatically tracked by the kernel | ⚠️ Every process must be injected into |
| Pollutes the target's module list | ❌ No | ⚠️ One extra DLL |
| Obtains the full command line | ✅ Read from the PEB | ⚠️ Must be assembled by hand in the hook |
| 32-bit targets | ✅ Supported (dual PEB handling) | ⚠️ Requires a separate 32-bit DLL |
| Detected by the target | ⚠️ `BeingDebugged == 1` | ⚠️ Equally detectable |

### 3.4 When Detours Would Actually Be Valuable

If in the future we need information from **inside the target process** --
for example "which Win32 APIs did this process call" or "which files were opened" --
then Detours (or ETW's user-mode providers) would make sense.

**That is not CommandCapture's goal.** CommandCapture wants "commands",
not "system calls".

### 3.5 Conclusion

> **Most things Detours can do, the debugger API can also do, and more cleanly.
> The bit of extra capability Detours offers (hooking inside the process) happens to be exactly what CommandCapture does not need.**
>
> So the debugger API was chosen. Detours is kept as a future optional enhancement and stays out of the main path.

---

## 4. Why Not ETW

ETW (Event Tracing for Windows) is theoretically the "cleanest" solution --
the kernel provides process creation events, and **the target cannot detect it at all**.

But **administrator privileges cannot be obtained in the current environment**:

```
StartTrace() -> create kernel session
 -> ERROR_ACCESS_DENIED
```

A kernel session requires `SeSystemProfilePrivilege` (administrator).

The user-mode ETW provider (the user-mode channel of
`Microsoft-Windows-Kernel-Process`) can be used with normal privileges,
but event latency is high, and on some versions it is disabled by default, so it is unreliable.

**Conclusion: unavailable in the current environment.** If administrator privileges become obtainable some day, ETW is worth keeping as
a "zero detectability" supplementary path.

---

## 5. Why Not WMI

The `Win32_ProcessStartTrace` event:

- ✅ Does not require administrator privileges
- ❌ **Only notifies when the process ends** -- you cannot get the moment when it is "being created"
- ❌ The event contains **no full command line** (only the executable name and PID)
- ❌ The parent-child hierarchy cannot be obtained

For CommandCapture, missing the command line = useless.

---

## 6. Why Not `DebugActiveProcess`

`DebugActiveProcess` / `DEBUG_ONLY_THIS_PROCESS` can attach to a process that is **already running**.

But `DEBUG_ONLY_THIS_PROCESS` **does not track child processes** --
a child process has to call `DebugActiveProcess` itself before it is tracked.

Measured conclusion: **it can only attach to a single process and cannot capture its descendants.**

For the "run a script and see what it launches" scenario, the process has not started yet,
so `CreateProcessW(..., DEBUG_PROCESS)` is the right choice.

---

## 7. Detectability

`DEBUG_PROCESS` will always make the target see `PEB.BeingDebugged == 1`.

CommandCapture provides `--clear-being-debugged` to mitigate this:

| Detection method | Effect |
|---|---|
| `PEB.BeingDebugged` | ✅ Zeroed |
| `IsDebuggerPresent()` | ✅ Returns 0 (it reads the same flag) |
| `CheckRemoteDebuggerPresent()` | ❌ Still 1 |

The third one cannot be done: it queries the **kernel's debug object list**, not a user-mode flag,
and cannot be patched from user mode.

**Disabled by default.** Silently changing the behavior of the observed program is not good --
turn it on only when you genuinely know the target would change its behavior because of it.

---

## 8. Known Limitations (all reported in `notes`)

| Limitation | Cause | How it is reported |
|---|---|---|
| `CheckRemoteDebuggerPresent()` still returns 1 | Kernel query, cannot be changed from user mode | README "Known gaps" |
| The target's own exceptions may be swallowed | The debugger must participate in exception dispatch | Already optimized to "use `DBG_CONTINUE` only for the first loader breakpoint of each thread" |
| Force-killing the debugger may terminate the target as well | Debugger exit = target exit | The normal path waits for all child processes to exit |
| Timeout truncation | `--timeout` reached | `notes` explicitly marks `PARTIAL` |
| Static linking / setuid (Linux side) | Kernel limitation | Each event is marked individually |

**Principle: when loss is known, it must be explicitly reported as `PARTIAL` with the reason;
silent under-reporting is not allowed.**

---

## 9. Empirically Verified Coverage

Windows 10 19045, non-administrator environment.

| Scenario | Result |
|---|---|
| cmd built-in commands | ✅ Captured by the static layer |
| Direct children / both sides of a pipe | ✅ |
| Nested scripts, three levels | ✅ depth 0/1/2 |
| **cmake configure** | ✅ 38 events, including TryCompile probes |
| **cmake + Ninja** | ✅ |
| **ninja invoked directly** | ✅ `ninja` → `cmd.exe /C` → `c++.exe` → `c++` → `cc1plus` → `as` → `collect2` → `ld`, depth 5 |
| **GNU make** | ✅ `make` → `sh -c` → `g++` → `cc1plus` |
| **MSVC cl.exe invoked directly** | ✅ Command line fully read out, including `/EHsc` `/std:c++17` |
| **MSBuild (32-bit WOW64)** | ✅ Captured the hidden `cmd.exe /Q /D /C...exec.cmd` |
| **32-bit target (a true 32-bit exe)** | ✅ Command line fully read out, no arguments lost |
| **PowerShell scripts / runtime** | ✅ 12 events (git / npm / dotnet all captured) |
| Short-lived processes / redirection | ✅ |
| Anti-debugging flags | ✅ `--clear-being-debugged` |
| Process tree view / filtering / aggregation | ✅ |

Regression tests: `tests/run_tests.ps1` -- all 87 cases pass.
