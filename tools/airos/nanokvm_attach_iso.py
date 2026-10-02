#!/usr/bin/env python3
"""Attach the air/OS ISO to the ROCK 5 as a read-only USB disk via the NanoKVM.

Read-only so the live session never changes the image; the kernel mounts a
hybrid ISO read-only beneath a write overlay either way.
"""
import json, sys
sys.path.insert(0, '/mnt/HaikuWork/airos/release/tools/rock5-itx')
import lab, nanokvm

image = sys.argv[1] if len(sys.argv) > 1 else '/data/airos-arm64.iso'
config = json.loads(lab.CONFIG.read_text())
with lab.lock('hardware'):
	before = lab.gadget(config)
	code = f'''from pathlib import Path
p = Path({image!r})
if not p.is_file(): raise RuntimeError('Image is missing')
l = Path({config['gadget']!r}) / 'functions/mass_storage.disk0/lun.0'
(l / 'file').write_text('\\n')
(l / 'cdrom').write_text('0\\n')
(l / 'ro').write_text('1\\n')
'''
	lab.remote_python(config, code)
	nanokvm.api('/api/storage/image/mount', {'file': image, 'cdrom': False})
	after = lab.gadget(config)
	persisted = lab.ssh(config, 'nanokvm_ssh', 'cat /boot/usb.disk0').strip()
	print(json.dumps({'before': before, 'after': after, 'usb.disk0': persisted}, indent=2))
	if after != {'file': image, 'ro': '1', 'cdrom': '0'}:
		raise SystemExit('unexpected gadget state')
