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
