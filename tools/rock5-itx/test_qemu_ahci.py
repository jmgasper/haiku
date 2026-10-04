"""The independent AHCI oracle rejects missing writes and altered guard bytes."""
import os
from pathlib import Path
import tempfile
import unittest

import qemu_ahci


class QEMUAHCITests(unittest.TestCase):
    def test_backing_file_oracle(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            fixture = qemu_ahci.prepare(Path(temporary))
            self.assertEqual(len({d['disk'] for d in fixture['disks']}), 2)
            self.assertEqual({d['physical_sector'] for d in fixture['disks']}, {512, 4096})
            self.assertEqual({d['sector'] for d in fixture['disks']}, {512})
            with self.assertRaisesRegex(RuntimeError, 'readback failed'):
                qemu_ahci.verify_host(fixture)
            for disk in fixture['disks']:
                with Path(disk['disk']).open('r+b') as stream:
                    data = stream.read(8 * qemu_ahci.MIB)
                    stream.seek(4096 * qemu_ahci.MIB)
                    stream.write(data)
                    for operation in disk['operations']:
                        stream.seek(operation['offset'])
                        stream.write(data[:operation['bytes']])
            result = qemu_ahci.verify_host(fixture)
            self.assertEqual(result['status'], 'pass')
            self.assertEqual(len(result['hashes']), 6)
            with Path(fixture['disks'][1]['disk']).open('r+b') as stream:
                offset = 6144 * qemu_ahci.MIB + 2 * qemu_ahci.MIB - 1
                stream.seek(offset)
                value = stream.read(1)[0]
                stream.seek(offset)
                stream.write(bytes([value ^ 1]))
            with self.assertRaisesRegex(RuntimeError, 'readback failed'):
                qemu_ahci.verify_host(fixture)


if __name__ == '__main__':
    unittest.main()
