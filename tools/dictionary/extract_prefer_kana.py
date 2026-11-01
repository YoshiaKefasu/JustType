"""Extract the readings that Japanese writes in kana far more often than in kanji.

Meltype/Mozc converts こと to 事 whenever the kanji entry is cheaper, which is
wrong for words whose kana spelling is the common one.  JPDB's
term_meta_bank carries a frequency rank per surface:

  ["こと", "freq", {"value": 14, "displayValue": "14㋕"}]
  ["事",   "freq", {"reading": "こと", "frequency": {"value": 193, ...}}]

A kana spelling is marked with ㋕ in displayValue; a kanji spelling carries the
reading it belongs to.  Grouping by reading gives the two ranks to compare: when
the kana spelling is strictly more frequent than every kanji spelling of the
same reading, live conversion should keep the kana.

Space conversion is untouched: it still offers every Mozc candidate, so the
user can pick 事 whenever they want it.

Usage:
  python tools/dictionary/extract_prefer_kana.py \
      --jpdb "A:/[Freq] JPDB (Recommended).zip" \
      --output src/meltype/dictionaries/prefer_kana.txt
"""

import argparse
import collections
import io
import json
import os
import zipfile

# U+32D5 KATAKANA LETTER SMALL KA, JPDB's "this spelling is kana" marker.
KANA_MARKER = "\u32d5"


def is_pure_hiragana(text):
    """True when every character is hiragana (U+3041..U+309F).

    A reading that contains katakana can never equal the key of a Mozc preedit
    segment, which is hiragana, so keeping it would only add dead entries.
    """
    if not text:
        return False
    return all(0x3041 <= ord(ch) <= 0x309F for ch in text)


def load_entries(zip_path):
    """reading -> list of (term, rank, is_kana_spelling)."""
    with zipfile.ZipFile(zip_path) as archive:
        raw = archive.read("term_meta_bank_1.json").decode("utf-8")
    by_reading = collections.defaultdict(list)
    for entry in json.loads(raw):
        if len(entry) < 3 or entry[1] != "freq":
            continue
        term, meta = entry[0], entry[2]
        if "reading" in meta:
            reading = meta["reading"]
            frequency = meta["frequency"]
        else:
            reading = term
            frequency = meta
        rank = frequency.get("value")
        if not isinstance(rank, (int, float)):
            continue
        display = str(frequency.get("displayValue", ""))
        by_reading[reading].append((term, rank, display.endswith(KANA_MARKER)))
    return by_reading


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--jpdb", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    by_reading = load_entries(args.jpdb)

    keep = []
    for reading, entries in by_reading.items():
        # ひらがなだけの読みに限る。カタカナを含む読み (ごとに の ご ではなく
        # カタカナの語など) は、Mozc のプリエディットの key と決して一致
        # しないので、リストにあっても死にエントリになる。
        if not is_pure_hiragana(reading):
            continue
        kana = [rank for _, rank, is_kana in entries if is_kana]
        kanji = [rank for _, rank, is_kana in entries if not is_kana]
        if not kana or not kanji:
            continue
        best_kana = min(kana)
        best_kanji = min(kanji)
        if best_kana < best_kanji:
            keep.append((best_kana, reading))

    keep.sort()
    directory = os.path.dirname(args.output)
    if directory:
        os.makedirs(directory, exist_ok=True)
    with io.open(args.output, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# ライブ変換でかなのまま表示する読み。\n")
        fh.write("#\n")
        fh.write("# JPDB の頻度ランクで、かな表記が同じ読みのどの漢字表記よりも\n")
        fh.write("# 頻出のものだけを入れる (こと / もの / ごとに / わけ ...)。\n")
        fh.write("# Space による手動変換はこのリストを見ない: Mozc の候補が\n")
        fh.write("# そのまま並ぶので、漢字を選びたいときは今までどおり選べる。\n")
        fh.write("#\n")
        fh.write("# tools/dictionary/extract_prefer_kana.py が生成する。手で編集しない。\n")
        for _, reading in keep:
            fh.write(reading + "\n")

    print("readings=%d -> %s" % (len(keep), args.output))


if __name__ == "__main__":
    main()
