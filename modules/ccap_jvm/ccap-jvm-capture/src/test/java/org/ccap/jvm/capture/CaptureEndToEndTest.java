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
import java.util.List;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.EnabledIf;
import org.junit.jupiter.api.io.TempDir;

/**
 * The end-to-end test: a real compile, through the real agent.
 *
 * <p><b>Why this test is not optional.</b> Every other test checks a piece. This one checks the
 * thing that matters: that a compile which really happened leaves a capture behind, with the
 * classpath and the sources in it.
 *
 * <p>The failure mode it guards against is the one that never shows up in a unit test: the agent
 * installs, the advice applies, and then the recorder cannot be resolved from the bootstrap loader
 * -- so the compile succeeds, no capture appears, and nothing says why.
 */
class CaptureEndToEndTest {

  /** Whether a JDK is on the path -- the test needs {@code javac}. */
  static boolean hasJavac() {
    try {
      Path javaHome = Path.of(System.getProperty("java.home"));
      Path javac = javaHome.resolve("bin").resolve(isWindows() ? "javac.exe" : "javac");
      return Files.isExecutable(javac);
    } catch (RuntimeException e) {
      return false;
    }
  }

  private static boolean isWindows() {
    return System.getProperty("os.name", "").toLowerCase().contains("win");
  }

  @Test
  @EnabledIf("hasJavac")
  @DisplayName("a real javac compile leaves a capture with the classpath and the sources")
  void javacCompileIsCaptured(@TempDir Path dir) throws Exception {
    // A source file and a classpath entry.
    Path source = dir.resolve("Hello.java");
    Files.writeString(source, "public class Hello { public static void main(String[] a) {} }");
    Path lib = dir.resolve("lib.jar");
    // A real (empty) jar: javac opens the classpath entries, and a file
    // that is not a jar is an error.
    writeEmptyJar(lib);
    Path out = dir.resolve("out");
    // javac requires the output directory to exist.
    Files.createDirectories(out);

    Path captures = dir.resolve("captures");
    Files.createDirectories(captures);

    // Run javac with the agent.
    Path java = Path.of(System.getProperty("java.home")).resolve("bin")
        .resolve(isWindows() ? "java.exe" : "java");
    Path agentJar = agentJar();

    ProcessBuilder builder =
        new ProcessBuilder(
            java.toString(),
            "-javaagent:" + agentJar,
            "-D" + CaptureRecorder.PROP_OUTPUT_DIR + "=" + captures,
            // javac's own main class. The `javac` launcher is a wrapper
            // around it, and the wrapper is not what is being hooked --
            // the compiler classes are.
            "-cp",
            System.getProperty("java.class.path"),
            "com.sun.tools.javac.Main",
            // The compile's own classpath. It is separate from the JVM's:
            // the JVM's classpath is where the compiler lives, and the
            // compiler's classpath is what the compiled code sees.
            "--class-path",
            lib.toString(),
            "-d",
            out.toString(),
            source.toString());
    builder.redirectErrorStream(true);
    Process process = builder.start();
    String output = new String(process.getInputStream().readAllBytes());
    assertThat(process.waitFor(120, TimeUnit.SECONDS)).as("javac finished").isTrue();
    assertThat(process.exitValue()).as("javac output: " + output).isZero();

    // The capture must be there.
    List<CaptureEntry> entries = CaptureReader.readAll(captures);
    assertThat(entries).as("captures in " + captures).hasSize(1);

    CaptureEntry entry = entries.get(0);
    assertThat(entry.compiler()).isEqualTo("javac");
    assertThat(entry.language()).isEqualTo("java");
    assertThat(entry.classpath()).hasSize(1);
    assertThat(entry.classpath().get(0).toString()).contains("lib.jar");
    assertThat(entry.outputDirectory()).isNotNull();
    assertThat(entry.sourceFiles()).hasSize(1);
    assertThat(entry.sourceFiles().get(0).toString()).endsWith("Hello.java");
    assertThat(entry.compilation()).isTrue();
    // The class file really was produced.
    assertThat(Files.exists(out.resolve("Hello.class"))).isTrue();
  }

  @Test
  @EnabledIf("hasJavac")
  @DisplayName("a compile reached through the javax.tools API is captured too")
  void apiCompileIsCaptured(@TempDir Path dir) throws Exception {
    // The API path is what Gradle, Maven and the IDEs use, and it is a
    // different set of entry points than the command line.
    Path source = dir.resolve("Hello.java");
    Files.writeString(source, "public class Hello { }");
    Path out = dir.resolve("out");
    Files.createDirectories(out);

    Path captures = dir.resolve("captures");
    Files.createDirectories(captures);

    Path java = Path.of(System.getProperty("java.home")).resolve("bin")
        .resolve(isWindows() ? "java.exe" : "java");
    Path agentJar = agentJar();

    ProcessBuilder builder =
        new ProcessBuilder(
            java.toString(),
            "-javaagent:" + agentJar,
            "-D" + CaptureRecorder.PROP_OUTPUT_DIR + "=" + captures,
            "-cp",
            System.getProperty("java.class.path"),
            ApiCompileRunner.class.getName(),
            source.toString(),
            out.toString());
    builder.redirectErrorStream(true);
    Process process = builder.start();
    String output = new String(process.getInputStream().readAllBytes());
    assertThat(process.waitFor(120, TimeUnit.SECONDS)).as("runner output: " + output).isTrue();
    assertThat(process.exitValue()).as("runner output: " + output).isZero();

    List<CaptureEntry> entries = CaptureReader.readAll(captures);
    assertThat(entries).as("captures in " + captures).hasSize(1);
    CaptureEntry entry = entries.get(0);
    assertThat(entry.api()).isEqualTo("api");
    assertThat(entry.compiler()).isEqualTo("javac");
    assertThat(entry.sourceFiles()).hasSize(1);
  }

  /**
   * The agent jar.
   *
   * <p><b>Why the test builds it itself.</b> The agent has to be a jar with an agent manifest:
   * the {@code -javaagent} mechanism reads the manifest entries to find {@code premain}, and it
   * loads the jar with only its own classes on the classpath -- so ByteBuddy has to be inside
   * it.
   *
   * <p>Building it here, rather than depending on a packaging phase, keeps the test honest: it
   * proves that the classes in {@code target/classes} plus ByteBuddy really are enough to capture
   * a compile.
   */
  static Path agentJar() throws Exception {
    // The agent's own classes -- the MAIN classes, not the test classes.
    // The test's code source is target/test-classes; the agent's is
    // target/classes, next to it.
    Path testClasses =
        Path.of(
            CaptureEndToEndTest.class.getProtectionDomain().getCodeSource().getLocation().toURI());
    Path module = testClasses.getParent().getParent();
    Path target = module.resolve("target");
    Path jar = target.resolve("ccap-jvm-capture-agent-test.jar");

    Path byteBuddy = findByteBuddy();

    Path staging = target.resolve("agent-staging");
    deleteRecursively(staging);
    Files.createDirectories(staging);

    // The agent's classes, with the manifest.
    Path classesDir = module.resolve("target").resolve("classes");
    if (Files.isDirectory(classesDir)) {
      copyTree(classesDir, staging);
    } else {
      // The module was packaged; unpack the agent jar instead.
      Path packaged = target.resolve("ccap-jvm-capture-0.1.0-SNAPSHOT.jar");
      if (!Files.isRegularFile(packaged)) {
        throw new org.opentest4j.TestAbortedException("the agent classes are not built: " + classesDir);
      }
      unzipInto(packaged, staging);
    }

    // ByteBuddy, unpacked into the same tree.
    unzipInto(byteBuddy, staging);

    // The manifest. It is written outside the staged tree, because the jar
    // tool would otherwise include it as a regular entry.
    Path manifest = target.resolve("agent-manifest.mf");
    Files.writeString(
        manifest,
        "Manifest-Version: 1.0\r\n"
            + "Premain-Class: org.ccap.jvm.capture.CaptureAgent\r\n"
            + "Agent-Class: org.ccap.jvm.capture.CaptureAgent\r\n"
            + "Can-Redefine-Classes: true\r\n"
            + "Can-Retransform-Classes: true\r\n"
            + "\r\n");

    // The jar. The manifest is passed explicitly with -m: the jar tool
    // does not pick a MANIFEST.MF up from the staged tree on its own.
    Path jarTool = Path.of(System.getProperty("java.home")).resolve("bin")
        .resolve(isWindows() ? "jar.exe" : "jar");
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

  /**
   * The ByteBuddy jar, from the local Maven repository.
   *
   * <p>Found by walking the repository for the artifact the build depends on. The version is
   * read from the build's own dependency list, so the test does not hard-code it.
   */
  private static Path findByteBuddy() throws Exception {
    // The test's own classpath carries ByteBuddy, because the module
    // depends on it. So the jar is right there.
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

  /** Write an empty but valid jar. */
  private static void writeEmptyJar(Path jar) throws Exception {
    try (java.util.jar.JarOutputStream out =
        new java.util.jar.JarOutputStream(Files.newOutputStream(jar))) {
      // A manifest, so the jar is a well-formed one.
      out.putNextEntry(new java.util.jar.JarEntry("META-INF/MANIFEST.MF"));
      out.write("Manifest-Version: 1.0\r\n\r\n".getBytes(java.nio.charset.StandardCharsets.UTF_8));
      out.closeEntry();
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
