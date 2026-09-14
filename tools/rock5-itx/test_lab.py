"""Regression checks for deployment integrity and recovery after failed trials."""
import json
import contextlib
import os
from pathlib import Path
import tempfile
import struct
import unittest
from unittest.mock import Mock, patch

import lab


class LabTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        self.work = Path(self.directory.name)
        (self.work / 'state').mkdir()
        self.patcher = patch.object(lab, 'WORK', self.work)
        self.patcher.start()
        self.config = {'recovery_image': '/data/recovery.img'}

    def tearDown(self):
        self.patcher.stop()
        self.directory.cleanup()

    def manifest(self):
        image = self.work / 'test.img'
        image.write_bytes(b'original boot image')
        path = self.work / 'test.json'
        path.write_text(json.dumps({'image': str(image), 'sha256': lab.digest(image),
                                    'bytes': image.stat().st_size}))
        return path, image

    def test_corrupted_artifact_never_reaches_hardware(self):
        manifest, image = self.manifest()
        image.write_bytes(b'changed boot image!')
        with patch.object(lab, 'gadget') as gadget:
            with self.assertRaisesRegex(RuntimeError, 'checksum'):
                lab.deploy(self.config, manifest)
            gadget.assert_not_called()

    def test_exported_data_partition_prevents_upload(self):
        manifest, _ = self.manifest()
        with patch.object(lab, 'gadget', return_value={'file': '/dev/mmcblk0p3'}), \
                patch.object(lab, 'attach') as attach:
            with self.assertRaisesRegex(RuntimeError, 'Whole /data'):
                lab.deploy(self.config, manifest)
            attach.assert_not_called()

    def test_packaging_uses_build_revision_not_current_checkout(self):
        _manifest, image = self.manifest()
        build = self.work / 'build/arm64'
        build.mkdir(parents=True)
        (build / 'build-record.json').write_text(json.dumps({
            'sha256': lab.digest(image), 'target': '@minimum-mmc',
            'started_utc': 'start', 'finished_utc': 'finish', 'haiku_revision': 'hrev1+1', 'layout': {},
            'inputs': {'source_status': '', 'source_revision': 'built-revision',
                       'source_dirty': False, 'buildtools_revision': 'built-tools'},
        }))
        with patch.object(lab, 'run', return_value='different-current-revision'):
            result = lab.artifact(image)
        self.assertEqual(result['source_revision'], 'built-revision')
        self.assertEqual(result['buildtools_revision'], 'built-tools')

    def test_zero_length_efi_loader_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'empty'):
            lab.efi_metadata(b'')

    def test_x86_efi_loader_is_rejected_for_arm64(self):
        data = bytearray(256)
        data[:2] = b'MZ'
        struct.pack_into('<I', data, 0x3c, 64)
        data[64:68] = b'PE\0\0'
        struct.pack_into('<H', data, 68, 0x8664)
        struct.pack_into('<H', data, 88, 0x20b)
        struct.pack_into('<H', data, 156, 10)
        with self.assertRaisesRegex(RuntimeError, 'ARM64'):
            lab.efi_metadata(data)

    def test_failed_deployment_still_recovers_and_preserves_error(self):
        with patch.object(lab.nanokvm, 'api'), patch.object(lab, 'boot_id', return_value='before'), \
                patch.object(lab, 'remote_python'), \
                patch.object(lab, 'deploy', side_effect=RuntimeError('upload failed')), \
                patch.object(lab, 'recover', return_value={'boot_id': 'after'}) as recover:
            result = lab.cycle(self.config, 'unused', 5)
        recover.assert_called_once()
        self.assertEqual(result['status'], 'error')
        self.assertEqual(result['error'], 'upload failed')
        self.assertTrue((Path(result['evidence']) / 'result.json').is_file())

    def test_failed_recovery_is_never_reported_as_success(self):
        with patch.object(lab.nanokvm, 'api'), patch.object(lab, 'boot_id', return_value='before'), \
                patch.object(lab, 'remote_python'), \
                patch.object(lab, 'deploy', side_effect=RuntimeError('trial failed')), \
                patch.object(lab, 'recover', side_effect=RuntimeError('ROOBI missing')):
            result = lab.cycle(self.config, 'unused', 5)
        self.assertEqual(result['status'], 'recovery_failed')
        self.assertEqual(result['recovery_error'], 'ROOBI missing')

    def test_missing_video_does_not_prevent_recovery(self):
        with patch.object(lab.nanokvm, 'api'), patch.object(lab, 'boot_id', return_value='before'), \
                patch.object(lab, 'remote_python'), patch.object(lab, 'deploy', return_value={}), \
                patch.object(lab.time, 'monotonic', side_effect=[0, 0, 6, 6]), \
                patch.object(lab.time, 'sleep'), \
                patch.object(lab.nanokvm, 'screenshot', side_effect=TimeoutError('no HDMI')), \
                patch.object(lab, 'recover', return_value={'boot_id': 'after'}) as recover:
            result = lab.cycle(self.config, 'unused', 5)
        recover.assert_called_once()
        self.assertEqual(result['captures'], [{'capture_error': 'no HDMI'}])
        self.assertEqual(result['status'], 'observed')

    def test_serial_start_failure_prevents_deployment_and_reset(self):
        with patch.object(lab.nanokvm, 'api'), patch.object(lab, 'boot_id', return_value='before'), \
                patch.object(lab, 'remote_python'), \
                patch.object(lab, 'start_serial', side_effect=RuntimeError('UART unavailable')), \
                patch.object(lab, 'deploy') as deploy, patch.object(lab, 'recover') as recover:
            result = lab.cycle(self.config, 'unused', 5)
        deploy.assert_not_called()
        recover.assert_not_called()
        self.assertEqual(result['status'], 'error')

    def test_serial_failure_preserves_failed_recovery_and_result_file(self):
        serial = Mock()
        serial.stop.side_effect = RuntimeError('SSH dropped')
        with patch.object(lab.nanokvm, 'api'), patch.object(lab, 'boot_id', return_value='before'), \
                patch.object(lab, 'remote_python'), patch.object(lab, 'start_serial', return_value=serial), \
                patch.object(lab, 'deploy', side_effect=RuntimeError('trial failed')), \
                patch.object(lab, 'recover', side_effect=RuntimeError('ROOBI missing')):
            result = lab.cycle(self.config, 'unused', 5)
        self.assertEqual(result['status'], 'recovery_failed')
        self.assertEqual(result['serial_error'], 'SSH dropped')
        self.assertEqual(json.loads((Path(result['evidence']) / 'result.json').read_text()), result)

    def test_trial_and_recovery_use_their_configured_serial_speeds(self):
        serial = Mock()
        serial.stop.return_value = {'status': 'captured'}
        config = dict(self.config, serial_baud=1500000, serial_trial_baud=115200)
        with patch.object(lab.nanokvm, 'api'), patch.object(lab, 'boot_id', return_value='before'), \
                patch.object(lab, 'remote_python'), patch.object(lab, 'start_serial', return_value=serial), \
                patch.object(lab, 'deploy', return_value={}), \
                patch.object(lab.time, 'monotonic', side_effect=[0, 6]), \
                patch.object(lab, 'recover', return_value={'boot_id': 'after'}):
            result = lab.cycle(config, 'unused', 5)
        self.assertEqual([call.args[0] for call in serial.set_baud.call_args_list],
                         [115200, 1500000])
        self.assertEqual(result['status'], 'observed')

    def test_baud_control_failure_does_not_prevent_recovery(self):
        serial = Mock()
        serial.set_baud.side_effect = RuntimeError('SSH dropped')
        serial.stop.return_value = {'status': 'captured'}
        config = dict(self.config, serial_trial_baud=115200)
        with patch.object(lab.nanokvm, 'api'), patch.object(lab, 'boot_id', return_value='before'), \
                patch.object(lab, 'remote_python'), patch.object(lab, 'start_serial', return_value=serial), \
                patch.object(lab, 'deploy', return_value={}), \
                patch.object(lab, 'recover', return_value={'boot_id': 'after'}) as recover:
            result = lab.cycle(config, 'unused', 5)
        recover.assert_called_once()
        self.assertEqual(result['status'], 'error')
        self.assertEqual(result['serial_error'], 'SSH dropped')

    def test_recovery_waits_for_configured_efi_boot_duration(self):
        with patch.object(lab, 'boot_id', return_value='before'), \
                patch.object(lab, 'attach'), patch.object(lab.nanokvm, 'api'), \
                patch.object(lab, 'wait_recovery', return_value='after') as wait:
            lab.recover(dict(self.config, recovery_timeout=180))
        self.assertEqual(wait.call_args.kwargs['seconds'], 180)

    def test_recovery_media_rejects_late_writes_from_previous_guest(self):
        medium = self.work / 'recovery.img'
        original = b'known recovery kernel and initrd'
        medium.write_bytes(original)
        selected = {}
        attempts = []

        def late_write(stage):
            attempts.append(stage)
            if not selected['readonly']:
                medium.write_bytes(b'late filesystem journal write')

        def select(_config, name, readonly=False):
            selected.update(name=name, readonly=readonly)
            late_write('media selected before reset')

        def gpio(path, body):
            self.assertEqual(path, '/api/vm/gpio')
            self.assertEqual(body['type'], 'reset')
            late_write('reset not yet effective')

        with patch.object(lab, 'boot_id', return_value='before'), \
                patch.object(lab, 'attach', side_effect=select), \
                patch.object(lab.nanokvm, 'api', side_effect=gpio), \
                patch.object(lab, 'wait_recovery', return_value='after'):
            result = lab.recover(self.config)
        self.assertEqual(result['boot_id'], 'after')
        self.assertEqual(attempts, ['media selected before reset', 'reset not yet effective'])
        self.assertEqual(medium.read_bytes(), original)

    @contextlib.contextmanager
    def shutdown_fixture(self, raw, usb='not attached', poll=None):
        path = self.work / 'serial.log'
        path.write_bytes(raw)
        serial = Mock(output=path)
        clock = [0.0]
        actions = []
        controller = '84b69ae8-4d9e-4c03-9f5c-6c9547084819'

        def read_state(*_args, **_kwargs):
            if poll:
                poll(clock[0], path, serial)
            return controller + '\n' + usb

        with patch.object(lab, 'boot_id', return_value=None), \
                patch.object(lab.time, 'monotonic', side_effect=lambda: clock[0]), \
                patch.object(lab.time, 'sleep', side_effect=lambda n: clock.__setitem__(0, clock[0] + n)), \
                patch.object(lab, 'ssh', side_effect=read_state) as ssh, \
                patch.object(lab, 'attach', side_effect=lambda *a, **kw: actions.append(('attach', clock[0], a, kw))), \
                patch.object(lab.nanokvm, 'api', side_effect=lambda *a: actions.append(('gpio', clock[0], a))), \
                patch.object(lab, 'wait_recovery', return_value='new-linux-boot'):
            yield serial, actions, ssh

    def test_shutdown_recovery_changes_media_only_after_quiet_disconnection(self):
        raw = b'Haiku revision: test\r\nPSCI: requesting system off\r\n\x00\xfe\xff'
        with self.shutdown_fixture(raw) as (serial, actions, _ssh):
            result = lab.recover(self.config, shutdown_serial=serial)
        self.assertEqual([a[0] for a in actions], ['attach', 'gpio'])
        self.assertGreaterEqual(actions[0][1], 25)
        self.assertEqual(actions[0][3], {'readonly': True})
        self.assertEqual(actions[1][2], ('/api/vm/gpio', {'type': 'power', 'duration': 800}))
        self.assertEqual(result['method'], 'after_verified_shutdown')
        self.assertEqual(result['off_observation']['serial_bytes'], len(raw))
        self.assertEqual(result['off_observation']['serial_sha256'], lab.digest(serial.output))

    def test_incomplete_or_stale_shutdown_never_changes_media(self):
        marker = b'PSCI: requesting system off\r\n'
        cases = [(b'Haiku running', 'not attached'), (marker, 'configured'),
            (marker + b'DDR new boot', 'not attached'),
            (marker + b'PANIC: bad filesystem', 'not attached')]
        for raw, usb in cases:
            with self.subTest(raw=raw, usb=usb), \
                    self.shutdown_fixture(raw, usb) as (serial, actions, _ssh):
                with self.assertRaises(RuntimeError):
                    lab.recover(self.config, shutdown_serial=serial)
                self.assertEqual(actions, [])

    def test_shutdown_observation_rejects_controller_restart_and_lost_capture(self):
        raw = b'PSCI: requesting system off\r\n'
        with self.shutdown_fixture(raw) as (serial, actions, ssh):
            ssh.side_effect = ['84b69ae8-4d9e-4c03-9f5c-6c9547084819\nnot attached',
                '84b69ae8-4d9e-4c03-9f5c-6c9547084820\nnot attached']
            with self.assertRaisesRegex(RuntimeError, 'NanoKVM restarted'):
                lab.recover(self.config, shutdown_serial=serial)
            self.assertEqual(actions, [])
        def lost_capture(now, _path, serial):
            if now >= 24:
                serial.check.side_effect = RuntimeError('Serial transport ended')
        with self.shutdown_fixture(raw, poll=lost_capture) as (serial, actions, _ssh):
            with self.assertRaisesRegex(RuntimeError, 'Serial transport ended'):
                lab.recover(self.config, shutdown_serial=serial)
            self.assertEqual(actions, [])

    def test_shutdown_quiet_period_restarts_when_more_serial_arrives(self):
        raw = b'PSCI: requesting system off\r\n'
        def more_data(now, path, _serial):
            if now == 20:
                path.write_bytes(raw + b'\x00\xfe')
        with self.shutdown_fixture(raw, poll=more_data) as (serial, actions, _ssh):
            result = lab.recover(self.config, shutdown_serial=serial)
        self.assertGreaterEqual(actions[0][1], 45)
        self.assertGreaterEqual(result['off_observation']['quiet_seconds'], 25)

    def test_symlink_cannot_escape_project_drive(self):
        (self.work / 'escape').symlink_to('/etc')
        with self.assertRaises(ValueError):
            lab.local_path(self.work / 'escape/passwd')

    def test_remote_image_rejects_traversal_and_shell_text(self):
        for value in ('/data/../etc/disk.img', '/data/a;reboot.img', '/data/a/b.img', '/dev/sda'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                lab.remote_image(value)

    def test_lock_excludes_second_operation(self):
        with lab.lock('hardware'):
            with self.assertRaisesRegex(RuntimeError, 'lock'):
                with lab.lock('hardware'):
                    self.fail('Concurrent operation acquired lock')


if __name__ == '__main__':
    unittest.main()
