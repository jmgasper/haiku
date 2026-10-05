import sys, itertools

def gob_off(x, y):
    # standard Fermi+ 64x8 GOB byte swizzle
    return ((x // 32) * 256) + ((y // 2) * 64) + (((x % 32) // 16) * 32) + ((y % 2) * 16) + (x % 16)

def deswizzle(data, base, width_bytes, height, pitch_bytes, bh):
    out = bytearray(width_bytes * height)
    gobs_per_row = pitch_bytes // 64
    for y in range(height):
        gy, iy = divmod(y, 8)
        by, giy = divmod(gy, bh)
        for xg in range(width_bytes // 64):
            block = by * gobs_per_row + xg
            go = base + (block * bh + giy) * 512
            for x in range(64):
                out[y * width_bytes + xg * 64 + x] = data[go + gob_off(x, iy)]
    return bytes(out)

ref = open('t1.yuv','rb').read()
W,H = 320,240
refY = ref[:W*H]
best=None
for fn in ['out-1-0.nv12','out-1-1.nv12','out-1-2.nv12','out-256-1.nv12']:
    d = open(fn,'rb').read()
    for pitch in (320, 512):
        for bh in (1,2,4,8,16):
            need = (pitch//64)*((H+8*bh-1)//(8*bh))*bh*512
            if need > len(d): continue
            try:
                y = deswizzle(d, 0, W, H, pitch, bh)
            except IndexError:
                continue
            mse = sum((a-b)*(a-b) for a,b in zip(refY,y))/len(refY)
            if best is None or mse < best[0]:
                best = (mse, fn, pitch, bh)
            if mse < 50:
                print('MATCH', fn, 'pitch', pitch, 'block height', bh, 'mse', round(mse,2))
print('best:', best)
