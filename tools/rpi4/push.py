#!/usr/bin/env python3
"""Copy a file to the lab image through its telnet login (base64 over the
terminal), and check its sha256 there.

    push.py <address> <local file> <remote path>
"""

import base64
import hashlib
import json
import os
import secrets
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shell


def main():
    host, local, remote = sys.argv[1:4]
    with open(local, "rb") as stream:
        data = stream.read()
    with open(os.path.join(shell.STATE, "lab-overlay", "credentials.json")) as stream:
        credentials = json.load(stream)

    client = shell.Telnet(host, 10)
    client.read_until(rb"login: ?", 20)
    client.write(credentials["username"] + "\r\n")
    client.read_until(rb"[Pp]assword: ?", 20)
    client.write(credentials["password"] + "\r\n")

    marker = "RPI4_" + secrets.token_hex(8)
    client.write(f"stty -echo; echo {marker}_READY\r\n")
    client.read_until(marker.encode() + rb"_READY\r?\n", 30)
    client.write(f"base64 -d > '{remote}.new' <<'{marker}'\r\n")

    encoded = base64.encodebytes(data).decode()
    lines = encoded.splitlines()
    for start in range(0, len(lines), 16):
        client.write("\n".join(lines[start:start + 16]) + "\n")
        time.sleep(0.004)
    client.write(f"{marker}\r\n")
    client.write(f"sha256sum '{remote}.new'; echo {marker}_DONE\r\n")
    output, _ = client.read_until(marker.encode() + rb"_DONE", 300)

    expected = hashlib.sha256(data).hexdigest()
    if expected.encode() not in output:
        sys.exit("checksum differs: " + output.decode(errors="replace")[-300:])
    client.write(f"mv '{remote}.new' '{remote}' && sync; echo {marker}_MOVED $?\r\n")
    _, match = client.read_until(marker.encode() + rb"_MOVED (\d+)", 60)
    client.write("exit\r\n")
    print(f"{remote}: {len(data)} bytes, sha256 {expected}")
    sys.exit(int(match.group(1)))


if __name__ == "__main__":
    main()
