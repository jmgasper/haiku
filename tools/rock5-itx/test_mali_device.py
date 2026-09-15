import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MaliDeviceTest(unittest.TestCase):
    def test_kernel_allocation_ioctl_and_interrupt_lifetime(self):
        directory = Path(__file__).resolve().parent
        source = directory.parents[1] / 'src/add-ons/kernel/drivers/graphics/mali_csf'
        code = (source / 'CsfDevice.cpp').read_text().replace(
            '#include "CsfDmaMemory.h"', (source / 'CsfDmaMemory.h').read_text())
        code = code[code.index('using namespace MaliCSF;'):]
        begin = code.index('static status_t\nMakeFirmwareRamNoncacheable')
        end = code.index('template<typename Memory>\nstatic area_id\nAllocateFirmwareMemory', begin)
        # Only CPU memory-type/cache instructions are supplied by the fixture.
        # Allocation limits, copying, cleanup, request and IRQ code are production.
        code = code[:begin] + code[end:]
        with tempfile.TemporaryDirectory(prefix='mali-device-') as temporary:
            root = Path(temporary)
            (root / 'device.inc').write_text(code)
            binary = root / 'device-test'
            result = subprocess.run([
                'g++', '-pthread', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                '-I', str(source), '-I', str(root), str(directory / 'test_mali_device.cpp'),
                str(source / 'CsfFirmware.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                timeout=30, env=dict(os.environ,
                    ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                    UBSAN_OPTIONS='halt_on_error=1'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('MALI_CSF_DEVICE_TEST_PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
