"""Keep production ARM64 page aging consistent with its software mapping records."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_arm64_asid import function


class ARM64PageAgingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/system/kernel/arch/arm64'
        translation = (source / 'VMSAv8TranslationMap.cpp').read_text()
        header = (source / 'VMSAv8TranslationMap.h').read_text()
        constants = header[header.index('static constexpr uint64_t kPteAddrMask'):
                           header.index('struct VMSAv8TranslationMap')]
        signatures = ['static bool\nis_pte_dirty(', 'static uint64_t\nset_pte_clean(',
            'static bool\nis_pte_accessed(', 'bool\nflush_va_if_accessed(',
            'bool\nVMSAv8TranslationMap::FlushVAIfAccessed(',
            'bool\nVMSAv8TranslationMap::ClearAccessedAndModified(',
            'status_t\nVMSAv8TranslationMap::UnmapPage(']
        bodies = '\n'.join(function(translation, signature) for signature in signatures)
        for instruction, replacement in [
            ('asm("tlbi vaae1is, %0" ::"r"(((va >> 12) & kTLBIMask)));',
                'Invalidate(true, ((va >> 12) & kTLBIMask));'),
            ('asm("tlbi vae1is, %0" ::"r"(((va >> 12) & kTLBIMask) | (uint64_t(asid) << 48)));',
                'Invalidate(false, ((va >> 12) & kTLBIMask) | (uint64_t(asid) << 48));')]:
            if bodies.count(instruction) != 1:
                raise AssertionError('Expected production VA invalidation instruction')
            bodies = bodies.replace(instruction, replacement)
        temporary = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        cls.addClassCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / 'aging_constants.inc').write_text(constants)
        (root / 'aging_production.inc').write_text(bodies)
        cls.binary = root / 'aging-test'
        result = subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', str(root),
            str(directory / 'test_arm64_page_aging.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=45)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('ARM64 production page aging case passed', result.stdout)

    def test_accessed_page_survives_asid_eviction(self):
        self.run_case('evicted')

    def test_global_and_assigned_mappings_keep_accessed_state(self):
        self.run_case('mapped')

    def test_unaccessed_and_absent_pages_have_consistent_bookkeeping(self):
        self.run_case('unaccessed')

    def test_hardware_access_before_unmap_keeps_mapping(self):
        self.run_case('concurrent-access')


if __name__ == '__main__':
    unittest.main()
