"""air/OS logo geometry, shared by every generated asset.

The mark is a disc: sky above a wave, a sun with a cream ring, a teal
band and a teal bottom, separated by cream gaps.  Coordinates are
normalised: centre (0, 0), radius 1, y pointing down.  The four wave
boundaries are cubic Beziers fitted to the reference artwork
(rms error 0.5% of the radius).
"""

import math

SKY = (0xA7, 0xD8, 0xF0)
BREEZE = (0x4E, 0x8D, 0x8A)
DAWN = (0xE3, 0xA2, 0x5B)
CLOUD = (0xF6, 0xF3, 0xEA)
SLATE = (0x2F, 0x3A, 0x40)

SUN_CENTER = (0.459, -0.240)
SUN_RADIUS = 0.258
SUN_RING_RADIUS = 0.306

# Wave boundaries, top to bottom: sky/gap, gap/band, band/gap, gap/bottom.
WAVES = {
	'B1': [(-1.1, -0.097), (0.268, -0.280), (0.229, 0.604), (1.1, 0.243)],
	'B2': [(-1.1, 0.061), (0.086, -0.135), (0.274, 0.731), (1.1, 0.311)],
	'B3': [(-1.1, 0.283), (0.354, 0.063), (-0.185, 0.715), (1.1, 0.486)],
	'B4': [(-1.1, 0.451), (0.158, 0.154), (-0.108, 0.879), (1.1, 0.545)],
}


def lerp(a, b, t):
	return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def bez_point(p, t):
	a = lerp(p[0], p[1], t); b = lerp(p[1], p[2], t); c = lerp(p[2], p[3], t)
	d = lerp(a, b, t); e = lerp(b, c, t)
	return lerp(d, e, t)


def bez_split(p, t):
	a = lerp(p[0], p[1], t); b = lerp(p[1], p[2], t); c = lerp(p[2], p[3], t)
	d = lerp(a, b, t); e = lerp(b, c, t); f = lerp(d, e, t)
	return [p[0], a, d, f], [f, e, c, p[3]]


def bez_segment(p, t0, t1):
	_, right = bez_split(p, t0)
	if t1 >= 1.0:
		return right
	left, _ = bez_split(right, (t1 - t0) / (1.0 - t0))
	return left


def circle_crossings(p, center=(0.0, 0.0), radius=1.0, steps=4000):
	"""Parameters where the curve crosses the circle, by bisection."""
	def f(t):
		x, y = bez_point(p, t)
		return math.hypot(x - center[0], y - center[1]) - radius
	result = []
	prev_t, prev_v = 0.0, f(0.0)
	for i in range(1, steps + 1):
		t = i / steps
		v = f(t)
		if (prev_v < 0) != (v < 0):
			lo, hi = prev_t, t
			for _ in range(60):
				mid = (lo + hi) / 2
				if (f(lo) < 0) != (f(mid) < 0):
					hi = mid
				else:
					lo = mid
			result.append((lo + hi) / 2)
		prev_t, prev_v = t, v
	return result


def arc(center, radius, a0, a1):
	"""Cubic segments for the arc from angle a0 to a1 (radians, y down)."""
	n = max(1, int(math.ceil(abs(a1 - a0) / (math.pi / 2) - 1e-9)))
	step = (a1 - a0) / n
	k = 4.0 / 3.0 * math.tan(step / 4.0) * radius
	segs = []
	cx, cy = center
	for i in range(n):
		s = a0 + i * step
		e = s + step
		p0 = (cx + radius * math.cos(s), cy + radius * math.sin(s))
		p3 = (cx + radius * math.cos(e), cy + radius * math.sin(e))
		p1 = (p0[0] - k * math.sin(s), p0[1] + k * math.cos(s))
		p2 = (p3[0] + k * math.sin(e), p3[1] - k * math.cos(e))
		segs.append([p0, p1, p2, p3])
	return segs


def circle(center, radius):
	return arc(center, radius, 0.0, 2 * math.pi)


def angle(pt):
	return math.atan2(pt[1], pt[0])


def _wave_inside(name):
	p = WAVES[name]
	t = circle_crossings(p)
	assert len(t) == 2, (name, t)
	seg = bez_segment(p, t[0], t[1])
	return seg, seg[0], seg[3]


def _ccw_to(a_from, a_to):
	"""a_to adjusted so that a_from -> a_to runs with decreasing angle."""
	while a_to > a_from:
		a_to -= 2 * math.pi
	return a_to


def _cw_to(a_from, a_to):
	while a_to < a_from:
		a_to += 2 * math.pi
	return a_to


def regions():
	"""Closed outlines (lists of cubic segments), back to front."""
	b1, l1, r1 = _wave_inside('B1')
	b2, l2, r2 = _wave_inside('B2')
	b3, l3, r3 = _wave_inside('B3')
	b4, l4, r4 = _wave_inside('B4')

	def rev(seg):
		return [seg[3], seg[2], seg[1], seg[0]]

	sky = [b1] + arc((0, 0), 1, angle(r1), _ccw_to(angle(r1), angle(l1)))
	band = ([b2] + arc((0, 0), 1, angle(r2), _cw_to(angle(r2), angle(r3)))
		+ [rev(b3)] + arc((0, 0), 1, angle(l3), _cw_to(angle(l3), angle(l2))))
	bottom = [b4] + arc((0, 0), 1, angle(r4), _cw_to(angle(r4), angle(l4)))
	return {
		'disc': circle((0, 0), 1.0),
		'sky': sky,
		'ring': circle(SUN_CENTER, SUN_RING_RADIUS),
		'sun': circle(SUN_CENTER, SUN_RADIUS),
		'band': band,
		'bottom': bottom,
	}


def transform(segs, scale, dx, dy):
	return [[(x * scale + dx, y * scale + dy) for x, y in s] for s in segs]


def svg_path(segs):
	out = ['M %.3f %.3f' % segs[0][0]]
	for s in segs:
		out.append('C %.3f %.3f %.3f %.3f %.3f %.3f' % (s[1] + s[2] + s[3]))
	out.append('Z')
	return ' '.join(out)


def cairo_path(ctx, segs):
	ctx.move_to(*segs[0][0])
	for s in segs:
		ctx.curve_to(*(s[1] + s[2] + s[3]))
	ctx.close_path()


# --- Wordmark -------------------------------------------------------------

_WORDMARK = None
WORDMARK_LEFT, WORDMARK_BASELINE, WORDMARK_XHEIGHT = 114.4, 498.7, 80.9


def wordmark():
	"""Traced 'air/os' contours (even-odd), in x-height units: x from the
	left edge of the 'a', y = 0 on the baseline, x-height = 1."""
	global _WORDMARK
	if _WORDMARK is None:
		import json, os
		raw = json.load(open(os.path.join(os.path.dirname(__file__), 'wordmark_paths.json')))
		_WORDMARK = [[[((x - WORDMARK_LEFT) / WORDMARK_XHEIGHT, (y - WORDMARK_BASELINE) / WORDMARK_XHEIGHT)
			for x, y in s] for s in path] for path in raw]
	return _WORDMARK


def wordmark_bounds():
	pts = [p for path in wordmark() for s in path for p in s]
	return (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts))


def draw_wordmark(ctx, x, baseline, xheight, rgb, alpha=1.0):
	import cairo
	ctx.save()
	ctx.set_fill_rule(cairo.FILL_RULE_EVEN_ODD)
	ctx.new_path()
	for path in wordmark():
		cairo_path(ctx, [[(x + px * xheight, baseline + py * xheight) for px, py in s] for s in path])
	ctx.set_source_rgba(rgb[0] / 255, rgb[1] / 255, rgb[2] / 255, alpha)
	ctx.fill()
	ctx.restore()


def draw_mark_flat(ctx, cx, cy, r, gap=CLOUD, alpha=1.0):
	"""The flat brand mark (as on the reference sheet)."""
	reg = regions()
	ctx.save()
	if alpha < 1.0:
		ctx.push_group()
	for name, color in (('disc', gap), ('sky', SKY), ('ring', gap), ('sun', DAWN),
			('band', BREEZE), ('bottom', BREEZE)):
		ctx.new_path()
		cairo_path(ctx, transform(reg[name], r, cx, cy))
		ctx.set_source_rgb(*[c / 255 for c in color])
		ctx.fill()
	if alpha < 1.0:
		ctx.pop_group_to_source()
		ctx.paint_with_alpha(alpha)
	ctx.restore()


def below(name):
	"""The part of the disc below wave boundary `name`."""
	seg, l, r = _wave_inside(name)
	return [seg] + arc((0, 0), 1, angle(r), _cw_to(angle(r), angle(l)))
