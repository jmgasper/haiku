#!/usr/bin/env python3
"""Resolve HaikuPorts packages needed for a set of requirements.

usage: hpkg-resolve.py <repo-verbose.txt> <installed.txt> <package|requirement> ...
Prints package file names (name-version-arch.hpkg) to install.
"""
import re, sys

def parse(path):
    pkgs, cur = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*(name|version|provides|requires|architecture): (.*)", line)
        if not m:
            continue
        key, val = m.group(1), m.group(2).strip()
        if key == "name":
            cur = {"name": val, "provides": [], "requires": [], "version": "", "arch": ""}
            pkgs.append(cur)
        elif cur is None:
            continue
        elif key == "version":
            cur["version"] = val
        elif key == "architecture":
            cur["arch"] = val
        else:
            cur[key].append(val)
    return pkgs

def vkey(v):
    return [int(x) if x.isdigit() else x for x in re.split(r"[._~-]", v or "0")]

def cmp(a, b):
    ka, kb = vkey(a), vkey(b)
    for x, y in zip(ka, kb):
        if x == y: continue
        if isinstance(x, int) and isinstance(y, int): return (x > y) - (x < y)
        return (str(x) > str(y)) - (str(x) < str(y))
    return (len(ka) > len(kb)) - (len(ka) < len(kb))

def split_req(r):
    m = re.match(r"([^<>=!\s]+)\s*(>=|<=|==|!=|<|>|=)?\s*([^\s]*)(?:\s+compat\s*>=\s*(\S+))?", r)
    return m.group(1), m.group(2), m.group(3)

def split_prov(p):
    m = re.match(r"([^=\s]+)\s*(?:=\s*(\S+))?(?:\s+compat\s*>=\s*(\S+))?", p)
    return m.group(1), m.group(2), m.group(3)

def satisfies(prov, req):
    pn, pv, pcompat = split_prov(prov)
    rn, op, rv = split_req(req)
    if pn != rn: return False
    if not op: return True
    if pv is None: return False
    c = cmp(pv, rv)
    ok = {"==": c == 0, "=": c == 0, ">=": c >= 0, "<=": c <= 0, ">": c > 0, "<": c < 0, "!=": c != 0}[op]
    if not ok and op in (">=", ">") and pcompat and cmp(rv, pcompat) >= 0 and c <= 0:
        return True
    return ok

repo = parse(sys.argv[1])
installed = parse(sys.argv[2])
wanted = sys.argv[3:]
chosen, queue = {}, list(wanted)
def provided(req, pool):
    return [p for p in pool if any(satisfies(pr, req) for pr in p["provides"])]
while queue:
    req = queue.pop()
    if provided(req, installed) or provided(req, chosen.values()):
        continue
    cands = provided(req, repo) or [p for p in repo if p["name"] == req]
    cands = [c for c in cands if c["arch"] in ("x86_64", "any", "source") and not c["name"].endswith(("_source", "_debuginfo"))]
    if not cands:
        print("UNSATISFIED: " + req, file=sys.stderr)
        continue
    best = sorted(cands, key=lambda p: vkey(p["version"]))[-1]
    chosen[best["name"]] = best
    queue.extend(best["requires"])
for p in sorted(chosen.values(), key=lambda p: p["name"]):
    print("%s-%s-%s.hpkg" % (p["name"], p["version"], p["arch"]))
