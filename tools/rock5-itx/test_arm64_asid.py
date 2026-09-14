"""Exercise production ARM64 ASID switching with independent per-CPU TLBs."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


def function(text, signature):
    start = text.index(signature)
    end = text.index('\n}\n', start) + 3
    return text[start:end]


class ARM64ASIDTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        translation = (source / 'src/system/kernel/arch/arm64/VMSAv8TranslationMap.cpp').read_text()
        architecture = (source / 'src/system/kernel/arch/arm64/arch_vm_translation_map.cpp').read_text()
        management = translation[translation.index('// ASID Management'):
                                 translation.index('static bool\nis_pte_dirty(')]
        instruction = 'asm("tlbi aside1is, %0" ::"r"(asid << 48));'
        if management.count(instruction) != 1:
            raise AssertionError('Expected the production ASID invalidation instruction')
        management = management.replace(instruction, 'Invalidate(asid << 48);')
        empty = function(architecture, 'void\narch_vm_install_empty_table_ttbr0(')
        if empty.count('asm("isb");') != 1:
            raise AssertionError('Expected the empty-table publication barrier')
        empty = empty.replace('asm("isb");', 'ISB();')
        names = ['VMSAv8TranslationMap::VMSAv8TranslationMap(',
                 'VMSAv8TranslationMap::~VMSAv8TranslationMap(',
                 'void\nVMSAv8TranslationMap::SwitchUserMap(',
                 'int\nVMSAv8TranslationMap::CalcStartLevel(']
        bodies = '\n'.join(function(translation, name) for name in names)
        temporary = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        cls.addClassCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / 'asid_production.inc').write_text(empty + '\n' + management + bodies)
        cls.binary = root / 'asid-test'
        result = subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', str(root),
            str(directory / 'test_arm64_asid.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=45)
        if result.returncode != 0:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('ARM64 production ASID case passed with CnP off and on', result.stdout)

    def test_empty_table_has_distinct_asid(self):
        self.run_case('empty')

    def test_exhaustion_and_cross_cpu_reuse(self):
        self.run_case('exhaustion')

    def test_reserved_id_rejects_release(self):
        self.run_case('reserved-free')


if __name__ == '__main__':
    unittest.main()
