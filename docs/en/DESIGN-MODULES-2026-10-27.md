# CommandCapture Modular Design

<!-- i18n:begin -->
中文 | [**English**](../zh/DESIGN-MODULES-2026-10-27.md)
<!-- i18n:end -->

**Date**: 2026-10-27
**Status**: All three modules fully implemented, naming convention unified, gates established

---

## 1. Why modularize

Currently `ccap` is a single exe that mixes three things together:

1. **Capture** (process creation observation + script static analysis)
2. **Command parsing** (recognizing which captured results are compile commands)
3. **Compile command consumption** (turning compile commands into a clang compilation database / passing them to an analyzer)

These three things have **different lifecycles**:

- Capture is one-shot -- once it runs, you have the data
- Parsing operates on **existing data**; it can be done repeatedly, and you can parse once and consume many times
- Consumption targets differ (clang database / clang itself / analyzer frontend)

This is exactly how industry static analysis tools are layered:

```
configure → configure "which compilers count as compile commands" (configure once, reuse for a long time)
build → capture (writes build-log.txt + an intermediate directory)
translate → consume (translate compile commands into analyzer invocations)
analyze → analyze
```

**The input and output of every step are files on disk**, not shared state in memory.
That makes each step independently debuggable and independently re-runnable -- this is the part worth borrowing.

---

## 2. Target architecture

```
┌─────────────────────────────────────────────────────────────────┐
│ ccap.exe (entry point / CLI) │
│ run → calls ccap_capture │
│ scan → calls ccap_capture │
│ compdb → calls ccap_compdb │
└───────────────┬──────────────────────────────┬──────────────────┘
 │ │
 ┌───────────▼──────────┐ ┌────────────▼─────────────┐
 │ ccap_capture │ │ ccap_compdb │
 │ (static lib + so/dll) │ │ (static lib + so/dll) │
 │ │ │ │
 │ - process capture │ │ - compiler recognition │
 │ Win: DEBUG_PROCESS │ │ - command parsing │
 │ Lin: LD_PRELOAD │ │ - filtering / aliases │
 │ - script static analysis│ │ - clang database gen │
 │ - unified event model│ │ - compiler adapters │
 └───────────────────────┘ └──────────────────────────┘
 │ │
 └──────────────┬───────────────┘
 │
 ┌──────────▼──────────┐
 │ ccap_core │
 │ (pure, cross-platform, no platform)│
 │ - event model │
 │ - JSON │
 │ - command line splitting│
 │ - path normalization│
 │ - static analysis │
 │ - filters / tree │
 └─────────────────────┘
```

### Why this split

| Module | Responsibility | Dependency |
|---|---|---|
| `ccap_core` | Pure data and pure algorithms | None (STL only) |
| `ccap_capture` | The parts that need OS capabilities | `ccap_core` + platform APIs |
| `ccap_compdb` | Pure text processing + compiler knowledge | `ccap_core` |
| `ccap` (exe) | CLI glue | All of the above |

**Key constraint**: `ccap_compdb` **does not depend on** `ccap_capture`.
It only consumes "a list of command lines" and "a working directory"; it does not care where those commands came from.

This brings a direct benefit: **you can use compile commands produced by other tools**
(CMake's `CMAKE_EXPORT_COMPILE_COMMANDS`, ninja's `-t commands`)
without having to run a capture yourself.

---

## 3. Module one: ccap_capture (wrapping existing capabilities)

### Artifacts

- `ccap_capture` static library (for internal linking)
- `ccap_capture.so` / `ccap_capture.dll` (**dynamically loadable by external programs**)

### Why also ship so/dll

The user explicitly asked to "produce so and dll". Use cases:

1. **Embedding**: other tools (IDE plugins, CI scripts) want to use our capture capability
 without recompiling the whole exe
2. **Multi-language bindings**: Python/Go/Rust call it through FFI
3. **Long-term evolution**: a stable ABI, so the main program can change freely

### C ABI

A shared library must export a **stable C interface** (the C++ ABI is not reusable across compilers):

```c
// ccap_capture.h
typedef struct ccap_session ccap_session; // opaque handle

ccap_session* ccap_session_create(void);
void ccap_session_destroy(ccap_session* s);

// Run one capture; events are written to the callback as JSON lines
typedef void (*ccap_event_cb)(void* user, const char* json_line, size_t len);
typedef void (*ccap_note_cb)(void* user, const char* note);

int ccap_session_run(ccap_session* s,
 const char* target,
 const char* const* args, size_t nargs,
 const ccap_run_options* opts,
 ccap_event_cb on_event,
 ccap_note_cb on_note,
 void* user);
```

This way Python can use `ctypes`, Go can use `cgo`, and Rust can use `bindgen` directly.

---

## 4. Module two: ccap_compdb (compile command recognition and parsing)

### 4.1 Full compiler list

**Following the `--list-compiler-types` style of industry tools**:
every tool has a "compiler type" table, where each type describes
the pattern of compiler executable names, argument translation rules, system header paths, and so on.

Our counterpart:

```
CompilerKind:
 gcc gcc, g++, cc, c++, gcc-<ver>, g++-<ver>, <target>-gcc...
 clang clang, clang++, clang-<ver>, clang-cl
 msvc cl, cl.exe
...
```

Each kind carries a **matching rule**:

- Exact names (`gcc`, `g++`, `clang`, `cl`)
- Prefix patterns (`arm-none-eabi-g++` → a cross compiler, still the gcc kind)
- Suffix patterns (`gcc-11` → a version suffix)

### 4.2 Symlink and alias mutual recognition

**This is a core feature the user explicitly requested.**

Real-world situations:

```
/usr/bin/gcc -> /usr/bin/gcc-11 (hard link / symlink)
/usr/bin/cc -> /usr/bin/gcc
/usr/bin/c++ -> /usr/bin/g++
/usr/bin/g++ -> /usr/bin/g++-11
```

Toolchains may also define their own aliases:

```
/opt/toolchain/bin/arm-none-eabi-g++ -> /opt/toolchain/bin/arm-none-eabi-gcc
```

**Rules**:

1. Resolve the **real path** (`realpath` / `GetFinalPathNameByHandle`)
2. All names that share the same real path are **aliases of each other**
3. If **any one** member of the alias set matches a compiler kind, the whole group counts as that kind
4. Conversely, if an alias is configured as "a compiler to capture",
 all of its aliases must be captured too (we must not miss it just because the build system used `cc`)

The corresponding behavior in industry tools: `configure --compiler /usr/bin/gcc` configures gcc,
and a build that invokes `cc` may also be recognized (depending on the implementation),
but **the same name at a different path** (`arm-none-eabi-g++` vs `g++`) must be configured separately.

Our approach is stricter: symlinks are **automatically** recognized as mutual aliases,
because this is a deterministic fact at the file system level, not a guess.

### 4.3 Command sources

Three sources, all of which must be parseable:

| Source | Format | Description |
|---|---|---|
| **Runtime capture** | `ccap`'s own event stream | The result of `ccap run <build>` |
| **Files** | Text file, one command per line | Commands the user pasted out of a log |
| **Build log build-log** | `EXECUTING '...'` / `COMPILING '...'` / `CWD =...` | Read build-log.txt directly |

The common build-log.txt format (the output of a static analysis tool's build wrapper):

```
EXECUTING 'make all '
EXECUTING '/bin/sh -c cd qmake && make '
CWD = /export/home/acc/test-packages/qt-x11-free-3.3.2/qmake
COMPILING '/export/home/acc/prevent/bin/<wrapper> g++ -c -o property.o -I.... property.cpp '
```

Rules:
- `EXECUTING` = a command executed by the build system, **unrelated to compilation**
- `COMPILING` = an invocation recognized as a compiler
- `CWD = <path>` = the working directory for subsequent commands

**Our implementation must be able to read this format**, so users can directly consume the output of an off-the-shelf build wrapper.

### 4.4 Filtering: keep only genuine compile commands

What to filter out:

- **Scripts** (`sh`, `bash`, `cmd.exe`, `powershell`, `.bat`, `.sh`, `.ps1`)
- **Linkers** (`ld`, `link`, `collect2`, `ld.lld`) -- configurable
- **Assemblers** (`as`, `ml64`) -- configurable
- **Archivers** (`ar`, `lib`) -- configurable

What to keep: **genuine compile invocations** (have a source file, have `-c`, go through the compiler frontend).

Decision logic (by priority):

1. Compiler kind matches (the compiler list in the configuration) → it is a compile command
2. The command line has `-c` and has a source file → it may be a compile command
3. The exclusion list matches → it is not
4. None of the above → it is not

### 4.5 Extracting header files, macros, and all kinds of information

Extracted from the compile command:

| Category | Source | Example |
|---|---|---|
| **include paths** | `-I<dir>` / `-I <dir>` / `/I<dir>` / `-isystem <dir>` | `-I/usr/include` |
| **macro definitions** | `-D<name>[=<val>]` / `/D<name>[=<val>]` | `-DFOO=1` |
| **system headers** | The compiler's built-in search paths + `-isystem` | gcc's `/usr/lib/gcc/x86_64-linux-gnu/11/include` |
| **source files** | Non-option arguments whose extension is `.c/.cc/.cpp/.cxx/.m/.mm` | `main.cpp` |
| **output files** | `-o <file>` | `main.obj` |
| **language standard** | `-std=c++17` / `/std:c++17` | |
| **other options** | Everything else | |

**Where system headers come from**:
- The compiler knows them itself (`gcc -E -v` prints them), but we do not run the compiler
- So: rely on **configuration** (each compiler kind gets a set of default system include paths)
- The user can override them

### 4.6 Output: the clang compilation database

`compile_commands.json` format (the official clang specification):

```json
[
 {
 "directory": "/home/user/proj/build",
 "command": "c++ -I../include -DFOO=1 -c../src/main.cpp",
 "file": "/home/user/proj/src/main.cpp"
 },
 {
 "directory": "...",
 "arguments": ["c++", "-I../include", "-c", "../src/main.cpp"],
 "file": "..."
 }
]
```

Notes:
- `command` and `arguments` are mutually exclusive (we use `arguments`, because escaping is easier to handle)
- `directory` is the **working directory** (required -- relative `-I` paths are resolved against it)
- `file` is the **absolute path** of the source file (the specification recommends an absolute path)

---

## 5. Module three: compiler adapters (the consume side)

### 5.1 Interface

```cpp
// ccap_compdb/compiler_adapter.h
class CompilerAdapter {
public:
 virtual ~CompilerAdapter() = default;

 // Can this adapter handle this command?
 virtual bool matches(const CompileCommand& cmd) const = 0;

 // Translate the native compile command into "arguments for the analyzer"
 // (analyzer style: --sys_include / --preinclude / -D / -I...)
 virtual std::vector<std::string> translate(
 const CompileCommand& cmd,
 const AdapterContext& ctx) const = 0;

 // The kind of this adapter
 virtual CompilerKind kind() const = 0;
};
```

### 5.2 One implementation per compiler family, isolated from each other

```
adapters/
 gcc_adapter.cpp GCC / G++ / cross compilers
 clang_adapter.cpp Clang / Clang++
 msvc_adapter.cpp MSVC cl.exe
 generic_adapter.cpp Fallback: generic extraction of -I/-D only
```

**What "isolated from each other" means**:
- Each adapter only understands its own option syntax
- Adding a new compiler = adding a new file, **without touching any other file**
- Adapters never call each other

### 5.3 Configuration-driven

Configuration file (JSON):

```json
{
 "capture": {
 "compilers": [
 { "kind": "gcc", "paths": ["/usr/bin/gcc", "/usr/bin/g++"], "enabled": true },
 { "kind": "clang", "paths": ["/usr/lib/llvm-15/bin/clang"], "enabled": true }
 ],
 "exclude": ["sh", "bash", "cmd.exe", "ld", "as", "ar", "collect2"],
 "include_aliases": true
 },
 "system_includes": {
 "gcc": ["/usr/lib/gcc/x86_64-linux-gnu/11/include", "/usr/include/c++/11"]
 },
 "output": {
 "format": "clang",
 "path": "compile_commands.json"
 }
}
```

**Key point**: the `compilers` list supports **wildcard prefixes**, for example `"prefix": "arm-none-eabi-"`,
which matches all `arm-none-eabi-*` compilers.

---

## 6. CLI design

```
ccap run <script|exe> [args...] capture (existing)
ccap scan <script> static analysis (existing)
ccap compdb <subcommand> compilation database tool (new)

ccap compdb capture <build-cmd...> run one build and grab the compile commands
ccap compdb parse <file...> parse compile commands out of files
ccap compdb list list the known compiler types (full list)
ccap compdb aliases <compiler-path> look up all aliases of one compiler
ccap compdb show <file...> show details of the parsed compile commands
ccap compdb export --out <file> generate compile_commands.json
```

### Input sources are composable

```
# From a runtime capture
ccap compdb capture -- make -j8

# From a file
ccap compdb parse build.log

# From a build log
ccap compdb parse build-int/build-log.txt

# From an existing compile_commands.json
ccap compdb parse compile_commands.json
```

---

## 7. Implementation order

| Step | Content | Acceptance criterion | Status |
|---|---|---|---|
| 1 | Extract `ccap_core` + turn `ccap_capture` into a library + C ABI + so/dll | The existing 73 tests must not regress; the so/dll can be loaded by `ctypes` and run one capture | ✅ Done |
| 2 | Compiler kind table + matcher | `ccap compdb list` outputs the full list | ✅ Done |
| 3 | Alias resolution (symlink mutual recognition) | `ccap compdb aliases /usr/bin/gcc` lists `cc` / `gcc-11`, etc. | ✅ Done |
| 4 | Command parser (three sources) | Can read build-log.txt, plain text, and JSON | ✅ Done (4 sources) |
| 5 | Filtering + header/macro extraction | `ccap compdb show` shows structured details | ✅ Done |
| 6 | clang database export | Produces a valid `compile_commands.json` | ✅ Done |
| 7 | Compiler adapters (gcc/clang/msvc) | Argument translation is correct; adapters are isolated from each other | ✅ Done (+ iar / generic) |
| 8 | Configuration parsing + CLI integration | Usable end to end | ✅ Done |
| 9 | Unify naming conventions for LLVM/clang | camelBack / PascalCase on the C++ side, snake_case for files, C ABI keeps snake_case | ✅ Done |
| 10 | Naming gate | Deliberately injected violations are reported; a missing tool / compile error counts as a failure | ✅ Done (`tests/check_naming.py`) |
| 11 | shim ABI symbol table gate | `nm -D` asserts that the 9 exec/spawn symbols exist | ✅ Done (in `run_tests_linux_depth.sh`) |
| 12 | Dual-branch gate pass | Both sides of `#if` / `#else` are checked | ✅ Done (native + `-U_WIN32`) |
| 13 | Unify internal struct fields to camelBack | `RunOptions` / `cli::Options` no longer paper over it with exemptions | ✅ Done |
| 14 | String literal diff in the rename script | Confirm that only include paths changed, and that contracts such as JSON keys were left untouched | ✅ Done (found and fixed `system_includes`) |

### The actual directory layout after implementation

```
modules/
 ccap_core/core/ pure algorithms, no platform API
 event.* json.* json_parser.*
 command_line.* normalize.*
 script_scanner.* powershell_scanner.*
 tree.* filter.*
 ccap_capture/
 capture/ platform implementation + C ABI
 platform.* windows_debugger.* peb_reader.*
 linux_preload.* linux_shim.* capi.cpp
 include/ccap_capture.h stable C ABI (the only external contract)
 ccap_compdb/
 compdb/ recognition / parsing / filtering / export / configuration
 compiler_table.cpp alias_resolver.cpp command_parser.cpp
 compile_command.cpp filter_rules.cpp compdb_exporter.cpp config.cpp
 adapters/ gcc / clang / msvc / generic (isolated from each other)
 include/ccap_compdb.h
 ccap/app/ CLI entry point (main / cli / compdb_cmd)
```

CMake targets:

| target | Type | Windows artifact | Linux artifact |
|---|---|---|---|
| `ccap_core` | STATIC | `ccap_core.lib` | `libccap_core.a` |
| `ccap_capture` | STATIC | `ccap_capture.lib` | `libccap_capture.a` |
| `ccap_capture_shared` | SHARED | `shared/Release/ccap_capture.dll` | `shared/libccap_capture.so` |
| `ccap_compdb` | STATIC | `ccap_compdb.lib` | `libccap_compdb.a` |
| `ccap_compdb_shared` | SHARED | `shared/Release/libccap_compdb.dll` | `shared/libccap_compdb.so` |
| `ccap` | EXE | `Release/ccap.exe` | `ccap` |
| `ccap_preload` | SHARED (Linux) | -- | `ccap_shim.so` |
| `test_cmdline` / `test_compdb` / `test_capi` | EXE | `Release/` | `build-linux/` |

> The shared library output directory **must be separate from the static library** --
> MSVC's import lib is also called `<name>.lib`, and the Linux shared library is also called `lib<name>.so`;
> putting them together makes them overwrite each other (on Windows this once caused `run_capture` not to link,
> and on Linux it once caused `test_capi` not to find `libccap_capture.so`).

### Problems found and fixed during implementation

| Problem | Root cause | Fix |
|---|---|---|
| When the build log has a UTF-8 BOM, **0 commands are parsed** and no error is reported | The BOM makes the `rfind("EXECUTING ", 0)` prefix match fail | `read_file` strips the BOM uniformly, solving it for all consumers in one place |
| The symlink alias test fails on Linux | The test first does `rename(real, alias)` to move the source file away, leaving the symlink dangling | The test now creates the file first, then does `ln -sf` |
| `test_capi` is not registered in CTest | Only manual verification, no gate | Add `add_test(NAME capi...)`, passing different targets per platform |
| Linux `libccap_capture.so` is not found | The shared library output directory was not set to `shared/` | Aligned with the Windows branch |
| Linking a Linux static library into a shared library reports `relocation R_X86_64_PC32... can not be used when making a shared object` | The static libraries were not built with PIC | Set `POSITION_INDEPENDENT_CODE ON` for `ccap_core` / `ccap_capture` / `ccap_compdb` |
| The Windows shared library overwrites the static library of the same name | MSVC's import lib is also called `<name>.lib` | Move the shared library output to `build/shared/Release/` |
| The bulk rename changed `posix_spawn` to `posixSpawn` | The style unification collateral-damaged a dynamic symbol name | Restore the symbol names + add a `nm -D` symbol table gate (see §8.4) |
| The bulk rename changed the configuration JSON key `system_includes` to `systemIncludes` | The script cannot tell an "identifier" from a "data contract" | Restore the key name + adopt a verification method that diffs string literals file by file (see §8.8) |
| Internal struct fields were always snake_case | They were shoved into the `.clang-tidy` exemption list, using exemptions to hide "not changed" | Actually change the internal fields to camelBack; keep only ABI in the exemption list (see §8.7) |
| The gate cannot see violations inside the `#else` branch | clang-tidy only checks the code that still exists after preprocessing | Run each file twice (native + `-U_WIN32`, see §8.6) |
| The naming gate shows green when there is a compile error | clang-tidy emits no naming warnings when it hits a compile error | A compile error counts as a failure (exit code 1), see §8.5 |
| When the naming gate is given multiple TUs at once, the latter half reports "file not found" | clang-tidy merges the compilation context, and `--extra-arg` only applies to the first file | Execute file by file |
| `test_capi` fails to compile with C4819 | MSVC reads UTF-8 source files using the system code page (936/GBK) | Add `/utf-8` for `test_capi` (the other targets already had it) |
| The tree renderer loses grandchild processes | `std::move` moves a node out of the pool while the pid index still points at the moved-from node | Switch to a stable node pool + child index; add a regression assertion that "each event is rendered exactly once" |
| PowerShell misses the command inside `Invoke-Expression "git status"` | Only cmdlet names were recognized; string literal arguments were not parsed | Parse the **literal** arguments of `Invoke-Expression` / `iex`; the variable form is not guessed |
| PowerShell treats hash table keys and splatting as commands | `@{ Path = "x" }` / `@params` are data, not commands | Exclude hash table keys and splatting tokens |
| PowerShell treats here-string content as commands | The content is text, not code | Treat here-strings as text; the terminator does not produce a pseudo-command, and line numbers are preserved |
| The naming gate goes red in CTest (`'cstdio' file not found`) | The `INCLUDE` environment variable points to an **uninstalled** MSVC version; the old logic "use it if INCLUDE is non-empty" → the only include path is dead → compile error → no naming warnings emitted → the gate silently degrades into an empty check | Merge three sources: environment variables + `vswhere` (MSVC toolset) + the registry (Windows SDK), discarding only directories that do not exist |

---

## 8. Naming conventions and the ABI boundary

For the complete rules see [`NAMING-CONVENTION.md`](NAMING-CONVENTION.md). Here we only record the **design decisions**
and that single most important boundary.

### 8.1 The C++ side: LLVM / clang style

| Category | Style | Example |
|---|---|---|
| class / struct / enum | `PascalCase` | `CommandEvent`, `CompilerKind::Gcc` |
| function / method / variable / parameter | `camelBack` | `splitCommandLine` |
| private member | `camelBack` + trailing `_` | `member_` |
| constant | `k` + `PascalCase` | `kMaxDepth` |
| namespace | lowercase | `ccap`, `ccap::compdb` |
| source file | `snake_case.cpp` | `command_parser.cpp` |

Enforced by `.clang-tidy`'s `readability-identifier-naming`,
driven by `tests/check_naming.py`.

### 8.2 File renaming

13 source files were renamed to snake_case following the clang convention, with the **file name corresponding to the main class name**:

| Before | After |
|---|---|
| `core/cmdline.*` | `core/command_line.*` |
| `core/json_parse.*` | `core/json_parser.*` |
| `core/ps_scan.*` | `core/powershell_scanner.*` |
| `core/script_scan.*` | `core/script_scanner.*` |
| `compdb/compdb_export.cpp` | `compdb/compdb_exporter.cpp` |
| `capture/win_debugger.*` | `capture/windows_debugger.*` |
| `capture/win_peb.*` | `capture/peb_reader.*` |

The bulk rename touched 341 identifiers. **String literals, JSON field names, and include paths**
are all protected -- the output contract did not change.

### 8.3 The boundary: which names must not follow the style

This is the most important point in this section:

> **ABI names never take part in style unification.**

Three categories:

| Category | Example | Why it must not change |
|---|---|---|
| C ABI functions / types / enum values / fields | `ccap_session_run`, `ccap_run_options` | It is an external contract; changing it breaks the interface |
| glibc hook symbols | `posix_spawn`, `posix_spawnp`, `execve`... | They are **dynamic symbol names**; changing them silently disables the hook |
| Windows NT ABI fields | `PROCESS_BASIC_INFORMATION.InheritedFromUniqueProcessId` | They must correspond one-to-one with the struct layout of ntdll |

### 8.4 The `posix_spawn` incident (something that really happened)

During the bulk rename, `posix_spawn` / `posix_spawnp` in the Linux shim were changed to
`posixSpawn` / `posixSpawnp`.

**Symptom**: `make` only captured the root event; all child processes were lost.

**Why there was no error at all**: the shim loaded normally, exported normally, and exited normally.
It simply exported `posixSpawn`, which **nobody called**, while `posix_spawn` still pointed to glibc's real implementation.
`LD_PRELOAD` works by **snatching symbols by name** -- with the wrong name, it cannot snatch them.

**Fix**:

1. Restore the exact symbol names (9 exec/spawn symbols).
2. Add a **symbol table gate**: in `tests/run_tests_linux_depth.sh`, use
 `nm -D --defined-only build-linux/ccap_shim.so` to assert that these 9 symbols exist.

**Lesson -- this kind of bug only shows up on a specific code path; you cannot catch it by just "running it once and seeing no error".**
The value of a gate lies in turning "the things that must stay unchanged" into something assertable.

### 8.5 Three principles of the naming gate

The design of `tests/check_naming.py` is not "run clang-tidy and see whether it prints anything",
but rather **distinguishing "not checked" from "checked and no violations"**:

1. **clang-tidy missing → exit code 2** (failure), not a skip.
 A gate that "skips when it cannot run" is equivalent to no gate -- it will keep showing green.
2. **Compile error → exit code 1** (failure), and print "the naming check is **incomplete**".
 clang-tidy **emits no naming warnings** when a file has compile errors;
 "0 warnings" does not mean "compliant with the convention".
3. **Execute file by file**, not by passing all TUs at once.
 When multiple files are passed at once, clang-tidy merges the compilation context,
 and `--extra-arg` only takes effect for the first file → subsequent files fail to compile → the check is skipped.

In addition, you must **verify that the gate itself can go red**:
deliberately inject a `bad_variable_name` and confirm that it is really reported:

```
FAIL: 1 处命名违规
modules\ccap_core\core\event.cpp:68:9 local variable 'bad_variable_name'
```

A gate that has never been verified to go red is no different from no gate at all.

### 8.6 Dual-branch pass: the code in `#else` had never been checked

The point above solves the problem "across files". There is also a problem "within a file":

> **clang-tidy only checks what still exists after preprocessing.**

In code like `#if defined(_WIN32)... #else... #endif`,
on Windows only the `_WIN32` branch survives and the `#else` branch is preprocessed away --
**that code had never been analyzed**.

Measured: in `normalize.cpp`, the `#else` branch hid a `size_t name_start`,
and the gate reported "0 violations".

Even more insidious: **MSVC's SDK headers implicitly bring in `_WIN32`**,
so even without passing `-D_WIN32` explicitly, the Windows branch is still taken.
You must use `-U_WIN32` to **cancel** it.

Fix: run each file **twice**.

| pass | Arguments | Coverage |
|---|---|---|
| native | default | The `#if` branch of the current platform |
| no-win32 | `--extra-arg -U_WIN32` | The `#else` branch on the other side |

The no-win32 pass is guaranteed to produce compile errors (that side's platform-specific headers do not exist on this machine),
but that **does not affect the conclusion** -- the compiler has already checked everything it could parse before reporting the errors.
So: **compile errors in the native pass count as a failure, while those in the no-win32 pass are only recorded as a note**.

### 8.7 The exemption rule must be able to answer "why did I exempt this"

The early version also shoved the snake_case fields of the internal struct `ccap::RunOptions`
into the `.clang-tidy` exemption list (`PublicMemberIgnoredRegexp`),
and the gate immediately turned green.

But those fields **are not ABI**:

| Struct | Is it ABI | Style |
|---|---|---|
| `ccap_run_options` (C ABI) | ✅ External contract | snake_case |
| `ccap::RunOptions` (internal C++) | ❌ | camelBack |
| `ccap::cli::Options` (internal C++) | ❌ | camelBack |

Moreover, exemptions match **by name** -- once `timeout_ms` is in the list,
**any** field named `timeout_ms` is exempt, including the internal ones.
When the same name appears in both the ABI and an internal struct, exemptions cannot tell them apart at all.

The correct approach: actually change the internal fields to camelBack, and keep only the ABI ones in the exemption list.

### 8.8 A bulk rename must not touch data contracts

A bulk rename script cannot distinguish an "identifier" from a "data contract inside a string literal",
so it also changed the JSON keys in the configuration file:

```cpp
"system_includes": {... } // correct
"systemIncludes": {... } // broken
```

The parser reads `"system_includes"` -- the system header paths in the configuration are **silently lost**.

**Lesson -- JSON keys, command-line options, output field names, and file paths are not identifiers;
they are interfaces. Changing them breaks compatibility.**

Verification method: **diff all string literals file by file** before and after the rename,
and confirm that only the include paths changed. The single loss this diff turned up was exactly that JSON key.

### 8.9 Extra pitfalls on Windows

| Pitfall | Symptom | Fix |
|---|---|---|
| MSVC's include paths are not in `compile_commands.json` | `'cstdint' file not found` | Merge three sources -- environment variables + `vswhere` + the registry -- to add `-isystem` |
| CMake records MSVC-syntax `-I<path>` (single dash) | The project's own headers are all not found | Additionally add clang-syntax `-I` |
| `_WIN32` is not defined | Reports "Linux headers not found", while the root cause is "the Windows macro is not defined" | Add `-D_WIN32` |
| `INCLUDE` contains **stale** MSVC version directories | Reports a pile of file-not-found errors, drowning the real problem | Skip directories that do not exist, and print a note |
| **`INCLUDE` is non-empty but consists entirely of stale directories** | The only include path is dead → compile error → **no naming warnings emitted** → the gate shows "0 violations" | Merge all sources (do not trust only `INCLUDE`), discarding only directories that do not exist |
| The Windows SDK is installed **outside** the VS installation directory | `vswhere -find` always returns empty for it | Get it from the registry key `KitsRoot10`, taking the SDK with the highest version number |

> The last two rows are two sides of the same kind of problem: **the gate's own inputs must also be existence-checked**.
> "INCLUDE is not set" and "INCLUDE points to a deleted directory" look exactly the same in the code,
> so the fix cannot be just adding an `if not INCLUDE` branch.

---

## 9. Risks and trade-offs

| Risk | Countermeasure |
|---|---|
| Symlink resolution on Windows requires `GetFinalPathNameByHandle` | When there is a `CreateFile` permission problem it degrades to path normalization, which is explained in the notes |
| System header paths vary from machine to machine | They go through configuration; the defaults are only a fallback; the notes explicitly mark "this is a configured value, not a compiler-measured value" |
| The compiler kind table is never complete | Provide a `generic` fallback + allow custom kinds in the configuration file |
| MSVC mixes `/D` and `-D` | The msvc adapter handles it independently, without polluting the gcc adapter |
| A BOM in the file causes prefix matching to silently fail | `read_file` strips the BOM uniformly; the parser's prefix matching only applies to **content** |
| Style unification collateral-damages ABI names | The boundary in §8.3 + the symbol table gate; ABI names do not enter the style unification flow |
| The naming gate shows false green when tools are missing / there are compile errors | Missing tool → exit code 2; compile error → exit code 1 |
| The naming gate stays permanently red in CTest due to a missing clang-tidy | Exit code 2 → CTest `SKIP_RETURN_CODE 2`, so it is displayed as skipped while the log states "not checked" |
| The naming gate shows false red in CTest due to environment differences | Merge include paths from multiple sources; execute file by file; dual pass with native / `-U_WIN32` |

---

## 10. References

- **The `build-log.txt` format of static analysis build wrappers** (three kinds of line: `EXECUTING` / `COMPILING` / `CWD =`) --
 this is the input format recognized by this project's `parseBuildLog()`.
 For a description of the format see the cited sources of the research report
 [`RESEARCH-COMMAND-CAPTURE-2026-10-27.md`](RESEARCH-COMMAND-CAPTURE-2026-10-27.md).
- **Compiler name matching is exact and case-sensitive** -- this is why we use symlink mutual recognition (grouping by real path) instead of fuzzy name matching.
- clang JSON Compilation Database specification:
 <https://clang.llvm.org/docs/JSONCompilationDatabase.html>
