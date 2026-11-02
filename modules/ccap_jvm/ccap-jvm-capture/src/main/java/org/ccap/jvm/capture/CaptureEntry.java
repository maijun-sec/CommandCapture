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

import java.nio.file.Path;
import java.util.List;

/**
 * One captured compile, in the form a static analyser needs.
 *
 * <p>Unlike a raw argv dump, this carries the <em>effective</em> compile: the classpath that was
 * actually used (not just the {@code -classpath} argument), the source files, the encoding, the
 * language level and the output directory.
 *
 * <p><b>Why the language is a field of its own.</b> The three JVM languages produce the same shape
 * of information but not the same information: a Kotlin compile has a language version and a JVM
 * target but no module path, and a Scala compile has neither. Recording the language explicitly lets
 * a consumer tell them apart without inferring it from the argv.
 *
 * <p><b>Why this is a class and not a record.</b> A record would be shorter, but records arrived in
 * Java 16 and the agent has to load on Java 8 -- see {@link Jdk8}. So the fields are written out
 * and the accessors are hand-written. They keep the record's shape, so the callers do not change.
 */
public final class CaptureEntry {

  private final String api;
  private final String compiler;
  private final String language;
  private final List<String> argv;
  private final String encoding;
  private final String languageLevel;
  private final Path outputDirectory;
  private final List<Path> classpath;
  private final List<Path> sourcePath;
  private final List<Path> modulePath;
  private final List<Path> processorPath;
  private final List<Path> bootClassPath;
  private final List<Path> sourceFiles;
  private final List<String> addModules;
  private final List<String> addExports;
  private final List<String> addReads;
  private final boolean preview;
  private final boolean compilation;
  private final String javaVersion;
  private final String javaHome;
  private final String workingDirectory;

  /**
   * One captured compile.
   *
   * @param api which entry point produced this capture: {@code cli} or {@code api}
   * @param compiler which compiler performed it: {@code javac}, {@code ecj}, {@code sjavac}, {@code
   *     kotlin}, {@code scala3} or {@code scala2}
   * @param language the language the sources are in: {@code java}, {@code kotlin} or {@code scala}
   * @param argv the raw compiler arguments
   * @param encoding the source encoding, or {@code null} when the platform default applies
   * @param languageLevel the {@code --release} / {@code -source} / language version level, or {@code
   *     null}
   * @param outputDirectory the {@code -d} directory, or {@code null}
   * @param classpath the classpath the compiler used
   * @param sourcePath the source path, or empty
   * @param modulePath the module path, or empty
   * @param processorPath the annotation processor path, or empty
   * @param bootClassPath the boot class path, or empty
   * @param sourceFiles the compiled source files
   * @param addModules the {@code --add-modules} entries, or empty
   * @param addExports the {@code --add-exports} entries, or empty
   * @param addReads the {@code --add-reads} entries, or empty
   * @param preview whether {@code --enable-preview} was given
   * @param compilation whether the command line actually produced class files
   * @param javaVersion the {@code java.version} of the compiling JVM
   * @param javaHome the {@code java.home} of the compiling JVM
   * @param workingDirectory the {@code user.dir} of the compiling JVM
   */
  public CaptureEntry(
      String api,
      String compiler,
      String language,
      List<String> argv,
      String encoding,
      String languageLevel,
      Path outputDirectory,
      List<Path> classpath,
      List<Path> sourcePath,
      List<Path> modulePath,
      List<Path> processorPath,
      List<Path> bootClassPath,
      List<Path> sourceFiles,
      List<String> addModules,
      List<String> addExports,
      List<String> addReads,
      boolean preview,
      boolean compilation,
      String javaVersion,
      String javaHome,
      String workingDirectory) {
    this.api = api;
    this.compiler = compiler;
    this.language = language;
    this.argv = argv;
    this.encoding = encoding;
    this.languageLevel = languageLevel;
    this.outputDirectory = outputDirectory;
    this.classpath = classpath;
    this.sourcePath = sourcePath;
    this.modulePath = modulePath;
    this.processorPath = processorPath;
    this.bootClassPath = bootClassPath;
    this.sourceFiles = sourceFiles;
    this.addModules = addModules;
    this.addExports = addExports;
    this.addReads = addReads;
    this.preview = preview;
    this.compilation = compilation;
    this.javaVersion = javaVersion;
    this.javaHome = javaHome;
    this.workingDirectory = workingDirectory;
  }

  /** Which entry point produced this capture: {@code cli} or {@code api}. */
  public String api() {
    return api;
  }

  /** Which compiler performed it: {@code javac}, {@code ecj}, {@code sjavac}, {@code kotlin}, {@code
   * scala3} or {@code scala2}. */
  public String compiler() {
    return compiler;
  }

  /** The language the sources are in: {@code java}, {@code kotlin} or {@code scala}. */
  public String language() {
    return language;
  }

  /** The raw compiler arguments. */
  public List<String> argv() {
    return argv;
  }

  /** The source encoding, or {@code null} when the platform default applies. */
  public String encoding() {
    return encoding;
  }

  /** The {@code --release} / {@code -source} / language version level, or {@code null}. */
  public String languageLevel() {
    return languageLevel;
  }

  /** The {@code -d} directory, or {@code null}. */
  public Path outputDirectory() {
    return outputDirectory;
  }

  /** The classpath the compiler used. */
  public List<Path> classpath() {
    return classpath;
  }

  /** The source path, or empty. */
  public List<Path> sourcePath() {
    return sourcePath;
  }

  /** The module path, or empty. */
  public List<Path> modulePath() {
    return modulePath;
  }

  /** The annotation processor path, or empty. */
  public List<Path> processorPath() {
    return processorPath;
  }

  /** The boot class path, or empty. */
  public List<Path> bootClassPath() {
    return bootClassPath;
  }

  /** The compiled source files. */
  public List<Path> sourceFiles() {
    return sourceFiles;
  }

  /** The {@code --add-modules} entries, or empty. */
  public List<String> addModules() {
    return addModules;
  }

  /** The {@code --add-exports} entries, or empty. */
  public List<String> addExports() {
    return addExports;
  }

  /** The {@code --add-reads} entries, or empty. */
  public List<String> addReads() {
    return addReads;
  }

  /** Whether {@code --enable-preview} was given. */
  public boolean preview() {
    return preview;
  }

  /** Whether the command line actually produced class files. */
  public boolean compilation() {
    return compilation;
  }

  /** The {@code java.version} of the compiling JVM. */
  public String javaVersion() {
    return javaVersion;
  }

  /** The {@code java.home} of the compiling JVM. */
  public String javaHome() {
    return javaHome;
  }

  /** The {@code user.dir} of the compiling JVM. */
  public String workingDirectory() {
    return workingDirectory;
  }

  /** Whether this capture describes the same compile as another. */
  public boolean sameCompileAs(CaptureEntry other) {
    return other != null && compileIdentity().equals(other.compileIdentity());
  }

  @Override
  public boolean equals(Object other) {
    if (this == other) {
      return true;
    }
    if (!(other instanceof CaptureEntry)) {
      return false;
    }
    return compileIdentity().equals(((CaptureEntry) other).compileIdentity());
  }

  @Override
  public int hashCode() {
    return compileIdentity().hashCode();
  }

  @Override
  public String toString() {
    return "CaptureEntry[" + compileIdentity().replace('\u0000', ' ') + "]";
  }
  /**
   * The identity of the compile this capture describes.
   *
   * <p><b>Why this is not the argv.</b> One compile reaches several hooked entry points: the
   * command-line driver goes through {@code com.sun.tools.javac.Main#compile} and then {@code
   * com.sun.tools.javac.main.Main#compile}, and the API path goes through {@code JavacTool#getTask}
   * and then {@code JavacTaskImpl#call}. Each of them would otherwise write its own file, and the
   * analyser would see the same compile several times over.
   *
   * <p><b>Why this is not the classpath either.</b> The classpath is the biggest and most expensive
   * part of the capture, and it does not distinguish two compiles of the same sources into
   * different directories.
   *
   * <p>So the identity is what actually distinguishes one compile from another: the compiler, the
   * language, the source files it compiles, the directory it compiles them into, and the options
   * that change the result. Two captures with the same identity are the same compile seen through
   * two entry points.
   *
   * @return a stable string that is equal for two views of the same compile
   */
  public String compileIdentity() {
    StringBuilder sb = new StringBuilder();
    sb.append("compiler=").append(compiler).append('\u0000');
    sb.append("lang=").append(language).append('\u0000');
    sb.append("out=").append(outputDirectory).append('\u0000');
    sb.append("cp=").append(classpath).append('\u0000');
    sb.append("sp=").append(sourcePath).append('\u0000');
    sb.append("mp=").append(modulePath).append('\u0000');
    sb.append("enc=").append(encoding).append('\u0000');
    sb.append("ll=").append(languageLevel).append('\u0000');
    sb.append("preview=").append(preview).append('\u0000');
    sb.append("compilation=").append(compilation).append('\u0000');
    // The source files are sorted, because the same set of files can arrive in a
    // different order from the API than from the command line.
    List<String> sources = new java.util.ArrayList<>();
    for (Path p : sourceFiles) {
      sources.add(String.valueOf(p));
    }
    java.util.Collections.sort(sources);
    sb.append("src=").append(sources).append('\u0000');
    return sb.toString();
  }

  /**
   * A capture with only the fields that matter for a simple test or a synthetic entry.
   *
   * <p>The full constructor has twenty-one arguments, and most callers do not care about most of
   * them. This one takes the handful that any test wants to assert on.
   */
  public static CaptureEntry simple(
      String api,
      List<String> argv,
      String encoding,
      String languageLevel,
      Path outputDirectory,
      List<Path> classpath,
      List<Path> sourceFiles) {
    return new CaptureEntry(
        api,
        "javac",
        "java",
        argv,
        encoding,
        languageLevel,
        outputDirectory,
        classpath,
        Jdk8.of(),
        Jdk8.of(),
        Jdk8.of(),
        Jdk8.of(),
        sourceFiles,
        Jdk8.of(),
        Jdk8.of(),
        Jdk8.of(),
        false,
        true,
        "17.0.1",
        "/opt/jdk",
        "/work");
  }

  /**
   * Render the capture as JSON.
   *
   * <p>Hand-rolled so the module has no JSON dependency: the recorder runs inside an arbitrary
   * compiler JVM and must stay self-contained.
   */
  public String toJson() {
    StringBuilder sb = new StringBuilder();
    sb.append("{\n");
    sb.append("  \"api\": ").append(quote(api)).append(",\n");
    sb.append("  \"compiler\": ").append(quote(compiler)).append(",\n");
    sb.append("  \"language\": ").append(quote(language)).append(",\n");
    sb.append("  \"argv\": ").append(jsonArray(argv)).append(",\n");
    sb.append("  \"encoding\": ").append(quote(encoding)).append(",\n");
    sb.append("  \"languageLevel\": ").append(quote(languageLevel)).append(",\n");
    sb.append("  \"outputDirectory\": ").append(quote(path(outputDirectory))).append(",\n");
    sb.append("  \"classpath\": ").append(jsonArray(paths(classpath))).append(",\n");
    sb.append("  \"sourcePath\": ").append(jsonArray(paths(sourcePath))).append(",\n");
    sb.append("  \"modulePath\": ").append(jsonArray(paths(modulePath))).append(",\n");
    sb.append("  \"processorPath\": ").append(jsonArray(paths(processorPath))).append(",\n");
    sb.append("  \"bootClassPath\": ").append(jsonArray(paths(bootClassPath))).append(",\n");
    sb.append("  \"sourceFiles\": ").append(jsonArray(paths(sourceFiles))).append(",\n");
    sb.append("  \"addModules\": ").append(jsonArray(addModules)).append(",\n");
    sb.append("  \"addExports\": ").append(jsonArray(addExports)).append(",\n");
    sb.append("  \"addReads\": ").append(jsonArray(addReads)).append(",\n");
    sb.append("  \"preview\": ").append(preview).append(",\n");
    sb.append("  \"compilation\": ").append(compilation).append(",\n");
    sb.append("  \"javaVersion\": ").append(quote(javaVersion)).append(",\n");
    sb.append("  \"javaHome\": ").append(quote(javaHome)).append(",\n");
    sb.append("  \"workingDirectory\": ").append(quote(workingDirectory)).append("\n");
    sb.append("}\n");
    return sb.toString();
  }

  private static List<String> paths(List<Path> in) {
    List<String> out = new java.util.ArrayList<>(in.size());
    for (Path p : in) {
      out.add(path(p));
    }
    return out;
  }

  private static String path(Path p) {
    return p == null ? null : p.toString();
  }

  private static String jsonArray(List<String> values) {
    StringBuilder sb = new StringBuilder("[");
    for (int i = 0; i < values.size(); i++) {
      if (i > 0) {
        sb.append(", ");
      }
      sb.append(quote(values.get(i)));
    }
    return sb.append(']').toString();
  }

  private static String quote(String s) {
    if (s == null) {
      return "null";
    }
    StringBuilder sb = new StringBuilder(s.length() + 8);
    sb.append('"');
    for (int i = 0; i < s.length(); i++) {
      char c = s.charAt(i);
      // A plain switch, not a switch expression: the agent has to load
      // on Java 8, where the arrow form does not exist.
      switch (c) {
        case '"':
          sb.append("\\\"");
          break;
        case '\\':
          sb.append("\\\\");
          break;
        case '\n':
          sb.append("\\n");
          break;
        case '\r':
          sb.append("\\r");
          break;
        case '\t':
          sb.append("\\t");
          break;
        default:
          if (c < 0x20) {
            sb.append(String.format("\\u%04x", (int) c));
          } else {
            sb.append(c);
          }
          break;
      }
    }
    return sb.append('"').toString();
  }
}

