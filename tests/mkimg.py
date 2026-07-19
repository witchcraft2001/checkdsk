#!/usr/bin/env python3
"""FAT12/16/32 test-image generator, corruptor and verifier for the
chkdsk host harness.

  mkimg.py build  <img> <fat12|fat16|fat32>
  mkimg.py corrupt <img> <scenario> [scenario...]
  mkimg.py verify <img> [--allow-orphans]

`build` creates a small volume populated with files and nested dirs
whose content is a deterministic pattern. `corrupt` mutates the FAT /
dirents to inject a specific defect. `verify` re-checks filesystem
invariants from scratch (independent of the chkdsk code under test):

  V1  FAT copies identical
  V2  every FAT value is free / BAD / EOC / a valid cluster index
  V3  every dirent chain is linear, EOC-terminated, unshared
  V4  file size fits its chain (ceil(size/cl) == chain length)
  V5  no orphan clusters (allocated but unreferenced)
  V6  file content pattern intact (for the generator's own files)

Exit code: 0 = all invariants hold, 1 = violations (printed).
"""
import struct
import sys

SEC = 512


def ceil_div(a, b):
    return (a + b - 1) // b


class Geo:
    """Volume geometry, derived the same way mount.c derives it."""

    def __init__(self, img):
        b = img
        self.bps = struct.unpack_from("<H", b, 11)[0]
        self.spc = b[13]
        self.rsvd = struct.unpack_from("<H", b, 14)[0]
        self.nfats = b[16]
        self.nroot = struct.unpack_from("<H", b, 17)[0]
        totsec16 = struct.unpack_from("<H", b, 19)[0]
        fatsz16 = struct.unpack_from("<H", b, 22)[0]
        totsec32 = struct.unpack_from("<I", b, 32)[0]
        self.totsec = totsec16 or totsec32
        self.fatsz = fatsz16 or struct.unpack_from("<I", b, 36)[0]
        self.fatbase = self.rsvd
        root_secs = ceil_div(self.nroot * 32, SEC)
        self.dirbase = self.fatbase + self.nfats * self.fatsz  # fat12/16 root sector
        self.database = self.dirbase + root_secs
        data_secs = self.totsec - self.database
        self.nclust = data_secs // self.spc
        self.nfatent = self.nclust + 2
        if self.nclust < 4085:
            self.type = 12
            self.eoc_min, self.bad, self.eoc = 0xFF8, 0xFF7, 0xFFF
        elif self.nclust < 65525:
            self.type = 16
            self.eoc_min, self.bad, self.eoc = 0xFFF8, 0xFFF7, 0xFFFF
        else:
            self.type = 32
            self.eoc_min, self.bad, self.eoc = 0x0FFFFFF8, 0x0FFFFFF7, 0x0FFFFFFF
            self.rootclus = struct.unpack_from("<I", b, 44)[0]

    def clus_lba(self, c):
        return self.database + (c - 2) * self.spc

    def clus_bytes(self):
        return self.spc * SEC


def get_fat(img, g, c, copy=0):
    base = (g.fatbase + copy * g.fatsz) * SEC
    if g.type == 12:
        off = base + c + c // 2
        v = img[off] | (img[off + 1] << 8)
        return (v >> 4) if (c & 1) else (v & 0xFFF)
    if g.type == 16:
        return struct.unpack_from("<H", img, base + c * 2)[0]
    return struct.unpack_from("<I", img, base + c * 4)[0] & 0x0FFFFFFF


def set_fat(img, g, c, val, copies=(0, 1)):
    for copy in copies:
        if copy >= g.nfats:
            continue
        base = (g.fatbase + copy * g.fatsz) * SEC
        if g.type == 12:
            off = base + c + c // 2
            if c & 1:
                img[off] = (img[off] & 0x0F) | ((val & 0xF) << 4)
                img[off + 1] = (val >> 4) & 0xFF
            else:
                img[off] = val & 0xFF
                img[off + 1] = (img[off + 1] & 0xF0) | ((val >> 8) & 0x0F)
        elif g.type == 16:
            struct.pack_into("<H", img, base + c * 2, val & 0xFFFF)
        else:
            old = struct.unpack_from("<I", img, base + c * 4)[0]
            struct.pack_into("<I", img, base + c * 4, (old & 0xF0000000) | (val & 0x0FFFFFFF))


def find_free(img, g, n):
    out = []
    c = 2
    while len(out) < n and c < g.nfatent:
        if get_fat(img, g, c) == 0:
            out.append(c)
        c += 1
    if len(out) < n:
        raise SystemExit("image full")
    return out


def alloc_chain(img, g, n):
    chain = find_free(img, g, n)
    for a, b in zip(chain, chain[1:]):
        set_fat(img, g, a, b)
    set_fat(img, g, chain[-1], g.eoc)
    return chain


def content_for(name83, size):
    """Deterministic pattern; repeats '<NAME>#<i> ' blocks."""
    tag = name83.replace(" ", "")
    out = bytearray()
    i = 0
    while len(out) < size:
        out += ("%s#%d " % (tag, i)).encode()
        i += 1
    return bytes(out[:size])


def write_chain_data(img, g, chain, data):
    cb = g.clus_bytes()
    for i, c in enumerate(chain):
        part = data[i * cb:(i + 1) * cb]
        off = g.clus_lba(c) * SEC
        img[off:off + len(part)] = part


def dirent(name83, attr, clus, size):
    e = bytearray(32)
    e[0:11] = name83.encode()
    e[11] = attr
    struct.pack_into("<H", e, 20, (clus >> 16) & 0xFFFF)
    struct.pack_into("<H", e, 22, 0x6000)          # 12:00:00
    struct.pack_into("<H", e, 24, (46 << 9) | (7 << 5) | 17)  # 2026-07-17
    struct.pack_into("<H", e, 26, clus & 0xFFFF)
    struct.pack_into("<I", e, 28, size)
    return bytes(e)


def dir_slots(img, g, dirclus):
    """Yield (byte_offset) of each 32-byte slot of a directory.
    dirclus == 0 means the FAT12/16 root region."""
    if dirclus == 0 and g.type != 32:
        base = g.dirbase * SEC
        for i in range(g.nroot):
            yield base + i * 32
        return
    c = dirclus if dirclus else g.rootclus
    while True:
        base = g.clus_lba(c) * SEC
        for i in range(g.clus_bytes() // 32):
            yield base + i * 32
        v = get_fat(img, g, c)
        if not (2 <= v < g.nfatent):
            return
        c = v


def add_entry(img, g, dirclus, ent):
    for off in dir_slots(img, g, dirclus):
        if img[off] in (0x00, 0xE5):
            img[off:off + 32] = ent
            return off
    raise SystemExit("directory full")


def lfn_checksum(name83):
    """Standard FAT LFN checksum over the 11 raw SFN bytes."""
    s = 0
    for ch in name83.encode("latin-1"):
        s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
    return s


# UCS-2 char slots within a 32-byte LFN entry: 5 + 6 + 2 = 13 chars.
_LFN_CHAR_OFF = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)


def lfn_slots(longname, name83, chk=None):
    """Build a well-formed LFN group, returned in on-disk order (the
    physically-first slot carries the highest order number plus 0x40)."""
    if chk is None:
        chk = lfn_checksum(name83)
    if not longname or len(longname) > 255:
        raise ValueError("LFN length must be 1..255")
    units = [ord(c) for c in longname]
    # An exact multiple of 13 occupies full slots with no terminator or
    # padding. Only a partial final slot gets NUL + 0xFFFF padding.
    if len(units) % 13:
        units.append(0x0000)
        while len(units) % 13:
            units.append(0xFFFF)
    n = len(units) // 13
    out = []
    for i in range(n):
        part = units[i * 13:(i + 1) * 13]
        e = bytearray(32)
        e[0] = (i + 1) | (0x40 if i == n - 1 else 0)
        e[11] = 0x0F
        e[13] = chk
        for k, p in enumerate(_LFN_CHAR_OFF):
            e[p] = part[k] & 0xFF
            e[p + 1] = (part[k] >> 8) & 0xFF
        out.append(bytes(e))
    out.reverse()
    return out


def add_lfn_file(img, g, dirclus, longname, name83, size, chk=None, bad_order=False):
    """Add a file preceded by a real LFN group. Slots must be written
    before the SFN and stay contiguous -- add_entry fills the first free
    slot each time, and a real LFN slot's byte 0 is an order number
    (never 0x00/0xE5), so it is correctly seen as occupied."""
    n = max(1, ceil_div(size, g.clus_bytes()))
    chain = alloc_chain(img, g, n)
    write_chain_data(img, g, chain, content_for(name83, size))
    slots = lfn_slots(longname, name83, chk)
    if bad_order and len(slots) > 1:
        s = bytearray(slots[1])
        s[0] = 0x09                     # wrong sequence number
        slots[1] = bytes(s)
    slot_offs = [add_entry(img, g, dirclus, s) for s in slots]
    sfn_off = add_entry(img, g, dirclus, dirent(name83, 0x20, chain[0], size))
    return chain, slot_offs, sfn_off


def extend_dir_chain(img, g, dirclus):
    """Append one zero-filled cluster to a chain directory (needed to
    put a FAT32-root test group across a sector/cluster boundary)."""
    c = dirclus if dirclus else g.rootclus
    while True:
        v = get_fat(img, g, c)
        if not (2 <= v < g.nfatent):
            break
        c = v
    new = alloc_chain(img, g, 1)[0]
    set_fat(img, g, c, new)
    return new


def pad_dir_to_slot_mod(img, g, dirclus, target):
    """Add harmless empty SFNs until the next free slot is target mod 16."""
    seq = 0
    while True:
        for idx, off in enumerate(dir_slots(img, g, dirclus)):
            if img[off] in (0x00, 0xE5):
                break
        else:
            raise SystemExit("directory full while padding")
        if idx % 16 == target:
            return
        name = ("PAD%05d" % seq) + "TMP"
        add_entry(img, g, dirclus, dirent(name, 0x20, 0, 0))
        seq += 1


def add_file(img, g, dirclus, name83, size):
    n = max(1, ceil_div(size, g.clus_bytes()))
    chain = alloc_chain(img, g, n)
    write_chain_data(img, g, chain, content_for(name83, size))
    add_entry(img, g, dirclus, dirent(name83, 0x20, chain[0], size))
    return chain


def add_dir(img, g, parentclus, name83):
    chain = alloc_chain(img, g, 1)
    c = chain[0]
    off = g.clus_lba(c) * SEC
    img[off:off + g.clus_bytes()] = b"\x00" * g.clus_bytes()
    img[off:off + 32] = dirent(".          ", 0x10, c, 0)
    img[off + 32:off + 64] = dirent("..         ", 0x10, parentclus, 0)
    add_entry(img, g, parentclus, dirent(name83, 0x10, c, 0))
    return c


# ---------------------------------------------------------------- build

def build(path, fstype):
    if fstype == "fat12":
        nclust, spc, nroot, rsvd = 1000, 1, 224, 1
    elif fstype == "fat16":
        nclust, spc, nroot, rsvd = 4600, 1, 512, 1
    elif fstype == "fat32":
        nclust, spc, nroot, rsvd = 65600, 1, 0, 32
    else:
        raise SystemExit("bad fs type")

    nfats = 2
    nfatent = nclust + 2
    if fstype == "fat12":
        fatsz = ceil_div(nfatent * 3 // 2 + 1, SEC)
    elif fstype == "fat16":
        fatsz = ceil_div(nfatent * 2, SEC)
    else:
        fatsz = ceil_div(nfatent * 4, SEC)
    root_secs = ceil_div(nroot * 32, SEC)
    total = rsvd + nfats * fatsz + root_secs + nclust * spc

    img = bytearray(total * SEC)

    # --- boot sector ---
    img[0:3] = b"\xEB\x3C\x90"
    img[3:11] = b"CHKDSKTS"
    struct.pack_into("<H", img, 11, SEC)
    img[13] = spc
    struct.pack_into("<H", img, 14, rsvd)
    img[16] = nfats
    struct.pack_into("<H", img, 17, nroot)
    if total < 0x10000:
        struct.pack_into("<H", img, 19, total)
    else:
        struct.pack_into("<I", img, 32, total)
    img[21] = 0xF8
    if fstype != "fat32":
        struct.pack_into("<H", img, 22, fatsz)
        img[38] = 0x29
        struct.pack_into("<I", img, 39, 0x20260717)
        img[43:54] = b"CHKTEST    "
        img[54:62] = ("FAT%s   " % fstype[3:]).encode()[:8]
    else:
        struct.pack_into("<I", img, 36, fatsz)
        struct.pack_into("<I", img, 44, 2)   # root cluster
        struct.pack_into("<H", img, 48, 1)   # FSInfo sector
        struct.pack_into("<H", img, 50, 6)   # backup boot
        img[66] = 0x29
        struct.pack_into("<I", img, 67, 0x20260717)
        img[71:82] = b"CHKTEST    "
        img[82:90] = b"FAT32   "
    img[510], img[511] = 0x55, 0xAA

    if fstype == "fat32":
        fsi = bytearray(SEC)
        struct.pack_into("<I", fsi, 0, 0x41615252)
        struct.pack_into("<I", fsi, 484, 0x61417272)
        struct.pack_into("<I", fsi, 488, 0xFFFFFFFF)
        struct.pack_into("<I", fsi, 492, 0xFFFFFFFF)
        fsi[510], fsi[511] = 0x55, 0xAA
        img[SEC:2 * SEC] = fsi
        # backup boot region at sector 6 (boot copy + FSInfo copy)
        img[6 * SEC:7 * SEC] = img[0:SEC]
        img[7 * SEC:8 * SEC] = fsi

    g = Geo(img)

    # FAT[0]/FAT[1]
    set_fat(img, g, 0, (g.eoc & ~0xFF) | 0xF8)
    set_fat(img, g, 1, g.eoc)
    if fstype == "fat32":
        set_fat(img, g, 2, g.eoc)   # root dir cluster

    # --- content: a few files + nested dirs ---
    cb = g.clus_bytes()
    add_file(img, g, 0, "FILE1   TXT", cb * 2 + cb // 2)
    add_file(img, g, 0, "FILE2   TXT", cb // 3)
    add_file(img, g, 0, "README  TXT", cb + 7)
    # A well-formed LFN group in the base image: every scenario then
    # doubles as a false-positive guard for the sequence/checksum
    # cross-check -- a valid long name must never be reported.
    add_lfn_file(img, g, 0, "Long Name File.txt", "LONGNA~1TXT", cb // 2)
    # Exact 13-char LFN: one full slot, with no NUL/0xFFFF padding.
    add_lfn_file(img, g, 0, "ABCDEFGHIJKLM", "EXACT13 TXT", cb // 4)
    # A future LFN-attribute dirent type is not a type-0 name component.
    # Maintenance utilities must preserve/ignore it, not delete it.
    future = bytearray(lfn_slots("Future Slot", "FUTURE~1TMP")[0])
    future[12] = 1
    future[26], future[27] = 0x34, 0x12
    add_entry(img, g, 0, bytes(future))
    sub1 = add_dir(img, g, 0, "SUB1       ")
    add_file(img, g, sub1, "NOTES   TXT", cb + cb // 2)
    sub2 = add_dir(img, g, sub1, "SUB2       ")
    add_file(img, g, sub2, "DEEP    TXT", cb // 2)

    with open(path, "wb") as f:
        f.write(img)
    print("built %s: %s, %d clusters, %d sectors" % (path, fstype, nclust, total))


# ------------------------------------------------------------- corrupt

def load(path):
    img = bytearray(open(path, "rb").read())
    return img, Geo(img)


def save(path, img):
    with open(path, "wb") as f:
        f.write(img)


def find_file_chain(img, g, name83):
    """Locate a file by walking from root (generator names only)."""
    stack = [0]
    seen = set()
    while stack:
        d = stack.pop()
        if d in seen:
            continue
        seen.add(d)
        for off in dir_slots(img, g, d):
            e = img[off:off + 32]
            if e[0] == 0x00:
                break
            if e[0] == 0xE5 or e[11] == 0x0F:
                continue
            clus = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
            nm = e[0:11].decode("ascii", "replace")
            if nm == name83:
                chain = []
                c = clus
                while 2 <= c < g.nfatent and c not in chain:
                    chain.append(c)
                    v = get_fat(img, g, c)
                    if v >= g.eoc_min:
                        break
                    c = v
                return off, chain
            if e[11] & 0x10 and nm[0] != ".":
                stack.append(clus)
    raise SystemExit("file %r not found" % name83)


def corrupt(img, g, scenario):
    if scenario == "orphan":
        # linear 5-cluster orphan chain, properly EOC'd, plus content
        chain = alloc_chain(img, g, 5)
        write_chain_data(img, g, chain, content_for("ORPHAN", g.clus_bytes() * 5))
    elif scenario == "selfloop":
        c = find_free(img, g, 1)[0]
        set_fat(img, g, c, c)
    elif scenario == "cycle2":
        a, b = find_free(img, g, 2)
        set_fat(img, g, a, b)
        set_fat(img, g, b, a)
    elif scenario == "cycle3tail":
        # tail leads into a 3-cycle: t -> a -> b -> c -> a
        t, a, b, c = find_free(img, g, 4)
        set_fat(img, g, t, a)
        set_fat(img, g, a, b)
        set_fat(img, g, b, c)
        set_fat(img, g, c, a)
    elif scenario == "garb":
        # orphan chain of 3 whose tail value is out of range, plus a
        # standalone entry holding the reserved value 1 (user-disk case)
        a, b, c, d = find_free(img, g, 4)
        set_fat(img, g, a, b)
        set_fat(img, g, b, c)
        set_fat(img, g, c, g.nfatent + 100)
        set_fat(img, g, d, 1)
    elif scenario == "desync":
        # FAT2 loses part of FILE1's chain (like an interrupted write)
        _, chain = find_file_chain(img, g, "FILE1   TXT")
        for c in chain[1:]:
            set_fat(img, g, c, 0, copies=(1,))
    elif scenario == "broken":
        # FILE1's mid-chain link is garbage -> phase 3 BROKEN
        _, chain = find_file_chain(img, g, "FILE1   TXT")
        set_fat(img, g, chain[1], g.nfatent + 77)
    elif scenario == "excess":
        # FILE1's chain gains 2 clusters beyond its size -> EXCESS
        _, chain = find_file_chain(img, g, "FILE1   TXT")
        extra = alloc_chain(img, g, 2)
        set_fat(img, g, chain[-1], extra[0])
    elif scenario == "cross":
        # README's tail links into FILE1's chain -> CROSS
        _, c1 = find_file_chain(img, g, "FILE1   TXT")
        _, c2 = find_file_chain(img, g, "README  TXT")
        set_fat(img, g, c2[-1], c1[1])
    elif scenario == "chkmess":
        # aftermath of the old buggy /F /C on the user's disk: LOSTCHN
        # holding CHK entries whose chains are cyclic, shared and
        # unterminated. A fixed chkdsk must converge this to clean.
        lostchn = add_dir(img, g, 0, "LOSTCHN    ")
        a, b = find_free(img, g, 2)
        set_fat(img, g, a, b)
        set_fat(img, g, b, a)                      # 2-cycle
        c = find_free(img, g, 1)[0]
        set_fat(img, g, c, c)                      # self-loop
        cb = g.clus_bytes()
        add_entry(img, g, lostchn, dirent("FILE0001CHK", 0x20, a, cb))
        add_entry(img, g, lostchn, dirent("FILE0002CHK", 0x20, b, cb))  # head shared with 0001
        add_entry(img, g, lostchn, dirent("FILE0003CHK", 0x20, c, cb))
    elif scenario == "userdisk":
        # the C: disk pattern: many self-loops + reserved values + desync
        for c in find_free(img, g, 40)[::2]:
            set_fat(img, g, c, c)
        for c in find_free(img, g, 10):
            set_fat(img, g, c, 1)
        _, chain = find_file_chain(img, g, "NOTES   TXT")
        set_fat(img, g, chain[0], 0, copies=(1,))
    elif scenario == "badname":
        # FILE1's SFN gets one forbidden byte -> DE_NAME_BAD_CHAR. No
        # preceding LFN, no collision -- must sanitize cleanly under /F.
        off, _ = find_file_chain(img, g, "FILE1   TXT")
        img[off + 4] = ord('*')   # "FILE1   TXT" -> "FILE*   TXT"
    elif scenario == "badname_lfn":
        # A VALID 2-slot LFN group whose checksum matches its (lowercase,
        # therefore flagged) SFN. Folding the SFN to upper case
        # invalidates that checksum, so the name-fix must RESTAMP the
        # group rather than delete it -- the long name has to survive.
        #
        # The previous fixture here used zero-filled dummy slots, which
        # never actually reached the image: add_entry treats byte 0 ==
        # 0x00 as a free slot, so each dummy was overwritten by the next
        # entry and the SFN ended up with no LFN in front of it at all.
        add_lfn_file(img, g, 0, "Lower Case Name.txt", "lower~1 txt",
                     g.clus_bytes() // 2)
    elif scenario == "lfn_badsum":
        # Well-formed sequence, wrong checksum byte -> the group cannot
        # belong to the SFN behind it. Detected, and dropped under /F.
        add_lfn_file(img, g, 0, "Broken Checksum.txt", "BROKEN~1TXT",
                     g.clus_bytes() // 2,
                     chk=lfn_checksum("BROKEN~1TXT") ^ 0xFF)
    elif scenario == "lfn_badord":
        # Correct checksum, scrambled order byte on the second slot.
        add_lfn_file(img, g, 0, "Broken Order Name.txt", "BRKORD~1TXT",
                     g.clus_bytes() // 2, bad_order=True)
    elif scenario == "lfn_badordbit":
        _, slots, _ = add_lfn_file(img, g, 0, "Reserved Order Bit.txt",
                                   "ORDBIT~1TXT", g.clus_bytes() // 2)
        img[slots[0]] |= 0x80
    elif scenario == "lfn_earlynul":
        _, slots, _ = add_lfn_file(img, g, 0, "Early Null Damage.txt",
                                   "EARLYN~1TXT", g.clus_bytes() // 2)
        # Physical last slot is ordinal 1 (not LAST): it must contain 13
        # real characters, never an early terminator.
        img[slots[-1] + 1:slots[-1] + 3] = b"\x00\x00"
    elif scenario == "lfn_badpad":
        _, slots, _ = add_lfn_file(img, g, 0, "Padding Damage.txt",
                                   "BADPAD~1TXT", g.clus_bytes() // 2)
        # Highest/LAST slot contains a short tail, NUL, then 0xFFFF. Put
        # a real 'A' into the last padding code unit.
        img[slots[0] + 30:slots[0] + 32] = b"A\x00"
    elif scenario == "lfn_orphan":
        for slot in lfn_slots("Orphan Long Name.txt", "ORPHAN~1TXT"):
            add_entry(img, g, 0, slot)
    elif scenario == "lfn_overlong":
        # 21 full slots encode 273 characters, beyond FAT's 255-char /
        # 20-slot maximum. Ensure FAT32 root has enough chain capacity.
        if g.type == 32:
            extend_dir_chain(img, g, 0)
            extend_dir_chain(img, g, 0)
        name83 = "TOOLONG TXT"
        chk = lfn_checksum(name83)
        for ordno in range(21, 0, -1):
            e = bytearray(32)
            e[0] = ordno | (0x40 if ordno == 21 else 0)
            e[11] = 0x0F
            e[13] = chk
            for p in _LFN_CHAR_OFF:
                e[p], e[p + 1] = ord("A"), 0
            add_entry(img, g, 0, bytes(e))
        chain = alloc_chain(img, g, 1)
        add_entry(img, g, 0,
                  dirent(name83, 0x20, chain[0], g.clus_bytes() // 2))
    elif scenario in ("badname_lfn_cross", "lfn_badsum_cross"):
        # Place two LFN slots at sector offsets 14/15 and the SFN at
        # offset 0 of the next sector. FAT32's one-sector root cluster
        # needs an explicit chain extension first.
        if g.type == 32:
            extend_dir_chain(img, g, 0)
        pad_dir_to_slot_mod(img, g, 0, 14)
        if scenario == "badname_lfn_cross":
            add_lfn_file(img, g, 0, "Cross Sector Lower.txt", "cross~1 txt",
                         g.clus_bytes() // 2)
        else:
            name83 = "CRSUM~1 TXT"
            add_lfn_file(img, g, 0, "Cross Sector Checksum.txt", name83,
                         g.clus_bytes() // 2, chk=lfn_checksum(name83) ^ 0xFF)
    elif scenario == "badname_cyr":
        # README's extension byte 0 becomes CP866 lowercase Cyrillic
        # 'a' (0xA0, DOS_Proc.asm UPPER range 0xA0-0xAF) -- must fold
        # to uppercase (0x80) under /F like Estex-DSS's own UPPER
        # routine, not get replaced with '_' like a real forbidden
        # character. README, not FILE2/DEEP -- verify()'s V6 content
        # check anchors on the latter two by their unchanged literal
        # name and would false-fail once this byte folds.
        off, _ = find_file_chain(img, g, "README  TXT")
        img[off + 8] = 0xA0
    elif scenario == "fakedir":
        # FILE1.TXT keeps its data and size but gains ATTR_DIR, the
        # shape seen on a real FAT16 volume (three such entries under
        # XCOPY*/ZCOPY). It has no "." / ".." so has_dot == 0 and /F
        # marks it deleted -- after which the end-of-run report must no
        # longer count it among the directories. Its chain stays claimed
        # in this run's bitmap, so Phase 4 only reports it as lost on a
        # SECOND pass; that is expected, not a defect.
        off, _ = find_file_chain(img, g, "FILE1   TXT")
        img[off + 11] |= 0x10
    elif scenario == "badtime":
        # specs.md's "2099-13-32 25:61:62". Day 32 does not fit the 5-bit
        # day field at all, so the encodable equivalent is day 31 with the
        # out-of-range month 13. A VALID creation timestamp is planted
        # alongside the corrupt write timestamp: the repair resets only
        # the offending fields, so this pair has to come back untouched.
        off, _ = find_file_chain(img, g, "FILE1   TXT")
        struct.pack_into("<H", img, off + 14, (12 << 11) | (30 << 5) | 15)
        struct.pack_into("<H", img, off + 16, (40 << 9) | (6 << 5) | 15)
        struct.pack_into("<H", img, off + 22, (25 << 11) | (61 << 5) | 31)
        struct.pack_into("<H", img, off + 24, (119 << 9) | (13 << 5) | 31)
    elif scenario == "badtime_leap":
        # Feb 29 is corrupt in 2021 and correct in 2020. Both are planted
        # so a run must flag exactly one -- the guard against a lazy
        # "day <= 31" check that would either miss the first or destroy
        # the second.
        off, _ = find_file_chain(img, g, "FILE1   TXT")
        struct.pack_into("<H", img, off + 24, (41 << 9) | (2 << 5) | 29)
        off, _ = find_file_chain(img, g, "README  TXT")
        struct.pack_into("<H", img, off + 24, (40 << 9) | (2 << 5) | 29)
    elif scenario == "fsinfo":
        # FAT32 only: FSInfo caches a free_count that contradicts the FAT.
        # Deliberately not 0xFFFFFFFF -- that value means "unknown" and is
        # a legitimate state that must NOT be reported.
        if g.type != 32:
            raise SystemExit("scenario 'fsinfo' is FAT32-only")
        struct.pack_into("<I", img, SEC + 488, 1)
    elif scenario == "badname_collide":
        # A lowercase twin of the existing FILE2 entry: sanitizing it
        # (uppercase) would exactly collide with FILE2's own name. The
        # name-collision guard must skip the rename -- report only,
        # both entries left exactly as they are.
        add_file(img, g, 0, "file2   TXT", g.clus_bytes() // 4)
    else:
        raise SystemExit("unknown scenario %r" % scenario)
    print("corrupted: %s" % scenario)


def has_live_entry(img, g, dirclus, name83):
    for off in dir_slots(img, g, dirclus):
        e = img[off:off + 32]
        if e[0] == 0x00:
            break
        if e[0] == 0xE5 or e[11] == 0x0F:
            continue
        if bytes(e[0:11]) == name83.encode():
            return True
    return False


def lfn_group_before(img, g, dirclus, name83):
    run = []
    for off in dir_slots(img, g, dirclus):
        e = img[off:off + 32]
        if e[0] == 0x00:
            break
        if e[0] == 0xE5:
            run = []
            continue
        if (e[11] & 0x3F) == 0x0F and e[12] == 0:
            run.append(off)
            continue
        if bytes(e[0:11]) == name83.encode("latin-1"):
            return run
        run = []
    return []


def valid_lfn_before(img, g, dirclus, name83):
    run = lfn_group_before(img, g, dirclus, name83)
    want = lfn_checksum(name83)
    if not run:
        return False
    for off in run:
        if img[off] == 0xE5 or (img[off + 11] & 0x3F) != 0x0F:
            return False
        if img[off + 13] != want:
            return False
    return True


# -------------------------------------------------------------- verify

def verify(path, allow_orphans=False):
    img, g = load(path)
    bad = []

    # V1: FAT copies identical
    for copy in range(1, g.nfats):
        a = (g.fatbase) * SEC
        b = (g.fatbase + copy * g.fatsz) * SEC
        if img[a:a + g.fatsz * SEC] != img[b:b + g.fatsz * SEC]:
            bad.append("V1: FAT1 != FAT%d" % (copy + 1))

    # V2: every entry classifiable
    for c in range(2, g.nfatent):
        v = get_fat(img, g, c)
        if v == 0 or v == g.bad or v >= g.eoc_min or (2 <= v < g.nfatent):
            continue
        bad.append("V2: FAT[%d] = %#x invalid" % (c, v))

    # V3/V4: walk tree, claim clusters
    owner = {}
    def claim_chain(head, who, expect_len=None):
        seen_here = []
        c = head
        while True:
            if not (2 <= c < g.nfatent):
                bad.append("V3: %s: link out of range (%#x)" % (who, c))
                break
            if c in owner:
                bad.append("V3: %s: cluster %d already owned by %s" % (who, c, owner[c]))
                break
            owner[c] = who
            seen_here.append(c)
            v = get_fat(img, g, c)
            if v >= g.eoc_min:
                break
            if v == g.bad or v == 0:
                bad.append("V3: %s: chain not EOC-terminated (FAT[%d]=%#x)" % (who, c, v))
                break
            c = v
        if expect_len is not None and len(seen_here) != expect_len:
            bad.append("V4: %s: chain len %d != expected %d" % (who, len(seen_here), expect_len))
        return seen_here

    if g.type == 32:
        claim_chain(g.rootclus, "/")
    stack = [(0, "/")]
    visited_dirs = set()
    while stack:
        d, pathname = stack.pop()
        if d in visited_dirs:
            bad.append("V3: dir cluster %d re-entered (%s)" % (d, pathname))
            continue
        visited_dirs.add(d)
        for off in dir_slots(img, g, d):
            e = img[off:off + 32]
            if e[0] == 0x00:
                break
            if e[0] == 0xE5 or e[11] == 0x0F or (e[11] & 0x08):
                continue
            nm = e[0:11].decode("ascii", "replace")
            clus = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
            size = struct.unpack_from("<I", e, 28)[0]
            if nm[0] == ".":
                continue
            who = pathname + nm.strip()
            if e[11] & 0x10:
                claim_chain(clus, who)
                stack.append((clus, who + "/"))
            else:
                if clus == 0 and size == 0:
                    continue
                expect = max(1, ceil_div(size, g.clus_bytes()))
                claim_chain(clus, who, expect)

    # V5: orphans
    orphans = 0
    for c in range(2, g.nfatent):
        v = get_fat(img, g, c)
        if v == 0 or v == g.bad:
            continue
        if c not in owner:
            orphans += 1
    if orphans and not allow_orphans:
        bad.append("V5: %d orphan cluster(s)" % orphans)

    # V6: content of generator files that should be untouched
    for name in ("FILE2   TXT", "DEEP    TXT"):
        try:
            off, chain = find_file_chain(img, g, name)
            e = img[off:off + 32]
            size = struct.unpack_from("<I", e, 28)[0]
            data = bytearray()
            cb = g.clus_bytes()
            for c in chain:
                o = g.clus_lba(c) * SEC
                data += img[o:o + cb]
            if bytes(data[:size]) != content_for(name, size):
                bad.append("V6: %s content damaged" % name.strip())
        except SystemExit:
            bad.append("V6: %s missing" % name.strip())

    # V7: base-image LFN guards. EXACT13 exercises the no-NUL exact
    # multiple representation; the future type must survive every /F run.
    if not valid_lfn_before(img, g, 0, "EXACT13 TXT"):
        bad.append("V7: exact-13 LFN missing or invalid")
    future_ok = False
    for off in dir_slots(img, g, 0):
        e = img[off:off + 32]
        if e[0] == 0x00:
            break
        if (e[11] & 0x3F) == 0x0F and e[12] == 1:
            future_ok = e[26] == 0x34 and e[27] == 0x12
            break
    if not future_ok:
        bad.append("V7: future LFN dirent type was modified/deleted")

    if bad:
        print("VERIFY FAIL (%d):" % len(bad))
        for m in bad[:30]:
            print("  " + m)
        if len(bad) > 30:
            print("  ... and %d more" % (len(bad) - 30))
        return 1
    print("VERIFY OK")
    return 0


# ---------------------------------------------------------------- main

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "lfnsum":
        # args: lfnsum <name83> -- print the LFN checksum as hex, so a
        # shell test can assert a restamped group carries the checksum
        # of the SFN it now sits in front of. `path` is the name here.
        print("%#04x" % lfn_checksum(path))
        return 0
    if cmd == "build":
        build(path, sys.argv[3])
        return 0
    if cmd == "corrupt":
        img, g = load(path)
        for sc in sys.argv[3:]:
            corrupt(img, g, sc)
        save(path, img)
        return 0
    if cmd == "verify":
        return verify(path, allow_orphans="--allow-orphans" in sys.argv)
    if cmd == "hasname":
        img, g = load(path)
        ok = has_live_entry(img, g, 0, sys.argv[3])
        print("PRESENT" if ok else "ABSENT")
        return 0 if ok else 1
    if cmd == "haslfn":
        img, g = load(path)
        ok = valid_lfn_before(img, g, 0, sys.argv[3])
        print("VALID" if ok else "MISSING/BAD")
        return 0 if ok else 1
    if cmd == "locate":
        # Print the absolute byte offset of name83's dirent. Must be
        # run against a still-ASCII (pre-corruption) name -- find_file_
        # chain compares via ascii-decode, which can't match a name
        # already holding a high byte.
        img, g = load(path)
        off, _ = find_file_chain(img, g, sys.argv[3])
        print(off)
        return 0
    if cmd == "ts":
        # args: ts <name83> -- print the five timestamp words as hex in
        # dirent order, so a shell test can assert which fields a repair
        # rewrote and which it left alone.
        img, g = load(path)
        off, _ = find_file_chain(img, g, sys.argv[3])
        print("crt_time=%04x crt_date=%04x acc_date=%04x "
              "wrt_time=%04x wrt_date=%04x"
              % (struct.unpack_from("<H", img, off + 14)[0],
                 struct.unpack_from("<H", img, off + 16)[0],
                 struct.unpack_from("<H", img, off + 18)[0],
                 struct.unpack_from("<H", img, off + 22)[0],
                 struct.unpack_from("<H", img, off + 24)[0]))
        return 0
    if cmd == "checkbyte":
        # args: path abs-offset expected-hex-byte
        img, g = load(path)
        off = int(sys.argv[3])
        want = int(sys.argv[4], 16)
        got = img[off]
        print("byte@%d = %#04x (want %#04x)" % (off, got, want))
        return 0 if got == want else 1
    print("unknown command %r" % cmd)
    return 2


if __name__ == "__main__":
    sys.exit(main())
