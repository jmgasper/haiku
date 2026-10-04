"""Run the production DMA mapper with fault-injecting host kernel substitutes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class BusDMATests(unittest.TestCase):
    def test_contiguous_allocator_policy_and_failures(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/libs/compat/freebsd_network/malloc.cpp'
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            # Retain every production body; replace only platform includes with
            # the VM/cache primitives supplied by the independent host fixture.
            (root / 'fbsd-contigmalloc.inc').write_text('\n'.join(
                line for line in source.read_text().splitlines()
                if not line.startswith('#include')))
            for noncoherent in (False, True):
                with self.subTest(noncoherent=noncoherent):
                    binary = root / ('contig-nc' if noncoherent else 'contig')
                    command = [
                        'g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-function',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I', str(root), str(directory / 'test_fbsd_contigmalloc.cpp'),
                        '-o', str(binary),
                    ]
                    if noncoherent:
                        command.append('-DFBSD_NONCOHERENT_DMA=1')
                    result = subprocess.run(command, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    subprocess.run([str(binary)], check=True, timeout=30)

    def test_mapper_with_and_without_noncoherent_dma(self):
        directory = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            for noncoherent in (False, True):
                with self.subTest(noncoherent=noncoherent):
                    binary = Path(temporary) / ('bus-dma-nc' if noncoherent else 'bus-dma')
                    command = [
                        'g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-I', str(directory / 'dma_host'),
                        str(directory / 'test_bus_dma.cpp'), '-o', str(binary),
                    ]
                    if noncoherent:
                        command.append('-DFBSD_NONCOHERENT_DMA=1')
                    result = subprocess.run(command, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
