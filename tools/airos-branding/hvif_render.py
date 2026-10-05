"""Render HVIF bytes with cairo, following src/libs/icon's importer and
IconRenderer, to preview icons on the host."""

import struct, cairo


class Reader:
	def __init__(self, data):
		self.d = data; self.i = 0
	def u8(self):
		v = self.d[self.i]; self.i += 1; return v
	def coord(self):
		v = self.u8()
		if v & 128:
			lo = self.u8()
			return (((v & 127) << 8) | lo) / 102.0 - 128.0
		return v - 32.0
	def f24(self):
		b = self.d[self.i:self.i + 3]; self.i += 3
		s = (b[0] << 16) | (b[1] << 8) | b[2]
		if s == 0:
			return 0.0
		sign = (s & 0x800000) >> 23
		exp = ((s & 0x7e0000) >> 17) - 32
		man = (s & 0x01ffff) << 6
		bits = (sign << 31) | ((exp + 127) << 23) | man
		return struct.unpack('<f', struct.pack('<I', bits))[0]


def parse(data):
	r = Reader(data)
	assert data[:4] == b'ncif', data[:4]
	r.i = 4
	styles = []
	for _ in range(r.u8()):
		t = r.u8()
		if t == 1:
			styles.append(('solid', (r.u8(), r.u8(), r.u8(), r.u8())))
		elif t == 3:
			styles.append(('solid', (r.u8(), r.u8(), r.u8(), 255)))
		elif t == 4:
			g = r.u8(); a = r.u8(); styles.append(('solid', (g, g, g, a)))
		elif t == 5:
			g = r.u8(); styles.append(('solid', (g, g, g, 255)))
		elif t == 2:
			gtype = r.u8(); flags = r.u8(); n = r.u8()
			m = [r.f24() for _ in range(6)] if flags & 2 else [1, 0, 0, 1, 0, 0]
			alpha = not (flags & 4); gray = flags & 16
			stops = []
			for _ in range(n):
				off = r.u8() / 255.0
				if gray:
					g = r.u8(); a = r.u8() if alpha else 255; c = (g, g, g, a)
				else:
					c = (r.u8(), r.u8(), r.u8(), r.u8() if alpha else 255)
				stops.append((off, c))
			styles.append(('gradient', gtype, m, stops))
		else:
			raise ValueError('style type %d' % t)
	paths = []
	for _ in range(r.u8()):
		flags = r.u8(); n = r.u8(); pts = []
		if flags & 8:
			for _ in range(n):
				p = (r.coord(), r.coord()); pts.append((p, p, p))
		elif flags & 4:
			cmds = []
			nbytes = (n + 3) // 4
			raw = r.d[r.i:r.i + nbytes]; r.i += nbytes
			for k in range(n):
				cmds.append((raw[k // 4] >> ((k % 4) * 2)) & 3)
			last = (0.0, 0.0)
			for c in cmds:
				if c == 0:
					p = (r.coord(), last[1]); pts.append((p, p, p))
				elif c == 1:
					p = (last[0], r.coord()); pts.append((p, p, p))
				elif c == 2:
					p = (r.coord(), r.coord()); pts.append((p, p, p))
				else:
					p = (r.coord(), r.coord()); pin = (r.coord(), r.coord()); pout = (r.coord(), r.coord())
					pts.append((p, pin, pout))
				last = pts[-1][0]
		else:
			for _ in range(n):
				p = (r.coord(), r.coord()); pin = (r.coord(), r.coord()); pout = (r.coord(), r.coord())
				pts.append((p, pin, pout))
		paths.append((bool(flags & 2), pts))
	shapes = []
	for _ in range(r.u8()):
		t = r.u8(); assert t == 10, t
		style = r.u8(); n = r.u8(); pidx = [r.u8() for _ in range(n)]
		flags = r.u8()
		matrix = None
		if flags & 2:
			matrix = [r.f24() for _ in range(6)]
		elif flags & 32:
			tx, ty = r.coord(), r.coord(); matrix = [1, 0, 0, 1, tx, ty]
		lod = (0.0, 4.0)
		if flags & 8:
			lod = (r.u8() / 63.75, r.u8() / 63.75)
		transformers = []
		if flags & 16:
			for _ in range(r.u8()):
				tt = r.u8()
				if tt == 23:
					transformers.append(('stroke', r.u8() - 128.0, r.u8(), r.u8()))
				elif tt == 21:
					transformers.append(('contour', r.u8() - 128.0, r.u8(), r.u8()))
				elif tt == 20:
					transformers.append(('affine', [struct.unpack('<f', bytes(r.d[r.i + 4 * k:r.i + 4 * k + 4]))[0] for k in range(6)])); r.i += 24
				elif tt == 22:
					transformers.append(('perspective', [r.f24() for _ in range(9)]))
				else:
					raise ValueError('transformer %d' % tt)
		shapes.append(dict(style=style, paths=pidx, matrix=matrix, lod=lod, transformers=transformers, hinting=bool(flags & 4)))
	return styles, paths, shapes


def render(data, size, background=None):
	styles, paths, shapes = parse(data)
	surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, size, size)
	ctx = cairo.Context(surf)
	if background:
		ctx.set_source_rgb(*[c / 255 for c in background]); ctx.paint()
	scale = size / 64.0
	ctx.scale(scale, scale)
	ctx.set_fill_rule(cairo.FILL_RULE_WINDING)
	for sh in shapes:
		if not (sh['lod'][0] <= scale <= sh['lod'][1] or (sh['lod'][1] >= 3.99 and scale >= sh['lod'][0])):
			continue
		ctx.save()
		if sh['matrix']:
			m = sh['matrix']
			ctx.transform(cairo.Matrix(m[0], m[1], m[2], m[3], m[4], m[5]))
		ctx.new_path()
		for pi in sh['paths']:
			closed, pts = paths[pi]
			if not pts:
				continue
			ctx.move_to(*pts[0][0])
			n = len(pts)
			for k in range(1, n + (1 if closed else 0)):
				a = pts[k - 1]; b = pts[k % n]
				ctx.curve_to(a[2][0], a[2][1], b[1][0], b[1][1], b[0][0], b[0][1])
			if closed:
				ctx.close_path()
		st = styles[sh['style']]
		if st[0] == 'solid':
			c = st[1]; ctx.set_source_rgba(c[0] / 255, c[1] / 255, c[2] / 255, c[3] / 255)
		else:
			_, gtype, m, stops = st
			if gtype == 0:
				pat = cairo.LinearGradient(-64, 0, 64, 0)
			else:
				pat = cairo.RadialGradient(0, 0, 0, 0, 0, 64)
			for off, c in stops:
				pat.add_color_stop_rgba(off, c[0] / 255, c[1] / 255, c[2] / 255, c[3] / 255)
			mat = cairo.Matrix(m[0], m[1], m[2], m[3], m[4], m[5])
			mat.invert()
			pat.set_matrix(mat)
			pat.set_extend(cairo.EXTEND_PAD)
			ctx.set_source(pat)
		stroke = [t for t in sh['transformers'] if t[0] == 'stroke']
		contour = [t for t in sh['transformers'] if t[0] == 'contour']
		if stroke:
			ctx.set_line_width(stroke[0][1])
			ctx.set_line_join([cairo.LINE_JOIN_MITER, cairo.LINE_JOIN_MITER, cairo.LINE_JOIN_ROUND, cairo.LINE_JOIN_BEVEL][stroke[0][2] & 3] if (stroke[0][2] & 15) < 4 else cairo.LINE_JOIN_ROUND)
			ctx.set_line_cap([cairo.LINE_CAP_BUTT, cairo.LINE_CAP_SQUARE, cairo.LINE_CAP_ROUND][min(2, stroke[0][2] >> 4)])
			ctx.stroke()
		elif contour:
			ctx.fill_preserve(); ctx.set_line_width(abs(contour[0][1]) * 2); ctx.stroke()
		else:
			ctx.fill()
		ctx.restore()
	return surf


def sheet(icons, sizes=(16, 24, 32, 48, 64, 128), backgrounds=((216, 216, 216), (51, 102, 152), (40, 40, 40)), path='sheet.png'):
	"""icons: list of (name, data)."""
	from PIL import Image
	import io
	W = sum(sizes) + 10 * (len(sizes) + 1)
	H = (max(sizes) + 10) * len(icons) * len(backgrounds) + 10
	out = Image.new('RGB', (W, H), (255, 255, 255))
	y = 10
	for name, data in icons:
		for bg in backgrounds:
			x = 10
			for s in sizes:
				surf = render(data, s, bg)
				buf = io.BytesIO(); surf.write_to_png(buf); buf.seek(0)
				out.paste(Image.open(buf).convert('RGB'), (x, y))
				x += s + 10
			y += max(sizes) + 10
	out.save(path)
	return path


def render_wide(data, width, height, background=None):
	"""Like BIconUtils::GetVectorIcon into a width x height bitmap: the
	scale comes from the width alone."""
	import io
	from PIL import Image
	big = render(data, width, background)
	buf = io.BytesIO(); big.write_to_png(buf); buf.seek(0)
	return Image.open(buf).crop((0, 0, width, height))
