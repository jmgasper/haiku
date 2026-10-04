"""Require complete pixels, fixed heap accounting and actual incremental work."""
import re
import heap_pressure_validation


def validate(text, output=None, software=False):
    result = heap_pressure_validation.validate(text, output, software, limit=True)
    message = r'Incremental rendering was triggered (\d+) time\(s\)'
    counts = re.findall(message, text)
    if software:
        assert not counts
        return result
    assert len(counts) == 4 and all(int(value) > 0 for value in counts)
    previous = 0
    for frame, begin in zip(result['frames'], re.finditer(r'^ROCK5_LIMIT_PIXELS_BEGIN ', text, re.M)):
        local = re.findall(message, text[previous:begin.start()])
        assert len(local) == 1 and int(local[0]) > 0
        frame['incremental_passes'] = int(local[0])
        previous = begin.end()
    assert result['grown_chunks'] == 0
    result['incremental_passes'] = sum(map(int, counts))
    return result
