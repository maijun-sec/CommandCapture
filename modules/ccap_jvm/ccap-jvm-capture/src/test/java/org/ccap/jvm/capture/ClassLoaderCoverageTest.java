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
 * The agent has to capture a compile no matter which class loader holds the compiler.
 *
 * <p><b>Why this test exists.</b> ByteBuddy's {@code AgentBuilder.Default} ignores two class loaders by
 * default: the bootstrap loader and the platform loader. A compiler class loaded by either of them is
 * therefore never transformed -- and an advice that was never woven in does not fail, does not warn,
 * and does not stop the compile. The compile succeeds, no capture appears, and nothing says why.
 *
 * <p>This was not theoretical. It is exactly what happened with Gradle:
 *
 * <ul>
 *   <li>Gradle's daemon runs the compiler in process, through {@code javax.tools}.
 *   <li>The compiler classes come from the toolchain's own {@code tools.jar}, which the JDK's extension
 *       class loader holds.
 *   <li>The agent was installed, printed "agent installed, hooking 30 entry point(s)", and produced
 *       <b>zero</b> captures -- while the compile itself worked perfectly.
 * </ul>
 *
 * <p>The same javac, reached through a different class path, is loaded by a different loader and does
 * get captured. That is what makes this worth a test: the behaviour differs between two ways of
 * running the same compiler, and the difference is invisible from the outside.
 *
 * <p>The test drives the compiler through the loader that ByteBuddy excludes, and asserts that the
 * capture appears.
 */
class ClassLoaderCoverageTest {

  /**
   * The class loader that the extension loader holds the compiler in.
   *
   * <p>On Java 8 the compiler lives in {@code lib/tools.jar}, and the extension class loader holds it
   * when the jar is on the extension path rather than on the application class path.
   */
  private static boolean isJava8() {
    return System.getProperty("java.specification.version", "").startsWith("1.8");
  }

  @Test
  @EnabledIf("hasToolsJar")
  @DisplayName("a compile through the application class loader is captured")
  void compileThroughTheApplicationLoaderIsCaptured(@TempDir Path dir) throws Exception {
    Path source = dir.resolve("Hello.java");
    Files.writeString(source, "public class Hello { }");
    Path out = dir.resolve("out");
    Files.createDirectories(out);
    Path captures = dir.resolve("captures");
    Files.createDirectories(captures);

    // The compiler is on the application class path, so the application
    // loader holds it. This is the case that always worked.
    Path compiler = compilerJar();
    List<String> command = new java.util.ArrayList<>();
    command.add(javaExecutable().toString());
    command.add("-javaagent:" + CaptureEndToEndTest.agentJar());
    command.add("-D" + CaptureRecorder.PROP_OUTPUT_DIR + "=" + captures);
    command.add("-cp");
    command.add(compiler.toString());
    command.add("com.sun.tools.javac.Main");
    command.add("-d");
    command.add(out.toString());
    command.add(source.toString());

    run(command);

    assertThat(Files.exists(out.resolve("Hello.class"))).as("the compiled class").isTrue();
    assertThat(CaptureReader.readAll(captures)).as("the captures").hasSize(1);
  }

  @Test
  @EnabledIf("hasToolsJar")
  @DisplayName("a compile through the bootstrap class loader is captured")
  void compileThroughTheBootstrapLoaderIsCaptured(@TempDir Path dir) throws Exception {
    Path source = dir.resolve("Hello.java");
    Files.writeString(source, "public class Hello { }");
    Path out = dir.resolve("out");
    Files.createDirectories(out);
    Path captures = dir.resolve("captures");
    Files.createDirectories(captures);

    // -Xbootclasspath/a puts the compiler in the BOOTSTRAP loader, which
    // ByteBuddy ignores by default. Before the loader matcher was widened,
    // this produced no capture at all: the compile succeeded, the agent
    // announced itself, and the capture directory stayed empty.
    Path compiler = compilerJar();
    List<String> command = new java.util.ArrayList<>();
    command.add(javaExecutable().toString());
    command.add("-Xbootclasspath/a:" + compiler);
    command.add("-javaagent:" + CaptureEndToEndTest.agentJar());
    command.add("-D" + CaptureRecorder.PROP_OUTPUT_DIR + "=" + captures);
    command.add("-cp");
    command.add(compiler.toString());
    command.add("com.sun.tools.javac.Main");
    command.add("-d");
    command.add(out.toString());
    command.add(source.toString());

    run(command);

    assertThat(Files.exists(out.resolve("Hello.class"))).as("the compiled class").isTrue();
    assertThat(CaptureReader.readAll(captures)).as("the captures").hasSize(1);
  }

  /** Runs a command and asserts that it succeeded. */
  private static void run(List<String> command) throws Exception {
    ProcessBuilder builder = new ProcessBuilder(command);
    builder.redirectErrorStream(true);
    Process process = builder.start();
    String output = new String(process.getInputStream().readAllBytes());
    assertThat(process.waitFor()).as("the output was:\n" + output).isZero();
  }

  /** The JDK's compiler jar, or the compiler's module on Java 9 and later. */
  private static Path compilerJar() {
    Path home = jdk8Home();
    Path tools = home.resolve("lib").resolve("tools.jar");
    if (Files.isRegularFile(tools)) {
      return tools;
    }
    throw new org.opentest4j.TestAbortedException("no tools.jar in " + home);
  }

  /**
   * The Java 8 home.
   *
   * <p>The test runs the compiler on the Java 8 JVM on purpose: the class loader under test is the
   * one of the JVM that runs the compiler, so the compiler has to run there.
   */
  private static Path jdk8Home() {
    return Path.of(System.getenv("CCAP_TEST_JDK8"));
  }

  /** The java executable of the Java 8 JDK. */
  private static Path javaExecutable() {
    return jdk8Home()
        .resolve("bin")
        .resolve(
            System.getProperty("os.name", "").toLowerCase().contains("win") ? "java.exe" : "java");
  }

  /**
   * Whether the test can run here.
   *
   * <p>It needs a JDK that has the compiler in a jar -- that is, a Java 8 JDK, whose {@code
   * tools.jar} can be put on the bootstrap path on purpose. On Java 9 and later the compiler is a
   * module and there is no jar to move around, so the test does not apply.
   *
   * <p>The Java 8 JDK is the one named by {@code CCAP_TEST_JDK8}, because the test has to <em>run</em>
   * the compiler on that JVM: the class loader under test is the one of the JVM that runs the
   * compiler.
   */
  static boolean hasToolsJar() {
    String home = System.getenv("CCAP_TEST_JDK8");
    if (home == null || home.isBlank()) {
      return false;
    }
    Path tools = Path.of(home).resolve("lib").resolve("tools.jar");
    return Files.isRegularFile(tools);
  }
}
