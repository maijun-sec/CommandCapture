#!/usr/bin/env python3
"""Mutation test for the documentation gates.

A gate that never fails is a gate that does nothing. This applies a
series of deliberate damage to the documentation and checks that each
gate notices.

Each mutation restores the tree byte-for-byte before the next one, so
a failure cannot leak into the following case.
"""

import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

GATE_PARITY = ROOT / "scripts" / "check_docs_i18n.py"
GATE_TRANSLATION = ROOT / "scripts" / "check_docs_translation.py"

README_EN = ROOT / "README.en.md"
README_ZH = ROOT / "README.md"
DOC_EN = ROOT / "docs" / "en" / "NAMING-CONVENTION.md"
DOC_ZH = ROOT / "docs" / "zh" / "NAMING-CONVENTION.md"


def run_gate(script):
    proc = subprocess.run(
        [sys.executable, str(script)], cwd=str(ROOT), capture_output=True, text=True
    )
    return proc.returncode, proc.stdout + proc.stderr


def mutations():
    """Yield (name, files_to_touch, transform, gate_that_must_fail)."""
    # 1. Drop a heading from the English side.
    yield (
        "english doc loses a heading",
        [(DOC_EN, lambda t: t.replace("## 2.", "### 2.", 1))],
        GATE_PARITY,
    )
    # 2. Drop a table row from the English side.
    yield (
        "english doc loses a table row",
        [(DOC_EN, lambda t: _drop_table_row(t))],
        GATE_PARITY,
    )
    # 3. Break a relative link in the English side.
    yield (
        "english doc has a dead link",
        [(DOC_EN, lambda t: t.replace("../zh/NAMING-CONVENTION.md", "../zh/NOPE.md", 1))],
        GATE_PARITY,
    )
    # 4. Leave untranslated Chinese prose in the English side.
    yield (
        "english doc keeps untranslated prose",
        [(README_EN, lambda t: t.replace(
            "Cross-platform (Windows / Linux), zero third-party dependencies, "
            "single-file executable.",
            "跨平台（Windows / Linux），零第三方依赖，单文件可执行程序。",
            1,
        ))],
        GATE_TRANSLATION,
    )
    # 5. Remove a code fence from the English side.
    yield (
        "english doc loses a code block",
        [(README_EN, lambda t: _drop_fence(t))],
        GATE_PARITY,
    )


def _drop_table_row(text):
    lines = text.split("\n")
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith("|") and s.endswith("|") and not set(s) <= set("|- :"):
            # Keep the header and the separator; drop a body row.
            if i > 0 and lines[i - 1].strip().startswith("|"):
                del lines[i]
                break
    return "\n".join(lines)


def _drop_fence(text):
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if line.strip().startswith("```"):
            del lines[i]
            break
    return "\n".join(lines)


def main():
    results = []
    for name, edits, gate in mutations():
        backup = []
        try:
            for path, transform in edits:
                original = path.read_text(encoding="utf-8")
                backup.append((path, original))
                changed = transform(original)
                if changed == original:
                    results.append((name, "MUTATION-NOOP", "the mutation changed nothing"))
                    break
                path.write_text(changed, encoding="utf-8")
            else:
                code, output = run_gate(gate)
                gate_name = gate.name
                if code != 0:
                    results.append((name, "KILLED", f"{gate_name} exit={code}"))
                else:
                    results.append((name, "SURVIVED", f"{gate_name} did not notice"))
        finally:
            for path, original in backup:
                path.write_text(original, encoding="utf-8")

    width = max(len(r[0]) for r in results) + 2
    print("Mutation test for the documentation gates")
    print()
    killed = 0
    for name, verdict, detail in results:
        mark = {"KILLED": "killed  ", "SURVIVED": "SURVIVED", "MUTATION-NOOP": "NO-OP   "}[verdict]
        print(f"  {mark} {name.ljust(width)} {detail}")
        if verdict == "KILLED":
            killed += 1
    print()
    print(f"{killed} of {len(results)} mutations killed")

    return 0 if killed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
