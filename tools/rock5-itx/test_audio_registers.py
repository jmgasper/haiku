"""Check the ROCK 5 ITX audio clock and sample packing helpers."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class RK3588AudioRegisterTests(unittest.TestCase):
    def test_module_names_match_installed_driver_path(self):
        top = Path(__file__).resolve().parents[2]
        header = (top / 'src/add-ons/kernel/drivers/audio/rk3588/driver.h').read_text()
        compact = ''.join(header.split())
        self.assertIn(
            '"drivers/audio/hmulti/rk3588_audio/driver_v1"', compact)
        self.assertIn(
            '"drivers/audio/hmulti/rk3588_audio/device_v1"', compact)

    def test_thread_context_spinlock_disables_interrupts(self):
        top = Path(__file__).resolve().parents[2]
        hardware = (top / 'src/add-ons/kernel/drivers/audio/rk3588/hardware.cpp').read_text()
        start = hardware.index('rk3588_audio_start')
        stop = hardware.index('rk3588_audio_stop', start)
        body = hardware[start:stop]
        disable = body.index('disable_interrupts()')
        acquire = body.index('acquire_spinlock')
        release = body.index('release_spinlock')
        restore = body.index('restore_interrupts')
        self.assertLess(disable, acquire)
        self.assertLess(acquire, release)
        self.assertLess(release, restore)

    def test_force_stop_keeps_shared_buffers_alive(self):
        top = Path(__file__).resolve().parents[2]
        multi = (top / 'src/add-ons/kernel/drivers/audio/rk3588/multi_audio.cpp').read_text()
        force_stop = multi.index('case B_MULTI_BUFFER_FORCE_STOP:')
        next_case = multi.index('case B_MULTI_LIST_MIX_CONTROLS:', force_stop)
        body = multi[force_stop:next_case]
        self.assertIn('rk3588_audio_stop(controller)', body)
        self.assertNotIn('rk3588_audio_release_buffers(controller)', body)

    def test_clock_registers_and_stereo_packing(self):
        directory = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as temporary:
            binary = Path(temporary) / 'rk3588-audio-register-test'
            result = subprocess.run([
                'g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=undefined', '-fno-sanitize-recover=all',
                str(directory / 'test_audio_registers.cpp'), '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
