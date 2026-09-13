"""Exercise the production receive routine against controlled DMA descriptors."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


class RgeReceiveTests(unittest.TestCase):
    def test_receive_lengths_fragments_and_ring_ownership(self):
        directory = Path(__file__).resolve().parent
        driver = directory.parents[1] / 'src/add-ons/kernel/drivers/network/ether/rtl8125/dev/pci'
        source = (driver / 'if_rge.c').read_text()
        start = source.index('\nint\nrge_rxeof(')
        end = source.index('\nint\nrge_txeof(', start)
        # Keep the complete receive routine and its actual hardware constants.
        constants = [line for line in (driver / 'if_rgereg.h').read_text().splitlines()
                     if re.match(r'#define RGE_(RDCMDSTS_|RDEXTSTS_|RX_LIST_CNT\b|NEXT_RX_DESC\()', line)]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            (root / 'rge_receive_constants.h').write_text('\n'.join(constants) + '\n')
            (root / 'rge_receive_body.inc').write_text(source[start:end])
            binary = root / 'receive-tests'
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I', str(root), str(directory / 'test_rge_receive.cpp'),
                            '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('rge receive: lengths, fragments, ownership and wrap passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
