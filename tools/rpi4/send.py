#!/usr/bin/env python3
"""Copy a file to the lab image quickly: serve it on a TCP port and have
rpi4_fetch (tools/rpi4/fetch.cpp, in the lab image's non-packaged bin) pull
it; the sha256 is checked on the board.

    send.py <address> <local file> <remote path>
"""

import hashlib
import os
import socket
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    host, local, remote = sys.argv[1:4]
    with open(local, "rb") as stream:
        data = stream.read()
    expected = hashlib.sha256(data).hexdigest()

    # the address of this host as the board sees it
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect((host, 23))
    own = probe.getsockname()[0]
    probe.close()

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((own, 0))
    listener.listen(1)
    listener.settimeout(60)
    port = listener.getsockname()[1]

    def serve():
        # The board may come in from another of its addresses (it has
        # Ethernet and Wi-Fi on one network); the checksum is what counts.
        connection, peer = listener.accept()
        connection.sendall(data)
        connection.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()

    fetch = os.environ.get("RPI4_FETCH", "rpi4_fetch")
    command = (f"{fetch} {own} {port} '{remote}.new' && "
        f"sha256sum '{remote}.new' | cut -c1-64")
    result = subprocess.run([os.path.join(HERE, "shell.py"), host, command, "600"],
        capture_output=True, text=True)
    thread.join(5)
    if expected not in result.stdout:
        sys.exit("transfer failed: " + result.stdout[-300:] + result.stderr[-300:])
    subprocess.run([os.path.join(HERE, "shell.py"), host,
        f"mv '{remote}.new' '{remote}'"], check=True)
    print(f"{remote}: {len(data)} bytes, sha256 {expected}")


if __name__ == "__main__":
    main()
