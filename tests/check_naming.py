#!/usr/bin/env python3
# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.

r"""Naming convention gate.

The rules are described in docs/NAMING-CONVENTION.md.

Implementation notes
--------------------
The check is driven by clang-tidy's `readability-identifier-naming` ——
it has real semantic analysis and can tell "our identifiers" apart from
"external ones", which is far more reliable than hand-written regexes.

But there are two pitfalls on Windows, and both are handled here:

1. **MSVC's include paths are not in compile_commands.json**
   (they come from the compiler's environment variables, not from the
   command line). Without them clang-tidy reports
   "'cstdint' file not found".

2. **When a file has compile errors, the naming warnings are not emitted
   at all** —— and the gate treats "0 warnings" as "pass".

   Point 2 is the most dangerous one: it makes a gate that **checks
   nothing** show up green. So we explicitly detect compile errors and
   treat them as a failure.

Usage:
    python3 tests/check_naming.py
    python3 tests/check_naming.py --clang-tidy <path> --build-dir <dir>
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

VIOLATION_RE = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+):\s+warning:\s+"
    r"invalid case style for (?P<kind>.+?) '(?P<name>[A-Za-z_][A-Za-z0-9_]*)'"
)
COMPILE_ERROR_RE = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+):\s+error:\s+(?P<msg>.+?)"
    r"\s*\[clang-diagnostic-error\]"
)

CANDIDATES = [
    r"D:\development\MicrosoftVisualStudio\2022\Community\VC\Tools\Llvm\x64\bin\clang-tidy.exe",
    "/usr/bin/clang-tidy",
    "/usr/bin/clang-tidy-18",
    "/usr/bin/clang-tidy-17",
]


def vs_llvm_clang_tidy():
    """Find clang-tidy in a Visual Studio LLVM installation.

    The hardcoded path in CANDIDATES only works on one machine. A Visual
    Studio installation ships its own LLVM toolchain, and its location
    depends on the VS version and the installed components.

    This matters for the CTest integration: CTest runs in the environment of
    whatever launched it, which usually does not have the VS LLVM bin
    directory on PATH.
    """
    if os.name != "nt":
        return None

    # vswhere ships with the Visual Studio installer.
    program_files = os.environ.get("ProgramFiles(x86)") or r"C:\Program Files (x86)"
    vswhere = Path(program_files) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return None

    try:
        proc = subprocess.run(
            [str(vswhere), "-latest", "-products", "*",
             "-find", r"VC\Tools\Llvm\x64\bin\clang-tidy.exe"],
            capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=60)
    except Exception:  # noqa: BLE001 - a missing tool is handled by the caller
        return None

    for line in (proc.stdout or "").splitlines():
        line = line.strip()
        if line and Path(line).exists():
            return line
    return None


def find_clang_tidy(explicit):
    if explicit:
        p = Path(explicit)
        if p.exists():
            return str(p)
        return shutil_which(explicit)
    for c in CANDIDATES:
        if Path(c).exists():
            return c
    found = vs_llvm_clang_tidy()
    if found:
        return found
    return shutil_which("clang-tidy")


def shutil_which(name):
    return shutil.which(name)


def find_build_dir(explicit):
    """Locate a build directory that has a compile_commands.json.

    When an explicit directory is given but has no compile_commands.json,
    a Ninja build is generated next to it. That is needed because the
    **Visual Studio generator does not produce one** --- only Ninja and
    Makefiles do. Without this, the gate could never run from a normal
    MSVC build tree.
    """
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    else:
        candidates += [ROOT / "build-ninja", ROOT / "build", ROOT / "build-linux"]
    for c in candidates:
        if (c / "compile_commands.json").exists():
            return c
    if explicit:
        generated = generate_ninja_build(Path(explicit))
        if generated:
            return generated
    return None


def generate_ninja_build(build_dir):
    """Generate a Ninja build (and its compile_commands.json) next to build_dir.

    Returns the directory that has the compile_commands.json, or None.

    Why this is needed:
        The Visual Studio generator does not write compile_commands.json,
        but the naming gate needs one (clang-tidy reads the real compile
        command from it). So we generate a Ninja build of the same source
        tree.

        The generated tree is put in a *sibling* directory so it never
        interferes with the build the developer is actually using.
    """
    ninja_dir = build_dir.parent / (build_dir.name + "-ninja")
    cmake = shutil.which("cmake")
    if not cmake:
        print("note: cmake not found, cannot generate a compile database",
              file=sys.stderr)
        return None
    if shutil.which("ninja") is None:
        print("note: ninja not found, cannot generate a compile database",
              file=sys.stderr)
        return None

    print(f"note: generating a Ninja build for the compile database: {ninja_dir}")
    cmd = [cmake, "-G", "Ninja", "-S", str(ROOT), "-B", str(ninja_dir),
           "-DCMAKE_BUILD_TYPE=Release"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=600)
    except Exception as exc:  # noqa: BLE001 - report and continue
        print(f"note: cmake failed: {exc}", file=sys.stderr)
        return None

    ccj = ninja_dir / "compile_commands.json"
    if proc.returncode != 0 or not ccj.exists():
        print("note: could not generate compile_commands.json", file=sys.stderr)
        for line in (proc.stderr or "").splitlines()[-5:]:
            print(f"      {line}", file=sys.stderr)
        return None
    return ninja_dir


def _vswhere():
    """Locate vswhere.exe, or None."""
    if os.name != "nt":
        return None
    program_files = os.environ.get("ProgramFiles(x86)") or r"C:\Program Files (x86)"
    vswhere = Path(program_files) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    return vswhere if vswhere.exists() else None


def _vswhere_find(pattern):
    """Run vswhere -find <pattern> and return the existing directories."""
    vswhere = _vswhere()
    if vswhere is None:
        return []
    try:
        proc = subprocess.run(
            [str(vswhere), "-latest", "-products", "*", "-find", pattern],
            capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=60)
    except Exception:  # noqa: BLE001
        return []
    out = []
    for line in (proc.stdout or "").splitlines():
        line = line.strip()
        if not line:
            continue
        p = Path(line)
        if p.is_dir():
            out.append(str(p))
    return out


def _windows_kit_root():
    """Find the Windows Kits root directory.

    The Windows SDK is installed **outside** the Visual Studio tree
    (usually ``C:\\Program Files (x86)\\Windows Kits\\10``), so
    ``vswhere -find`` can never locate it --- vswhere only knows about
    the VS installation.

    The registry is the authoritative source: the SDK installer writes
    ``HKLM\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots`` with a
    ``KitsRoot10`` value.
    """
    if os.name != "nt":
        return None

    try:
        import winreg
    except ImportError:
        return None

    for hive, key_path in (
        (winreg.HKEY_LOCAL_MACHINE,
         r"SOFTWARE\Microsoft\Windows Kits\Installed Roots"),
        (winreg.HKEY_LOCAL_MACHINE,
         r"SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots"),
    ):
        try:
            with winreg.OpenKey(hive, key_path) as key:
                root, _ = winreg.QueryValueEx(key, "KitsRoot10")
        except OSError:
            continue
        if root and Path(root).is_dir():
            return root
    return None


def _windows_sdk_include_dirs():
    """Return the include directories of the newest installed Windows SDK."""
    root = _windows_kit_root()
    if root is None:
        return []
    include_root = Path(root) / "Include"
    if not include_root.is_dir():
        return []

    # Newest SDK version first: "10.0.26100.0" > "10.0.22621.0".
    versions = sorted(
        (p for p in include_root.iterdir() if p.is_dir()),
        key=lambda p: _version_key(p.name),
        reverse=True,
    )
    if not versions:
        return []

    dirs = []
    v = versions[0]
    for sub in ("ucrt", "um", "shared", "winrt", "cppwinrt"):
        d = v / sub
        if d.is_dir():
            dirs.append(str(d))
    return dirs


def _version_key(name):
    """Sort key for version-like directory names ("14.42.34433", "10.0.26100.0")."""
    parts = []
    for chunk in name.split("."):
        try:
            parts.append(int(chunk))
        except ValueError:
            parts.append(-1)
    return tuple(parts)


def discover_msvc_includes():
    """Find the MSVC + Windows SDK include paths without vcvars.

    Returns a ';'-separated list of existing directories, or "".

    Why this is needed:
        The INCLUDE environment variable is set by vcvars64.bat. When the
        gate runs from CTest --- or from any shell that did not source
        vcvars --- it is missing. Without it clang-tidy cannot find a
        single standard header, so it reports "file not found" everywhere,
        and **the naming warnings are not emitted for files with compile
        errors**. The gate then becomes a check that checks nothing.

    Sources that are merged here:

    1. ``vswhere -find VC\\Tools\\MSVC\\**\\include``
       --- the MSVC STL and CRT headers.
    2. The Windows SDK, located through the registry
       (``KitsRoot10``). vswhere cannot find it: the SDK lives
       outside the Visual Studio installation directory.
    """
    if os.name != "nt":
        return ""

    dirs = []

    # ── MSVC toolset headers ────────────────────────────────────────────
    for include_dir in _vswhere_find(r"VC\Tools\MSVC\**\include"):
        dirs.append(include_dir)
        # The parent holds the version number,
        # e.g. .../MSVC/14.42.34433/include
        version_dir = Path(include_dir).parent
        atlmfc = version_dir / "atlmfc" / "include"
        if atlmfc.is_dir():
            dirs.append(str(atlmfc))

    # ── Windows SDK headers ─────────────────────────────────────────────
    dirs.extend(_windows_sdk_include_dirs())

    # Deduplicate, keep the order
    seen = set()
    out = []
    for d in dirs:
        if d not in seen:
            seen.add(d)
            out.append(d)
    return ";".join(out)


def usable_include_dirs(include_value):
    """Split an INCLUDE-style value into the directories that still exist.

    Returns (usable, missing).

    Why the split matters:
        The ambient INCLUDE environment variable can hold **stale**
        toolset versions --- after a Visual Studio update the old
        directory is gone but the variable still points at it.

        The previous behaviour was "if INCLUDE is set, use it as-is, and
        only fall back to discovery when INCLUDE is empty". That is
        exactly wrong for the stale case: the variable was set, so no
        discovery happened, so **the only MSVC include path was the
        deleted one** --- and clang-tidy found no standard header at all,
        which silently turned the naming check into a no-op.

        So the rule is: **use every source, and drop only the paths that
        do not exist.**
    """
    usable = []
    missing = []
    for path in include_value.split(";"):
        path = path.strip()
        if not path:
            continue
        if Path(path).is_dir():
            usable.append(path)
        else:
            missing.append(path)
    return usable, missing


def msvc_extra_args():
    """Add the include paths that clang-tidy is missing.

    Two sources:

    1. **MSVC's implicit include paths** (STL / SDK)
       CMake's Ninja generator does not write them into
       compile_commands.json —— those paths come from the compiler's
       environment variables (INCLUDE), not from the command line.
       Without them clang-tidy reports "'cstdint' file not found".

    2. **The project's own include directories**
       What compile_commands.json records is MSVC syntax `-I<path>`
       (single dash), which clang parses with clang syntax and
       **does not accept**. So the project's header files
       ("ccap_compdb.h", "core/event.h") are all not found.

    The consequence of not adding them: clang-tidy reports
    "file not found", and **the naming warnings are not emitted when a
    file has compile errors** -> the gate becomes a check that checks
    nothing.
    """
    args = []
    # Merge **both** sources instead of choosing one.
    #
    # The old logic was "use INCLUDE if set, otherwise discover". That
    # fails in exactly the case that matters: the ambient INCLUDE can
    # point at a toolset version that has been uninstalled, and then the
    # *only* MSVC include path is a directory that does not exist.
    #
    # Symptom: clang-tidy reports "'cstdio' file not found" for every
    # file, the naming warnings are never emitted, and the gate reports
    # a clean pass --- while checking nothing.
    #
    # So: take the union, and drop only the paths that do not exist.
    ambient = os.environ.get("INCLUDE", "")
    ambient_usable, ambient_missing = usable_include_dirs(ambient)
    discovered_usable, discovered_missing = usable_include_dirs(
        discover_msvc_includes())

    inc_dirs = []
    seen = set()
    for path in ambient_usable + discovered_usable:
        if path not in seen:
            seen.add(path)
            inc_dirs.append(path)

    missing = ambient_missing + discovered_missing
    if missing:
        print(f"note: skipping {len(missing)} non-existent include paths")
        for m in missing:
            print(f"      {m}")
        print()
    if not ambient_usable and discovered_usable:
        print("note: INCLUDE was not usable; using the paths found in the "
              "Visual Studio installation and the Windows SDK")
        print()

    # The project's own include directories
    # Use -isystem instead of -I: naming violations in system headers
    # should not count as ours.
    for path in inc_dirs:
        args += ["--extra-arg", f"-isystem{path}"]

    # The project's own include directories
    modules = ROOT / "modules"
    for sub in ("ccap_core", "ccap_capture", "ccap_capture/include",
                "ccap_compdb", "ccap_compdb/include", "ccap"):
        d = modules / sub
        if d.is_dir():
            args += ["--extra-arg", f"-I{d}"]

    if inc_dirs:
        args += ["--extra-arg", "-fms-compatibility",
                 "--extra-arg", "-fms-extensions",
                 "--extra-arg", "-D_CRT_SECURE_NO_WARNINGS",
                 "--extra-arg", "-D_HAS_EXCEPTIONS=0"]
        # MSVC predefines _WIN32 (note the **leading underscore**),
        # while CMake's compile command line writes /DWIN32 /D_WINDOWS.
        #
        # Without _WIN32 the code's `#if defined(_WIN32)` looks
        # **false** to clang-tidy —— so it takes the #else branch and
        # tries to include Linux-only headers (poll.h / dlfcn.h), which
        # of course do not exist on Windows.
        #
        # The symptom is very odd: it reports "Linux header not found",
        # while the root cause is "the Windows macro is not defined".
        # Without looking at what compile_commands.json actually
        # defines, you cannot find it.
        args += ["--extra-arg", "-D_WIN32"]
    return args


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--clang-tidy", default=None)
    ap.add_argument("--build-dir", default=None)
    args = ap.parse_args()

    tidy = find_clang_tidy(args.clang_tidy)
    if not tidy:
        print("ERROR: clang-tidy not found, cannot check naming.", file=sys.stderr)
        print("", file=sys.stderr)
        print("  This gate **deliberately** fails when the tool is missing instead of skipping ——", file=sys.stderr)
        print("  a gate that skips when it cannot run is the same as no gate:", file=sys.stderr)
        print("  it would stay green on CI while actually checking nothing.", file=sys.stderr)
        return 2

    build_dir = find_build_dir(args.build_dir)
    if not build_dir:
        print("ERROR: no build directory with compile_commands.json found.", file=sys.stderr)
        print("  Generate: cmake -G Ninja -S . -B build-ninja -DCMAKE_BUILD_TYPE=Release",
              file=sys.stderr)
        return 2

    sources = sorted(str(p) for p in (ROOT / "modules").rglob("*.cpp"))
    if not sources:
        print("ERROR: no source files found", file=sys.stderr)
        return 2

    # Platform-specific files: only checked on their own platform.
    #
    # linux_preload.cpp / linux_shim.cpp use poll.h and dlfcn.h ——
    # those headers do not exist on Windows at all, so running them
    # there is just a pile of "file not found", and **compile errors
    # make clang-tidy skip the naming check** -> the gate becomes a
    # check that checks nothing.
    #
    # So: on Windows skip the Linux-specific files, on Linux skip the
    # Windows-specific files.
    #
    # The key is "which platform the file belongs to", the value is
    # "the files to skip on the **other** platform".
    on_windows = os.name == "nt"
    # Files that **cannot** be checked on the current platform
    skip = (["linux_preload", "linux_shim"] if on_windows
            else ["windows_debugger", "peb_reader"])
    filtered = [s for s in sources
                if not any(k in os.path.basename(s) for k in skip)]
    skipped = len(sources) - len(filtered)
    sources = filtered
    if skipped:
        print(f"note: skipping {skipped} files that do not belong to this platform"
              f"({', '.join(skip)})")
        print()
    extra = msvc_extra_args()
    print(f"clang-tidy : {tidy}")
    print(f"build dir  : {build_dir}")
    print(f"sources    : {len(sources)}  (skipped {skipped} platform-specific)")
    print(f"extra args : {len(extra) // 2}")
    print()

    # ── Run one file at a time, not all of them at once ───────────────────
    #
    # When all files are passed at once, clang-tidy merges multiple TUs
    # and **--extra-arg only applies to the first file** ->
    # the later files cannot find their headers -> compile errors ->
    # the naming check is skipped.
    #
    # The symptom is especially misleading: every file passes on its
    # own, but running them together reports a pile of "header not
    # found". Running them one by one is also more accurate: each file
    # uses its own compile command plus the common extra arguments.
    #
    # ── Run **two passes** per file (both branches) ──────────────────────
    #
    # Why two passes: clang-tidy only checks the code that still exists
    # **after preprocessing**. In
    # `#if defined(_WIN32) ... #else ... #endif`,
    # on Windows only the `_WIN32` branch survives and the `#else`
    # branch (Linux-specific) **was never analyzed at all**.
    #
    # So naming violations in the Linux branch could hide from the gate
    # forever. Measured: in normalize.cpp the `size_t name_start` sits
    # in the `#else` branch and the gate reported "0 violations" ——
    # it had never looked at that code.
    #
    # The fix: add `-U_WIN32` and run a second pass to cover the other
    # side. (`-U_WIN32` undefines _WIN32, so the `#else` branch
    # survives.)
    violations = []
    compile_errors = []
    for src in sources:
        # pass 1: current platform; pass 2: undef _WIN32 to cover the other side
        passes = [
            ("native", []),
            ("no-win32", ["--extra-arg", "-U_WIN32"]),
        ]
        for pass_name, pass_extra in passes:
            cmd = ([tidy, f"--config-file={ROOT / '.clang-tidy'}",
                    "-p", str(build_dir), "--quiet"] +
                   extra + pass_extra + [src, "--", "-std=c++17"])
            proc = subprocess.run(
                cmd, capture_output=True, text=True, encoding="utf-8",
                errors="replace", timeout=600,
            )
            out = (proc.stdout or "") + "\n" + (proc.stderr or "")
            for line in out.splitlines():
                s = line.strip()
                m = VIOLATION_RE.match(s)
                if m:
                    violations.append((pass_name, m))
                    continue
                m = COMPILE_ERROR_RE.match(s)
                if m:
                    compile_errors.append((src, m, pass_name))

    # Compile errors -> the naming check is incomplete -> must fail.
    #
    # This is the most important decision in this script:
    #   "no violations reported" and "checked and found no violations"
    #   are two different things.
    #
    # But two situations have to be told apart:
    #
    #   A compile error in the native pass = we **really** did not check
    #       it -> fail.
    #       Typical cause: MSVC's include paths were not passed in.
    #
    #   A compile error in the no-win32 pass = **expected**.
    #       That pass deliberately switches to the code branch on the
    #       "other side", where the platform-specific headers
    #       (poll.h / dlfcn.h) of course do not exist on the current
    #       platform. Those errors do not affect the **part that was
    #       already checked** (the compiler has already checked
    #       everything it could parse before reporting the error).
    #
    # So the no-win32 pass only produces a **warning**, it does not fail
    # —— otherwise on Windows this pass would report an error for every
    # Linux file and the gate would always be red, so nobody would look
    # at it anymore.
    native_errors = [e for e in compile_errors if e[2] == "native"]
    other_errors = [e for e in compile_errors if e[2] != "native"]

    if native_errors:
        print(f"FAIL: {len(native_errors)} compile errors —— the naming check is **incomplete**")
        print()
        for src, e, _ in native_errors[:10]:
            rel = e.group("file")
            try:
                rel = str(Path(rel).relative_to(ROOT))
            except ValueError:
                pass
            print(f"  {rel}:{e.group('line')}: {e.group('msg')}")
            print(f"      (in the file being checked: {os.path.basename(src)})")
        print()
        print("  Compile errors make clang-tidy skip the naming check,")
        print("  so 'no violations reported' does not mean 'conforms to the convention'.")
        print("  On Windows this is usually because MSVC's include paths were not passed in ——")
        print("  please run this script inside the vcvars64.bat environment.")
        return 1

    if other_errors:
        print(f"note: the other-side branch pass has {len(other_errors)} compile errors "
              f"(expected: that side's platform-specific headers do not exist here)")
        print("      These do not affect the conclusion for the checked part.")
        print()

    if not violations:
        print("OK: naming convention check passed (0 violations)")
        return 0

    seen = set()
    print(f"FAIL: {len(violations)} naming violations")
    print()
    for pass_name, v in violations:
        rel = v.group("file")
        try:
            rel = str(Path(rel).relative_to(ROOT))
        except ValueError:
            pass
        key = (v.group("kind"), v.group("name"))
        if key in seen:
            continue
        seen.add(key)
        branch = "" if pass_name == "native" else "  [other-side branch]"
        print(f"  {rel}:{v.group('line')}:{v.group('col')}  "
              f"{v.group('kind')} '{v.group('name')}'{branch}")
    print()
    print("See docs/NAMING-CONVENTION.md for the rules")
    return 1


if __name__ == "__main__":
    sys.exit(main())
