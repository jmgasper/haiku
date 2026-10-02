#!/usr/bin/env python3
"""Run a program in a pseudo-terminal for N seconds and print what it drew.

usage: pty-run.py <seconds> <program> [args...]
Used to check that an interactive TUI (claude) starts; sends Ctrl-C twice at
the end and kills it if it is still running. PTY_KEYS="8:\r" sends Enter
after 8 seconds (several comma-separated "time:keys" entries allowed).
"""
import os, pty, re, select, signal, struct, sys, time, fcntl, termios

secs = float(sys.argv[1])
pid, fd = pty.fork()
if pid == 0:
    os.execvp(sys.argv[2], sys.argv[2:])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
out = b""
start = time.time()
end = start + secs
keys = []
for item in filter(None, os.environ.get("PTY_KEYS", "").split(",")):
    t, k = item.split(":", 1)
    keys.append([start + float(t), k.encode().decode("unicode_escape").encode()])
sent = 0
while True:
    now = time.time()
    for k in keys:
        if k[1] and now >= k[0]:
            os.write(fd, k[1])
            k[1] = b""
    if now > end and sent < 2:
        os.write(fd, b"\x03")
        sent += 1
        end = now + 1.5
    elif now > end:
        break
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try:
            data = os.read(fd, 65536)
        except OSError:
            break
        if not data:
            break
        out += data
try:
    os.kill(pid, signal.SIGKILL)
except ProcessLookupError:
    pass
text = out.decode("utf-8", "replace")
text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07]*\x07|\x1b[()][A-Z0-9]|\r", "", text)
lines = [l.rstrip() for l in text.split("\n") if l.strip()]
print("\n".join(lines[-60:]))
print("-- %d bytes of terminal output" % len(out))
