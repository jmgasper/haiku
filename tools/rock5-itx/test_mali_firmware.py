import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MaliFirmwareTest(unittest.TestCase):
    def test_production_container_and_section_copy(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/add-ons/kernel/drivers/graphics/mali_csf'
        with tempfile.TemporaryDirectory(prefix='mali-firmware-') as temporary:
            binary = Path(temporary) / 'firmware-test'
            subprocess.run([
                'g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                '-I', str(source), str(directory / 'test_mali_firmware.cpp'),
                str(source / 'CsfFirmware.cpp'), '-o', str(binary),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True,
                text=True, timeout=30,
                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                    UBSAN_OPTIONS='halt_on_error=1'))
            self.assertIn('MALI_CSF_FIRMWARE_TEST_PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
