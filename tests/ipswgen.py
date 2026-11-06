import sys, zipfile, struct, subprocess, os
out, enc, code = sys.argv[1], sys.argv[2], sys.argv[3]
def tlv(tag, b):
    n = len(b)
    l = bytes([n]) if n < 128 else (bytes([0x81, n]) if n < 256 else (bytes([0x82, n >> 8, n & 255]) if n < 65536 else bytes([0x83, n >> 16, (n >> 8) & 255, n & 255]) if n < 1 << 24 else bytes([0x84]) + struct.pack(">I", n)))
    return bytes([tag]) + l + b
def im4p(typ, data, kbag=False):
    body = tlv(0x16, b"IM4P") + tlv(0x16, typ) + tlv(0x16, b"test") + tlv(0x04, data)
    if kbag: body += tlv(0x04, tlv(0x30, tlv(0x30, tlv(0x02, b"\x01") + tlv(0x04, b"K" * 16) + tlv(0x04, b"V" * 32))))
    return tlv(0x30, body)
def macho(codebytes):
    base, off = 0xFFFFFFF007004000, 0x1000
    seg = struct.pack("<II16sQQQQIIII", 0x19, 72, b"__TEXT", base, 0x4000, 0, off + len(codebytes), 7, 5, 0, 0)
    th = struct.pack("<IIII", 5, 16 + 68 * 4, 6, 68) + b"".join(struct.pack("<Q", base + off if i == 32 else 0) for i in range(34))
    hdr = struct.pack("<IIIIIIII", 0xFEEDFACF, 0x0100000C, 0, 2, 2, len(seg) + len(th), 1, 0) + seg + th
    return hdr.ljust(off, b"\0") + codebytes
def lzss_encode(d):                         # greedy LZSS matching Apple's decoder (N=4096, F=18, T=2)
    N, F, T = 4096, 18, 2; buf = bytearray(b" " * N); r = N - F; o = bytearray(); i = 0
    ring = bytearray(b" " * (N - F)) ; hist = bytearray(b" " * (N - F))   # logical history, position = len(hist) mod N
    out = bytearray(); items = []
    while i < len(d):
        best, bp = 0, 0
        for p in range(max(0, len(hist) - N + F), len(hist)):             # positions still in the ring
            l = 0
            while l < F and i + l < len(d) and (hist + d[i:i + l])[p + l] == d[i + l]: l += 1
            if l > best: best, bp = l, p
        if best > T:
            items.append(("m", bp % N, best)); hist += d[i:i + best]; i += best
        else:
            items.append(("l", d[i])); hist.append(d[i]); i += 1
    for k in range(0, len(items), 8):
        grp = items[k:k + 8]; flag = 0; payload = bytearray()
        for b, it in enumerate(grp):
            if it[0] == "l": flag |= 1 << b; payload.append(it[1])
            else: pos, ln = it[1], it[2]; payload += bytes([pos & 255, ((pos >> 4) & 0xF0) | (ln - T - 1)])
        out.append(flag); out += payload
    return bytes(out)
def complzss(d):
    c = lzss_encode(d)
    return b"complzss" + b"\0\0\0\0" + struct.pack(">II", len(d), len(c)) + b"\0" * 4 + b"\0" * (0x180 - 24) + c
def lz(d):
    open(out + "/tmp.bin", "wb").write(d); subprocess.check_call([enc, out + "/tmp.bin", out + "/tmp.lz"]); return open(out + "/tmp.lz", "rb").read()
kern = macho(open(code, "rb").read())
dt = im4p(b"dtre", b"FAKE-DEVICETREE-BLOB\0")
meta = {"BuildManifest.plist": b"<plist/>", "Restore.plist": b"<plist/>", "Firmware/all_flash/DeviceTree.t8030.im4p": dt}
def mk(name, kc, method, extra=None):
    with zipfile.ZipFile(f"{out}/{name}", "w", method) as z:
        for k, v in meta.items(): z.writestr(k, v)
        z.writestr("kernelcache.release.t8030", kc)
        for k, v in (extra or {}).items(): z.writestr(k, v)
if len(sys.argv) > 4:
    mk("spin.zip", im4p(b"krnl", lz(macho(open(sys.argv[4], "rb").read()))), zipfile.ZIP_DEFLATED)
if len(sys.argv) > 5:
    mk("vecspin.zip", im4p(b"krnl", lz(macho(open(sys.argv[5], "rb").read()))), zipfile.ZIP_DEFLATED)
mk("ok_lzfse.zip", im4p(b"krnl", lz(kern)), zipfile.ZIP_DEFLATED)
mk("ok_lzss_stored.zip", im4p(b"krnl", complzss(kern)), zipfile.ZIP_STORED)
mk("ok_plain.zip", kern, zipfile.ZIP_DEFLATED)                     # unwrapped Mach-O (e.g. user-decrypted)
mk("encrypted.zip", im4p(b"krnl", os.urandom(4096), kbag=True), zipfile.ZIP_DEFLATED)
mk("arm32.zip", im4p(b"krnl", lz(struct.pack("<IIIIIIII", 0xFEEDFACE, 12, 9, 2, 0, 0, 0, 0) + b"\0" * 64)), zipfile.ZIP_DEFLATED)
mk("garbage.zip", im4p(b"krnl", lz(os.urandom(5000))), zipfile.ZIP_DEFLATED)
with zipfile.ZipFile(out + "/nokernel.zip", "w") as z: z.writestr("BuildManifest.plist", "x")
# round-trip data for the LZSS unit check
open(out + "/lzss_in.bin", "wb").write(kern); open(out + "/lzss.cmp", "wb").write(complzss(kern))
