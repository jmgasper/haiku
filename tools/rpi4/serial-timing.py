#!/usr/bin/env python3
"""Capture serial with host monotonic timestamps for comparing boot stages.

Usage: serial-timing.py SECONDS OUTPUT_PREFIX
Start before rebooting. OUTPUT_PREFIX.log is the unmodified byte stream;
OUTPUT_PREFIX.jsonl records each line's receipt time relative to capture
start. Compare markers within a capture; SSH/UART buffering means these
are observations, not cycle-accurate target timestamps. Hold the lab's
hardware lock separately. The serial lock excludes other timing captures.
"""

import fcntl
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time


def main():
    seconds = int(sys.argv[1])
    if not 1 <= seconds <= 3600:
        raise ValueError("capture duration must be 1..3600 seconds")
    prefix = Path(sys.argv[2])
    prefix.parent.mkdir(parents=True, exist_ok=True)
    state = Path(os.environ.get("RPI4_STATE", "/mnt/HaikuWork/rpi4/state"))
    tty = shlex.quote(os.environ.get("RPI4_KVM_TTY", "/dev/ttyS1"))
    config = os.environ.get("NANOKVM_SSH_CONFIG",
        "/mnt/HaikuWork/nanokvm/.ssh/config")
    with (state / "serial.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        command = ["ssh", "-F", config, "nanokvm",
            f"stty -F {tty} 115200 raw -echo -crtscts || exit; "
            f"cat {tty} & reader=$!; "
            f"(sleep {seconds}; kill $reader 2>/dev/null) & timer=$!; "
            "trap 'kill $reader $timer 2>/dev/null' EXIT; wait $reader"]
        start = time.monotonic()
        with prefix.with_suffix(".log").open("wb") as raw, \
                prefix.with_suffix(".jsonl").open("w") as timed:
            timed.write(json.dumps({"event": "capture_start", "elapsed": 0,
                "unix_time": time.time()}) + "\n")
            with subprocess.Popen(command, stdout=subprocess.PIPE) as process:
                pending = b""
                while True:
                    chunk = process.stdout.read1(65536)
                    if not chunk:
                        break
                    elapsed = time.monotonic() - start
                    raw.write(chunk)
                    raw.flush()
                    pending += chunk
                    while b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        timed.write(json.dumps({"elapsed": elapsed,
                            "line": line.decode(errors="replace").rstrip("\r")}) + "\n")
                    timed.flush()
                if pending:
                    timed.write(json.dumps({"elapsed": time.monotonic() - start,
                        "line": pending.decode(errors="replace")}) + "\n")
                result = process.wait()
                if result not in (0, 124, 143):
                    raise RuntimeError(f"serial capture exited with {result}")
    print(str(prefix))


if __name__ == "__main__":
    main()
