#!/usr/bin/env python3
"""Turn the maps of a Doom IWAD into the level pack Cybiko Doom is built with.

    wad2cy.py doom1.wad levels.s [--maps E1M1,E1M2] [--list]

No WAD data is kept in this repository. You give the converter your own WAD
(the shareware doom1.wad, the registered doom.wad or Ultimate Doom, or
Freedoom's freedoom1.wad) and it writes an assembler file with everything
the game needs from it:

  * the map geometry: vertexes, sectors, sides, lines, segs, subsectors,
    BSP nodes, things, the blockmap and the reject table;
  * one grey tone for every wall texture and floor/ceiling flat, the
    average brightness of its pixels, since the Cybiko draws flat walls;
  * small 1-bit silhouettes of the monster, item and weapon sprites, which
    the game draws as outlines ("wireframe billboards").

The pack is big-endian (as the H8S is) and starts with a directory, see
PACK FORMAT below and src/level.h, which must agree with it.

Python 3, no extra packages.
"""
import argparse
import struct
import sys

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
# Sprite silhouettes
#
# A silhouette is the sprite's opaque pixels shrunk by a whole factor f so it
# is at most 32 cells tall, stored as runs per column. Format (bytes):
#   u8 width (cells), u8 height (cells), u8 f (map units per cell),
#   s8 left offset (cells), s8 top offset (cells), u8 0
#   then per column: u8 run count, then (u8 start, u8 end) per run,
#   end exclusive.

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


def silhouette(wad, name):
    data = sprite_lump(wad, name)
    if data is None:
        return None
    w, h, lo, to, cols = picture_pixels(data)
    f = max(1, -(-h // 32), -(-w // 32))
    cw, ch = -(-w // f), -(-h // f)
    grid = [[0] * ch for _ in range(cw)]
    for x, col in enumerate(cols):
        for y, _ in col:
            if 0 <= y < h:
                grid[x // f][y // f] += 1
    # a cell is solid when at least a third of its pixels are
    need = max(1, (f * f) // 3)
    out = bytearray([cw, ch, f, max(-128, min(127, round(lo / f))) & 0xff,
                     max(-128, min(127, round(to / f))) & 0xff, 0])
    for x in range(cw):
        runs = []
        y = 0
        while y < ch:
            if grid[x][y] >= need:
                s = y
                while y < ch and grid[x][y] >= need:
                    y += 1
                runs.append((s, y))
            else:
                y += 1
        runs = runs[:8]
        out.append(len(runs))
        for s, e in runs:
            out += bytes([s, e])
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


def convert_map(wad, name, ttones, ftones, class_of):
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
    for i in range(len(L["SIDEDEFS"]) // 30):
        xo, yo, top, bot, mid, sec = struct.unpack_from("<hh8s8s8sh", L["SIDEDEFS"], 30 * i)
        sides.append((tone(ttones, s8(top, 8)), tone(ttones, s8(bot, 8)), tone(ttones, s8(mid, 8)), sec))
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
        segs.append((v1, v2, line, side | edge << 1))
    ssecs = []
    for i in range(len(L["SSECTORS"]) // 4):
        n, first = struct.unpack_from("<hh", L["SSECTORS"], 4 * i)
        ln = lines[segs[first][2]]
        sd = ln[5] if segs[first][3] & 1 == 0 else ln[6]
        ssecs.append((n, first, sides[sd][3]))
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
    #   header: 12 x (u16 count, u32 offset from the map start)  (see level.h)
    parts = []
    parts.append((len(verts), b"".join(struct.pack(">hh", *v) for v in verts)))
    parts.append((len(sectors), b"".join(struct.pack(">hhBBhBBBx", *s) for s in sectors)))
    parts.append((len(sides), b"".join(struct.pack(">BBBxh", *s) for s in sides)))
    parts.append((len(lines), b"".join(struct.pack(">hhHhhhh", *l) for l in lines)))
    parts.append((len(segs), b"".join(struct.pack(">hhhh", *s) for s in segs)))
    parts.append((len(ssecs), b"".join(struct.pack(">hhh", *s) for s in ssecs)))
    parts.append((len(nodes), b"".join(struct.pack(">hhhhhhhhhhhhHH", *n) for n in nodes)))
    parts.append((len(things), b"".join(struct.pack(">hhhhh", *t) for t in things)))
    bm = struct.unpack("<%dh" % (len(bmap) // 2), bmap)
    parts.append((len(bm), struct.pack(">%dh" % len(bm), *bm)))
    parts.append((len(reject), bytes(reject)))
    head_size = 4 + 6 * len(parts)
    head = bytearray(name.encode("latin-1")[:4].ljust(4, b"\0"))
    body = bytearray()
    for count, blob in parts:
        while len(body) % 4:
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
    p.add_argument("out", nargs="?")
    p.add_argument("--maps", help="comma-separated map names (default: the first three of episode 1)")
    p.add_argument("--list", action="store_true", help="list the maps in the WAD")
    p.add_argument("--bin", action="store_true", help="write the pack as raw bytes (for pc/)")
    a = p.parse_args()
    wad = Wad(a.wad)
    maps = all_maps(wad)
    if a.list or not a.out:
        print(" ".join(maps))
        return
    if a.maps:
        want = [m.strip().upper() for m in a.maps.split(",") if m.strip()]
    else:
        want = [m for m in maps if m.startswith("E1M") or m.startswith("MAP0")][:3]
    lum = palette_lum(wad)
    ttones = texture_tones(wad, lum)
    ftones = flat_tones(wad, lum)

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
            s = silhouette(wad, names[k]) if k < len(names) else None
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
        r = convert_map(wad, m, ttones, ftones, class_of)
        if r is None:
            sys.exit("%s: no map %s" % (a.wad, m))
        blob, st = r
        blobs.append(blob)
        print("%s: %d vertexes, %d lines, %d segs, %d subsectors, %d sectors, %d things, %d bytes"
              % (m, st["verts"], st["lines"], st["segs"], st["ssecs"], st["sectors"],
                 st["things"], st["bytes"]))
    if missing:
        print("sprites not in this WAD (drawn as boxes): " + " ".join(sorted(set(missing))))

    # PACK FORMAT: "CYDM", u16 version, u16 map count, u16 sprite slots,
    # u16 0, u32 offset of each map, u32 offset of each sprite (0 = none),
    # then the data. Offsets are from the pack start.
    head = bytearray(b"CYDM" + struct.pack(">HHHH", 1, len(blobs), len(sil), 0))
    table_size = len(head) + 4 * (len(blobs) + len(sil))
    body = bytearray()
    offs = []
    for b in blobs:
        while len(body) % 4:
            body.append(0)
        offs.append(table_size + len(body))
        body += b
    for s in sil:
        if not s:
            offs.append(0)
            continue
        offs.append(table_size + len(body))
        body += s
    for o in offs:
        head += struct.pack(">I", o)
    pack = bytes(head + body)
    if a.bin:
        open(a.out, "wb").write(pack)
        print("%s: %d bytes, %d maps" % (a.out, len(pack), len(blobs)))
        return
    with open(a.out, "w") as f:
        f.write("; Generated by tools/wad2cy.py from %s. Do not commit: it holds\n"
                "; data converted from the WAD.\n" % a.wad.split("/")[-1])
        f.write("\t.h8300s\n\t.section .text\n\t.align 2\n\t.global _levelpack\n_levelpack:\n")
        for i in range(0, len(pack), 32):
            f.write("\t.byte " + ",".join(str(c) for c in pack[i:i + 32]) + "\n")
        f.write("\t.align 2\n")
    print("%s: %d bytes, %d maps" % (a.out, len(pack), len(blobs)))


if __name__ == "__main__":
    main()
