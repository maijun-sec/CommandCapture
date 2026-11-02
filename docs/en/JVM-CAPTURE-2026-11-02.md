# JVM compile capture (ccap_jvm)

> Design document · 2026-11-02
>
> Module location: `modules/ccap_jvm/`
> Status: implemented; 97 Maven tests pass (four of them, the Java 8 end-to-end
> tests, only run when a Java 8 JDK is provided). The ECJ / Kotlin / Scala 3
> end-to-end tests drive the real compilers.
> Supported JDKs: **8 and up** -- the agent itself is compiled to Java 8 byte code.

---

## 1. What this module is for

Some projects cannot be introspected statically: the classpath may be assembled by a script,
by a plugin, or by an IDE. For those, the only ground truth is **the compile the compiler
actually performed**.

`ccap_jvm` is a ByteBuddy agent that hooks the compilers' entry points and records that
compile -- not the *requested* command line, but the **effective** compile:

- the classpath that was really used (not the one the command line named)
- the source files the compiler really parsed (including the ones it found on the source path)
- the encoding, the language level, the output directory, the module path, the processor path

**Why not reuse Plumb's implementation?** Plumb's `plumb-build-capture` uses btrace: it needs
elevated privileges, it needs a native agent, and it is effectively unusable on Windows.
ByteBuddy goes through the standard `java.lang.instrument` interface, so it runs as an ordinary
user process.

---

## 2. Three languages, three sets of entry points

This is the central fact of the module: **a "compiler command line" is not one method in any
language**, and the three JVM languages do not even agree on its shape.

| Language | Command-line entry | How the options arrive |
|---|---|---|
| javac | 8 methods (CLI / `javax.tools` API / the IDE path / the run-style API) | the CLI passes a `String[]`; the API passes an `Iterable<String>` plus a separate `Iterable<FileObject>` |
| kotlinc | `CLICompiler#exec(PrintStream, String...)` | the argv is the **free arguments**; the classpath lives in a **generated arguments object** |
| scalac | `Main#main` / `Main#process` | a plain `String[]` |

So a single generic reflective extractor would have to know three shapes, and it would be
wrong for at least one of them.

The entry points are **declarative** (`CompilerEntryPoints`): adding a compiler or an entry
point is a data change, not a code change.

### 2.1 Every entry point is checked against the real jar

The class names and method names are **not guessed**. They are read off the real compiler jars
with `javap`:

```
javap -p -cp kotlin-compiler-embeddable-2.2.0.jar org.jetbrains.kotlin.cli.common.CLICompiler
javap -p -cp scala3-compiler_3-3.10.0.jar dotty.tools.dotc.Driver
javap -p -cp ecj-3.33.0.jar org.eclipse.jdt.internal.compiler.Compiler
```

That rule has saved the module several times (see section 6).

---

## 3. When the capture is written

**When the compile ends**, not when it starts.

The reason: the sources the compiler really compiled are only known once it has compiled them.
The command line names the files it was given; the compiler also compiles the files it finds
itself on the source path -- and those are exactly the files a static analyser has to look at.

So the flow is:

1. **At the start**: the argv is known (the CLI path), or has just been handed to the task (the
   API path). It is **staged** (`PENDING_COMPILES`).
2. **At the end**: the sources the compiler parsed are known. The staged compile is taken back,
   and the capture is written.

### 3.1 Where the API path's options come from

`JavacTaskImpl#call()` and `#generate()` carry **no options at all** -- they were set on the
task when it was created.

And they cannot be read back off the task: `com.sun.tools.javac.api` is not exported by the
`jdk.compiler` module, so any reflective access to the task's `options` field fails with
`InaccessibleObjectException` unless the JVM is started with
`--add-opens jdk.compiler/com.sun.tools.javac.api=ALL-UNNAMED`. Requiring that flag would make
the agent unusable in a normal build.

So the options are remembered at the one place where they are still visible -- `getTask` -- and
are looked up again when the task is driven. The lookup is by identity, so two tasks never share
one set of options.

---

## 4. The bootstrap class loader: the subtlest part of the module

The agent's code runs inside the compiler's process, but the **instrumented bytecode lives in
the compiler's own classes** (`com.sun.tools.javac.Main` and friends), which are loaded by the
**application class loader**. The agent jar is only visible to the agent's own class loader.
So a direct call from the compiler to an agent class fails with `NoClassDefFoundError`.

Appending a small jar to the **bootstrap** class loader solves it: the bootstrap loader is the
ancestor of every other loader, so a class placed there is reachable from the compiler, from
the application and from the agent alike.

### 4.1 But a module cannot see it

**This is the hardest part of the module, and the failure is completely silent.**

The `jdk.compiler` module reads `java.base`, `java.compiler`, `jdk.internal.opt` and
`jdk.zipfs` -- and **not** the unnamed module of the application class loader. So a direct call
from the advice to a class on the agent's own loader fails with an `IllegalAccessError` -- and
the advice's `suppress = Throwable` swallows it whole.

The result: the compile succeeds, the agent prints "agent installed", the capture **never
appears**, and **nothing says why**.

What *is* visible across modules is a class on the **bootstrap** loader: the boot layer's
unnamed module is read by every module.

So the rule is:

> **The advice may only call `CompiledSourcesBridge`** (it is on the bootstrap loader), and the
> bridge reaches everything else reflectively.

`BootstrapJarTest.theAdviceOnlyCallsBootstrapClasses` is the gate for that rule.

### 4.2 The bootstrap jar's class closure is **computed**, not listed

The bootstrap loader sees nothing but the JDK. So every class the recorder references has to be
in the bootstrap jar -- **including the ones the compiler generates for it**:

- the synthetic `Foo$1` class that `javac` emits for a `switch` over an enum (to map the enum
  constants onto a dense int array)
- the nested `Foo$Bar` classes of a record or an enum
- the synthetic `Foo$1` that a lambda's method produces

A hand-written list **will** be wrong. And it was: `JvmOptions$1` -- the switch-map class of
`JvmOptions#isSourceFile` -- was missing, and the capture failed with a `NoClassDefFoundError`
that the advice swallowed.

So the closure is **derived from the bytecode**: `BootstrapClosure` reads each class's constant
pool (the authoritative list of "what do I mention") and follows the references inside this
package until nothing new is found. Add a class to the recorder and the closure follows
automatically, because the list is computed from **the bytecode that is actually shipped**.

> **Lesson**: `Foo$1` is generated by the compiler, and **the person writing the code does not
> think about it**. Any hand-maintained dependency list will eventually miss a synthetic class
> -- and the failure mode is "completely silent, and total".

---

## 5. The capture file format

One JSON file per compile:

```json
{
  "api": "cli",
  "compiler": "javac",
  "language": "java",
  "argv": ["-d", "out", "Hello.java"],
  "encoding": null,
  "languageLevel": null,
  "outputDirectory": "D:\\build\\out",
  "classpath": ["D:\\build\\lib.jar"],
  "sourcePath": [],
  "modulePath": [],
  "processorPath": [],
  "bootClassPath": [],
  "sourceFiles": ["D:\\build\\Hello.java"],
  "addModules": [], "addExports": [], "addReads": [],
  "preview": false,
  "compilation": true,
  "javaVersion": "25.0.1",
  "javaHome": "D:\\jdk-25",
  "workingDirectory": "D:\\build"
}
```

The `compiler` values (`javac` / `ecj` / `sjavac` / `kotlin` / `scala3` / `scala2`) are the
**same names** the `ccap_compdb` compiler table uses, so the two join without a mapping table.

`language` is separate from `compiler`: javac and ECJ compile the same language, and a consumer
usually cares about the language, not about which front end did it.

---

## 6. Each language's entry points, and the traps in them

### 6.1 javac

| Entry point | Role |
|---|---|
| `com.sun.tools.javac.Main#compile(String[])` | the command line |
| `com.sun.tools.javac.main.Main#compile(String[])` | the command line (the instance form) |
| `com.sun.tools.javac.api.JavacTool#getTask(...)` | the `javax.tools` API -- Gradle / Maven / the IDEs |
| `com.sun.tools.javac.api.JavacTool#run(...)` | the run-style API |
| `com.sun.tools.javac.api.JavacTaskImpl#call()` / `#generate()` | the IDE path (no options) |
| `com.sun.tools.javac.main.JavaCompiler#parse(...)` | the parse step -- the sources really compiled |
| `com.sun.tools.javac.main.JavaCompiler#compile(...)` / `#generate(...)` | the compile's end |

**Trap**: `javax.tools.JavaCompiler` is an **interface** on a modern JDK, with only `getTask`
and `getStandardFileManager` -- and **no `run` at all**. The `run` method lives on
`com.sun.tools.javac.api.JavacTool`. A hook on the interface never fires.

**Trap**: `run(InputStream, OutputStream, OutputStream, String...)` has the argv in the
**fourth** parameter. Binding the advice to parameter 0 hands it an `InputStream`; ByteBuddy
refuses to apply such an advice -- and it refuses *loudly*, but only at transform time. The
compile goes on, and the capture is silently lost.

### 6.2 ECJ

| Entry point | Role |
|---|---|
| `org.eclipse.jdt.internal.compiler.batch.Main#compile(String[])` | the command line |
| `org.eclipse.jdt.internal.compiler.Compiler#beginToCompile(ICompilationUnit[])` | the parse step |
| `org.eclipse.jdt.internal.compiler.Compiler#compile(ICompilationUnit[])` | the compile's end |

**Trap**: ECJ's options are not javac's: the classpath is spelled `--class-path`, and it has
options javac does not (`-src`, `-profile`, `-log`). Parsing them with javac's table drops the
classpath entirely -- and the classpath is the one thing a static analyser cannot do without.

**Trap**: ECJ's `ICompilationUnit#getFileName()` returns a `char[]`, not a `String`. It is the
only one of the three languages whose file name is not a string.

### 6.3 sjavac

sjavac **does not compile anything itself**; it drives javac. So the sources it compiles are
seen by javac's parse hook, and the compile itself by javac's compile hook.

What sjavac does need is its **own option table** (it consumes options javac does not know) and
its **own argv**.

### 6.4 Kotlin

| Entry point | Role |
|---|---|
| `org.jetbrains.kotlin.cli.common.CLICompiler#exec(...)` ×4 | the command line -- 4 overloads, the argv sits at a different index in each |
| `org.jetbrains.kotlin.cli.jvm.K2JVMCompiler#main(String[])` | the command line |
| `org.jetbrains.kotlin.psi.KtPsiFactory#createFile(String)` | the parse step -- every source file passes through it |
| the return of `org.jetbrains.kotlin.cli.common.CLICompiler#exec(...)` | the compile's end (the exit code) |

**Trap one**: Kotlin's `exec` has **four** overloads, with the argv at parameters 1, 2 and 3:

```
exec(PrintStream, String...)                          -> argv at 1
exec(PrintStream, MessageRenderer, String...)         -> argv at 2
exec(PrintStream, Services, MessageRenderer, String[])-> argv at 3
exec(MessageCollector, Services, A)                   -> no argv at all
```

Matching by argument count alone hits several of them, and one advice class cannot bind to four
different positions. So each overload is declared separately, **with the argv's type** -- there
are two three-argument `exec` methods, and in one of them the third parameter is the arguments
object rather than a `String[]`.

**Trap two**: Kotlin 2.x's CLI takes the **phased** compile path (`doExecutePhased`), which does
**not** go through any of the methods that look like a compile step:

- `KotlinCoreEnvironment#getSourceFiles()` -- the environment does hold the right files, but the
  phased path never **asks** the environment for them, so the advice never fires
- `KotlinToJVMBytecodeCompiler#compileModules$cli(...)` -- the bytecode of `K2JVMCompiler` does
  call it at offset 852, but the phased compiler does not take that path
- `CoreEnvironmentUtilsKt#createSourceFilesFromSourceRoots(...)` -- the environment's constructor
  calls it to fill its `sourceFiles` field, but the compile path does not go there

It ends up at `KtPsiFactory#createFile(String)`: **every** source the compiler compiles is
created through it, on every path. It is the one point that is reliably reached.

**Trap three**: Kotlin's `-source` names a source **root**, not a language level. Parsing
Kotlin's argv with javac's table records the source root as a language level -- and records it
confidently wrong.

### 6.5 Scala 3

| Entry point | Role |
|---|---|
| `dotty.tools.dotc.Main#main(String[])` | the command line (a forwarder) |
| `dotty.tools.dotc.Driver#main(String[])` | the command line (the real one) |
| `dotty.tools.dotc.Driver#process(String[])` | the command line |
| `dotty.tools.dotc.Driver#doCompile(Compiler, List<AbstractFile>, Context)` | the compile's end, with the sources |

**Trap**: `dotty.tools.dotc.Main#main` is only a forwarder -- it calls `Main$.main`, which is
`Driver#main` (Scala's `object Main` extends the `Driver` class). Likewise `Main#process` is a
forward to `Driver#process`.

Scala's file list is a `scala.collection.immutable.List`, which the agent cannot name, so it is
walked reflectively.

### 6.6 Scala 2

| Entry point | Role |
|---|---|
| `scala.tools.nsc.Main#main(String[])` / `#process(String[])` | the command line |
| `scala.tools.nsc.Global#compile(List<String>)` | the compile's end, with the sources |

Scala 2's option table differs from Scala 3's in a few places that matter
(`-dependencyfile`, `-Ystop-after`), so it gets its own advice.

---

## 7. The option tables: one per compiler

Options are indexed by `(name, compiler)`, so ECJ's, sjavac's, Kotlin's and Scala's options
cannot collide.

The three languages also disagree on what a source file is (the extension):

| Language | Source extensions |
|---|---|
| Java | `.java` |
| Kotlin | `.kt` / `.kts` |
| Scala | `.scala` / `.sc` |

**The argument-file expansion is per-language too**: javac and scalac split on whitespace and
strip the quotes (without grouping them); **Kotlin has no `@file` convention at all**, so
expanding one for it would be inventing a feature that does not exist.

---

## 8. Failure modes: the most valuable section of this document

**Every trap in this module fails the same way: the compile succeeds, the capture does not
appear, and nothing is reported.**

The chain is always the same:

1. The advice is applied (the transform succeeds)
2. But one of the advice's assumptions is wrong -- the class is not visible / the parameter
   position is wrong / the method is never called
3. `suppress = Throwable` swallows the exception, or the method simply is not reached
4. The capture does not appear

So there are a few hard rules:

| Rule | Why |
|---|---|
| **The advice only calls the bootstrap bridge** | a module cannot see the agent's classes |
| **The bootstrap closure is computed** | a hand-written list always misses a synthetic class |
| **Both the parameter position and the argv's _type_ are matched** | matching by count alone hits the wrong overload |
| **Entry points are checked against the real jar with `javap`** | a guessed entry point never fires |
| **A failure in the bridge is reported, not swallowed** | a silent failure is an invisible, incomplete capture |
| **The end-to-end test runs the real compilers** | a unit test cannot prove that the advice fires |

The last one is the hard-won lesson: a mock proves that the mock works.

---

## 9. Integration with ccap_compdb

`ccap_compdb` gained five compiler kinds (`javac` / `ecj` / `kotlin` / `scala3` / `scala2`),
one adapter each, and a new `jvm_capture_parser.cpp` that reads a capture directory.

**Why the classpath is a separate field.** `CompileCommand` used to have only `includeDirs`
(`-I`), which is a **header search path**. A JVM compile is described by its **classpath** -- a
set of jars and class directories. These are genuinely different things: a consumer may have to
look for a class *inside a jar* rather than a header *inside a directory*. So
`classpathDirs` / `moduleDirs` / `processorDirs` are three separate fields, rather than reusing
`includeDirs`.

**Why one kind per language rather than one "jvm" kind.** Each compiler has its own option
syntax, and the isolation of the adapters is this project's standing convention. A consumer also
has to be able to tell a Kotlin compile from a Scala one.

**Why the options are not parsed again.** The agent already parsed them -- it has a per-compiler
option table, on the Java side, where the compiler's semantics are known. Parsing them again
here would mean two implementations of the same table, and the two would drift. So the
structured fields are taken as they are.

On the CLI side, `ccap compdb parse <directory>` recognises a JVM capture directory **by its
contents** (JSON files with `compiler` and `language` fields), not by its name -- so the user
does not have to name it in any particular way.

---

## 10. Supported JDKs: 8 and up

The agent is attached with `-javaagent` into **the compiler's JVM**, so it has to load on **the JDK
the build runs on**. A great many projects are still on Java 8 -- and a Java 11 agent cannot load
there at all: the build succeeds, no capture appears, and nothing says why.

So the agent's own classes are compiled to **Java 8 byte code** (class file major version 52).

### 10.1 Two independent limits

"Which JDKs are supported?" has two independent answers:

| Question | Decided by | Result |
|---|---|---|
| Which class files can it read? | The ByteBuddy version | 1.18.x knows Java 28, and its own base classes are Java 8 |
| Which JVM can load it? | **Our own** `maven.compiler.release` | `release=8` gives major 52 |

**ByteBuddy was never the bottleneck.** ByteBuddy 1.18.17's base classes are major 52 (Java 8);
the newer ones live under `META-INF/versions/`. So "one ByteBuddy version covers the whole JDK 8+
range" is true -- **there is no need for several implementations or several packages**.

(The one case that does need a variant is Java 5/6/7 runtimes, and ByteBuddy ships `1.18.7-jdk5`
for that.)

### 10.2 The downgrade is not one line of pom

Changing `release` from 17 to 8 makes the compiler report these errors, and every one of them has
to be rewritten for real:

| Kind | Where it was used |
|---|---|
| `record` (Java 16) | `CaptureEntry`, `CaptureRecorder.PendingCompile`, `CompilerEntryPoints.EntryPoint` |
| switch expressions / arrow rules (Java 14) | `JvmOptions`, `CompilerHooks`, `CaptureRecorder`, `CaptureEntry`, `BootstrapClosure`, `CaptureReader` |
| `instanceof` pattern matching (Java 16) | `SourcePaths` (13 sites), `CaptureRecorder` (2 sites) |
| Java 9+ APIs | `Path.of`, `String.isBlank`, `List.of`, `List.copyOf`, `Files.readString/writeString`, `InputStream.readAllBytes`, `Stream.toList`, `Method.trySetAccessible`, `ClassLoader.getPlatformClassLoader` |

Every Java 9+ API call lives in `Jdk8`, which is the **single list** of what the agent is allowed
to use.

**The test sources are not bound by this.** They run on the build machine and have no floor to
honour. So the compiler plugin configures the two separately:

```xml
<maven.compiler.release>8</maven.compiler.release>         <!-- the main sources -->
<maven.compiler.testRelease>17</maven.compiler.testRelease> <!-- the test sources -->
```

### 10.3 The gate

`AgentJdkCompatibilityTest` is the gate for this constraint, and it checks the **shipped byte
code**, not the build configuration:

- the agent's own class files must be **52** (Java 8);
- the agent's byte code must **not mention any Java 9+ API** -- this is the important one, because
  the compiler only enforces the *language* level. `Path.of` compiles fine against a newer JDK and
  is a `NoSuchMethodError` on Java 8 -- and that happens on the compiler's critical path;
- ByteBuddy's own floor must be Java 8 -- which proves it is not the bottleneck.

`Jdk8EndToEndTest` is the **real** verification: it runs a real compile on a real Java 8 JVM. The
JDK is named by the `CCAP_TEST_JDK8` environment variable; without it the test is skipped, because
a 100 MB JDK does not belong in a repository.

### 10.4 Compiler versions on Java 8

The agent runs on Java 8, but the **compiler** has to run on Java 8 as well:

| Language | Versions that work on Java 8 | Note |
|---|---|---|
| javac | 8 | |
| ECJ | any recent version | ECJ has always supported Java 8 |
| sjavac | ships with JDK 8 | |
| Kotlin | **up to 2.4.x** | 2.5 raises the minimum to JDK 17 |
| Scala 3 | **3.3 LTS to 3.7** | 3.8 raises the minimum to JDK 17 |
| Scala 2 | 2.13 | |

**The entry point table does not change.** Kotlin's `K2JVMCompiler#main` and `KtPsiFactory#createFile`
have existed since 1.3, and Scala 3's `Driver#doCompile` and `Run#files` since 3.0. Measured with
`javap` against the real jars:

- kotlin-compiler-embeddable **1.9.22**: `KtPsiFactory#createFile(String)` is present;
  `CLICompiler#exec(PrintStream, String...)` is **absent** (it arrived in 2.x), but
  `K2JVMCompiler#main(String[])` is present -- so the argv is still captured.
- scala3-compiler **3.3.4**: `Driver#doCompile`, `Driver#main` and `Run#files` are identical to
  3.10.0; and every class file in the jar is **52** -- pure Java 8 byte code.

### 10.5 The Scala standard library does not follow the compiler version

An easy trap: **the Scala 3 compiler version and the standard library it uses are not the same
number.**

| Scala 3 compiler | the `scala-library` it pairs with |
|---|---|
| 3.3.x | **2.13.x** (the Scala 2 standard library, which Scala 3 reads the `scala` package from) |
| 3.10.x | **3.10.x** (a `scala-library-bootstrapped` artifact) |

So `scala-library` has to be tracked on its own version. Writing `${scala3.version}` for it -- the
obvious thing to do -- makes Scala 3.3.4 ask for a `scala-library:3.3.4` that **does not exist**:
Maven leaves a set of empty `.lastUpdated` shells behind and the dependency resolution fails.

In the build that is `scalaLibrary.version` in `modules/ccap_jvm/pom.xml`, which the
`jdk8-compilers` profile swaps together with `scala3.version`.

### 10.6 Whether the compiler recognises the JDK is a separate axis

"Can the compiler run on JDK 8" and "can the compiler run on **the JDK the build uses**" are two
different questions.

The Kotlin compiler ships a relocated copy of IntelliJ's `JavaVersion`, which parses
`java.version`. On a JDK it does not know, that parse throws:

```
java.lang.IllegalArgumentException: 25.0.1
  at com.intellij.util.lang.JavaVersion.parse(JavaVersion.java:307)
  at com.intellij.util.lang.JavaVersion.current(JavaVersion.java:176)
  at org.jetbrains.kotlin.cli.jvm.modules.JavaVersionUtilsKt.isAtLeastJava9(...)
```

That is a **Kotlin limitation** of its own (KT-73967), and it has nothing to do with the capture:
the agent installs, every entry point is hooked, and the compiler dies before it reads a single
source file. Measured: Kotlin **2.1.0 always fails on JDK 25**, **2.2.0 is fine**.

So `MultiLanguageEndToEndTest` uses a **capability probe** for Kotlin rather than a hardcoded
version table: it asks the compiler's own `JavaVersion` whether it can read the current JDK.

```
CompilerJdkSupport.compilerUnderstandsThisJdk()
```

That is exactly the question the compiler is about to ask itself, so the answer is authoritative.
A table saying "Kotlin 2.1.0 does not know JDK 25" would need a new entry every time either side
moves.

> The counter-intuitive consequence: the Kotlin version used for the JDK 8 scenario is precisely an
> old one like 2.1.0, and those do **not** run on a new JDK. So the JDK 8 end-to-end test has to
> run the compilers **on the Java 8 JVM** (which is what `Jdk8EndToEndTest` does), not on the build
> JDK.

### 10.7 javac on Java 8 needs tools.jar on the classpath

On Java 8 the compiler classes are **not on the runtime classpath**: `com.sun.tools.javac.Main`
lives in `lib/tools.jar`, and the `javac` launcher adds it explicitly.

A plain `java -cp ... com.sun.tools.javac.Main` therefore fails with
`ClassNotFoundException: com.sun.tools.javac.Main`. From Java 9 on the compiler is a module
(`jdk.compiler`) and needs nothing of the sort -- which is exactly why this shows up **only** in
the Java 8 test.

`Jdk8EndToEndTest` therefore puts `tools.jar` on the classpath with `jdk8CompilerClasspath()`.

### 10.8 Whether a compiler loads on Java 8 is a question about the class file version

The Scala 3.10.0 of the default profile is **class file version 61** throughout -- Java 17 byte
code. Loading its main class on a Java 8 JVM blows up immediately:

```
java.lang.UnsupportedClassVersionError: dotty/tools/dotc/Main has been compiled by a
more recent version of the Java Runtime (class file version 61.0), this version of the
Java Runtime only recognizes class file versions up to 52.0
```

The Scala 3.3.4 of the `jdk8-compilers` profile is **52** throughout.

So `Jdk8EndToEndTest` decides whether to skip by looking at the class file version, rather than
by trusting the profile name:

```
CompilerJdkSupport.runsOnJava8("scala3-compiler_3-")
```

That way running the test under the default profile does not report a failure just because "this
compiler is not meant for Java 8".

---

## 11. Usage

### 11.1 Attaching the agent

**This layer does not modify command lines, and it does not inject anything by itself.** It expects
the agent to be attached already -- attaching it is the build configuration's job, capturing is the
agent's job.

```bash
# Attach the agent to a compile
java -javaagent:ccap-jvm-capture.jar \
     -Dccap.capture.dir=/tmp/captures \
     -cp ... com.sun.tools.javac.Main -d out Hello.java

# Or attach it to a JVM that is already running
java -cp ccap-jvm-capture.jar org.ccap.jvm.capture.CaptureAgent attach <pid>
```

### 11.2 Can the three commands take a `-javaagent`?

**The short answer: the `javac` command line cannot take one directly. Maven and Gradle can, but
not the way you would expect.**

| Command | Can it take `-javaagent`? | How |
|---|---|---|
| `javac` | **No** | `javac`'s own option parser does not know JVM options |
| `mvn` | Yes | `MAVEN_OPTS` |
| `gradle` | **With a catch** | `JAVA_TOOL_OPTIONS`, *not* `org.gradle.jvmargs` |

**Why `javac` cannot.** It is a program, not a JVM launcher. `-javaagent` is a JVM option, and only
`java` takes JVM options. So attaching to javac means going through
`java -cp tools.jar com.sun.tools.javac.Main`, or using `JAVA_TOOL_OPTIONS` -- which the JVM reads
at startup, so every java process picks it up.

**Maven** (the default `fork=false`, where the compile happens in Maven's own JVM):

```bash
MAVEN_OPTS="-javaagent:/path/ccap-jvm-capture.jar=ccap.capture.dir=/tmp/captures" mvn clean install
```

**Gradle -- `org.gradle.jvmargs` does not work.** Measured: `Unrecognized option: -javaagent=...`

```
Process command line: java.exe --add-opens=... -javaagent=... -Dfile.encoding=UTF-8 -cp gradle-daemon-main.jar ...
                                                                        ^^^^^^^^^^^^^ after -cp
```

A `-javaagent` that appears **after** `-cp` is an *application* argument, not a JVM option. That is
where Gradle puts the `org.gradle.jvmargs` entries -- its own instrumentation agent ends up in the
same place.

**`JAVA_TOOL_OPTIONS` works**, because it is injected before the JVM starts, so the position is
always right:

```bash
JAVA_TOOL_OPTIONS="-javaagent:/path/ccap-jvm-capture.jar=ccap.capture.dir=/tmp/captures" gradle build
```

> While measuring this: when Gradle uses a toolchain, the compiler classes come from that JDK's
> `tools.jar`, held by the **extension class loader**. That is the cause of the trap in 11.5.

### 11.3 One command with the launcher

All three of the above need environment variables set up. **The launcher wraps that up**:

```bash
# Point the captures somewhere
export CCAP_CAPTURE_DIR=/tmp/captures

# Then wrap any command with the launcher
java -jar ccap-jvm-capture.jar javac -cp lib.jar -d out src/Foo.java
java -jar ccap-jvm-capture.jar mvn -q clean install
java -jar ccap-jvm-capture.jar gradle build
```

**What the launcher does**: it adds `JAVA_TOOL_OPTIONS` to the child's environment, and then runs
your command **exactly as given**.

- **It does not touch the command line** -- a build script that inspects its own arguments sees
  what it expects.
- **It does not replace an existing value** -- the agent is *appended* to whatever
  `JAVA_TOOL_OPTIONS` already said.
- **The capture directory travels as the agent's own argument** (`-javaagent:...=ccap.capture.dir=...`),
  because that is the only channel that reaches the compiler's JVM.

Measured, all three commands capture through the launcher:

| Command | Captured through the launcher |
|---|---|
| `javac` | Yes |
| `mvn` | Yes |
| `gradle` | Yes |

> The jar does two things: the `Main-Class` is `org.ccap.jvm.capture.Main`, which dispatches on the
> first argument -- `attach` is the dynamic attach, anything else is the launcher. The
> `-javaagent` path is the JVM's own and does not go through here.

### 11.4 The agent's arguments

The `key=value` part of `-javaagent:jar=key=value` becomes system properties. **That is the one
channel Gradle lets through**, because Gradle forwards `-javaagent` but drops every `-D`.

### 11.5 Reading the captures back

```bash
ccap compdb parse /tmp/captures
ccap compdb show /tmp/captures
ccap compdb export /tmp/captures -o compile_commands.json
```

### 11.6 Which class loader holds the compiler decides whether it is captured

This is the deepest trap found while measuring, and it is the classic shape of "the compile
succeeded, the capture is empty, and nothing says why".

ByteBuddy's `AgentBuilder.Default` **ignores two class loaders by default**: the bootstrap loader
and the platform loader. A compiler class loaded by either of them is **never woven into**, the
advice never fires, the compile succeeds perfectly, and not one capture appears.

| The compiler's class loader | Captured? |
|---|---|
| the application loader (`java -cp tools.jar`) | Yes |
| **bootstrap** (`-Xbootclasspath/a:tools.jar`) | **No -- ignored by default** |
| **platform** (the `jdk.compiler` module on JDK 9+) | **No -- ignored by default** |

Gradle hits exactly this. So `CompilerHooks` widens the class loader matcher: it relaxes **only**
the loader restriction, and keeps ByteBuddy's own protection of its own classes and the reflection
internals.

`ClassLoaderCoverageTest` guards that line -- it runs a real compile through the application loader
and through the bootstrap loader, and asserts that both produce a capture. Measured: before the
fix the second case failed; after it, both pass.

---

## 12. Tests

| Test | Covers |
|---|---|
| `CompilerOptionTest` | the per-compiler option tables |
| `JvmOptionsTest` | option parsing, source-file recognition, argument-file expansion |
| `CompilerEntryPointsTest` | the entry point table -- every language has a source-file hook |
| `CaptureFormatTest` | the capture's JSON format |
| `BootstrapJarTest` | the bootstrap closure is complete; the advice only calls bootstrap classes |
| `AgentJdkCompatibilityTest` | **the Java 8 floor**: class version 52, no Java 9+ API |
| `CaptureEndToEndTest` | the javac command line and the `javax.tools` API |
| `MultiLanguageEndToEndTest` | **ECJ / Kotlin / Scala 3, with the real compilers** |
| `Jdk8EndToEndTest` | **a real compile on a real Java 8 JVM** |
| `ClassLoaderCoverageTest` | **the compiler must be caught on the bootstrap and the application loader** |
| `CaptureLauncherTest` | the launcher's injection logic (appends, never replaces) |
| `CaptureLauncherEndToEndTest` | **the launcher really captures, through javac and java** |
| `CompilerJdkSupport` | probes whether the compiler recognises the current JDK (Kotlin's version parsing) |

`MultiLanguageEndToEndTest` is the critical one: it runs the **real** compilers (ECJ 3.33.0,
kotlin-compiler 2.2.0, scala3-compiler 3.10.0 -- all `test` scope dependencies). Because a
mismatch between the advice and the target is *silent*, only a real compile proves that the
advice actually fires.
