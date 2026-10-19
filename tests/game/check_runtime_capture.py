"""Read the healthy HUD's binary frame marker from an actual game capture."""
import json
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert('RGB')
# WGC includes the window's title bar/borders. Locate the unique magenta frame
# instead of assuming capture coordinates equal browser client coordinates.
points = [(x, y) for y in range(im.height // 5) for x in range(im.width // 3)
          if (lambda rgb: rgb[0] > 200 and rgb[1] < 60 and rgb[2] > 200)(im.getpixel((x, y)))]
if not points:
    raise SystemExit('Healthy HUD marker frame is missing')
left, top = min(x for x, _ in points), min(y for _, y in points)
right, bottom = max(x for x, _ in points), max(y for _, y in points)
scale = (right - left + 1) / 304
if abs((bottom - top + 1) / scale - 48) > 3:
    raise SystemExit('Healthy HUD marker frame is malformed')
value = 0
for bit in range(12):
    rgb = im.getpixel((round(left + (8 + bit * 24 + 12) * scale), round(top + 24 * scale)))
    if min(rgb) > 160:
        value |= 1 << bit
    elif max(rgb) > 70:
        raise SystemExit(f'Invalid marker bit {bit}: {rgb}')
anchor = im.getpixel((round(left + (424 - 32) * scale), round(top + 24 * scale)))
if not (anchor[1] > 150 and anchor[0] < 100 and anchor[2] < 100):
    raise SystemExit(f'Healthy HUD anchor is missing: {anchor}')
print(json.dumps({'path': sys.argv[1], 'frame': value, 'anchor': anchor}))
