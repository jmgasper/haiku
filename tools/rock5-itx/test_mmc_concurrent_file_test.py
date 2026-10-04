"""Reject lost writers, damaged guards and incomplete concurrent-I/O evidence."""
import os
from pathlib import Path
import tempfile
import unittest

import mmc_concurrent_file_test as concurrent
import mmc_file_test as files


class MMCConcurrentFileTests(unittest.TestCase):
    def test_independent_filesystem_oracle(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            fixture = concurrent.prepare(Path(temporary))
            root = Path(fixture['evidence'])
            self.assertEqual(files.verify_host(fixture, initial=True, label='initial')['status'], 'pass')
            with self.assertRaisesRegex(RuntimeError, 'Independent MMC file readback failed'):
                files.verify_host(fixture, label='absent-writes')
            image = fixture['disk'] + '@@' + str(fixture['partition_offset'])
            initial = (root / 'target-initial.bin').read_bytes()
            final = (root / 'target-final.bin').read_bytes()
            for label, data in [('complete', final),
                    ('lost-worker', final[:9 * files.MIB] + initial[9 * files.MIB:11 * files.MIB]
                        + final[11 * files.MIB:]),
                    ('corrupt-guard', final[:-1] + bytes([final[-1] ^ 1]))]:
                saved = root / (label + '.bin')
                saved.write_bytes(data)
                files.run_tool(['mcopy', '-o', '-i', image, str(saved), '::/HAIKUTST/TARGET.BIN'],
                    root / (label + '-copy.log'))
                if label == 'complete':
                    self.assertEqual(files.verify_host(fixture, label=label)['status'], 'pass')
                else:
                    with self.assertRaisesRegex(RuntimeError, 'Independent MMC file readback failed'):
                        files.verify_host(fixture, label=label)

    def test_transcript_rejects_missing_or_corrupt_cases(self):
        source, initial, _, _ = files.patterns()
        final, operations = concurrent.model(source, initial)
        import hashlib
        fixture = dict(operations=operations, final_sha256=hashlib.sha256(final).hexdigest())
        rows = [f'MMC_CONCURRENT_WRITE_PASS worker={op["worker"]} round={op["round"]} sha256={op["region_sha256"]}'
            for op in operations]
        flush = 'MMC_FLUSH method=B_FLUSH_DRIVE_CACHE result=pass\n'
        suffix = f'MMC_FILE_HASH {fixture["final_sha256"]} TARGET.BIN\nMMC_CONCURRENT_FINAL_PASS\nMMC_CONCURRENT_PASS phase=first-boot\n'
        text = '\n'.join(rows) + '\n' + flush * 65 + suffix
        self.assertEqual(concurrent.validate(text, fixture)['writes'], 64)
        for broken in [text.replace(rows[0] + '\n', '', 1), text + rows[0] + '\n',
                text.replace(operations[0]['region_sha256'], '0' * 64, 1),
                text.replace(flush, '', 1), text.replace('MMC_CONCURRENT_FINAL_PASS', ''),
                text.replace(fixture['final_sha256'], 'f' * 64)]:
            with self.assertRaises(ValueError):
                concurrent.validate(broken, fixture)
        after = suffix.replace('phase=first-boot', 'phase=after-reboot')
        self.assertEqual(concurrent.validate(after, fixture, True)['writes'], 0)
        with self.assertRaises(ValueError):
            concurrent.validate(after + rows[0] + '\n', fixture, True)


if __name__ == '__main__':
    unittest.main()
