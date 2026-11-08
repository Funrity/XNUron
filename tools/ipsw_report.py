#!/usr/bin/env python3
"""Read-only report of an IPSW: members, sizes, and for each Firmware image whether its IMG4 carries a KBAG (= the payload is encrypted).
Nothing is decrypted or extracted; this only answers 'could this firmware be used at all'."""
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
print("members:", len(z.namelist()))
for i in z.infolist():
    n = i.filename
    big = i.file_size > 50_000_000
    if big or n.startswith("Firmware/") or n.startswith("kernelcache") or n in ("BuildManifest.plist", "Restore.plist"):
        info = ""
        if n.endswith((".im4p", ".img4", ".trustcache", ".dmg")) or n.startswith("Firmware/") and not n.endswith(".plist"):
            try:
                head = z.open(n).read(min(i.file_size, 4_000_000)) if not big else z.open(n).read(16)
                if big:
                    info = "(large)"
                else:
                    info = "KBAG=%s IM4P=%s" % (b"KBAG" in head, b"IM4P" in head)
            except Exception as e:
                info = "err %s" % e
        print(f"{n}\t{i.file_size}\t{info}")
for n in z.namelist():
    if n.endswith(".dmg"):
        i = z.getinfo(n)
        with z.open(n) as f:
            head = f.read(8)
        print(f"DMG {n} size {i.file_size} first8 {head.hex()} ({'zlib/other' if head[:2]==b'x\\x9c' else 'unknown header'})")

# ---- 0.4.121: IM4P/IMG4 structure (read-only): type, description, payload size, KBAG present?, compression
def der(b, p):
    tag = b[p]; l = b[p+1]; p += 2
    if l & 0x80:
        n = l & 0x7f; l = int.from_bytes(b[p:p+n], "big"); p += n
    return tag, l, p
def items(b, p, end):
    out = []
    while p < end:
        t, l, q = der(b, p); out.append((t, l, q)); p = q + l
    return out
def im4p_info(b):
    t, l, p = der(b, 0)
    top = items(b, p, p + l)
    if top and top[0][0] == 0x16 and b[top[0][2]:top[0][2]+4] == b"IMG4":
        for t2, l2, q2 in top[1:]:
            if t2 == 0x30 and b[q2] == 0x16 and b[q2+2:q2+6] == b"IM4P": return im4p_info(b[q2-0:])
        t, l, p = der(b, top[1][2] - 0)  # unreachable fallback
    its = items(b, p, p + l)
    s = lambda x: b[x[2]:x[2]+x[1]].decode("latin1")
    r = {"magic": s(its[0]), "type": s(its[1]), "desc": s(its[2])[:40], "payload": its[3][1]}
    r["kbag"] = any(it[0] == 0x04 for it in its[4:5])
    r["extra"] = [(hex(it[0]), it[1]) for it in its[4:]]
    # compression info: SEQUENCE{INTEGER algo, INTEGER size}
    for it in its[4:]:
        if it[0] == 0x30:
            sub = items(b, it[2], it[2] + it[1])
            r["compression"] = [int.from_bytes(b[x[2]:x[2]+x[1]], "big") for x in sub if x[0] == 0x02]
    r["payload_head"] = b[its[3][2]:its[3][2]+16].hex()
    return r
print("---- IM4P structure ----")
for n in z.namelist():
    if n.endswith((".im4p", ".dmg", ".trustcache")) and not n.endswith("/"):
        try:
            b = z.read(n)
            print(n, im4p_info(b))
        except Exception as e:
            print(n, "parse error", e)

# ---- 0.4.123: BuildManifest excerpt around RestoreRamDisk / the first Path after it (binary <data> blobs removed)
import re
mf = z.read("BuildManifest.plist").decode("utf8", "replace")
mf = re.sub(r"<data>[^<]*</data>", "<data/>", mf)
for key in ("RestoreRamDisk",):
    for mm in list(re.finditer("<key>%s</key>" % key, mf))[:3]:
        print("---- manifest excerpt at", mm.start(), "of", len(mf))
        print(re.sub(r"\s+", " ", mf[mm.start() - 200: mm.start() + 900]))
print("---- all Path strings that mention .dmg:", sorted(set(re.findall(r"<string>([^<]*\.dmg)</string>", mf))))
print("---- BuildIdentities:", mf.count("<key>BuildIdentities</key>"), "RestoreRamDisk keys:", mf.count("<key>RestoreRamDisk</key>"), "UpdateRamDisk keys:", mf.count("<key>UpdateRamDisk</key>"))
