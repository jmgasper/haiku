#!/usr/bin/env python3
"""Apply an ordered patch series, recognizing an already applied prefix.

Later patches can change the context of earlier ones. Validate the whole
series on copies of its affected files before touching the source tree.
Temporary files stay alongside the source, on the project filesystem.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def run_patch(source, patch, reverse=False):
    command = ["patch", "--batch", "--forward", "-p1", "-d", str(source)]
    if reverse:
        command.append("--reverse")
    return subprocess.run(command, input=patch.read_bytes(),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def affected_files(patches):
    files = set()
    for patch in patches:
        for line in patch.read_text().splitlines():
            if not line.startswith(("--- ", "+++ ")):
                continue
            name = line[4:].split("\t", 1)[0]
            if name == "/dev/null":
                continue
            if not name.startswith(("a/", "b/")):
                raise ValueError(f"Unexpected patch path: {name}")
            path = Path(name[2:])
            if path.is_absolute() or ".." in path.parts:
                raise ValueError(f"Unsafe patch path: {name}")
            files.add(path)
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("patches", type=Path, nargs="+")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    patches = [p.resolve(strict=True) for p in args.patches]
    files = affected_files(patches)
    with tempfile.TemporaryDirectory(prefix="mesa-patch-check-",
            dir=source.parent) as temporary:
        for prefix in range(len(patches), -1, -1):
            trial = Path(temporary) / str(prefix)
            trial.mkdir()
            for path in files:
                if (source / path).exists():
                    (trial / path).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source / path, trial / path)
            if any(run_patch(trial, p, True).returncode
                    for p in reversed(patches[:prefix])):
                continue
            if any(run_patch(trial, p).returncode for p in patches):
                continue
            for patch in patches[prefix:]:
                result = run_patch(source, patch)
                print(result.stdout.decode(errors="replace"), end="")
                if result.returncode:
                    raise SystemExit("Source changed after patch validation")
            print(f"Patch series ready: {prefix} already applied, "
                  f"{len(patches) - prefix} added")
            return
    raise SystemExit("Cannot recognize or apply the patch series; "
        "inspect the pinned source tree. No source files were changed.")


if __name__ == "__main__":
    main()
