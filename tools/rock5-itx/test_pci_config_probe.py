"""Check the root-link precondition for the optional onboard PCI inventory."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


class PCIConfigProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        root = Path(cls.directory.name)
        source = root / 'root-link.cpp'
        source.write_text('''#include <stdio.h>
#include "pci_config_probe_checks.h"
int main() {
    uint32_t words[64];
    if (fread(words, sizeof(words), 1, stdin) != 1)
        return 2;
    return Rock5RootLinkReady(words) ? 0 : 1;
}
''')
        cls.binary = root / 'root-link'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-I', str(Path(__file__).resolve().parent), str(source), '-o', str(cls.binary)],
            check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def root(self):
        words = [0] * 64
        # Fields from the separately recorded EDK2 v1.1 root configuration.
        words[:4] = [0x35881d87, 0x00100007, 0x06040001, 0x00010000]
        words[6] = 0x00010100
        words[0x70 // 4] = 0x1042b010
        words[0x80 // 4] = 0x30230000
        return words

    def check(self, words, expected):
        result = subprocess.run([str(self.binary)], input=struct.pack('=64I', *words),
            capture_output=True, timeout=5)
        self.assertEqual(result.returncode, expected, result.stderr.decode(errors='replace'))

    def test_active_nvme_and_ethernet_links(self):
        for link in (0x3023, 0x3012):
            with self.subTest(link=hex(link)):
                words = self.root()
                words[0x80 // 4] = link << 16
                self.check(words, 0)

    def test_invalid_roots_and_inactive_links_are_rejected(self):
        cases = [(0, 0xffffffff), (1, 0x00100005), (2, 0x02000001),
            (3, 0), (6, 0x00020200), (6, 0x00000100), (0x70 // 4, 0x05),
            (0x80 // 4, 0x10230000), (0x80 // 4, 0x38230000),
            (0x80 // 4, 0x30200000), (0x80 // 4, 0x30030000)]
        for index, value in cases:
            with self.subTest(index=index, value=hex(value)):
                words = self.root()
                words[index] = value
                self.check(words, 1)


if __name__ == '__main__':
    unittest.main()
