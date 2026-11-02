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
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * One compiler option, described as data.
 *
 * <p><b>Why the options are data and not a chain of {@code if}s.</b> The original implementation
 * hard-coded a dozen option names in a switch. Every option it did not know fell through to the
 * default branch, and the default branch treated anything ending in {@code .java} as a source file.
 * So {@code --processor-path some/where.jar} was silently dropped, and an option whose value
 * happened to end in {@code .java} would be mistaken for a source.
 *
 * <p>Adding an option is therefore <b>a data change</b>: one line in {@link #ALL}.
 *
 * <p><b>Why the index is keyed by the name AND the compiler.</b> Two compilers can spell an option
 * the same way and still mean different things by it. ECJ and sjavac both have {@code -src}, {@code
 * -copy}, {@code -x}, {@code -i}, {@code -tr} and {@code -j} -- and sjavac's {@code -j} is
 * repeatable while ECJ's is not. An index keyed by the name alone holds ONE option per name, so the
 * second entry silently overwrote the first and the ECJ lookup rejected all six: the option was
 * dropped, and the value that followed it was read as a source file.
 *
 * <p>So the index holds one option per (name, compiler) pair.
 */
public final class CompilerOption {

  /** How many values an option takes, and what kind. */
  public enum Value {
    /** The option takes no value: {@code --enable-preview}. */
    NONE,
    /** The option takes one value: {@code -encoding}. */
    SINGLE,
    /**
     * The option takes one value that is a list of paths, separated by the platform's path
     * separator: {@code -classpath}.
     */
    PATH_LIST,
    /**
     * The option takes one value that is a list of paths, separated by commas: {@code --add-exports
     * module/package=other/package}.
     */
    COMMA_LIST,
    /**
     * The option takes one value that is a list of paths, separated by <em>either</em> the platform
     * path separator or a comma.
     *
     * <p><b>Why this exists.</b> The three JVM languages disagree on the separator. javac uses the
     * platform separator ({@code ;} on Windows, {@code :} elsewhere). Kotlin's {@code -classpath}
     * uses the platform separator too. Scala's {@code -classpath} uses the platform separator, but
     * its {@code -bootclasspath} and the plugin option {@code -P} have their own conventions. A
     * single "split on the platform separator" rule would cut a Windows drive letter in half, and
     * a single "split on comma" rule would break a Unix path that contains a comma.
     *
     * <p>So a value of this kind is split on the platform separator <em>and</em> on a comma, but
     * only when the comma is a genuine separator -- a comma inside a path segment is kept.
     */
    PATH_LIST_OR_COMMA
  }

  /**
   * How repeated uses of the option combine.
   *
   * <p>This is the distinction that decides what the compile actually did. {@code -classpath} given
   * twice means the <em>last</em> one wins, not both. {@code --add-exports} given twice means both
   * module lists apply. Getting this wrong produces a classpath that never existed.
   */
  public enum Repeat {
    /** Only the last occurrence counts: {@code -classpath}. */
    LAST_WINS,
    /** Every occurrence applies: {@code --add-exports}. */
    ALL_APPLY
  }

  /**
   * Whether the option affects the compilation at all.
   *
   * <p>{@code -Xprint} and {@code -proc:only} do not produce a class file: the first prints
   * internal information, the second runs only the annotation processors. A capture that treats
   * them as compilation options would report a compile that never happened.
   */
  public enum Effect {
    /** The option changes the compilation. */
    COMPILATION,
    /** The option does not produce a class file. */
    NOT_COMPILATION
  }

  /** The compilers whose options differ. */
  public enum Compiler {
    /** javac and everything built on it. */
    JAVAC,
    /** Eclipse's compiler. */
    ECJ,
    /** The sjavac server. */
    SJAVAC,
    /** The Kotlin compiler (kotlinc). */
    KOTLIN,
    /** The Scala 3 compiler (dotc). */
    SCALA3,
    /** The Scala 2 compiler (nsc). */
    SCALA2
  }

  private final String name;
  private final String canonicalName;
  private final Value value;
  private final Repeat repeat;
  private final Effect effect;
  private final Compiler compiler;

  private CompilerOption(
      String name,
      String canonicalName,
      Value value,
      Repeat repeat,
      Effect effect,
      Compiler compiler) {
    this.name = name;
    this.canonicalName = canonicalName;
    this.value = value;
    this.repeat = repeat;
    this.effect = effect;
    this.compiler = compiler;
  }

  /** The option as the compiler spells it, e.g. {@code -cp}. */
  public String name() {
    return name;
  }

  /**
   * The name the option is recorded under, e.g. {@code --classpath}.
   *
   * <p>Synonyms share a canonical name: {@code -cp}, {@code -classpath} and {@code --class-path}
   * are all {@code --classpath}. That is what makes two captures comparable, and it is why the
   * compile identity does not change when a build switches between the spellings.
   */
  public String canonicalName() {
    return canonicalName;
  }

  public Value value() {
    return value;
  }

  public Repeat repeat() {
    return repeat;
  }

  public Effect effect() {
    return effect;
  }

  /** The compiler this option applies to, or {@code null} when it applies to all of them. */
  public Compiler compiler() {
    return compiler;
  }

  /** Whether the option takes a value. */
  public boolean takesValue() {
    return value != Value.NONE;
  }

  @Override
  public String toString() {
    return name;
  }

  // ------------------------------------------------------------------ the table

  /** Every option, in one table. */
  public static final List<CompilerOption> ALL = buildTable();

  /**
   * The options by the name the compiler spells them with, and the compiler they belong to.
   *
   * <p>The index is keyed by the name AND the compiler -- see the class comment.
   */
  private static final Map<String, Map<Compiler, CompilerOption>> BY_NAME = index();

  private static Map<String, Map<Compiler, CompilerOption>> index() {
    Map<String, Map<Compiler, CompilerOption>> out = new LinkedHashMap<>();
    for (CompilerOption option : ALL) {
      Map<Compiler, CompilerOption> perCompiler =
          out.computeIfAbsent(option.name(), k -> new LinkedHashMap<>());
      // An option that belongs to no compiler in particular is shared, and it
      // is the fallback for a compiler that has an option of its own.
      perCompiler.put(option.compiler(), option);
    }
    return out;
  }

  /**
   * The option with that name, or {@code null} when the compiler does not know it.
   *
   * <p>Without a compiler this is ambiguous for a name two compilers share, so the option that
   * belongs to no particular compiler is preferred and otherwise the first entry wins.
   */
  public static CompilerOption byName(String name) {
    Map<Compiler, CompilerOption> perCompiler = BY_NAME.get(name);
    if (perCompiler == null) {
      return null;
    }
    CompilerOption shared = perCompiler.get(null);
    if (shared != null) {
      return shared;
    }
    return perCompiler.values().iterator().next();
  }

  /**
   * The option with that name, for a given compiler.
   *
   * <p>The compiler matters. ECJ has options javac does not have ({@code -src}, {@code -profile},
   * {@code -log}). A global lookup would give javac an option it does not have, and the value
   * would be swallowed -- the option would vanish.
   *
   * <p>Where the two compilers agree -- on the classpath's three spellings, for instance -- the
   * option belongs to neither one and both lookups find it.
   *
   * @param name the option as the compiler spells it
   * @param compiler which compiler is doing the parsing
   * @return the option, or {@code null} when that compiler does not know it
   */
  public static CompilerOption byName(String name, Compiler compiler) {
    Map<Compiler, CompilerOption> perCompiler = BY_NAME.get(name);
    if (perCompiler == null) {
      return null;
    }
    // The compiler's own option wins over a shared one.
    CompilerOption own = perCompiler.get(compiler);
    if (own != null) {
      return own;
    }
    // An option that belongs to no compiler in particular applies to all of them.
    return perCompiler.get(null);
  }

  /**
   * The options of one compiler.
   *
   * <p>The path options differ between the compilers: ECJ's {@code bootclasspath} means something
   * different from javac's, and sjavac takes its own set. The options that apply to every compiler
   * are included in each compiler's list.
   */
  public static List<CompilerOption> forCompiler(Compiler compiler) {
    List<CompilerOption> out = new ArrayList<>();
    for (CompilerOption option : ALL) {
      if (option.compiler() == null || option.compiler() == compiler) {
        out.add(option);
      }
    }
    return out;
  }

  private static List<CompilerOption> buildTable() {
    List<CompilerOption> out = new ArrayList<>();

    buildJavaTable(out);
    buildKotlinTable(out);
    buildScalaTable(out);

    return Jdk8.copyOf(out);
  }

  // ------------------------------------------------------------------ Java

  private static void buildJavaTable(List<CompilerOption> out) {
    // ---- the output directory ----
    // javac: -d.  ECJ: -d.  sjavac: -d.
    out.add(opt("-d", "--output-dir", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(
        opt(
            "--output-dir",
            "--output-dir",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));

    // ---- the classpath ----
    // These are the three spellings javac accepts, and ECJ accepts the first two.
    out.add(
        opt(
            "-classpath",
            "--classpath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "--classpath",
            "--classpath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(opt("-cp", "--classpath", Value.PATH_LIST, Repeat.LAST_WINS, Effect.COMPILATION, null));
    // javac's own help lists "--class-path <path>, -classpath <path>, -cp
    // <path>" -- so the double-dash spelling belongs to javac too, and not
    // only to ECJ.  It was missing, so `javac --class-path ...` lost the
    // classpath: the option was unknown, and the value that followed it was
    // not a source file either, so it vanished entirely.
    out.add(
        opt(
            "--class-path",
            "--classpath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));

    // ---- the source path ----
    out.add(
        opt(
            "-sourcepath",
            "--sourcepath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "--sourcepath",
            "--sourcepath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "--source-path",
            "--sourcepath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));

    // ---- the boot class path ----
    out.add(
        opt(
            "-bootclasspath",
            "bootclasspath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "--boot-class-path",
            "bootclasspath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "-Xbootclasspath",
            "bootclasspath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));

    // ---- the extension and endorsed directories ----
    out.add(
        opt("-extdirs", "extdirs", Value.PATH_LIST, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(
        opt(
            "-Djava.ext.dirs",
            "extdirs",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "-endorseddirs",
            "endorseddirs",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "-Djava.endorsed.dirs",
            "endorseddirs",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    // The -Xbootclasspath/p: and /a: forms prepend and append.
    out.add(
        opt(
            "-Xbootclasspath/p",
            "prebootclasspath",
            Value.PATH_LIST,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "-Xbootclasspath/a",
            "postbootclasspath",
            Value.PATH_LIST,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            null));

    // ---- the module paths ----
    out.add(
        opt(
            "--module-path",
            "--module-path",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(opt("-p", "--module-path", Value.PATH_LIST, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(
        opt(
            "--module-source-path",
            "--module-source-path",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "--upgrade-module-path",
            "--upgrade-module-path",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));

    // ---- the modules themselves ----
    out.add(opt("--module", "--module", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(opt("-m", "--module", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(
        opt(
            "--limit-modules",
            "--limit-modules",
            Value.COMMA_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "--add-modules",
            "--add-modules",
            Value.COMMA_LIST,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "--add-exports",
            "--add-exports",
            Value.COMMA_LIST,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "--add-reads",
            "--add-reads",
            Value.COMMA_LIST,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "--patch-module",
            "--patch-module",
            Value.SINGLE,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            null));

    // ---- the system image ----
    out.add(opt("--system", "--system", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, null));

    // ---- the processor paths ----
    out.add(
        opt(
            "-processorpath",
            "--processor-path",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "--processor-path",
            "--processor-path",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(
        opt(
            "--processor-module-path",
            "--processor-module-path",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(opt("-processor", "--processor", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, null));

    // ---- the annotation processing directory ----
    out.add(
        opt(
            "-s",
            "--annotation-processing-dir",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));

    // ---- the language level ----
    out.add(
        opt("--release", "--release", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(opt("-source", "--source", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(opt("--source", "--source", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(opt("-target", "--target", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(opt("--target", "--target", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, null));

    // ---- the encoding ----
    out.add(
        opt("-encoding", "--encoding", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, null));

    // ---- the options that take no value ----
    out.add(
        opt(
            "--enable-preview",
            "--enable-preview",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));

    // ---- the options that do not compile anything ----
    // -Xprint prints the compiler's internal state; -proc:only runs only the processors.
    out.add(opt("-Xprint", "-Xprint", Value.NONE, Repeat.LAST_WINS, Effect.NOT_COMPILATION, null));
    out.add(
        opt(
            "-proc:only",
            "-proc:only",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.NOT_COMPILATION,
            null));

    // ---- the annotation processing switch ----
    out.add(
        opt("-proc:none", "-proc:none", Value.NONE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(
        opt("-proc:full", "-proc:full", Value.NONE, Repeat.LAST_WINS, Effect.COMPILATION, null));

    // ---- the implicit compilation switch ----
    out.add(
        opt("-implicit:none", "-implicit", Value.NONE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(
        opt(
            "-implicit:class",
            "-implicit",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));

    // sjavac spells the same switch differently: `-implicit:` with the value
    // in the NEXT token, so `-implicit: none` and not `-implicit:none`.
    out.add(
        opt(
            "-implicit:",
            "--implicit",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SJAVAC));

    // ---- the module of the files that are created ----
    out.add(
        opt(
            "--default-module-for-created-files",
            "--default-module-for-created-files",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));

    // ---- the annotation processor options ----
    out.add(opt("-A", "-A", Value.SINGLE, Repeat.ALL_APPLY, Effect.COMPILATION, null));

    // ---- the line comments ----
    out.add(
        opt(
            "--disable-line-doc-comments",
            "--disable-line-doc-comments",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(opt("--help-lint", "--help-lint", Value.NONE, Repeat.LAST_WINS, Effect.COMPILATION, null));

    // ---- the debug-info switch ----
    out.add(opt("-g:none", "-g", Value.NONE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(opt("-g:lines", "-g", Value.NONE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(opt("-g:vars", "-g", Value.NONE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(opt("-g:source", "-g", Value.NONE, Repeat.LAST_WINS, Effect.COMPILATION, null));

    // ---- ECJ's own set ----
    out.add(opt("-h", "-h", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(opt("-i", "-i", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, Compiler.ECJ));
    out.add(opt("-j", "-j", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, Compiler.ECJ));
    out.add(opt("-log", "-log", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, Compiler.ECJ));
    out.add(
        opt(
            "--module-version",
            "--module-version",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.ECJ));
    out.add(
        opt(
            "-profile",
            "-profile",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.ECJ));
    out.add(opt("-tr", "-tr", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, Compiler.ECJ));
    out.add(opt("-x", "-x", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, Compiler.ECJ));
    out.add(
        opt(
            "-Xmaxerrs",
            "-Xmaxerrs",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.ECJ));
    out.add(
        opt(
            "-Xmaxwarns",
            "-Xmaxwarns",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.ECJ));
    out.add(
        opt(
            "-Xstdout",
            "-Xstdout",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.ECJ));
    out.add(
        opt("-copy", "-copy", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, Compiler.ECJ));

    // ---- the source directories ----
    // ECJ's -src names a directory of sources rather than a file.
    out.add(
        opt(
            "-src",
            "source_files_or_dirs",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.ECJ));

    // ---- the options measured against the real ECJ ----
    //
    // `-annotationpath` is the one that matters: it names the external
    // annotations that support annotation-based null analysis, so it changes
    // what the compiler checks.  Without it in the table the option was
    // unknown AND the path that followed it was not a source file either --
    // so both the option and its value vanished, exactly like the missing
    // `--class-path` cost javac its whole classpath.
    out.add(
        opt(
            "-annotationpath",
            "annotationpath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.ECJ));

    // ECJ's spelling of the warning switch.
    out.add(
        opt(
            "-warn:none",
            "-warn:none",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.ECJ));

    // The advanced warning options.
    out.add(
        opt(
            "-?:warn",
            "-?:warn",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.ECJ));
    out.add(
        opt(
            "-help:warn",
            "-help:warn",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.ECJ));

    // ---- the options measured against the real sjavac ----
    //
    // sjavac was removed from the JDK after Java 11, so these were measured
    // against the real `com.sun.tools.sjavac.options.Option` enum of a JDK 11.
    //
    // THE KEY FACT: sjavac has its OWN option table and it CONSUMES those
    // options itself.  Everything sjavac does not recognise is forwarded to
    // javac.  Measured on the real compiler:
    //
    //   sjavac consumes: -src --source-path -sourcepath --module-path -p
    //                    --class-path -classpath -cp -x -i -tr -copy -j
    //                    --server: --startserver: -implicit: --log= -verbose
    //                    --permit-artifact= --permit-unidentified-artifacts
    //                    --permit-sources-without-package
    //                    --compare-found-sources -d -s -h --state-dir=
    //
    //   sjavac forwards to javac: -encoding -source -target --release -g
    //                    -nowarn -proc:none -processor -processorpath
    //                    -bootclasspath --add-modules -Xlint -parameters -A
    //
    // A THIRD fact: the value of an sjavac option is taken from the NEXT
    // argument, not from the same token.  `-j 2` works and `-j=2` is
    // REJECTED.
    out.add(
        opt(
            "-src",
            "source_files_or_dirs",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SJAVAC));
    out.add(opt("-x", "-x", Value.SINGLE, Repeat.ALL_APPLY, Effect.COMPILATION, Compiler.SJAVAC));
    out.add(opt("-i", "-i", Value.SINGLE, Repeat.ALL_APPLY, Effect.COMPILATION, Compiler.SJAVAC));
    out.add(opt("-tr", "-tr", Value.SINGLE, Repeat.ALL_APPLY, Effect.COMPILATION, Compiler.SJAVAC));
    out.add(
        opt("-copy", "-copy", Value.SINGLE, Repeat.ALL_APPLY, Effect.COMPILATION, Compiler.SJAVAC));
    // The number of cores.  It does not change the class files, but it IS a
    // value-taking option, so it has to be recognised -- otherwise the number
    // after it is read as a source file.
    out.add(opt("-j", "-j", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, Compiler.SJAVAC));
    out.add(
        opt(
            "--server:",
            "--server",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SJAVAC));
    out.add(
        opt(
            "--startserver:",
            "--startserver",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SJAVAC));
    out.add(
        opt(
            "--permit-artifact=",
            "--permit-artifact",
            Value.SINGLE,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            Compiler.SJAVAC));
    out.add(
        opt(
            "--permit-unidentified-artifacts",
            "--permit-unidentified-artifacts",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SJAVAC));
    out.add(
        opt(
            "--permit-sources-without-package",
            "--permit-sources-without-package",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SJAVAC));
    out.add(
        opt(
            "--compare-found-sources",
            "--compare-found-sources",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SJAVAC));
    out.add(
        opt(
            "--state-dir=",
            "--state-dir",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SJAVAC));

    // The logging level.  sjavac spells it with a trailing "=", but it takes
    // the value from the NEXT token.
    out.add(
        opt(
            "--log=",
            "--log",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SJAVAC));

    // ---- the options that only affect diagnostics ----
    out.add(opt("-nowarn", "-nowarn", Value.NONE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(opt("-verbose", "-verbose", Value.NONE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(
        opt(
            "-deprecation",
            "-deprecation",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            null));
    out.add(opt("-Xlint", "-Xlint", Value.SINGLE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(opt("-Werror", "-Werror", Value.NONE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    // The value is IN the spelling -- `-g:lines` names what to include.
    out.add(opt("-g", "-g", Value.NONE, Repeat.LAST_WINS, Effect.COMPILATION, null));
    out.add(
        opt("-parameters", "-parameters", Value.NONE, Repeat.LAST_WINS, Effect.COMPILATION, null));
  }

  // ------------------------------------------------------------------ Kotlin

  /**
   * The Kotlin compiler's options.
   *
   * <p><b>The Kotlin options were measured against the real compiler</b>, the same way ECJ's and
   * sjavac's were: the Kotlin compiler's argument names come from the generated argument classes,
   * and the values that matter for a static analyser are the classpath, the output directory, the
   * language version, the JVM target and the source roots.
   *
   * <p><b>Why the Kotlin table is mostly its own.</b> Kotlin's option names overlap javac's only
   * by accident. {@code -d} means the output directory in both, but {@code -cp} in Kotlin means the
   * classpath while {@code -source} in Kotlin means the source root -- a completely different thing
   * from javac's {@code -source} (the language level). Sharing the javac entry would have recorded
   * the Kotlin source root as a language level, which is confidently wrong.
   */
  private static void buildKotlinTable(List<CompilerOption> out) {
    // ---- the output directory ----
    // Kotlin's -d takes a directory, or a .jar for a packaged output.
    out.add(
        opt(
            "-d",
            "--output-dir",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.KOTLIN));

    // ---- the classpath ----
    out.add(
        opt(
            "-classpath",
            "--classpath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.KOTLIN));
    out.add(
        opt(
            "-cp",
            "--classpath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.KOTLIN));

    // ---- the source roots ----
    // Kotlin's -source names a source ROOT (a directory), not a language
    // level.  It is repeatable: several roots accumulate.
    out.add(
        opt(
            "-source",
            "--source-root",
            Value.PATH_LIST,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            Compiler.KOTLIN));
    out.add(
        opt(
            "-sources",
            "--source-root",
            Value.PATH_LIST,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            Compiler.KOTLIN));

    // ---- the language version ----
    out.add(
        opt(
            "-language-version",
            "--language-version",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.KOTLIN));

    // ---- the JVM target ----
    out.add(
        opt(
            "-jvm-target",
            "--jvm-target",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.KOTLIN));

    // ---- the API version ----
    out.add(
        opt(
            "-api-version",
            "--api-version",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.KOTLIN));

    // ---- the module name ----
    out.add(
        opt(
            "-module-name",
            "--module-name",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.KOTLIN));

    // ---- the friend paths ----
    // The paths whose internals this module may see.  They are part of the
    // compile: a friend path widens what the compiler can resolve.
    out.add(
        opt(
            "-Xfriend-paths",
            "--friend-paths",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.KOTLIN));

    // ---- the plugin path and the plugin options ----
    out.add(
        opt(
            "-Xplugin",
            "--plugin-path",
            Value.PATH_LIST,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            Compiler.KOTLIN));
    out.add(
        opt(
            "-P",
            "--plugin-option",
            Value.SINGLE,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            Compiler.KOTLIN));

    // ---- the Java source roots ----
    // Kotlin compiles Java sources too when they are named or found in a
    // source root, and they end up in the class file output.
    out.add(
        opt(
            "-Xjava-source-roots",
            "--java-source-roots",
            Value.PATH_LIST,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            Compiler.KOTLIN));

    // ---- the options that take no value ----
    out.add(
        opt(
            "-include-runtime",
            "--include-runtime",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.KOTLIN));
    out.add(
        opt(
            "-no-stdlib",
            "--no-stdlib",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.KOTLIN));
    out.add(
        opt(
            "-no-reflect",
            "--no-reflect",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.KOTLIN));

    // ---- the options that do not compile anything ----
    // -version and -help print and exit; -Xprint-... dumps the compiler's
    // internal state.
    out.add(
        opt(
            "-version",
            "--version",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.NOT_COMPILATION,
            Compiler.KOTLIN));
    out.add(
        opt(
            "-help",
            "--help",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.NOT_COMPILATION,
            Compiler.KOTLIN));
    out.add(
        opt(
            "-Xprint-",
            "--print",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.NOT_COMPILATION,
            Compiler.KOTLIN));

    // ---- the diagnostics ----
    out.add(
        opt(
            "-Werror",
            "-Werror",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.KOTLIN));
    out.add(
        opt(
            "-nowarn",
            "-nowarn",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.KOTLIN));
  }

  // ------------------------------------------------------------------ Scala

  /**
   * The Scala compilers' options.
   *
   * <p><b>The Scala options were measured against the real compilers</b> -- {@code scalac -help}
   * on both Scala 3 (dotc) and Scala 2 (nsc).
   *
   * <p><b>Why Scala 3 and Scala 2 get separate tables.</b> They disagree on the spellings that
   * matter. Scala 3 spells the classpath {@code -classpath} and the source path {@code -sourcepath}
   * (as one word); Scala 2 spells the source path {@code -sourcepath} too but adds {@code
   * -dependencyfile}, and Scala 3 adds {@code -from-tasty} and {@code -semanticdb-target}. Where
   * they agree the option is shared; where they differ each gets its own entry.
   */
  private static void buildScalaTable(List<CompilerOption> out) {
    // ---- the output directory ----
    // Scala's -d takes a directory or a jar.
    out.add(
        opt(
            "-d",
            "--output-dir",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-d",
            "--output-dir",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));

    // ---- the classpath ----
    //
    // Scala's classpath is the one that varies: a build that assembles it
    // on one platform and compiles on another hands over a comma-separated
    // list, so both separators are accepted.
    out.add(
        opt(
            "-classpath",
            "--classpath",
            Value.PATH_LIST_OR_COMMA,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-classpath",
            "--classpath",
            Value.PATH_LIST_OR_COMMA,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));
    out.add(
        opt(
            "-cp",
            "--classpath",
            Value.PATH_LIST_OR_COMMA,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-cp",
            "--classpath",
            Value.PATH_LIST_OR_COMMA,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));

    // ---- the boot class path ----
    out.add(
        opt(
            "-bootclasspath",
            "bootclasspath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-bootclasspath",
            "bootclasspath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));

    // ---- the source path ----
    // Scala spells it as one word.  It is repeatable: several source roots
    // accumulate, unlike javac's -sourcepath which is last-wins.
    out.add(
        opt(
            "-sourcepath",
            "--sourcepath",
            Value.PATH_LIST,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-sourcepath",
            "--sourcepath",
            Value.PATH_LIST,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            Compiler.SCALA2));

    // ---- the encoding ----
    out.add(
        opt(
            "-encoding",
            "--encoding",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-encoding",
            "--encoding",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));

    // ---- the language and target levels ----
    // Scala 3 spells them -source and -target, and they take a value with a
    // colon: -source:3.0.  The colon spelling is a SEPARATE table entry,
    // not a prefix match: the attached-value lookup finds the longest
    // option name the argument starts with, and "-source" would match
    // "-source:3.0" first and leave the value as ":3.0".
    out.add(
        opt(
            "-source",
            "--source",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-source:",
            "--source",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-target",
            "--target",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-target:",
            "--target",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    // Scala 2 uses -target:jvm-1.8 and has no -source of its own.
    out.add(
        opt(
            "-target",
            "--target",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));
    out.add(
        opt(
            "-target:",
            "--target",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));

    // ---- the plugin options ----
    // Scala's -P passes an option to a plugin: -P:plugin:option=value.
    out.add(
        opt(
            "-P",
            "--plugin-option",
            Value.SINGLE,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-P",
            "--plugin-option",
            Value.SINGLE,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            Compiler.SCALA2));

    // ---- the plugin classpath ----
    out.add(
        opt(
            "-Xplugin",
            "--plugin-path",
            Value.PATH_LIST,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-Xplugin",
            "--plugin-path",
            Value.PATH_LIST,
            Repeat.ALL_APPLY,
            Effect.COMPILATION,
            Compiler.SCALA2));

    // ---- the priority classpath ----
    // Scala's -priorityclasspath takes precedence over all other paths.
    out.add(
        opt(
            "-priorityclasspath",
            "--priority-classpath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));

    // ---- the extension directories ----
    out.add(
        opt(
            "-extdirs",
            "extdirs",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-extdirs",
            "extdirs",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));

    // ---- Scala 3's own options ----
    // -from-tasty compiles from TASTy files instead of sources.
    out.add(
        opt(
            "-from-tasty",
            "--from-tasty",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    // -semanticdb-target names where the SemanticDB files go.
    out.add(
        opt(
            "-semanticdb-target",
            "--semanticdb-target",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    // -sourceroot names the workspace root.
    out.add(
        opt(
            "-sourceroot",
            "--sourceroot",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    // -usejavacp puts java.class.path on the classpath.
    out.add(
        opt(
            "-usejavacp",
            "--usejavacp",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    // -javabootclasspath is Scala 3's spelling of the Java boot class path.
    out.add(
        opt(
            "-javabootclasspath",
            "javabootclasspath",
            Value.PATH_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));

    // ---- Scala 2's own options ----
    // -dependencyfile names the dependency-tracking file.
    out.add(
        opt(
            "-dependencyfile",
            "--dependencyfile",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));
    // -Ystop-after stops the compiler after a given phase.
    out.add(
        opt(
            "-Ystop-after",
            "--stop-after",
            Value.SINGLE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));
    // -Ycache-plugin-class-loader is last-wins but takes no value.
    out.add(
        opt(
            "-Ycache-plugin-class-loader",
            "--cache-plugin-class-loader",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));

    // ---- the options that do not compile anything ----
    out.add(
        opt(
            "-version",
            "--version",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.NOT_COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-version",
            "--version",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.NOT_COMPILATION,
            Compiler.SCALA2));
    out.add(
        opt(
            "-help",
            "--help",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.NOT_COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-help",
            "--help",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.NOT_COMPILATION,
            Compiler.SCALA2));

    // ---- the diagnostics ----
    out.add(
        opt(
            "-Werror",
            "-Werror",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-Werror",
            "-Werror",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));
    out.add(
        opt(
            "-deprecation",
            "-deprecation",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-deprecation",
            "-deprecation",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));
    out.add(
        opt(
            "-nowarn",
            "-nowarn",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-nowarn",
            "-nowarn",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));
    out.add(
        opt(
            "-verbose",
            "-verbose",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-verbose",
            "-verbose",
            Value.NONE,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));
    // -Xlint takes a list of lint names.
    out.add(
        opt(
            "-Xlint",
            "-Xlint",
            Value.COMMA_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA3));
    out.add(
        opt(
            "-Xlint",
            "-Xlint",
            Value.COMMA_LIST,
            Repeat.LAST_WINS,
            Effect.COMPILATION,
            Compiler.SCALA2));
  }

  private static CompilerOption opt(
      String name,
      String canonicalName,
      Value value,
      Repeat repeat,
      Effect effect,
      Compiler compiler) {
    return new CompilerOption(name, canonicalName, value, repeat, effect, compiler);
  }

  /**
   * The options whose values are paths, split into their entries.
   *
   * <p>The separator is platform-dependent, and the drive letter is part of the path. On Windows
   * the separator is {@code ;} and a path looks like {@code D:\cache\repo.jar}; on Unix it is
   * {@code :} and a path looks like {@code /home/u/repo.jar}. Splitting on both separators at once
   * turns {@code D:\cache\repo.jar} into {@code D} and {@code \cache\repo.jar} -- two paths that
   * are both wrong, and the first one happens to look like a valid relative path, so the breakage
   * is easy to miss.
   *
   * @param value the raw option value
   * @return the paths, empty when the value is empty
   */
  public static List<Path> splitPath(String value) {
    List<Path> out = new ArrayList<>();
    if (value == null || Jdk8.isBlank(value)) {
      return out;
    }
    char separator = isWindows() ? ';' : ':';
    for (String part : value.split(java.util.regex.Pattern.quote(String.valueOf(separator)))) {
      String trimmed = part.trim();
      if (!trimmed.isEmpty()) {
        try {
          out.add(Jdk8.path(trimmed));
        } catch (RuntimeException e) {
          // Not a path -- skip it rather than failing the whole capture.
        }
      }
    }
    return out;
  }

  /**
   * The options whose values are paths that may be separated by either the platform separator or a
   * comma.
   *
   * <p><b>Why the comma is only a separator when it really is one.</b> A Unix path may contain a
   * comma, and a Windows path may contain one too. Splitting blindly on the comma would cut such a
   * path in half and produce two paths that are both wrong.
   *
   * <p><b>The rule.</b> A comma separates two paths only when <em>both</em> sides look like a
   * complete path: each side must contain a path separator, or carry a file extension. So
   * {@code /opt/a.jar,/opt/b.jar} splits (both sides have a separator), while
   * {@code /opt/my,lib/repo.jar} does not (the left side has neither a separator nor an extension).
   *
   * @param value the raw option value
   * @return the paths
   */
  public static List<Path> splitPathOrComma(String value) {
    List<Path> out = new ArrayList<>();
    if (value == null || Jdk8.isBlank(value)) {
      return out;
    }
    char separator = isWindows() ? ';' : ':';
    for (String part : value.split(java.util.regex.Pattern.quote(String.valueOf(separator)))) {
      for (String piece : splitCommaSegments(part.trim())) {
        if (!piece.isEmpty()) {
          try {
            out.add(Jdk8.path(piece));
          } catch (RuntimeException e) {
            // Not a path -- skip it.
          }
        }
      }
    }
    return out;
  }

  /**
   * Split one segment on its commas, keeping the commas that belong to a path.
   *
   * <p><b>The rule.</b> A comma separates two paths only when the <em>left</em> side looks like a
   * complete path entry: it must carry a file extension, or be a bare directory that the build
   * named deliberately.
   *
   * <p>So {@code a.jar,b.jar} splits (both carry an extension) and {@code /opt/a,/opt/b} does not
   * (neither carries an extension, and a directory on the classpath is rare enough that guessing
   * is worse than keeping the path whole).
   *
   * <p>And {@code /opt/my,lib/repo.jar} does not split either: the left side has no extension, so
   * the comma is part of the path. Splitting it would produce {@code /opt/my} and {@code
   * lib/repo.jar} -- the first one a directory that was never on the classpath, the second one a
   * relative path that resolves against the wrong directory.
   */
  private static List<String> splitCommaSegments(String segment) {
    List<String> out = new ArrayList<>();
    if (segment.isEmpty()) {
      return out;
    }
    int start = 0;
    for (int i = 0; i < segment.length(); i++) {
      if (segment.charAt(i) != ',') {
        continue;
      }
      String left = segment.substring(start, i);
      String right = segment.substring(i + 1);
      // "C:" alone is a drive letter, not a path -- the comma after it is
      // part of the path.
      if (isDriveLetterOnly(left)) {
        continue;
      }
      if (!looksLikeCompleteEntry(left) || !looksLikeCompleteEntry(right)) {
        continue;
      }
      out.add(left);
      start = i + 1;
    }
    out.add(segment.substring(start));
    return out;
  }

  /**
   * Whether a segment looks like a complete classpath entry.
   *
   * <p>An entry is complete when it carries a file extension -- {@code a.jar}, {@code b.jar},
   * {@code classes/}. A path without an extension is a directory, and a directory in the middle of
   * a comma-separated list is far more likely to be part of a path than a deliberate entry.
   */
  private static boolean looksLikeCompleteEntry(String segment) {
    if (segment.isEmpty()) {
      return false;
    }
    int dot = segment.lastIndexOf('.');
    // A leading dot is a hidden file, not an extension. A dot in a
    // directory name is not an extension either, so the last segment is
    // what counts.
    int lastSeparator = Math.max(segment.lastIndexOf('/'), segment.lastIndexOf('\\'));
    if (dot <= lastSeparator) {
      return false;
    }
    return dot < segment.length() - 1;
  }

  /** Whether a segment is a bare drive letter such as {@code C:}. */
  private static boolean isDriveLetterOnly(String segment) {
    return segment.length() == 2
        && Character.isLetter(segment.charAt(0))
        && segment.charAt(1) == ':';
  }

  /**
   * The comma-separated entries of a value like {@code module/package=other/package}.
   *
   * <p>These are not paths: {@code --add-exports} takes a list of module/package pairs, separated
   * by commas. Splitting them on the path separator would be wrong on Unix, where the separator is
   * a colon and the value contains no colon at all.
   */
  public static List<String> splitCommaList(String value) {
    List<String> out = new ArrayList<>();
    if (value == null || Jdk8.isBlank(value)) {
      return out;
    }
    for (String part : value.split(",")) {
      String trimmed = part.trim();
      if (!trimmed.isEmpty()) {
        out.add(trimmed);
      }
    }
    return out;
  }

  /** True when the platform separates classpath entries with {@code ;}. */
  public static boolean isWindows() {
    return System.getProperty("os.name", "").toLowerCase(java.util.Locale.ROOT).contains("win");
  }
}

