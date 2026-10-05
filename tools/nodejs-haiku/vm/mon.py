#!/usr/bin/env python3
# Send one HMP command to the VM monitor; print only the command's output.
import socket, sys, time, os, re
vm = os.path.dirname(os.path.abspath(__file__))
s = socket.socket(socket.AF_UNIX); s.connect(vm + "/monitor.sock")
time.sleep(0.3); s.recv(65536)
s.sendall((" ".join(sys.argv[1:]) + "\n").encode()); time.sleep(1.0)
s.setblocking(False)
try: out = s.recv(1 << 20).decode(errors="replace")
except BlockingIOError: out = ""
out = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", out)
lines = out.splitlines()[1:]
print("\n".join(l for l in lines if not l.startswith("(qemu)")))
