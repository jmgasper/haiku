"""Run-length encoded true-colour TGA (type 10), packets per scanline,
bottom-left origin: what Haiku's TGATranslator reads on every platform
(arm64 has no PNG or JPEG translator)."""

import struct


def write_rle(path, img):
	img = img.convert('RGB')
	w, h = img.size
	px = img.tobytes()
	out = bytearray(struct.pack('<BBBHHBHHHHBB', 0, 0, 10, 0, 0, 0, 0, 0, w, h, 24, 0))
	for y in range(h - 1, -1, -1):
		row = px[y * w * 3:(y + 1) * w * 3]
		pix = [row[i * 3:i * 3 + 3][::-1] for i in range(w)]  # BGR
		i = 0
		while i < w:
			j = i + 1
			while j < w and j - i < 128 and pix[j] == pix[i]:
				j += 1
			if j - i >= 2:
				out.append(0x80 | (j - i - 1))
				out += pix[i]
				i = j
				continue
			j = i + 1
			while j < w and j - i < 128 and not (j + 1 < w and pix[j] == pix[j + 1]):
				j += 1
			out.append(j - i - 1)
			for k in range(i, j):
				out += pix[k]
			i = j
	out += b'\0\0\0\0\0\0\0\0TRUEVISION-XFILE.\0'
	open(path, 'wb').write(out)
	return len(out)


def read_rle(path):
	"""Decoder for checking write_rle."""
	from PIL import Image
	d = open(path, 'rb').read()
	idlen, cmtype, itype = d[0], d[1], d[2]
	w, h = struct.unpack('<HH', d[12:16])
	pos = 18 + idlen
	pix = bytearray()
	while len(pix) < w * h * 3:
		c = d[pos]; pos += 1
		n = (c & 0x7f) + 1
		if c & 0x80:
			pix += d[pos:pos + 3] * n; pos += 3
		else:
			pix += d[pos:pos + 3 * n]; pos += 3 * n
	img = Image.frombytes('RGB', (w, h), bytes(pix), 'raw', 'BGR', 0, -1)
	return img
