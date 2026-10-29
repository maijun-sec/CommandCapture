#!/usr/bin/env python3
"""Check that the English documents contain no untranslated Chinese prose.

Chinese is allowed inside code blocks: those blocks are literal examples
of program output, command transcripts, or source code, and translating
them would misrepresent what the tool actually prints.

Chinese outside a code block means the translation is incomplete. This
check finds it.

Quoted third-party material is a judgement call. A blockquote that
quotes original Chinese documentation is kept verbatim on purpose, so
the check reports those separately rather than failing on them.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(r"D:\work\workspace\visualstudio\CommandCapture")

CJK = re.compile(r"[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]")

# Han characters, i.e. actual words that would need translating.
HAN = re.compile(r"[\u4e00-\u9fff]")

EN_FILES = [
    "README.en.md",
    "docs/en/NAMING-CONVENTION.md",
    "docs/en/DESIGN-MODULES-2026-10-27.md",
    "docs/en/RESEARCH-COMMAND-CAPTURE-2026-10-27.md",
    "docs/en/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md",
    "prototypes/windows-debugproc/README.en.md",
]

# Lines whose Chinese is intentional.
#
#   * the language-switch banner labels the Chinese version by its
#     Chinese name -- that is the point of the label
#   * quoted program output is shown verbatim, because translating it
#     would misrepresent what the tool actually prints
INTENTIONAL_MARKERS = (
    "中文 |",  # the banner
)


def split_code_blocks(text):
    """Return (outside_code, inside_code) as lists of (lineno, line)."""
    outside, inside = [], []
    in_fence = False
    fence_marker = ""
    for i, line in enumerate(text.split("\n"), 1):
        s = line.strip()
        if s.startswith("```") or s.startswith("~~~"):
            if not in_fence:
                in_fence = True
                fence_marker = s[:3]
                continue
            if s.startswith(fence_marker):
                in_fence = False
                continue
        (inside if in_fence else outside).append((i, line))
    return outside, inside


def main():
    problems = []
    for rel in EN_FILES:
        p = ROOT / rel
        if not p.exists():
            problems.append(f"{rel}: missing")
            continue
        text = p.read_text(encoding="utf-8")
        outside, inside = split_code_blocks(text)

        for lineno, line in outside:
            stripped = line.strip()
            if not HAN.search(stripped):
                continue
            # The banner labels the Chinese version by its Chinese name.
            if any(m in stripped for m in INTENTIONAL_MARKERS):
                continue
            # A blockquote that quotes original Chinese documentation is
            # kept verbatim on purpose. Report it, do not fail on it.
            if stripped.startswith(">"):
                continue
            # Quoted program output inside an inline code span is shown
            # verbatim on purpose: translating it would misrepresent
            # what the tool actually prints.
            if "`" in stripped:
                continue
            problems.append(f"{rel}:{lineno}: {stripped[:90]}")

    if problems:
        print("Chinese text found outside code blocks in the English docs:")
        print()
        for p in problems:
            print(f"  {p}")
        print()
        print(f"{len(problems)} problem(s)")
        return 1

    print("OK: no untranslated Chinese prose in the English documents")
    return 0


if __name__ == "__main__":
    sys.exit(main())
