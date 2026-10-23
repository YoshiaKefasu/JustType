#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Extract modern vocabulary that the JustType/Mozc dictionary is missing.

Sources (Anki/Yomichan-style `term_meta_bank_1.json` decks):

  JPDB        https://jpdb.io   -- (surface, reading) pairs with a global rank
  BCCWJ       -- 国語研 現代日本語書き言葉均衡コーパス frequency
  YouTubeFreq -- modern spoken/streamed Japanese frequency

JPDB supplies the reading, so it is the spine: every candidate is a JPDB
`(surface, reading)` pair.  BCCWJ and YouTubeFreq supply the *modernity*
evidence -- a spelling nobody writes any more is not worth a dictionary entry
however well attested it is in a corpus of classical Japanese.

The output is `reading<TAB>surface<TAB>pos`, the format of
`data/dictionary_manual/words.tsv`.

--------------------------------------------------------------------------
Why the filters look like this
--------------------------------------------------------------------------
The naive query ("JPDB pairs the dictionary lacks, filtered to modern spellings")
returns 14,694 rows and almost all of them are damage:

    為る(する)      縡(こと)      維(これ)      鳥渡(ちょっと)

These are 当て字: orthographies that exist in a corpus but that no modern writer
uses.  Adding them makes the IME worse, not better -- `する` would start
offering 為る.

JPDB already knows the difference, and it records it in the same file.  For one
reading it stores two numbers:

    ["する", "freq", {"value": 12, "displayValue": "12㋕"}]
        how often the reading is written in KANA

    ["為る", "freq", {"reading": "する", "frequency": {"value": 35871}}]
        how often this KANJI spelling is used

An ateji is exactly the case where the kana spelling dominates, so rule V6 keeps
a kanji spelling only when it is at least as frequent as the kana spelling of
the same reading.  Measured on the noise set:

    為る   kanji    35871 / kana    12  -> reject (kana wins 2989x)
    縡     kanji   173338 / kana    14  -> reject
    維     kanji   130761 / kana    40  -> reject
    乃     kanji    12265 / kana     1  -> reject
    鳥渡   kanji        - / not a JPDB word -> reject (V4: not in BCCWJ/YouTube)

and it keeps the entries the dictionary genuinely lacks:

    一緒に kanji      183 / kana  5202  -> keep  (kanji wins 28x)
    咄嗟   kanji    16228 / kana 20970  -> keep
    間髪   kanji    47814 / kana161628  -> keep

V5 (>= 2 characters) is the second half of the same defence: every single-kanji
surface in the pool is a rare reading of a common character (種(くさ), 寺(じ),
皇(すめ)), which is a dictionary curiosity rather than a missing word.

--------------------------------------------------------------------------
Rules
--------------------------------------------------------------------------
V1  (reading, surface) absent from dictionary*.txt and from words.tsv
V2  reading is pure hiragana -- a katakana reading duplicates the hiragana entry
    the dictionary already keys on
V3  surface contains at least one kanji
V4  surface occurs in BCCWJ or YouTubeFreq  (modernity)
V5  len(surface) >= 2                      (see above)
V6  kanji rank <= kana rank of the reading (anti-当て字, see above)
V7  JPDB rank <= --max-rank
V8  the reading is the surface's *dominant* JPDB reading.  A surface with two
    readings (皇子 = おうじ 14506 / みこ 20166) must not be filed under the rare
    one: doing so makes the reading みこ produce 皇子, a new regression.
V9  a POS alias can be assigned (see below)
V10 the surface is the best-attested spelling for that reading (see below)

--------------------------------------------------------------------------
V10 -- the best spelling for the reading, not just any spelling
--------------------------------------------------------------------------
A reading usually has several spellings in JPDB, and only one of them is the one
people write:

    ひるごはん  昼ご飯(11113)  昼ごはん(22742)  昼御飯(57162)
    いちおく    １億(15367)    一億(21139)
    いっぴき    一匹(3620)     １匹(9024)      一疋(101706)

Adding a runner-up does not improve anything -- the new entry competes with the
winner and can push the correct spelling down.  At --max-rank 60000 the 昼御飯
entry turned ひるごはん from 昼ご飯 into 昼御飯, which the evaluation run recorded
as a real regression.  V10 keeps only the highest-ranked spelling.

--------------------------------------------------------------------------
What was tried and rejected: a composability gate
--------------------------------------------------------------------------
Mozc converts a whole reading by scoring segmentations, so a spelling that
assembles out of existing entries is already reachable:

    ひるごはん -> ひる(昼) + ごはん(ご飯) = 昼ご飯   composable
    かんはつ   -> かん(かん) + はつ(初)  = かん初   not composable

It is tempting to drop every composable candidate ("Mozc can already build it"),
and a version of this tool did.  It was rejected on measurement:

    V1-V10                   1084 rows  490 OK / 185 FAILED  (+7, 0 regressions)
    V1-V11 (composable gate)  103 rows  483 OK / 192 FAILED  (unchanged)

The gate threw away every one of the 7 fixes while buying no safety: V10 already
prevents the regression the gate was added for (the 昼御飯 entry), because it
drops the runner-up spellings that caused it.  So V10 is the gate, and the
composability question is only a diagnostic (`--report` counts it as
`composable`).

--------------------------------------------------------------------------
POS assignment (V9)
--------------------------------------------------------------------------
words.tsv is a `key / value / pos` TSV; gen_aux_dictionary.py turns the POS into
a lid *and* rid and takes the cost from the median of the existing entries that
share it.  So the POS decides both the connection costs and the word cost, and a
POS outside `PosIds.pos_alias` is a hard error.

Two tiers, both evidence-based:

  A. the reading already has dictionary entries
     -> use the family most of those entries belong to.  The dictionary has
        already decided what part of speech this reading is (かんぱつ is 4x
        名詞,サ変接続), and following it keeps the new entry in the same
        connection-cost neighbourhood as its neighbours.

  B. the reading is new and the surface is pure kanji (自分自身, 日常生活,
     王太子, 孤児院)
     -> 名詞.  A kanji-only compound with no kana tail is a noun by
        construction; there is no inflection to get wrong.

     a new reading whose surface mixes kanji and kana (好きなだけ, 食べ過ぎる,
     に過ぎない) is dropped.  Its POS is genuinely ambiguous -- the kana tail
     could be a particle, an auxiliary or an inflection -- and a wrong lid means
     the entry can never be produced for the input it was extracted from, so it
     is dead weight in the dictionary rather than a fix.

--------------------------------------------------------------------------
Curated block
--------------------------------------------------------------------------
A handful of readings the requirement names explicitly are not JPDB pairs at all
-- かんはつ is a variant reading JPDB files under かんぱつ -- so they are listed
in CURATED and emitted unconditionally.  They still have to clear V1.

Usage:
    python extract_freq_vocab.py --jpdb PATH --bccwj PATH --youtube PATH --report
    python extract_freq_vocab.py ... --output candidates.tsv
"""

from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import re
import sys
import zipfile

KANA_MARK = "\u32d5"    # ㋕ -- "this number is the kana-spelling frequency"
ABSENT_MARK = "\u274c"  # ❌ -- "this term is not in the JPDB corpus"

HIRAGANA = "\u3041-\u309F"
KATAKANA = "\u30A1-\u30FF"
KANJI = "\u4E00-\u9FFF\u3400-\u4DBF"
PURE_HIRAGANA = re.compile("^[%s]+$" % HIRAGANA)
HAS_KANJI = re.compile("[%s]" % KANJI)
HAS_HIRAGANA = re.compile("[%s]" % HIRAGANA)
HAS_KATAKANA = re.compile("[%s]" % KATAKANA)

# -------------------------------------------------------------------------
# The curated block: (reading, surface, pos).
#
# These are the spellings the requirement names by hand.  They bypass the JPDB
# spine because the spine cannot reach them:
#
#   かんはつ / かんぱつ  the 連濁 pair; both spellings get typed, and JPDB files
#                       only かんぱつ.
# -------------------------------------------------------------------------
CURATED = [
    ("かんはつ", "間髪", "名詞"),
    ("かんはついれず", "間髪入れず", "副詞"),
    ("かんぱついれず", "間髪入れず", "副詞"),
]

# -------------------------------------------------------------------------
# POS aliases accepted by gen_aux_dictionary.py (PosIds.pos_alias), keyed by the
# leading fields of id.def's POS name.
# -------------------------------------------------------------------------
POS_BY_FAMILY = {
    "名詞,一般": "名詞",
    "名詞,固有名詞": "固有名詞",
    "名詞,サ変接続": "名詞サ変",
    "名詞,形容動詞語幹": "名詞形動",
    "名詞,数": "名詞",
    "名詞,接続詞的": "名詞",
    "名詞,副詞可能": "名詞",
    "副詞": "副詞",
    "接続詞": "接続詞",
    "感動詞": "感動詞",
    "連体詞": "連体詞",
}

# Families that cannot be expressed in words.tsv at all.
POS_UNREPRESENTABLE = ("動詞", "形容詞", "助詞", "助動詞", "接尾", "接頭詞")


def kata2hira(text: str) -> str:
    return "".join(
        chr(ord(c) - 0x60) if 0x30A1 <= ord(c) <= 0x30F6 else c for c in text
    )


def read_json_from_maybe_zip(path: str, member: str) -> object:
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            return json.loads(archive.read(member).decode("utf-8"))
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def load_jpdb(path: str):
    """Returns (word_rank, kana_rank, readings_of, spellings_of).

    word_rank[(surface, reading)] : rank of that kanji spelling
    kana_rank[reading]            : rank of the reading written in kana
    readings_of[surface]          : {reading: rank} -- every reading JPDB knows
    spellings_of[reading]         : {surface: rank} -- every spelling JPDB knows

    The first two are different quantities stored in the same deck and must not
    be mixed: reading the ㋕ entries as word ranks made 会議 look like rank 114011
    instead of 2539 in the earlier frequency work.

    readings_of answers V8 (is this the surface's dominant reading?) and
    spellings_of answers V10 (is this the reading's best spelling?); they are
    inverses of each other and both are needed.
    """
    data = read_json_from_maybe_zip(path, "term_meta_bank_1.json")
    word_rank: dict[tuple[str, str], int] = {}
    kana_rank: dict[str, int] = {}
    readings_of: dict[str, dict[str, int]] = collections.defaultdict(dict)
    spellings_of: dict[str, dict[str, int]] = collections.defaultdict(dict)

    for entry in data:
        if len(entry) < 3 or entry[1] != "freq":
            continue
        term, meta = entry[0], entry[2]
        if not isinstance(meta, dict):
            continue
        if "reading" in meta:
            frequency = meta.get("frequency")
            if not isinstance(frequency, dict):
                continue
            display = frequency.get("displayValue", "")
            if ABSENT_MARK in display or KANA_MARK in display:
                continue  # not a corpus hit, or a kana-spelling count
            value = int(frequency["value"])
            reading = meta["reading"]
            key = (term, reading)
            if key not in word_rank or value < word_rank[key]:
                word_rank[key] = value
            if reading not in readings_of[term] or value < readings_of[term][reading]:
                readings_of[term][reading] = value
            if term not in spellings_of[reading] or value < spellings_of[reading][term]:
                spellings_of[reading][term] = value
        elif "value" in meta:
            if KANA_MARK not in meta.get("displayValue", ""):
                continue
            if term not in kana_rank:
                kana_rank[term] = int(meta["value"])
    return word_rank, kana_rank, readings_of, spellings_of


def load_flat(path: str) -> dict[str, int]:
    """BCCWJ and YouTubeFreq are both flat [term, "freq", rank] decks."""
    data = read_json_from_maybe_zip(path, "term_meta_bank_1.json")
    out: dict[str, int] = {}
    for entry in data:
        if len(entry) < 3 or entry[1] != "freq":
            continue
        meta = entry[2]
        raw = meta.get("value") if isinstance(meta, dict) else meta
        try:
            rank = int(raw)
        except (TypeError, ValueError):
            continue
        if entry[0] not in out or rank < out[entry[0]]:
            out[entry[0]] = rank
    return out


def load_id_def(path: str) -> dict[str, str]:
    out = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            lid, name = line.rstrip("\n").split(" ")
            out[lid] = name
    return out


class Dictionary:
    """The existing dictionary, read for its keys and its POS distributions."""

    def __init__(self, dictionary_glob, words_tsv):
        self.pairs: set[tuple[str, str]] = set()
        # surface -> [(lid, cost)]: the POS the dictionary already assigned to
        # this exact spelling (under whatever reading).  This is the only POS
        # evidence that is about the candidate word itself; the reading's
        # homophones are about other words.
        self.surface_pos: dict[str, list[tuple[str, int]]] = (
            collections.defaultdict(list)
        )
        # reading -> set of surfaces: the pieces `composable` may use.
        self.by_reading: dict[str, set[str]] = collections.defaultdict(set)
        for path in sorted(glob.glob(dictionary_glob)):
            self._read_dictionary(path)
        if words_tsv and os.path.exists(words_tsv):
            self._read_words(words_tsv)

    def _read_dictionary(self, path):
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                fields = line.rstrip("\n").split("\t")
                if len(fields) < 5:
                    continue
                reading, lid, _rid, cost, surface = fields[:5]
                self.pairs.add((reading, surface))
                self.surface_pos[surface].append((lid, int(cost)))
                self.by_reading[reading].add(surface)

    def _read_words(self, path):
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("#") or not line.strip():
                    continue
                reading, surface, _pos = line.rstrip("\n").split("\t")
                self.pairs.add((reading, surface))
                self.by_reading[reading].add(surface)

    def composable(self, reading: str, surface: str) -> bool:
        """True when the dictionary can already build |surface| from |reading|.

        A DIAGNOSTIC, not a gate.  It segments the reading into existing entries
        and asks whether their surfaces concatenate to the candidate:

            ひるごはん -> ひる(昼) + ごはん(ご飯) = 昼ご飯   -> composable
            かんはつ   -> かん(かん) + はつ(初)   = かん初  != 間髪 -> not

        Gating on this was measured and rejected: it dropped all 103+ candidates
        that produced the 7 evaluation fixes while buying no safety, because V10
        already removes the regression it was introduced for.  See the
        "What was tried and rejected" section of the module docstring.
        """
        return self._compose(reading, surface, 0, "", 0)

    def _compose(self, reading: str, surface: str, start: int, built: str,
                 depth: int) -> bool:
        """Recursive segmentation; depth-capped because a 5-piece build of a
        whole-word candidate is not what "the dictionary is missing" means."""
        if built == surface:
            return True
        if depth >= 5 or start >= len(reading) or not surface.startswith(built):
            return False
        for end in range(start + 1, min(len(reading), start + 12) + 1):
            for piece_surface in self.by_reading.get(reading[start:end], ()):
                if self._compose(reading, surface, end, built + piece_surface,
                                 depth + 1):
                    return True
        return False

    def assign_pos(self, reading: str, surface: str, id_def: dict[str, str]):
        """Tier A / tier B POS assignment.  Returns (alias, reason).

        Tier A -- the dictionary already knows this exact spelling under some
        reading, so its POS is a statement about this word:

            俺達 (おとこだて is absent, おれたち is what is missing) -> 名詞

        The reading's homophones are NOT evidence and are not consulted.  そのなか's
        only existing entry is the surname 園中 and あいだに's is the place name
        相谷, so copying their POS filed その中 and 間に as 固有名詞 -- a surname's
        connection-cost model, which no noun phrase ever follows.  Japanese
        homophones are unrelated words; only the spelling identifies the word.

        Tier B -- a new spelling.  A pure-kanji compound is a noun by
        construction (自分自身, 日常生活, 王太子, 孤児院), which is also the shape
        the task asks for (複合語・熟語・慣用句).  A mixed-script spelling
        (その中, 食べ過ぎる, に過ぎない) is genuinely ambiguous -- the kana tail
        could be a particle, an auxiliary or an inflection -- and a wrong lid
        means the entry can never be produced for the input it came from, so it
        is dropped rather than guessed.  The named targets that are mixed-script
        are in CURATED, where the POS is stated explicitly.
        """
        by_surface = self.surface_pos.get(surface)
        if by_surface:
            counts = collections.Counter()
            for lid, _cost in by_surface:
                name = id_def.get(lid, "")
                family = ",".join(name.split(",")[:2])
                alias = POS_BY_FAMILY.get(family)
                if alias is not None:
                    counts[alias] += 1
            if counts:
                return counts.most_common(1)[0][0], "tier-A(surface-known)"
            return None, "pos-unrepresentable"
        if HAS_HIRAGANA.search(surface) or HAS_KATAKANA.search(surface):
            return None, "new-reading-mixed-script"
        return "名詞", "tier-B(pure-kanji)"


def extract(args):
    dictionary = Dictionary(
        os.path.join(args.dictionary_dir, "dictionary*.txt"), args.words_tsv
    )
    id_def = load_id_def(args.id_def)
    word_rank, kana_rank, readings_of, spellings_of = load_jpdb(args.jpdb)
    bccwj = load_flat(args.bccwj)
    youtube = load_flat(args.youtube)

    kept = []
    dropped = collections.Counter()
    dropped_samples = collections.defaultdict(list)

    def drop(reason, sample):
        dropped[reason] += 1
        if len(dropped_samples[reason]) < 6:
            dropped_samples[reason].append(sample)

    for (surface, reading), rank in word_rank.items():
        sample = "%s(%s)" % (surface, reading)
        if (reading, surface) in dictionary.pairs:
            drop("V1 already-in-dictionary", sample)
            continue
        if not PURE_HIRAGANA.match(reading):
            drop("V2 reading-not-hiragana", sample)
            continue
        if not HAS_KANJI.search(surface):
            drop("V3 surface-has-no-kanji", sample)
            continue
        if surface not in bccwj and surface not in youtube:
            drop("V4 not-modern", sample)
            continue
        if len(surface) < 2:
            drop("V5 single-character", sample)
            continue
        kana = kana_rank.get(reading)
        if kana is not None and rank > kana:
            drop("V6 kana-spelling-wins", sample)
            continue
        if rank > args.max_rank:
            drop("V7 rank-above-cutoff", sample)
            continue
        others = readings_of.get(surface, {})
        if len(others) > 1:
            best_reading = min(others.items(), key=lambda kv: kv[1])[0]
            if best_reading != reading:
                drop("V8 rare-reading", sample)
                continue
        alias, reason = dictionary.assign_pos(reading, surface, id_def)
        if alias is None:
            drop("V9 " + reason, sample)
            continue
        # V10: the surface must be the spelling people actually write for this
        # reading (see the docstring).
        alternatives = spellings_of.get(reading, {})
        if alternatives:
            best = min(alternatives.items(), key=lambda item: item[1])
            if best[0] != surface:
                drop("V10 not-the-best-spelling",
                     "%s(%s) vs %s(%d)" % (surface, reading, best[0], best[1]))
                continue
        kept.append((reading, surface, alias, rank, reason))

    kept.sort(key=lambda row: row[3])

    curated = [
        (reading, surface, pos)
        for reading, surface, pos in CURATED
        if (reading, surface) not in dictionary.pairs
    ]
    return kept, curated, dropped, dropped_samples, dictionary


def report(kept, curated, dropped, dropped_samples, dictionary):
    print("kept %d rows (+%d curated)" % (len(kept), len(curated)))
    composable = sum(
        1 for reading, surface, *_ in kept
        if dictionary.composable(reading, surface)
    )
    print("diagnostic: %d of %d kept rows are composable from existing entries"
          % (composable, len(kept)))
    print("\nPOS distribution:")
    for alias, count in collections.Counter(row[2] for row in kept).most_common():
        print("   %-10s %d" % (alias, count))
    print("\nPOS tier:")
    for tier, count in collections.Counter(row[4] for row in kept).most_common():
        print("   %-26s %d" % (tier, count))
    print("\nrank bands:")
    for low, high in ((0, 5000), (5000, 10000), (10000, 20000),
                      (20000, 30000), (30000, 50000), (50000, 10 ** 9)):
        count = sum(1 for row in kept if low <= row[3] < high)
        if count:
            print("   %6d-%-6d %d" % (low, high, count))
    print("\ndropped:")
    for reason, count in sorted(dropped.items()):
        print("   %-30s %6d  e.g. %s"
              % (reason, count, ", ".join(dropped_samples[reason][:4])))
    print("\nfirst 40 kept:")
    for reading, surface, alias, rank, _tier in kept[:40]:
        print("   %7d %-16s %-18s %s" % (rank, surface, reading, alias))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--jpdb", required=True)
    parser.add_argument("--bccwj", required=True)
    parser.add_argument("--youtube", required=True)
    parser.add_argument("--dictionary-dir", default="src/data/dictionary_oss")
    parser.add_argument("--words-tsv",
                        default="src/data/dictionary_manual/words.tsv")
    parser.add_argument("--id-def", default="src/data/dictionary_oss/id.def")
    parser.add_argument("--max-rank", type=int, default=30000)
    parser.add_argument("--output", help="write `reading<TAB>surface<TAB>pos`")
    parser.add_argument("--report", action="store_true")
    args = parser.parse_args()

    kept, curated, dropped, dropped_samples, dictionary = extract(args)
    if args.report:
        report(kept, curated, dropped, dropped_samples, dictionary)
    if args.output:
        with open(args.output, "w", encoding="utf-8", newline="\n") as handle:
            for reading, surface, pos in curated:
                handle.write("%s\t%s\t%s\n" % (reading, surface, pos))
            for reading, surface, pos, _rank, _tier in kept:
                handle.write("%s\t%s\t%s\n" % (reading, surface, pos))
        print("wrote %d rows to %s" % (len(kept) + len(curated), args.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
