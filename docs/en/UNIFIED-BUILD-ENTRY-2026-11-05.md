# The Unified Build Entry

<!-- i18n:begin -->
中文 | [**English**](../en/UNIFIED-BUILD-ENTRY-2026-11-05.md)
<!-- i18n:end -->

**Date**: 2026-11-05
**Status**: Implemented and tested end to end

`ccap build <cmd> [args...]` runs the user's build and produces a compile database from
**every language** the build uses. The user supplies only the build command; the capture
works out which mechanisms apply and merges the results into one output.

---

## 1. The Problem

A polyglot repository does not have one build command, it has one build command per
language, and no single capture mechanism covers all of them:

| Language | How the compiler runs | What a process hook sees |
|---|---|---|
| C, C++, Rust, Go | as a child process | the compiler invocation |
| Java, Kotlin, Scala | **inside** the JVM | only the `java` launch |
| C#, VB, F# | **inside** the build process | nothing at all |

The third row is the hard one. The .NET compiler is a library the build loads in-process,
and under the default configuration it runs inside the long-lived compiler server -- so it
never appears as a process.

The previous answer was "run the tool, then read whatever it left behind". That is not
reproducible: it depends on the tool's cache state, on whether the machine has the SDK, on
whether the project was restored.

The answer here is a **single entry point** that picks the right mechanism per language
and merges the results.

---

## 2. The Three Backends

### 2.1 native -- the process capture

The build runs under the debugger (Windows) or the `LD_PRELOAD` shim (Linux). Every process
the build spawns is observed, and the ones that are compilers are recognised.

This covers everything that runs a compiler as a child process: C, C++, Rust, Go, and
anything else of that shape.

### 2.2 jvm -- the in-process agent

The JVM compilers (`javac`, `kotlinc`, `scalac`) run **inside** the JVM. A process hook sees
only the `java` launch, not the compile. So the agent hooks the compilers' own entry points
instead, and is injected into the whole process tree through `JAVA_TOOL_OPTIONS`.

### 2.3 dotnet -- the MSBuild diagnostic log

The .NET compiler (`csc`) is a library the build loads **in-process**, so under the default
configuration it never appears as a process at all. The build's own diagnostic log is the
reliable source: it records the fully expanded compiler command.

Measured: the `Csc` task command in a diagnostic log is ~21 000 characters and ~216
arguments, with **no response file** -- the `@rsp` is already expanded. Replaying it
produces a valid assembly (exit 0, 4 608 bytes, correct `MZ` header).

---

## 3. How the .NET Log Is Obtained

This is the part with the sharp edges, so it is worth being precise about what was
measured.

### 3.1 Why not the process hook

Measured on .NET SDK 9.0.101: `dotnet build` defaults to `UseSharedCompilation=true`, and
the compiler then runs **in-process** inside the compiler server. There is no process
creation event, so the process hook sees nothing.

Setting `UseSharedCompilation=false` does produce a visible process
(`dotnet exec .../csc.dll /noconfig @...rsp`), but the arguments are in a response file,
and the compdb parser classifies response files without expanding them. So that path
yields "35 commands, 0 are compilation commands".

### 3.2 The file logger, not the stdout

MSBuild writes its diagnostic log to its **stdout**, in UTF-16LE, and the console messages
go to the **stderr**, in the console code page. Redirecting the stdout to a file therefore
takes the build's own output away from the user, and merging the two streams corrupts the
log -- the interleaved file decodes to garbage.

The **file logger** avoids both problems:

```
-flp:logfile=<path>;verbosity=diag
```

Measured: the log is UTF-8, it contains the fully expanded `csc` command, and the build's
output still reaches the terminal. The diagnostic verbosity goes **on the logger**, not on
the build -- raising the build's own verbosity prints the whole log to the console
(measured: ~100 000 lines, which drowns everything else).

### 3.3 There is no environment variable

MSBuild's verbosity and its file logger are **command line switches only**. Measured, all
ignored:

| Variable | Result |
|---|---|
| `MSBUILDVERBOSITY` | ignored -- the log stays at the default verbosity |
| `MSBUILDLOGTEXT` | ignored -- no file is created |
| `MSBuildResponseFiles` | ignored -- the response file is not read |

So the switches have to reach the MSBuild command, and that is what decides the shape of
the whole entry point.

---

## 4. The Command Classification

The build command is classified, because the two questions have different answers:

- *May this build contain a .NET part?* -- a wrapper script says **yes**.
- *May I hand this command an MSBuild switch?* -- a wrapper script says **no**.

| Kind | Examples | MSBuild switches | Diagnostic log |
|---|---|---|---|
| `MsBuild` | `dotnet build`, `msbuild`, `devenv` | added | the file logger writes it |
| `NativeCompiler` | `gcc`, `clang`, `cl` | none | none -- it has no .NET part |
| `Wrapper` | `make`, `ninja`, `cmake`, `cmd /c build.bat` | none by default | from the stdout |

Measured why the wrapper gets nothing by default: `cmake --build -v:diag` fails with
`Unknown argument -v:diag`, and so does `cmake --build --no-incremental`. Guessing would
break the build.

The default for an unrecognised command is `Wrapper`. The cost of a wrongly redirected
stdout is lower than the cost of a silently empty .NET capture.

### 4.1 The script build

For a script build, the user says what the script accepts:

```
ccap build cmd /c build.bat --msbuild-args "-v:diag -flp:logfile=build.log;verbosity=diag"
```

The switches are added to the wrapper's own command line, where a script that forwards its
arguments (`%*` in a batch file, `"$@"` in a shell script) passes them on to the MSBuild
it calls.

When the user supplies their own file logger, **that** file is the one read -- not a
second copy taken from the stdout.

When the .NET half is missing, the run says so in an actionable way, rather than reporting
"this build has no .NET part":

> the .NET backend found no compile command. This is a script build, and the MSBuild it
> calls was not verbose. Pass --msbuild-args to hand it "-v:diag
> -flp:logfile=<file>;verbosity=diag", and make sure the script forwards its arguments

The message is only given when an MSBuild really ran. A build with no MSBuild in it --
`cmake --build` on a C++ tree -- leaves a log with none of MSBuild's markers in it, and
that one gets the neutral message instead. The markers have to survive the **localization**,
so they are the ones that are not translated: the compiler's own output (`CSC :`), the
"output -> file" line, and the environment dump the diagnostic log starts with.

---

## 5. The Up-to-Date Build

An **up-to-date** build compiles nothing, so the capture comes out empty -- which is the
worst possible outcome for a capture tool: it looks like it worked, and it produced
nothing.

Measured: a second `dotnet build` of an already-built project wrote a 5 MB diagnostic log
with **no** `Csc` task in it at all.

So the build is forced to run the compiler:

| Switch | Effect |
|---|---|
| `--no-incremental` | added to an MSBuild-compatible command, unless the user decided otherwise |

The native side has no universal equivalent, so there the caller asks for a clean build.

---

## 6. The Output

One list, whatever the build used:

```
ccap build <cmd> [args...] --build-output compile_commands.json
```

| Option | Meaning |
|---|---|
| `--build-output <file>` | where the compile database is written |
| `--build-format <clang\|clang-cmd\|translated>` | the output format (default `clang`) |
| `--build-events` | also write the capture events |
| `--msbuild-log <file>` | read the .NET commands from this log instead of producing one |
| `--msbuild-args <switches>` | the MSBuild switches to hand to a script build |
| `--no-dotnet` | do not run the .NET backend |

The `clang` format is the compilation database with the original commands verbatim. The
`translated` format applies the switch table of
[COMPILER-SWITCH-TABLE-2026-11-03.md](./COMPILER-SWITCH-TABLE-2026-11-03.md): the options
are classified, the ones that decide the AST are kept, and everything dropped is reported
with a reason.

---

## 7. Measured Results

| Case | Result |
|---|---|
| `ccap build dotnet build Hello.csproj` | 1 command, correct absolute source path and directory |
| `ccap build cmd /c build.bat` (C++ + C#) | 9 commands: 8 C++ and 1 C#, in one database |
| the same, with `--msbuild-args` | 9 commands, the C# one read from the user's own log |
| the same, without `--msbuild-args` | 8 commands and an actionable message about the missing .NET half |

The C# source path is resolved against `MSBuildProjectDirectory` -- the project's own
directory, not the directory the build was started in.

---

## 8. The Design Decisions

1. **The plan is separate from the run.** The decisions -- which backends, what the build
   command looks like -- are pure, so they can be tested without starting a build. The
   plan lives in the compdb library, and the app layer only runs it.
2. **The backend that reads a file is a separate step.** The native capture and the JVM
   agent compose in one pass; the .NET backend reads what the build produced, so it runs
   after.
3. **One output, not one per language.** The consumer wants "how was every file in this
   repository compiled".
4. **Nothing is dropped silently.** A missing .NET half is reported with the switch that
   would have caught it.
5. **The capture never breaks the build.** The switches are only handed to a command that
   understands them.

---

## 9. The Tests

`tests/test_compdb_dotnet.cpp` -- 74 assertions.

It is built on a **real** MSBuild diagnostic log, checked in as UTF-16LE with a BOM, not a
synthetic line. A synthetic line would test the parser against the input the author
imagined, and the three things that actually broke during development -- the UTF-16
encoding, the unquoted executable path, the task-id annotation -- are all properties of
the real log.

It covers:

- the Windows command line split, including the backslash-before-quote rule;
- the extraction from the real log, including the repaired executable path, the stripped
  task-id annotation and the already-expanded response file;
- the encoding, in UTF-8 and in UTF-16, and the two failure modes that are not the same
  thing (a log without a compiler task is an answer, a missing log is an error);
- the command recognition;
- the C# adapter, including that the analyzers survive the translation;
- the build plan: the classification, the switches each kind gets, the file logger, and
  the script build.
