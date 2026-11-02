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

/**
 * The jar's entry point.
 *
 * <p><b>Why there is a dispatcher.</b> The jar does two different things, and they want different
 * invocations:
 *
 * <ul>
 *   <li>It is an <b>agent</b>: {@code -javaagent:ccap-jvm-capture.jar} makes the JVM call
 *       {@link CaptureAgent#premain}. That path is fixed by the JVM and cannot be a choice.
 *   <li>It is a <b>launcher</b>: {@code java -jar ccap-jvm-capture.jar javac ...} runs a command
 *       with the agent attached to it.
 * </ul>
 *
 * <p>The manifest has one {@code Main-Class}, so it points here, and this decides which of the two
 * the caller meant.
 *
 * <p>The rule is simple: a first argument of {@code attach} means the attach command, anything else
 * means the launcher. A process id cannot be a command name, so the two cannot be confused.
 */
public final class Main {

  /** The argument that selects the attach command. */
  static final String ATTACH = "attach";

  private Main() {}

  /**
   * Run the jar.
   *
   * @param args the arguments
   * @throws Exception when the command fails
   */
  public static void main(String[] args) throws Exception {
    if (args.length > 0 && ATTACH.equals(args[0])) {
      String[] rest = new String[args.length - 1];
      System.arraycopy(args, 1, rest, 0, rest.length);
      CaptureAgent.main(rest);
      return;
    }
    CaptureLauncher.main(args);
  }
}
