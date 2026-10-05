import cairo, airos_art as a
S=400
surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, S*2, S)
ctx = cairo.Context(surf)
def rgb(c): return tuple(v/255 for v in c)
ctx.set_source_rgb(*rgb(a.CLOUD)); ctx.paint()
reg = a.regions()
order = [('sky',a.SKY),('ring',a.CLOUD),('sun',a.DAWN),('band',a.BREEZE),('bottom',a.BREEZE)]
for k,c in order:
    a.cairo_path(ctx, a.transform(reg[k], 180, 200, 200)); ctx.set_source_rgb(*rgb(c)); ctx.fill()
surf.write_to_png('/mnt/HaikuWork/airos/art/ref/flat-left.png')
from PIL import Image
ref = Image.open('/home/jmgasper/Downloads/airOSLogo.png').convert('RGB')
# reference circle center 312.12,224.84 R 153.55 -> scale to R=180 centered 200,200
sc = 180/153.55
crop = ref.crop((int(312.12-200/sc), int(224.84-200/sc), int(312.12+200/sc), int(224.84+200/sc))).resize((S,S), Image.LANCZOS)
out = Image.open('/mnt/HaikuWork/airos/art/ref/flat-left.png').convert('RGB')
out.paste(crop, (S,0))
out.save('/mnt/HaikuWork/airos/art/ref/compare.png')
