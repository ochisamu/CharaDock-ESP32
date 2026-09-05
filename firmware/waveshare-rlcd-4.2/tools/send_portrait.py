#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Convert an image to fixed-phase monochrome and upload it atomically."""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import time
import zlib
from pathlib import Path

from protocol_serial import MAX_PAYLOAD, exchange_frame, require_success

WIDTH = 400
HEIGHT = 300
FRAME_ASSET_META = 0x50
FRAME_ASSET_CHUNK = 0x51
FRAME_ASSET_END = 0x52

BAYER_4 = (
    (0, 8, 2, 10),
    (12, 4, 14, 6),
    (3, 11, 1, 9),
    (15, 7, 13, 5),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--port", required=True, help="Serial port, for example COM7")
    parser.add_argument("--baud", type=int, default=500_000)
    parser.add_argument("--revision")
    parser.add_argument("--frame", default="portrait")
    parser.add_argument("--style", choices=("illustration", "manga"), default="illustration")
    parser.add_argument("--startup-wait", type=float, default=1.2)
    return parser.parse_args()


def safe_identifier(value: str, *, allow_colon: bool, maximum: int) -> bytes:
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
    if allow_colon:
        allowed += ":"
    encoded = value.encode("ascii", errors="strict")
    if not encoded or len(encoded) > maximum or any(chr(byte) not in allowed for byte in encoded):
        raise ValueError(f"unsafe identifier: {value!r}")
    return encoded


def convert_to_raw1(path: Path, style: str) -> bytes:
    try:
        from PIL import Image, ImageEnhance, ImageFilter, ImageOps
    except ImportError as error:
        raise RuntimeError("Pillow is required: py -m pip install pillow") from error

    with Image.open(path) as source:
        rgba = ImageOps.fit(
            source.convert("RGBA"),
            (WIDTH, HEIGHT),
            method=Image.Resampling.LANCZOS,
            centering=(0.5, 0.42),
        )
        background = Image.new("RGBA", (WIDTH, HEIGHT), "white")
        background.alpha_composite(rgba)
        gray = ImageOps.autocontrast(background.convert("L"), cutoff=1)
        if style == "manga":
            gray = ImageEnhance.Contrast(gray).enhance(1.35)
            gray = gray.filter(ImageFilter.UnsharpMask(radius=1.0, percent=140, threshold=3))

    output = bytearray((WIDTH // 8) * HEIGHT)
    pixels = gray.load()
    threshold_span = 112 if style == "illustration" else 72
    threshold_base = 128 - threshold_span // 2
    for y in range(HEIGHT):
        for x in range(WIDTH):
            threshold = threshold_base + (BAYER_4[y & 3][x & 3] * threshold_span // 15)
            if pixels[x, y] < threshold:
                output[y * (WIDTH // 8) + x // 8] |= 0x80 >> (x & 7)
    return bytes(output)


def metadata_payload(pixels: bytes, revision: bytes, frame_name: bytes) -> bytes:
    return struct.pack(
        "<BHHIIBB",
        1,
        WIDTH,
        HEIGHT,
        len(pixels),
        zlib.crc32(pixels) & 0xFFFFFFFF,
        len(revision),
        len(frame_name),
    ) + revision + frame_name


def upload(args: argparse.Namespace) -> None:
    try:
        import serial
    except ImportError as error:
        raise RuntimeError("pyserial is required: py -m pip install pyserial") from error
    pixels = convert_to_raw1(args.image, args.style)
    revision = safe_identifier(
        args.revision or hashlib.sha256(pixels).hexdigest()[:32],
        allow_colon=True,
        maximum=64,
    )
    frame = safe_identifier(args.frame, allow_colon=False, maximum=32)

    with serial.Serial(args.port, args.baud, timeout=0.1, write_timeout=3.0) as port:
        time.sleep(max(0.0, args.startup_wait))
        port.reset_input_buffer()
        sequence = 1
        require_success(
            exchange_frame(
                port,
                FRAME_ASSET_META,
                sequence,
                metadata_payload(pixels, revision, frame),
            )
        )
        sequence += 1
        for offset in range(0, len(pixels), MAX_PAYLOAD - 4):
            chunk = pixels[offset : offset + MAX_PAYLOAD - 4]
            require_success(
                exchange_frame(
                    port,
                    FRAME_ASSET_CHUNK,
                    sequence,
                    struct.pack("<I", offset) + chunk,
                )
            )
            sequence += 1
            print(f"\rTransferred {offset + len(chunk):5d}/{len(pixels)} bytes", end="", flush=True)
        print()
        require_success(exchange_frame(port, FRAME_ASSET_END, sequence))
    print(f"Verified portrait {revision.decode()} ({args.style}).")


def main() -> int:
    try:
        upload(parse_args())
        return 0
    except (OSError, RuntimeError, TimeoutError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
