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

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * The few JDK APIs the agent uses that Java 8 does not have.
 *
 * <p><b>Why this exists.</b> The agent has to load on the JDK it is capturing a compile on, and a
 * large share of the JVM installations still on Java 8 run their builds there. So the agent's own
 * classes are compiled for Java 8 ({@code maven.compiler.release=8}), which means every API that
 * arrived after Java 8 is out of reach.
 *
 * <p>There are only a handful of them, and they are all trivial:
 *
 * <ul>
 *   <li>{@code InputStream#readAllBytes()} -- Java 9;
 *   <li>{@code Path#of(String)} -- Java 11;
 *   <li>{@code String#isBlank()} and {@code String#strip()} -- Java 11;
 *   <li>{@code List#copyOf(Collection)} -- Java 10;
 *   <li>{@code Files#readString(Path)} and {@code Files#writeString(Path, CharSequence)} -- Java 11;
 *   <li>{@code Method#trySetAccessible()} -- Java 9;
 *   <li>{@code Stream#toList()} -- Java 16.
 * </ul>
 *
 * <p>They live here rather than being scattered over the callers, so that the list of what the
 * agent may not use is in one place -- and so that a future contributor does not have to remember
 * that {@code Path.of} does not exist on Java 8.
 *
 * <p><b>Why not just require Java 11.</b> The compilers this module captures do run on Java 11 and
 * newer, but the <em>build</em> they are driven from is a separate process, and that build runs on
 * whatever JDK the project uses. A project on Java 8 cannot load a Java 11 agent, so it would get
 * no capture at all -- silently, because the build would still succeed.
 */
final class Jdk8 {

  private Jdk8() {}

  /**
   * Read a stream to its end.
   *
   * <p>The Java 9 replacement for {@code InputStream#readAllBytes()}.
   *
   * @param in the stream
   * @return the bytes
   * @throws IOException if the stream fails
   */
  static byte[] readAllBytes(InputStream in) throws IOException {
    ByteArrayOutputStream out = new ByteArrayOutputStream(Math.max(64, in.available()));
    byte[] buffer = new byte[8192];
    int read;
    while ((read = in.read(buffer)) != -1) {
      out.write(buffer, 0, read);
    }
    return out.toByteArray();
  }

  /**
   * Whether a string is empty or only whitespace.
   *
   * <p>The Java 11 replacement for {@code String#isBlank()}.
   *
   * @param s the string, may be null
   * @return true when it is null, empty, or only whitespace
   */
  static boolean isBlank(String s) {
    if (s == null || s.isEmpty()) {
      return true;
    }
    for (int i = 0; i < s.length(); i++) {
      if (!Character.isWhitespace(s.charAt(i))) {
        return false;
      }
    }
    return true;
  }

  /**
   * An immutable list of the given elements.
   *
   * <p>The Java 9 replacement for {@code List.of(...)}.
   *
   * @param values the values
   * @param <T> the element type
   * @return an immutable list
   */
  static <T> java.util.List<T> of(T... values) {
    java.util.List<T> out = new java.util.ArrayList<T>(values.length);
    for (T value : values) {
      out.add(value);
    }
    return java.util.Collections.unmodifiableList(out);
  }

  /**
   * The platform class loader.
   *
   * <p>The Java 9 replacement for {@code ClassLoader#getPlatformClassLoader()}. On Java 8 there is
   * no such loader: the extension loader is the closest thing, and it is the one that owns the
   * platform classes.
   *
   * @return the platform class loader
   */
  static ClassLoader platformClassLoader() {
    try {
      // Java 9 and newer.
      return (ClassLoader)
          ClassLoader.class.getMethod("getPlatformClassLoader").invoke(null);
    } catch (Exception e) {
      // Java 8: the extension loader is the platform loader's ancestor,
      // and it is the one that can see the classes the agent needs.
      return ClassLoader.getSystemClassLoader().getParent();
    }
  }

  /**
   * Make an immutable copy of a list.
   *
   * <p>The Java 10 replacement for {@code List#copyOf(Collection)}. The difference that matters
   * here is that {@code copyOf} rejects a null element, which the agent's data never has but which
   * would turn a malformed capture into an exception.
   *
   * @param values the values
   * @param <T> the element type
   * @return an immutable copy
   */
  static <T> java.util.List<T> copyOf(java.util.Collection<? extends T> values) {
    return java.util.Collections.unmodifiableList(new java.util.ArrayList<T>(values));
  }

  /**
   * Make a path from a string.
   *
   * <p>The Java 11 replacement for {@code Path#of(String)}.
   *
   * @param path the path
   * @return the path
   */
  static java.nio.file.Path path(String path) {
    return java.nio.file.Paths.get(path);
  }

  /**
   * Read a file as a string.
   *
   * <p>The Java 11 replacement for {@code Files#readString(Path)}.
   *
   * @param file the file
   * @param charset the charset
   * @return the contents
   * @throws IOException if the file cannot be read
   */
  static String readString(java.nio.file.Path file, java.nio.charset.Charset charset)
      throws IOException {
    byte[] bytes = java.nio.file.Files.readAllBytes(file);
    return new String(bytes, charset);
  }

  /**
   * Write a string to a file.
   *
   * <p>The Java 11 replacement for {@code Files#writeString(Path, CharSequence)}.
   *
   * @param file the file
   * @param content the content
   * @param charset the charset
   * @throws IOException if the file cannot be written
   */
  static void writeString(java.nio.file.Path file, String content, java.nio.charset.Charset charset)
      throws IOException {
    java.nio.file.Files.write(file, content.getBytes(charset));
  }

  /**
   * Make a member accessible, if the JVM allows it.
   *
   * <p>The Java 9 replacement for {@code Method#trySetAccessible()}. On Java 8 the call always
   * succeeds unless a security manager forbids it, which is signalled by an exception.
   *
   * @param member the field or method
   * @return true when the member is accessible
   */
  static boolean trySetAccessible(java.lang.reflect.AccessibleObject member) {
    try {
      member.setAccessible(true);
      return true;
    } catch (RuntimeException e) {
      // A security manager said no.
      return false;
    }
  }
}

