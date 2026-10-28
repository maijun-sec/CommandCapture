#!/usr/bin/env python3
# CommandCapture - capture every command a script or executable spawns.
#
# Copyright (c) 2026 maijun
#
# SPDX-License-Identifier: MIT
#
# Released under the MIT License. See the LICENSE file at the repository
# root for the full text.
r"""License header gate.

Checks that every source file carries the project's MIT header:

    Copyright (c) 2026 maijun
    SPDX-License-Identifier: MIT

Why this is a gate and not a convention:
    A license header that is present on 95% of the files is worse than no
    header at all, because it looks like the project is licensed while
    some files carry no terms.

Why it is a **separate** script from the naming gate:
    The naming gate needs clang-tidy, which is not installed everywhere.
    The license check needs nothing but Python, so it can run everywhere.
    Conflating them would mean "no clang-tidy -> no license check either".

Usage:
    python3 tests/check_license.py
    python3 tests/check_license.py --fix
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

COPYRIGHT = "Copyright (c) 2026 maijun"
SPDX = "SPDX-License-Identifier: MIT"

# The comment marker each language uses.
COMMENT_STYLES = {
    ".cpp": "//",
    ".h": "//",
    ".c": "//",
    ".cs": "//",
    ".py": "#",
    ".ps1": "#",
    ".sh": "#",
}

# The header, as plain lines. The comment marker is prepended per file.
HEADER_BODY = [
    "CommandCapture - capture every command a script or executable spawns.",
    "",
    COPYRIGHT,
    "",
    SPDX,
    "",
    "Released under the MIT License. See the LICENSE file at the repository",
    "root for the full text.",
]

# The files that must be checked. The glob set is explicit so that a new
# file type cannot silently escape the check.
SOURCE_GLOBS = [
    "modules/**/*.cpp",
    "modules/**/*.h",
    "modules/**/*.c",
    "tests/**/*.cpp",
    "tests/**/*.h",
    "tests/**/*.c",
    "tests/**/*.py",
    "tests/**/*.ps1",
    "tests/**/*.sh",
    "prototypes/**/*.cs",
    "tools/**/*.py",
    "tools/**/*.sh",
    "tools/**/*.ps1",
]

# Files that are deliberately excluded, with the reason.
EXCLUDED = {
    "tests/fixtures": "test data -- a fixture project's files are inputs "
                      "to the build-system tests, not part of the product",
    "CMakeLists.txt": "handled separately (it has no extension)",
}


def comment_lines(lines, prefix):
    out = []
    for line in lines:
        out.append(prefix + " " + line if line else prefix)
    return out


def expected_header(suffix, prefix):
    return "\n".join(comment_lines(HEADER_BODY, prefix))


def collect_targets():
    targets = []
    for pattern in SOURCE_GLOBS:
        for path in ROOT.glob(pattern):
            rel = path.relative_to(ROOT)
            if any(rel.parts[:len(e.split("/"))] == tuple(e.split("/"))
                   for e in EXCLUDED if "/" in e):
                continue
            targets.append(path)
    # The root CMakeLists.txt has no extension.
    targets.append(ROOT / "CMakeLists.txt")
    return sorted(set(targets))


def check_file(path):
    """Return a list of problems with one file (empty means fine)."""
    rel = path.relative_to(ROOT)
    text = path.read_text(encoding="utf-8")

    if path.name == "CMakeLists.txt":
        prefix = "#"
    else:
        prefix = COMMENT_STYLES.get(path.suffix.lower())
        if prefix is None:
            return [f"{rel}: unknown comment style for {path.suffix}"]

    problems = []

    if COPYRIGHT not in text:
        problems.append(f"{rel}: missing '{COPYRIGHT}'")
    if SPDX not in text:
        problems.append(f"{rel}: missing '{SPDX}'")

    # The header must be at the **top** of the file -- a copyright buried
    # in the middle of a source file is easy to miss and easy to lose in
    # a copy-paste.
    head = text[:4000]
    if COPYRIGHT not in head:
        problems.append(f"{rel}: the copyright line is not near the top")

    # The marker must be a comment, not code. A stray "SPDX-License-Identifier"
    # in a string would satisfy the substring check but not be a header.
    header_lines = []
    for line in text.split("\n")[:20]:
        stripped = line.strip()
        if stripped.startswith("#!") or not stripped:
            continue
        header_lines.append(stripped)
        if len(header_lines) >= 12:
            break
    if not any(l.startswith(prefix) for l in header_lines):
        problems.append(f"{rel}: the header does not use the {prefix!r} comment style")

    return problems


def fix_file(path):
    """Rewrite the file with the header on top (idempotent)."""
    text = path.read_text(encoding="utf-8")
    if path.name == "CMakeLists.txt":
        prefix = "#"
    else:
        prefix = COMMENT_STYLES[path.suffix.lower()]

    header = comment_lines(HEADER_BODY, prefix)
    lines = text.split("\n")

    # Drop an existing header (idempotency).
    #
    # The cut has to be **exact**: the header is a fixed block, so the
    # reliable way is to look for the whole block as a prefix. Cutting
    # "line by line while it looks like the header" leaves the empty
    # comment lines behind -- and those are exactly the kind of debris
    # that accumulates in a source file.
    header_text = "\n".join(header)
    if text.startswith(header_text):
        rest = text[len(header_text):]
        # Drop the blank line that separated the header from the body.
        if rest.startswith("\n"):
            rest = rest[1:]
        lines = rest.split("\n")
    elif COPYRIGHT in text and SPDX in text:
        # The header is there but not at the very top (someone inserted
        # something above it). Cut it out by locating its exact lines.
        header_lines = set(header)
        cut = 0
        for i, line in enumerate(lines):
            if line.strip() in {h.strip() for h in header_lines}:
                cut = i + 1
            else:
                break
        if cut:
            while cut < len(lines) and not lines[cut].strip():
                cut += 1
            lines = lines[cut:]

    # Preserve a shebang.
    shebang = None
    if lines and lines[0].startswith("#!"):
        shebang = lines[0]
        lines = lines[1:]

    out = []
    if shebang is not None:
        out.append(shebang)
    out.extend(header)
    out.append("")
    out.extend(lines)
    path.write_text("\n".join(out), encoding="utf-8", newline="")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fix", action="store_true",
                    help="add the missing headers instead of only reporting")
    args = ap.parse_args()

    targets = collect_targets()
    if not targets:
        print("ERROR: no source files found -- is the path wrong?",
              file=sys.stderr)
        return 2

    problems = []
    for path in targets:
        found = check_file(path)
        if found:
            if args.fix:
                fix_file(path)
                # Re-check after fixing.
                found = check_file(path)
            problems.extend(found)

    if problems:
        print(f"FAIL: {len(problems)} license header problem(s)")
        print()
        for p in problems:
            print(f"  {p}")
        print()
        print("Every source file must carry:")
        print(f"    {COPYRIGHT}")
        print(f"    {SPDX}")
        return 1

    print(f"OK: license header check passed ({len(targets)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
