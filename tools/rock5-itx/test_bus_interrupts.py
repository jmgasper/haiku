"""Compile the actual interrupt lifecycle and Realtek top half with host faults."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class BusInterruptTests(unittest.TestCase):
    def test_msi_vector_storage_is_wide(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        device = (source / 'src/libs/compat/freebsd_network/device.h').read_text()
        bus = (source / 'src/libs/compat/freebsd_network/bus.cpp').read_text()
        pci = (source / 'src/libs/compat/freebsd_network/pci.cpp').read_text()
        self.assertIn('uint32\t\t\tmsi_start_vector;', device)
        self.assertIn('root->msi_start_vector + *rid - 1', bus)
        self.assertEqual(pci.count('root->msi_start_vector = startVector;'), 2)
        self.assertNotIn('info->u.h0.interrupt_line = startVector;', pci)

    def test_setup_failures_and_level_interrupt_masking(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        bus = (source / 'src/libs/compat/freebsd_network/bus.cpp').read_text()
        rge = (source / 'src/add-ons/kernel/drivers/network/ether/rtl8125/dev/pci/if_rge.c').read_text()
        pci = (source / 'src/add-ons/kernel/bus_managers/pci/pci.cpp').read_text()
        compat_pci = (source / 'src/libs/compat/freebsd_network/pci.cpp').read_text()
        # The bodies are copied verbatim, with kernel services substituted by
        # the harness. This tests production code, not a second implementation.
        sections = [
            bus[bus.index('struct internal_intr {'):bus.index('static area_id\nmap_mem')],
            bus[bus.index('static int\nbus_alloc_irq_resource'):bus.index('static int\nbus_alloc_mem_resource')],
            bus[bus.index('static int32\nintr_wrapper'):bus.index('int\nbus_bind_intr')],
            rge[rge.index('int\nHAIKU_CHECK_DISABLE_INTERRUPTS'):rge.index('#endif\n\nint\nrge_intr')],
        ]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            (root / 'interrupts_under_test.inc').write_text('\n'.join(sections))
            (root / 'pci_intx_under_test.inc').write_text(
                pci[pci.index('status_t\nPCI::GetIntxIRQ'):pci.index('//#pragma mark - MSI')])
            (root / 'pci_init_under_test.inc').write_text(
                compat_pci[compat_pci.index('pci_module_info *gPci;'):compat_pci.index('pci_info*\nget_device_pci_info')])
            binary = root / 'interrupt-tests'
            result = subprocess.run([
                'g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-cast-function-type',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(root), str(directory / 'test_bus_interrupts.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
