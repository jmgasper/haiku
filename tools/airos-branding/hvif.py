"""Minimal HVIF ("ncif") writer, matching src/libs/icon/flat_icon.

Icon space is 64x64 units.  Linear gradients run along x from -64 to 64 in
gradient space, radial ones have radius 64; a gradient's affine matrix maps
that space into icon space.
"""

import math
import struct

STYLE_SOLID = 1
STYLE_GRADIENT = 2
STYLE_SOLID_NO_ALPHA = 3
GRADIENT_LINEAR = 0
GRADIENT_CIRCULAR = 1
GRADIENT_FLAG_TRANSFORM = 1 << 1
GRADIENT_FLAG_NO_ALPHA = 1 << 2
PATH_FLAG_CLOSED = 1 << 1
PATH_FLAG_USES_COMMANDS = 1 << 2
PATH_FLAG_NO_CURVES = 1 << 3
SHAPE_TYPE_PATH_SOURCE = 10
SHAPE_FLAG_TRANSFORM = 1 << 1
SHAPE_FLAG_HINTING = 1 << 2
SHAPE_FLAG_LOD_SCALE = 1 << 3
SHAPE_FLAG_HAS_TRANSFORMERS = 1 << 4
TRANSFORMER_STROKE = 23
TRANSFORMER_CONTOUR = 21


def coord(v):
	v = max(-128.0, min(192.0, v))
	if abs(v - round(v)) < 1e-6 and -32 <= round(v) <= 95:
		return bytes([int(round(v)) + 32])
	value = int(round((v + 128.0) * 102.0)) | 0x8000
	return bytes([value >> 8, value & 0xff])


def float24(v):
	if v == 0:
		return b'\0\0\0'
	bits = struct.unpack('<I', struct.pack('<f', v))[0]
	sign = bits >> 31
	exponent = ((bits >> 23) & 0xff) - 127
	mantissa = bits & 0x7fffff
	if exponent >= 32 or exponent < -32:
		return b'\0\0\0'
	short = (sign << 23) | ((exponent + 32) << 17) | (mantissa >> 6)
	return bytes([(short >> 16) & 0xff, (short >> 8) & 0xff, short & 0xff])


def linear_matrix(p0, p1):
	"""Gradient space x in [-64, 64] onto the segment p0 -> p1."""
	dx, dy = p1[0] - p0[0], p1[1] - p0[1]
	length = math.hypot(dx, dy)
	s = length / 128.0
	c, n = dx / length, dy / length
	return [s * c, s * n, -s * n, s * c, (p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2]


def radial_matrix(center, radius, sy=None):
	s = radius / 64.0
	return [s, 0.0, 0.0, (sy if sy else radius) / 64.0, center[0], center[1]]


class Icon:
	def __init__(self):
		self.styles = []
		self.paths = []
		self.shapes = []

	# Styles return their index.
	def solid(self, rgb, alpha=255):
		if alpha == 255:
			data = bytes([STYLE_SOLID_NO_ALPHA]) + bytes(rgb)
		else:
			data = bytes([STYLE_SOLID]) + bytes(rgb) + bytes([alpha])
		self.styles.append(data)
		return len(self.styles) - 1

	def gradient(self, kind, matrix, stops):
		"""stops: list of (offset 0..1, (r, g, b) or (r, g, b, a))."""
		has_alpha = any(len(c) == 4 and c[3] != 255 for _, c in stops)
		flags = GRADIENT_FLAG_TRANSFORM | (0 if has_alpha else GRADIENT_FLAG_NO_ALPHA)
		data = bytes([STYLE_GRADIENT, kind, flags, len(stops)])
		data += b''.join(float24(v) for v in matrix)
		for offset, color in stops:
			data += bytes([int(round(offset * 255))])
			rgb = bytes(color[:3])
			data += rgb + (bytes([color[3] if len(color) == 4 else 255]) if has_alpha else b'')
		self.styles.append(data)
		return len(self.styles) - 1

	def path(self, segs, closed=True):
		"""segs: cubic segments [p0, c1, c2, p3] forming one contour."""
		n = len(segs)
		points = []
		for i, s in enumerate(segs):
			prev = segs[i - 1]
			points.append((s[0], prev[2], s[1]))
		if not closed:
			points.append((segs[-1][3], segs[-1][2], segs[-1][3]))
			points[0] = (segs[0][0], segs[0][0], segs[0][1])
		data = bytes([PATH_FLAG_CLOSED if closed else 0, len(points)])
		for p, cin, cout in points:
			for v in (p[0], p[1], cin[0], cin[1], cout[0], cout[1]):
				data += coord(v)
		self.paths.append(data)
		return len(self.paths) - 1

	def polygon(self, pts, closed=True):
		data = bytes([(PATH_FLAG_CLOSED if closed else 0) | PATH_FLAG_NO_CURVES, len(pts)])
		for x, y in pts:
			data += coord(x) + coord(y)
		self.paths.append(data)
		return len(self.paths) - 1

	def shape(self, style, paths, stroke=None, lod=None, hinting=False,
			matrix=None, contour=None):
		flags = 0
		extra = b''
		if hinting:
			flags |= SHAPE_FLAG_HINTING
		if matrix is not None:
			flags |= SHAPE_FLAG_TRANSFORM
			extra += b''.join(float24(v) for v in matrix)
		if lod is not None:
			flags |= SHAPE_FLAG_LOD_SCALE
			extra += bytes([int(round(lod[0] * 63.75)), int(round(min(lod[1], 4.0) * 63.75))])
		transformers = []
		if stroke is not None:
			# width (int), line join 0=miter 1=round, cap 1=round
			width, join, cap = (stroke + (1, 1))[:3] if isinstance(stroke, tuple) else (stroke, 1, 1)
			transformers.append(bytes([TRANSFORMER_STROKE, int(round(width)) + 128, (cap << 4) | join, 4]))
		if contour is not None:
			transformers.append(bytes([TRANSFORMER_CONTOUR, int(round(contour)) + 128, 1, 4]))
		if transformers:
			flags |= SHAPE_FLAG_HAS_TRANSFORMERS
			extra += bytes([len(transformers)]) + b''.join(transformers)
		data = bytes([SHAPE_TYPE_PATH_SOURCE, style, len(paths)]) + bytes(paths) + bytes([flags]) + extra
		self.shapes.append(data)
		return len(self.shapes) - 1

	def data(self):
		out = b'ncif'
		out += bytes([len(self.styles)]) + b''.join(self.styles)
		out += bytes([len(self.paths)]) + b''.join(self.paths)
		out += bytes([len(self.shapes)]) + b''.join(self.shapes)
		return out

	def rdef(self, name='vector_icon', indent='\t'):
		h = self.data().hex().upper()
		lines = [h[i:i + 64] for i in range(0, len(h), 64)]
		return 'resource %s {\n' % name + '\n'.join('%s$"%s"' % (indent, l) for l in lines) + '\n};\n'
