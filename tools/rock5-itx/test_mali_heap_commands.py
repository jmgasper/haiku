"""Interpret the native heap readback commands across the 32 KiB boundary."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MaliHeapCommandsTest(unittest.TestCase):
    def test_signed_offsets_and_guarded_readback(self):
        directory = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(prefix='mali-heap-commands-') as temporary:
            binary = Path(temporary) / 'commands-test'
            built = subprocess.run([
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                str(directory / 'test_mali_heap_commands.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                timeout=15, env=dict(os.environ,
                    ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                    UBSAN_OPTIONS='halt_on_error=1'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('MALI_HEAP_COMMANDS_PASS programs=33 signed_offset_regression=1 invalid_inputs=7', result.stdout)


if __name__ == '__main__':
    unittest.main()
