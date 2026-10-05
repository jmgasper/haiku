"""The MMC persistence oracle rejects absent writes and damage outside writes."""
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import qemu_mmc
import qemu_shell


class QEMUMMCTests(unittest.TestCase):
    def test_rediscovery_uses_unique_capacity_after_renumbering(self):
        first = [('/dev/disk/mmc/0/raw', '512', '8589934592'),
            ('/dev/disk/mmc/1/raw', '512', '9663676416'),
            ('/dev/disk/mmc/2/raw', '512', '7818182656')]
        second = [first[0], (first[1][0], '512', first[2][2]),
            (first[2][0], '512', first[1][2])]
        self.assertEqual(qemu_mmc.find_device(first, 512, 7818182656), first[2][0])
        self.assertEqual(qemu_mmc.find_device(second, 512, 7818182656), first[1][0])
        with self.assertRaisesRegex(RuntimeError, 'uniquely'):
            qemu_mmc.find_device(second + [first[2]], 512, 7818182656)
        with self.assertRaisesRegex(RuntimeError, 'uniquely'):
            qemu_mmc.find_device(second, 4096, 7818182656)

    def test_persistence_oracle(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            fixture = qemu_mmc.prepare(Path(temporary))
            self.assertEqual({d['kind'] for d in fixture['disks']}, {'sd-card', 'emmc'})
            self.assertEqual(len({d['bytes'] for d in fixture['disks']}), 2)
            with self.assertRaisesRegex(RuntimeError, 'readback failed'):
                qemu_mmc.verify_host(fixture)
            for disk in fixture['disks']:
                with Path(disk['disk']).open('r+b') as stream:
                    data = stream.read(8 * qemu_mmc.MIB)
                    stream.seek(5120 * qemu_mmc.MIB)
                    stream.write(data)
                    for operation in disk['operations']:
                        stream.seek(operation['offset'])
                        stream.write(data[:operation['bytes']])
            result = qemu_mmc.verify_host(fixture)
            self.assertEqual(result['status'], 'pass')
            self.assertEqual(len(result['hashes']), 6)
            with Path(fixture['disks'][1]['disk']).open('r+b') as stream:
                offset = 6144 * qemu_mmc.MIB + 2 * qemu_mmc.MIB - 1
                stream.seek(offset)
                value = stream.read(1)[0]
                stream.seek(offset)
                stream.write(bytes([value ^ 1]))
            with self.assertRaisesRegex(RuntimeError, 'readback failed'):
                qemu_mmc.verify_host(fixture)

    def test_requires_persistence_and_component_pins(self):
        with patch.object(qemu_shell.lab, 'read_manifest') as read:
            for options in ({}, {'power': True}, {'power': True, 'normal': True, 'pci_config': True}):
                with self.assertRaisesRegex(ValueError, 'MMC requires normal'):
                    qemu_shell.run(None, mmc=True, **options)
            read.assert_not_called()
            read.return_value = ({'private_image': True}, Path('unused'))
            with self.assertRaisesRegex(ValueError, 'MMC requires pinned'):
                qemu_shell.run(None, mmc=True, power=True, normal=True)


if __name__ == '__main__':
    unittest.main()
