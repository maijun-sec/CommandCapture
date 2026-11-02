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

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Where captured compiles are recorded.
 *
 * <p>The agent runs <em>inside</em> the compiler's JVM, which is a different process from the
 * analyser. So the capture has to travel back out. There are two channels:
 *
 * <ul>
 *   <li>a JSON file written into a directory the analyser watches -- this is what {@link
 *       CaptureRecorder} does, because the recorder is loaded by the bootstrap class loader and
 *       cannot reach this class;
 *   <li>an in-memory list, for the case where the agent and the analyser share a JVM.
 * </ul>
 *
 * <p>The original {@code java-capture} project shipped a separate "collect" module and shelled out
 * to it from the agent. Writing a file directly is simpler and avoids spawning a process from
 * inside the compiler.
 */
public final class CaptureSink {

  /** Environment variable naming the directory captures are written to. */
  public static final String ENV_OUTPUT_DIR = CaptureRecorder.ENV_OUTPUT_DIR;

  private static final CaptureSink INSTANCE = new CaptureSink();

  private final List<CaptureEntry> captures = new CopyOnWriteArrayList<>();

  private volatile Path outputDirectory;

  private CaptureSink() {}

  public static CaptureSink get() {
    return INSTANCE;
  }

  /**
   * Configure the sink from the environment.
   *
   * <p>Reads {@value #ENV_OUTPUT_DIR}; when it is unset the sink keeps captures in memory only.
   */
  public void configureFromEnvironment() {
    String dir = System.getenv(ENV_OUTPUT_DIR);
    if (dir == null || Jdk8.isBlank(dir)) {
      return;
    }
    try {
      Path p = Jdk8.path(dir);
      Files.createDirectories(p);
      this.outputDirectory = p;
    } catch (IOException e) {
      System.err.println(
          "[ccap-capture] could not prepare output directory " + dir + ": " + e.getMessage());
    }
  }

  /** Set the output directory explicitly. */
  public void setOutputDirectory(Path dir) {
    this.outputDirectory = dir;
  }

  public Path outputDirectory() {
    return outputDirectory;
  }

  /**
   * Record one captured compile.
   *
   * @param entry the capture
   */
  public void record(CaptureEntry entry) {
    if (entry == null) {
      return;
    }
    captures.add(entry);

    Path dir = outputDirectory;
    if (dir != null) {
      writeJson(dir, entry);
    }
  }

  /** Every capture recorded so far, in order. */
  public List<CaptureEntry> captures() {
    return new ArrayList<>(captures);
  }

  /** The most recent capture, or {@code null} when nothing was captured. */
  public CaptureEntry latest() {
    return captures.isEmpty() ? null : captures.get(captures.size() - 1);
  }

  /** Discard all recorded captures. */
  public void clear() {
    captures.clear();
  }

  /**
   * Write the capture as JSON.
   *
   * <p>Uses {@link CaptureEntry#toJson()} so the on-disk format is identical no matter which
   * channel produced the capture.
   */
  private void writeJson(Path dir, CaptureEntry entry) {
    try {
      Path file = dir.resolve(entry.compiler() + "-" + System.nanoTime() + ".json");
      Jdk8.writeString(file, entry.toJson(), StandardCharsets.UTF_8);
    } catch (IOException e) {
      System.err.println("[ccap-capture] could not write capture file: " + e.getMessage());
    }
  }
}

