"""Run ARM64 empty-table initialization with dirty, guarded physical memory."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ARM64EmptyUserTableTests(unittest.TestCase):
    def test_production_initialization_and_publication(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        driver = (source / 'src/system/kernel/arch/arm64/arch_vm_translation_map.cpp').read_text()
        start = driver.index('\t// Create an empty page table')
        end = driver.index('\n\treturn B_OK;', start)
        install = driver[driver.index('void\narch_vm_install_empty_table_ttbr0('):
                         driver.index('status_t\narch_vm_translation_map_create_map(')]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            (root / 'empty_table.inc').write_text(install + '\nvoid Initialize(void* args)\n{\n'
                                                + driver[start:end] + '\n}\n')
            binary = root / 'empty-user-table-test'
            result = subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra',
                '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(root), str(directory / 'test_arm64_empty_user_table.cpp'),
                '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('ARM64 empty table: dirty memory, publication and allocation failure passed',
                          result.stdout)


if __name__ == '__main__':
    unittest.main()
