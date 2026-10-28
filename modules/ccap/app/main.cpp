// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// ccap - main entry point
//
// Usage:
//   ccap run <script|exe> [args...]        run the target and capture
//   ccap scan <script>                     static analysis of the script only
//   ccap compdb <subcommand>               compile command tool
//   ccap --help
//
// Output:
//   --format json    a single JSON document (default)
//   --format ndjson  one JSON object per line
//   --format text    human readable

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "ccap_compdb.h"
#include "cli.h"
#include "core/event.h"
#include "core/filter.h"
#include "core/json.h"
#include "core/normalize.h"
#include "core/script_scanner.h"
#include "core/tree.h"
#include "capture/platform.h"

namespace ccap {
namespace compdb {
// Subcommand implementations (in compdb_cmd.cpp)
int cmdList(const cli::Options& opts);
int cmdAliases(const cli::Options& opts);
int cmdParse(const cli::Options& opts);
int cmdShow(const cli::Options& opts);
int cmdExport(const cli::Options& opts);
int cmdTranslate(const cli::Options& opts);
}  // namespace compdb
}  // namespace ccap

namespace {

using ccap::CommandEvent;
using ccap::EventCollector;

std::string eventToJson(const CommandEvent& e) {
    std::string j = "{";
    j += ccap::jsonKvNum("seq", static_cast<int64_t>(e.seq));
    j += ",";
    j += ccap::jsonKvNum("timestamp_ms", e.timestampMs);
    j += ",";
    j += ccap::jsonKvStr("platform", ccap::platformName(e.platform));
    j += ",";
    j += ccap::jsonKvStr("source", ccap::sourceName(e.source));
    j += ",";
    j += ccap::jsonKvNum("pid", e.pid);
    j += ",";
    j += ccap::jsonKvNum("ppid", e.ppid);
    j += ",";
    j += ccap::jsonKvNum("depth", e.depth);
    j += ",";
    j += ccap::jsonKvStr("kind", e.kind);
    j += ",";
    j += ccap::jsonKvStr("executable", e.executable);
    j += ",";
    j += ccap::jsonKv("argv", ccap::jsonStringArray(e.argv));
    j += ",";
    j += ccap::jsonKvStr("command_line", e.rawCommandLine);
    j += ",";
    j += ccap::jsonKvStr("cwd", e.cwd);
    j += ",";
    j += ccap::jsonKvBool("is_builtin", e.isBuiltin);
    j += ",";
    j += ccap::jsonKvBool("resolved", e.resolved);
    j += ",";
    j += ccap::jsonKvBool("truncated", e.truncated);
    j += ",";
    j += ccap::jsonKvStr("script_path", e.scriptPath);
    j += ",";
    j += ccap::jsonKvNum("script_line", e.scriptLine);
    j += ",";
    j += ccap::jsonKvNum("exec_index", e.execIndex);
    j += ",";
    j += ccap::jsonKv("notes", ccap::jsonStringArray(e.notes));
    j += "}";
    return j;
}

std::string eventToText(const CommandEvent& e) {
    std::string out;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "[%06llu] ",
                  static_cast<unsigned long long>(e.seq));
    out += buf;

    std::snprintf(buf, sizeof(buf), "d=%d ", e.depth);
    out += buf;

    out += (e.source == ccap::EventSource::Runtime) ? "RT " : "ST ";

    if (e.pid) {
        std::snprintf(buf, sizeof(buf), "pid=%u ", e.pid);
        out += buf;
    } else {
        out += "pid=- ";
    }

    out += e.executable;
    for (size_t i = 1; i < e.argv.size(); ++i) {
        out += " ";
        out += e.argv[i];
    }

    if (e.source == ccap::EventSource::Static && !e.scriptPath.empty()) {
        out += "   [";
        out += e.scriptPath;
        out += ":";
        out += std::to_string(e.scriptLine);
        out += "]";
    }
    if (e.isBuiltin) out += "   (builtin)";
    return out;
}

// Assemble the event stream into the output
std::string renderOutput(const ccap::cli::Options& cli,
                          const std::vector<CommandEvent>& events,
                          const ccap::RunResult& rr,
                          size_t filteredOut) {
    std::string out;

    if (cli.tree) {
        ccap::renderTree(events, &out);
    } else if (cli.format == "ndjson") {
        for (const auto& e : events) {
            out += eventToJson(e);
            out += "\n";
        }
    } else if (cli.format == "text") {
        for (const auto& e : events) {
            out += eventToText(e);
            out += "\n";
        }
    } else {
        // json
        out += "{\n";
        out += "  \"tool\": \"CommandCapture\",\n";
        out += "  \"platform\": ";
        out += ccap::jsonQuote(ccap::currentPlatformName());
        out += ",\n";
        out += "  \"target\": ";
        out += ccap::jsonQuote(cli.target);
        out += ",\n";
        out += "  \"exit_code\": ";
        out += std::to_string(rr.exitCode);
        out += ",\n";
        out += "  \"timed_out\": ";
        out += (rr.timedOut ? "true" : "false");
        out += ",\n";
        out += "  \"event_count\": ";
        out += std::to_string(events.size());
        out += ",\n";
        out += "  \"notes\": ";
        out += ccap::jsonStringArray(rr.notes);
        out += ",\n";

        // Summary statistics.
        //
        // The summary goes into a **JSON field**, not into appended text:
        // in json/ndjson mode the output must stay a valid document that a
        // consumer can parse, so appending a human readable block would
        // corrupt it. The text mode gets its own block below instead.
        if (cli.summary) {
            std::map<std::string, size_t> byExe;
            size_t runtimeN = 0, staticN = 0;
            int maxDepth = 0;
            for (const auto& e : events) {
                if (e.source == ccap::EventSource::Static) {
                    ++staticN;
                } else {
                    ++runtimeN;
                    byExe[e.executable]++;
                    if (e.depth > maxDepth) maxDepth = e.depth;
                }
            }
            out += "  \"summary\": {\n";
            out += "    \"runtime_events\": " + std::to_string(runtimeN) + ",\n";
            out += "    \"static_events\": " + std::to_string(staticN) + ",\n";
            out += "    \"max_depth\": " + std::to_string(maxDepth) + ",\n";
            out += "    \"unique_executables\": " + std::to_string(byExe.size()) + ",\n";
            out += "    \"executables\": {";
            bool firstExe = true;
            for (const auto& kv : byExe) {
                if (!firstExe) out += ", ";
                firstExe = false;
                out += ccap::jsonQuote(kv.first) + ": " + std::to_string(kv.second);
            }
            out += "}";
            if (filteredOut > 0) {
                out += ",\n    \"filtered_out\": " + std::to_string(filteredOut);
            }
            out += "\n  },\n";
        }

        out += "  \"events\": [\n";
        for (size_t i = 0; i < events.size(); ++i) {
            out += "    ";
            out += eventToJson(events[i]);
            if (i + 1 < events.size()) out += ",";
            out += "\n";
        }
        out += "  ]\n";
        out += "}\n";
    }

    // The summary block in text mode
    if (cli.summary && !cli.tree && cli.format == "text") {
        std::map<std::string, size_t> byExe;
        size_t runtimeN = 0, staticN = 0;
        int maxDepth = 0;
        for (const auto& e : events) {
            if (e.source == ccap::EventSource::Static) {
                ++staticN;
            } else {
                ++runtimeN;
                byExe[e.executable]++;
                if (e.depth > maxDepth) maxDepth = e.depth;
            }
        }
        out += "\n--- summary ---\n";
        out += "runtime events: " + std::to_string(runtimeN) + "\n";
        out += "static events:  " + std::to_string(staticN) + "\n";
        out += "max depth:      " + std::to_string(maxDepth) + "\n";
        out += "unique executables: " + std::to_string(byExe.size()) + "\n";
        for (const auto& kv : byExe) {
            out += "  " + kv.first + ": " + std::to_string(kv.second) + "\n";
        }
        if (filteredOut > 0) {
            out += "filtered out:   " + std::to_string(filteredOut) + "\n";
        }
    }

    return out;
}

}  // namespace

int main(int argc, char** argv) {
    ccap::cli::Options cli;
    std::string err;
    if (!ccap::cli::parse(argc, argv, &cli, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        ccap::cli::printUsage();
        return 2;
    }

    if (cli.command == ccap::cli::Command::Help) {
        ccap::cli::printUsage();
        return 0;
    }

    // ── compdb subcommands ───────────────────────────────────────────
    if (cli.command == ccap::cli::Command::Compdb) {
        using namespace ccap::compdb;
        switch (cli.compdbCommand) {
            case ccap::cli::CompdbCommand::List:
                return cmdList(cli);
            case ccap::cli::CompdbCommand::Aliases:
                return cmdAliases(cli);
            case ccap::cli::CompdbCommand::Parse:
                return cmdParse(cli);
            case ccap::cli::CompdbCommand::Show:
                return cmdShow(cli);
            case ccap::cli::CompdbCommand::Export:
                return cmdExport(cli);
            case ccap::cli::CompdbCommand::Translate:
                return cmdTranslate(cli);
            case ccap::cli::CompdbCommand::Capture:
                // capture needs to run a build -- reuse the capture
                // capability of run, then hand the result to the compdb
                // parser as an event stream.
                //
                // How: first write the capture result as ndjson into
                // memory, then go through the compdb parsing path --
                // that way there is **only one parsing path**, so two
                // implementations cannot drift apart.
                {
                    if (cli.compdbInputs.empty()) {
                        std::fprintf(stderr,
                                     "error: compdb capture needs a build command\n");
                        return 2;
                    }
                    // The first input element is the build command,
                    // the rest are its arguments
                    std::string buildCmd = cli.compdbInputs[0];
                    std::vector<std::string> buildArgs(
                        cli.compdbInputs.begin() + 1, cli.compdbInputs.end());

                    EventCollector collector;
                    ccap::RunOptions opts;
                    opts.target = buildCmd;
                    opts.args = buildArgs;
                    opts.cwd = cli.cwd;
                    opts.scanScript = false;      // only compile commands matter
                    opts.timeoutMs = cli.timeoutMs;
                    opts.quietMs = cli.quietMs;
                    opts.redirectOutputToStderr = true;

                    ccap::RunResult rr = ccap::runCapture(opts, &collector);

                    // events -> ndjson text
                    std::string ndjson;
                    for (const auto& e : collector.deduped()) {
                        ndjson += eventToJson(e);
                        ndjson += "\n";
                    }

                    // Go through the compdb parsing path
                    ccap::compdb::CompdbConfig config;
                    std::string cerr;
                    if (!cli.compdbConfig.empty()) {
                        if (!ccap::compdb::CompdbConfig::loadFile(
                                cli.compdbConfig, &config, &cerr)) {
                            std::fprintf(stderr, "error: %s\n", cerr.c_str());
                            return 2;
                        }
                    }
                    ccap::compdb::CommandParser parser;
                    ccap::compdb::CompilerTable table =
                        ccap::compdb::CompilerTable::builtin();
                    for (const auto& ce : config.compilers) {
                        ccap::compdb::CompilerRule r;
                        ccap::compdb::compilerKindFromName(ce.kind, &r.kind);
                        r.id = ce.id.empty() ? ce.kind : ce.id;
                        r.exactNames = ce.names;
                        r.prefixRules = ce.prefixes;
                        for (const auto& p : ce.paths) {
                            r.exactNames.push_back(ccap::baseName(p));
                        }
                        table.addRule(r);
                    }
                    parser.setCompilerTable(table);

                    auto cmds = parser.parseCaptureEvents(ndjson);

                    ccap::compdb::FilterConfig fc =
                        ccap::compdb::FilterConfig::builtin();
                    for (const auto& n : config.filter.excludeNames) {
                        fc.excludeNames.push_back(n);
                    }
                    for (const auto& n : cli.compdbExclude) {
                        fc.excludeNames.push_back(n);
                    }
                    if (!config.filter.includeNames.empty()) {
                        fc.includeNames = config.filter.includeNames;
                    }
                    for (const auto& n : cli.compdbInclude) {
                        fc.includeNames.push_back(n);
                    }
                    ccap::compdb::FilterRules rules(fc);

                    std::vector<ccap::compdb::CompileCommand> kept;
                    for (const auto& c : cmds) {
                        if (rules.keep(c)) kept.push_back(c);
                    }

                    std::string outPath = cli.compdbOutput.empty()
                                               ? config.outputPath
                                               : cli.compdbOutput;
                    std::string format = cli.compdbFormat.empty()
                                             ? config.outputFormat
                                             : cli.compdbFormat;

                    std::string content;
                    if (format == "translated") {
                        ccap::compdb::AdapterRegistry registry;
                        ccap::compdb::AdapterContext actx;
                        for (const auto& c : kept) {
                            auto it = config.systemIncludes.find(
                                ccap::compdb::compilerKindName(c.kind));
                            if (it != config.systemIncludes.end()) {
                                for (const auto& d : it->second) {
                                    actx.extraSystemIncludes.push_back(d);
                                }
                            }
                        }
                        content = ccap::compdb::exportTranslatedCommands(
                            kept, registry, actx);
                    } else if (format == "clang-cmd") {
                        content = ccap::compdb::exportClangCommands(kept);
                    } else {
                        content = ccap::compdb::exportClangCompilationDb(kept);
                    }

                    if (!outPath.empty() && outPath != "-") {
                        if (!ccap::writeFile(outPath, content)) {
                            std::fprintf(stderr, "error: cannot write %s\n",
                                         outPath.c_str());
                            return 3;
                        }
                        std::fprintf(stderr,
                                     "[ccap] wrote %zu compile command(s) to %s\n",
                                     kept.size(), outPath.c_str());
                    } else {
                        std::fwrite(content.data(), 1, content.size(), stdout);
                    }
                    return 0;
                }
            default:
                ccap::cli::printUsage();
                return 0;
        }
    }

    // ── run / scan ───────────────────────────────────────────────────
    if (cli.target.empty()) {
        std::fprintf(stderr, "error: no target specified\n");
        ccap::cli::printUsage();
        return 2;
    }

    EventCollector collector;
    ccap::RunResult rr;

    if (cli.command == ccap::cli::Command::Scan) {
        ccap::ScanOptions so;
        so.recursive = cli.recursive;
        so.maxDepth = cli.depth;
        ccap::scanScript(cli.target, so, &collector, 0);
        rr.ok = true;
    } else {
        ccap::RunOptions opts;
        opts.target = cli.target;
        opts.args = cli.args;
        opts.cwd = cli.cwd;
        opts.scanScript = cli.scan;
        opts.scriptRecursive = cli.recursive;
        opts.scriptDepth = cli.depth;
        opts.timeoutMs = cli.timeoutMs;
        opts.quietMs = cli.quietMs;
        opts.verbose = cli.verbose;
        opts.clearBeingDebugged = cli.clearBeingDebugged;
        opts.redirectOutputToStderr = cli.redirectOutputToStderr;

        rr = ccap::runCapture(opts, &collector);
    }

    auto events = collector.deduped();

    // ── filtering ────────────────────────────────────────────────────
    //
    // No filtering by default -- silently dropping events is the worst
    // failure mode. Filter only when the user explicitly passed
    // --exclude/--include, and the number of dropped events goes into
    // notes so the user knows how much was lost.
    ccap::EventFilter filter;
    for (const auto& x : cli.excludes) filter.excludeName(x);
    for (const auto& x : cli.excludeStrs) filter.excludeSubstr(x);
    for (const auto& x : cli.includes) filter.includeName(x);

    size_t rawCount = events.size();
    if (!filter.empty()) {
        events = filter.apply(events);
    }

    ccap::RunResult rrFiltered = rr;
    if (!filter.empty() && filter.excludedCount() > 0) {
        rrFiltered.notes.push_back(
            "filtered out " + std::to_string(filter.excludedCount()) +
            " event(s) by --exclude/--include (kept " +
            std::to_string(events.size()) + " of " +
            std::to_string(rawCount) + ")");
    }

    std::string out =
        renderOutput(cli, events, rrFiltered, filter.excludedCount());

    if (!cli.outFile.empty()) {
        if (!ccap::writeFile(cli.outFile, out)) {
            std::fprintf(stderr, "error: cannot write %s\n", cli.outFile.c_str());
            return 3;
        }
    } else {
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
    }

    if (cli.verbose) {
        std::fprintf(stderr, "[ccap] events=%llu exit=%d notes=%llu\n",
                     static_cast<unsigned long long>(events.size()),
                     rrFiltered.exitCode,
                     static_cast<unsigned long long>(rrFiltered.notes.size()));
        for (const auto& n : rrFiltered.notes) {
            std::fprintf(stderr, "[ccap] note: %s\n", n.c_str());
        }
    }

    return rr.ok ? 0 : 1;
}
