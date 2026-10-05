"""Check NVMe trim bounds against an independent wide-integer interval oracle."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class NVMeTrimTests(unittest.TestCase):
    def test_sector_rounding_clipping_and_integer_boundaries(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            binary = Path(temporary) / 'nvme-trim-test'
            result = subprocess.run([
                'g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(source / 'src/add-ons/kernel/drivers/disk/nvme'),
                str(directory / 'test_nvme_trim.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == '__main__':
    unittest.main()
