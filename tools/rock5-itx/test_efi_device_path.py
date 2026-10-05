"""Exercise the loader's actual EFI device-path matcher with a host compiler."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class DevicePathTests(unittest.TestCase):
    def test_boot_disk_matching_and_malformed_paths(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            binary = Path(temporary) / 'efi-device-path-test'
            subprocess.run([
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I', str(source / 'src/system/boot/platform/efi'),
                str(directory / 'test_efi_device_path.cpp'), '-o', str(binary),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
