"""Exercise production recovery against independent GPU/reset fault models."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MaliRecoveryTest(unittest.TestCase):
    def test_active_reset_address_spaces_and_retention(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/add-ons/kernel/drivers/graphics/mali_csf'
        with tempfile.TemporaryDirectory(prefix='mali-recovery-') as temporary:
            binary = Path(temporary) / 'recovery-test'
            built = subprocess.run([
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                '-I', str(source), str(directory / 'test_mali_recovery.cpp'),
                str(source / 'CsfFirmware.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                timeout=30, env=dict(os.environ,
                    ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                    UBSAN_OPTIONS='halt_on_error=1'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('MALI_CSF_RECOVERY_TEST_PASS cases=35', result.stdout)


if __name__ == '__main__':
    unittest.main()
