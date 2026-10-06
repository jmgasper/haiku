#!/usr/bin/env python3
"""Check that reducing WebKit's exports preserves symbols used by consumers.

check-exports.py [--provider LIBRARY] ORIGINAL CANDIDATE CONSUMER [...]
Consumers should include Summit, WebProcess, NetworkProcess and Natter.
Providers are explicitly named dependencies that supply common symbols
(e.g. libstdc++ supplies operator new/delete). This checks dynamic-symbol
coverage; it does not replace runtime tests or validate symbol versions.
"""
import argparse
import subprocess
import sys


def symbols(path):
    output = subprocess.check_output(
        ["readelf", "--dyn-syms", "--wide", path], text=True)
    defined, undefined = set(), set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].rstrip(":").isdigit():
            continue
        if fields[4] not in ("GLOBAL", "WEAK"):
            continue
        name = fields[7].split("@")[0]
        (undefined if fields[6] == "UND" else defined).add(name)
    return defined, undefined


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provider", action="append", default=[])
    parser.add_argument("original")
    parser.add_argument("candidate")
    parser.add_argument("consumers", nargs="+")
    args = parser.parse_args()
    original, _ = symbols(args.original)
    candidate, _ = symbols(args.candidate)
    providers = {path: symbols(path)[0] for path in args.provider}
    failures = 0
    print(f"Dynamic exports: {len(original)} -> {len(candidate)}")
    for path in args.consumers:
        _, undefined = symbols(path)
        required = undefined & original
        missing = required - candidate
        for provider, supplied in providers.items():
            resolved = missing & supplied
            if resolved:
                print(f"  {path}: {', '.join(sorted(resolved))} "
                      f"supplied by {provider}")
                missing -= resolved
        print(f"{'FAIL' if missing else 'PASS'} {path}: "
              f"{len(required)} WebKit symbols, {len(missing)} missing")
        for name in sorted(missing):
            print(f"  {name}")
        failures += len(missing)
    return int(failures != 0)


if __name__ == "__main__":
    sys.exit(main())
