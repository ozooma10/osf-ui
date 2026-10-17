"""Require both independent page markers in a completed game-window capture."""
import json
import sys
from PIL import Image

image = Image.open(sys.argv[1]).convert("RGB")
magenta = green = 0
pixels = image.tobytes()
for r, g, b in zip(pixels[0::3], pixels[1::3], pixels[2::3]):
    magenta += r > 150 and b > 150 and g < 100
    green += g > 150 and r < 100 and b < 100
result = {"path": sys.argv[1], "magenta_pixels": magenta, "green_pixels": green}
print(json.dumps(result))
if min(magenta, green) < 100:
    raise SystemExit("Both browser markers must be visible in the actual game capture")
