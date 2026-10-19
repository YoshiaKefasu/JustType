"""Regenerates gui/base/tr_{ja,en}.qm without the Qt Linguist tools.

The Qt build in this checkout sets QT_WILL_BUILD_TOOLS=FALSE, so lrelease.exe
does not exist and the checked-in .qm files could not otherwise be regenerated
after editing tr_{ja,en}.qtts.  This script emits the same minimal format
lrelease does: a magic header, a hash table (tag 0x42), and a message table
(tag 0x69).  It was verified to reproduce the previously checked-in files
byte-for-byte.

Message block layout, from qt_src/src/corelib/kernel/qtranslator.cpp:
  Tag_Translation(3) len <utf16be>   len 0xffffffff means "no translation"
  Tag_Comment(8)     len <utf8>      len 0 always
  Tag_SourceText(6)  len <utf8>
  Tag_Context(7)     len <utf8>
  Tag_End(1)

Usage (from the src directory):
  python build_tools/gen_qm.py
"""

import os
import struct
import sys

MAGIC = bytes.fromhex("3cb86418caef9c95cd211cbf60a1bddd")
TAG_HASHES = 0x42
TAG_MESSAGES = 0x69

TAG_END = 1
TAG_TRANSLATION = 3
TAG_SOURCE_TEXT = 6
TAG_CONTEXT = 7
TAG_COMMENT = 8

NO_TRANSLATION = 0xFFFFFFFF


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
    out = bytearray()
    if translation:
        data = translation.encode("utf-16-be")
        out += bytes([TAG_TRANSLATION]) + u32(len(data)) + data
    else:
        out += bytes([TAG_TRANSLATION]) + u32(NO_TRANSLATION)
    out += bytes([TAG_COMMENT]) + u32(0)
    for tag, text in ((TAG_SOURCE_TEXT, source), (TAG_CONTEXT, context)):
        encoded = text.encode("utf-8")
        out += bytes([tag]) + u32(len(encoded)) + encoded
    out += bytes([TAG_END])
    return bytes(out)


def build(messages):
    """messages: list of (context, source, translation)."""
    message_array = bytearray()
    entries = []
    for context, source, translation in messages:
        entries.append((elf_hash(source.encode("utf-8")), len(message_array)))
        message_array += message_block(source, context, translation)

    # QTranslator binary-searches this table, so it must be hash-sorted.
    entries.sort()
    hash_array = b"".join(u32(h) + u32(off) for h, off in entries)

    out = bytearray(MAGIC)
    out += bytes([TAG_HASHES]) + u32(len(hash_array)) + hash_array
    out += bytes([TAG_MESSAGES]) + u32(len(message_array)) + message_array
    return bytes(out)


# Keep these in sync with the <message> entries in tr_{ja,en}.qtts.
JA_MESSAGES = [
    ("QObject", "Google Japanese Input", "Google 日本語入力"),
    ("QObject", "JustType", "ジャストタイプ"),
    ("QObject", "product forum", "プロダクトフォーラム"),
]

EN_MESSAGES = [
    ("QObject", "Google Japanese Input", ""),
    ("QObject", "JustType", ""),
    ("QObject", "product forum", ""),
]


def main():
    out_dir = os.path.join("gui", "base")
    outputs = [
        (os.path.join(out_dir, "tr_ja.qm"), JA_MESSAGES),
        (os.path.join(out_dir, "tr_en.qm"), EN_MESSAGES),
    ]
    for path, messages in outputs:
        data = build(messages)
        with open(path, "wb") as f:
            f.write(data)
        print(f"wrote {path} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
