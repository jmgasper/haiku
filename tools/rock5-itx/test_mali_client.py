"""Run the production client/BO implementation with VM and user-copy fixtures."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MaliClientTest(unittest.TestCase):
    def test_buffer_ownership_mapping_and_cleanup(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/add-ons/kernel/drivers/graphics/mali_csf'
        code = (source / 'CsfClient.cpp').read_text()
        code = code[code.index('using namespace MaliCSF;'):]
        begin = code.index('static status_t\nMakeClientRamNoncacheable')
        end = code.index('static status_t\nCheckAccess', begin)
        code = code[:begin] + code[end:]
        with tempfile.TemporaryDirectory(prefix='mali-client-') as temporary:
            root = Path(temporary)
            (root / 'client.inc').write_text(code)
            binary = root / 'client-test'
            built = subprocess.run([
                'g++', '-pthread', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                '-I', str(source), '-I', str(root),
                str(directory / 'test_mali_client.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                timeout=45, env=dict(os.environ,
                    ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                    UBSAN_OPTIONS='halt_on_error=1'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('MALI_CSF_CLIENT_TEST_PASS', result.stdout)
            self.assertIn('MALI_CSF_VM_TEST_PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
