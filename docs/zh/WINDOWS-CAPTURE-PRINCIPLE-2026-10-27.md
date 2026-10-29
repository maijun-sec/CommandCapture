# Windows 采集原理：为什么用调试器 API，而不是 Detours

<!-- i18n:begin -->
[**English**](../en/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md) | 中文
<!-- i18n:end -->

**日期**: 2026-10-27
**状态**: 已实现并实测

本文说明 Windows 上 CommandCapture **怎么**采集进程树，以及为什么选了
「`CreateProcessW` + `DEBUG_PROCESS`」这条路，而不是 Detours / ETW / WMI。

---

## 1. 先说结论

| 方案 | 要管理员吗 | 能看到整棵树吗 | 会被目标察觉吗 | 结论 |
|---|---|---|---|---|
| **`DEBUG_PROCESS` 调试器 API** | ❌ 不要 | ✅ 内核自动跟踪全部子孙 | ⚠️ `PEB.BeingDebugged == 1` | **主路径** |
| Detours（API hook） | ❌ 不要 | ⚠️ 依赖注入，不覆盖系统服务 | ⚠️ 模块列表里出现 detour dll | 只作可选增强 |
| ETW（内核 session） | ✅ **要** | ✅ | ❌ | 当前环境不可用 |
| WMI 事件订阅 | ❌ 不要 | ⚠️ 只在进程结束时才通知 | ❌ | 拿不到命令行 |
| `DebugActiveProcess` | ❌ 不要 | ❌ **不跟踪子进程** | ⚠️ | 只能附加单个进程 |

一句话：**调试器 API 是唯一一条「非管理员 + 整棵树 + 带完整命令行」的组合。**

---

## 2. 机制

### 2.1 启动

```
CreateProcessW(target, ..., DEBUG_PROCESS)
```

`DEBUG_PROCESS` 这个标志的含义是：**让 Windows 内核把这个进程的所有子孙进程
也纳入调试**。不需要我们做任何事 —— 不需要 hook `CreateProcess`，
不需要遍历子进程列表，内核自己会跟踪。

```
ccap.exe
  └─ cmd.exe /c build.bat            ← 调试事件：CREATE_PROCESS
       └─ whoami.exe                 ← 调试事件：CREATE_PROCESS
            └─ findstr.exe           ← 调试事件：CREATE_PROCESS
```

### 2.2 事件循环

```
WaitForDebugEvent 循环
  ├─ CREATE_PROCESS_DEBUG_EVENT → 记录事件
  ├─ EXCEPTION_DEBUG_EVENT      → 决定 DBG_CONTINUE 还是 DBG_EXCEPTION_NOT_HANDLED
  ├─ EXIT_PROCESS_DEBUG_EVENT   → 从存活集合里移除
  └─ 全部被调试进程退出 → 结束
```

**结束判据是「所有被调试进程都退出了」，不是「一段时间没有事件」。**

> **为什么不能用静默期。**
> `cmd.exe` 执行 `dir /b` 扫 10 万个文件时，期间不会有任何调试事件 ——
> 但脚本还在跑。用静默期早退会漏掉整棵子树。
> 正确判据是：根进程已退出 **且** 所有子进程都已退出。

### 2.3 命令行从哪来

这是最容易踩坑的地方：

> `CREATE_PROCESS_DEBUG_INFO` **没有 `lpCommandLine` 字段**。

所以命令行必须从被调试进程的 **PEB** 里读：

```
PEB
 └─ ProcessParameters            (PEB+0x20)
     ├─ CurrentDirectory         (PP+0x38)   工作目录
     ├─ ImagePathName            (PP+0x60)   镜像路径
     └─ CommandLine              (PP+0x70)   ★ 命令行
```

### 2.4 父进程 PID 从哪来

`CREATE_PROCESS_DEBUG_INFO` 里也**没有父进程信息**。

父 PID 来自 `NtQueryInformationProcess`：

```
NtQueryInformationProcess(hProcess, ProcessBasicInformation, ...)
  → PROCESS_BASIC_INFORMATION.InheritedFromUniqueProcessId
```

> **不要去猜 PEB+0x08。** 那个位置读出来是 `0xFFFFFFFF` ——
> 它在 64 位下不是父 PID。

### 2.5 32 位目标：WOW64 有两个 PEB

这是 32 位目标最隐蔽的坑。64 位调试器里，一个 32 位目标进程**同时存在两个 PEB**：

| 来源 | 是什么 |
|---|---|
| `NtQueryInformationProcess(ProcessBasicInformation)` | PEB64（WOW64 影子 PEB） |
| `NtQueryInformationProcess(ProcessWOW64Information)` | PEB32 ← **32 位代码真正用的** |

实测（`tests/probe/DbgPeb.cpp`）：

```
ProcessBasicInformation  -> peb = 0x914000
ProcessWOW64Information  -> peb = 0x915000
目标自己报的 fs:[0x30]    -> 0x915000
```

差 `0x1000`。**对 PEB64 打补丁，目标完全没反应。**

而且两个 PEB 里的 `RTL_USER_PROCESS_PARAMETERS` 布局不一样
（PEB32 里全是 4 字节指针，字段偏移跟 x64 的 8 字节指针布局对不上），所以：

- 命令行 / 镜像路径 / 工作目录 → 从 **PEB64** 读
- `BeingDebugged` 补丁 → 打在 **PEB32** 上

### 2.6 调试事件里的句柄要关

`CREATE_PROCESS_DEBUG_INFO` 里的 `hProcess` / `hThread` / `hFile`
都由调试器负责关闭。

不关的话，跑几千个进程就把句柄耗光 —— 症状是跑到一半突然
`CreateProcessW` 失败，而且原因看起来跟句柄毫无关系。

### 2.7 首次断点必须用 `DBG_CONTINUE`

```
DBG_CONTINUE            (0x00010002)   ← 首次 loader 断点
DBG_EXCEPTION_NOT_HANDLED (0x80010001) ← 之后程序自己抛的异常
```

用 `DBG_EXCEPTION_NOT_HANDLED` 处理首次断点，目标会**卡在初始断点**，
只创建 1 个进程就停住了。

之后的异常要**交还目标**：按线程记录是否已经见过异常，
只对每个线程的**第一个** loader 断点用 `DBG_CONTINUE`，
程序自己抛的断点/异常用 `DBG_EXCEPTION_NOT_HANDLED`。

---

## 3. 为什么不用 Detours

Detours 是微软自己的 API hook 库，做的事是**在目标进程里改写函数入口**，
把 `CreateProcessW` 的入口跳到我们的 trampoline。

它比调试器 API 强的地方只有一个：**能 hook 目标进程内部的 API 调用**，
而不只是「进程被创建」这一瞬间。

但我们要解决的问题，调试器 API 已经解决了，而且成本低得多。

### 3.1 Detours 解决不了我们的核心问题

我们要的是「**这个脚本拉起了哪些命令**」。

- 脚本内建命令（`echo` / `cd` / `for`）**根本不创建进程** ——
  Detours 和调试器 API 都看不到。
- 静态分析层已经解决了这一类，**不需要注入**。
- 真正创建进程的那些，调试器 API 一次不漏。

所以 Detours 在这里**没有增量价值**。

### 3.2 Detours 的实际代价

| 问题 | 说明 |
|---|---|
| **注入竞态** | DLL 注入发生在进程启动后的一段时间内。这段时间里进程的 API 调用**不被 hook**。用 `DetourCreateProcessWithDllEx` 可以把注入提前到进程初始化阶段，但仍有窗口。 |
| **跨系统服务边界** | 服务、Session 0 进程、某些系统组件有完整性级别限制，普通用户注入不进去。 |
| **API Set 转发** | Windows 10+ 上 `kernel32.dll` 里的导出大多是 `ApiSet` 转发，hook 目标地址和实际执行地址可能不是同一个，hook 会**静默失效**。 |
| **完整性级别 (IL) 检查** | 高 IL 进程拒绝低 IL 进程注入。 |
| **EDR / 反作弊标记** | 目标进程的模块列表里出现一个不认识的 DLL，很多 EDR 产品会直接告警。 |
| **需要处理 32/64 位两套** | 32 位目标要用 32 位版本的 DLL，路径和加载方式都不一样。 |
| **不能覆盖已有进程** | 只能 hook **之后**启动的进程。 |

### 3.3 调试器 API 没有这些问题

| 方面 | 调试器 API | Detours |
|---|---|---|
| 权限 | 普通用户即可 | 普通用户即可（但目标可能拒绝） |
| 覆盖已存在的进程 | ✅ `DebugActiveProcess` | ❌ |
| 覆盖所有子孙进程 | ✅ 内核自动跟踪 | ⚠️ 每个进程都要注入 |
| 目标模块列表被污染 | ❌ 不会 | ⚠️ 会多一个 DLL |
| 拿到完整命令行 | ✅ 从 PEB 读 | ⚠️ 要在 hook 里自己拼 |
| 32 位目标 | ✅ 支持（双 PEB 处理） | ⚠️ 需要单独的 32 位 DLL |
| 目标察觉 | ⚠️ `BeingDebugged == 1` | ⚠️ 同样能察觉 |

### 3.4 那 Detours 什么时候才有价值

如果将来需要**目标进程内部**的信息 ——
比如「这个进程调用了哪些 Win32 API」「哪些文件被打开过」——
那 Detours（或 ETW 的用户态 provider）才有意义。

**那不是 CommandCapture 的目标。** CommandCapture 要的是「命令」，
不是「系统调用」。

### 3.5 结论

> **Detours 能做的事，调试器 API 大多能做，而且更干净。
> Detours 多出来的那点能力（进程内部 hook），正好是 CommandCapture 不需要的。**
>
> 所以选了调试器 API。Detours 留作将来的可选增强，不进主路径。

---

## 4. 为什么不用 ETW

ETW（Event Tracing for Windows）理论上是最"干净"的方案 ——
内核提供进程创建事件，**目标完全察觉不到**。

但**当前环境拿不到管理员权限**：

```
StartTrace() -> 创建内核 session
  -> ERROR_ACCESS_DENIED
```

内核 session 需要 `SeSystemProfilePrivilege`（管理员）。

用户态的 ETW provider（`Microsoft-Windows-Kernel-Process` 的
`Microsoft-Windows-Kernel-Process` 用户态通道）可以用普通权限，
但事件延迟高、而且在某些版本上默认关闭，不可靠。

**结论：当前环境不可用。** 如果哪天能拿到管理员，ETW 值得作为
「零可察觉」的补充路径。

---

## 5. 为什么不用 WMI

`Win32_ProcessStartTrace` 事件：

- ✅ 不需要管理员
- ❌ **只���进程结束时才通知** —— 拿不到「正在创建」的瞬间
- ❌ 事件里**没有完整命令行**（只有可执行文件名和 PID）
- ❌ 拿不到父进程的层级关系

对 CommandCapture 来说，缺命令行 = 没用。

---

## 6. 为什么不用 `DebugActiveProcess`

`DebugActiveProcess` / `DEBUG_ONLY_THIS_PROCESS` 可以附加到一个**已经在运行**的进程。

但 `DEBUG_ONLY_THIS_PROCESS` **不跟踪子进程** ——
子进程要自己 `DebugActiveProcess` 才会被跟踪。

实测结论：**只能附加单个进程，抓不到它的后代。**

对「跑一个脚本，看它拉起了什么」这个场景，进程还没开始，
所以用 `CreateProcessW(..., DEBUG_PROCESS)` 才是对的。

---

## 7. 可察觉性

`DEBUG_PROCESS` 一定会让目标看到 `PEB.BeingDebugged == 1`。

CommandCapture 提供 `--clear-being-debugged` 来缓解：

| 检测方式 | 效果 |
|---|---|
| `PEB.BeingDebugged` | ✅ 清零 |
| `IsDebuggerPresent()` | ✅ 返回 0（读的是同一个标志） |
| `CheckRemoteDebuggerPresent()` | ❌ 仍是 1 |

第三种做不到：它查的是**内核的调试对象列表**，不是用户态标志，
用户态 patch 不到。

**默认关闭。** 静默改变被观测程序的行为是不好的 ——
只有确实知道目标会因此改变行为时才开。

---

## 8. 已知限制（都已在 `notes` 里报出来）

| 限制 | 原因 | 怎么报的 |
|---|---|---|
| `CheckRemoteDebuggerPresent()` 仍返回 1 | 内核查询，用户态改不了 | README「已知缺口」 |
| 目标自身异常可能被吞 | 调试器必须参与异常分发 | 已优化为「只对每线程首个 loader 断点用 `DBG_CONTINUE`」 |
| 调试器被强杀可能连带终止目标 | 调试器退出 = 目标退出 | 正常路径等所有子进程退出 |
| 超时截断 | `--timeout` 到点 | `notes` 明确标 `PARTIAL` |
| 静态链接 / setuid（Linux 侧） | 内核限制 | 每个事件单独标记 |

**原则：已知有丢失时必须明确说 `PARTIAL` 并说明原因，
不能静默漏报。**

---

## 9. 实测覆盖

Windows 10 19045，非管理员环境。

| 场景 | 结果 |
|---|---|
| cmd 内建命令 | ✅ 静态层捕获 |
| 直接子进程 / 管道两侧 | ✅ |
| 嵌套脚本三层 | ✅ depth 0/1/2 |
| **cmake configure** | ✅ 38 个事件，含 TryCompile 探测 |
| **cmake + Ninja** | ✅ |
| **ninja 直调** | ✅ `ninja` → `cmd.exe /C` → `c++.exe` → `c++` → `cc1plus` → `as` → `collect2` → `ld`，depth 5 |
| **GNU make** | ✅ `make` → `sh -c` → `g++` → `cc1plus` |
| **MSVC cl.exe 直调** | ✅ 命令行完整读出，含 `/EHsc` `/std:c++17` |
| **MSBuild（32 位 WOW64）** | ✅ 捕获隐藏的 `cmd.exe /Q /D /C ...exec.cmd` |
| **32 位目标（真 32 位 exe）** | ✅ 命令行完整读出，参数不丢 |
| **PowerShell 脚本 / 运行时** | ✅ 12 个事件（git / npm / dotnet 全抓到） |
| 短命进程 / 重定向 | ✅ |
| 反调试标志 | ✅ `--clear-being-debugged` |
| 进程树视图 / 过滤 / 汇总 | ✅ |

回归测试：`tests/run_tests.ps1` 87 项全过。
