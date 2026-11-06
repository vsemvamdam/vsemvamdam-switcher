# Builds res/app.ico and res/icon256.png from the author's avatar (the common icon for all vsemvamdam programs).
# Usage: python3 tools/make_icon.py <source.ico|.png> res/app.ico res/icon256.png
# A source .ico is copied as is (its hand-made frames are kept); a .png is resampled into the standard sizes.
import shutil
import sys

from PIL import Image

src, out_ico, out_png = sys.argv[1], sys.argv[2], sys.argv[3]
im = Image.open(src)
if src.lower().endswith(".ico"):
    shutil.copyfile(src, out_ico)
    im.size = max(im.ico.sizes())
    im.load()
else:
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    im.convert("RGBA").save(out_ico, sizes=[(s, s) for s in sizes])
im.convert("RGBA").resize((256, 256), Image.LANCZOS).save(out_png)
