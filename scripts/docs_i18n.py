#!/usr/bin/env python3
"""Insert or refresh the language-switch banner on a documentation file.

Every document in this repository exists in two languages. The banner is
the only thing that tells a reader which language they are looking at and
where the other one lives.

Design rules:

  * The banner is generated, never hand-edited. Two copies of the same
    banner text would drift, and a stale link is worse than no link.
  * It is placed **after** the H1 title, so the title stays the first
    line. A file whose first line is a navigation bar reads like a
    table of contents.
  * The banner is a single line, so it stays out of the way in the
    rendered output.
  * It is idempotent: running it twice changes nothing.

Usage:
    python3 scripts/docs_i18n.py banner <file> --lang zh|en --alt <path>
    python3 scripts/docs_i18n.py check
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# The banner is delimited so it can be found and replaced exactly.
BEGIN = "<!-- i18n:begin -->"
END = "<!-- i18n:end -->"

TEMPLATES = {
    "zh": "[**English**]({alt}) | 中文",
    "en": "中文 | [**English**]({alt})",
}


def banner_text(lang, alt):
    return f"{BEGIN}\n{TEMPLATES[lang].format(alt=alt)}\n{END}"


def relative_link(from_file, to_file):
    """Build a relative link that works on GitHub and from disk."""
    return pathlib.PurePosixPath(
        pathlib.PurePosixPath(to_file).relative_to("/")
    ).as_posix()


def apply_banner(path, lang, alt_link):
    """Insert or refresh the banner in one file. Returns True if changed."""
    raw = path.read_text(encoding="utf-8")

    # A UTF-8 BOM is metadata, not content. If it stays at the head of
    # the file it hides the "# " of the title, and any script that looks
    # for the title by prefix (including this one) will miss it and
    # insert the banner in the wrong place.
    had_bom = raw.startswith("\ufeff")
    text = raw[1:] if had_bom else raw

    banner = banner_text(lang, alt_link)

    # Remove an existing banner, if any.
    #
    # The removal has to be exact and it has to consume exactly what
    # the insertion adds. The banner block is written as
    #
    #     <begin>\n<link>\n<end>\n\n
    #
    # so the removal pattern takes the whole block *and* the blank line
    # that follows it. Leaving that blank line behind makes every run
    # add one more, and the file grows a blank line at a time.
    pattern = re.compile(
        re.escape(BEGIN) + r".*?" + re.escape(END) + r"\n\n?", re.DOTALL
    )
    if pattern.search(text):
        text = pattern.sub("", text, count=1)

    # Place the banner after the H1 title and its blank line.
    lines = text.split("\n")
    insert_at = 0
    for i, line in enumerate(lines):
        if line.startswith("# "):
            insert_at = i + 1
            # Skip the blank line that follows the title.
            if insert_at < len(lines) and lines[insert_at].strip() == "":
                insert_at += 1
            break
    else:
        # No H1 title: put the banner at the very top.
        insert_at = 0

    # The banner block plus the single blank line that separates it from
    # the body. The removal above consumes exactly this.
    banner_block = banner.split("\n") + [""]
    new_lines = lines[:insert_at] + banner_block + lines[insert_at:]
    result = "\n".join(new_lines)

    # Preserve the original BOM presence.
    if had_bom:
        result = "\ufeff" + result

    if result != raw:
        path.write_text(result, encoding="utf-8")
        return True
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("banner", help="insert or refresh the banner")
    b.add_argument("file")
    b.add_argument("--lang", choices=["zh", "en"], required=True)
    b.add_argument("--alt", required=True, help="path to the other language")

    sub.add_parser("check", help="report files whose banner is missing")

    args = parser.parse_args()

    if args.cmd == "banner":
        path = pathlib.Path(args.file)
        if not path.is_absolute():
            path = ROOT / path
        changed = apply_banner(path, args.lang, args.alt)
        print(f"{'updated' if changed else 'unchanged'}: {path}")
        return 0

    # check mode
    missing = []
    for md in sorted(ROOT.rglob("*.md")):
        if ".trash" in md.parts or "build" in md.parts:
            continue
        if md.name.endswith(".en.md") or "docs/zh" in md.as_posix() or "docs/en" in md.as_posix():
            text = md.read_text(encoding="utf-8")
            if BEGIN not in text:
                missing.append(md.relative_to(ROOT).as_posix())
    for m in missing:
        print(f"missing banner: {m}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
