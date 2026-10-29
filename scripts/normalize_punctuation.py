#!/usr/bin/env python3
"""Replace Chinese full-width punctuation with its ASCII equivalent.

Chinese typography uses these marks as ordinary punctuation. When the
prose is translated to English they survive as leftovers and look like
translation debris:

    ——   ->  --
    ：    ->  :
    ，    ->  ,
    。    ->  .
    （）  ->  ()
    、    ->  ,
    ！    ->  !
    ？    ->  ?
    ；    ->  ;
    「」  ->  ""
    『』  ->  ""
    《》  ->  ""
    …    ->  ...
    ·    ->  (keep: it appears in C++ scope names)

Only characters that are pure punctuation are touched. Han characters
are never touched -- those need real translation, not substitution.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(r"D:\work\workspace\visualstudio\CommandCapture")

PAIRS = [
    # Two em dashes. In Chinese typography a double dash separates a
    # clause; in English the same mark reads as a single dash, so a
    # pair of them is translation debris.
    ("——", "--"),
    ("—", "--"),
    ("…", "..."),
    ("，", ", "),
    ("。", ". "),
    ("：", ": "),
    ("；", "; "),
    ("（", " ("),
    ("）", ") "),
    ("、", ", "),
    ("！", "! "),
    ("？", "? "),
    ("「", "\""),
    ("」", "\""),
    ("『", "\""),
    ("』", "\""),
    ("《", "\""),
    ("》", "\""),
    ("“", "\""),
    ("”", "\""),
    ("‘", "'"),
    ("’", "'"),
]

FILES = [
    "README.en.md",
    "docs/en/NAMING-CONVENTION.md",
    "docs/en/DESIGN-MODULES-2026-10-27.md",
    "docs/en/RESEARCH-COMMAND-CAPTURE-2026-10-27.md",
    "docs/en/WINDOWS-CAPTURE-PRINCIPLE-2026-10-27.md",
    "prototypes/windows-debugproc/README.en.md",
]


def tidy_spaces(text):
    """Collapse the double spaces and stray spaces the substitution leaves."""
    # A space before punctuation that does not want one.
    text = re.sub(r" +([.,;:!?])", r"\1", text)
    # A space just after an opening bracket.
    text = re.sub(r"\( +", "(", text)
    # A space just before a closing bracket.
    text = re.sub(r" +\)", ")", text)
    # Two or more spaces collapse to one.
    text = re.sub(r"  +", " ", text)
    return text


def main():
    total = 0
    for rel in FILES:
        p = ROOT / rel
        text = p.read_text(encoding="utf-8")
        original = text
        for old, new in PAIRS:
            text = text.replace(old, new)
        text = tidy_spaces(text)
        if text != original:
            p.write_text(text, encoding="utf-8")
            changed = sum(1 for a, b in zip(original.split("\n"), text.split("\n")) if a != b)
            total += changed
            print(f"  {rel}: {changed} line(s) tidied")
    print(f"total lines changed: {total}")


if __name__ == "__main__":
    main()
    sys.exit(0)
