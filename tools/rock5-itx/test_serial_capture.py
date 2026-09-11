"""Exercise the capture worker against a real PTY, including broken transports."""
import fcntl
import os
from pathlib import Path
import pty
import sys
import tempfile
import termios
import time
import unittest

from serial_capture import Capture, worker_code


class SerialTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        self.output = Path(self.directory.name) / 'serial.log'
        self.master, self.slave = pty.openpty()
        self.previous = termios.tcgetattr(self.slave)
        self.capture = None

    def tearDown(self):
        if self.capture:
            try:
                self.capture.stop()
            except RuntimeError:
                pass
        os.close(self.master)
        os.close(self.slave)
        self.directory.cleanup()

    def start(self, code=None):
        self.capture = Capture([sys.executable, '-u', '-c', code or
                                worker_code(os.ttyname(self.slave), 1500000)], self.output)
        return self.capture

    def wait_for(self, predicate):
        deadline = time.monotonic() + 3
        while not predicate():
            if time.monotonic() > deadline:
                self.fail('Capture did not reach expected state')
            time.sleep(0.01)

    def test_binary_capture_and_terminal_restoration(self):
        capture = self.start()
        payload = bytes(range(256)) * 8
        os.write(self.master, payload)
        self.wait_for(lambda: capture.bytes == len(payload))
        result = capture.stop()
        self.assertEqual(self.output.read_bytes(), payload)
        self.assertEqual(result['status'], 'captured')
        self.assertEqual(termios.tcgetattr(self.slave), self.previous)

    def test_busy_port_fails_before_changing_terminal(self):
        fcntl.flock(self.slave, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with self.assertRaises(RuntimeError):
            self.start()
        self.assertEqual(termios.tcgetattr(self.slave), self.previous)
        self.assertIn('BlockingIOError', self.output.with_suffix('.stderr.log').read_text())

    def test_baud_change_preserves_stream_and_restores_original_terminal(self):
        capture = self.start()
        os.write(self.master, b'firmware')
        self.wait_for(lambda: capture.bytes == 8)
        capture.set_baud(115200)
        self.assertEqual(termios.tcgetattr(self.slave)[4:6], [termios.B115200] * 2)
        os.write(self.master, b'Haiku')
        self.wait_for(lambda: capture.bytes == 13)
        capture.set_baud(1500000)
        result = capture.stop()
        self.assertEqual(self.output.read_bytes(), b'firmwareHaiku')
        self.assertEqual(result['baud_changes'], [
            {'serial_baud': 115200, 'byte_offset': 8},
            {'serial_baud': 1500000, 'byte_offset': 13}])
        self.assertEqual(termios.tcgetattr(self.slave), self.previous)

    def test_invalid_baud_does_not_disconnect_capture(self):
        capture = self.start()
        with self.assertRaises(ValueError):
            capture.set_baud(0)
        capture.check()

    def test_dropped_transport_is_reported_and_settings_restored(self):
        capture = self.start()
        capture.process.terminate()
        capture.process.wait(timeout=3)
        with self.assertRaises(RuntimeError):
            capture.check()
        with self.assertRaises(RuntimeError):
            capture.stop()
        self.assertEqual(termios.tcgetattr(self.slave), self.previous)
        self.assertIn('"status": "error"', self.output.with_suffix('.json').read_text())

    def test_readiness_cannot_be_inferred_from_an_open_process(self):
        with self.assertRaisesRegex(RuntimeError, 'readiness timeout'):
            Capture([sys.executable, '-c', 'import time; time.sleep(30)'],
                    self.output, startup_timeout=0.1)


if __name__ == '__main__':
    unittest.main()
