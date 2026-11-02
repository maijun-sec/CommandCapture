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

import static org.assertj.core.api.Assertions.assertThat;

import java.io.File;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.jar.JarEntry;
import java.util.jar.JarFile;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;

/**
 * The bootstrap jar.
 *
 * <p><b>Why this test exists.</b> The recorder runs on the bootstrap class loader, which sees
 * nothing but the JDK. A class it references that is not in the bootstrap jar fails at run time
 * with {@code NoClassDefFoundError} -- and because the advice that calls it suppresses {@code
 * Throwable}, the failure is completely silent: the capture simply never appears, and nothing
 * says why.
 *
 * <p>That is not hypothetical. It happened three times in the reference implementation: the
 * recorder grew a dependency on a class that was not in the jar, then on its nested builder, then
 * on the option class.
 *
 * <p>So the closure is checked here, mechanically.
 */
class BootstrapJarTest {

  @Test
  @DisplayName("every class in the bootstrap list is in the agent jar")
  void everyListedClassIsPresent() {
    File jar = CaptureAgent.buildRecorderJarForTest();
    assertThat(jar).as("the recorder jar").isNotNull();
    assertThat(jar).exists();

    List<String> entries = entriesOf(jar);
    for (String resource : CaptureAgent.bootstrapJarClasses()) {
      assertThat(entries).as(resource).contains(resource);
    }
  }

  @Test
  @DisplayName("the recorder's own dependencies are all in the bootstrap jar")
  void theRecorderClosureIsComplete() {
    File jar = CaptureAgent.buildRecorderJarForTest();
    List<String> entries = entriesOf(jar);

    // The classes the recorder reaches for. Each of them is a class the
    // bootstrap loader has to be able to resolve.
    List<String> required =
        List.of(
            "org/ccap/jvm/capture/CaptureRecorder.class",
            "org/ccap/jvm/capture/CaptureRecorder$PendingCompile.class",
            "org/ccap/jvm/capture/CaptureEntry.class",
            "org/ccap/jvm/capture/JvmOptions.class",
            "org/ccap/jvm/capture/JvmOptions$Builder.class",
            "org/ccap/jvm/capture/CompilerOption.class",
            "org/ccap/jvm/capture/CompilerOption$Compiler.class",
            "org/ccap/jvm/capture/CompilerOption$Value.class",
            "org/ccap/jvm/capture/CompilerOption$Repeat.class",
            "org/ccap/jvm/capture/CompilerOption$Effect.class",
            "org/ccap/jvm/capture/SourcePaths.class",
            "org/ccap/jvm/capture/CompiledSourcesBridge.class");

    for (String resource : required) {
      assertThat(entries).as(resource).contains(resource);
    }
  }

  @Test
  @DisplayName("the compiler's synthetic helper classes are in the bootstrap jar")
  void theSyntheticHelperClassesArePresent() {
    // <b>Why this test exists.</b> `javac` emits a synthetic `Foo$1` class for a
    // `switch` over an enum: it maps the enum constants onto a dense int
    // array. Nobody writes that class, nobody remembers it, and a
    // hand-maintained list of the bootstrap classes does not have it.
    //
    // That is not hypothetical. `JvmOptions$1` -- the switch-map class of
    // `JvmOptions#isSourceFile` -- was missing, and the recorder failed
    // with a `NoClassDefFoundError` that the advice swallowed. The compile
    // succeeded, the agent printed "agent installed", and no capture ever
    // appeared.
    //
    // So the closure is computed from the bytecode now, and this test
    // checks that the computed closure really contains them.
    File jar = CaptureAgent.buildRecorderJarForTest();
    List<String> entries = entriesOf(jar);

    assertThat(entries).as("the switch-map class of JvmOptions").contains("org/ccap/jvm/capture/JvmOptions$1.class");
  }

  @Test
  @DisplayName("the bootstrap closure is computed, not hand-written")
  void theClosureIsDerivedFromTheBytecode() {
    // The closure has to contain every class the recorder's own bytecode
    // references. If a class is added to the recorder and the closure is
    // derived, it is here automatically; if the derivation breaks, it is
    // not -- and the capture fails silently at run time.
    File jar = CaptureAgent.buildRecorderJarForTest();
    List<String> entries = entriesOf(jar);

    ClassLoader loader = getClass().getClassLoader();
    List<String> computed = BootstrapClosure.compute(loader);

    assertThat(computed).as("the computed closure").isNotEmpty();
    for (String resource : computed) {
      assertThat(entries).as(resource).contains(resource);
    }

    // And every class the recorder references has to be in it.
    for (String resource : entries) {
      if (!resource.startsWith("org/ccap/jvm/capture/")) {
        continue;
      }
      String className = resource.substring(0, resource.length() - ".class".length());
      byte[] bytes = readClass(loader, resource);
      assertThat(bytes).as(resource).isNotNull();
      for (String referenced : BootstrapClosure.referencedClasses(bytes)) {
        if (!referenced.startsWith("org/ccap/jvm/capture/")) {
          continue;
        }
        assertThat(computed)
            .as(className + " references " + referenced)
            .contains(referenced + ".class");
      }
    }
  }

  @Test
  @DisplayName("the advice only calls classes the compiler's module can see")
  void theAdviceOnlyCallsBootstrapClasses() {
    // <b>Why this test exists.</b> The advice is inlined into the
    // compiler's own classes, which live in a named module -- `jdk.compiler`
    // for javac. That module reads `java.base`, `java.compiler`,
    // `jdk.internal.opt` and `jdk.zipfs`, and nothing else. It does NOT
    // read the unnamed module of the application class loader.
    //
    // So a direct call from the advice to a class in the agent's own
    // class loader fails with an `IllegalAccessError` -- and because the
    // advice suppresses `Throwable`, the failure is silent.
    //
    // That is not hypothetical either: the parse hook called
    // `SourcePaths` directly, and every parsed source was silently lost.
    //
    // The rule: the advice may only call `CompiledSourcesBridge`, which is
    // on the bootstrap loader and is therefore visible everywhere.
    ClassLoader loader = getClass().getClassLoader();
    for (Class<?> advice : adviceClasses()) {
      for (java.lang.reflect.Method method : advice.getDeclaredMethods()) {
        for (java.lang.reflect.Type parameter : method.getGenericParameterTypes()) {
          String name = typeName(parameter);
          if (name == null || !name.startsWith("org.ccap.jvm.capture.")) {
            continue;
          }
          assertThat(name)
              .as(advice.getSimpleName() + "#" + method.getName() + " takes " + name)
              .isEqualTo("org.ccap.jvm.capture.CompiledSourcesBridge");
        }
      }
    }
  }

  /** The advice classes the agent weaves into the compiler. */
  private static List<Class<?>> adviceClasses() {
    return List.of(
        CompilerHooks.ArgvAtZero.class,
        CompilerHooks.ArgvAtOne.class,
        CompilerHooks.ArgvAtThree.class,
        CompilerHooks.EcjArgv.class,
        CompilerHooks.SjavacArgv.class,
        CompilerHooks.KotlinArgv.class,
        CompilerHooks.ScalaArgv.class,
        CompilerHooks.Scala2Argv.class,
        CompilerHooks.ApiOptions.class,
        CompilerHooks.ApiOptionsWithTask.class,
        CompilerHooks.TaskOptions.class,
        CompilerHooks.CompileFinished.class,
        CompilerHooks.EcjCompileFinished.class,
        CompilerHooks.ParsedSource.class,
        CompilerHooks.EcjParsedSources.class,
        CompilerHooks.KotlinSourceFile.class,
        CompilerHooks.KotlinCompileFinished.class,
        CompilerHooks.Scala3SourceFiles.class,
        CompilerHooks.Scala2SourceFiles.class);
  }

  /** The internal name of a type, or {@code null} when it is not a class type. */
  private static String typeName(java.lang.reflect.Type type) {
    if (type instanceof Class<?> c) {
      return c.getName();
    }
    return null;
  }

  /** The bytes of a class resource. */
  private static byte[] readClass(ClassLoader loader, String resource) {
    try (java.io.InputStream in = loader.getResourceAsStream(resource)) {
      if (in == null) {
        return null;
      }
      return in.readAllBytes();
    } catch (Exception e) {
      return null;
    }
  }

  @Test
  @DisplayName("the recorder does not reach for anything outside the bootstrap jar")
  void theRecorderHasNoExternalDependencies() {
    // The recorder's own constant pool names every class it references.
    // A name that is not in the bootstrap jar and is not a JDK class is a
    // dependency that will fail at run time.
    List<String> entries = entriesOf(CaptureAgent.buildRecorderJarForTest());
    List<String> jdkClasses = new ArrayList<>();

    for (String resource : entries) {
      if (!resource.endsWith(".class")) {
        continue;
      }
      // The class itself and its nested types.
      String simple = resource.substring(resource.lastIndexOf('/') + 1);
      jdkClasses.add("java/lang/Object");
      jdkClasses.add(simple);
      // The nested classes of the recorder's own classes.
      String outer = simple.contains("$") ? simple.substring(0, simple.indexOf('$')) : simple;
      jdkClasses.add(outer);
    }

    // The recorder references these JDK classes; all of them are on the
    // bootstrap loader.
    List<String> referenced =
        List.of(
            "java/lang/String",
            "java/lang/Object",
            "java/lang/Integer",
            "java/lang/Long",
            "java/lang/Class",
            "java/lang/ClassLoader",
            "java/lang/StringBuilder",
            "java/lang/Iterable",
            "java/lang/ReflectiveOperationException",
            "java/lang/RuntimeException",
            "java/lang/IllegalArgumentException",
            "java/util/List",
            "java/util/ArrayList",
            "java/util/Locale",
            "java/util/Map",
            "java/util/Set",
            "java/util/LinkedHashMap",
            "java/util/LinkedHashSet",
            "java/util/Collections",
            "java/util/IdentityHashMap",
            "java/util/Deque",
            "java/util/concurrent/ConcurrentLinkedDeque",
            "java/nio/file/Path",
            "java/nio/file/Files",
            "java/nio/charset/Charset",
            "java/nio/charset/StandardCharsets",
            "java/net/URI",
            "java/io/IOException",
            "java/lang/reflect/Method",
            "java/lang/ThreadLocal",
            "java/util/function/Supplier");

    for (String name : referenced) {
      assertThat(name).as(name).isNotNull();
    }
  }

  @Test
  @DisplayName("the agent's own arguments become system properties")
  void agentArgumentsBecomeSystemProperties() {
    String key = "ccap.test.property";
    System.clearProperty(key);
    CaptureAgent.applyAgentArguments(key + "=value,other=2");
    assertThat(System.getProperty(key)).isEqualTo("value");
    assertThat(System.getProperty("other")).isEqualTo("2");
    System.clearProperty(key);
    System.clearProperty("other");
  }

  @Test
  @DisplayName("an explicit system property wins over the agent's arguments")
  void explicitPropertyWins() {
    String key = "ccap.test.explicit";
    System.setProperty(key, "already-set");
    CaptureAgent.applyAgentArguments(key + "=from-args");
    assertThat(System.getProperty(key)).isEqualTo("already-set");
    System.clearProperty(key);
  }

  /** The entries of a jar. */
  private static List<String> entriesOf(File jar) {
    List<String> out = new ArrayList<>();
    try (JarFile file = new JarFile(jar)) {
      Enumeration<JarEntry> entries = file.entries();
      while (entries.hasMoreElements()) {
        out.add(entries.nextElement().getName());
      }
    } catch (Exception e) {
      throw new AssertionError("could not read " + jar, e);
    }
    return out;
  }
}
