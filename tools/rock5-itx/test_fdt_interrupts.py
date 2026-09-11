"""Run the kernel's interrupt decoder against native and malformed fixtures."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class InterruptTests(unittest.TestCase):
    def test_native_and_malformed_interrupt_specifiers(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            binary = Path(temporary) / 'fdt-interrupt-test'
            subprocess.run([
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I', str(source / 'src/add-ons/kernel/bus_managers/fdt'),
                str(directory / 'test_fdt_interrupts.cpp'), '-o', str(binary),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
