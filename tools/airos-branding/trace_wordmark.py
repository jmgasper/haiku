"""Trace the air/os wordmark from the brand sheet into cubic paths
(wordmark_paths.json).  One-off: needs numpy and potracer (pip install
potracer); the first traced contour is the crop frame and is dropped."""
import json, numpy as np, potrace
from PIL import Image
import os
SHEET = os.path.join(os.path.dirname(__file__), '..', '..', 'data', 'artwork', 'airos', 'brand-sheet.png')
ref = Image.open(SHEET).convert('L')
box = (100, 380, 525, 525)
crop = ref.crop(box)
UP = 6
big = crop.resize((crop.width*UP, crop.height*UP), Image.LANCZOS)
arr = np.asarray(big).astype(float)
# text is dark (~50) on light (~245)
bm = potrace.Bitmap(arr < 150)
plist = bm.trace(turdsize=50, alphamax=1.0, opticurve=True, opttolerance=0.2)
paths = []
for curve in plist:
    segs = []
    start = curve.start_point
    pt = (start.x, start.y)
    for seg in curve.segments:
        if seg.is_corner:
            c = seg.c; e = seg.end_point
            segs.append([pt, pt, (c.x, c.y), (c.x, c.y)])
            segs.append([(c.x, c.y), (c.x, c.y), (e.x, e.y), (e.x, e.y)])
            pt = (e.x, e.y)
        else:
            c1, c2, e = seg.c1, seg.c2, seg.end_point
            segs.append([pt, (c1.x, c1.y), (c2.x, c2.y), (e.x, e.y)])
            pt = (e.x, e.y)
    # back to sheet coordinates
    segs = [[(x / UP + box[0], y / UP + box[1]) for x, y in s] for s in segs]
    paths.append(segs)
xs = [p[0] for path in paths for s in path for p in s]
ys = [p[1] for path in paths for s in path for p in s]
print('paths', len(paths), 'bbox', min(xs), max(xs), min(ys), max(ys))
json.dump(paths[1:], open(os.path.join(os.path.dirname(__file__), 'wordmark_paths.json'), 'w'))
