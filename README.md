# CommandCapture

捕获一个脚本（或可执行文件）**直接或间接拉起的所有命令**，输出结构化的进程树。

跨平台（Windows / Linux），零第三方依赖，单文件可执行程序。

**开源协议：[MIT](LICENSE)** —— Copyright (c) 2026 maijun。
每个源文件的头部都带 `SPDX-License-Identifier: MIT` 标识。

---

## 为什么需要两层捕获

单靠任何一种进程级机制都拿不全命令：

| 命令类型 | 例子 | 进程级机制能看到吗 |
|---|---|---|
| 外部程序 | `whoami`、`gcc`、`cmake` | ✅ 会创建进程 |
| shell 内建命令 | `echo`、`cd`、`set`、`for`、`if` | ❌ **不创建进程** |
| 管道两侧 | `a \| b` | ✅ 但中间有 shell 自己的 `cmd.exe /c` |
| 构建系统包装 | `ninja` → `cmd.exe /C cd . && cl ...` | ✅ 需要逐层跟踪 |

所以 CommandCapture 是**两层**：

1. **运行时层**（runtime）—— 真的创建了进程的事件
   - Windows：`CreateProcessW(..., DEBUG_PROCESS)` 调试器 API，捕获整棵进程树
   - Linux：`LD_PRELOAD` 注入 shim，拦截 `execve` 系与 `posix_spawn` 系
2. **静态分析层**（static）—— 扫描脚本内容，捕获内建命令
   - 支持 `.bat` / `.cmd` / `.sh` / `.ps1`

两层的事件用统一的结构体输出，靠 `source` 字段区分。

### 为什么 `cd` 这种命令 hook 不到

因为**它根本不是一个进程**。

`cd` 的语义是改变**当前进程**的工作目录，而工作目录是**每进程**的属性 ——
子进程改自己的那份，对父进程无效。所以 `cd` 只能由 shell **在自己的进程里**实现：

```
shell (cwd = /home/me)
  └─ fork → 子进程执行 "cd /tmp" → 子进程 cwd 变了 → 子进程退出
shell (cwd = /home/me)   ← 什么都没发生
```

如果 `cd` 是外部程序，改了目录也白改。**这是语义约束，不是性能选择** ——
所以所有 shell 都必须把它做成内建命令。`exit`（要终止 shell 自己）、
`export`（要改 shell 自己的环境变量）同理。

实测（Windows 10，本机）：

```bat
@echo off
cd C:\Windows
echo hello
where.exe cmd
```

```
$ ccap run mixed.bat --no-scan --format text
[000001] d=0 RT pid=16684 cmd.exe /c .../mixed.bat
[000002] d=1 RT pid=19044 where.exe cmd      ← 外部程序：有进程
```

`cd` 和 `echo` 一个事件都没有 —— 因为它们没创建任何进程。
加上静态分析层之后它们才会出现，并且标成 `ST`（`pid=-` 表示"没有进程"）：

```
[000002] d=0 ST pid=- cd C:\Windows   [mixed.bat:2]   (builtin)
```

**所以"能拿到 `cd`"和"能抓到进程"是两种不同的能力：**
前者要**读脚本源码**（静态分析），后者要**看进程**（hook/调试器）。
任何工具想覆盖内建命令，都必须有静态分析这一层。

完整推导和边界（变量展开、`for` 循环、动态构造）见
[`docs/RESEARCH-COMMAND-CAPTURE-2026-10-27.md`](docs/RESEARCH-COMMAND-CAPTURE-2026-10-27.md) §4.0。

---

## 快速开始

### Windows

```powershell
# 用 MSVC 编译
cmake -G "Visual Studio 17 2022" -A x64 -S . -B build
cmake --build build --config Release

# 跑一个构建脚本
.\build\Release\ccap.exe run build.bat --format text
```

### Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

./build/ccap run ./build.sh --format text
```

> Linux 需要 `ccap_shim.so` 和 `ccap` 在同一目录（或用 `CCAP_SHIM` 指定路径）。

---

## 命令行

```
ccap run <script|exe> [args...]   运行目标并捕获整棵进程树
ccap scan <script>                仅做脚本静态分析（捕获内建命令）

选项:
  --format <json|ndjson|text>   输出格式（默认 json）
  --out <file>                   写入文件而非 stdout
  --cwd <dir>                    目标工作目录
  --no-scan                      关闭脚本静态分析
  --no-recursive                 静态分析不递归进被调用脚本
  --depth <n>                    静态分析递归深度（默认 4）
  --timeout <ms>                 运行超时
  --quiet-ms <ms>                根进程退出后，等待残留子进程的静默期
                                 （默认 300；这是安全网，不是结束判据）
  --exclude <name>              排除可执行文件名（可重复）
  --exclude-str <text>           排除命令行包含该子串的事件（可重复）
  --include <name>              只保留该可执行文件名（可重复；白名单）
  --tree                        输出进程树视图（缩进）
  --summary                     追加一行汇总统计
  --clear-being-debugged         清掉目标 PEB 的 BeingDebugged 标志
                                 （Windows 专用；见下）
  --stdout-to-stderr             把目标的 stdout 转到 stderr
                                 （ccap 自己的 stdout 只留采集数据，
                                   这样 --format json 可以直接管道消费）
  --verbose                      额外输出诊断信息到 stderr
```

### 过滤事件

跑一次完整的构建会产出几百个事件（`as` / `cc1plus` / `collect2` / `ld` …），
其中绝大多数是编译器的内部步骤。

```bash
ccap run build.bat --exclude cc1plus.exe --exclude as.exe
ccap run build.bat --include git --include npm     # 只看白名单
```

**默认不过滤任何东西** —— 静默丢事件是最坏的失败模式。
过滤掉的条目会**计数并写进 `notes`**，所以你知道漏了多少：

```
"notes": ["filtered out 47 event(s) by --exclude/--include (kept 12 of 59)"]
```

### 进程树视图

```bash
ccap run build.bat --tree
```

```
+- cmd.exe /c D:/work/build.bat   [pid=13596 ppid=2992 d=0]
  +- whoami   [pid=18660 ppid=13596 d=1]
  +- cmd.exe /S /D /c echo piped    [pid=20448 ppid=13596 d=1]
  +- findstr piped   [pid=19720 ppid=13596 d=1]
  +- cmd /c D:\work\level3.bat   [pid=19976 ppid=13596 d=1]

static (script builtins, no process created):
  +- echo hello from echo   [D:\work\build.bat:3]
```

静态事件没有进程关系，单独列在末尾。

### 汇总统计

```bash
ccap run build.bat --summary
```

```
--- summary ---
runtime events: 9
static events:  16
max depth:      2
unique executables: 6
  cmd: 2
  cmd.exe: 2
  findstr: 1
  hostname: 2
  where: 1
  whoami: 1
```

**JSON 模式下 summary 是一个顶层字段**，不是追加在后面的文本：

```json
{
  "tool": "CommandCapture",
  "summary": {
    "runtime_events": 9,
    "static_events": 16,
    "max_depth": 2,
    "unique_executables": 6,
    "executables": { "cmd": 2, "cmd.exe": 2, "findstr": 1 }
  },
  "events": [ ... ]
}
```

> JSON / NDJSON 是**机器消费**的格式 —— 往后面追加任何非 JSON 文本都会让
> `jq` / `ConvertFrom-Json` 直接失败。所以 JSON 模式用字段承载 summary，
> NDJSON 模式干脆不输出（那个格式的约定是"每行一个事件对象"）。

### 捕获完整性

每次采集结束，`notes` 里都会有一句结论，明确回答
「你看到的这些，是不是全部？」：

```
"capture completeness: COMPLETE for process creation (DEBUG_PROCESS sees every
 descendant); script builtins are covered by the static analysis layer"
```

已知有丢失时会明确说 `PARTIAL` 并说明原因
（超时截断 / 静态链接的中间产物）。

### 把输出当机器数据消费

目标程序自己的输出默认会混进 stdout（就像直接跑一遍那样）。
要用管道消费 JSON 时，加 `--stdout-to-stderr`：

```bash
ccap run build.sh --format json --stdout-to-stderr | jq '.events[] | .executable'
```

### 被反调试程序观测到时

`DEBUG_PROCESS` 一定会让目标看到 `PEB.BeingDebugged == 1`。
加 `--clear-being-debugged` 可以清掉它：

| 检测方式 | 效果 |
|---|---|
| `PEB.BeingDebugged` | ✅ 清零 |
| `IsDebuggerPresent()` | ✅ 返回 0（读的是同一个标志） |
| `CheckRemoteDebuggerPresent()` | ❌ 仍是 1（查内核的调试对象列表，patch 不了） |

**默认关闭**：静默改变被观测程序的行为是不好的。
只有确实知道目标会因此改变行为时才开。

---

## 编译命令工具（`ccap compdb`）

`ccap compdb` 把「一堆命令行」变成「结构化的编译命令」，最后导出成
clang 的 `compile_commands.json`（或者分析器风格的翻译命令）。

它**不关心命令是怎么来的** —— 只吃「命令行文本 + 工作目录」。所以下面四种来源都能吃：

| 来源 | 怎么喂给它 |
|---|---|
| ccap 自己捕获的事件流 | `ccap compdb export events.ndjson` |
| 普通文本（每行一条命令） | `ccap compdb export commands.txt` |
| 构建日志 `build-log.txt` | `ccap compdb export build-log.txt` |
| 已有的 `compile_commands.json` | `ccap compdb export compile_commands.json` |

格式**自动识别**，不用告诉它。

### 子命令

```
ccap compdb list                    列出编译器类型全量表
ccap compdb aliases <编译器...>      查某个编译器的所有别名（软连接互认）
ccap compdb parse <文件...>          解析并统计
ccap compdb show <文件...>           展示解析出的编译命令详情
ccap compdb export <文件...>         生成 compile_commands.json（或 clang 命令）
ccap compdb translate <文件...>      生成分析器风格的翻译命令
ccap compdb capture <构建命令...>    跑一次构建并抓编译命令

compdb 选项:
  --config <file>              配置文件（JSON）
  --output <file>              输出文件（默认 compile_commands.json）
  --output-format <clang|clang-cmd|translated>
                               输出格式（默认 clang）
                                 clang       compile_commands.json，原样保留
                                 clang-cmd   每行一条真正的 clang 命令
                                 translated  分析器风格的参数
  --keep-scripts               保留脚本命令（默认过滤掉）
  --all                        show 时不过滤（同时标注 KEEP/DROP）
  --exclude <name>             额外排除（可重复）
  --include <name>             白名单（可重复）
```

### 识别：编译器全量表

`ccap compdb list` 打印的是一张**可扩展的规则表**，不是散落在代码里的 if-else：

```
Known compiler types:

  gcc      (id: gcc)
      names:  gcc g++ cc c++ cpp gcc-cc gpp
  gcc      (id: gcc-versioned)
  clang    (id: clang)
      names:  clang clang++ clang-cl clang-cpp
  msvc     (id: msvc)
      names:  cl
  iar      (id: iar)
      names:  iccarm iccarm.exe iarc ilink
  generic  (id: generic)

Versioned names are recognized too: gcc-11, g++-12, clang-15 ...
```

`gcc-versioned` 那条规则只有后缀、没有精确名 —— 它负责 `gcc-11` / `g++-12`
这类**带版本号**的名字。

匹配顺序是：**精确名 → 前缀规则 → 后缀规则**。

前缀规则存在是因为交叉编译器 —— `arm-none-eabi-g++` 的 basename 是 `g++`，
但它跟本机 `g++` **不是同一个编译器**，不能混为一谈。

### 别名：软连接真实路径互认

现实中的情况：

```
/usr/bin/gcc  -> /usr/bin/gcc-11     (软连接)
/usr/bin/cc   -> /usr/bin/gcc
/usr/bin/c++  -> /usr/bin/g++
```

`ccap compdb aliases /usr/bin/cc` 会解析**真实路径**，然后把同一真实路径下的
所有名字归成一组 —— 真实命令和软连接命令**互为别名**：

```
/usr/bin/cc
  real path: /usr/bin/x86_64-linux-gnu-gcc-11
  kind:      gcc
  rule:      gcc
  aliases:   /usr/bin/gcc /usr/bin/gcc-11 /usr/bin/x86_64-linux-gnu-gcc /usr/bin/x86_64-linux-gnu-gcc-11
```

这是文件系统层面的**确定事实**，不是猜测 —— 所以比「按名字精确匹配」
更严格也更可靠。

（真实路径不一定是 `/usr/bin/gcc-11`：Debian/Ubuntu 上 gcc 是个**两段**软连接，
`/usr/bin/gcc` → `/usr/bin/x86_64-linux-gnu-gcc-11`。这正是为什么必须
**解析真实路径**而不是按名字猜。）

扫别名的方式是**读目录**：拿到已知编译器的所在目录，列出该目录下所有条目，
逐个解析真实路径并比对。所以只扫**你关心的那几个编译器所在的目录**，
不做全盘 PATH 扫描（全盘扫描在 CI 机器上可能非常慢）。

### 过滤：只留真正的编译命令

默认会过滤掉**脚本、链接器、汇编器、归档器** —— 只留真正的编译调用。

过滤和「脚本判定」是**两个独立机制**：

- 脚本判定 = 「这个名字是 shell 解释器」（`sh` / `bash` / `cmd.exe` / `powershell.exe` …）
- 排除名单 = 用户配置的名字黑名单

所以 `--keep-scripts` 能把脚本命令放回来，它不是从黑名单里删掉的。

### 提取：头文件、宏、源文件

`ccap compdb show` 会把一条编译命令拆开：

```
[DROP] make all
       dropped: script (no compilation happens)
[KEEP] g++ -c -o property.o -I. -Igenerators -DQT_NO_TEXTCODEC property.cpp
       kind:       gcc
       directory:  /export/home/acc/qt/qmake
       sources: /export/home/acc/qt/qmake/property.cpp
       includes: . generators
       defines: QT_NO_TEXTCODEC
       output:     property.o
```

被过滤掉的命令也会显示出来，**并带上被丢弃的原因** —— 这样你能确认
「过滤掉的是脚本」而不是「它没被识别」。

支持的提取项：

| 类别 | GCC/Clang | MSVC |
|---|---|---|
| 包含路径 | `-I` | `/I` |
| 系统包含路径 | `-isystem` | `/external:I` |
| 宏定义 | `-D` | `/D` |
| 语言标准 | `-std=` | `/std:` |
| 源文件 | 位置参数 | 位置参数 |
| 输出文件 | `-o` | `/Fe` `/Fo` |
| 强制包含 | `-include` | `/FI` |
| 显式语言 | — | `/TP` `/Tp<file>` `/TC` |

> **构建日志里的 wrapper 前缀会被自动剥掉。**
> 静态分析工具的构建日志里，编译器前面常套着一层 wrapper：
> ```
> COMPILING '/opt/bin/sa-translate g++ -c -o property.o property.cpp '
> ```
> 真正的编译器是**第二个** token。不剥掉的话，编译器会被认成 wrapper 的名字
> —— 一个根本不存在的编译器 —— 于是 kind 判断不出来，参数全部解析错。
>
> 只剥**已知的** wrapper 名（`sa-*` / `tool-*` / `sast-*`），
> 不剥任意同前缀的名字 —— 用户真有个名字差不多的工具怎么办。
> 名字是**工具无关的通用写法**，不绑定任何厂商的拼法。

### 导出：clang 编译数据库

```bash
ccap compdb export build-log.txt --output compile_commands.json
```

输出遵循 [clang JSON Compilation Database](https://clang.llvm.org/docs/JSONCompilationDatabase.html) 规范，
用 `arguments` 而不是 `command` —— 前者不需要处理命令行转义，消费方两种都支持，
但 `arguments` 更不容易出错：

```json
[
  {
    "directory": "/export/home/acc/qt/qmake",
    "arguments": ["g++", "-c", "-o", "property.o", "-I.", "-Igenerators", "-DQT_NO_TEXTCODEC", "property.cpp"],
    "file": "/export/home/acc/qt/qmake/property.cpp"
  }
]
```

### 导出：分析器风格翻译命令

```bash
ccap compdb translate build-log.txt --output-format translated
```

输出**翻译后的参数**，每行一条：

```
g++ -I . -I generators -DQT_NO_TEXTCODEC /export/home/acc/qt/qmake/property.cpp
```

约定：

- **不替你加任何前端前缀** —— 前缀由调用方决定
  （任何静态分析前端都行）。工具负责的是**参数翻译**。
- 系统头文件用 `--sys_include <dir>`（分析器前端的参数名），
  而不是 gcc 自己的 `-isystem` —— 目标是**分析器**的内部表示，不是 gcc。
- 宏用 `-D<name>[=<val>]`，包含路径用 `-I <dir>`。

参数处理**按编译器分派**：GCC/Clang 用 `-I` / `-D`，MSVC 用 `/I` / `/D`，
避免语法污染。适配器之间**互不依赖**，加一种新编译器 = 加一个文件，
其它文件一行都不用动。

### 导出：真正的 clang 命令（`clang-cmd`）

这是**把原始编译命令改写成 clang 命令**的能力 —— 用于「用 clang 重放一次构建」。

```bash
ccap compdb export build-log.txt --output-format clang-cmd
```

输入是 MSVC：

```
cl.exe /c /I"..\inc" /DWIN32 /DVERSION=2 /std:c++17 /EHsc /Zi /O2 /Fo"x64\main.obj" /MD main.cpp
```

输出是 clang：

```
clang++ -I ..\inc -DWIN32 -DVERSION=2 -std=c++17 -g -O2 -c -o x64\main.obj main.cpp
```

对应的翻译规则：

| MSVC | clang |
|---|---|
| `/I<dir>` | `-I<dir>` |
| `/D<name>[=<val>]` | `-D<name>[=<val>]` |
| `/std:c++17` | `-std=c++17` |
| `/Zi` | `-g` |
| `/O2` | `-O2` |
| `/Fo<file>` | `-o <file>` |
| `/c` | `-c` |
| `/FI<file>` | `-include <file>` |
| `/external:I<dir>` | `-isystem <dir>` |

驱动按源文件类型选：`.cpp` / `.cc` / `.cxx` → `clang++`，其余 → `clang`。
（链接 C++ 目标文件需要 C++ 驱动，它会带上 libstdc++ / libc++。）

**但扩展名不是唯一依据。** MSVC 可以直接指定语言：

| MSVC | 含义 | clang |
|---|---|---|
| `/TP` | 后面所有 `.c` 按 C++ 编译 | 驱动换成 `clang++` |
| `/Tp<file>` | 指定文件按 C++ 编译 | 该文件按 C++ 处理 |
| `/TC` | 后面所有 `.c` 按 C 编译 | 驱动换成 `clang` |

这一条很重要：驱动选错**会改变 ABI** ——
`clang++` 链接 libstdc++，`clang` 不链接。
一个叫 `foo.c` 但用 `/TP` 编译的文件，如果按扩展名选驱动，
翻译出来的命令就会和原始构建产出**不同的目标文件**。

```bash
$ ccap compdb export build-log.txt --output-format clang-cmd
clang++ -include prelude.h -c foo.c     # /TP /FIprelude.h
clang -include prelude.h -c bar.c       # gcc -include prelude.h
```

`/FI`（强制包含）同样不能丢 —— 强制包含的头会定义宏，
宏会改变**哪些代码被编译**，丢掉它等于编译出另一个东西。

**翻译是有损的，而且会说出来。** 没有 clang 等价物的选项**不会静默丢掉**：

```
$ ccap compdb show build-log.txt
...
notes: no clang equivalent for /EHsc
notes: runtime library selection /MD was dropped; the ABI may differ from the original build
```

运行时库（`/MD` / `/MT`）那一行特别标出来 —— 它影响的是 **ABI**，
不只是开关，丢掉之后可能表现为链接错误，而不是编译错误。

三种输出格式的区别：

| 格式 | 含义 |
|---|---|
| `clang` | `compile_commands.json`，**原样保留**原始 argv —— 忠实记录这次构建 |
| `clang-cmd` | 每行一条**真正的 clang 命令** —— 用 clang 重放这次构建 |
| `translated` | 分析器风格的参数（`--sys_include` 等） |

### 配置

配置文件是 JSON，**可选**。没有配置时用内置默认值，一切照常工作。

```json
{
  "capture": {
    "compilers": [
      {
        "kind": "gcc",
        "id": "arm-gcc",
        "paths": ["/opt/gcc-arm/bin/arm-none-eabi-g++"],
        "prefixes": ["arm-none-eabi-"]
      },
      {
        "kind": "msvc",
        "id": "msvc-2022",
        "paths": ["C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC"],
        "names": ["cl.exe"]
      }
    ],
    "exclude": ["cproto", "protoc"],
    "exclude_prefixes": ["ccache", "sccache", "distcc"],
    "include": []
  },
  "system_includes": {
    "gcc": ["/usr/lib/gcc/x86_64-linux-gnu/12/include"],
    "clang": ["/usr/lib/llvm-15/lib/clang/15.0.7/include"]
  },
  "output": {
    "path": "compile_commands.json",
    "format": "clang"
  }
}
```

配置的三条原则（参考业界静态分析工具的配置思路）：

1. **配置是可选的** —— 没有配置时用内置默认值。
2. **配置只做加法** —— 用户配的规则**追加**到内置规则上，不是替换。
   配一个 `arm-none-eabi-gcc` 不会把 `gcc` 弄丢。
3. **配置不影响识别，只影响过滤和输出** —— 识别靠编译器名（文件系统事实），
   配置用来告诉系统「哪些算我要的」和「系统头文件在哪」。

> 解析远端构建日志时，**不会**拿本机 PATH 去补全远端编译器路径 ——
> 否则会把本机的 `g++` 错配到远端记录上。只有命令行本身带绝对路径时
> 才记录 `compiler_path`。

---

## C ABI（动态库）

捕获能力通过**稳定 C ABI** 对外暴露：

- 头文件：`modules/ccap_capture/include/ccap_capture.h`
- Windows：`ccap_capture.dll`（+ import lib）
- Linux：`libccap_capture.so`

**为什么是 C ABI 而不是直接暴露 C++**：C++ 的 ABI 不跨编译器 / 跨版本稳定 ——
同一个头文件用 MSVC 编的和用 GCC 编的，符号名和对象布局都不一样。
动态库一旦要给多语言绑定用，就必须走 C ABI。

### 接口

```c
ccap_session* ccap_session_create(void);
void          ccap_session_destroy(ccap_session* session);
int           ccap_session_run(ccap_session* session,
                               const char* target,
                               const char* const* args, size_t arg_count,
                               const ccap_run_options* options,
                               ccap_event_callback on_event,
                               ccap_note_callback on_note,
                               void* user);
int           ccap_session_scan(ccap_session* session,
                                const char* script_path,
                                const ccap_run_options* options,
                                ccap_event_callback on_event,
                                ccap_note_callback on_note,
                                void* user);

void          ccap_run_options_init(ccap_run_options* options);
ccap_platform ccap_current_platform(void);
const char*  ccap_platform_name(ccap_platform platform);
const char*  ccap_version(void);
```

`on_event` 每收到一个事件就回调一次，传入一行 JSON（**不含**结尾换行）。
回调返回后 ccap 会复用那块内存 —— 要留存就自己拷贝。

### 用法（C）

```c
#include "ccap_capture.h"
#include <stdio.h>

static void on_event(void* user, const char* json_line, size_t len) {
    printf("%s\n", json_line);
}

int main(void) {
    ccap_session* s = ccap_session_create();
    if (!s) return 1;

    ccap_run_options opts;
    ccap_run_options_init(&opts);

    const char* args[] = {"/c", "build.bat"};
    int rc = ccap_session_run(s, "cmd.exe", args, 2, &opts,
                              on_event, NULL, NULL);
    ccap_session_destroy(s);
    return rc;
}
```

`tests/test_capi.c` 就是这个程序的完整版本 —— 它**故意只链接动态库**，
不链接 `ccap_capture` 静态库。只有这样才能真正验证导出符号、C ABI 自包含性、
以及外部程序能拿到事件回调。

### 用法（Python ctypes）

实测可用（Windows / Linux 都验证过）：

```python
import ctypes

lib = ctypes.CDLL("./build/shared/Release/ccap_capture.dll")   # Linux: ./build-linux/shared/libccap_capture.so
lib.ccap_version.restype = ctypes.c_char_p
lib.ccap_platform_name.restype = ctypes.c_char_p

print(lib.ccap_version().decode())                                  # b'1.0.0'
print(lib.ccap_platform_name(lib.ccap_current_platform()).decode())  # b'windows'
```

---

## 输出格式

### `--format text`（人读）

```
[000017] d=0 RT pid=9500 cmd.exe /c D:/work/.../build.bat
[000018] d=1 RT pid=14476 whoami
[000019] d=1 RT pid=2864 where cmd.exe
[000023] d=1 RT pid=11816 cmd /c D:\work\.../level3.bat
[000024] d=2 RT pid=14452 hostname
[000004] d=0 ST pid=- echo hello from echo   [D:\work\...\build.bat:3]   (builtin)
```

- `d=N` —— 相对根进程的层级
- `RT` / `ST` —— runtime / static
- `ST` 行带脚本路径和行号

### `--format json`

```json
{
  "tool": "CommandCapture",
  "platform": "windows",
  "target": "build.bat",
  "exit_code": 0,
  "timed_out": false,
  "event_count": 24,
  "notes": [],
  "events": [
    {
      "seq": 17,
      "timestamp_ms": 1793069779950,
      "platform": "windows",
      "source": "runtime",
      "pid": 9500,
      "ppid": 0,
      "depth": 0,
      "kind": "process",
      "executable": "cmd.exe",
      "argv": ["cmd.exe", "/c", "D:/work/.../build.bat"],
      "command_line": "C:\\WINDOWS\\system32\\cmd.exe /c D:/work/.../build.bat",
      "cwd": "D:\\work\\...",
      "is_builtin": false,
      "resolved": true,
      "truncated": false,
      "script_path": "",
      "script_line": 0,
      "exec_index": 0,
      "notes": ["image=C:\\WINDOWS\\system32\\cmd.exe"]
    }
  ]
}
```

`exec_index` 是同一 `(pid, executable, argv)` 组合的第几次出现。
Linux 上 `exec` 不改 pid，同一 pid 连续 exec 相同命令是 N 次独立执行 ——
靠这个字段区分，不会被误合并。

### PowerShell

PowerShell 有自己的一套语法，所以走单独的扫描器
（`modules/ccap_core/core/powershell_scanner.cpp`）：

| 特性 | 处理方式 |
|---|---|
| 别名 `ls` / `dir` / `cat` | 展开成规范名，标 `alias_of=` |
| cmdlet `Get-ChildItem` | 识别成 `kind: "cmdlet"` |
| 管道 `a \| b` | 按管道切分成多条命令 |
| 脚本块 `{ Get-Date }` | 递归进去 |
| 调用运算符 `& "prog.exe"` / `. "s.ps1"` | 剥掉运算符，保留真正的目标 |
| **子表达式 `$(Get-Date)`** | **抽出内部命令单独报出**（外层参数保持完整） |
| **嵌套子表达式 `$((Get-Date).Year)`** | 递归提取 |
| 变量 `$env:PATH` / `$x = 1` | **不是命令**，排除 |
| 注释 `#` / `<# #>` | 排除 |
| `param($x)` | 不是命令，排除 |
| `cmd(参数)` 写法 | 拆开，让命令名保持干净 |
| **`Invoke-Expression "git status"`** | **抽出字面量里的实际命令**（`iex` 同样） |
| `Invoke-Expression $cmd` | 只报 cmdlet 本身 —— 变量形式**不猜**，避免伪命令 |
| 哈希表 `@{ Path = "C:\temp" }` | 键不是命令，排除 |
| splatting `@params` | 是参数不是命令，排除 |
| here-string `@"..."@` | 内容按**文本**处理，不产生命令；终止符 `@` 不产生伪命令 |

输出里 `kind` 字段区分：`command`（外部程序）/ `cmdlet`（内建）/ `script`（dot-source）。

子表达式的处理方式值得单独说：

```powershell
Write-Output "$(Get-Date -Format o)"
```

会产生**两条**事件：

```
Write-Output $(Get-Date -Format o)     ← 外层命令，参数完整
Get-Date -Format o                     ← 子表达式里真的被调用的命令
```

外层命令的参数**保持完整** —— 不能因为抽走了子表达式就把参数截断，
否则用户看到的命令跟脚本里写的对不上。


`--format ndjson` 每行一个事件对象，适合流式处理。

---

## 实测覆盖的场景

### Windows（Windows 10 19045，非管理员）

| 场景 | 结果 |
|---|---|
| cmd 内建命令 | ✅ 静态层捕获（`echo`/`cd`/`set`/`for`/`if`/`dir`） |
| 直接子进程 | ✅ `whoami` / `where` |
| 管道 | ✅ `cmd.exe /S /D /c echo piped` + `findstr piped` |
| 嵌套脚本 | ✅ 三层深度（depth 0/1/2） |
| **cmake configure** | ✅ 38 个事件，含 TryCompile 探测 |
| **cmake + Ninja 生成器** | ✅ |
| **ninja 直调**（不经 cmake） | ✅ `ninja` → `cmd.exe /C` → `c++.exe` → `c++` → `cc1plus` → `as` → `collect2` → `ld`（depth 5） |
| **GNU make** | ✅ `make` → `sh -c` → `g++` → `cc1plus` |
| **MSVC cl.exe 直调** | ✅ 命令行完整读出，含 `/EHsc` `/std:c++17` |
| **MSBuild（32 位 WOW64）** | ✅ 捕获隐藏的 `cmd.exe /Q /D /C ...exec.cmd` |
| **多个 MSBuild 版本** | ✅ 用 `vswhere` 找出所有已安装版本，逐个验证 |
| **32 位目标（真 32 位 exe）** | ✅ 命令行完整读出，参数不丢 |
| **PowerShell 脚本** | ✅ 别名/管道/脚本块/调用运算符/外部程序 |
| **PowerShell 运行时** | ✅ 12 个事件（git / npm / dotnet 全抓到） |
| 短命进程 | ✅ `cmd /c exit 0` |
| 重定向 | ✅ 不影响捕获 |
| **反调试标志** | ✅ `--clear-being-debugged`（见上表） |
| **进程树视图** | ✅ `--tree`，缩进 + pid/ppid |
| **事件过滤** | ✅ `--exclude` / `--include`，过滤数量进 `notes` |
| **汇总统计** | ✅ `--summary` |
| **完整性结论** | ✅ `notes` 里有 `capture completeness` |
| **PowerShell 子表达式** | ✅ `$(Get-Date -Format o)` 里的命令被单独报出 |
| **PowerShell `Invoke-Expression`** | ✅ `Invoke-Expression "git status"` 里的实际命令被提取；变量形式不猜 |
| **cmd REM 注释** | ✅ `REM` / `Rem` / `rem` 都不再出现在命令列表里 |
| **树视图完整性** | ✅ 每个运行时事件**恰好渲染一次**（孙进程不再消失） |

> Windows 采集原理（为什么用调试器 API 而不是 Detours）详见
> [`docs/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md`](docs/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md)。

### Linux（Ubuntu under WSL2）

| 场景 | 结果 |
|---|---|
| sh 内建命令 | ✅ 静态层捕获 |
| `execve` 系 | ✅ `execve`/`execv`/`execvp`/`execvpe`/`execle`/`fexecve` |
| **`execveat`** | ✅（GLIBC 2.34+ 导出，之前漏了） |
| `posix_spawn` 系 | ✅ `posix_spawn`/`posix_spawnp` |
| make | ✅ `sh -c` 包装 + 内层命令 |
| cmake + g++ 工具链 | ✅ 完整链路，depth 最高 3 |
| **深度：真正的父子嵌套** | ✅ bash(0) → sh(1) → echo(2) |
| **深度：exec 进程替换** | ✅ make(0) → echo(0)，pid 不变则层级不变 |
| **根事件** | ✅ 第一次 exec 在 `LD_PRELOAD` 生效前，由父进程补报 |
| **静态链接的中间产物** | ⚠️ 内核限制，抓不到 —— 但**逐个事件**标出来了 |
| **静态链接程序** | ⚠️ 内核限制，捕获不到 —— 但会**明确报出来**（见下） |
| **setuid 程序** | ⚠️ 内核限制，捕获不到 —— 但会**明确报出来** |
| **树视图完整性** | ✅ 每个运行时事件恰好渲染一次（孙进程不再消失） |
| **shim ABI 符号** | ✅ `nm -D` 确认 9 个 exec/spawn 符号**精确匹配** |


---

## 架构

四个模块，依赖方向**单向**：

```
modules/
  ccap_core/            纯算法与纯数据，**不允许**引用任何平台 API
    core/
      event.h/.cpp             统一事件模型（CommandEvent / EventCollector）
      json.h/.cpp              极简 JSON 序列化
      json_parser.h/.cpp       极简 JSON 解析（给 compdb 读配置用）
      command_line.h/.cpp      Windows CRT & POSIX shell 命令行切分
      normalize.h/.cpp         路径归一化、可执行文件解析、环境变量展开
      script_scanner.h/.cpp    脚本静态分析（bat/cmd/sh）
      powershell_scanner.h/.cpp PowerShell 静态分析（别名/管道/脚本块/调用运算符/子表达式）
      tree.h/.cpp              进程树视图渲染
      filter.h/.cpp            事件过滤（--exclude / --include）

  ccap_capture/         需要 OS 能力（进程捕获）
    capture/
      platform.h/.cpp          平台抽象 + 统一调度 + 完整性结论
      windows_debugger.cpp     Windows：CreateProcessW + DEBUG_PROCESS 事件循环
      peb_reader.cpp           Windows：从被调试进程读 PEB 命令行 / 父进程 PID
      linux_preload.cpp        Linux：fork/exec + 管道收集
      linux_shim.cpp           Linux：LD_PRELOAD 共享库（shim，独立构建）
      capi.cpp                 C ABI 实现
    include/
      ccap_capture.h           **稳定 C ABI**（对外唯一契约）

  ccap_compdb/          编译命令识别 / 解析 / 导出
    compdb/
      compiler_table.cpp 编译器全量表（可扩展规则）
      alias_resolver.cpp 软连接真实路径别名互认
      command_parser.cpp 多来源命令解析
      compile_command.cpp 编译命令结构化
      filter_rules.cpp   脚本/链接器/汇编器过滤
      compdb_exporter.cpp 导出 clang 编译数据库 / 分析器翻译命令
      clang_translator.cpp 把任意编译器的命令翻译成真正的 clang 命令
      config.cpp         JSON 配置加载
    adapters/
      adapter_registry.cpp  适配器注册表
      gcc_adapter.cpp       GCC / G++
      clang_adapter.cpp     Clang
      msvc_adapter.cpp      MSVC cl.exe
      generic_adapter.cpp   兜底
    include/
      ccap_compdb.h

  ccap/                 CLI 入口
    app/
      main.cpp           命令分发
      cli.cpp            参数解析
      compdb_cmd.cpp     compdb 子命令实现
```

```
   ccap_compdb ──► ccap_core
   ccap_capture ──► ccap_core
   ccap ──────────► ccap_capture, ccap_compdb
```

`ccap_compdb` **不依赖** `ccap_capture` —— 它只吃「一串命令行 + 一个工作目录」，
所以能吃 CMake / ninja / 各家构建工具产生的编译命令，
不一定要自己跑捕获。

### 命名规范（参考 LLVM / clang）

完整规则见 [`docs/NAMING-CONVENTION.md`](docs/NAMING-CONVENTION.md)。

| 类别 | 风格 | 例子 |
|---|---|---|
| 类 / struct / enum | `PascalCase` | `CommandEvent` |
| 函数 / 方法 | `camelBack` | `splitCommandLine` |
| 局部变量 / 参数 | `camelBack` | `sourceFile` |
| 成员变量 | `camelBack` | `eventCollector` |
| 私有成员 | `camelBack` + 尾 `_` | `member_` |
| 常量 | `k` + `PascalCase` | `kMaxDepth` |
| 枚举值 | `PascalCase` | `CompilerKind::Gcc` |
| 命名空间 | 小写 | `ccap`、`ccap::compdb` |
| 宏 | `UPPER_CASE` | `CCAP_PLATFORM_WINDOWS` |
| 源文件 | `snake_case.cpp` | `command_parser.cpp` |
| C API | 模块前缀 + `snake_case` | `ccap_session_run` |

**C ABI 保持 snake_case 不是例外，是惯例** —— LLVM 自己的 C API
（`clang-c/Index.h`）就是 `clang_` 前缀 + snake_case。

规则由 `.clang-tidy` 的 `readability-identifier-naming` **强制执行**，
由 `tests/check_naming.py` 驱动：

```powershell
# 不需要先进 MSVC 环境 —— include 路径会自动从
# Visual Studio 安装目录 + Windows SDK 注册表项探测
python tests\check_naming.py
```

```
clang-tidy : ...\VC\Tools\Llvm\x64\bin\clang-tidy.exe
build dir  : ...\build-ninja
sources    : 29  (skipped 2 platform-specific)

OK: naming convention check passed (0 violations)
```

这个门禁有四个刻意的设计：

1. **缺 clang-tidy → 失败**（退出码 2），不是跳过。
   一个"跑不通就跳过"的门禁等于没有门禁 —— 它在 CI 上会一直显示绿色，
   而实际上什么都没检查。
   （例外：CTest 里把退出码 2 标记为 **skipped**，
   这样"本机没装 clang-tidy"不会让 CI 永久红灯，但日志会明确显示未检查。）
2. **编译错误 → 失败**（退出码 1）。
   clang-tidy 在文件有编译错误时**根本不输出命名警告**，
   "0 条警告"不等于"符合规范"。
3. **逐文件执行**，不是一次传全部 TU。
   一次传多个文件时 clang-tidy 会合并编译上下文，
   `--extra-arg` 只对第一个文件生效 → 后面的文件找不到头文件 →
   编译错误 → 命名检查被跳过。
4. **include 路径合并多个来源**，只丢弃**不存在**的。
   环境变量 `INCLUDE` 可能指向**已卸载**的工具集版本 ——
   只用它的话 clang-tidy 一个标准头都找不到，门禁静默变成空检查。
   所以同时用 Visual Studio 安装目录 + Windows SDK 注册表项。

而且每个文件要跑**两遍**（native + `-U_WIN32`），
这样 `#if defined(_WIN32) ... #else ... #endif` 的**两侧**都会被检查 ——
只看当前平台的话，另一侧的代码**从来没被分析过**。

它还会自动跳过**当前平台编译不了**的文件
（Windows 上跳过 `linux_preload` / `linux_shim`，反之亦然）——
不跳过的话，Linux 专有的 `poll.h` / `dlfcn.h` 会报一堆 "file not found"，
真实的问题被淹没在里面。

门禁已作为 `naming` 测试**纳入 CTest**：

```powershell
ctest --test-dir build -C Release --output-on-failure
```

```
4/4 Test #4: naming ...........................   Passed  157.80 sec
```

VS 生成器不产 `compile_commands.json`，脚本会**自动生成一个 sibling 的 Ninja build**
来当编译数据库 —— 不会去干扰开发者正在用的构建树。

### 产物

| 目标 | Windows | Linux |
|---|---|---|
| `ccap` | `build/Release/ccap.exe` | `build/ccap` |
| `ccap_core`（静态） | `build/Release/ccap_core.lib` | `build/libccap_core.a` |
| `ccap_capture`（静态） | `build/Release/ccap_capture.lib` | `build/libccap_capture.a` |
| `ccap_capture`（动态，C ABI） | `build/shared/Release/ccap_capture.dll` | `build/shared/libccap_capture.so` |
| `ccap_compdb`（静态） | `build/Release/ccap_compdb.lib` | `build/libccap_compdb.a` |
| `ccap_compdb`（动态） | `build/shared/Release/libccap_compdb.dll` | `build/shared/libccap_compdb.so` |
| `ccap_shim`（Linux 注入用） | — | `build/ccap_shim.so` |

> 共享库输出目录**必须和静态库分开** —— MSVC 的 import lib 也叫 `<name>.lib`，
> 放一起会覆盖同名静态库（曾经因此链不到 `run_capture`）。

### Windows 采集原理

```
CreateProcessW(target, ..., DEBUG_PROCESS)
        ↓
  ┌─────────────────────────────────────────────┐
  │  WaitForDebugEvent 循环                     │
  │   CREATE_PROCESS_DEBUG_EVENT → 记录事件     │
  │   EXCEPTION_DEBUG_EVENT    → DBG_CONTINUE    │
  │   EXIT_PROCESS_DEBUG_EVENT → 移除存活集合   │
  └─────────────────────────────────────────────┘
        ↓
  从被调试进程 PEB 读：
    ProcessParameters->CommandLine  （真实命令行）
    ProcessParameters->ImagePathName
    ProcessParameters->CurrentDirectory
    PEB->InheritedFromUniqueProcessId（父进程 PID）
```

`DEBUG_PROCESS` 会让 Windows 自动把所有子孙进程也纳入调试，所以整棵树都能捕获，不需要提权。

### Linux 采集原理

```
fork() → 子进程
  setenv("LD_PRELOAD", ccap_shim.so)
  setenv("CCAP_SHIM_FD", 3)
  execv(target)
        ↓
  shim 拦截 execve 系 / posix_spawn 系
    → 把 (pid, ppid, argv, cwd) 写到 fd 3
        ↓
  父进程 poll() 读管道 → 构造事件 → 算深度
```

#### 深度怎么算

`exec` / `posix_spawn` **不改变 pid** —— 被替换的进程和替换它的进程是同一个 pid。
所以 shim 报出的 `(pid, ppid)` 含义是：

> 「pid 为 P 的那个进程（父进程是 Q）即将被替换成新程序」

于是：

```
depth(P) = depth(Q) + 1
```

Q 就是事件里的 `ppid`。同一个 pid 反复 exec 出来的每一条事件都是**同一层**
（进程替换不改变层级）。

有两个地方容易写错：

1. **兜底值必须是 0，不是 1。** 查不到父进程只可能是根进程这一层。
2. **ccap 自己的 pid 要预先登记为 -1。** 否则根进程的 `ppid`（= ccap 的 pid）
   查不到，整条 exec 链都会掉回兜底值。

#### 根事件从哪来

fork 出来的子进程是 ccap 自己（没链接 shim），它调用 `execv` 时 shim 还没加载 ——
**那次 exec 拦不住**。

所以根命令由**父进程补报**一条，事件里带说明：

```
"notes": ["root (reported by ccap: the first exec happens before LD_PRELOAD takes effect)"]
```

#### 静态链接的诊断

shim 在每次 hook 时检查目标 ELF 的程序头（只读文件头，几百字节）。
静态链接的二进制没有 `PT_INTERP`，`LD_PRELOAD` 对它完全无效 ——
**它内部再拉起的子进程永远抓不到**。

早期版本只诊断**根目标**，中间产物是盲区。现在每个事件都单独标记：

```
"notes": ["target is statically linked - LD_PRELOAD cannot hook it, so any
           child processes it spawns will NOT be captured"]
```

诊断开关 `CCAP_SHIM_DEBUG=1` 会把每次 hook 打到 stderr ——
捕获失败时这是唯一的观察点（能看到 shim 到底 hook 到没有）。

---

## 踩过的坑（都有实测依据）

写在这里是因为这些坑都很隐蔽，且**症状和原因不匹配**。

### 1. 可执行文件路径不能换成正斜杠

`C:/WINDOWS/system32/cmd.exe` 会让 cmd.exe 解析自己镜像路径时失败，
报 "The syntax of the command is incorrect"，而且**只创建 1 个进程就退出**。

脚本参数用正斜杠是安全的，但 `lpApplicationName` 必须用原生分隔符。

### 2. 静默期不能用来判定结束

「一段时间没有调试事件」不等于「跑完了」。
cmd.exe 执行 `dir /b` 扫 10 万个文件时期间不会有任何调试事件，
但脚本还在跑 —— 用静默期早退会漏掉整棵子树。

正确判据是**根进程已退出且所有子进程都已退出**。

### 3. 32 位进程的 PEB 偏移和 64 位一样

曾经以为 x86 有一套不同的偏移（0x24/0x38/0x40），结果读 32 位 MSBuild 时
CommandLine 读出来是垃圾（len=7950, buf=0x685421CD4C01B821），
`ReadProcessMemory` 报 err=998。

实测 PEB raw dump：
```
PEB+0x20 = ProcessParameters
PP+0x38  = CurrentDirectory
PP+0x60  = ImagePathName
PP+0x70  = CommandLine
```
区别只在**指针宽度**，不在字段偏移。

### 4. 父进程 PID 只能从 PEB 拿

`CREATE_PROCESS_DEBUG_EVENT` 结构里**没有**父进程信息，也没有 `lpCommandLine`。
父进程 PID 来自 `NtQueryInformationProcess` 返回的
`PROCESS_BASIC_INFORMATION.InheritedFromUniqueProcessId`。

### 5. 反斜杠转义的 CRT 规则

单个反斜杠在非引号字符前是**字面量**。实现错的话
`C:\Windows\x.exe` 会变成 `C:Windowsx.exe`。

完整规则（`2k` 个反斜杠 + X）：
- X 是 `"` → `k` 个反斜杠 + 切换引号状态
- X 是 `"`（奇数个）→ `k` 个反斜杠 + 字面量 `"`
- X 是其它 → `k`（奇数时 `k+1`）个反斜杠 + X 本身

### 6. `ContinueDebugEvent` 的常量

首次断点必须用 `DBG_CONTINUE` (0x00010002)。
用 `DBG_EXCEPTION_NOT_HANDLED` (0x80010001) 会让目标卡在初始断点，
只创建 1 个进程。

### 7. `dedupe_ld_preload` 不能返回局部 vector 的指针

Linux shim 里 `out.data()` 指向局部 `std::vector` 的内部缓冲，
函数返回后就悬空了 —— `execve` 读野指针报 "Bad address"。
必须用 `thread_local` 静态缓冲。

### 8. `execle` 的变参布局

`execle(path, arg0, arg1, ..., NULL, envp)` —— NULL 之后还有一个 envp 指针。
少传 NULL，envp 会被当成第 argc+1 个 argv 元素。

### 9. 调试事件里的句柄要关

`CREATE_PROCESS_DEBUG_INFO` 里的 `hProcess` / `hThread` / `hFile`
都由调试器负责关闭。不关的话跑几千个进程就把句柄耗光。

### 10. WOW64 有两个 PEB

这是 32 位目标最隐蔽的坑。

64 位调试器里，一个 32 位目标进程**有两个 PEB**：

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

差 0x1000。**对 PEB64 打补丁，目标完全没反应。**

而且两个 PEB 里的 `RTL_USER_PROCESS_PARAMETERS` 布局不一样
（PEB32 里全是 4 字节指针，字段偏移跟 x64 的 8 字节指针布局对不上），
所以：
- 命令行 / 镜像路径 / 工作目录 → 从 **PEB64** 读
- `BeingDebugged` 补丁 → 打在 **PEB32** 上

### 11. 静态事件的去重不能只按 (pid, exe, argv)

早期版本的去重 key 是 `(pid, executable, argv)`，对静态事件也用同一套。
静态事件的 pid 恒为 0，于是：

```bat
echo same
echo same
echo same
```

三条命令里后两条被当成重复项**静默丢掉** —— 输出只有 2 条。
现在静态事件按 `(脚本路径, 行号, argv)` 区分。

运行时事件也不能只按 `(pid, exe, argv)`：Linux 上 `exec` 不改 pid，
同一 pid 连续 exec 相同命令是 N 次独立执行。现在加了 `exec_index`。

### 12. 引号里的 token 不能丢

PowerShell 静态分析里，词法切分后有一个分支负责处理引号包裹的 token。
那个分支**忘了 `push_back`** —— 结果所有带引号的 token 全部消失：

```powershell
& "C:\Windows\System32\whoami.exe" /all
```
→ 0 个事件（而不是 1 个）

`Write-Output "hello"` → 只剩 `Write-Output`，参数丢了。

这类 bug 的特征是：**输出看起来"少了点东西"，但没有任何报错**。
静态分析器必须用"期望条数"来测，不能只看"有输出"。

### 13. Linux 深度的兜底值写错了

早期版本算深度是：

```cpp
int depth = 1;                          // 兜底
auto it = depth_of.find(e.ppid);
if (it != depth_of.end()) depth = it->second + 1;
```

两个错误叠在一起：

1. **兜底值是 1，应该是 0。** 查不到父进程只可能是根进程这一层。
2. **ccap 自己的 pid 没登记。** 根进程的 `ppid` 是 ccap 的 pid，
   查不到 → 掉回兜底值 → **根进程 exec 出来的每一条都是 depth 1**。

症状：真正嵌套的层级对，但 `make`（把自己 exec 成 echo）显示成
`make(0) → echo(1)` —— 明明是同一层。

修法：把 ccap 的 pid 预先登记为 `-1`，根进程自然算成 0。

### 14. `REM` 注释从来没被识别过

cmd 的 `REM` 是注释（大小写不敏感）。但之前的判断写在"逐字符扫描"过程中：

```cpp
size_t j = i;
while (j > 0 && isspace(line[j-1])) --j;
if (j == 0 && line.compare(0, 3, "rem") == 0) { ... }
```

`j` 是"跳过前导空白后的起始位置"。扫描到第 3 个字符时 `j` 已经等于 3，
**`j == 0` 永远不成立** —— 条件形同虚设。

而且 `compare(0, 3, "rem")` 是大小写敏感的，`REM` 也匹配不上。

症状：`REM === 注释 ===` 出现在命令列表里。

修法：把判断独立出来 —— 先找行首第一个非空白字符，看它是不是 `rem`，
再看后面是不是分隔符（否则 `remote` 是真实命令，不能当注释）。

**Lesson —— "条件形同虚设"和"条件写错"一样危险。**
代码看起来在检查某件事，实际上一百次都不成立。

### 15. PowerShell 动词表不全会把 cmdlet 误判成外部程序

cmdlet 一定是 `Verb-Noun` 形式，所以判定方法很简单：查动词表。

但动词表不全就会误判：

| 命令 | 动词 | 结果 |
|---|---|---|
| `Where-Object` | `Where` | 表里没有 → 被当成外部程序 |
| `ForEach-Object` | `ForEach` | 表里没有 → 被当成外部程序 |

症状：`is_builtin` 是 false，`kind` 是 `command` ——
用户看到"这两个是外部程序"，但它们明明是 cmdlet。

修法：补齐 PowerShell 官方的全部动词。

### 16. 子表达式抽走之后，外层参数不能被截断

处理 `$(...)` 子表达式时，如果把子表达式从行里**抠掉**再走主流程：

```powershell
Write-Output "now is $(Get-Date -Format HH:mm:ss)"
```

会变成 `Write-Output "now is"` —— 参数少了一截。

用户看到的命令跟脚本里写的对不上，这是**错误的**，不是"精度损失"。

正确做法：**原始行照常切词**（参数完整），
子表达式里的命令**额外**单独报出来。两条事件，各司其职。

### 17. `--timeout 2000` 实际跑了 7 秒

超时杀完进程树之后，事件循环跳出，但收尾代码无条件等 5 秒：

```cpp
if (!root_exited) {
    WaitForSingleObject(pi.hProcess, 5000);   // ← 超时路径上不该等这么久
```

用户设了 2 秒超时，却要等 7 秒。这不是"稍微慢一点"，
是**超时参数没被尊重** —— 自动化流程里这会导致连锁超时。

修法：超时路径用 500ms 兜底（`TerminateProcess` 之后几乎立即完成），
非超时路径保持 5000ms（那时候进程确实可能在正常收尾）。

实测：`--timeout 2000` 从 7156ms 降到 2738ms。

### 18. `--summary` 不能追加在 JSON 后面

`--format json --summary` 早期版本把统计文本**追加在 JSON 文档后面**，
结果整个文件不是合法 JSON —— `jq` / `ConvertFrom-Json` 直接报错。

JSON 是**机器消费**的格式，往后面追加任何非 JSON 文本都是破坏契约。

修法：
- JSON 模式 → `summary` 作为一个**顶层 JSON 字段**
- NDJSON 模式 → 不输出（那个格式的约定是"每行一个事件对象"）
- text 模式 → 追加人类可读的文本块

### 19. 内建命令不能报 "resolved" 路径

cmd 的 `echo` 是**内建命令**，根本不创建进程。

但 PATH 里可能真的有个 `echo.exe`（w64devkit / MSYS / Git Bash 都带），
静态分析器解析它就会输出：

```
"resolved": true,
"notes": ["resolved=D:/development/w64devkit/bin/echo.exe"]
```

这会让人以为 `echo` 是个外部程序 —— 与 `is_builtin: true` 自相矛盾。

修法：**内建命令不做路径解析**，`resolved` 恒为 `false`。
内建命令没有"可执行文件路径"这回事。

### 20. UTF-8 BOM 会让前缀匹配静默失效

Windows 上的编辑器、PowerShell 5.1 的 `Set-Content -Encoding UTF8` 都会写 BOM。

BOM 不是**内容**，是编码标记。留着它，所有**基于前缀匹配**的解析都会静默失效：

```cpp
line.rfind("EXECUTING ", 0) == 0     // BOM 在最前面 -> 永远不等
```

症状是「解析出 **0 条**命令」，而不是「解析失败」—— 最坏的一类 bug：
没有任何报错，用户只会觉得「这工具不支持这种构建日志」。

修法放在 `read_file` 里，一处解决所有消费方
（构建日志 / JSON 配置 / 编译数据库 / 捕获事件流）：

```cpp
if (content.size() >= 3 &&
    content[0] == 0xEF && content[1] == 0xBB && content[2] == 0xBF) {
    content.erase(0, 3);
}
```

**Lesson —— 解析器的前缀匹配只应该对「内容」生效。**
编码标记必须在**读进来的时候**剥掉，而不是在每个解析器里各自处理 ——
后者会漏（总有新的消费方），而且症状是静默的。

### 21. 测试自己把源文件移走，软连接就成了断链

Linux 上软连接别名测试一直失败。查下来不是产品有 bug，是测试写错了：

```cpp
bool linked = (std::rename(real.c_str(), alias.c_str()) == 0);  // ← 把源文件移走了
std::string cmd = "ln -sf '" + real + "' '" + alias + "'";      // ← 指向一个不存在的东西
```

`rename` 把 `real` 移成了 `alias`，于是：

- `realpath(real)` 失败 → 退化成路径归一化
- `realpath(alias)` 失败（断链）→ 也退化成路径归一化
- 两边永远匹配不上 → 别名是空的

**Lesson —— 测试里的「准备数据」步骤本身也可能有 bug。**
症状看起来像产品缺陷（"Linux 上软连接别名认不出来"），
但根因在测试。判断方法：**同样的测试在 Windows 上过、Linux 上不过**，
先怀疑平台差异的**成因**是不是测试自己写的。

修法：先建文件，再 `ln -sf` 建软连接，不要动源文件。

### 22. 批量改名把 `posix_spawn` 改成了 `posixSpawn`

LLVM/clang 命名规范重构时，341 个标识符被批量改名。
其中一个是 Linux shim 的 hook 函数：

```cpp
// 改之前（正确）
extern "C" int posix_spawn(pid_t* pid, const char* path, ...);

// 改之后（静默失效）
extern "C" int posixSpawn(pid_t* pid, const char* path, ...);
```

`posix_spawn` 是 **glibc 的动态符号名** —— `LD_PRELOAD` 靠这个名字把符号
"抢过来"。改名之后 shim 导出的是一个**没人调用**的 `posixSpawn`，
而 `posix_spawn` 仍然指向 glibc 的真实现。

症状特别有欺骗性：

```
make 只捕获到根事件，子进程全部丢失
```

因为 `make` 走的是 `posix_spawn` 路径，hook 根本没生效。
**没有任何报错**，shim 正常加载、正常退出，只是没干活。

**Lesson —— ABI 名字永远不能跟着"命名规范"一起改。**
名字本身就是契约：`posix_spawn` / `execve` 是 glibc 导出符号，
`ccap_session_run` 是对外 C ABI，
`PROCESS_BASIC_INFORMATION` 是 ntdll 的结构体布局。
这些是**别人的代码在找的名字**，不是我们的风格问题。

修法：

1. 恢复精确符号名（`posix_spawn` / `posix_spawnp` 等 9 个 exec/spawn 符号）。
2. 加**符号表门禁**：`tests/run_tests_linux_depth.sh` 里断言
   `nm -D --defined-only build-linux/ccap_shim.so` 必须包含那 9 个符号。
   改名 → 门禁立刻变红。

> 门禁的存在意义就在这儿：光靠"跑一遍没报错"，
> 这种静默失效能潜伏很久 —— 因为它只在**特定代码路径**上才显形。

### 23. 编译错误会让命名门禁变成"空检查"

clang-tidy 的 `readability-identifier-naming` 在文件**有编译错误**时，
**根本不输出命名警告**。

```
编译错误 → clang-tidy 跳过命名检查 → 0 条警告 → 门禁显示绿色
```

门禁本来是防跑偏的，结果它自己变成了"什么都没检查但显示通过"。

而且触发这个静默失效的原因往往**不相关**：

- MSVC 的 include 路径不在 `compile_commands.json` 里（来自环境变量）
- 项目自己的 `-I` 是 MSVC 语法（单横杠），clang 不认
- `_WIN32` 没定义 → 代码走 `#else` 分支去 include `poll.h`（Windows 上当然找不到）

**Lesson —— "没报违规" 和 "检查过了没违规" 是两件事。**
门禁必须能区分这两者，否则它给出的绿灯毫无意义。

修法（`tests/check_naming.py`）：

1. **编译错误 → 退出码 1**，并明确打印"命名检查**不完整**"。
2. **缺 clang-tidy → 退出码 2**（不是跳过）。
3. **逐文件执行** —— 一次传多个 TU 时 clang-tidy 会合并编译上下文，
   `--extra-arg` 只对第一个文件生效。

另外还加了一条**自验证**：故意注入一个 `bad_variable_name`，
确认门禁真的会报：

```
FAIL: 1 处命名违规
modules\ccap_core\core\event.cpp:68:9  local variable 'bad_variable_name'
```

一个从没被验证过会红的门禁，跟没有门禁是一回事。

### 27. 门禁的 include 路径"有环境变量就用环境变量" —— 变量指向已卸载版本就全废

这次 clean rebuild 之后，命名门禁在 CTest 里直接红了：

```
modules\ccap\app\main.cpp:14: 'cstdio' file not found
```

根因不是代码变了，是**环境变了**：

| 来源 | 内容 | 状态 |
|---|---|---|
| 环境变量 `INCLUDE` | `MSVC\14.41.34120\include` | **该目录已被卸载**（现存的是 14.42.34433） |
| `vswhere -find VC\Tools\MSVC\**\include` | `14.42.34433\include` | 有效，但**只在 INCLUDE 为空时**才被查 |
| Windows SDK | `D:\Windows Kits\10\Include\10.0.26100.0\...` | 有效，但 `vswhere` **查不到** |

旧逻辑是「INCLUDE 非空就用它，否则去探测安装目录」。这个逻辑在**恰好**的情况下是对的
（INCLUDE 有效），在**危险**的情况下是错的：INCLUDE 非空但**指向一个不存在的目录**，
于是唯一的 MSVC include 路径是死的 → clang-tidy 找不到任何标准头 →
**编译错误 → 命名警告不输出 → 门禁显示"0 处违规"**。

又是坑 23 那个静默失效，只是换了个触发条件。

另外发现 `vswhere` 结构性地**找不到 Windows SDK**：SDK 装在
`C:\Program Files (x86)\Windows Kits\10`，在 VS 安装目录**之外**，
`vswhere -find` 只认识 VS 安装树。SDK 的权威来源是注册表
`HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots` 的 `KitsRoot10`。

**Lesson —— "有环境变量就用环境变量" 是错的默认值。**
环境变量是**可能过期**的缓存；权威来源（安装目录 / 注册表）才是真的。
正确做法是**合并所有来源，只丢弃不存在的路径**。

**推论 —— 门禁自己的输入也要做存在性校验。**
"INCLUDE 没设置" 和 "INCLUDE 指向已删除的目录" 在代码里长得一模一样
（都是"拼不出一个有效路径"），所以修复不能只加一个 `if not INCLUDE` 分支。

**推论 2 —— 结构性的盲区要用不同工具补。**
`vswhere` 查 SDK 是结构性的能力缺失，不是配置问题 ——
换参数没用，得换数据源（注册表）。

修法（`tests/check_naming.py`）：

```python
# 合并所有来源，只丢弃不存在的路径
ambient_usable, ambient_missing = usable_include_dirs(os.environ["INCLUDE"])
discovered_usable, _ = usable_include_dirs(discover_msvc_includes())
inc_dirs = dedupe(ambient_usable + discovered_usable)
```

Windows SDK 改从注册表 `KitsRoot10` 拿，并**取版本号最大的那个** SDK。

现在这个门禁在**没有 vcvars64.bat 环境**的 CTest 里也能跑通了：

```
4/4 Test #4: naming ...........................   Passed  157.80 sec
```

### 28. 树渲染器把节点 move 出去，pid 索引就指向了空壳

旧 `tree.cpp` 在把子节点挂到父节点下时：

```cpp
child = std::move(*it);   // 把节点从 pool 里搬走了
```

节点池是 `std::vector`，`std::move` 之后元素还在 vector 里，
但它已经**变成了 moved-from 状态**。而 pid → 节点的索引表**还指着它**。

结果：**孙进程从树里消失**。4 个原始事件只渲染出 3 个节点。

```
[000001] d=0 RT pid=600 ccap_repeat
[000002] d=0 RT pid=611 echo first        <- 少了一个
```

修法：改成**稳定节点池 + 子节点索引**：

```cpp
std::vector<TreeNode> pool_;              // 节点只增不减
std::vector<std::vector<size_t>> kids_;   // 子节点存**索引**，不是指针
```

这样每个运行时事件**恰好渲染一次**，且不依赖节点在 vector 里的位置。

**Lesson —— 容器里存对象时，`std::move` 会让所有"指向它的索引"失效。**
要么存 `shared_ptr`，要么用索引而不是引用 —— 但**索引只有在对象不搬家时才有效**。

**推论 —— 用"移动"换取的简化，往往要在别处还回去。**
这里原本是想省掉一次拷贝，代价是整棵树的正确性。

**推论 2 —— 回归测试要能抓住"少了一个"这种 bug。**
现在的 `run_tests_linux_depth.sh` 会断言
「4 个原始事件渲染出 4 个节点」。用旧版 buggy 的 `tree.cpp` 跑这个测试，
会稳定失败（渲染 3 个）—— 这才是有效的回归测试。


### 24. 门禁看不见 `#else` 分支里的代码

上面那条门禁一开始是绿的 —— 但代码里确实还有 snake_case 的局部变量。

原因：**clang-tidy 只检查预处理后还存在的东西**。

```cpp
#if defined(_WIN32)
    ... Windows 分支 ...
#else
    size_t name_start = open + 1;   // ← 违规，但 Windows 上这行根本不存在
#endif
```

在 Windows 上跑 clang-tidy，`_WIN32` 分支活下来，`#else` 分支被预处理掉 ——
**那段代码从来没被分析过**。

实测证据（`normalize.cpp`）：

```
把 name_start 改回 nameStart 前后，门禁都是 "0 处违规"
```

而且更隐蔽的是：**MSVC 的 SDK 头会隐式带上 `_WIN32`**，
所以就算你不显式传 `-D_WIN32`，走的还是 Windows 分支。
想覆盖另一侧必须用 `-U_WIN32` 把它**取消**掉。

修法：门禁对每个文件跑**两遍**。

| pass | 参数 | 覆盖 |
|---|---|---|
| native | 默认 | 当前平台的 `#if` 分支 |
| no-win32 | `--extra-arg -U_WIN32` | 另一侧的 `#else` 分支 |

no-win32 pass 里必然有一堆编译错误（那一侧的平台专有头文件在本机不存在），
但那**不影响结论** —— 编译器在报错之前已经把能解析的部分检查过了。
所以：**native pass 的编译错误判失败，no-win32 pass 的只警告**。

验证（故意把 `name_start` 塞回 `#else` 分支）：

```
FAIL: 1 处命名违规
modules\ccap_core\core\normalize.cpp:380:16  local variable 'name_start'  [另一侧分支]
```

**Lesson —— 一个门禁"检查了哪些代码"和"检查了没有"同样重要。**
只报"0 处违规"没有意义，除非你知道它**覆盖了哪些分支**。

### 25. 批量改名把 JSON 键也改了

命名重构时用脚本批量改名。脚本分不清「标识符」和「字符串字面量里的数据契约」，
于是把配置文件的 JSON 键也改了：

```cpp
// 改之前（正确）
"system_includes": { "gcc": ["..."] }

// 改之后（错的）
"systemIncludes": { "gcc": ["..."] }
```

解析器读的是 `"system_includes"` —— 于是配置里的系统头文件路径**静默丢失**。

测试抓到了它（`FAIL systemIncludes 正确`），这是运气好：
那个断言恰好覆盖了这个键。如果配置里有更多键、测试没覆盖到，
这就是一次**静默的功能退化**。

**Lesson —— 改名脚本必须区分「标识符」和「数据契约」。**
JSON 键、命令行选项、输出字段名、文件路径 —— 这些**不是标识符**，
它们是**接口**，改了就是破坏兼容。

验证手段：改名前后**逐文件 diff 所有字符串字面量**，确认只有 include 路径变了：

```python
old_lits = extract_literals(backup_file)
new_lits = extract_literals(current_file)
lost = old_lits - new_lits
```

这次 diff 出来的唯一一处丢失，就是那个 JSON 键。

### 26. 用「豁免」掩盖「没改」

改名时发现内部结构体 `RunOptions` 的字段还是 snake_case
（`timeout_ms` / `quiet_ms` / `clear_being_debugged` / `redirect_output_to_stderr`）。

最快的处理是往 `.clang-tidy` 的豁免列表里加一条 —— 门禁立刻变绿。

但这是**用豁免掩盖没改**。那几个字段根本不是 ABI：

| 结构体 | 是不是 ABI | 该用什么风格 |
|---|---|---|
| `ccap_run_options`（C ABI） | ✅ 是，对外契约 | snake_case |
| `ccap::RunOptions`（内部 C++） | ❌ 不是 | camelBack |
| `ccap::cli::Options`（内部 C++） | ❌ 不是 | camelBack |

而且豁免列表是**按名字**匹配的 —— `timeout_ms` 在豁免列表里，
就意味着**任何**叫 `timeout_ms` 的字段都豁免，包括内部那些。
一个字段名同时出现在 ABI 和内部结构体里时，豁免根本区分不了。

正确做法：把内部字段**真的改成** camelBack，豁免列表只留 ABI 那些。

**Lesson —— 一个豁免规则要能回答："我为什么豁免这个？"
答不上来，就是它在替你掩盖问题。**

---

## 已知缺口

| 缺口 | 状态 | 说明 |
|---|---|---|
| PowerShell 静态分析不是完整 parser | 仍然存在 | 我们做的是词法切分 + 子表达式提取，不是真正的语法树。`& { }` 动态块仍然解析不了 |
| `Invoke-Expression` 只认字面量 | 部分完成 | `Invoke-Expression "git status"` / `iex "npm install"` 里的实际命令**会**被提取。`Invoke-Expression $cmd` 只报 cmdlet 本身 —— 变量形式不猜，避免伪命令 |
| PowerShell 动态构造的命令名 | 仍然存在 | `$cmd = "git"; & $cmd` 只能看到变量名，看不到实际命令 |
| cmd `for` 循环的迭代列表 | 已修复 | 括号里装的是**迭代列表**不是命令。老实现把 `for %i in (a b) do echo loop` 报成 `a b` 而丢掉 `echo loop`。现在解析 `do` 之后的命令体 |
| 脚本里的变量展开 | 仍然存在 | `cd %MYDIR%` 报的是**未展开**的原文。要得到实际目录需要建模环境变量 |
| 脚本里的循环展开 | 部分完成 | `for %i in (1 2 3) do cmd` 报**一次** `cmd`，不报 3 次。语义上够用（命令是什么是对的），但次数不准 |
| `CheckRemoteDebuggerPresent()` | 无法修复 | 查内核的调试对象列表，不是用户态标志。`--clear-being-debugged` 只能盖住另外两种检测方式 |
| Windows `BeingDebugged=1` | 已缓解 | `--clear-being-debugged` 可清零（默认关闭） |
| 首次异常由调试器吞掉 | 已改善 | 现在只对**每个线程的第一个**断点用 `DBG_CONTINUE`（那是 loader 放的初始断点），程序自己抛的断点交还目标 |
| 调试器退出连带终止目标 | 基本没有 | 正常路径等所有子进程退出才返回；强制中断时可能发生 |
| Linux setuid 程序 | 内核限制 | 内核会丢弃 `LD_PRELOAD`。**会在 `notes` 里明确报出来** |
| Linux 静态链接程序 | 内核限制 | 不走动态链接器，shim 完全失效。**会在 `notes` 里明确报出来** |
| Linux 静态链接的中间产物 | 已缓解 | 早期只诊断根目标，中间产物是盲区。现在 shim **逐个 exec** 检查 ELF 并标记 |
| `__execve` / `__posix_spawn` 内部符号 | 不适用 | 现代 glibc **不导出**这两个符号（`nm -D` 查不到），按名字 hook 是不可能的。`posix_spawn`/`execve` 本身已覆盖，实测 make/cmake/gcc 全链路都抓到了 |
| 巨量事件去重 | 已改善 | 静态事件按 (脚本, 行号) 区分，运行时事件按 (pid, 可执行, argv, exec 序号) 区分。同一 pid 连续 exec 相同命令不会被合并 |
| 32 位目标 | 已验证 | 真 32 位 exe 的命令行能完整读出。**注意 WOW64 有两个 PEB**（见下） |
| 事件去重后 `seq` 不连续 | 仍然存在 | 被去重掉的事件不占 seq，所以 seq 是"输出序列号"不是"原始事件号" |
| 捕获完整性判断 | 已实现 | 每次采集结束在 `notes` 里给出 `COMPLETE` / `PARTIAL` 结论，并说明原因 |
| 树视图里的中间产物 | 仍然存在 | Linux 上同一 pid 多次 exec 会渲染成兄弟节点（体现"进程替换"），但和 Windows 的父子树不完全对称 |
| 树渲染丢节点 | 已修复 | 旧实现把子节点 `std::move` 出 pool，pid 索引仍指向 moved-from 节点 —— **孙进程从树里消失**。现在用稳定节点池 + 子节点索引，每个运行时事件恰好渲染一次 |
| 文件带 BOM | 已修复 | `read_file` 统一剥 UTF-8 BOM。之前带 BOM 的构建日志会**静默解析出 0 条命令** |
| `ccap_compdb` 的 C ABI | 不适用 | 只有 `ccap_capture` 对外提供 C ABI。`ccap_compdb` 是 C++ 库，共享库仅供同编译器使用 |
| 命名门禁未覆盖 Linux 专属文件 | 部分完成 | Windows 上跳过 `linux_preload.cpp` / `linux_shim.cpp`（本平台编译不了）。**需要在装了 clang-tidy 的 Linux 环境补跑** `python3 tests/check_naming.py` |
| 命名门禁未接入 CTest | 已修复 | 已作为 `naming` 测试纳入 CTest。缺 clang-tidy 时退出码 2 → CTest 标记为 **skipped**（不永久红灯），但日志明确显示未检查 |
| 命名门禁的 MSVC include 路径靠环境变量 | 已修复 | 旧逻辑「INCLUDE 非空就用它」在 INCLUDE 指向**已卸载的工具集版本**时会静默失效 —— clang-tidy 找不到任何标准头，命名警告根本不输出，门禁变成空检查。现在**合并**环境变量与安装目录/注册表探测到的路径，只丢弃**不存在**的 |
| 命名门禁的双分支 pass 在 Windows 上有 40 个预期编译错误 | 已知 | `-U_WIN32` 那侧的平台专有头文件在本机不存在。这些**不影响结论**，脚本只记 note 不判失败 |
| clang 翻译的有损选项 | 已报出 | MSVC 的 `/EHsc` / `/MD` 等没有 clang 等价物。**不会静默丢掉** —— `clangTranslationLosses()` 逐条报出来，运行时库那类影响 ABI 的会单独标出 |
| clang 翻译不处理 MSVC 的 `/external:I` 之外的 exotic 选项 | 部分完成 | `/external:I` / `/imsvc` → `-isystem`，`/FI` → `-include`，`/TP` `/Tp` `/TC` → 驱动选择，都已处理。**仍有**罕见选项（如 `/analyze`）按原样透传并在损失报告里列出 |
| clang 翻译的驱动选择 | 已修复 | 原本只按扩展名选驱动。MSVC 的 `/TP`（`.c` 按 C++ 编译）会选错驱动 —— 而 `clang++` 链接 libstdc++，**会改变 ABI**。现在按编译器显式指定的语言来选 |
| 构建工具覆盖只到「驱动」层 | 已知 | 测的是 cmake / ninja / make / msbuild / cl.exe 这几个**驱动**怎么拉起编译器。Qt 这类上层构建系统没有装，所以它的 qmake/moc/uic 链路没测 —— 但它最终也是调编译器，落到同一层 |


---

## 测试

```powershell
# Windows 回归测试（100 项）
powershell -File tests\run_tests.ps1

# Windows 编译命令端到端测试（29 项）
powershell -File tests\run_tests_compdb.ps1

# 单元测试 + C ABI 验证 + 命名门禁 + 协议头门禁（5 项，约 160 秒）
ctest --test-dir build -C Release --output-on-failure

# 命名规范门禁（也可以单独跑）
python tests\check_naming.py

# 协议头门禁（也可以单独跑；--fix 会自动补上缺失的头）
python tests\check_license.py
```

```bash
# Linux 回归测试
bash tests/run_tests_linux.sh              # cmake / make 全链路
bash tests/run_tests_linux_extra.sh       # execveat / posix_spawn / 静态链接诊断
bash tests/run_tests_linux_dedup.sh       # 去重不能丢事件
bash tests/run_tests_linux_stdout.sh      # --stdout-to-stderr
bash tests/run_tests_linux_depth.sh       # 深度 / 根事件 / 超时 / 完整性 / 树渲染 / shim ABI 符号（33 项）
bash tests/run_tests_compdb.sh            # 编译命令端到端（25 项）

# 或者一次跑完全部
bash tests/run_all_linux_suites.sh

# Linux 上的命名规范门禁（需要 clang-tidy）
python3 tests/check_naming.py
```

当前结果：

| 套件 | 结果 |
|---|---|
| Windows 回归 `run_tests.ps1` | 100/100 |
| Windows 编译命令 `run_tests_compdb.ps1` | 29/29 |
| Windows 单元测试 `test_compdb.exe` | 163/163 |
| Windows 单元测试 `test_cmdline.exe` | ALL PASSED |
| Windows CTest（含 C ABI + 命名门禁 + 协议头门禁） | 5/5 |
| Windows 命名门禁 `check_naming.py` | 0 处违规（29 个文件 × 2 个分支 pass） |
| Windows 构建 | 零警告 |
| Linux 回归 `run_tests_linux.sh` | 退出码 0 |
| Linux `run_tests_linux_extra.sh` | 退出码 0 |
| Linux `run_tests_linux_dedup.sh` | 退出码 0 |
| Linux `run_tests_linux_stdout.sh` | 退出码 0 |
| Linux 深度 `run_tests_linux_depth.sh` | 33/33 |
| Linux 编译命令 `run_tests_compdb.sh` | 25/25 |
| Linux 单元测试 `test_compdb` | 165/165 |
| Linux CTest | 5/5（`naming` 因本机没装 clang-tidy 标记为 skipped） |
| Linux 构建 | 零警告 |
| shim ABI 符号（`nm -D`） | 9/9 |
| DLL 导出符号（`dumpbin /exports`） | 8/8 |

> Windows 单元测试比 Linux 少 2 项：软连接别名那两条断言需要 `ln`，
> Windows 上会**显式跳过**（`skip 软连接测试（ln 失败）`）而不是假装通过。

> `check_naming.py` 在 Linux 上会检查 `linux_preload.cpp` / `linux_shim.cpp`，
> Windows 上这两个文件会被**显式跳过**（打印 `note: 跳过 2 个非本平台的文件`），
> 而不是假装检查过。

`tests/probe/` 下是调试期用的一次性探针程序（`DbgTree*.cpp`、`DbgPeb.cpp`），
用来定位上面列的那些坑，保留下来是为了让结论可复现。

它们默认**不参与构建**，避免拖慢主构建。需要时显式开启：

```bash
cmake -S . -B build -DCCAP_BUILD_PROBES=ON
```

> `tests/run_all_linux_suites.sh` 是把 6 个 Linux 套件串起来跑的总入口。
> 写成文件而不是 `wsl -e bash -lc "..."` 的内联脚本，是因为从 PowerShell 里
> 传多行 bash 脚本，引号嵌套很容易被吃掉 —— 而且失败时它会把**全文**打出来，
> 不是只有 tail。

---

## 相关文档

- [`docs/RESEARCH-COMMAND-CAPTURE-2026-10-27.md`](docs/RESEARCH-COMMAND-CAPTURE-2026-10-27.md)
  —— 方案调研：Windows Detours / ETW / 调试器 API / Linux LD_PRELOAD / seccomp 的对比，
  以及为什么最终选了「调试器 API + LD_PRELOAD + 静态分析」这个组合。
- [`docs/DESIGN-MODULES-2026-10-27.md`](docs/DESIGN-MODULES-2026-10-27.md)
  —— 模块化设计：四个模块的职责边界、依赖方向、C ABI 契约、compdb 架构。
- [`docs/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md`](docs/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md)
  —— Windows 采集原理：`DEBUG_PROCESS` 调试器 API 的机制、PEB 读取、
  WOW64 双 PEB，以及**为什么不用 Detours / ETW / WMI** 的逐项对比。
- [`docs/NAMING-CONVENTION.md`](docs/NAMING-CONVENTION.md)
  —— 命名规范（参考 LLVM / clang）：各类标识符的风格、C API 为什么保持 snake_case、
  以及哪些名字**不能**跟着风格一起改（ABI 契约）。
- [`prototypes/windows-debugproc/`](prototypes/windows-debugproc/)
  —— C# 机制验证原型（证明非管理员环境下 `DEBUG_PROCESS` 能捕获三层进程树）。
