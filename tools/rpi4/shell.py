#!/usr/bin/env python3
"""Run a command on the lab image through its telnet login.

    shell.py <address> '<command>' [timeout seconds]

The account comes from $RPI4_STATE/lab-overlay/credentials.json (the lab
overlay of tools/rpi4/UserBuildConfig). Output is printed; the exit status is
the command's.
"""

import json
import os
import re
import secrets
import socket
import sys
import time

STATE = os.environ.get("RPI4_STATE", "/mnt/HaikuWork/rpi4/state")
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240


class Telnet:
    def __init__(self, host, timeout):
        self.socket = socket.create_connection((host, 23), timeout)
        self.buffer = b""

    def _negotiate(self, data):
        """Refuse every option; return the plain text."""
        text = bytearray()
        i = 0
        while i < len(data):
            byte = data[i]
            if byte != IAC:
                text.append(byte)
                i += 1
            elif i + 1 >= len(data):
                break
            elif data[i + 1] in (DO, DONT, WILL, WONT):
                if i + 2 >= len(data):
                    break
                reply = WONT if data[i + 1] in (DO, DONT) else DONT
                self.socket.sendall(bytes([IAC, reply, data[i + 2]]))
                i += 3
            elif data[i + 1] == SB:
                end = data.find(bytes([IAC, SE]), i)
                if end < 0:
                    break
                i = end + 2
            else:
                i += 2
        return bytes(text)

    def read_until(self, pattern, timeout):
        deadline = time.monotonic() + timeout
        while True:
            match = re.search(pattern, self.buffer)
            if match:
                before = self.buffer[:match.start()]
                self.buffer = self.buffer[match.end():]
                return before, match
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"no {pattern!r} in {self.buffer[-200:]!r}")
            self.socket.settimeout(remaining)
            try:
                data = self.socket.recv(65536)
            except socket.timeout:
                continue
            if not data:
                raise ConnectionError("connection closed")
            self.buffer += self._negotiate(data)

    def write(self, text):
        self.socket.sendall(text.encode())


def main():
    host, command = sys.argv[1], sys.argv[2]
    timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 60
    with open(os.path.join(STATE, "lab-overlay", "credentials.json")) as stream:
        credentials = json.load(stream)

    client = Telnet(host, 10)
    client.read_until(rb"login: ?", 20)
    client.write(credentials["username"] + "\r\n")
    client.read_until(rb"[Pp]assword: ?", 20)
    client.write(credentials["password"] + "\r\n")

    marker = "RPI4_" + secrets.token_hex(8)
    client.write(f"stty -echo; echo {marker}_BEGIN; ( {command} ); "
        f"echo {marker}_END $?\r\n")
    client.read_until(marker.encode() + rb"_BEGIN\r?\n", 30)
    output, match = client.read_until(
        marker.encode() + rb"_END (\d+)", timeout)
    sys.stdout.write(output.decode(errors="replace").replace("\r\n", "\n"))
    client.write("exit\r\n")
    sys.exit(int(match.group(1)))


if __name__ == "__main__":
    main()
