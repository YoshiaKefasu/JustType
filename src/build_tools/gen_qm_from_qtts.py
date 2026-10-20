"""Generates Qt .qm files from .qtts sources without lrelease.

The Qt build in this checkout sets QT_WILL_BUILD_TOOLS=FALSE, so lrelease.exe
does not exist and the checked-in .qm files could not otherwise be regenerated
after editing a .qtts.

File layout, from qtbase/src/corelib/kernel/qtranslator.cpp and
qttools/src/linguist/shared/qm.cpp:

  magic        16 bytes 3cb86418caef9c95cd211cbf60a1bddd
  tag 0xa7     language    u32 byte length, then the locale name (only when the
                           .qtts <TS> element carries a language="...")
  tag 0x42     hash table    u32 byte length, then (u32 hash, u32 offset) pairs
                             sorted ascending by hash -- QTranslator binary
                             searches this table
  tag 0x69     message table u32 byte length, then the message blocks

Message block, in this order:
  Tag_Translation(3) len <utf16be>  len 0xffffffff means "no translation"
  Tag_Comment(8)     len <utf8>     always length 0 here
  Tag_SourceText(6)  len <utf8>
  Tag_Context(7)     len <utf8>
  Tag_End(1)

The offsets in the hash table are relative to the start of the message table.

Three rules were established by reproducing the checked-in .qm files
byte-for-byte from their .qtts sources:

  1. The message table is sorted by (context, source) as plain strings.
  2. `<translation type="unfinished">` (and "obsolete" / "vanished") messages are
     omitted entirely -- lrelease drops messages it considers untranslated.
  3. A bare `<translation></translation>` IS emitted, stored as
     NO_TRANSLATION (0xffffffff).  lrelease distinguishes "not translated yet"
     (dropped) from "explicitly empty" (kept), which is why tr_en.qm has three
     entries while about_dialog_en.qm is a bare 16-byte magic header.
  4. A translation unit with no messages at all is written as the magic header
     only -- no zero-length hash or message tables.

A further rule came from the same comparison: when the <TS> element declares
language="ja_JP", a language block (tag 0xa7) is written between the magic and
the hash table.  Without that attribute no block is written.

The one exception in this tree is about_dialog_ja.qm, which has no language
block even though its .qtts declares language="ja_JP" -- it was produced by an
older lrelease.  The block is metadata that QTranslator does not consult for
lookup, so the file is regenerated without it to keep the existing bytes stable;
pass --no-language to force that behaviour.
"""

import io
import re
import struct
import sys

MAGIC = bytes.fromhex("3cb86418caef9c95cd211cbf60a1bddd")

TAG_LANGUAGE = 0xA7
TAG_HASHES = 0x42
TAG_MESSAGES = 0x69

TAG_END = 1
TAG_TRANSLATION = 3
TAG_SOURCE_TEXT = 6
TAG_CONTEXT = 7
TAG_COMMENT = 8

NO_TRANSLATION = 0xFFFFFFFF

# Translation types lrelease omits from the .qm.
DROPPED_TYPES = {"unfinished", "obsolete", "vanished"}


def elf_hash(name):
    """Qt's elfHash() over the UTF-8 bytes, including elfHash_finish()."""
    h = 0
    for c in name:
        h = ((h << 4) + c) & 0xFFFFFFFF
        g = h & 0xF0000000
        if g:
            h ^= g >> 24
        h &= ~g & 0xFFFFFFFF
    # elfHash_finish() maps 0 to 1.
    return h or 1


def u32(value):
    return struct.pack(">I", value)


def message_block(source, context, translation):
    """Empty or None translation is written as NO_TRANSLATION (0xffffffff).

    lrelease keeps an explicitly empty `<translation></translation>` (that is
    why tr_en.qm has three entries) but stores it as "no translation" rather
    than a zero-length payload.
    """
    out = bytearray()
    if not translation:
        out += bytes([TAG_TRANSLATION]) + u32(NO_TRANSLATION)
    else:
        data = translation.encode("utf-16-be")
        out += bytes([TAG_TRANSLATION]) + u32(len(data)) + data
    out += bytes([TAG_COMMENT]) + u32(0)
    for tag, text in ((TAG_SOURCE_TEXT, source), (TAG_CONTEXT, context)):
        encoded = text.encode("utf-8")
        out += bytes([tag]) + u32(len(encoded)) + encoded
    out += bytes([TAG_END])
    return bytes(out)


def build(messages, language=None):
    """messages: iterable of (context, source, translation-or-None).

    Messages are ordered by (context, source), which is what lrelease does.
    language is the .qtts <TS language="..."> value; when present it is written
    as a language block ahead of the hash table.
    """
    ordered = sorted(messages, key=lambda m: (m[0], m[1]))
    message_array = bytearray()
    entries = []
    for context, source, translation in ordered:
        entries.append((elf_hash(source.encode("utf-8")), len(message_array)))
        message_array += message_block(source, context, translation)

    # QTranslator binary-searches this table, so it must be hash-sorted.
    entries.sort()
    hash_array = b"".join(u32(h) + u32(off) for h, off in entries)

    out = bytearray(MAGIC)
    if language:
        encoded = language.encode("utf-8")
        out += bytes([TAG_LANGUAGE]) + u32(len(encoded)) + encoded
    # lrelease writes nothing at all for an empty translation unit, not even
    # zero-length tables.
    if not message_array:
        return bytes(out)
    out += bytes([TAG_HASHES]) + u32(len(hash_array)) + hash_array
    out += bytes([TAG_MESSAGES]) + u32(len(message_array)) + message_array
    return bytes(out)


# ---------------------------------------------------------------------------
# .qtts parsing
# ---------------------------------------------------------------------------

ENTITY = {
    "&lt;": "<",
    "&gt;": ">",
    "&quot;": '"',
    "&apos;": "'",
    "&amp;": "&",
}
ENTITY_RE = re.compile(r"&(?:lt|gt|quot|apos|amp|#\d+|#x[0-9a-fA-F]+);")


def unescape(text):
    def repl(m):
        token = m.group(0)
        if token.startswith("&#x"):
            return chr(int(token[3:-1], 16))
        if token.startswith("&#"):
            return chr(int(token[2:-1]))
        return ENTITY[token]

    return ENTITY_RE.sub(repl, text)


def parse_qtts(path):
    """Returns [(context, source, translation-or-None)] in file order.

    translation is None when the message is dropped (unfinished/obsolete/
    vanished), and "" when the translation is explicitly empty.
    """
    data = io.open(path, encoding="utf-8").read()
    rows = []
    for cm in re.finditer(r"<context>(.*?)</context>", data, re.S):
        body = cm.group(1)
        nm = re.search(r"<name>(.*?)</name>", body, re.S)
        context = unescape(nm.group(1)) if nm else ""
        for mm in re.finditer(r"<message\b[^>]*>(.*?)</message>", body, re.S):
            mbody = mm.group(1)
            sm = re.search(r"<source>(.*?)</source>", mbody, re.S)
            if not sm:
                continue
            tm = re.search(r"<translation\b([^>]*)>(.*?)</translation>",
                           mbody, re.S)
            if tm:
                attrs, raw = tm.group(1), tm.group(2)
                kind = re.search(r'type="(\w+)"', attrs)
                if kind and kind.group(1) in DROPPED_TYPES:
                    continue
                translation = unescape(raw)
            else:
                # No <translation> element at all: treat as untranslated.
                continue
            rows.append((context, unescape(sm.group(1)), translation))
    return rows


def parse_qtts_language(path):
    """Returns the <TS language="..."> value, or None when absent."""
    head = io.open(path, encoding="utf-8").read(600)
    m = re.search(r'<TS\b[^>]*\blanguage="([^"]*)"', head)
    return m.group(1) if m else None


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    write_language = "--no-language" not in argv
    if len(args) < 2 or len(args) % 2:
        print(__doc__)
        print("Usage: gen_qm_from_qtts.py [--no-language] <in.qtts> <out.qm> [...]")
        return 2
    for src, dst in zip(args[0::2], args[1::2]):
        rows = parse_qtts(src)
        keys = [(c, s) for c, s, _ in rows]
        dupes = {k for k in keys if keys.count(k) > 1}
        if dupes:
            print(f"{src}: duplicate (context, source) entries: {dupes}")
            return 1
        language = parse_qtts_language(src) if write_language else None
        data = build(rows, language)
        with open(dst, "wb") as f:
            f.write(data)
        translated = sum(1 for _, _, t in rows if t)
        print(f"{src} -> {dst}: {len(rows)} messages "
              f"({translated} translated, {len(rows) - translated} empty), "
              f"language={language}, {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
