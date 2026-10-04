"""Check bounded writes, independent expected bytes and corruption detection."""
import array
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


MIB = 1024 * 1024


def expected(offset, length, round_number):
    mask = (1 << 64) - 1
    salt = round_number * 0xd1b54a32d192ed03 & mask
    words = array.array('Q', ((i * 0x9e3779b97f4a7c15 & mask) ^ salt
                            for i in range(offset // 8, (offset + length) // 8)))
    for i in range(len(words)):
        words[i] ^= words[i] >> 29
    if sys.byteorder != 'little':
        words.byteswap()
    return words.tobytes()


class NVMeStressTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        cls.root = Path(cls.directory.name)
        cls.binary = cls.root / 'nvme-stress'
        wrapper = cls.root / 'flush-fault.cpp'
        wrapper.write_text('''#include <errno.h>
#include <stdlib.h>
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (getenv("ROCK5_TEST_FSYNC_ERROR")) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
''')
        subprocess.run(['g++', '-std=c++17', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(Path(__file__).with_name('nvme_stress.cpp')), str(wrapper),
                        '-Wl,--wrap=fsync', '-o', str(cls.binary)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def setUp(self):
        self.disk = self.root / (self.id().split('.')[-1] + '.img')
        self.guard = b'\x6d' * MIB
        self.disk.write_bytes(self.guard * 10)

    def run_probe(self, mode, offset=1, length=8, workers=8, rounds=2, status=0, env=None):
        result = subprocess.run([str(self.binary), mode, str(self.disk), str(offset),
                                 str(length), str(workers), str(rounds)],
                                capture_output=True, text=True, timeout=20, env=env)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_concurrent_write_flush_cross_worker_read_and_guards(self):
        output = self.run_probe('write')
        self.assertIn('ROCK5_NVME_STRESS_PASS', output)
        self.assertEqual(output.count('ROCK5_NVME_STRESS_WRITE round='), 2)
        self.assertEqual(output.count('method=fsync status=pass'), 2)
        data = self.disk.read_bytes()
        self.assertEqual(data[:MIB], self.guard)
        self.assertEqual(data[9*MIB:], self.guard)
        self.assertEqual(data[MIB:9*MIB], expected(MIB, 8*MIB, 2))
        before = hashlib.sha256(data).digest()
        verification = self.run_probe('verify')
        self.assertIn('ROCK5_NVME_STRESS_PASS', verification)
        self.assertNotIn('ROCK5_NVME_STRESS_FLUSH', verification)
        self.assertEqual(hashlib.sha256(self.disk.read_bytes()).digest(), before)

    def test_corruption_is_detected_at_beginning_middle_and_end(self):
        self.run_probe('write')
        with self.disk.open('r+b') as stream:
            for offset in (MIB, 5*MIB+7, 9*MIB-1):
                stream.seek(offset)
                original = stream.read(1)
                stream.seek(offset)
                stream.write(bytes([original[0] ^ 1]))
                stream.flush()
                self.assertIn('MISMATCH', self.run_probe('verify', status=1))
                stream.seek(offset)
                stream.write(original)
                stream.flush()

    def test_flush_failure_stops_before_another_write_round(self):
        output = self.run_probe('write', status=1,
                                env=dict(os.environ, ROCK5_TEST_FSYNC_ERROR='1'))
        self.assertIn('method=fsync status=fail', output)
        self.assertIn('ROCK5_NVME_STRESS_FAIL', output)
        self.assertNotIn('ROCK5_NVME_STRESS_PASS', output)
        self.assertNotIn('round=2', output)
        data = self.disk.read_bytes()
        self.assertEqual(data[:MIB], self.guard)
        self.assertEqual(data[9*MIB:], self.guard)
        self.assertEqual(data[MIB:9*MIB], expected(MIB, 8*MIB, 1))

    def test_bad_bounds_and_worker_partition_do_not_write(self):
        original = self.disk.read_bytes()
        for offset, length, workers, rounds in [(-1,8,8,2),(3,8,8,2),
                (1,8,3,2),(1,8,0,2),(1,8,8,0),(1,8,8,33),(1,8193,8,2)]:
            self.run_probe('write', offset, length, workers, rounds, status=2)
        self.assertEqual(self.disk.read_bytes(), original)

    def test_missing_target_is_not_created(self):
        self.disk.unlink()
        self.run_probe('write', status=1)
        self.assertFalse(self.disk.exists())


if __name__ == '__main__':
    unittest.main()
