"""Check NDP source selection while the interface still exposes its old list."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class NdpSourceTests(unittest.TestCase):
    def test_replacement_deletion_and_iteration_references(self):
        directory = Path(__file__).resolve().parent
        source = (directory.parents[1] / 'src/add-ons/kernel/network/datalink_protocols'
                  '/ipv6_datagram/ipv6_datagram.cpp').read_text()
        start = source.index('\nstatic void\nndp_replace_local_source(')
        end = source.index('\nstatic void\nndp_remove_local_entry(', start)
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            (root / 'ndp_source_body.inc').write_text(source[start:end])
            binary = root / 'ndp-source-test'
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I', str(root), str(directory / 'test_ndp_source.cpp'),
                            '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('ndp source: replacement and deletion passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
