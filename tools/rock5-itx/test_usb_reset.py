"""Interlocks and evidence for an explicit SSD-session USB reset."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import usb_reset


class USBResetTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        self.output = Path(self.directory.name)
        self.serial = self.output / 'serial.log'
        self.serial.write_bytes(b'Mounted boot partition: /dev/disk/nvme/0/1\n')
        self.config = {'recovery_image': '/data/recovery.img'}
        self.gadget = {'file': '/data/recovery.img', 'ro': '0', 'cdrom': '0'}
        self.controller = {'boot_id': 'controller-one',
                           'persistent_image': '/data/recovery.img'}
        self.work_patch = patch.object(usb_reset.lab, 'WORK', self.output)
        self.work_patch.start()
        self.mocks = [patch.object(usb_reset.lab, 'gadget', return_value=self.gadget),
                      patch.object(usb_reset, 'controller_state', return_value=self.controller),
                      patch.object(usb_reset.nanokvm, 'api'),
                      patch.object(usb_reset.shell, 'run_commands', side_effect=self.preflight)]
        self.gadget_mock, self.controller_mock, self.api, self.shell = [
            p.start() for p in self.mocks]

    def tearDown(self):
        for p in reversed(self.mocks):
            p.stop()
        self.work_patch.stop()
        self.directory.cleanup()

    def preflight(self, config, target, commands, transcript, timeout):
        transcript.write_text('ROCK5_USB_RESET_READY\n')

    def prepare(self):
        return usb_reset.prepare(self.config, {'boot_source': 'nvme'},
                                 self.output, '10.239.6.102')

    def test_usb_boot_is_rejected_before_any_device_command(self):
        with self.assertRaisesRegex(RuntimeError, 'NVMe boot'):
            usb_reset.prepare(self.config, {'boot_source': 'usb'},
                              self.output, '10.239.6.102')
        self.shell.assert_not_called()
        self.api.assert_not_called()

    def test_uart_usb_boot_cannot_use_an_nvme_deployment_receipt(self):
        self.serial.write_bytes(b'Mounted boot partition: /dev/disk/usb/0/0/1\n')
        with self.assertRaisesRegex(RuntimeError, 'UART'):
            self.prepare()
        self.shell.assert_not_called()

    def test_persistent_image_mismatch_is_rejected(self):
        self.controller_mock.return_value = {**self.controller,
                                            'persistent_image': '/dev/mmcblk0p3'}
        with self.assertRaisesRegex(RuntimeError, 'persistent'):
            self.prepare()
        self.shell.assert_not_called()
        self.api.assert_not_called()

    def test_preflight_failure_prevents_preparation(self):
        self.shell.side_effect = RuntimeError('USB filesystem is mounted')
        with self.assertRaisesRegex(RuntimeError, 'mounted'):
            self.prepare()
        self.assertFalse((self.output / 'usb-reset-preparation.json').exists())
        self.api.assert_not_called()

    def test_reboot_invalidates_preparation(self):
        preparation = self.prepare()
        with self.serial.open('ab') as stream:
            stream.write(b'UEFI firmware\n')
        with self.assertRaisesRegex(RuntimeError, 'boot changed'):
            usb_reset.reset(self.config, preparation, self.output)
        self.api.assert_not_called()

    def test_changed_controller_invalidates_preparation(self):
        preparation = self.prepare()
        self.controller_mock.return_value = {**self.controller, 'boot_id': 'controller-two'}
        with self.assertRaisesRegex(RuntimeError, 'changed'):
            usb_reset.reset(self.config, preparation, self.output)
        self.api.assert_not_called()

    def test_failed_api_is_preserved_as_a_failure(self):
        preparation = self.prepare()
        self.api.side_effect = TimeoutError('reset response missing')
        with self.assertRaises(TimeoutError):
            usb_reset.reset(self.config, preparation, self.output)
        receipts = [p for p in self.output.glob('usb-reset-*.json')
                    if p.name != 'usb-reset-preparation.json']
        self.assertEqual(len(receipts), 1)
        self.assertEqual(json.loads(receipts[0].read_text())['status'], 'error')

    def test_success_does_not_claim_reconnection(self):
        preparation = self.prepare()
        self.api.return_value = None
        result = usb_reset.reset(self.config, preparation, self.output)
        self.api.assert_called_once_with('/api/hid/reset', {})
        self.assertEqual(result['status'], 'reset_completed_reconnection_unverified')

    def test_image_change_during_reset_is_not_reported_as_success(self):
        preparation = self.prepare()
        self.api.return_value = None
        self.gadget_mock.side_effect = [self.gadget,
                                       {**self.gadget, 'file': '/data/other.img'}]
        with self.assertRaisesRegex(RuntimeError, 'changed'):
            usb_reset.reset(self.config, preparation, self.output)
        receipts = [p for p in self.output.glob('usb-reset-*.json')
                    if p.name != 'usb-reset-preparation.json']
        self.assertEqual(json.loads(receipts[0].read_text())['status'], 'error')


if __name__ == '__main__':
    unittest.main()
