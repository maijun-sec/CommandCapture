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

import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/**
 * Works out which classes the bootstrap class loader has to be able to resolve.
 *
 * <p><b>Why this is computed rather than listed.</b> The recorder runs on the bootstrap class
 * loader, which sees nothing but the JDK. Every class it references has to be in the bootstrap jar
 * -- including the ones the compiler generates for it, which nobody thinks about when they write the
 * class:
 *
 * <ul>
 *   <li>the synthetic {@code Foo$1} class that {@code javac} emits for a {@code switch} over an
 *       enum, to map the enum constants onto a dense int array;
 *   <li>the nested {@code Foo$Bar} classes of a record or an enum;
 *   <li>the synthetic {@code Foo$1} that a lambda's method produces.
 * </ul>
 *
 * <p>A hand-maintained list gets these wrong. It did: {@code JvmOptions$1} -- the switch-map class
 * of {@code JvmOptions#isSourceFile} -- was missing, and the capture failed with a {@code
 * NoClassDefFoundError} that the advice swallowed. The compile succeeded, the agent reported
 * success, and no capture ever appeared. That is the worst possible failure mode: invisible and
 * total.
 *
 * <p>So the closure is <em>derived</em>: the constant pool of each class names every class it
 * references, and the ones in this package are followed until nothing new is found. A class that
 * is added to the recorder cannot be forgotten, because the list is computed from the bytecode that
 * is actually shipped.
 */
public final class BootstrapClosure {

  /** The package the bootstrap classes live in. */
  private static final String PACKAGE_PREFIX = "org/ccap/jvm/capture/";

  /**
   * The classes the closure starts from.
   *
   * <p>These are the two classes the instrumented bytecode can call: the recorder itself and the
   * bridge. Everything else is reached from them.
   */
  private static final List<String> ROOTS =
      Jdk8.of(
          "org/ccap/jvm/capture/CaptureRecorder",
          "org/ccap/jvm/capture/CompiledSourcesBridge");

  /**
   * The classes that are reached by name rather than by a bytecode reference.
   *
   * <p><b>Why this list exists.</b> The bridge reaches the recorder -- and the recorder's helper
   * {@code SourcePaths} -- reflectively, by class <em>name</em>. A name in a string constant is not
   * a reference the constant pool can show: the pool holds the string, not the class it names. So
   * the closure cannot discover those classes by itself.
   *
   * <p>They are listed here instead. That is a smaller list than the one this class replaced, and
   * a different kind of entry: these are classes reached <em>by name</em>, not classes whose
   * absence would go unnoticed. A class that is only referenced reflectively has to be named
   * somewhere, and this is where.
   */
  private static final List<String> NAMED_REFERENCES =
      Jdk8.of(
          "org/ccap/jvm/capture/SourcePaths",
          "org/ccap/jvm/capture/CaptureEntry",
          "org/ccap/jvm/capture/JvmOptions",
          "org/ccap/jvm/capture/CompilerOption");

  private BootstrapClosure() {}

  /**
   * The class resources that go into the bootstrap jar.
   *
   * <p>Computed from the agent's own class loader: the classes are looked up as they are packaged,
   * so the result is exactly what the shipped jar contains.
   *
   * @param loader the loader the agent's classes are on
   * @return the class resources, {@code .class} files, in a stable order
   */
  public static List<String> compute(ClassLoader loader) {
    Set<String> found = new LinkedHashSet<>();
    Deque<String> queue = new ArrayDeque<>(ROOTS);
    queue.addAll(NAMED_REFERENCES);

    while (!queue.isEmpty()) {
      String className = queue.removeFirst();
      String resource = className + ".class";
      if (!found.add(resource)) {
        continue;
      }
      byte[] bytes = read(loader, resource);
      if (bytes == null) {
        // Not on the agent's classpath. The caller reports it; there is
        // nothing to follow.
        continue;
      }
      for (String referenced : referencedClasses(bytes)) {
        if (referenced.startsWith(PACKAGE_PREFIX) && !found.contains(referenced + ".class")) {
          queue.addLast(referenced);
        }
      }
    }

    List<String> out = new ArrayList<>(found);
    java.util.Collections.sort(out);
    return out;
  }

  /**
   * The classes a class file references.
   *
   * <p>Read off the constant pool. The pool holds every class the file mentions -- as a super
   * class, an interface, a field type, a method descriptor, a {@code catch} type, a {@code new}
   * instruction, and so on. It also holds classes that are only mentioned, not used -- which is
   * fine: putting one extra class in the jar costs a few hundred bytes, while missing one costs
   * the whole capture.
   *
   * <p><b>Why the constant pool and not a real bytecode walk.</b> The pool is the authoritative
   * list of what a class file mentions, it is trivially parseable without a bytecode library, and it
   * cannot miss a reference the way a naive instruction scan can.
   *
   * @param bytes the class file
   * @return the internal names of the referenced classes
   */
  static Set<String> referencedClasses(byte[] bytes) {
    Set<String> out = new LinkedHashSet<>();
    try (DataInputStream in = new DataInputStream(new java.io.ByteArrayInputStream(bytes))) {
      int magic = in.readInt();
      if (magic != 0xCAFEBABE) {
        return out;
      }
      in.readUnsignedShort(); // minor
      in.readUnsignedShort(); // major
      int constantPoolCount = in.readUnsignedShort();

      // The pool is indexed from 1; slot 0 is unused.
      String[] utf8 = new String[constantPoolCount];
      int[] tag = new int[constantPoolCount];
      int[] first = new int[constantPoolCount];
      int[] second = new int[constantPoolCount];

      for (int i = 1; i < constantPoolCount; i++) {
        int t = in.readUnsignedByte();
        tag[i] = t;
        // A plain switch, not a switch expression: the agent has to load
        // on Java 8, where the arrow form does not exist.
        switch (t) {
          case 1:
            utf8[i] = in.readUTF();
            break;
          case 7:
          case 8:
          case 16:
          case 19:
          case 20:
            first[i] = in.readUnsignedShort();
            break;
          case 15:
            in.readUnsignedByte();
            first[i] = in.readUnsignedShort();
            break;
          case 3:
          case 4:
          case 9:
          case 10:
          case 11:
          case 12:
          case 17:
          case 18:
            in.readInt();
            first[i] = -1;
            break;
          case 5:
          case 6:
            in.readLong();
            // Longs and doubles take two pool slots.
            i++;
            break;
          default:
            // An unknown tag means an unknown class file version. Bail
            // out rather than misparse.
            return out;
        }
      }

      // Now resolve the references.
      for (int i = 1; i < constantPoolCount; i++) {
        switch (tag[i]) {
          case 7:
            // A class: the name is a UTF8.
            String name = utf8[first[i]];
            if (name != null) {
              out.add(name);
            }
            break;
          case 12:
          case 13:
          case 14:
            // NameAndType: the descriptor is a UTF8 and may name classes.
            String descriptor = utf8[second[i]];
            if (descriptor != null) {
              addDescriptorClasses(out, descriptor);
            }
            break;
          default:
            // Nothing else names a class.
            break;
        }
      }
    } catch (IOException | RuntimeException e) {
      // A class file that cannot be read yields no references. The
      // verification in the test loads the classes, which catches this.
    }
    return out;
  }

  /**
   * The classes named by a field or method descriptor.
   *
   * <p>A descriptor like {@code (Ljava/lang/String;[[Ljava/util/List;)V} names classes in its
   * parameter and return types. They are references the class file has to resolve, so they count.
   *
   * @param out the set to add to
   * @param descriptor the descriptor
   */
  static void addDescriptorClasses(Set<String> out, String descriptor) {
    int i = 0;
    while (i < descriptor.length()) {
      char c = descriptor.charAt(i);
      if (c != 'L') {
        i++;
        continue;
      }
      int end = descriptor.indexOf(';', i);
      if (end < 0) {
        break;
      }
      out.add(descriptor.substring(i + 1, end));
      i = end + 1;
    }
  }

  /** Read a class resource from a loader. */
  private static byte[] read(ClassLoader loader, String resource) {
    try (InputStream in = loader.getResourceAsStream(resource)) {
      if (in == null) {
        return null;
      }
      return Jdk8.readAllBytes(in);
    } catch (IOException e) {
      return null;
    }
  }
}

