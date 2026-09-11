"""Failure-path checks for the private lab command transport."""

import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch

import lab
import shell
import shell_image


class ShellTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        self.work = Path(self.directory.name)
        (self.work / 'state').mkdir()
        self.patcher = patch.object(lab, 'WORK', self.work)
        self.patcher.start()

    def tearDown(self):
        self.patcher.stop()
        self.directory.cleanup()

    def test_disconnect_preserves_partial_output_and_redacts_password(self):
        client = Mock()
        client.read_very_eager.side_effect = [b'partial secret-token output\n', EOFError('closed')]
        output = self.work / 'partial.txt'
        with patch.object(shell.time, 'sleep'):
            with self.assertRaises(EOFError):
                shell.execute(client, 'echo example', output, {'password': 'secret-token'})
        self.assertEqual(output.read_text(), 'partial [REDACTED] output\n')
        self.assertEqual(output.stat().st_mode & 0o777, 0o600)
        self.assertEqual(json.loads(output.with_suffix('.json').read_text())['status'], 'error')

    def test_failed_guest_command_is_not_a_transport_success(self):
        client = Mock()

        def accept(payload):
            begin = re.search(rb'ROCK5_BEGIN_[0-9a-f]+', payload)[0]
            end = re.search(rb'ROCK5_END_[0-9a-f]+', payload)[0]
            client.read_very_eager.return_value = b'\n' + begin + b'\r\nfailed\n' + end + b':7\r\n'

        client.write.side_effect = accept
        output = self.work / 'failed.txt'
        with self.assertRaisesRegex(RuntimeError, 'exit status 7'):
            shell.execute(client, 'exit 7', output, {'password': 'secret-token'})
        result = json.loads(output.with_suffix('.json').read_text())
        self.assertEqual(result['status'], 'error')
        self.assertEqual(result['exit_status'], 7)

    def test_non_usb_target_never_starts_an_ssh_process(self):
        with patch.object(shell.subprocess, 'Popen') as process:
            with self.assertRaises(ValueError):
                with shell.tunnel({}, '192.168.1.1', self.work / 'outside.txt'):
                    self.fail('Tunnel opened')
            process.assert_not_called()

    def test_shared_credentials_are_rejected(self):
        path = shell_image.credentials_path()
        path.write_text('{}')
        path.chmod(0o644)
        with self.assertRaisesRegex(RuntimeError, '0600'):
            shell_image.read_credentials()

    def test_upload_cannot_escape_guest_lab_directory(self):
        source = self.work / 'payload'
        source.write_bytes(b'payload')
        with patch.object(shell, 'run_commands') as run:
            with self.assertRaises(ValueError):
                shell.upload({}, '10.239.6.146', source, '../etc/passwd', self.work / 'upload.txt')
            run.assert_not_called()

    def test_binary_receiver_rejects_a_truncated_stream(self):
        ready = b'{"address":"10.239.6.1","port":12345}\n'
        wire = ready + struct.pack('!Q', 4096) + b'partial file'
        start_process = subprocess.Popen

        def peer(*args, **kwargs):
            return start_process([sys.executable, '-c',
                                  'import sys; sys.stdout.buffer.write(' + repr(wire) + ')'],
                                 **kwargs)

        destination = self.work / 'partial.bin'
        with patch.object(shell.subprocess, 'Popen', side_effect=peer):
            with self.assertRaisesRegex(EOFError, 'Truncated download'):
                with shell.binary_receiver({'ssh_config': str(self.work / 'config'),
                                            'nanokvm_ssh': 'unused'}, '10.239.6.146',
                                           destination, self.work / 'receive.txt'):
                    pass
        self.assertEqual(destination.read_bytes(), b'partial file')
        self.assertEqual(destination.stat().st_mode & 0o777, 0o600)

    def test_download_does_not_overwrite_existing_file(self):
        destination = self.work / 'saved.bin'
        destination.write_bytes(b'previous result')
        with patch.object(shell, 'binary_receiver') as receiver:
            with self.assertRaises(FileExistsError):
                shell.download({}, '10.239.6.146', 'file.bin', destination,
                               self.work / 'download.txt')
            receiver.assert_not_called()
        self.assertEqual(destination.read_bytes(), b'previous result')

    def test_large_command_drains_replies_before_send_completes(self):
        # Force both TCP windows to fill: the peer replies before consuming the
        # rest of the command. A send-all-then-read client deadlocks here.
        errors = []
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(10)

            def peer():
                try:
                    with listener.accept()[0] as connection:
                        connection.settimeout(10)
                        connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16384)
                        data = connection.recv(4096)
                        begin = re.search(rb'ROCK5_BEGIN_[0-9a-f]+', data)[0]
                        connection.sendall(b'\n' + begin + b'\n' + b'x' * 262144 + b'\n')
                        while not data.endswith(b'"$rock5_result"\r\n'):
                            chunk = connection.recv(16384)
                            if not chunk:
                                raise EOFError('Command was truncated')
                            data += chunk
                        end = re.search(rb'ROCK5_END_[0-9a-f]+', data)[0]
                        connection.sendall(b'\n' + end + b':0\r\n')
                except BaseException as error:
                    errors.append(error)

            worker = threading.Thread(target=peer, daemon=True)
            worker.start()
            with shell.telnetlib.Telnet('127.0.0.1', listener.getsockname()[1], 10) as client:
                client.get_socket().setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 16384)
                result = shell.execute(client, '#' * 524288, self.work / 'duplex.txt',
                                       {'password': 'secret-token'}, timeout=10)
            worker.join(timeout=10)
            self.assertFalse(worker.is_alive())
            self.assertEqual(errors, [])
            self.assertEqual(result['status'], 'pass')


if __name__ == '__main__':
    unittest.main()
