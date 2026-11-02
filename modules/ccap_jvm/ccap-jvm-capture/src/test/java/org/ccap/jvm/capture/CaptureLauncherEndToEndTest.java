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
 * The launcher attaches the agent to a real compile.
 *
 * <p><b>Why this test exists.</b> The launcher's job is to make the agent land in the compiler's
 * JVM. That can go wrong in several ways that all look the same -- a compile that succeeds and no
 * capture -- so the only way to know it works is to run a real compile through it and look for the
 * capture.
 *
 * <p>The {@code javac} case is the interesting one, because the {@code javac} command line cannot
 * take a {@code -javaagent}: it is a program, not a JVM launcher, and its option parser rejects the
 * flag. The launcher therefore attaches the agent through the environment, which every JVM honours.
 */
class CaptureLauncherEndToEndTest {

  /** Whether a java executable is on the path. */
  static boolean hasJava() {
    try {
      Path home = Path.of(System.getProperty("java.home"));
      Path java = home.resolve("bin").resolve(isWindows() ? "java.exe" : "java");
      return Files.isExecutable(java);
    } catch (RuntimeException e) {
      return false;
    }
  }

  private static boolean isWindows() {
    return System.getProperty("os.name", "").toLowerCase().contains("win");
  }

  @Test
  @EnabledIf("hasJava")
  @DisplayName("a javac run through the launcher leaves a capture")
  void javacThroughTheLauncherIsCaptured(@TempDir Path dir) throws Exception {
    Path source = dir.resolve("Hello.java");
    Files.writeString(source, "public class Hello { }");
    Path out = dir.resolve("out");
    Files.createDirectories(out);
    Path captures = dir.resolve("captures");
    Files.createDirectories(captures);

    // The javac launcher: it takes the compiler's own options, and it
    // rejects -javaagent. This is the case the launcher has to solve.
    Path javac = Path.of(System.getProperty("java.home"))
        .resolve("bin")
        .resolve(isWindows() ? "javac.exe" : "javac");

    List<CaptureEntry> entries =
        run(dir, captures, javac.toString(), "-d", out.toString(), source.toString());

    assertThat(entries).as("the captures in " + captures).hasSize(1);
    CaptureEntry entry = entries.get(0);
    assertThat(entry.compiler()).isEqualTo("javac");
    assertThat(entry.language()).isEqualTo("java");
    assertThat(entry.sourceFiles()).hasSize(1);
    assertThat(entry.sourceFiles().get(0).toString()).endsWith("Hello.java");
    // The class file really was produced.
    assertThat(Files.exists(out.resolve("Hello.class"))).isTrue();
  }

  @Test
  @EnabledIf("hasJava")
  @DisplayName("a java run through the launcher leaves a capture")
  void javaThroughTheLauncherIsCaptured(@TempDir Path dir) throws Exception {
    Path source = dir.resolve("Hello.java");
    Files.writeString(source, "public class Hello { }");
    Path out = dir.resolve("out");
    Files.createDirectories(out);
    Path captures = dir.resolve("captures");
    Files.createDirectories(captures);

    Path java = Path.of(System.getProperty("java.home"))
        .resolve("bin")
        .resolve(isWindows() ? "java.exe" : "java");

    List<CaptureEntry> entries =
        run(
            dir,
            captures,
            java.toString(),
            "-cp",
            System.getProperty("java.class.path"),
            "com.sun.tools.javac.Main",
            "-d",
            out.toString(),
            source.toString());

    assertThat(entries).as("the captures in " + captures).hasSize(1);
    assertThat(entries.get(0).compiler()).isEqualTo("javac");
    assertThat(Files.exists(out.resolve("Hello.class"))).isTrue();
  }

  /**
   * Runs the launcher with the given command, and reads the captures back.
   *
   * @param workDir the working directory
   * @param captures the capture directory
   * @param command the command and its arguments
   * @return the captures
   */
  private static List<CaptureEntry> run(Path workDir, Path captures, String... command)
      throws Exception {
    List<String> argv = new ArrayList<>();
    argv.add(javaExecutable().toString());
    argv.add("-cp");
    argv.add(System.getProperty("java.class.path"));
    // The launcher has to find the agent jar. It is normally the jar this
    // class came from, but the test runs from target/classes, so it is
    // passed explicitly.
    argv.add("-D" + CaptureLauncher.PROP_AGENT_JAR + "=" + CaptureEndToEndTest.agentJar());
    argv.add(CaptureLauncher.class.getName());
    argv.addAll(java.util.Arrays.asList(command));

    ProcessBuilder builder = new ProcessBuilder(argv);
    builder.redirectErrorStream(true);
    builder.directory(workDir.toFile());
    builder.environment().put(CaptureLauncher.ENV_CAPTURE_DIR, captures.toString());
    Process process = builder.start();
    String output = new String(process.getInputStream().readAllBytes());
    assertThat(process.waitFor(180, TimeUnit.SECONDS)).as("the launcher finished").isTrue();
    assertThat(process.exitValue()).as("the output was:\n" + output).isZero();

    return CaptureReader.readAll(captures);
  }

  /** The java executable of the JDK that runs the test. */
  private static Path javaExecutable() {
    return Path.of(System.getProperty("java.home"))
        .resolve("bin")
        .resolve(isWindows() ? "java.exe" : "java");
  }
}
