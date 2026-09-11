"""Regression checks for deployment integrity and recovery after failed trials."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

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
            'started_utc': 'start', 'finished_utc': 'finish', 'haiku_revision': 'hrev1+1',
            'inputs': {'source_status': '', 'source_revision': 'built-revision',
                       'source_dirty': False, 'buildtools_revision': 'built-tools'},
        }))
        with patch.object(lab, 'run', return_value='different-current-revision'):
            result = lab.artifact(image)
        self.assertEqual(result['source_revision'], 'built-revision')
        self.assertEqual(result['buildtools_revision'], 'built-tools')

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
