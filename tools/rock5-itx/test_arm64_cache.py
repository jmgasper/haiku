"""Check the kernel's CTR_EL0 decoding with undefined-behavior detection."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ARM64CacheTests(unittest.TestCase):
    def test_architectural_cache_line_sizes(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            binary = Path(temporary) / 'arm64-cache-test'
            subprocess.run([
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=undefined', '-fno-sanitize-recover=all',
                '-I', str(source / 'headers/private/kernel/arch/arm64'),
                str(directory / 'test_arm64_cache.cpp'), '-o', str(binary),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
