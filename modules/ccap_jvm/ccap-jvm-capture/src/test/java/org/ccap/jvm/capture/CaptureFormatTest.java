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

import java.nio.file.Path;
import java.util.List;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

/** The capture format, written and read back. */
class CaptureFormatTest {

  @Test
  @DisplayName("a capture survives the round trip")
  void roundTrip(@TempDir Path dir) throws Exception {
    CaptureEntry original =
        new CaptureEntry(
            "cli",
            "kotlin",
            "kotlin",
            List.of("-classpath", "a.jar", "-d", "out", "Main.kt"),
            "UTF-8",
            "2.0",
            Path.of("out"),
            List.of(Path.of("a.jar")),
            List.of(),
            List.of(),
            List.of(),
            List.of(),
            List.of(Path.of("Main.kt")),
            List.of(),
            List.of(),
            List.of(),
            false,
            true,
            "25.0.1",
            "/opt/jdk",
            "/work");

    Path file = dir.resolve("kotlin-1.json");
    java.nio.file.Files.writeString(file, original.toJson());

    CaptureEntry read = CaptureReader.read(file);
    assertThat(read).isNotNull();
    assertThat(read.compiler()).isEqualTo("kotlin");
    assertThat(read.language()).isEqualTo("kotlin");
    assertThat(read.argv()).containsExactly("-classpath", "a.jar", "-d", "out", "Main.kt");
    assertThat(read.encoding()).isEqualTo("UTF-8");
    assertThat(read.languageLevel()).isEqualTo("2.0");
    assertThat(read.outputDirectory()).isEqualTo(Path.of("out"));
    assertThat(read.classpath()).containsExactly(Path.of("a.jar"));
    assertThat(read.sourceFiles()).containsExactly(Path.of("Main.kt"));
    assertThat(read.compilation()).isTrue();
    assertThat(read.preview()).isFalse();
  }

  @Test
  @DisplayName("the language is recorded")
  void languageIsRecorded(@TempDir Path dir) {
    CaptureEntry java =
        new CaptureEntry(
            "cli", "javac", "java", List.of("Foo.java"), null, null, null,
            List.of(), List.of(), List.of(), List.of(), List.of(),
            List.of(Path.of("Foo.java")), List.of(), List.of(), List.of(),
            false, true, "17", "/jdk", "/work");
    CaptureEntry scala =
        new CaptureEntry(
            "cli", "scala3", "scala", List.of("Main.scala"), null, null, null,
            List.of(), List.of(), List.of(), List.of(), List.of(),
            List.of(Path.of("Main.scala")), List.of(), List.of(), List.of(),
            false, true, "17", "/jdk", "/work");
    assertThat(java.language()).isEqualTo("java");
    assertThat(scala.language()).isEqualTo("scala");
  }

  @Test
  @DisplayName("the reader picks up the capture files of every language")
  void readerPicksUpEveryLanguage(@TempDir Path dir) throws Exception {
    for (String prefix : List.of("javac", "kotlin", "scala3", "scala2", "ecj", "sjavac")) {
      java.nio.file.Files.writeString(
          dir.resolve(prefix + "-1.json"),
          """
          {
            "api": "cli",
            "compiler": "%s",
            "language": "java",
            "argv": ["Foo.java"],
            "encoding": null,
            "languageLevel": null,
            "outputDirectory": null,
            "classpath": [],
            "sourcePath": [],
            "modulePath": [],
            "processorPath": [],
            "bootClassPath": [],
            "sourceFiles": ["Foo.java"],
            "addModules": [],
            "addExports": [],
            "addReads": [],
            "preview": false,
            "compilation": true,
            "javaVersion": "17",
            "javaHome": "/jdk",
            "workingDirectory": "/work"
          }
          """
              .formatted(prefix));
    }
    // A file that is not a capture must be ignored.
    java.nio.file.Files.writeString(dir.resolve("notes.txt"), "hello");
    java.nio.file.Files.writeString(dir.resolve("other.json"), "{}");

    List<CaptureEntry> all = CaptureReader.readAll(dir);
    assertThat(all).hasSize(6);
    assertThat(all).extracting(CaptureEntry::compiler)
        .containsExactlyInAnyOrder("javac", "kotlin", "scala3", "scala2", "ecj", "sjavac");
  }

  @Test
  @DisplayName("the reader can filter by language")
  void readerFiltersByLanguage(@TempDir Path dir) throws Exception {
    java.nio.file.Files.writeString(
        dir.resolve("javac-1.json"),
        "{\"compiler\":\"javac\",\"language\":\"java\",\"sourceFiles\":[\"A.java\"]}");
    java.nio.file.Files.writeString(
        dir.resolve("kotlin-1.json"),
        "{\"compiler\":\"kotlin\",\"language\":\"kotlin\",\"sourceFiles\":[\"A.kt\"]}");

    assertThat(CaptureReader.readAll(dir, "java")).hasSize(1);
    assertThat(CaptureReader.readAll(dir, "kotlin")).hasSize(1);
    assertThat(CaptureReader.readAll(dir, "scala")).isEmpty();
  }

  @Test
  @DisplayName("a capture written before a field existed still parses")
  void olderCapturesStillParse() {
    // An unknown key is ignored and a missing key yields null.
    CaptureEntry entry =
        CaptureReader.parse(
            "{\"api\":\"cli\",\"compiler\":\"javac\",\"argv\":[\"A.java\"],\"sourceFiles\":[\"A.java\"]}");
    assertThat(entry).isNotNull();
    assertThat(entry.compiler()).isEqualTo("javac");
    assertThat(entry.language()).isNull();
    assertThat(entry.argv()).containsExactly("A.java");
  }

  @Test
  @DisplayName("two views of one compile have the same identity")
  void identityIsStableAcrossViews() {
    CaptureEntry a =
        new CaptureEntry(
            "cli", "javac", "java", List.of("A.java"), null, null, Path.of("out"),
            List.of(Path.of("cp.jar")), List.of(), List.of(), List.of(), List.of(),
            List.of(Path.of("A.java")), List.of(), List.of(), List.of(),
            false, true, "17", "/jdk", "/work");
    CaptureEntry b =
        new CaptureEntry(
            "api", "javac", "java", List.of("-cp", "cp.jar"), null, null, Path.of("out"),
            List.of(Path.of("cp.jar")), List.of(), List.of(), List.of(), List.of(),
            List.of(Path.of("A.java")), List.of(), List.of(), List.of(),
            false, true, "17", "/jdk", "/work");

    assertThat(a.sameCompileAs(b)).isTrue();
  }

  @Test
  @DisplayName("two different compiles have different identities")
  void identityDistinguishesCompiles() {
    CaptureEntry a =
        new CaptureEntry(
            "cli", "javac", "java", List.of("A.java"), null, null, Path.of("out"),
            List.of(), List.of(), List.of(), List.of(), List.of(),
            List.of(Path.of("A.java")), List.of(), List.of(), List.of(),
            false, true, "17", "/jdk", "/work");
    CaptureEntry b =
        new CaptureEntry(
            "cli", "javac", "java", List.of("A.java"), null, null, Path.of("out2"),
            List.of(), List.of(), List.of(), List.of(), List.of(),
            List.of(Path.of("A.java")), List.of(), List.of(), List.of(),
            false, true, "17", "/jdk", "/work");

    assertThat(a.sameCompileAs(b)).isFalse();
  }

  @Test
  @DisplayName("the language is part of the identity")
  void languageIsPartOfTheIdentity() {
    CaptureEntry java =
        new CaptureEntry(
            "cli", "javac", "java", List.of("A"), null, null, Path.of("out"),
            List.of(), List.of(), List.of(), List.of(), List.of(),
            List.of(Path.of("A")), List.of(), List.of(), List.of(),
            false, true, "17", "/jdk", "/work");
    CaptureEntry kotlin =
        new CaptureEntry(
            "cli", "kotlin", "kotlin", List.of("A"), null, null, Path.of("out"),
            List.of(), List.of(), List.of(), List.of(), List.of(),
            List.of(Path.of("A")), List.of(), List.of(), List.of(),
            false, true, "17", "/jdk", "/work");

    assertThat(java.sameCompileAs(kotlin)).isFalse();
  }
}
