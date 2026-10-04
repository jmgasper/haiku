"""Production vm_clone_area: RAM/device attributes and a pre-fix regression."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class VmCloneMemoryTypeTest(unittest.TestCase):
    def test_ram_and_device_alias_attributes(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/system/kernel/vm/vm.cpp'
        code = source.read_text()
        code = 'area_id\n' + code[code.index('vm_clone_area(team_id'):]
        code = code[:code.index('\n\n/*!')]
        old = code.replace('\tnewArea->SetMemoryType(sourceArea->MemoryType());\n', '')
        old = old.replace('uint32 memoryType = sourceArea->MemoryType();',
            'uint32 memoryType = sourceArea->MemoryType();\n'
            '\t\t\tnewArea->SetMemoryType(sourceArea->MemoryType());')
        with tempfile.TemporaryDirectory(prefix='vm-clone-type-') as temporary:
            root = Path(temporary)
            for name, contents in (('corrected', code), ('before-fix', old)):
                (root / 'clone.inc').write_text(contents)
                binary = root / name
                built = subprocess.run([
                    'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                    '-I', str(root), str(directory / 'test_vm_clone_memory_type.cpp'),
                    '-o', str(binary),
                ], capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True,
                    timeout=20, env=dict(os.environ,
                        ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                        UBSAN_OPTIONS='halt_on_error=1'))
                if name == 'corrected':
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn('VM_CLONE_MEMORY_TYPE_TEST_PASS', result.stdout)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('memoryType == sExpectedType', result.stderr)


if __name__ == '__main__':
    unittest.main()
