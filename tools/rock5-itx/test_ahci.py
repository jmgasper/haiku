"""Exercise production AHCI payload and submission code with a faulting HBA."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class AHCITests(unittest.TestCase):
    def test_controller_interrupt_lifecycle(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/add-ons/kernel/busses/scsi/ahci'
        controller = (source / 'ahci_controller.cpp').read_text()
        definitions = (source / 'ahci_defs.h').read_text()
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            header = (source / 'ahci_controller.h').read_text()
            (root / 'ahci_controller.h').write_text('\n'.join(
                line for line in header.splitlines() if not line.startswith('#include')))
            (root / 'definitions.inc').write_text(definitions[
                definitions.index('enum {'):definitions.index('typedef struct {\n\tuint16 vendor;')])
            (root / 'controller.inc').write_text(controller[
                controller.index('AHCIController::AHCIController('):
                controller.index('status_t\nAHCIController::ResetController')])
            binary = root / 'ahci-controller-test'
            result = subprocess.run([
                'g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-unused-but-set-variable',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(root), str(directory / 'test_ahci_controller.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('AHCI interrupt setup and partial-init cleanup passed', result.stdout)

    def test_dma_ownership_bounds_and_failures(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/add-ons/kernel/busses/scsi/ahci'
        port = (source / 'ahci_port.cpp').read_text()
        request = (source / 'sata_request.cpp').read_text()
        definitions = (source / 'ahci_defs.h').read_text()
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            for name in ('ahci_port.h', 'sata_request.h'):
                text = (source / name).read_text()
                (root / name).write_text('\n'.join(
                    line for line in text.splitlines() if not line.startswith('#include')))
            (root / 'definitions.inc').write_text(definitions[
                definitions.index('enum {'):definitions.index('typedef struct {\n\tuint16 vendor;')])
            sections = [
                port[port.index('AHCIPort::AHCIPort('):port.index('status_t\nAHCIPort::Init1')],
                port[port.index('status_t\nAHCIPort::FillPrdTable(volatile prd* prdTable, int* prdCount, int prdMax,\n\tconst physical_entry*'):
                     port.index('status_t\nAHCIPort::WaitForTransfer')],
                port[port.index('void\nAHCIPort::FinishTransfer'):port.index('void\nAHCIPort::ScsiTestUnitReady')],
                port[port.index('void\nAHCIPort::ExecuteSataRequest'):port.index('void\nAHCIPort::ScsiExecuteRequest')],
                port[port.index('void\nAHCIPort::ScsiGetRestrictions'):port.index('bool\nAHCIPort::Enable')],
                request[request.index('status_t\nsata_request::CopyData'):request.index('void\nsata_request::SetATACommand')],
            ]
            (root / 'production.inc').write_text('\n'.join(sections))
            binary = root / 'ahci-test'
            result = subprocess.run([
                'g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-address-of-packed-member',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(root), str(directory / 'test_ahci.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('AHCI DMA ownership, bounds and failure cases passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
