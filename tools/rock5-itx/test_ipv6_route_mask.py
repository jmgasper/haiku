"""Verify the production IPv6 mask rank obeys the routing table contract."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class IPv6RouteMaskTests(unittest.TestCase):
    def test_prefix_and_least_significant_bit_ranks(self):
        directory = Path(__file__).resolve().parent
        source = (directory.parents[1] / 'src/add-ons/kernel/network/protocols'
                  '/ipv6/ipv6_address.cpp').read_text()
        start = source.index('\nstatic int32\nipv6_first_mask_bit(')
        end = source.index('\n\n/*!', start)
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            (root / 'ipv6_route_mask_body.inc').write_text(source[start:end])
            binary = root / 'ipv6-route-mask-test'
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I', str(root), str(directory / 'test_ipv6_route_mask.cpp'),
                            '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('129 prefixes and all 128 bit positions passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
