"""Check the entire shader/descriptor/CS allocation against the Linux reference."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MaliShaderTest(unittest.TestCase):
    def test_linux_matched_shader_and_command_allocation(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/add-ons/kernel/drivers/graphics/mali_csf'
        with tempfile.TemporaryDirectory(prefix='mali-shader-') as temporary:
            binary = Path(temporary) / 'shader-test'
            built = subprocess.run([
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                '-I', str(source), str(directory / 'test_mali_shader.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, timeout=30,
                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                    UBSAN_OPTIONS='halt_on_error=1'))
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
            self.assertEqual(len(result.stdout), 4096)
            # Pinned Mesa 25.3.6 architecture-10 packer and Valhall assembler;
            # all four native Linux 6.18.52 submissions passed on this board.
            self.assertEqual(hashlib.sha256(result.stdout).hexdigest(),
                'dcc72b3bb9448e0acda21fa9dfe6052e83856460608b57c114552d1f1aa0d669')


if __name__ == '__main__':
    unittest.main()
