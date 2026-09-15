import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MaliCommandsTest(unittest.TestCase):
    def test_queue_execution_completion_and_teardown_faults(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/add-ons/kernel/drivers/graphics/mali_csf'
        with tempfile.TemporaryDirectory(prefix='mali-commands-') as temporary:
            binary = Path(temporary) / 'memory-test'
            result = subprocess.run([
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                '-I', str(source), str(directory / 'test_mali_commands.cpp'),
                str(source / 'CsfFirmware.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                timeout=30, env=dict(os.environ,
                    ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                    UBSAN_OPTIONS='halt_on_error=1'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('MALI_CSF_COMMANDS_TEST_PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
