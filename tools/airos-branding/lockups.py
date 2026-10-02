"""Raster lockups of the air/OS mark and wordmark."""

import cairo, math
import airos_art as A


def _surface(w, h):
	surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, int(math.ceil(w)), int(math.ceil(h)))
	return surf, cairo.Context(surf)


def horizontal(height, text_rgb=A.SLATE, gap_rgb=A.CLOUD, pad=0.0):
	"""Mark on the left, wordmark to its right; height = mark diameter."""
	r = height / 2.0
	xh = height * 0.36
	x0, top, x1, bottom = A.wordmark_bounds()
	word_w = (x1 - x0) * xh
	spacing = height * 0.22
	w = pad * 2 + height + spacing + word_w
	surf, ctx = _surface(w, height + pad * 2)
	A.draw_mark_flat(ctx, pad + r, pad + r, r - 0.5, gap=gap_rgb)
	baseline = pad + r + xh * 0.5
	A.draw_wordmark(ctx, pad + height + spacing - x0 * xh, baseline, xh, text_rgb)
	return surf


def vertical(width, text_rgb=A.SLATE, gap_rgb=A.CLOUD, pad=0.0):
	"""The primary lockup: mark above the wordmark (as on the brand sheet)."""
	# Proportions from the sheet: mark diameter 307, wordmark 395 wide,
	# 16 px between the circle and the top of the 'i' dot.
	x0, top, x1, bottom = A.wordmark_bounds()
	word_w = width
	xh = word_w / (x1 - x0)
	d = word_w * 307.0 / 395.3
	r = d / 2.0
	gap = xh * 0.2
	h = pad * 2 + d + gap + (bottom - top) * xh
	surf, ctx = _surface(width + pad * 2, h)
	cx = pad + width / 2.0
	A.draw_mark_flat(ctx, cx, pad + r, r - 0.5, gap=gap_rgb)
	baseline = pad + d + gap - top * xh
	A.draw_wordmark(ctx, pad - x0 * xh, baseline, xh, text_rgb)
	return surf


if __name__ == '__main__':
	import os
	out = '/mnt/HaikuWork/airos/art/out'
	os.makedirs(out, exist_ok=True)
	horizontal(64).write_to_png(out + '/lockup-h64-light.png')
	horizontal(64, text_rgb=(0xF6, 0xF3, 0xEA)).write_to_png(out + '/lockup-h64-dark.png')
	vertical(400, pad=20).write_to_png(out + '/lockup-v400.png')
