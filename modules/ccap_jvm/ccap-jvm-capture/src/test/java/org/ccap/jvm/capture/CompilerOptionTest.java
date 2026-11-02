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

/** The option tables, one language at a time. */
class CompilerOptionTest {

  @Nested
  @DisplayName("Java")
  class Java {

    @Test
    @DisplayName("the classpath is found under all three spellings")
    void classpathSpellings() {
      for (String spelling : List.of("-classpath", "--classpath", "-cp", "--class-path")) {
        CompilerOption option =
            CompilerOption.byName(spelling, CompilerOption.Compiler.JAVAC);
        assertThat(option).as(spelling).isNotNull();
        assertThat(option.canonicalName()).as(spelling).isEqualTo("--classpath");
        assertThat(option.value()).as(spelling).isEqualTo(CompilerOption.Value.PATH_LIST);
      }
    }

    @Test
    @DisplayName("the classpath is last-wins, not additive")
    void classpathIsLastWins() {
      assertThat(
              CompilerOption.byName("-cp", CompilerOption.Compiler.JAVAC).repeat())
          .isEqualTo(CompilerOption.Repeat.LAST_WINS);
    }

    @Test
    @DisplayName("--add-modules and --add-exports are additive")
    void additiveOptions() {
      assertThat(
              CompilerOption.byName("--add-modules", CompilerOption.Compiler.JAVAC).repeat())
          .isEqualTo(CompilerOption.Repeat.ALL_APPLY);
      assertThat(
              CompilerOption.byName("--add-exports", CompilerOption.Compiler.JAVAC).repeat())
          .isEqualTo(CompilerOption.Repeat.ALL_APPLY);
    }

    @Test
    @DisplayName("-Xprint and -proc:only do not compile anything")
    void notCompilationOptions() {
      assertThat(
              CompilerOption.byName("-Xprint", CompilerOption.Compiler.JAVAC).effect())
          .isEqualTo(CompilerOption.Effect.NOT_COMPILATION);
      assertThat(
              CompilerOption.byName("-proc:only", CompilerOption.Compiler.JAVAC).effect())
          .isEqualTo(CompilerOption.Effect.NOT_COMPILATION);
    }

    @Test
    @DisplayName("ECJ's own options are not javac's")
    void ecjOwnOptions() {
      // ECJ has -src, -profile and -log; javac does not.
      assertThat(CompilerOption.byName("-src", CompilerOption.Compiler.ECJ)).isNotNull();
      assertThat(CompilerOption.byName("-profile", CompilerOption.Compiler.ECJ)).isNotNull();
      assertThat(CompilerOption.byName("-log", CompilerOption.Compiler.ECJ)).isNotNull();
      // ... and javac must not pick them up: an unknown option loses its value.
      assertThat(CompilerOption.byName("-src", CompilerOption.Compiler.JAVAC)).isNull();
      assertThat(CompilerOption.byName("-profile", CompilerOption.Compiler.JAVAC)).isNull();
    }

    @Test
    @DisplayName("ECJ's classpath spelling is the shared one")
    void ecjClasspath() {
      CompilerOption option = CompilerOption.byName("--class-path", CompilerOption.Compiler.ECJ);
      assertThat(option).isNotNull();
      assertThat(option.canonicalName()).isEqualTo("--classpath");
    }

    @Test
    @DisplayName("sjavac's -j is last-wins while ECJ's is too, but sjavac's -x is additive")
    void sjavacOwnOptions() {
      assertThat(CompilerOption.byName("-j", CompilerOption.Compiler.SJAVAC)).isNotNull();
      assertThat(CompilerOption.byName("-x", CompilerOption.Compiler.SJAVAC).repeat())
          .isEqualTo(CompilerOption.Repeat.ALL_APPLY);
    }
  }

  @Nested
  @DisplayName("Kotlin")
  class Kotlin {

    @Test
    @DisplayName("the classpath is found under -classpath and -cp")
    void classpathSpellings() {
      for (String spelling : List.of("-classpath", "-cp")) {
        CompilerOption option =
            CompilerOption.byName(spelling, CompilerOption.Compiler.KOTLIN);
        assertThat(option).as(spelling).isNotNull();
        assertThat(option.canonicalName()).as(spelling).isEqualTo("--classpath");
      }
    }

    @Test
    @DisplayName("Kotlin's -source is a source root, not a language level")
    void sourceIsASourceRoot() {
      // This is the difference that would have made the capture confidently
      // wrong: javac's -source is the language level, and Kotlin's is a
      // directory of sources.
      CompilerOption option = CompilerOption.byName("-source", CompilerOption.Compiler.KOTLIN);
      assertThat(option).isNotNull();
      assertThat(option.canonicalName()).isEqualTo("--source-root");
      assertThat(option.repeat()).isEqualTo(CompilerOption.Repeat.ALL_APPLY);

      // javac's own -source is a different thing, and it must stay that way.
      assertThat(CompilerOption.byName("-source", CompilerOption.Compiler.JAVAC).canonicalName())
          .isEqualTo("--source");
    }

    @Test
    @DisplayName("the output directory is -d")
    void outputDirectory() {
      CompilerOption option = CompilerOption.byName("-d", CompilerOption.Compiler.KOTLIN);
      assertThat(option).isNotNull();
      assertThat(option.canonicalName()).isEqualTo("--output-dir");
    }

    @Test
    @DisplayName("the language version and the JVM target are separate options")
    void languageAndTarget() {
      assertThat(CompilerOption.byName("-language-version", CompilerOption.Compiler.KOTLIN))
          .isNotNull();
      assertThat(CompilerOption.byName("-jvm-target", CompilerOption.Compiler.KOTLIN)).isNotNull();
      // javac must not pick them up.
      assertThat(CompilerOption.byName("-jvm-target", CompilerOption.Compiler.JAVAC)).isNull();
    }

    @Test
    @DisplayName("-version and -help do not compile anything")
    void notCompilationOptions() {
      assertThat(CompilerOption.byName("-version", CompilerOption.Compiler.KOTLIN).effect())
          .isEqualTo(CompilerOption.Effect.NOT_COMPILATION);
      assertThat(CompilerOption.byName("-help", CompilerOption.Compiler.KOTLIN).effect())
          .isEqualTo(CompilerOption.Effect.NOT_COMPILATION);
    }

    @Test
    @DisplayName("the plugin options are additive")
    void pluginOptions() {
      assertThat(CompilerOption.byName("-Xplugin", CompilerOption.Compiler.KOTLIN).repeat())
          .isEqualTo(CompilerOption.Repeat.ALL_APPLY);
      assertThat(CompilerOption.byName("-P", CompilerOption.Compiler.KOTLIN).repeat())
          .isEqualTo(CompilerOption.Repeat.ALL_APPLY);
    }
  }

  @Nested
  @DisplayName("Scala")
  class Scala {

    @Test
    @DisplayName("the classpath is found under -classpath and -cp")
    void classpathSpellings() {
      for (CompilerOption.Compiler compiler :
          List.of(CompilerOption.Compiler.SCALA3, CompilerOption.Compiler.SCALA2)) {
        for (String spelling : List.of("-classpath", "-cp")) {
          CompilerOption option = CompilerOption.byName(spelling, compiler);
          assertThat(option).as(compiler + " " + spelling).isNotNull();
          assertThat(option.canonicalName()).as(compiler + " " + spelling)
              .isEqualTo("--classpath");
          // Scala's classpath accepts a comma-separated list as well.
          assertThat(option.value()).as(compiler + " " + spelling)
              .isEqualTo(CompilerOption.Value.PATH_LIST_OR_COMMA);
        }
      }
    }

    @Test
    @DisplayName("the source path is additive, unlike javac's")
    void sourcePathIsAdditive() {
      // javac's -sourcepath is last-wins; Scala accumulates several roots.
      assertThat(CompilerOption.byName("-sourcepath", CompilerOption.Compiler.JAVAC).repeat())
          .isEqualTo(CompilerOption.Repeat.LAST_WINS);
      assertThat(CompilerOption.byName("-sourcepath", CompilerOption.Compiler.SCALA3).repeat())
          .isEqualTo(CompilerOption.Repeat.ALL_APPLY);
      assertThat(CompilerOption.byName("-sourcepath", CompilerOption.Compiler.SCALA2).repeat())
          .isEqualTo(CompilerOption.Repeat.ALL_APPLY);
    }

    @Test
    @DisplayName("Scala 3 and Scala 2 keep their own options apart")
    void scala3AndScala2AreDistinct() {
      assertThat(CompilerOption.byName("-from-tasty", CompilerOption.Compiler.SCALA3)).isNotNull();
      assertThat(CompilerOption.byName("-from-tasty", CompilerOption.Compiler.SCALA2)).isNull();

      assertThat(CompilerOption.byName("-dependencyfile", CompilerOption.Compiler.SCALA2)).isNotNull();
      assertThat(CompilerOption.byName("-dependencyfile", CompilerOption.Compiler.SCALA3)).isNull();
    }

    @Test
    @DisplayName("the output directory is -d")
    void outputDirectory() {
      for (CompilerOption.Compiler compiler :
          List.of(CompilerOption.Compiler.SCALA3, CompilerOption.Compiler.SCALA2)) {
        CompilerOption option = CompilerOption.byName("-d", compiler);
        assertThat(option).as(compiler.toString()).isNotNull();
        assertThat(option.canonicalName()).as(compiler.toString()).isEqualTo("--output-dir");
      }
    }

    @Test
    @DisplayName("-version and -help do not compile anything")
    void notCompilationOptions() {
      assertThat(CompilerOption.byName("-version", CompilerOption.Compiler.SCALA3).effect())
          .isEqualTo(CompilerOption.Effect.NOT_COMPILATION);
      assertThat(CompilerOption.byName("-help", CompilerOption.Compiler.SCALA2).effect())
          .isEqualTo(CompilerOption.Effect.NOT_COMPILATION);
    }
  }

  @Nested
  @DisplayName("the shared index")
  class Index {

    @Test
    @DisplayName("an option belongs to no compiler in particular is shared by all of them")
    void sharedOptions() {
      for (CompilerOption.Compiler compiler : CompilerOption.Compiler.values()) {
        CompilerOption option = CompilerOption.byName("--classpath", compiler);
        assertThat(option).as(compiler.toString()).isNotNull();
        assertThat(option.canonicalName()).as(compiler.toString()).isEqualTo("--classpath");
      }
    }

    @Test
    @DisplayName("the lookup without a compiler prefers the shared entry")
    void lookupWithoutCompiler() {
      CompilerOption option = CompilerOption.byName("-cp");
      assertThat(option).isNotNull();
      assertThat(option.canonicalName()).isEqualTo("--classpath");
    }

    @Test
    @DisplayName("every option's canonical name is unique per compiler")
    void canonicalNamesAreConsistent() {
      // Two spellings of the same option must agree on the canonical name,
      // or the compile identity changes when a build switches between them.
      assertThat(CompilerOption.byName("-cp", CompilerOption.Compiler.JAVAC).canonicalName())
          .isEqualTo(
              CompilerOption.byName("-classpath", CompilerOption.Compiler.JAVAC).canonicalName());
      assertThat(CompilerOption.byName("-cp", CompilerOption.Compiler.KOTLIN).canonicalName())
          .isEqualTo(
              CompilerOption.byName("-classpath", CompilerOption.Compiler.KOTLIN).canonicalName());
      assertThat(CompilerOption.byName("-cp", CompilerOption.Compiler.SCALA3).canonicalName())
          .isEqualTo(
              CompilerOption.byName("-classpath", CompilerOption.Compiler.SCALA3).canonicalName());
    }

    @Test
    @DisplayName("every compiler has at least the options it needs")
    void everyCompilerIsCovered() {
      for (CompilerOption.Compiler compiler : CompilerOption.Compiler.values()) {
        assertThat(CompilerOption.forCompiler(compiler))
            .as(compiler.toString())
            .isNotEmpty();
        assertThat(CompilerOption.byName("-d", compiler)).as(compiler.toString()).isNotNull();
      }
    }
  }

  @Nested
  @DisplayName("the path splitting")
  class Paths {

    @Test
    @DisplayName("a Windows drive letter is not split in half")
    void windowsDriveLetter() {
      // Splitting on both separators at once would turn "D:\cache\repo.jar"
      // into "D" and "\cache\repo.jar".
      List<Path> paths = CompilerOption.splitPath("D:\\cache\\repo.jar");
      assertThat(paths).hasSize(1);
      assertThat(paths.get(0).toString()).isEqualTo("D:\\cache\\repo.jar");
    }

    @Test
    @DisplayName("the platform separator splits a list")
    void platformSeparator() {
      char separator = CompilerOption.isWindows() ? ';' : ':';
      List<Path> paths = CompilerOption.splitPath("a.jar" + separator + "b.jar");
      assertThat(paths).hasSize(2);
    }

    @Test
    @DisplayName("a comma inside a path is not a separator")
    void commaInsideAPath() {
      // The left side has neither an extension nor a complete path, so it
      // is not a classpath entry -- and the comma is part of the path.
      List<Path> paths = CompilerOption.splitPathOrComma("/opt/my,lib/repo.jar");
      assertThat(paths).hasSize(1);
      // The path is normalized by the platform, so the separators are the
      // platform's.
      assertThat(paths.get(0).toString().replace('\\', '/'))
          .isEqualTo("/opt/my,lib/repo.jar");
    }

    @Test
    @DisplayName("a real comma separates two paths")
    void commaSeparatesTwoPaths() {
      List<Path> paths = CompilerOption.splitPathOrComma("/opt/a.jar,/opt/b.jar");
      assertThat(paths).hasSize(2);
    }

    @Test
    @DisplayName("two jars with no directory split on the comma")
    void twoJarsSplit() {
      List<Path> paths = CompilerOption.splitPathOrComma("a.jar,b.jar");
      assertThat(paths).hasSize(2);
    }

    @Test
    @DisplayName("a comma after a drive letter is not a separator")
    void commaAfterDriveLetter() {
      List<Path> paths = CompilerOption.splitPathOrComma("C:\\lib\\a.jar,C:\\lib\\b.jar");
      assertThat(paths).hasSize(2);
    }
  }
}
