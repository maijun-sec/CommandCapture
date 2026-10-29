#!/usr/bin/env python3
"""Compare the Chinese and English versions of each document.

The two versions must stay structurally in step. A translation that
drops a table row, loses a heading, or changes the shape of a code
block has introduced a difference that no reader would notice, and the
documentation then quietly disagrees with itself.

Checks per pair:

  * same number of headings, same levels, same order
  * same number of fenced code blocks
  * same number of table rows and the same column count per row
  * same number of blockquote lines
  * every relative link target resolves to a file that exists
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

PAIRS = [
    ("README.md", "README.en.md"),
    ("docs/zh/NAMING-CONVENTION.md", "docs/en/NAMING-CONVENTION.md"),
    ("docs/zh/DESIGN-MODULES-2026-10-27.md", "docs/en/DESIGN-MODULES-2026-10-27.md"),
    (
        "docs/zh/RESEARCH-COMMAND-CAPTURE-2026-10-27.md",
        "docs/en/RESEARCH-COMMAND-CAPTURE-2026-10-27.md",
    ),
    (
        "docs/zh/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md",
        "docs/en/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md",
    ),
    (
        "prototypes/windows-debugproc/README.md",
        "prototypes/windows-debugproc/README.en.md",
    ),
]

HEADING = re.compile(r"^(#{1,6})\s", re.MULTILINE)
FENCE = re.compile(r"^\s*```", re.MULTILINE)
QUOTE = re.compile(r"^\s*>", re.MULTILINE)
LINK = re.compile(r"\[[^\]]*\]\(([^)]+)\)")


def headings(text):
    return [(len(m.group(1)), m.group(0)) for m in HEADING.finditer(text)]


def table_rows(text):
    rows = []
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("|") and s.endswith("|"):
            rows.append(s.count("|"))
    return rows


def quote_paragraphs(text):
    """Count blockquote paragraphs, not lines.

    English prose does not wrap at the same places as Chinese prose, so
    the same sentence can occupy three lines in one language and four in
    the other. Counting lines would report a difference where there is
    none. What must match is the number of blockquote *paragraphs* --
    a blank line separates them, and a `>` that continues the previous
    line without a blank one is the same paragraph.
    """
    paragraphs = 0
    in_quote = False
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith(">"):
            if not in_quote:
                paragraphs += 1
                in_quote = True
        else:
            in_quote = False
    return paragraphs


def links(text):
    out = []
    for m in LINK.finditer(text):
        target = m.group(1).split("#")[0].strip()
        if not target or target.startswith(("http://", "https://", "mailto:")):
            continue
        out.append(target)
    return out


def check_pair(zh_rel, en_rel):
    problems = []
    zh_path = ROOT / zh_rel
    en_path = ROOT / en_rel

    if not zh_path.exists():
        return [f"{zh_rel}: missing"]
    if not en_path.exists():
        return [f"{en_rel}: missing"]

    zh = zh_path.read_text(encoding="utf-8")
    en = en_path.read_text(encoding="utf-8")

    zh_h, en_h = headings(zh), headings(en)
    if len(zh_h) != len(en_h):
        problems.append(
            f"{en_rel}: heading count {len(en_h)} != {len(zh_h)}"
        )
    else:
        for i, (a, b) in enumerate(zip(zh_h, en_h)):
            if a[0] != b[0]:
                problems.append(
                    f"{en_rel}: heading #{i + 1} level {b[0]} != {a[0]} "
                    f"({b[1]!r} vs {a[1]!r})"
                )

    zh_f, en_f = len(FENCE.findall(zh)), len(FENCE.findall(en))
    if zh_f != en_f:
        problems.append(f"{en_rel}: code fence count {en_f} != {zh_f}")

    zh_t, en_t = table_rows(zh), table_rows(en)
    if len(zh_t) != len(en_t):
        problems.append(f"{en_rel}: table row count {len(en_t)} != {len(zh_t)}")
    else:
        for i, (a, b) in enumerate(zip(zh_t, en_t)):
            if a != b:
                problems.append(
                    f"{en_rel}: table row #{i + 1} column count {b} != {a}"
                )

    zh_q, en_q = quote_paragraphs(zh), quote_paragraphs(en)
    if zh_q != en_q:
        problems.append(f"{en_rel}: blockquote paragraph count {en_q} != {zh_q}")

    # Every relative link in the English file must resolve.
    for target in links(en):
        if target.startswith("#"):
            continue
        resolved = (en_path.parent / target).resolve()
        if not resolved.exists():
            problems.append(f"{en_rel}: dead link -> {target}")

    return problems


def main():
    all_problems = []
    for zh_rel, en_rel in PAIRS:
        problems = check_pair(zh_rel, en_rel)
        status = "OK  " if not problems else "FAIL"
        print(f"{status} {zh_rel}  <->  {en_rel}")
        all_problems.extend(problems)

    if all_problems:
        print()
        for p in all_problems:
            print(f"  - {p}")
        return 1

    print()
    print(f"OK: {len(PAIRS)} document pairs are structurally in step")
    return 0


if __name__ == "__main__":
    sys.exit(main())
