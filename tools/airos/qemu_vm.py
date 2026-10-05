#!/usr/bin/env python3
"""Interactive arm64 QEMU for testing the air/OS installer ISO.

  qemu_vm.py start DIR --iso ISO [--cdrom] [--disk NAME:SIZE_OR_FILE ...]
  qemu_vm.py shot DIR [NAME]          screendump -> DIR/NAME.png (default: shot-<n>)
  qemu_vm.py click DIR X Y [--double] (screen pixels)
  qemu_vm.py move DIR X Y
  qemu_vm.py key DIR QCODE[+QCODE...] [...]
  qemu_vm.py type DIR TEXT
  qemu_vm.py stop DIR

Disks are attached as NVMe controllers (serial airos-<name>); a SIZE such as
8G creates a blank raw file DIR/<name>.img, an existing path is used as is.
"""
import argparse, json, os, socket, subprocess, sys, time

W, H = 1024, 768


def qmp(dir_, *commands):
	sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	sock.connect(os.path.join(dir_, 'qmp.sock'))
	f = sock.makefile('rwb')
	f.readline()
	results = []
	for name, args in (('qmp_capabilities', {}),) + commands:
		f.write((json.dumps({'execute': name, 'arguments': args}) + '\n').encode())
		f.flush()
		while True:
			r = json.loads(f.readline())
			if 'return' in r or 'error' in r:
				results.append(r)
				break
	sock.close()
	for r in results:
		if 'error' in r:
			raise SystemExit(json.dumps(r))
	return results[1:]


def abs_event(x, y):
	return [{'type': 'abs', 'data': {'axis': 'x', 'value': x * 32767 // (W - 1)}},
		{'type': 'abs', 'data': {'axis': 'y', 'value': y * 32767 // (H - 1)}}]


def start(a):
	os.makedirs(a.dir, exist_ok=True)
	fw = os.path.join(a.dir, 'QEMU_EFI.fd')
	if not os.path.exists(fw):
		subprocess.check_call(['cp', '/usr/share/qemu-efi-aarch64/QEMU_EFI.fd', fw])
	cmd = ['qemu-system-aarch64', '-M', 'virt', '-cpu', 'max', '-m', str(a.memory),
		'-smp', '4', '-bios', fw, '-device', 'qemu-xhci,id=usb',
		'-device', 'usb-kbd,bus=usb.0', '-device', 'usb-tablet,bus=usb.0',
		'-device', 'ramfb', '-display', 'none', '-monitor', 'none', '-nic', 'none',
		'-serial', 'file:%s' % os.path.join(a.dir, 'serial.log'),
		'-qmp', 'unix:%s,server=on,wait=off' % os.path.join(a.dir, 'qmp.sock'),
		'-pidfile', os.path.join(a.dir, 'qemu.pid'), '-daemonize']
	if a.iso:
		cmd += ['-drive', 'file=%s,if=none,id=iso,format=raw,readonly=on' % os.path.abspath(a.iso)]
		if a.cdrom:
			# a SCSI CD-ROM on the USB bus, as the NanoKVM presents one
			cmd += ['-device', 'usb-bot,id=bot,bus=usb.0', '-device', 'scsi-cd,bus=bot.0,drive=iso']
		else:
			cmd += ['-device', 'usb-storage,bus=usb.0,drive=iso,removable=on']
	for spec in a.disk or []:
		name, _, what = spec.partition(':')
		path = what
		if not os.path.exists(what):
			path = os.path.join(a.dir, name + '.img')
			if not os.path.exists(path):
				subprocess.check_call(['qemu-img', 'create', '-q', '-f', 'raw', path, what])
		cmd += ['-drive', 'file=%s,if=none,id=%s,format=raw' % (os.path.abspath(path), name),
			'-device', 'nvme,drive=%s,serial=airos-%s' % (name, name)]
	with open(os.path.join(a.dir, 'cmd.json'), 'w') as f:
		json.dump(cmd, f)
	subprocess.check_call(cmd)
	print('started', open(os.path.join(a.dir, 'qemu.pid')).read().strip())


def shot(a):
	name = a.name
	if not name:
		n = len([f for f in os.listdir(a.dir) if f.startswith('shot-')])
		name = 'shot-%03d' % n
	ppm = os.path.abspath(os.path.join(a.dir, name + '.ppm'))
	qmp(a.dir, ('screendump', {'filename': ppm}))
	time.sleep(0.3)
	png = os.path.join(a.dir, name + '.png')
	subprocess.check_call(['python3', '-c', 'import sys;from PIL import Image;'
		'Image.open(sys.argv[1]).save(sys.argv[2])', ppm, png])
	os.remove(ppm)
	print(png)


def click(a):
	events = abs_event(a.x, a.y)
	qmp(a.dir, ('input-send-event', {'events': events}))
	time.sleep(0.2)
	for _ in range(2 if a.double else 1):
		qmp(a.dir, ('input-send-event', {'events': [{'type': 'btn', 'data': {'down': True, 'button': a.button}}]}))
		time.sleep(0.08)
		qmp(a.dir, ('input-send-event', {'events': [{'type': 'btn', 'data': {'down': False, 'button': a.button}}]}))
		time.sleep(0.08)
	print('clicked', a.x, a.y)


def move(a):
	qmp(a.dir, ('input-send-event', {'events': abs_event(a.x, a.y)}))


def key(a):
	for combo in a.keys:
		qmp(a.dir, ('send-key', {'keys': [{'type': 'qcode', 'data': k} for k in combo.split('+')]}))
		time.sleep(0.15)


SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
	'(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left', '}': 'bracket_right',
	'|': 'backslash', ':': 'semicolon', '"': 'apostrophe', '~': 'grave_accent', '<': 'comma',
	'>': 'dot', '?': 'slash'}
PLAIN = {' ': 'spc', '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right',
	'\\': 'backslash', ';': 'semicolon', "'": 'apostrophe', '`': 'grave_accent', ',': 'comma',
	'.': 'dot', '/': 'slash', '\n': 'ret', '\t': 'tab'}


def type_(a):
	for ch in a.text:
		if ch.isalpha() and ch.isupper():
			keys = ['shift', ch.lower()]
		elif ch.isalnum():
			keys = [ch]
		elif ch in PLAIN:
			keys = [PLAIN[ch]]
		elif ch in SHIFTED:
			keys = ['shift', SHIFTED[ch]]
		else:
			raise SystemExit('cannot type %r' % ch)
		qmp(a.dir, ('send-key', {'keys': [{'type': 'qcode', 'data': k} for k in keys]}))
		time.sleep(0.05)


def stop(a):
	try:
		qmp(a.dir, ('quit', {}))
	except Exception as e:
		print('quit:', e)


p = argparse.ArgumentParser()
sub = p.add_subparsers(dest='cmd', required=True)
s = sub.add_parser('start'); s.add_argument('dir'); s.add_argument('--iso')
s.add_argument('--cdrom', action='store_true'); s.add_argument('--disk', action='append')
s.add_argument('--memory', type=int, default=3072)
s = sub.add_parser('shot'); s.add_argument('dir'); s.add_argument('name', nargs='?')
s = sub.add_parser('click'); s.add_argument('dir'); s.add_argument('x', type=int); s.add_argument('y', type=int)
s.add_argument('--double', action='store_true'); s.add_argument('--button', default='left')
s = sub.add_parser('move'); s.add_argument('dir'); s.add_argument('x', type=int); s.add_argument('y', type=int)
s = sub.add_parser('key'); s.add_argument('dir'); s.add_argument('keys', nargs='+')
s = sub.add_parser('type'); s.add_argument('dir'); s.add_argument('text')
s = sub.add_parser('stop'); s.add_argument('dir')
a = p.parse_args()
{'start': start, 'shot': shot, 'click': click, 'move': move, 'key': key, 'type': type_,
	'stop': stop}[a.cmd](a)
