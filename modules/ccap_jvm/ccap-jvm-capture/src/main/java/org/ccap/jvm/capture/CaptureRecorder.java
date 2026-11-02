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

import java.io.IOException;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * The class the instrumented compiler calls into.
 *
 * <p><b>Why this class cannot be a normal part of the agent.</b> The instrumented bytecode lives in
 * the compiler's own classes ({@code com.sun.tools.javac.Main} and friends), which are loaded by the
 * application class loader. The agent jar, meanwhile, is only visible to the agent's own class
 * loader. A direct call from the compiler's code to an agent class would fail with {@code
 * NoClassDefFoundError}.
 *
 * <p>Appending a small jar to the <b>bootstrap</b> class loader solves it: the bootstrap loader is
 * the ancestor of every other loader, so a class placed there is reachable from the compiler, from
 * the application and from the agent alike.
 *
 * <p>That visibility comes with a constraint: <b>this class must not reference anything that is not
 * on the bootstrap loader</b>. It cannot use SLF4J, and it cannot call {@link CaptureSink}. It
 * writes the capture file itself. Keep this class dependency-free.
 *
 * <p><b>What is recorded.</b> Not just the argv. the reference's option parser goes further: it
 * reconstructs the <em>effective</em> compile -- the classpath that was actually used, the source
 * path, the encoding, the release level -- because the argv alone does not say what the compiler
 * saw. {@code -classpath ""} and no {@code -classpath} at all mean different things, and only the
 * second is knowable from the environment.
 */
public final class CaptureRecorder {

  /** Environment variable naming the directory captures are written to. */
  public static final String ENV_OUTPUT_DIR = "CCAP_CAPTURE_DIR";

  /**
   * System property naming the directory captures are written to.
   *
   * <p>The environment variable is the right way to configure the agent for a real build: the agent
   * is started by the build tool, and the build tool can set an environment variable but not a
   * system property. The system property exists for the case where the agent is installed <em>into
   * a running JVM</em> (the dynamic-attach path, and the tests): the environment of a running
   * process cannot be changed, but a system property can.
   *
   * <p>The system property wins when both are set.
   */
  public static final String PROP_OUTPUT_DIR = "ccap.capture.dir";

  /**
   * The identity of the compile most recently recorded, used to suppress duplicates.
   *
   * <p>A single compile reaches several hooked entry points: the command-line driver goes through
   * {@code com.sun.tools.javac.Main#compile} and then {@code com.sun.tools.javac.main.Main
   * #compile}; the API path goes through {@code JavacTool#getTask} and then {@code
   * JavacTaskImpl#call}. Each of them would otherwise write its own file, and the analyser would
   * see one compile several times over.
   *
   * <p><b>The key is the compile's identity, not its argv.</b> The argv differs between the CLI and
   * the API path even for the same compile, and it is identical for two genuinely different
   * compiles that happen to be invoked with the same options. The identity -- sources, output
   * directory, classpath, encoding, language level -- is what actually says "same compile".
   */
  private static volatile String lastIdentity;

  /**
   * How many distinct compiles are remembered for duplicate detection.
   *
   * <p>More than one, because a build compiles many modules in a row and the identity of the
   * previous compile is not enough to tell a repeat from a new one.
   */
  private static final int IDENTITY_HISTORY = 8;

  /** The identities of the compiles seen so far, most recent first. */
  private static final java.util.Set<String> SEEN_IDENTITIES =
      java.util.Collections.synchronizedSet(new java.util.LinkedHashSet<String>());

  /**
   * The compiler options of the tasks that have been created but not yet compiled.
   *
   * <p><b>This is how the IDE path gets its options.</b> {@code JavacTaskImpl#call()} and {@code
   * JavacTaskImpl#generate()} carry no options -- they were set on the task when it was created.
   * And they cannot be read back off the task: {@code com.sun.tools.javac.api} is not exported by
   * the {@code jdk.compiler} module, so every reflective access to the task's {@code options} field
   * fails with {@code InaccessibleObjectException} unless the JVM is started with {@code
   * --add-opens jdk.compiler/com.sun.tools.javac.api=ALL-UNNAMED}. Requiring that flag would make
   * the agent unusable in a normal build.
   *
   * <p>So the options are remembered at the point where they are still visible -- {@code
   * JavacTool#getTask}, which receives them as an argument -- and are looked up again when the task
   * is driven. The lookup is by identity, so two tasks never share options.
   */
  private static final Map<Object, List<String>> TASK_OPTIONS = new IdentityHashMap<>();

  /**
   * The source files of the tasks that have been created but not yet compiled.
   *
   * <p>Remembered for the same reason as the options.
   */
  private static final Map<Object, List<String>> TASK_SOURCES = new IdentityHashMap<>();

  /**
   * How many compiles are in flight on this thread.
   *
   * <p><b>Why the depth is counted.</b> The compiler's generate step is <em>inside</em> its compile
   * step: {@code JavaCompiler#compile} desugars and then calls {@code JavaCompiler#generate}. Both
   * are hooked, because the IDE's {@code JavacTaskImpl#generate()} path does not go through {@code
   * compile} at all -- but a compile that goes through {@code compile} would then write two
   * captures for one compile.
   *
   * <p>So the capture is written by the <em>outermost</em> step. The inner one sees a depth above
   * one and does nothing.
   */
  private static final ThreadLocal<int[]> COMPILE_DEPTH = ThreadLocal.withInitial(() -> new int[1]);

  /**
   * The sources the compiler parsed during the compile in flight.
   *
   * <p><b>Why the argv is not enough.</b> The command line names the sources it was given. The
   * compiler also compiles the sources it finds itself, through the source path: a class that is
   * referenced but not named is looked up on the source path, parsed, and compiled. A capture that
   * lists only the command-line sources therefore misses the code the compiler really compiled --
   * and that code is exactly the code a static analyser has to look at.
   *
   * <p>So the compiler's own list of parsed sources is recorded, and it is what the capture reports
   * as the source files. The command-line sources are still recorded, because a compile that names
   * a file the parser never reached -- a file that failed to parse, say -- is still a compile.
   *
   * <p><b>Why it is a thread-local.</b> A build that compiles modules in parallel runs several
   * compiles at once, and each one has its own parsed sources. A single shared set would mix them.
   */
  private static final ThreadLocal<java.util.LinkedHashSet<String>> PARSED_SOURCES =
      ThreadLocal.withInitial(java.util.LinkedHashSet::new);

  /**
   * The options of the compiles that have started but not finished.
   *
   * <p><b>Why the capture is written when the compile ends.</b> The sources the compiler really
   * compiled are only known once it has compiled them, so a capture written when the compile
   * <em>starts</em> cannot report them. The options, on the other hand, are known at the start --
   * the command line has them, and the API hands them over with the task -- so they are remembered
   * until the compile ends and the capture is written with everything.
   *
   * <p>This is a stack, not a single value, because the entry points nest: the command-line driver
   * is reached through two of them, and the API path through two others.
   */
  private static final java.util.Deque<PendingCompile> PENDING_COMPILES =
      new java.util.concurrent.ConcurrentLinkedDeque<>();

  /**
   * A compile that has started but has not finished yet.
   *
   * <p><b>Why this is a class and not a record.</b> Records arrived in Java 16, and the agent has
   * to load on Java 8 -- see {@link Jdk8}. The accessors keep the record's shape, so the callers do
   * not change.
   */
  private static final class PendingCompile {

    private final String api;
    private final CompilerOption.Compiler compiler;
    private final List<String> argv;
    private final List<String> sources;

    private PendingCompile(
        String api,
        CompilerOption.Compiler compiler,
        List<String> argv,
        List<String> sources) {
      this.api = api;
      this.compiler = compiler;
      this.argv = argv;
      this.sources = sources;
    }

    private String api() {
      return api;
    }

    private CompilerOption.Compiler compiler() {
      return compiler;
    }

    private List<String> argv() {
      return argv;
    }

    private List<String> sources() {
      return sources;
    }
  }

  private CaptureRecorder() {}

  /**
   * Remember a source the compiler parsed.
   *
   * <p>Called from the advice that runs inside the compiler, on the method every parsed source
   * passes through.
   *
   * @param path the path of the parsed source
   */
  public static void rememberParsedSource(String path) {
    if (path == null || Jdk8.isBlank(path)) {
      return;
    }
    PARSED_SOURCES.get().add(path);
  }

  /**
   * A compile step has begun.
   *
   * @return true when this is the outermost compile step
   */
  public static boolean enterCompileStep() {
    int[] depth = COMPILE_DEPTH.get();
    depth[0]++;
    return depth[0] == 1;
  }

  /**
   * A compile step has ended.
   *
   * @return true when this was the outermost compile step
   */
  public static boolean exitCompileStep() {
    int[] depth = COMPILE_DEPTH.get();
    if (depth[0] <= 0) {
      return true;
    }
    depth[0]--;
    return depth[0] == 0;
  }

  /**
   * The sources the compiler parsed during the compile in flight.
   *
   * <p>Drained by the caller: the sources belong to one compile, and the next compile starts a
   * fresh set.
   *
   * @return the parsed source paths, in the order they were parsed
   */
  static List<String> drainParsedSources() {
    java.util.LinkedHashSet<String> set = PARSED_SOURCES.get();
    List<String> out = new ArrayList<>(set);
    set.clear();
    return out;
  }

  /**
   * Record a captured CLI invocation.
   *
   * @param arguments the argv the compiler received
   */
  public static void onArgv(String[] arguments) {
    onArgv(arguments, CompilerOption.Compiler.JAVAC);
  }

  /**
   * Record a captured CLI invocation, for a known compiler.
   *
   * <p>The compiler matters because the option tables differ: ECJ spells the classpath {@code
   * --class-path}, sjavac has its own set, and Kotlin's {@code -source} names a source root rather
   * than a language level. Parsing one compiler's arguments with another's table would drop the
   * classpath, which is the one thing a static analyser cannot do without.
   *
   * @param arguments the argv the compiler received
   * @param compiler which compiler received it
   */
  public static void onArgv(String[] arguments, CompilerOption.Compiler compiler) {
    if (arguments == null || arguments.length == 0) {
      return;
    }
    List<String> argv = new ArrayList<>(arguments.length);
    for (String a : arguments) {
      argv.add(a);
    }
    // The compile is staged, not written: the sources it really compiled are
    // only known once it has compiled them.
    PENDING_COMPILES.push(
        new PendingCompile("cli", compiler, argv, givenSourcesOf(argv, compiler)));
  }

  /**
   * Record a captured Kotlin CLI invocation.
   *
   * <p><b>Why Kotlin needs its own entry point.</b> The Kotlin compiler's {@code exec} takes the
   * argv as a varargs array behind a {@code PrintStream}, and the source files are the
   * <em>free arguments</em> -- the positional ones -- while the classpath and the output directory
   * live in the generated arguments object, not in the argv.
   *
   * <p>So the argv alone is enough to find the sources, but not to find the classpath. The
   * classpath is read separately by {@link #onKotlinOptions}.
   *
   * @param arguments the argv the compiler received
   */
  public static void onKotlinArgv(String[] arguments) {
    onArgv(arguments, CompilerOption.Compiler.KOTLIN);
  }

  /**
   * The source files a command line names.
   *
   * <p>Only the positional arguments with a source extension are sources. The other values belong
   * to options, and a jar on the classpath is not a source.
   *
   * @param argv the compiler's arguments
   * @param compiler which compiler received them
   * @return the source paths the command line named
   */
  private static List<String> givenSourcesOf(List<String> argv, CompilerOption.Compiler compiler) {
    List<String> out = new ArrayList<>();
    for (java.nio.file.Path p : JvmOptions.parse(argv, compiler).sourceFiles()) {
      out.add(p.toString());
    }
    return out;
  }

  /**
   * Record a compile that has finished.
   *
   * <p>This is where the capture is written. The compile has run, so the sources it parsed are
   * known -- including the ones it found on the source path -- and the capture can report the
   * effective compile rather than the request for one.
   *
   * <p><b>Why the staged compile is matched, not just taken.</b> One compile is seen through
   * several entry points -- the command line through three of them, the API through two others --
   * so the same compile is staged more than once. And a build may have more than one compile in
   * flight: Gradle, for one, creates the task for the next module before the previous module's
   * compile has finished.
   *
   * <p>So the staged compile is chosen by <em>matching</em>: the staged compile whose sources are
   * the ones this compile actually parsed.
   *
   * @param compiler which compiler did the compile
   */
  public static void onCompileFinished(CompilerOption.Compiler compiler) {
    try {
      List<String> parsed = drainParsedSources();
      PendingCompile pending = takePending(parsed);
      if (pending == null) {
        // A compile that was never staged -- for instance a compile the
        // agent saw only at its very end. There is nothing to report: the
        // options are unknown, and a capture without options is useless.
        return;
      }
      record(
          buildCapture(pending.argv(), merge(pending.sources(), parsed), pending.api(), compiler));
    } catch (Throwable t) {
      // A capture must never break the compile.
      System.err.println("[ccap-capture] could not record the compile: " + t);
    }
  }

  /**
   * Record a compile that has finished, for a compiler whose sources are known by another route.
   *
   * <p><b>Why Kotlin and Scala need this.</b> They have no "compile end" method that the recorder
   * can hook in the same way javac does -- their pipelines are different. So the compiler's own
   * "here are the files I compiled" step is hooked, and the capture is written right there: the
   * file list is already known at that point, which is what the capture needs.
   *
   * @param compiler which compiler did the compile
   * @param compiledSources the sources the compiler compiled
   */
  public static void onCompileFinished(
      CompilerOption.Compiler compiler, List<String> compiledSources) {
    try {
      List<String> parsed = drainParsedSources();
      List<String> sources = compiledSources == null ? Jdk8.of() : compiledSources;
      PendingCompile pending = takePending(sources);
      if (pending == null) {
        return;
      }
      record(
          buildCapture(
              pending.argv(), merge(pending.sources(), sources), pending.api(), compiler));
    } catch (Throwable t) {
      System.err.println("[ccap-capture] could not record the compile: " + t);
    }
  }

  /**
   * The staged compile that belongs to the compile that just finished.
   *
   * <p>The staged compile whose sources appear among the compiled sources is the view of this
   * compile. The most recent match wins: a compile that was staged again -- the command line
   * reaches three entry points for one compile -- is staged more than once, and the most recent
   * staging is the most complete one.
   *
   * @param compiled the sources the compiler compiled
   * @return the staged compile, or {@code null} when none of them matches
   */
  private static PendingCompile takePending(List<String> compiled) {
    List<PendingCompile> staged = new ArrayList<>();
    PendingCompile candidate;
    // The deque holds the most recent staging at its head, because the
    // staging pushes to the head. So the head is taken first, and the staged
    // compiles come out from the most recent to the oldest.
    while ((candidate = PENDING_COMPILES.pollFirst()) != null) {
      staged.add(candidate);
    }
    if (staged.isEmpty()) {
      return null;
    }
    for (PendingCompile p : staged) {
      if (overlap(p.sources(), compiled) > 0) {
        return p;
      }
    }
    // Nothing matched: the compile was staged without its sources, or the
    // compiler compiled files that were never named. The most recent staged
    // compile is the best guess.
    return staged.get(0);
  }

  /**
   * How many of a staged compile's sources appear among the compiled sources.
   *
   * <p>The paths are compared by their normalized form, because the same file can be named two
   * ways: the command line may name it relative to the working directory, while the compiler
   * reports it absolute.
   *
   * @param given the sources the staged compile was given
   * @param compiled the sources the compiler compiled
   * @return the number of sources they have in common
   */
  private static int overlap(List<String> given, List<String> compiled) {
    int count = 0;
    for (String s : given) {
      if (containsSource(compiled, s)) {
        count++;
      }
    }
    return count;
  }

  /**
   * Whether the compiled sources include a file.
   *
   * @param compiled the compiled source paths
   * @param candidate the path to look for
   * @return true when the file is among them
   */
  private static boolean containsSource(List<String> compiled, String candidate) {
    if (compiled.contains(candidate)) {
      return true;
    }
    String wanted = normalisePath(candidate);
    for (String p : compiled) {
      if (normalisePath(p).equals(wanted)) {
        return true;
      }
    }
    return false;
  }

  /**
   * A path in a form that can be compared.
   *
   * <p>The path is made absolute against the working directory, and its separators are made the
   * same on every platform, so that a path written one way and reported another is still recognized
   * as the same file.
   *
   * @param path the path to normalize
   * @return the comparable form
   */
  private static String normalisePath(String path) {
    if (path == null || Jdk8.isBlank(path)) {
      return "";
    }
    try {
      java.nio.file.Path p = Jdk8.path(path);
      if (!p.isAbsolute()) {
        p = Jdk8.path(System.getProperty("user.dir", ".")).resolve(p);
      }
      return p.normalize().toString().replace('\\', '/');
    } catch (RuntimeException e) {
      return path.replace('\\', '/');
    }
  }

  /**
   * The command-line sources and the compiled sources, together.
   *
   * <p>Both are kept. The command line names the sources the compile was given; the compiled
   * sources are the ones it really compiled. A compile that names a file the compiler never reached
   * -- a file that failed to parse, say -- is still a compile, so its source stays in the list.
   *
   * @param given the sources the compile was given
   * @param compiled the sources the compiler compiled
   * @return both, without duplicates
   */
  private static List<String> merge(List<String> given, List<String> compiled) {
    List<String> out = new ArrayList<>();
    for (String s : given) {
      if (s != null && !Jdk8.isBlank(s) && !out.contains(s)) {
        out.add(s);
      }
    }
    for (String s : compiled) {
      if (s != null && !Jdk8.isBlank(s) && !out.contains(s)) {
        out.add(s);
      }
    }
    return out;
  }

  /**
   * Remember a task's options and sources.
   *
   * <p>Called from the {@code getTask} hook, which has both.
   *
   * @param task the task that was created
   * @param options the compiler options
   * @param sources the source files
   */
  public static void rememberTask(Object task, List<String> options, List<String> sources) {
    if (task == null || options == null || options.isEmpty()) {
      return;
    }
    synchronized (TASK_OPTIONS) {
      TASK_OPTIONS.put(task, options);
      TASK_SOURCES.put(task, sources == null ? Jdk8.of() : sources);
    }
  }

  /**
   * The compiler options of a {@code getTask} call, as strings.
   *
   * @param options the options iterable, may be null
   * @return the options, empty when there are none
   */
  public static List<String> toStrings(Iterable<?> options) {
    List<String> out = new ArrayList<>();
    if (options == null) {
      return out;
    }
    for (Object o : options) {
      if (o != null) {
        out.add(String.valueOf(o));
      }
    }
    return out;
  }

  /**
   * The source files of a {@code getTask} call, as paths.
   *
   * @param sources the sources iterable, may be null
   * @return the source paths, empty when there are none
   */
  public static List<String> toPaths(Iterable<?> sources) {
    List<String> out = new ArrayList<>();
    if (sources == null) {
      return out;
    }
    for (Object o : sources) {
      if (o != null) {
        String path = toPath(o);
        if (path != null) {
          out.add(path);
        }
      }
    }
    return out;
  }

  /**
   * Remember the sources a task actually compiled.
   *
   * <p>Called from the advice that runs inside the compiler, where the list of parsed units is
   * visible. The list is keyed by the task's identity, so two tasks never share it.
   *
   * @param task the task that was driven
   * @param sources the source files the compiler compiled, as paths
   */
  public static void rememberCompiledSources(Object task, List<String> sources) {
    if (task == null || sources == null) {
      return;
    }
    // The task's own sources are the ones it was given; the parsed sources
    // are collected separately and merged in when the compile is written.
    synchronized (TASK_OPTIONS) {
      TASK_SOURCES.put(task, merge(TASK_SOURCES.getOrDefault(task, Jdk8.of()), sources));
    }
  }

  /**
   * Record a compile driven through a task object, as an IDE does.
   *
   * <p>{@code JavacTaskImpl#call()} and {@code JavacTaskImpl#generate()} carry no compiler options:
   * they were set on the task when it was created, by {@code JavacTool#getTask}. So the options are
   * looked up from the ones remembered then.
   *
   * <p><b>Why this does not stage a compile.</b> The {@code getTask} hook already staged it, and the
   * compile-end hook takes that one. Staging a second copy here would leave a stale entry behind,
   * and the <em>next</em> compile in the build would pick it up and report the previous compile's
   * options.
   *
   * @param task the task object, or {@code null}
   */
  public static void onTask(Object task) {
    if (task == null) {
      return;
    }
    try {
      releaseTask(task);
    } catch (Throwable t) {
      // A capture must never break the compile.
      System.err.println("[ccap-capture] could not record the compile: " + t);
    }
  }

  /**
   * Release a task that has been driven.
   *
   * <p>The task is consumed: it will not be driven again, and holding on to it would keep the
   * compiler's file manager alive.
   *
   * @param task the task that was driven
   */
  private static void releaseTask(Object task) {
    synchronized (TASK_OPTIONS) {
      TASK_OPTIONS.remove(task);
      TASK_SOURCES.remove(task);
    }
  }

  /**
   * Record a capture, unless this compile has already been recorded.
   *
   * <p>The duplicate check is on the compile's identity, not on its argv. One compile reaches
   * several entry points -- the CLI driver through two of them, the API path through two others --
   * and each would otherwise write a file. The identity says whether two captures describe the same
   * compile.
   *
   * <p><b>The history is bounded.</b> A build compiles module after module; keeping every identity
   * forever would grow without limit, and a build that compiles the same sources twice with
   * different options is two different compiles.
   *
   * @param entries the capture to record
   */
  private static void record(List<CaptureEntry> entries) {
    if (entries == null || entries.isEmpty()) {
      return;
    }
    CaptureEntry entry = entries.get(0);
    String identity = entry.compileIdentity();
    synchronized (SEEN_IDENTITIES) {
      if (SEEN_IDENTITIES.contains(identity)) {
        // The same compile, seen through another entry point.
        return;
      }
      SEEN_IDENTITIES.add(identity);
      while (SEEN_IDENTITIES.size() > IDENTITY_HISTORY) {
        java.util.Iterator<String> it = SEEN_IDENTITIES.iterator();
        it.next();
        it.remove();
      }
    }
    lastIdentity = identity;
    write(entries);
  }

  /**
   * Record a captured {@code javax.tools} API invocation.
   *
   * @param options the compiler options, as passed to {@code getTask}
   * @param sources the source files, as passed to {@code getTask}
   */
  public static void onApiOptions(Iterable<?> options, Iterable<?> sources) {
    try {
      doRecordApi(options, sources);
    } catch (Throwable t) {
      // A capture must never break the compile. Report it, but do not
      // propagate: the advice suppresses Throwable anyway, and a stack
      // trace on the compiler's stderr is noise the user cannot act on.
      System.err.println("[ccap-capture] could not record the compile: " + t);
    }
  }

  private static void doRecordApi(Iterable<?> options, Iterable<?> sources) {
    List<String> argv = new ArrayList<>();
    if (options != null) {
      for (Object o : options) {
        if (o != null) {
          argv.add(String.valueOf(o));
        }
      }
    }
    if (argv.isEmpty()) {
      return;
    }
    List<String> sourcePaths = new ArrayList<>();
    if (sources != null) {
      for (Object o : sources) {
        String path = toPath(o);
        if (path != null) {
          sourcePaths.add(path);
        }
      }
    }
    // Staged, not written: the compile has not run yet, so the sources it
    // really compiles are not known.
    PENDING_COMPILES.push(
        new PendingCompile("api", CompilerOption.Compiler.JAVAC, argv, sourcePaths));
  }

  /**
   * Assemble the capture.
   *
   * <p>This is where the argv is turned into the <em>effective</em> compile: the classpath is
   * resolved against the environment the way the compiler would have resolved it, the source path
   * is filled in from {@code -sourcepath} or the current directory, and the encoding defaults to
   * the platform's.
   */
  private static List<CaptureEntry> buildCapture(
      List<String> argv, List<String> extraSources, String api) {
    return buildCapture(argv, extraSources, api, CompilerOption.Compiler.JAVAC);
  }

  /**
   * Assemble the capture for a known compiler.
   *
   * <p>The compiler matters because the options differ: ECJ spells the classpath {@code
   * --class-path}, and sjavac has its own set. Parsing ECJ's arguments with javac's table would
   * drop the classpath entirely.
   */
  private static List<CaptureEntry> buildCapture(
      List<String> argv, List<String> extraSources, String api, CompilerOption.Compiler compiler) {
    JvmOptions options = JvmOptions.parse(argv, compiler);

    // The classpath the compiler actually used. `-classpath` wins; when it
    // is absent the compiler falls back to the CLASSPATH environment
    // variable, and then to the current directory.
    List<Path> classpath = new ArrayList<>(options.classpath());
    if (classpath.isEmpty()) {
      classpath.addAll(environmentClasspath());
    }

    // The source files. The CLI passes them as bare arguments; the API
    // passes them as JavaFileObjects.
    Set<Path> sources = new LinkedHashSet<>(options.sourceFiles());
    for (String s : extraSources) {
      try {
        sources.add(Jdk8.path(s));
      } catch (RuntimeException ignored) {
        // not a path -- skip
      }
    }

    List<CaptureEntry> entries = new ArrayList<>();
    entries.add(
        new CaptureEntry(
            api,
            compilerName(compiler),
            languageOf(compiler),
            argv,
            options.encodingName(),
            options.languageLevel(),
            options.outputDirectory(),
            classpath,
            options.sourcePath(),
            options.modulePath(),
            options.processorPath(),
            options.bootClassPath(),
            new ArrayList<>(sources),
            options.addModules(),
            options.addExports(),
            options.addReads(),
            options.preview(),
            options.isCompilation(),
            System.getProperty("java.version"),
            System.getProperty("java.home"),
            System.getProperty("user.dir")));
    return entries;
  }

  /**
   * The name a compiler is recorded under.
   *
   * <p>Lower case, and the same spelling in the capture file and in the compiler table of the
   * consumer -- so the two can be joined without a mapping table.
   */
  static String compilerName(CompilerOption.Compiler compiler) {
    // A plain switch, not a switch expression: the agent has to load on
    // Java 8, where the arrow form does not exist.
    switch (compiler) {
      case JAVAC:
        return "javac";
      case ECJ:
        return "ecj";
      case SJAVAC:
        return "sjavac";
      case KOTLIN:
        return "kotlin";
      case SCALA3:
        return "scala3";
      case SCALA2:
        return "scala2";
      default:
        return "capture";
    }
  }

  /**
   * The language a compiler compiles.
   *
   * <p><b>Why this is separate from the compiler name.</b> Two compilers compile the same language
   * -- javac and ECJ both compile Java -- and a consumer usually cares about the language, not
   * about which front end happened to do it.
   */
  static String languageOf(CompilerOption.Compiler compiler) {
    switch (compiler) {
      case JAVAC:
      case ECJ:
      case SJAVAC:
        return "java";
      case KOTLIN:
        return "kotlin";
      case SCALA3:
      case SCALA2:
        return "scala";
      default:
        return "java";
    }
  }

  /**
   * The classpath the compiler would use when the command line does not say.
   *
   * <p>{@code CLASSPATH} is the documented source; {@code java.class.path} is what the JVM actually
   * started with, and it is what a build tool's forked compiler inherits.
   */
  private static List<Path> environmentClasspath() {
    List<Path> out = new ArrayList<>();
    String cp = System.getenv("CLASSPATH");
    if (cp == null || Jdk8.isBlank(cp)) {
      cp = System.getProperty("java.class.path");
    }
    if (cp == null || Jdk8.isBlank(cp)) {
      return out;
    }
    // The separator is platform-dependent: ';' on Windows, ':' elsewhere.
    // Splitting on both would cut a Windows drive letter in half.
    char separator =
        System.getProperty("os.name", "").toLowerCase(Locale.ROOT).contains("win") ? ';' : ':';
    for (String part : cp.split(java.util.regex.Pattern.quote(String.valueOf(separator)))) {
      String trimmed = part.trim();
      if (!trimmed.isEmpty()) {
        try {
          out.add(Jdk8.path(trimmed));
        } catch (RuntimeException ignored) {
          // not a path -- skip
        }
      }
    }
    return out;
  }

  /**
   * Turn a source object into a path.
   *
   * <p>The API hands the compiler a mix of {@code PathFileObject}s, {@code JavaFileObjects} and
   * plain strings, so all three are handled.
   *
   * <p><b>The object is handled reflectively, on purpose.</b> The recorder runs on the bootstrap
   * class loader, which sees only the JDK's bootstrap classes. {@code javax.tools.FileObject} lives
   * in the {@code java.compiler} module, which the <em>platform</em> loader owns -- so a direct
   * reference to it would fail with {@code NoClassDefFoundError} on the bootstrap copy of this
   * class. A reflective call is resolved against the <em>calling</em> object's class loader, which
   * is the one that can see {@code javax.tools}.
   */
  private static String toPath(Object source) {
    if (source == null) {
      return null;
    }
    if (source instanceof Path) {
      Path p = (Path) source;
      return p.toString();
    }
    if (source instanceof String) {
      String s = (String) source;
      // A plain path. The API is given file objects, but a caller that
      // builds its own file manager may hand over strings, and a string is
      // already a path.
      return Jdk8.isBlank(s) ? null : s;
    }
    // A JavaFileObject.
    //
    // `toUri()` is the obvious accessor, but it is not reliable: the
    // JDK's own SimpleFileObject throws UnsupportedOperationException
    // from it. `getName()` always works -- it is the path the file
    // manager was given.
    String name = callString(source, "getName");
    if (name != null && !Jdk8.isBlank(name)) {
      return name;
    }
    // Fall back to the URI.
    String uri = callString(source, "toUri");
    if (uri == null) {
      return null;
    }
    try {
      URI u = URI.create(uri);
      String path = u.getPath();
      if (path == null || Jdk8.isBlank(path)) {
        return null;
      }
      // A Windows URI is "/C:/dir/File.java"; the leading slash is part
      // of the drive, not of the path.
      if (path.length() > 2
          && path.charAt(0) == '/'
          && Character.isLetter(path.charAt(1))
          && path.charAt(2) == ':') {
        path = path.substring(1);
      }
      return path;
    } catch (IllegalArgumentException e) {
      return uri.startsWith("file:") ? uri.substring("file:".length()) : uri;
    }
  }

  /**
   * Call a no-argument method that returns a String, reflectively.
   *
   * <p>Reflective because the bootstrap class loader cannot see {@code javax.tools}, and because
   * the method may not exist on a given implementation.
   *
   * <p><b>The method is looked up on the interface, not on the implementation class.</b> The
   * implementation is {@code com.sun.tools.javac.file.PathFileObject$SimpleFileObject}, and the
   * {@code jdk.compiler} module does not export {@code com.sun.tools.javac.file} to unnamed
   * modules. Calling {@code getName()} on the implementation class therefore fails with {@code
   * IllegalAccessException}, even though the method is public. The interface {@code
   * javax.tools.JavaFileObject} <em>is</em> exported, so the call has to go through it.
   *
   * @return the value, or {@code null} when the call failed
   */
  private static String callString(Object target, String methodName) {
    try {
      java.lang.reflect.Method m = findPublicMethod(target, methodName);
      if (m == null) {
        return null;
      }
      Object value = m.invoke(target);
      return value == null ? null : String.valueOf(value);
    } catch (ReflectiveOperationException | RuntimeException e) {
      return null;
    }
  }

  /**
   * Find a method that this class is allowed to call.
   *
   * <p>The implementation class is not accessible from the bootstrap loader, so the method is
   * looked up on every interface the object implements, and on {@code Object}. An interface method
   * is found by name, which is enough here because {@code getName} and {@code toUri} are not
   * overloaded on {@code JavaFileObject}.
   */
  private static java.lang.reflect.Method findPublicMethod(Object target, String methodName) {
    for (Class<?> iface : target.getClass().getInterfaces()) {
      try {
        return iface.getMethod(methodName);
      } catch (NoSuchMethodException ignored) {
        // try the next interface
      }
    }
    // Some implementations do not implement the interface directly; walk
    // the superclasses too.
    for (Class<?> c = target.getClass().getSuperclass(); c != null; c = c.getSuperclass()) {
      for (Class<?> iface : c.getInterfaces()) {
        try {
          return iface.getMethod(methodName);
        } catch (NoSuchMethodException ignored) {
          // try the next interface
        }
      }
    }
    try {
      return target.getClass().getMethod(methodName);
    } catch (NoSuchMethodException e) {
      return null;
    }
  }

  /**
   * Write the capture files.
   *
   * <p>Never throws: a broken capture must not break the compile.
   */
  private static void write(List<CaptureEntry> entries) {
    String dir = outputDirectory();
    if (dir == null || Jdk8.isBlank(dir)) {
      // No destination configured: the capture stays in this JVM's memory.
      return;
    }
    try {
      Path out = Jdk8.path(dir);
      Files.createDirectories(out);
      for (CaptureEntry entry : entries) {
        Path file = out.resolve(filePrefix(entry) + System.nanoTime() + ".json");
        Jdk8.writeString(file, entry.toJson(), StandardCharsets.UTF_8);
      }
    } catch (IOException | RuntimeException e) {
      System.err.println("[ccap-capture] could not write capture: " + e.getMessage());
    }
  }

  /**
   * The prefix of a capture file name.
   *
   * <p>The compiler's name, so that a directory holding captures of several languages can be read
   * at a glance -- and so that the reader can pick the files of one language without parsing them.
   */
  private static String filePrefix(CaptureEntry entry) {
    String compiler = entry.compiler() == null ? "capture" : entry.compiler();
    return compiler + "-";
  }

  /**
   * Where the captures go.
   *
   * <p>The system property wins over the environment variable, because it can be set from inside a
   * running JVM and the environment cannot.
   */
  static String outputDirectory() {
    String prop = System.getProperty(PROP_OUTPUT_DIR);
    if (prop != null && !Jdk8.isBlank(prop)) {
      return prop;
    }
    return System.getenv(ENV_OUTPUT_DIR);
  }
}

