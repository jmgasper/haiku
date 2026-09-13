"""Exercise the production NDP sender with separate links and address lifetimes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class NdpSendTests(unittest.TestCase):
    def test_link_selection_and_address_lifetime(self):
        directory = Path(__file__).resolve().parent
        source = (directory.parents[1] / 'src/add-ons/kernel/network/datalink_protocols'
                  '/ipv6_datagram/ipv6_datagram.cpp').read_text()
        start = source.index('\nstatic status_t\nndp_send_data(')
        end = source.index('\n\n//\t#pragma mark', start)
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            (root / 'ndp_send_body.inc').write_text(source[start:end])
            binary = root / 'ndp-send-test'
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-pthread', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all', '-I', str(root),
                            str(directory / 'test_ndp_send.cpp'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('ndp send: links, lifetimes and failure ownership passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
