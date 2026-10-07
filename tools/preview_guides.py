# SPDX-FileCopyrightText: 2026 VectorText contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Show complete production-rendered rows with measured Y guide metadata.

Usage: python tools/preview_guides.py [build/render-qa/bgra-guides.bmp]
Requires Pillow. No game files, font offsets or rendering parameters are edited.
"""
import json
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageOps

source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "build/render-qa/bgra-guides.bmp"
metadata = json.loads(Path(str(source) + ".json").read_text(encoding="utf-8"))
image = Image.open(source).convert("RGB")
font = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", 16)
details = []
for index in (1, 2, 5, 12):
    row = metadata["rows"][index]
    y, top, bottom = row["y"], row["inkTop"], row["inkBottom"]
    # Bounds come from the same raster cells as production, not visible ink
    # after guide compositing. Include all overhang AND the lower guide.
    first = y + min(0, top) - 4
    last = y + max(metadata["cellHeight"] + 1, bottom) + 4
    assert 0 <= first < last <= image.height, "Complete row exceeds preview canvas"
    right = min(image.width, row["width"] + 8)
    strip = image.crop((0, first, right, last))
    # Natural row width includes the full last glyph and extra right padding.
    strip = ImageOps.expand(strip, border=4, fill="black")
    zoom = strip.resize((strip.width*3, strip.height*3), Image.Resampling.NEAREST)
    details.append((row, strip, zoom))

width = max(900, max(zoom.width for _, _, zoom in details))
height = 82 + sum(strip.height + zoom.height + 72 for _, strip, zoom in details)
canvas = Image.new("RGB", (width, height), (24, 24, 24))
draw = ImageDraw.Draw(canvas)
draw.text((12, 8), "辅助线固定于游戏行框：红=上 Y；蓝=中 Y+8；黄=下 Y+16。", font=font, fill="white")
draw.text((12, 34), f"共享基线=Y+{metadata['baseline']}；上方原尺寸，下方 3 倍最近邻放大。辅助线不进入游戏。", font=font, fill="white")
offset = 82
for row, strip, zoom in details:
    caption = f"第 {row['index']+1} 行：实际墨迹 Y{row['inkTop']:+d} 到 Y{row['inkBottom']-1:+d}；下边界为 Y{row['inkBottom']:+d}（不含该像素）"
    draw.text((12, offset), caption, font=font, fill="white")
    offset += 28
    canvas.paste(strip, (0, offset))
    offset += strip.height + 8
    canvas.paste(zoom, (0, offset))
    # Mark the real baseline beside, rather than over, the glyphs.
    baseline = (4 + metadata["baseline"] - min(0, row["inkTop"]) + 4) * 3
    draw.line((0, offset+baseline, 9, offset+baseline), fill=(218, 145, 255), width=2)
    offset += zoom.height + 36
destination = source.with_suffix(".png")
canvas.save(destination)
print(destination)
