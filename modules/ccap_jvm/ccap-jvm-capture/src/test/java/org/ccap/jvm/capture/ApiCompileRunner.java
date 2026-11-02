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
import javax.tools.JavaCompiler;
import javax.tools.JavaFileObject;
import javax.tools.StandardJavaFileManager;
import javax.tools.ToolProvider;

/**
 * Compiles a file through the {@code javax.tools} API.
 *
 * <p><b>Why this exists as a separate class.</b> The API path reaches the compiler through a
 * different set of entry points than the command line -- {@code JavacTool#getTask} and then {@code
 * JavacTaskImpl#call} -- and those carry no options of their own. The options were set on the
 * task when it was created, so the capture has to remember them there.
 *
 * <p>So this class is compiled into the test tree and run in a separate JVM with the agent
 * attached, which is the only way to exercise the API path.
 */
public final class ApiCompileRunner {

  private ApiCompileRunner() {}

  /**
   * Compile one source file.
   *
   * @param args the source file, then the output directory
   */
  public static void main(String[] args) throws Exception {
    if (args.length < 2) {
      System.err.println("usage: ApiCompileRunner <source> <output dir>");
      System.exit(2);
    }
    Path source = Path.of(args[0]);
    Path out = Path.of(args[1]);

    JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();
    if (compiler == null) {
      System.err.println("no system java compiler");
      System.exit(3);
    }
    try (StandardJavaFileManager fileManager =
        compiler.getStandardFileManager(null, null, null)) {
      Iterable<? extends JavaFileObject> units =
          fileManager.getJavaFileObjects(source.toFile());
      List<String> options =
          List.of("-d", out.toString(), "-classpath", System.getProperty("java.class.path"));
      JavaCompiler.CompilationTask task =
          compiler.getTask(null, fileManager, null, options, null, units);
      boolean ok = task.call();
      if (!ok) {
        System.err.println("the compile failed");
        System.exit(1);
      }
    }
  }
}
