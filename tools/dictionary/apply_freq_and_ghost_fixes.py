#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Apply JPDB + YouTubeFreq frequency data and the ghost-kanji repair to
src/data/dictionary_oss/dictionary*.txt.

This is the reproducible form of the 6171 dictionary work.  It documents and
re-runs the two changes that were made:

  Rule G -- ghost-kanji penalty
  Rule J -- near-tie settlement

Both rules were validated against `quality_regression_main` before being kept;
see "Validation" at the bottom of this docstring.

--------------------------------------------------------------------------
Frequency sources
--------------------------------------------------------------------------
`term_meta_bank_1.json` from two Anki/Yomichan-style frequency decks:

  JPDB   ("[Freq] JPDB (Recommended).zip", https://jpdb.io)
  YouTube ("[Freq] YoutubeFreq.zip")  -- modern spoken/streamed Japanese

The JPDB file has three entry shapes and they must be parsed differently:

  ["の", "freq", {"value": 1, "displayValue": "1㋕"}]
      A plain term.  `displayValue` carrying U+32D5 (㋕) means this number is the
      frequency of writing the reading in *kana*.  For a kana word the value is
      the word rank.

  ["初めて", "freq", {"reading": "はじめて",
                     "frequency": {"value": 316, "displayValue": "316"}}]
      A real word: (term, reading) -> word rank.  USE THIS.

  ["初めて", "freq", {"reading": "はじめて",
                     "frequency": {"value": 4203, "displayValue": "4203㋕"}}]
      The kana-spelling frequency for the same reading.  It duplicates the plain
      "はじめて" entry and MUST NOT overwrite the word rank.

Getting this wrong is not a subtle bug: reading the ㋕ entries as the word rank
made 会議 look like rank 114011 instead of 2539, which made the whole corpus
appear fiction-biased and caused real conversion regressions.

The YouTube deck is a flat list: [term, "freq", "<rank>"].

Blend: geometric mean in log space, so a source that does not know a term
contributes nothing and the other one decides.

--------------------------------------------------------------------------
Rule G -- ghost-kanji penalty
--------------------------------------------------------------------------
`id.def` 488-511 declare the causative suffix paradigm
(`動詞,接尾,*,*,五段・サ行,...`), which real Japanese writes in kana
(させる / さす / させ).  Six rare kanji verbs were conjugated into that same
table -- 剳す 扠す 扨す 箚す 點す 捺す -- so their surfaces inherited the
suffix's near-zero cost.  Measured effect before the fix:

    startconversion させ
      0/50 させ   (the reading)
      1/50 箚せ   cost 6907
      ...
      (the real させ candidate sat far below the ghost forms)

Being a kanji surface inside 動詞,接尾 at low cost is *not* itself a bug:
掛かる (〜がかり), 付ける (〜づけ) and 狩り (〜がり) are real suffix forms.  The
discriminator is whether the surface's own reading (as recorded outside the
suffix paradigm) is this entry's key modulo rendaku:

    がかる vs own かかる  same word, rendaku  => keep
    づける vs own つける  same word, rendaku  => keep
    させ   vs own とぼせ  different word      => GHOST
    さす   vs own おす    different word      => GHOST

That matches exactly 42 entries (6 lemmas x 7 conjugated forms, lids 490-543)
and nothing else in the 1,289,076-entry dictionary.

--------------------------------------------------------------------------
Rule J -- near-tie settlement
--------------------------------------------------------------------------
Mozc's own costs already agree with the frequency data on most readings, and its
deliberate choices must survive.  What is genuinely broken is a *near-tie* Mozc
left undecided:

    おまかせ 6472 / お任せ 6474      (2 apart)
    JPDB お任せ 3569 vs おまかせ 21715, YouTube 16883 vs 26591

Rule J acts only when ALL of these hold:

  * both spellings are known to BOTH sources,
  * both sources agree on which is more frequent,
  * the JPDB gap is at least MIN_RATIO,
  * Mozc's cost gap is at most MAX_GAP (a tie, not a decision),
  * the current winner is NOT in Mozc's explicit hiragana-first tier
    (cost <= HIRAGANA_TIER).

That last guard matters: わたし / いう / いま / はなし are deliberately cheap in
kana, and the task requires keeping hiragana winning there.

The fix swaps the two costs, so the reading's cost multiset is unchanged and
Mozc's LM scale survives.

--------------------------------------------------------------------------
Rule K -- connection-cost override (お任せ)
--------------------------------------------------------------------------
Rule J cannot fix お任せ, and this is worth spelling out because it looks like a
Rule J case and is not.

In the dictionary the word costs are already correct the wrong way round:

    お任せ   1953
    おまかせ 3435

Yet the converter ranked おまかせ first.  The reason is the *left context* cost,
not the word cost.  お任せ is stored under lid 2597 (接頭詞,動詞接続,*,*,*,*,お)
and おまかせ under lid 2601 (接頭詞,名詞接続,*,*,*,*,お).  Measured overheads from
`converter_main --mode=conversion` (total cost minus word cost):

    lid 2597 -> 4521
    lid 2601 -> 3037
    handicap   1484

So お任せ pays 1484 more to start a phrase.  With word costs 1953 vs 3435 the
totals land at 6474 vs 6472 -- a 2-point loss, which is exactly the near-tie the
user sees as "the screen shows hiragana".

Rule J deliberately does not touch this: its MAX_GAP guard looks at word costs
and sees a 1482-point gap, i.e. a decision Mozc made on purpose.  The correct
place to express the frequency evidence is the word cost of the phrase that the
connection cost penalises, so Rule K sets the お任せ entries to OMAKASE_COST,
which clears the handicap with a margin while staying in the same cost band as
the surrounding candidates.

Evidence: JPDB お任せ 3569 vs おまかせ 21715, YouTube 16883 vs 26591 -- both
sources put お任せ ahead by a wide margin.  The value 200 was verified against
quality_regression (483 OK / 192 FAILED, identical to baseline) and against the
converter (おまかせ -> お任せ first, おまかせします -> お任せします first).

--------------------------------------------------------------------------
Validation
--------------------------------------------------------------------------
quality_regression_main over
data/test/quality_regression_test/{oss,regression}.tsv:

    baseline (before this work)   483 OK / 192 FAILED
    Rule G only                   483 OK / 192 FAILED   (identical)
    Rule G + Rule J               483 OK / 192 FAILED   (identical)
    broad whole-reading reorder   478-482 OK            (REJECTED, real
                                                         regressions such as
                                                         会議だ -> 懐疑だ)

The broad rule was rejected because Mozc's per-candidate cost also carries the
connection cost of the following word; reordering inflected forms by frequency
alone breaks sentence-level decisions even when the frequency signal is right.

Usage:
    python apply_freq_and_ghost_fixes.py --jpdb PATH --youtube PATH --dry-run
    python apply_freq_and_ghost_fixes.py --jpdb PATH --youtube PATH --apply
"""

from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import sys

KANA_MARK = "\u32d5"  # ㋕ -- "this number is the kana-spelling frequency"

# --- Rule G ---------------------------------------------------------------
SUFFIX_PREFIX = "動詞,接尾"
SUSPICIOUS = 1000   # "implausibly cheap inside a kana-only suffix paradigm"
GHOST_COST = 16000  # above p99.9 of the dictionary and above any JPDB fit

# --- Rule J ---------------------------------------------------------------
MIN_RATIO = 5.0      # only settle a tie when one side leads by this factor
MAX_GAP = 100        # ... and only when Mozc itself left them this close
HIRAGANA_TIER = 100  # Mozc's deliberate hiragana-first costs

# --- Rule K ---------------------------------------------------------------
# お任せ carries a 1484-point connection-cost handicap (lid 2597 vs 2601), so a
# word cost that merely beats おまかせ is not enough.  See the module docstring.
OMAKASE_COST = 200
OMAKASE_READING = "おまかせ"
OMAKASE_SURFACE = "お任せ"

VOICED = {}
for _h in "かきくけこさしすせそたちつてと":
    VOICED[_h] = chr(ord(_h) + 1)
VOICED.update({"は": "ば", "ひ": "び", "ふ": "ぶ", "へ": "べ", "ほ": "ぼ"})
HALF = {"は": "ぱ", "ひ": "ぴ", "ふ": "ぷ", "へ": "ぺ", "ほ": "ぽ"}


# -------------------------------------------------------------------------
# Frequency data
# -------------------------------------------------------------------------

def kata2hira(s: str) -> str:
    """Katakana -> hiragana, so readings compare across scripts."""
    return "".join(
        chr(ord(c) - 0x60) if 0x30A1 <= ord(c) <= 0x30F6 else c for c in s
    )


def load_jpdb(path: str):
    """Returns (word_rank, term_rank, readings_of).

    word_rank  : (term, reading) -> rank   the real word frequency
    term_rank  : term -> rank              a term carrying its own rank (kana
                                           words, and terms whose only entry is
                                           a plain one)
    readings_of: term -> set of readings JPDB knows for it

    The ㋕ entries are excluded: they measure how often the reading is written in
    kana, which is a different quantity and would overwrite the word rank.
    """
    data = json.load(open(path, encoding="utf-8"))
    word_rank: dict[tuple[str, str], int] = {}
    term_rank: dict[str, int] = {}
    readings_of: dict[str, set[str]] = collections.defaultdict(set)

    for term, kind, meta in data:
        if kind != "freq":
            continue
        if "reading" in meta:
            freq = meta["frequency"]
            if KANA_MARK in freq.get("displayValue", ""):
                continue  # kana-spelling frequency, not the word's rank
            word_rank[(term, meta["reading"])] = freq["value"]
            readings_of[term].add(meta["reading"])
        else:
            prev = term_rank.get(term)
            if prev is None or meta["value"] < prev:
                term_rank[term] = meta["value"]
    return word_rank, term_rank, readings_of


def load_youtube(path: str) -> dict[str, int]:
    """The YouTube deck is [term, "freq", "<rank>"] with the rank as a string."""
    out: dict[str, int] = {}
    for entry in json.load(open(path, encoding="utf-8")):
        if entry[1] == "freq":
            out[entry[0]] = int(entry[2])
    return out


def make_rank_lookup(word_rank, term_rank, readings_of):
    """Returns rank_of(key, value): the best rank JPDB has for this spelling."""

    def rank_of(key: str, value: str):
        rank = word_rank.get((value, key))
        if rank is not None:
            return rank
        normalized = kata2hira(key)
        for reading in readings_of.get(value, ()):
            if kata2hira(reading) == normalized:
                return word_rank[(value, reading)]
        # A plain entry means the kana spelling.  It only describes this
        # spelling when the surface really is the reading.
        if value == key or kata2hira(value) == kata2hira(key):
            return term_rank.get(value)
        return None

    return rank_of


# -------------------------------------------------------------------------
# Dictionary access
# -------------------------------------------------------------------------

def load_pos(id_def_path: str) -> dict[int, str]:
    pos: dict[int, str] = {}
    with open(id_def_path, encoding="utf-8") as handle:
        for line in handle:
            parts = line.rstrip("\n").split(" ", 1)
            if len(parts) == 2:
                pos[int(parts[0])] = parts[1]
    return pos


def same_word_reading(a: str, b: str) -> bool:
    """True when |a| and |b| can be the same word with a rendaku difference."""
    if a == b:
        return True
    if len(a) != len(b) or not a or not b or a[1:] != b[1:]:
        return False
    x, y = a[0], b[0]
    return (
        VOICED.get(x) == y
        or VOICED.get(y) == x
        or HALF.get(x) == y
        or HALF.get(y) == x
    )


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    src = os.path.abspath(os.path.join(here, "..", "..", "src"))
    default_dict = os.path.join(src, "data", "dictionary_oss")

    parser = argparse.ArgumentParser(
        description="Apply JPDB + YouTubeFreq frequency data and the "
                    "ghost-kanji repair to dictionary*.txt.")
    parser.add_argument("--jpdb", required=True,
                        help="path to the JPDB term_meta_bank_1.json")
    parser.add_argument("--youtube", required=True,
                        help="path to the YouTubeFreq term_meta_bank_1.json")
    parser.add_argument("--dictionary-dir", default=default_dict,
                        help="directory holding dictionary*.txt and id.def")
    parser.add_argument("--apply", action="store_true",
                        help="write the files (default is a dry run)")
    parser.add_argument("--min-ratio", type=float, default=MIN_RATIO)
    parser.add_argument("--max-gap", type=int, default=MAX_GAP)
    parser.add_argument("--hiragana-tier", type=int, default=HIRAGANA_TIER)
    parser.add_argument("--ghost-cost", type=int, default=GHOST_COST)
    parser.add_argument("--skip-rule-g", action="store_true")
    parser.add_argument("--skip-rule-j", action="store_true")
    parser.add_argument("--skip-rule-k", action="store_true")
    args = parser.parse_args()

    word_rank, term_rank, readings_of = load_jpdb(args.jpdb)
    rank_of = make_rank_lookup(word_rank, term_rank, readings_of)
    youtube = load_youtube(args.youtube)
    pos_of = load_pos(os.path.join(args.dictionary_dir, "id.def"))

    files = sorted(glob.glob(os.path.join(args.dictionary_dir, "dictionary*.txt")))
    if not files:
        print(f"no dictionary*.txt under {args.dictionary_dir}", file=sys.stderr)
        return 1
    per_file = {
        path: [line.rstrip("\n").split("\t")
               for line in open(path, encoding="utf-8")]
        for path in files
    }
    original = {path: [list(row) for row in rows]
                for path, rows in per_file.items()}

    # ---------------- Rule G: ghost-kanji penalty ----------------
    ghost_changes = []
    if not args.skip_rule_g:
        # surface -> readings it legitimately has outside the suffix paradigm
        own_readings: dict[str, set[str]] = collections.defaultdict(set)
        for rows in per_file.values():
            for row in rows:
                if len(row) < 5:
                    continue
                if pos_of.get(int(row[1]), "").startswith(SUFFIX_PREFIX):
                    continue
                own_readings[row[4]].add(row[0])

        for path, rows in per_file.items():
            for index, row in enumerate(rows):
                if len(row) < 5:
                    continue
                key, lid, cost, value = row[0], int(row[1]), int(row[3]), row[4]
                if key == value:
                    continue  # kana surface: a real suffix form
                if not pos_of.get(lid, "").startswith(SUFFIX_PREFIX):
                    continue
                if cost >= SUSPICIOUS:
                    continue
                own = own_readings.get(value, set())
                if own and any(same_word_reading(key, o) for o in own):
                    continue  # real suffix form (rendaku of its own reading)
                ghost_changes.append((path, index, key, cost, value))
                row[3] = str(args.ghost_cost)

    # ---------------- Rule J: near-tie settlement ----------------
    by_key: dict[str, list] = collections.defaultdict(list)
    for path, rows in per_file.items():
        for index, row in enumerate(rows):
            if len(row) >= 5:
                by_key[row[0]].append((path, index, row))

    tie_changes = []
    if not args.skip_rule_j:
        for key, entries in by_key.items():
            # One cost slot per surface: a surface repeated with different POS
            # ids is one candidate to the user, so only its cheapest entry
            # takes part.
            best: dict[str, tuple] = {}
            for path, index, row in entries:
                value = row[4]
                if value not in best or int(row[3]) < int(best[value][2][3]):
                    best[value] = (path, index, row)
            if len(best) < 2:
                continue
            ranked = []
            for value, (path, index, row) in best.items():
                jpdb = rank_of(key, value)
                yt = youtube.get(value)
                if jpdb and yt:  # both sources must know it
                    ranked.append((value, path, index, row, jpdb, yt))
            if len(ranked) < 2:
                continue
            ranked.sort(key=lambda r: r[4])  # JPDB order
            for i in range(len(ranked) - 1):
                v1, p1, i1, r1, j1, y1 = ranked[i]
                v2, p2, i2, r2, j2, y2 = ranked[i + 1]
                if y1 > y2:
                    continue  # YouTube disagrees: not a settled case
                if j2 / max(1, j1) < args.min_ratio:
                    continue  # gap too small to trust
                c1, c2 = int(r1[3]), int(r2[3])
                if c1 <= c2:
                    continue  # Mozc already agrees
                if c1 - c2 > args.max_gap:
                    continue  # Mozc decided this deliberately
                if v2 == key and c2 <= args.hiragana_tier:
                    continue  # protect Mozc's hiragana-first tier
                r1[3], r2[3] = str(c2), str(c1)  # swap: multiset preserved
                tie_changes.append((key, v1, c1, c2, v2, c2, c1))

    # ---------------- Rule K: connection-cost override ----------------
    # お任せ loses to おまかせ only because lid 2597 pays 1484 more connection
    # cost than lid 2601; its word cost is already cheaper.  Lower the word cost
    # so the total clears the handicap.
    omakase_changes = []
    if not args.skip_rule_k:
        for path, rows in per_file.items():
            for index, row in enumerate(rows):
                if len(row) < 5:
                    continue
                if row[0] != OMAKASE_READING or row[4] != OMAKASE_SURFACE:
                    continue
                if int(row[3]) <= OMAKASE_COST:
                    continue
                omakase_changes.append((path, index, int(row[3])))
                row[3] = str(OMAKASE_COST)

    # ---------------- report ----------------
    print(f"Rule G (ghost penalty)   : {len(ghost_changes)} entries")
    print(f"Rule J (near-tie settle) : {len(tie_changes) * 2} entries "
          f"({len(tie_changes)} swaps)")
    print(f"Rule K (お任せ override)  : {len(omakase_changes)} entries")
    print()
    print("--- Rule G ---")
    for path, index, key, cost, value in ghost_changes[:10]:
        print(f"  {os.path.basename(path)}:{index + 1} key={key} "
              f"{cost} -> {args.ghost_cost} value={value}")
    print()
    print("--- Rule J ---")
    for key, v1, c1, new1, v2, c2, new2 in tie_changes:
        print(f"  key={key:<14} {v1:<12} {c1:>5} -> {new1:<5}  "
              f"(over {v2:<12} {c2:>5} -> {new2:<5})")
    print()
    print("--- Rule K ---")
    for path, index, old in omakase_changes:
        print(f"  {os.path.basename(path)}:{index + 1} "
              f"key={OMAKASE_READING} value={OMAKASE_SURFACE} "
              f"{old} -> {OMAKASE_COST}")

    if not args.apply:
        print()
        print("(dry run: nothing written; pass --apply to write)")
        return 0

    for path, rows in per_file.items():
        with open(path, "w", encoding="utf-8", newline="\n") as out:
            for row in rows:
                out.write("\t".join(row) + "\n")
    print()
    print(f"wrote {len(per_file)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
