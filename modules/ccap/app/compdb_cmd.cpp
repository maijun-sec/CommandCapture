// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap - implementation of the compdb subcommands
//
// Subcommands:
//   list       list the known compiler types (the full list)
//   aliases   look up every alias of a compiler (symlinks are recognised)
//   capture    run a build and capture the compile commands
//   parse      parse compile commands from files
//   show       show the details of the parsed compile commands
//   export     generate compile_commands.json
//   translate  generate analyzer-style translated commands
//
// Key point: **does not depend on ccap_capture** --
// except for the `capture` subcommand (it has to run a build),
// every other subcommand only consumes command line text from files.

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ccap_compdb.h"
#include "cli.h"
#include "core/event.h"
#include "core/json.h"
#include "core/normalize.h"
#include "capture/platform.h"

namespace ccap {

namespace compdb {

// ═══════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Read a file (with diagnostics)
bool readInput(const std::string& path, std::string* out, std::string* err) {
    if (!readFile(path, out)) {
        *err = "cannot read " + path;
        return false;
    }
    return true;
}

// Decide whether a file is a build log or plain text
//
// Criterion: a build log contains "EXECUTING " or "COMPILING " lines.
// The marker is unambiguous, so there is no false positive.
bool looksLikeBuildLog(const std::string& text) {
    return text.find("EXECUTING ") != std::string::npos ||
           text.find("COMPILING ") != std::string::npos ||
           text.find("CWD = ") != std::string::npos;
}

// Decide whether a file is JSON
bool looksLikeJson(const std::string& text) {
    size_t b = 0;
    while (b < text.size() && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    return b < text.size() && text[b] == '{' || text[b] == '[';
}

// Build the compiler table from the config
CompilerTable buildCompilerTable(const CompdbConfig& config) {
    CompilerTable table = CompilerTable::builtin();

    for (const auto& c : config.compilers) {
        CompilerRule r;
        if (!compilerKindFromName(c.kind, &r.kind)) {
            r.kind = CompilerKind::Generic;
        }
        r.id = c.id.empty() ? c.kind : c.id;
        r.exactNames = c.names;
        r.prefixRules = c.prefixes;
        r.enabled = c.enabled;
        // The basename of a path is added to the exact names as well
        for (const auto& p : c.paths) {
            std::string base = baseName(p);
            if (!base.empty()) r.exactNames.push_back(base);
        }
        table.addRule(r);
    }

    return table;
}

// Merge the filter configuration
FilterConfig mergeFilter(const CompdbConfig& config, const cli::Options& opts,
                           bool keepScripts) {
    FilterConfig f = FilterConfig::builtin();

    // Exclusions from the config
    for (const auto& n : config.filter.excludeNames) {
        f.excludeNames.push_back(n);
    }
    for (const auto& n : config.filter.excludePrefixes) {
        f.excludePrefixes.push_back(n);
    }

    // Exclusions from the command line
    for (const auto& n : opts.compdbExclude) {
        f.excludeNames.push_back(n);
    }

    // Allowlist
    if (!config.filter.includeNames.empty()) {
        for (const auto& n : config.filter.includeNames) {
            f.includeNames.push_back(n);
        }
    }
    for (const auto& n : opts.compdbInclude) {
        f.includeNames.push_back(n);
    }

    // Scripts
    //
    // By default scripts are **excluded** -- the user explicitly asked
    // to "filter out the scripts and only get the real compile
    // commands". But the switch is kept, because sometimes the user
    // wants to see the whole picture.
    //
    // Note: detecting a script is an independent mechanism (is_script),
    // it is not "being in exclude_names" -- so only the flag is set here.
    f.keepScripts = keepScripts;

    return f;
}

// Build the adapter context
AdapterContext buildContext(const CompdbConfig& config,
                             const std::vector<CompileCommand>& commands) {
    AdapterContext ctx;
    for (const auto& cmd : commands) {
        auto it = config.systemIncludes.find(compilerKindName(cmd.kind));
        if (it != config.systemIncludes.end()) {
            for (const auto& d : it->second) {
                ctx.extraSystemIncludes.push_back(d);
            }
        }
    }
    return ctx;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// list -- the full list of compiler types
// ═══════════════════════════════════════════════════════════════════════════

int cmdList(const cli::Options& opts) {
    CompdbConfig config;
    std::string err;
    if (!opts.compdbConfig.empty()) {
        if (!CompdbConfig::loadFile(opts.compdbConfig, &config, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
    }

    CompilerTable table = buildCompilerTable(config);

    if (opts.format == "json") {
        std::string out = "{\n";
        out += "  \"tool\": \"CommandCapture\",\n";
        out += "  \"compilers\": [\n";
        bool first = true;
        for (const auto& r : table.rules()) {
            if (!first) out += ",\n";
            first = false;
            out += "    {\n";
            out += "      \"kind\": " + jsonQuote(compilerKindName(r.kind)) + ",\n";
            out += "      \"id\": " + jsonQuote(r.id) + ",\n";
            out += "      \"exact_names\": " + jsonStringArray(r.exactNames) + ",\n";
            out += "      \"prefixes\": " + jsonStringArray(r.prefixRules) + ",\n";
            out += "      \"suffixes\": " + jsonStringArray(r.suffixRules) + ",\n";
            out += std::string("      \"enabled\": ") + (r.enabled ? "true" : "false") + "\n";
            out += "    }";
        }
        out += "\n  ]\n}\n";
        std::fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    }

    // text
    std::printf("Known compiler types:\n\n");
    for (const auto& r : table.rules()) {
        std::printf("  %-8s (id: %s)\n", compilerKindName(r.kind), r.id.c_str());
        if (!r.exactNames.empty()) {
            std::printf("      names:  ");
            for (const auto& n : r.exactNames) std::printf("%s ", n.c_str());
            std::printf("\n");
        }
        if (!r.prefixRules.empty()) {
            std::printf("      prefix: ");
            for (const auto& n : r.prefixRules) std::printf("%s ", n.c_str());
            std::printf("\n");
        }
        if (!r.suffixRules.empty()) {
            std::printf("      suffix: ");
            for (const auto& n : r.suffixRules) std::printf("%s ", n.c_str());
            std::printf("\n");
        }
    }
    std::printf("\nVersioned names are recognized too: gcc-11, g++-12, clang-15 ...\n");
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// aliases -- look up every alias of a compiler
// ═══════════════════════════════════════════════════════════════════════════

int cmdAliases(const cli::Options& opts) {
    CompdbConfig config;
    std::string err;
    if (!opts.compdbConfig.empty()) {
        if (!CompdbConfig::loadFile(opts.compdbConfig, &config, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
    }

    CompilerTable table = buildCompilerTable(config);
    AliasResolver resolver;

    if (opts.compdbInputs.empty()) {
        std::fprintf(stderr, "error: need at least one compiler path\n");
        return 2;
    }

    // Add the compilers from the config first -- that way the alias
    // relations across configs become visible
    std::vector<std::string> names;
    for (const auto& c : config.compilers) {
        for (const auto& p : c.paths) names.push_back(p);
        for (const auto& n : c.names) names.push_back(n);
    }
    for (const auto& in : opts.compdbInputs) names.push_back(in);

    resolver.scanPathAliases(names);

    for (const auto& in : opts.compdbInputs) {
        std::string resolved = AliasResolver::resolveRealPath(in);
        auto id = resolver.identify(in, table);

        std::printf("%s\n", in.c_str());
        std::printf("  real path: %s\n", resolved.c_str());
        std::printf("  kind:      %s\n", compilerKindName(id.kind));
        if (!id.ruleId.empty()) {
            std::printf("  rule:      %s\n", id.ruleId.c_str());
        }

        const auto& al = resolver.aliasesOf(in);
        if (al.empty()) {
            std::printf("  aliases:   (none known)\n");
        } else {
            std::printf("  aliases:   ");
            for (const auto& a : al) std::printf("%s ", a.c_str());
            std::printf("\n");
        }
        std::printf("\n");
    }

    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Parsing sources
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Parse the compile commands out of a file
//
// The format is detected automatically:
//   - a build log (EXECUTING / COMPILING / CWD = format)
//   - compile_commands.json
//   - ccap's own ndjson event stream
//   - plain text (one command per line)
std::vector<CompileCommand> parseFileAuto(const CommandParser& parser,
                                             const std::string& path,
                                             std::string* err) {
    std::string text;
    if (!readInput(path, &text, err)) return {};

    if (looksLikeJson(text)) {
        // It may be a compile_commands.json (array) or a ccap event
        // stream (one object per line)
        if (text.find("\"events\"") != std::string::npos) {
            return parser.parseCaptureEvents(text);
        }
        // The top level is an array -> compilation database
        size_t b = 0;
        while (b < text.size() && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
        if (b < text.size() && text[b] == '[') {
            return parser.parseCompilationDb(text);
        }
        return parser.parseCaptureEvents(text);
    }

    if (looksLikeBuildLog(text)) {
        return parser.parseBuildLog(text);
    }

    return parser.parseTextFile(text);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// parse -- parse and count
// ═══════════════════════════════════════════════════════════════════════════

int cmdParse(const cli::Options& opts) {
    CompdbConfig config;
    std::string err;
    if (!opts.compdbConfig.empty()) {
        if (!CompdbConfig::loadFile(opts.compdbConfig, &config, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
    }

    CommandParser parser;
    parser.setCompilerTable(buildCompilerTable(config));

    std::vector<CompileCommand> all;
    for (const auto& in : opts.compdbInputs) {
        auto cmds = parseFileAuto(parser, in, &err);
        if (!err.empty()) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
        all.insert(all.end(), cmds.begin(), cmds.end());
    }

    FilterRules rules(mergeFilter(config, opts, opts.compdbIncludeScripts));

    size_t kept = 0;
    std::map<std::string, size_t> byCompiler;
    for (const auto& c : all) {
        if (rules.keep(c)) {
            ++kept;
            byCompiler[c.compilerBase]++;
        }
    }

    if (opts.format == "json") {
        std::string out = "{\n";
        out += "  \"tool\": \"CommandCapture\",\n";
        out += "  \"total_commands\": " + std::to_string(all.size()) + ",\n";
        out += "  \"compile_commands\": " + std::to_string(kept) + ",\n";
        out += "  \"by_compiler\": {";
        bool first = true;
        for (const auto& kv : byCompiler) {
            if (!first) out += ", ";
            first = false;
            out += jsonQuote(kv.first) + ": " + std::to_string(kv.second);
        }
        out += "}\n}\n";
        std::fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    }

    std::printf("Parsed %zu command(s), %zu are compilation command(s).\n\n",
                all.size(), kept);
    for (const auto& kv : byCompiler) {
        std::printf("  %-20s %zu\n", kv.first.c_str(), kv.second);
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// show -- show the details
// ═══════════════════════════════════════════════════════════════════════════

int cmdShow(const cli::Options& opts) {
    CompdbConfig config;
    std::string err;
    if (!opts.compdbConfig.empty()) {
        if (!CompdbConfig::loadFile(opts.compdbConfig, &config, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
    }

    CommandParser parser;
    parser.setCompilerTable(buildCompilerTable(config));

    std::vector<CompileCommand> all;
    for (const auto& in : opts.compdbInputs) {
        auto cmds = parseFileAuto(parser, in, &err);
        if (!err.empty()) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
        all.insert(all.end(), cmds.begin(), cmds.end());
    }

    FilterRules rules(mergeFilter(config, opts, opts.compdbIncludeScripts));

    for (const auto& c : all) {
        bool keep = opts.compdbShowAll ? true : rules.keep(c);
        std::printf("%s %s\n", keep ? "[KEEP]" : "[DROP]", c.reconstruct().c_str());
        if (!keep) {
            std::printf("       %s\n", rules.explain(c).c_str());
            continue;
        }
        std::printf("       kind:       %s\n", compilerKindName(c.kind));
        std::printf("       directory:  %s\n", c.directory.c_str());
        if (!c.compilerPath.empty()) {
            std::printf("       compiler:   %s\n", c.compilerPath.c_str());
        }
        if (!c.sourceFiles.empty()) {
            std::printf("       sources:");
            for (const auto& s : c.sourceFiles) std::printf(" %s", s.c_str());
            std::printf("\n");
        }
        if (!c.includeDirs.empty()) {
            std::printf("       includes:");
            for (const auto& s : c.includeDirs) std::printf(" %s", s.c_str());
            std::printf("\n");
        }
        if (!c.systemDirs.empty()) {
            std::printf("       sys_include:");
            for (const auto& s : c.systemDirs) std::printf(" %s", s.c_str());
            std::printf("\n");
        }
        if (!c.defines.empty()) {
            std::printf("       defines:");
            for (const auto& kv : c.defines) {
                std::printf(" %s", kv.first.c_str());
                if (!kv.second.empty()) std::printf("=%s", kv.second.c_str());
            }
            std::printf("\n");
        }
        if (!c.languageStandard.empty()) {
            std::printf("       std:        %s\n", c.languageStandard.c_str());
        }
        if (!c.preIncludes.empty()) {
            std::printf("       preinclude:");
            for (const auto& s : c.preIncludes) std::printf(" %s", s.c_str());
            std::printf("\n");
        }
        if (!c.forcedLanguage.empty()) {
            std::printf("       language:   %s (forced by the compiler)\n",
                        c.forcedLanguage.c_str());
        }
        if (!c.perFileLanguages.empty()) {
            std::printf("       language:  ");
            for (const auto& kv : c.perFileLanguages) {
                std::printf(" %s=%s", kv.first.c_str(), kv.second.c_str());
            }
            std::printf("\n");
        }
        if (!c.outputFile.empty()) {
            std::printf("       output:     %s\n", c.outputFile.c_str());
        }
        std::printf("\n");
    }

    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// export -- generate the clang compilation database
// ═══════════════════════════════════════════════════════════════════════════

int cmdExport(const cli::Options& opts) {
    CompdbConfig config;
    std::string err;
    if (!opts.compdbConfig.empty()) {
        if (!CompdbConfig::loadFile(opts.compdbConfig, &config, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
    }

    CommandParser parser;
    parser.setCompilerTable(buildCompilerTable(config));

    std::vector<CompileCommand> all;
    for (const auto& in : opts.compdbInputs) {
        auto cmds = parseFileAuto(parser, in, &err);
        if (!err.empty()) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
        all.insert(all.end(), cmds.begin(), cmds.end());
    }

    FilterRules rules(mergeFilter(config, opts, opts.compdbIncludeScripts));
    std::vector<CompileCommand> kept;
    for (const auto& c : all) {
        if (rules.keep(c)) kept.push_back(c);
    }

    std::string outPath = opts.compdbOutput.empty() ? config.outputPath
                                                       : opts.compdbOutput;
    std::string format = opts.compdbFormat.empty() ? config.outputFormat
                                                     : opts.compdbFormat;

    std::string content;
    if (format == "translated") {
        AdapterRegistry registry;
        AdapterContext ctx = buildContext(config, kept);
        content = exportTranslatedCommands(kept, registry, ctx);
    } else if (format == "clang-cmd") {
        content = exportClangCommands(kept);
    } else {
        content = exportClangCompilationDb(kept);
    }

    if (!writeFile(outPath, content)) {
        std::fprintf(stderr, "error: cannot write %s\n", outPath.c_str());
        return 3;
    }

    if (opts.verbose) {
        std::fprintf(stderr, "[ccap] wrote %zu compile command(s) to %s\n",
                     kept.size(), outPath.c_str());
    }
    if (opts.format == "json") {
        std::string out = "{\n";
        out += "  \"tool\": \"CommandCapture\",\n";
        out += "  \"output\": " + jsonQuote(outPath) + ",\n";
        out += "  \"format\": " + jsonQuote(format) + ",\n";
        out += "  \"compile_commands\": " + std::to_string(kept.size()) + ",\n";
        out += "  \"total_commands\": " + std::to_string(all.size()) + "\n";
        out += "}\n";
        std::fwrite(out.data(), 1, out.size(), stdout);
    } else {
        std::printf("Wrote %zu compile command(s) to %s\n", kept.size(),
                    outPath.c_str());
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// translate -- analyzer style
//
// The difference to export: the result goes straight to **stdout**,
// no file is written. The translated commands usually have to be piped
// into the front end that consumes them.
// ═══════════════════════════════════════════════════════════════════════════

int cmdTranslate(const cli::Options& opts) {
    CompdbConfig config;
    std::string err;
    if (!opts.compdbConfig.empty()) {
        if (!CompdbConfig::loadFile(opts.compdbConfig, &config, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
    }

    CommandParser parser;
    parser.setCompilerTable(buildCompilerTable(config));

    std::vector<CompileCommand> all;
    for (const auto& in : opts.compdbInputs) {
        auto cmds = parseFileAuto(parser, in, &err);
        if (!err.empty()) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
        all.insert(all.end(), cmds.begin(), cmds.end());
    }

    FilterRules rules(mergeFilter(config, opts, opts.compdbIncludeScripts));
    std::vector<CompileCommand> kept;
    for (const auto& c : all) {
        if (rules.keep(c)) kept.push_back(c);
    }

    AdapterRegistry registry;
    AdapterContext ctx = buildContext(config, kept);
    std::string content = exportTranslatedCommands(kept, registry, ctx);

    std::fwrite(content.data(), 1, content.size(), stdout);
    return 0;
}

}  // namespace compdb
}  // namespace ccap
