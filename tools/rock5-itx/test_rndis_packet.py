"""Exercise the driver's actual frame extraction with memory sanitizers."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class RNDISPacketTests(unittest.TestCase):
    def test_frame_lengths_and_packet_batches(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            binary = Path(temporary) / 'rndis-packet-test'
            subprocess.run([
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                '-I', str(source / 'src/add-ons/kernel/drivers/network/ether/usb_rndis'),
                str(directory / 'test_rndis_packet.cpp'), '-o', str(binary),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
