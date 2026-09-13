"""The independent FAT oracle must reject absent writes and unrelated damage."""
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import mmc_file_test as files
import qemu_shell


class MMCFileTests(unittest.TestCase):
    def test_fat_persistence_oracle(self):
        if not (shutil.which('mkfs.fat') or Path('/usr/sbin/mkfs.fat').exists()):
            self.skipTest('dosfstools is needed for the FAT fixture')
        if not shutil.which('mcopy'):
            self.skipTest('mtools is needed for the FAT fixture')
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            fixture = files.prepare(Path(temporary))
            self.assertEqual(files.verify_host(fixture, initial=True, label='initial')['status'], 'pass')
            with self.assertRaisesRegex(RuntimeError, 'Independent MMC file readback failed'):
                files.verify_host(fixture, label='absent-writes')
            root = Path(fixture['evidence'])
            image = fixture['disk'] + '@@' + str(fixture['partition_offset'])
            _, _, final, _ = files.patterns()
            target = root / 'target-final.bin'
            target.write_bytes(final)
            files.run_tool(['mcopy', '-o', '-i', image, str(target), '::/HAIKUTST/TARGET.BIN'],
                root / 'final-copy.log')
            self.assertEqual(files.verify_host(fixture, label='final')['status'], 'pass')
            # Last byte lies outside every write. Corruption there must fail.
            target.write_bytes(final[:-1] + bytes([final[-1] ^ 1]))
            files.run_tool(['mcopy', '-o', '-i', image, str(target), '::/HAIKUTST/TARGET.BIN'],
                root / 'corrupt-copy.log')
            with self.assertRaisesRegex(RuntimeError, 'Independent MMC file readback failed'):
                files.verify_host(fixture, label='corrupt-guard')

    def test_requires_mmc_and_fat_pin(self):
        with patch.object(qemu_shell.lab, 'read_manifest') as read:
            with self.assertRaisesRegex(ValueError, 'requires the MMC fixture'):
                qemu_shell.run(None, mmc_filesystem=True)
            read.assert_not_called()
            read.return_value = ({'private_image': True, 'mmc_test':
                {key: 'a' * 64 for key in qemu_shell.qemu_mmc.COMPONENTS}}, Path('unused'))
            with self.assertRaisesRegex(ValueError, 'requires a pinned FAT'):
                qemu_shell.run(None, mmc=True, power=True, normal=True, mmc_filesystem=True)


if __name__ == '__main__':
    unittest.main()
