"""Proves gen_qm_from_qtts.py reproduces the checked-in .qm files byte-for-byte.

For each (qtts, qm) pair, the .qm is regenerated from the .qtts and compared.
The pair is taken from the commit that last touched the .qm, so the two files
are guaranteed to have been generated together; a pair that reproduces exactly
proves the generator handles every feature that file uses.

Run from the repository root:
    python src/build_tools/gen_qm_from_qtts_history_test.py
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join("src", "build_tools"))
import gen_qm_from_qtts as g  # noqa: E402

PAIRS = [
    "src/gui/about_dialog/about_dialog_ja",
    "src/gui/about_dialog/about_dialog_en",
    "src/gui/config_dialog/config_dialog_ja",
    "src/gui/config_dialog/config_dialog_en",
    "src/gui/config_dialog/keymap_ja",
    "src/gui/config_dialog/keymap_en",
    "src/gui/base/tr_ja",
    "src/gui/base/tr_en",
]


def git(*args):
    return subprocess.run(["git"] + list(args), check=True,
                          capture_output=True).stdout


def main():
    failures = []
    skipped = []
    for base in PAIRS:
        qtts = base + ".qtts"
        qm = base + ".qm"
        rev = git("log", "-1", "--format=%H", "--", qm).decode().strip()
        if not rev:
            skipped.append((qm, "no commit touches it"))
            continue
        try:
            qtts_bytes = git("show", f"{rev}:{qtts}")
            qm_bytes = git("show", f"{rev}:{qm}")
        except subprocess.CalledProcessError:
            skipped.append((qm, f"{qtts} absent at {rev[:10]}"))
            continue

        with tempfile.NamedTemporaryFile(suffix=".qtts", delete=False) as f:
            f.write(qtts_bytes)
            tmp = f.name
        try:
            rows = g.parse_qtts(tmp)
            language = g.parse_qtts_language(tmp)
        finally:
            os.unlink(tmp)

        generated = g.build(rows, language)
        ok = generated == qm_bytes
        note = ""
        if not ok:
            # Older lrelease builds omitted the language block, so accept a
            # byte-exact match either way rather than encoding one tool's
            # vintage as a requirement.
            alt = g.build(rows, None)
            if alt == qm_bytes:
                ok = True
                note = " (matched without language block)"
        print(f"{qm:52s} {'IDENTICAL' if ok else 'DIFFERS':10s} "
              f"({len(rows)} msgs, {len(generated)}B, lang={language}){note}")
        if not ok:
            failures.append((qm, rev, len(generated), len(qm_bytes)))

    for qm, why in skipped:
        print(f"{qm:52s} SKIPPED    ({why})")

    if failures:
        print("\nFAILURES:")
        for qm, rev, got, want in failures:
            print(f"  {qm} @ {rev[:10]}: generated {got}B vs expected {want}B")
        return 1
    print("\nOK: every comparable pair reproduced byte-for-byte")
    return 0


if __name__ == "__main__":
    sys.exit(main())
