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

import java.lang.instrument.Instrumentation;
import java.util.List;
import net.bytebuddy.agent.builder.AgentBuilder;
import net.bytebuddy.asm.Advice;
import net.bytebuddy.description.method.MethodDescription;
import net.bytebuddy.description.type.TypeDescription;
import net.bytebuddy.dynamic.DynamicType;
import net.bytebuddy.matcher.ElementMatcher;
import net.bytebuddy.matcher.ElementMatchers;
import net.bytebuddy.utility.JavaModule;

/**
 * The {@link Advice} hooks the agent weaves into the compiler.
 *
 * <p><b>This class must be public.</b> ByteBuddy's {@code Advice} inlines the hook's bytecode into
 * the target method, and the inlined code calls the advice class's method. A package-private advice
 * class cannot be called from the instrumented class, and ByteBuddy fails to apply the advice --
 * <b>silently</b>, because the failure surfaces as a transform error that a default agent swallows.
 *
 * <p><b>Why Advice and not a hand-written ASM visitor.</b> {@code Advice} is ByteBuddy's
 * <em>inlining</em> instrumentation: the hook's body is copied into the target method, so it needs
 * neither a super method nor a delegation target. It works on a plain static method such as {@code
 * Main#compile(String[])} exactly as well as on an instance method.
 *
 * <p><b>One advice class per argument position.</b> {@code @Advice.Argument(N)} takes a constant
 * ordinal, so a hook that reads the compiler options from a different argument needs a different
 * advice class. There are only a few positions in play, so there are a few classes.
 *
 * <p><b>Why the Kotlin and Scala hooks do not read the argv the same way.</b> The three languages
 * deliver the compiler arguments in three different shapes, and the advice has to match each one:
 *
 * <ul>
 *   <li>javac's CLI takes a {@code String[]} at argument 0.
 *   <li>kotlinc's {@code exec} takes a {@code PrintStream} and then a varargs {@code String[]}, so
 *       the argv is at argument 1 -- and the advice is bound to {@code @Advice.Argument(1)}.
 *   <li>scalac's {@code main} and {@code process} take a {@code String[]} at argument 0, like
 *       javac.
 * </ul>
 */
public final class CompilerHooks {

  private CompilerHooks() {}

  /**
   * Hook for the entry points whose compiler options are argument 0.
   *
   * <p>Used by {@code com.sun.tools.javac.Main#compile(String[])}, by the ECJ driver and by the
   * sjavac drivers.
   */
  public static final class ArgvAtZero {

    private ArgvAtZero() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(0) String[] arguments) {
      CompiledSourcesBridge.recordArgv(arguments);
    }
  }

  /**
   * Hook for the entry points whose compiler options are argument 1.
   *
   * <p><b>This is Kotlin's {@code CLICompiler#exec(PrintStream, String...)}.</b> The signature is
   * {@code exec(PrintStream, String...)}, so the argv is the second parameter. Binding the advice
   * to argument 1 is what makes the Kotlin capture work at all -- binding it to argument 0 would
   * hand the advice a {@code PrintStream} where it expects a {@code String[]}, and the advice
   * would fail at the first cast.
   */
  public static final class ArgvAtOne {

    private ArgvAtOne() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(1) String[] arguments) {
      CompiledSourcesBridge.recordArgv(arguments, "kotlin");
    }
  }

  /**
   * Hook for the entry points whose compiler options are argument 2.
   *
   * <p><b>This is Kotlin's other {@code exec} overloads.</b> {@code
   * exec(PrintStream, MessageRenderer, String...)} and {@code
   * exec(MessageCollector, Services, String...)} both put the argv at argument 2.
   */
  public static final class ArgvAtTwo {

    private ArgvAtTwo() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(2) String[] arguments) {
      CompiledSourcesBridge.recordArgv(arguments, "kotlin");
    }
  }

  /**
   * Hook for the entry points whose compiler options are argument 3.
   *
   * <p><b>This is Kotlin's widest {@code exec} overload</b>, {@code
   * exec(PrintStream, Services, MessageRenderer, String[])}.
   */
  public static final class ArgvAtThree {

    private ArgvAtThree() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(3) String[] arguments) {
      CompiledSourcesBridge.recordArgv(arguments, "kotlin");
    }
  }

  /**
   * Hook for the entry points whose compiler options are argument 4.
   *
   * <p><b>This is javac's {@code JavacTool#run(InputStream, OutputStream, OutputStream,
   * String...)}.</b> The argv is the <em>fourth</em> parameter: the first three are the streams
   * the diagnostics are written to.
   *
   * <p><b>Why the index matters.</b> Binding the advice to argument 0 would hand it the {@code
   * InputStream} where it expects a {@code String[]}. ByteBuddy refuses to apply such an advice --
   * and it refuses loudly, but only when the transform runs. The compile itself goes on, so the
   * capture is silently missing.
   */
  public static final class ArgvAtFour {

    private ArgvAtFour() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(4) String[] arguments) {
      CompiledSourcesBridge.recordArgv(arguments);
    }
  }

  /**
   * Hook for Eclipse's compiler.
   *
   * <p>ECJ's options are not javac's options: it spells the classpath {@code --class-path}, and it
   * has options javac does not have ({@code -src}, {@code -profile}, {@code -log}). Parsing them
   * with javac's table would drop the classpath, which is the one thing a static analyser cannot do
   * without.
   */
  public static final class EcjArgv {

    private EcjArgv() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(0) String[] arguments) {
      CompiledSourcesBridge.recordArgv(arguments, "ecj");
    }
  }

  /**
   * Hook for the sjavac server.
   *
   * <p>sjavac has its own option set, and it normalizes how the arguments are attached to the
   * options ({@code SjavacOptions#normalizeArgumentAttachment}).
   */
  public static final class SjavacArgv {

    private SjavacArgv() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(0) String[] arguments) {
      CompiledSourcesBridge.recordArgv(arguments, "sjavac");
    }
  }

  /**
   * Hook for the Kotlin compiler.
   *
   * <p>Kotlin's options are its own: {@code -classpath} means the classpath, but {@code -source}
   * means a source <em>root</em> -- not javac's language level. Parsing Kotlin's argv with javac's
   * table would record the source root as a language level, which is confidently wrong.
   */
  public static final class KotlinArgv {

    private KotlinArgv() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(0) String[] arguments) {
      CompiledSourcesBridge.recordArgv(arguments, "kotlin");
    }
  }

  /**
   * Hook for the Scala compilers.
   *
   * <p>Scala 3 and Scala 2 spell the options they share the same way ({@code -classpath}, {@code
   * -d}, {@code -sourcepath}), and the ones they do not share are separate entries in the option
   * table. So one advice class serves both, and the compiler is decided by the entry point.
   */
  public static final class ScalaArgv {

    private ScalaArgv() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(0) String[] arguments) {
      CompiledSourcesBridge.recordArgv(arguments, "scala3");
    }
  }

  /**
   * Hook for the Scala 2 compiler.
   *
   * <p>Scala 2's option table differs from Scala 3's in a few places that matter ({@code
   * -dependencyfile}, {@code -Ystop-after}), so it gets its own hook rather than sharing Scala 3's.
   */
  public static final class Scala2Argv {

    private Scala2Argv() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(0) String[] arguments) {
      CompiledSourcesBridge.recordArgv(arguments, "scala2");
    }
  }

  /**
   * Hook for the entry points whose compiler options are argument 1, for javac.
   *
   * <p>The same shape as {@link ArgvAtOne}, but parsed with javac's option table rather than
   * Kotlin's. The two differ: javac's {@code -source} is a language level, Kotlin's is a source
   * <em>root</em>.
   */
  public static final class ArgvAtOneJavac {

    private ArgvAtOneJavac() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(1) String[] arguments) {
      CompiledSourcesBridge.recordArgv(arguments);
    }
  }

  /**
   * Hook for the entry points whose compiler options are argument 2, for javac.
   */
  public static final class ArgvAtTwoJavac {

    private ArgvAtTwoJavac() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(2) String[] arguments) {
      CompiledSourcesBridge.recordArgv(arguments);
    }
  }

  /**
   * Hook for the entry points whose compiler options are argument 3, for javac.
   */
  public static final class ArgvAtThreeJavac {

    private ArgvAtThreeJavac() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(3) String[] arguments) {
      CompiledSourcesBridge.recordArgv(arguments);
    }
  }

  /**
   * Hook for the {@code javax.tools} API entry point.
   *
   * <p>{@code JavacTool#getTask(Writer, FileManager, DiagnosticListener, Iterable options, Iterable
   * classes, Iterable files)} receives the compiler options in argument 3 and the source files in
   * argument 5.
   *
   * <p><b>The returned task is remembered too.</b> The task is what an IDE later drives with {@code
   * call()} or {@code generate()}, and those methods carry no options. Remembering the task here is
   * what lets the IDE path be captured at all -- the options cannot be read back off the task
   * later, because the {@code jdk.compiler} module does not export the package the task lives in.
   */
  public static final class ApiOptions {

    private ApiOptions() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(
        @Advice.Argument(3) Iterable<?> options, @Advice.Argument(5) Iterable<?> sources) {
      CompiledSourcesBridge.recordApiOptions(options, sources);
    }
  }

  /**
   * Hook for the task-creating {@code javax.tools} API entry point.
   *
   * <p>Same method as {@link ApiOptions}, but this one also remembers the task that was created, so
   * that the options can be attached to the compile the task eventually performs.
   *
   * <p><b>Why the return value is needed.</b> {@code JavacTaskImpl#call()} and {@code
   * JavacTaskImpl#generate()} take no options: they were set on the task when it was created. The
   * only place both the options and the task are visible at the same time is inside {@code
   * getTask}.
   */
  public static final class ApiOptionsWithTask {

    private ApiOptionsWithTask() {}

    @Advice.OnMethodExit(suppress = Throwable.class)
    public static void exit(
        @Advice.Argument(3) Iterable<?> options,
        @Advice.Argument(5) Iterable<?> sources,
        @Advice.Return Object task) {
      CompiledSourcesBridge.rememberTask(
          task, CompiledSourcesBridge.toStrings(options), CompiledSourcesBridge.toPaths(sources));
    }
  }

  /**
   * Hook for the IDE path.
   *
   * <p>{@code JavacTaskImpl#call()} and {@code JavacTaskImpl#generate()} take no options: they were
   * set on the task when it was created, by {@code JavacTool#getTask}. The options are looked up
   * from the ones remembered then.
   *
   * <p>This hook does not write the capture. It stages the compile's options, and the capture is
   * written when the compile itself ends -- by {@link CompileFinished}. A capture written here
   * would report the sources the compile was <em>given</em>, not the ones it really compiled.
   */
  public static final class TaskOptions {

    private TaskOptions() {}

    @Advice.OnMethodExit(suppress = Throwable.class)
    public static void exit(@Advice.This Object task) {
      CompiledSourcesBridge.record(task, Jdk8.of());
    }
  }

  /**
   * Hook for the compiler's compile step.
   *
   * <p>{@code JavaCompiler#compile(Collection, Collection, Iterable, Collection)} is the one
   * compile step both the command line and the {@code javax.tools} API go through. When it returns,
   * the compile has run, so the sources it really compiled are known -- including the ones it found
   * on the source path -- and the capture can be written.
   *
   * <p><b>Why the depth is counted.</b> The compiler's generate step is inside its compile step:
   * {@code compile} desugars and then calls {@code generate}. Both are hooked -- the IDE's {@code
   * JavacTaskImpl#generate()} path does not go through {@code compile} at all -- so without the
   * depth a compile that goes through {@code compile} would write two captures for one compile.
   * Only the outermost step writes.
   */
  public static final class CompileFinished {

    private CompileFinished() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter() {
      CompiledSourcesBridge.enterCompileStep();
    }

    @Advice.OnMethodExit(suppress = Throwable.class)
    public static void exit() {
      if (!CompiledSourcesBridge.exitCompileStep()) {
        // An inner step: the outer one writes the capture.
        return;
      }
      CompiledSourcesBridge.recordCompileFinished("javac");
    }
  }

  /**
   * Hook for ECJ's compile step.
   *
   * <p>{@code org.eclipse.jdt.internal.compiler.Compiler#compile(ICompilationUnit[])} is the one
   * compile step every ECJ compile goes through. It has no nesting inside it, so it does not need
   * the depth counting that javac's does.
   */
  public static final class EcjCompileFinished {

    private EcjCompileFinished() {}

    @Advice.OnMethodExit(suppress = Throwable.class)
    public static void exit() {
      CompiledSourcesBridge.recordCompileFinished("ecj");
    }
  }

  /**
   * Hook for javac's parse step: the file being parsed.
   *
   * <p>Every source the compiler parses passes through {@code JavaCompiler#parse(JavaFileObject,
   * CharSequence)}. The first argument is the file, and the second its contents.
   */
  public static final class ParsedSource {

    private ParsedSource() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(0) Object file) {
      CompiledSourcesBridge.recordParsed(CompiledSourcesBridge.pathOf(file));
    }
  }

  /**
   * Hook for ECJ's parse step: the units it is about to compile.
   *
   * <p>{@code Compiler#beginToCompile(ICompilationUnit[])} is called with the units before they
   * are compiled, so it sees the same set of files that javac's {@code parse} sees.
   *
   * <p>The units are ECJ's own {@code ICompilationUnit} objects, whose file name is a {@code
   * char[]} -- the only one of the three languages that is not a String.
   */
  public static final class EcjParsedSources {

    private EcjParsedSources() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(0) Object units) {
      for (String path : CompiledSourcesBridge.pathsOf(units)) {
        CompiledSourcesBridge.recordParsed(path);
      }
    }
  }

  /**
   * Hook for the Kotlin compiler's parse step: the file being parsed.
   *
   * <p>{@code KtPsiFactory#createFile(String)} creates the PSI file for a source. The first
   * argument is the file's path.
   *
   * <p><b>Why the parse step and not the compiler's file list.</b> The Kotlin compiler has
   * several places that hold "the files I am compiling", and which of them the CLI reaches depends
   * on the version and on the path taken -- the phased compiler, which is what Kotlin 2.x's CLI
   * uses, goes through the pipeline and never asks the environment for its files. The PSI factory
   * is different: <em>every</em> source the compiler parses is created through it, on every path.
   *
   * <p>Verified against kotlin-compiler-embeddable 2.2.0 with javap:
   *   public final org.jetbrains.kotlin.psi.KtFile createFile(java.lang.String);
   *   public final org.jetbrains.kotlin.psi.KtFile createFile(java.lang.String, java.lang.String);
   */
  public static final class KotlinSourceFile {

    private KotlinSourceFile() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(0) String path) {
      CompiledSourcesBridge.recordParsed(path);
    }
  }

  /**
   * Hook for the Kotlin compiler's exit.
   *
   * <p>{@code CLICompiler#exec(PrintStream, String...)} returns the compiler's exit code, and the
   * compile has run by then. So the sources the compiler parsed are known, and the capture can be
   * written.
   *
   * <p><b>Why the exit and not a "compile end" method.</b> The Kotlin compiler has no single
   * method that means "the compile is over" in the way javac's {@code JavaCompiler#compile} does.
   * The exit code is the one thing every path produces, and it is produced last.
   */
  public static final class KotlinCompileFinished {

    private KotlinCompileFinished() {}

    @Advice.OnMethodExit(suppress = Throwable.class)
    public static void exit() {
      CompiledSourcesBridge.recordCompileFinished("kotlin");
    }
  }


  /**
   * Hook for Scala 3's compile step.
   *
   * <p>{@code dotty.tools.dotc.Driver#doCompile(Compiler, List<AbstractFile>, Context)} receives
   * the files the run compiles, and the compile has run by the time it returns.
   *
   * <p>The files are a Scala {@code List}, which the agent cannot name -- so they are walked
   * reflectively.
   *
   * <p>Verified against scala3-compiler 3.10.0 with javap:
   *   public Reporter doCompile(Compiler, scala.collection.immutable.List<AbstractFile>, Context);
   */
  public static final class Scala3CompileFinished {

    private Scala3CompileFinished() {}

    @Advice.OnMethodExit(suppress = Throwable.class)
    public static void exit(@Advice.Argument(1) Object files) {
      List<String> paths = CompiledSourcesBridge.pathsOf(files);
      for (String path : paths) {
        CompiledSourcesBridge.recordParsed(path);
      }
      CompiledSourcesBridge.recordCompileFinished("scala3", paths);
    }
  }

  /**
   * Hook for the Scala 3 run: the files it holds.
   *
   * <p>{@code dotty.tools.dotc.Run#files()} returns the {@code AbstractFile} set. A Scala
   * {@code AbstractFile} knows its path through {@code path()}, and through {@code jpath()} for the
   * Java path form.
   */
  public static final class Scala3SourceFiles {

    private Scala3SourceFiles() {}

    @Advice.OnMethodExit(suppress = Throwable.class)
    public static void exit(@Advice.Return Object files) {
      List<String> paths = CompiledSourcesBridge.pathsOf(files);
      for (String path : paths) {
        CompiledSourcesBridge.recordParsed(path);
      }
      CompiledSourcesBridge.recordCompileFinished("scala3", paths);
    }
  }

  /**
   * Hook for Scala 2's compile: the source paths it compiles.
   *
   * <p>{@code scala.tools.nsc.Global#compile(List<String>)} receives the source paths directly, so
   * no reflection is needed at all.
   */
  public static final class Scala2SourceFiles {

    private Scala2SourceFiles() {}

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(0) Object sources) {
      List<String> paths = CompiledSourcesBridge.stringsOf(sources);
      for (String path : paths) {
        CompiledSourcesBridge.recordParsed(path);
      }
      CompiledSourcesBridge.recordCompileFinished("scala2", paths);
    }
  }

  /**
   * Install the agent.
   *
   * <p><b>Why the class loader matcher has to be widened.</b> ByteBuddy's {@code AgentBuilder.Default}
   * ignores two class loaders by default: the bootstrap loader (a null loader) and the platform loader.
   * A compiler class loaded by either of them is therefore never transformed -- and the advice never
   * fires, which means the compile succeeds, no capture appears, and nothing says why.
   *
   * <p>This is not a theoretical case. The same javac, reached through different class paths, is loaded
   * by different loaders:
   *
   * <ul>
   *   <li>{@code java -cp tools.jar com.sun.tools.javac.Main} -- the application loader, transformed, and
   *       the capture works.
   *   <li>{@code -Xbootclasspath/a:tools.jar} -- the bootstrap loader, ignored, and the capture is
   *       silently empty.
   *   <li>A JDK 9+ build where {@code jdk.compiler} is loaded by the platform loader -- ignored, and the
   *       capture is silently empty.
   * </ul>
   *
   * <p>The compiler is the thing being observed, so the loader it happens to live in must not decide
   * whether it is observed. The matcher below therefore lets the bootstrap and platform loaders
   * through.
   *
   * <p><b>Why that is safe here.</b> The matcher is combined with the type matcher, so only the
   * compiler's own classes -- the ones named in {@link CompilerEntryPoints} -- are ever transformed,
   * and only their named methods are woven into. The bootstrap loader's other classes are untouched.
   *
   * <p>The default ignore list is <em>replaced</em> rather than extended, and that is deliberate but
   * has to be done carefully: ByteBuddy's default list also protects ByteBuddy's own classes and the
   * reflection internals from being instrumented. The replacement below keeps that protection and
   * only drops the loader restriction, so the set of types that can be transformed is still exactly
   * the compiler's own types.
   *
   * @param inst the instrumentation interface
   */
  static void install(Instrumentation inst) {
    new AgentBuilder.Default()
        .disableClassFormatChanges()
        // ByteBuddy ignores the bootstrap loader and the platform loader by
        // default. The compiler's classes can live in either of them, and an
        // ignored class is a silently missing capture.
        //
        // The default ignore list is replaced here: it is a disjunction of
        // (type matcher, class loader matcher, module matcher) triples, and
        // the loader clause is the one that has to go. The type clauses
        // that protect ByteBuddy's own classes and the reflection
        // internals are kept.
        .ignore(
            new AgentBuilder.RawMatcher() {

              @Override
              public boolean matches(
                  TypeDescription typeDescription,
                  ClassLoader classLoader,
                  net.bytebuddy.utility.JavaModule module,
                  Class<?> classBeingRedefined,
                  java.security.ProtectionDomain protectionDomain) {
                if (classLoader == null
                    || classLoader.getClass().getName().equals("jdk.internal.loader.ClassLoaders$PlatformClassLoader")) {
                  // The bootstrap and the platform loader are the ones
                  // ByteBuddy excludes by default. The compiler's classes
                  // can be loaded by either of them.
                  return false;
                }
                // Everything else keeps ByteBuddy's own protection: its
                // classes, the reflection internals, and synthetic types.
                return typeDescription.getName().startsWith("net.bytebuddy.")
                    || typeDescription.getName().startsWith("sun.reflect.")
                    || typeDescription.getName().startsWith("jdk.internal.reflect.")
                    || typeDescription.isSynthetic();
              }
            })
        .with(
            new AgentBuilder.Listener.Adapter() {


              @Override
              public void onError(
                  String typeName,
                  ClassLoader classLoader,
                  net.bytebuddy.utility.JavaModule module,
                  boolean loaded,
                  Throwable throwable) {
                // The logger is not enough on its own. The agent is often
                // attached to a build that has no logging provider, and then
                // the warning goes nowhere -- and an advice that failed to
                // apply means the capture is silently incomplete. So the
                // failure is also reported on the compiler's own stderr.
                System.err.println(
                    "[ccap-capture] could not instrument " + typeName + ": " + throwable);
              }
            })
        .with(AgentBuilder.RedefinitionStrategy.RETRANSFORMATION)
        .type(ElementMatchers.namedOneOf(CompilerEntryPoints.types().toArray(new String[0])))
        .transform(CompilerHooks::instrument)
        .installOn(inst);
  }

  /**
   * The advice class for an entry point that carries its options in argument 0.
   *
   * <p>The compiler decides, because the option tables differ: ECJ spells the classpath {@code
   * --class-path}, and parsing its arguments with javac's table would drop the classpath.
   */
  private static Class<?> argvAdvice(CompilerOption.Compiler compiler) {
    if (compiler == null) {
      return ArgvAtZero.class;
    }
    // A plain switch, not a switch expression: the agent has to load on
    // Java 8, where the arrow form does not exist.
    switch (compiler) {
      case ECJ:
        return EcjArgv.class;
      case SJAVAC:
        return SjavacArgv.class;
      case KOTLIN:
        return KotlinArgv.class;
      case SCALA3:
        return ScalaArgv.class;
      case SCALA2:
        return Scala2Argv.class;
      case JAVAC:
      default:
        return ArgvAtZero.class;
    }
  }

  /**
   * The advice class for an entry point whose argv sits at the given index.
   *
   * <p><b>Why the index is part of the selection.</b> The advice is bound to a fixed argument
   * ordinal, so an entry point whose argv is at index 2 needs a different advice class from one
   * whose argv is at index 0. ByteBuddy refuses to apply an advice whose argument type does not
   * fit the target -- loudly, but only at transform time, and the compile goes on regardless. So a
   * mismatch is a silent loss of the capture.
   *
   * <p>The compiler is part of the selection too, because the option tables differ.
   */
  private static Class<?> argvAdviceAt(int index, CompilerOption.Compiler compiler) {
    // A plain switch, not a switch expression: the agent has to load on
    // Java 8, where the arrow form does not exist.
    switch (index) {
      case 0:
        return argvAdvice(compiler);
      case 1:
        return compiler == CompilerOption.Compiler.KOTLIN
            ? ArgvAtOne.class
            : ArgvAtOneJavac.class;
      case 2:
        return compiler == CompilerOption.Compiler.KOTLIN
            ? ArgvAtTwo.class
            : ArgvAtTwoJavac.class;
      case 3:
        return compiler == CompilerOption.Compiler.KOTLIN
            ? ArgvAtThree.class
            : ArgvAtThreeJavac.class;
      case 4:
        return ArgvAtFour.class;
      default:
        return null;
    }
  }

  /**
   * Weave the hooks into one type.
   *
   * <p>For each entry point declared on that type, the matching method gets the appropriate advice.
   */
  private static DynamicType.Builder<?> instrument(
      DynamicType.Builder<?> builder,
      TypeDescription typeDescription,
      ClassLoader classLoader,
      JavaModule module,
      java.security.ProtectionDomain pd) {

    DynamicType.Builder<?> result = builder;
    for (CompilerEntryPoints.EntryPoint entry :
        CompilerEntryPoints.forType(typeDescription.getName())) {
      ElementMatcher.Junction<MethodDescription> matcher =
          ElementMatchers.named(entry.methodName());
      if (entry.argumentCount() >= 0) {
        matcher = matcher.and(ElementMatchers.takesArguments(entry.argumentCount()));
      }
      // The arity alone does not identify an overload. Kotlin's compiler
      // has two three-argument `exec` methods, and only one of them takes
      // a String[] -- so the argv's own type has to be part of the match.
      if (entry.optionsIndex() >= 0
          && (entry.kind() == CompilerEntryPoints.EntryPoint.Kind.ARGV
              || entry.kind() == CompilerEntryPoints.EntryPoint.Kind.KOTLIN_EXEC)) {
        matcher =
            matcher.and(
                ElementMatchers.takesArgument(entry.optionsIndex(), String[].class));
      }

      // A plain switch, not a switch expression: the agent has to load on
      // Java 8, where the arrow form does not exist.
      switch (entry.kind()) {
        case ARGV:
          // The advice is chosen by the argv's index and by the compiler:
          // the index decides which argument is bound, and the compiler
          // decides which option table the arguments are parsed with.
          Class<?> argvAdvice = argvAdviceAt(entry.optionsIndex(), entry.compiler());
          if (argvAdvice != null) {
            result = result.visit(Advice.to(argvAdvice).on(matcher));
          }
          break;
        case FILE_OBJECTS:
          result = result.visit(Advice.to(ApiOptions.class).on(matcher));
          break;
        case FILE_OBJECTS_WITH_TASK:
          // Two hooks on the same method: the options are captured on the way in,
          // and the task is remembered with them on the way out.
          result = result.visit(Advice.to(ApiOptions.class).on(matcher));
          result = result.visit(Advice.to(ApiOptionsWithTask.class).on(matcher));
          break;
        case NO_OPTIONS:
          // The IDE path: the task carries no options of its own, so they
          // are looked up from the ones remembered at getTask.
          result = result.visit(Advice.to(TaskOptions.class).on(matcher));
          break;
        case PARSED_SOURCE: {
          // The compiler's parse step: the file being parsed is the source
          // the compiler really compiled.
          //
          // ECJ's parse step receives an array of units rather than one
          // file, so it gets its own advice.
          Class<?> advice =
              entry.compiler() == CompilerOption.Compiler.ECJ
                  ? EcjParsedSources.class
                  : ParsedSource.class;
          result = result.visit(Advice.to(advice).on(matcher));
          break;
        }
        case COMPILE_END: {
          // The compiler's compile step: the compile has run, so this is
          // where the capture is written with the sources it compiled.
          //
          // ECJ's compile step is a different method with a different
          // shape, so it gets its own advice -- and it does not need the
          // depth counting, because ECJ has no nested generate step.
          Class<?> advice =
              entry.compiler() == CompilerOption.Compiler.ECJ
                  ? EcjCompileFinished.class
                  : CompileFinished.class;
          result = result.visit(Advice.to(advice).on(matcher));
          break;
        }
        case KOTLIN_EXEC: {
          // Kotlin's exec: the argv is at the entry point's own index, and
          // the method's return is the compile's exit code.
          //
          // Both hooks are applied: the argv on the way in, and the
          // capture on the way out.
          Class<?> argv;
          switch (entry.optionsIndex()) {
            case 1:
              argv = ArgvAtOne.class;
              break;
            case 2:
              argv = ArgvAtTwo.class;
              break;
            case 3:
              argv = ArgvAtThree.class;
              break;
            default:
              argv = null;
              break;
          }
          if (argv != null) {
            result = result.visit(Advice.to(argv).on(matcher));
          }
          result = result.visit(Advice.to(KotlinCompileFinished.class).on(matcher));
          break;
        }
        case COMPILED_SOURCES: {
          // Scala's "here is what I compiled" step. Each language has its
          // own shape, so each has its own advice.
          Class<?> advice;
          switch (entry.compiler()) {
            case SCALA3:
              advice = Scala3SourceFiles.class;
              break;
            case SCALA2:
              advice = Scala2SourceFiles.class;
              break;
            default:
              advice = null;
              break;
          }
          if (advice != null) {
            result = result.visit(Advice.to(advice).on(matcher));
          }
          break;
        }
        case KOTLIN_SOURCE_FILE:
          // Kotlin's parse step: the file being parsed is the source the
          // compiler really compiled.
          result = result.visit(Advice.to(KotlinSourceFile.class).on(matcher));
          break;
        case SCALA3_COMPILE_END:
          // Scala 3's compile step: the sources are in its arguments.
          result = result.visit(Advice.to(Scala3CompileFinished.class).on(matcher));
          break;
        default:
          // no other kinds
          break;
      }
    }
    return result;
  }
}

