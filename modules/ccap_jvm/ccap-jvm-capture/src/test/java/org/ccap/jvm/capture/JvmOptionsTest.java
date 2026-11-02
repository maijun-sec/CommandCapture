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
import org.junit.jupiter.api.Nested;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

/** The argv parser, one language at a time. */
class JvmOptionsTest {

  @Nested
  @DisplayName("Java")
  class Java {

    @Test
    @DisplayName("the classpath is recovered")
    void classpath() {
      JvmOptions options =
          JvmOptions.parse(
              List.of("-cp", "a.jar" + sep() + "b.jar", "-d", "out", "Foo.java"),
              CompilerOption.Compiler.JAVAC);
      assertThat(options.classpath()).hasSize(2);
      assertThat(options.outputDirectory()).isEqualTo(Path.of("out"));
      assertThat(options.sourceFiles()).containsExactly(Path.of("Foo.java"));
    }

    @Test
    @DisplayName("the language level falls back the way javac's does")
    void languageLevelFallback() {
      assertThat(
              JvmOptions.parse(List.of("--release", "17"), CompilerOption.Compiler.JAVAC)
                  .languageLevel())
          .isEqualTo("17");
      assertThat(
              JvmOptions.parse(List.of("-source", "11"), CompilerOption.Compiler.JAVAC)
                  .languageLevel())
          .isEqualTo("11");
      assertThat(
              JvmOptions.parse(List.of("-target", "8"), CompilerOption.Compiler.JAVAC)
                  .languageLevel())
          .isEqualTo("8");
      // --release wins over -source.
      assertThat(
              JvmOptions.parse(
                      List.of("--release", "17", "-source", "11"), CompilerOption.Compiler.JAVAC)
                  .languageLevel())
          .isEqualTo("17");
    }

    @Test
    @DisplayName("only the last classpath counts")
    void classpathIsLastWins() {
      JvmOptions options =
          JvmOptions.parse(
              List.of("-cp", "first.jar", "-cp", "second.jar"), CompilerOption.Compiler.JAVAC);
      assertThat(options.classpath()).hasSize(1);
      assertThat(options.classpath().get(0).toString()).isEqualTo("second.jar");
    }

    @Test
    @DisplayName("every --add-modules occurrence applies")
    void addModulesIsAdditive() {
      JvmOptions options =
          JvmOptions.parse(
              List.of("--add-modules", "a", "--add-modules", "b"), CompilerOption.Compiler.JAVAC);
      assertThat(options.addModules()).containsExactly("a", "b");
    }

    @Test
    @DisplayName("a jar is not a source file")
    void jarIsNotASource() {
      JvmOptions options =
          JvmOptions.parse(List.of("-cp", "lib.jar", "Foo.java"), CompilerOption.Compiler.JAVAC);
      assertThat(options.sourceFiles()).containsExactly(Path.of("Foo.java"));
    }

    @Test
    @DisplayName("an option's value is not mistaken for a source file")
    void optionValueIsNotASource() {
      // The option table consumes the values, so only a positional argument
      // reaches the source branch.
      JvmOptions options =
          JvmOptions.parse(
              List.of("--processor-path", "some/where.jar", "Foo.java"),
              CompilerOption.Compiler.JAVAC);
      assertThat(options.processorPath()).hasSize(1);
      assertThat(options.sourceFiles()).containsExactly(Path.of("Foo.java"));
    }

    @Test
    @DisplayName("-Xprint means no compilation happened")
    void notACompilation() {
      assertThat(
              JvmOptions.parse(List.of("-Xprint", "Foo.java"), CompilerOption.Compiler.JAVAC)
                  .isCompilation())
          .isFalse();
      assertThat(
              JvmOptions.parse(List.of("Foo.java"), CompilerOption.Compiler.JAVAC).isCompilation())
          .isTrue();
    }

    @Test
    @DisplayName("ECJ's classpath spelling is understood")
    void ecjClasspath() {
      JvmOptions options =
          JvmOptions.parse(List.of("--class-path", "a.jar"), CompilerOption.Compiler.ECJ);
      assertThat(options.classpath()).hasSize(1);
    }

    @Test
    @DisplayName("an argument file is expanded")
    void argumentFile(@TempDir Path dir) throws Exception {
      Path args = dir.resolve("args.txt");
      java.nio.file.Files.writeString(args, "-cp a.jar -d out Foo.java");
      JvmOptions options =
          JvmOptions.parse(
              List.of("@" + args.toString()), CompilerOption.Compiler.JAVAC);
      assertThat(options.classpath()).hasSize(1);
      assertThat(options.outputDirectory()).isEqualTo(Path.of("out"));
      assertThat(options.sourceFiles()).containsExactly(Path.of("Foo.java"));
    }
  }

  @Nested
  @DisplayName("Kotlin")
  class Kotlin {

    @Test
    @DisplayName("the classpath and the output directory are recovered")
    void classpathAndOutput() {
      JvmOptions options =
          JvmOptions.parse(
              List.of("-classpath", "a.jar", "-d", "out", "Main.kt"),
              CompilerOption.Compiler.KOTLIN);
      assertThat(options.classpath()).hasSize(1);
      assertThat(options.outputDirectory()).isEqualTo(Path.of("out"));
      assertThat(options.sourceFiles()).containsExactly(Path.of("Main.kt"));
    }

    @Test
    @DisplayName("-source is a source root, not a language level")
    void sourceIsASourceRoot() {
      JvmOptions options =
          JvmOptions.parse(
              List.of("-source", "src/main/kotlin", "Main.kt"), CompilerOption.Compiler.KOTLIN);
      assertThat(options.sourcePath()).hasSize(1);
      // The path is normalized by the platform, so the separators are the
      // platform's.
      assertThat(options.sourcePath().get(0).toString().replace('\\', '/'))
          .isEqualTo("src/main/kotlin");
      // The language level is NOT the source root.
      assertThat(options.languageLevel()).isNull();
    }

    @Test
    @DisplayName("the language version is the language level")
    void languageVersion() {
      JvmOptions options =
          JvmOptions.parse(
              List.of("-language-version", "2.0", "Main.kt"), CompilerOption.Compiler.KOTLIN);
      assertThat(options.languageLevel()).isEqualTo("2.0");
    }

    @Test
    @DisplayName("the JVM target becomes the language level when there is none")
    void jvmTarget() {
      JvmOptions options =
          JvmOptions.parse(List.of("-jvm-target", "17", "Main.kt"), CompilerOption.Compiler.KOTLIN);
      assertThat(options.languageLevel()).isEqualTo("17");
    }

    @Test
    @DisplayName("a .kts script is a source file")
    void scriptIsASource() {
      JvmOptions options =
          JvmOptions.parse(List.of("build.gradle.kts"), CompilerOption.Compiler.KOTLIN);
      assertThat(options.sourceFiles()).containsExactly(Path.of("build.gradle.kts"));
    }

    @Test
    @DisplayName("a .java file is not a Kotlin source")
    void javaIsNotAKotlinSource() {
      JvmOptions options =
          JvmOptions.parse(List.of("Foo.java"), CompilerOption.Compiler.KOTLIN);
      assertThat(options.sourceFiles()).isEmpty();
    }

    @Test
    @DisplayName("an argument file is NOT expanded -- Kotlin has no such convention")
    void noArgumentFiles() {
      // Kotlin has no @file convention. Expanding one would swallow an
      // argument that merely starts with "@".
      JvmOptions options =
          JvmOptions.parse(
              List.of("@some-file.txt"), CompilerOption.Compiler.KOTLIN);
      assertThat(options.sourceFiles()).isEmpty();
      assertThat(options.rawArguments()).containsExactly("@some-file.txt");
    }
  }

  @Nested
  @DisplayName("Scala")
  class Scala {

    @Test
    @DisplayName("the classpath and the output directory are recovered")
    void classpathAndOutput() {
      JvmOptions options =
          JvmOptions.parse(
              List.of("-classpath", "a.jar", "-d", "out", "Main.scala"),
              CompilerOption.Compiler.SCALA3);
      assertThat(options.classpath()).hasSize(1);
      assertThat(options.outputDirectory()).isEqualTo(Path.of("out"));
      assertThat(options.sourceFiles()).containsExactly(Path.of("Main.scala"));
    }

    @Test
    @DisplayName("several source paths accumulate")
    void sourcePathsAccumulate() {
      JvmOptions options =
          JvmOptions.parse(
              List.of("-sourcepath", "src/a", "-sourcepath", "src/b", "Main.scala"),
              CompilerOption.Compiler.SCALA3);
      assertThat(options.sourcePath()).hasSize(2);
    }

    @Test
    @DisplayName("the source version is the language level")
    void sourceVersion() {
      JvmOptions options =
          JvmOptions.parse(List.of("-source:3.0", "Main.scala"), CompilerOption.Compiler.SCALA3);
      assertThat(options.languageLevel()).isEqualTo("3.0");
    }

    @Test
    @DisplayName("Scala 2's own options are understood")
    void scala2Options() {
      JvmOptions options =
          JvmOptions.parse(
              List.of("-dependencyfile", "deps.txt", "Main.scala"), CompilerOption.Compiler.SCALA2);
      assertThat(options.rawArguments()).contains("-dependencyfile");
      assertThat(options.sourceFiles()).containsExactly(Path.of("Main.scala"));
    }

    @Test
    @DisplayName("a .java file is not a Scala source")
    void javaIsNotAScalaSource() {
      JvmOptions options =
          JvmOptions.parse(List.of("Foo.java"), CompilerOption.Compiler.SCALA3);
      assertThat(options.sourceFiles()).isEmpty();
    }
  }

  @Nested
  @DisplayName("the source-file rule")
  class Sources {

    @Test
    @DisplayName("the extensions are per language")
    void extensionsArePerLanguage() {
      assertThat(JvmOptions.sourceExtensions(CompilerOption.Compiler.JAVAC)).containsExactly("java");
      assertThat(JvmOptions.sourceExtensions(CompilerOption.Compiler.KOTLIN))
          .containsExactly("kt", "kts");
      assertThat(JvmOptions.sourceExtensions(CompilerOption.Compiler.SCALA3))
          .containsExactly("scala", "sc");
    }

    @Test
    @DisplayName("the extension check is case insensitive")
    void caseInsensitive() {
      assertThat(JvmOptions.isSourceFile("Foo.JAVA", CompilerOption.Compiler.JAVAC)).isTrue();
      assertThat(JvmOptions.isSourceFile("Main.KT", CompilerOption.Compiler.KOTLIN)).isTrue();
      assertThat(JvmOptions.isSourceFile("Main.SCALA", CompilerOption.Compiler.SCALA3)).isTrue();
    }
  }

  /** The platform's path separator. */
  private static String sep() {
    return CompilerOption.isWindows() ? ";" : ":";
  }
}
