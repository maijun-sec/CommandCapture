# Windows 调试器 API 命令捕获原型

<!-- i18n:begin -->
[**English**](README.en.md) | 中文
<!-- i18n:end -->




本目录是调研期写的验证代码，用于确认 **Windows 调试器 API（DEBUG_PROCESS）** 能否在**非管理员权限**下捕获完整进程树。

## 结论（已在本机验证）

| 机制 | 需要管理员 | 能看到子进程 | 备注 |
|---|---|---|---|
| `CreateProcessW` + `DEBUG_PROCESS` | **否** | **是**（整棵树） | 商业静态分析工具在 Windows 上用的就是这条路 |
| `DebugActiveProcess` | **否** | **否**（只跟被 attach 的那一个） | 等价于 `DEBUG_ONLY_THIS_PROCESS` |
| `CreateProcessW` + `DEBUG_ONLY_THIS_PROCESS` | 否 | 否 | 只跟直接子进程 |
| Detours 注入 hook | 视目标而定 | 是 | 需注入，有竞态，会被 EDR 标记 |
| ETW Kernel-Process | **是**（创建内核 session） | 是 | 内核层，但本项目拿不到权限 |
| 编译器 wrapper（拦截调用） | 否 | 是（只对编译） | 见 `docs/zh/RESEARCH-COMMAND-CAPTURE-2026-10-27.md` 的对比 |

**关键**：用 `DEBUG_PROCESS` 启动目标进程后，**不需要任何权限提升**，就能收到整棵进程树的 `CREATE_PROCESS_DEBUG_EVENT`。

## 文件

- `DbgProbe5.cs` — `DEBUG_PROCESS` 整树捕获（**主原型**，`StartupObject`）
- `DbgProbe4.cs` — `DebugActiveProcess` 对照组（只抓到一个进程）
- `DbgProbe.csproj` — 构建配置（同时包含上面两个文件，入口点为 `DbgProbe5`）

## 运行

```powershell
cd prototypes\windows-debugproc
dotnet build DbgProbe.csproj -c Release
.\bin\Release\net9.0-windows\DbgProbe.exe
```

想跑对照组，把入口点改成 `DbgProbe4`（或在 csproj 里改 `<StartupObject>`）。

## DbgProbe5 实测输出（本机，Windows 10 19045，非管理员）

```
[create] ok=True err=0 root=19852
  [CREATE] pid=19852          <- cmd.exe (根)
  [CREATE] pid=5760           <- cmd.exe (孙进程)
  [CREATE] pid=13296          <- whoami.exe (曾孙进程)
  [EXIT]   pid=13296
  [EXIT]   pid=19852
[total] create-events=3  rootExitCode=0x00000000
[marker file exists] = True
```

3 个 `CREATE_PROCESS` 事件，覆盖了根 → 子 → 孙三层，且 marker 文件证明被调试进程**真的执行了**（不是被挂起后饿死）。


## 实现要点

1. **`CreateProcessW` 必须传 `ref STARTUPINFO`**（`cb = Marshal.SizeOf(typeof(STARTUPINFO))`），传 `IntPtr.Zero` 会直接失败返回 `ok=False err=0`。
2. **`ContinueDebugEvent` 的 status 用 `DBG_CONTINUE`（0x00010002）**，不要用 `0`——用 `0` 时被调试进程在第一次异常（event 1）后不会继续跑，表现为"只收到 1 个 CREATE 事件"然后卡死。
3. **事件类型**：
   - `1` = EXCEPTION_DEBUG_EVENT
   - `2` = CREATE_THREAD_DEBUG_EVENT
   - `3` = CREATE_PROCESS_DEBUG_EVENT ← **我们要的**
   - `4` = EXIT_THREAD_DEBUG_EVENT
   - `5` = EXIT_PROCESS_DEBUG_EVENT
   - `6` = LOAD_DLL_DEBUG_EVENT（噪音很多，务必过滤）
4. **进程映像名**在 `CREATE_PROCESS_DEBUG_INFO` 的偏移 `40`（`lpImageName`），要用**被调试进程自己的句柄**（偏移 `8` 的 `hProcess`）去 `ReadProcessMemory`，用伪句柄读不到。
   > 原型里这一项显示为 `<none>`，是因为 `lpImageName` 在 `CREATE_PROCESS_DEBUG_EVENT` 时刻可能还没填好；**生产实现应改从子进程 PEB 读 `ProcessParameters->CommandLine`**，那才是完整命令行。
5. **事件循环要有"静默退出"策略**：不能一收到 EXIT 就退出，要用 quiet-tick 计数，否则会漏掉还在跑的兄弟进程。
6. **`LOAD_DLL`(6) 噪音极大** —— 实测一次启动 20+ 个，生产实现必须按事件类型过滤，否则吞吐会塌。

## 已知限制

- **被调试进程知道自己被调试**（PEB.BeingDebugged = 1）。少数工具会因此改变行为（有些反作弊、部分 JIT）。本项目的缓解手段是 `ccap run --clear-being-debugged`（默认关闭）。商业静态分析工具对这类场景提供的是**另一条捕获路径**（编译器 wrapper 拦截，而不是调试器），细节见调研报告。
- **调试器退出 = 被调试进程被杀**。需要做好异常处理，避免目标进程被连带终止。
- **性能**：每个事件有内核往返开销。`LOAD_DLL` 事件非常密集（上面实测一次启动就 20+ 个），必须过滤，否则吞吐会塌。
- **异常处理策略**：默认用 `DBG_CONTINUE` 会吞掉 first-chance 异常，某些依赖异常语义的工具可能异常。更保险的是区分 `DBG_EXCEPTION_NOT_HANDLED`（未处理异常，让程序自己处理）和 `DBG_CONTINUE`。
- **WOW64**：x86 目标在 x64 调试器下，寄存器上下文需要 `Wow64GetThreadContext`。命令行捕获不受影响（命令行在 PEB 里，直接读）。
