#!/usr/bin/env python3
"""Ship of Harkinian for the PlayStation 5 - eboot.sym, the crash handler's symbol table.

    make-sym.py [--nm llvm-nm] TITLE.elf OUT.sym

From the linked ELF (link-title.sh's llvm-pie.elf, before signing): its functions,
sorted, in the binary layout ps5/src/crash.c reads with pread:

    "SOHSYM1\\n"  u64 link address of ps5_crash_anchor (tells another build's file)
    u32 count, u32 file offset of the names
    count x { u64 address, u32 size, u32 name offset }
    names, NUL-terminated (demangled, at most 160 bytes)

SPDX-License-Identifier: MIT
"""
import argparse, struct, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument("--nm", default="llvm-nm")
ap.add_argument("elf")
ap.add_argument("out")
a = ap.parse_args()
text = subprocess.run([a.nm, "--defined-only", "-S", "-C", a.elf], check=True,
                      capture_output=True, text=True).stdout
syms, anchor = {}, None
for line in text.splitlines():
    parts = line.split(None, 3)
    if len(parts) == 4:
        addr, size, kind, name = parts
    elif len(parts) == 3:
        addr, kind, name = parts
        size = "0"
    else:
        continue
    if kind not in "tTwW":
        continue
    addr, size = int(addr, 16), int(size, 16)
    if name == "ps5_crash_anchor":
        anchor = addr
    # one name per address: the sized, global one
    old = syms.get(addr)
    if old is None or (old[0] == 0 and size) or (kind in "TW" and old[2] in "tw"):
        syms[addr] = (size, name[:160], kind)
if anchor is None:
    sys.exit("make-sym.py: no ps5_crash_anchor in %s" % a.elf)
names, offsets, table = bytearray(), {}, []
for addr in sorted(syms):
    size, name, _ = syms[addr]
    if name not in offsets:
        offsets[name] = len(names)
        names += name.encode() + b"\0"
    table.append(struct.pack("<QII", addr, min(size, 0xFFFFFFFF), offsets[name]))
head = b"SOHSYM1\n" + struct.pack("<QII", anchor, len(table), 24 + 16 * len(table))
with open(a.out, "wb") as f:
    f.write(head + b"".join(table) + bytes(names))
print("make-sym.py: %s: %d functions, %d KB" % (a.out, len(table), (len(head) + 16 * len(table) + len(names)) >> 10))
