"""Raw serial capture with a readiness handshake and explicit transport failures."""

import inspect
import json
from pathlib import Path
import shlex
import subprocess
import sys
import threading
import time


def worker(device, baud):
    """Runs locally or through SSH; stdin controls lifetime, stdout carries bytes."""
    import fcntl
    import json
    import os
    import select
    import signal
    import sys
    import termios
    import tty

    def interrupted(signum, _frame):
        raise RuntimeError(f'Serial worker interrupted by signal {signum}')

    for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    descriptor = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    previous = None
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        previous = termios.tcgetattr(descriptor)
        speed = getattr(termios, f'B{baud}')
        tty.setraw(descriptor)
        settings = termios.tcgetattr(descriptor)
        settings[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
        settings[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
        settings[4] = settings[5] = speed
        termios.tcsetattr(descriptor, termios.TCSANOW, settings)
        print(json.dumps({'serial_ready': True, 'device': device, 'baud': baud}),
              file=sys.stderr, flush=True)
        control = b''
        received = 0
        while True:
            ready = select.select([descriptor, 0], [], [], 1)[0]
            if descriptor in ready:
                chunk = os.read(descriptor, 65536)
                if not chunk:
                    raise RuntimeError('Serial device disconnected')
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
                received += len(chunk)
            if 0 in ready:
                chunk = os.read(0, 32)
                if not chunk:
                    raise RuntimeError('Serial controller connection closed unexpectedly')
                control += chunk
                if len(control) > 64:
                    raise RuntimeError('Invalid serial control message')
                while b'\n' in control:
                    line, control = control.split(b'\n', 1)
                    if line == b'STOP':
                        return
                    if not line.startswith(b'BAUD ') or not line[5:].isdigit():
                        raise RuntimeError('Invalid serial control message')
                    baud = int(line[5:])
                    settings[4] = settings[5] = getattr(termios, f'B{baud}')
                    termios.tcsetattr(descriptor, termios.TCSANOW, settings)
                    print(json.dumps({'serial_baud': baud, 'byte_offset': received}),
                          file=sys.stderr, flush=True)
    finally:
        try:
            if previous is not None:
                termios.tcsetattr(descriptor, termios.TCSANOW, previous)
        finally:
            os.close(descriptor)


def worker_code(device, baud):
    return inspect.getsource(worker) + f'\nworker({device!r}, {int(baud)!r})\n'


class Capture:
    def __init__(self, command, output, startup_timeout=15):
        self.output = Path(output)
        self.errors = []
        self.bytes = 0
        self.ready = threading.Event()
        self.baud_changed = threading.Event()
        self.baud_changes = []
        self.stopping = threading.Event()
        self.stopped = False
        self.metadata = {}
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.threads = [threading.Thread(target=self._read_data, daemon=True),
                        threading.Thread(target=self._read_errors, daemon=True)]
        for thread in self.threads:
            thread.start()
        deadline = time.monotonic() + startup_timeout
        try:
            while not self.ready.wait(0.05):
                self.check()
                if time.monotonic() >= deadline:
                    raise RuntimeError('Serial worker readiness timeout')
            self.check()
        except Exception as error:
            self.errors.append(str(error))
            try:
                self.stop()
            except RuntimeError:
                pass
            raise

    def _read_data(self):
        try:
            with self.output.open('wb') as stream:
                while chunk := self.process.stdout.read1(65536):
                    stream.write(chunk)
                    stream.flush()
                    self.bytes += len(chunk)
            if not self.stopping.is_set():
                self.errors.append('Serial transport ended before capture was stopped')
        except Exception as error:
            self.errors.append(f'Serial log: {error}')
        finally:
            self.process.stdout.close()

    def _read_errors(self):
        try:
            with self.output.with_suffix('.stderr.log').open('wb') as stream:
                for line in self.process.stderr:
                    stream.write(line)
                    stream.flush()
                    try:
                        message = json.loads(line)
                    except (ValueError, UnicodeError):
                        continue
                    if isinstance(message, dict) and message.get('serial_ready') is True:
                        self.metadata = message
                        self.ready.set()
                    elif isinstance(message, dict) and 'serial_baud' in message:
                        self.baud_changes.append(message)
                        self.baud_changed.set()
        except Exception as error:
            self.errors.append(f'Serial diagnostics: {error}')
        finally:
            self.process.stderr.close()

    def check(self):
        if self.errors:
            raise RuntimeError('; '.join(self.errors))
        if self.process.poll() is not None and not self.stopping.is_set():
            raise RuntimeError(f'Serial transport exited with status {self.process.returncode}; '
                               f'see {self.output.with_suffix(".stderr.log")}')

    def set_baud(self, baud, timeout=10):
        """Change the receiver speed and require an acknowledgement from the worker."""
        import termios
        baud = int(baud)
        if baud <= 0 or not hasattr(termios, f'B{baud}'):
            raise ValueError('Unsupported serial baud rate')
        self.check()
        self.baud_changed.clear()
        self.process.stdin.write(f'BAUD {baud}\n'.encode())
        self.process.stdin.flush()
        deadline = time.monotonic() + timeout
        while not self.baud_changed.wait(0.05):
            self.check()
            if time.monotonic() >= deadline:
                self.errors.append('Serial baud-change acknowledgement timeout')
                self.check()
        self.check()
        if self.baud_changes[-1]['serial_baud'] != baud:
            self.errors.append('Serial worker acknowledged an unexpected baud rate')
            self.check()

    def stop(self):
        if not self.stopped:
            try:
                self.check()
            except RuntimeError as error:
                if str(error) not in self.errors:
                    self.errors.append(str(error))
            self.stopping.set()
            try:
                self.process.stdin.write(b'STOP\n')
                self.process.stdin.flush()
            except (BrokenPipeError, OSError):
                self.errors.append('Serial transport was unavailable at shutdown')
            finally:
                try:
                    self.process.stdin.close()
                except BrokenPipeError:
                    pass
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.errors.append('Serial worker failed to stop')
                self.process.terminate()
                try:
                    self.process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=3)
            for thread in self.threads:
                thread.join(timeout=3)
                if thread.is_alive():
                    self.errors.append('Serial reader thread failed to stop')
            if self.process.returncode:
                self.errors.append(f'Serial transport exit status {self.process.returncode}')
            self.stopped = True
        result = {**self.metadata, 'bytes': self.bytes, 'exit_status': self.process.returncode,
                  'baud_changes': self.baud_changes,
                  'status': 'error' if self.errors else 'captured', 'errors': self.errors,
                  'log': str(self.output)}
        self.output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
        if self.errors:
            raise RuntimeError('; '.join(self.errors))
        return result


def start(config, output):
    local = config.get('serial_device')
    remote = config.get('serial_remote_device')
    if local and remote:
        raise ValueError('Configure one serial transport, local or remote')
    if not (local or remote):
        return None
    device = remote or str(Path(local).resolve())
    if not device.startswith('/dev/tty') or not device[5:].replace('_', '').isalnum():
        raise ValueError('Serial device must be a /dev/tty device')
    code = worker_code(device, int(config.get('serial_baud', 1500000)))
    if remote:
        command = ['ssh', '-T', '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=2',
                   '-F', config['ssh_config'], config['nanokvm_ssh'],
                   'python3 -u -c ' + shlex.quote(code)]
    else:
        command = [sys.executable, '-u', '-c', code]
    return Capture(command, output)
