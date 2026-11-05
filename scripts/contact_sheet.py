# Project Oogie: tile the frames of a folder (logs\<run>\t*.png) into one
# image with each file name written on it, to look at many frames at once.
#   py -3 scripts\contact_sheet.py logs\cf20a [cols] [thumb_width] [out.png] [first] [last]
import sys, os
from PIL import Image, ImageDraw

d = sys.argv[1]
cols = int(sys.argv[2]) if len(sys.argv) > 2 else 6
tw = int(sys.argv[3]) if len(sys.argv) > 3 else 320
out = sys.argv[4] if len(sys.argv) > 4 else os.path.join(d, 'sheet.png')
names = sorted(f for f in os.listdir(d) if f.lower().endswith('.png') and f != os.path.basename(out) and not f.startswith('sheet'))
if len(sys.argv) > 6:
    names = names[int(sys.argv[5]):int(sys.argv[6])]
ims = []
for n in names:
    im = Image.open(os.path.join(d, n)).convert('RGB')
    th = int(im.height * tw / im.width)
    ims.append((n, im.resize((tw, th))))
th = ims[0][1].height
rows = (len(ims) + cols - 1) // cols
sheet = Image.new('RGB', (cols * tw, rows * th), (40, 40, 40))
dr = ImageDraw.Draw(sheet)
for k, (n, im) in enumerate(ims):
    x, y = (k % cols) * tw, (k // cols) * th
    sheet.paste(im, (x, y))
    dr.rectangle([x, y, x + 70, y + 14], fill=(0, 0, 0))
    dr.text((x + 2, y + 1), n[:-4], fill=(255, 255, 0))
sheet.save(out)
print(out, sheet.size)
