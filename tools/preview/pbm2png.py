"""Convert out/*.pbm to OLED-looking PNGs and a contact sheet."""
import glob, os, sys
from PIL import Image, ImageDraw

d = sys.argv[1] if len(sys.argv) > 1 else "out"
S = 4
ON, OFF, GAP = (205, 236, 255), (8, 12, 18), (20, 26, 36)
tiles = []
for p in sorted(glob.glob(os.path.join(d, "*.pbm"))):
    im = Image.open(p).convert("1")
    big = Image.new("RGB", (128 * S, 64 * S), OFF)
    px = im.load()
    dr = ImageDraw.Draw(big)
    for y in range(64):
        for x in range(128):
            if px[x, y] == 0:  # PBM: 1 = black in file, PIL inverts
                dr.rectangle([x * S, y * S, x * S + S - 2, y * S + S - 2], fill=ON)
    out = p[:-4] + ".png"
    big.save(out)
    os.remove(p)
    tiles.append((os.path.basename(out)[:-4], big))
cols = 3
w, h = 128 * S, 64 * S
lab = 26
rows = (len(tiles) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (w + 16) + 16, rows * (h + lab + 16) + 16), (233, 236, 239))
dr = ImageDraw.Draw(sheet)
for i, (name, im) in enumerate(tiles):
    x = 16 + (i % cols) * (w + 16)
    y = 16 + (i // cols) * (h + lab + 16)
    dr.text((x, y), name, fill=(28, 36, 51))
    sheet.paste(im, (x, y + lab - 8))
sheet.save(os.path.join(d, "_sheet.png"))
print(f"{len(tiles)} screens -> {d}/_sheet.png")
