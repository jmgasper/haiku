#!/usr/bin/env python3
"""Run commands or upload a checked file to Haiku through NanoKVM's SSH tunnel."""

import argparse
import base64
import contextlib
import ipaddress
import json
import os
import re
import select
import secrets
import shlex
import socket
import struct
import subprocess
import threading
import time
import warnings

with warnings.catch_warnings():
    warnings.simplefilter('ignore', DeprecationWarning)
    import telnetlib  # The lab workstation uses Python 3.12.

import lab
import shell_image


def ssh_command(config):
    return ['ssh', '-F', str(lab.local_path(config['ssh_config'])),
            '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=3']


def usb_address(value):
    address = ipaddress.IPv4Address(value)
    network = ipaddress.IPv4Network('10.239.6.0/24')
    if address not in network or address in (network.network_address, network.broadcast_address):
        raise ValueError('Target must be a host on the private 10.239.6.0/24 USB link')
    return str(address)


def login(host, port, credentials, timeout=20):
    client = telnetlib.Telnet(host, port, timeout)
    try:
        if not client.read_until(b'login:', timeout).endswith(b'login:'):
            raise TimeoutError('No lab login prompt')
        client.write((credentials['username'] + '\r\n').encode())
        if not client.read_until(b'password:', timeout).endswith(b'password:'):
            raise TimeoutError('No lab password prompt')
        client.write((credentials['password'] + '\r\n').encode())
        return client
    except BaseException:
        client.close()
        raise


def execute(client, commands, output, credentials, timeout=60):
    """Save partial output even on EOF, timeout or a failed guest command."""
    output = lab.local_path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    token = secrets.token_hex(8)
    begin = f'ROCK5_BEGIN_{token}'
    end = f'ROCK5_END_{token}'
    payload = (f"PS1= PS2=\nprintf '\\n{begin}\\n'\n(\nset -e\n{commands}\n)\n"
               f"rock5_result=$?; printf '\\n{end}:%d\\n' \"$rock5_result\"\n")
    received = b''
    result = {'status': 'incomplete', 'transcript': str(output)}
    send_errors = []

    def send():
        try:
            client.write(payload.replace('\n', '\r\n').encode())
        except BaseException as error:
            send_errors.append(error)

    # A large here-document can fill the guest's output window with prompts
    # while its input is still being sent. Drain replies throughout the send.
    writer = threading.Thread(target=send, name='Haiku command sender', daemon=True)
    try:
        client.get_socket().settimeout(timeout)
        writer.start()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            received += client.read_very_eager()
            if send_errors:
                raise send_errors[0]
            match = re.search(rb'\n' + end.encode() + rb':([0-9]+)\r*\n', received)
            if match:
                if not re.search(rb'\n' + begin.encode() + rb'\r*\n', received):
                    raise RuntimeError('Missing command start marker')
                result['exit_status'] = int(match.group(1))
                if result['exit_status'] != 0:
                    raise RuntimeError(f"Guest command failed with exit status {result['exit_status']}")
                result['status'] = 'pass'
                return result
            time.sleep(.1)
        raise TimeoutError('Guest commands did not complete before the deadline')
    except BaseException as error:
        result.update(status='error', error=str(error))
        raise
    finally:
        if writer.is_alive():
            try:
                client.get_socket().shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            writer.join(timeout=5)
        output.write_text(received.decode(errors='replace').replace(
            credentials['password'], '[REDACTED]'))
        output.chmod(0o600)
        lab.save(output.with_suffix('.json'), result)


@contextlib.contextmanager
def tunnel(config, target, output):
    target = usb_address(target)
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    command = ssh_command(config) + ['-NT', '-o', 'ExitOnForwardFailure=yes',
               '-L', f'127.0.0.1:{port}:{target}:23', config['nanokvm_ssh']]
    errors_path = lab.local_path(output).with_suffix('.ssh.log')
    errors_path.parent.mkdir(parents=True, exist_ok=True)
    with errors_path.open('w') as errors:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=errors)
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('SSH tunnel exited; see its log')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=1):
                        break
                except OSError:
                    time.sleep(.2)
            else:
                raise TimeoutError('SSH tunnel did not open')
            yield port
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)


def run_commands(config, target, commands, output, timeout=60):
    """Caller owns the hardware lock; the CLI acquires it below."""
    credentials = shell_image.read_credentials()
    with tunnel(config, target, output) as port:
        with login('127.0.0.1', port, credentials) as client:
            time.sleep(2)
            result = execute(client, commands, output, credentials, timeout)
            client.write(b'exit\r\n')
    return dict(result, target=target)


@contextlib.contextmanager
def binary_relay(config, target, source, output):
    """Stream the local file through SSH; NanoKVM stores no extra file."""
    target = usb_address(target)
    token = secrets.token_hex(32)
    script = '''import json, socket, struct, sys
target = %r
token = %r
with socket.socket() as listener:
    listener.bind(('10.239.6.1', 0))
    listener.listen(1)
    listener.settimeout(90)
    print(json.dumps({'address': '10.239.6.1', 'port': listener.getsockname()[1]}), flush=True)
    connection, peer = listener.accept()
    with connection:
        connection.settimeout(60)
        if peer[0] != target:
            raise RuntimeError('Unexpected USB peer')
        received = b''
        while not received.endswith(b'\\n') and len(received) <= 64:
            chunk = connection.recv(1)
            if not chunk:
                raise EOFError('Missing transfer token')
            received += chunk
        if received != token.encode() + b'\\n':
            raise RuntimeError('Incorrect transfer token')
        connection.sendall(struct.pack('!Q', %d))
        total = 0
        while True:
            chunk = sys.stdin.buffer.read(65536)
            if not chunk:
                break
            connection.sendall(chunk)
            total += len(chunk)
    print(json.dumps({'sent': total}), flush=True)
''' % (target, token, source.stat().st_size)
    command = ssh_command(config) + [config['nanokvm_ssh'],
                                    'python3 -u -c ' + shlex.quote(script)]
    errors_path = lab.local_path(output).with_suffix('.relay.log')
    errors_path.parent.mkdir(parents=True, exist_ok=True)
    with source.open('rb') as data, errors_path.open('w') as errors:
        process = subprocess.Popen(command, stdin=data, stdout=subprocess.PIPE,
                                   stderr=errors, text=True)
        try:
            if not select.select([process.stdout], [], [], 20)[0]:
                raise TimeoutError('Binary relay did not start')
            ready = json.loads(process.stdout.readline())
            if ready.get('address') != '10.239.6.1' or not 1 <= ready.get('port', 0) <= 65535:
                raise RuntimeError('Invalid binary relay endpoint')
            yield ready, token
            remaining, _ = process.communicate(timeout=10)
            if process.returncode != 0:
                raise RuntimeError('Binary relay failed; see its log')
            receipt = json.loads(remaining)
            if receipt.get('sent') != source.stat().st_size:
                raise RuntimeError('Binary relay byte count differs from the source')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)


def upload(config, target, source, name, output, executable=False, transport='tcp'):
    source = lab.local_path(source)
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', name):
        raise ValueError('Use a simple destination filename')
    if source.stat().st_size > 16 * 1024 * 1024:
        raise ValueError('This shell transport is limited to 16 MiB per file')
    digest = lab.digest(source)
    destination = '/boot/home/rock5-lab/' + name
    incoming = destination + '.incoming-' + secrets.token_hex(6)
    mode = '700' if executable else '600'
    prefix = 'umask 077\nmkdir -p /boot/home/rock5-lab\n'
    verify = (
        f"sync\nactual=$(sha256sum {shlex.quote(incoming)})\n"
        f"[ \"${{actual%% *}}\" = {digest} ]\n"
        f"chmod {mode} {shlex.quote(incoming)}\n"
        f"mv {shlex.quote(incoming)} {shlex.quote(destination)}\nsync\n"
        f"printf 'ROCK5_UPLOAD_SHA256 %s\\n' \"${{actual%% *}}\"\n")
    started = time.monotonic()
    if transport == 'tcp':
        with binary_relay(config, target, source, output) as (relay, token):
            commands = (prefix + '/boot/home/config/non-packaged/bin/rock5_file_transfer '
                        f"receive {relay['address']} {relay['port']} {token} "
                        f"{shlex.quote(incoming)}\n" + verify)
            result = run_commands(config, target, commands, output, timeout=300)
    elif transport == 'terminal':
        encoded = base64.encodebytes(source.read_bytes()).decode()
        delimiter = 'ROCK5_BASE64_' + secrets.token_hex(8)
        commands = (prefix + f"base64 -d > {shlex.quote(incoming)} <<'{delimiter}'\n"
                    + encoded + delimiter + '\n' + verify)
        result = run_commands(config, target, commands, output, timeout=300)
    else:
        raise ValueError('Unknown upload transport')
    if f'ROCK5_UPLOAD_SHA256 {digest}' not in lab.local_path(output).read_text():
        raise RuntimeError('Missing verified upload checksum')
    result.update(source=str(source), destination=destination, sha256=digest,
                  bytes=source.stat().st_size, executable=executable, transport=transport,
                  elapsed_seconds=time.monotonic() - started)
    lab.save(lab.local_path(output).with_suffix('.upload.json'), result)
    return result


@contextlib.contextmanager
def binary_receiver(config, target, destination, output):
    """Drain the SSH binary stream while the guest sends its file."""
    target = usb_address(target)
    token = secrets.token_hex(32)
    script = '''import json, socket, struct, sys
with socket.socket() as listener:
    listener.bind(('10.239.6.1', 0))
    listener.listen(1)
    listener.settimeout(90)
    print(json.dumps({'address': '10.239.6.1', 'port': listener.getsockname()[1]}), flush=True)
    connection, peer = listener.accept()
    with connection:
        connection.settimeout(60)
        if peer[0] != %r:
            raise RuntimeError('Unexpected USB peer')
        def read_exactly(size):
            data = bytearray()
            while len(data) < size:
                chunk = connection.recv(size - len(data))
                if not chunk:
                    raise EOFError('Truncated guest transfer')
                data.extend(chunk)
            return data
        if read_exactly(65) != %r:
            raise RuntimeError('Incorrect transfer token')
        header = read_exactly(8)
        remaining, = struct.unpack('!Q', header)
        if remaining > 16 * 1024 * 1024:
            raise ValueError('Oversized guest transfer')
        sys.stdout.buffer.write(header)
        while remaining:
            chunk = read_exactly(min(65536, remaining))
            sys.stdout.buffer.write(chunk)
            remaining -= len(chunk)
        sys.stdout.buffer.flush()
''' % (target, token.encode() + b'\n')
    command = ssh_command(config) + [config['nanokvm_ssh'],
                                    'python3 -u -c ' + shlex.quote(script)]
    errors_path = lab.local_path(output).with_suffix('.relay.log')
    errors_path.parent.mkdir(parents=True, exist_ok=True)
    failures = []
    reader = None
    with errors_path.open('w') as errors:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=errors)
        try:
            if not select.select([process.stdout], [], [], 20)[0]:
                raise TimeoutError('Binary receiver did not start')
            ready = json.loads(process.stdout.readline())
            if ready.get('address') != '10.239.6.1' or not 1 <= ready.get('port', 0) <= 65535:
                raise RuntimeError('Invalid binary receiver endpoint')

            def receive():
                try:
                    header = process.stdout.read(8)
                    if len(header) != 8:
                        raise EOFError('Missing download length')
                    remaining, = struct.unpack('!Q', header)
                    if remaining > 16 * 1024 * 1024:
                        raise ValueError('Oversized download')
                    with destination.open('xb') as data:
                        destination.chmod(0o600)
                        while remaining:
                            chunk = process.stdout.read(min(65536, remaining))
                            if not chunk:
                                raise EOFError('Truncated download')
                            data.write(chunk)
                            remaining -= len(chunk)
                    if process.stdout.read(1):
                        raise ValueError('Unexpected trailing download bytes')
                except BaseException as error:
                    failures.append(error)

            reader = threading.Thread(target=receive, name='Haiku file receiver', daemon=True)
            reader.start()
            yield ready, token
            process.wait(timeout=10)
            reader.join(timeout=10)
            if reader.is_alive():
                raise TimeoutError('Binary receiver did not finish')
            if failures:
                raise failures[0]
            if process.returncode != 0:
                raise RuntimeError('Binary receiver failed; see its log')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
            if reader:
                reader.join(timeout=10)
            process.stdout.close()


@contextlib.contextmanager
def staged_binary_receiver(config, target, destination, output, rate_limit=256 * 1024):
    """Finish USB reception on NanoKVM before downloading over its Ethernet link."""
    target = usb_address(target)
    if type(rate_limit) is not int or (rate_limit != 0 and not 65536 <= rate_limit <= 16 * 1024 * 1024):
        raise ValueError('Receive rate must be zero or between 65536 and 16777216 bytes/second')
    token = secrets.token_hex(32)
    staging = '/data/haiku-download-' + secrets.token_hex(12) + '.bin'
    output = lab.local_path(output)
    record = {'status': 'incomplete', 'remote_staging_file': staging,
              'remote_file_removed': False, 'rate_limit_bytes_per_second': rate_limit}
    script = '''import hashlib, json, os, socket, struct, time
os.umask(0o077)
rate_limit = %r
with socket.socket() as listener:
    if rate_limit:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16384)
    listener.bind(('10.239.6.1', 0))
    listener.listen(1)
    listener.settimeout(90)
    print(json.dumps({'address': '10.239.6.1', 'port': listener.getsockname()[1]}), flush=True)
    connection, peer = listener.accept()
    with connection:
        connection.settimeout(60)
        if peer[0] != %r:
            raise RuntimeError('Unexpected USB peer')
        def read_exactly(size):
            data = bytearray()
            while len(data) < size:
                chunk = connection.recv(size - len(data))
                if not chunk:
                    raise EOFError('Truncated guest transfer')
                data.extend(chunk)
            return data
        if read_exactly(65) != %r:
            raise RuntimeError('Incorrect transfer token')
        count, = struct.unpack('!Q', read_exactly(8))
        if count > 16 * 1024 * 1024:
            raise ValueError('Oversized guest transfer')
        remaining = count
        digest = hashlib.sha256()
        with open(%r, 'xb') as data:
            while remaining:
                chunk = read_exactly(min(4096 if rate_limit else 65536, remaining))
                data.write(chunk)
                digest.update(chunk)
                remaining -= len(chunk)
                if rate_limit:
                    time.sleep(len(chunk) / rate_limit)
            data.flush()
            os.fsync(data.fileno())
        print(json.dumps({'bytes': count, 'sha256': digest.hexdigest()}), flush=True)
''' % (rate_limit, target, token.encode() + b'\n', staging)
    command = ssh_command(config) + [config['nanokvm_ssh'],
                                    'python3 -u -c ' + shlex.quote(script)]
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with output.with_suffix('.staging.log').open('w') as errors:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=errors)
            try:
                if not select.select([process.stdout], [], [], 20)[0]:
                    raise TimeoutError('Staged receiver did not start')
                ready = json.loads(process.stdout.readline())
                if ready.get('address') != '10.239.6.1' or not 1 <= ready.get('port', 0) <= 65535:
                    raise RuntimeError('Invalid staged receiver endpoint')
                yield ready, token
                receipt_data, _ = process.communicate(timeout=30)
                if process.returncode != 0:
                    raise RuntimeError('Staged USB receiver failed; see its log')
                receipt = json.loads(receipt_data)
                if (type(receipt.get('bytes')) is not int or not 0 <= receipt['bytes'] <= 16 * 1024 * 1024
                        or not re.fullmatch(r'[0-9a-f]{64}', receipt.get('sha256', ''))):
                    raise RuntimeError('Invalid staged transfer receipt')
                record['receipt'] = receipt
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=10)
                process.stdout.close()

            # The receiver has exited and closed the USB socket before this
            # second SSH connection starts moving the file onto the workstation.
            with destination.open('xb') as data:
                destination.chmod(0o600)
                subprocess.run(ssh_command(config) + [config['nanokvm_ssh'],
                               'cat ' + shlex.quote(staging)], stdin=subprocess.DEVNULL,
                               stdout=data, stderr=errors, timeout=90, check=True)
            if destination.stat().st_size != receipt['bytes'] or lab.digest(destination) != receipt['sha256']:
                raise RuntimeError('Staged download differs from the USB receipt')
            lab.remote_python(config, 'from pathlib import Path\nPath(' + repr(staging) + ').unlink()')
            record.update(status='pass', remote_file_removed=True)
    except BaseException as error:
        record.update(status='error', error=str(error))
        raise
    finally:
        # Failed scratch files are retained for diagnosis, with their exact path
        # recorded here. Cleanup never guesses or removes another trial's file.
        lab.save(output.with_suffix('.staging.json'), record)


def download(config, target, name, destination, output, transport='staged', rate_limit=256 * 1024):
    destination = lab.local_path(destination)
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', name):
        raise ValueError('Use a simple source filename')
    if destination.exists():
        raise FileExistsError('Download destination already exists')
    if transport not in ('staged', 'relay'):
        raise ValueError('Download transport must be staged or relay')
    destination.parent.mkdir(parents=True, exist_ok=True)
    incoming = destination.with_name(destination.name + '.incoming-' + secrets.token_hex(6))
    source = '/boot/home/rock5-lab/' + name
    started = time.monotonic()
    receiver = (staged_binary_receiver(config, target, incoming, output, rate_limit)
                if transport == 'staged' else binary_receiver(config, target, incoming, output))
    with receiver as (relay, token):
        commands = (
            f'actual=$(sha256sum {shlex.quote(source)})\n'
            '/boot/home/config/non-packaged/bin/rock5_file_transfer '
            f'send {relay["address"]} {relay["port"]} {token} {shlex.quote(source)}\n'
            f'after=$(sha256sum {shlex.quote(source)})\n'
            '[ "${after%% *}" = "${actual%% *}" ]\n'
            'printf "ROCK5_DOWNLOAD_SHA256 %s\\n" "${actual%% *}"\n')
        result = run_commands(config, target, commands, output, timeout=300)
    matches = re.findall(r'(?:^|\n)ROCK5_DOWNLOAD_SHA256 ([0-9a-f]{64})\r?\n',
                         lab.local_path(output).read_text())
    if len(matches) != 1 or lab.digest(incoming) != matches[0]:
        raise RuntimeError('Downloaded bytes differ from the guest checksum')
    incoming.rename(destination)
    result.update(source=source, destination=str(destination), sha256=matches[0],
                  bytes=destination.stat().st_size, transport=transport,
                  rate_limit_bytes_per_second=rate_limit if transport == 'staged' else None,
                  elapsed_seconds=time.monotonic() - started)
    lab.save(lab.local_path(output).with_suffix('.download.json'), result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    run = commands.add_parser('run', help='Run a command file with set -e')
    run.add_argument('target', type=usb_address)
    run.add_argument('commands')
    run.add_argument('--timeout', type=int, default=60)
    copy = commands.add_parser('upload', help='Verify and install a file under /boot/home/rock5-lab')
    copy.add_argument('target', type=usb_address)
    copy.add_argument('source')
    copy.add_argument('--name', required=True)
    copy.add_argument('--executable', action='store_true')
    copy.add_argument('--transport', choices=('tcp', 'terminal'), default='tcp')
    fetch = commands.add_parser('download', help='Verify a file copied from /boot/home/rock5-lab')
    fetch.add_argument('target', type=usb_address)
    fetch.add_argument('name')
    fetch.add_argument('destination')
    fetch.add_argument('--transport', choices=('staged', 'relay'), default='staged')
    fetch.add_argument('--rate-limit', type=int, default=256 * 1024,
                       help='Staged USB receive bytes/second; zero disables pacing')
    for command in (run, copy, fetch):
        command.add_argument('--output', required=True, help='Local transcript under /mnt/HaikuWork')
    args = parser.parse_args()
    if not os.path.ismount(lab.WORK):
        raise RuntimeError(f'Required filesystem is not mounted: {lab.WORK}')
    os.umask(0o077)
    config = json.loads(lab.CONFIG.read_text())
    with lab.lock('hardware'):
        if args.action == 'run':
            result = run_commands(config, args.target, lab.local_path(args.commands).read_text(),
                                  args.output, args.timeout)
        elif args.action == 'upload':
            result = upload(config, args.target, args.source, args.name, args.output,
                            args.executable, args.transport)
        else:
            result = download(config, args.target, args.name, args.destination, args.output,
                              args.transport, args.rate_limit)
    print(json.dumps(result))


if __name__ == '__main__':
    main()
