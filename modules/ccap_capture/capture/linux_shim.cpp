// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - LD_PRELOAD shim
//
// Intercepts the execve family and the posix_spawn family and writes the
// command that is about to be executed to the pipe given by CCAP_SHIM_FD.
//
// Why posix_spawn must be covered as well:
//   GNU make / ninja / node / cmake prefer posix_spawn (a glibc-internal
//   implementation that goes through no execve symbol at all), so hooking only
//   execve misses more than half of them.
//
// Why the hook has to handle LD_PRELOAD:
//   We **keep** LD_PRELOAD so it is inherited with the environment, thereby
//   covering the whole process tree.
//   But duplicate entries must be removed (after several execs the environment
//   may contain several of our shim paths).
//
// Note: the kernel ignores LD_PRELOAD for setuid/setgid programs; this is a
// kernel limit that cannot be bypassed.
//
// _GNU_SOURCE is provided uniformly by the build system (CMake's
// CXX_EXTENSIONS OFF means the C++ compiler does not define it automatically,
// so it has to be defined here) --
// wrapped in #ifndef to avoid a duplicate definition warning from the build
// system.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace {

// Original function pointers
using ExecveFn = int (*)(const char*, char* const[], char* const[]);
using ExecvFn = int (*)(const char*, char* const[]);
using ExecvpFn = int (*)(const char*, char* const[]);
using ExecvpeFn = int (*)(const char*, char* const[], char* const[]);
using ExecleFn = int (*)(const char*, const char*, ...);
using FexecveFn = int (*)(int, char* const[], char* const[]);
using ExecveatFn = int (*)(int, const char*, char* const[], char* const[], int);
using PosixSpawnFn = int (*)(pid_t*, const char*, const posix_spawn_file_actions_t*,
                            const posix_spawnattr_t*, char* const[], char* const[]);
using PosixSpawnpFn = int (*)(pid_t*, const char*, const posix_spawn_file_actions_t*,
                             const posix_spawnattr_t*, char* const[], char* const[]);

ExecveFn   gExecve   = nullptr;
ExecvFn    gExecv    = nullptr;
ExecvpFn   gExecvp   = nullptr;
ExecvpeFn  gExecvpe  = nullptr;
ExecleFn   gExecle   = nullptr;
FexecveFn  gFexecve  = nullptr;
ExecveatFn gExecveat = nullptr;
PosixSpawnFn  gPosixSpawn  = nullptr;
PosixSpawnpFn gPosixSpawnp = nullptr;

int gFd = -1;
bool gInited = false;

void initReal() {
    if (gInited) return;
    gInited = true;
    gExecve  = reinterpret_cast<ExecveFn>(dlsym(RTLD_NEXT, "execve"));
    gExecv   = reinterpret_cast<ExecvFn>(dlsym(RTLD_NEXT, "execv"));
    gExecvp  = reinterpret_cast<ExecvpFn>(dlsym(RTLD_NEXT, "execvp"));
    gExecvpe = reinterpret_cast<ExecvpeFn>(dlsym(RTLD_NEXT, "execvpe"));
    gExecle  = reinterpret_cast<ExecleFn>(dlsym(RTLD_NEXT, "execle"));
    gFexecve = reinterpret_cast<FexecveFn>(dlsym(RTLD_NEXT, "fexecve"));
    gExecveat = reinterpret_cast<ExecveatFn>(dlsym(RTLD_NEXT, "execveat"));
    gPosixSpawn  = reinterpret_cast<PosixSpawnFn>(dlsym(RTLD_NEXT, "posix_spawn"));
    gPosixSpawnp = reinterpret_cast<PosixSpawnpFn>(dlsym(RTLD_NEXT, "posix_spawnp"));

    const char* fd = getenv("CCAP_SHIM_FD");
    if (fd) {
        gFd = atoi(fd);
    }
}

// Remove duplicate LD_PRELOAD entries from envp
//
// The returned pointer must outlive the execve call -- so a **static buffer**
// is used, not a local std::vector (after the function returns out.data() is
// dangling,
// and execve then reads a wild pointer and reports "Bad address").
//
// A static buffer is not thread safe, but:
//   1. a successful execve never returns, so the buffer content is not
//      modified again;
//   2. the genuinely dangerous case is two threads entering execve at the
//      same time -- which is inherently racy anyway.
// For correctness, thread_local is used here.
static thread_local std::vector<char*> gDedupScratch;

char** dedupeLdPreload(char* const envp[]) {
    if (!envp) return const_cast<char**>(envp);

    std::vector<char*>& out = gDedupScratch;
    out.clear();
    for (int i = 0; envp[i]; ++i) {
        if (strncmp(envp[i], "LD_PRELOAD=", 11) == 0) {
            bool dup = false;
            for (char* existing : out) {
                if (strncmp(existing, "LD_PRELOAD=", 11) == 0) { dup = true; break; }
            }
            if (dup) continue;
        }
        out.push_back(envp[i]);
    }
    out.push_back(nullptr);
    return out.data();
}

std::string baseOf(const std::string& p) {
    size_t s = p.find_last_of('/');
    return (s == std::string::npos) ? p : p.substr(s + 1);
}

std::string cwdNow() {
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf))) return std::string(buf);
    return std::string();
}

// ─────────────────────────────────────────────────────────────────────────────
// Check whether the target ELF is statically linked
//
// Why this has to be done **in the shim** (and not only in the parent when
// diagnosing the root target):
//   The root target may be dynamically linked (so it is capturable), while its
//   **intermediate products** are statically linked --
//   for example some toolchains statically link their own helpers.
//   In that case the event **does appear** (the exec was hooked),
//   but the child processes inside that process are never captured --
//   the user sees a "captured but incomplete" result with no hint at all.
//
//   Checking in the shim lets **every** exec target be tagged,
//   so the user can see at which level the information starts being lost.
//
// Cost: only the file header is read (a few hundred bytes), not the whole
// file.
// ─────────────────────────────────────────────────────────────────────────────
bool isStaticElf(const char* path) {
    if (!path) return false;

    // Relative paths are resolved against cwd (execve allows relative paths)
    char resolved[PATH_MAX];
    if (path[0] != '/') {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return false;
        if (snprintf(resolved, sizeof(resolved), "%s/%s", cwd, path) >=
            static_cast<int>(sizeof(resolved))) {
            return false;
        }
    } else {
        if (snprintf(resolved, sizeof(resolved), "%s", path) >=
            static_cast<int>(sizeof(resolved))) {
            return false;
        }
    }

    int fd = open(resolved, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    // Read the file header: ELF header (64) + a few program headers
    unsigned char buf[512];
    ssize_t n = pread(fd, buf, sizeof(buf), 0);
    close(fd);
    if (n < 64) return false;

    // ELF magic
    if (buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        return false;
    }

    // Only handle 64-bit (the 32-bit program header offset is different, the
    // risk of a false verdict is high)
    if (buf[4] != 2) return false;

    auto rd64 = [&](size_t off) -> uint64_t {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | buf[off + i];
        return v;
    };
    auto rd16 = [&](size_t off) -> uint16_t {
        return static_cast<uint16_t>(buf[off] | (buf[off + 1] << 8));
    };

    uint64_t phoff = rd64(32);
    uint16_t phentsize = rd16(54);
    uint16_t phnum = rd16(56);

    bool hasInterp = false;
    bool isDynamic = false;

    for (uint16_t i = 0; i < phnum; ++i) {
        size_t ph = static_cast<size_t>(phoff) + static_cast<size_t>(i) * phentsize;
        if (ph + 4 > sizeof(buf)) break;
        uint32_t pType = static_cast<uint32_t>(buf[ph]) |
                          (static_cast<uint32_t>(buf[ph + 1]) << 8) |
                          (static_cast<uint32_t>(buf[ph + 2]) << 16) |
                          (static_cast<uint32_t>(buf[ph + 3]) << 24);
        if (pType == 3) hasInterp = true;         // PT_INTERP
        if (pType == 2) isDynamic = true;         // PT_DYNAMIC
    }

    // No PT_INTERP and not the dynamic linker itself -> statically linked
    return !hasInterp && !isDynamic;
}

void writeEvent(const char* path, char* const argv[]) {
    if (gFd < 0) return;

    // Diagnostic switch: with CCAP_SHIM_DEBUG=1 every hook is printed to
    // stderr.
    //
    // This is not leftover from the debugging phase -- when a capture fails
    // the user needs to know
    // "did the shim hook anything at all", and this environment variable is
    // the only observation point.
    // The pid printed is the pid of **the process that is about to be
    // replaced**,
    // so the same pid appearing multiple times = a process replacement
    // (exec), not multiple processes.
    if (getenv("CCAP_SHIM_DEBUG")) {
        char buf[512];
        int n = snprintf(buf, sizeof(buf),
                          "[shim] pid=%d fd=%d path=%s\n", (int)getpid(), gFd,
                          path ? path : "(null)");
        // It does not matter if the diagnostic output fails to be written
        // (stderr may already be closed), but it must not leave a warning.
        if (n > 0) {
            ssize_t ignored = write(2, buf, static_cast<size_t>(n));
            (void)ignored;
        }
    }

    // Static linking check -- placed first, because it decides whether this
    // event carries a diagnostic tag.
    bool staticLinked = false;
    if (path) {
        staticLinked = isStaticElf(path);
    }

    std::string line;
    char pidbuf[32], ppidbuf[32];
    snprintf(pidbuf, sizeof(pidbuf), "%d", (int)getpid());
    snprintf(ppidbuf, sizeof(ppidbuf), "%d", (int)getppid());

    line += "pid"; line += '\x1f'; line += pidbuf; line += '\x1f';
    line += "ppid"; line += '\x1f'; line += ppidbuf; line += '\x1f';
    line += "exe";  line += '\x1f'; line += baseOf(path ? path : ""); line += '\x1f';
    line += "cwd";  line += '\x1f'; line += cwdNow(); line += '\x1f';
    if (staticLinked) {
        line += "static"; line += '\x1f'; line += "1"; line += '\x1f';
    }

    // argv is separated by \x1e
    std::string argstr;
    if (argv) {
        for (int i = 0; argv[i]; ++i) {
            if (i) argstr += '\x1e';
            argstr += argv[i];
        }
    }
    line += "argv"; line += '\x1f'; line += argstr; line += '\x1f';

    // Full command line (joined with spaces)
    std::string cmd;
    if (argv) {
        for (int i = 0; argv[i]; ++i) {
            if (i) cmd += ' ';
            cmd += argv[i];
        }
    }
    line += "cmd"; line += '\x1f'; line += cmd; line += '\x1f';

    line += '\n';

    // One write, the pipe write is atomic (<= PIPE_BUF)
    size_t off = 0;
    while (off < line.size()) {
        ssize_t n = write(gFd, line.data() + off, line.size() - off);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += static_cast<size_t>(n);
    }
}

}  // namespace

extern "C" {

int execve(const char* path, char* const argv[], char* const envp[]) {
    initReal();
    writeEvent(path, argv);
    if (!gExecve) { errno = ENOSYS; return -1; }
    return gExecve(path, argv, dedupeLdPreload(envp));
}

int execv(const char* path, char* const argv[]) {
    initReal();
    writeEvent(path, argv);
    extern char** environ;
    if (!gExecv) { errno = ENOSYS; return -1; }
    return gExecv(path, argv);
}

int execvp(const char* file, char* const argv[]) {
    initReal();
    writeEvent(file, argv);
    if (!gExecvp) { errno = ENOSYS; return -1; }
    return gExecvp(file, argv);
}

int execvpe(const char* file, char* const argv[], char* const envp[]) {
    initReal();
    writeEvent(file, argv);
    if (!gExecvpe) { errno = ENOSYS; return -1; }
    return gExecvpe(file, argv, dedupeLdPreload(envp));
}

// The variadic layout of execle is: path, arg0, arg1, ..., NULL, envp
// In other words, there is an envp pointer after the NULL.
// The variadic arguments must all be collected in one go, so that the split
// can happen at the NULL.
int execle(const char* path, const char* arg0, ...) {
    initReal();

    std::vector<char*> all;
    all.push_back(const_cast<char*>(arg0));

    va_list ap;
    va_start(ap, arg0);
    for (;;) {
        char* a = va_arg(ap, char*);
        all.push_back(a);
        if (!a) break;
    }
    // The last element of all is now the NULL terminator
    va_end(ap);

    // Split: everything before the NULL is argv, everything after it is envp
    std::vector<char*> argv;
    std::vector<char*> envp;
    size_t i = 0;
    for (; i < all.size() && all[i]; ++i) argv.push_back(all[i]);
    argv.push_back(nullptr);
    // all[i] is the NULL; the elements after it are the first elements of envp
    for (size_t j = i + 1; j < all.size(); ++j) {
        if (!all[j]) break;
        envp.push_back(all[j]);
    }
    envp.push_back(nullptr);

    writeEvent(path, argv.data());

    if (!gExecle) { errno = ENOSYS; return -1; }
    // The variadic layout of execle: path, arg0, arg1, ..., NULL, envp
    // Prototype: int execle(const char *path, const char *arg, ...)
    //
    // glibc's implementation uses va_arg up to the NULL and then treats the
    // pointer after it as envp.
    // So we have to pass: exactly argc arguments + one NULL + envp.
    // Without the NULL, envp is taken as the (argc+1)-th argv element.
    size_t argc = argv.size() - 1;   // drop the trailing NULL
    const int kMax = 9;
    if (argc > static_cast<size_t>(kMax)) argc = static_cast<size_t>(kMax);

    const char* cargs[kMax + 1];
    for (size_t k = 0; k < argc; ++k) {
        cargs[k] = argv[k];
    }
    cargs[argc] = nullptr;   // the NULL terminator of argv

    switch (argc) {
        case 0: return gExecle(path, cargs[0], envp.data());
        case 1: return gExecle(path, cargs[0], cargs[1], cargs[2], envp.data());
        case 2: return gExecle(path, cargs[0], cargs[1], cargs[2], cargs[3], envp.data());
        case 3: return gExecle(path, cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], envp.data());
        case 4: return gExecle(path, cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], envp.data());
        case 5: return gExecle(path, cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], envp.data());
        case 6: return gExecle(path, cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], envp.data());
        case 7: return gExecle(path, cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], envp.data());
        case 8: return gExecle(path, cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], envp.data());
        default: return gExecle(path, cargs[0], cargs[1], cargs[2], cargs[3], cargs[4], cargs[5], cargs[6], cargs[7], cargs[8], cargs[9], cargs[10], envp.data());
    }
}

int fexecve(int fd, char* const argv[], char* const envp[]) {
    initReal();
    char path[PATH_MAX];
    std::string fdlink = "/proc/self/fd/" + std::to_string(fd);
    ssize_t n = readlink(fdlink.c_str(), path, sizeof(path) - 1);
    if (n > 0) path[n] = '\0';
    else snprintf(path, sizeof(path), "fd:%d", fd);
    writeEvent(path, argv);
    if (!gFexecve) { errno = ENOSYS; return -1; }
    return gFexecve(fd, argv, dedupeLdPreload(envp));
}

// execveat(dirfd, path, argv, envp, flags)
//   Exported by GLIBC 2.34+. For relative paths, dirfd specifies the base
//   directory.
//   This was not hooked before, so programs that use execveat (some runtimes,
//   some container tooling) were not captured at all.
int execveat(int dirfd, const char* path, char* const argv[], char* const envp[],
             int flags) {
    initReal();
    writeEvent(path, argv);
    if (!gExecveat) { errno = ENOSYS; return -1; }
    return gExecveat(dirfd, path, argv, dedupeLdPreload(envp), flags);
}

// posix_spawn(pid, path, file_actions, attrp, argv, envp)
//
// Note: the **function name here must be exactly `posix_spawn`** --
// it is not "our function name", it is **the glibc symbol we want to
// intercept**.
//
// A bulk rename once (snake_case -> camelBack) turned it into
// `posixSpawn`,
// so the shim exported `posixSpawn` and glibc's `posix_spawn` was never
// intercepted --
// and GNU make / ninja happen to prefer posix_spawn,
// so the child processes exec'd by make were **not captured at all**.
//
// Symptom: make only reported the root event, with no child events.
//
// **Renaming a place where "the function name itself is the ABI" = it fails
// silently.**
// So the function names in this file's extern "C" block have dedicated symbol
// table assertions
// (the "shim exports the correct symbols" section of
// tests/run_tests_linux_depth.sh).
int posix_spawn(pid_t* pid, const char* path,
                const posix_spawn_file_actions_t* file_actions,
                const posix_spawnattr_t* attrp,
                char* const argv[], char* const envp[]) {
    initReal();
    writeEvent(path, argv);
    if (!gPosixSpawn) { errno = ENOSYS; return -1; }
    return gPosixSpawn(pid, path, file_actions, attrp, argv,
                         dedupeLdPreload(envp));
}

int posix_spawnp(pid_t* pid, const char* file,
                 const posix_spawn_file_actions_t* file_actions,
                 const posix_spawnattr_t* attrp,
                 char* const argv[], char* const envp[]) {
    initReal();
    writeEvent(file, argv);
    if (!gPosixSpawnp) { errno = ENOSYS; return -1; }
    return gPosixSpawnp(pid, file, file_actions, attrp, argv,
                          dedupeLdPreload(envp));
}

}  // extern "C"
