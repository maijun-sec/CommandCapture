// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap_compdb - the JVM capture parser
//
// Reads the JSON files the JVM capture agent writes and turns each of
// them into a CompileCommand.
//
// **Why a separate parser.** The capture agent's format is not a command
// line: it is the *effective* compile. That is the point of the agent --
// it records what the compiler actually did, not what it was asked to do
// -- so the information that matters (the classpath the compiler used, the
// sources it really compiled) is not in the argv at all.
//
// **Why the argv is still there.** Two reasons. A consumer that wants to
// replay the compile needs the exact command line. And the argv is what
// identifies the compiler's own option syntax, which the adapter needs.
//
// **Why the per-compiler parsing is not repeated here.** The agent
// already parsed the options -- it has a per-compiler option table, on the
// Java side, where the compiler's own semantics are known. Re-parsing
// here would mean two implementations of the same table, and the two
// would drift. So the structured fields are taken as they are.
//
// **Fully isolated**: this file knows the capture format and nothing else.

#include "ccap_compdb.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/json_parser.h"

namespace ccap {
namespace compdb {

namespace {

// The kind a capture's compiler name maps to.
//
// The capture records the compiler under the same name the compiler table
// uses, so the two join without a mapping table. The names are the ones
// the capture agent writes: javac, ecj, sjavac, kotlin, scala3, scala2.
CompilerKind kindOfCapture(const std::string& compiler) {
    CompilerKind kind = CompilerKind::Unknown;
    if (compilerKindFromName(compiler, &kind)) {
        return kind;
    }
    return CompilerKind::Unknown;
}

// The executable name a capture's compiler is invoked as.
//
// The capture does not record the executable's path -- it records what
// the compiler *is*, not how it was launched -- so the base name is
// reconstructed from the compiler kind. That is enough for the
// compiler table to match it.
std::string executableOf(CompilerKind kind, const std::string& compiler) {
    switch (kind) {
        case CompilerKind::Javac:  return "javac";
        case CompilerKind::Ecj:    return "ecj";
        case CompilerKind::Kotlin: return "kotlinc";
        case CompilerKind::Scala3: return "scalac";
        case CompilerKind::Scala2: return "scalac";
        default: return compiler;
    }
}

// The paths of a JSON array of strings.
std::vector<std::string> stringArray(const JsonValue* value) {
    if (!value) return {};
    if (value->isArray()) {
        std::vector<std::string> out;
        for (const auto& item : value->array) {
            if (item && item->isString()) {
                out.push_back(item->string);
            }
        }
        return out;
    }
    if (value->isString()) {
        return value->asStringArray();
    }
    return {};
}

// Turn one capture object into a compile command.
bool commandOfCapture(const JsonValue& cap, CompileCommand* out) {
    const std::string compiler = cap.get("compiler") ? cap.get("compiler")->asString() : "";
    if (compiler.empty()) {
        // A capture without a compiler is not a compile command.
        return false;
    }

    CompileCommand cc;
    cc.kind = kindOfCapture(compiler);
    cc.compilerBase = executableOf(cc.kind, compiler);
    cc.compilerPath = cc.compilerBase;

    // The working directory the compile ran in.
    if (const JsonValue* cwd = cap.get("workingDirectory")) {
        cc.directory = cwd->asString();
    }

    // The raw command line: the argv the compiler received.
    std::vector<std::string> argv = stringArray(cap.get("argv"));
    if (!argv.empty()) {
        cc.argv = argv;
    } else {
        // The API path has no argv of its own -- the options were handed
        // over as a list. The list is still there, under "argv", because
        // the capture agent records it there too.
        cc.argv.push_back(cc.compilerBase);
    }
    for (const auto& a : cc.argv) {
        cc.rawCommand += (cc.rawCommand.empty() ? "" : " ");
        cc.rawCommand += a;
    }

    // The sources the compiler really compiled.
    cc.sourceFiles = stringArray(cap.get("sourceFiles"));

    // The classpath, the source path, the module path and the processor
    // path.
    cc.classpathDirs = stringArray(cap.get("classpath"));
    cc.systemDirs = stringArray(cap.get("sourcePath"));
    cc.moduleDirs = stringArray(cap.get("modulePath"));
    cc.processorDirs = stringArray(cap.get("processorPath"));

    // The language level.
    if (const JsonValue* level = cap.get("languageLevel")) {
        cc.languageStandard = level->asString();
    }

    // The output directory.
    if (const JsonValue* outDir = cap.get("outputDirectory")) {
        cc.outputFile = outDir->asString();
    }

    // A capture that says it was not a compilation -- a "javac -version",
    // a "kotlinc -help" -- is not a compile command.
    if (const JsonValue* compilation = cap.get("compilation")) {
        if (!compilation->asBool(true)) {
            return false;
        }
    }

    *out = cc;
    return true;
}

}  // namespace

std::vector<CompileCommand> CommandParser::parseJvmCaptures(
    const std::string& text) const {
    std::vector<CompileCommand> out;

    // The agent writes **one JSON object per file**. A directory of them
    // is read file by file, and the concatenation of those files is what
    // arrives here -- so the text is a sequence of JSON objects, not one.
    //
    // Each object is parsed on its own. The naive approach -- parse the
    // whole text as one document -- fails on the second object, because
    // there is no comma between two top-level values.
    size_t pos = 0;
    while (pos < text.size()) {
        // Find the start of the next object.
        size_t open = text.find('{', pos);
        if (open == std::string::npos) break;

        // Find its end, counting the braces and ignoring the ones inside
        // a string.
        size_t i = open;
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        for (; i < text.size(); ++i) {
            char c = text[i];
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    inString = false;
                }
                continue;
            }
            if (c == '"') {
                inString = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) {
                    ++i;  // past the closing brace
                    break;
                }
            }
        }
        if (depth != 0) {
            // Unterminated object: the rest of the text is not usable.
            break;
        }

        std::string object = text.substr(open, i - open);
        std::string err;
        JsonPtr value = jsonParse(object, &err);
        if (value && value->isObject()) {
            CompileCommand cc;
            if (commandOfCapture(*value, &cc)) {
                out.push_back(cc);
            }
        }

        pos = i;
    }

    return out;
}

std::vector<CompileCommand> CommandParser::parseJvmCaptureDirectory(
    const std::string& directory) const {
    std::vector<CompileCommand> out;

    std::error_code ec;
    std::filesystem::path dir(directory);
    if (!std::filesystem::is_directory(dir, ec)) {
        return out;
    }

    // The files are read in name order, so the result is deterministic.
    // The name ends with the nanosecond timestamp, so the order is the
    // order the compiles happened in.
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".json") continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            // One unreadable file must not lose the others.
            continue;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        std::vector<CompileCommand> parsed = parseJvmCaptures(buffer.str());
        out.insert(out.end(), parsed.begin(), parsed.end());
    }

    return out;
}

}  // namespace compdb
}  // namespace ccap
