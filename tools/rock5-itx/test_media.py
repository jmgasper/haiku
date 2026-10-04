"""Check actual media reporting against extended Ethernet speed values."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


class MediaTests(unittest.TestCase):
    def test_extended_ethernet_speed_and_names(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        compat = source / 'src/libs/compat'
        media = (compat / 'freebsd_network/fbsd_if_media.c').read_text()
        types = (compat / 'freebsd_network/compat/net/if.h').read_text()
        start = media.index('static const struct ifmedia_baudrate')
        end = media.index('#ifdef IFMEDIA_DEBUG', start)
        macros = re.findall(r'^#define\s+IF_[KMG]bps\(x\).*$', types, re.M)
        self.assertEqual(len(macros), 3)
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            (root / 'media_under_test.inc').write_text('\n'.join(macros) + '\n' + media[start:end])
            binary = root / 'media-tests'
            result = subprocess.run([
                'g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-D__HAIKU__',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(root), '-I', str(compat),
                '-I', str(source / 'src/bin/network/ifconfig'),
                str(directory / 'test_media.cpp'),
                str(source / 'src/bin/network/ifconfig/MediaTypes.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
