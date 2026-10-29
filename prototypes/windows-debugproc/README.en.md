# Windows Debugger API Command Capture Prototype

<!-- i18n:begin -->
中文 | [**English**](README.md)
<!-- i18n:end -->

This directory contains the verification code written during the research phase, used to confirm whether the **Windows debugger API (`DEBUG_PROCESS`)** can capture a complete process tree under **non-administrator privileges**.

## Conclusion (verified on this machine)

| Mechanism | Requires admin | Sees child processes | Notes |
|---|---|---|---|
| `CreateProcessW` + `DEBUG_PROCESS` | **No** | **Yes** (the whole tree) | This is exactly the path commercial static analysis tools use on Windows |
| `DebugActiveProcess` | **No** | **No** (only follows the single process it attached to) | Equivalent to `DEBUG_ONLY_THIS_PROCESS` |
| `CreateProcessW` + `DEBUG_ONLY_THIS_PROCESS` | No | No | Only follows direct children |
| Detours injection hook | Depends on the target | Yes | Requires injection, has races, gets flagged by EDR |
| ETW Kernel-Process | **Yes** (creating a kernel session) | Yes | Kernel level, but this project cannot obtain the permission |
| Compiler wrapper (intercepts calls) | No | Yes (compilation only) | See the comparison in `docs/zh/RESEARCH-COMMAND-CAPTURE-2026-10-27.md` |

**Key point**: after launching the target process with `DEBUG_PROCESS`, you receive `CREATE_PROCESS_DEBUG_EVENT` for the entire process tree **without any privilege elevation**.

## Files

- `DbgProbe5.cs` -- `DEBUG_PROCESS` whole-tree capture (**main prototype**, `StartupObject`)
- `DbgProbe4.cs` -- `DebugActiveProcess` control group (captures only one process)
- `DbgProbe.csproj` -- build configuration (includes both files above, entry point is `DbgProbe5`)

## Running

```powershell
cd prototypes\windows-debugproc
dotnet build DbgProbe.csproj -c Release
.\bin\Release\net9.0-windows\DbgProbe.exe
```

To run the control group, change the entry point to `DbgProbe4` (or change `<StartupObject>` in the csproj).

## Measured output of DbgProbe5 (this machine, Windows 10 19045, non-administrator)

```
[create] ok=True err=0 root=19852
 [CREATE] pid=19852 <- cmd.exe (根)
 [CREATE] pid=5760 <- cmd.exe (孙进程)
 [CREATE] pid=13296 <- whoami.exe (曾孙进程)
 [EXIT] pid=13296
 [EXIT] pid=19852
[total] create-events=3 rootExitCode=0x00000000
[marker file exists] = True
```

Three `CREATE_PROCESS` events covered three levels -- root → child → grandchild -- and the marker file proves the debugged process **actually ran** (it was not suspended and starved).

## Implementation notes

1. **`CreateProcessW` must be passed `ref STARTUPINFO`** (`cb = Marshal.SizeOf(typeof(STARTUPINFO))`); passing `IntPtr.Zero` fails outright and returns `ok=False err=0`.
2. **The status for `ContinueDebugEvent` must use `DBG_CONTINUE` (0x00010002)** -- do not use `0`: with `0` the debugged process does not continue after the first exception (event 1), which shows up as "only 1 CREATE event received" followed by a hang.
3. **Event types**:
 - `1` = EXCEPTION_DEBUG_EVENT
 - `2` = CREATE_THREAD_DEBUG_EVENT
 - `3` = CREATE_PROCESS_DEBUG_EVENT ← **the one we want**
 - `4` = EXIT_THREAD_DEBUG_EVENT
 - `5` = EXIT_PROCESS_DEBUG_EVENT
 - `6` = LOAD_DLL_DEBUG_EVENT (very noisy, must be filtered)
4. The **process image name** is at offset `40` (`lpImageName`) in `CREATE_PROCESS_DEBUG_INFO`, and it must be read with `ReadProcessMemory` using **the debugged process's own handle** (the `hProcess` at offset `8`); a pseudo-handle cannot read it.
 > In the prototype this item shows up as `<none>` because `lpImageName` may not yet be filled in at the moment of `CREATE_PROCESS_DEBUG_EVENT`; **the production implementation should instead read `ProcessParameters->CommandLine` from the child process PEB**, which is the complete command line.
5. **The event loop needs a "silent exit" strategy**: it must not exit as soon as an EXIT arrives; use quiet-tick counting, otherwise still-running sibling processes will be missed.
6. **`LOAD_DLL` (6) is extremely noisy** -- measured at 20+ per launch; the production implementation must filter by event type, otherwise throughput collapses.

## Known limitations

- **The debugged process knows it is being debugged** (PEB.BeingDebugged = 1). A few tools change their behavior because of this (some anti-cheat systems, some JIT compilers). This project's mitigation is `ccap run --clear-being-debugged` (disabled by default). Commercial static analysis tools offer **a different capture path** for this scenario (a compiler wrapper interception rather than a debugger); see the research report for details.
- **Debugger exit = the debugged process is killed**. Proper exception handling is needed to avoid the target process being terminated along with it.
- **Performance**: every event has a kernel round-trip cost. `LOAD_DLL` events are very dense (20+ for a single launch in the measurement above), so they must be filtered or throughput collapses.
- **Exception handling strategy**: the default use of `DBG_CONTINUE` swallows first-chance exceptions, which may behave unexpectedly for tools that depend on exception semantics. A safer approach is to distinguish `DBG_EXCEPTION_NOT_HANDLED` (unhandled exceptions, let the program handle them itself) from `DBG_CONTINUE`.
- **WOW64**: for an x86 target under an x64 debugger, the register context requires `Wow64GetThreadContext`. Command-line capture is unaffected (the command line is in the PEB and is read directly).
