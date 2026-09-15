"""Keep sustained-test output in guest RAM and check staged pieces independently."""
import hashlib
from pathlib import Path
import re

CHUNK = 8 * 1024 * 1024
RAM = '/boot/home/rock5-sustained-ram'
PREFIX = 'rock5-sustained-'


def script(software=False):
    return r'''set -eu
rock5_capture=/boot/home/rock5-sustained-ram
mkdir -p "$rock5_capture" /boot/home/rock5-lab
mount -t ramfs "$rock5_capture"
df "$rock5_capture"
set +e
(
set -eu
/boot/home/mesa-trial/run-sustained @MODE@
) > "$rock5_capture/output.log" 2>&1
rock5_capture_status=$?
set -e
printf '\nROCK5_SUSTAIN_INVENTORY_EXIT=%s\n' "$rock5_capture_status" >> "$rock5_capture/output.log"
rock5_capture_bytes=$(stat -c %s "$rock5_capture/output.log")
[ "$rock5_capture_bytes" -gt 0 ]
[ "$rock5_capture_bytes" -le 134217728 ]
rock5_capture_hash=$(sha256sum "$rock5_capture/output.log")
split -b 8388608 -d -a 3 "$rock5_capture/output.log" "$rock5_capture/rock5-sustained-"
rock5_capture_count=0
for rock5_capture_piece in "$rock5_capture"/rock5-sustained-*; do
    rock5_capture_name=${rock5_capture_piece##*/}
    rock5_capture_link=/boot/home/rock5-lab/$rock5_capture_name
    if [ -e "$rock5_capture_link" ] || [ -L "$rock5_capture_link" ]; then
        [ -L "$rock5_capture_link" ]
        [ "$(readlink "$rock5_capture_link")" = "$rock5_capture_piece" ]
        rm "$rock5_capture_link"
    fi
    ln -s "$rock5_capture_piece" "$rock5_capture_link"
    rock5_capture_piece_hash=$(sha256sum "$rock5_capture_piece")
    printf 'ROCK5_CAPTURE_CHUNK index=%s name=%s bytes=%s sha256=%s\n' "$rock5_capture_count" "$rock5_capture_name" "$(stat -c %s "$rock5_capture_piece")" "${rock5_capture_piece_hash%% *}"
    rock5_capture_count=$((rock5_capture_count + 1))
done
printf 'ROCK5_CAPTURE file_bytes=%s sha256=%s chunks=%s\n' "$rock5_capture_bytes" "${rock5_capture_hash%% *}" "$rock5_capture_count"
'''.replace('@MODE@', '--software' if software else '--native')


def cleanup():
    return r'''set -eu
for rock5_capture_piece in /boot/home/rock5-sustained-ram/rock5-sustained-*; do
    rock5_capture_link=/boot/home/rock5-lab/${rock5_capture_piece##*/}
    [ -L "$rock5_capture_link" ]
    [ "$(readlink "$rock5_capture_link")" = "$rock5_capture_piece" ]
    rm "$rock5_capture_link"
done
unmount /boot/home/rock5-sustained-ram
echo ROCK5_CAPTURE_RELEASED
'''


def manifest(text):
    totals = re.findall(r'^ROCK5_CAPTURE file_bytes=(\d+) sha256=([0-9a-f]{64}) chunks=(\d+)\s*$', text, re.M)
    assert len(totals) == text.count('ROCK5_CAPTURE file_bytes=') == 1
    size, digest, count = totals[0]
    size, count = int(size), int(count)
    assert 0 < size <= 16 * CHUNK and count == (size + CHUNK - 1) // CHUNK
    parts = re.findall(r'^ROCK5_CAPTURE_CHUNK index=(\d+) name=(rock5-sustained-\d{3}) bytes=(\d+) sha256=([0-9a-f]{64})\s*$', text, re.M)
    assert len(parts) == text.count('ROCK5_CAPTURE_CHUNK ') == count
    result = dict(bytes=size, sha256=digest, chunks=[])
    for index, (number, name, length, sha) in enumerate(parts):
        assert int(number) == index and name == PREFIX + f'{index:03d}'
        length = int(length)
        assert length == min(CHUNK, size - CHUNK * index)
        result['chunks'].append(dict(index=index, name=name, bytes=length, sha256=sha))
    return result


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def assemble(receipt, paths, destination):
    assert len(paths) == len(receipt['chunks'])
    destination = Path(destination)
    with destination.open('xb') as output:
        for part, path in zip(receipt['chunks'], paths):
            path = Path(path)
            assert path.stat().st_size == part['bytes'] and digest(path) == part['sha256']
            with path.open('rb') as source:
                for block in iter(lambda: source.read(1024 * 1024), b''):
                    output.write(block)
    assert destination.stat().st_size == receipt['bytes']
    assert digest(destination) == receipt['sha256']
    return str(destination)
