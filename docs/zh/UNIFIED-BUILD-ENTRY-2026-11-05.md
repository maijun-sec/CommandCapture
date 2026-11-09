# 统一构建入口

<!-- i18n:begin -->
[**中文**](./UNIFIED-BUILD-ENTRY-2026-11-05.md) | [English](../en/UNIFIED-BUILD-ENTRY-2026-11-05.md)
<!-- i18n:end -->

**日期**：2026-11-05
**状态**：已实现，并已端到端实测

`ccap build <cmd> [args...]` 运行用户的构建，并从构建用到的**每一种语言**产出编译数据库。
用户只提供构建命令；捕获过程自己判断该用哪些机制，并把结果合并成一份输出。

---

## 1. 问题

一个多语言仓库不是有一个构建命令，而是每种语言各有一个构建命令，而且没有任何单一捕获机制
能覆盖它们全部：

| 语言 | 编译器怎么跑 | 进程钩子能看到什么 |
|---|---|---|
| C、C++、Rust、Go | 作为子进程 | 编译器调用本身 |
| Java、Kotlin、Scala | **在 JVM 内部** | 只有 `java` 启动 |
| C#、VB、F# | **在构建进程内部** | 什么都没有 |

第三行是最难的。.NET 编译器是构建以进程内方式加载的库，在默认配置下它跑在常驻的编译服务器里 ——
所以它从来不会作为一个进程出现。

以前的做法是“把工具跑起来，然后读它留下的东西”。那不可复现：它取决于工具的缓存状态、
取决于这台机器上有没有 SDK、取决于项目有没有还原过。

这里的做法是一个**统一入口**，按语言挑对应的机制，再把结果合并。

---

## 2. 三个后端

### 2.1 native —— 进程捕获

构建在调试器下运行（Windows），或在 `LD_PRELOAD` shim 下运行（Linux）。构建派生的每个进程
都被观察，其中是编译器的那些被识别出来。

这覆盖了所有以子进程方式运行编译器的场景：C、C++、Rust、Go，以及其他同形状的东西。

### 2.2 jvm —— 进程内 agent

JVM 编译器（`javac`、`kotlinc`、`scalac`）跑在 **JVM 内部**。进程钩子只看到 `java` 启动，
看不到编译。所以 agent 改为钩住编译器自己的入口点，并通过 `JAVA_TOOL_OPTIONS` 注入到整个
进程树。

### 2.3 dotnet —— MSBuild diagnostic log

.NET 编译器（`csc`）是构建以进程内方式加载的库，在默认配置下它从来不会作为一个进程出现。
构建自己的 diagnostic log 是可靠的来源：它记录了完全展开的编译器命令行。

实测：diagnostic log 里的 `Csc` 任务命令行约 21 000 字符、约 216 个参数，且**没有响应文件** ——
`@rsp` 已经展开。重放它能产出合法程序集（exit 0，4 608 字节，`MZ` 头正确）。

---

## 3. .NET 日志怎么拿到

这一段坑最多，所以把实测结论说精确。

### 3.1 为什么不用进程钩子

在 .NET SDK 9.0.101 上实测：`dotnet build` 默认 `UseSharedCompilation=true`，此时编译器
**在进程内**跑于编译服务器。没有进程创建事件，进程钩子什么也看不到。

设 `UseSharedCompilation=false` 确实会出现一个可见进程
（`dotnet exec .../csc.dll /noconfig @...rsp`），但参数在响应文件里，而 compdb 解析器
只对响应文件做分类、不做展开。于是这条路给出的是「35 条命令，0 条是编译命令」。

### 3.2 用 file logger，而不是 stdout

MSBuild 把 diagnostic log 写到它的 **stdout**，编码 UTF-16LE；控制台消息写到 **stderr**，
编码是控制台代码页。所以把 stdout 重定向到文件会顺带拿掉用户自己的构建输出，而把两个流
合并会破坏日志 —— 交错后的文件解码出来是乱码。

**file logger** 两个问题都绕开了：

```
-flp:logfile=<path>;verbosity=diag
```

实测：日志是 UTF-8，里面有完全展开的 `csc` 命令行，而构建输出仍然到达终端。诊断级别加在
**logger** 上，不是加在构建上 —— 把构建自己的级别提到 diagnostic 会把整份日志打到控制台
（实测约 100 000 行，把别的东西全淹了）。

### 3.3 没有环境变量可用

MSBuild 的级别和 file logger **只是命令行开关**。实测，以下全部被忽略：

| 变量 | 结果 |
|---|---|
| `MSBUILDVERBOSITY` | 被忽略 —— 日志保持默认级别 |
| `MSBUILDLOGTEXT` | 被忽略 —— 不产生文件 |
| `MSBuildResponseFiles` | 被忽略 —— 响应文件不被读取 |

所以开关必须送到 MSBuild 命令行，这也决定了整个入口的形状。

---

## 4. 命令分类

构建命令要分类，因为两个问题的答案不一样：

- *这个构建可能含有 .NET 部分吗？* —— 包装脚本的答案是**会**。
- *我能把 MSBuild 开关交给这个命令吗？* —— 包装脚本的答案是**不能**。

| 类别 | 例子 | MSBuild 开关 | diagnostic log |
|---|---|---|---|
| `MsBuild` | `dotnet build`、`msbuild`、`devenv` | 加上 | 由 file logger 写出 |
| `NativeCompiler` | `gcc`、`clang`、`cl` | 不加 | 无 —— 它没有 .NET 部分 |
| `Wrapper` | `make`、`ninja`、`cmake`、`cmd /c build.bat` | 默认不加 | 从 stdout 取 |

实测为什么包装脚本默认什么都拿不到：`cmake --build -v:diag` 会报
`Unknown argument -v:diag`，`cmake --build --no-incremental` 同样报错。猜错是把构建搞坏。

认不出来的命令一律归 `Wrapper`。stdout 被错误重定向的代价，低于 .NET 捕获静默为空的代价。

### 4.1 脚本构建

脚本构建由用户来声明脚本接受什么：

```
ccap build cmd /c build.bat --msbuild-args "-v:diag -flp:logfile=build.log;verbosity=diag"
```

开关加在包装脚本自己的命令行上，而转发自己参数的脚本（批处理里的 `%*`、shell 脚本里的 `"$@"`）
会把它们继续传给它调用的 MSBuild。

用户给了自己的 file logger 时，读的就是**那个**文件 —— 不是从 stdout 再取一份副本。

.NET 部分缺失时，运行会以可操作的方式说明，而不是报成「这个构建没有 .NET 部分」：

> the .NET backend found no compile command. This is a script build, and the MSBuild it
> calls was not verbose. Pass --msbuild-args to hand it "-v:diag
> -flp:logfile=<file>;verbosity=diag", and make sure the script forwards its arguments

只有真的跑过 MSBuild 时才给这条提示。完全没有 MSBuild 的构建 —— C++ 树上的
`cmake --build` —— 留下的日志里没有任何 MSBuild 的标记，那种情况给的是中性提示。
标记必须能扛住**本地化**，所以取的是不会被翻译的那几个：编译器自己的输出（`CSC :`）、
「输出 -> 文件」那一行，以及 diagnostic log 开头的环境变量转储。

---

## 5. 已经是最新状态的构建

**已是最新状态**的构建不编译任何东西，于是捕获结果为空 —— 这对捕获工具是最坏的结果：
看起来成功了，实际什么都没产出。

实测：对已构建过的项目再跑一次 `dotnet build`，写出了 5 MB 的 diagnostic log，
里面**一个** `Csc` 任务都没有。

所以构建被强制去跑编译器：

| 开关 | 作用 |
|---|---|
| `--no-incremental` | 加到 MSBuild 兼容的命令上，除非用户自己已经决定过 |

原生那边没有通用等价物，所以那边由调用方要求一次干净构建。

---

## 6. 输出

一份列表，与构建用到的语言无关：

```
ccap build <cmd> [args...] --build-output compile_commands.json
```

| 选项 | 含义 |
|---|---|
| `--build-output <file>` | 编译数据库写到哪里 |
| `--build-format <clang\|clang-cmd\|translated>` | 输出格式（默认 `clang`） |
| `--build-events` | 同时写出捕获事件 |
| `--msbuild-log <file>` | 从这份日志读 .NET 命令，而不是自己产生一份 |
| `--msbuild-args <switches>` | 交给脚本构建的 MSBuild 开关 |
| `--no-dotnet` | 不跑 .NET 后端 |

`clang` 格式是保留原始命令行的编译数据库。`translated` 格式套用
[COMPILER-SWITCH-TABLE-2026-11-03.md](./COMPILER-SWITCH-TABLE-2026-11-03.md) 里的开关表：
选项被分类，决定 AST 的那些被保留，每一条被丢弃的都带着原因报出来。

---

## 7. 实测结果

| 场景 | 结果 |
|---|---|
| `ccap build dotnet build Hello.csproj` | 1 条命令，源文件与目录都是正确的绝对路径 |
| `ccap build cmd /c build.bat`（C++ + C#） | 9 条命令：8 条 C++ 加 1 条 C#，在同一个数据库里 |
| 同上，带 `--msbuild-args` | 9 条命令，C# 那条读自用户自己的日志 |
| 同上，不带 `--msbuild-args` | 8 条命令，外加一条关于缺失 .NET 部分的可操作提示 |

C# 源文件路径按 `MSBuildProjectDirectory` 解析 —— 项目自己的目录，而不是构建启动时的目录。

---

## 8. 设计决策

1. **计划与运行分离。** 决策 —— 跑哪些后端、构建命令行长什么样 —— 是纯函数，所以不用真启动
   构建就能测。计划放在 compdb 库里，app 层只负责执行它。
2. **读文件的后端是独立的一步。** native 捕获与 JVM agent 在同一趟里合成；.NET 后端读的是
   构建产出的东西，所以它在之后跑。
3. **一份输出，不是每种语言一份。** 使用者要的是「这个仓库里每个文件是怎么被编译的」。
4. **不静默丢弃。** 缺失的 .NET 部分会带着本该抓住它的开关一起报出来。
5. **捕获不破坏构建。** 开关只交给懂得它们的命令。

---

## 9. 测试

`tests/test_compdb_dotnet.cpp` —— 74 条断言。

它建立在**真实**的 MSBuild diagnostic log 上（按 UTF-16LE 带 BOM 入库），而不是合成的一行。
合成的一行只会拿作者想象的输入去测解析器，而开发中真正坏掉的三处 —— UTF-16 编码、
未加引号的可执行文件路径、任务 ID 注解 —— 全都是真实日志的性质。

覆盖范围：

- Windows 命令行切分，包括反斜杠加引号的规则；
- 从真实日志里提取，包括修复过的可执行文件路径、剥掉的任务 ID 注解，以及已展开的响应文件；
- 编码，UTF-8 与 UTF-16 各一份，以及两种**不相同**的失败（没有编译器任务的日志是一个答案，
  不存在的日志是一个错误）；
- 命令识别；
- C# 适配器，包括分析器在翻译后没有丢；
- 构建计划：分类、每类拿到哪些开关、file logger，以及脚本构建。
