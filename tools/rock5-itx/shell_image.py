#!/usr/bin/env python3
"""Create a private image with an authenticated shell on the USB lab network."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess

import lab


def credentials_path():
    return lab.WORK / 'state/haiku-lab-login.json'


def read_credentials():
    path = credentials_path()
    if path.stat().st_mode & 0o077:
        raise RuntimeError('Lab credentials must have mode 0600')
    value = json.loads(path.read_text())
    if value.get('username') != 'baron' or not re.fullmatch(
            r'\$s\$14\$[0-9a-f]{64}\$[0-9a-f]{64}', value.get('shadow_hash', '')):
        raise RuntimeError('Unsupported lab account or password hash format')
    if not re.fullmatch(r'[A-Za-z0-9_-]{24,}', value.get('password', '')):
        raise RuntimeError('Lab password must be a generated URL-safe token')
    return value


def create_credentials():
    path = credentials_path()
    if not path.exists():
        salt = secrets.token_bytes(32)
        password = secrets.token_urlsafe(24)
        hashed = hashlib.scrypt(password.encode(), salt=salt, n=16384, r=8, p=1,
                                dklen=32)
        value = {'username': 'baron', 'password': password,
                 'shadow_hash': '$s$14$' + salt.hex() + '$' + hashed.hex()}
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, 'w') as stream:
            json.dump(value, stream, indent=2)
            stream.write('\n')
    return read_credentials()


def prepare(manifest_path, rndis_only=False):
    base, source = lab.read_manifest(manifest_path)
    if base.get('private_image'):
        raise ValueError('Start with an ordinary build, not another private overlay')
    layout = lab.validate_image(source)
    partitions = [p for p in layout['partitions'] if p['type'] == 0xeb]
    if len(partitions) != 1:
        raise ValueError('Expected exactly one BFS partition')
    credentials = create_credentials()
    output = lab.WORK / 'artifacts/lab-shell-images' / lab.timestamp()
    output.mkdir(parents=True, mode=0o700)
    image = output / 'haiku-lab-shell.img'
    shutil.copyfile(source, image)
    image.chmod(0o600)
    files = {
        'system/settings/etc/shadow': 'baron:' + credentials['shadow_hash']
            + ':20000:0:99999:7:::\n',
        'home/config/settings/rock5-lab/enable-shell': 'Authenticated USB lab login\n',
    }
    if rndis_only:
        # QEMU's usb-net exposes both configurations; isolate the driver under test.
        files['system/settings/packages'] = (
            'Package haiku {\n    BlockedEntries {\n'
            '        add-ons/kernel/drivers/network/usb_ecm\n    }\n}\n')
    commands = ['mkdir /myfs/home/config/settings/rock5-lab']
    changes = []
    for index, (target, content) in enumerate(files.items()):
        local = output / f'input-{index}'
        local.write_text(content)
        local.chmod(0o600)
        readback = output / f'readback-{index}'
        commands += [f'cp -f :{local} /myfs/{target}',
                     f'chmod 600 /myfs/{target}',
                     f'cp -f /myfs/{target} :{readback}']
        changes.append({'path': target, 'sha256': hashlib.sha256(content.encode()).hexdigest(),
                        'permissions': '0600'})
    command = [str(lab.WORK / 'build/arm64/objects/linux/x86_64/release/tools/bfs_shell/bfs_shell'),
               '--start-offset', str(partitions[0]['start_sector'] * 512), str(image)]
    result = subprocess.run(command, input='\n'.join(commands + ['quit']) + '\n',
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=60)
    (output / 'mutation.log').write_text(result.stdout)
    result.check_returncode()
    for index, content in enumerate(files.values()):
        if (output / f'readback-{index}').read_text() != content:
            raise RuntimeError('Private overlay read-back mismatch')
    manifest = dict(base)
    manifest.update(image=str(image), sha256=lab.digest(image), created_utc=lab.timestamp(),
                    lab_overlay={'parent_sha256': base['sha256'], 'files': changes,
                                 'rndis_only': rndis_only,
                                 'purpose': 'Authenticated USB lab shell via NanoKVM SSH'},
                    private_image=True)
    lab.save(output / 'manifest.json', manifest)
    return output / 'manifest.json', manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', help='Manifest from lab.py artifact')
    parser.add_argument('--rndis-only', action='store_true',
                        help='Block ECM for the QEMU RNDIS test profile')
    args = parser.parse_args()
    if not os.path.ismount(lab.WORK):
        raise RuntimeError(f'Required filesystem is not mounted: {lab.WORK}')
    os.umask(0o077)
    with lab.lock('shell-image'):
        path, manifest = prepare(args.manifest, args.rndis_only)
    print(json.dumps({'manifest': str(path), 'sha256': manifest['sha256'],
                      'private_image': True}))


if __name__ == '__main__':
    main()
