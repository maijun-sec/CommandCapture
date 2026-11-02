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

import java.util.ArrayList;
import java.util.Collection;
import java.util.List;

/**
 * Getting the file paths out of the objects the three compilers hand over.
 *
 * <p><b>Why this is reflective.</b> The advice runs inside the compiler's own classes, and the
 * objects it sees belong to the compiler's own class loader -- Kotlin's {@code KtFile}, Scala 3's
 * {@code AbstractFile}. The agent cannot reference those types: they are not on its class path, and
 * the bootstrap loader that the recorder lives on certainly cannot see them.
 *
 * <p>So the paths are read by name, reflectively. The method names are fixed and well known:
 *
 * <ul>
 *   <li>Kotlin's {@code KtFile#getVirtualFilePath()} -- the path of the file the PSI element
 *       stands for.
 *   <li>Scala 3's {@code AbstractFile#path()} -- the path of the file; and {@code jpath()} for the
 *       Java path form, which is what an analyser can open.
 *   <li>Scala 2's {@code Global#compile(List<String>)} -- the paths are the argument itself, so no
 *       reflection is needed.
 * </ul>
 *
 * <p><b>Why the method is looked up on the interfaces too.</b> A Kotlin {@code KtFile} is a
 * concrete class that is not exported to unnamed modules; its interface may be. The lookup
 * therefore walks the interfaces first, exactly as the recorder does for {@code JavaFileObject}.
 */
public final class SourcePaths {

  private SourcePaths() {}

  /**
   * The path of one file object, or {@code null} when it has none.
   *
   * <p>The method names are tried in order. The first one that exists and returns a usable value
   * wins.
   *
   * @param file the file object
   * @return the path, or {@code null}
   */
  static String pathOf(Object file) {
    if (file == null) {
      return null;
    }
    if (file instanceof java.nio.file.Path) {
      java.nio.file.Path p = (java.nio.file.Path) file;
      return p.toString();
    }
    if (file instanceof String) {
      String s = (String) file;
      return Jdk8.isBlank(s) ? null : s;
    }
    if (file instanceof char[]) {
      char[] chars = (char[]) file;
      // ECJ's ICompilationUnit#getFileName() returns a char[], not a
      // String. This is the only JVM language whose file name is not a
      // String or a path object.
      String s = new String(chars);
      return Jdk8.isBlank(s) ? null : s;
    }
    // The candidates, in the order they are tried.  The first is the one
    // that is specific to the file's own class; the others are the
    // fallbacks that a wrapper or a proxy may expose instead.
    String[] candidates = {
      "getVirtualFilePath", // Kotlin's KtFile
      "getOriginalPath", // Kotlin's LightVirtualFile and friends
      "path", // Scala 3's AbstractFile
      "jpath", // Scala 3's AbstractFile, in Java path form
      "getName", // javax.tools.JavaFileObject
      "getPath", // a plain file wrapper
      "getFileName", // ECJ's ICompilationUnit
    };
    for (String candidate : candidates) {
      String value = callString(file, candidate);
      if (value != null && !Jdk8.isBlank(value)) {
        return value;
      }
    }
    return null;
  }

  /**
   * The paths of a collection of file objects.
   *
   * <p>The collection is walked reflectively: the compilers hand over their own collection types
   * ({@code List<KtFile>}, {@code Set<AbstractFile>}, Scala's {@code List}), and the agent cannot
   * name any of them.
   *
   * @param files the collection, may be null
   * @return the paths, empty when there are none
   */
  static List<String> pathsOf(Object files) {
    List<String> out = new ArrayList<>();
    if (files == null) {
      return out;
    }
    if (files instanceof String) {
      String s = (String) files;
      if (!Jdk8.isBlank(s)) {
        out.add(s);
      }
      return out;
    }
    if (files instanceof Iterable<?>) {
      Iterable<?> iterable = (Iterable<?>) files;
      for (Object item : iterable) {
        String path = pathOf(item);
        if (path != null) {
          out.add(path);
        }
      }
      return out;
    }
    // A Scala collection is not necessarily an Iterable from the Java side --
    // it is, through scala.collection.Iterable, but the element type may be
    // a value class. The collection is therefore walked reflectively too.
    Object iterable = callMethod(files, "iterator");
    if (iterable != null && iterable instanceof Iterable<?>) {
      Iterable<?> it = (Iterable<?>) iterable;
      for (Object item : it) {
        String path = pathOf(item);
        if (path != null) {
          out.add(path);
        }
      }
      return out;
    }
    // A plain array of paths (Scala 2's compile() receives one).
    if (files.getClass().isArray()) {
      int length = java.lang.reflect.Array.getLength(files);
      for (int i = 0; i < length; i++) {
        String path = pathOf(java.lang.reflect.Array.get(files, i));
        if (path != null) {
          out.add(path);
        }
      }
      return out;
    }
    return out;
  }

  /**
   * The strings of a collection.
   *
   * <p>Scala 2's {@code Global#compile} receives a {@code scala.collection.immutable.List<String>},
   * whose elements are strings. The collection is walked reflectively, because the agent cannot
   * name the Scala type.
   *
   * @param values the collection, may be null
   * @return the strings, empty when there are none
   */
  static List<String> stringsOf(Object values) {
    List<String> out = new ArrayList<>();
    if (values == null) {
      return out;
    }
    if (values instanceof String) {
      String s = (String) values;
      if (!Jdk8.isBlank(s)) {
        out.add(s);
      }
      return out;
    }
    if (values instanceof Iterable<?>) {
      Iterable<?> iterable = (Iterable<?>) values;
      for (Object item : iterable) {
        if (item != null) {
          out.add(String.valueOf(item));
        }
      }
      return out;
    }
    Object iterator = callMethod(values, "iterator");
    if (iterator != null && iterator instanceof Iterable<?>) {
      Iterable<?> it = (Iterable<?>) iterator;
      for (Object item : it) {
        if (item != null) {
          out.add(String.valueOf(item));
        }
      }
      return out;
    }
    if (values.getClass().isArray()) {
      int length = java.lang.reflect.Array.getLength(values);
      for (int i = 0; i < length; i++) {
        Object item = java.lang.reflect.Array.get(values, i);
        if (item != null) {
          out.add(String.valueOf(item));
        }
      }
      return out;
    }
    return out;
  }

  /**
   * Call a no-argument method that returns a String, reflectively.
   *
   * <p>The method is looked up on the interfaces the object implements as well as on the object
   * itself: the concrete classes are often in a package that is not exported, and calling a public
   * method on an inaccessible class fails with {@code IllegalAccessException} even though the
   * method is public.
   */
  private static String callString(Object target, String methodName) {
    Object value = callMethod(target, methodName);
    if (value == null) {
      return null;
    }
    if (value instanceof String) {
      String s = (String) value;
      return s;
    }
    if (value instanceof char[]) {
      char[] chars = (char[]) value;
      // ECJ's ICompilationUnit#getFileName() returns a char[].
      return new String(chars);
    }
    // A path object (a java.io.File, a scala.reflect.io.File) is turned
    // into its path form.
    Object path = callMethod(value, "getPath");
    if (path instanceof String) {
      String s = (String) path;
      return s;
    }
    Object absolute = callMethod(value, "getAbsolutePath");
    if (absolute instanceof String) {
      String s = (String) absolute;
      return s;
    }
    return String.valueOf(value);
  }

  /**
   * Call a no-argument method, reflectively, and return whatever it returned.
   *
   * @param target the object
   * @param methodName the method name
   * @return the value, or {@code null} when the call failed
   */
  private static Object callMethod(Object target, String methodName) {
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
    Object value = invoke(target.getClass(), target, methodName);
    if (value != null) {
      return value;
    }
    // A method declared on the class itself but not accessible (a Kotlin
    // `internal`, say) is still worth trying: setAccessible may work when
    // the module is open.
    try {
      java.lang.reflect.Method m = target.getClass().getDeclaredMethod(methodName);
      if (!Jdk8.trySetAccessible(m)) {
        return null;
      }
      return m.invoke(target);
    } catch (ReflectiveOperationException | RuntimeException e) {
      return null;
    }
  }

  /** Invoke a method looked up on a specific class, swallowing every failure. */
  private static Object invoke(Class<?> owner, Object target, String methodName) {
    try {
      java.lang.reflect.Method m = owner.getMethod(methodName);
      return m.invoke(target);
    } catch (ReflectiveOperationException | RuntimeException e) {
      return null;
    }
  }

  /**
   * The collection of paths, for the tests.
   *
   * @param files a collection of file-like objects
   * @return the paths
   */
  static List<String> pathsOfCollection(Collection<?> files) {
    return pathsOf(files);
  }
}

