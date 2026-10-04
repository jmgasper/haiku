"""The default air/OS desktop: a hazy sky, a low sun and layered hills,
after the 'Applications' panel of the brand sheet."""

import math, random, sys
import cairo
import numpy as np
import airos_art as A


def mix(a, b, t):
	return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def rgb(c):
	return tuple(v / 255.0 for v in c)


def ridge(width, base, amplitude, seed, octaves=4, roughness=0.32, peaks=()):
	rng = random.Random(seed)
	xs = np.linspace(0, 1, 512)
	y = np.zeros_like(xs)
	amp, freq = 1.0, 1.3
	for _ in range(octaves):
		phase = rng.uniform(0, 2 * math.pi)
		phase2 = rng.uniform(0, 2 * math.pi)
		y += amp * (0.6 * np.sin(2 * math.pi * freq * xs + phase)
			+ 0.4 * np.sin(2 * math.pi * freq * 1.7 * xs + phase2))
		amp *= roughness
		freq *= 2.1
	y = (y - y.min()) / (y.max() - y.min())
	for px, ph, pw in peaks:
		y += ph * np.exp(-((xs - px) / pw) ** 2)
	return xs * width, base - y * amplitude


def render(W, H, path=None, wordmark=True, dither=True):
	surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
	ctx = cairo.Context(surf)
	s = H / 1080.0

	# Sky: deeper blue at the top, warm haze towards the horizon.
	sky = cairo.LinearGradient(0, 0, 0, H)
	sky.add_color_stop_rgb(0.00, *rgb((0x7F, 0xB4, 0xD6)))
	sky.add_color_stop_rgb(0.30, *rgb((0xA7, 0xD0, 0xE6)))
	sky.add_color_stop_rgb(0.55, *rgb((0xD3, 0xE3, 0xE6)))
	sky.add_color_stop_rgb(0.72, *rgb((0xF1, 0xE1, 0xCC)))
	sky.add_color_stop_rgb(1.00, *rgb((0xF3, 0xDF, 0xC6)))
	ctx.set_source(sky); ctx.paint()

	# Sun with a soft glow.
	sx, sy, sr = W * 0.62, H * 0.585, 118 * s
	glow = cairo.RadialGradient(sx, sy, sr * 0.8, sx, sy, sr * 5.5)
	glow.add_color_stop_rgba(0.0, *rgb((0xF4, 0xC8, 0x95)), 0.55)
	glow.add_color_stop_rgba(0.4, *rgb((0xF4, 0xD6, 0xB0)), 0.18)
	glow.add_color_stop_rgba(1.0, *rgb((0xF4, 0xDE, 0xC0)), 0.0)
	ctx.set_source(glow); ctx.paint()
	sun = cairo.LinearGradient(0, sy - sr, 0, sy + sr)
	sun.add_color_stop_rgb(0.0, *rgb((0xF0, 0xB8, 0x7A)))
	sun.add_color_stop_rgb(1.0, *rgb((0xEB, 0xC0, 0x95)))
	ctx.arc(sx, sy, sr, 0, 2 * math.pi)
	ctx.set_source(sun); ctx.fill()

	# Hills, back to front: lighter and hazier with distance.
	far = (0xB4, 0xC9, 0xCB)
	near = (0x3E, 0x75, 0x73)
	layers = [
		# base (fraction of H), amplitude (px at 1080), seed, peaks
		(0.70, 170, 11, [(0.28, 0.7, 0.13), (0.86, 0.45, 0.12)]),
		(0.77, 160, 23, [(0.12, 0.5, 0.12), (0.47, 0.35, 0.13)]),
		(0.84, 150, 37, [(0.80, 0.6, 0.15)]),
		(0.91, 140, 41, [(0.30, 0.5, 0.17)]),
		(0.98, 120, 53, [(0.72, 0.4, 0.18)]),
		(1.06, 110, 67, [(0.12, 0.35, 0.16)]),
	]
	n = len(layers)
	for i, (base, amp, seed, peaks) in enumerate(layers):
		t = i / (n - 1)
		color = mix(far, near, t ** 0.9)
		xs, ys = ridge(W, base * H, amp * s, seed, peaks=peaks)
		ctx.new_path()
		ctx.move_to(0, H)
		for x, y in zip(xs, ys):
			ctx.line_to(x, y)
		ctx.line_to(W, H)
		ctx.close_path()
		top = float(ys.min())
		grad = cairo.LinearGradient(0, top, 0, H)
		grad.add_color_stop_rgb(0.0, *rgb(color))
		grad.add_color_stop_rgb(1.0, *rgb(mix(color, (0xEE, 0xE6, 0xD8), 0.35 * (1 - t))))
		ctx.set_source(grad)
		ctx.fill()
		# Mist settling in front of this layer.
		mist = cairo.LinearGradient(0, base * H - amp * s * 0.2, 0, base * H + 40 * s)
		mist.add_color_stop_rgba(0.0, 1, 1, 1, 0.0)
		mist.add_color_stop_rgba(1.0, *rgb((0xF2, 0xEE, 0xE4)), 0.22 * (1 - t * 0.7))
		ctx.rectangle(0, 0, W, H)
		ctx.set_source(mist); ctx.fill()

	if wordmark:
		xh = 26 * s
		A.draw_wordmark(ctx, 72 * s, H - 70 * s, xh, (255, 255, 255), alpha=0.82)

	surf.flush()
	# Dither away the banding that 8-bit gradients leave.
	buf = np.ndarray((H, W, 4), np.uint8, surf.get_data()).copy()
	rgbimg = buf[..., :3]
	if dither:
		rng = np.random.default_rng(7)
		noise = rng.integers(-2, 3, size=(H, W, 1), dtype=np.int16)
		rgbimg = np.clip(rgbimg.astype(np.int16) + noise, 0, 255).astype(np.uint8)
	from PIL import Image
	img = Image.fromarray(rgbimg[..., ::-1].copy(), 'RGB')  # cairo is BGRA
	if path is None:
		return img
	if path.endswith('.jpg'):
		img.save(path, quality=90, optimize=True, progressive=False)
	else:
		img.save(path)
	return path


if __name__ == '__main__':
	W, H = int(sys.argv[1]), int(sys.argv[2])
	render(W, H, sys.argv[3])
