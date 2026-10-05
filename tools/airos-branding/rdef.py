"""Pull HVIF blobs out of .rdef files and put new ones back."""

import re


def find_block(text, start_pattern, occurrence=0):
	"""Return (start, end) of the resource statement whose header matches
	start_pattern (regex), up to and including its closing '};'."""
	matches = list(re.finditer(start_pattern, text))
	m = matches[occurrence]
	end = text.index('};', m.end()) + 2
	return m.start(), end


def blob(text, span):
	body = text[span[0]:span[1]]
	hexdata = ''.join(re.findall(r'\$"([0-9A-Fa-f]*)"', body))
	return bytes.fromhex(hexdata)


def hex_lines(data, indent='\t', width=64):
	h = data.hex().upper()
	return '\n'.join('%s$"%s"' % (indent, h[i:i + width]) for i in range(0, len(h), width))


def replace_blob(text, span, data, indent='\t'):
	"""Keep the resource header, swap the $"..." data lines."""
	body = text[span[0]:span[1]]
	first = body.index('$"')
	header = body[:first].rstrip() + '\n'
	# keep anything between the hex data and the closing brace (rare)
	return text[:span[0]] + header + hex_lines(data, indent) + '\n};' + text[span[1]:]


def c_array(data, name, per_line=12):
	parts = ['0x%02x' % b for b in data]
	lines = [', '.join(parts[i:i + per_line]) for i in range(0, len(parts), per_line)]
	return 'const unsigned char %s[] = {\n\t' % name + ',\n\t'.join(lines) + '\n};\n'
