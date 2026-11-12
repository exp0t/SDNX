# Vertical seam finder: mean absolute step between adjacent columns of a PNG, printed around the columns where it is
# largest (relative to its neighbours). Usage: python seam.py image.png [x_from x_to]
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert('L')
px = im.load()
w, h = im.size
x0 = int(sys.argv[2]) if len(sys.argv) > 2 else 1
x1 = int(sys.argv[3]) if len(sys.argv) > 3 else w - 1
steps = {}
for x in range(max(1, x0), min(w, x1)):
    steps[x] = sum(abs(px[x, y] - px[x - 1, y]) for y in range(0, h, 2)) / (h / 2)
mean = sum(steps.values()) / len(steps)
top = sorted(steps, key=lambda x: -steps[x])[:6]
print(im.size, 'mean step %.2f' % mean, 'largest:', ', '.join('x=%d %.1f' % (x, steps[x]) for x in sorted(top)))
