#!/usr/bin/env python3
"""Finds the iPhone XS Max (iPhone11,4 = D331AP) iOS 12.x IPSW whose kernelcache member has the given compressed size
(reads only the ZIP central directory with HTTP range requests).  Prints the URL."""
import json, struct, sys, urllib.request
dev, want = sys.argv[1], int(sys.argv[2])
def get(url, rng=None):
    rq = urllib.request.Request(url, headers={"User-Agent": "xnuron-ci"} | ({"Range": rng} if rng else {}))
    return urllib.request.urlopen(rq, timeout=60)
fw = json.load(get(f"https://api.ipsw.me/v4/device/{dev}?type=ipsw"))["firmwares"]
fw = [x for x in fw if x["version"].startswith("12.")]
print(f"{len(fw)} iOS 12.x firmwares for {dev}", file=sys.stderr)
for x in fw:
    url, size = x["url"], int(x["filesize"])
    try:
        tail = get(url, f"bytes={size-70000}-{size-1}").read()
        i = tail.rfind(b"PK\x05\x06"); cd_size, cd_off = struct.unpack("<II", tail[i+12:i+20])
        cd = get(url, f"bytes={cd_off}-{cd_off+cd_size-1}").read() if cd_off != 0xFFFFFFFF else b""
        p = 0
        while p + 46 <= len(cd) and cd[p:p+4] == b"PK\x01\x02":
            csz = struct.unpack("<I", cd[p+20:p+24])[0]; nl, el, cl = struct.unpack("<HHH", cd[p+28:p+34])
            nm = cd[p+46:p+46+nl].decode(errors="replace")
            if nm.startswith("kernelcache.release.iphone11"):
                print(f"{x['version']} {x['buildid']}: {nm} compressed {csz}", file=sys.stderr)
                if csz == want: print(url); sys.exit(0)
            p += 46 + nl + el + cl
    except Exception as e:
        print(f"{x['version']}: {e}", file=sys.stderr)
sys.exit("no firmware with that kernelcache size")
