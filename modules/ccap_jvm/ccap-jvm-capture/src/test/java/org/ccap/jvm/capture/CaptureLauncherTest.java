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

import java.io.File;
import java.util.HashMap;
import java.util.Map;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;

/**
 * How the launcher attaches the agent to a child process.
 *
 * <p>The rule under test is the one that was measured rather than assumed: the agent goes into the
 * environment, not into the command line.
 */
class CaptureLauncherTest {

  @Test
  @DisplayName("the agent goes into JAVA_TOOL_OPTIONS, not into the command line")
  void theAgentGoesIntoTheEnvironment() {
    Map<String, String> environment = new HashMap<>();
    File agent = new File("/opt/ccap/ccap-jvm-capture.jar");

    String options = CaptureLauncher.toolOptions(environment, agent, "/tmp/captures");

    assertThat(options).startsWith("-javaagent:");
    assertThat(options).contains(agent.getPath());
    // The capture directory travels as the agent's own argument, because
    // that is the only channel that survives into the compiler's JVM.
    assertThat(options).contains("ccap.capture.dir=/tmp/captures");
  }

  @Test
  @DisplayName("an existing JAVA_TOOL_OPTIONS is kept, not replaced")
  void anExistingValueIsKept() {
    Map<String, String> environment = new HashMap<>();
    environment.put(CaptureLauncher.ENV_TOOL_OPTIONS, "-Xmx512m -Dfoo=bar");
    File agent = new File("/opt/ccap/ccap-jvm-capture.jar");

    String options = CaptureLauncher.toolOptions(environment, agent, "/tmp/captures");

    assertThat(options).startsWith("-Xmx512m -Dfoo=bar ");
    assertThat(options).contains("-javaagent:");
  }

  @Test
  @DisplayName("an empty JAVA_TOOL_OPTIONS is treated as absent")
  void anEmptyValueIsTreatedAsAbsent() {
    Map<String, String> environment = new HashMap<>();
    environment.put(CaptureLauncher.ENV_TOOL_OPTIONS, "   ");
    File agent = new File("/opt/ccap/ccap-jvm-capture.jar");

    String options = CaptureLauncher.toolOptions(environment, agent, "/tmp/captures");

    assertThat(options).startsWith("-javaagent:");
    assertThat(options).doesNotContain("   ");
  }

  @Test
  @DisplayName("the build tools are recognised by their file name")
  void theBuildToolsAreRecognised() {
    assertThat(CaptureLauncher.isBuildTool("mvn")).isTrue();
    assertThat(CaptureLauncher.isBuildTool("mvnw")).isTrue();
    assertThat(CaptureLauncher.isBuildTool("gradle")).isTrue();
    assertThat(CaptureLauncher.isBuildTool("gradlew")).isTrue();
    // The real command on Windows carries an extension.
    assertThat(CaptureLauncher.isBuildTool("C:\\tools\\mvn.cmd")).isTrue();
    assertThat(CaptureLauncher.isBuildTool("C:\\tools\\gradle.bat")).isTrue();
    assertThat(CaptureLauncher.isBuildTool("C:\\jdk\\bin\\java.exe")).isFalse();
    assertThat(CaptureLauncher.isBuildTool("javac")).isFalse();
    assertThat(CaptureLauncher.isBuildTool(null)).isFalse();
  }
}
