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

import java.util.HashSet;
import java.util.List;
import java.util.Set;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;

/** The declarative entry-point table. */
class CompilerEntryPointsTest {

  @Test
  @DisplayName("all three languages are covered")
  void allLanguagesAreCovered() {
    Set<CompilerOption.Compiler> covered = new HashSet<>();
    for (CompilerEntryPoints.EntryPoint entry : CompilerEntryPoints.all()) {
      covered.add(entry.compiler());
    }
    assertThat(covered)
        .containsExactlyInAnyOrder(
            CompilerOption.Compiler.JAVAC,
            CompilerOption.Compiler.ECJ,
            CompilerOption.Compiler.SJAVAC,
            CompilerOption.Compiler.KOTLIN,
            CompilerOption.Compiler.SCALA3,
            CompilerOption.Compiler.SCALA2);
  }

  @Test
  @DisplayName("every entry point names a compiler that has an option table")
  void everyEntryPointHasAnOptionTable() {
    for (CompilerEntryPoints.EntryPoint entry : CompilerEntryPoints.all()) {
      assertThat(entry.compiler()).as(entry.describe()).isNotNull();
      assertThat(CompilerOption.forCompiler(entry.compiler()))
          .as(entry.describe())
          .isNotEmpty();
    }
  }

  @Test
  @DisplayName("every entry point's argument count is sane")
  void argumentCountsAreSane() {
    for (CompilerEntryPoints.EntryPoint entry : CompilerEntryPoints.all()) {
      assertThat(entry.argumentCount())
          .as(entry.describe())
          .isGreaterThanOrEqualTo(-1);
      if (entry.argumentCount() >= 0) {
        // The options index must be inside the method.
        if (entry.optionsIndex() >= 0) {
          assertThat(entry.optionsIndex())
              .as(entry.describe())
              .isLessThan(entry.argumentCount());
        }
        if (entry.sourcesIndex() >= 0) {
          assertThat(entry.sourcesIndex())
              .as(entry.describe())
              .isLessThan(entry.argumentCount());
        }
      }
    }
  }

  @Test
  @DisplayName("the javac CLI and the javac API are both hooked")
  void javacHasBothPaths() {
    List<String> javacTypes =
        CompilerEntryPoints.forCompiler(CompilerOption.Compiler.JAVAC).stream()
            .map(CompilerEntryPoints.EntryPoint::typeName)
            .distinct()
            .toList();
    // The command-line driver.
    assertThat(javacTypes).contains("com.sun.tools.javac.Main");
    // The javax.tools API, which is what Gradle, Maven and the IDEs use.
    assertThat(javacTypes).contains("com.sun.tools.javac.api.JavacTool");
    // The compiler itself, for the parse and compile steps.
    assertThat(javacTypes).contains("com.sun.tools.javac.main.JavaCompiler");
  }

  @Test
  @DisplayName("the Kotlin entry point is the exec method, which is where the argv is")
  void kotlinEntryPoint() {
    List<CompilerEntryPoints.EntryPoint> kotlin =
        CompilerEntryPoints.forCompiler(CompilerOption.Compiler.KOTLIN);
    assertThat(kotlin).isNotEmpty();
    assertThat(kotlin)
        .anyMatch(
            e ->
                e.typeName().equals("org.jetbrains.kotlin.cli.common.CLICompiler")
                    && e.methodName().equals("exec"));
    // The argv is at argument 1, because exec takes a PrintStream first.
    assertThat(kotlin)
        .filteredOn(e -> e.kind() == CompilerEntryPoints.EntryPoint.Kind.KOTLIN_EXEC)
        .isNotEmpty();
  }

  @Test
  @DisplayName("the Scala entry points are the drivers of both Scala versions")
  void scalaEntryPoints() {
    List<String> scala3Types =
        CompilerEntryPoints.forCompiler(CompilerOption.Compiler.SCALA3).stream()
            .map(CompilerEntryPoints.EntryPoint::typeName)
            .distinct()
            .toList();
    assertThat(scala3Types).contains("dotty.tools.dotc.Main");

    List<String> scala2Types =
        CompilerEntryPoints.forCompiler(CompilerOption.Compiler.SCALA2).stream()
            .map(CompilerEntryPoints.EntryPoint::typeName)
            .distinct()
            .toList();
    assertThat(scala2Types).contains("scala.tools.nsc.Main");
  }

  @Test
  @DisplayName("every language has a hook that reports the compiled sources")
  void everyLanguageHasACompiledSourcesHook() {
    for (CompilerOption.Compiler compiler : CompilerOption.Compiler.values()) {
      // sjavac is the exception, and for a reason: it drives javac rather
      // than compiling anything itself, so its sources are seen by javac's
      // hooks. See sjavacDelegatesToJavac below.
      if (compiler == CompilerOption.Compiler.SJAVAC) {
        continue;
      }
      // The kinds that report sources. Each language picks the one that
      // fits its compiler:
      //   PARSED_SOURCE       -- javac's parse step
      //   COMPILED_SOURCES    -- Scala's "here is what I compiled"
      //   KOTLIN_SOURCE_FILE  -- Kotlin's PSI factory, which every parsed
      //                          source passes through
      //   SCALA3_COMPILE_END  -- Scala 3's compile step
      assertThat(
              CompilerEntryPoints.forCompiler(compiler).stream()
                  .anyMatch(
                      e ->
                          e.kind() == CompilerEntryPoints.EntryPoint.Kind.COMPILED_SOURCES
                              || e.kind() == CompilerEntryPoints.EntryPoint.Kind.PARSED_SOURCE
                              || e.kind() == CompilerEntryPoints.EntryPoint.Kind.KOTLIN_SOURCE_FILE
                              || e.kind()
                                  == CompilerEntryPoints.EntryPoint.Kind.SCALA3_COMPILE_END))
          .as(compiler.toString())
          .isTrue();
    }
  }

  @Test
  @DisplayName("sjavac's sources are seen through javac, not through sjavac")
  void sjavacDelegatesToJavac() {
    // sjavac is a JDK-internal tool that was removed after JDK 11, and it
    // does not compile anything itself: it drives javac. So the sources it
    // compiles are seen by javac's parse hook.
    //
    // This is why sjavac needs no hook of its own for the sources -- and it
    // is worth stating, because "sjavac has no compiled-sources hook" looks
    // like a gap and is not one.
    assertThat(
            CompilerEntryPoints.forCompiler(CompilerOption.Compiler.SJAVAC).stream()
                .noneMatch(
                    e ->
                        e.kind() == CompilerEntryPoints.EntryPoint.Kind.COMPILED_SOURCES
                            || e.kind() == CompilerEntryPoints.EntryPoint.Kind.PARSED_SOURCE))
        .isTrue();

    // The javac parse hook is what sees the sources.
    assertThat(
            CompilerEntryPoints.forCompiler(CompilerOption.Compiler.JAVAC).stream()
                .anyMatch(
                    e ->
                        e.typeName().equals("com.sun.tools.javac.main.JavaCompiler")
                            && e.methodName().equals("parse")))
        .isTrue();
  }

  @Test
  @DisplayName("the type list has no duplicates")
  void typesAreUnique() {
    List<String> types = CompilerEntryPoints.types();
    assertThat(types).doesNotHaveDuplicates();
  }
}
