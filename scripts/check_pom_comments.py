#!/usr/bin/env python3
"""Check that no XML comment in a POM contains a double dash.

An XML comment may not contain "--" anywhere inside it.  Maven's parser
rejects the whole POM when it finds one, and the error message points at
the *position* rather than at the offending text, so the cause is easy to
miss: the message says "in comment after two dashes (--) next character
must be >".

This is not hypothetical.  It happened repeatedly while writing the
comments in the ccap_jvm module, where the natural prose is full of
em-dashes.

Usage:
    python scripts/check_pom_comments.py [root]
"""

from __future__ import annotations

import sys
from pathlib import Path


def comment_lines(text: str):
    """Yield (line number, line) for every line that is inside an XML comment.

    Also yields a marker for a comment that is never closed, which is the
    other way a POM fails to parse: the parser reaches the end of the file
    still inside a comment, and the message points at the last line rather
    than at the comment that was opened.
    """
    in_comment = False
    opened_at = 0
    for number, line in enumerate(text.splitlines(), start=1):
        # A comment that opens and closes on the same line is not checked:
        # the "-->" that closes it is legitimate.
        if not in_comment and "<!--" in line and "-->" not in line:
            in_comment = True
            opened_at = number
            continue
        if in_comment and "-->" in line:
            in_comment = False
            continue
        if in_comment:
            yield number, line

    if in_comment:
        yield opened_at, f"<!-- a comment opened here is never closed"


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent

    problems: list[str] = []
    poms = sorted(root.rglob("pom.xml"))
    # The build output is not source.
    poms = [p for p in poms if "target" not in p.parts]

    for pom in poms:
        try:
            text = pom.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as error:
            problems.append(f"{pom}: cannot read ({error})")
            continue

        for number, line in comment_lines(text):
            stripped = line.strip()
            if stripped.startswith("<!-- a comment opened here"):
                problems.append(
                    f"{pom}:{number}: a comment is opened here and never closed"
                )
                continue
            # "--" followed by something is the illegal form.  The closing
            # "-->" is handled above.
            index = stripped.find("--")
            while index != -1:
                rest = stripped[index + 2 :]
                if rest and not rest.startswith(">"):
                    problems.append(
                        f"{pom}:{number}: '--' inside an XML comment: {stripped}"
                    )
                    break
                index = stripped.find("--", index + 2)

    if problems:
        print("XML comments must not contain '--':")
        for problem in problems:
            print(f"  {problem}")
        return 1

    print(f"ok: {len(poms)} POM file(s), no '--' inside an XML comment")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
