#!/usr/bin/env python3
"""Patch copies of Debian arm64 glibc (ld.so, libc.so.6) and libuv for Android app seccomp.
- glibc: every inline `mov x8,#NR ... svc #0` (svc within 12 instructions, x8 not rewritten in between)
  with NR in BLOCK -> the svc becomes `movn x0,#37` (x0 = -ENOSYS).
- libc: "/etc/resolv.conf\\0" -> "/proc/self/fd/63\\0" (same length); the launcher opens fd 63 on the real file.
- libuv: `mov x0,#NR; bl syscall@plt` with NR in io_uring_* -> the bl becomes `movn x0,#0` (returns -1).
Usage: patch-glibc.py ld|libc|libuv SRC DST
(From the tablet spike, ~/glibc-spike/patch-glibc.py, 2026-10-30. Hits on Debian glibc 2.41-12+deb13u4:
 ld.so 0x10734 set_robust_list, 0x107d8 rseq; libc 0x85f6c/0xbfc08 set_robust_list, 0x86224 rseq,
 resolv.conf string at 0x166870; libuv 1.50: enter 0x23838/0x23ad0/0x24738/0x25ad8,
 setup 0x24250/0x246f8, register 0x24774.)
"""
import struct, sys

BLOCK = {99: 'set_robust_list', 100: 'get_robust_list', 293: 'rseq',
         425: 'io_uring_setup', 426: 'io_uring_enter', 427: 'io_uring_register'}
SVC0 = 0xD4000001
ENOSYS_X0 = 0x928004A0      # movn x0,#37  -> -38
MINUS1_X0 = 0x92800000      # movn x0,#0   -> -1

def words(b):
    return struct.unpack_from('<%dI' % (len(b) // 4), b)

def movz(reg, w):
    """return imm if w is `movz Xreg/Wreg, #imm` (no shift), else None"""
    if (w & 0x7FE00000) == 0x52800000 and (w & 0x1F) == reg and ((w >> 21) & 3) == 0:
        return (w >> 5) & 0xFFFF
    return None

def writes_reg(w, reg):
    # conservative: any instruction whose Rd field is reg, except stores/branches/svc (approximation good enough here)
    if w == SVC0: return False
    op = w & 0x3B000000
    if op in (0x39000000, 0x38000000, 0x29000000, 0x28000000):   # load/store families: Rt may be reg on loads
        return (w & 0x00400000) and (w & 0x1F) == reg
    if (w & 0x7C000000) == 0x14000000: return False             # b/bl
    return (w & 0x1F) == reg

def patch_svc(b, nrs):
    w = words(b); hits = []
    for i, x in enumerate(w):
        nr = movz(8, x)
        if nr is None or nr not in nrs: continue
        for j in range(i + 1, min(i + 13, len(w))):
            if w[j] == SVC0:
                struct.pack_into('<I', b, j * 4, ENOSYS_X0); hits.append((hex(j * 4), BLOCK[nr])); break
            if writes_reg(w[j], 8): break
    return hits

def patch_libuv(b):
    w = words(b); hits = []
    for i, x in enumerate(w[:-4]):
        nr = movz(0, x)
        if nr not in (425, 426, 427): continue
        for j in range(i + 1, i + 5):
            if (w[j] & 0xFC000000) == 0x94000000:                   # bl
                struct.pack_into('<I', b, j * 4, MINUS1_X0); hits.append((hex(j * 4), BLOCK[nr])); break
    return hits

def main():
    if len(sys.argv) != 4:
        sys.exit('Použití: patch-glibc.py ld|libc|libuv ZDROJ CÍL')
    kind, src, dst = sys.argv[1:4]
    b = bytearray(open(src, 'rb').read())
    if len(b) < 64 or b[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', b, 18)[0] != 183:
        sys.exit('Chyba: zdroj musí být ELF64 pro aarch64 (little endian).')
    if kind in ('ld', 'libc'):
        hits = patch_svc(b, {99, 100, 293})
        if not hits:
            sys.exit('Chyba: nebyly nalezeny instrukce pro úpravu glibc; výstup nebyl zapsán.')
        if kind == 'libc':
            old, new = b'/etc/resolv.conf\0', b'/proc/self/fd/63\0'
            n = b.count(old)
            if n != 1:
                sys.exit('Chyba: očekáván právě jeden řetězec resolv.conf, nalezeno %d.' % n)
            k = b.find(old); b[k:k + len(new)] = new; hits.append((hex(k), 'resolv.conf -> /proc/self/fd/63'))
    elif kind == 'libuv':
        hits = patch_libuv(b)
    else:
        sys.exit('Chyba: typ musí být ld|libc|libuv.')
    if not hits:
        sys.exit('Chyba: nebyly nalezeny instrukce pro úpravu; výstup nebyl zapsán.')
    open(dst, 'wb').write(b); __import__('os').chmod(dst, 0o755)
    for h in hits: print(kind, *h)

if __name__ == '__main__':
    main()
