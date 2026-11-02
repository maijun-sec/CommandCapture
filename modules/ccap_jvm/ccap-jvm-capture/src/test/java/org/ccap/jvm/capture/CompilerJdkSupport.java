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

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * Whether the compiler on the test classpath can run on the JVM that is running the test.
 *
 * <p><b>Why this exists.</b> The compilers have different JDK support ranges, and the test suite
 * runs on the build's JDK, which is the newest one available. A compiler that does not know the
 * build's JDK does not fail with a message about the capture -- it fails <em>inside the
 * compiler</em>, before it compiles anything, with an exception from the compiler's own version
 * handling.
 *
 * <p>The concrete case is Kotlin. The Kotlin compiler ships a relocated copy of IntelliJ's
 * {@code JavaVersion}, which parses {@code java.version} into a known set of JDK versions. On a JDK
 * it does not know, the parse throws:
 *
 * <pre>
 *   java.lang.IllegalArgumentException: 25.0.1
 *     at com.intellij.util.lang.JavaVersion.parse(JavaVersion.java:307)
 *     at com.intellij.util.lang.JavaVersion.current(JavaVersion.java:176)
 *     at org.jetbrains.kotlin.cli.jvm.modules.JavaVersionUtilsKt.isAtLeastJava9(...)
 * </pre>
 *
 * That is an upstream Kotlin limitation, not a capture problem: the agent installs, hooks every
 * entry point, and the compiler then dies before it reads a source file. Kotlin 2.1.0 has this
 * problem on JDK 25; Kotlin 2.2.0 does not.
 *
 * <p><b>Why the probe is a capability probe and not a version table.</b> The set of JDKs a given
 * compiler understands moves with the compiler, and hardcoding "Kotlin 2.1.0 does not know JDK
 * 25" would need a new entry every time either side changes. Instead this asks the compiler's own
 * version parser whether it can read the running JDK. That is exactly the question the compiler is
 * about to ask itself, so the answer is authoritative.
 *
 * <p>The probe is done reflectively, against the compiler jar on the classpath, so that the test
 * does not have to depend on the compiler's internals at compile time.
 */
final class CompilerJdkSupport {

  private CompilerJdkSupport() {}

  /**
   * Whether the compiler whose relocated IntelliJ classes are on the classpath can read the running
   * JDK's version.
   *
   * <p>The check is deliberately narrow: it asks whether the compiler's own {@code JavaVersion}
   * parser accepts the running JDK's {@code java.version} string. A compiler that does not use
   * that class at all -- ECJ, for instance -- passes, because the class is absent.
   *
   * @return true when the compiler can run on this JVM
   */
  static boolean compilerUnderstandsThisJdk() {
    try {
      Class<?> javaVersion =
          Class.forName("org.jetbrains.kotlin.com.intellij.util.lang.JavaVersion");
      Object current = javaVersion.getMethod("current").invoke(null);
      // `current` parses java.version. If the compiler does not know this
      // JDK, it throws IllegalArgumentException -- and that is the answer.
      return current != null;
    } catch (ClassNotFoundException e) {
      // The compiler does not use that class, so it has no such limitation.
      return true;
    } catch (ReflectiveOperationException | RuntimeException e) {
      // The compiler's version parser rejected the running JDK.
      return false;
    }
  }

  /**
   * The reason the compiler cannot run on this JVM, for the test report.
   *
   * @return a short description, or null when there is no problem
   */
  static String incompatibilityReason() {
    try {
      Class<?> javaVersion =
          Class.forName("org.jetbrains.kotlin.com.intellij.util.lang.JavaVersion");
      Object current = javaVersion.getMethod("current").invoke(null);
      return current == null ? null : null;
    } catch (ClassNotFoundException e) {
      return null;
    } catch (ReflectiveOperationException | RuntimeException e) {
      Throwable cause = e instanceof java.lang.reflect.InvocationTargetException
          ? e.getCause()
          : e;
      return "the compiler on the classpath cannot parse this JDK's version ("
          + System.getProperty("java.version")
          + "): "
          + cause;
    }
  }

  /**
   * The class file version of the first class in the jar with the given file name prefix, or -1
   * when there is no such jar on the classpath.
   *
   * <p>The class file version is what decides whether a class can be loaded on a JVM: major
   * version 52 is Java 8, 61 is Java 17, and so on. A compiler jar built for a newer Java than the
   * one it is run on fails with {@code UnsupportedClassVersionError} the moment its main class is
   * loaded -- which is the situation of the Scala 3.10 compiler (major 61) on a Java 8 JVM.
   *
   * @param prefix the jar file name prefix
   * @return the major class file version, or -1
   */
  static int majorClassFileVersion(String prefix) {
    for (String entry : System.getProperty("java.class.path", "").split(File.pathSeparator)) {
      Path p = Path.of(entry);
      String name = p.getFileName() == null ? "" : p.getFileName().toString();
      if (!name.startsWith(prefix) || !name.endsWith(".jar")) {
        continue;
      }
      try (java.util.jar.JarFile jar = new java.util.jar.JarFile(p.toFile())) {
        java.util.Enumeration<java.util.jar.JarEntry> entries = jar.entries();
        while (entries.hasMoreElements()) {
          java.util.jar.JarEntry jarEntry = entries.nextElement();
          if (!jarEntry.getName().endsWith(".class")) {
            continue;
          }
          try (java.io.InputStream in = jar.getInputStream(jarEntry)) {
            byte[] header = in.readNBytes(8);
            if (header.length == 8) {
              int major = ((header[6] & 0xFF) << 8) | (header[7] & 0xFF);
              return major;
            }
          }
        }
      } catch (java.io.IOException | RuntimeException e) {
        return -1;
      }
    }
    return -1;
  }

  /**
   * Whether the compiler jar with the given file name prefix can be loaded on a Java 8 JVM.
   *
   * @param prefix the jar file name prefix
   * @return true when the jar is Java 8 byte code, or when there is no such jar
   */
  static boolean runsOnJava8(String prefix) {
    int major = majorClassFileVersion(prefix);
    return major < 0 || major <= 52;
  }

  /**
   * Whether a JDK is available at the given home.
   *
   * @param home the JDK home
   * @return true when the home contains a {@code bin/java}
   */
  static boolean isJdkHome(Path home) {
    if (home == null) {
      return false;
    }
    File dir = home.toFile();
    if (!dir.isDirectory()) {
      return false;
    }
    String exe = System.getProperty("os.name", "").toLowerCase().contains("win") ? "java.exe" : "java";
    return new File(dir, "bin" + File.separator + exe).isFile();
  }

  /**
   * Whether a file exists.
   *
   * @param path the path
   * @return true when the path is a regular file
   */
  static boolean isFile(Path path) {
    return path != null && Files.isRegularFile(path);
  }
}
