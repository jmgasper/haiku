"""Preserve a used lab USB copy locally, then remove only that verified remote file.

Usage: archive_used_image.py /data/<name>.img LABEL [--keep]

The remote file is streamed through gzip while its inode, size and mtime are
checked before and after; the local copy is re-hashed before the remote copy
is removed. The currently attached gadget file is never a candidate. With
--keep the remote copy is preserved after archiving.
"""
import gzip
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

os.umask(0o077)
sys.path.insert(0, str(Path(__file__).resolve().parent))
import lab

if len(sys.argv) not in (3, 4) or (len(sys.argv) == 4 and sys.argv[3] != '--keep'):
    raise SystemExit(__doc__)
used, label = sys.argv[1], sys.argv[2]
keep = len(sys.argv) == 4
assert re.fullmatch(r'/data/[A-Za-z0-9._-]+\.img', used) and re.fullmatch(r'[a-z0-9-]{1,48}', label)
config = json.loads(lab.CONFIG.read_text())
root = lab.WORK / 'artifacts/nanokvm-image-archive' / (lab.timestamp() + '-' + label)
root.mkdir(parents=True)
(root / Path(__file__).name).write_bytes(Path(__file__).read_bytes())
with lab.lock('hardware'):
    current = lab.gadget(config)
    assert used != current['file'], 'refusing to archive the attached gadget file'
    stat_code = '''import json,os,stat
from pathlib import Path
p=Path(%r);s=p.lstat()
assert stat.S_ISREG(s.st_mode) and s.st_nlink==1
print(json.dumps(dict(path=str(p),bytes=s.st_size,mtime_ns=s.st_mtime_ns)))
''' % used
    entry = json.loads(lab.remote_python(config, stat_code, timeout=30))
    lab.save(root / 'plan.json', dict(entries=[entry], gadget=current, keep=keep,
        method='verified_streaming_archive'))
    remote = r'''import gzip,hashlib,json,os,re,stat,sys
from pathlib import Path
entry=%r
p=Path(entry['path'])
assert p.parent==Path('/data') and re.fullmatch(r'[A-Za-z0-9._-]+\.img',p.name)
s=p.lstat()
assert stat.S_ISREG(s.st_mode) and s.st_nlink==1
assert s.st_size==entry['bytes'] and s.st_mtime_ns==entry['mtime_ns']
h=hashlib.sha256()
with p.open('rb') as f, gzip.GzipFile(fileobj=sys.stdout.buffer,mode='wb',compresslevel=1,mtime=0) as archive:
 for block in iter(lambda:f.read(1048576),b''):
  h.update(block);archive.write(block)
sys.stdout.buffer.flush()
after=p.lstat()
assert (after.st_ino,after.st_size,after.st_mtime_ns)==(s.st_ino,s.st_size,s.st_mtime_ns)
print('ROCK5_ARCHIVE '+json.dumps(dict(entry,sha256=h.hexdigest(),inode=s.st_ino)),file=sys.stderr)
''' % entry
    (root / (Path(used).stem + '-read.py')).write_text(remote)
    saved = root / Path(used).name
    command = ['ssh', '-F', config['ssh_config'], '-o', 'ConnectTimeout=8',
        config['nanokvm_ssh'], 'python3 -c ' + shlex.quote(remote)]
    compressed = saved.with_suffix('.img.gz')
    with compressed.open('xb') as output:
        result = subprocess.run(command, stdout=output, stderr=subprocess.PIPE, timeout=900)
        output.flush()
        os.fsync(output.fileno())
    (saved.with_suffix('.stderr.txt')).write_bytes(result.stderr)
    result.check_returncode()
    rows = [line[len('ROCK5_ARCHIVE '):] for line in result.stderr.decode().splitlines()
        if line.startswith('ROCK5_ARCHIVE ')]
    assert len(rows) == 1
    receipt = json.loads(rows[0])
    total = 0
    with gzip.open(compressed, 'rb') as source, saved.open('xb') as target:
        for block in iter(lambda: source.read(1048576), b''):
            total += len(block)
            assert total <= receipt['bytes']
            target.write(block)
        target.flush()
        os.fsync(target.fileno())
    compressed.chmod(0o400)
    receipt.update(compressed_local=str(compressed), compressed_bytes=compressed.stat().st_size)
    assert saved.stat().st_size == receipt['bytes']
    assert lab.digest(saved) == receipt['sha256']
    saved.chmod(0o400)
    archived = [dict(receipt, local=str(saved))]
    lab.save(root / 'archived.json', archived)
    assert lab.gadget(config) == current
    removed = dict(status='kept', removed=[], free_bytes=None)
    if not keep:
        remove = '''import hashlib,json,os,shutil,stat
from pathlib import Path
entries=%r
for entry in entries:
 p=Path(entry['path']);s=p.lstat()
 assert stat.S_ISREG(s.st_mode) and s.st_nlink==1
 assert s.st_ino==entry['inode'] and s.st_size==entry['bytes'] and s.st_mtime_ns==entry['mtime_ns']
 h=hashlib.sha256()
 with p.open('rb') as f:
  for block in iter(lambda:f.read(1048576),b''):h.update(block)
 assert h.hexdigest()==entry['sha256']
for entry in entries:Path(entry['path']).unlink()
os.sync()
print(json.dumps(dict(status='pass',removed=[e['path'] for e in entries],free_bytes=shutil.disk_usage('/data').free)))
''' % archived
        (root / 'remove-verified-copies.py').write_text(remove)
        removed = json.loads(lab.remote_python(config, remove, timeout=600))
        assert len(removed['removed']) == 1 and lab.gadget(config) == current
    result = dict(status='pass', label=label, archived=archived, removed=removed, gadget=current,
        root=str(root), method='verified_streaming_archive')
    lab.save(root / 'result.json', result)
print(json.dumps(dict(status='pass', root=str(root), sha256=receipt['sha256'],
    free_bytes=removed['free_bytes'])), flush=True)
