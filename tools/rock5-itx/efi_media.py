#!/usr/bin/env python3
"""Package an ARM64 EFI application and sibling files into an immutable USB image."""
import argparse
import hashlib
import json
import os
from pathlib import PurePosixPath
import re
import struct
import subprocess

import lab


def package(name, entry, files):
    if not os.path.ismount(lab.WORK):
        raise RuntimeError('The project filesystem is not mounted')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', name):
        raise ValueError('Use a simple image name')
    inputs = {'EFI/BOOT/BOOTAA64.EFI': lab.local_path(entry)}
    for value in files:
        destination, source = value.split('=', 1)
        path = PurePosixPath(destination)
        if (path.is_absolute() or str(path) != destination
                or any(not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', part)
                       or part in ('.', '..') for part in path.parts)):
            raise ValueError('Use a relative FAT destination without dot components')
        if destination.lower() in [key.lower() for key in inputs]:
            raise ValueError('Duplicate FAT destination')
        inputs[destination] = lab.local_path(source)
    lab.efi_metadata(inputs['EFI/BOOT/BOOTAA64.EFI'].read_bytes())
    metadata = {name: {'source': str(path), 'bytes': path.stat().st_size,
                       'sha256': lab.digest(path)} for name, path in inputs.items()}
    output = lab.WORK / 'artifacts/efi-media' / lab.timestamp()
    output.mkdir(parents=True)
    image = output / (name + '.img')
    total_sectors = 96 * 1024 * 1024 // 512
    start = 2048
    mbr = bytearray(512)
    mbr[446:450] = b'\x00\xfe\xff\xff'
    mbr[450:454] = b'\xef\xfe\xff\xff'
    struct.pack_into('<II', mbr, 454, start, total_sectors - start)
    mbr[510:] = b'\x55\xaa'
    with image.open('xb') as stream:
        stream.write(mbr)
        stream.truncate(total_sectors * 512)
    volume = f'{image}@@{start * 512}'
    commands = [['mformat', '-i', volume, '-T', str(total_sectors - start),
                 '-h', '64', '-s', '32', '-H', str(start), '-v', 'HAIKULAB', '::']]
    directories = set()
    for destination in inputs:
        for parent in PurePosixPath(destination).parents:
            if str(parent) != '.':
                directories.add(str(parent))
    for directory in sorted(directories, key=lambda value: (value.count('/'), value)):
        commands.append(['mmd', '-i', volume, '::/' + directory])
    for destination, source in inputs.items():
        commands.append(['mcopy', '-i', volume, str(source), '::/' + destination])
    with (output / 'packaging.log').open('w') as log:
        for command in commands:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    for destination, item in metadata.items():
        data = subprocess.run(['mtype', '-i', volume, '::/' + destination],
                              check=True, capture_output=True).stdout
        if len(data) != item['bytes'] or hashlib.sha256(data).hexdigest() != item['sha256']:
            raise RuntimeError('FAT read-back differs from the source: ' + destination)
    manifest = {'image': str(image), 'bytes': image.stat().st_size,
                'sha256': lab.digest(image), 'architecture': 'arm64', 'purpose': name,
                'files': metadata, 'source_revision': lab.run(['git', '-C', str(lab.SOURCE),
                                                              'rev-parse', 'HEAD'])}
    lab.save(image.with_suffix('.json'), manifest)
    image.chmod(0o444)
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('name')
    parser.add_argument('entry', help='Local EFI executable placed at EFI/BOOT/BOOTAA64.EFI')
    parser.add_argument('--file', action='append', default=[], metavar='DESTINATION=SOURCE')
    args = parser.parse_args()
    print(json.dumps(package(args.name, args.entry, args.file), indent=2))
