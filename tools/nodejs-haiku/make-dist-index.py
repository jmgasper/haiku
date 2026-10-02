#!/usr/bin/env python3
"""Write SHASUMS256.txt, index.json and index.tab for a Haiku Node.js mirror.

usage: make-dist-index.py <dist-dir> [upstream-index.json]

<dist-dir> has the nodejs.org layout: v<version>/node-v<version>-haiku-x64.tar.{xz,gz}.
Each release's metadata (date, npm, v8, uv, zlib, openssl, modules, lts,
security) is copied from the upstream nodejs.org index.json (fetched when not
given) and its "files" list is replaced by what the mirror actually holds.
"""
import hashlib, json, os, re, sys, urllib.request

dist = sys.argv[1]
if len(sys.argv) > 2:
    upstream = json.load(open(sys.argv[2]))
else:
    upstream = json.load(urllib.request.urlopen("https://nodejs.org/dist/index.json"))
meta = {e["version"]: e for e in upstream}

entries = []
for d in sorted(os.listdir(dist)):
    if not re.fullmatch(r"v\d+\.\d+\.\d+", d):
        continue
    path = os.path.join(dist, d)
    files = sorted(f for f in os.listdir(path) if f.startswith("node-") and f.endswith((".tar.xz", ".tar.gz")))
    if not files:
        continue
    with open(os.path.join(path, "SHASUMS256.txt"), "w") as out:
        for f in files:
            h = hashlib.sha256()
            with open(os.path.join(path, f), "rb") as fp:
                for chunk in iter(lambda: fp.read(1 << 20), b""):
                    h.update(chunk)
            out.write("%s  %s\n" % (h.hexdigest(), f))
    e = dict(meta.get(d, {"version": d, "date": "", "npm": "", "v8": "", "uv": "", "zlib": "",
                           "openssl": "", "modules": "", "lts": False, "security": False}))
    e["files"] = sorted({re.sub(r"^node-v[\d.]+-(.*)\.tar\.(xz|gz)$", r"\1", f) for f in files})
    entries.append(e)

def vkey(e):
    return [int(x) for x in e["version"][1:].split(".")]
entries.sort(key=vkey, reverse=True)
json.dump(entries, open(os.path.join(dist, "index.json"), "w"), indent=0)

cols = ["version", "date", "files", "npm", "v8", "uv", "zlib", "openssl", "modules", "lts", "security"]
def cell(e, c):
    v = e.get(c, "")
    if c == "files":
        return ",".join(v)
    if isinstance(v, bool):
        return "true" if v and c == "security" else ("-" if not v else str(v))
    return str(v) if v not in ("", None) else "-"
with open(os.path.join(dist, "index.tab"), "w") as out:
    out.write("\t".join(cols) + "\n")
    for e in entries:
        out.write("\t".join(cell(e, c) for c in cols) + "\n")
print("%d releases indexed in %s" % (len(entries), dist))
