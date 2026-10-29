# CommandCapture Naming Convention (modeled on LLVM / clang)

<!-- i18n:begin -->
中文 | [**English**](../zh/NAMING-CONVENTION.md)
<!-- i18n:end -->

This document defines the naming rules for every identifier in this project.

Reference sources:

- LLVM Coding Standards -- <https://llvm.org/docs/CodingStandards.html>
- clang-tidy `readability-identifier-naming` --
 <https://releases.llvm.org/18.1.6/tools/clang/tools/extra/docs/clang-tidy/checks/readability/identifier-naming.html>

The rules are **enforced** by `.clang-tidy` (see "How to keep from drifting" at the end of this document).

---

## 1. Master Table

| Category | Style | Example |
|---|---|---|
| class / struct / enum | `PascalCase` | `CommandEvent` |
| function / method | `camelBack` | `splitCommandLine` |
| local variable / parameter | `camelBack` | `sourceFile` |
| member variable | `camelBack` | `eventCollector` |
| static member variable | `camelBack` | `instanceCount` |
| namespace | lowercase | `ccap`, `ccap::compdb` |
| private member | `camelBack` + trailing `_` | `member_` |
| constant | `k` + `PascalCase` | `kMaxDepth` |
| enum value | `PascalCase` | `CompilerKind::Gcc` |
| macro | `UPPER_CASE` | `CCAP_PLATFORM_WINDOWS` |
| source file | `snake_case.cpp` | `command_parser.cpp` |
| header file | `snake_case.h` | `command_parser.h` |
| C API function | `snake_case` + module prefix | `ccap_session_run` |
| C API struct field | `snake_case` | `ccap_run_options.timeout_ms` |

**Private members** get an extra trailing underscore: `name_` (clang uses a trailing `_` to mark non-public members).

> Note the last row: **only C API struct fields** stay snake_case.
> Internal C++ structs (`ccap::RunOptions` / `ccap::cli::Options`)
> use camelBack like every other C++ identifier.

---

## 2. Why the C API Stays snake_case

The C ABI is an **external contract** and cannot be changed on a whim. LLVM's own C API
(`clang-c/Index.h`) uses exactly a `clang_` prefix + snake_case:

```c
CXIndex clang_createIndex(int excludeDeclarationsFromPCH);
void clang_disposeIndex(CXIndex index);
```

So writing `ccap_session_create` / `ccap_session_run` **follows LLVM convention**;
it is not an exception.

On the C++ side, everything is `camelBack` / `PascalCase`.

### 2.1 The dividing line: which names do not participate in style unification

> **ABI names never participate in style unification.**

| Category | Example | Why it cannot be changed |
|---|---|---|
| C ABI function / type / enum value / field | `ccap_session_run`, `ccap_run_options.timeout_ms` | External contract -- changing it breaks the interface |
| glibc hook symbol | `posix_spawn`, `posix_spawnp`, `execve`... | **Dynamic symbol names** -- renaming them silently breaks the hook |
| Windows NT ABI field | `PROCESS_BASIC_INFORMATION.InheritedFromUniqueProcessId` | Must correspond one-to-one with the struct layout in ntdll |

**A real incident**: a bulk rename changed the Linux shim's `posix_spawn` into `posixSpawn`.
The shim then exported `posixSpawn`, so glibc's `posix_spawn` was never intercepted --
and since GNU make prefers `posix_spawn`, make's child processes were **all missed**,
**with no error reported at all**.

This kind of bug only shows up on specific code paths, so it must be guarded by a
**symbol-table assertion** (`tests/run_tests_linux_depth.sh` uses `nm -D` to assert that
those 9 symbols exist).

---

## 3. Constants Use the `k` Prefix

The clang way (LLVM core uses `UPPER_CASE`, clang itself uses the `k` prefix):

```cpp
static constexpr int kMaxDepth = 64;
static const char* kTranslatorPrefixes[] = {...};
```

The criterion: **anything whose value is fixed at compile time and should not be modified**
uses the `k` prefix.

---

## 4. Namespaces Use Lowercase

LLVM's namespaces are `llvm`, `clang`, `mlir` -- all **lowercase**,
because they are **acronyms** (initialisms), not ordinary words.

The equivalents in this project:

```cpp
namespace ccap { // 顶层
namespace ccap::compdb { // 编译数据库子模块
namespace ccap::compdb::adapters { // 编译器适配器
}
```

`.clang-tidy` configures `NamespaceCase: lower_case`,
and uses `NamespaceIgnoredRegexp: '^[a-z][a-z0-9]*$'` to let purely lowercase namespaces through.

---

## 5. Private Members Carry a Trailing Underscore

The clang convention: **private members** (and other non-public members) are marked with a
trailing underscore:

```cpp
class EventCollector {
private:
 std::vector<CommandEvent> events_;
 uint64_t nextSeq_ = 1;
};
```

`.clang-tidy` must configure `PrivateMemberSuffix: '_'`,
otherwise `events_` is judged as "invalid case style" --
the trailing `_` means it is no longer pure camelBack.

**Without this rule, the entire private member category becomes "always failing"**,
and the error message looks like the naming is genuinely wrong.

---

## 5. File Names Use snake_case

The clang file naming rules (LLVM Coding Standards):

> File names are all lowercase, with words separated by underscores, and no underscores between words.

Examples: `clang/Frontend/CompilerInstance.cpp`, `clang/Basic/Diagnostic.h`

This project:

```
modules/ccap_core/core/command_parser.cpp
modules/ccap_core/core/command_parser.h
modules/ccap_compdb/compdb/compiler_table.cpp
modules/ccap_compdb/adapters/gcc_adapter.cpp
```

**One class per file**, with the file name matching the main class name (the clang convention
is a snake_case file name with a PascalCase class name):

| File | Main class |
|---|---|
| `compiler_table.cpp` | `CompilerTable` |
| `alias_resolver.cpp` | `AliasResolver` |
| `command_parser.cpp` | `CommandParser` |
| `compile_command.cpp` | `CompileCommand` |
| `filter_rules.cpp` | `FilterRules` |
| `adapter_registry.cpp` | `AdapterRegistry` |
| `gcc_adapter.cpp` | `GccAdapter` |
| `clang_adapter.cpp` | `ClangAdapter` |
| `msvc_adapter.cpp` | `MsvcAdapter` |
| `generic_adapter.cpp` | `GenericAdapter` |

---

## 6. Enums

clang's `enum class` enum values use `PascalCase`:

```cpp
enum class CompilerKind {
 Unknown,
 Gcc,
 Clang,
 Msvc,
 Iar,
 Generic,
};
```

Note that it is not `COMPILER_KIND_GCC` (that is the C style / Windows SDK style).

---

## 7. How to Keep From Drifting

### 7.1 `.clang-format`

Unified formatting (indent width, line width, pointer alignment, etc.):

```bash
clang-format -i modules/ccap_core/core/*.cpp
```

### 7.2 `.clang-tidy`

`readability-identifier-naming` turns the table above into an **executable check**:

```bash
clang-tidy -p build modules/ccap_core/core/command_parser.cpp
```

Non-compliant names are reported as warnings -- this does **not** rely on people being
disciplined.

### 7.3 A naming check cannot rely on "there were no errors"

`clang-tidy`'s `readability-identifier-naming` has a pitfall:
**if the rule for a given kind is not configured, it does not check that category** --
it looks like "no errors", but it actually means "not checked".

So the way to verify must be:

```bash
# 故意写错一个名字, 看它报不报
clang-tidy... # 期望: 报出命名违规
```

rather than "run it once and call it a pass if there are no errors".

### 7.4 Both Sides of `#if` Must Be Covered

**clang-tidy only checks what still exists after preprocessing.**

```cpp
#if defined(_WIN32)
 // Windows 分支
#else
 size_t name_start = 1; // ← 违规, 但 Windows 上这行不存在
#endif
```

Running on Windows, the `_WIN32` branch survives and the `#else` branch is preprocessed away --
**that code has never been analyzed**.

Moreover, **MSVC's SDK headers implicitly bring in `_WIN32`**,
so even without explicitly passing `-D_WIN32`, the Windows branch is still taken.
To cover the other side you must use `-U_WIN32` to **cancel** it.

Therefore the gate runs **twice** for every file:

| pass | Arguments | Coverage |
|---|---|---|
| native | default | the `#if` branch of the current platform |
| no-win32 | `--extra-arg -U_WIN32` | the `#else` branch on the other side |

The no-win32 pass is bound to produce compile errors (the platform-specific headers on that
side do not exist on this machine), but that does not affect the conclusion -- the compiler
has already checked everything it could parse before erroring out.
→ So **only compile errors in the native pass count as failure**.

### 7.5 Exemption Rules Must Answer "Why am I exempting this?"

The `*IgnoredRegexp` entries in `.clang-tidy` are matched **by name** --
once a name is on the exemption list, **any** identifier with the same name is exempt.

So the exemption list should only contain names that are **genuinely contracts**:

| Should be exempt | Should not be exempt |
|---|---|
| C ABI functions / types / enum values / fields | Same-named fields of internal C++ structs |
| glibc hook symbols (`posix_spawn`, etc.) | Our own functions with the same name |
| Windows NT ABI struct fields | Ordinary internal members |

> The internal struct `ccap::RunOptions` and the C ABI struct `ccap_run_options`
> once had **fields with exactly the same names** (`timeout_ms`, etc.).
> As a result the exemption rule could not tell them apart -- the internal ones were never renamed.
> The correct approach: really change the internal fields to camelBack, and keep the exemptions
> for the ABI ones only.

**Criterion**: before adding something to the exemption list, ask "whose contract is this?"
If you cannot answer, the exemption is **hiding a problem** for you.

### 7.6 How the Gate Is Run

`tests/check_naming.py` is the executor, and it has been **included in CTest** as the `naming`
test:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

```
4/4 Test #4: naming........................... Passed 157.80 sec
```

The VS generator does not produce a `compile_commands.json`, so the script will **automatically
generate a sibling Ninja build** to serve as the compilation database -- it does not interfere
with the build tree the developer is currently using.

The semantics of the exit code:

| Exit code | Meaning | Behavior in CTest |
|---|---|---|
| 0 | The check passed | passed |
| 1 | There are naming violations, or the native pass has compile errors | failed |
| 2 | **The check could not be performed** (clang-tidy missing / compilation database missing) | **skipped** |

Exit code 2 was split out into a separate value precisely so that "clang-tidy is not installed
on this machine" shows up as skipped rather than failed in CI -- but the log explicitly states
"not checked", so nobody is misled into thinking it was checked.

### 7.7 Include Paths Must Merge Multiple Sources

clang-tidy only receives the compile commands from `compile_commands.json`,
and **MSVC's standard header paths are not on the command line** -- they come from the
`INCLUDE` environment variable.

So there is a trap:

> The `INCLUDE` environment variable is **non-empty**, but it points at a toolset version
> that has already been **uninstalled**.

In that situation, if you write it as "use it as long as INCLUDE is non-empty",
clang-tidy cannot find a single standard header → every file fails to compile →
**the naming warnings are never emitted** → the gate reports "0 violations" → green light.

So the include paths must **merge** all sources, discarding only directories that
**do not exist**:

| Source | How to obtain it | Why it is needed |
|---|---|---|
| Environment variable `INCLUDE` | `os.environ` | Accurate inside a vcvars environment |
| MSVC toolset | `vswhere -find VC\Tools\MSVC\**\include` | Fallback when the environment variable is stale |
| Windows SDK | Registry `HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots` → `KitsRoot10` | **The SDK is installed outside the VS installation directory, and `vswhere` structurally cannot find it** |

The registry entry is necessary, not redundant: `vswhere` only knows the Visual Studio
installation tree, while the Windows SDK usually lives in
`C:\Program Files (x86)\Windows Kits\10`, and `vswhere -find` always returns empty for it.

**Lesson -- environment variables are caches that can go stale; the authoritative source is the
real one.** "Use the environment variable if it exists" is the wrong default.

**Corollary -- the gate's own inputs must also be existence-checked.**
"INCLUDE is not set" and "INCLUDE points at a deleted directory" look exactly the same in code,
so the fix cannot be just adding an `if not INCLUDE` branch -- that would only trade one bug
for another.

---

## 8. Known Style Deviations

Places where this project **deliberately** does not follow LLVM style:

| Deviation | Reason |
|---|---|
| Indent with 4 spaces (not LLVM's 2) | The project has always used 4 spaces; it is set explicitly in `.clang-format`, and changing it would churn too much for the benefit |
| Headers use `#pragma once` (LLVM uses include guards) | It is the status quo, and `#pragma once` is available on all target compilers |
| C++17 (LLVM uses C++17 but disables some features) | Kept consistent with LLVM |

**Explicitly followed**: the naming style (the entire content of this document).

Formatting and naming are two separate things -- LLVM itself manages them separately
(`clang-format` handles formatting, `clang-tidy` handles naming).
This project enforces **naming** only and does not enforce formatting, to avoid producing a
large number of meaningless diffs.
