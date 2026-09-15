"""Check native GPU synchronization semantics and module-owned FD lifetimes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from mali_sync_fixture import prepare_sync_fixture


class MaliSyncTest(unittest.TestCase):
    def test_shared_objects_snapshots_timelines_and_descriptor_lifetime(self):
        directory = Path(__file__).resolve().parent
        repository = directory.parents[1]
        source = repository / 'src/add-ons/kernel/drivers/graphics/mali_csf'
        with tempfile.TemporaryDirectory(prefix='mali-sync-') as temporary:
            root = Path(temporary)
            prepare_sync_fixture(root, repository)
            binary = root / 'sync-test'
            build = subprocess.run([
                'g++', '-pthread', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                '-I', str(source), '-I', str(root), str(directory / 'test_mali_sync.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=45,
                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                    UBSAN_OPTIONS='halt_on_error=1'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('MALI_CSF_SYNC_TEST_PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
