# CommandCapture

<!-- i18n:begin -->
中文 | [**English**](README.md)
<!-- i18n:end -->

Captures **every command spawned directly or indirectly by a script (or executable)** and outputs a structured process tree.

Cross-platform (Windows / Linux), zero third-party dependencies, single-file executable.

**License: [MIT](LICENSE)** -- Copyright (c) 2026 maijun.
Every source file carries the `SPDX-License-Identifier: MIT` tag in its header.

---

## Why two layers of capture are needed

No single process-level mechanism can get all the commands:

| Command type | Example | Can a process-level mechanism see it? |
|---|---|---|
| External program | `whoami`, `gcc`, `cmake` | ✅ creates a process |
| shell builtin | `echo`, `cd`, `set`, `for`, `if` | ❌ **does not create a process** |
| Both sides of a pipeline | `a \| b` | ✅ but there is a `cmd.exe /c` from the shell itself in between |
| Build-system wrapper | `ninja` → `cmd.exe /C cd. && cl...` | ✅ requires following each layer |

So CommandCapture is **two layers**:

1. **runtime layer** -- events where a process was really created
 - Windows: `CreateProcessW(..., DEBUG_PROCESS)` debugger API, captures the entire process tree
 - Linux: `LD_PRELOAD` injected shim, intercepting the `execve` family and the `posix_spawn` family
2. **static analysis layer** -- scans script contents, capturing shell builtins
 - Supports `.bat` / `.cmd` / `.sh` / `.ps1`

Both layers emit events in a unified structure, distinguished by the `source` field.

### Why a command like `cd` cannot be hooked

Because **it is not a process at all**.

The semantics of `cd` are to change the working directory of the **current process**, and the working directory is a **per-process** attribute --
a child process changing its own copy has no effect on the parent. So `cd` can only be implemented by the shell **inside its own process**:

```
shell (cwd = /home/me)
 └─ fork → 子进程执行 "cd /tmp" → 子进程 cwd 变了 → 子进程退出
shell (cwd = /home/me) ← 什么都没发生
```

If `cd` were an external program, changing the directory would be pointless anyway. **This is a semantic constraint, not a performance choice** --
which is why every shell must implement it as a shell builtin. `exit` (it has to terminate the shell itself) and
`export` (it has to modify the shell's own environment variables) are the same story.

Measured (Windows 10, local machine):

```bat
@echo off
cd C:\Windows
echo hello
where.exe cmd
```

```
$ ccap run mixed.bat --no-scan --format text
[000001] d=0 RT pid=16684 cmd.exe /c.../mixed.bat
[000002] d=1 RT pid=19044 where.exe cmd ← 外部程序: 有进程
```

`cd` and `echo` produce no events at all -- because they did not create any process.
They only show up once the static analysis layer is added, and they are then tagged `ST` (`pid=-` means "no process"):

```
[000002] d=0 ST pid=- cd C:\Windows [mixed.bat:2] (builtin)
```

**So "being able to get `cd`" and "being able to catch processes" are two different abilities:**
the former requires **reading the script source** (static analysis), the latter requires **watching processes** (hook/debugger).
Any tool that wants to cover shell builtins must have a static analysis layer.

For the full derivation and its boundaries (variable expansion, `for` loops, dynamic construction) see
[`docs/en/RESEARCH-COMMAND-CAPTURE-2026-10-27.md`](docs/en/RESEARCH-COMMAND-CAPTURE-2026-10-27.md) §4.0.

---

## Quick start

### Windows

```powershell
# 用 MSVC 编译

cmake -G "Visual Studio 17 2022" -A x64 -S. -B build
cmake --build build --config Release

# 跑一个构建脚本
.\build\Release\ccap.exe run build.bat --format text
```

### Linux

```bash
cmake -S. -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

./build/ccap run./build.sh --format text
```

> Linux needs `ccap_shim.so` and `ccap` in the same directory (or use `CCAP_SHIM` to specify the path).

---

## Command line

```
ccap run <script|exe> [args...] 运行目标并捕获整棵进程树
ccap scan <script> 仅做脚本静态分析 (捕获内建命令) 

选项:
 --format <json|ndjson|text> 输出格式 (默认 json) 
 --out <file> 写入文件而非 stdout
 --cwd <dir> 目标工作目录
 --no-scan 关闭脚本静态分析
 --no-recursive 静态分析不递归进被调用脚本
 --depth <n> 静态分析递归深度 (默认 4) 
 --timeout <ms> 运行超时
 --quiet-ms <ms> 根进程退出后, 等待残留子进程的静默期
 (默认 300; 这是安全网, 不是结束判据) 
 --exclude <name> 排除可执行文件名 (可重复) 
 --exclude-str <text> 排除命令行包含该子串的事件 (可重复) 
 --include <name> 只保留该可执行文件名 (可重复; 白名单) 
 --tree 输出进程树视图 (缩进) 
 --summary 追加一行汇总统计
 --clear-being-debugged 清掉目标 PEB 的 BeingDebugged 标志
 (Windows 专用; 见下) 
 --stdout-to-stderr 把目标的 stdout 转到 stderr
 (ccap 自己的 stdout 只留采集数据, 
 这样 --format json 可以直接管道消费) 
 --verbose 额外输出诊断信息到 stderr
```

### Filtering events

A full build produces hundreds of events (`as` / `cc1plus` / `collect2` / `ld`...),
of which the overwhelming majority are the compiler's internal steps.

```bash
ccap run build.bat --exclude cc1plus.exe --exclude as.exe
ccap run build.bat --include git --include npm # 只看白名单
```

**Nothing is filtered by default** -- silently dropping events is the worst failure mode.
Filtered-out entries are **counted and written into `notes`**, so you know how many were lost:

```
"notes": ["filtered out 47 event(s) by --exclude/--include (kept 12 of 59)"]
```

### Process tree view

```bash
ccap run build.bat --tree
```

```
+- cmd.exe /c D:/work/build.bat [pid=13596 ppid=2992 d=0]
 +- whoami [pid=18660 ppid=13596 d=1]
 +- cmd.exe /S /D /c echo piped [pid=20448 ppid=13596 d=1]
 +- findstr piped [pid=19720 ppid=13596 d=1]
 +- cmd /c D:\work\level3.bat [pid=19976 ppid=13596 d=1]

static (script builtins, no process created):
 +- echo hello from echo [D:\work\build.bat:3]
```

Static events have no process relationship, so they are listed separately at the end.

### Summary statistics

```bash
ccap run build.bat --summary
```

```
--- summary ---
runtime events: 9
static events: 16
max depth: 2
unique executables: 6
 cmd: 2
 cmd.exe: 2
 findstr: 1
 hostname: 2
 where: 1
 whoami: 1
```

**In JSON mode the summary is a top-level field**, not text appended at the end:

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
 "events": [... ]
}
```

> JSON / NDJSON are formats meant for **machine consumption** -- appending any non-JSON text at the end makes
> `jq` / `ConvertFrom-Json` fail outright. So in JSON mode the summary is carried by a field,
> and in NDJSON mode it is simply not emitted (the convention of that format is "one event object per line").

### Capture completeness

At the end of every capture run, `notes` contains a conclusion that answers
"are the things you are seeing all of them?":

```
"capture completeness: COMPLETE for process creation (DEBUG_PROCESS sees every
 descendant); script builtins are covered by the static analysis layer"
```

When loss is known, it explicitly says `PARTIAL` and explains why
(timeout truncation / statically linked intermediates).

### Consuming the output as machine data

The target program's own output is by default mixed into stdout (just as if you ran it directly).
When you want to pipe JSON, add `--stdout-to-stderr`:

```bash
ccap run build.sh --format json --stdout-to-stderr | jq '.events[] |.executable'
```

### When observed by an anti-debugging program

`DEBUG_PROCESS` inevitably makes the target see `PEB.BeingDebugged == 1`.
Adding `--clear-being-debugged` clears it:

| Detection method | Effect |
|---|---|
| `PEB.BeingDebugged` | ✅ cleared to zero |
| `IsDebuggerPresent()` | ✅ returns 0 (it reads the same flag) |
| `CheckRemoteDebuggerPresent()` | ❌ still 1 (it queries the kernel's debug object list, which cannot be patched) |

**Disabled by default**: silently changing the behavior of an observed program is bad practice.
Turn it on only when you know for sure that the target would change behavior because of it.

---

## Compile command tooling (`ccap compdb`)

`ccap compdb` turns "a pile of command lines" into "structured compile commands", and finally exports them as
clang's `compile_commands.json` (or an analyzer-style translated command).

It **does not care where the commands came from** -- it only consumes "command line text + working directory". So all four of the following sources work:

| Source | How to feed it in |
|---|---|
| The event stream captured by ccap itself | `ccap compdb export events.ndjson` |
| Plain text (one command per line) | `ccap compdb export commands.txt` |
| Build log `build-log.txt` | `ccap compdb export build-log.txt` |
| An existing `compile_commands.json` | `ccap compdb export compile_commands.json` |

The format is **detected automatically** -- you don't have to tell it.

### Subcommands

```
ccap compdb list 列出编译器类型全量表
ccap compdb aliases <编译器...> 查某个编译器的所有别名 (软连接互认) 
ccap compdb parse <文件...> 解析并统计
ccap compdb show <文件...> 展示解析出的编译命令详情
ccap compdb export <文件...> 生成 compile_commands.json (或 clang 命令) 
ccap compdb translate <文件...> 生成分析器风格的翻译命令
ccap compdb capture <构建命令...> 跑一次构建并抓编译命令

compdb 选项:
 --config <file> 配置文件 (JSON) 
 --output <file> 输出文件 (默认 compile_commands.json) 
 --output-format <clang|clang-cmd|translated>
 输出格式 (默认 clang) 
 clang compile_commands.json, 原样保留
 clang-cmd 每行一条真正的 clang 命令
 translated 分析器风格的参数
 --keep-scripts 保留脚本命令 (默认过滤掉) 
 --all show 时不过滤 (同时标注 KEEP/DROP) 
 --exclude <name> 额外排除 (可重复) 
 --include <name> 白名单 (可重复) 
```

### Recognition: the full compiler table

`ccap compdb list` prints an **extensible rule table**, not if-else branches scattered through the code:

```
Known compiler types:

 gcc (id: gcc)
 names: gcc g++ cc c++ cpp gcc-cc gpp
 gcc (id: gcc-versioned)
 clang (id: clang)
 names: clang clang++ clang-cl clang-cpp
 msvc (id: msvc)
 names: cl
 iar (id: iar)
 names: iccarm iccarm.exe iarc ilink
 generic (id: generic)

Versioned names are recognized too: gcc-11, g++-12, clang-15...
```

The `gcc-versioned` rule only has a suffix, no exact name -- it is responsible for **versioned** names
like `gcc-11` / `g++-12`.

The matching order is: **exact name → prefix rule → suffix rule**.

Prefix rules exist because of cross-compilers -- the basename of `arm-none-eabi-g++` is `g++`,
but it is **not the same compiler** as the native `g++`, and the two must not be conflated.

### Aliases: recognizing each other through symlink real paths

Reality on a real machine:

```
/usr/bin/gcc -> /usr/bin/gcc-11 (软连接)
/usr/bin/cc -> /usr/bin/gcc
/usr/bin/c++ -> /usr/bin/g++
```

`ccap compdb aliases /usr/bin/cc` resolves the **real path**, then groups all names under the same real path --
the real command and the symlink command are **aliases of each other**:

```
/usr/bin/cc
 real path: /usr/bin/x86_64-linux-gnu-gcc-11
 kind: gcc
 rule: gcc
 aliases: /usr/bin/gcc /usr/bin/gcc-11 /usr/bin/x86_64-linux-gnu-gcc /usr/bin/x86_64-linux-gnu-gcc-11
```

This is a **deterministic fact** at the filesystem level, not a guess -- so it is stricter and more reliable
than "matching exactly by name".

(The real path is not necessarily `/usr/bin/gcc-11`: on Debian/Ubuntu gcc is a **two-stage** symlink,
`/usr/bin/gcc` → `/usr/bin/x86_64-linux-gnu-gcc-11`. This is exactly why the **real path must be resolved**
instead of guessing by name.)

Aliases are scanned by **reading the directory**: take the directory of a known compiler, list all entries in
that directory, resolve each one's real path and compare. So only the directories of **the compilers you care about**
are scanned -- there is no full-disk PATH scan (a full-disk scan can be very slow on CI machines).

### Filtering: keeping only real compile commands

By default, **scripts, linkers, assemblers and archivers** are filtered out -- only real compilation invocations are kept.

Filtering and "script detection" are **two independent mechanisms**:

- Script detection = "is this name a shell interpreter" (`sh` / `bash` / `cmd.exe` / `powershell.exe`...)
- Exclusion list = a user-configured name blacklist

So `--keep-scripts` puts script commands back; it does not remove them from a blacklist.

### Extraction: headers, macros, source files

`ccap compdb show` breaks a compile command apart:

```
[DROP] make all
 dropped: script (no compilation happens)
[KEEP] g++ -c -o property.o -I. -Igenerators -DQT_NO_TEXTCODEC property.cpp
 kind: gcc
 directory: /export/home/acc/qt/qmake
 sources: /export/home/acc/qt/qmake/property.cpp
 includes:. generators
 defines: QT_NO_TEXTCODEC
 output: property.o
```

Filtered-out commands are shown too, **together with the reason they were dropped** -- so you can confirm
that "what was filtered out was a script" rather than "it wasn't recognized".

Supported extraction items:

| Category | GCC/Clang | MSVC |
|---|---|---|
| Include path | `-I` | `/I` |
| System include path | `-isystem` | `/external:I` |
| Macro definition | `-D` | `/D` |
| Language standard | `-std=` | `/std:` |
| Source file | positional argument | positional argument |
| Output file | `-o` | `/Fe` `/Fo` |
| Forced include | `-include` | `/FI` |
| Explicit language | -- | `/TP` `/Tp<file>` `/TC` |

> **Wrapper prefixes in build logs are stripped automatically.**
> In the build logs of static analysis tools, the compiler is often wrapped in another layer:
> ```
> COMPILING '/opt/bin/sa-translate g++ -c -o property.o property.cpp '
> ```
> The real compiler is the **second** token. Without stripping it, the compiler is recognized as the wrapper's name
> -- a compiler that doesn't even exist -- so the kind cannot be determined and every argument is parsed wrong.
>
> Only **known** wrapper names are stripped (`sa-*` / `tool-*` / `sast-*`),
> not arbitrary names with the same prefix -- what if the user really has a similarly named tool?
> The names are a **tool-agnostic generic form**, not tied to any vendor's spelling.

### Export: the clang compilation database

```bash
ccap compdb export build-log.txt --output compile_commands.json
```

The output follows the [clang JSON Compilation Database](https://clang.llvm.org/docs/JSONCompilationDatabase.html) specification,
and uses `arguments` rather than `command` -- the former does not require handling command line escaping, consumers support both,
but `arguments` is less error-prone:

```json
[
 {
 "directory": "/export/home/acc/qt/qmake",
 "arguments": ["g++", "-c", "-o", "property.o", "-I.", "-Igenerators", "-DQT_NO_TEXTCODEC", "property.cpp"],
 "file": "/export/home/acc/qt/qmake/property.cpp"
 }
]
```

### Export: analyzer-style translated commands

```bash
ccap compdb translate build-log.txt --output-format translated
```

The output is the **translated arguments**, one per line:

```
g++ -I. -I generators -DQT_NO_TEXTCODEC /export/home/acc/qt/qmake/property.cpp
```

Conventions:

- **No front-end prefix is added for you** -- the prefix is decided by the caller
 (any static analysis front end will do). The tool is responsible for **argument translation**.
- System headers use `--sys_include <dir>` (the analyzer front end's option name),
 not gcc's own `-isystem` -- the target is the **analyzer's** internal representation, not gcc.
- Macros use `-D<name>[=<val>]`, include paths use `-I <dir>`.

Argument handling is **dispatched per compiler**: GCC/Clang use `-I` / `-D`, MSVC uses `/I` / `/D`,
which avoids syntactic pollution. The adapters are **independent of each other**; adding a new compiler = adding one file,
and no other file has to change.

### Export: real clang commands (`clang-cmd`)

This is the ability to **rewrite the original compile command as a clang command** -- used for "replaying a build with clang".

```bash
ccap compdb export build-log.txt --output-format clang-cmd
```

The input is MSVC:

```
cl.exe /c /I"..\inc" /DWIN32 /DVERSION=2 /std:c++17 /EHsc /Zi /O2 /Fo"x64\main.obj" /MD main.cpp
```

The output is clang:

```
clang++ -I..\inc -DWIN32 -DVERSION=2 -std=c++17 -g -O2 -c -o x64\main.obj main.cpp
```

The corresponding translation rules:

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

The driver is chosen by source file type: `.cpp` / `.cc` / `.cxx` → `clang++`, everything else → `clang`.
(Linking C++ object files requires the C++ driver, which brings in libstdc++ / libc++.)

**But the extension is not the only criterion.** MSVC can specify the language directly:

| MSVC | Meaning | clang |
|---|---|---|
| `/TP` | compile all following `.c` as C++ | switch the driver to `clang++` |
| `/Tp<file>` | compile the given file as C++ | treat that file as C++ |
| `/TC` | compile all following `.c` as C | switch the driver to `clang` |

This one matters: picking the wrong driver **changes the ABI** --
`clang++` links libstdc++, `clang` does not.
A file named `foo.c` but compiled with `/TP`, if the driver is chosen by extension,
produces a translated command that yields a **different object file** than the original build.

```bash
$ ccap compdb export build-log.txt --output-format clang-cmd
clang++ -include prelude.h -c foo.c # /TP /FIprelude.h
clang -include prelude.h -c bar.c # gcc -include prelude.h
```

`/FI` (forced include) likewise must not be dropped -- a forced-include header defines macros,
and macros change **which code gets compiled**; dropping it means compiling something else entirely.

**Translation is lossy, and it says so.** Options with no clang equivalent are **not silently dropped**:

```
$ ccap compdb show build-log.txt
...
notes: no clang equivalent for /EHsc
notes: runtime library selection /MD was dropped; the ABI may differ from the original build
```

The runtime library (`/MD` / `/MT`) line is flagged specially -- it affects the **ABI**,
not just a switch; dropping it may show up as a link error rather than a compile error.

The differences between the three output formats:

| Format | Meaning |
|---|---|
| `clang` | `compile_commands.json`, keeping the original argv **as-is** -- a faithful record of this build |
| `clang-cmd` | one **real clang command** per line -- replay this build with clang |
| `translated` | analyzer-style arguments (`--sys_include` etc.) |

### Configuration

The configuration file is JSON and **optional**. Without it, the built-in defaults are used and everything works as usual.

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

The three principles of configuration (inspired by the configuration thinking of industry static analysis tools):

1. **The configuration is optional** -- without it, the built-in defaults are used.
2. **The configuration only adds** -- user-configured rules are **appended** to the built-in rules, not substituted.
 Configuring an `arm-none-eabi-gcc` does not make you lose `gcc`.
3. **The configuration does not affect recognition, only filtering and output** -- recognition relies on compiler names (filesystem facts),
 and the configuration tells the system "which ones count as what I want" and "where the system headers are".

> When parsing a remote build log, the local PATH is **never** used to complete remote compiler paths --
> otherwise the local `g++` would be mismatched onto the remote record. `compiler_path` is only recorded
> when the command line itself carries an absolute path.

---

## C ABI (shared library)

The capture capability is exposed through a **stable C ABI**:

- Header: `modules/ccap_capture/include/ccap_capture.h`
- Windows: `ccap_capture.dll` (+ import lib)
- Linux: `libccap_capture.so`

**Why a C ABI instead of exposing C++ directly**: the C++ ABI is not stable across compilers / versions --
the same header compiled with MSVC and with GCC produces different symbol names and object layouts.
Once a shared library is meant to be used by bindings in multiple languages, it has to go through a C ABI.

### Interface

```c
ccap_session* ccap_session_create(void);
void ccap_session_destroy(ccap_session* session);
int ccap_session_run(ccap_session* session,
 const char* target,
 const char* const* args, size_t arg_count,
 const ccap_run_options* options,
 ccap_event_callback on_event,
 ccap_note_callback on_note,
 void* user);
int ccap_session_scan(ccap_session* session,
 const char* script_path,
 const ccap_run_options* options,
 ccap_event_callback on_event,
 ccap_note_callback on_note,
 void* user);

void ccap_run_options_init(ccap_run_options* options);
ccap_platform ccap_current_platform(void);
const char* ccap_platform_name(ccap_platform platform);
const char* ccap_version(void);
```

`on_event` is called once per event received, with one line of JSON (**without** a trailing newline).
After the callback returns, ccap reuses that memory -- copy it yourself if you want to keep it.

### Usage (C)

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

`tests/test_capi.c` is the complete version of this program -- it **deliberately links only the shared library**,
not the `ccap_capture` static library. Only that way can you truly verify the exported symbols, the self-containment of the C ABI,
and that external programs can receive event callbacks.

### Usage (Python ctypes)

Verified working (validated on both Windows / Linux):

```python
import ctypes

lib = ctypes.CDLL("./build/shared/Release/ccap_capture.dll") # Linux:./build-linux/shared/libccap_capture.so
lib.ccap_version.restype = ctypes.c_char_p
lib.ccap_platform_name.restype = ctypes.c_char_p

print(lib.ccap_version().decode()) # b'1.0.0'
print(lib.ccap_platform_name(lib.ccap_current_platform()).decode()) # b'windows'
```

---

## Output formats

### `--format text` (for humans)

```
[000017] d=0 RT pid=9500 cmd.exe /c D:/work/.../build.bat
[000018] d=1 RT pid=14476 whoami
[000019] d=1 RT pid=2864 where cmd.exe
[000023] d=1 RT pid=11816 cmd /c D:\work\.../level3.bat
[000024] d=2 RT pid=14452 hostname
[000004] d=0 ST pid=- echo hello from echo [D:\work\...\build.bat:3] (builtin)
```

- `d=N` -- the level relative to the root process
- `RT` / `ST` -- runtime / static
- `ST` lines carry the script path and the line number

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

`exec_index` is the n-th occurrence of the same `(pid, executable, argv)` combination.
On Linux `exec` doesn't change the pid, so repeatedly exec'ing the same command with the same pid is N independent executions --
this field distinguishes them so they are not wrongly merged.

### PowerShell

PowerShell has its own syntax, so it gets a dedicated scanner
(`modules/ccap_core/core/powershell_scanner.cpp`):

| Feature | Handling |
|---|---|
| Aliases `ls` / `dir` / `cat` | expanded into canonical names, tagged `alias_of=` |
| Cmdlet `Get-ChildItem` | recognized as `kind: "cmdlet"` |
| Pipeline `a \| b` | split into multiple commands along the pipe |
| Script block `{ Get-Date }` | recursed into |
| Call operator `& "prog.exe"` / `. "s.ps1"` | operator stripped, the real target preserved |
| **Subexpression `$(Get-Date)`** | **the inner command is extracted and reported separately** (outer arguments stay intact) |
| **Nested subexpression `$((Get-Date).Year)`** | extracted recursively |
| Variables `$env:PATH` / `$x = 1` | **not commands**, excluded |
| Comments `#` / `<# #>` | excluded |
| `param($x)` | not a command, excluded |
| `cmd(argument)` form | split apart so the command name stays clean |
| **`Invoke-Expression "git status"`** | **the actual command inside the literal is extracted** (same for `iex`) |
| `Invoke-Expression $cmd` | only the cmdlet itself is reported -- the variable form is **not guessed**, to avoid pseudo-commands |
| Hashtable `@{ Path = "C:\temp" }` | keys are not commands, excluded |
| Splatting `@params` | it is an argument, not a command, excluded |
| Here-string `@"..."@` | contents are treated as **text**, producing no command; the terminator `@` produces no pseudo-command |

The `kind` field distinguishes them in the output: `command` (external program) / `cmdlet` (builtin) / `script` (dot-source).

The handling of subexpressions deserves a separate word:

```powershell
Write-Output "$(Get-Date -Format o)"
```

This produces **two** events:

```
Write-Output $(Get-Date -Format o) ← 外层命令, 参数完整
Get-Date -Format o ← 子表达式里真的被调用的命令
```

The outer command's arguments **stay intact** -- the arguments must not be truncated just because the subexpression was pulled out,
otherwise the command the user sees does not match what is written in the script.

`--format ndjson` emits one event object per line, which suits streaming processing.

---

## Scenarios verified in practice

### Windows (Windows 10 19045, non-administrator)

| Scenario | Result |
|---|---|
| cmd builtins | ✅ captured by the static layer (`echo`/`cd`/`set`/`for`/`if`/`dir`) |
| Direct child processes | ✅ `whoami` / `where` |
| Pipelines | ✅ `cmd.exe /S /D /c echo piped` + `findstr piped` |
| Nested scripts | ✅ three levels deep (depth 0/1/2) |
| **cmake configure** | ✅ 38 events, including TryCompile probes |
| **cmake + Ninja generator** | ✅ |
| **Direct ninja invocation** (without cmake) | ✅ `ninja` → `cmd.exe /C` → `c++.exe` → `c++` → `cc1plus` → `as` → `collect2` → `ld` (depth 5) |
| **GNU make** | ✅ `make` → `sh -c` → `g++` → `cc1plus` |
| **Direct MSVC cl.exe invocation** | ✅ command line read out completely, including `/EHsc` `/std:c++17` |
| **MSBuild (32-bit WOW64)** | ✅ captures the hidden `cmd.exe /Q /D /C...exec.cmd` |
| **Multiple MSBuild versions** | ✅ uses `vswhere` to find all installed versions and verifies each one |
| **32-bit target (a real 32-bit exe)** | ✅ command line read out completely, no arguments lost |
| **PowerShell scripts** | ✅ aliases/pipelines/script blocks/call operators/external programs |
| **PowerShell runtime** | ✅ 12 events (git / npm / dotnet all caught) |
| Short-lived processes | ✅ `cmd /c exit 0` |
| Redirection | ✅ does not affect capture |
| **Anti-debugging flag** | ✅ `--clear-being-debugged` (see the table above) |
| **Process tree view** | ✅ `--tree`, indentation + pid/ppid |
| **Event filtering** | ✅ `--exclude` / `--include`, filtered count goes into `notes` |
| **Summary statistics** | ✅ `--summary` |
| **Completeness conclusion** | ✅ `capture completeness` present in `notes` |
| **PowerShell subexpressions** | ✅ the command inside `$(Get-Date -Format o)` is reported separately |
| **PowerShell `Invoke-Expression`** | ✅ the actual command inside `Invoke-Expression "git status"` is extracted; the variable form is not guessed |
| **cmd REM comments** | ✅ `REM` / `Rem` / `rem` no longer appear in the command list |
| **Tree view completeness** | ✅ every runtime event is **rendered exactly once** (grandchildren no longer disappear) |

> For the Windows capture principle (why the debugger API is used instead of Detours) see
> [`docs/en/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md`](docs/en/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md).

### Linux (Ubuntu under WSL2)

| Scenario | Result |
|---|---|
| sh builtins | ✅ captured by the static layer |
| `execve` family | ✅ `execve`/`execv`/`execvp`/`execvpe`/`execle`/`fexecve` |
| **`execveat`** | ✅ (exported since GLIBC 2.34, it was previously missed) |
| `posix_spawn` family | ✅ `posix_spawn`/`posix_spawnp` |
| make | ✅ `sh -c` wrapper + the inner command |
| cmake + g++ toolchain | ✅ the full chain, max depth 3 |
| **Depth: genuine parent-child nesting** | ✅ bash(0) → sh(1) → echo(2) |
| **Depth: exec process replacement** | ✅ make(0) → echo(0), an unchanged pid means an unchanged level |
| **Root event** | ✅ the first exec happens before `LD_PRELOAD` takes effect, reported by the parent process |
| **Statically linked intermediates** | ⚠️ kernel limitation, cannot be caught -- but they are **flagged event by event** |
| **Statically linked programs** | ⚠️ kernel limitation, cannot be captured -- but they are **explicitly reported** (see below) |
| **setuid programs** | ⚠️ kernel limitation, cannot be captured -- but they are **explicitly reported** |
| **Tree view completeness** | ✅ every runtime event is rendered exactly once (grandchildren no longer disappear) |
| **shim ABI symbols** | ✅ `nm -D` confirms 9 exec/spawn symbols **match exactly** |

---

## Architecture

Four modules, with a **one-way** dependency direction:

```
modules/
 ccap_core/ 纯算法与纯数据, **不允许**引用任何平台 API
 core/
 event.h/.cpp 统一事件模型 (CommandEvent / EventCollector) 
 json.h/.cpp 极简 JSON 序列化
 json_parser.h/.cpp 极简 JSON 解析 (给 compdb 读配置用) 
 command_line.h/.cpp Windows CRT & POSIX shell 命令行切分
 normalize.h/.cpp 路径归一化, 可执行文件解析, 环境变量展开
 script_scanner.h/.cpp 脚本静态分析 (bat/cmd/sh) 
 powershell_scanner.h/.cpp PowerShell 静态分析 (别名/管道/脚本块/调用运算符/子表达式) 
 tree.h/.cpp 进程树视图渲染
 filter.h/.cpp 事件过滤 (--exclude / --include) 

 ccap_capture/ 需要 OS 能力 (进程捕获) 
 capture/
 platform.h/.cpp 平台抽象 + 统一调度 + 完整性结论
 windows_debugger.cpp Windows: CreateProcessW + DEBUG_PROCESS 事件循环
 peb_reader.cpp Windows: 从被调试进程读 PEB 命令行 / 父进程 PID
 linux_preload.cpp Linux: fork/exec + 管道收集
 linux_shim.cpp Linux: LD_PRELOAD 共享库 (shim, 独立构建) 
 capi.cpp C ABI 实现
 include/
 ccap_capture.h **稳定 C ABI** (对外唯一契约) 

 ccap_compdb/ 编译命令识别 / 解析 / 导出
 compdb/
 compiler_table.cpp 编译器全量表 (可扩展规则) 
 alias_resolver.cpp 软连接真实路径别名互认
 command_parser.cpp 多来源命令解析
 compile_command.cpp 编译命令结构化
 filter_rules.cpp 脚本/链接器/汇编器过滤
 compdb_exporter.cpp 导出 clang 编译数据库 / 分析器翻译命令
 clang_translator.cpp 把任意编译器的命令翻译成真正的 clang 命令
 config.cpp JSON 配置加载
 adapters/
 adapter_registry.cpp 适配器注册表
 gcc_adapter.cpp GCC / G++
 clang_adapter.cpp Clang
 msvc_adapter.cpp MSVC cl.exe
 generic_adapter.cpp 兜底
 include/
 ccap_compdb.h

 ccap/ CLI 入口
 app/
 main.cpp 命令分发
 cli.cpp 参数解析
 compdb_cmd.cpp compdb 子命令实现
```

```
 ccap_compdb ──► ccap_core
 ccap_capture ──► ccap_core
 ccap ──────────► ccap_capture, ccap_compdb
```

`ccap_compdb` **does not depend on** `ccap_capture` -- it only consumes "a string of command lines + a working directory",
so it can consume the compile commands produced by CMake / ninja / the various build tools,
without having to run the capture itself.

### Naming convention (modeled on LLVM / clang)

For the full rules see [`docs/en/NAMING-CONVENTION.md`](docs/en/NAMING-CONVENTION.md).

| Category | Style | Example |
|---|---|---|
| Class / struct / enum | `PascalCase` | `CommandEvent` |
| Function / method | `camelBack` | `splitCommandLine` |
| Local variable / parameter | `camelBack` | `sourceFile` |
| Member variable | `camelBack` | `eventCollector` |
| Private member | `camelBack` + trailing `_` | `member_` |
| Constant | `k` + `PascalCase` | `kMaxDepth` |
| Enumerator | `PascalCase` | `CompilerKind::Gcc` |
| Namespace | lowercase | `ccap`, `ccap::compdb` |
| Macro | `UPPER_CASE` | `CCAP_PLATFORM_WINDOWS` |
| Source file | `snake_case.cpp` | `command_parser.cpp` |
| C API | module prefix + `snake_case` | `ccap_session_run` |

**Keeping snake_case in the C ABI is not an exception, it is a convention** -- LLVM's own C API
(`clang-c/Index.h`) is exactly a `clang_` prefix + snake_case.

The rules are **enforced** by `readability-identifier-naming` in `.clang-tidy`,
and driven by `tests/check_naming.py`:

```powershell
# 不需要先进 MSVC 环境 -- include 路径会自动从
# Visual Studio 安装目录 + Windows SDK 注册表项探测
python tests\check_naming.py
```

```
clang-tidy:...\VC\Tools\Llvm\x64\bin\clang-tidy.exe
build dir:...\build-ninja
sources: 29 (skipped 2 platform-specific)

OK: naming convention check passed (0 violations)
```

This gate has four deliberate design decisions:

1. **Missing clang-tidy → failure** (exit code 2), not a skip.
 A gate that "skips when it can't run" is the same as no gate -- on CI it will keep showing green,
 while in reality nothing was checked.
 (Exception: CTest marks exit code 2 as **skipped**,
 so "clang-tidy isn't installed locally" doesn't keep CI permanently red, but the log clearly shows that nothing was checked.)
2. **Compile errors → failure** (exit code 1).
 clang-tidy **outputs no naming warnings at all** when a file has compile errors,
 and "0 warnings" does not mean "conforms to the convention".
3. **Executed file by file**, not all translation units at once.
 When multiple files are passed at once, clang-tidy merges the compilation context,
 so `--extra-arg` only takes effect for the first file → the later files can't find their headers →
 compile errors → the naming check gets skipped.
4. **Include paths are merged from multiple sources**, discarding only the **non-existent** ones.
 The environment variable `INCLUDE` may point at an **uninstalled** toolset version --
 using only it means clang-tidy can't find a single standard header, and the gate silently degrades into an empty check.
 So the Visual Studio install directory + Windows SDK registry entries are used as well.

Moreover, each file is run **twice** (native + `-U_WIN32`),
so that **both sides** of `#if defined(_WIN32)... #else... #endif` are checked --
looking only at the current platform means the other side's code **has never been analyzed**.

It also automatically skips files that **cannot be compiled on the current platform**
(on Windows it skips `linux_preload` / `linux_shim`, and vice versa) --
without skipping, Linux-specific `poll.h` / `dlfcn.h` would produce a pile of "file not found",
drowning the real problems inside them.

The gate has been integrated into CTest as the `naming` test:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

```
4/4 Test #4: naming........................... Passed 157.80 sec
```

The VS generator does not produce a `compile_commands.json`, so the script **automatically generates a sibling Ninja build**
to serve as the compilation database -- it does not interfere with the build tree the developer is using.

### Artifacts

| Target | Windows | Linux |
|---|---|---|
| `ccap` | `build/Release/ccap.exe` | `build/ccap` |
| `ccap_core` (static) | `build/Release/ccap_core.lib` | `build/libccap_core.a` |
| `ccap_capture` (static) | `build/Release/ccap_capture.lib` | `build/libccap_capture.a` |
| `ccap_capture` (shared, C ABI) | `build/shared/Release/ccap_capture.dll` | `build/shared/libccap_capture.so` |
| `ccap_compdb` (static) | `build/Release/ccap_compdb.lib` | `build/libccap_compdb.a` |
| `ccap_compdb` (shared) | `build/shared/Release/libccap_compdb.dll` | `build/shared/libccap_compdb.so` |
| `ccap_shim` (for Linux injection) | -- | `build/ccap_shim.so` |

> The shared library output directory **must be separate from the static library** -- MSVC's import lib is also called `<name>.lib`,
> and putting them together overwrites the static library of the same name (which once made `run_capture` unlinkable).

> `tools/git-push.sh` is an **optional local helper script**, not part of the project,
> and is needed neither for building nor for testing. It is only useful when your network route to GitHub is unstable
> (see [pitfall 29](#29-httpcurloptresolve-is-silently-ignored)).
> When the network is fine, just use `git push`.

### Windows capture principle

```
CreateProcessW(target,..., DEBUG_PROCESS)
 ↓
 ┌─────────────────────────────────────────────┐
 │ WaitForDebugEvent 循环 │
 │ CREATE_PROCESS_DEBUG_EVENT → 记录事件 │
 │ EXCEPTION_DEBUG_EVENT → DBG_CONTINUE │
 │ EXIT_PROCESS_DEBUG_EVENT → 移除存活集合 │
 └─────────────────────────────────────────────┘
 ↓
 从被调试进程 PEB 读: 
 ProcessParameters->CommandLine (真实命令行) 
 ProcessParameters->ImagePathName
 ProcessParameters->CurrentDirectory
 PEB->InheritedFromUniqueProcessId (父进程 PID) 
```

`DEBUG_PROCESS` makes Windows automatically include all descendant processes in the debugging session, so the whole tree can be captured without elevation.

### Linux capture principle

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

#### How depth is computed

`exec` / `posix_spawn` **do not change the pid** -- the replaced process and the process that replaced it are the same pid.
So the `(pid, ppid)` reported by the shim means:

> "the process with pid P (whose parent is Q) is about to be replaced by a new program"

Hence:

```
depth(P) = depth(Q) + 1
```

Q is the `ppid` in the event. Every event produced by the same pid exec'ing repeatedly is at the **same level**
(process replacement does not change the level).

There are two places that are easy to get wrong:

1. **The fallback value must be 0, not 1.** Not finding the parent process can only mean the root process level.
2. **ccap's own pid must be pre-registered as -1.** Otherwise the root process's `ppid` (= ccap's pid)
 can't be found, and the whole exec chain falls back to the fallback value.

#### Where the root event comes from

The child process produced by `fork` is ccap itself (it doesn't link the shim), and when it calls `execv` the shim hasn't been loaded yet --
**that exec cannot be intercepted**.

So the root command is **reported additionally by the parent process**, with an explanation in the event:

```
"notes": ["root (reported by ccap: the first exec happens before LD_PRELOAD takes effect)"]
```

#### Diagnostics for statically linked binaries

The shim checks the target ELF's program headers on every hook (reading only the file header, a few hundred bytes).
A statically linked binary has no `PT_INTERP`, and `LD_PRELOAD` is completely ineffective on it --
**any child process it spawns internally can never be caught**.

Early versions only diagnosed the **root target**, leaving the intermediates as a blind spot. Now every event is tagged individually:

```
"notes": ["target is statically linked - LD_PRELOAD cannot hook it, so any
 child processes it spawns will NOT be captured"]
```

The diagnostic switch `CCAP_SHIM_DEBUG=1` prints every hook to stderr --
when capture fails, this is the only observation point (you can see whether the shim hooked at all).

---

## Pitfalls hit along the way (all backed by measurements)

They are written here because these pitfalls are all subtle, and **the symptoms do not match the causes**.

### 1. The executable path must not be converted to forward slashes

`C:/WINDOWS/system32/cmd.exe` makes cmd.exe fail when parsing its own image path,
reporting "The syntax of the command is incorrect", and it **creates only 1 process and exits**.

Forward slashes are safe in script arguments, but `lpApplicationName` must use the native separator.

### 2. The quiet period must not be used to decide that the run has ended

"No debug events for a while" does not mean "it has finished running".
While cmd.exe executes `dir /b` over 100,000 files, there are no debug events at all,
yet the script is still running -- returning early on the quiet period loses the entire subtree.

The correct criterion is: **the root process has exited and all child processes have exited**.

### 3. The PEB offsets of a 32-bit process are the same as 64-bit

There was once the belief that x86 has a different set of offsets (0x24/0x38/0x40), but reading a 32-bit MSBuild gave
garbage for CommandLine (len=7950, buf=0x685421CD4C01B821),
and `ReadProcessMemory` reported err=998.

Measured PEB raw dump:
```
PEB+0x20 = ProcessParameters
PP+0x38 = CurrentDirectory
PP+0x60 = ImagePathName
PP+0x70 = CommandLine
```
The only difference is the **pointer width**, not the field offsets.

### 4. The parent process PID can only be obtained from the PEB

The `CREATE_PROCESS_DEBUG_EVENT` structure contains **no** parent process information and no `lpCommandLine` either.
The parent process PID comes from `NtQueryInformationProcess` returning
`PROCESS_BASIC_INFORMATION.InheritedFromUniqueProcessId`.

### 5. The CRT rules for backslash escaping

A single backslash before a non-quote character is a **literal**. If implemented incorrectly,
`C:\Windows\x.exe` becomes `C:Windowsx.exe`.

The full rules (`2k` backslashes + X):
- X is `"` → `k` backslashes + toggle the quoting state
- X is `"` (an odd count) → `k` backslashes + a literal `"`
- X is something else → `k` (`k+1` when odd) backslashes + X itself

### 6. The constant for `ContinueDebugEvent`

The first breakpoint must use `DBG_CONTINUE` (0x00010002).
Using `DBG_EXCEPTION_NOT_HANDLED` (0x80010001) leaves the target stuck at the initial breakpoint,
creating only 1 process.

### 7. `dedupe_ld_preload` must not return a pointer to a local vector

In the Linux shim, `out.data()` points into the internal buffer of a local `std::vector`,
which dangles once the function returns -- `execve` then reads a wild pointer and reports "Bad address".
A `thread_local` static buffer must be used.

### 8. The variadic layout of `execle`

`execle(path, arg0, arg1,..., NULL, envp)` -- there is one more envp pointer after the NULL.
If the NULL is omitted, envp is taken as the (argc+1)-th argv element.

### 9. Handles in debug events must be closed

The `hProcess` / `hThread` / `hFile` in `CREATE_PROCESS_DEBUG_INFO`
are all the debugger's responsibility to close. Not closing them exhausts the handles after a few thousand processes.

### 10. WOW64 has two PEBs

This is the most subtle pitfall for 32-bit targets.

In a 64-bit debugger, a 32-bit target process **has two PEBs**:

| Source | What it is |
|---|---|
| `NtQueryInformationProcess(ProcessBasicInformation)` | PEB64 (the WOW64 shadow PEB) |
| `NtQueryInformationProcess(ProcessWOW64Information)` | PEB32 ← **what the 32-bit code actually uses** |

Measured (`tests/probe/DbgPeb.cpp`):
```
ProcessBasicInformation -> peb = 0x914000
ProcessWOW64Information -> peb = 0x915000
目标自己报的 fs:[0x30] -> 0x915000
```

They differ by 0x1000. **Patching the PEB64 has no effect whatsoever on the target.**

And the `RTL_USER_PROCESS_PARAMETERS` layouts in the two PEBs differ
(all 4-byte pointers in PEB32, so the field offsets don't line up with the 8-byte pointer layout of x64),
so:
- command line / image path / working directory → read from the **PEB64**
- `BeingDebugged` patch → applied to the **PEB32**

### 11. Deduplication of static events cannot rely on (pid, exe, argv) alone

In early versions the dedup key was `(pid, executable, argv)`, and the same scheme was used for static events.
The pid of a static event is always 0, so:

```bat
echo same
echo same
echo same
```

The last two of the three commands were treated as duplicates and **silently dropped** -- the output had only 2 entries.
Now static events are distinguished by `(script path, line number, argv)`.

Runtime events can't rely on `(pid, exe, argv)` alone either: on Linux `exec` doesn't change the pid,
so repeatedly exec'ing the same command with the same pid is N independent executions. `exec_index` was added for this.

### 12. Quoted tokens must not be lost

In the PowerShell static analysis, after lexical splitting there is a branch that handles quoted tokens.
That branch **forgot the `push_back`** -- as a result all quoted tokens disappeared:

```powershell
& "C:\Windows\System32\whoami.exe" /all
```
→ 0 events (instead of 1)

`Write-Output "hello"` → only `Write-Output` is left, the argument is lost.

The signature of this kind of bug is: **the output looks like "something is missing", but there is no error at all**.
Static analyzers must be tested with an "expected count", not just by "there is output".

### 13. The fallback value for Linux depth was wrong

In early versions the depth was computed as:

```cpp
int depth = 1; // 兜底
auto it = depth_of.find(e.ppid);
if (it!= depth_of.end()) depth = it->second + 1;
```

Two mistakes were stacked on top of each other:

1. **The fallback value was 1, it should be 0.** Not finding the parent process can only mean the root process level.
2. **ccap's own pid was not registered.** The root process's `ppid` is ccap's pid,
 so it can't be found → it falls back to the fallback value → **every exec of the root process ends up at depth 1**.

Symptom: genuinely nested levels are correct, but `make` (which exec's itself into echo) shows up as
`make(0) → echo(1)` -- even though it is the same level.

Fix: pre-register ccap's pid as `-1`, so the root process naturally computes to 0.

### 14. `REM` comments were never recognized

cmd's `REM` is a comment (case-insensitive). But the previous check was written inside the "scan character by character" process:

```cpp
size_t j = i;
while (j > 0 && isspace(line[j-1])) --j;
if (j == 0 && line.compare(0, 3, "rem") == 0) {... }
```

`j` is "the starting position after skipping leading whitespace". By the time the scan reaches the 3rd character `j` already equals 3,
→ **`j == 0` never holds** -- the condition is effectively dead code.

Moreover `compare(0, 3, "rem")` is case-sensitive, so `REM` doesn't match either.

Symptom: `REM === 注释 ===` shows up in the command list.

Fix: pull the check out into its own logic -- first find the first non-whitespace character at the beginning of the line and see whether it is `rem`,
then check whether what follows is a separator (otherwise `remote` is a real command and must not be treated as a comment).

**Lesson -- a "condition that is effectively dead" is as dangerous as a "condition that is wrong".**
The code looks like it is checking something, but in reality it never holds even once.

### 15. An incomplete PowerShell verb table misjudges cmdlets as external programs

A cmdlet is always in `Verb-Noun` form, so the detection method is simple: look up the verb table.

But an incomplete verb table causes misjudgments:

| Command | Verb | Result |
|---|---|---|
| `Where-Object` | `Where` | not in the table → treated as an external program |
| `ForEach-Object` | `ForEach` | not in the table → treated as an external program |

Symptom: `is_builtin` is false, `kind` is `command` --
the user sees "these two are external programs", even though they are clearly cmdlets.

Fix: fill in all of PowerShell's official verbs.

### 16. After a subexpression is pulled out, the outer arguments must not be truncated

When handling a `$(...)` subexpression, if you **carve** the subexpression out of the line and then run the main flow:

```powershell
Write-Output "now is $(Get-Date -Format HH:mm:ss)"
```

it becomes `Write-Output "now is"` -- part of the argument is gone.

The command the user sees doesn't match what is written in the script; that is **wrong**, not "a loss of precision".

The correct approach: **tokenize the original line as usual** (arguments intact),
and report the commands inside the subexpression **additionally** on their own. Two events, each with its own job.

### 17. `--timeout 2000` actually ran for 7 seconds

After the timeout kills the process tree, the event loop breaks out, but the cleanup code unconditionally waits 5 seconds:

```cpp
if (!root_exited) {
 WaitForSingleObject(pi.hProcess, 5000); // ← 超时路径上不该等这么久
```

The user set a 2-second timeout but has to wait 7 seconds. This is not "a bit slower",
it is **the timeout parameter not being respected** -- in automated pipelines that leads to cascading timeouts.

Fix: use a 500ms fallback on the timeout path (it completes almost immediately after `TerminateProcess`),
and keep 5000ms on the non-timeout path (the process really may be winding down normally there).

Measured: `--timeout 2000` went from 7156ms down to 2738ms.

### 18. `--summary` must not be appended after the JSON

In early versions `--format json --summary` **appended** the statistics text **after the JSON document**,
which made the whole file invalid JSON -- `jq` / `ConvertFrom-Json` errored out immediately.

JSON is a format meant for **machine consumption**; appending any non-JSON text at the end breaks the contract.

Fix:
- JSON mode → `summary` as a **top-level JSON field**
- NDJSON mode → not emitted (the convention of that format is "one event object per line")
- text mode → append a human-readable text block

### 19. Builtins must not report a "resolved" path

cmd's `echo` is a **shell builtin** and creates no process at all.

But there may really be an `echo.exe` in PATH (w64devkit / MSYS / Git Bash all ship one),
and if the static analyzer resolves it, the output is:

```
"resolved": true,
"notes": ["resolved=D:/development/w64devkit/bin/echo.exe"]
```

That makes people think `echo` is an external program -- contradicting `is_builtin: true`.

Fix: **builtins are not path-resolved at all**, `resolved` is always `false`.
A builtin simply has no "executable path".

### 20. A UTF-8 BOM makes prefix matching fail silently

Editors on Windows and PowerShell 5.1's `Set-Content -Encoding UTF8` both write a BOM.

A BOM is not **content**, it is an encoding marker. Leave it in place, and every **prefix-based** parser fails silently:

```cpp
line.rfind("EXECUTING ", 0) == 0 // BOM 在最前面 -> 永远不等
```

The symptom is "**0 commands** parsed", not "parsing failed"-- the worst kind of bug:
no error at all, the user just concludes "this tool doesn't support this kind of build log".

The fix goes into `read_file`, solving all consumers at once
(build logs / JSON config / compilation database / captured event streams):

```cpp
if (content.size() >= 3 &&
 content[0] == 0xEF && content[1] == 0xBB && content[2] == 0xBF) {
 content.erase(0, 3);
}
```

**Lesson -- a parser's prefix matching should only ever apply to "content".**
The encoding marker must be stripped **when it is read in**, not handled separately in every parser --
the latter misses cases (there is always a new consumer), and the symptom is silent.

### 21. The test moved the source file away, so the symlink became a broken link

The symlink alias test kept failing on Linux. On closer inspection the product had no bug; the test was wrong:

```cpp
bool linked = (std::rename(real.c_str(), alias.c_str()) == 0); // ← 把源文件移走了
std::string cmd = "ln -sf '" + real + "' '" + alias + "'"; // ← 指向一个不存在的东西
```

`rename` moved `real` to `alias`, so:

- `realpath(real)` fails → it degrades to path normalization
- `realpath(alias)` fails (broken link) → it also degrades to path normalization
- The two can never match → the alias set is empty

**Lesson -- the "prepare the data" step of a test can itself have a bug.**
The symptom looks like a product defect ("symlink aliases aren't recognized on Linux"),
but the root cause is in the test. How to tell: **the same test passes on Windows and fails on Linux**,
so first suspect whether the **cause** of the platform difference is something the test itself wrote.

Fix: create the file first, then create the symlink with `ln -sf`; don't touch the source file.

### 22. A bulk rename turned `posix_spawn` into `posixSpawn`

During the LLVM/clang naming convention refactor, 341 identifiers were renamed in bulk.
One of them was the Linux shim's hook function:

```cpp
// 改之前 (正确) 
extern "C" int posix_spawn(pid_t* pid, const char* path,...);

// 改之后 (静默失效) 
extern "C" int posixSpawn(pid_t* pid, const char* path,...);
```

`posix_spawn` is **glibc's dynamic symbol name** -- `LD_PRELOAD` relies on that name to "hijack" the symbol.
After the rename, the shim exports a `posixSpawn` that **nobody calls**,
while `posix_spawn` still points at glibc's real implementation.

The symptom is especially deceptive:

```
make 只捕获到根事件, 子进程全部丢失
```

Because `make` goes through the `posix_spawn` path, the hook never took effect.
**There is no error at all**; the shim loads fine and exits fine, it just doesn't do anything.

**Lesson -- ABI names must never be renamed along with the "naming convention".**
The name itself is the contract: `posix_spawn` / `execve` are glibc's exported symbols,
`ccap_session_run` is the public C ABI,
`PROCESS_BASIC_INFORMATION` is ntdll's struct layout.
These are **names other people's code looks for**, not a matter of our style.

Fix:

1. Restore the exact symbol names (the 9 exec/spawn symbols such as `posix_spawn` / `posix_spawnp`).
2. Add a **symbol table gate**: in `tests/run_tests_linux_depth.sh`, assert that
 `nm -D --defined-only build-linux/ccap_shim.so` must contain those 9 symbols.
 A rename → the gate turns red immediately.

> This is exactly what a gate is for: relying only on "I ran it and nothing errored",
> such a silent failure can hide for a long time -- because it only shows up on **specific code paths**.

### 23. Compile errors turn the naming gate into an "empty check"

clang-tidy's `readability-identifier-naming` **outputs no naming warnings at all**
when a file **has compile errors**.

```
编译错误 → clang-tidy 跳过命名检查 → 0 条警告 → 门禁显示绿色
```

The gate was supposed to prevent drift, but it turned into "checked nothing yet shows as passed".

And the reasons that trigger this silent failure are often **unrelated**:

- MSVC's include paths are not in `compile_commands.json` (they come from an environment variable)
- The project's own `-I` uses MSVC syntax (single dash), which clang doesn't understand
- `_WIN32` isn't defined → the code takes the `#else` branch and includes `poll.h` (which of course doesn't exist on Windows)

**Lesson -- "no violations reported" and "checked and found no violations" are two different things.**
A gate must be able to tell them apart, otherwise the green light it gives is meaningless.

Fix (`tests/check_naming.py`):

1. **Compile errors → exit code 1**, and explicitly print that the naming check is **incomplete**.
2. **Missing clang-tidy → exit code 2** (not a skip).
3. **Execute file by file** -- when multiple translation units are passed at once, clang-tidy merges the compilation context,
 and `--extra-arg` only takes effect for the first file.

A **self-verification** was also added: deliberately inject a `bad_variable_name`
and confirm the gate really reports it:

```
FAIL: 1 处命名违规
modules\ccap_core\core\event.cpp:68:9 local variable 'bad_variable_name'
```

A gate that has never been verified to be able to turn red is the same as no gate at all.

### 27. The gate's include path "uses the environment variable if it exists" -- if the variable points at an uninstalled version, everything is wasted

After this clean rebuild, the naming gate turned red straight away in CTest:

```
modules\ccap\app\main.cpp:14: 'cstdio' file not found
```

The root cause is not that the code changed, but that **the environment changed**:

| Source | Content | Status |
|---|---|---|
| Environment variable `INCLUDE` | `MSVC\14.41.34120\include` | **that directory has been uninstalled** (the one that exists is 14.42.34433) |
| `vswhere -find VC\Tools\MSVC\**\include` | `14.42.34433\include` | valid, but it is **only queried when INCLUDE is empty** |
| Windows SDK | `D:\Windows Kits\10\Include\10.0.26100.0\...` | valid, but `vswhere` **cannot find it** |

The old logic was "use INCLUDE if it is non-empty, otherwise probe the install directories". That logic is right in the **convenient** case
(INCLUDE is valid), and wrong in the **dangerous** case: INCLUDE is non-empty but **points at a directory that doesn't exist**,
so the only MSVC include path is dead → clang-tidy can't find any standard header →
**compile errors → no naming warnings emitted → the gate shows "0 violations"**.

This is the same silent failure as pitfall 23, just with a different trigger.

It was also discovered that `vswhere` **structurally cannot find the Windows SDK**: the SDK is installed under
`C:\Program Files (x86)\Windows Kits\10`, which is **outside** the VS install tree,
and `vswhere -find` only knows the VS install tree. The authoritative source for the SDK is the registry
`HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots`, key `KitsRoot10`.

**Lesson -- "use the environment variable if it exists" is a wrong default.**
Environment variables are **potentially stale caches**; the authoritative sources (install directory / registry) are the real thing.
The correct approach is to **merge all sources and discard only the paths that don't exist**.

**Corollary -- a gate's own inputs must be existence-checked too.**
"INCLUDE is not set" and "INCLUDE points at a deleted directory" look exactly the same in code
(both mean "no valid path can be assembled"), so the fix cannot be just an `if not INCLUDE` branch.

**Corollary 2 -- structural blind spots need a different tool to cover them.**
`vswhere` failing to find the SDK is a structural capability gap, not a configuration problem --
changing parameters doesn't help, you have to change the data source (the registry).

Fix (`tests/check_naming.py`):

```python
# 合并所有来源, 只丢弃不存在的路径
ambient_usable, ambient_missing = usable_include_dirs(os.environ["INCLUDE"])
discovered_usable, _ = usable_include_dirs(discover_msvc_includes())
inc_dirs = dedupe(ambient_usable + discovered_usable)
```

The Windows SDK is now taken from the registry's `KitsRoot10`, and the SDK with the **highest version number** is used.

Now this gate also works in a CTest environment **without a vcvars64.bat environment**:

```
4/4 Test #4: naming........................... Passed 157.80 sec
```

### 28. The tree renderer moves nodes out, so the pid index points at an empty shell

The old `tree.cpp`, when attaching child nodes to their parent:

```cpp
child = std::move(*it); // 把节点从 pool 里搬走了
```

The node pool is a `std::vector`; after `std::move` the element is still in the vector,
but it has **become moved-from**. And the pid → node index table **still points at it**.

Result: **grandchildren disappear from the tree**. 4 original events rendered only 3 nodes.

```
[000001] d=0 RT pid=600 ccap_repeat
[000002] d=0 RT pid=611 echo first <- 少了一个
```

Fix: switch to a **stable node pool + child index**:

```cpp
std::vector<TreeNode> pool_; // 节点只增不减
std::vector<std::vector<size_t>> kids_; // 子节点存**索引**, 不是指针
```

This way every runtime event is **rendered exactly once**, and it doesn't depend on the node's position in the vector.

**Lesson -- when storing objects in a container, `std::move` invalidates every "index pointing at it".**
Either store `shared_ptr`, or use indices instead of references -- but **indices are only valid as long as the object doesn't move**.

**Corollary -- simplification bought with a "move" often has to be paid back elsewhere.**
The original intent here was to save one copy; the price was the correctness of the entire tree.

**Corollary 2 -- a regression test must be able to catch a "one is missing" bug.**
The current `run_tests_linux_depth.sh` asserts that
"4 original events render 4 nodes". Running this test with the old buggy `tree.cpp`
fails consistently (3 rendered)-- that is what makes it an effective regression test.

### 24. The gate can't see the code in the `#else` branch

The gate above was green at first -- yet there really was snake_case local variables in the code.

Reason: **clang-tidy only checks what still exists after preprocessing**.

```cpp
#if defined(_WIN32)
... Windows 分支...
#else
 size_t name_start = open + 1; // ← 违规, 但 Windows 上这行根本不存在
#endif
```

Running clang-tidy on Windows, the `_WIN32` branch survives and the `#else` branch is preprocessed away --
**that code has never been analyzed**.

Measured evidence (`normalize.cpp`):

```
把 name_start 改回 nameStart 前后, 门禁都是 "0 处违规"
```

And there is an even more subtle point: **MSVC's SDK headers implicitly bring in `_WIN32`**,
so even if you don't explicitly pass `-D_WIN32`, the Windows branch is taken anyway.
To cover the other side you must use `-U_WIN32` to **cancel** it.

Fix: the gate runs **twice** for every file.

| pass | Arguments | Covers |
|---|---|---|
| native | default | the current platform's `#if` branch |
| no-win32 | `--extra-arg -U_WIN32` | the other side's `#else` branch |

The no-win32 pass inevitably produces a pile of compile errors (that side's platform-specific headers don't exist locally),
but that **doesn't affect the conclusion** -- the compiler has already checked the parseable parts before reporting errors.
So: **compile errors in the native pass mean failure, in the no-win32 pass they only mean a warning**.

Verification (deliberately putting `name_start` back into the `#else` branch):

```
FAIL: 1 处命名违规
modules\ccap_core\core\normalize.cpp:380:16 local variable 'name_start' [另一侧分支]
```

**Lesson -- which code a gate "checked" matters just as much as whether it checked at all.**
Reporting only "0 violations" is meaningless unless you know **which branches it covered**.

### 25. A bulk rename also changed the JSON keys

During the naming refactor, a script was used to rename in bulk. The script cannot tell "an identifier" apart from "a data contract in a string literal",
so it changed the JSON keys of the configuration file as well:

```cpp
// 改之前 (正确) 
"system_includes": { "gcc": ["..."] }

// 改之后 (错的) 
"systemIncludes": { "gcc": ["..."] }
```

The parser reads `"system_includes"` -- so the system header paths in the configuration are **silently lost**.

The tests caught it (`FAIL systemIncludes 正确`), which was lucky:
that assertion happened to cover this key. If the configuration had more keys and the tests hadn't covered them,
this would have been a **silent functional regression**.

**Lesson -- a rename script must distinguish "identifiers" from "data contracts".**
JSON keys, command line options, output field names, file paths -- these are **not identifiers**,
they are **interfaces**, and changing them breaks compatibility.

Verification method: **diff all string literals file by file** before and after the rename, confirming that only include paths changed:

```python
old_lits = extract_literals(backup_file)
new_lits = extract_literals(current_file)
lost = old_lits - new_lits
```

The only loss this diff turned up was exactly that JSON key.

### 26. Using an "exemption" to cover up "not changed"

While renaming, the fields of the internal struct `RunOptions` were found to still be snake_case
(`timeout_ms` / `quiet_ms` / `clear_being_debugged` / `redirect_output_to_stderr`).

The fastest fix is to add an entry to the exemption list in `.clang-tidy` -- the gate turns green immediately.

But that is **using an exemption to cover up a non-change**. Those fields are not ABI at all:

| Struct | Is it ABI? | Which style it should use |
|---|---|---|
| `ccap_run_options` (C ABI) | ✅ yes, an external contract | snake_case |
| `ccap::RunOptions` (internal C++) | ❌ no | camelBack |
| `ccap::cli::Options` (internal C++) | ❌ no | camelBack |

And the exemption list matches **by name** -- `timeout_ms` being in the exemption list
means that **any** field named `timeout_ms` is exempt, including the internal ones.
When a field name appears in both the ABI and internal structs, an exemption simply cannot tell them apart.

The correct approach: really change the internal fields to camelBack, and keep only the ABI ones in the exemption list.

**Lesson -- an exemption rule has to be able to answer: "why am I exempting this?"
If you can't answer, it is covering up a problem for you.**

### 29. `http.curloptResolve` is **silently ignored**

The route from this machine to GitHub is unstable: the `20.205.243.166` returned by DNS gives
"Connection refused". Changing the IP is the standard solution -- but the standard solution itself has a pitfall.

git maps `http.curloptResolve` to libcurl's `CURLOPT_RESOLVE`,
and **whether it actually takes effect depends on how git/libcurl were built**:

| git | Result |
|---|---|
| 2.40 (Windows) | ✅ takes effect, `Trying 140.82.112.3:443` |
| 2.34 (Linux/WSL) | ❌ **silently ignored**, still `Trying 20.205.243.166:443` |

`GIT_TRACE_CURL=1` makes it immediately obvious:

```bash
GIT_TRACE_CURL=1 git -c http.curloptResolve=github.com:443:140.82.112.3 ls-remote
```

**A setting that is silently ignored is the worst kind of setting**: the command sometimes succeeds and looks like "it's working",
so you keep fighting a knob that never took effect at all. The initial 900-second timeout of the
`push` on this machine came from exactly that.

**Lesson -- any "DNS bypass" setting must be verified to actually take effect,
rather than assumed to.** The criterion is whether the address in the trace is the one you specified.

`tools/git-push.sh` bakes this check in: first confirm via the trace that the current git
**can** accept this setting, then retry candidate IPs one by one, and only fall back to plain git if they all fail.

---

## Known gaps

| Gap | Status | Explanation |
|---|---|---|
| PowerShell static analysis is not a full parser | Still present | What we do is lexical splitting + subexpression extraction, not a real syntax tree. `& { }` dynamic blocks still can't be parsed |
| `Invoke-Expression` only recognizes literals | Partially done | The actual command inside `Invoke-Expression "git status"` / `iex "npm install"` **is** extracted. `Invoke-Expression $cmd` only reports the cmdlet itself -- the variable form is not guessed, to avoid pseudo-commands |
| Dynamically constructed command names in PowerShell | Still present | `$cmd = "git"; & $cmd` only shows the variable name, not the actual command |
| Iteration lists in cmd `for` loops | Fixed | What's inside the parentheses is an **iteration list**, not a command. The old implementation reported `for %i in (a b) do echo loop` as `a b` and lost `echo loop`. Now the command body after `do` is parsed |
| Variable expansion in scripts | Still present | `cd %MYDIR%` reports the **unexpanded** original text. Getting the actual directory requires modeling environment variables |
| Loop expansion in scripts | Partially done | `for %i in (1 2 3) do cmd` reports `cmd` **once**, not 3 times. Semantically good enough (the command itself is right), but the count isn't accurate |
| `CheckRemoteDebuggerPresent()` | Cannot be fixed | It queries the kernel's debug object list, not a user-mode flag. `--clear-being-debugged` can only cover the other two detection methods |
| Windows `BeingDebugged=1` | Mitigated | `--clear-being-debugged` can clear it (disabled by default) |
| The first exception is swallowed by the debugger | Improved | Now `DBG_CONTINUE` is used only for the **first breakpoint of each thread** (that's the initial breakpoint placed by the loader), and breakpoints thrown by the program itself are handed back to the target |
| The debugger exiting also terminates the target | Mostly absent | The normal path waits for all child processes to exit before returning; it can happen on a forced abort |
| Linux setuid programs | Kernel limitation | The kernel drops `LD_PRELOAD`. **It is explicitly reported in `notes`** |
| Linux statically linked programs | Kernel limitation | They don't go through the dynamic linker, so the shim is completely ineffective. **It is explicitly reported in `notes`** |
| Statically linked intermediates on Linux | Mitigated | Early versions only diagnosed the root target, leaving the intermediates as a blind spot. Now the shim checks the ELF **on every exec** and tags it |
| The internal symbols `__execve` / `__posix_spawn` | Not applicable | Modern glibc **does not export** these symbols (`nm -D` can't find them), so hooking by name is impossible. `posix_spawn`/`execve` themselves are covered; the full make/cmake/gcc chains were all caught in practice |
| Deduplication of huge event counts | Improved | Static events are distinguished by (script, line number), runtime events by (pid, executable, argv, exec index). Repeated execs of the same command with the same pid are not merged |
| 32-bit targets | Verified | The command line of a real 32-bit exe can be read out completely. **Note that WOW64 has two PEBs** (see below) |
| `seq` is not contiguous after event deduplication | Still present | Deduplicated events don't take up a seq, so seq is an "output sequence number", not an "original event number" |
| Capture completeness determination | Implemented | Every capture run ends with a `COMPLETE` / `PARTIAL` conclusion in `notes`, including the reason |
| Intermediates in the tree view | Still present | On Linux, repeated execs with the same pid are rendered as sibling nodes (reflecting "process replacement"), but this isn't fully symmetric with the parent-child tree on Windows |
| The tree renderer losing nodes | Fixed | The old implementation `std::move`d child nodes out of the pool while the pid index still pointed at the moved-from node -- **grandchildren disappeared from the tree**. Now a stable node pool + child index is used, and every runtime event is rendered exactly once |
| Files with a BOM | Fixed | `read_file` strips the UTF-8 BOM uniformly. Previously, a build log with a BOM would **silently parse out 0 commands** |
| The C ABI of `ccap_compdb` | Not applicable | Only `ccap_capture` provides a public C ABI. `ccap_compdb` is a C++ library, and the shared library is only usable with the same compiler |
| The naming gate does not cover Linux-specific files | Partially done | On Windows, `linux_preload.cpp` / `linux_shim.cpp` are skipped (they can't be compiled on this platform). **They need to be run additionally on a Linux environment with clang-tidy installed**: `python3 tests/check_naming.py` |
| The naming gate is not integrated into CTest | Fixed | It is now integrated into CTest as the `naming` test. When clang-tidy is missing, exit code 2 → CTest marks it as **skipped** (not permanently red), but the log clearly shows that nothing was checked |
| The naming gate relies on an environment variable for MSVC include paths | Fixed | The old logic "use INCLUDE if it is non-empty" fails silently when INCLUDE points at an **uninstalled toolset version** -- clang-tidy can't find any standard header, the naming warnings are not emitted at all, and the gate becomes an empty check. The paths from the **merged** environment variable and install directory/registry probing are now used, discarding only the ones that **don't exist** |
| The gate's dual-branch pass produces 40 expected compile errors on Windows | Known | The platform-specific headers on the `-U_WIN32` side don't exist locally. These **don't affect the conclusion**; the script only records a note and does not fail |
| Lossy options in the clang translation | Reported | MSVC's `/EHsc` / `/MD` etc. have no clang equivalent. They are **not silently dropped** -- `clangTranslationLosses()` reports them one by one, and the ones that affect the ABI, such as the runtime library, are flagged separately |
| The clang translation doesn't handle exotic MSVC options beyond `/external:I` | Partially done | `/external:I` / `/imsvc` → `-isystem`, `/FI` → `-include`, `/TP` `/Tp` `/TC` → driver selection are all handled. **Rare** options (such as `/analyze`) are still passed through as-is and are listed in the loss report |
| Driver selection in the clang translation | Fixed | Originally the driver was chosen only by extension. MSVC's `/TP` (`.c` compiled as C++) picks the wrong driver -- and `clang++` links libstdc++, which **changes the ABI**. The driver is now chosen according to the language explicitly specified by the compiler |
| Build-tool coverage only reaches the "driver" layer | Known | What is tested is how the **drivers** cmake / ninja / make / msbuild / cl.exe invoke the compiler. Upper-layer build systems like Qt are not installed, so its qmake/moc/uic chain isn't tested -- but it ultimately calls the compiler too, landing on the same layer |

---

## Tests

```powershell
# Windows 回归测试 (100 项) 
powershell -File tests\run_tests.ps1

# Windows 编译命令端到端测试 (29 项) 
powershell -File tests\run_tests_compdb.ps1

# 单元测试 + C ABI 验证 + 命名门禁 + 协议头门禁 (5 项, 约 160 秒) 
ctest --test-dir build -C Release --output-on-failure

# 命名规范门禁 (也可以单独跑) 
python tests\check_naming.py

# 协议头门禁 (也可以单独跑; --fix 会自动补上缺失的头) 
python tests\check_license.py
```

```bash
# Linux 回归测试
bash tests/run_tests_linux.sh # cmake / make 全链路
bash tests/run_tests_linux_extra.sh # execveat / posix_spawn / 静态链接诊断
bash tests/run_tests_linux_dedup.sh # 去重不能丢事件
bash tests/run_tests_linux_stdout.sh # --stdout-to-stderr
bash tests/run_tests_linux_depth.sh # 深度 / 根事件 / 超时 / 完整性 / 树渲染 / shim ABI 符号 (33 项) 
bash tests/run_tests_compdb.sh # 编译命令端到端 (25 项) 

# 或者一次跑完全部
bash tests/run_all_linux_suites.sh

# Linux 上的命名规范门禁 (需要 clang-tidy) 
python3 tests/check_naming.py
```

Current results:

| Suite | Result |
|---|---|
| Windows regression `run_tests.ps1` | 100/100 |
| Windows compile commands `run_tests_compdb.ps1` | 29/29 |
| Windows unit tests `test_compdb.exe` | 163/163 |
| Windows unit tests `test_cmdline.exe` | ALL PASSED |
| Windows CTest (incl. C ABI + naming gate + license header gate) | 5/5 |
| Windows naming gate `check_naming.py` | 0 violations (29 files × 2 branch passes) |
| Windows build | zero warnings |
| Linux regression `run_tests_linux.sh` | exit code 0 |
| Linux `run_tests_linux_extra.sh` | exit code 0 |
| Linux `run_tests_linux_dedup.sh` | exit code 0 |
| Linux `run_tests_linux_stdout.sh` | exit code 0 |
| Linux depth `run_tests_linux_depth.sh` | 33/33 |
| Linux compile commands `run_tests_compdb.sh` | 25/25 |
| Linux unit tests `test_compdb` | 165/165 |
| Linux CTest | 5/5 (`naming` marked as skipped because clang-tidy isn't installed locally) |
| Linux build | zero warnings |
| shim ABI symbols (`nm -D`) | 9/9 |
| DLL exported symbols (`dumpbin /exports`) | 8/8 |

> The Windows unit tests have 2 fewer than Linux: the two symlink alias assertions need `ln`,
> and on Windows they are **explicitly skipped** (`skip 软连接测试 (ln 失败) `) rather than pretending to pass.

> `check_naming.py` checks `linux_preload.cpp` / `linux_shim.cpp` on Linux,
> while on Windows those two files are **explicitly skipped** (it prints `note: 跳过 2 个非本平台的文件`),
> rather than pretending to have checked them.

Under `tests/probe/` are one-off probe programs used during debugging (`DbgTree*.cpp`, `DbgPeb.cpp`),
used to locate the pitfalls listed above; they are kept so that the conclusions are reproducible.

They are **not part of the build** by default, to avoid slowing down the main build. Enable them explicitly when needed:

```bash
cmake -S. -B build -DCCAP_BUILD_PROBES=ON
```

> `tests/run_all_linux_suites.sh` is the single entry point that chains the 6 Linux suites together.
> It is a file rather than an inline `wsl -e bash -lc "..."` script because passing a multi-line bash script
> from PowerShell makes the nested quoting very easy to lose -- and on failure it dumps the **entire text**,
> not just the tail.

---

## Related documents

Every document exists in both Chinese and English. Switch languages from the banner at the top of any page.

| Document | 中文 | English |
|---|---|---|
| Solution research | [`docs/zh/RESEARCH-COMMAND-CAPTURE-2026-10-27.md`](docs/zh/RESEARCH-COMMAND-CAPTURE-2026-10-27.md) | [`docs/en/RESEARCH-COMMAND-CAPTURE-2026-10-27.md`](docs/en/RESEARCH-COMMAND-CAPTURE-2026-10-27.md) |
| Modular design | [`docs/zh/DESIGN-MODULES-2026-10-27.md`](docs/zh/DESIGN-MODULES-2026-10-27.md) | [`docs/en/DESIGN-MODULES-2026-10-27.md`](docs/en/DESIGN-MODULES-2026-10-27.md) |
| Windows capture principle | [`docs/zh/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md`](docs/zh/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md) | [`docs/en/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md`](docs/en/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md) |
| Naming convention | [`docs/zh/NAMING-CONVENTION.md`](docs/zh/NAMING-CONVENTION.md) | [`docs/en/NAMING-CONVENTION.md`](docs/en/NAMING-CONVENTION.md) |

- **Solution research** -- a comparison of Windows Detours / ETW / debugger API / Linux LD_PRELOAD / seccomp,
 and why the combination "debugger API + LD_PRELOAD + static analysis" was ultimately chosen.
- **Modular design** -- the responsibility boundaries of the four modules, dependency direction, the C ABI contract, the compdb architecture.
- **Windows capture principle** -- the mechanics of the `DEBUG_PROCESS` debugger API, PEB reading,
 the WOW64 dual PEB, and an item-by-item comparison of **why Detours / ETW / WMI are not used**.
- **Naming convention** -- the style of each kind of identifier, why the C API keeps snake_case,
 and which names **must not** be renamed along with the style (ABI contracts).
- [`prototypes/windows-debugproc/`](prototypes/windows-debugproc/)
 -- A C# mechanism-validation prototype (proving that `DEBUG_PROCESS` can capture a three-level process tree without administrator rights).
