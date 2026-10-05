#!/usr/bin/env python3
# Type text into the VM through the QEMU monitor (sendkey), US layout.
import socket, sys, time, os
vm = os.path.dirname(os.path.abspath(__file__))
s = socket.socket(socket.AF_UNIX); s.connect(vm + "/monitor.sock")
time.sleep(0.3); s.recv(65536)
plain = {' ': 'spc', '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right',
         '\\': 'backslash', ';': 'semicolon', "'": 'apostrophe', '`': 'grave_accent',
         ',': 'comma', '.': 'dot', '/': 'slash', '\n': 'ret'}
shifted = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
           '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left',
           '}': 'bracket_right', '|': 'backslash', ':': 'semicolon', '"': 'apostrophe',
           '~': 'grave_accent', '<': 'comma', '>': 'dot', '?': 'slash'}
def key(k):
    s.sendall(("sendkey %s\n" % k).encode()); time.sleep(0.04)
text = " ".join(sys.argv[1:]) if sys.argv[1:] != ['-'] else sys.stdin.read()
for ch in text:
    if ch.isalpha() and ch.isupper(): key("shift-" + ch.lower())
    elif ch.isalnum(): key(ch)
    elif ch in plain: key(plain[ch])
    elif ch in shifted: key("shift-" + shifted[ch])
    else: raise SystemExit("cannot type %r" % ch)
time.sleep(0.3)
