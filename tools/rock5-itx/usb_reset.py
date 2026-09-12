"""Explicit NanoKVM USB reset trials while Haiku runs from the SSD.

Called only by the session owning the hardware lock. Preparation requires a
working shell; the single subsequent reset can run after USB control is lost.
"""
import json

import lab
import nanokvm
import shell


PREFLIGHT = '''set -e
[ "$(uname -s)" = Haiku ]
volumes=$(df -h)
printf '%s\n' "$volumes"
case "$volumes" in *"/dev/disk/usb/"*) exit 1;; esac
case "$volumes" in *"/dev/disk/nvme/0/1"*) ;; *) exit 1;; esac
sync
echo ROCK5_USB_RESET_READY
'''


def controller_state(config):
    return json.loads(lab.remote_python(config, '''import json
from pathlib import Path
print(json.dumps({'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
    'persistent_image': Path('/boot/usb.disk0').read_text().strip()}))
'''))


def prepare(config, deployment, output, target):
    if deployment.get('boot_source') != 'nvme':
        raise RuntimeError('USB reset preparation requires an installed NVMe boot')
    serial = output / 'serial.log'
    boots = [line for line in serial.read_bytes().splitlines()
             if b'Mounted boot partition: ' in line]
    if not boots or boots[-1].strip() != b'Mounted boot partition: /dev/disk/nvme/0/1':
        raise RuntimeError('UART does not confirm the installed NVMe boot')
    before = lab.gadget(config)
    controller = controller_state(config)
    if before['file'] != config['recovery_image'] \
            or controller['persistent_image'] != config['recovery_image']:
        raise RuntimeError('Selected and persistent USB images must both be recovery')
    transcript = output / ('usb-reset-preflight-' + lab.timestamp() + '.txt')
    shell.run_commands(config, shell.usb_address(target), PREFLIGHT, transcript, 60)
    if 'ROCK5_USB_RESET_READY' not in transcript.read_text():
        raise RuntimeError('USB reset preflight marker missing')
    preparation = {'controller': controller, 'gadget': before,
                   'serial_bytes': serial.stat().st_size, 'preflight': str(transcript)}
    lab.save(output / 'usb-reset-preparation.json', preparation)
    return preparation


def reset(config, preparation, output):
    if preparation is None:
        raise RuntimeError('Prepare this USB reset while the SSD shell is reachable')
    serial = (output / 'serial.log').read_bytes()
    offset = preparation['serial_bytes']
    if len(serial) < offset or any(marker in serial[offset:] for marker in
            (b'Haiku revision:', b'UEFI firmware', b'Linux version',
             b'Mounted boot partition:')):
        raise RuntimeError('Target boot changed after USB reset preparation')
    if controller_state(config) != preparation['controller'] \
            or lab.gadget(config) != preparation['gadget']:
        raise RuntimeError('Controller or USB image changed after preparation')
    receipt = {'status': 'incomplete', 'preparation': preparation,
               'method': 'NanoKVM 2.4.3 POST /api/hid/reset'}
    path = output / ('usb-reset-' + lab.timestamp() + '.json')
    lab.save(path, receipt)
    try:
        receipt['api_result'] = nanokvm.api('/api/hid/reset', {})
        receipt['controller_after'] = controller_state(config)
        receipt['gadget_after'] = lab.gadget(config)
        if receipt['controller_after'] != preparation['controller'] \
                or receipt['gadget_after'] != preparation['gadget']:
            raise RuntimeError('USB reset changed the controller or selected image')
        receipt['status'] = 'reset_completed_reconnection_unverified'
    except Exception as error:
        receipt.update(status='error', error=str(error))
        raise
    finally:
        lab.save(path, receipt)
    return {'evidence': str(path), **receipt}
