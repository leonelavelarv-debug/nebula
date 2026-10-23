#!/usr/bin/env python3
"""Synthetic Wii discs for nebula-pak's tests: a small RMGE01-shaped disc (real partition encryption and hash tree,
made-up files and main.dol) as ISO, GCZ and RVZ (written here), plus the game.pak nebula-pak must make from it.
WBFS, CISO and WIA copies come from wit (Wiimm's ISO Tools) in run_tests.sh, an independent implementation.

  make_test_discs.py <out dir> <zstd binary>

Needs python3 with `cryptography`. GPL-3.0-or-later, as Nebula.
"""
import hashlib
import os
import random
import struct
import subprocess
import sys
import zlib

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

COMMON_KEY = bytes.fromhex("ebe42a225e8593e448d9c5457381aaf7")
PART_OFFSET = 0xF800000
DATA_OFFSET = 0x20000
CLUSTERS = 448  # 7 groups of 64 clusters: ~13.6 MiB of partition data
rng = random.Random(1234)


def rand(n):
    return rng.randbytes(n)


def aes_cbc(key, iv, data, decrypt=False):
    c = Cipher(algorithms.AES(key), modes.CBC(iv))
    op = c.decryptor() if decrypt else c.encryptor()
    return op.update(data) + op.finalize()


# --- the lagged Fibonacci padding generator (RVZ packing), as Dolphin's LaggedFibonacciGenerator ---------------
class Lfg:
    K, J = 521, 32

    def __init__(self, seed):
        b = [0] * self.K
        for i in range(17):
            b[i] = struct.unpack(">I", seed[4 * i:4 * i + 4])[0]
        for i in range(17, self.K):
            b[i] = ((b[i - 17] << 23) ^ (b[i - 16] >> 9) ^ b[i - 1]) & 0xFFFFFFFF
        b = [((x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000)) for x in b]
        self.b = b  # big-endian words: byte order of the output
        for _ in range(4):
            self.forward_block()
        self.pos = 0

    def forward_block(self):
        b = self.b
        for i in range(self.J):
            b[i] ^= b[i + self.K - self.J]
        for i in range(self.J, self.K):
            b[i] ^= b[i - self.J]

    def raw(self):
        return b"".join(struct.pack(">I", x) for x in self.b)

    def forward(self, n):
        self.pos += n
        while self.pos >= self.K * 4:
            self.forward_block()
            self.pos -= self.K * 4

    def get(self, n):
        out = bytearray()
        while n > 0:
            raw = self.raw()
            take = min(n, self.K * 4 - self.pos)
            out += raw[self.pos:self.pos + take]
            self.pos += take
            n -= take
            if self.pos == self.K * 4:
                self.forward_block()
                self.pos = 0
        return bytes(out)


def build_partition():
    """Decrypted partition data (0x7C00 per cluster), the expected game.pak, and the padding runs."""
    size = CLUSTERS * 0x7C00
    data = bytearray(size)
    pak_parts = []  # (offset, bytes)

    def put(offset, blob):
        data[offset:offset + len(blob)] = blob
        pak_parts.append((offset, blob))

    # main.dol: one text and one data section.
    dol_offset = 0x4000
    dol = bytearray(rand(0x3900))
    dol[0:0x100] = bytes(0x100)
    struct.pack_into(">I", dol, 0x00, 0x100)      # text 0 offset
    struct.pack_into(">I", dol, 0x1C, 0x3100)     # data 0 offset
    struct.pack_into(">I", dol, 0x90, 0x3000)     # text 0 size
    struct.pack_into(">I", dol, 0xAC, 0x800)      # data 0 size
    struct.pack_into(">I", dol, 0x48, 0x80004000)  # text 0 address
    dol = bytes(dol)

    # Files: random, zeros, text-like; laid out with gaps (padding junk on a real disc).
    names = ["AudioRes/Seqs.arc", "ObjectData/Kuribo.arc", "StageData/Big.arc", "Zero.bin", "Empty.bin",
             "LayoutData/Text.arc", "tiny.txt"]
    sizes = [0x123457, 0x4321, 0x6A0001, 0x300000, 0, 0x2000, 3]
    fst_offset = 0x8000
    layout = []
    at = 0x20000
    for name, length in zip(names, sizes):
        at = (at + 0x7FFF + rng.randrange(0, 3) * 0x8000) & ~0x7FFF
        layout.append((name, at, length))
        at += length
    # The FST: root, two levels of directories.
    entries, strings = [], bytearray()

    def name_off(n):
        o = len(strings)
        strings.extend(n.encode() + b"\0")
        return o
    tree = {}
    for name, off, length in layout:
        parts = name.split("/")
        node = tree
        for d in parts[:-1]:
            node = node.setdefault(d, {})
        node[parts[-1]] = (off, length)
    flat = []

    def walk(node, parent):
        for key in sorted(node):
            value = node[key]
            if isinstance(value, dict):
                index = len(flat)
                flat.append(["dir", key, parent, 0])
                walk(value, index + 1)
                flat[index][3] = len(flat) + 1
            else:
                flat.append(["file", key, value[0], value[1]])
    walk(tree, 0)
    count = len(flat) + 1
    entries.append(struct.pack(">BBHII", 1, 0, 0, 0, count))
    name_off("")
    for kind, name, a, b in flat:
        n = name_off(name)
        if kind == "dir":
            entries.append(struct.pack(">I", (1 << 24) | n) + struct.pack(">II", a, b))
        else:
            entries.append(struct.pack(">I", n) + struct.pack(">II", a >> 2, b))
    fst = b"".join(entries) + bytes(strings)
    fst += bytes((-len(fst)) % 4)
    fst_max = len(fst) + 0x40

    boot = bytearray(0x440)
    boot[0:6] = b"RMGE01"
    boot[0x18:0x1C] = bytes.fromhex("5D1C9EA3")
    boot[0x20:0x20 + 18] = b"SUPER MARIO GALAXY"
    struct.pack_into(">IIII", boot, 0x420, dol_offset >> 2, fst_offset >> 2, len(fst) >> 2, fst_max >> 2)
    bi2 = rand(0x2000)
    apploader = bytearray(rand(0x20 + 0x1000 + 0x100))
    struct.pack_into(">II", apploader, 0x14, 0x1000, 0x100)

    put(0, bytes(boot))
    put(0x440, bi2)
    put(0x2440, bytes(apploader))
    put(dol_offset, dol)
    put(fst_offset, fst)
    contents = {}
    for name, off, length in layout:
        if name == "Zero.bin":
            blob = bytes(length)
        elif name == "LayoutData/Text.arc":
            blob = (b"Super Mario Galaxy " * (length // 19 + 1))[:length]
        else:
            blob = rand(length)
        contents[name] = blob
        put(off, blob)

    # Padding junk in the gaps (what the RVZ writer packs as seeds), split at 0x1F000 (an RVZ group in data units).
    used = sorted((o, o + len(b)) for o, b in pak_parts if b)
    junk = []
    cursor = 0
    gaps = []
    for s, e in used:
        if s > cursor:
            gaps.append((cursor, s))
        cursor = max(cursor, e)
    gaps.append((cursor, size))
    for s, e in gaps:
        if s < 0x40000 and e <= 0x40000:  # leave the system area's small gaps zero
            continue
        p = s
        while p < e:
            q = min(e, (p // 0x1F000 + 1) * 0x1F000)
            seed = rand(68)
            g = Lfg(seed)
            g.forward(p % 0x8000)
            data[p:q] = g.get(q - p)
            junk.append((p, q - p, seed))
            p = q

    end = max(o + len(b) for o, b in pak_parts)
    pak = bytearray(end)
    for o, b in pak_parts:
        pak[o:o + len(b)] = b
    return bytes(data), bytes(pak), junk, dol


def encrypt_partition(data, key):
    """Clusters with H0/H1/H2 hashes, encrypted; and the H3 table."""
    out = bytearray(CLUSTERS * 0x8000)
    h3 = bytearray(0x18000)
    for group in range(CLUSTERS // 64):
        h0s, h1s = [], []
        for c in range(64):
            cl = group * 64 + c
            d = data[cl * 0x7C00:(cl + 1) * 0x7C00]
            h0s.append(b"".join(hashlib.sha1(d[i * 0x400:(i + 1) * 0x400]).digest() for i in range(31)))
        for sg in range(8):
            h1s.append(b"".join(hashlib.sha1(h0s[sg * 8 + i]).digest() for i in range(8)))
        h2 = b"".join(hashlib.sha1(h1).digest() for h1 in h1s)
        h3[group * 20:group * 20 + 20] = hashlib.sha1(h2).digest()
        for c in range(64):
            cl = group * 64 + c
            hb = h0s[c] + bytes(0x14) + h1s[c // 8] + bytes(0x20) + h2 + bytes(0x20)
            assert len(hb) == 0x400
            ehb = aes_cbc(key, bytes(16), hb)
            d = data[cl * 0x7C00:(cl + 1) * 0x7C00]
            out[cl * 0x8000:cl * 0x8000 + 0x400] = ehb
            out[cl * 0x8000 + 0x400:(cl + 1) * 0x8000] = aes_cbc(key, ehb[0x3D0:0x3E0], d)
    return bytes(out), bytes(h3)


def build_disc(out_dir):
    data, pak, junk, dol = build_partition()
    title_key = rand(16)
    title_id = bytes.fromhex("00010000524d4745")

    ticket = bytearray(0x2A4)
    struct.pack_into(">I", ticket, 0, 0x00010001)
    ticket[0x140:0x140 + 26] = b"Root-CA00000001-XS00000003"
    ticket[0x1DC:0x1E4] = title_id
    ticket[0x1BF:0x1CF] = aes_cbc(COMMON_KEY, title_id + bytes(8), title_key)
    ticket[0x1F1] = 0

    enc, h3 = encrypt_partition(data, title_key)
    tmd = bytearray(0x208)
    struct.pack_into(">I", tmd, 0, 0x00010001)
    tmd[0x18C:0x194] = title_id
    struct.pack_into(">H", tmd, 0x1DE, 1)
    struct.pack_into(">IHHQ", tmd, 0x1E4, 0, 0, 3, 0xFF7C0000)
    tmd[0x1F4:0x208] = hashlib.sha1(h3).digest()

    header = bytearray(0x2C0)
    header[0:0x2A4] = ticket
    struct.pack_into(">IIIIIII", header, 0x2A4, 0x208, 0x2C0 >> 2, 0x400, 0x4C0 >> 2, 0x8000 >> 2,
                     DATA_OFFSET >> 2, (CLUSTERS * 0x8000) >> 2)
    part = bytearray(DATA_OFFSET)
    part[0:0x2C0] = header
    part[0x2C0:0x2C0 + 0x208] = tmd
    part[0x4C0:0x4C0 + 0x400] = rand(0x400)  # certificate chain stand-in
    part[0x8000:0x8000 + len(h3)] = h3

    disc_size = PART_OFFSET + DATA_OFFSET + len(enc)
    iso = os.path.join(out_dir, "smg.iso")
    with open(iso, "wb") as f:
        f.truncate(disc_size)
        head = bytearray(0x100)
        head[0:6] = b"RMGE01"
        head[0x18:0x1C] = bytes.fromhex("5D1C9EA3")
        head[0x20:0x20 + 18] = b"SUPER MARIO GALAXY"
        f.seek(0)
        f.write(head)
        f.seek(0x40000)
        f.write(struct.pack(">II", 1, 0x40020 >> 2) + bytes(24) + struct.pack(">II", PART_OFFSET >> 2, 0))
        f.seek(0x4E000)
        f.write(struct.pack(">I", 1) + bytes(12) + bytes([0x80] * 16))
        f.seek(PART_OFFSET)
        f.write(part)
        f.write(enc)
    with open(os.path.join(out_dir, "expected.pak"), "wb") as f:
        f.write(pak)
    with open(os.path.join(out_dir, "dol.sha1"), "w") as f:
        f.write(hashlib.sha1(dol).hexdigest())
    return iso, data, junk, title_key, part


def write_gcz(iso, out):
    block = 0x8000
    with open(iso, "rb") as f:
        disc = f.read()
    n = (len(disc) + block - 1) // block
    pointers, hashes, body = [], [], bytearray()
    for i in range(n):
        raw = disc[i * block:(i + 1) * block].ljust(block, b"\0")
        comp = zlib.compress(raw, 6)
        if len(comp) >= block or i % 7 == 3:  # some stored blocks too
            pointers.append(len(body) | (1 << 63))
            stored = raw
        else:
            pointers.append(len(body))
            stored = comp
        hashes.append(zlib.adler32(stored) & 0xFFFFFFFF)
        body += stored
    with open(out, "wb") as f:
        f.write(struct.pack("<IIQQII", 0xB10BC001, 1, len(body), len(disc), block, n))
        f.write(b"".join(struct.pack("<Q", p) for p in pointers))
        f.write(b"".join(struct.pack("<I", h) for h in hashes))
        f.write(body)


def zstd(tool, blob):
    return subprocess.run([tool, "-q", "-c", "-3", "--no-check"], input=blob, stdout=subprocess.PIPE,
                          check=True).stdout


def write_rvz(iso, data, junk, title_key, out, tool, chunk=0x20000):
    """RVZ as Dolphin writes it (docs/WiaAndRvz.md): raw data + one partition in two data entries, Zstandard,
    RVZ packing of the padding, some groups stored uncompressed, some hash exception lists."""
    with open(iso, "rb") as f:
        disc = f.read()
    chunk_d = chunk // 0x8000 * 0x7C00
    groups = []  # (stored bytes, compressed?, packed size)
    body = bytearray()
    body_start = 0x10000

    def add_group(payload, exceptions=None, packed=0, compress=True):
        if payload is not None and exceptions is None and not packed and not any(payload):
            groups.append((None, 0, 0))
            return
        lists = b""
        if exceptions is not None:
            lists = struct.pack(">H", len(exceptions)) + b"".join(struct.pack(">H", o) + h for o, h in exceptions)
        if compress:
            stored = zstd(tool, lists + payload)
        else:
            stored = lists + bytes((-len(lists)) % 4 if lists else 0) + payload
        groups.append((stored, compress, packed))

    raw_entries = []

    def add_raw(offset, size):
        first = len(groups)
        skipped = offset % 0x8000
        start = offset - skipped
        total = size + skipped
        for g in range(0, total, chunk):
            add_group(disc[start + g:start + min(total, g + chunk)])
        raw_entries.append((offset, size, first, len(groups) - first))

    add_raw(0x80, PART_OFFSET - 0x80)
    add_raw(PART_OFFSET, DATA_OFFSET)

    junk_at = {p: (n, s) for p, n, s in junk}
    junk_starts = sorted(junk_at)

    def pack(start, end, entry_start):
        out = bytearray()
        p = start
        while p < end:
            run = None
            for js in junk_starts:
                n, s = junk_at[js]
                if js <= p < js + n:
                    run = (js, n, s)
                    break
            if run and run[0] == p and p + run[1] <= end and run[1] >= 68 + 4:
                out += struct.pack(">I", run[1] | 0x80000000) + run[2]
                p += run[1]
                continue
            q = end
            for js in junk_starts:
                if p < js < q:
                    q = js
            out += struct.pack(">I", q - p) + data[p:q]
            p = q
        return bytes(out)

    first_sector = (PART_OFFSET + DATA_OFFSET) // 0x8000
    split = 64  # sectors in the first data entry
    pd = []
    for j, (s0, n) in enumerate([(0, split), (split, CLUSTERS - split)]):
        first_group = len(groups)
        start = s0 * 0x7C00
        length = n * 0x7C00
        for g in range(0, length, chunk_d):
            a, b = start + g, start + min(length, g + chunk_d)
            gi = len(groups)
            exceptions = []
            if gi % 3 == 0:
                exceptions = [(rng.randrange(0, 0x400 * (chunk // 0x8000) - 20), rand(20)) for _ in range(gi % 5 + 1)]
            compress = gi % 4 != 1
            if any(js < b and js + junk_at[js][0] > a for js in junk_starts):
                packed = pack(a, b, start)
                add_group(packed, exceptions, packed=len(packed), compress=compress)
            else:
                add_group(data[a:b], exceptions, compress=compress)
        pd.append((first_sector + s0, n, first_group, len(groups) - first_group))

    # Group data in the file, then the tables.
    group_table = bytearray()
    for stored, compressed, packed in groups:
        if stored is None:
            group_table += struct.pack(">III", 0, 0, 0)
            continue
        while (body_start + len(body)) % 4:
            body.append(0)
        offset = body_start + len(body)
        body += stored
        size = len(stored) | (0x80000000 if compressed else 0)
        group_table += struct.pack(">III", offset >> 2, size, packed)
    raw_table = b"".join(struct.pack(">QQII", *e) for e in raw_entries)
    part_entry = title_key + b"".join(struct.pack(">IIII", *p) for p in pd)

    raw_c = zstd(tool, raw_table)
    group_c = zstd(tool, bytes(group_table))
    part_off = body_start + len(body)
    raw_off = part_off + len(part_entry)
    group_off = raw_off + len(raw_c)
    file_size = group_off + len(group_c)

    h2 = bytearray(0xDC)
    struct.pack_into(">IIiI", h2, 0, 2, 5, 3, chunk)
    h2[0x10:0x90] = disc[:0x80]
    struct.pack_into(">IIQ", h2, 0x90, 1, len(part_entry), part_off)
    h2[0xA0:0xB4] = hashlib.sha1(part_entry).digest()
    struct.pack_into(">IQI", h2, 0xB4, len(raw_entries), raw_off, len(raw_c))
    struct.pack_into(">IQI", h2, 0xC4, len(groups), group_off, len(group_c))
    h2[0xD4] = 0
    h1 = bytearray(0x48)
    h1[0:4] = b"RVZ\x01"
    struct.pack_into(">III", h1, 4, 0x01000000, 0x00030000, len(h2))
    h1[0x10:0x24] = hashlib.sha1(h2).digest()
    struct.pack_into(">QQ", h1, 0x24, len(disc), file_size)
    h1[0x34:0x48] = hashlib.sha1(h1[:0x34]).digest()
    with open(out, "wb") as f:
        f.write(h1)
        f.write(h2)
        f.write(bytes(body_start - 0x48 - len(h2)))
        f.write(body)
        f.write(part_entry)
        f.write(raw_c)
        f.write(group_c)


def main():
    out_dir, tool = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    iso, data, junk, key, part = build_disc(out_dir)
    write_gcz(iso, os.path.join(out_dir, "smg.gcz"))
    write_rvz(iso, data, junk, key, os.path.join(out_dir, "smg.rvz"), tool)
    write_rvz(iso, data, junk, key, os.path.join(out_dir, "smg-2m.rvz"), tool, chunk=0x200000)
    print("discos de prueba en", out_dir, "(%d tramos de relleno)" % len(junk))


if __name__ == "__main__":
    main()
