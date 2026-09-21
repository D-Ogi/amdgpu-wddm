"""Pack a diagnostic summary into QR-sized text chunks and back.

Format of one chunk (all characters are in the QR alphanumeric set, so codes stay small):

    BC250:1:<CRC32 of payload, 8 hex>:<index>/<count>:<base41 data>

payload = zlib(JSON). Base41 uses 0-9 A-Z and "$+-./": no space, '%', '*' or ':' so that the
text survives phone scanner apps, messengers and copy-paste, and the header splits on ':'.
Used by the USB payload (encode) and by tools/diagusb/decode_qr.py on the PC (decode).
"""

import json
import re
import zlib

ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ$+-./"
BASE = len(ALPHABET)  # 41: 41**3 >= 65536 and 41**2 >= 256
_INDEX = {c: i for i, c in enumerate(ALPHABET)}
MAGIC = "BC250:1"
CHUNK_CHARS = 1200  # + header fits QR version 20-L (1249 alphanumeric characters)


def b41encode(data):
    out = []
    for i in range(0, len(data) - 1, 2):
        n = (data[i] << 8) | data[i + 1]
        out.append(ALPHABET[n // (BASE * BASE)] + ALPHABET[(n // BASE) % BASE] + ALPHABET[n % BASE])
    if len(data) % 2:
        n = data[-1]
        out.append(ALPHABET[n // BASE] + ALPHABET[n % BASE])
    return "".join(out)


def b41decode(text):
    if len(text) % 3 == 1:
        raise ValueError("truncated base41 data")
    out = bytearray()
    for i in range(0, len(text) - len(text) % 3, 3):
        n = (_INDEX[text[i]] * BASE + _INDEX[text[i + 1]]) * BASE + _INDEX[text[i + 2]]
        if n > 0xFFFF:
            raise ValueError("corrupt base41 data")
        out += bytes((n >> 8, n & 0xFF))
    if len(text) % 3 == 2:
        n = _INDEX[text[-2]] * BASE + _INDEX[text[-1]]
        if n > 0xFF:
            raise ValueError("corrupt base41 data")
        out.append(n)
    return bytes(out)


def encode(summary, chunk_chars=CHUNK_CHARS):
    """dict -> list of chunk strings, one per QR code."""
    payload = zlib.compress(json.dumps(summary, separators=(",", ":"), sort_keys=True).encode(), 9)
    crc = f"{zlib.crc32(payload):08X}"
    text = b41encode(payload)
    # Cut on 3-character boundaries so every chunk decodes on its own.
    step = chunk_chars - chunk_chars % 3
    parts = [text[i:i + step] for i in range(0, len(text), step)] or [""]
    return [f"{MAGIC}:{crc}:{i + 1}/{len(parts)}:{p}" for i, p in enumerate(parts)]


_CHUNK_RE = re.compile(r"BC250:1:([0-9A-F]{8}):(\d+)/(\d+):([0-9A-Z$+\-./]*)")


def find_chunks(text):
    """{(crc, count): {index: data}} for every run the text mentions. Duplicates collapse."""
    found = {}
    for crc, idx, count, data in _CHUNK_RE.findall(text.upper()):
        found.setdefault((crc, int(count)), {})[int(idx)] = data
    return found


def decode(text):
    """Any text containing the scanned chunks (any order, duplicates fine) -> dict.

    Raises ValueError naming the missing chunk numbers if the set is incomplete.
    """
    found = find_chunks(text)
    if not found:
        raise ValueError("no BC250 chunks found in input")
    (crc, count), parts = max(found.items(), key=lambda kv: len(kv[1]))
    missing = [i for i in range(1, count + 1) if i not in parts]
    if missing:
        raise ValueError(f"run {crc}: missing chunk(s) {missing} of {count}")
    payload = b41decode("".join(parts[i] for i in range(1, count + 1)))
    if f"{zlib.crc32(payload):08X}" != crc:
        raise ValueError(f"run {crc}: CRC mismatch, rescan the codes")
    return json.loads(zlib.decompress(payload))
