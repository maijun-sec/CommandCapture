/*
 * CommandCapture - capture every command a script or executable spawns.
 *
 * Copyright (c) 2026 maijun
 *
 * SPDX-License-Identifier: MIT
 *
 * Released under the MIT License. See the LICENSE file at the repository
 * root for the full text.
 */
package org.ccap.jvm.capture;

import java.util.ArrayList;
import java.util.List;

/**
 * The set of compiler entry points the agent hooks.
 *
 * <p>This is a <b>declarative</b> description: a list of (class, method, argument) triples. The
 * agent reads it and asks ByteBuddy to match it, so adding a compiler or a new entry point is a
 * data change, not a code change.
 *
 * <h2>Why the three languages need three sets of entry points</h2>
 *
 * <p><b>This is the central fact of the module.</b> A "compiler command line" is not one method in
 * any language, and the three JVM languages do not even agree on what shape it has:
 *
 * <ul>
 *   <li><b>javac</b> reaches the compiler through eight different methods depending on how it was
 *       invoked: the command-line driver, the {@code javax.tools} API, the IDE path, and the run
 *       style API. The options come as a {@code String[]} on the CLI path and as an {@code
 *       Iterable<String>} plus a separate {@code Iterable<FileObject>} on the API path.
 *   <li><b>kotlinc</b> has one real entry -- {@code CLICompiler#exec(PrintStream, String...)} -- and
 *       the {@code main} method is only a shell around it. The options are then carried in a
 *       <em>generated</em> arguments object, not in the argv: the classpath is
 *       {@code K2JVMCompilerArguments#getClasspath()}, the output directory is {@code
 *       getDestination()}, and the source files are the <em>free arguments</em>.
 *   <li><b>scalac</b> takes a plain {@code String[]} on {@code Main#main} / {@code Main#process},
 *       with no arguments object at all.
 * </ul>
 *
 * <p>So a single generic reflective extractor would have to know three different shapes, and it
 * would be wrong for at least one of them. The entry points are therefore declared per language,
 * and the {@link EntryPoint.Kind} says how the arguments are delivered.
 *
 * <p>Hooking only one entry point means the capture silently misses every build that takes a
 * different path. That failure is invisible: the scan runs, and there is simply no capture.
 *
 * <p><b>The class names are the compiler's, verified against the real jars.</b> They are not
 * guessed from the documentation: {@code javax.tools.JavaCompiler} is an <em>interface</em> on a
 * modern JDK and has no {@code run} method, so a hook on it never fires. The {@code run} method
 * lives on {@code com.sun.tools.javac.api.JavacTool}. Likewise the Kotlin and Scala entry points
 * were read off the real compiler jars with {@code javap}, not inferred.
 */
public final class CompilerEntryPoints {

  /**
   * One hooked method.
   *
   * <p><b>Why this is a class and not a record.</b> Records arrived in Java 16, and the agent has
   * to load on Java 8 -- see {@link Jdk8}. The accessors keep the record's shape, so the callers do
   * not change.
   */
  public static final class EntryPoint {

    private final String typeName;
    private final String methodName;
    private final int argumentCount;
    private final int optionsIndex;
    private final int sourcesIndex;
    private final Kind kind;
    private final CompilerOption.Compiler compiler;

    /**
     * One hooked method.
     *
     * @param typeName the fully qualified class name
     * @param methodName the method name
     * @param argumentCount how many arguments the method takes; {@code -1} means "any"
     * @param optionsIndex which argument carries the compiler options, or {@code -1} when the entry
     *     point does not take them
     * @param sourcesIndex which argument carries the source files, or {@code -1}
     * @param kind how the arguments are delivered
     * @param compiler which compiler's option table the arguments are parsed with
     */
    public EntryPoint(
        String typeName,
        String methodName,
        int argumentCount,
        int optionsIndex,
        int sourcesIndex,
        Kind kind,
        CompilerOption.Compiler compiler) {
      this.typeName = typeName;
      this.methodName = methodName;
      this.argumentCount = argumentCount;
      this.optionsIndex = optionsIndex;
      this.sourcesIndex = sourcesIndex;
      this.kind = kind;
      this.compiler = compiler;
    }

    /** The fully qualified class name. */
    public String typeName() {
      return typeName;
    }

    /** The method name. */
    public String methodName() {
      return methodName;
    }

    /** How many arguments the method takes; {@code -1} means "any". */
    public int argumentCount() {
      return argumentCount;
    }

    /** Which argument carries the compiler options, or {@code -1}. */
    public int optionsIndex() {
      return optionsIndex;
    }

    /** Which argument carries the source files, or {@code -1}. */
    public int sourcesIndex() {
      return sourcesIndex;
    }

    /** How the arguments are delivered. */
    public Kind kind() {
      return kind;
    }

    /** Which compiler's option table the arguments are parsed with. */
    public CompilerOption.Compiler compiler() {
      return compiler;
    }

    /** The entry point as it is written in the table, for the gate and the logs. */
    public String describe() {
      return typeName + "#" + methodName + "/" + argumentCount;
    }

    /**
     * How the compiler arguments reach the hook.
     *
     * <p>The distinction matters, because the three languages deliver the arguments differently:
     * the CLI drivers take a {@code String[]}, the {@code javax.tools} API takes the options as a
     * {@code List<String>} and the sources as an {@code Iterable} of {@code JavaFileObject}s, and
     * kotlinc's options live in a generated arguments object that has to be read by accessor.
     */
    public enum Kind {
      /** The options are a {@code String[]}. */
      ARGV,
      /** The options are an {@code Iterable} and the sources are a second {@code Iterable}. */
      FILE_OBJECTS,
      /**
       * The options and sources are given, and the method also <em>returns</em> a task.
       *
       * <p>This is {@code JavacTool#getTask}. It behaves like {@link #FILE_OBJECTS} and additionally
       * hands back the task, which is what makes the IDE path capturable: the task is remembered
       * with its options here, and looked up again when the task is driven.
       */
      FILE_OBJECTS_WITH_TASK,
      /**
       * The method takes no compiler options at all.
       *
       * <p>{@code JavacTaskImpl#call()} and {@code JavacTaskImpl#generate()} are in this category:
       * by the time they run, the options were already handed to {@code JavacTool #getTask}. The
       * IDE path goes through them, so hooking them is what makes the capture work in an IDE -- but
       * the options have to come from somewhere else.
       */
      NO_OPTIONS,
      /**
       * The method is the compiler's parse step, and its argument is the file being parsed.
       *
       * <p><b>This is where the sources the compiler really compiled are found.</b> Every source
       * the compiler parses passes through {@code JavaCompiler#parse(JavaFileObject,
       * CharSequence)}: the ones it was given, through {@code parseFiles}, and the ones it found on
       * the source path, through {@code readSourceFile}. So one hook here sees exactly the sources
       * the compiler parsed -- and a source in the source path that nobody referenced is never
       * parsed, so it never appears.
       */
      PARSED_SOURCE,
      /**
       * The method is the compiler's compile step, and it carries no options.
       *
       * <p><b>This is where the capture is written.</b> The compile has run by then, so the sources
       * it really compiled are known -- and a capture written when the compile <em>starts</em>
       * cannot report them. The options are known at the start, so they are staged until the
       * compile ends.
       */
      COMPILE_END,
      /**
       * The method takes the argv as a <em>varargs</em> array, and the compiler is Kotlin's.
       *
       * <p><b>Why Kotlin needs its own kind.</b> {@code CLICompiler#exec} has the signature {@code
       * exec(PrintStream, String...)} -- the argv is the second parameter, and it is a varargs
       * array. The parameter index is therefore 1, and the {@code PrintStream} in front of it has
       * to be skipped. That is a different shape from every other entry point, and it is why the
       * advice is bound to the parameter index rather than to a fixed position.
       *
       * <p>The kind also carries the compile's end: the method returns the compiler's exit code,
       * so it is the one place where the capture is written for Kotlin.
       */
      KOTLIN_EXEC,
      /**
       * The method is the compiler's "here is the file I am about to compile" step, and it carries
       * the file as its argument.
       *
       * <p><b>This is Kotlin's and Scala's counterpart of {@link #PARSED_SOURCE}.</b> The shape
       * differs per compiler, which is why each one gets its own advice class rather than a
       * shared one:
       *
       * <ul>
       *   <li>Kotlin: {@code KotlinCoreEnvironment#getSourceFiles()} returns the {@code KtFile}
       *       list the environment holds.
       *   <li>Scala 3: {@code dotty.tools.dotc.Run#files()} returns the {@code AbstractFile} set the
       *       run holds.
       *   <li>Scala 2: {@code scala.tools.nsc.Global#compile(List<String>)} receives the source
       *       paths.
       * </ul>
       */
      COMPILED_SOURCES,

      /**
       * The method creates the PSI file for a source, and the source's path is in its argument.
       *
       * <p>This is Kotlin's counterpart of javac's {@code JavaCompiler#parse}: the compiler's
       * parse step. Every source it compiles passes through it, so it is the one point that is
       * reached on every path.
       */
      KOTLIN_SOURCE_FILE,

      /**
       * The method runs the compile, and the source files are in its arguments.
       *
       * <p>This is Scala 3's counterpart of javac's {@code JavaCompiler#compile}: the compile step
       * itself, with the files it compiles.
       */
      SCALA3_COMPILE_END
    }
  }

  /**
   * The entry points, in the order they are probed.
   *
   * <p>Order matters only for diagnostics: the first entry point whose class is present is the one
   * that will fire for a given build. Classes that are absent (ECJ in a javac build, sjavac in a
   * plain build) are simply skipped.
   */
  private static final List<EntryPoint> ENTRY_POINTS =
      Jdk8.of(
          // ============ Java: the javac command-line driver ============
          // com.sun.tools.javac.Main#compile(String[]) -- static, the CLI's own entry point.
          new EntryPoint(
              "com.sun.tools.javac.Main",
              "compile",
              1,
              0,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.JAVAC),
          // com.sun.tools.javac.Main#compile(String[], PrintWriter) -- the same, but the
          // diagnostics go to a writer the caller supplies.
          new EntryPoint(
              "com.sun.tools.javac.Main",
              "compile",
              2,
              0,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.JAVAC),
          // com.sun.tools.javac.main.Main#compile(String[]) -- instance, the options are the
          // FIRST declared parameter (the instance `this` is not a parameter).
          new EntryPoint(
              "com.sun.tools.javac.main.Main",
              "compile",
              1,
              0,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.JAVAC),
          // com.sun.tools.javac.main.Main#compile(String[], Context) -- same, plus the context.
          new EntryPoint(
              "com.sun.tools.javac.main.Main",
              "compile",
              2,
              0,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.JAVAC),

          // ============ Java: the javax.tools API: what Gradle, Maven and the IDEs use ============
          // JavacTool#getTask(Writer, FileManager, DiagnosticListener,
          //                  Iterable<String> options, Iterable<String> classes,
          //                  Iterable<FileObject>)
          //
          // This one also returns the task, so it is remembered with its options: the task is
          // what an IDE later drives with call() or generate(), and those carry no options.
          new EntryPoint(
              "com.sun.tools.javac.api.JavacTool",
              "getTask",
              6,
              3,
              5,
              EntryPoint.Kind.FILE_OBJECTS_WITH_TASK,
              CompilerOption.Compiler.JAVAC),
          // JavacTool#getTask(..., Context) -- the seven-argument overload, same positions.
          new EntryPoint(
              "com.sun.tools.javac.api.JavacTool",
              "getTask",
              7,
              3,
              5,
              EntryPoint.Kind.FILE_OBJECTS_WITH_TASK,
              CompilerOption.Compiler.JAVAC),

          // ============ Java: the old javax.tools.JavaCompiler#run API ============
          //
          // The `run` method is on com.sun.tools.javac.api.JavacTool, NOT on
          // javax.tools.JavaCompiler: on a modern JDK the latter is an interface with only
          // `getTask` and `getStandardFileManager`, and no `run` at all. Hooking
          // `javax.tools.JavaCompiler#run` therefore never fires -- the capture silently
          // does nothing for every caller that uses the run-style API.
          //
          // The signature is `run(InputStream, OutputStream, OutputStream, String...)`, so the
          // options are the varargs array in argument 3 -- but the *declared* parameters are
          // four, so the argument ordinal of the varargs array is 3.
          //
          // Careful: for a varargs parameter, ByteBuddy's argument ordinal counts the varargs
          // array as one parameter, so it is the fourth (index 3) -- the same as the declared
          // parameter index. The advice binds @Advice.Argument(3).
          new EntryPoint(
              "com.sun.tools.javac.api.JavacTool",
              "run",
              4,
              3,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.JAVAC),

          // ============ Java: the IDE path ============
          //
          // An IDE creates a task with getTask and then drives it with call() or generate().
          // Neither takes the options -- they were set on the task when it was created -- so the
          // hook looks them up from the ones remembered at getTask. They cannot be read off the
          // task directly: com.sun.tools.javac.api is not exported by the jdk.compiler module.
          new EntryPoint(
              "com.sun.tools.javac.api.JavacTaskImpl",
              "call",
              0,
              -1,
              -1,
              EntryPoint.Kind.NO_OPTIONS,
              CompilerOption.Compiler.JAVAC),
          new EntryPoint(
              "com.sun.tools.javac.api.JavacTaskImpl",
              "generate",
              0,
              -1,
              -1,
              EntryPoint.Kind.NO_OPTIONS,
              CompilerOption.Compiler.JAVAC),

          // ============ Java: the compiler's own parse step ============
          //
          // Every source the compiler parses passes through this method: the
          // ones it was given, through parseFiles, and the ones it found on
          // the source path, through readSourceFile. A source in the source
          // path that nobody referenced is never parsed, so it never appears here.
          //
          // The method is `protected`, which is fine: ByteBuddy instruments
          // methods by name and argument count, not by visibility.
          new EntryPoint(
              "com.sun.tools.javac.main.JavaCompiler",
              "parse",
              2,
              -1,
              -1,
              EntryPoint.Kind.PARSED_SOURCE,
              CompilerOption.Compiler.JAVAC),

          // ============ Java: the compiler's own compile step ============
          //
          // The compile has run by then, so the sources it really compiled are
          // known. Both paths reach this method: the command line through
          // com.sun.tools.javac.main.Main, the javax.tools API through
          // JavacTaskImpl. One hook therefore covers every compile.
          new EntryPoint(
              "com.sun.tools.javac.main.JavaCompiler",
              "compile",
              4,
              -1,
              -1,
              EntryPoint.Kind.COMPILE_END,
              CompilerOption.Compiler.JAVAC),

          // The IDE's other path. `JavacTaskImpl#generate()` does NOT go
          // through `compile`: it parses, attributes and then calls
          // `JavaCompiler#generate(Queue)`. So the generate step is hooked as
          // well -- otherwise an IDE that generates code would produce no
          // capture at all.
          new EntryPoint(
              "com.sun.tools.javac.main.JavaCompiler",
              "generate",
              1,
              -1,
              -1,
              EntryPoint.Kind.COMPILE_END,
              CompilerOption.Compiler.JAVAC),

          // ============ Java: Eclipse's compiler ============
          new EntryPoint(
              "org.eclipse.jdt.internal.compiler.batch.Main",
              "compile",
              1,
              0,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.ECJ),

          // ============ Java: ECJ's own compile steps ============
          //
          // ECJ's driver (`batch.Main#compile`) takes the argv, but the
          // compiler itself is a separate object, and these are the methods
          // that say "here is what I am compiling".
          //
          // `Compiler#compile(ICompilationUnit[])` is ECJ's compile step --
          // the one every ECJ compile goes through.  It is the counterpart of
          // javac's `JavaCompiler#compile`.
          //
          // `Compiler#beginToCompile(ICompilationUnit[])` is ECJ's parse
          // step: it is called with the units before they are compiled, so
          // it sees the same set of files that javac's `parse` sees.
          //
          // Verified against ecj 3.33.0 with javap:
          //   public void compile(org.eclipse.jdt.internal.compiler.env.ICompilationUnit[]);
          //   protected void beginToCompile(org.eclipse.jdt.internal.compiler.env.ICompilationUnit[]);
          new EntryPoint(
              "org.eclipse.jdt.internal.compiler.Compiler",
              "compile",
              1,
              -1,
              -1,
              EntryPoint.Kind.COMPILE_END,
              CompilerOption.Compiler.ECJ),
          new EntryPoint(
              "org.eclipse.jdt.internal.compiler.Compiler",
              "beginToCompile",
              1,
              -1,
              -1,
              EntryPoint.Kind.PARSED_SOURCE,
              CompilerOption.Compiler.ECJ),

          // ============ Java: the sjavac server ============
          //
          // sjavac has no hook of its own for the compiled sources, and
          // that is not a gap: sjavac does not compile anything itself, it
          // drives javac. So the sources it compiles are seen by javac's
          // parse hook, and the compile itself by javac's compile hook.
          //
          // What sjavac does need is its own option table -- it consumes
          // options javac does not know -- and its own argv, which is what
          // these two entry points give.
          new EntryPoint(
              "com.sun.tools.sjavac.client.ClientMain",
              "run",
              1,
              0,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.SJAVAC),
          new EntryPoint(
              "com.sun.tools.sjavac.server.ServerMain",
              "run",
              1,
              0,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.SJAVAC),

          // ============ Kotlin: the CLI entry ============
          //
          // `K2JVMCompiler#main(String[])` is only a shell: it calls
          // `CLICompiler#doMain`, which calls `CLICompiler#exec`.
          //
          // **Why the exec overloads are listed one by one.** The Kotlin
          // compiler has *four* `exec` overloads, and the argv sits at a
          // different index in each of them:
          //
          //   exec(PrintStream, String...)                          -> argv at 1
          //   exec(PrintStream, MessageRenderer, String...)         -> argv at 2
          //   exec(PrintStream, Services, MessageRenderer, String[])-> argv at 3
          //   exec(MessageCollector, Services, A)                   -> **no argv**
          //
          // The last one takes the *generated arguments object*, not a
          // string array, so it is not hooked at all: there is no argv
          // there to read.
          //
          // A single entry point with "any argument count" would match all
          // of them, and one advice class cannot bind to four different
          // indices. ByteBuddy refuses to apply an advice whose argument
          // type does not fit -- and it refuses *loudly*, but only at
          // transform time, so the capture is silently missing for the
          // overloads that were not the one the advice was written for.
          //
          // So each overload is declared with its own argument count and
          // its own argv index. The advice is chosen by the index, so a
          // new overload is a new entry point, not a silent breakage.
          //
          // Verified against kotlin-compiler-embeddable 2.1.0 with javap:
          //   public final ExitCode exec(PrintStream, String...);
          //   public final ExitCode exec(PrintStream, MessageRenderer, String...);
          //   protected final ExitCode exec(PrintStream, Services, MessageRenderer, String[]);
          //   public final ExitCode exec(MessageCollector, Services, A);
          new EntryPoint(
              "org.jetbrains.kotlin.cli.common.CLICompiler",
              "exec",
              2,
              1,
              -1,
              EntryPoint.Kind.KOTLIN_EXEC,
              CompilerOption.Compiler.KOTLIN),
          new EntryPoint(
              "org.jetbrains.kotlin.cli.common.CLICompiler",
              "exec",
              3,
              2,
              -1,
              EntryPoint.Kind.KOTLIN_EXEC,
              CompilerOption.Compiler.KOTLIN),
          new EntryPoint(
              "org.jetbrains.kotlin.cli.common.CLICompiler",
              "exec",
              4,
              3,
              -1,
              EntryPoint.Kind.KOTLIN_EXEC,
              CompilerOption.Compiler.KOTLIN),
          new EntryPoint(
              "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
              "main",
              1,
              0,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.KOTLIN),

          // ============ Kotlin: the source files it parses ============
          //
          // `KtPsiFactory#createFile(String)` and `#createFile(String, String)` create the PSI
          // file for a source. The first argument is the file's path.
          //
          // **Why the PSI factory and not the compiler's own file list.** The Kotlin compiler has
          // several places that hold "the files I am compiling" -- the environment's
          // `getSourceFiles()`, the compiler's `compileModules$cli`, the frontend pipeline's
          // artifacts -- and which of them the CLI actually reaches depends on the version and on
          // the path taken. The *phased* compiler, which is what Kotlin 2.x's CLI uses, goes
          // through the pipeline and never asks the environment for its files.
          //
          // Hooking the wrong one means the advice never fires, and the capture never appears --
          // with no error, because the transform succeeded and the method simply was not called.
          //
          // The PSI factory is different: **every** source file the compiler parses is created
          // through it, on every path. It is the one point that is reliably reached, and the path
          // is in its argument.
          //
          // Verified against kotlin-compiler-embeddable 2.2.0 with javap:
          //   public final org.jetbrains.kotlin.psi.KtFile createFile(java.lang.String);
          //   public final org.jetbrains.kotlin.psi.KtFile createFile(java.lang.String, java.lang.String);
          new EntryPoint(
              "org.jetbrains.kotlin.psi.KtPsiFactory",
              "createFile",
              1,
              0,
              -1,
              EntryPoint.Kind.KOTLIN_SOURCE_FILE,
              CompilerOption.Compiler.KOTLIN),
          new EntryPoint(
              "org.jetbrains.kotlin.psi.KtPsiFactory",
              "createFile",
              2,
              0,
              -1,
              EntryPoint.Kind.KOTLIN_SOURCE_FILE,
              CompilerOption.Compiler.KOTLIN),

          // ============ Scala 3: the driver ============
          //
          // `dotty.tools.dotc.Main#main(String[])` is only a forwarder:
          // it calls `Main$.main`, which is `Driver#main` (Scala's `object`
          // Main extends the `Driver` class). So the method that really runs
          // is on `Driver`.
          //
          // **Why `Driver#main` and not `Main#main`.** The static forwarder
          // on `Main` is a thin shell -- the argv is passed straight
          // through -- so hooking it works too. But `Driver#main` is the
          // one that is called on every path, including the ones that go
          // through `process`.
          //
          // Verified against scala3-compiler 3.10.0 with javap:
          //   public class dotty.tools.dotc.Driver {
          //     public void main(java.lang.String[]);
          //     public dotty.tools.dotc.reporting.Reporter process(java.lang.String[]);
          //   }
          new EntryPoint(
              "dotty.tools.dotc.Main",
              "main",
              1,
              0,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.SCALA3),
          new EntryPoint(
              "dotty.tools.dotc.Driver",
              "main",
              1,
              0,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.SCALA3),
          new EntryPoint(
              "dotty.tools.dotc.Driver",
              "process",
              1,
              0,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.SCALA3),

          // ============ Scala 3: the sources it compiles ============
          //
          // `dotty.tools.dotc.Driver#doCompile(Compiler, List<AbstractFile>, Context)` receives
          // the files the run compiles, and the compile has run by the time it returns. It is the
          // Scala 3 counterpart of javac's `JavaCompiler#compile`.
          //
          // Verified against scala3-compiler 3.10.0 with javap:
          //   public dotty.tools.dotc.reporting.Reporter doCompile(dotty.tools.dotc.Compiler,
          //       scala.collection.immutable.List<dotty.tools.io.AbstractFile>,
          //       dotty.tools.dotc.core.Contexts$Context);
          new EntryPoint(
              "dotty.tools.dotc.Driver",
              "doCompile",
              3,
              -1,
              -1,
              EntryPoint.Kind.SCALA3_COMPILE_END,
              CompilerOption.Compiler.SCALA3),

          // ============ Scala 2: the driver ============
          //
          // `scala.tools.nsc.Main#main(String[])` delegates to
          // `Main#process(String[])`, which returns a boolean.
          //
          // Verified against scala-compiler 2.13.15 with javap:
          //   public static boolean process(java.lang.String[]);
          new EntryPoint(
              "scala.tools.nsc.Main",
              "main",
              1,
              0,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.SCALA2),
          new EntryPoint(
              "scala.tools.nsc.Main",
              "process",
              1,
              0,
              -1,
              EntryPoint.Kind.ARGV,
              CompilerOption.Compiler.SCALA2),

          // ============ Scala 2: the compiled sources ============
          //
          // `scala.tools.nsc.Global#compile(List<String>)` receives the
          // source paths the run compiles.
          //
          // Verified against scala-compiler 2.13.15 with javap:
          //   public void compile(scala.collection.immutable.List<java.lang.String>);
          new EntryPoint(
              "scala.tools.nsc.Global",
              "compile",
              1,
              -1,
              -1,
              EntryPoint.Kind.COMPILED_SOURCES,
              CompilerOption.Compiler.SCALA2));

  private CompilerEntryPoints() {}

  /** Every entry point the agent hooks. */
  public static List<EntryPoint> all() {
    return ENTRY_POINTS;
  }

  /** The entry points that live in a given class. */
  public static List<EntryPoint> forType(String typeName) {
    List<EntryPoint> out = new ArrayList<>();
    for (EntryPoint e : ENTRY_POINTS) {
      if (e.typeName().equals(typeName)) {
        out.add(e);
      }
    }
    return out;
  }

  /** The distinct class names the agent hooks. */
  public static List<String> types() {
    List<String> out = new ArrayList<>();
    for (EntryPoint e : ENTRY_POINTS) {
      if (!out.contains(e.typeName())) {
        out.add(e.typeName());
      }
    }
    return out;
  }

  /** The entry points of one compiler. */
  public static List<EntryPoint> forCompiler(CompilerOption.Compiler compiler) {
    List<EntryPoint> out = new ArrayList<>();
    for (EntryPoint e : ENTRY_POINTS) {
      if (e.compiler() == compiler) {
        out.add(e);
      }
    }
    return out;
  }
}

