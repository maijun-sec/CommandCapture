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
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * The compile options recovered from a captured JVM-language compiler command line.
 *
 * <p>This carries the information a static analyser needs: the source encoding, the language level,
 * the classpath, the source path, the output directory and the source files.
 *
 * <p><b>The option knowledge lives in {@link CompilerOption}.</b> This class only walks the argv
 * and applies the table.
 *
 * <p><b>Why the source-file rule is per-language.</b> A positional argument is a source file when
 * its extension says so, and the three languages use different extensions: {@code .java} for Java,
 * {@code .kt} and {@code .kts} for Kotlin, {@code .scala} and {@code .sc} for Scala. Using one
 * rule for all of them would either miss the Kotlin and Scala sources or admit a jar of classes as
 * a source file.
 *
 * <p><b>Why the argument-file expansion is per-language too.</b> javac splits its argument file on
 * whitespace and strips the quotes without grouping them. scalac does the same. Kotlin does not
 * read argument files at all -- it has no {@code @file} convention -- so expanding one for Kotlin
 * would be inventing a feature that does not exist.
 */
public final class JvmOptions {

  private final List<String> rawArguments;
  private final Charset encoding;
  private final String encodingName;
  private final String source;
  private final String target;
  private final String release;
  private final String languageLevel;
  private final List<Path> classpath;
  private final List<Path> sourcePath;
  private final List<Path> modulePath;
  private final List<Path> moduleSourcePath;
  private final List<Path> processorPath;
  private final List<Path> bootClassPath;
  private final Path outputDirectory;
  private final Path annotationProcessingDirectory;
  private final List<Path> sourceFiles;
  private final List<String> addModules;
  private final List<String> addExports;
  private final List<String> addReads;
  private final boolean preview;
  private final boolean compilation;

  private JvmOptions(Builder b) {
    this.rawArguments = Jdk8.copyOf(b.rawArguments);
    this.encoding = b.encoding;
    this.encodingName = b.encodingName;
    this.source = b.source;
    this.target = b.target;
    this.release = b.release;
    this.languageLevel = b.languageLevel;
    this.classpath = Jdk8.copyOf(b.classpath);
    this.sourcePath = Jdk8.copyOf(b.sourcePath);
    this.modulePath = Jdk8.copyOf(b.modulePath);
    this.moduleSourcePath = Jdk8.copyOf(b.moduleSourcePath);
    this.processorPath = Jdk8.copyOf(b.processorPath);
    this.bootClassPath = Jdk8.copyOf(b.bootClassPath);
    this.outputDirectory = b.outputDirectory;
    this.annotationProcessingDirectory = b.annotationProcessingDirectory;
    this.sourceFiles = Jdk8.copyOf(b.sourceFiles);
    this.addModules = Jdk8.copyOf(b.addModules);
    this.addExports = Jdk8.copyOf(b.addExports);
    this.addReads = Jdk8.copyOf(b.addReads);
    this.preview = b.preview;
    this.compilation = b.compilation;
  }

  /** The raw argv, exactly as the compiler received it. */
  public List<String> rawArguments() {
    return rawArguments;
  }

  public Charset encoding() {
    return encoding;
  }

  /**
   * The encoding as the compiler was told, e.g. {@code UTF-8}.
   *
   * <p>Null when the command line did not say, which means the platform default applied.
   */
  public String encodingName() {
    return encodingName;
  }

  /** The {@code -source} level, or {@code null} when the compiler was not told. */
  public String source() {
    return source;
  }

  /** The {@code -target} level, or {@code null} when the compiler was not told. */
  public String target() {
    return target;
  }

  /** The {@code --release} level, or {@code null} when the compiler was not told. */
  public String release() {
    return release;
  }

  /**
   * The language level to analyse at.
   *
   * <p>{@code --release} wins over {@code -source}, because it pins both the language level and the
   * platform API. Falls back to {@code -source}, then to {@code -target}.
   *
   * <p><b>For Kotlin and Scala this is the option that carries the meaning.</b> Kotlin's language
   * version is {@code -language-version} and its JVM target is {@code -jvm-target}; Scala 3's are
   * {@code -source} and {@code -target}. They are recorded under the same canonical names as
   * javac's, so a consumer does not have to know which compiler produced the capture.
   */
  public String languageLevel() {
    return languageLevel;
  }

  public List<Path> classpath() {
    return classpath;
  }

  public List<Path> sourcePath() {
    return sourcePath;
  }

  public List<Path> modulePath() {
    return modulePath;
  }

  /** The {@code --module-source-path}, or empty. */
  public List<Path> moduleSourcePath() {
    return moduleSourcePath;
  }

  /** The {@code --processor-path}, or empty. */
  public List<Path> processorPath() {
    return processorPath;
  }

  /** The boot class path, or empty. */
  public List<Path> bootClassPath() {
    return bootClassPath;
  }

  /** The {@code -d} output directory, or {@code null}. */
  public Path outputDirectory() {
    return outputDirectory;
  }

  /** The {@code -s} annotation processing directory, or {@code null}. */
  public Path annotationProcessingDirectory() {
    return annotationProcessingDirectory;
  }

  public List<Path> sourceFiles() {
    return sourceFiles;
  }

  /** The {@code --add-modules} entries, in order. */
  public List<String> addModules() {
    return addModules;
  }

  /** The {@code --add-exports} entries, in order. */
  public List<String> addExports() {
    return addExports;
  }

  /** The {@code --add-reads} entries, in order. */
  public List<String> addReads() {
    return addReads;
  }

  /** Whether {@code --enable-preview} was given. */
  public boolean preview() {
    return preview;
  }

  /**
   * Whether this command line actually produced class files.
   *
   * <p>False for {@code -Xprint} and {@code -proc:only}: the first prints the compiler's internal
   * state, the second runs only the annotation processors. Neither writes a class file, so a
   * capture that counted them as compiles would report a compilation that never happened.
   */
  public boolean isCompilation() {
    return compilation;
  }

  /**
   * Parse a captured javac argv into structured options.
   *
   * @param arguments the argv javac received
   * @return the parsed options
   */
  public static JvmOptions parse(List<String> arguments) {
    return parse(arguments, CompilerOption.Compiler.JAVAC);
  }

  /**
   * Parse a captured argv into structured options, for a given compiler.
   *
   * <p>The compiler matters because the options differ: ECJ spells the classpath {@code
   * --class-path}, and sjavac has its own set. Parsing ECJ's arguments with javac's table would
   * drop the classpath.
   *
   * @param arguments the argv the compiler received
   * @param compiler which compiler received it
   * @return the parsed options
   */
  public static JvmOptions parse(List<String> arguments, CompilerOption.Compiler compiler) {
    Builder b = new Builder();
    if (arguments == null || arguments.isEmpty()) {
      return b.build();
    }
    b.rawArguments = arguments;
    arguments = expandArgumentFiles(arguments, compiler);

    // The values seen per canonical option name, so that "only the last one counts" and "every
    // occurrence applies" can be applied without hard-coding either.
    Map<String, List<String>> values = new LinkedHashMap<>();

    for (int i = 0; i < arguments.size(); i++) {
      String arg = arguments.get(i);
      if (arg == null || arg.isEmpty()) {
        continue;
      }

      // An option and its value may be attached: "-encodingUTF-8" or "-dout".
      CompilerOption option = CompilerOption.byName(arg, compiler);
      String attached = null;
      if (option == null) {
        // Try the attached form: find the LONGEST option name this argument
        // starts with, where the option takes a value.
        //
        // The longest one matters. Scala spells its level "-source:3.0", and
        // the table has both "-source" and "-source:" -- matching "-source"
        // first would leave the value as ":3.0", which is not a version.
        CompilerOption best = null;
        for (CompilerOption candidate : CompilerOption.forCompiler(compiler)) {
          if (candidate.value() == CompilerOption.Value.NONE) {
            continue;
          }
          if (arg.length() > candidate.name().length()
              && arg.startsWith(candidate.name())
              && candidate.name().startsWith("-")) {
            if (best == null || candidate.name().length() > best.name().length()) {
              best = candidate;
            }
          }
        }
        if (best != null) {
          option = best;
          attached = arg.substring(best.name().length());
        }
      }

      if (option == null) {
        // Not an option: it is a source file -- if its extension says so.
        //
        // A jar is NOT a source file, even though javac accepts one on the
        // source path.  Recording it as one put a jar into the source list,
        // and the source list is part of the compile identity -- so two
        // genuinely different compiles could collide, and the analyser would
        // try to parse an archive as source.
        if (isSourceFile(arg, compiler)) {
          b.sourceFiles.add(Jdk8.path(arg));
        }
        continue;
      }

      String value = attached;
      if (value == null && option.takesValue()) {
        if (i + 1 < arguments.size()) {
          value = arguments.get(++i);
        } else {
          value = "";
        }
      }

      if (option.effect() == CompilerOption.Effect.NOT_COMPILATION) {
        b.compilation = false;
        continue;
      }

      List<String> seen = values.computeIfAbsent(option.canonicalName(), k -> new ArrayList<>());
      if (option.repeat() == CompilerOption.Repeat.LAST_WINS) {
        seen.clear();
      }
      if (value != null) {
        seen.add(value);
      }

      apply(b, option, value);
    }

    return b.build();
  }

  /**
   * Whether a positional argument names a source file of the given language.
   *
   * <p><b>Why the extension decides.</b> The compiler itself decides by extension: javac compiles
   * {@code .java}, kotlinc compiles {@code .kt} and {@code .kts}, scalac compiles {@code .scala}.
   * A file with another extension is not a source, and treating it as one would put a jar or a
   * resource into the source list.
   */
  static boolean isSourceFile(String argument, CompilerOption.Compiler compiler) {
    if (argument == null || argument.isEmpty()) {
      return false;
    }
    String lower = argument.toLowerCase(Locale.ROOT);
    // A plain switch, not a switch expression: the agent has to load on
    // Java 8, where the arrow form does not exist.
    switch (compiler) {
      case JAVAC:
      case ECJ:
      case SJAVAC:
        return lower.endsWith(".java");
      case KOTLIN:
        return lower.endsWith(".kt") || lower.endsWith(".kts");
      case SCALA3:
      case SCALA2:
        return lower.endsWith(".scala") || lower.endsWith(".sc");
      default:
        return false;
    }
  }

  /**
   * The source extensions of a compiler, for the tests and for documentation.
   *
   * @param compiler which compiler
   * @return the extensions, without the dot
   */
  static List<String> sourceExtensions(CompilerOption.Compiler compiler) {
    switch (compiler) {
      case JAVAC:
      case ECJ:
      case SJAVAC:
        return Jdk8.of("java");
      case KOTLIN:
        return Jdk8.of("kt", "kts");
      case SCALA3:
      case SCALA2:
        return Jdk8.of("scala", "sc");
      default:
        return Jdk8.of();
    }
  }

  /**
   * Replace an argument file with the arguments it holds.
   *
   * <p><b>Why this matters.</b> javac reads its options from a file when an argument starts with
   * {@code @}. A build that puts its classpath in such a file has told javac everything through the
   * file, and the command line itself carries almost nothing. A capture that did not open the file
   * recorded a compile with an empty classpath and an unknown language level -- confidently wrong,
   * which is worse than nothing.
   *
   * <p><b>The format, measured against javac.</b> The file is split on WHITESPACE, and the quotes
   * around a token are stripped -- but they do NOT group it. So {@code -d "C:\a"} is the option
   * {@code -d} with the value {@code C:\a}, while {@code -d "C:\a b"} is the option {@code -d} with
   * the value {@code C:\a} followed by a stray {@code b"} that javac treats as a source file.
   *
   * <p>A file that cannot be read is left as it was: the argument stays, and the compile is
   * recorded with whatever else the command line said.
   *
   * <p>The expansion is bounded, so an argument file that refers to itself cannot loop.
   *
   * <p><b>Kotlin has no argument files.</b> The Kotlin compiler has no {@code @file} convention, so
   * expanding one for Kotlin would invent a feature that does not exist -- and would silently
   * swallow an argument that happened to start with {@code @}.
   *
   * @param arguments the argv, possibly containing argument files
   * @param compiler which compiler received them
   * @return the argv with the argument files expanded
   */
  static List<String> expandArgumentFiles(
      List<String> arguments, CompilerOption.Compiler compiler) {
    if (compiler == CompilerOption.Compiler.KOTLIN) {
      return arguments;
    }
    List<String> out = new ArrayList<>();
    expandArgumentFiles(arguments, out, 0);
    return out;
  }

  /** Bound on how deep an argument file may refer to another one. */
  private static final int MAX_ARGUMENT_FILE_DEPTH = 8;

  private static void expandArgumentFiles(List<String> arguments, List<String> out, int depth) {
    for (String argument : arguments) {
      if (argument == null
          || argument.isEmpty()
          || argument.charAt(0) != '@'
          || depth >= MAX_ARGUMENT_FILE_DEPTH) {
        out.add(argument);
        continue;
      }
      String fileName = argument.substring(1);
      if (fileName.isEmpty()) {
        out.add(argument);
        continue;
      }
      List<String> contents = readArgumentFile(Jdk8.path(fileName));
      if (contents == null) {
        // The file cannot be read.  The argument stays, so the record shows
        // that the compile was driven by an argument file.
        out.add(argument);
        continue;
      }
      expandArgumentFiles(contents, out, depth + 1);
    }
  }

  /**
   * The arguments in an argument file, or {@code null} when the file cannot be read.
   *
   * <p>The file is read as UTF-8, which is what javac does for an argument file on a modern JDK.
   */
  private static List<String> readArgumentFile(Path file) {
    try {
      String text = Jdk8.readString(file, StandardCharsets.UTF_8);
      List<String> out = new ArrayList<>();
      for (String token : text.split("\\s+")) {
        String stripped = stripQuotes(token);
        if (!stripped.isEmpty()) {
          out.add(stripped);
        }
      }
      return out;
    } catch (IOException | RuntimeException e) {
      return null;
    }
  }

  /**
   * The quotes around a token, removed.
   *
   * <p>Only a matching pair at the two ends is removed: a token that merely starts with a quote --
   * which is what a split value with a space inside it looks like -- keeps it, because that is what
   * javac sees.
   */
  private static String stripQuotes(String token) {
    if (token.length() >= 2 && token.startsWith("\"") && token.endsWith("\"")) {
      return token.substring(1, token.length() - 1);
    }
    return token;
  }

  /** Fold one option's value into the builder. */
  private static void apply(Builder b, CompilerOption option, String value) {
    String canonical = option.canonicalName();
    // A plain switch, not a switch expression: the agent has to load on
    // Java 8, where the arrow form does not exist.
    switch (canonical) {
      case "--output-dir":
        if (value != null && !Jdk8.isBlank(value)) {
          b.outputDirectory = Jdk8.path(value);
        }
        break;
      case "--classpath":
        b.classpath = splitClasspath(value, option);
        break;
      case "--sourcepath":
        b.sourcePath = mergePaths(b.sourcePath, CompilerOption.splitPath(value));
        break;
      case "--source-root":
        b.sourcePath = mergePaths(b.sourcePath, CompilerOption.splitPath(value));
        break;
      case "--module-path":
        b.modulePath = CompilerOption.splitPath(value);
        break;
      case "--module-source-path":
        b.moduleSourcePath = CompilerOption.splitPath(value);
        break;
      case "--processor-path":
        b.processorPath = CompilerOption.splitPath(value);
        break;
      case "bootclasspath":
        b.bootClassPath = CompilerOption.splitPath(value);
        break;
      case "--annotation-processing-dir":
        if (value != null && !Jdk8.isBlank(value)) {
          b.annotationProcessingDirectory = Jdk8.path(value);
        }
        break;
      case "--encoding":
        if (value != null && !Jdk8.isBlank(value)) {
          b.encoding = charset(value);
          b.encodingName = value;
        }
        break;
      case "--source":
        b.source = value;
        break;
      case "--target":
        b.target = value;
        break;
      case "--release":
        b.release = value;
        break;
      case "--add-modules":
        b.addModules.addAll(CompilerOption.splitCommaList(value));
        break;
      case "--add-exports":
        b.addExports.addAll(CompilerOption.splitCommaList(value));
        break;
      case "--add-reads":
        b.addReads.addAll(CompilerOption.splitCommaList(value));
        break;
      case "--enable-preview":
        b.preview = true;
        break;
      case "--language-version":
        b.languageLevel = value;
        break;
      case "--jvm-target":
        // Kotlin's -jvm-target is the level the bytecode targets, which is
        // what an analyser has to know -- so it becomes the language level
        // when the language version was not given.
        if (b.languageLevel == null) {
          b.languageLevel = value;
        }
        break;
      default:
        // An option we record but do not interpret, e.g. --system or --patch-module.
        break;
    }
  }

  /**
   * The paths of two occurrences of an additive option, together.
   *
   * <p>The first occurrence wins the list, and the later ones are appended: so the order the
   * compiler saw is preserved, and no occurrence is lost.
   */
  private static List<Path> mergePaths(List<Path> existing, List<Path> added) {
    if (existing.isEmpty()) {
      return added;
    }
    List<Path> out = new ArrayList<>(existing);
    for (Path p : added) {
      if (!out.contains(p)) {
        out.add(p);
      }
    }
    return out;
  }

  /**
   * The paths of a classpath value.
   *
   * <p>How the entries are separated depends on the option: javac and Kotlin use the platform
   * separator, and a comma-separated list is also accepted for the classpath of a build that
   * assembles it on another platform's conventions.
   */
  private static List<Path> splitClasspath(String value, CompilerOption option) {
    if (option.value() == CompilerOption.Value.PATH_LIST_OR_COMMA) {
      return CompilerOption.splitPathOrComma(value);
    }
    return CompilerOption.splitPath(value);
  }

  private static Charset charset(String name) {
    try {
      return Charset.forName(name);
    } catch (Exception e) {
      return StandardCharsets.UTF_8;
    }
  }

  /** Builder for {@link JvmOptions}. */
  private static final class Builder {
    List<String> rawArguments = Jdk8.of();
    Charset encoding = StandardCharsets.UTF_8;
    String encodingName;
    String source;
    String target;
    String release;
    String languageLevel;
    List<Path> classpath = new ArrayList<>();
    List<Path> sourcePath = new ArrayList<>();
    List<Path> modulePath = new ArrayList<>();
    List<Path> moduleSourcePath = new ArrayList<>();
    List<Path> processorPath = new ArrayList<>();
    List<Path> bootClassPath = new ArrayList<>();
    Path outputDirectory;
    Path annotationProcessingDirectory;
    Set<Path> sourceFiles = new LinkedHashSet<>();
    List<String> addModules = new ArrayList<>();
    List<String> addExports = new ArrayList<>();
    List<String> addReads = new ArrayList<>();
    boolean preview;
    boolean compilation = true;

    JvmOptions build() {
      // The language level falls back the way javac's does: --release wins,
      // then -source, then -target.
      if (languageLevel == null) {
        if (release != null) {
          languageLevel = release;
        } else if (source != null) {
          languageLevel = source;
        } else {
          languageLevel = target;
        }
      }
      return new JvmOptions(this);
    }
  }
}

