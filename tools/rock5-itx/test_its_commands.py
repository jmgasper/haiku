"""Check ITS command packets, queue wraparound and DMA address limits."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ItsCommandTests(unittest.TestCase):
    def test_command_packets_and_bounds(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            binary = Path(temporary) / 'its-commands'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(source / 'headers/private/kernel'),
                str(directory / 'test_its_commands.cpp'), '-o', str(binary)],
                check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
