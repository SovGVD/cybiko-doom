#!/usr/bin/env python3
"""Unpack Cybiko EZ Loader packs (.cap, .csu) and Cybiko application archives.

    cybiko_cap.py list PACK.cap              what the pack holds
    cybiko_cap.py extract PACK.cap DIR       write its files to DIR
    cybiko_cap.py app FILE.app [DIR]         list (or unpack) the parts of a
                                             CyOS archive (.app, .dl, MXD clips)

`extract` writes the files the way the Cybiko stores them, so they can go
straight into /Cybiko/apps on the board's SD card (Cybiko mode copies them
onto the RAM disk). Scripts (install.js, update.js) come out as UTF-8 text;
EZ Loader ran them on the PC and they are not for the Cybiko.

The formats are described in file-formats.md of cybiko-docs
(https://github.com/SovGVD/cybiko-docs). Python 3, no
extra packages.
"""
import bz2
import os
import struct
import sys

TYPE_NAMES = {1: "folder", 2: "file", 3: "script"}


# ---- EZ Loader packs ------------------------------------------------------

def unchain16(words, key):
    """Undo the 16-bit XOR chain: p[0] = c[0] ^ key, p[i] = c[i] ^ p[i-1]."""
    out, prev = [], key
    for i, c in enumerate(words):
        p = c ^ (key if i == 0 else prev)
        out.append(p)
        prev = p
    return out


def unchain8(data, key):
    """The same chain over bytes."""
    out = bytearray(len(data))
    prev = key
    for i, c in enumerate(data):
        prev = out[i] = c ^ prev
    return bytes(out)


def parse_pack(blob):
    """Return the entries of a .cap/.csu pack and its decompressed data."""
    if blob[:3] != b"CAP":
        raise ValueError("not an EZ Loader pack (no CAP header)")
    count = struct.unpack_from("<I", blob, 0x17)[0]
    pos = 0x1B
    entries = []
    for i in range(count):
        etype, size, mtime, attr = struct.unpack_from("<IIII", blob, pos)
        size ^= 0x55555555
        pos += 16
        # The name is UTF-16 under a 16-bit XOR chain (key 0x8228) with no
        # terminator; a 32-bit word follows it, then the next entry's type
        # (1-3) or, after the last entry, the bzip2 stream.
        name_len = None
        for n in range(1, 128):
            after = pos + 2 * n + 4
            nxt = blob[after:after + 4]
            if i + 1 < count:
                ok = len(nxt) == 4 and nxt[1:] == b"\0\0\0" and 1 <= nxt[0] <= 3
            else:
                ok = nxt[:3] == b"BZh"
            if ok:
                name_len = n
                break
        if name_len is None:
            raise ValueError("entry %d: cannot find the end of its name" % i)
        words = struct.unpack_from("<%dH" % name_len, blob, pos)
        name = "".join(chr(c) for c in unchain16(words, 0x8228))
        pos += 2 * name_len
        extra = struct.unpack_from("<I", blob, pos)[0]
        pos += 4
        entries.append({"type": etype, "size": size, "time": mtime, "attr": attr,
                        "name": name, "extra": extra})
    data = bz2.decompress(blob[pos:])
    off = 0
    for e in entries:
        e["data"] = data[off:off + e["size"]]
        off += e["size"]
    return entries


def decode_file(e):
    """A pack entry's contents as the Cybiko (or EZ Loader) uses them.

    Each file starts with a 12-byte header (a 32-bit check value, then
    zeros). Files are under the 8-bit chain with key 0x82, scripts are
    UTF-16 text under the 16-bit chain with key 0x8228."""
    body = e["data"][12:]
    if e["type"] == 3:
        n = len(body) // 2
        words = unchain16(struct.unpack_from("<%dH" % n, body), 0x8228)
        text = struct.pack("<%dH" % n, *words).decode("utf-16-le", errors="replace")
        return text.replace("\r\n", "\n").encode("utf-8")
    return unchain8(body, 0x82)


# ---- CyOS archives ("Cy", the .app/.dl format) ----------------------------

class Bits:
    """LSB-first bit reader."""

    def __init__(self, data):
        self.data, self.pos, self.cur, self.left = data, 0, 0, 0

    def bit(self):
        if not self.left:
            self.cur = self.data[self.pos] if self.pos < len(self.data) else 0
            self.pos += 1
            self.left = 8
        b = self.cur & 1
        self.cur >>= 1
        self.left -= 1
        return b

    def bits(self, n):
        v = 0
        for _ in range(n):
            v = v << 1 | self.bit()
        return v


def _length(b):
    if not b.bit():
        return 2 + b.bit()
    if not b.bit():
        return 4 + b.bit()
    if not b.bit():
        return 6 if not b.bit() else 8 + b.bit()
    if not b.bit():
        return 7 if not b.bit() else 10 + b.bit()
    t = b.bits(3)
    if t == 0:
        return 12
    if t == 1:
        return 13
    if t == 2:
        return 0x14 + b.bits(2)
    if t == 3:
        return 16 + b.bit()
    if t == 4:
        return 0x18 + b.bits(3)
    if t == 5:
        return 14
    if t == 6:
        return 0x20 + b.bits(5) if not b.bit() else 18 + b.bit()
    if not b.bit():
        return 15
    if not b.bit():
        return 0x40 + b.bits(6)
    if not b.bit():
        return 0x80 + b.bits(7)
    return 0x100


def _offset(b):
    t = b.bits(3)
    if t == 0:
        return 0x400 + b.bits(10)
    if t == 1:
        return 0x200 + b.bits(9)
    if t == 2:
        return 0x100 + b.bits(8)
    if t == 3:
        return 0x40 + b.bits(6)
    if t == 4:
        return 0x80 + b.bits(7)
    if t == 5:
        return 0x800 + b.bits(11) if not b.bit() else 0x1000 + b.bits(12)
    if t == 6:
        return 0x20 + b.bits(5) if not b.bit() else 0x10 + b.bits(4)
    u = b.bits(2)
    if u == 0:
        return b.bits(1)
    if u == 1:
        return 6 + b.bits(2)
    if u == 2:
        return 2 if b.bit() else 4 + b.bit()
    if not b.bit():
        return 0xC + b.bits(2)
    if not b.bit():
        return 3
    return 10 + b.bit()


def cy_unpack(data, size):
    """CyOS method 2: literals and back references (see the doc)."""
    b, out = Bits(data), bytearray()
    while len(out) < size:
        if not b.bit():
            out.append(b.bits(8))
        else:
            length = _length(b)
            dist = _offset(b) + length
            for _ in range(length):
                out.append(out[-dist] if len(out) >= dist else 0)
    return bytes(out[:size])


def cy_entries(blob):
    """(name, method, contents) for each part of a Cy archive."""
    if blob[:2] != b"Cy":
        raise ValueError("not a CyOS archive (no Cy header)")
    n = struct.unpack_from(">H", blob, 2)[0]
    parts = []
    for i in range(n):
        name_off, off, size = struct.unpack_from(">HII", blob, 6 + 10 * i)
        name = blob[name_off:blob.index(b"\0", name_off)].decode("latin-1")
        raw = blob[off:off + size]
        method = raw[0] if raw else 0
        if method == 0:
            data = raw[1:]
        elif method == 2:
            data = cy_unpack(raw[5:], struct.unpack_from(">I", raw, 1)[0])
        else:
            data = None
        parts.append((name, method, data))
    return parts


# Codes for method 2, as (bits read before the extra bits, base, extra bits);
# they mirror _length and _offset above.
_LENGTH_CODES = [("0", 2, 1), ("10", 4, 1), ("1100", 6, 0), ("1101", 8, 1),
                 ("11100", 7, 0), ("11101", 10, 1), ("1111000", 12, 0),
                 ("1111001", 13, 0), ("1111010", 20, 2), ("1111011", 16, 1),
                 ("1111100", 24, 3), ("1111101", 14, 0), ("11111100", 32, 5),
                 ("11111101", 18, 1), ("11111110", 15, 0), ("111111110", 64, 6),
                 ("1111111110", 128, 7), ("1111111111", 256, 0)]
_OFFSET_CODES = [("000", 1024, 10), ("001", 512, 9), ("010", 256, 8), ("011", 64, 6),
                 ("100", 128, 7), ("1010", 2048, 11), ("1011", 4096, 12),
                 ("1100", 32, 5), ("1101", 16, 4), ("11100", 0, 1), ("11101", 6, 2),
                 ("111101", 2, 0), ("111100", 4, 1), ("111110", 12, 2),
                 ("1111110", 3, 0), ("1111111", 10, 1)]


def _code_table(codes, top):
    table = [None] * (top + 1)
    for prefix, base, extra in codes:
        for v in range(1 << extra):
            bits = prefix + (format(v, "0%db" % extra) if extra else "")
            table[base + v] = bits
    return table


_LENGTHS = _code_table(_LENGTH_CODES, 256)
_OFFSETS = _code_table(_OFFSET_CODES, 8191)
MAX_MATCH = 256
MAX_DIST = 8191                 # the CyOS decoder's window


def cy_pack(data):
    """Compress with CyOS method 2 (greedy LZ77). A back reference reaches
    `length` to `length` + 8191 bytes back in the format, but the CyOS
    decoder keeps only an 8 KB window: a reference further back than 8191
    bytes reads stale bytes there, so none is made."""
    bits = []
    heads = {}
    i, n = 0, len(data)

    def remember(pos):
        if pos + 3 <= n:
            heads.setdefault(data[pos:pos + 3], []).append(pos)

    while i < n:
        best_len, best_dist = 0, 0
        for j in reversed(heads.get(data[i:i + 3], ())[-48:]):
            dist = i - j
            if dist > MAX_DIST:
                break
            limit = min(MAX_MATCH, dist, n - i)
            if limit <= best_len:
                continue
            k = 0
            while k < limit and data[j + k] == data[i + k]:
                k += 1
            if k >= 2 and k > best_len:
                best_len, best_dist = k, dist
                if k == MAX_MATCH:
                    break
        if best_len >= 2:
            bits.append("1" + _LENGTHS[best_len] + _OFFSETS[best_dist - best_len])
            for p in range(i, i + best_len):
                remember(p)
            i += best_len
        else:
            bits.append("0" + format(data[i], "08b"))
            remember(i)
            i += 1
    s = "".join(bits)
    out = bytearray((len(s) + 7) // 8)
    for k, c in enumerate(s):
        if c == "1":
            out[k >> 3] |= 1 << (k & 7)
    return bytes(out)


def cy_archive(parts):
    """Build a Cy archive from (name, contents, compress) tuples.

    Layout: "Cy", u16 count, u16 size of the directory that follows, then
    count x (u16 name offset, u32 data offset, u32 stored size), the names
    (NUL-terminated), and the data. Each part's data starts with its method:
    0 and the bytes, or 2, the u32 unpacked size and the bit stream. All
    numbers are big-endian; offsets count from the start of the archive."""
    names = b"".join(p[0].encode("latin-1") + b"\0" for p in parts)
    stored = []
    for name, data, compress in parts:
        packed = cy_pack(data) if compress else None
        if packed is not None and len(packed) + 5 < len(data) + 1:
            stored.append(b"\x02" + struct.pack(">I", len(data)) + packed)
        else:
            stored.append(b"\x00" + data)
    table = 6 + 10 * len(parts)
    data_start = table + len(names)
    head = bytearray(b"Cy" + struct.pack(">HH", len(parts), data_start - 6))
    name_off, off = table, data_start
    for (name, _, _), blob in zip(parts, stored):
        head += struct.pack(">HII", name_off, off, len(blob))
        name_off += len(name.encode("latin-1")) + 1
        off += len(blob)
    return bytes(head) + names + b"".join(stored)


# ---- command line --------------------------------------------------------

def safe_name(name):
    name = name.replace("\\", "/").split("/")[-1]
    return name if name not in ("", ".", "..") else "_"


def main(argv):
    if len(argv) < 3 or argv[1] not in ("list", "extract", "app"):
        print(__doc__)
        return 2
    cmd, path = argv[1], argv[2]
    blob = open(path, "rb").read()
    if cmd == "app":
        out = argv[3] if len(argv) > 3 else None
        for name, method, data in cy_entries(blob):
            size = len(data) if data is not None else "?"
            print("%-24s method %d  %s bytes" % (name, method, size))
            if out and data is not None:
                os.makedirs(out, exist_ok=True)
                with open(os.path.join(out, safe_name(name)), "wb") as f:
                    f.write(data)
        return 0
    entries = parse_pack(blob)
    out = argv[3] if cmd == "extract" else None
    if cmd == "extract" and not out:
        print("extract needs a folder")
        return 2
    for e in entries:
        kind = TYPE_NAMES.get(e["type"], "type %d" % e["type"])
        print("%-7s %8d  %s" % (kind, e["size"], e["name"]))
        if out and e["type"] in (2, 3):
            os.makedirs(out, exist_ok=True)
            with open(os.path.join(out, safe_name(e["name"])), "wb") as f:
                f.write(decode_file(e))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
