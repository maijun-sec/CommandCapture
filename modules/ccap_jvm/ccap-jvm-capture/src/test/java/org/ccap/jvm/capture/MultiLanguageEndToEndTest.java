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

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.EnabledIf;
import org.junit.jupiter.api.io.TempDir;

/**
 * The end-to-end test for every language the agent supports.
 *
 * <p><b>Why this test runs the real compilers.</b> The capture hooks methods that have to exist,
 * with argument shapes that have to be right. A unit test can check the option table and the
 * entry point table, but only a real compile proves that the advice actually fires -- and a
 * mismatch between the advice and the target is *silent*: the transform succeeds, the method is
 * simply never called.
 *
 * <p>That is not hypothetical. It happened for every one of the languages:
 *
 * <ul>
 *   <li>javac: the {@code JavacTool#run} advice was bound to argument 0, which is an {@code
 *       InputStream}, not the argv.
 *   <li>Kotlin: the {@code exec} overloads were matched by arity alone, so the advice was applied
 *       to an overload whose third argument is the arguments object rather than the argv.
 *   <li>Kotlin: the "compile end" was hooked on a method the phased compiler never calls, so the
 *       advice never fired.
 *   <li>Scala 3: {@code Main#main} is a forwarder to {@code Driver#main}, and the compile step is
 *       {@code Driver#doCompile}.
 * </ul>
 *
 * <p>Every one of those produced a compile that succeeded and no capture at all.
 */
class MultiLanguageEndToEndTest {

  /** Whether a JDK is on the path -- the test needs {@code java}. */
  static boolean hasJava() {
    try {
      Path javaHome = Path.of(System.getProperty("java.home"));
      Path java = javaHome.resolve("bin").resolve(isWindows() ? "java.exe" : "java");
      return Files.isExecutable(java);
    } catch (RuntimeException e) {
      return false;
    }
  }

  private static boolean isWindows() {
    return System.getProperty("os.name", "").toLowerCase().contains("win");
  }

  /**
   * Whether the Kotlin test can run here: a JDK is on the path, and the Kotlin compiler on the
   * classpath understands this JDK.
   *
   * <p>The two conditions are combined rather than stacked, because {@code @EnabledIf} is not a
   * repeatable annotation in this JUnit version.
   */
  static boolean hasJavaAndKotlinRuns() {
    return hasJava() && kotlinRunsOnThisJdk();
  }

  // ── ECJ ────────────────────────────────────────────────────────────────

  @Test
  @EnabledIf("hasJava")
  @DisplayName("a real ECJ compile leaves a capture")
  void ecjCompileIsCaptured(@TempDir Path dir) throws Exception {
    Path source = dir.resolve("Hello.java");
    Files.writeString(source, "public class Hello { }");
    Path out = dir.resolve("out");
    Files.createDirectories(out);
    Path captures = dir.resolve("captures");
    Files.createDirectories(captures);

    List<CaptureEntry> entries =
        runCompiler(
            dir,
            captures,
            "org.eclipse.jdt.internal.compiler.batch.Main",
            "-d",
            out.toString(),
            source.toString());

    assertThat(entries).as("captures in " + captures).hasSize(1);
    CaptureEntry entry = entries.get(0);
    assertThat(entry.compiler()).isEqualTo("ecj");
    assertThat(entry.language()).isEqualTo("java");
    assertThat(entry.sourceFiles()).hasSize(1);
    assertThat(entry.sourceFiles().get(0).toString()).endsWith("Hello.java");
    assertThat(entry.outputDirectory()).isNotNull();
    // The class file really was produced.
    assertThat(Files.exists(out.resolve("Hello.class"))).isTrue();
  }

  // ── Kotlin ─────────────────────────────────────────────────────────────

  /**
   * Whether the Kotlin compiler on the classpath can run on the JVM that is running this test.
   *
   * <p>The Kotlin compiler parses {@code java.version} with its own relocated copy of IntelliJ's
   * {@code JavaVersion}, and on a JDK it does not know that parse throws. The compiler then dies
   * before it reads a source file, which has nothing to do with the capture. See
   * {@link CompilerJdkSupport}.
   */
  static boolean kotlinRunsOnThisJdk() {
    return CompilerJdkSupport.compilerUnderstandsThisJdk();
  }

  @Test
  @EnabledIf("hasJavaAndKotlinRuns")
  @DisplayName("a real Kotlin compile leaves a capture")
  void kotlinCompileIsCaptured(@TempDir Path dir) throws Exception {
    Path source = dir.resolve("Main.kt");
    Files.writeString(source, "fun main() { println(\"hi\") }");
    Path out = dir.resolve("out");
    Files.createDirectories(out);
    Path captures = dir.resolve("captures");
    Files.createDirectories(captures);

    List<CaptureEntry> entries =
        runCompiler(
            dir,
            captures,
            "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
            "-no-stdlib",
            "-classpath",
            kotlinStdlib(),
            "-d",
            out.toString(),
            source.toString());

    assertThat(entries).as("captures in " + captures).hasSize(1);
    CaptureEntry entry = entries.get(0);
    assertThat(entry.compiler()).isEqualTo("kotlin");
    assertThat(entry.language()).isEqualTo("kotlin");
    assertThat(entry.sourceFiles()).hasSize(1);
    assertThat(entry.sourceFiles().get(0).toString()).endsWith("Main.kt");
    // The class file really was produced.
    assertThat(Files.exists(out.resolve("MainKt.class"))).isTrue();
  }

  // ── Scala 3 ────────────────────────────────────────────────────────────

  @Test
  @EnabledIf("hasJava")
  @DisplayName("a real Scala 3 compile leaves a capture")
  void scala3CompileIsCaptured(@TempDir Path dir) throws Exception {
    Path source = dir.resolve("Hello.scala");
    Files.writeString(
        source, "object Hello { def main(a: Array[String]): Unit = println(\"hi\") }");
    Path out = dir.resolve("out");
    Files.createDirectories(out);
    Path captures = dir.resolve("captures");
    Files.createDirectories(captures);

    List<CaptureEntry> entries =
        runCompiler(
            dir,
            captures,
            "dotty.tools.dotc.Main",
            // The Scala 3 compiler needs its own library on the *compile*
            // classpath -- without it, it cannot find the `scala` package
            // and refuses to compile anything.
            "-classpath",
            scalaLibrary(),
            "-d",
            out.toString(),
            source.toString());

    assertThat(entries).as("captures in " + captures).hasSize(1);
    CaptureEntry entry = entries.get(0);
    assertThat(entry.compiler()).isEqualTo("scala3");
    assertThat(entry.language()).isEqualTo("scala");
    assertThat(entry.sourceFiles()).hasSize(1);
    assertThat(entry.sourceFiles().get(0).toString()).endsWith("Hello.scala");
    // The class file really was produced.
    assertThat(Files.exists(out.resolve("Hello.class"))
            || Files.exists(out.resolve("Hello$.class")))
        .isTrue();
  }

  // ── The harness ────────────────────────────────────────────────────────

  /**
   * Run a compiler with the agent attached, and read the captures it left.
   *
   * <p>The compiler is run in its own JVM, with the agent attached, because that is the only way
   * the agent can instrument it.
   */
  private static List<CaptureEntry> runCompiler(
      Path workDir, Path captures, String mainClass, String... arguments) throws Exception {
    Path java = Path.of(System.getProperty("java.home")).resolve("bin")
        .resolve(isWindows() ? "java.exe" : "java");

    List<String> command = new ArrayList<>();
    command.add(java.toString());
    command.add("-javaagent:" + agentJar());
    command.add("-D" + CaptureRecorder.PROP_OUTPUT_DIR + "=" + captures);
    command.add("-cp");
    command.add(compilerClasspath());
    command.add(mainClass);
    for (String a : arguments) {
      command.add(a);
    }

    ProcessBuilder builder = new ProcessBuilder(command);
    builder.redirectErrorStream(true);
    builder.directory(workDir.toFile());
    Process process = builder.start();
    String output = new String(process.getInputStream().readAllBytes());
    assertThat(process.waitFor(180, TimeUnit.SECONDS)).as("the compiler finished").isTrue();
    assertThat(process.exitValue()).as("compiler output: " + output).isZero();

    return CaptureReader.readAll(captures);
  }

  /** The classpath the compiler runs on: the agent's classes plus the compilers. */
  private static String compilerClasspath() {
    return String.join(java.io.File.pathSeparator, System.getProperty("java.class.path").split(
        java.io.File.pathSeparator));
  }

  /** The Kotlin standard library's path, which the Kotlin compile needs. */
  private static String kotlinStdlib() {
    return libraryJar("kotlin-stdlib-");
  }

  /** The Scala 3 library's path, which the Scala compile needs on its own classpath. */
  private static String scalaLibrary() {
    // The Scala 3 compiler needs the whole library set on its *compile*
    // classpath -- the library, the interfaces, the TASTy core and the
    // standard library -- not just the one jar. Without them it cannot
    // find the `scala` package and refuses to compile anything.
    List<String> jars = new ArrayList<>();
    for (String prefix :
        List.of(
            "scala3-library_3-",
            "scala3-interfaces-",
            "scala-library-",
            "tasty-core_3-")) {
      for (String entry : System.getProperty("java.class.path").split(java.io.File.pathSeparator)) {
        Path p = Path.of(entry);
        String name = p.getFileName() == null ? "" : p.getFileName().toString();
        if (name.startsWith(prefix) && name.endsWith(".jar")) {
          jars.add(p.toString());
          break;
        }
      }
    }
    if (jars.isEmpty()) {
      throw new org.opentest4j.TestAbortedException("no Scala library on the classpath");
    }
    return String.join(java.io.File.pathSeparator, jars);
  }

  /**
   * The path of a jar on the test's classpath, found by its file name prefix.
   *
   * @param prefix the file name prefix
   * @return the path
   */
  private static String libraryJar(String prefix) {
    for (String entry : System.getProperty("java.class.path").split(java.io.File.pathSeparator)) {
      Path p = Path.of(entry);
      String name = p.getFileName() == null ? "" : p.getFileName().toString();
      if (name.startsWith(prefix) && name.endsWith(".jar")) {
        return p.toString();
      }
    }
    throw new org.opentest4j.TestAbortedException("no jar starting with " + prefix + " on the classpath");
  }

  /**
   * The agent jar.
   *
   * <p>The test builds it itself, from {@code target/classes} plus ByteBuddy. That keeps the test
   * honest: it proves that the classes that are shipped plus ByteBuddy really are enough to
   * capture a compile.
   */
  private static Path agentJar() throws Exception {
    Path testClasses =
        Path.of(
            CaptureEndToEndTest.class.getProtectionDomain().getCodeSource().getLocation().toURI());
    Path module = testClasses.getParent().getParent();
    Path target = module.resolve("target");
    Path jar = target.resolve("ccap-jvm-capture-multi-agent-test.jar");

    Path staging = target.resolve("multi-agent-staging");
    deleteRecursively(staging);
    Files.createDirectories(staging);

    Path classesDir = target.resolve("classes");
    if (!Files.isDirectory(classesDir)) {
      throw new org.opentest4j.TestAbortedException("the agent classes are not built: " + classesDir);
    }
    copyTree(classesDir, staging);
    unzipInto(findByteBuddy(), staging);

    Path manifest = target.resolve("multi-agent-manifest.mf");
    Files.writeString(
        manifest,
        "Manifest-Version: 1.0\r\n"
            + "Premain-Class: org.ccap.jvm.capture.CaptureAgent\r\n"
            + "Agent-Class: org.ccap.jvm.capture.CaptureAgent\r\n"
            + "Can-Redefine-Classes: true\r\n"
            + "Can-Retransform-Classes: true\r\n"
            + "\r\n");

    Path jarTool =
        Path.of(System.getProperty("java.home")).resolve("bin").resolve(isWindows() ? "jar.exe" : "jar");
    ProcessBuilder builder =
        new ProcessBuilder(
            jarTool.toString(),
            "--create",
            "--file",
            jar.toString(),
            "--manifest",
            manifest.toString(),
            "-C",
            staging.toString(),
            ".");
    builder.redirectErrorStream(true);
    Process process = builder.start();
    String output = new String(process.getInputStream().readAllBytes());
    assertThat(process.waitFor(120, TimeUnit.SECONDS)).as("jar: " + output).isTrue();
    assertThat(process.exitValue()).as("jar: " + output).isZero();

    return jar;
  }

  /** The ByteBuddy jar, from the test's own classpath. */
  private static Path findByteBuddy() {
    for (String entry : System.getProperty("java.class.path").split(java.io.File.pathSeparator)) {
      Path p = Path.of(entry);
      String name = p.getFileName() == null ? "" : p.getFileName().toString();
      if (name.startsWith("byte-buddy-") && name.endsWith(".jar")) {
        return p;
      }
    }
    throw new org.opentest4j.TestAbortedException("ByteBuddy is not on the test classpath");
  }

  /** Copy a directory tree. */
  private static void copyTree(Path from, Path to) throws Exception {
    try (java.util.stream.Stream<Path> stream = Files.walk(from)) {
      for (Path path : stream.toList()) {
        Path target = to.resolve(from.relativize(path).toString());
        if (Files.isDirectory(path)) {
          Files.createDirectories(target);
        } else {
          Files.createDirectories(target.getParent());
          Files.copy(path, target);
        }
      }
    }
  }

  /** Unpack a jar into a directory. */
  private static void unzipInto(Path jar, Path to) throws Exception {
    try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(jar.toFile())) {
      java.util.Enumeration<? extends java.util.zip.ZipEntry> entries = zip.entries();
      while (entries.hasMoreElements()) {
        java.util.zip.ZipEntry entry = entries.nextElement();
        Path out = to.resolve(entry.getName());
        if (entry.isDirectory()) {
          Files.createDirectories(out);
          continue;
        }
        Files.createDirectories(out.getParent());
        Files.copy(zip.getInputStream(entry), out);
      }
    }
  }

  /** Delete a directory tree, ignoring what is not there. */
  private static void deleteRecursively(Path dir) throws Exception {
    if (!Files.exists(dir)) {
      return;
    }
    try (java.util.stream.Stream<Path> stream = Files.walk(dir)) {
      for (Path path : stream.sorted(java.util.Comparator.reverseOrder()).toList()) {
        Files.deleteIfExists(path);
      }
    }
  }
}
