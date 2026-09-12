"""Check that emulated storage validation rejects misplaced and truncated data."""

import os
from pathlib import Path
import tempfile
import unittest

import qemu_nvme


class NVMeFixtureTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        self.fixture = qemu_nvme.prepare(Path(self.directory.name))
        self.disk = Path(self.fixture['disk'])

    def tearDown(self):
        self.directory.cleanup()

    def copy_region(self, offset):
        with self.disk.open('r+b') as stream:
            data = stream.read(self.fixture['region_bytes'])
            stream.seek(offset)
            stream.write(data)

    def test_write_above_4gib_must_reach_the_correct_offset(self):
        with self.assertRaisesRegex(RuntimeError, 'data differs'):
            qemu_nvme.verify_host(self.fixture)
        # A wrapped 32-bit offset would write over the first region instead.
        self.copy_region(0)
        with self.assertRaisesRegex(RuntimeError, 'data differs'):
            qemu_nvme.verify_host(self.fixture)
        self.copy_region(qemu_nvme.HIGH_OFFSET)
        self.assertEqual(qemu_nvme.verify_host(self.fixture)[
            'host_readback_after_shutdown'], 'pass')
        with self.disk.open('r+b') as stream:
            stream.write(b'corrupt!')
        with self.assertRaisesRegex(RuntimeError, 'data differs'):
            qemu_nvme.verify_host(self.fixture)

    def test_truncated_namespace_is_rejected(self):
        self.copy_region(qemu_nvme.HIGH_OFFSET)
        with self.disk.open('r+b') as stream:
            stream.truncate(qemu_nvme.HIGH_OFFSET + qemu_nvme.REGION_BYTES - 1)
        with self.assertRaisesRegex(RuntimeError, 'size changed'):
            qemu_nvme.verify_host(self.fixture)
