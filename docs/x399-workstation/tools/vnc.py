#!/usr/bin/env python3
"""Headless RFB client for the X399 workstation's vncserver.

Enough of RFB 3.8 to grab the framebuffer as a PNG and to inject key and
pointer events. Raw encoding only -- the link is gigabit LAN and the server
is AGMS' VNC 4.0 port, so there is no reason to carry a codec zoo.

  vnc.py shot out.png [--scale 0.25] [--rect X,Y,W,H]
  vnc.py move X Y
  vnc.py click X Y [--button 1] [--double]
  vnc.py drag X0 Y0 X1 Y1 [--button 1]
  vnc.py type "some text"
  vnc.py key Return [Escape ...]
  vnc.py info
"""

import argparse
import socket
import struct
import sys
import time

HOST = "192.168.1.244"
PORT = 5900
PASSWORD = "ghostar1"

# X11 keysyms for the names we actually reach for.
KEYSYMS = {
    "BackSpace": 0xFF08, "Tab": 0xFF09, "Return": 0xFF0D, "Enter": 0xFF0D,
    "Escape": 0xFF1B, "Esc": 0xFF1B, "Insert": 0xFF63, "Delete": 0xFFFF,
    "Home": 0xFF50, "End": 0xFF57, "PageUp": 0xFF55, "PageDown": 0xFF56,
    "Left": 0xFF51, "Up": 0xFF52, "Right": 0xFF53, "Down": 0xFF54,
    "F1": 0xFFBE, "F2": 0xFFBF, "F3": 0xFFC0, "F4": 0xFFC1, "F5": 0xFFC2,
    "F6": 0xFFC3, "F7": 0xFFC4, "F8": 0xFFC5, "F9": 0xFFC6, "F10": 0xFFC7,
    "F11": 0xFFC8, "F12": 0xFFC9,
    "Shift": 0xFFE1, "Control": 0xFFE3, "Ctrl": 0xFFE3, "Alt": 0xFFE9,
    "Command": 0xFFE9, "Super": 0xFFEB, "Space": 0x0020,
}


def des_response(password, challenge):
    """VncAuth: DES-ECB the 16-byte challenge with the bit-reversed password."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    key = password.encode("latin-1")[:8].ljust(8, b"\0")
    # VNC feeds the key to DES least-significant-bit first.
    key = bytes(int(f"{b:08b}"[::-1], 2) for b in key)
    enc = Cipher(algorithms.TripleDES(key), modes.ECB()).encryptor()
    return enc.update(challenge) + enc.finalize()


class VNC:
    def __init__(self, host=HOST, port=PORT, password=PASSWORD, timeout=120):
        self.sock = socket.create_connection((host, port), timeout=30)
        self.sock.settimeout(timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._handshake(password)

    # -- plumbing ---------------------------------------------------------
    def recv(self, n):
        buf = bytearray(n)
        view = memoryview(buf)
        got = 0
        while got < n:
            k = self.sock.recv_into(view[got:], n - got)
            if not k:
                raise EOFError(f"server closed after {got}/{n} bytes")
            got += k
        return bytes(buf)

    def send(self, data):
        self.sock.sendall(data)

    def _handshake(self, password):
        version = self.recv(12)
        if not version.startswith(b"RFB "):
            raise RuntimeError(f"not an RFB server: {version!r}")
        self.send(b"RFB 003.008\n")

        count = self.recv(1)[0]
        if count == 0:
            reason = self.recv(struct.unpack(">I", self.recv(4))[0])
            raise RuntimeError(f"server refused: {reason.decode(errors='replace')}")
        types = self.recv(count)
        if 2 in types:
            self.send(b"\x02")
            self.send(des_response(password, self.recv(16)))
        elif 1 in types:
            # No authentication (a local QEMU display, say); RFB 3.8 still
            # sends a security result.
            self.send(b"\x01")
        else:
            raise RuntimeError(f"no VncAuth offered, got {list(types)}")
        if struct.unpack(">I", self.recv(4))[0] != 0:
            reason = self.recv(struct.unpack(">I", self.recv(4))[0])
            raise RuntimeError(f"auth failed: {reason.decode(errors='replace')}")

        self.send(b"\x01")  # ClientInit, shared
        self.width, self.height = struct.unpack(">HH", self.recv(4))
        self.recv(16)  # server pixel format; we override it below
        self.name = self.recv(struct.unpack(">I", self.recv(4))[0]).decode(
            "latin-1", errors="replace")

        # 32bpp little-endian BGRX, which is what PIL's "BGRX" wants.
        self.send(struct.pack(">BxxxBBBBHHHBBBxxx", 0,
                              32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
        self.send(struct.pack(">BxHi", 2, 1, 0))  # SetEncodings: Raw only

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    # -- framebuffer ------------------------------------------------------
    def capture(self, rect=None):
        """Non-incremental update for rect (x, y, w, h); returns (PIL image)."""
        from PIL import Image

        x, y, w, h = rect or (0, 0, self.width, self.height)
        self.send(struct.pack(">BBHHHH", 3, 0, x, y, w, h))

        image = Image.new("RGB", (w, h))
        while True:
            msg = self.recv(1)[0]
            if msg == 0:
                break
            elif msg == 1:  # SetColourMapEntries
                _, _, n = struct.unpack(">xHH", self.recv(5))
                self.recv(n * 6)
            elif msg == 2:  # Bell
                pass
            elif msg == 3:  # ServerCutText
                self.recv(struct.unpack(">I", self.recv(7)[3:])[0])
            else:
                raise RuntimeError(f"unexpected server message {msg}")

        (nrects,) = struct.unpack(">xH", self.recv(3))
        for _ in range(nrects):
            rx, ry, rw, rh, enc = struct.unpack(">HHHHi", self.recv(12))
            if enc != 0:
                raise RuntimeError(f"server used encoding {enc}, expected Raw")
            if rw and rh:
                tile = Image.frombytes("RGB", (rw, rh), self.recv(rw * rh * 4),
                                       "raw", "BGRX")
                image.paste(tile, (rx - x, ry - y))
        return image

    # -- input ------------------------------------------------------------
    def pointer(self, x, y, mask=0):
        self.send(struct.pack(">BBHH", 5, mask, x, y))

    def click(self, x, y, button=1, double=False):
        bit = 1 << (button - 1)
        self.pointer(x, y)
        time.sleep(0.05)
        for _ in range(2 if double else 1):
            self.pointer(x, y, bit)
            time.sleep(0.05)
            self.pointer(x, y, 0)
            time.sleep(0.05)

    def drag(self, x0, y0, x1, y1, button=1, steps=12):
        bit = 1 << (button - 1)
        self.pointer(x0, y0)
        time.sleep(0.05)
        self.pointer(x0, y0, bit)
        time.sleep(0.1)
        for i in range(1, steps + 1):
            x = x0 + (x1 - x0) * i // steps
            y = y0 + (y1 - y0) * i // steps
            self.pointer(x, y, bit)
            time.sleep(0.03)
        time.sleep(0.1)
        self.pointer(x1, y1, 0)
        time.sleep(0.05)

    def key(self, keysym, down=None):
        if down is None:
            self.key(keysym, True)
            time.sleep(0.02)
            self.key(keysym, False)
            return
        self.send(struct.pack(">BBxxI", 4, 1 if down else 0, keysym))

    def type_text(self, text):
        for ch in text:
            self.key(0xFF0D if ch == "\n" else ord(ch))
            time.sleep(0.02)


def keysym(name):
    if name in KEYSYMS:
        return KEYSYMS[name]
    if len(name) == 1:
        return ord(name)
    if name.lower().startswith("0x"):
        return int(name, 16)
    raise SystemExit(f"unknown key name: {name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=HOST)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--password", default=PASSWORD)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("shot", help="save the screen as a PNG")
    p.add_argument("out")
    p.add_argument("--scale", type=float, default=1.0)
    p.add_argument("--rect", help="X,Y,W,H")

    p = sub.add_parser("move")
    p.add_argument("x", type=int)
    p.add_argument("y", type=int)

    p = sub.add_parser("click")
    p.add_argument("x", type=int)
    p.add_argument("y", type=int)
    p.add_argument("--button", type=int, default=1)
    p.add_argument("--double", action="store_true")

    p = sub.add_parser("drag", help="press at X0 Y0, move to X1 Y1, release")
    p.add_argument("x0", type=int)
    p.add_argument("y0", type=int)
    p.add_argument("x1", type=int)
    p.add_argument("y1", type=int)
    p.add_argument("--button", type=int, default=1)

    p = sub.add_parser("type")
    p.add_argument("text")

    p = sub.add_parser("key")
    p.add_argument("names", nargs="+")

    sub.add_parser("info")

    args = ap.parse_args()
    vnc = VNC(args.host, args.port, args.password)
    try:
        if args.cmd == "info":
            print(f"{vnc.name}: {vnc.width}x{vnc.height}")
        elif args.cmd == "shot":
            rect = tuple(int(v) for v in args.rect.split(",")) if args.rect else None
            image = vnc.capture(rect)
            if args.scale != 1.0:
                size = (max(1, int(image.width * args.scale)),
                        max(1, int(image.height * args.scale)))
                from PIL import Image
                image = image.resize(size, Image.LANCZOS)
            image.save(args.out)
            print(f"{args.out}: {image.width}x{image.height}")
        elif args.cmd == "move":
            vnc.pointer(args.x, args.y)
        elif args.cmd == "click":
            vnc.click(args.x, args.y, args.button, args.double)
        elif args.cmd == "drag":
            vnc.drag(args.x0, args.y0, args.x1, args.y1, args.button)
        elif args.cmd == "type":
            vnc.type_text(args.text)
        elif args.cmd == "key":
            for name in args.names:
                vnc.key(keysym(name))
        time.sleep(0.2)  # let queued input reach the server before FIN
    finally:
        vnc.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
