#!/usr/bin/env python3
"""Turn the maps of a Doom IWAD into the level pack Cybiko Doom plays.

    wad2cy.py doom1.wad OUTDIR [--maps E1M1,E1M2] [--texels N] [--list]

No WAD data is kept in this repository. You give the converter your own WAD
(the shareware doom1.wad, the registered doom.wad or Ultimate Doom, or
Freedoom's freedoom1.wad) and it writes the level pack into OUTDIR:
doom.dat, with the sprites and the map list, and a file per map,
doom_e1m1.dat and so on. Together they hold everything the game needs:

  * the map geometry: vertexes, sectors, sides, lines, segs, subsectors,
    BSP nodes, things, the blockmap and the reject table;
  * one grey tone for every wall texture and floor/ceiling flat, the
    average brightness of its pixels, since the Cybiko draws flat walls;
  * the monster, item and weapon sprites shrunk to at most 32x32 cells of
    16 grey tones, with their outline marked.

The pack is big-endian (as the H8S is) and packed with Deflate (zlib; the
game has its own decoder, src/inflate.c); see PACK FORMAT below and
src/level.c, which must agree with it. The game reads it from the MP3
cartridge's card or the Cybiko's own storage, one map at a time, and plays
the maps whose files it finds.

Python 3, no extra packages.
"""
import argparse
import math
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# WAD access

class Wad:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        magic, n, off = struct.unpack_from("<4sii", self.data, 0)
        if magic not in (b"IWAD", b"PWAD"):
            sys.exit("%s: not a WAD file" % path)
        self.lumps = []
        for i in range(n):
            pos, size, name = struct.unpack_from("<ii8s", self.data, off + 16 * i)
            name = name.split(b"\0")[0].decode("latin-1").upper()
            self.lumps.append((name, pos, size))
        self.index = {}
        for i, (name, _, _) in enumerate(self.lumps):
            self.index.setdefault(name, i)

    def find(self, name, start=0):
        for i in range(start, len(self.lumps)):
            if self.lumps[i][0] == name:
                return i
        return -1

    def lump(self, i):
        _, pos, size = self.lumps[i]
        return self.data[pos:pos + size]

    def get(self, name):
        i = self.index.get(name.upper())
        return None if i is None else self.lump(i)

    def between(self, start, end):
        """The lumps between marker lumps such as S_START and S_END."""
        out = {}
        inside = False
        for i, (name, _, size) in enumerate(self.lumps):
            if name in start:
                inside = True
            elif name in end:
                inside = False
            elif inside and size:
                out[name] = i
        return out


# ---------------------------------------------------------------------------
# Brightness of textures and flats

def palette_lum(wad):
    pal = wad.get("PLAYPAL")
    out = []
    for i in range(256):
        r, g, b = pal[3 * i:3 * i + 3]
        out.append(0.299 * r + 0.587 * g + 0.114 * b)
    return out


def picture_pixels(data):
    """The opaque pixels of a Doom picture (patch or sprite), column by
    column: (width, height, leftoffset, topoffset, columns), where each
    column is a list of (row, palette index)."""
    w, h, lo, to = struct.unpack_from("<hhhh", data, 0)
    cols = []
    for x in range(w):
        pos = struct.unpack_from("<I", data, 8 + 4 * x)[0]
        col = []
        top = -1
        while pos < len(data):
            rowstart = data[pos]
            if rowstart == 255:
                break
            # tall patches: a start at or below the last one is relative
            top = rowstart if rowstart > top else top + rowstart
            n = data[pos + 1]
            for k in range(n):
                col.append((top + k, data[pos + 3 + k]))
            pos += n + 4
        cols.append(col)
    return w, h, lo, to, cols


def texture_tones(wad, lum):
    """Average brightness (0-255) of every wall texture."""
    names = []
    pnames = wad.get("PNAMES")
    n = struct.unpack_from("<i", pnames, 0)[0]
    for i in range(n):
        names.append(pnames[4 + 8 * i:12 + 8 * i].split(b"\0")[0].decode("latin-1").upper())
    patch_lum = {}

    def plum(name):
        if name not in patch_lum:
            data = wad.get(name)
            if data is None:
                patch_lum[name] = None
            else:
                _, _, _, _, cols = picture_pixels(data)
                px = [lum[c] for col in cols for _, c in col]
                patch_lum[name] = (sum(px), len(px)) if px else None
        return patch_lum[name]

    tones = {}
    for lname in ("TEXTURE1", "TEXTURE2"):
        t = wad.get(lname)
        if t is None:
            continue
        count = struct.unpack_from("<i", t, 0)[0]
        for i in range(count):
            off = struct.unpack_from("<i", t, 4 + 4 * i)[0]
            name = t[off:off + 8].split(b"\0")[0].decode("latin-1").upper()
            npatch = struct.unpack_from("<h", t, off + 20)[0]
            s = c = 0
            for p in range(npatch):
                _, _, pidx, _, _ = struct.unpack_from("<hhhhh", t, off + 22 + 10 * p)
                r = plum(names[pidx]) if 0 <= pidx < len(names) else None
                if r:
                    s += r[0]
                    c += r[1]
            tones[name] = int(s / c) if c else 128
    return tones


def pow2(n):
    p = 1
    while p < n:
        p <<= 1
    return p


class Textures:
    """Wall textures composed from their patches and shrunk, in 16 tones, so
    that the longer side has at most `texels` texels: one texel covers a
    square of 2**shift pixels. The texel counts are rounded up to powers of
    two (the texture is repeated to fill them) so the game can wrap with a
    mask."""

    def __init__(self, wad, lum, texels):
        self.wad, self.lum, self.texels = wad, lum, texels
        pnames = wad.get("PNAMES")
        n = struct.unpack_from("<i", pnames, 0)[0]
        self.pnames = [pnames[4 + 8 * i:12 + 8 * i].split(b"\0")[0].decode("latin-1").upper()
                       for i in range(n)]
        self.defs = {}
        for lname in ("TEXTURE1", "TEXTURE2"):
            t = wad.get(lname)
            if t is None:
                continue
            for i in range(struct.unpack_from("<i", t, 0)[0]):
                off = struct.unpack_from("<i", t, 4 + 4 * i)[0]
                name = t[off:off + 8].split(b"\0")[0].decode("latin-1").upper()
                w, h = struct.unpack_from("<hh", t, off + 12)
                patches = [struct.unpack_from("<hhh", t, off + 22 + 10 * p)
                           for p in range(struct.unpack_from("<h", t, off + 20)[0])]
                self.defs[name] = (w, h, patches)
        self.patches = {}
        self.cache = {}

    def patch(self, idx):
        if idx not in self.patches:
            data = self.wad.get(self.pnames[idx]) if 0 <= idx < len(self.pnames) else None
            self.patches[idx] = picture_pixels(data) if data else None
        return self.patches[idx]

    def get(self, name):
        """(log2 width, log2 height, shift, texels column by column) or None."""
        if name in self.cache:
            return self.cache[name]
        d = self.defs.get(name)
        out = None
        if d:
            w, h, patches = d
            img = [[None] * h for _ in range(w)]
            for ox, oy, idx in patches:
                pic = self.patch(idx)
                if not pic:
                    continue
                for x, col in enumerate(pic[4]):
                    if 0 <= ox + x < w:
                        for y, c in col:
                            if 0 <= oy + y < h:
                                img[ox + x][oy + y] = self.lum[c]
            px = [v for col in img for v in col if v is not None]
            mean = sum(px) / len(px) if px else 128
            f = pow2(-(-max(w, h) // self.texels))
            tw, th = pow2(-(-w // f)), pow2(-(-h // f))
            texels = bytearray()
            for tx in range(tw):
                for ty in range(th):
                    vals = [img[(tx * f + i) % w][(ty * f + j) % h]
                            for i in range(f) for j in range(f)]
                    vals = [v for v in vals if v is not None]
                    v = sum(vals) / len(vals) if vals else mean
                    v = mean + (v - mean) * 1.6         # more contrast for 4 greys
                    texels.append(max(0, min(15, int(v / 16))))
            out = (tw.bit_length() - 1, th.bit_length() - 1, f.bit_length() - 1, bytes(texels))
        self.cache[name] = out
        return out


def flat_tones(wad, lum):
    tones = {}
    for name, i in wad.between(("F_START", "FF_START"), ("F_END", "FF_END")).items():
        data = wad.lump(i)
        tones[name] = int(sum(lum[b] for b in data) / len(data)) if data else 128
    return tones


# ---------------------------------------------------------------------------
# Things the game knows. Each entry: doomednum -> (class, sprite frames).
# The class numbers are the indices of the table in src/info.c; keep them in
# step. Sprite frames are lump names; missing ones are skipped (Freedoom has
# all of them, shareware Doom lacks the registered-only monsters, which are
# then dropped from the maps).

THINGS = {
    # player start
    1: ("PLAYER", []),
    14: ("TELEPORTMAN", []),
    # monsters: stand/walk A, walk C, attack, pain, dead
    3004: ("ZOMBIE", ["POSSA1", "POSSC1", "POSSF1", "POSSG1", "POSSL0"]),
    9: ("SERGEANT", ["SPOSA1", "SPOSC1", "SPOSF1", "SPOSG1", "SPOSL0"]),
    3001: ("IMP", ["TROOA1", "TROOC1", "TROOG1", "TROOH1", "TROOM0"]),
    3002: ("DEMON", ["SARGA1", "SARGC1", "SARGF1", "SARGH1", "SARGN0"]),
    58: ("SPECTRE", ["SARGA1", "SARGC1", "SARGF1", "SARGH1", "SARGN0"]),
    3005: ("CACODEMON", ["HEADA1", "HEADA1", "HEADD1", "HEADE1", "HEADL0"]),
    3003: ("BARON", ["BOSSA1", "BOSSC1", "BOSSG1", "BOSSH1", "BOSSO0"]),
    3006: ("LOSTSOUL", ["SKULA1", "SKULB1", "SKULC1", "SKULE1", "SKULK0"]),
    2035: ("BARREL", ["BAR1A0", "BAR1B0", "BEXPA0", "BEXPB0", "BEXPE0"]),
    # weapons
    2001: ("SHOTGUN", ["SHOTA0"]),
    2002: ("CHAINGUN", ["MGUNA0"]),
    2003: ("LAUNCHER", ["LAUNA0"]),
    2004: ("PLASMA", ["PLASA0"]),
    2005: ("CHAINSAW", ["CSAWA0"]),
    2006: ("BFG", ["BFUGA0"]),
    # ammo
    2007: ("CLIP", ["CLIPA0"]),
    2048: ("AMMOBOX", ["AMMOA0"]),
    2008: ("SHELLS", ["SHELA0"]),
    2049: ("SHELLBOX", ["SBOXA0"]),
    2010: ("ROCKET", ["ROCKA0"]),
    2046: ("ROCKETBOX", ["BROKA0"]),
    2047: ("CELL", ["CELLA0"]),
    17: ("CELLPACK", ["CELPA0"]),
    8: ("BACKPACK", ["BPAKA0"]),
    # health and armour
    2011: ("STIMPACK", ["STIMA0"]),
    2012: ("MEDIKIT", ["MEDIA0"]),
    2014: ("HEALTHBONUS", ["BON1A0"]),
    2015: ("ARMORBONUS", ["BON2A0"]),
    2018: ("GREENARMOR", ["ARM1A0"]),
    2019: ("BLUEARMOR", ["ARM2A0"]),
    2013: ("SOULSPHERE", ["SOULA0"]),
    2022: ("INVULN", ["PINVA0"]),
    2023: ("BERSERK", ["PSTRA0"]),
    2024: ("INVIS", ["PINSA0"]),
    2025: ("RADSUIT", ["SUITA0"]),
    2026: ("MAP", ["PMAPA0"]),
    2045: ("VISOR", ["PVISA0"]),
    # keys
    5: ("BLUECARD", ["BKEYA0"]),
    6: ("YELLOWCARD", ["YKEYA0"]),
    13: ("REDCARD", ["RKEYA0"]),
    40: ("BLUESKULL", ["BSKUA0"]),
    39: ("YELLOWSKULL", ["YSKUA0"]),
    38: ("REDSKULL", ["RSKUA0"]),
    # decorations that block
    2028: ("LAMP", ["COLUA0"]),
    48: ("TECHCOLUMN", ["ELECA0"]),
    30: ("GREENPILLAR", ["COL1A0"]),
    32: ("REDPILLAR", ["COL3A0"]),
    31: ("SHORTGREEN", ["COL2A0"]),
    33: ("SHORTRED", ["COL4A0"]),
    35: ("CANDELABRA", ["CBRAA0"]),
    41: ("EVILEYE", ["CEYEA0"]),
    42: ("FLOATSKULL", ["FSKUA0"]),
    43: ("BURNTTREE", ["TRE1A0"]),
    44: ("BLUETORCH", ["TBLUA0"]),
    45: ("GREENTORCH", ["TGRNA0"]),
    46: ("REDTORCH", ["TREDA0"]),
    47: ("STUB", ["SMITA0"]),
    54: ("BIGTREE", ["TRE2A0"]),
    55: ("SHORTBLUETORCH", ["SMBTA0"]),
    56: ("SHORTGREENTORCH", ["SMGTA0"]),
    57: ("SHORTREDTORCH", ["SMRTA0"]),
    70: ("BURNINGBARREL", ["FCANA0"]),
    # decorations that do not block
    34: ("CANDLE", ["CANDA0"]),
    10: ("BLOODYMESS", ["PLAYW0"]),
    12: ("BLOODYMESS2", ["PLAYW0"]),
    15: ("DEADPLAYER", ["PLAYN0"]),
    18: ("DEADZOMBIE", ["POSSL0"]),
    19: ("DEADSERGEANT", ["SPOSL0"]),
    20: ("DEADIMP", ["TROOM0"]),
    21: ("DEADDEMON", ["SARGN0"]),
    24: ("POOLOFBLOOD", ["POL5A0"]),
    # (hanging bodies, shared by both lists in Doom)
    25: ("IMPALED", ["POL1A0"]),
    26: ("TWITCHING", ["POL6A0"]),
    27: ("HEADONSTICK", ["POL4A0"]),
    28: ("HEADSONSTICK", ["POL2A0"]),
    29: ("HEADCANDLES", ["POL3A0"]),
    49: ("HANGINGBODY", ["GOR1A0"]),
    50: ("HANGINGARMS", ["GOR2A0"]),
    51: ("HANGINGLEG1", ["GOR3A0"]),
    52: ("HANGINGLEGS", ["GOR4A0"]),
    53: ("HANGINGLEG2", ["GOR5A0"]),
}

# Sprites that are not things on a map but are drawn by the game.
EXTRA = {
    "FIREBALL": ["BAL1A0", "BAL1C0"],      # imp fireball, flying / exploding
    "PLASMABALL": ["BAL2A0", "BAL2C0"],    # cacodemon
    "BARONBALL": ["BAL7A1", "BAL7C0"],
    "ROCKETSHOT": ["MISLA1", "MISLB0"],
    "PUFF": ["PUFFA0", "PUFFC0"],
    "BLOOD": ["BLUDA0", "BLUDC0"],
    "W_FIST": ["PUNGA0", "PUNGB0"],
    "W_PISTOL": ["PISGA0", "PISGB0"],
    "W_SHOTGUN": ["SHTGA0", "SHTGB0"],
    "W_CHAINGUN": ["CHGGA0", "CHGGB0"],
    "W_LAUNCHER": ["MISGA0", "MISGB0"],
    "W_PLASMA": ["PLSGA0", "PLSGB0"],
    "W_CHAINSAW": ["SAWGA0", "SAWGC0"],
    "W_BFG": ["BFGGA0", "BFGGB0"],
}

# Order of the classes, as in src/info.c.
CLASS_ORDER = [
    "PLAYER", "ZOMBIE", "SERGEANT", "IMP", "DEMON", "SPECTRE", "CACODEMON",
    "BARON", "LOSTSOUL", "BARREL",
    "SHOTGUN", "CHAINGUN", "LAUNCHER", "PLASMA", "CHAINSAW", "BFG",
    "CLIP", "AMMOBOX", "SHELLS", "SHELLBOX", "ROCKET", "ROCKETBOX", "CELL",
    "CELLPACK", "BACKPACK",
    "STIMPACK", "MEDIKIT", "HEALTHBONUS", "ARMORBONUS", "GREENARMOR",
    "BLUEARMOR", "SOULSPHERE", "INVULN", "BERSERK", "INVIS", "RADSUIT", "MAP",
    "VISOR",
    "BLUECARD", "YELLOWCARD", "REDCARD", "BLUESKULL", "YELLOWSKULL", "REDSKULL",
    "LAMP", "TECHCOLUMN", "GREENPILLAR", "REDPILLAR", "SHORTGREEN", "SHORTRED",
    "CANDELABRA", "EVILEYE", "FLOATSKULL", "BURNTTREE", "BLUETORCH",
    "GREENTORCH", "REDTORCH", "STUB", "BIGTREE", "SHORTBLUETORCH",
    "SHORTGREENTORCH", "SHORTREDTORCH", "BURNINGBARREL",
    "CANDLE", "BLOODYMESS", "BLOODYMESS2", "DEADPLAYER", "DEADZOMBIE",
    "DEADSERGEANT", "DEADIMP", "DEADDEMON", "POOLOFBLOOD",
    "IMPALED", "TWITCHING", "HEADONSTICK", "HEADSONSTICK", "HEADCANDLES",
    "HANGINGBODY", "HANGINGARMS", "HANGINGLEG1", "HANGINGLEGS", "HANGINGLEG2",
    "FIREBALL", "PLASMABALL", "BARONBALL", "ROCKETSHOT", "PUFF", "BLOOD",
    "W_FIST", "W_PISTOL", "W_SHOTGUN", "W_CHAINGUN", "W_LAUNCHER", "W_PLASMA",
    "W_CHAINSAW", "W_BFG",
    "TELEPORTMAN",
]
FRAMES_PER_CLASS = 5        # sprite slots per class; unused ones are empty

# ---------------------------------------------------------------------------
# Sprites
#
# A sprite is shrunk by a whole factor f so it is at most 32 cells tall and
# wide, and stored as runs of opaque cells per column. Format (bytes):
#   u8 width (cells), u8 height (cells), u8 f (map units per cell),
#   s8 left offset (cells), s8 top offset (cells), u8 0
#   then per column: u8 run count, then per run u8 start, u8 end (end
#   exclusive) and one byte per cell of the run:
#     bits 0-3  tone, 0 black .. 15 white
#     bit 4     outline on the left (no opaque cell to the left)
#     bit 5     outline on the right
# The tones are stretched to the sprite's own range of brightness, so a dark
# monster still shows its shape on the 4-grey screen.

def sprite_lump(wad, name):
    """A sprite frame by name; frames that share a lump with a mirrored
    rotation are stored as e.g. BAL7A1A5."""
    data = wad.get(name)
    if data is not None:
        return data
    for lname, _, size in wad.lumps:
        if size and len(lname) == 8 and (lname[:6] == name or lname[4:] == name[4:6] + name[4:6]
                                         and lname[:4] == name[:4] and lname[6:] == name[4:6]):
            return wad.get(lname)
    return None


def shrink_sprite(wad, name, lum):
    data = sprite_lump(wad, name)
    if data is None:
        return None
    w, h, lo, to, cols = picture_pixels(data)
    f = max(1, -(-h // 32), -(-w // 32))
    cw, ch = -(-w // f), -(-h // f)
    count = [[0] * ch for _ in range(cw)]
    total = [[0.0] * ch for _ in range(cw)]
    for x, col in enumerate(cols):
        for y, c in col:
            if 0 <= y < h:
                count[x // f][y // f] += 1
                total[x // f][y // f] += lum[c]
    # a cell is opaque when at least a third of its pixels are
    need = max(1, (f * f) // 3)
    solid = [[count[x][y] >= need for y in range(ch)] for x in range(cw)]
    vals = sorted(total[x][y] / count[x][y] for x in range(cw) for y in range(ch) if solid[x][y])
    if vals:
        lo_v = vals[len(vals) // 20]
        hi_v = vals[-1 - len(vals) // 20]
    else:
        lo_v, hi_v = 0, 255
    if hi_v - lo_v < 96:                    # do not blow up flat sprites
        mid = (hi_v + lo_v) / 2
        lo_v, hi_v = mid - 48, mid + 48

    def cell(x, y):
        t = (total[x][y] / count[x][y] - lo_v) * 15 / (hi_v - lo_v)
        b = max(0, min(15, int(round(t))))
        if x == 0 or not solid[x - 1][y]:
            b |= 0x10
        if x == cw - 1 or not solid[x + 1][y]:
            b |= 0x20
        return b

    out = bytearray([cw, ch, f, max(-128, min(127, round(lo / f))) & 0xff,
                     max(-128, min(127, round(to / f))) & 0xff, 0])
    for x in range(cw):
        runs = []
        y = 0
        while y < ch:
            if solid[x][y]:
                s = y
                while y < ch and solid[x][y]:
                    y += 1
                runs.append((s, y))
            else:
                y += 1
        runs = runs[:8]
        out.append(len(runs))
        for s, e in runs:
            out += bytes([s, e]) + bytes(cell(x, y) for y in range(s, e))
    return bytes(out)


# ---------------------------------------------------------------------------
# Maps

def map_lumps(wad, name):
    i = wad.find(name)
    if i < 0:
        return None
    out = {}
    for k in range(i + 1, min(i + 12, len(wad.lumps))):
        n = wad.lumps[k][0]
        if n in ("THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES", "SEGS", "SSECTORS",
                 "NODES", "SECTORS", "REJECT", "BLOCKMAP"):
            out[n] = wad.lump(k)
        else:
            break
    return out


def tone(tones, name, default=0):
    """Texture name -> tone 1-255, 0 = no texture."""
    name = name.upper()
    if name in ("-", "") or name.startswith("\0"):
        return 0
    return max(1, min(255, tones.get(name, default or 128)))


def convert_map(wad, name, ttones, ftones, class_of, textures):
    L = map_lumps(wad, name)
    if not L:
        return None
    be = bytearray()

    def s8(b, n):
        return b.split(b"\0")[0].decode("latin-1").upper()

    verts = [struct.unpack_from("<hh", L["VERTEXES"], 4 * i) for i in range(len(L["VERTEXES"]) // 4)]
    sectors = []
    for i in range(len(L["SECTORS"]) // 26):
        fh, ch, ft, ct, light, special, tag = struct.unpack_from("<hh8s8shhh", L["SECTORS"], 26 * i)
        ft, ct = s8(ft, 8), s8(ct, 8)
        sectors.append((fh, ch, light, special, tag,
                        max(1, min(255, ftones.get(ft, 128))),
                        0 if ct.startswith("F_SKY") else max(1, min(255, ftones.get(ct, 128))),
                        1 if ft.startswith("NUKAGE") or ft.startswith("FWATER")
                        or ft.startswith("LAVA") or ft.startswith("BLOOD") or ft.startswith("SLIME") else 0))
    sides = []
    texids = {}                 # texture name -> index in this map's textures

    def tex(name):
        name = s8(name, 8)
        if not textures.texels or name in ("-", "") or textures.get(name) is None:
            return 255              # --texels 0: no textures, flat walls
        if name not in texids:
            if len(texids) >= 255:
                return 255
            texids[name] = len(texids)
        return texids[name]

    for i in range(len(L["SIDEDEFS"]) // 30):
        xo, yo, top, bot, mid, sec = struct.unpack_from("<hh8s8s8sh", L["SIDEDEFS"], 30 * i)
        sides.append((tone(ttones, s8(top, 8)), tone(ttones, s8(bot, 8)), tone(ttones, s8(mid, 8)),
                      tex(top), tex(bot), tex(mid), xo, yo, sec))
    lines = []
    for i in range(len(L["LINEDEFS"]) // 14):
        v1, v2, flags, special, tag, s0, s1 = struct.unpack_from("<hhhhhhh", L["LINEDEFS"], 14 * i)
        lines.append((v1, v2, flags & 0xffff, special, tag, s0, s1))
    segs = []
    for i in range(len(L["SEGS"]) // 12):
        v1, v2, ang, line, side, off = struct.unpack_from("<hhhhhh", L["SEGS"], 12 * i)
        ln = lines[line]
        lv1, lv2 = (ln[0], ln[1]) if side == 0 else (ln[1], ln[0])
        edge = (1 if v1 == lv1 else 0) | (2 if v2 == lv2 else 0)
        sd = ln[5] if side == 0 else ln[6]
        u = (off + (sides[sd][6] if 0 <= sd < len(sides) else 0)) % 4096
        length = int(round(math.hypot(verts[v2][0] - verts[v1][0], verts[v2][1] - verts[v1][1])))
        segs.append((v1, v2, line, side | edge << 1, u, length))
    ssecs = []
    for i in range(len(L["SSECTORS"]) // 4):
        n, first = struct.unpack_from("<hh", L["SSECTORS"], 4 * i)
        ln = lines[segs[first][2]]
        sd = ln[5] if segs[first][3] & 1 == 0 else ln[6]
        ssecs.append((n, first, sides[sd][8]))
    nodes = []
    for i in range(len(L["NODES"]) // 28):
        nodes.append(struct.unpack_from("<hhhhhhhhhhhhHH", L["NODES"], 28 * i))
    things = []
    for i in range(len(L["THINGS"]) // 10):
        x, y, ang, typ, fl = struct.unpack_from("<hhhhh", L["THINGS"], 10 * i)
        if fl & 16:            # multiplayer only
            continue
        if typ in (2, 3, 4, 11):   # other players' starts, deathmatch starts
            continue
        if typ not in class_of:
            continue
        things.append((x, y, ang, class_of[typ], fl))
    bmap = L["BLOCKMAP"]
    reject = L.get("REJECT", b"")

    # PACK FORMAT, one map:
    #   name (4 bytes), then 11 x (u16 count, u32 offset from the map start):
    #   vertexes, sectors, sides, lines, segs, subsectors, nodes, things,
    #   blockmap, reject, textures (see src/level.c)
    parts = []
    parts.append((len(verts), b"".join(struct.pack(">hh", *v) for v in verts)))
    parts.append((len(sectors), b"".join(struct.pack(">hhBBhBBBx", *s) for s in sectors)))
    parts.append((len(sides), b"".join(struct.pack(">BBBBBBhhh", *s) for s in sides)))
    parts.append((len(lines), b"".join(struct.pack(">hhHhhhh", *l) for l in lines)))
    parts.append((len(segs), b"".join(struct.pack(">hhhhhh", *s) for s in segs)))
    parts.append((len(ssecs), b"".join(struct.pack(">hhh", *s) for s in ssecs)))
    parts.append((len(nodes), b"".join(struct.pack(">hhhhhhhhhhhhHH", *n) for n in nodes)))
    parts.append((len(things), b"".join(struct.pack(">hhhhh", *t) for t in things)))
    bm = struct.unpack("<%dh" % (len(bmap) // 2), bmap)
    parts.append((len(bm), struct.pack(">%dh" % len(bm), *bm)))
    parts.append((len(reject), bytes(reject)))
    # textures: per texture u8 log2 width, u8 log2 height (in texels), u8
    # log2 of the pixels per texel, u8 0, u32 offset of its texels from the
    # start of this part; texels column by column. The part and each
    # texture's texels are at 64-byte boundaries, so every column (at most
    # 64 texels) is aligned to its own size: the game's column loop then
    # steps through it by setting the low address bits.
    tl = sorted(texids, key=texids.get)
    tdir, tdata = b"", b""
    tbase = -(-8 * len(tl) // 64) * 64
    for n in tl:
        lw, lh, sh, px = textures.get(n)
        while len(tdata) % 64:
            tdata += b"\0"
        tdir += struct.pack(">BBBBI", lw, lh, sh, 0, tbase + len(tdata))
        tdata += px
    parts.append((len(tl), tdir.ljust(tbase, b"\0") + tdata))
    head_size = 4 + 6 * len(parts)
    head = bytearray(name.encode("latin-1")[:4].ljust(4, b"\0"))
    body = bytearray()
    for k, (count, blob) in enumerate(parts):
        align = 64 if k == len(parts) - 1 else 4        # the textures
        while (head_size + len(body)) % align:
            body.append(0)
        head += struct.pack(">HI", count & 0xffff, head_size + len(body))
        body += blob
    out = bytes(head + body)
    return out, {"verts": len(verts), "lines": len(lines), "segs": len(segs),
                 "ssecs": len(ssecs), "sectors": len(sectors), "things": len(things),
                 "bytes": len(out)}


# ---------------------------------------------------------------------------

def all_maps(wad):
    out = []
    for name, _, _ in wad.lumps:
        if (len(name) == 4 and name[0] == "E" and name[2] == "M" and name[1].isdigit()
                and name[3].isdigit()):
            out.append(name)
        elif len(name) == 5 and name.startswith("MAP") and name[3:].isdigit():
            out.append(name)
    return out


def main():
    p = argparse.ArgumentParser(description="Convert Doom maps for Cybiko Doom")
    p.add_argument("wad")
    p.add_argument("out", nargs="?", help="folder for doom.dat and the map files")
    p.add_argument("--maps", help="comma-separated map names (default: all)")
    p.add_argument("--texels", type=int, default=32,
                   help="texels along a texture's longer side, at most (default 32; "
                   "0: no textures, the walls are flat shades)")
    p.add_argument("--list", action="store_true", help="list the maps in the WAD")
    a = p.parse_args()
    wad = Wad(a.wad)
    maps = all_maps(wad)
    if a.list or not a.out:
        print(" ".join(maps))
        return
    if a.maps:
        want = [m.strip().upper() for m in a.maps.split(",") if m.strip()]
    else:
        want = maps
    lum = palette_lum(wad)
    ttones = texture_tones(wad, lum)
    ftones = flat_tones(wad, lum)
    textures = Textures(wad, lum, a.texels)

    # sprites: slot = class * FRAMES_PER_CLASS + frame
    classes = {name: i for i, name in enumerate(CLASS_ORDER)}
    frames = {}
    for dn, (cls, spr) in THINGS.items():
        frames[cls] = spr
    frames.update(EXTRA)
    sil = []
    missing = []
    for cls in CLASS_ORDER:
        for k in range(FRAMES_PER_CLASS):
            names = frames.get(cls, [])
            s = shrink_sprite(wad, names[k], lum) if k < len(names) else None
            if k < len(names) and s is None:
                missing.append(names[k])
            sil.append(s or b"")
    class_of = {}
    for dn, (cls, spr) in THINGS.items():
        # a monster whose sprite this WAD lacks cannot be in its maps
        if spr and not sil[classes[cls] * FRAMES_PER_CLASS]:
            continue
        class_of[dn] = classes[cls]

    blobs = []
    for m in want:
        r = convert_map(wad, m, ttones, ftones, class_of, textures)
        if r is None:
            sys.exit("%s: no map %s" % (a.wad, m))
        blob, st = r
        blobs.append(blob)
        print("%s: %d vertexes, %d lines, %d segs, %d subsectors, %d sectors, %d things, %d bytes"
              % (m, st["verts"], st["lines"], st["segs"], st["ssecs"], st["sectors"],
                 st["things"], st["bytes"]))
    if missing:
        print("sprites not in this WAD (drawn as boxes): " + " ".join(sorted(set(missing))))

    # PACK FORMAT. doom.dat: "CYDM", u16 version (4), u16 map count, u16
    # sprite slots, u16 0, then the sprites' packed size, unpacked size and
    # room (u32 each), then per map its 4 name bytes and its unpacked size,
    # packed size and room (u32 each), then per sprite slot the u32 offset
    # of its sprite in the unpacked sprites (0xffffffff = none), then the
    # sprites packed. A map's file, doom_<name>.dat: its 4 name bytes, then
    # the map packed. "Packed" is raw Deflate; "room" is the buffer the game
    # unpacks it in, with the packed data at its end.
    os.makedirs(a.out, exist_ok=True)
    entries = b""
    total = 0
    for b in blobs:
        packed, room = deflate(b)
        entries += b[:4] + struct.pack(">III", len(b), len(packed), room)
        fn = os.path.join(a.out, map_file(b[:4]))
        open(fn, "wb").write(b[:4] + packed)
        total += 4 + len(packed)
        print("%s: %d bytes (%d unpacked)" % (fn, 4 + len(packed), len(b)))
    spr = bytearray()
    soffs = b""
    for s in sil:
        if not s:
            soffs += struct.pack(">I", 0xffffffff)
            continue
        while len(spr) % 2:
            spr.append(0)
        soffs += struct.pack(">I", len(spr))
        spr += s
    packed, room = deflate(bytes(spr))
    head = b"CYDM" + struct.pack(">HHHHIII", 4, len(blobs), len(sil), 0, len(packed), len(spr), room)
    pack = head + entries + soffs + packed
    fn = os.path.join(a.out, "doom.dat")
    open(fn, "wb").write(pack)
    total += len(pack)
    snd = sound_pack(wad)
    if snd:
        sfn = os.path.join(a.out, "doom_snd.dat")
        open(sfn, "wb").write(snd)
        total += len(snd)
        print("%s: %d bytes of PC speaker sounds" % (sfn, len(snd)))
    print("%s: %d bytes (sprites %d unpacked); %d maps, %d bytes in all"
          % (fn, len(pack), len(spr), len(blobs), total))


# PC speaker sounds, in the order of src/sound.h (SFX_PISTOL is 1)
SOUNDS = [
    "PISTOL", "SHOTGN", "PUNCH", "SAWFUL", "RLAUNC", "PLASMA", "BFG",
    "BAREXP", "FIRSHT", "FIRXPL",
    "DOROPN", "DORCLS", "BDOPN", "BDCLS", "PSTART", "PSTOP",
    "SWTCHN", "SWTCHX",
    "ITEMUP", "WPNUP", "GETPOW", "TELEPT", "NOWAY", "OOF",
    "PLPAIN", "PLDETH", "SLOP",
    "POSIT1", "BGSIT1", "SGTSIT", "CACSIT", "BRSSIT",
    "POPAIN", "DMPAIN",
    "PODTH1", "BGDTH1", "SGTDTH", "CACDTH", "BRSDTH", "SKLDTH",
    "SGTATK", "CLAW", "SKLATK",
]


def sound_pack(wad):
    """doom_snd.dat: "CYSN", u16 version (1), u16 sound count, u16 tone
    bytes, then per sound u16 offset and u16 length of its tones, then the
    tones: the DP* lumps' PC speaker tones, 140 a second, 0 silence, else
    an index in Doom's table of 127 pitches (src/sound.c). None if the WAD
    has no PC speaker sounds."""
    tones = bytearray()
    table = struct.pack(">HH", 0, 0)            # sound 0 is none
    found = 0
    for name in SOUNDS:
        d = wad.get("DP" + name)
        if not d or len(d) < 4:
            table += struct.pack(">HH", 0, 0)
            continue
        fmt, n = struct.unpack_from("<HH", d, 0)
        t = bytes(b if b < 128 else 0 for b in d[4:4 + n])
        if fmt != 0 or not t:
            table += struct.pack(">HH", 0, 0)
            continue
        table += struct.pack(">HH", len(tones), len(t))
        tones += t
        found += 1
    if not found:
        return None
    return b"CYSN" + struct.pack(">HHH", 1, len(SOUNDS) + 1, len(tones)) + table + bytes(tones)


def map_file(name):
    """doom_e1m1.dat (src/level.c makes the same name)."""
    return "doom_" + name.rstrip(b"\0").decode("latin-1").lower() + ".dat"


def deflate(data):
    """(raw Deflate data, room): room is the size of a buffer in which the
    game can unpack it in place, the packed data at its end. The decoder
    writes byte w only after reading the input that zlib needs for it, so
    the packed data must start at least max(out(k) - k) bytes in, where
    out(k) is what zlib gives from its first k bytes."""
    c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    packed = c.compress(data) + c.flush()
    d = zlib.decompressobj(-15)
    out = ahead = 0
    for k in range(len(packed)):
        out += len(d.decompress(packed[k:k + 1]))
        ahead = max(ahead, out - (k + 1))
    out += len(d.flush())
    assert out == len(data)
    room = max(len(data), len(packed) + ahead + 8)
    return packed, (room + 3) & ~3

if __name__ == "__main__":
    main()
