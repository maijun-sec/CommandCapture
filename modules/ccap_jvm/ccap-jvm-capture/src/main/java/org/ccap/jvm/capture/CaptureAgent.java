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

import java.lang.instrument.Instrumentation;
import java.util.List;

/**
 * A ByteBuddy agent that captures the real compile performed by a JVM-language compiler.
 *
 * <p>Some projects cannot be introspected statically: the classpath may be assembled by a script, a
 * plugin, or an IDE. For those, the ground truth is the compile the compiler actually performs.
 * This agent hooks the compiler's entry points and records it.
 *
 * <h2>Which entry points</h2>
 *
 * <p>See {@link CompilerEntryPoints} for the list. The short version: a "compiler command line" is
 * many different methods depending on how the compiler was reached <em>and on which language it
 * is</em>, and hooking only one of them means the capture silently misses every build that takes a
 * different path.
 *
 * <h2>How</h2>
 *
 * <p>ByteBuddy's {@link net.bytebuddy.asm.Advice} does the instrumentation. Advice is
 * <em>inlining</em>: the hook body is copied into the target method, so it needs no super method
 * and no delegation target. That is what makes it work on a plain static method like {@code
 * Main#compile(String[])}.
 *
 * <p>The original project this descends from used btrace, which needs elevated privileges and a
 * native agent. ByteBuddy needs neither: it attaches through the standard {@code
 * java.lang.instrument} API, so the capture runs as an ordinary user process.
 *
 * <h2>Usage</h2>
 *
 * <pre>
 *   java -javaagent:ccap-jvm-capture.jar \
 *        -DCCAP_CAPTURE_DIR=/tmp/captures \
 *        -cp ... com.sun.tools.javac.Main -d out Foo.java
 * </pre>
 *
 * <p>or, to attach to a running compiler:
 *
 * <pre>
 *   java -cp ccap-jvm-capture.jar org.ccap.jvm.capture.CaptureAgent attach &lt;pid&gt;
 * </pre>
 */
public final class CaptureAgent {

  /**
   * The classes that go into the bootstrap jar.
   *
   * <p><b>This list is computed, not written.</b> It used to be a hand-maintained array, and it
   * was wrong: {@code JvmOptions$1} -- the synthetic switch-map class the compiler generates for a
   * {@code switch} over an enum -- was missing, and the recorder failed with a {@code
   * NoClassDefFoundError} that the advice swallowed. The compile succeeded, the agent printed
   * "agent installed", and no capture ever appeared.
   *
   * <p>Nobody thinks about {@code Foo$1} when they write {@code Foo}, so nobody remembers to add
   * it. {@link BootstrapClosure} reads the constant pool of the shipped classes instead, and
   * follows every reference within this package. A class that is added to the recorder cannot be
   * forgotten: the list comes from the bytecode that is actually shipped.
   */
  private static final List<String> BOOTSTRAP_CLASSES =
      BootstrapClosure.compute(CaptureAgent.class.getClassLoader());

  private CaptureAgent() {}

  /**
   * Agent entry point for {@code -javaagent:}.
   *
   * @param agentArgs agent arguments
   * @param inst the instrumentation interface
   */
  public static void premain(String agentArgs, Instrumentation inst) {
    install(inst, agentArgs);
  }

  /**
   * Agent entry point for dynamic attach.
   *
   * @param agentArgs agent arguments
   * @param inst the instrumentation interface
   */
  public static void agentmain(String agentArgs, Instrumentation inst) {
    install(inst, agentArgs);
  }

  /**
   * Install the instrumentation.
   *
   * @param inst the instrumentation interface
   */
  public static void install(Instrumentation inst) {
    install(inst, null);
  }

  /**
   * Install the instrumentation.
   *
   * @param inst the instrumentation interface
   * @param agentArgs the agent's own arguments, or {@code null}
   */
  public static void install(Instrumentation inst, String agentArgs) {
    applyAgentArguments(agentArgs);
    allowNewerClassFiles();
    CaptureSink.get().configureFromEnvironment();
    injectRecorder(inst);
    CompilerHooks.install(inst);
    report("agent installed, hooking " + CompilerEntryPoints.all().size() + " entry point(s)");
  }

  /**
   * Allow ByteBuddy to instrument classes from a JDK it does not know yet.
   *
   * <p><b>Why this is needed.</b> ByteBuddy checks the class file version of everything it
   * instruments, and refuses to touch a class from a newer JDK than the one it was built against.
   * The refusal is not a technical limit -- the instrumentation it does is version independent -- but
   * a guard against surprises in a new class file format.
   *
   * <p>So on a JDK newer than ByteBuddy's, every hook would fail and the capture would be empty
   * -- with the failure printed as an instrumentation error, but the compile itself perfectly
   * fine. The agent would look like it worked.
   *
   * <p>The flag turns the refusal into a warning. The capture then works on a JDK that came out
   * after the agent was built, which is what a user with a new JDK needs.
   *
   * <p>An explicit setting by the user is respected: if they set the property themselves, they
   * know what they are doing.
   */
  private static void allowNewerClassFiles() {
    String key = "net.bytebuddy.experimental";
    if (System.getProperty(key) == null) {
      System.setProperty(key, "true");
    }
  }

  /**
   * Apply the agent's own arguments as system properties.
   *
   * <p><b>Why the agent needs its own arguments.</b> Gradle does not forward system properties to
   * the JVM it builds in. Measured on Gradle 9.2.0: {@code org.gradle.jvmargs} keeps the {@code
   * -javaagent} and drops every {@code -D}; {@code systemProp.} in {@code gradle.properties} does
   * not reach the forked daemon either. So a system property is not a way to configure the agent
   * inside Gradle.
   *
   * <p>The agent's own arguments are. {@code -javaagent:jar=key=value,key2=value2} hands them to
   * {@code premain}, and {@code -javaagent} is the one JVM argument Gradle does forward. So the
   * agent's arguments become system properties, which makes the agent configurable inside Gradle
   * without a system property ever being set.
   *
   * @param agentArgs the agent's arguments, or {@code null}
   */
  static void applyAgentArguments(String agentArgs) {
    if (agentArgs == null || Jdk8.isBlank(agentArgs)) {
      return;
    }
    for (String pair : agentArgs.split(",")) {
      int equals = pair.indexOf('=');
      if (equals <= 0) {
        continue;
      }
      String key = pair.substring(0, equals).trim();
      String value = pair.substring(equals + 1).trim();
      if (key.isEmpty()) {
        continue;
      }
      if (System.getProperty(key) == null) {
        // An explicit system property still wins: it was set deliberately.
        System.setProperty(key, value);
      }
    }
  }

  /**
   * Make the recorder visible to the compiler's own classes.
   *
   * <p>The instrumented bytecode lives in {@code com.sun.tools.javac.Main}, which belongs to the
   * {@code jdk.compiler} module and is loaded by the application class loader. The agent jar,
   * meanwhile, is only visible to the agent's own class loader -- so a direct call from the
   * compiler's code to the recorder would fail with {@code NoClassDefFoundError}.
   *
   * <p>Appending a small jar to the <b>bootstrap</b> class loader solves it: the bootstrap loader
   * is the ancestor of every other loader, so the recorder is reachable from the compiler's code,
   * from the application, and from the agent alike.
   */
  private static void injectRecorder(Instrumentation inst) {
    try {
      java.io.File jar = buildRecorderJar();
      if (jar == null) {
        report("could not build the recorder jar, the capture may fail");
        return;
      }
      inst.appendToBootstrapClassLoaderSearch(new java.util.jar.JarFile(jar));
    } catch (Exception e) {
      report("could not inject the recorder class: " + e.getMessage());
    }
  }

  /**
   * Build a jar containing the recorder and everything it needs, for the bootstrap loader.
   *
   * <p>Only the recorder's own classes go in: they are the ones the instrumented bytecode calls,
   * and they are the ones that have to resolve on the bootstrap loader. The recorder must not
   * reference anything outside the bootstrap loader, which is why it has no logging and no
   * dependency on the rest of the module.
   *
   * @return the jar file, or {@code null} when it could not be built
   */
  private static java.io.File buildRecorderJar() {
    try {
      java.io.File jar = java.io.File.createTempFile("ccap-capture-recorder", ".jar");
      jar.deleteOnExit();
      try (java.util.jar.JarOutputStream out =
          new java.util.jar.JarOutputStream(new java.io.FileOutputStream(jar))) {
        for (String classResource : bootstrapJarClasses()) {
          byte[] bytes = readClassBytes(classResource);
          if (bytes == null) {
            report(classResource + " is missing from the agent jar");
            continue;
          }
          out.putNextEntry(new java.util.zip.ZipEntry(classResource));
          out.write(bytes);
          out.closeEntry();
        }
      }
      return jar;
    } catch (Exception e) {
      return null;
    }
  }

  /**
   * The recorder jar, for the tests.
   *
   * <p>The tests need to see what goes into the bootstrap loader: a class that is missing from it
   * fails silently at run time, and the only way to see that is to look at the jar.
   */
  static java.io.File buildRecorderJarForTest() {
    return buildRecorderJar();
  }

  /**
   * The class resources that go into the bootstrap jar.
   *
   * <p>Each one is looked up in the agent's own class loader. A class that cannot be found is
   * reported rather than silently skipped, because a missing class in this jar means the capture
   * fails silently at run time.
   */
  static java.util.List<String> bootstrapJarClasses() {
    // The closure already yields the class resources (with the .class
    // suffix), so they are used as they are.
    return Jdk8.copyOf(BOOTSTRAP_CLASSES);
  }
  /** The bytes of one class, or {@code null} when it is not on the agent's classpath. */
  private static byte[] readClassBytes(String classResource) {
    try (java.io.InputStream in =
        CaptureAgent.class.getClassLoader().getResourceAsStream(classResource)) {
      if (in == null) {
        return null;
      }
      return Jdk8.readAllBytes(in);
    } catch (Exception e) {
      return null;
    }
  }

  /**
   * Attach this agent to a running JVM.
   *
   * <p>The target is identified by its process id; the agent jar is located from this class's own
   * code source, so the caller only needs the pid.
   *
   * @param args the target process id
   * @throws Exception if the attachment fails
   */
  public static void main(String[] args) throws Exception {
    if (args == null || args.length == 0) {
      System.err.println("usage: CaptureAgent attach <pid>");
      System.err.println("  attaches the JVM compile capture agent to the given process");
      System.exit(2);
    }
    long pid = Long.parseLong(args[0]);

    java.net.URI location =
        CaptureAgent.class.getProtectionDomain().getCodeSource().getLocation().toURI();
    java.io.File agentJar = new java.io.File(location);
    if (!agentJar.isFile()) {
      System.err.println("Capture: cannot locate the agent jar at " + agentJar);
      System.exit(2);
    }

    net.bytebuddy.agent.ByteBuddyAgent.attach(agentJar, Long.toString(pid));
    System.out.println("Capture: attached to pid " + pid);
  }

  /** The entry points the agent hooks. Exposed for the tests and for documentation. */
  public static List<CompilerEntryPoints.EntryPoint> entryPoints() {
    return CompilerEntryPoints.all();
  }

  /**
   * Report a message.
   *
   * <p><b>Why not a logging framework.</b> The agent runs inside an arbitrary compiler JVM, which
   * usually has no logging provider -- and a message that goes nowhere is a message nobody reads.
   * The compiler's own stderr is always there, and it is where the user is looking when a compile
   * goes wrong.
   */
  private static void report(String message) {
    System.err.println("[ccap-capture] " + message);
  }
}

