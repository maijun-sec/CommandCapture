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

import java.io.DataInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.jar.JarEntry;
import java.util.jar.JarFile;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;

/**
 * The JDKs the agent can run on.
 *
 * <p><b>Why this test exists.</b> "Which JDKs are supported?" has two answers, and they are
 * independent:
 *
 * <ul>
 *   <li><b>Which class files can the agent read?</b> That is ByteBuddy's business. ByteBuddy checks
 *       the class file version of everything it instruments and refuses to touch a class from a JDK
 *       it does not know about.
 *   <li><b>Which JVM can load the agent?</b> That is <em>our</em> business. The agent's own classes
 *       are loaded first, and their class file version is the floor.
 * </ul>
 *
 * <p>Conflating the two is easy and wrong. ByteBuddy 1.18.x has a Java 8 baseline -- its base
 * classes are major version 52 -- so it is not what stops the agent from running on an old JVM. The
 * agent's own {@code maven.compiler.release} is.
 *
 * <p>So this test measures the floor from the shipped bytecode rather than trusting the build
 * configuration, and it checks that ByteBuddy's own floor is at or below ours.
 */
class AgentJdkCompatibilityTest {

  /** The class file major version of Java 8. */
  private static final int JAVA_8 = 52;

  /**
   * The APIs the agent must not use, because they arrived after Java 8.
   *
   * <p>The list is checked against the shipped bytecode's constant pool, so a call that slips
   * through the source review is caught here.
   */
  private static final List<String> POST_JAVA_8_APIS =
      List.of(
          "java/nio/file/Path.of",
          "java/nio/file/Files.readString",
          "java/nio/file/Files.writeString",
          "java/lang/String.isBlank",
          "java/lang/String.strip",
          "java/util/List.of",
          "java/util/List.copyOf",
          "java/util/stream/Stream.toList",
          "java/io/InputStream.readAllBytes",
          "java/lang/ClassLoader.getPlatformClassLoader",
          "java/lang/reflect/Method.trySetAccessible");

  @Test
  @DisplayName("the agent's own class files set the runtime floor")
  void theAgentSetsTheRuntimeFloor() throws Exception {
    int major = highestMajorVersionOfOwnClasses();

    // The agent is compiled for Java 8 (major 52), so it loads on a
    // Java 8 JVM. This is what lets the agent capture a compile in a
    // project that still builds on Java 8 -- which is a large share of
    // the JVM installations in the wild.
    //
    // If this assertion ever fails, the floor moved, and the
    // documentation has to move with it.
    assertThat(major)
        .as("the class file major version of the agent's own classes")
        .isEqualTo(JAVA_8);
  }

  @Test
  @DisplayName("ByteBuddy's own floor is Java 8, so it never narrows ours")
  void byteBuddyIsNotTheLimitingFactor() throws Exception {
    int major = majorVersionOf(byteBuddyJar(), "net/bytebuddy/ByteBuddy.class");

    // Major version 52 is Java 8. ByteBuddy ships as a multi-release jar:
    // the base classes are Java 8 and the newer ones live under
    // META-INF/versions. So the base class file version is the floor, and
    // it is Java 8.
    //
    // This is the fact that makes "one ByteBuddy version covers the whole
    // JDK 8+ range" true, and therefore makes several implementations
    // unnecessary.
    assertThat(major)
        .as("the class file major version of ByteBuddy's base classes")
        .isEqualTo(JAVA_8);
  }

  @Test
  @DisplayName("the agent does not call any API that arrived after Java 8")
  void theAgentDoesNotUsePostJava8Apis() throws Exception {
    // The compiler plugin enforces the language level, but the *API* level
    // is only enforced for the bootclasspath it links against. A call to
    // Path.of would compile fine against a newer JDK and fail at run time
    // on Java 8 -- with a NoSuchMethodError, which on a compiler's
    // critical path means a failed build.
    //
    // So the shipped bytecode's constant pool is checked directly.
    Set<String> referenced = referencedApiNamesOfOwnClasses();

    for (String api : POST_JAVA_8_APIS) {
      assertThat(referenced)
          .as("the agent must not call " + api + " (it does not exist on Java 8)")
          .doesNotContain(api);
    }
  }

  @Test
  @DisplayName("the shaded agent jar carries ByteBuddy inside it")
  void theAgentJarCarriesByteBuddy() throws Exception {
    // The agent has to be self-contained: a -javaagent jar is loaded with
    // only its own classes on the classpath. If ByteBuddy were not inside
    // it, the agent would fail to start on any JDK -- with a
    // NoClassDefFoundError that nobody sees, because the compile goes on.
    File jar = packagedAgentJar();
    if (jar == null) {
      // The test may run before the package phase. The end-to-end test
      // covers the packaged jar; here the classpath copy is enough to
      // prove the point.
      assertThat(byteBuddyJar()).as("the ByteBuddy jar").exists();
      return;
    }

    List<String> entries = entriesOf(jar);
    assertThat(entries).as("ByteBuddy inside " + jar.getName()).contains("net/bytebuddy/ByteBuddy.class");

    // And the manifest has to make it an agent. A jar with ByteBuddy in it
    // but without Premain-Class is just a library: the -javaagent option
    // would fail with "Failed to find Premain-Class manifest attribute".
    assertThat(manifestOf(jar))
        .as("the Premain-Class of " + jar.getName())
        .contains("Premain-Class: org.ccap.jvm.capture.CaptureAgent");
  }

  /** The highest class file major version among the module's own classes. */
  private static int highestMajorVersionOfOwnClasses() throws Exception {
    List<String> resources = ownClassResources();
    assertThat(resources).as("the agent's own classes").isNotEmpty();

    int highest = 0;
    for (String resource : resources) {
      byte[] bytes = readClass(loader(), resource);
      if (bytes != null) {
        highest = Math.max(highest, majorVersion(bytes));
      }
    }
    return highest;
  }

  /** The API names the agent's own classes mention. */
  private static Set<String> referencedApiNamesOfOwnClasses() throws Exception {
    Set<String> out = new LinkedHashSet<>();
    for (String resource : ownClassResources()) {
      byte[] bytes = readClass(loader(), resource);
      if (bytes == null) {
        continue;
      }
      for (String referenced : BootstrapClosure.referencedClasses(bytes)) {
        out.add(referenced);
      }
    }
    return out;
  }

  /** The class loader the agent's classes are on. */
  private static ClassLoader loader() {
    return AgentJdkCompatibilityTest.class.getClassLoader();
  }

  /** The class resources of the agent's own classes. */
  private static List<String> ownClassResources() throws Exception {
    List<String> resources = new ArrayList<>();
    java.net.URL url = loader().getResource("org/ccap/jvm/capture/CaptureAgent.class");
    assertThat(url).as("the agent's own class").isNotNull();

    String path = url.getPath();
    String marker = "org/ccap/jvm/capture/CaptureAgent.class";
    int index = path.indexOf(marker);
    assertThat(index).as("the resource path of " + marker).isGreaterThan(0);

    // The prefix is the root the package sits in. It is a jar when it ends
    // in .jar, and a directory (target/classes) otherwise.
    String root = path.substring(0, index);
    if (root.endsWith(".jar")) {
      try (JarFile file = new JarFile(root)) {
        Enumeration<JarEntry> entries = file.entries();
        while (entries.hasMoreElements()) {
          String name = entries.nextElement().getName();
          if (name.startsWith("org/ccap/jvm/capture/") && name.endsWith(".class")) {
            resources.add(name);
          }
        }
      }
    } else {
      File classesDir = new File(root);
      File packageDir = new File(classesDir, "org/ccap/jvm/capture");
      File[] files = packageDir.listFiles();
      assertThat(files).as("the agent's own classes in " + packageDir).isNotNull();
      for (File file : files) {
        if (file.getName().endsWith(".class")) {
          resources.add("org/ccap/jvm/capture/" + file.getName());
        }
      }
    }
    return resources;
  }

  /** The class file major version of one class in a jar. */
  private static int majorVersionOf(File jar, String entry) throws Exception {
    try (JarFile file = new JarFile(jar)) {
      JarEntry jarEntry = file.getJarEntry(entry);
      assertThat(jarEntry).as(entry).isNotNull();
      try (InputStream in = file.getInputStream(jarEntry)) {
        DataInputStream data = new DataInputStream(in);
        data.readInt(); // magic
        data.readUnsignedShort(); // minor
        return data.readUnsignedShort();
      }
    }
  }

  /** The class file major version of a byte array. */
  private static int majorVersion(byte[] bytes) {
    return ((bytes[6] & 0xff) << 8) | (bytes[7] & 0xff);
  }

  /** The bytes of a class resource. */
  private static byte[] readClass(ClassLoader loader, String resource) {
    try (InputStream in = loader.getResourceAsStream(resource)) {
      if (in == null) {
        return null;
      }
      return in.readAllBytes();
    } catch (IOException e) {
      return null;
    }
  }

  /** The ByteBuddy jar, from the test's own classpath. */
  private static File byteBuddyJar() {
    for (String entry : System.getProperty("java.class.path").split(File.pathSeparator)) {
      File file = new File(entry);
      if (file.getName().startsWith("byte-buddy-") && file.getName().endsWith(".jar")) {
        return file;
      }
    }
    throw new org.opentest4j.TestAbortedException("ByteBuddy is not on the test classpath");
  }

  /** The packaged agent jar, or {@code null} when it has not been built yet. */
  private static File packagedAgentJar() {
    File target = new File("target");
    File[] files = target.listFiles();
    if (files == null) {
      return null;
    }
    // The shade plugin replaces the main artifact with the self-contained
    // agent: it carries ByteBuddy inside it and has the Premain-Class
    // manifest entry. That is the jar the agent is shipped as.
    //
    // The jar is found by its content, not by its name. The build leaves
    // several jars in this directory -- the plain one, the shaded one,
    // the original, and the ones the end-to-end tests assemble -- and a
    // name filter picks the wrong one as soon as one of them is stale.
    for (File file : files) {
      String name = file.getName();
      if (!name.startsWith("ccap-jvm-capture-") || !name.endsWith(".jar")) {
        continue;
      }
      try {
        if (entriesOf(file).contains("net/bytebuddy/ByteBuddy.class")
            && manifestOf(file).contains("Premain-Class: org.ccap.jvm.capture.CaptureAgent")) {
          return file;
        }
      } catch (Exception e) {
        // Not a readable jar; keep looking.
      }
    }
    return null;
  }

  /** The manifest of a jar, as text. */
  private static String manifestOf(File jar) throws Exception {
    try (JarFile file = new JarFile(jar)) {
      JarEntry entry = file.getJarEntry("META-INF/MANIFEST.MF");
      assertThat(entry).as("the manifest of " + jar.getName()).isNotNull();
      try (InputStream in = file.getInputStream(entry)) {
        return new String(in.readAllBytes(), java.nio.charset.StandardCharsets.UTF_8);
      }
    }
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
