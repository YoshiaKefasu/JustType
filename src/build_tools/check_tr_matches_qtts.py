"""Checks that tr() literals in config_dialog.cc agree with the .qtts sources.

QTranslator looks a message up by its exact source text, so a tr() string that
has drifted from the <source> it was translated from silently falls back to the
untranslated text.  The drift this project actually suffers from is branding
churn (Zenz -> システムOne, Mozc -> JustType), so the check normalises every
branding term to one token and reports a mismatch only when the normalised forms
are identical -- i.e. the *only* difference is branding.  Unrelated pre-existing
differences (a %1 placeholder vs [ProductName], for example) are not this
check's business and are listed separately as informational.
"""

import io
import os
import re
import sys

sys.path.insert(0, os.path.join("src", "build_tools"))
import gen_qm_from_qtts as g  # noqa: E402

CC = "src/gui/config_dialog/config_dialog.cc"
QTTS_FILES = [
    "src/gui/config_dialog/config_dialog_ja.qtts",
    "src/gui/config_dialog/config_dialog_en.qtts",
]

TR_RE = re.compile(r'tr\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)', re.S)
LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

# Terms that branding churn swaps around.  Normalising them collapses
# "Zenz correction" / "システムOne correction" to the same token.
BRANDING = [
    "システムOne",
    "Zenz",
    "Mozc",
    "JustType",
    "Google Japanese Input",
]
BRANDING_RE = re.compile("|".join(re.escape(t) for t in BRANDING))
BRAND_TOKEN = "\x00BRAND\x00"


def c_unescape(text):
    return (text.replace('\\"', '"').replace("\\n", "\n")
                .replace("\\t", "\t").replace("\\\\", "\\"))


def tr_strings(path):
    data = io.open(path, encoding="utf-8").read()
    out = []
    for m in TR_RE.finditer(data):
        joined = "".join(c_unescape(l) for l in LITERAL_RE.findall(m.group(1)))
        line = data[:m.start()].count("\n") + 1
        out.append((line, joined))
    return out


def normalise(text):
    return BRANDING_RE.sub(BRAND_TOKEN, text)


def main():
    sources = []
    for path in QTTS_FILES:
        for _ctx, src, _tr in g.parse_qtts(path):
            sources.append(src)

    strings = tr_strings(CC)
    print(f"{CC}: {len(strings)} tr() calls, {len(sources)} .qtts sources")

    exact = 0
    branding_drift = []
    unrelated = []
    for line, text in strings:
        if text in sources:
            exact += 1
            continue
        norm = normalise(text)
        near = [s for s in sources if normalise(s) == norm]
        if near:
            branding_drift.append((line, text, near[0]))
        else:
            unrelated.append((line, text))

    print(f"  exact source matches: {exact}")
    print(f"  branding-only drift : {len(branding_drift)}")
    print(f"  unrelated drift     : {len(unrelated)} (pre-existing, not checked)")

    if branding_drift:
        print("\nBRANDING DRIFT (tr() differs from <source> only by branding):")
        for line, text, near in branding_drift:
            print(f"  line {line}:")
            print(f"    tr()    : {text[:150]!r}")
            print(f"    <source>: {near[:150]!r}")
        return 1

    print("\nOK: no tr() string is orphaned from its .qtts <source> by branding")
    return 0


if __name__ == "__main__":
    sys.exit(main())
