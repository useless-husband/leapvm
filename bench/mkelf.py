#!/usr/bin/env python3
"""把 clang 產生的單一 ELF 目的檔（.o）組成可以在 Linux 上執行的靜態執行檔。

手邊沒有 Linux 的連結器，這支程式代替它：把程式碼區段排好、套用重定位、寫出執行檔的檔頭。
只支援這個測試會用到的少數重定位類型。
"""
import struct
import sys

BASE = 0x400000


def main(obj_path, out_path):
    d = open(obj_path, "rb").read()
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
            "<IIQQQQIIQQ", d, shoff + i * shentsize)
        secs.append(dict(name=name, type=typ, flags=flags, off=off, size=size, link=link, info=info,
                         align=max(align, 1), entsize=entsize))
    shstr = secs[shstrndx]
    for s in secs:
        s["name"] = d[shstr["off"] + s["name"]:d.index(b"\0", shstr["off"] + s["name"])].decode()

    # 排版：所有要載入的區段（SHF_ALLOC）依序放好
    image = bytearray()
    placed = {}
    hdr = 0x1000
    for i, s in enumerate(secs):
        if not s["flags"] & 2:
            continue
        pos = (len(image) + s["align"] - 1) // s["align"] * s["align"]
        image.extend(b"\0" * (pos - len(image)))
        placed[i] = BASE + hdr + pos
        image.extend(b"\0" * s["size"] if s["type"] == 8 else d[s["off"]:s["off"] + s["size"]])

    symtab = next(s for s in secs if s["type"] == 2)
    strtab = secs[symtab["link"]]
    syms = []
    for k in range(symtab["size"] // 24):
        name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", d, symtab["off"] + k * 24)
        nm = d[strtab["off"] + name:d.index(b"\0", strtab["off"] + name)].decode()
        addr = placed.get(shndx, 0) + value if shndx in placed else None
        syms.append((nm, addr))

    def patch(off, mask, val):
        ins, = struct.unpack_from("<I", image, off)
        struct.pack_into("<I", image, off, (ins & ~mask) | (val & mask))

    for s in secs:
        if s["type"] != 4 or s["info"] not in placed:  # SHT_RELA
            continue
        target = placed[s["info"]]
        for k in range(s["size"] // 24):
            r_off, r_info, addend = struct.unpack_from("<QQq", d, s["off"] + k * 24)
            sym, typ = r_info >> 32, r_info & 0xffffffff
            name, S = syms[sym]
            if S is None:
                sys.exit(f"找不到符號 {name}（程式用到了 C 函式庫？）")
            P = target + r_off
            off = P - BASE - hdr
            v = S + addend
            if typ in (282, 283):  # JUMP26 / CALL26
                patch(off, 0x03ffffff, (v - P) >> 2)
            elif typ == 275:  # ADR_PREL_PG_HI21
                pg = ((v & ~0xfff) - (P & ~0xfff)) >> 12
                patch(off, (3 << 29) | (0x7ffff << 5), ((pg & 3) << 29) | (((pg >> 2) & 0x7ffff) << 5))
            elif typ == 277:  # ADD_ABS_LO12_NC
                patch(off, 0xfff << 10, (v & 0xfff) << 10)
            elif typ in (278, 284, 285, 286, 299):  # LDST8/16/32/64/128_ABS_LO12_NC
                shift = {278: 0, 284: 1, 285: 2, 286: 3, 299: 4}[typ]
                patch(off, 0xfff << 10, ((v & 0xfff) >> shift) << 10)
            else:
                sys.exit(f"不支援的重定位類型 {typ}（符號 {name}）")

    entry = next(a for n, a in syms if n == "_start")
    total = hdr + len(image)
    ehdr = struct.pack("<4sBBBBB7sHHIQQQIHHHHHH", b"\x7fELF", 2, 1, 1, 0, 0, b"\0" * 7,
                       2, 183, 1, entry, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    phdr = struct.pack("<IIQQQQQQ", 1, 7, 0, BASE, BASE, total, total, 0x10000)
    out = bytearray(ehdr + phdr)
    out.extend(b"\0" * (hdr - len(out)))
    out.extend(image)
    open(out_path, "wb").write(out)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
