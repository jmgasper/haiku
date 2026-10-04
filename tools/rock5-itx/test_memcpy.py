"""Exercise the production ARM64 copy algorithm with real protected pages."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MemcpyTests(unittest.TestCase):
    def test_alignments_canaries_and_protected_pages(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/system/libroot/posix/string/arch/arm64/arm64_memcpy.c'
        text = source.read_text()
        self.assertEqual(text.count('\nmemcpy('), 1)
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            renamed = root / 'memcpy_under_test.c'
            # Only rename the symbol to avoid intercepting the host sanitizer.
            renamed.write_text(text.replace('\nmemcpy(', '\nrock5_memcpy_under_test('))
            flags = ['-O2', '-Wall', '-Wextra', '-Werror', '-fstrict-aliasing',
                     '-fno-builtin', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
            subprocess.run(['gcc', '-std=c11', *flags, '-c', str(renamed),
                            '-o', str(root / 'copy.o')], check=True)
            binary = root / 'memcpy-tests'
            subprocess.run(['g++', '-std=c++17', *flags,
                            '-DCOPY_FUNCTION=rock5_memcpy_under_test',
                            str(directory / 'memcpy_probe.cpp'), str(root / 'copy.o'),
                            '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout,
                             'ROCK5_MEMCPY_PASS cases=51301 alignments=16 guarded_pages=yes\n')


if __name__ == '__main__':
    unittest.main()
