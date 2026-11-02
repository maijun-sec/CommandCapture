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
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.EnabledIf;
import org.junit.jupiter.api.io.TempDir;

/**
 * The agent on a real Java 8 JVM.
 *
 * <p><b>Why this test exists.</b> The agent is compiled for Java 8, because a large share of the
 * JVM installations still run their builds on Java 8 -- and a Java 11 agent cannot even load there.
 * But "compiled for Java 8" is a claim about the class file version, not about the behaviour: the
 * agent could still load and still fail to hook, and the symptom would be the familiar one -- the
 * compile succeeds, no capture appears, nothing says why.
 *
 * <p>So this test runs a <em>real</em> compile on a <em>real</em> Java 8 JVM. The JDK is found
 * through the {@code CCAP_TEST_JDK8} environment variable; when it is not set the test is skipped
 * rather than failed, because the JDK is not part of the repository.
 *
 * <p><b>Why the JDK is not part of the repository.</b> A JDK is 100 MB of binaries, and the point
 * of the test is that the agent works on <em>a</em> Java 8 -- not on one particular distribution.
 * The gate that always runs is {@link AgentJdkCompatibilityTest}, which checks the class file
 * version and the API references of the shipped bytecode.
 */
class Jdk8EndToEndTest {

  /** The environment variable naming the Java 8 home to test against. */
  private static final String JDK8_HOME = "CCAP_TEST_JDK8";

  /** Whether a Java 8 JDK was provided. */
  static boolean hasJdk8() {
    String home = System.getenv(JDK8_HOME);
    if (home == null || home.isBlank()) {
      return false;
    }
    File dir = new File(home);
    if (!dir.isDirectory()) {
      return false;
    }
    String exe = isWindows() ? "java.exe" : "java";
    return new File(dir, "bin" + File.separator + exe).isFile();
  }

  private static boolean isWindows() {
    return System.getProperty("os.name", "").toLowerCase().contains("win");
  }

  /** The Java 8 home. */
  private static Path jdk8Home() {
    return Path.of(System.getenv(JDK8_HOME));
  }

  /** The Java executable of the Java 8 JDK. */
  private static Path jdk8Java() {
    return jdk8Home()
        .resolve("bin")
        .resolve(isWindows() ? "java.exe" : "java");
  }

  /**
   * The classpath the compiler runs on: the test's own classpath, plus the Java 8 JDK's
   * {@code tools.jar}.
   *
   * <p>The {@code tools.jar} is the reason this method exists. On Java 8 the compiler classes are
   * not on the runtime classpath: {@code com.sun.tools.javac.Main} lives in {@code lib/tools.jar},
   * and the {@code javac} launcher adds it explicitly. A plain {@code java -cp ...} invocation
   * therefore fails with {@code ClassNotFoundException: com.sun.tools.javac.Main} unless the jar is
   * on the classpath.
   *
   * <p>On Java 9 and later the compiler is a module ({@code jdk.compiler}) and needs nothing of the
   * sort -- which is exactly why this only shows up in the Java 8 test.
   *
   * @return the classpath
   */
  private static String jdk8CompilerClasspath() {
    Path tools = jdk8Home().resolve("lib").resolve("tools.jar");
    if (!Files.isRegularFile(tools)) {
      throw new org.opentest4j.TestAbortedException(
          "no tools.jar in the Java 8 home: " + jdk8Home());
    }
    return System.getProperty("java.class.path")
        + File.pathSeparator
        + tools;
  }

  @Test
  @EnabledIf("hasJdk8")
  @DisplayName("the agent loads on Java 8 and hooks the compiler")
  void theAgentLoadsOnJdk8(@TempDir Path dir) throws Exception {
    Path captures = dir.resolve("captures");
    Files.createDirectories(captures);

    ProcessBuilder builder =
        new ProcessBuilder(
            jdk8Java().toString(),
            "-javaagent:" + agentJar(),
            "-D" + CaptureRecorder.PROP_OUTPUT_DIR + "=" + captures,
            "-version");
    builder.redirectErrorStream(true);
    Process process = builder.start();
    String output = new String(process.getInputStream().readAllBytes());

    assertThat(process.waitFor()).as("the output was:\n" + output).isZero();

    // The agent has to announce itself. If it did not, it did not load --
    // and on Java 8 a class file version that the JVM rejects is exactly
    // how that happens.
    assertThat(output)
        .as("the agent's output on Java 8:\n" + output)
        .contains("[ccap-capture] agent installed");

    // And it must not have failed to resolve the recorder.
    assertThat(output).as("the agent's output on Java 8:\n" + output).doesNotContain("NoClassDefFoundError");
    assertThat(output).as("the agent's output on Java 8:\n" + output).doesNotContain("UnsupportedClassVersionError");
  }

  @Test
  @EnabledIf("hasJdk8")
  @DisplayName("a real javac compile on Java 8 leaves a capture")
  void javacCompileOnJdk8IsCaptured(@TempDir Path dir) throws Exception {
    Path source = dir.resolve("Hello.java");
    Files.writeString(source, "public class Hello { public static void main(String[] a) {} }");
    Path out = dir.resolve("out");
    Files.createDirectories(out);
    Path captures = dir.resolve("captures");
    Files.createDirectories(captures);

    ProcessBuilder builder =
        new ProcessBuilder(
            jdk8Java().toString(),
            "-javaagent:" + agentJar(),
            "-D" + CaptureRecorder.PROP_OUTPUT_DIR + "=" + captures,
            // javac's own main class: the `javac` launcher is a wrapper
            // around it, and the compiler classes are what is hooked.
            "-cp",
            jdk8CompilerClasspath(),
            "com.sun.tools.javac.Main",
            "-d",
            out.toString(),
            source.toString());
    builder.redirectErrorStream(true);
    Process process = builder.start();
    String output = new String(process.getInputStream().readAllBytes());

    assertThat(process.waitFor()).as("the output was:\n" + output).isZero();

    // The class file really was produced.
    assertThat(out.resolve("Hello.class")).as("the compiled class").exists();

    // And the capture really was written.
    List<CaptureEntry> entries = CaptureReader.readAll(captures);
    assertThat(entries).as("the captures:\n" + output).hasSize(1);

    CaptureEntry entry = entries.get(0);
    assertThat(entry.compiler()).isEqualTo("javac");
    assertThat(entry.language()).isEqualTo("java");
    assertThat(entry.compilation()).isTrue();
    assertThat(entry.sourceFiles()).hasSize(1);
    assertThat(entry.sourceFiles().get(0).toString()).endsWith("Hello.java");
    assertThat(entry.outputDirectory()).isNotNull();
    assertThat(entry.javaVersion()).as("the JDK that did the compile").startsWith("1.8");
  }

  /**
   * Whether the Kotlin compiler on the classpath can be loaded on a Java 8 JVM.
   *
   * <p>The compiler jar has to be Java 8 byte code. The Kotlin compiler that runs on Java 8 is an
   * older one (2.1.0 in the {@code jdk8-compilers} profile), and older compilers are built for
   * older JVMs.
   */
  static boolean kotlinRunsOnJava8() {
    return CompilerJdkSupport.runsOnJava8("kotlin-compiler-embeddable-");
  }

  /**
   * Whether the Scala 3 compiler on the classpath can be loaded on a Java 8 JVM.
   *
   * <p>This one matters: the default profile uses Scala 3.10.0, whose jar is class file version
   * 61 -- Java 17 byte code. Loaded on a Java 8 JVM it fails with
   * {@code UnsupportedClassVersionError}. The {@code jdk8-compilers} profile swaps in Scala 3.3.4,
   * which is Java 8 byte code throughout.
   */
  static boolean scala3RunsOnJava8() {
    return CompilerJdkSupport.runsOnJava8("scala3-compiler_3-");
  }

  /**
   * Whether the Kotlin test can run: there is a Java 8 JDK, and the Kotlin compiler on the
   * classpath is Java 8 byte code.
   *
   * <p>The two conditions are combined rather than stacked, because {@code @EnabledIf} is not a
   * repeatable annotation in this JUnit version.
   */
  static boolean hasJdk8AndKotlinRunsOnJava8() {
    return hasJdk8() && kotlinRunsOnJava8();
  }

  /**
   * Whether the Scala 3 test can run: there is a Java 8 JDK, and the Scala 3 compiler on the
   * classpath is Java 8 byte code.
   */
  static boolean hasJdk8AndScala3RunsOnJava8() {
    return hasJdk8() && scala3RunsOnJava8();
  }

  @Test
  @EnabledIf("hasJdk8AndKotlinRunsOnJava8")
  @DisplayName("a real Kotlin compile on Java 8 leaves a capture")
  void kotlinCompileOnJdk8IsCaptured(@TempDir Path dir) throws Exception {
    Path source = dir.resolve("Main.kt");
    Files.writeString(source, "fun main() { println(\"hi\") }");
    Path out = dir.resolve("out");
    Files.createDirectories(out);
    Path captures = dir.resolve("captures");
    Files.createDirectories(captures);

    List<CaptureEntry> entries =
        runCompiler(
            jdk8Java(),
            captures,
            "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
            "-no-stdlib",
            "-classpath",
            libraryJar("kotlin-stdlib-"),
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

  @Test
  @EnabledIf("hasJdk8AndScala3RunsOnJava8")
  @DisplayName("a real Scala 3 compile on Java 8 leaves a capture")
  void scala3CompileOnJdk8IsCaptured(@TempDir Path dir) throws Exception {
    Path source = dir.resolve("Hello.scala");
    Files.writeString(source, "object Hello { def main(args: Array[String]): Unit = () }");
    Path out = dir.resolve("out");
    Files.createDirectories(out);
    Path captures = dir.resolve("captures");
    Files.createDirectories(captures);

    List<CaptureEntry> entries =
        runCompiler(
            jdk8Java(),
            captures,
            "dotty.tools.dotc.Main",
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
  }

  /** Runs a compiler with the agent attached and reads the captures back. */
  private static List<CaptureEntry> runCompiler(
      Path javaExecutable, Path captures, String mainClass, String... arguments) throws Exception {
    List<String> command = new java.util.ArrayList<>();
    command.add(javaExecutable.toString());
    command.add("-javaagent:" + CaptureEndToEndTest.agentJar());
    command.add("-D" + CaptureRecorder.PROP_OUTPUT_DIR + "=" + captures);
    command.add("-cp");
    command.add(System.getProperty("java.class.path"));
    command.add(mainClass);
    command.addAll(java.util.Arrays.asList(arguments));

    ProcessBuilder builder = new ProcessBuilder(command);
    builder.redirectErrorStream(true);
    Process process = builder.start();
    String output = new String(process.getInputStream().readAllBytes());
    assertThat(process.waitFor()).as("the output was:\n" + output).isZero();

    return CaptureReader.readAll(captures);
  }

  /** The jar of a library on the test classpath, by its file name prefix. */
  private static String libraryJar(String prefix) {
    for (String entry : System.getProperty("java.class.path").split(File.pathSeparator)) {
      Path p = Path.of(entry);
      String name = p.getFileName() == null ? "" : p.getFileName().toString();
      if (name.startsWith(prefix) && name.endsWith(".jar")) {
        return p.toString();
      }
    }
    throw new org.opentest4j.TestAbortedException("no jar named " + prefix + " on the test classpath");
  }

  /**
   * The Scala 3 compile classpath.
   *
   * <p>The Scala 3 compiler needs the whole library set on its *compile* classpath -- the library,
   * the interfaces, the TASTy core and the standard library -- not just the one jar.
   */
  private static String scalaLibrary() {
    java.util.List<String> jars = new java.util.ArrayList<>();
    for (String entry : System.getProperty("java.class.path").split(File.pathSeparator)) {
      Path p = Path.of(entry);
      String name = p.getFileName() == null ? "" : p.getFileName().toString();
      if (name.endsWith(".jar")
          && (name.startsWith("scala3-library_3-")
              || name.startsWith("scala3-interfaces-")
              || name.startsWith("tasty-core_3-")
              || name.startsWith("scala-library-"))) {
        jars.add(p.toString());
      }
    }
    if (jars.isEmpty()) {
      throw new org.opentest4j.TestAbortedException("the Scala 3 libraries are not on the test classpath");
    }
    return String.join(File.pathSeparator, jars);
  }

  /** The agent jar, assembled from the classes and ByteBuddy. */
  private static Path agentJar() throws Exception {
    return CaptureEndToEndTest.agentJar();
  }
}
