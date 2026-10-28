// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - Linux runner (LD_PRELOAD approach)
//
// Mechanism:
//   1. Locate ccap_shim.so (next to this program, or given by CCAP_SHIM)
//   2. Set LD_PRELOAD=<shim>, then fork/exec the target
//   3. The shim intercepts the execve family / posix_spawn family and writes
//      the command into a pipe
//   4. The parent process reads the pipe and builds the events
//
// Why the shim must "restore LD_PRELOAD":
//   execve inherits environment variables; if LD_PRELOAD is not removed,
//   deep child processes keep getting injected too, which is theoretically fine
//   (we want to capture the whole tree),
//   but for setuid programs or certain security-sensitive binaries LD_PRELOAD
//   is ignored or causes failure.
//   Our approach: the shim removes itself proactively inside the hook, so only
//   one layer is injected.
//   Capturing deep child processes relies on "the shim survives in the
//   environment after the parent is exec'd"? -- no, it does not,
//   so on Linux we take a different route: **do not remove it**, let the shim
//   be inherited with the environment,
//   thereby covering the whole process tree; only "replace its own entry" in
//   execve's envp to avoid duplication.
//   For setuid programs the kernel ignores LD_PRELOAD; this limit cannot be
//   bypassed.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "core/command_line.h"
#include "core/normalize.h"
#include "capture/linux_preload.h"

namespace ccap {

namespace {

// The shim writes events (JSON lines) through this fd
constexpr const char* kShimFdEnv = "CCAP_SHIM_FD";

std::string findShimPath() {
    const char* env = getenv("CCAP_SHIM");
    if (env && *env) {
        std::string p = env;
        if (fileExists(p)) return p;
    }

    // Look for ccap_shim.so in the same directory as this program
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string dir = dirName(std::string(buf));
        const char* names[] = {"/ccap_shim.so", "/libccap_shim.so"};
        for (const char* nm : names) {
            std::string cand = dir + nm;
            if (fileExists(cand)) return cand;
        }
    }

    // Then try a few common locations
    const char* guesses[] = {
        "/usr/local/lib/ccap_shim.so",
        "/usr/lib/ccap_shim.so",
        "./ccap_shim.so",
    };
    for (const char* g : guesses) {
        if (fileExists(g)) return g;
    }
    return "";
}

// Read NDJSON lines from the pipe and parse them into events
//
// ─────────────────────────────────────────────────────────────────────────
// The correct depth algorithm (it took two rewrites to get right)
//
// The shim reports **(pid, ppid)**, meaning:
//   "the process with pid P (whose parent is Q) is about to be replaced by a
//    new program"
//
// Note that exec / posix_spawn **do not change the pid** -- the process being
// replaced and the process replacing it are the same pid. So the **level of P
// is fixed** and does not change with the number of execs.
//
// Therefore:
//   depth(P) = depth(Q) + 1
//
// And Q is the ppid in the event. So looking up depth_of[ppid] is correct --
// what was wrong is the **fallback value** and the **registration of the root
// process**.
//
// It was written wrong twice before:
//   1. The fallback value was 1 -- so every event whose ppid was not in the
//      table (the root process and its exec chain) was counted as depth 1;
//      the root event had depth=0 but the processes it exec'd still had
//      depth 1.
//   2. The root event registered depth_of[pid]=0, but the shim events look up
//      depth_of[ppid] (ccap's own pid), which is not found -> it fell back to
//      the fallback value again.
//
// Now: ccap's own pid is pre-registered as -1 ("no level above ccap"),
// so the root process = -1 + 1 = 0, and every step of the root exec chain
// finds depth_of[ccap_pid].
//
// Events arrive in creation order, so the parent's depth is always already
// computed.
// ─────────────────────────────────────────────────────────────────────────
void drainShimPipe(int fd, EventCollector* collector,
                     std::unordered_map<uint32_t, int>* depthOf) {
    std::string buf;
    char chunk[4096];
    ssize_t n;

    auto handleLine = [&](const std::string& line) {
        if (line.empty() || !collector) return;
        // Simple parsing: the format we write ourselves is key:value separated
        // by \x1f
        CommandEvent e;
        e.platform = Platform::Linux;
        e.source = EventSource::Runtime;
        // kind must match the Windows side -- previously the kind of Linux
        // events was the empty string,
        // so downstream grouping by kind missed half of them.
        e.kind = "process";

        size_t pos = 0;
        while (pos < line.size()) {
            size_t sep = line.find('\x1f', pos);
            if (sep == std::string::npos) break;
            std::string key = line.substr(pos, sep - pos);
            size_t vpos = sep + 1;
            size_t vend = line.find('\x1f', vpos);
            if (vend == std::string::npos) vend = line.size();
            std::string val = line.substr(vpos, vend - vpos);

            if (key == "pid") e.pid = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
            else if (key == "ppid") e.ppid = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
            else if (key == "exe") e.executable = val;
            else if (key == "cwd") e.cwd = val;
            else if (key == "cmd") e.rawCommandLine = val;
            else if (key == "static") {
                // The shim detected that the target is a statically linked
                // ELF.
                //
                // Consequence: **child processes inside this process are never
                // captured**
                // (statically linked means no dynamic linker, so LD_PRELOAD
                // is completely ineffective).
                //
                // It must be reported -- otherwise the user sees a
                // "captured but incomplete" result
                // with no hint at all, and can only suspect that the tool is
                // broken.
                if (val == "1") {
                    e.notes.push_back(
                        "target is statically linked - LD_PRELOAD cannot hook it, "
                        "so any child processes it spawns will NOT be captured");
                }
            }
            else if (key == "argv") {
                size_t p2 = 0;
                while (p2 < val.size()) {
                    size_t s2 = val.find('\x1e', p2);
                    if (s2 == std::string::npos) s2 = val.size();
                    e.argv.push_back(val.substr(p2, s2 - p2));
                    p2 = s2 + 1;
                }
            }
            pos = vend + 1;
        }

        // depth = parent depth + 1
        //
        // depth_of holds "the level of the process with this pid".
        // ccap's own pid is pre-registered as -1 (see run_linux_preload),
        // so the root process works out to 0.
        //
        // The fallback value must be 0, not 1 -- the fallback means "the parent
        // process was not found",
        // and the parent process not being found can only mean the root
        // process level.
        if (e.pid && depthOf) {
            int depth = 0;
            auto it = depthOf->find(e.ppid);
            if (it != depthOf->end()) depth = it->second + 1;
            e.depth = depth;
            // The level of a pid does not change across exec -- every event
            // produced by the same pid exec'ing repeatedly
            // is at the same level.
            (*depthOf)[e.pid] = depth;
        }

        // resolved: only counts as resolved when argv[0] is an absolute path
        // or contains a path separator.
        // The shim reports the basename, so argv[0] has to be used to decide.
        if (!e.argv.empty()) {
            e.resolved = isAbsolutePath(e.argv[0]) ||
                          e.argv[0].find('/') != std::string::npos;
        }

        if (e.pid) collector->add(std::move(e));
    };

    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        buf.append(chunk, static_cast<size_t>(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            handleLine(buf.substr(0, nl));
            buf.erase(0, nl + 1);
        }
    }
    if (!buf.empty()) handleLine(buf);
}

}  // namespace

namespace {

// Read a 4-byte little-endian integer out of the ELF
uint32_t rd32Le(const std::string& buf, size_t off) {
    if (off + 4 > buf.size()) return 0;
    return static_cast<uint32_t>(static_cast<unsigned char>(buf[off])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(buf[off + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(buf[off + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(buf[off + 3])) << 24);
}

// Check the target ELF and decide whether it is **certain** to be uncapturable.
//
// This is a kernel-level limit, not something we can work around:
//   - statically linked: no dynamic linker, LD_PRELOAD is completely
//     ineffective
//   - setuid/setgid: the kernel drops LD_PRELOAD
//
// Telling the user up front is far better than "silently capturing nothing" --
// the latter makes people think the tool is broken.
std::vector<std::string> diagnoseTarget(const std::string& exePath) {
    std::vector<std::string> notes;

    if (exePath.empty()) return notes;

    std::string content;
    if (!readFile(exePath, &content)) return notes;
    if (content.size() < 64) return notes;

    // ELF magic
    if (content.compare(0, 4, "\x7f" "ELF") != 0) return notes;

    // ELF class is at offset 4: 1 = 32-bit, 2 = 64-bit.
    // Only 64-bit is supported (the program header offset of a 32-bit ELF is
    // different; when the shim does not cover it, notes act as the fallback).
    bool is64 = (static_cast<unsigned char>(content[4]) == 2);
    if (!is64) return notes;

    // PT_INTERP (3) present in the program header table -> dynamically linked
    // PT_DYNAMIC (2) present -> it is the dynamic linker itself
    // e_phoff / e_phentsize / e_phnum
    auto rd64 = [&](size_t off) -> uint64_t {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) {
            v = (v << 8) | static_cast<unsigned char>(content[off + i]);
        }
        return v;
    };
    auto rd16 = [&](size_t off) -> uint16_t {
        return static_cast<uint16_t>(
            static_cast<unsigned char>(content[off]) |
            (static_cast<unsigned char>(content[off + 1]) << 8));
    };

    uint64_t phoff = rd64(32);
    uint16_t phentsize = rd16(54);
    uint16_t phnum = rd16(56);

    bool hasInterp = false;
    bool isDynamicLinker = false;

    for (uint16_t i = 0; i < phnum; ++i) {
        size_t ph = static_cast<size_t>(phoff) + static_cast<size_t>(i) * phentsize;
        if (ph + 56 > content.size()) break;
        uint32_t pType = static_cast<uint32_t>(rd32Le(content, ph));
        if (pType == 3) hasInterp = true;        // PT_INTERP
        if (pType == 2) isDynamicLinker = true; // PT_DYNAMIC
    }

    // Sign of a statically linked binary: no PT_INTERP
    if (!hasInterp && !isDynamicLinker) {
        notes.push_back(
            "target appears statically linked - LD_PRELOAD cannot hook it "
            "(no dynamic linker involved)");
    }

    // setuid / setgid bits
    // st_mode comes after e_ident: e_type(2) e_machine(2) e_version(4)
    //   e_entry(8) e_phoff(8) e_shoff(8) e_flags(4) e_ehsize(2) ...
    // e_flags is at offset 48 (4 bytes)
    uint32_t eFlags = static_cast<uint32_t>(rd32Le(content, 48));
    constexpr uint32_t kSetuid = 04000;
    constexpr uint32_t kSetgid = 02000;
    if (eFlags & kSetuid) {
        notes.push_back(
            "target is setuid - the kernel drops LD_PRELOAD for it "
            "(not bypassable)");
    }
    if (eFlags & kSetgid) {
        notes.push_back(
            "target is setgid - the kernel drops LD_PRELOAD for it "
            "(not bypassable)");
    }

    return notes;
}

}  // namespace

CaptureResult runLinuxPreload(const RunOptions& opts,
                                 EventCollector* collector) {
    CaptureResult result;

    // pid -> depth. Computed in place; events arrive in creation order, so the
    // parent arrives first.
    std::unordered_map<uint32_t, int> depthOf;

    // ccap itself is the top of the whole tree -- register it as -1,
    // so its child (the root process) works out to 0.
    depthOf[static_cast<uint32_t>(getpid())] = -1;

    std::string shim = findShimPath();
    if (shim.empty()) {
        result.error = "ccap_shim.so not found (set CCAP_SHIM or place next to ccap)";
        return result;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        result.error = std::string("pipe failed: ") + strerror(errno);
        return result;
    }

    // Resolve the target path early (done in the parent, so the diagnostics
    // can be handed back to the user)
    std::string resolvedTarget = resolveExecutable(opts.target);
    if (resolvedTarget.empty() && isAbsolutePath(opts.target)) {
        resolvedTarget = opts.target;
    }

    // Diagnose whether the target is certain to be uncapturable
    // (statically linked / setuid)
    for (const auto& n : diagnoseTarget(resolvedTarget)) {
        result.notes.push_back(n);
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        result.error = std::string("fork failed: ") + strerror(errno);
        return result;
    }

    if (pid == 0) {
        // Child process
        close(pipefd[0]);
        // dup the write end onto a fixed fd
        if (pipefd[1] != 3) {
            dup2(pipefd[1], 3);
            close(pipefd[1]);
        }

        setenv("LD_PRELOAD", shim.c_str(), 1);
        setenv(kShimFdEnv, "3", 1);

        // Redirect stdout to stderr (optional) --
        // ccap's own stdout keeps only the capture data, so the JSON can be
        // consumed directly from a pipe.
        if (opts.redirectOutputToStderr) {
            dup2(STDERR_FILENO, STDOUT_FILENO);
        }

        // Target command line
        std::string target = opts.target;
        std::vector<std::string> argv;
        std::string ext = extensionOf(target);

        if (ext == ".sh" || ext == ".bash") {
            std::string bash = resolveExecutable("bash");
            if (bash.empty()) bash = "/bin/bash";
            argv.push_back(bash);
            argv.push_back(target);
        } else if (ext == ".py") {
            std::string py = resolveExecutable("python3");
            if (py.empty()) py = "python3";
            argv.push_back(py);
            argv.push_back(target);
        } else {
            std::string p = resolveExecutable(target);
            argv.push_back(p.empty() ? target : p);
        }
        for (const auto& a : opts.args) argv.push_back(a);

        if (!opts.cwd.empty()) {
            if (chdir(opts.cwd.c_str()) != 0) {
                // Ignore it, let the target report the error itself
            }
        }

        std::vector<char*> cargv;
        for (auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
        cargv.push_back(nullptr);

        execv(cargv[0], cargv.data());
        // exec failed
        _exit(127);
    }

    // Parent process
    close(pipefd[1]);

    // **The root command must be recorded by the parent process itself.**
    //
    // Reason: the forked child is ccap itself (it does not link the shim),
    // so when it calls execv the shim is not loaded yet -- that exec cannot
    // be intercepted.
    // The shim can only intercept execs that happen "after it is loaded".
    //
    // Before this was added, the result was:
    //   ccap run /bin/true          -> 0 events
    //   ccap run /bin/sh -c '/bin/true' -> 1 event (the sh layer was exec'd
    //   after the shim was loaded)
    {
        // Build the root event from the command line the child is about to
        // exec
        std::string rootExe;
        std::vector<std::string> rootArgv;
        {
            std::string target = opts.target;
            std::string ext = extensionOf(target);
            if (ext == ".sh" || ext == ".bash") {
                std::string bash = resolveExecutable("bash");
                if (bash.empty()) bash = "/bin/bash";
                rootExe = bash;
                rootArgv.push_back(bash);
                rootArgv.push_back(target);
            } else if (ext == ".py") {
                std::string py = resolveExecutable("python3");
                if (py.empty()) py = "python3";
                rootExe = py;
                rootArgv.push_back(py);
                rootArgv.push_back(target);
            } else {
                std::string p = resolveExecutable(target);
                rootExe = p.empty() ? target : p;
                rootArgv.push_back(rootExe);
            }
            for (const auto& a : opts.args) rootArgv.push_back(a);
        }

        CommandEvent rootEv;
        rootEv.timestampMs = nowUnixMs();
        rootEv.platform = Platform::Linux;
        rootEv.source = EventSource::Runtime;
        rootEv.pid = static_cast<uint32_t>(pid);
        rootEv.ppid = static_cast<uint32_t>(getpid());
        rootEv.executable = baseName(rootExe);
        rootEv.argv = rootArgv;
        for (size_t i = 0; i < rootArgv.size(); ++i) {
            if (i) rootEv.rawCommandLine += ' ';
            rootEv.rawCommandLine += rootArgv[i];
        }
        rootEv.cwd = opts.cwd.empty() ? currentDir() : opts.cwd;
        rootEv.kind = "process";
        // The root event's depth uses exactly the same algorithm as the shim
        // events:
        //   depth = depth_of[ppid] + 1
        // ccap's own pid is already registered as -1, so this yields 0.
        //
        // It must not be hardcoded to 0 -- that way the shim events exec'd
        // later by the same pid
        // (looking up depth_of[ccap_pid] = -1 -> 0) would not match the root
        // event.
        {
            int d = 0;
            auto it = depthOf.find(rootEv.ppid);
            if (it != depthOf.end()) d = it->second + 1;
            rootEv.depth = d;
        }
        rootEv.resolved = !rootExe.empty() && rootExe.find('/') != std::string::npos;
        rootEv.notes.push_back("root (reported by ccap: the first exec happens "
                                "before LD_PRELOAD takes effect)");
        if (collector) collector->add(std::move(rootEv));
        depthOf[static_cast<uint32_t>(pid)] = rootEv.depth;
    }

    // Read the pipe (while also waiting for the child to exit)
    // Use poll to read and wait at the same time, so a full pipe cannot
    // deadlock
    {
        struct pollfd pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        int64_t start = nowUnixMs();
        bool done = false;

        while (!done) {
            int timeout = 100;
            if (opts.timeoutMs > 0) {
                int64_t el = nowUnixMs() - start;
                if (el > opts.timeoutMs) {
                    result.timedOut = true;
                    kill(pid, SIGKILL);
                    timeout = 0;
                }
            }
            int rc = poll(&pfd, 1, timeout);
            if (rc > 0 && (pfd.revents & POLLIN)) {
                drainShimPipe(pipefd[0], collector, &depthOf);
            }
            if (rc > 0 && (pfd.revents & POLLHUP)) {
                drainShimPipe(pipefd[0], collector, &depthOf);
                done = true;
            }
            // Check the child process status
            int status = 0;
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                // The process has exited, but there may still be pipe data
                drainShimPipe(pipefd[0], collector, &depthOf);
                if (WIFEXITED(status)) {
                    result.exitCode = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    result.exitCode = 128 + WTERMSIG(status);
                }
                done = true;
            }
        }
    }

    close(pipefd[0]);

    // Wait once more to make sure the child is reaped
    if (result.exitCode < 0) {
        int status = 0;
        if (waitpid(pid, &status, 0) == pid) {
            if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
        }
    }

    // The depth was already computed in place in drain_shim_pipe (events
    // arrive in creation order, the parent first)
    result.notes.push_back("shim=" + shim);
    return result;
}

}  // namespace ccap
