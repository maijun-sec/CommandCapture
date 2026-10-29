# CommandCapture Research Report: Cross-Platform Script/Build Command Capture

<!-- i18n:begin -->
中文 | [**English**](../zh/RESEARCH-COMMAND-CAPTURE-2026-10-27.md)
<!-- i18n:end -->

**Date**: 2026-10-27 (**v2 revision**: the Windows path was changed to the debugger API approach, with local measurements attached)
**Scope**: Survey of command-capture mechanisms on both Linux and Windows, evaluating the feasibility of "pass in a script and capture every command it launches, directly or indirectly"
**Summary of conclusions**: Feasible. **Use `LD_PRELOAD` on Linux and the debugger API (`DEBUG_PROCESS`) on Windows** -- neither requires **administrator privileges**. The technical paths for the two platforms cannot be designed symmetrically.

> **About the product names in this document**: this report is a **survey**; its value lies precisely in citing each vendor's **public documentation** as evidence
> --remove the citations and the conclusions can no longer be re-verified. So this document keeps those names.
>
> However, the **product code, tests, and user-facing documentation** no longer contain them
> (see [README](../../README.en.md) and [DESIGN-MODULES-2026-10-27](DESIGN-MODULES-2026-10-27.md)).
> The only one retained in the code is the **frontend wrapper name that actually appears in the logs**
> --and even that was changed to a **tool-agnostic generic spelling** (`sa-translate` / `tool-emit` /...),
> because what the parser needs to recognize is the **shape "there is a wrapper in front of the compiler"**, not any particular vendor's spelling.

---

## 0. One-Sentence Conclusion

| Platform | Feasibility | Recommended primary path | Requires administrator | Main risks |
|---|---|---|---|---|
| Linux | High (verified by local measurement) | `LD_PRELOAD` interception of `execve`/`posix_spawn` + environment variable "self-healing" | No | Statically linked binaries, setuid, SELinux/AppArmor |
| Windows | **High** | **Debugger API `DEBUG_PROCESS` (the proven approach of comparable tools)** + script-layer parsing | **No** (verified locally) | cmd.exe **shell builtins produce no process**; the debuggee knows it is being debugged |

> **Major v2 revision**: the first edition recommended ETW for Windows. After checking that tool's official documentation and measuring locally, **ETW requires administrator privileges** (creating a kernel session fails with `Access is denied`), whereas **the debugger API approach it uses requires no elevation whatsoever**. The user's constraint of "not being able to obtain higher privileges" directly moved the approach from ETW to the debugger API.

> **The two most critical findings (Windows)**:
> 1. `cmd.exe`'s **shell builtins** (`echo`/`dir`/`copy`/`set`/`cd`/`for` and roughly 30 others) **do not create child processes at all**, so no process-level mechanism can **capture them** → a script static-analysis layer must be added.
> 2. **`DEBUG_PROCESS` can capture the entire process tree without administrator privileges** → no ETW, no injection, no elevation.

---

## 0.5 Revision Log: Why the Windows Approach Changed

The first edition concluded "use ETW + Detours to fill the gaps on Windows". The user pointed out that **administrator privileges are not available**,
which pushed me to investigate **how comparable commercial static analysis tools do it**. What I found split into two halves:

**This class of tool does not use hooks on Windows at all -- it uses the debugger API.** The official documentation of a commercial static analysis tool
(Windows platform page), verbatim:

> "The cov-build command uses a **different mechanism** to capture compiler process creation on Windows than on UNIX platforms. The **Windows version of cov-build runs the build command and its child processes in debug mode.**"

"runs... in debug mode" in that sentence means `CreateProcess` with the `DEBUG_PROCESS` flag. The knowledge base further explains the capture principle:

> "我们监视由 'make' 创建的 '进程/线程生成' 活动...当产生一个新进程/线程时, 在中间目录中的文件 `build-log.txt` 中捕获此信息, 然后我们查看正在启动的可执行文件的名称"

**Verified by local measurement (local machine, non-administrator, Windows 10 19045):**

| Mechanism | Requires administrator | Number of CREATE_PROCESS events captured | Coverage depth |
|---|---|---|---|
| `CreateProcessW` + `DEBUG_PROCESS` | **No** | **3** (root/child/grandchild) | **The entire tree** |
| `DebugActiveProcess` | **No** | 1 (only the attached one) | Does not follow children |
| ETW Kernel-Process | **Yes** | -- | The entire tree, but requires elevation |

The prototype code for `DEBUG_PROCESS` is in `prototypes/windows-debugproc/`, and the measured output is:

```
[create] ok=True err=0 root=6420
 [CREATE] pid=6420 <- cmd.exe
 [CREATE] pid=8808 <- cmd.exe (grandchild) 
 [CREATE] pid=3596 <- whoami.exe (great-grandchild) 
[total] create-events=3
[marker file exists] = True <- proves the debuggee really executed
```

> Incidental clarification: the `java-capture.properties` in this class of tool
> (the declarative configuration set with `compiler.classes` / `compiler.methods` / `arg.positions`)
> **is a different thing** -- it is a **JVM-level** hook (`com.sun.tools.javac.Main.compile`),
> used for Java capture and **unrelated to OS-level process-creation capture**. Don't be misled by its "Detours" lineage.

---

## 1. Requirement Breakdown

User request: pass in a script and capture **every command that script launches, directly or indirectly**.

This actually contains three layers, which must be handled separately:

```
L0 the commands the script itself executes (e.g. cmd.exe /c build.bat)
L1 the "actions" built into the script interpreter (e.g. cmd echo/copy -- no process is created)
L2 the external processes the script spawns (e.g. cl.exe / git inside build.bat)
```

L2 is the main battlefield for LD_PRELOAD / Detours / ETW. **L1 can only be handled by parsing the script source.** This is the essential difference between this project and the class of tools that "only care about compiler invocations":
the goal of that class of tools is entirely L2; what this project needs is "all commands".

---

## 2. Linux Side: Mainstream Implementation Mechanisms

### 2.1 Core Mechanism

A typical tool of this class (which uses LD_PRELOAD to intercept exec and writes the results back into the compilation database) works like this:

```
<tool> -- make -j8
```

1. **Injection**: `LD_PRELOAD=<tool>/libexec.so` (macOS uses `DYLD_INSERT_LIBRARIES`)
2. **Interception**: override a series of variadic libc functions such as `execve` / `execl` / `execlp` / `execvpe` / `posix_spawn` / `posix_spawnp`
3. **Reporting**: the intercepted process sends (path, argv[], cwd) back to the collector process over a **TCP socket**
4. **Environment self-healing**: the most easily overlooked yet most critical piece of design -- see 2.2
5. **Degradation**: if the target program is statically linked (the dynamic linker does not take effect), fall back to **wrapper mode** (replace `cc`/`gcc` etc. with same-named wrapper scripts, record the call, then forward it to the real compiler)

> Key implementation locations:
> - `src/intercept-preload/src/implementation.rs` -- hook implementations for the various exec functions
> - `src/intercept-preload/src/c/shim.c` -- C shim for the variadic functions
> - `src/intercept-preload/src/session.rs` -- session management
> - `src/environment.rs` -- environment variable setup

### 2.2 Environment Variable "Self-Healing" Mechanism (Must Read)

**Problem**: many build systems clear or rewrite environment variables. If `LD_PRELOAD` is cleared, deep child processes escape.

**What this class of tool does**: inside the hooked `execve`, **write `LD_PRELOAD` and the collector address back** into the environment block that is about to be passed to the child process.

```rust
// the semantics of that tool's intercept-preload/src/session.rs
// when an environment variable is detected as cleared, LD_PRELOAD and INTERCEPT_COLLECTOR_ADDRESS are restored automatically
```

**Meaning for this project**: on Windows there is a **completely corresponding** mechanism -- the `PATH` variable being cleared, `cmd.exe`'s `/D` switch, PowerShell's `$env:PATH` override. The design must treat these equivalently.

### 2.3 Known Limitations on Linux

| Limitation | Reason | Mitigation |
|---|---|---|
| Statically linked binaries cannot be captured | The dynamic linker does not participate | Wrapper mode (mature implementations already exist) |
| setuid/setgid programs | The kernel discards LD_PRELOAD | Cannot be circumvented; must be recorded as a known gap |
| SELinux / AppArmor restrictions | Policy forbids preloading | Policy must be relaxed, or ptrace used as the alternative |
| Variants of `posix_spawn` | glibc internals may bypass it | Must cover `posix_spawn{,p}`, and note that glibc turns them into `clone+execve` |
| Script shell builtins (`cd`/`echo`/`export`) | Not processes | Same as Windows; needs script-layer parsing |

### 2.4 Linux Alternative: ptrace + seccomp-BPF

If LD_PRELOAD is unavailable (e.g. the target process is statically linked), you can degrade to the ptrace approach:

- `PTRACE_TRACEME` + `PTRACE_O_TRACEEXEC` / `PTRACE_O_TRACEFORK` / `PTRACE_O_TRACECLONE` / `PTRACE_O_TRACEVFORK`
- **seccomp-BPF pre-filtering**: use `SECCOMP_RET_TRACE` to trigger a ptrace stop only for `execve`/`execveat`, letting all other syscalls run at full speed

This is exactly what `grith.ai` does, and the published performance numbers are **P95 < 50µs per interception, end-to-end < 5% slowdown**.

> Performance comparison (compiled from devmindset.dev):
> | Approach | Cost per event | Impact on the traced process |
> |---|---|---|
> | ptrace (PTRACE_SYSCALL) | Stop + schedule + extra system calls | Stops at every syscall boundary |
> | ptrace + seccomp-BPF | Same, but **only** for filtered syscalls | Stops only on matching calls |
> | eBPF (tracepoints) | BPF executes inside the kernel | **No stops** |

### 2.5 Choosing a Linux Approach

| Scenario | Recommendation |
|---|---|
| General build/script capture | **LD_PRELOAD** -- good performance, clear semantics |
| Target is statically linked / preload is blocked | ptrace + seccomp-BPF as the alternative |
| Long-running resident monitoring | eBPF (but cannot obtain the full argv, so it needs to be combined) |

---

## 3. Windows Side: Debugger API (the Proven Approach of Comparable Tools)

### 3.0 First, Correct a Premise

The user mentioned that "a certain commercial static analysis tool uses detours". The verification result:

**This class of tool uses the debugger API on Windows, not Detours.**

Official documentation (Windows platform page), verbatim:

> "The cov-build command uses a different mechanism to capture compiler process creation on Windows than on UNIX platforms. **The Windows version of cov-build runs the build command and its child processes in debug mode.** If your build system has problems running in debug mode, try using the `--instrument` option with `cov-build`."

The official knowledge base states the mechanism clearly:

> "在构建系统上运行时, 'cov-build' 基本上是为执行构建而生成的进程/线程的**'包装器'**...我们监视由 'make' 创建的**'进程/线程生成'活动**...当产生一个新进程/线程时, 在中间目录中的文件 **`build-log.txt`** 中捕获此信息, 然后我们查看正在启动的可执行文件的名称"

**"runs in debug mode" = `CreateProcess` with the `DEBUG_PROCESS` flag.** This is the so-called "wrapper" --
that wrapping command *is* the debugger.

As for `java-capture.properties` (the `compiler.classes` / `compiler.methods` / `compiler.signatures` / `arg.positions` set), that is **a different thing**: a JVM-level hook (`com.sun.tools.javac.Main.compile`) used for Java capture and **unrelated to OS-level process-creation capture**. Its declarative configuration format (6 parallel lists correlated by index) is indeed related to the Detours lineage, but that is a different use.

### 3.1 Approach One: Debugger API (Recommended Primary Path)

#### 3.1.1 The Difference Between the Three Debugger Modes (This Is the Key to the Choice)

| Mode | Constant | Follows child processes | Purpose |
|---|---|---|---|
| `DEBUG_PROCESS` | `0x00000001` | **Yes** (the entire tree) | **This is what comparable tools use** |
| `DEBUG_ONLY_THIS_PROCESS` | `0x00000002` | No | Debug only the direct child process |
| `DebugActiveProcess` | API | **No** | Attach to an already-running process (equivalent to `DEBUG_ONLY_THIS_PROCESS`) |

Microsoft's definition of `DEBUG_PROCESS`:

> "DEBUG_PROCESS sets up the parent/child relationship so that the debugger will receive debug events from a process being debugged **and any other processes created by that process**."

**This difference is the watershed for whether this project can obtain "indirectly launched commands".** `DebugActiveProcess` follows only one process; `DEBUG_PROCESS` follows the entire tree.

#### 3.1.2 Local Measurement (Key Evidence)

Prototype code: `prototypes/windows-debugproc/DbgProbe5.cs`

**Test environment**: Windows 10 19045.6466, **non-administrator session** (`IsAdmin: False`)

The command being debugged:
```
cmd.exe /c "cmd.exe /c echo GRANDCHILD_RAN > <marker> & whoami.exe"
```

**Result**:
```
[create] ok=True err=0 root=6420
 [CREATE] pid=6420 <- cmd.exe (root)
 [CREATE] pid=8808 <- cmd.exe (child)
 [CREATE] pid=3596 <- whoami.exe (grandchild)
 [EXIT] pid=3596
 [EXIT] pid=8808
 [EXIT] pid=6420
[total] create-events=3 rootExitCode=0x00000000
[marker file exists] = True
```

**Three conclusions**:
1. ✅ **No administrator privileges required** -- `CreateProcess` with `DEBUG_PROCESS` works perfectly in an ordinary user session
2. ✅ **The entire tree is visible** -- all three layers of `CREATE_PROCESS_DEBUG_EVENT` (root → child → grandchild) were received
3. ✅ **The debuggee really executed** -- the marker file was written, proving it was not "suspended and starved"

Control group (`DbgProbe4.cs`, `DebugActiveProcess`):
```
[attach] DebugActiveProcess => True err=0
 [CREATE] pid=2664
[total] create-events=1 <- only the one that was attached
```

#### 3.1.3 Implementation Notes (Pitfalls Encountered)

```c
// 1. STARTUPINFO must be passed, not IntPtr.Zero
STARTUPINFO si = { sizeof(si) };
CreateProcessW(NULL, cmd, NULL, NULL, FALSE, DEBUG_PROCESS,
 NULL, NULL, &si, &pi); // passing NULL for si gives ok=False err=0
```

```c
// 2. The status of ContinueDebugEvent must be DBG_CONTINUE (0x00010002), not 0
// with 0 the debuggee does not continue after the first exception
// symptom: only 1 CREATE_PROCESS event is received and then it hangs
ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
```

**Event type table**:

| Value | Event | Description |
|---|---|---|
| 1 | `EXCEPTION_DEBUG_EVENT` | Noise; requires correct status handling |
| 2 | `CREATE_THREAD_DEBUG_EVENT` | Noise |
| **3** | **`CREATE_PROCESS_DEBUG_EVENT`** | **← what we want** |
| 4 | `EXIT_THREAD_DEBUG_EVENT` | Noise |
| 5 | `EXIT_PROCESS_DEBUG_EVENT` | Used to finish off the process tree |
| 6 | `LOAD_DLL_DEBUG_EVENT` | **Extremely noisy** (measured 20+ per startup), must be filtered |

**Getting the command line**: `CREATE_PROCESS_DEBUG_INFO` layout

```
hFile(0,8) hProcess(8,8) hThread(16,8) lpBaseOfImage(24,8)
dwDebugInfoFileOffset(32,4) dwDebugInfoSize(36,4) lpImageName(40,8) fUnicode(48,4)
```

- `lpImageName` is at offset **40**
- You must use **the debuggee's own handle** (the `hProcess` at offset **8**) to `ReadProcessMemory`; reading remote process memory with a pseudo-handle (`GetCurrentProcess()`) **will fail**
- After obtaining `lpImageName` you still have to read the child process's PEB to get the full command line (`lpCommandLine`)

**Event loop**: you must not exit as soon as you receive an `EXIT`; use quiet-tick counting (only conclude after N consecutive `WaitForDebugEvent` timeouts), otherwise you will miss sibling processes that are still running.

#### 3.1.4 Known Limitations

| Limitation | Description | Response |
|---|---|---|
| **The debuggee knows it is being debugged** | PEB.BeingDebugged = 1 | A few tools change behavior. This project provides `--clear-being-debugged` (off by default) to mitigate; the alternative wrapper-style capture is described in §3.1.5 |
| **Debugger exit = debuggee gets killed** | System behavior | Exception handling / signal forwarding is mandatory; the debugger must not exit unexpectedly |
| **LOAD_DLL event noise** | Measured 20+ per startup | Filtering by event type is mandatory, otherwise throughput collapses |
| **Exception handling policy** | `DBG_CONTINUE` swallows first-chance exceptions | Distinguish `DBG_EXCEPTION_NOT_HANDLED` (unhandled exception handed back to the program) from `DBG_CONTINUE` |
| **WOW64** | x86 target under an x64 debugger | Register context requires `Wow64GetThreadContext`; **command-line capture is unaffected** (it is read directly from the PEB) |
| **cmd.exe shell builtins** | They produce no process | Solved at the script layer; see Section 4 |
| **Performance** | One kernel round trip per event | Filter out LOAD_DLL / CREATE_THREAD / EXIT_THREAD |

#### 3.1.5 Alternative Path: `--instrument` (A Capture Method That Doesn't Use the Debugger)

The Windows debugger approach has one side effect that cannot be avoided: **the debuggee knows it is being debugged**
(`PEB.BeingDebugged = 1`, and `IsDebuggerPresent()` also returns 1).
A few tools change behavior as a result -- anti-cheat, some JIT, self-verification code.

The alternative offered by a commercial static analysis tool is an `--instrument` switch. Its positioning (official text):

> "If your build system has problems running in debug mode, try using the `--instrument`
> option with `cov-build`."

> `--instrument` -- Compatible with Java, C, C++, C#, and Visual Basic builds on
> **Windows only**, uses the **instrumentation mode instead of the debugger**.
> For certain builds, this configuration can significantly improve build times.
> In particular, **parallel builds will benefit most from `--instrument`**.

**How it is implemented -- it is not "a different kind of hook", it is a different "interception point"**:

| | Debugger mode (default) | `--instrument` mode |
|---|---|---|
| Interception point | **Process creation** (`CreateProcess`) | **Compiler invocation** (compile invocation) |
| How the command line is obtained | Debug events + reading the child process's PEB | The compiler wrapper obtains argv directly |
| Coverage | **The entire process tree** (any program) | **Compiler invocations only** (C/C++/Java/C#/VB) |
| Can non-compiler processes be seen | Yes | **No** |
| Does the debuggee know | **Yes** | **No** |
| Relation to parallel builds | The debugger has to take over processes one by one, severely serializing | It does not take over processes, so **parallel builds can truly run in parallel** |

The key difference is that **the interception point has changed**:

- **Debugger mode** intercepts at the **OS's process-creation boundary**, so it can see the entire tree;
 the cost is that every process gets attached to a debugger (the process knows, and throughput is dragged down by debug events).
- **`--instrument` mode** intercepts at the **compiler-invocation boundary** -- it relies on a wrapper
 placed between the compiler and the build system (the build system calls the wrapper, the wrapper forwards to the real compiler,
 and records the command at the same time). This way **the target process has no idea it was ever "processed"**,
 but the cost is that **only the class of events that is compiler invocation is covered**.

**Two hard constraints (these are what prevent it from replacing debugger mode):**

1. **It is not a general-purpose process-capture mechanism.** It only works for compiled languages
 and cannot catch invocations of other programs during the build. For this project's requirement of "capturing **all** commands
 launched by a script", it is inherently **incompletely covering**.
2. **It is mutually exclusive with parallel emit.** The official documentation explicitly states:
 `--parallel-emit` "has no effect and **cannot be used in combination with
 `--instrument`**". In other words, choosing `--instrument` means losing parallel compilation acceleration
 (`--parallel-emit` is on by default on Windows).

**The scenario it explicitly mentions** happens to expose a real weakness of debugger mode:

> "This option might be useful is for capturing a **32-bit javac compilation on
> 64-bit Windows**."

-- When capturing a 32-bit `javac` on 64-bit Windows, debugger mode tends to hit problems
because of the complexity of WOW64's dual PEB.

**The trade-off for this project:**

| Requirement | Debugger mode | `--instrument`-style wrapper |
|---|---|---|
| Capture **all** commands during the build | ✅ | ❌ Compiler invocations only |
| Capture non-compiler programs | ✅ | ❌ |
| Target behavior unchanged | ⚠️ Changed by default (`--clear-being-debugged` can mitigate) | ✅ |
| Parallel build throughput | ⚠️ Limited | ✅ |

So this project's choice is **to make the debugger API the primary path**, and to provide
`--clear-being-debugged` (off by default) as a mitigation;
**wrapper-style capture is not implemented** -- because it cannot satisfy the "all commands" requirement.

> Reference: the official Windows platform documentation of a commercial static analysis tool (debugger mode description),
> plus <https://sig-synopsys.my.site.com/community/s/article/Improving-Build-and-Analysis-Time>
> (the description of the `--instrument` / `--parallel-emit` mutual exclusion)

### 3.2 Approach Two: Detours (Not Recommended as the Primary Path)

Detours itself is capable enough (inline trampolines, transactional attach/detach, x86/x64/ARM/ARM64 support, MIT license, single interception < 400ns), but it is **not suitable as the primary path for "capturing the entire tree"**:

| Problem | Description |
|---|---|
| Injection race | Child processes forked by the target before injection completes will be **missed** |
| Requires injecting into system services | `CreateProcessAsUserW` (UAC elevation) goes through the AppInfo service, `ShellExecuteEx` goes through the DcomLaunch service; both are `svchost.exe`, and ordinary applications cannot inject into them |
| API Set forwarding | `CreateProcessAsUserW` actually lives in `api-ms-win-core-processthreads-l1-1-0.dll`, so hooking only the kernel32 export misses it |
| WOW64 | Requires both 32-bit and 64-bit DLLs |
| Integrity level | UAC isolation; a low-privilege process cannot inject into a high-privilege process |
| Static linking | Same as Linux; if the target is statically linked the hook does not reach it |
| Flagged by EDR | Inline hooks are suspicious behavior |

**Scenarios where Detours is appropriate** (if needed):
- Finer-grained parameters are required than the debugger API provides (e.g. the full parameters of `CreateProcessInternal`, job object inheritance relationships)
- Behavior needs to be modified rather than merely observed

**But for this project, the requirement to "observe all commands" is already fully covered by the debugger API, with zero privilege requirements.**

### 3.3 Approach Three: ETW (Eliminated by the Privilege Constraint)

ETW is a kernel-level event stream; `Microsoft-Windows-Kernel-Process` + keyword `0x10` can obtain `CommandLine`:

```powershell
logman create trace CCProbe -p "Microsoft-Windows-Kernel-Process" 0x10 4 -o C:\ccprobe.etl -ets
```

**Locally measured: creating the session fails immediately with `Access is denied`**, because creating a kernel session requires administrator privileges.

| Dimension | ETW | Debugger API |
|---|---|---|
| Requires administrator | **Yes** | **No** |
| Requires injection | No | No |
| Covers the entire tree | Yes | **Yes** |
| Misses child processes | No | No (`DEBUG_PROCESS`) |
| Elevated processes | Yes | Requires the target to elevate itself (`DebugActiveProcess` has equivalent privileges) |
| Flagged by EDR | No | No |
| Captures cmd builtins | No | No |
| Child processes spawned by statically linked programs | **Yes** (kernel level) | Yes (created at kernel level, the debugger receives the event) |

**ETW's only technical advantage** is that it is "kernel level" -- it can catch child processes spawned by statically linked programs. But the debugger API is likewise notified when the kernel creates a process, so **this advantage does not actually exist**.

→ **Under the constraint of "administrator privileges are unavailable", ETW is eliminated outright.**

### 3.4 Approach Four: WMI / CIM Polling (Supplement Only)

`Get-CimInstance Win32_Process` can obtain `ProcessId` / `ParentProcessId` / `Name` / `CommandLine`.

**Problem**: polling misses short-lived processes. In local measurements, `cmd.exe /c echo etw_probe_marker` exited immediately after starting, so it was already gone from the list by the time of the query.

**Conclusion**: usable only as a **supplement** (reconstructing the process tree afterwards, cross-validation), **not as the primary capture channel**.

### 3.5 Overall Comparison Table of Windows Approaches

| Approach | Requires administrator | Follows the whole tree | Misses short-lived processes | Captures builtins | Complexity | Recommendation |
|---|---|---|---|---|---|---|
| **Debugger API `DEBUG_PROCESS`** | **No** | **Yes** | No | No | Medium | **✅ Primary path** |
| ETW Kernel-Process | Yes | Yes | No | No | Medium | ❌ Eliminated by privileges |
| Detours injection | Depends on the target | Yes | Possibly (race) | No | High | ⚠️ Enhancement only |
| WMI polling | No | No | **Yes** | No | Low | ⚠️ Supplement only |
| Script static analysis | No | N/A | N/A | **Yes** | High | **✅ Mandatory (the other leg)** |

## 4. Script Layer: Where L1 Is Actually Solved

Since L1 (shell builtins) is invisible under any process-level approach, the only way out is to **parse the script source**.

### 4.0 Why `cd` Cannot Be Hooked -- And How "It's Obviously Obtainable" Actually Works

This is the most commonly misunderstood point, so let's clear it up first.

#### 4.0.1 `cd` Is Not a Process at the OS Level At All

The semantics of `cd` are **change the current process's working directory**. And the working directory is a **per-process** property --
a child process inherits a copy from its parent, and changing its own copy has no effect on the parent.

So `cd` **can only** be implemented by the shell itself inside its own process (calling `SetCurrentDirectory` /
`chdir`). It cannot possibly be an external program:

```
shell (cwd = /home/me)
 └─ fork → the child process runs "cd /tmp"
 the child process's cwd becomes /tmp
 the child process exits
shell (cwd = /home/me) ← nothing happened
```

If `cd` were an external program, it would change the directory in vain -- the parent's directory would not change along with it.
**This is the fundamental reason every shell makes `cd` a builtin command** -- not a performance choice,
but a semantic constraint.

Similar cases include `exit` (it must terminate the shell itself, not a child process),
`export`/`unset` (they must modify the shell's own environment variables),
and `source`/`.` (they must execute in the current shell process rather than forking a new one).

`echo`/`pwd` are different: they **can** be external programs (`/bin/echo` does exist),
but they are made builtins to save the fork overhead.

#### 4.0.2 Empirical Evidence (Local Machine, Windows 10)

**Evidence A: a script that runs only builtins produces 0 child processes at runtime.**

```bat
@echo off
cd C:\Windows
echo hello
set FOO=bar
dir > nul 2>&1
if 1==1 echo cond
```

```
$ ccap run builtins_only.bat --no-scan --format text
[000001] d=0 RT pid=16684 cmd.exe /c.../builtins_only.bat
```

Only cmd.exe itself. **7 builtins, 0 runtime events.**

**Evidence B: switch to an external command and events appear immediately.**

```bat
@echo off
where.exe cmd
```

```
[000001] d=0 RT pid=17452 cmd.exe /c.../external.bat
[000002] d=1 RT pid=19044 where.exe cmd ← there really is a process
```

**Evidence C: builtins are marked `ST` (static layer) in the output, not `RT` (runtime layer).**

```
[000002] d=0 ST pid=- cd C:\Windows [builtin.bat:2] (builtin)
```

`pid=-` means "**no process**".

#### 4.0.3 So How Does "It's Obviously Obtainable" Work?

The key point: **it is not obtained by a "process-level mechanism"; it is obtained by "reading the script source".**

Any tool that wants to obtain `cd` goes down the same path:

1. **Know where the script file is** (the build system knows, or it can be learned from the command-line arguments);
2. **Read that file**;
3. **Parse it itself** to determine which are builtin commands and which are external programs.

Step 3 is "static analysis". The only difference is **how well the parsing is done**:
some tools only recognize a few lines of `COMPILING`/`EXECUTING` logs (which only covers compiler invocations),
while others do real lexical analysis of the script (which is required to cover builtin commands).

**So "being able to obtain `cd`" and "being able to capture processes" are two different capabilities.**
The former requires **reading scripts**, the latter requires **observing processes**. Any claim of "we obtain `cd` via a hook" is
either static analysis misnamed as a hook, or that tool supplements the parsing elsewhere.

#### 4.0.4 Why Static Analysis Has an Inherent Boundary

Because it reads **text**, not **execution**:

| Case | Can static analysis obtain it | Why |
|---|---|---|
| `cd C:\Windows` | ✅ | That is literally the line in the text |
| `cd %MYDIR%` | ⚠️ | You have to expand the variable to know the actual directory |
| `cd $dir` (PowerShell) | ❌ | The variable value is only known at runtime |
| `for %i in (a b) do cd %i` | ⚠️ | You have to model the loop to know how many times it cds and where to |
| `eval "cd /tmp"` | ❌ | The command is hidden inside a string |
| `cmd /c "cd /tmp"` | ✅ | Script parsing at the child-process layer can cover this |

**How this project handles it**:
- Statically determinable → report it, and mark `is_builtin: true`, `kind: "builtin"`,
 `resolved: false` (builtin commands have **no** notion of an "executable file path");
- Not determinable → **do not guess**. Better to under-report than to emit phantom commands.

> A real defect fixed along the way:
> the parentheses in `for %i in (a b) do echo loop` hold the **iteration list**,
> not a command. The old implementation treated the parentheses content as a command, so it reported `a b` and lost the real `echo loop`.
> Now it parses the command body after `do`.

#### 4.0.5 Summary

| Question | Answer |
|---|---|
| Why can't `cd` be hooked? | It **is not a process**. The working directory is a per-process property, so `cd` must be implemented by the shell within its own process |
| What about `echo`/`dir`? | Also not processes. Although `/bin/echo` exists, the shell uses its builtin implementation |
| So how do you obtain `cd`? | **Read the script source** (static analysis), not by hooking |
| Where is the ceiling of static analysis? | Variable expansion, dynamic construction, loop expansion -- these are inherently "only knowable by execution" pieces of information |
| What does this project do? | Report what is determinable and mark it as a builtin; **do not guess** what is not |

### 4.1 Script Types That Need Support

| Type | Extension | Shell builtin semantics | Parsing difficulty |
|---|---|---|---|
| POSIX sh | `.sh` | `cd` `echo` `export` `read` `set` `source` `.` | Medium (the grammar is well specified) |
| Bash | `.sh` | Same as above + arrays/functions | Medium |
| Batch | `.bat` `.cmd` | `cd` `copy` `del` `set` `echo` `if` `for` `call` `goto` | **High** (`%var%` expansion, `&` splitting, `for` loops) |
| PowerShell | `.ps1` | Cmdlets are half cmdlet, half exe | **High** (`$` expansion, pipelines, aliases) |
| Python | `.py` | `os.system` / `subprocess` → both are real processes | Low (only need to locate the call sites) |
| Node.js | `.js` | `child_process.spawn` → real process | Low |

**Note**: in Python/Node/Ruby/Perl scripts, **all external commands are ultimately real processes** (`subprocess` → `posix_spawn`/`CreateProcess`), so the L1 problem **does not exist**. Only **sh / batch / PowerShell** have genuine shell builtin semantics.

### 4.2 cmd.exe Parsing Rules (Based on First-Party Sources)

See [How cmd.exe parses a command](https://github.com/johnstevenson/winbox-args/wiki/How-cmd.exe-parses-a-command):

**Metacharacters**: `" ^ & | < > () %!`

**Order of transformations**:
1. `%var%` expansion (over the whole command)
2. Splitting into multiple commands on `&` `|` `(` `)`
3. `!var!` expansion (only when `DelayedExpansion` is enabled)
4. Identifying the command name → builtin or `CreateProcess`

**Execution after splitting**:
```c
// cmd internal command: SET -- creates no process
// cmd external command: 
CreateProcess(C:\\projects\\prog.exe, "prog paramA \"C:\\ProgramData\"",...);
```

### 4.3 Key Points for PowerShell

- `Start-Process` uses `UseShellExecute` by default → `ShellExecuteEx` → **DCOM Launcher**
- `Start-Process -Verb RunAs` → UAC elevation → **AppInfo service**
- The `&` call operator / `cmd /c` → `CreateProcess`
- `Invoke-Expression` → evaluated inside the PowerShell process; **builtin cmdlets produce no process**

**Verified empirically** (local machine, `Start-Process cmd.exe` inside a PowerShell script):
```
ProcessId: 16284
ParentProcessId: 18248
Name: cmd.exe
CommandLine: C:\Windows\System32\cmd.exe /c call "..."
```
→ `Start-Process` does indeed produce a `cmd.exe` child process, and the parent-child relationship can be reconstructed from `ParentProcessId`.

---

## 5. Differentiation on the Build-System Side

The user asked to "survey the different underlying command-line invocation logic of different build approaches".

### 5.1 Comparison of Compilation Database Generation Tools

| Tool | Principle | Platform | Coverage |
|---|---|---|---|
| **CMake** | Native `CMAKE_EXPORT_COMPILE_COMMANDS=ON` | All | Records only C/C++ compilation, not other commands |
| **Ninja** | Native `ninja -t compdb` | All | Same as above |
| **Clang** | Per-file generation with `-MJ` | All | Same as above |
| **Compilation database generator** | LD_PRELOAD interception of execve | Linux/macOS | Commands for **all** execs |
| **intercept-build** (scan-build) | Compiler wrapper or LD_PRELOAD | Unix | Mostly compilers |
| **cdcc** | Wrapper + SQLite | Linux | Compilers |
| **compiledb-generator** | Parses the output of `make --dry-run` | All | Make only, and it is **parsing, not observation** |
| **Sourcetrail VS Extension** | Parses.sln/.vcxproj | Windows | MSBuild projects |

**Core insight**: the "native support" of CMake/Ninja/Clang is **declarative** (the build system states what it intends to run), whereas the LD_PRELOAD family of tools is **observational** (what actually ran).

**The fatal weakness of the declarative approach**: CMake's `compile_commands.json` does **not** contain `custom_command`, `add_test`, install steps, or any non-compilation invocation. **For the requirement of "capturing all commands", a declarative approach simply cannot satisfy it.**

→ **The observational route is mandatory.**

### 5.2 Actual Command Invocation Patterns of the Various Build Systems

| Build system | Invocation method | Observation point |
|---|---|---|
| **make** | `fork` + `execve("/bin/sh", ["sh", "-c", "recipe"])` | `execve` ✓ (note the sh in between) |
| **ninja** | `posix_spawn` or `fork+execve` | `posix_spawn` ✓ (**must be covered**) |
| **cmake --build** | Invokes make/ninja | Indirect |
| **MSBuild** |.NET `Process.Start` → `CreateProcessW` | `CreateProcessW` ✓ |
| **gradle** | JVM `Runtime.exec` → `fork+execve` | ✓ |
| **npm/yarn** | node `child_process.spawn` → `posix_spawn` | ✓ |
| **cargo** | `posix_spawn` | ✓ |
| **bazel** | Its own spawn implementation, **may bypass libc** | ⚠️ Requires additional verification |
| **autotools** | `sh` script → `execve` | ✓ (one extra sh layer) |

**Note the peculiarity of make**: what make actually executes is `sh -c "recipe"`, so the first layer LD_PRELOAD sees is `sh`, and the real command is execve'd inside the sh. **That sh layer is normal**, but if the script is in the `sh -c` form, L1's shell builtins (`cd`/`echo`) happen inside the sh → only the script layer can fill that in.

### 5.3 An Important Behavioral Difference

Compilation database generators only record **compiler invocations** and emit `compile_commands.json`. This project needs to record **all** commands, so:

> **Architecturally, borrow its interception mechanism, but not its filtering logic.**

---

## 6. Recommended Architecture

### 6.1 Overall Structure

```
┌─────────────────────────────────────────────────────────┐
│ CommandCapture CLI (ccap run <script> [args]) │
│ ┌───────────────────────────────────────────────────┐ │
│ │ 1. Script analysis layer (Script Analyzer) │ │
│ │ parse sh/bat/ps1 → extract the builtin commands (L1) │ │
│ │ output: a static command list + variable reference warnings │ │
│ └───────────────────────────────────────────────────┘ │
│ ┌───────────────────────────────────────────────────┐ │
│ │ 2. Runtime capture layer (Runtime Capture) │ │
│ │ Linux: LD_PRELOAD shim → execve/posix_spawn │ │
│ │ Win: debugger API (DEBUG_PROCESS) collector │ │
│ └───────────────────────────────────────────────────┘ │
│ ┌───────────────────────────────────────────────────┐ │
│ │ 3. Normalization + merging layer (Normalizer) │ │
│ │ unified event model → merge L1/L2 → deduplicate → process tree │ │
│ └───────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

### 6.2 Unified Event Model

```json
{
 "seq": 42,
 "timestamp": "2026-10-27T09:52:26.123+08:00",
 "platform": "windows",
 "pid": 16284,
 "ppid": 18248,
 "executable": "C:\\Windows\\System32\\cmd.exe",
 "argv": ["cmd.exe", "/c", "echo hello"],
 "cwd": "D:\\work\\build",
 "source": "runtime",
 "script": null
}
```

L1 events instead carry `"source": "static"` + `"script": "build.bat"` + `"line": 17`.

### 6.3 Windows Implementation Recommendations

**Tier 1 -- Debugger API collector (recommended primary path)**

C# implementation, P/Invoking the Win32 debugger API directly. See `prototypes/windows-debugproc/DbgProbe5.cs`.

Core flow:

```csharp
// 1) start the target with DEBUG_PROCESS
const uint DEBUG_PROCESS = 0x00000001;
var si = new STARTUPINFO { cb = Marshal.SizeOf<STARTUPINFO>() };
CreateProcessW(null, cmdLine, IntPtr.Zero, IntPtr.Zero, false,
 DEBUG_PROCESS, IntPtr.Zero, null, ref si, out var pi);

// 2) event loop: only care about CREATE_PROCESS(3) / EXIT_PROCESS(5), discard the rest
while (true) {
 if (!WaitForDebugEvent(out var ev, 250)) { if (++quiet >= 6) break; continue; }
 quiet = 0;

 if (ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
 var hProc = ReadIntPtr(ev.u, 8); // handle of the debuggee
 var namePtr = ReadIntPtr(ev.u, 40); // lpImageName
 var image = ReadRemoteString(hProc, namePtr);
 var cmdline = ReadCommandLineFromPeb(hProc); // read the child process PEB
 Record(new CapturedCommand { Executable = image, Argv = cmdline,... });
 }
 ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
}
```

Implementation notes (details in [prototypes/windows-debugproc/README](../../prototypes/windows-debugproc/README.en.md)):

1. `STARTUPINFO` must be passed as `ref` with `cb` filled in correctly; passing `IntPtr.Zero` → `ok=False err=0` (a silent failure, the hardest kind to diagnose)
2. The status for `ContinueDebugEvent` must be `DBG_CONTINUE (0x00010002)`; using `0` leaves the debuggee stuck at the first exception
3. **Filtering by event type is mandatory**: `LOAD_DLL(6)` is extremely noisy (20+ per startup)
4. Reading remote memory requires the **debuggee's hProcess**, not a pseudo-handle
5. The event loop needs a quiet-tick shutdown; do not exit just because you saw an `EXIT`
6. The debugger must exit gracefully, otherwise it will kill the debuggee along with it

**Tier 2 -- Detours gap-filling (optional, not done by default)**

Only add it when information finer-grained than the debugger API provides is needed (e.g. the full parameters of `CreateProcessInternal`, job object inheritance relationships). It requires both 32-bit and 64-bit DLLs + injection + `DetourFindFunction` with the API Set name. **Not done by default**, because the debugger API already covers the requirement with zero privilege demands.

**Tier 3 -- Script layer (mandatory, covers L1)**

- Parse `.bat` / `.cmd`: implement `%var%` expansion (single pass), `&`/`|`/`(`/`)` splitting, the builtin command table
- Parse `.ps1`: recognize cmdlets (builtin) vs external exes; mark `Invoke-Expression` as "inline execution, cannot be statically expanded"
- Parse `.sh`: recognize POSIX builtins

### 6.4 Linux Implementation Recommendations

**Tier 1 -- LD_PRELOAD shim (recommended primary path)**

Refer directly to the implementation of comparable tools (C + Rust mixed, or pure C):

```c
// the symbols that must be covered (complete list) 
execve, execv, execvp, execvpe, execl, execlp, execle,
fexecve, posix_spawn, posix_spawnp,
__execve, __posix_spawn // glibc internals
```

Key implementation points (learned from comparable tools):
1. **Variadic functions need a C shim** -- `execl`/`execlp` are variadic and Rust cannot handle them directly
2. **Environment variable self-healing** -- rewrite the envp passed to the child process inside the hook, restoring `LD_PRELOAD`
3. **Report over a TCP socket**, not a file (avoiding file-lock problems during fork)
4. **Fail silently** -- no error inside the hook may affect the target program

**Tier 2 -- ptrace + seccomp-BPF alternative**

For scenarios where the target is statically linked or preloading is blocked.

**Tier 3 -- Script layer** (same as Windows)

### 6.5 Coverage Guarantees for "Direct vs Indirect"

| Command source | Linux | Windows |
|---|---|---|
| External commands called directly by the script | ✓ LD_PRELOAD | ✓ Debugger API |
| Scripts called by the script (recursive) | ✓ | ✓ |
| Commands launched by build tools (ninja→cl) | ✓ | ✓ |
| Elevated / new-session processes | ✓ (except setuid) | ⚠️ Requires the target to elevate itself (UAC spawns a separate process) |
| Script shell builtins | ✗ → script layer | ✗ → script layer |
| Commands launched by statically linked programs | ✗ → ptrace alternative | ✓ Still visible (created at kernel level, the debugger receives the event) |
| Escaping the process tree (daemonize) | ✓ Still visible | ✓ Still visible |

> **The "kernel level" difference between the two platforms**:
> - Linux's `LD_PRELOAD` is a **user-mode** mechanism → child processes spawned by statically linked programs cannot be captured
> - Windows' debugger API is a **kernel-level** notification → child processes spawned by statically linked programs can still be captured
>
> **Neither platform captures script shell builtins** → the script static-analysis layer is mandatory on both.

---

## 7. Risks and Known Gaps

### 7.1 Must-Know Gaps (Should Be Written into the Product Documentation)

| Gap | Platform | Description |
|---|---|---|
| Script shell builtins | Both | Only static parsing can handle them; dynamically constructed ones (`eval "$CMD"`) have no solution |
| Commands assembled dynamically from variables | Both | `cmd.exe /c "%MY_CMD%"` cannot be statically expanded; at runtime you must rely on intercepting cmd.exe's reads |
| Static linking | Linux | Requires the ptrace alternative |
| setuid programs | Linux | The kernel discards LD_PRELOAD |
| SELinux/AppArmor | Linux | Requires policy cooperation |
| Extremely short-lived processes | Both | LD_PRELOAD / debugger API miss none of them (kernel level); WMI does miss them |
| The debuggee knows it is being debugged | Windows | `PEB.BeingDebugged = 1`; a few anti-debug tools change behavior. This project mitigates with `--clear-being-debugged`; the alternative wrapper-style capture is described in §3.1.5 |
| Debugger exit kills the process | Windows | System behavior; graceful exit + signal forwarding are mandatory |
| LOAD_DLL event noise | Windows | Measured 20+ per startup; not filtering by event type causes throughput to collapse |
| Exception handling policy | Windows | `DBG_CONTINUE` swallows first-chance exceptions; `DBG_EXCEPTION_NOT_HANDLED` must be distinguished |
| `cmd.exe` `for` loop expansion | Windows | `for %i in (a b c) do cmd` actually executes N times, so static parsing must model the loop variable |
| PowerShell pipelines | Windows | `cmd \| cmd` is statically parseable, but quoting must be handled carefully |

### 7.2 Event-Handling Cautions for the Windows Debugger API (Important)

The debugger API does not "drop events" (the kernel guarantees delivery), but **poor handling will cause misses or hangs**:

| Symptom | Cause | Countermeasure |
|---|---|---|
| Only one CREATE event is received and then it hangs | The status passed to `ContinueDebugEvent` was `0` instead of `DBG_CONTINUE` | Use `DBG_CONTINUE (0x00010002)` |
| `CreateProcess` returns `ok=False err=0` | `STARTUPINFO` was passed as `IntPtr.Zero` | Pass `ref STARTUPINFO` and fill in `cb` |
| The image name cannot be read | Remote memory was read with a pseudo-handle | Use the debuggee's `hProcess` |
| Sibling processes are missed | The event loop exits as soon as it sees an `EXIT` | Shut down using quiet-tick counting |
| Throughput collapses | `LOAD_DLL` was not filtered | Filter by event type |

---

## 8. Conclusions and Recommendations

### 8.1 Core Judgments

1. **Linux side: adopt the LD_PRELOAD mechanism**, but keep all commands rather than only compiler invocations. Technical risk is low and no administrator privileges are required.

2. **Windows side: use the debugger API (`DEBUG_PROCESS`); this is what comparable tools do on Windows.**
 - ✅ **No administrator privileges required** (verified in a local non-administrator session, capturing three layers: root/child/grandchild)
 - ✅ **The entire process tree is visible** (`DEBUG_PROCESS` follows child processes; `DebugActiveProcess` does not -- don't use the wrong one)
 - ✅ **No injection**, so the EDR minefield is untouched
 - ✅ **Kernel-level notification**, so child processes spawned by statically linked programs are still captured

 **Explicitly excluded**:
 - **ETW** -- requires an administrator to create a kernel session (measured: `Access is denied`), eliminated under the "cannot obtain privileges" constraint
 - **Detours as the primary path** -- needs injection into system services to be complete, has a pre-injection fork race, and gets flagged by EDR. Demoted to an optional enhancement

3. **Both platforms must have a script static-analysis layer**, otherwise the requirement of "all commands" cannot be satisfied semantically (cmd.exe / sh builtins produce no processes). This is this project's **main increment** over the class of tools that "only record compiler invocations".

4. **On the Windows side, the main technical risk shifts from "privileges" to "behavioral compatibility"**:
 - The debuggee knows `PEB.BeingDebugged = 1`, and a few tools change behavior (this project mitigates with `--clear-being-debugged`; the alternative wrapper-style capture is described in §3.1.5)
 - Debugger exit = debuggee gets killed, so graceful exit is mandatory
 - `LOAD_DLL` event noise must be filtered

### 8.2 Recommended Implementation Order

| Phase | Content | Deliverable | Status |
|---|---|---|---|
| **P0 validation** | Minimal LD_PRELOAD shim (Linux); minimal `DEBUG_PROCESS` collector (Windows) | Confirm technical feasibility | ✅ **Windows completed** (`prototypes/windows-debugproc/`) |
| **P1 core** | Linux: full LD_PRELOAD symbol coverage + environment self-healing<br>Windows: debugger API collector + PEB command-line reading + process tree reconstruction | Cross-platform L2 capture | To be done |
| **P2 increment** | Script analyzer: `.bat` (cmd syntax), `.sh` (POSIX sh) | L1 coverage | To be done |
| **P3 enhancement** | PowerShell parsing; ptrace alternative (Linux); Detours plugin (Windows, optional) | Coverage improvement | To be done |
| **P4 delivery** | Unified CLI, JSON/NDJSON output, documented known gaps | Usable tool | To be done |

### 8.3 Completed Validation Experiments

**Windows (non-administrator, verified)**:

```powershell
# 1. confirm that cmd.exe builtin commands create no process

& cmd.exe /c "echo INTERNAL & whoami.exe"

# 2. confirm the parent-child relationship of PowerShell Start-Process
Get-CimInstance Win32_Process | Where-Object Name -match 'cmd' |
 Select ProcessId, ParentProcessId, Name, CommandLine

# 3. debugger API whole-tree capture (the core validation) 
cd prototypes/windows-debugproc
dotnet build DbgProbe.csproj -c Release
.\bin\Release\net9.0-windows\DbgProbe.exe
# => [total] create-events=3 (root cmd / child cmd / grandchild whoami)
# => [marker file exists] = True
```

**Windows (requires administrator; the ETW route has been rejected)**:

```powershell
logman create trace CCProbe -p "Microsoft-Windows-Kernel-Process" 0x10 4 -o C:\ccprobe.etl -ets
# measured result: Access is denied (non-administrator session) 
```

**Linux (pending validation)**:

```bash
# whether the LD_PRELOAD shim can cover the execve family + the posix_spawn family
```

### 8.4 Two Points Still Needing Your Decision

1. **Is the Windows side effect of "the debuggee knows it is being debugged" acceptable?** If the target scripts contain anti-debugging detection (rare but existent), you have to fall back to an `--instrument`-style route that "does not attach a debugger", at the cost of not being able to obtain indirect child processes.
2. **The scope of the script parsing layer**: `.bat` / `.sh` are mandatory (shell builtin semantics matter a lot), while `.ps1` is the hardest (cmdlets and exes mixed, plus `Invoke-Expression` cannot be statically expanded). Do the first two first, or all three together?

## 9. References

**Linux**
- Architectural reference for the LD_PRELOAD approach: <https://man7.org/linux/man-pages/man8/ld.so.8.html>
 (the semantics of `LD_PRELOAD` / `LD_AUDIT`, and how the dynamic linker loads injected libraries in order)
- ptrace(2): https://man7.org/linux/man-pages/man2/ptrace.2.html
- seccomp(2) / seccomp-BPF practice: https://grith.ai/blog/supervising-ai-agents-with-seccomp-bpf-and-ptrace

**Windows**
- Microsoft Detours: https://github.com/microsoft/Detours
- CreateProcessW: https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw
- **Official documentation of a commercial static analysis tool (Windows debugger mode / `--instrument` alternative path)**: the Windows platform page on the product documentation site
- Windows debugger API (DEBUG_PROCESS vs DEBUG_ONLY_THIS_PROCESS): https://msdn.microsoft.com/zh-tw/ms809754
- ETW kernel provider (GUID / keyword table, excluded): https://justruss.tech/index.php/2025/08/05/seond-blog
- Process creation API hook matrix: https://blog.csdn.net/qq_59075481/article/details/128814911
- cmd.exe parsing rules: https://github.com/johnstevenson/winbox-args/wiki/How-cmd.exe-parses-a-command
- Windows command-line argument rules: https://comcomponent.com/zh-TW/blog/windows-command-line-argument-rules
- Start-Process reference: https://learn.microsoft.com/powershell/module/microsoft.powershell.management/start-process

**Build systems / compilation database**
- Overview: https://sarcasm.github.io/notes/dev/compilation-database
- JSON Compilation Database specification: https://clang.llvm.org/docs/JSONCompilationDatabase.html
- Nushell's Windows command execution fallback strategy (Stage 1/2/3): https://deepwiki.com/abab2025/nushell_nushell_main_afdb68d/10.2-windows-process-handling

---

*This report is based on research and local measurements conducted on 2026-10-27. The Windows-side debugger API approach has been validated end to end in a non-administrator session (3-layer process tree + execution confirmation); the prototype code is in `prototypes/windows-debugproc/`. The ETW route has been excluded because it requires administrator privileges. On the Linux side, LD_PRELOAD is still awaiting local validation.*
