"""Reject other boards, changed MBI resources, and a busy or unsuitable GIC."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MbiProfileTests(unittest.TestCase):
    def test_fdt_and_controller_admission(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                     '-I', str(source / 'headers/libs/libfdt'),
                     '-I', str(source / 'headers/private/kernel')]
            objects = []
            for name in ('fdt', 'fdt_ro', 'fdt_rw', 'fdt_sw', 'fdt_wip', 'fdt_strerror',
                         'fdt_empty_tree'):
                target = root / (name + '.o')
                subprocess.run(['gcc', '-std=c11', '-D_DEFAULT_SOURCE', *flags,
                    '-c', str(source / 'src/libs/libfdt' / (name + '.c')), '-o', str(target)],
                    check=True, capture_output=True, text=True)
                objects.append(str(target))
            program = root / 'mbi-profile'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags,
                str(directory / 'mbi_probe/test_profile.cpp'), *objects, '-o', str(program)],
                check=True, capture_output=True, text=True)
            subprocess.run([str(program)], check=True, timeout=5)
            captured = os.environ.get('ROCK5_MBI_CAPTURED_DTB')
            if captured:
                subprocess.run([str(program), captured], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
