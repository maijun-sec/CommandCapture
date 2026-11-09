# The Output Formats

<!-- i18n:begin -->
中文 | [**English**](../en/OUTPUT-FORMATS-2026-11-05.md)
<!-- i18n:end -->

**Date**: 2026-11-05
**Status**: Implemented, and measured against real toolchains

This document describes what `ccap build` writes, for each language family and each
output format. Every example here is a **real capture** -- the C++ ones from a CMake
build, the Java one from `javac`, the C# one from `dotnet build` -- not a hand-written
illustration.

---

## 1. The Three Formats

| Format | What it is | Who wants it |
|---|---|---|
| `clang` | the compilation database, original commands verbatim | clang tooling, IDEs, `clangd` |
| `clang-cmd` | one rewritten clang command per line | a build system, a shell script, a re-runner |
| `translated` | one analyzer-style command per line | a static analyser that takes its own argument shape |

The `clang` format is the **default**, and it is the only one that keeps the original
commands. The other two rewrite them, and each rewrite is driven by the language's
adapter.

The formats are chosen per run:

```
ccap build <cmd> [args...] --build-format clang
```

---

## 2. The `clang` Format -- the Compilation Database

The standard compilation database: a JSON array, one object per compilation unit.

```json
[
  {
    "directory": "D:/work/.../.trash/poly/cpp/build",
    "arguments": ["D:\\development\\w64devkit\\bin\\c++.exe", "-std=gnu++17", "-MD",
                  "-MT", "CMakeFiles/poly.dir/main.cpp.obj",
                  "-MF", "CMakeFiles\\poly.dir\\main.cpp.obj.d",
                  "-o", "CMakeFiles/poly.dir/main.cpp.obj", "-c",
                  "D:/work/.../.trash/poly/cpp/main.cpp"],
    "file": "D:/work/.../.trash/poly/cpp/main.cpp"
  }
]
```

Three fields, and they are the same for every language:

| Field | Meaning |
|---|---|
| `directory` | the absolute working directory of that compilation |
| `arguments` | the compiler and its arguments, as an **array** -- one token per element |
| `file` | the absolute path of the compilation unit |

`arguments` is used rather than `command` because the specification supports both, but an
array needs no command-line quote escaping. Consumers such as `clangd` and `ccls` accept
both.

### 2.1 What the arguments look like per language

The **shape** is the same; the **content** is the original command line, verbatim.

| Language | `arguments[0]` | Typical content |
|---|---|---|
| C / C++ | `D:\...\c++.exe` | `-std=`, `-I`, `-D`, `-c`, `-o` -- the original gcc/clang/MSVC spelling |
| Java | `D:\...\javac.exe` | `-classpath`, `-d`, `-sourcepath` |
| Kotlin / Scala | the compiler launcher | `-classpath`, `-d`, `-language-version` / `-source` |
| C# | `C:\Program Files\dotnet\dotnet.exe` | `exec`, `...\csc.dll`, then the `csc` switches |

The C# entry is the longest, because the .NET compiler is invoked through the host and
its command line is fully expanded:

```
["C:\\Program Files\\dotnet\\dotnet.exe", "exec",
 "C:\\Program Files\\dotnet\\sdk\\9.0.101\\Roslyn\\bincore\\csc.dll",
 "/noconfig", "/unsafe-", "/checked-", "/nowarn:1701,1702", "/fullpaths",
 "/nostdlib+", "/warn:9", "/define:TRACE;DEBUG;NET;NET9_0",
 "/reference:C:\\...\\ref\\net9.0\\System.dll", ...,
 "/out:obj\\Debug\\net9.0\\Hello.dll", "/target:exe", "/langversion:13.0",
 "Hello.cs"]
```

Measured: 207 arguments for a single-file project. The references are the .NET
**reference assemblies** -- the same role the `-I` paths play for C, but they are
`.dll` files in a directory, not headers.

---

## 3. The `clang-cmd` Format

One rewritten clang command per line, ready to be fed to a shell.

```
clang++ -std=gnu++17 -c -o CMakeFiles/poly.dir/main.cpp.obj D:/work/.../main.cpp
clang -o .trash/java\out D:/work/.../.trash/java/Hello.java
clang -o obj\Debug\net9.0\Hello.dll D:/work/.../.trash/poly/cs/Hello.cs
```

The driver becomes `clang` or `clang++`, and the options are translated to clang's
spelling. The `directory` of each entry is **not** carried over -- a line is a command,
not a compilation unit, so it has to be run from the right directory to mean anything.

> This is the one format whose output depends on the working directory, and the
> compilation database is the one that does not. Use `clang` when the consumer is a tool
> that reads a database, and `clang-cmd` when the consumer is going to run the lines.

---

## 4. The `translated` Format

One analyzer-style command per line. This is the format where the languages differ most,
because each language family has its own notion of what a compilation is described by.

```
D:\development\w64devkit\bin\c++.exe -std=gnu++17 D:/work/.../main.cpp
D:\development\jdk-25.0.1\bin\javac.exe --output-dir .trash/java\out D:/work/.../Hello.java
C:\Program Files\dotnet\sdk\9.0.101\Roslyn\bincore\csc.dll --sys_include C:\...\System.dll ... D:/work/.../Hello.cs
```

### 4.1 C / C++

The native shape: the include paths, the macros, the language level, the output.

| Concept | The option |
|---|---|
| the header search path | `-I` |
| the system header path | `-isystem` |
| a macro | `-D` |
| the language level | `-std=` |
| the output file | `-o` |

### 4.2 Java / Kotlin / Scala

**The JVM equivalent of an include path is the classpath.** A JVM compile is described by
the set of jars and class directories the compiler reads types from -- there is no `-I`
and no `-D` in the same sense.

| Concept | The option |
|---|---|
| the classpath | `--classpath` |
| the source path | `--sourcepath` |
| the module path | `--module-path` |
| the annotation processor path | `--processor-path` |
| the language level | `--language-level` |
| the output directory | `--output-dir` |

The spelling is **unified** across `javac`, `kotlinc`, `scalac` and `ecj`: the capture
agent already parsed each compiler with its own option table, so what reaches the
translator is structured, and one spelling serves them all. The compiler kind is still
recorded, so a consumer that needs to tell a Kotlin compile from a Scala one can.

### 4.3 C# / VB / F#

The .NET shape. The reference assemblies are the interesting part: they are the .NET
counterpart of the system include path, and they are emitted as `--sys_include`, one per
assembly.

| Concept | The option |
|---|---|
| a reference assembly | `--sys_include` |
| an analyzer / source generator | `--analyzer` |
| the output assembly | `-o` |
| the sources | the trailing positional arguments |

The analyzers are kept because an analyzer **changes what the compiler sees**: a source
generator rewrites the source before it is compiled, and an analyzer reports diagnostics
the plain compiler would not. A consumer that reproduces the compile has to have them.

---

## 5. What Is Deliberately Not Translated

The `translated` and `clang-cmd` formats are driven by the **switch table**, and the
switch table has two axes. The first decides what an option *means*; the second decides
what has to survive for a given consumer.

| Retention level | What it is for |
|---|---|
| `ast` | the arguments needed for clang to parse the translation unit into a correct AST |
| `bc` | the arguments needed for clang to emit a correct LLVM bitcode file |

The difference between the two is small and it is **measured**, not assumed:

| Class | Kept at AST | Kept at BC |
|---|---|---|
| include, system-include, framework, macro, pre-include | yes | yes |
| language standard, language, sysroot, target **triple** | yes | yes |
| target **CPU level** (`-march`, `/arch`) | no | **yes** |
| debug info (`-g`, `/Zi`) | no | **yes** |
| optimisation, warning, dependency generation | no | no |

The target triple is kept at both levels because it decides which builtin types exist,
and therefore whether the code compiles at all. The CPU level is not: it decides the
instruction set, and the AST does not care.

---

## 6. The Audit

Nothing is dropped silently. Every option that does not survive is reported with a
reason, and the reasons are separate buckets:

| Bucket | Meaning |
|---|---|
| `kept` | kept as it is |
| `mapped` | kept, in a different spelling |
| `dropped_no_equivalent` | the option has no clang equivalent |
| `dropped_by_level` | dropped because of the retention level |
| `dropped_by_policy` | dropped by an explicit rule (for example a version exclusion) |
| `unknown` | the switch table does not know this option |

```
ccap compdb map build-log.txt --level ast --explain
```

And the policy for the `unknown` bucket is configurable:

| Policy | Effect |
|---|---|
| `report` | drop it, but record it -- the default |
| `keep` | pass it through unchanged |
| `drop` | drop it silently |
| `fail` | fail the mapping |

---

## 7. A Known Duplication

A compilation can appear **twice** in the output. The reason is a two-stage compiler
wrapper, and it is not a capture bug:

```
D:/development/w64devkit/bin/c++.exe -std=gnu++17 ... main.cpp   <- the launcher
c++ -std=gnu++17 ... main.cpp                                    <- the real compiler
```

The launcher is a program that starts the real compiler, so **both** are processes, and
both are captured. The w64devkit toolchain does this for every compile.

How to tell the two apart: the launcher's `argv` has the full path and the real
compiler's has the bare name, and the real one is the **child** (one level deeper in the
process tree). The compiler that produced the object file is the child.

The same shape occurs with a shell wrapper around any compiler, and with `cc`/`gcc`
shims that re-exec the real driver.

---

## 8. Choosing a Format

| The consumer wants | Use |
|---|---|
| a compilation database to read | `clang` |
| commands to run again | `clang-cmd` |
| analyzer-style arguments, per language | `translated` |
| to know what was dropped and why | `--explain` on the `compdb map` path |

For the mixed-language case, all three formats carry **all** the languages in one file --
the C++, the Java and the C# entries side by side, in the order they were captured.

---

## 9. The Measurements Behind This Document

| What | Measured |
|---|---|
| a C++ compile in `clang` format | 11 arguments |
| a Java compile in `clang` format | 3 arguments |
| a C# compile in `clang` format | 207 arguments |
| the mixed build | 9 entries: 8 C++ and 1 C#, in one database |
| the C# reference assemblies | 164 `/reference:` arguments, 164 `--sys_include` in the translated form |
| the C# analyzers | 8 `/analyzer:` arguments, 8 `--analyzer` in the translated form |
| the C# translated line | 20 565 characters |
| the diagnostic verbosity on the console | ~100 000 lines -- the reason the verbosity goes on the logger instead |

The `clang` format for the mixed build, verbatim:

```
.cpp  .../CMakeFiles/4.2.3/CompilerIdCXX/CMakeCXXCompilerId.cpp   argv: 2
.h    D:/development/w64devkit/share/cmake-4.2/Modules/CXX-DetectStdlib.h   argv: 7
.cpp  .../CMakeFiles/CMakeScratch/TryCompile-*/CMakeCXXCompilerABI.cpp     argv: 6
.cpp  .../.trash/poly/cpp/main.cpp                                  argv: 11
.cs   .../.trash/poly/cs/Hello.cs                                  argv: 207
```
