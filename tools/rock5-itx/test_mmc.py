"""Run the production MMC decoders and SDHCI state machine with host fault fixtures."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MMCTests(unittest.TestCase):
    def compile_run(self, name, prepare, marker):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1]
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            root = Path(temporary)
            header = (source / 'headers/private/drivers/mmc.h').read_text()
            (root / 'mmc.h').write_text('\n'.join(
                line for line in header.splitlines() if not line.startswith('#include')))
            prepare(root, source)
            binary = root / name
            result = subprocess.run([
                'g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-unused-function',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(root), str(directory / (name + '.cpp')), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(marker, result.stdout)

    def test_card_register_decoding(self):
        def prepare(root, source):
            header = (source / 'src/add-ons/kernel/bus_managers/mmc/mmc_bus.h').read_text()
            (root / 'cid.inc').write_text(header[
                header.index('class CidWrapper'):header.index('extern device_manager_info')])
        self.compile_run('test_mmc_cid', prepare, 'MMC and SD register fixtures passed')

    def test_sdhci_protocol_and_failures(self):
        def prepare(root, source):
            directory = source / 'src/add-ons/kernel/busses/mmc'
            header = (directory / 'sdhci.h').read_text()
            header = header[:header.index('typedef void* sdhci_mmc_bus;')] + '\n#endif\n'
            header = '\n'.join(line for line in header.splitlines()
                               if not line.startswith('#include'))
            # Replace only memory-mapped register storage with a doorbell,
            # read-to-pop FIFO and write-one-to-clear status. Driver bodies
            # and all bitfield methods remain the production implementation.
            start, end = header.index('class Command {'), header.index('class PresentState {')
            header = (header[:start] + header[start:end].replace(
                'volatile uint16_t fBits;', 'Doorbell16 fBits;') + header[end:])
            header = header.replace('volatile uint32_t buffer_data_port;', 'Fifo32 buffer_data_port;')
            header = header.replace('volatile uint32_t interrupt_status;', 'W1C32 interrupt_status;')
            header = header.replace('const uint8_t specVersion;', 'uint8_t specVersion;')
            (root / 'sdhci.h').write_text(header)
            code = (directory / 'sdhci.cpp').read_text()
            (root / 'sdhci.inc').write_text(code[
                code.index('static int32\nsdhci_generic_interrupt'):code.index('void\nuninit_bus')])
        self.compile_run('test_mmc_sdhci', prepare, 'SDHCI protocol, bounds and faults passed')

    def test_disk_geometry_and_flush(self):
        def prepare(root, source):
            directory = source / 'src/add-ons/kernel/drivers/disk/mmc'
            header = (directory / 'mmc_disk.h').read_text()
            (root / 'mmc_disk.h').write_text('\n'.join(
                line for line in header.splitlines() if not line.startswith('#include')))
            code = (directory / 'mmc_disk.cpp').read_text()
            sections = [code[code.index('struct mmc_disk_csd {'):code.index('static float')],
                code[code.index('static status_t\nmmc_block_get_geometry'):
                     code.index('static status_t\nmmc_disk_init_driver')],
                code[code.index('static status_t\nmmc_block_open'):
                     code.index('static status_t\nmmc_block_ioctl')]]
            (root / 'disk.inc').write_text('\n'.join(sections))
        self.compile_run('test_mmc_disk', prepare, 'MMC geometry, flush and media restrictions passed')

    def test_bus_partial_initialization(self):
        def prepare(root, source):
            directory = source / 'src/add-ons/kernel/bus_managers/mmc'
            header = (directory / 'mmc_bus.h').read_text()
            (root / 'bus.h').write_text('#ifndef MMC_TEST_BUS_H\n#define MMC_TEST_BUS_H\n'
                                       + header[header.index('extern device_manager_info'):])
            code = (directory / 'mmc_bus.cpp').read_text()
            (root / 'bus.inc').write_text(code[code.index('MMCBus::MMCBus('):
                                              code.index('void\nMMCBus::Rescan')])
        self.compile_run('test_mmc_bus', prepare, 'MMC bus partial initialization cleanup passed')

    def test_rk3588_resource_and_clock_profile(self):
        def prepare(root, source):
            header = source / 'src/add-ons/kernel/busses/mmc/rk3588_profile.h'
            (root / header.name).write_bytes(header.read_bytes())
        self.compile_run('test_mmc_profile', prepare, 'RK3588 MMC admission and clock sequencing passed')

    def test_arm64_dma_allocation_failures(self):
        def prepare(root, source):
            code = (source / 'src/add-ons/kernel/busses/mmc/sdhci_dma.cpp').read_text()
            # The allocator is unchanged; only the ARM cache instructions and
            # kernel VM primitives are replaced by observed host fixtures.
            (root / 'dma.inc').write_text(code[code.index('status_t\nsdhci_allocate_dma'):])
        self.compile_run('test_mmc_dma', prepare, 'SDHCI DMA allocation and cleanup passed')

    def test_dma_resource_low_address_bounds(self):
        def prepare(root, source):
            directory = source / 'src/system/kernel/device_manager'
            header = (directory / 'dma_resources.h').read_text()
            (root / 'dma_resources.h').write_text('\n'.join(
                line for line in header.splitlines() if not line.startswith('#include')))
            code = (directory / 'dma_resources.cpp').read_text()
            sections = [code[code.index('DMABuffer*\nDMABuffer::Create'):
                             code.index('status_t\nDMAResource::Init')],
                        code[code.index('inline void\nDMAResource::_RestrictBoundaryAndSegmentSize'):
                             code.index('#if 0')]]
            (root / 'dma_bounds.inc').write_text('\n'.join(sections))
        self.compile_run('test_dma_resource_bounds', prepare,
                         'DMA low-address request and device-end bounds passed')

    def test_mmc_initialization_after_sd_probe(self):
        def prepare(root, source):
            code = (source / 'src/add-ons/kernel/bus_managers/mmc/mmc_bus.cpp').read_text()
            (root / 'initialization.inc').write_text(code[
                code.index('static status_t\ninitialize_mmc'):code.index('status_t\nMMCBus::_WorkerThread')])
        self.compile_run('test_mmc_initialization', prepare, 'MMC reset after SD probe and OCR failures passed')

    def test_mmc_width_and_data_validation(self):
        def prepare(root, source):
            code = (source / 'src/add-ons/kernel/bus_managers/mmc/mmc_bus.cpp').read_text()
            (root / 'width.inc').write_text(code[
                code.index('static status_t\nverify_mmc_extended_csd'):
                code.index('static status_t\ninitialize_mmc')])
        self.compile_run('test_mmc_width', prepare, 'MMC width sequencing and data corruption rejection passed')

    def test_mmc_cache_busy_completion(self):
        def prepare(root, source):
            directory = source / 'src/add-ons/kernel/bus_managers/mmc'
            code = (directory / 'mmc_bus.cpp').read_text()
            module = (directory / 'mmc_module.cpp').read_text()
            (root / 'cache_busy.inc').write_text(
                code[code.index('status_t\nMMCBus::ExecuteCommand'):
                     code.index('status_t\nMMCBus::DoIO')]
                + module[module.index('static status_t\nmmc_bus_execute_command'):
                         module.index('static status_t\nmmc_bus_do_io')])
        self.compile_run('test_mmc_cache_busy', prepare,
                         'MMC cache busy completion and bus serialization passed')

    def test_mmc_cache_configuration(self):
        def prepare(root, source):
            code = (source / 'src/add-ons/kernel/bus_managers/mmc/mmc_bus.cpp').read_text()
            (root / 'cache_config.inc').write_text(code[
                code.index('static status_t\nverify_mmc_extended_csd'):
                code.index('static status_t\ninitialize_mmc')])
        self.compile_run('test_mmc_cache_config', prepare,
                         'MMC cache capability and state verification passed')


if __name__ == '__main__':
    unittest.main()
