"""Run the production ARM64 sampler with guarded stacks and faulting user reads."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ARM64StackTraceTests(unittest.TestCase):
    def test_production_sampler(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        driver = (source / 'src/system/kernel/arch/arm64/arch_debug.cpp').read_text()
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            (root / 'stack_trace.inc').write_text(driver[
                driver.index('struct sampled_iframe {'):
                driver.index('void*\narch_debug_get_interrupt_pc(')])
            # Keep the real iframe layout and generic nested fault-handler guard.
            (root / 'kernel.h').write_text('')
            (root / 'thread.h').write_text('')
            binary = root / 'arm64-stack-trace-test'
            result = subprocess.run([
                'g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(root), '-I', str(source / 'headers/private/kernel'),
                str(directory / 'test_arm64_stack_trace.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('ARM64 sampler: frames, bounds, filters, faults and limits passed',
                          result.stdout)


if __name__ == '__main__':
    unittest.main()
