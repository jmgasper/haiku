"""Check the native stream helper against an independent wire fixture."""
import concurrent.futures
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import unittest


TOKEN = '9a' * 32
SEED = 18446744073709551610
SIZE = 131079  # Includes two full blocks and a partial final word.


def pattern(size, seed):
    mask = (1 << 64) - 1
    data = bytearray()
    for index in range((size + 7) // 8):
        word = (seed + index + 0x9e3779b97f4a7c15) & mask
        word = ((word ^ (word >> 30)) * 0xbf58476d1ce4e5b9) & mask
        word = ((word ^ (word >> 27)) * 0x94d049bb133111eb) & mask
        data.extend(struct.pack('<Q', word ^ (word >> 31)))
    return bytes(data[:size])


def read_exactly(connection, count):
    data = bytearray()
    while len(data) < count:
        chunk = connection.recv(count - len(data))
        if not chunk:
            raise EOFError('Truncated test stream')
        data.extend(chunk)
    return bytes(data)


class NetworkProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.binary = Path(cls.temporary.name) / 'network-probe'
        source = Path(__file__).with_name('network_probe.cpp')
        subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(source), '-o', str(cls.binary)], check=True)
        cls.fixture = pattern(SIZE, SEED)

    def header(self, direction, token=TOKEN):
        return token.encode() + b'\n' + struct.pack('!QQ', SIZE, SEED) + direction

    def client(self, direction, serve):
        with socket.socket() as listener, concurrent.futures.ThreadPoolExecutor(1) as pool:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(5)

            def peer():
                connection, address = listener.accept()
                with connection:
                    self.assertEqual(address[0], '127.0.0.1')
                    connection.settimeout(5)
                    self.assertEqual(read_exactly(connection, 82),
                                     self.header(b'R' if direction == 'receive' else b'S'))
                    connection.sendall(b'R')
                    serve(connection)

            future = pool.submit(peer)
            result = subprocess.run([str(self.binary), direction, '127.0.0.1',
                                     str(listener.getsockname()[1]), TOKEN,
                                     str(SIZE), str(SEED), '127.0.0.1'],
                                    capture_output=True, timeout=10)
            future.result(timeout=5)
            return result

    def test_client_receive_and_send(self):
        def send(connection):
            connection.sendall(self.fixture)
            self.assertEqual(read_exactly(connection, 1), b'P')

        def receive(connection):
            self.assertEqual(read_exactly(connection, SIZE), self.fixture)
            connection.sendall(b'P')

        for direction, serve in [('receive', send), ('send', receive)]:
            result = self.client(direction, serve)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b'ROCK5_NETWORK_PASS', result.stderr)

    def test_corruption_and_truncation_rejected(self):
        bad = bytearray(self.fixture)
        bad[65540] ^= 1
        for data in (bad, self.fixture[:65536]):
            def send(connection):
                connection.sendall(data)
                connection.shutdown(socket.SHUT_WR)
            result = self.client('receive', send)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertNotIn(b'ROCK5_NETWORK_PASS', result.stderr)

    def test_sender_requires_checked_acknowledgement(self):
        def receive(connection):
            self.assertEqual(read_exactly(connection, SIZE), self.fixture)
            connection.sendall(b'F')
        result = self.client('send', receive)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertNotIn(b'ROCK5_NETWORK_PASS', result.stderr)

    def test_peer_roles_and_header_rejection(self):
        for direction, header in [('peer-send', self.header(b'R')),
                                  ('peer-receive', self.header(b'S')),
                                  ('peer-send', self.header(b'R', '0' * 64)),
                                  ('peer-send', self.header(b'S'))]:
            parent, child = socket.socketpair()
            with parent, child:
                parent.settimeout(5)
                process = subprocess.Popen([str(self.binary), direction, TOKEN, str(SIZE), str(SEED)],
                                           stdin=child, stdout=child, stderr=subprocess.PIPE)
                child.close()
                try:
                    parent.sendall(header)
                    valid = header == self.header(b'R' if direction == 'peer-send' else b'S')
                    if valid:
                        self.assertEqual(read_exactly(parent, 1), b'R')
                        if direction == 'peer-send':
                            self.assertEqual(read_exactly(parent, SIZE), self.fixture)
                            parent.sendall(b'P')
                        else:
                            parent.sendall(self.fixture)
                            self.assertEqual(read_exactly(parent, 1), b'P')
                    _, error = process.communicate(timeout=5)
                    self.assertEqual(process.returncode, 0 if valid else 1, error)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate()

    def test_argument_bounds(self):
        for size, seed in [('0', '1'), ('536870913', '1'), ('-1', '1'),
                           ('+1', '1'), ('1', '18446744073709551616')]:
            result = subprocess.run([str(self.binary), 'peer-send', TOKEN, size, seed],
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 2, result.stderr)


if __name__ == '__main__':
    unittest.main()
