import sys, zipfile
# synthetic IPSW-like archive (empty placeholder members) with zip64 entries
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_STORED) as z:
    for n in ["BuildManifest.plist", "Restore.plist", "kernelcache.release.fake", "Firmware/dfu/iBSS.fake.im4p",
              "Firmware/all_flash/DeviceTree.fake.im4p", "018-00000-001.dmg", "readme.txt"]:
        zi = zipfile.ZipInfo(n); 
        with z.open(zi, "w", force_zip64=True) as f: f.write(b"x" * 10)
with zipfile.ZipFile(sys.argv[2], "w") as z:
    z.writestr("a.txt", "hello")

# old-style (iPhone OS 2.x era) IPSW: no BuildManifest.plist, 32-bit S5L8900 target
with zipfile.ZipFile(sys.argv[3], "w") as zf:
    for n in ["Restore.plist", "kernelcache.release.s5l8900xrb", "Firmware/dfu/WTF.s5l8900xall.RELEASE.dfu", "018-4455-002.dmg"]:
        zf.writestr(n, "x" * 10)
