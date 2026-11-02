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

import java.lang.reflect.Method;
import java.util.List;

/**
 * The bridge between the inlined advice and the recorder.
 *
 * <p><b>Why this class lives in the bootstrap jar.</b> ByteBuddy's {@code Advice} is
 * <em>inlined</em> into the target method, and the inlined code runs inside the compiler's own
 * module. A named module does <em>not</em> read the unnamed module, so the inlined code cannot
 * call a class in the agent's own class loader at all: the call fails with a linkage error, and
 * the advice's {@code suppress = Throwable} swallows it silently.
 *
 * <p>The boot layer's unnamed module <em>is</em> readable from the compiler's module, and the
 * bootstrap class loader is where the recorder lives. So the bridge is there too, and the inlined
 * code calls it.
 *
 * <p>And the bridge has to reach the recorder on the <em>same</em> loader: the agent's own copy of
 * the recorder is a different class, and it shares none of the recorder's state -- so a value
 * remembered by one is invisible to the other.
 */
public final class CompiledSourcesBridge {

  private static final String RECORDER = "org.ccap.jvm.capture.CaptureRecorder";
  private static final String ON_TASK = "onTask";
  private static final String REMEMBER = "rememberCompiledSources";
  private static final String REMEMBER_PARSED = "rememberParsedSource";
  private static final String ON_COMPILE_FINISHED = "onCompileFinished";
  private static final String ON_ARGV = "onArgv";
  private static final String ON_ARGV_COMPILER = "onArgv";
  private static final String ON_API_OPTIONS = "onApiOptions";
  private static final String REMEMBER_TASK = "rememberTask";
  private static final String ENTER_COMPILE_STEP = "enterCompileStep";
  private static final String EXIT_COMPILE_STEP = "exitCompileStep";
  private static final String SOURCE_PATHS = "org.ccap.jvm.capture.SourcePaths";

  private static volatile Method rememberMethod;
  private static volatile Method onTaskMethod;
  private static volatile Method rememberParsedMethod;
  private static volatile Method onCompileFinishedMethod;
  private static volatile Method onCompileFinishedSourcesMethod;
  private static volatile Method onArgvMethod;
  private static volatile Method onArgvCompilerMethod;
  private static volatile Method onApiOptionsMethod;
  private static volatile Method rememberTaskMethod;
  private static volatile Method enterCompileStepMethod;
  private static volatile Method exitCompileStepMethod;

  private CompiledSourcesBridge() {}

  /**
   * Record a source the compiler parsed.
   *
   * <p>This is the sources the compiler really compiled, which is a different set from the ones the
   * task was given: the compiler parses the sources it finds on the source path as well.
   *
   * @param path the path of the parsed source
   */
  public static void recordParsed(String path) {
    if (path == null || Jdk8.isBlank(path)) {
      return;
    }
    try {
      Method remember = rememberParsedMethod();
      if (remember != null) {
        remember.invoke(null, path);
      }
    } catch (Throwable e) {
      // A capture must never break the compile. The advice suppresses
      // Throwable anyway, so the failure is silent by design -- but the
      // capture still works: it reports the command-line sources.
      //
      // Throwable, not Exception: the failure this guards against is a
      // LinkageError, which is an Error and not an Exception.
    }
  }

  /**
   * The path of a file object the compiler handed over.
   *
   * <p><b>Why this is reflective rather than a direct call.</b> The advice runs inside the
   * compiler's own module. {@code jdk.compiler} reads {@code java.base}, {@code java.compiler},
   * {@code jdk.internal.opt} and {@code jdk.zipfs} -- and nothing else. It does <em>not</em> read
   * the unnamed module of the application class loader, so a direct call to a class there fails
   * with an {@code IllegalAccessError}.
   *
   * <p>But the boot layer's unnamed module -- where the bootstrap class loader's classes live -- is
   * read by every module. So a class on the bootstrap loader can be called directly, and it can
   * call anything else on the bootstrap loader. {@link SourcePaths} is on the bootstrap loader, so
   * the bridge can reach it; the advice reaches the bridge.
   *
   * @param file the file object
   * @return the path, or {@code null} when the object has none
   */
  public static String pathOf(Object file) {
    try {
      Class<?> sourcePaths = Class.forName(SOURCE_PATHS, false, bootstrapLoader());
      Method method = sourcePaths.getDeclaredMethod("pathOf", Object.class);
      method.setAccessible(true);
      Object value = method.invoke(null, file);
      return value == null ? null : String.valueOf(value);
    } catch (Throwable e) {
      reportLookupFailure(SOURCE_PATHS + "#pathOf", e);
      return null;
    }
  }

  /**
   * The paths of a collection of file objects.
   *
   * @param files the collection
   * @return the paths, empty when there are none
   */
  @SuppressWarnings("unchecked")
  public static List<String> pathsOf(Object files) {
    try {
      Class<?> sourcePaths = Class.forName(SOURCE_PATHS, false, bootstrapLoader());
      Method method = sourcePaths.getDeclaredMethod("pathsOf", Object.class);
      method.setAccessible(true);
      Object value = method.invoke(null, files);
      return value == null ? Jdk8.of() : (List<String>) value;
    } catch (Throwable e) {
      reportLookupFailure(SOURCE_PATHS + "#pathsOf", e);
      return Jdk8.of();
    }
  }

  /**
   * The strings of a collection.
   *
   * @param values the collection
   * @return the strings, empty when there are none
   */
  @SuppressWarnings("unchecked")
  public static List<String> stringsOf(Object values) {
    try {
      Class<?> sourcePaths = Class.forName(SOURCE_PATHS, false, bootstrapLoader());
      Method method = sourcePaths.getDeclaredMethod("stringsOf", Object.class);
      method.setAccessible(true);
      Object value = method.invoke(null, values);
      return value == null ? Jdk8.of() : (List<String>) value;
    } catch (Throwable e) {
      reportLookupFailure(SOURCE_PATHS + "#stringsOf", e);
      return Jdk8.of();
    }
  }

  /**
   * Record the compiler's argv.
   *
   * @param arguments the argv
   */
  public static void recordArgv(String[] arguments) {
    try {
      Method method = onArgvMethod();
      if (method != null) {
        method.invoke(null, (Object) arguments);
      }
    } catch (Throwable e) {
      // A capture must never break the compile -- but the failure is
      // reported, because a silent one leaves an incomplete capture.
      reportFailure("record the capture", e);
    }
  }

  /**
   * Record the compiler's argv, for a known compiler.
   *
   * @param arguments the argv
   * @param compiler the compiler's name, e.g. {@code javac}
   */
  public static void recordArgv(String[] arguments, String compiler) {
    try {
      Method method = onArgvCompilerMethod();
      Object value = compilerByName(compiler);
      if (method != null && value != null) {
        method.invoke(null, arguments, value);
      }
    } catch (Throwable e) {
      // A capture must never break the compile -- but the failure is
      // reported, because a silent one leaves an incomplete capture.
      reportFailure("record the capture", e);
    }
  }

  /**
   * Record the options and sources of a {@code javax.tools} API call.
   *
   * @param options the compiler options
   * @param sources the source files
   */
  public static void recordApiOptions(Iterable<?> options, Iterable<?> sources) {
    try {
      Method method = onApiOptionsMethod();
      if (method != null) {
        method.invoke(null, options, sources);
      }
    } catch (Throwable e) {
      // A capture must never break the compile -- but the failure is
      // reported, because a silent one leaves an incomplete capture.
      reportFailure("record the capture", e);
    }
  }

  /**
   * Remember a task's options and sources.
   *
   * @param task the task
   * @param options the options, as strings
   * @param sources the sources, as paths
   */
  public static void rememberTask(Object task, List<String> options, List<String> sources) {
    try {
      Method method = rememberTaskMethod();
      if (method != null) {
        method.invoke(null, task, options, sources);
      }
    } catch (Throwable e) {
      // A capture must never break the compile -- but the failure is
      // reported, because a silent one leaves an incomplete capture.
      reportFailure("record the capture", e);
    }
  }

  /**
   * A compile step has begun.
   *
   * @return true when this is the outermost compile step
   */
  public static boolean enterCompileStep() {
    try {
      Method method = enterCompileStepMethod();
      if (method != null) {
        return Boolean.TRUE.equals(method.invoke(null));
      }
    } catch (Throwable e) {
      // A capture must never break the compile -- but the failure is
      // reported, because a silent one leaves an incomplete capture.
      reportFailure("record the capture", e);
    }
    return false;
  }

  /**
   * A compile step has ended.
   *
   * @return true when this was the outermost compile step
   */
  public static boolean exitCompileStep() {
    try {
      Method method = exitCompileStepMethod();
      if (method != null) {
        return Boolean.TRUE.equals(method.invoke(null));
      }
    } catch (Throwable e) {
      // A capture must never break the compile -- but the failure is
      // reported, because a silent one leaves an incomplete capture.
      reportFailure("record the capture", e);
    }
    return false;
  }

  /**
   * Record a compile that has finished.
   *
   * @param compiler the compiler's name, e.g. {@code javac}
   */
  public static void recordCompileFinished(String compiler) {
    try {
      Method method = onCompileFinishedMethod();
      Object value = compilerByName(compiler);
      if (method != null && value != null) {
        method.invoke(null, value);
      }
    } catch (Throwable e) {
      // A capture must never break the compile -- but the failure is
      // reported, because a silent one leaves an incomplete capture.
      reportFailure("record the capture", e);
    }
  }

  /**
   * Call a no-argument getter on an object, reflectively.
   *
   * <p>The object belongs to the compiler's own class loader, so the method is looked up by name.
   * The interfaces are tried first, because the concrete class is often in a package that is not
   * exported -- calling a public method on an inaccessible class fails with {@code
   * IllegalAccessException} even though the method is public.
   *
   * @param target the object, may be null
   * @param methodName the getter's name
   * @return the value, or {@code null} when the call failed
   */
  public static Object callGetter(Object target, String methodName) {
    if (target == null) {
      return null;
    }
    for (Class<?> iface : target.getClass().getInterfaces()) {
      Object value = invoke(iface, target, methodName);
      if (value != null) {
        return value;
      }
    }
    for (Class<?> c = target.getClass().getSuperclass(); c != null; c = c.getSuperclass()) {
      for (Class<?> iface : c.getInterfaces()) {
        Object value = invoke(iface, target, methodName);
        if (value != null) {
          return value;
        }
      }
    }
    return invoke(target.getClass(), target, methodName);
  }

  /** Invoke a method looked up on a specific class, swallowing every failure. */
  private static Object invoke(Class<?> owner, Object target, String methodName) {
    try {
      java.lang.reflect.Method m = owner.getMethod(methodName);
      return m.invoke(target);
    } catch (Throwable e) {
      return null;
    }
  }

  /**
   * The strings of an options iterable.
   *
   * @param options the options
   * @return the options as strings
   */
  public static List<String> toStrings(Iterable<?> options) {
    try {
      Method method = recorderClass().getMethod("toStrings", Iterable.class);
      Object value = method.invoke(null, options);
      return value == null ? Jdk8.of() : castList(value);
    } catch (Throwable e) {
      reportLookupFailure(RECORDER + "#toStrings", e);
      return Jdk8.of();
    }
  }

  /**
   * The paths of a sources iterable.
   *
   * @param sources the sources
   * @return the source paths
   */
  public static List<String> toPaths(Iterable<?> sources) {
    try {
      Method method = recorderClass().getMethod("toPaths", Iterable.class);
      Object value = method.invoke(null, sources);
      return value == null ? Jdk8.of() : castList(value);
    } catch (Throwable e) {
      reportLookupFailure(RECORDER + "#toPaths", e);
      return Jdk8.of();
    }
  }

  /**
   * Record the task's compiled sources, and write the capture.
   *
   * <p>Both happen here, in this order: the compiled sources are only known once the compile has
   * run, so they are read first, and the capture that reports them is written after that.
   *
   * @param task the task that was driven
   * @param sources the source files the compiler compiled
   */
  public static void record(Object task, List<String> sources) {
    try {
      Method remember = rememberMethod();
      if (remember != null) {
        remember.invoke(null, task, sources == null ? Jdk8.of() : sources);
      }
      Method onTask = onTaskMethod();
      if (onTask != null) {
        onTask.invoke(null, task);
      }
    } catch (Throwable e) {
      // A capture must never break the compile -- but the failure is
      // reported, because a silent one leaves an incomplete capture.
      reportFailure("record the capture", e);
    }
  }

  /**
   * Write the capture for a compiler that has no separate "compile end" hook.
   *
   * <p><b>Why Kotlin and Scala need this.</b> Their compilers do not have a single method that
   * means "the compile is over" in the way javac's {@code JavaCompiler#compile} does. What they
   * have instead is a method that says "here are the files I compiled" -- and that is the same
   * information the capture needs, so the capture is written right there.
   *
   * @param compilerName the compiler's name, e.g. {@code kotlin}
   * @param sources the sources the compiler compiled
   */
  public static void recordCompileFinished(String compilerName, List<String> sources) {
    try {
      Method method = onCompileFinishedSourcesMethod();
      if (method == null) {
        return;
      }
      // The recorder's overload takes the compiler enum; the bridge cannot
      // name it (the enum class lives in the bootstrap jar, but the bridge
      // has to reach the recorder reflectively, so the enum value is
      // resolved by name too).
      Object compiler = compilerByName(compilerName);
      if (compiler == null) {
        return;
      }
      method.invoke(null, compiler, sources == null ? Jdk8.of() : sources);
    } catch (Throwable e) {
      // A capture must never break the compile -- but the failure is
      // reported, because a silent one leaves an incomplete capture.
      reportFailure("record the capture", e);
    }
  }

  /**
   * The compiler enum constant with that name.
   *
   * <p>Resolved by name because the bridge cannot reference the enum class directly: the enum is
   * in the bootstrap jar, and the bridge reaches it reflectively so that it works on the same
   * loader as the recorder.
   *
   * <p><b>Why the comparison ignores case.</b> The enum constants are named in upper case ({@code
   * JAVAC}), because that is the Java convention, while the name a capture is recorded under is
   * lower case ({@code javac}), because that is what the capture file and the consumer's compiler
   * table use. The bridge is the boundary between the two, so it matches on either.
   */
  private static Object compilerByName(String name) {
    try {
      Class<?> enumClass =
          Class.forName("org.ccap.jvm.capture.CompilerOption$Compiler", false, bootstrapLoader());
      for (Object constant : enumClass.getEnumConstants()) {
        if (String.valueOf(constant).equalsIgnoreCase(name)) {
          return constant;
        }
      }
      reportLookupFailure("the compiler " + name, new IllegalArgumentException("no such compiler"));
      return null;
    } catch (Throwable e) {
      reportLookupFailure("CompilerOption$Compiler", e);
      return null;
    }
  }

  /** The recorder's method, looked up once on the bootstrap loader. */
  private static Method rememberMethod() {
    Method cached = rememberMethod;
    if (cached != null) {
      return cached;
    }
    try {
      Method method = recorderClass().getMethod(REMEMBER, Object.class, List.class);
      rememberMethod = method;
      return method;
    } catch (Throwable e) {
      reportLookupFailure(REMEMBER, e);
      return null;
    }
  }

  /** The recorder's method, looked up once on the bootstrap loader. */
  private static Method onTaskMethod() {
    Method cached = onTaskMethod;
    if (cached != null) {
      return cached;
    }
    try {
      Method method = recorderClass().getMethod(ON_TASK, Object.class);
      onTaskMethod = method;
      return method;
    } catch (Throwable e) {
      reportLookupFailure(ON_TASK, e);
      return null;
    }
  }

  /** The recorder's method, looked up once on the bootstrap loader. */
  private static Method rememberParsedMethod() {
    Method cached = rememberParsedMethod;
    if (cached != null) {
      return cached;
    }
    try {
      Method method = recorderClass().getMethod(REMEMBER_PARSED, String.class);
      rememberParsedMethod = method;
      return method;
    } catch (Throwable e) {
      reportLookupFailure(REMEMBER_PARSED, e);
      return null;
    }
  }

  /**
   * The recorder's {@code onCompileFinished(Compiler)}, looked up once.
   *
   * <p><b>Why the signature is spelled out.</b> The recorder has two overloads: one that takes
   * just the compiler and one that takes the compiler and the compiled sources. The one-argument
   * lookup has to say {@code (Compiler)} exactly, or the reflection finds the other overload and
   * the call fails with {@code IllegalArgumentException} at run time.
   */
  private static Method onCompileFinishedMethod() {
    Method cached = onCompileFinishedMethod;
    if (cached != null) {
      return cached;
    }
    try {
      Class<?> compilerEnum =
          Class.forName("org.ccap.jvm.capture.CompilerOption$Compiler", false, bootstrapLoader());
      Method method = recorderClass().getMethod(ON_COMPILE_FINISHED, compilerEnum);
      onCompileFinishedMethod = method;
      return method;
    } catch (Throwable e) {
      reportLookupFailure(ON_COMPILE_FINISHED, e);
      return null;
    }
  }

  /** The recorder's {@code onCompileFinished(Compiler, List)}, looked up once. */
  private static Method onCompileFinishedSourcesMethod() {
    Method cached = onCompileFinishedSourcesMethod;
    if (cached != null) {
      return cached;
    }
    try {
      Class<?> compilerEnum =
          Class.forName("org.ccap.jvm.capture.CompilerOption$Compiler", false, bootstrapLoader());
      Method method = recorderClass().getMethod(ON_COMPILE_FINISHED, compilerEnum, List.class);
      onCompileFinishedSourcesMethod = method;
      return method;
    } catch (Throwable e) {
      reportLookupFailure(ON_COMPILE_FINISHED, e);
      return null;
    }
  }

  /** The recorder's {@code onArgv(String[])}, looked up once on the bootstrap loader. */  private static Method onArgvMethod() {
    Method cached = onArgvMethod;
    if (cached != null) {
      return cached;
    }
    try {
      Method method = recorderClass().getMethod(ON_ARGV, String[].class);
      onArgvMethod = method;
      return method;
    } catch (Throwable e) {
      reportLookupFailure(ON_ARGV, e);
      return null;
    }
  }

  /** The recorder's {@code onArgv(String[], Compiler)}, looked up once. */
  private static Method onArgvCompilerMethod() {
    Method cached = onArgvCompilerMethod;
    if (cached != null) {
      return cached;
    }
    try {
      Class<?> compilerEnum =
          Class.forName("org.ccap.jvm.capture.CompilerOption$Compiler", false, bootstrapLoader());
      Method method = recorderClass().getMethod(ON_ARGV_COMPILER, String[].class, compilerEnum);
      onArgvCompilerMethod = method;
      return method;
    } catch (Throwable e) {
      reportLookupFailure(ON_ARGV_COMPILER, e);
      return null;
    }
  }

  /** The recorder's {@code onApiOptions}, looked up once. */
  private static Method onApiOptionsMethod() {
    Method cached = onApiOptionsMethod;
    if (cached != null) {
      return cached;
    }
    try {
      Method method = recorderClass().getMethod(ON_API_OPTIONS, Iterable.class, Iterable.class);
      onApiOptionsMethod = method;
      return method;
    } catch (Throwable e) {
      reportLookupFailure(ON_API_OPTIONS, e);
      return null;
    }
  }

  /** The recorder's {@code rememberTask}, looked up once. */
  private static Method rememberTaskMethod() {
    Method cached = rememberTaskMethod;
    if (cached != null) {
      return cached;
    }
    try {
      Method method =
          recorderClass().getMethod(REMEMBER_TASK, Object.class, List.class, List.class);
      rememberTaskMethod = method;
      return method;
    } catch (Throwable e) {
      reportLookupFailure(REMEMBER_TASK, e);
      return null;
    }
  }

  /** The recorder's {@code enterCompileStep}, looked up once. */
  private static Method enterCompileStepMethod() {
    Method cached = enterCompileStepMethod;
    if (cached != null) {
      return cached;
    }
    try {
      Method method = recorderClass().getMethod(ENTER_COMPILE_STEP);
      enterCompileStepMethod = method;
      return method;
    } catch (Throwable e) {
      reportLookupFailure(ENTER_COMPILE_STEP, e);
      return null;
    }
  }

  /** The recorder's {@code exitCompileStep}, looked up once. */
  private static Method exitCompileStepMethod() {
    Method cached = exitCompileStepMethod;
    if (cached != null) {
      return cached;
    }
    try {
      Method method = recorderClass().getMethod(EXIT_COMPILE_STEP);
      exitCompileStepMethod = method;
      return method;
    } catch (Throwable e) {
      reportLookupFailure(EXIT_COMPILE_STEP, e);
      return null;
    }
  }

  /** Narrow a value to a list of strings. */
  @SuppressWarnings("unchecked")
  private static List<String> castList(Object value) {
    return (List<String>) value;
  }

  /**
   * Report a failure that would otherwise be invisible.
   *
   * <p><b>Why the failures are reported.</b> Every method on this bridge swallows its failures,
   * because a capture must never break a compile. But a swallowed failure means the capture is
   * silently incomplete -- the compile succeeds, and the capture that appears is missing exactly
   * the part that failed. That is the failure nobody notices.
   *
   * <p>So the failure is reported on the compiler's own stderr, once per kind: the lookups are
   * cached, so a failing one does not repeat.
   *
   * @param what what failed
   * @param cause the cause
   */
  private static void reportFailure(String what, Throwable cause) {
    System.err.println("[ccap-capture] could not " + what + ": " + cause);
  }

  /**
   * Report a lookup that failed, once.
   *
   * <p><b>Why this is reported at all.</b> A lookup that fails leaves the capture silently
   * incomplete: the compile still runs, the capture is still written, and it is just missing what
   * the failed lookup would have supplied. That is exactly the failure nobody notices.
   *
   * <p>So the failure is reported on the compiler's own stderr, once -- the lookups are cached, so
   * this runs at most once per method.
   */
  private static void reportLookupFailure(String method, Throwable cause) {
    System.err.println(
        "[ccap-capture] could not find "
            + RECORDER
            + "#"
            + method
            + " on the bootstrap loader: "
            + cause
            + " -- the capture will be incomplete");
  }

  /**
   * The recorder, as the instrumented code sees it.
   *
   * <p>The bootstrap loader holds the copy the instrumented code uses, so the state the bridge
   * touches is the state that matters.
   */
  private static Class<?> recorderClass() throws ClassNotFoundException {
    return Class.forName(RECORDER, false, bootstrapLoader());
  }

  /** The bootstrap class loader. */
  private static ClassLoader bootstrapLoader() {
    // The platform loader's parent is the bootstrap loader on Java 9 and
    // newer. On Java 8 the extension loader is the platform loader's
    // ancestor, and the bootstrap loader is its parent -- so the helper
    // handles both.
    return Jdk8.platformClassLoader().getParent();
  }
}

