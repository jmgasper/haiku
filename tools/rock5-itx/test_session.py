"""Exercise the session's clean-finish action and emergency fallback reporting."""
import contextlib
import io
import json
import os
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import Mock, patch

import lab
import nanokvm


class SessionTests(unittest.TestCase):
    def test_failed_shutdown_recovery_uses_fallback_but_keeps_failure(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as directory:
            work = Path(directory)
            (work / 'state').mkdir()
            config = work / 'state/lab.json'
            config.write_text(json.dumps({'nanokvm_url': 'http://test.invalid',
                'serial_trial_baud': 1500000, 'serial_baud': 1500000}))
            manifest = work / 'image.json'
            manifest.write_text(json.dumps({'sha256': 'pinned-image'}))
            qemu = work / 'qemu.json'
            qemu.write_text(json.dumps({'status': 'pass', 'artifact': {'sha256': 'pinned-image'}}))
            serial = Mock()
            serial.stop.return_value = {'exit_status': 0, 'errors': []}
            args = ['session.py', str(manifest), str(qemu)]
            with patch.object(lab, 'WORK', work), patch.object(lab, 'CONFIG', config), \
                    patch.object(lab, 'boot_id', return_value='previous-linux-boot'), \
                    patch.object(lab, 'start_serial', return_value=serial), \
                    patch.object(lab, 'deploy', return_value={'sha256': 'pinned-image'}), \
                    patch.object(lab, 'recover', side_effect=[RuntimeError('USB still connected'),
                        {'boot_id': 'recovered-linux-boot'}]) as recover, \
                    patch.object(nanokvm, 'BASE'), patch.object(nanokvm, 'SESSION'), \
                    patch.object(nanokvm, 'api'), patch.object(nanokvm, 'input_client'), \
                    patch('sys.argv', args), patch('signal.signal'), \
                    patch('os.path.ismount', return_value=True), \
                    patch('select.select', return_value=([0], [], [])), \
                    patch('os.read', return_value=b'{"action":"finish_stopped"}\n'), \
                    contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as exit_status:
                    runpy.run_path(str(lab.SOURCE / 'tools/rock5-itx/session.py'), run_name='__main__')
            self.assertEqual(exit_status.exception.code, 1)
            self.assertEqual(recover.call_count, 2)
            self.assertEqual(recover.call_args_list[0].kwargs, {'shutdown_serial': serial})
            self.assertEqual(recover.call_args_list[1].kwargs, {})
            result = json.loads((work / 'state/rock5-desktop-interactive.json').read_text())
            self.assertEqual(result['status'], 'error')
            self.assertEqual(result['shutdown_recovery_error'], 'USB still connected')
            self.assertEqual(result['recovery']['boot_id'], 'recovered-linux-boot')


if __name__ == '__main__':
    unittest.main()
