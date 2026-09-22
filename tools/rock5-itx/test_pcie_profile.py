"""Exercise the firmware profile against a PCI config fixture and bad layouts."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


class PCIeProfileTests(unittest.TestCase):
    def test_production_training_wait_and_failures(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/add-ons/kernel/busses/pci/rk3588'
        driver = (source / 'rk3588_firmware.cpp').read_text()
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            (root / 'training.inc').write_text(driver[
                driver.index('static void\nReadSnapshot('):
                driver.index('static status_t\nInitDriver(')])
            binary = root / 'pcie-training-test'
            result = subprocess.run([
                'g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(source), '-I', str(root),
                str(directory / 'test_pcie_training.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PCIe training transitions, profile rejection and bounded failures passed',
                result.stdout)

    def test_onboard_profiles_and_bar_containment(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            path = Path(temporary)
            binary = path / 'onboard-profile-test'
            subprocess.run([
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(source / 'src/add-ons/kernel/busses/pci/rk3588'),
                str(directory / 'test_pcie_onboard.cpp'), '-o', str(binary),
            ], check=True, capture_output=True, text=True)
            fixtures = []
            for segment, identity, class_revision, bars in (
                (0, 0xa802144d, 0x01080201, [0xf0000004, 0, 1, 0, 0, 0]),
                (1, 0x11641b21, 0x01060102, [0xf1002000, 0, 0, 0, 0, 0xf1000000]),
                (3, 0x812510ec, 0x02000005, [1, 0, 0xf3000004, 0, 0xf3010004, 0]),
                (4, 0x812510ec, 0x02000005, [1, 0, 0xf4000004, 0, 0xf4010004, 0]),
            ):
                root = [0] * 64
                root[:4] = [0x35881d87, 0x00100007, 0x06040001, 0x00010000]
                base = 0xf0000000 + segment * 0x1000000
                root[6], root[8] = 0x00010100, base | (base >> 16)
                root[0x70 // 4] = 0x1042b010
                root[0x80 // 4] = 0x30230000 if segment < 2 else 0x30120000
                endpoint = [0] * 64
                endpoint[:4] = [identity, 0x00100007, class_revision, 0]
                endpoint[4:10] = bars
                for name, words in [('root', root), ('endpoint', endpoint)]:
                    fixture = path / (str(segment) + '-' + name)
                    fixture.write_bytes(struct.pack('<64I', *words))
                    fixtures.append(str(fixture))
            subprocess.run([str(binary), *fixtures], check=True, timeout=5)

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
            root[0x70 // 4], root[0x80 // 4] = 0x1042b010, 0x30230000
            endpoint = [0] * 64
            endpoint[0:6] = [0xa802144d, 0x00100007, 0x01080201, 0, 0xf0000004, 0]
            for name, data in [('root', root), ('endpoint', endpoint)]:
                (path / name).write_bytes(struct.pack('<64I', *data))
            subprocess.run([str(binary), str(path / 'root'), str(path / 'endpoint')],
                           check=True, timeout=5)

            # The replacement 1f99:6100 controller is at f0200000 on the
            # same EDK2 port; keep the original Samsung fixture accepted too.
            root[8] = 0xf020f020
            endpoint[0] = 0x61001f99
            endpoint[4] = 0xf0200004
            (path / 'root').write_bytes(struct.pack('<64I', *root))
            (path / 'endpoint').write_bytes(struct.pack('<64I', *endpoint))
            subprocess.run([str(binary), str(path / 'root'), str(path / 'endpoint')],
                           check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
