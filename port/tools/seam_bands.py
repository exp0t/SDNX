# Seam per horizontal band of a debug_composite band image: mean column step around x (default 1100..1140).
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('L'); px = im.load()
names = sys.argv[2].split(',') if len(sys.argv) > 2 else [str(i) for i in range(6)]
for b in range(6):
    y0, y1 = b * 118 + 4, min(b * 118 + 114, 704)
    st = {x: sum(abs(px[x, y] - px[x - 1, y]) for y in range(y0, y1)) / (y1 - y0) for x in range(1001, 1199)}
    mean = sum(st.values()) / len(st); avg_l = sum(px[x, y] for x in range(1060, 1115) for y in range(y0, y1, 4)) / (55 * len(range(y0, y1, 4)))
    avg_r = sum(px[x, y] for x in range(1125, 1180) for y in range(y0, y1, 4)) / (55 * len(range(y0, y1, 4)))
    print('%-10s mean %.2f  x1118..1123: %s  left %.1f right %.1f' % (names[b], mean, ' '.join('%.1f' % st[x] for x in range(1118, 1124)), avg_l, avg_r))
