#!/usr/bin/env python3
"""Generate RGB565 weather assets from the MIT-licensed Weather Icons set.

Generation dependencies: Pillow and resvg-py. The ESP32 firmware only embeds
the resulting RGB565 data and does not need either dependency at runtime.
"""

from __future__ import annotations

import io
import struct
from pathlib import Path
from urllib.request import Request, urlopen

from PIL import Image, ImageDraw
from resvg_py import svg_to_bytes

SOURCE_COMMIT = "3793c6a1bf5c52402bb09e5d062b72f9fb5c0410"
SOURCE_ROOT = (
    "https://raw.githubusercontent.com/Makin-Things/weather-icons/"
    f"{SOURCE_COMMIT}/static"
)
ICON_NAMES = (
    "clear-day",
    "clear-night",
    "cloudy-1-day",
    "cloudy-1-night",
    "cloudy",
    "fog",
    "rainy-3",
    "snowy-2",
    "thunderstorms",
)
WIDTH = 96
HEIGHT = 72
CONTENT_WIDTH = 90
CONTENT_HEIGHT = 66
CARD_BACKGROUND = (22, 36, 51, 255)


def download(name: str) -> bytes:
    request = Request(f"{SOURCE_ROOT}/{name}.svg", headers={"User-Agent": "esp32-box2"})
    with urlopen(request, timeout=30) as response:
        return response.read()


def render(svg: bytes) -> Image.Image:
    png = svg_to_bytes(
        svg_string=svg.decode("utf-8"),
        width=WIDTH * 2,
        height=HEIGHT * 2,
        skip_system_fonts=True,
    )
    foreground = Image.open(io.BytesIO(png)).convert("RGBA")
    bounds = foreground.getchannel("A").getbbox()
    if bounds:
        foreground = foreground.crop(bounds)
        scale = min(
            CONTENT_WIDTH / foreground.width,
            CONTENT_HEIGHT / foreground.height,
        )
        foreground = foreground.resize(
            (max(1, round(foreground.width * scale)),
             max(1, round(foreground.height * scale))),
            Image.Resampling.LANCZOS,
        )
    background = Image.new("RGBA", (WIDTH, HEIGHT), CARD_BACKGROUND)
    position = ((WIDTH - foreground.width) // 2, (HEIGHT - foreground.height) // 2)
    background.alpha_composite(foreground, position)
    return background.convert("RGB")


def rgb565_bytes(image: Image.Image) -> bytes:
    output = bytearray()
    for red, green, blue in image.getdata():
        value = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        output.extend(struct.pack("<H", value))
    return bytes(output)


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    frames = [render(download(name)) for name in ICON_NAMES]
    payload = b"".join(rgb565_bytes(frame) for frame in frames)
    output_path = root / "main" / "weather_icons.bin"
    output_path.write_bytes(payload)

    preview = Image.new("RGB", (WIDTH * 3, (HEIGHT + 16) * 3), CARD_BACKGROUND[:3])
    draw = ImageDraw.Draw(preview)
    for index, (name, frame) in enumerate(zip(ICON_NAMES, frames)):
        x = (index % 3) * WIDTH
        y = (index // 3) * (HEIGHT + 16)
        preview.paste(frame, (x, y))
        draw.text((x + 2, y + HEIGHT), name, fill=(210, 224, 236))
    preview_path = root / "build" / "weather_icons_preview.png"
    preview.save(preview_path)
    print(f"wrote {output_path}: {len(payload)} bytes")
    print(f"wrote {preview_path}")


if __name__ == "__main__":
    main()
