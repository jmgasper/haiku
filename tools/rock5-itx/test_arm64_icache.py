"""Run production cache maintenance against independently indexed CPU caches."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ARM64InstructionAliasTests(unittest.TestCase):
    def test_instruction_aliases_on_mixed_cpus(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        text = (source / 'src/system/kernel/arch/arm64/arch_cpu.cpp').read_text()
        start = text.index('void\narch_cpu_sync_icache(')
        body = text[start:text.index('\n}\n', start) + 3]
        operations = {
            'asm volatile ("mrs\\t%0, ctr_el0":"=r" (ctr_el0));': 'ctr_el0 = ReadCTR();',
            'asm volatile ("dc cvau, %0" : : "r"(address_dcache) : "memory");': 'CleanData(address_dcache);',
            'asm volatile ("ic ivau, %0" : : "r"(address_icache) : "memory");': 'InvalidateAddress(address_icache);',
            'asm volatile("ic ialluis" : : : "memory");': 'InvalidateAll();',
            'asm volatile("dsb ish" : : : "memory");': 'DSB();',
            'asm volatile("isb" : : : "memory");': 'ISB();',
        }
        for instruction, replacement in operations.items():
            body = body.replace(instruction, replacement)
        self.assertNotIn('asm', body, 'An architecture operation has no model')
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            (root / 'icache_production.inc').write_text(body)
            binary = root / 'icache-test'
            result = subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(root), '-I', str(source / 'headers/private/kernel/arch/arm64'),
                str(directory / 'test_arm64_icache.cpp'), '-o', str(binary)],
                capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('instruction aliases passed on all eight modeled CPUs', result.stdout)


if __name__ == '__main__':
    unittest.main()
