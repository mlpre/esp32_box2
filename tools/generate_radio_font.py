#!/usr/bin/env python3
"""Build the project-local 16 px bitmap font from GNU Unifont HEX data.

GNU Unifont 17.0.04 is distributed under SIL OFL 1.1 or GPL-2.0-or-later
with the GNU Font Embedding Exception. This project uses the OFL option;
see LICENSES/UNIFONT-OFL-1.1.txt.
"""

from __future__ import annotations

import gzip
import hashlib
from pathlib import Path
from urllib.request import urlopen

UNIFONT_URL = (
    "https://ftp.gnu.org/gnu/unifont/unifont-17.0.04/"
    "unifont_all-17.0.04.hex.gz"
)
UNIFONT_SHA256 = "c31d210962408a00de8e2ebe2f2fc26824d7a4939d4eb15d347761fb2a0b39a6"

RANGES = (
    (0x0020, 0x007E),
    (0x00A0, 0x00FF),
    (0x2000, 0x206F),
    (0x3000, 0x303F),
    (0x3400, 0x4DBF),
    (0x4E00, 0x9FFF),
    (0xFF00, 0xFFEF),
)


def to_16_by_16(bitmap: bytes) -> bytes:
    if len(bitmap) == 32:
        return bitmap
    if len(bitmap) == 16:
        expanded = bytearray()
        for row in bitmap:
            expanded.extend((row, 0))
        return bytes(expanded)
    raise ValueError(f"unsupported Unifont glyph size: {len(bitmap)} bytes")


def main() -> None:
    output_path = Path(__file__).resolve().parents[1] / "main" / "radio_font.bin"
    with urlopen(UNIFONT_URL, timeout=60) as response:
        archive = response.read()
    actual_hash = hashlib.sha256(archive).hexdigest()
    if actual_hash != UNIFONT_SHA256:
        raise RuntimeError(
            f"GNU Unifont archive hash mismatch: {actual_hash}"
        )
    text = gzip.decompress(archive).decode("ascii")

    glyphs: dict[int, bytes] = {}
    for line in text.splitlines():
        codepoint_text, bitmap_text = line.split(":", 1)
        codepoint = int(codepoint_text, 16)
        if any(first <= codepoint <= last for first, last in RANGES):
            glyphs[codepoint] = to_16_by_16(bytes.fromhex(bitmap_text))

    fallback = glyphs[ord("?")]
    output = bytearray()
    missing = 0
    for first, last in RANGES:
        for codepoint in range(first, last + 1):
            bitmap = glyphs.get(codepoint)
            if bitmap is None:
                bitmap = fallback
                missing += 1
            output.extend(bitmap)

    output_path.write_bytes(output)
    print(
        f"wrote {output_path}: {len(output)} bytes, "
        f"{len(output) // 32} code points, {missing} fallbacks"
    )


if __name__ == "__main__":
    main()
