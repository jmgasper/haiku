"""Exercise the firmware profile against a PCI config fixture and bad layouts."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


class PCIeProfileTests(unittest.TestCase):
    def test_firmware_resources_and_config_bounds(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            path = Path(temporary)
            binary = path / 'pcie-profile-test'
            subprocess.run([
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(source / 'src/add-ons/kernel/busses/pci/rk3588'),
                str(directory / 'test_pcie_profile.cpp'), '-o', str(binary),
            ], check=True, capture_output=True, text=True)
            # Relevant fields from the separately captured v1.1 root/950 Pro
            # snapshots; the same executable also accepts the full raw files.
            root = [0] * 64
            root[0:4] = [0x35881d87, 0x00100007, 0x06040001, 0x00010000]
            root[6], root[8] = 0x00010100, 0xf000f000
            endpoint = [0] * 64
            endpoint[0:6] = [0xa802144d, 0x00100007, 0x01080201, 0, 0xf0000004, 0]
            for name, data in [('root', root), ('endpoint', endpoint)]:
                (path / name).write_bytes(struct.pack('<64I', *data))
            subprocess.run([str(binary), str(path / 'root'), str(path / 'endpoint')],
                           check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
