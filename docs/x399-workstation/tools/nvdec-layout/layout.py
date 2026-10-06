# Where the decoder puts the pixel at (x, y) of a plane, measured on the card.
# A 512 byte group of bytes covers 64 columns and 8 rows; groups run down a
# block of `bh` of them, then across, then down the picture.
def off(x, y, pitch, bh=2):
    blocks_per_row = pitch // 64
    block_y, gob_in_block = divmod(y // 8, bh)
    base = ((block_y * blocks_per_row + x // 64) * bh + gob_in_block) * 512
    xx, yy = x % 64, y % 8
    return (base + (xx // 32) * 256 + (yy // 4) * 128
            + ((xx % 32) // 16) * 64 + (yy % 4) * 16 + (xx % 16))

def untile(data, base, w, h, pitch, bh=2):
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            out[y * w + x] = data[base + off(x, y, pitch, bh)]
    return bytes(out)

def plane_size(pitch, h, bh=2):
    rows = ((h + 8 * bh - 1) // (8 * bh)) * 8 * bh
    return pitch * rows
