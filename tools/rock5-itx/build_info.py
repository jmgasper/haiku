#!/usr/bin/env python3
"""Record build-time provenance, so packaging an old image cannot relabel it."""
import json
from pathlib import Path
import sys

from lab import WORK, SOURCE, digest, run, save, timestamp, validate_image

OUTPUT = WORK / 'build/arm64'


def inputs():
    status = run(['git', '-C', str(SOURCE), 'status', '--porcelain'])
    patch = run(['git', '-C', str(SOURCE), 'diff', 'HEAD'])
    untracked = run(['git', '-C', str(SOURCE), 'ls-files', '--others', '--exclude-standard'])
    return {
        'source_revision': run(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD']),
        'source_dirty': bool(status), 'source_status': status, 'source_patch': patch,
        'untracked_sha256': {name: digest(SOURCE / name) for name in untracked.splitlines()},
        'buildtools_revision': run(['git', '-C', str(WORK / 'src/buildtools'), 'rev-parse', 'HEAD']),
    }


if __name__ == '__main__':
    pending = OUTPUT / 'pending-build.json'
    if sys.argv[1] == 'start':
        save(pending, {'inputs': inputs(), 'target': sys.argv[2], 'started_utc': timestamp()})
    elif sys.argv[1] == 'finish':
        record = json.loads(pending.read_text())
        current = inputs()
        if current != record['inputs']:
            # An object compiled from a file being edited can end up newer than
            # the file's final save, and jam would then reuse it: make every
            # file that changed since the build started newer than any object.
            changed = run(['git', '-C', str(SOURCE), 'diff', '--name-only', record['inputs']['source_revision']]).splitlines()
            changed += run(['git', '-C', str(SOURCE), 'ls-files', '--others', '--exclude-standard']).splitlines()
            for name in sorted(set(changed)):
                if (SOURCE / name).is_file():
                    (SOURCE / name).touch()
            raise RuntimeError('Sources changed during the build (%d files touched so jam rebuilds them). '
                'Rebuild before packaging.' % len(set(changed)))
        image = OUTPUT / 'haiku-arm64-mmc.image'
        record.update({'image': str(image), 'sha256': digest(image), 'finished_utc': timestamp(),
                       'layout': validate_image(image),
                       'haiku_revision': (OUTPUT / 'build/haiku-revision').read_text().strip()})
        save(OUTPUT / 'build-record.json', record)
    else:
        raise SystemExit('Use start TARGET or finish')
