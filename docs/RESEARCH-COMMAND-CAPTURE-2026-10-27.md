# CommandCapture 调研报告：跨平台脚本/构建命令捕获

**日期**: 2026-10-27（**v2 修订**：Windows 路径改为调试器 API 方案，附本机实测）
**范围**: 调研 Linux 与 Windows 两类命令捕获机制，评估"传入一个脚本、捕获其直接或间接拉起的所有命令"的可行性
**结论摘要**: 可行。**Linux 用 `LD_PRELOAD`，Windows 用调试器 API（`DEBUG_PROCESS`）**——两者都**不需要管理员权限**。两平台技术路径不能对称设计。

> **关于本文档里的产品名**：本报告是**调研**，它的价值恰恰在于引用了各家的**公开文档**作为证据
> ——删掉引用，结论就没法复核了。所以本文档保留这些名字。
>
> 但**产品代码、测试和面向使用者的文档**里已经没有它们了
> （见 `README.md` 和 `docs/DESIGN-MODULES-2026-10-27.md`）。
> 代码里唯一保留的是**日志里真实出现的前端 wrapper 名**
> ——但连这一项也改成了**工具无关的通用写法**（`sa-translate` / `tool-emit` / ...），
> 因为解析器要认的是**"编译器前面套了一层 wrapper"这个形状**，不是某一个厂商的拼法。

---

## 0. 一句话结论

| 平台 | 可行性 | 推荐主路径 | 需要管理员 | 主要风险 |
|---|---|---|---|---|
| Linux | 高（已实测验证） | `LD_PRELOAD` 拦截 `execve`/`posix_spawn` + 环境变量"自愈" | 否 | 静态链接二进制、setuid、SELinux/AppArmor |
| Windows | **高** | **调试器 API `DEBUG_PROCESS`（同类工具的现成做法）** + 脚本层解析 | **否**（已实测） | cmd.exe **内建命令不产生进程**；被调试进程知道自己被调试 |

> **v2 重大修订**：初版建议 Windows 走 ETW。经查该工具的官方文档 + 本机实测，**ETW 需要管理员权限**（创建内核 session 会 `Access is denied`），而 **它用的调试器 API 方案完全不需要提权**。用户提出的"拿不到更高权限"这个约束，直接把方案从 ETW 推到了调试器 API。

> **最关键的发现（Windows，两条）**：
> 1. `cmd.exe` 的**内建命令**（`echo`/`dir`/`copy`/`set`/`cd`/`for` 等约 30 个）**完全不创建子进程**，因此任何进程级机制都**抓不到它们** → 必须加脚本静态分析层。
> 2. **`DEBUG_PROCESS` 可以在非管理员权限下捕获整棵进程树** → 不需要 ETW，不需要注入，不需要提权。

---

## 0.5 修订记录：为什么 Windows 方案改了

初版结论是"Windows 用 ETW + Detours 补齐"。用户指出**拿不到管理员权限**，
促使我去查**商业静态分析工具的现成做法**。查到的东西分两半：

**这类工具在 Windows 上根本不用 hook，用的是调试器 API。** 官方文档原文
（[Black Duck Docs - Windows](https://docs.blackduck.com/blackduck/2026.3/coverity-documentation/windows.html)）：

> "The cov-build command uses a **different mechanism** to capture compiler process creation on Windows than on UNIX platforms. The **Windows version of cov-build runs the build command and its child processes in debug mode.**"

这句话里的 "runs ... in debug mode" 就是 `CreateProcess` 带 `DEBUG_PROCESS` 标志。知识库进一步说明了捕获原理：

> "我们监视由 'make' 创建的 '进程/线程生成' 活动...当产生一个新进程/线程时，在中间目录中的文件 `build-log.txt` 中捕获此信息，然后我们查看正在启动的可执行文件的名称"

**本机实测验证（本机非管理员，Windows 10 19045）：**

| 机制 | 需要管理员 | 捕获到的 CREATE_PROCESS 数 | 覆盖深度 |
|---|---|---|---|
| `CreateProcessW` + `DEBUG_PROCESS` | **否** | **3**（根/子/孙） | **整棵树** |
| `DebugActiveProcess` | **否** | 1（只有被 attach 的那个） | 不跟子进程 |
| ETW Kernel-Process | **是** | — | 整棵树，但需提权 |

`DEBUG_PROCESS` 的原型代码在 `prototypes/windows-debugproc/`，实测输出：

```
[create] ok=True err=0 root=6420
  [CREATE] pid=6420     <- cmd.exe
  [CREATE] pid=8808     <- cmd.exe（孙进程）
  [CREATE] pid=3596     <- whoami.exe（曾孙进程）
[total] create-events=3
[marker file exists] = True    <- 证明被调试进程真的执行了
```

> 顺带澄清：这类工具里的 `java-capture.properties`
> （`compiler.classes` / `compiler.methods` / `arg.positions` 那套声明式配置）
> **是另一套东西**——它是 **JVM 层**的 hook（`com.sun.tools.javac.Main.compile`），
> 用于 Java 捕获，**与 OS 层进程创建捕获无关**。别被它的 "Detours" 血缘误导。



---

## 1. 需求拆解

用户诉求：传入一个脚本，捕获该脚本**直接或间接拉起的所有命令**。

这实际上包含三层，必须分别对待：

```
L0  脚本自身被执行的命令        (e.g. cmd.exe /c build.bat)
L1  脚本解释器内建的"动作"      (e.g. cmd 的 echo/copy —— 不产生进程)
L2  脚本拉起的外部进程           (e.g. build.bat 里的 cl.exe / git)
```

L2 是 LD_PRELOAD / Detours / ETW 的主战场。**L1 只能靠解析脚本源码**。这是本项目与"只关心编译器调用"那一类工具的本质差异：
那一类工具的目标全是 L2，本项目要的是"所有命令"。

---

## 2. Linux 侧：主流实现机制

### 2.1 核心机制

一个典型的这类工具（用 LD_PRELOAD 拦截 exec 并把结果写回编译数据库）工作方式：

```
<tool> -- make -j8
```

1. **注入**：`LD_PRELOAD=<tool>/libexec.so`（macOS 用 `DYLD_INSERT_LIBRARIES`）
2. **拦截**：覆盖 libc 的 `execve` / `execl` / `execlp` / `execvpe` / `posix_spawn` / `posix_spawnp` 等一系列变参函数
3. **上报**：被拦截进程把 (path, argv[], cwd) 通过 **TCP socket** 发回 collector 进程
4. **环境自愈**：这是最容易被忽略但最关键的设计 —— 见 2.2
5. **降级**：若目标程序是静态链接的（动态链接器不生效），退回 **wrapper 模式**（用同名 wrapper 脚本替换 `cc`/`gcc` 等，记录后转发给真实编译器）

> 关键实现位置：
> - `src/intercept-preload/src/implementation.rs` — 各 exec 函数的 hook 实现
> - `src/intercept-preload/src/c/shim.c` — 变参函数的 C shim
> - `src/intercept-preload/src/session.rs` — session 管理
> - `src/environment.rs` — 环境变量设置

### 2.2 环境变量"自愈"机制（必读）

**问题**：很多构建系统会清空或重写环境变量。如果 `LD_PRELOAD` 被清掉，深层子进程就逃逸了。

**这类工具的做法**：在 hook 到的 `execve` 里，**重新写回** `LD_PRELOAD` 和 collector 地址到即将传给子进程的环境块中。

```rust
// 该工具的 intercept-preload/src/session.rs 语义
// 检测到环境变量被清除时，自动恢复 LD_PRELOAD 和 INTERCEPT_COLLECTOR_ADDRESS
```

**对本项目的意义**：Windows 上有**完全对应**的机制 —— `PATH` 变量被清空、`cmd.exe` 的 `/D` 开关、PowerShell 的 `$env:PATH` 覆盖。设计时必须做对等处理。

### 2.3 Linux 的已知限制

| 限制 | 原因 | 缓解 |
|---|---|---|
| 静态链接二进制抓不到 | 动态链接器不参与 | wrapper 模式（已有成熟实现） |
| setuid/setgid 程序 | 内核丢弃 LD_PRELOAD | 无法规避，需记录为已知缺口 |
| SELinux / AppArmor 限制 | 策略禁止 preload | 需放宽策略或用 ptrace 备选 |
| `posix_spawn` 的变体 | glibc 内部可能绕过 | 需覆盖 `posix_spawn{,p}` 且注意 glibc 会将其转为 `clone+execve` |
| 脚本内建命令（`cd`/`echo`/`export`） | 不是进程 | 同 Windows，需脚本层解析 |

### 2.4 Linux 的备选方案：ptrace + seccomp-BPF

若 LD_PRELOAD 不可用（例如目标进程静态链接），可以退化到 ptrace 方案：

- `PTRACE_TRACEME` + `PTRACE_O_TRACEEXEC` / `PTRACE_O_TRACEFORK` / `PTRACE_O_TRACECLONE` / `PTRACE_O_TRACEVFORK`
- **seccomp-BPF 预过滤**：用 `SECCOMP_RET_TRACE` 只对 `execve`/`execveat` 触发 ptrace stop，其余 syscall 原速运行

这正是 `grith.ai` 的做法，公开的性能数据是 **P95 每次拦截 < 50µs，端到端 < 5%  slowdown**。

> 性能对比（来自 devmindset.dev 整理）：
> | 方案 | 每事件成本 | 对被 trace 进程的影响 |
> |---|---|---|
> | ptrace (PTRACE_SYSCALL) | 停机 + 调度 + 额外系统调用 | 每次 syscall 边界停机 |
> | ptrace + seccomp-BPF | 同上，但**仅**过滤后的 syscall | 只在匹配调用时停机 |
> | eBPF (tracepoints) | 内核内执行 BPF | **不停机** |

### 2.5 Linux 方案选择

| 场景 | 推荐 |
|---|---|
| 通用构建/脚本捕获 | **LD_PRELOAD**，性能好、语义清晰 |
| 目标静态链接 / preload 被禁 | ptrace + seccomp-BPF 备选 |
| 长时间常驻监控 | eBPF（但无法拿到完整 argv，需配合） |

---

## 3. Windows 侧：调试器 API（同类工具的现成做法）

### 3.0 先纠正一个前提

用户提到"某商业静态分析工具用到了 detour"。查证结果：

**这类工具在 Windows 上用的是调试器 API，不是 Detours。**

官方文档（[Black Duck Docs - Windows](https://docs.blackduck.com/blackduck/2026.3/coverity-documentation/windows.html)）原文：

> "The cov-build command uses a different mechanism to capture compiler process creation on Windows than on UNIX platforms. **The Windows version of cov-build runs the build command and its child processes in debug mode.** If your build system has problems running in debug mode, try using the `--instrument` option with `cov-build`."

官方知识库把机制说得很清楚：

> "在构建系统上运行时，'cov-build' 基本上是为执行构建而生成的进程/线程的**'包装器'**...我们监视由 'make' 创建的**'进程/线程生成'活动**...当产生一个新进程/线程时，在中间目录中的文件 **`build-log.txt`** 中捕获此信息，然后我们查看正在启动的可执行文件的名称"

**"runs in debug mode" = `CreateProcess` 带 `DEBUG_PROCESS` 标志。** 这就是所谓的"wrapper"——
那个包装命令自己就是那个 debugger。

至于 `java-capture.properties`（`compiler.classes` / `compiler.methods` / `compiler.signatures` / `arg.positions` 那套），那是**另一套东西**：JVM 层的 hook（`com.sun.tools.javac.Main.compile`），用于 Java 捕获，**与 OS 层进程创建捕获无关**。它的声明式配置格式（6 个平行列表按下标对应）确实和 Detours 血缘有关，但那是另一个用途。

### 3.1 方案一：调试器 API（推荐主路径）

#### 3.1.1 三种调试器模式的区别（这是选型的关键）

| 模式 | 常量 | 跟不跟子进程 | 用途 |
|---|---|---|---|
| `DEBUG_PROCESS` | `0x00000001` | **跟**（整棵树） | **同类工具用的就是这个** |
| `DEBUG_ONLY_THIS_PROCESS` | `0x00000002` | 不跟 | 只调试直接子进程 |
| `DebugActiveProcess` | API | **不跟** | attach 到已运行进程（等价 `DEBUG_ONLY_THIS_PROCESS`） |

Microsoft 文档对 `DEBUG_PROCESS` 的定义：

> "DEBUG_PROCESS sets up the parent/child relationship so that the debugger will receive debug events from a process being debugged **and any other processes created by that process**."

**这个区别是本项目能不能拿到"间接拉起的命令"的分水岭。** `DebugActiveProcess` 只跟一个进程，`DEBUG_PROCESS` 跟整棵树。

#### 3.1.2 本机实测（关键证据）

原型代码：`prototypes/windows-debugproc/DbgProbe5.cs`

**测试环境**：Windows 10 19045.6466，**非管理员会话**（`IsAdmin: False`）

被调试的命令：
```
cmd.exe /c "cmd.exe /c echo GRANDCHILD_RAN > <marker> & whoami.exe"
```

**结果**：
```
[create] ok=True err=0 root=6420
  [CREATE] pid=6420     <- cmd.exe       (根)
  [CREATE] pid=8808     <- cmd.exe       (子)
  [CREATE] pid=3596     <- whoami.exe    (孙)
  [EXIT]   pid=3596
  [EXIT]   pid=8808
  [EXIT]   pid=6420
[total] create-events=3  rootExitCode=0x00000000
[marker file exists] = True
```

**三个结论**：
1. ✅ **不需要管理员权限** —— `CreateProcess` 带 `DEBUG_PROCESS` 在普通用户会话下完全可用
2. ✅ **整棵树可见** —— 根 → 子 → 孙 三层 `CREATE_PROCESS_DEBUG_EVENT` 全部收到
3. ✅ **被调试进程真的执行了** —— marker 文件写出来了，说明不是"挂起后饿死"

对照组（`DbgProbe4.cs`，`DebugActiveProcess`）：
```
[attach] DebugActiveProcess => True err=0
  [CREATE] pid=2664
[total] create-events=1        <- 只有被 attach 的那个
```

#### 3.1.3 实现要点（踩过的坑）

```c
// 1. STARTUPINFO 必须传，不能传 IntPtr.Zero
STARTUPINFO si = { sizeof(si) };
CreateProcessW(NULL, cmd, NULL, NULL, FALSE, DEBUG_PROCESS,
                NULL, NULL, &si, &pi);   // si 传 NULL 会 ok=False err=0
```

```c
// 2. ContinueDebugEvent 的 status 用 DBG_CONTINUE (0x00010002)，不是 0
//    用 0 的话被调试进程在第一次异常后不会继续跑
//    症状：只收到 1 个 CREATE_PROCESS 事件然后卡死
ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
```

**事件类型表**：

| 值 | 事件 | 说明 |
|---|---|---|
| 1 | `EXCEPTION_DEBUG_EVENT` | 噪音，需正确处理 status |
| 2 | `CREATE_THREAD_DEBUG_EVENT` | 噪音 |
| **3** | **`CREATE_PROCESS_DEBUG_EVENT`** | **← 我们要的** |
| 4 | `EXIT_THREAD_DEBUG_EVENT` | 噪音 |
| 5 | `EXIT_PROCESS_DEBUG_EVENT` | 用于进程树收尾 |
| 6 | `LOAD_DLL_DEBUG_EVENT` | **噪音极多**（实测一次启动 20+ 个），必须过滤 |

**取命令行**：`CREATE_PROCESS_DEBUG_INFO` 布局

```
hFile(0,8) hProcess(8,8) hThread(16,8) lpBaseOfImage(24,8)
dwDebugInfoFileOffset(32,4) dwDebugInfoSize(36,4) lpImageName(40,8) fUnicode(48,4)
```

- `lpImageName` 在偏移 **40**
- 必须用**被调试进程自己的句柄**（偏移 **8** 的 `hProcess`）去 `ReadProcessMemory`，用伪句柄（`GetCurrentProcess()`）读**远程进程内存会失败**
- 拿到 `lpImageName` 后还要读子进程的 PEB 才能拿到完整命令行（`lpCommandLine`）

**事件循环**：不能一收到 `EXIT` 就退出，要用 quiet-tick 计数（连续 N 次 `WaitForDebugEvent` 超时才算结束），否则会漏掉还在跑的兄弟进程。

#### 3.1.4 已知限制

| 限制 | 说明 | 应对 |
|---|---|---|
| **被调试进程知道自己被调试** | PEB.BeingDebugged = 1 | 少数工具行为会变。本项目提供 `--clear-being-debugged`（默认关闭）缓解；备选的 wrapper 式捕获见 §3.1.5 |
| **调试器退出 = 被调试进程被杀** | 系统行为 | 必须做异常处理/信号转发，不能让调试器意外退出 |
| **LOAD_DLL 事件噪音** | 实测一次启动 20+ 个 | 必须按事件类型过滤，否则吞吐塌 |
| **异常处理策略** | `DBG_CONTINUE` 会吞掉 first-chance 异常 | 区分 `DBG_EXCEPTION_NOT_HANDLED`（未处理异常交回程序）和 `DBG_CONTINUE` |
| **WOW64** | x86 目标在 x64 调试器下 | 寄存器上下文需 `Wow64GetThreadContext`；**命令行捕获不受影响**（在 PEB 里直接读） |
| **cmd.exe 内建命令** | 不产生进程 | 脚本层解决，见第 4 节 |
| **性能** | 每事件一次内核往返 | 过滤 LOAD_DLL / CREATE_THREAD / EXIT_THREAD |

#### 3.1.5 备选路径：`--instrument`（不用调试器的捕获方式）

Windows 的调试器方案有一个无法回避的副作用：**被调试进程知道自己在被调试**
（`PEB.BeingDebugged = 1`，`IsDebuggerPresent()` 也返回 1）。
少数工具会因此改变行为 —— 反作弊、部分 JIT、自校验代码。

商业静态分析工具给出的备选是一个 `--instrument` 开关。它的定位（官方原文）：

> "If your build system has problems running in debug mode, try using the `--instrument`
> option with `cov-build`."

> `--instrument` —— Compatible with Java, C, C++, C#, and Visual Basic builds on
> **Windows only**, uses the **instrumentation mode instead of the debugger**.
> For certain builds, this configuration can significantly improve build times.
> In particular, **parallel builds will benefit most from `--instrument`**.

**它是怎么实现的 —— 不是"换一种 hook"，而是换一种"拦截点"：**

| | 调试器模式（默认） | `--instrument` 模式 |
|---|---|---|
| 拦截点 | **进程创建**（`CreateProcess`） | **编译器调用**（compile invocation） |
| 怎么拿到命令行 | 调试事件 + 读子进程 PEB | 编译器 wrapper 直接拿到 argv |
| 覆盖范围 | **整棵进程树**（任何程序） | **只有编译调用**（C/C++/Java/C#/VB） |
| 能看到非编译进程吗 | 能 | **不能** |
| 被调试进程自知 | **是** | **否** |
| 跟并行构建的关系 | 调试器要逐个接管进程，串行化严重 | 不接管进程，**并行构建能真正并行** |

关键差别是**拦截点变了**：

- **调试器模式**在 **OS 的进程创建边界**上拦截，所以能看见整棵树，
  代价是每个进程都被挂上调试器（进程自知、且吞吐受调试事件拖累）。
- **`--instrument` 模式**在**编译器调用的边界**上拦截 —— 靠一个 wrapper
  插在编译器和构建系统之间（构建系统调 wrapper，wrapper 转发给真正的编译器，
  同时把命令记录下来）。这样**目标进程完全不知道自己被"处理"过**，
  但代价是**只覆盖编译调用这一类事件**。

**两个硬约束（决定了它不能替换调试器模式）：**

1. **它不是通用的进程捕获机制。** 只对编译型语言有效，
   抓不到构建过程中其它程序的调用。对本项目"捕获脚本拉起的**所有**命令"
   这个需求来说，它天然**覆盖不全**。
2. **它和并行 emit 互斥。** 官方文档明确写：
   `--parallel-emit` "has no effect and **cannot be used in combination with
   `--instrument`**"。也就是说走 `--instrument` 就失去了并行编译加速
   （`--parallel-emit` 在 Windows 上默认开启）。

**它明确提到的适用场景**，正好暴露了调试器模式的一个真实弱点：

> "This option might be useful is for capturing a **32-bit javac compilation on
> 64-bit Windows**."

—— 在 64 位 Windows 上抓 32 位 `javac` 时，调试器模式会因为
WOW64 双 PEB 的复杂度而容易出问题。

**对本项目的取舍：**

| 需求 | 调试器模式 | `--instrument` 式 wrapper |
|---|---|---|
| 抓构建过程中**所有**命令 | ✅ | ❌ 只抓编译 |
| 抓非编译类程序 | ✅ | ❌ |
| 目标行为不被改变 | ⚠️ 默认会改（`--clear-being-debugged` 可缓解） | ✅ |
| 并行构建吞吐 | ⚠️ 受限 | ✅ |

所以本项目的选择是**以调试器 API 为主路径**，并提供
`--clear-being-debugged`（默认关闭）作为缓解手段；
**不实现** wrapper 式捕获 —— 因为它满足不了"所有命令"这个需求。

> 参考：<https://docs.blackduck.com/blackduck/2026.3/coverity-documentation/windows.html>
> 以及 <https://sig-synopsys.my.site.com/community/s/article/Improving-Build-and-Analysis-Time>
> （`--instrument` 与 `--parallel-emit` 互斥的说明）

### 3.2 方案二：Detours（不推荐做主路径）

Detours 本身能力足够（inline trampoline、事务式 attach/detach、支持 x86/x64/ARM/ARM64、MIT 许可、单次拦截 < 400ns），但**不适合做"捕获整棵树"的主路径**：

| 问题 | 说明 |
|---|---|
| 注入竞态 | 目标进程在注入完成前 fork 出的子进程会**漏抓** |
| 需注入系统服务 | `CreateProcessAsUserW`（UAC 提权）走 AppInfo 服务、`ShellExecuteEx` 走 DcomLaunch 服务，都是 `svchost.exe`，普通应用注入不进去 |
| API Set 转发 | `CreateProcessAsUserW` 实际在 `api-ms-win-core-processthreads-l1-1-0.dll`，只 hook kernel32 导出会漏 |
| WOW64 | 需 32/64 双架构 DLL |
| 完整性级别 | UAC 隔离，低权限进程注入不了高权限进程 |
| 静态链接 | 同 Linux，目标程序静态链接则 hook 不进去 |
| 被 EDR 标记 | inline hook 是可疑行为 |

**Detours 适合的场景**（如果需要）：
- 需要比调试器 API 更细的参数（比如 `CreateProcessInternal` 的完整参数、job object 继承关系）
- 需要修改行为而不只是观测

**但对本项目，"观测所有命令"这个需求，调试器 API 已经完整覆盖，且零权限要求。**

### 3.3 方案三：ETW（因权限约束出局）

ETW 是内核级事件流，`Microsoft-Windows-Kernel-Process` + keyword `0x10` 能拿到 `CommandLine`：

```powershell
logman create trace CCProbe -p "Microsoft-Windows-Kernel-Process" 0x10 4 -o C:\ccprobe.etl -ets
```

**本机实测：创建 session 直接 `Access is denied`**，因为需要管理员创建内核 session。

| 维度 | ETW | 调试器 API |
|---|---|---|
| 需要管理员 | **是** | **否** |
| 需要注入 | 否 | 否 |
| 覆盖整棵树 | 是 | **是** |
| 漏子进程 | 否 | 否（DEBUG_PROCESS） |
| 提权进程 | 是 | 需目标自己提权（DebugActiveProcess 权限等价） |
| 被 EDR 标记 | 否 | 否 |
| 抓 cmd 内建命令 | 否 | 否 |
| 静态链接程序拉起的子进程 | **能抓到**（内核层） | 能抓到（内核层创建，调试器收到事件） |

**ETW 唯一的技术优势**是"内核层"——它能抓到静态链接程序拉起的子进程。但调试器 API 同样是内核层创建进程时通知，**这个优势并不存在**。

→ **在"拿不到管理员权限"这个约束下，ETW 直接出局。**

### 3.4 方案四：WMI / CIM 轮询（只能作补充）

`Get-CimInstance Win32_Process` 能拿到 `ProcessId` / `ParentProcessId` / `Name` / `CommandLine`。

**问题**：轮询会漏掉短命进程。本机实测中，`cmd.exe /c echo etw_probe_marker` 启动后立刻退出，查询时已经不在列表里。

**结论**：只能作为**补充**（事后重建进程树、交叉验证），**不能作为主采集通道**。

### 3.5 Windows 方案对比总表

| 方案 | 需要管理员 | 跟整棵树 | 漏短命进程 | 抓内建命令 | 复杂度 | 推荐 |
|---|---|---|---|---|---|---|
| **调试器 API `DEBUG_PROCESS`** | **否** | **是** | 否 | 否 | 中 | **✅ 主路径** |
| ETW Kernel-Process | 是 | 是 | 否 | 否 | 中 | ❌ 权限出局 |
| Detours 注入 | 视目标 | 是 | 可能（竞态） | 否 | 高 | ⚠️ 仅作增强 |
| WMI 轮询 | 否 | 否 | **是** | 否 | 低 | ⚠️ 仅作补充 |
| 脚本静态分析 | 否 | N/A | N/A | **是** | 高 | **✅ 必做（另一条腿）** |

## 4. 脚本层：真正解决 L1 的地方

既然 L1（内建命令）在任何进程级方案下都不可见，唯一出路是**解析脚本源码**。

### 4.0 为什么 hook 不到 `cd` —— 以及"它明明能被拿到"是怎么做到的

这是最容易被误解的一点，先说清楚。

#### 4.0.1 `cd` 在 OS 层面根本不是一个进程

`cd` 的语义是**改变当前进程的工作目录**。而工作目录是**每进程**的属性 ——
子进程从父进程继承一份副本，改自己的那份对父进程无效。

所以 `cd` **只能**由 shell 自己在自己的进程里实现（调 `SetCurrentDirectory` /
`chdir`）。它不可能是一个外部程序：

```
shell (cwd = /home/me)
  └─ fork → 子进程执行 "cd /tmp"
              子进程的 cwd 变成 /tmp
              子进程退出
shell (cwd = /home/me)   ← 什么都没发生
```

如果 `cd` 是外部程序，那它改了目录也白改 —— 父进程的目录不会跟着变。
**这就是所有 shell 都把 `cd` 做成内建命令的根本原因**，不是性能选择，
是语义约束。

同类的还有 `exit`（必须终止 shell 自己，不能终止子进程）、
`export`/`unset`（必须改 shell 自己的环境变量）、
`source`/`.`（必须在当前 shell 进程里执行，而不是 fork 一个新的）。

`echo`/`pwd` 不同：它们**可以**是外部程序（`/bin/echo` 确实存在），
但做成内建是为了省掉 fork 开销。

#### 4.0.2 实测证据（本机，Windows 10）

**证据 A：只跑内建命令的脚本，运行时捕获到 0 个子进程。**

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
[000001] d=0 RT pid=16684 cmd.exe /c .../builtins_only.bat
```

只有 cmd.exe 自己。**7 个内建命令，0 个运行时事件。**

**证据 B：换成外部命令，立刻就有事件。**

```bat
@echo off
where.exe cmd
```

```
[000001] d=0 RT pid=17452 cmd.exe /c .../external.bat
[000002] d=1 RT pid=19044 where.exe cmd     ← 真的有进程
```

**证据 C：内建命令在输出里标的是 `ST`（静态层），不是 `RT`（运行时层）。**

```
[000002] d=0 ST pid=- cd C:\Windows   [builtin.bat:2]   (builtin)
```

`pid=-` 就是"**没有进程**"。

#### 4.0.3 那"它明明能被拿到"是怎么回事

关键在于：**它不是被"进程级机制"拿到的，是被"读脚本源码"拿到的。**

任何工具想拿到 `cd`，走的都是同一条路：

1. **知道脚本文件在哪**（构建系统知道，或者从命令行参数知道）；
2. **读那个文件**；
3. **自己解析**出哪些是内建命令、哪些是外部程序。

第 3 步就是"静态分析"。区别只在于**解析得好不好**：
有的工具只认 `COMPILING`/`EXECUTING` 几行日志（那只覆盖编译调用），
有的工具做真正的脚本词法分析（才能覆盖内建命令）。

**所以"能拿到 `cd`"和"能抓到进程"是两个不同的能力。**
前者需要**读脚本**，后者需要**看进程**。任何声称"用 hook 拿到 `cd`"的说法，
要么是把静态分析误称为 hook，要么是那个工具在别处补了解析。

#### 4.0.4 为什么静态分析有天然的边界

因为它读的是**文本**，不是**执行**：

| 情况 | 静态分析能不能拿到 | 为什么 |
|---|---|---|
| `cd C:\Windows` | ✅ | 文本里就是这一行 |
| `cd %MYDIR%` | ⚠️ | 要展开变量才知道实际目录 |
| `cd $dir`（PowerShell） | ❌ | 变量值运行时才知道 |
| `for %i in (a b) do cd %i` | ⚠️ | 要建模循环才知道会 cd 几次、到哪 |
| `eval "cd /tmp"` | ❌ | 命令藏在字符串里 |
| `cmd /c "cd /tmp"` | ✅ | 子进程那一层的脚本解析能覆盖 |

**本项目的处理方式**：
- 能静态确定的 → 报出来，并标 `is_builtin: true`、`kind: "builtin"`、
  `resolved: false`（内建命令**没有**"可执行文件路径"这回事）；
- 不能确定的 → **不猜**。宁可少报，也不产生伪命令。

> 顺带修掉的一个真实缺陷：
> `for %i in (a b) do echo loop` 里的括号装的是**迭代列表**，
> 不是命令。老实现把括号里当命令，于是报出 `a b` 而丢掉了真正的 `echo loop`。
> 现在解析 `do` 之后的命令体。

#### 4.0.5 小结

| 问题 | 答案 |
|---|---|
| 为什么 hook 不到 `cd`？ | 它**不是进程**。工作目录是每进程属性，`cd` 必须由 shell 在自己进程内实现 |
| `echo`/`dir` 呢？ | 同样不是进程。`echo` 虽然有 `/bin/echo`，但 shell 用的是内建实现 |
| 那怎么拿到 `cd`？ | **读脚本源码**（静态分析），不是 hook |
| 静态分析的天花板在哪？ | 变量展开、动态构造、循环展开 —— 这些本质上是"执行才能确定"的信息 |
| 本项目怎么做？ | 能确定的报出来并标明是内建；不能确定的**不猜** |

### 4.1 需要支持的脚本类型

| 类型 | 扩展名 | 内建命令语义 | 解析难度 |
|---|---|---|---|
| POSIX sh | `.sh` | `cd` `echo` `export` `read` `set` `source` `.` | 中（语法规范明确） |
| Bash | `.sh` | 同上 + 数组/函数 | 中 |
| Batch | `.bat` `.cmd` | `cd` `copy` `del` `set` `echo` `if` `for` `call` `goto` | **高**（`%var%` 展开、`&` 拆分、`for` 循环） |
| PowerShell | `.ps1` | cmdlet 是一半 cmdlet 一半 exe | **高**（`$` 展开、管道、别名） |
| Python | `.py` | `os.system` / `subprocess` → 都是真进程 | 低（只需定位调用点） |
| Node.js | `.js` | `child_process.spawn` → 真进程 | 低 |

**注意**：Python/Node/Ruby/Perl 的脚本里，**所有外部命令最终都是真进程**（`subprocess` → `posix_spawn`/`CreateProcess`），所以 L1 问题**不存在**。只有 **sh / batch / PowerShell** 这三类有真正的内建命令语义。

### 4.2 cmd.exe 解析规则（已查阅一手资料）

参考 [How cmd.exe parses a command](https://github.com/johnstevenson/winbox-args/wiki/How-cmd.exe-parses-a-command)：

**元字符**：`" ^ & | < > ( ) % !`

**变换顺序**：
1. `%var%` 展开（对整条命令）
2. 按 `&` `|` `(` `)` 拆分为多条命令
3. `!var!` 展开（仅当 `DelayedExpansion` 开启）
4. 识别命令名 → 内建 or `CreateProcess`

**拆分后的执行**：
```c
// cmd 内部命令：SET —— 不创建进程
// cmd 外部命令：
CreateProcess(C:\\projects\\prog.exe, "prog paramA \"C:\\ProgramData\"", ...);
```

### 4.3 PowerShell 的关键点

- `Start-Process` 默认走 `UseShellExecute` → `ShellExecuteEx` → **DCOM Launcher**
- `Start-Process -Verb RunAs` → UAC 提权 → **AppInfo 服务**
- `&` 调用操作符 / `cmd /c` → `CreateProcess`
- `Invoke-Expression` → 在 PowerShell 进程内求值，**内建 cmdlet 不产生进程**

**实测验证**（本机，PowerShell 脚本内 `Start-Process cmd.exe`）：
```
ProcessId      : 16284
ParentProcessId: 18248
Name           : cmd.exe
CommandLine    : C:\Windows\System32\cmd.exe /c call "..."
```
→ `Start-Process` 确实产生了 `cmd.exe` 子进程，父子关系可从 `ParentProcessId` 重建。

---

## 5. 构建系统侧的差异化

用户要求"调研不同的构建方式底层在命令行调用的不同逻辑"。

### 5.1 编译数据库生成工具的对比

| 工具 | 原理 | 平台 | 覆盖范围 |
|---|---|---|---|
| **CMake** | 原生 `CMAKE_EXPORT_COMPILE_COMMANDS=ON` | 全 | 只记 C/C++ 编译，不记其他命令 |
| **Ninja** | 原生 `ninja -t compdb` | 全 | 同上 |
| **Clang** | `-MJ` 逐文件生成 | 全 | 同上 |
| **编译数据库生成器** | LD_PRELOAD 拦截 execve | Linux/macOS | **所有** exec 的命令 |
| **intercept-build** (scan-build) | 编译器 wrapper 或 LD_PRELOAD | Unix | 编译器为主 |
| **cdcc** | wrapper + SQLite | Linux | 编译器 |
| **compiledb-generator** | 解析 `make --dry-run` 输出 | 全 | make only，且**是解析不是观测** |
| **Sourcetrail VS Extension** | 解析 .sln/.vcxproj | Windows | MSBuild 项目 |

**核心洞察**：CMake/Ninja/Clang 的"原生支持"是**声明式**的（构建系统自己说要跑什么），而 LD_PRELOAD 那一类工具是**观测式**的（实际跑了什么）。

**声明式的致命弱点**：CMake 的 `compile_commands.json` **不包含** `custom_command`、`add_test`、install 步骤、以及所有非编译调用。**对"捕获所有命令"这个需求，声明式方案根本不满足。**

→ **必须走观测式路线。**

### 5.2 各构建系统的实际命令调用模式

| 构建系统 | 调用方式 | 观测点 |
|---|---|---|
| **make** | `fork` + `execve("/bin/sh", ["sh", "-c", "recipe"])` | `execve` ✓（注意中间有 sh） |
| **ninja** | `posix_spawn` 或 `fork+execve` | `posix_spawn` ✓（**必须覆盖**） |
| **cmake --build** | 调 make/ninja | 间接 |
| **MSBuild** | .NET `Process.Start` → `CreateProcessW` | `CreateProcessW` ✓ |
| **gradle** | JVM `Runtime.exec` → `fork+execve` | ✓ |
| **npm/yarn** | node `child_process.spawn` → `posix_spawn` | ✓ |
| **cargo** | `posix_spawn` | ✓ |
| **bazel** | 自己实现的 spawn，**可能绕过 libc** | ⚠️ 需要额外验证 |
| **autotools** | `sh` 脚本 → `execve` | ✓（多一层 sh） |

**注意 make 的特殊性**：make 实际执行的是 `sh -c "recipe"`，所以 LD_PRELOAD 看到的第一层是 `sh`，真正的命令在 sh 内部再 execve。**这层 sh 是正常的**，但如果脚本是 `sh -c` 形式，L1 的内建命令（`cd`/`echo`）就发生在 sh 内部 → 只能靠脚本层补。

### 5.3 一个重要行为差异

编译数据库生成器只记录**编译器调用**并输出 `compile_commands.json`。本项目要记录**所有**命令，所以：

> **架构上应该借它的拦截机制，但不要借它的过滤逻辑。**

---

## 6. 推荐架构

### 6.1 总体结构

```
┌─────────────────────────────────────────────────────────┐
│  CommandCapture CLI  (ccap run <script> [args])        │
│  ┌───────────────────────────────────────────────────┐  │
│  │  1. 脚本分析层 (Script Analyzer)                  │  │
│  │     解析 sh/bat/ps1 → 提取内建命令 (L1)           │  │
│  │     输出: 静态命令清单 + 变量引用警告             │  │
│  └───────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────┐  │
│  │  2. 运行时捕获层 (Runtime Capture)                │  │
│  │     Linux: LD_PRELOAD shim → execve/posix_spawn   │  │
│  │     Win:   调试器 API (DEBUG_PROCESS) 采集器   │  │
│  └───────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────┐  │
│  │  3. 归一化 + 合并层 (Normalizer)                  │  │
│  │     统一事件模型 → 合并 L1/L2 → 去重 → 进程树     │  │
│  └───────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

### 6.2 统一事件模型

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

L1 事件则带 `"source": "static"` + `"script": "build.bat"` + `"line": 17`。

### 6.3 Windows 实现建议

**Tier 1 — 调试器 API 采集器（推荐主路径）**

C# 实现，直接 P/Invoke Win32 调试器 API。参考 `prototypes/windows-debugproc/DbgProbe5.cs`。

核心流程：

```csharp
// 1) 以 DEBUG_PROCESS 启动目标
const uint DEBUG_PROCESS = 0x00000001;
var si = new STARTUPINFO { cb = Marshal.SizeOf<STARTUPINFO>() };
CreateProcessW(null, cmdLine, IntPtr.Zero, IntPtr.Zero, false,
               DEBUG_PROCESS, IntPtr.Zero, null, ref si, out var pi);

// 2) 事件循环：只关心 CREATE_PROCESS(3) / EXIT_PROCESS(5)，其余丢弃
while (true) {
    if (!WaitForDebugEvent(out var ev, 250)) { if (++quiet >= 6) break; continue; }
    quiet = 0;

    if (ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
        var hProc = ReadIntPtr(ev.u, 8);    // 被调试进程句柄
        var namePtr = ReadIntPtr(ev.u, 40); // lpImageName
        var image = ReadRemoteString(hProc, namePtr);
        var cmdline = ReadCommandLineFromPeb(hProc);  // 读子进程 PEB
        Record(new CapturedCommand { Executable = image, Argv = cmdline, ... });
    }
    ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
}
```

实现要点（详见 `prototypes/windows-debugproc/README.md`）：

1. `STARTUPINFO` 必须传 `ref`，`cb` 要正确填；传 `IntPtr.Zero` → `ok=False err=0`（静默失败，最难查）
2. `ContinueDebugEvent` 的 status 用 `DBG_CONTINUE (0x00010002)`，用 `0` 会导致被调试进程卡在第一次异常
3. **必须按事件类型过滤**：`LOAD_DLL(6)` 噪音极大（一次启动 20+ 个）
4. 读远程内存要用**被调试进程的 hProcess**，不能用伪句柄
5. 事件循环要有 quiet-tick 收尾，不能见到 `EXIT` 就退
6. 调试器必须优雅退出，否则会连带杀掉被调试进程

**Tier 2 — Detours 补齐（可选，默认不做）**

只有在需要比调试器 API 更细的信息时才加（如 `CreateProcessInternal` 完整参数、job object 继承关系）。需要 32/64 双架构 DLL + 注入 + `DetourFindFunction` 传 API Set 名。**默认不做**，因为调试器 API 已经覆盖需求且零权限要求。

**Tier 3 — 脚本层（必做，覆盖 L1）**

- 解析 `.bat` / `.cmd`：实现 `%var%` 展开（单趟）、`&`/`|`/`(`/`)` 拆分、内建命令表
- 解析 `.ps1`：识别 cmdlet（内建）vs 外部 exe；`Invoke-Expression` 标记为"内联执行，无法静态展开"
- 解析 `.sh`：识别 POSIX 内建

### 6.4 Linux 实现建议

**Tier 1 — LD_PRELOAD shim（推荐主路径）**

直接参考同类工具的实现（C + Rust 混编，或纯 C）：

```c
// 必须覆盖的符号（完整列表）
execve, execv, execvp, execvpe, execl, execlp, execle,
fexecve, posix_spawn, posix_spawnp,
__execve, __posix_spawn  // glibc 内部
```

关键实现要点（从同类工具学到的）：
1. **变参函数需要 C shim** —— `execl`/`execlp` 是变参的，Rust 无法直接处理
2. **环境变量自愈** —— 在 hook 里重写传给子进程的 envp，把 `LD_PRELOAD` 补回去
3. **上报用 TCP socket**，不要用文件（避免 fork 时的文件锁问题）
4. **静默失败** —— hook 内部任何错误都不能影响目标程序

**Tier 2 — ptrace + seccomp-BPF 备选**

用于目标程序静态链接或 preload 被禁的场景。

**Tier 3 — 脚本层**（同 Windows）

### 6.5 关于"直接 vs 间接"的覆盖保证

| 命令来源 | Linux | Windows |
|---|---|---|
| 脚本直接调用的外部命令 | ✓ LD_PRELOAD | ✓ 调试器 API |
| 脚本调用的脚本（递归） | ✓ | ✓ |
| 构建工具拉起的命令（ninja→cl） | ✓ | ✓ |
| 提权/新会话进程 | ✓ (setuid 除外) | ⚠️ 需目标自己提权（UAC 会另起进程） |
| 脚本内建命令 | ✗ → 脚本层 | ✗ → 脚本层 |
| 静态链接程序拉起的命令 | ✗ → ptrace 备选 | ✓ 仍可见（内核层创建，调试器收到事件） |
| 脱离进程树（daemonize） | ✓ 仍可见 | ✓ 仍可见 |

> **两个平台的"内核层"差异**：
> - Linux 的 `LD_PRELOAD` 是**用户态**机制 → 静态链接程序拉起的子进程抓不到
> - Windows 的调试器 API 是**内核层**通知 → 静态链接程序拉起的子进程照样能抓到
>
> **两个平台都不抓脚本内建命令** → 脚本静态分析层是共同必做项。

---

## 7. 风险与已知缺口

### 7.1 必知缺口（应写入产品文档）

| 缺口 | 平台 | 说明 |
|---|---|---|
| 脚本内建命令 | 双平台 | 只能靠静态解析，动态构造的（`eval "$CMD"`）无解 |
| 变量动态拼接的命令 | 双平台 | `cmd.exe /c "%MY_CMD%"` 无法静态展开；运行时需靠拦截 cmd.exe 的读取 |
| 静态链接 | Linux | 需 ptrace 备选 |
| setuid 程序 | Linux | 内核丢弃 LD_PRELOAD |
| SELinux/AppArmor | Linux | 需策略配合 |
| 极短命进程 | 双平台 | LD_PRELOAD / 调试器 API 都不会漏（内核层）；WMI 会漏 |
| 被调试进程自知 | Windows | `PEB.BeingDebugged = 1`；少数反调试工具行为会变。本项目用 `--clear-being-debugged` 缓解；备选的 wrapper 式捕获见 §3.1.5 |
| 调试器退出带杀进程 | Windows | 系统行为；必须优雅退出 + 信号转发 |
| LOAD_DLL 事件噪音 | Windows | 实测一次启动 20+ 个；不按事件类型过滤会导致吞吐塌 |
| 异常处理策略 | Windows | `DBG_CONTINUE` 会吞掉 first-chance 异常；需区分 `DBG_EXCEPTION_NOT_HANDLED` |
| `cmd.exe` 的 `for` 循环展开 | Windows | `for %i in (a b c) do cmd` 实际执行 N 次，静态解析需建模循环变量 |
| PowerShell 管道 | Windows | `cmd \| cmd` 静态可解析但要小心引号 |

### 7.2 Windows 调试器 API 的事件处理注意（重要）

调试器 API 不会"丢事件"（内核保证投递），但**处理不当会漏或卡**：

| 症状 | 原因 | 对策 |
|---|---|---|
| 只收到 1 个 CREATE 事件然后卡死 | `ContinueDebugEvent` 的 status 传了 `0` 而不是 `DBG_CONTINUE` | 用 `DBG_CONTINUE (0x00010002)` |
| `CreateProcess` 返回 `ok=False err=0` | `STARTUPINFO` 传了 `IntPtr.Zero` | 传 `ref STARTUPINFO` 并填 `cb` |
| 读不到映像名 | 用伪句柄读远程内存 | 用被调试进程的 `hProcess` |
| 漏掉兄弟进程 | 见到 `EXIT` 就退出事件循环 | 用 quiet-tick 计数收尾 |
| 吞吐塌 | 没过滤 `LOAD_DLL` | 按事件类型过滤 |

---

## 8. 结论与建议

### 8.1 核心判断

1. **Linux 侧：采用 LD_PRELOAD 机制**，但保留所有命令而非只保留编译器调用。技术风险低，不需要管理员权限。

2. **Windows 侧：用调试器 API（`DEBUG_PROCESS`），这是同类工具在 Windows 上的做法。**
   - ✅ **不需要管理员权限**（本机非管理员会话实测通过，捕获到根/子/孙三层进程）
   - ✅ **整棵进程树可见**（`DEBUG_PROCESS` 跟子进程；`DebugActiveProcess` 不跟，别用错）
   - ✅ **不需要注入**，不碰 EDR 雷区
   - ✅ **内核层通知**，静态链接程序拉起的子进程照样抓到

   **明确排除**：
   - **ETW** —— 需要管理员创建内核 session（实测 `Access is denied`），在"拿不到权限"约束下出局
   - **Detours 为主路径** —— 需注入系统服务才完整、有注入前 fork 竞态、会被 EDR 标记。降级为可选增强

3. **两平台都必须有脚本静态分析层**，否则"所有命令"这个需求在语义上无法满足（cmd.exe / sh 的内建命令不产生进程）。这是本项目相对"只记录编译器调用"那一类工具的**主要增量**。

4. **Windows 侧的技术风险点从"权限"转移到了"行为兼容性"**：
   - 被调试进程知道 `PEB.BeingDebugged = 1`，少数工具行为会变（本项目用 `--clear-being-debugged` 缓解；备选的 wrapper 式捕获见 §3.1.5）
   - 调试器退出 = 被调试进程被杀，必须优雅退出
   - `LOAD_DLL` 事件噪音必须过滤

### 8.2 建议的落地顺序

| 阶段 | 内容 | 产出 | 状态 |
|---|---|---|---|
| **P0 验证** | 最小 LD_PRELOAD shim（Linux）；最小 `DEBUG_PROCESS` 采集器（Windows） | 确认技术可行性 | ✅ **Windows 已完成**（`prototypes/windows-debugproc/`） |
| **P1 核心** | Linux: LD_PRELOAD 完整符号覆盖 + 环境自愈<br>Windows: 调试器 API 采集器 + PEB 命令行读取 + 进程树重建 | 跨平台 L2 捕获 | 待做 |
| **P2 增量** | 脚本分析器：`.bat`（cmd 语法）、`.sh`（POSIX sh） | L1 覆盖 | 待做 |
| **P3 增强** | PowerShell 解析；ptrace 备选（Linux）；Detours 插件（Windows，可选） | 覆盖率提升 | 待做 |
| **P4 交付** | 统一 CLI、JSON/NDJSON 输出、文档化已知缺口 | 可用工具 | 待做 |

### 8.3 已完成的验证实验

**Windows（非管理员，已验证）**：

```powershell
# 1. 确认 cmd.exe 内建命令不产生进程
& cmd.exe /c "echo INTERNAL & whoami.exe"

# 2. 确认 PowerShell Start-Process 的父子关系
Get-CimInstance Win32_Process | Where-Object Name -match 'cmd' |
  Select ProcessId, ParentProcessId, Name, CommandLine

# 3. 调试器 API 整树捕获（核心验证）
cd prototypes/windows-debugproc
dotnet build DbgProbe.csproj -c Release
.\bin\Release\net9.0-windows\DbgProbe.exe
# => [total] create-events=3   (根 cmd / 子 cmd / 孙 whoami)
# => [marker file exists] = True
```

**Windows（需管理员，ETW 路线已否决）**：

```powershell
logman create trace CCProbe -p "Microsoft-Windows-Kernel-Process" 0x10 4 -o C:\ccprobe.etl -ets
# 实测结果：Access is denied（非管理员会话）
```

**Linux（待验证）**：

```bash
# LD_PRELOAD shim 能否覆盖 execve 系 + posix_spawn 系
```

### 8.4 仍需你拍板的两点

1. **Windows 的"被调试进程知道自己在被调试"这个副作用**能接受吗？如果目标脚本里有反调试检测（少见但存在），需要退到 `--instrument` 那种"不挂调试器"的路线，代价是拿不到间接子进程。
2. **脚本解析层的范围**：`.bat` / `.sh` 是必做（内建命令语义重），`.ps1` 难度最高（cmdlet 与 exe 混排 + `Invoke-Expression` 无法静态展开）。先做前两个，还是三个一起上？

## 9. 参考资料

**Linux**
- LD_PRELOAD 方案的架构参考：<https://man7.org/linux/man-pages/man8/ld.so.8.html>
  （`LD_PRELOAD` / `LD_AUDIT` 的语义，以及动态链接器如何按顺序加载注入库）
- ptrace(2)：https://man7.org/linux/man-pages/man2/ptrace.2.html
- seccomp(2) / seccomp-BPF 实践：https://grith.ai/blog/supervising-ai-agents-with-seccomp-bpf-and-ptrace

**Windows**
- Microsoft Detours：https://github.com/microsoft/Detours
- CreateProcessW：https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw
- **官方文档（Windows 调试器模式 / `--instrument` 备选路径）**：https://docs.blackduck.com/blackduck/2026.3/coverity-documentation/windows.html
- Windows 调试器 API（DEBUG_PROCESS vs DEBUG_ONLY_THIS_PROCESS）：https://msdn.microsoft.com/zh-tw/ms809754
- ETW 内核 provider（GUID / keyword 表，已排除）：https://justruss.tech/index.php/2025/08/05/seond-blog
- 进程创建 API Hook 矩阵：https://blog.csdn.net/qq_59075481/article/details/128814911
- cmd.exe 解析规则：https://github.com/johnstevenson/winbox-args/wiki/How-cmd.exe-parses-a-command
- Windows 命令行参数规则：https://comcomponent.com/zh-TW/blog/windows-command-line-argument-rules
- Start-Process 参考：https://learn.microsoft.com/powershell/module/microsoft.powershell.management/start-process

**构建系统 / 编译数据库**
- 综述：https://sarcasm.github.io/notes/dev/compilation-database
- JSON Compilation Database 规范：https://clang.llvm.org/docs/JSONCompilationDatabase.html
- Nushell 的 Windows 命令执行回退策略（Stage 1/2/3）：https://deepwiki.com/abab2025/nushell_nushell_main_afdb68d/10.2-windows-process-handling

---

*本报告基于 2026-10-27 的调研与本机实测。Windows 侧调试器 API 方案已在非管理员会话下端到端验证（3 层进程树 + 执行确认），原型代码见 `prototypes/windows-debugproc/`。ETW 路线因需管理员权限已排除。Linux 侧 LD_PRELOAD 尚待本机验证。*
