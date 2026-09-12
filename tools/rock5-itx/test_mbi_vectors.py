"""Check MSI allocation boundaries, alignment, fragmentation and exact frees."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MbiVectorTests(unittest.TestCase):
    def test_vector_lifecycle(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            binary = Path(temporary) / 'mbi-vectors'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(source / 'headers/private/kernel'),
                str(directory / 'test_mbi_vectors.cpp'), '-o', str(binary)],
                check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
