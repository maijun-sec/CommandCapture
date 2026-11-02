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
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Runs a command with the capture agent attached to it.
 *
 * <p><b>Why this exists.</b> The agent only captures a compile if it is attached to the JVM that
 * runs the compiler. That is a build configuration problem, and the configuration differs per build
 * tool. Measured on this machine:
 *
 * <ul>
 *   <li>The {@code javac} command line <b>cannot</b> take {@code -javaagent}. It is a program, not a
 *       JVM launcher, and its own option parser rejects the flag with "invalid flag".
 *   <li>{@code mvn} takes it through {@code MAVEN_OPTS}.
 *   <li>{@code gradle}'s {@code org.gradle.jvmargs} does <b>not</b> work: Gradle places the argument
 *       after {@code -cp}, where the JVM treats it as an application argument rather than a JVM
 *       option. It fails with "Unrecognized option: -javaagent=...".
 * </ul>
 *
 * <p><b>Why the environment, not the command line.</b> {@code JAVA_TOOL_OPTIONS} is read by the JVM
 * <em>before</em> it starts, so its position in the command line cannot matter -- and it is honoured
 * by every JVM, whether it was started as {@code java}, {@code javac}, {@code mvn} or {@code gradle}.
 * That makes it the one channel that works for all of them.
 *
 * <p><b>What it does not do.</b> It does not modify the command line at all. The command runs with
 * exactly the arguments it was given, so a build that inspects its own arguments sees what it
 * expects.
 *
 * <p>The agent is appended to an existing {@code JAVA_TOOL_OPTIONS} rather than replacing it, so a
 * value that was already set -- by the user, or by another tool -- keeps working.
 *
 * <p><b>Usage.</b>
 *
 * <pre>
 *   java -jar ccap-jvm-capture.jar javac -cp lib.jar -d out src\/Foo.java
 *   java -jar ccap-jvm-capture.jar mvn -q clean install
 *   java -jar ccap-jvm-capture.jar gradle build
 * </pre>
 */
public final class CaptureLauncher {

  /** The environment variable the JVM reads for extra options. */
  static final String ENV_TOOL_OPTIONS = "JAVA_TOOL_OPTIONS";

  /** The environment variable that names the capture directory. */
  static final String ENV_CAPTURE_DIR = CaptureRecorder.ENV_OUTPUT_DIR;

  /** The system property that names the capture directory. */
  static final String PROP_CAPTURE_DIR = CaptureRecorder.PROP_OUTPUT_DIR;

  /** The agent's own option, which carries the capture directory into the compiler's JVM. */
  static final String AGENT_OPTION = "ccap.capture.dir";

  /** The system property that names the agent jar, when it is not the one this class came from. */
  static final String PROP_AGENT_JAR = "ccap.agent.jar";

  /** The name of the command that is not a compiler at all, such as "mvn" or "gradle". */
  private static final List<String> BUILD_TOOLS =
      Arrays.asList("mvn", "mvnw", "gradle", "gradlew", "ant", "sbt", "bazel", "make", "cmake", "ninja");

  private CaptureLauncher() {}

  /**
   * Run a command with the agent attached.
   *
   * @param args the command and its arguments
   * @throws IOException when the command cannot be started
   * @throws InterruptedException when the wait for the command is interrupted
   */
  public static void main(String[] args) throws IOException, InterruptedException {
    if (args.length == 0) {
      System.err.println("usage: ccap-jvm-capture <command> [arguments...]");
      System.err.println("       ccap-jvm-capture attach <pid>");
      System.err.println();
      System.err.println("Runs the command with the capture agent attached, and writes the");
      System.err.println("captures into the directory named by " + ENV_CAPTURE_DIR + ".");
      System.err.println();
      System.err.println("The agent is attached through " + ENV_TOOL_OPTIONS + ", which every");
      System.err.println("JVM honours -- the javac command line itself cannot take a");
      System.err.println("-javaagent, because it is a program and not a JVM launcher.");
      System.exit(2);
    }

    String captureDir = captureDirectory();
    if (captureDir == null) {
      System.err.println(
          "[ccap-capture] no capture directory: set " + ENV_CAPTURE_DIR + " or " + PROP_CAPTURE_DIR);
      System.exit(2);
    }

    File agent = agentJar();
    if (agent == null) {
      System.err.println("[ccap-capture] cannot find the agent jar");
      System.exit(2);
    }

    List<String> command = new ArrayList<>(Arrays.asList(args));
    ProcessBuilder builder = new ProcessBuilder(command);
    builder.environment().put(ENV_TOOL_OPTIONS, toolOptions(builder.environment(), agent, captureDir));

    // The child's output has to reach the caller's output. A ProcessBuilder
    // gives the child a pipe by default, and a pipe nobody reads is output
    // that is simply lost -- the build would appear to produce nothing at
    // all. So the child's streams are redirected to this process's own.
    builder.redirectInput(ProcessBuilder.Redirect.INHERIT);
    builder.redirectOutput(ProcessBuilder.Redirect.INHERIT);
    builder.redirectError(ProcessBuilder.Redirect.INHERIT);

    Process process = builder.start();
    int exit = process.waitFor();
    System.exit(exit);
  }

  /**
   * The value of {@code JAVA_TOOL_OPTIONS} for the child process.
   *
   * <p>The agent is <b>appended</b> to whatever was there, so an existing value keeps working.
   *
   * @param environment the child's environment
   * @param agent the agent jar
   * @param captureDir the capture directory
   * @return the value to set
   */
  static String toolOptions(java.util.Map<String, String> environment, File agent, String captureDir) {
    String agentOption = "-javaagent:" + agent.getPath() + "=" + AGENT_OPTION + "=" + captureDir;
    String existing = environment.get(ENV_TOOL_OPTIONS);
    if (existing == null || existing.trim().isEmpty()) {
      return agentOption;
    }
    return existing + " " + agentOption;
  }

  /**
   * The capture directory, from the environment or the system properties.
   *
   * @return the directory, or null when neither names one
   */
  static String captureDirectory() {
    String fromEnv = System.getenv(ENV_CAPTURE_DIR);
    if (fromEnv != null && !fromEnv.trim().isEmpty()) {
      return fromEnv;
    }
    String fromProperty = System.getProperty(PROP_CAPTURE_DIR);
    if (fromProperty != null && !fromProperty.trim().isEmpty()) {
      return fromProperty;
    }
    return null;
  }

  /**
   * The agent jar: the jar this class was loaded from.
   *
   * <p>When the class is run from a classes directory rather than a jar -- which is what the tests
   * do -- there is no jar, and the launcher reports that rather than pretending to work.
   *
   * @return the jar, or null
   */
  static File agentJar() {
    // An explicit path wins: it is how the tests point at the agent jar
    // they built, and how a caller points at a jar that is not the one
    // this class was loaded from.
    String configured = System.getProperty(PROP_AGENT_JAR);
    if (configured != null && !configured.trim().isEmpty()) {
      File file = new File(configured);
      return file.isFile() ? file : null;
    }
    try {
      java.net.URL location =
          CaptureLauncher.class.getProtectionDomain().getCodeSource().getLocation();
      File file = new File(location.toURI());
      return file.isFile() && file.getName().endsWith(".jar") ? file : null;
    } catch (Exception e) {
      return null;
    }
  }

  /**
   * Whether the command looks like a build tool rather than a compiler.
   *
   * <p>This is only used for the note the launcher prints. It does not change what is done: the
   * agent is attached the same way for every command, because {@code JAVA_TOOL_OPTIONS} works for
   * all of them.
   *
   * @param command the command name
   * @return true when the command is a build tool
   */
  static boolean isBuildTool(String command) {
    if (command == null) {
      return false;
    }
    String name = new File(command).getName().toLowerCase(java.util.Locale.ROOT);
    if (name.endsWith(".exe")) {
      name = name.substring(0, name.length() - 4);
    }
    if (name.endsWith(".cmd") || name.endsWith(".bat")) {
      name = name.substring(0, name.length() - 4);
    }
    return BUILD_TOOLS.contains(name);
  }
}
