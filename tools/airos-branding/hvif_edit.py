"""Load an existing HVIF into an hvif.Icon so shapes can be dropped and
new ones appended; styles and paths are copied as raw bytes."""

import hvif as H
from hvif_render import Reader


def _split(data):
	"""Byte ranges of every style, path and shape."""
	r = Reader(data)
	r.i = 4
	styles, paths, shapes = [], [], []
	for _ in range(r.u8()):
		start = r.i
		t = r.u8()
		if t == 1: r.i += 4
		elif t == 3: r.i += 3
		elif t == 4: r.i += 2
		elif t == 5: r.i += 1
		elif t == 2:
			gtype = r.u8(); flags = r.u8(); n = r.u8()
			if flags & 2: r.i += 18
			alpha = not (flags & 4); gray = flags & 16
			per = (2 if alpha else 1) if gray else (4 if alpha else 3)
			r.i += n * (1 + per)
		else:
			raise ValueError(t)
		styles.append(data[start:r.i])
	for _ in range(r.u8()):
		start = r.i
		flags = r.u8(); n = r.u8()
		if flags & 8:
			for _ in range(2 * n): r.coord()
		elif flags & 4:
			nbytes = (n + 3) // 4
			raw = data[r.i:r.i + nbytes]; r.i += nbytes
			for k in range(n):
				c = (raw[k // 4] >> ((k % 4) * 2)) & 3
				for _ in range({0: 1, 1: 1, 2: 2, 3: 6}[c]): r.coord()
		else:
			for _ in range(6 * n): r.coord()
		paths.append(data[start:r.i])
	for _ in range(r.u8()):
		start = r.i
		assert r.u8() == 10
		style = r.u8(); n = r.u8(); pidx = [r.u8() for _ in range(n)]
		flags = r.u8()
		if flags & 2: r.i += 18
		elif flags & 32: r.coord(); r.coord()
		if flags & 8: r.i += 2
		if flags & 16:
			for _ in range(r.u8()):
				tt = r.u8()
				r.i += {23: 3, 21: 3, 20: 24, 22: 27}[tt]
		shapes.append((style, pidx, data[start:r.i]))
	assert r.i == len(data), (r.i, len(data))
	return styles, paths, shapes


def load(data, drop_shapes=()):
	styles, paths, shapes = _split(data)
	icon = H.Icon()
	keep = [s for i, s in enumerate(shapes) if i not in set(drop_shapes)]
	used_styles = sorted({s[0] for s in keep})
	used_paths = sorted({p for s in keep for p in s[1]})
	smap = {old: i for i, old in enumerate(used_styles)}
	pmap = {old: i for i, old in enumerate(used_paths)}
	icon.styles = [styles[i] for i in used_styles]
	icon.paths = [paths[i] for i in used_paths]
	for style, pidx, raw in keep:
		n = len(pidx)
		rest = raw[3 + n:]
		icon.shapes.append(bytes([10, smap[style], n]) + bytes(pmap[p] for p in pidx) + rest)
	return icon
