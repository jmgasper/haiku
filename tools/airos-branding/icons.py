"""air/OS vector icons (HVIF) in Haiku's icon style.

Haiku icons are lit from the top left, use gradients for volume and carry a
dark outline: a stroke drawn behind the fill, 4 units wide down to about
21 px and 6 units below that, so the outline survives small sizes.  The
mark switches to a simplified drawing (sky, one wave, sun) at small sizes.
"""

import math
import airos_art as A
import hvif as H

OUTLINE = (0x14, 0x1E, 0x24)


def shade(c, f):
	"""f > 0 lightens towards white, f < 0 darkens towards black."""
	if f >= 0:
		return tuple(int(round(v + (255 - v) * f)) for v in c)
	return tuple(int(round(v * (1 + f))) for v in c)


class Styles:
	"""Gradient factory for one mark placement."""

	def __init__(self, icon, cx, cy, r):
		self.icon, self.cx, self.cy, self.r = icon, cx, cy, r

	def lin(self, a, b, stops):
		cx, cy, r = self.cx, self.cy, self.r
		p0 = (cx + a[0] * r, cy + a[1] * r)
		p1 = (cx + b[0] * r, cy + b[1] * r)
		return self.icon.gradient(H.GRADIENT_LINEAR, H.linear_matrix(p0, p1), stops)

	def rad(self, c, radius, stops):
		cx, cy, r = self.cx, self.cy, self.r
		return self.icon.gradient(H.GRADIENT_CIRCULAR,
			H.radial_matrix((cx + c[0] * r, cy + c[1] * r), radius * r), stops)


def add_mark(icon, cx, cy, r, outline=True, gloss=True, simple_below=0.3,
		outline_widths=(4, 6), outline_switch=0.33):
	"""The air/OS disc at centre (cx, cy), radius r, in icon units.

	simple_below: icon scale under which the simplified mark is drawn.
	"""
	T = lambda segs: A.transform(segs, r, cx, cy)
	reg = {k: T(v) for k, v in A.regions().items()}
	S = Styles(icon, cx, cy, r)
	full = (simple_below, 4.0)
	simple = (0.0, simple_below)

	disc = icon.path(reg['disc'])
	if outline:
		wide, narrow = outline_widths
		s_out = icon.solid(OUTLINE)
		icon.shape(s_out, [disc], stroke=(wide, 1, 1), lod=(outline_switch, 4.0))
		icon.shape(s_out, [disc], stroke=(narrow, 1, 1), lod=(0.0, outline_switch))

	# Cream disc, showing through the wave gaps.
	s_disc = S.lin((0, -1), (0, 1), [(0.0, shade(A.CLOUD, 0.5)), (1.0, shade(A.CLOUD, -0.08))])
	icon.shape(s_disc, [disc])

	# Sky, lit from the top left.
	s_sky = S.lin((-0.7, -0.9), (0.6, 0.4),
		[(0.0, shade(A.SKY, 0.38)), (0.55, A.SKY), (1.0, shade(A.SKY, -0.10))])
	icon.shape(s_sky, [icon.path(reg['sky'])])

	# Sun: a cream ring around an orange disc with a radial highlight.
	sx, sy = A.SUN_CENTER
	s_ring = S.lin((sx, sy - A.SUN_RING_RADIUS), (sx, sy + A.SUN_RING_RADIUS),
		[(0.0, shade(A.CLOUD, 0.6)), (1.0, shade(A.CLOUD, -0.05))])
	icon.shape(s_ring, [icon.path(reg['ring'])], lod=full)
	s_sun = S.rad((sx - A.SUN_RADIUS * 0.35, sy - A.SUN_RADIUS * 0.4), A.SUN_RADIUS * 1.45,
		[(0.0, shade(A.DAWN, 0.35)), (0.6, A.DAWN), (1.0, shade(A.DAWN, -0.12))])
	icon.shape(s_sun, [icon.path(reg['sun'])], lod=full)
	# Small sizes: a slightly larger sun without the ring.
	small_sun = icon.path(T(A.circle(A.SUN_CENTER, A.SUN_RING_RADIUS * 0.95)))
	icon.shape(s_sun, [small_sun], lod=simple)

	# Teal waves, darker towards the bottom.
	s_band = S.lin((0, 0.0), (0.2, 0.6), [(0.0, shade(A.BREEZE, 0.14)), (1.0, shade(A.BREEZE, -0.10))])
	icon.shape(s_band, [icon.path(reg['band'])], lod=full)
	s_low = S.lin((0, 0.35), (0, 1.0), [(0.0, shade(A.BREEZE, 0.06)), (1.0, shade(A.BREEZE, -0.22))])
	icon.shape(s_low, [icon.path(reg['bottom'])], lod=full)
	# Small sizes: one wave from the band's top edge down.
	icon.shape(s_low, [icon.path(T(A.below('B2')))], lod=simple)

	if gloss:
		g = T(A.arc((0, 0), 0.86, math.radians(200), math.radians(340)))
		hl = icon.path(g + [[g[-1][3], g[-1][3], g[0][0], g[0][0]]])
		s_hl = S.lin((0, -0.86), (0, -0.25), [(0.0, (255, 255, 255, 110)), (1.0, (255, 255, 255, 0))])
		icon.shape(s_hl, [hl], lod=full)
	return icon


def shadow(icon, cx, cy, rx, ry, alpha=90):
	sh = icon.path(A.circle((0, 0), 1.0))
	s_sh = icon.gradient(H.GRADIENT_CIRCULAR, H.radial_matrix((0, 0), 1.0),
		[(0.0, (0, 0, 0, alpha)), (1.0, (0, 0, 0, 0))])
	icon.shape(s_sh, [sh], matrix=[rx, 0, 0, ry, cx, cy])


def app_icon():
	"""Stand-alone mark, as an application icon (About this system)."""
	icon = H.Icon()
	shadow(icon, 34.0, 58.5, 25.0, 4.5)
	return add_mark(icon, 31.5, 30.5, 26.5)


def badge_icon(cx=30.0, cy=30.0, r=25.0):
	"""Mark only, no shadow (HaikuDepot's native badge, DriveSetup)."""
	return add_mark(H.Icon(), cx, cy, r, gloss=False)


def overlay(base_hvif, drop_shapes, cx, cy, r):
	"""A Haiku icon with some shapes removed and the mark added on top."""
	import hvif_edit
	icon = hvif_edit.load(base_hvif, drop_shapes)
	return add_mark(icon, cx, cy, r, gloss=False, simple_below=0.75)


def wordmark_paths(icon, x, baseline, xh):
	"""The wordmark's contours as paths: left edge x, baseline, x-height."""
	return [icon.path([[(x + px * xh, baseline + py * xh) for px, py in s] for s in path])
		for path in A.wordmark()]


def lockup(text_rgb, width=64.0, outline=False):
	"""Mark and wordmark side by side in a 64 x 21 unit band."""
	icon = H.Icon()
	r = 10.0
	cx, cy = r + 0.5, r + 0.5
	add_mark(icon, cx, cy, r, outline=outline, gloss=False, simple_below=0.0)
	x0, top, x1, bottom = A.wordmark_bounds()
	left = cx + r + 4.0
	xh = (width - 0.5 - left) / (x1 - x0)
	baseline = cy + xh * 0.5
	paths = wordmark_paths(icon, left - x0 * xh, baseline, xh)
	icon.shape(icon.solid(text_rgb), paths)
	return icon, cy * 2


def deskbar_logo(text_rgb=A.SLATE):
	"""Deskbar's menu button (rendered 63 x 22 at 12 pt): mark + wordmark.
	Deskbar picks the light or dark text variant from its panel colour."""
	icon = H.Icon()
	r = 8.6
	cx, cy = 10.5, 11.0
	add_mark(icon, cx, cy, r, outline=False, gloss=False, simple_below=0.0)
	# Thin outline ring so the disc reads on any Deskbar colour.
	ring_o = icon.path(A.circle((cx, cy), r + 0.45))
	ring_i = icon.path(A.arc((cx, cy), r - 0.25, 2 * math.pi, 0.0))
	icon.shape(icon.solid((0x2F, 0x5A, 0x63), 200), [ring_o, ring_i])
	x0, top, x1, bottom = A.wordmark_bounds()
	left = cx + r + 3.6
	xh = 6.9
	baseline = cy + xh * 0.5
	paths = wordmark_paths(icon, left - x0 * xh, baseline, xh)
	icon.shape(icon.solid(text_rgb), paths)
	return icon
