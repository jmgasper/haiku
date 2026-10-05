"""Read checkfs counters; exit status alone does not establish a clean BFS."""
import re


def allocation_counts(transcript):
    text = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', transcript)
    counters = {}
    for name, label in [('missing', 'not allocated'),
                        ('duplicate', 'already set'),
                        ('unreferenced', 'could be freed')]:
        matches = re.findall(r'^\s*(\d+) blocks ' + label + r',?\s*$',
                             text, re.MULTILINE)
        if len(matches) != 1:
            raise ValueError(f'Expected one checkfs {name} counter, got {len(matches)}')
        counters[name] = int(matches[0])
    return counters


def require_clean(transcript):
    counters = allocation_counts(transcript)
    if any(counters.values()):
        raise ValueError(f'BFS allocation check is not clean: {counters}')
    for diagnostic in [', some blocks weren\'t allocated', ', has blocks already set',
                       ', has invalid block run(s)', ', could not be opened',
                       ', has wrong type', ", names don't match", ', invalid b+tree']:
        if diagnostic in transcript:
            raise ValueError(f'BFS node check failed: {diagnostic[2:]}')
    return counters
