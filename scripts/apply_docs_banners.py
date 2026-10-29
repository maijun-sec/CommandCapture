#!/usr/bin/env python3
"""Apply the language-switch banner to every document.

The banner is the only place that tells a reader which language they
are looking at and where the other version lives. It is generated from
one template so the two sides cannot drift apart.

Relative paths differ per file (a doc in docs/zh/ points at ../en/...,
a doc at the repository root points at docs/en/...), so the pairs are
listed explicitly rather than derived.
"""

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# (chinese file, english file)
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


def relative_link(from_file, to_file):
    """Relative path from one file to another, as GitHub resolves it."""
    return pathlib.PurePosixPath(
        pathlib.PurePath(to_file).relative_to(pathlib.PurePath("x"))
    ).as_posix()


def link_between(from_file, to_file):
    import os

    return os.path.relpath(
        to_file.replace("/", os.sep),
        os.path.dirname(from_file.replace("/", os.sep)),
    ).replace(os.sep, "/")


def main():
    script = ROOT / "scripts" / "docs_i18n.py"
    failures = 0

    for zh_rel, en_rel in PAIRS:
        # Chinese page links to the English one.
        zh_to_en = link_between(zh_rel, en_rel)
        en_to_zh = link_between(en_rel, zh_rel)

        for rel, lang, alt in (
            (zh_rel, "zh", zh_to_en),
            (en_rel, "en", en_to_zh),
        ):
            proc = subprocess.run(
                [sys.executable, str(script), "banner", rel, "--lang", lang, "--alt", alt],
                cwd=str(ROOT),
                capture_output=True,
                text=True,
            )
            if proc.returncode != 0:
                failures += 1
                print(f"FAILED {rel}: {proc.stderr.strip()}")
            else:
                print(f"{proc.stdout.strip()}  (lang={lang} -> {alt})")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
