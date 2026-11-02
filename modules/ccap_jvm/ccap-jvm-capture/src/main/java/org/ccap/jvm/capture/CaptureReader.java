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
import java.util.Comparator;
import java.util.List;
import java.util.stream.Stream;

/**
 * Reads back the captures written by the agent.
 *
 * <p>This is the analyser-side counterpart of the agent: it turns the JSON files in a capture
 * directory back into {@link CaptureEntry} records.
 *
 * <p>The format is written by {@link CaptureEntry#toJson()}, which is deliberately a flat object
 * with string arrays -- so the reader is a small hand-rolled parser rather than a JSON library
 * dependency. The capture files are written from inside a compiler JVM, where a dependency would
 * have to be resolvable from an arbitrary classpath.
 */
public final class CaptureReader {

  /** The file names the reader picks up: a compiler name and a {@code .json} suffix. */
  private static final List<String> FILE_PREFIXES =
      Jdk8.of("javac-", "ecj-", "sjavac-", "kotlin-", "scala3-", "scala2-");

  private CaptureReader() {}

  /**
   * Read every capture in {@code directory}, oldest first.
   *
   * @param directory the capture directory
   * @return the parsed captures
   */
  public static List<CaptureEntry> readAll(Path directory) {
    List<CaptureEntry> out = new ArrayList<>();
    if (directory == null || !Files.isDirectory(directory)) {
      return out;
    }
    try (Stream<Path> stream = Files.list(directory)) {
      List<Path> files =
          stream
              .filter(p -> isCaptureFile(p.getFileName().toString()))
              .filter(p -> p.getFileName().toString().endsWith(".json"))
              .sorted(Comparator.comparingLong(CaptureReader::lastModified))
              .collect(java.util.stream.Collectors.toList());
      for (Path file : files) {
        CaptureEntry entry = read(file);
        if (entry != null) {
          out.add(entry);
        }
      }
    } catch (IOException e) {
      System.err.println(
          "[ccap-capture] could not list " + directory + ": " + e.getMessage());
    }
    return out;
  }

  /**
   * Whether a file name is one of the capture files.
   *
   * <p><b>Why the prefix is checked.</b> A capture directory may hold other files -- the build's own
   * logs, the analyser's output -- and picking those up would produce parse failures.
   */
  private static boolean isCaptureFile(String name) {
    for (String prefix : FILE_PREFIXES) {
      if (name.startsWith(prefix)) {
        return true;
      }
    }
    return false;
  }

  /**
   * Read the most recent capture in {@code directory}.
   *
   * @param directory the capture directory
   * @return the parsed capture, or {@code null} when there is none
   */
  public static CaptureEntry readLatest(Path directory) {
    List<CaptureEntry> all = readAll(directory);
    return all.isEmpty() ? null : all.get(all.size() - 1);
  }

  /**
   * Read the captures of one language.
   *
   * @param directory the capture directory
   * @param language the language: {@code java}, {@code kotlin} or {@code scala}
   * @return the parsed captures
   */
  public static List<CaptureEntry> readAll(Path directory, String language) {
    List<CaptureEntry> out = new ArrayList<>();
    for (CaptureEntry entry : readAll(directory)) {
      if (entry.language() != null && entry.language().equals(language)) {
        out.add(entry);
      }
    }
    return out;
  }

  /**
   * Read one capture file.
   *
   * @param file the JSON file
   * @return the parsed capture, or {@code null} when the file is unreadable
   */
  public static CaptureEntry read(Path file) {
    try {
      String json = Jdk8.readString(file, StandardCharsets.UTF_8);
      return parse(json);
    } catch (IOException e) {
      System.err.println("[ccap-capture] could not read " + file + ": " + e.getMessage());
      return null;
    }
  }

  /**
   * Parse the capture JSON.
   *
   * <p>The format is a flat object; every value is either a string, null, or an array of strings.
   * The parser looks a key up by name and reads its value, so an unknown key is ignored and a
   * missing key yields null -- a capture written by an older version still parses.
   */
  static CaptureEntry parse(String json) {
    if (json == null || Jdk8.isBlank(json)) {
      return null;
    }
    return new CaptureEntry(
        string(json, "api"),
        string(json, "compiler"),
        string(json, "language"),
        stringArray(json, "argv"),
        string(json, "encoding"),
        string(json, "languageLevel"),
        path(string(json, "outputDirectory")),
        paths(stringArray(json, "classpath")),
        paths(stringArray(json, "sourcePath")),
        paths(stringArray(json, "modulePath")),
        paths(stringArray(json, "processorPath")),
        paths(stringArray(json, "bootClassPath")),
        paths(stringArray(json, "sourceFiles")),
        stringArray(json, "addModules"),
        stringArray(json, "addExports"),
        stringArray(json, "addReads"),
        bool(json, "preview"),
        bool(json, "compilation"),
        string(json, "javaVersion"),
        string(json, "javaHome"),
        string(json, "workingDirectory"));
  }

  /**
   * The value of a top-level boolean key.
   *
   * <p>Absent means false, so a capture written before a flag existed still parses.
   */
  static boolean bool(String json, String key) {
    String marker = "\"" + key + "\":";
    int at = json.indexOf(marker);
    if (at < 0) {
      return false;
    }
    int i = at + marker.length();
    while (i < json.length() && Character.isWhitespace(json.charAt(i))) {
      i++;
    }
    return i < json.length() && json.charAt(i) == 't';
  }

  private static Path path(String s) {
    return s == null ? null : Jdk8.path(s);
  }

  private static List<Path> paths(List<String> values) {
    List<Path> out = new ArrayList<>(values.size());
    for (String v : values) {
      out.add(Jdk8.path(v));
    }
    return out;
  }

  /**
   * The value of a top-level string key.
   *
   * <p>Returns null when the key is absent or its value is the JSON literal {@code null}.
   */
  static String string(String json, String key) {
    String marker = "\"" + key + "\":";
    int at = json.indexOf(marker);
    if (at < 0) {
      return null;
    }
    int i = at + marker.length();
    while (i < json.length() && Character.isWhitespace(json.charAt(i))) {
      i++;
    }
    if (i >= json.length()) {
      return null;
    }
    if (json.charAt(i) == 'n' && json.startsWith("null", i)) {
      return null;
    }
    if (json.charAt(i) != '"') {
      // Not a string value -- read up to the next comma.
      int end = json.indexOf(',', i);
      if (end < 0) {
        end = json.length();
      }
      return json.substring(i, end).trim();
    }
    return unquote(json, i);
  }

  /** The value of a top-level array-of-strings key. */
  static List<String> stringArray(String json, String key) {
    String marker = "\"" + key + "\":";
    int at = json.indexOf(marker);
    if (at < 0) {
      return Jdk8.of();
    }
    int i = at + marker.length();
    while (i < json.length() && Character.isWhitespace(json.charAt(i))) {
      i++;
    }
    if (i >= json.length() || json.charAt(i) != '[') {
      return Jdk8.of();
    }
    int end = json.indexOf(']', i);
    if (end < 0) {
      end = json.length();
    }
    String body = json.substring(i + 1, end);

    List<String> out = new ArrayList<>();
    int p = 0;
    while (p < body.length()) {
      while (p < body.length()
          && (Character.isWhitespace(body.charAt(p)) || body.charAt(p) == ',')) {
        p++;
      }
      if (p >= body.length() || body.charAt(p) != '"') {
        break;
      }
      String value = unquote(body, p);
      out.add(value);
      // unquote returns the decoded value; find where the literal ended
      p = skipString(body, p) + 1;
    }
    return out;
  }

  /** The index just past the closing quote of the string that starts at {@code start}. */
  private static int skipString(String s, int start) {
    int i = start + 1;
    while (i < s.length()) {
      char c = s.charAt(i);
      if (c == '\\') {
        i += 2;
        continue;
      }
      if (c == '"') {
        return i;
      }
      i++;
    }
    return s.length();
  }

  /** Read the quoted string starting at {@code start} and decode its escapes. */
  private static String unquote(String s, int start) {
    int end = skipString(s, start);
    String raw = s.substring(start + 1, Math.min(end, s.length()));
    StringBuilder sb = new StringBuilder(raw.length());
    for (int i = 0; i < raw.length(); i++) {
      char c = raw.charAt(i);
      if (c != '\\' || i + 1 >= raw.length()) {
        sb.append(c);
        continue;
      }
      char n = raw.charAt(++i);
      // A plain switch, not a switch expression: the agent has to load
      // on Java 8, where the arrow form does not exist.
      switch (n) {
        case 'n':
          sb.append('\n');
          break;
        case 'r':
          sb.append('\r');
          break;
        case 't':
          sb.append('\t');
          break;
        case 'b':
          sb.append('\b');
          break;
        case 'f':
          sb.append('\f');
          break;
        case 'u':
          if (i + 4 < raw.length()) {
            sb.append((char) Integer.parseInt(raw.substring(i + 1, i + 5), 16));
            i += 4;
          }
          break;
        default:
          sb.append(n);
          break;
      }
    }
    return sb.toString();
  }

  private static long lastModified(Path p) {
    try {
      return Files.getLastModifiedTime(p).toMillis();
    } catch (IOException e) {
      return 0L;
    }
  }
}

