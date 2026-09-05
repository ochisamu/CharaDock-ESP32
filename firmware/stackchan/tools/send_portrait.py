#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Convert an image and send a protocol-v2 Static Portrait over USB serial."""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import time
import zlib
from pathlib import Path

from protocol_serial import (
    APPLY_RESULTS,
    AUDIO_RESULTS,
    FRAME_ACK,
    FRAME_ERROR,
    MAX_PAYLOAD,
    PORTRAIT_RESULTS,
    PROTOCOL_VERSION,
    encode_frame,
    exchange_frame,
    read_frame,
    require_success,
)

WIDTH = 320
HEIGHT = 240
FRAME_ASSET_META = 0x50
FRAME_ASSET_CHUNK = 0x51
FRAME_ASSET_END = 0x52
FRAME_DISPLAY_MODE = 0x54

DISPLAY_MODES = {"hybrid": 0, "character-art": 1, "native-face": 2}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Fit an image to 320x240, convert it to RGB565 little-endian, "
            "and stage it atomically on StackChan over USB."
        )
    )
    parser.add_argument("image", type=Path, help="PNG/JPEG/WebP source image")
    parser.add_argument(
        "--port", required=True, help="Serial port, for example COM3"
    )
    parser.add_argument("--baud", type=int, default=500_000)
    parser.add_argument(
        "--mode",
        choices=DISPLAY_MODES,
        default="hybrid",
        help="Display mode after upload",
    )
    parser.add_argument(
        "--background",
        default="#fff5df",
        help="RGB background used to composite transparency (default: #fff5df)",
    )
    parser.add_argument(
        "--revision",
        help="Safe revision identifier; defaults to the first 32 hex SHA-256 characters",
    )
    parser.add_argument("--frame", default="portrait", help="Safe frame name")
    parser.add_argument(
        "--startup-wait",
        type=float,
        default=1.5,
        help="Seconds to wait after opening the port (default: 1.5)",
    )
    return parser.parse_args()


def safe_identifier(value: str, *, allow_colon: bool, maximum: int) -> bytes:
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
    if allow_colon:
        allowed += ":"
    encoded = value.encode("ascii", errors="strict")
    if (
        not encoded
        or len(encoded) > maximum
        or any(chr(byte) not in allowed for byte in encoded)
    ):
        raise ValueError(f"unsafe identifier: {value!r}")
    return encoded


def parse_background(value: str) -> tuple[int, int, int, int]:
    normalized = value.removeprefix("#")
    if len(normalized) != 6:
        raise ValueError("background must be a six-digit RGB color")
    return (
        int(normalized[0:2], 16),
        int(normalized[2:4], 16),
        int(normalized[4:6], 16),
        255,
    )


def convert_to_rgb565(path: Path, background: str) -> bytes:
    try:
        from PIL import Image, ImageOps
    except ImportError as error:
        raise RuntimeError("Pillow is required: py -m pip install pillow") from error

    with Image.open(path) as source:
        fitted = ImageOps.fit(
            source.convert("RGBA"),
            (WIDTH, HEIGHT),
            method=Image.Resampling.LANCZOS,
            centering=(0.5, 0.5),
        )
        composite = Image.new("RGBA", (WIDTH, HEIGHT), parse_background(background))
        composite.alpha_composite(fitted)
        rgb = composite.convert("RGB")

    output = bytearray(WIDTH * HEIGHT * 2)
    offset = 0
    for red, green, blue in rgb.getdata():
        pixel = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        output[offset] = pixel & 0xFF
        output[offset + 1] = pixel >> 8
        offset += 2
    return bytes(output)


def send_and_require_ack(
    serial_port, frame_type: int, sequence: int, payload: bytes = b""
) -> None:
    require_success(exchange_frame(serial_port, frame_type, sequence, payload))


def metadata_payload(pixels: bytes, revision: bytes, frame_name: bytes) -> bytes:
    return struct.pack(
        "<BHHIIBB",
        0,
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

    pixels = convert_to_rgb565(args.image, args.background)
    default_revision = hashlib.sha256(pixels).hexdigest()[:32]
    revision = safe_identifier(
        args.revision or default_revision, allow_colon=True, maximum=64
    )
    frame_name = safe_identifier(args.frame, allow_colon=False, maximum=32)

    with serial.Serial(
        args.port, args.baud, timeout=0.1, write_timeout=3.0
    ) as port:
        time.sleep(max(0.0, args.startup_wait))
        port.reset_input_buffer()
        sequence = 1
        send_and_require_ack(
            port,
            FRAME_ASSET_META,
            sequence,
            metadata_payload(pixels, revision, frame_name),
        )
        sequence += 1
        chunk_data_bytes = MAX_PAYLOAD - 4
        for offset in range(0, len(pixels), chunk_data_bytes):
            chunk = pixels[offset : offset + chunk_data_bytes]
            send_and_require_ack(
                port, FRAME_ASSET_CHUNK, sequence, struct.pack("<I", offset) + chunk
            )
            sequence += 1
            completed = min(len(pixels), offset + len(chunk))
            print(
                f"\rTransferred {completed:6d}/{len(pixels)} bytes",
                end="",
                flush=True,
            )
        print()
        send_and_require_ack(port, FRAME_ASSET_END, sequence)
        sequence += 1
        send_and_require_ack(
            port, FRAME_DISPLAY_MODE, sequence, bytes([DISPLAY_MODES[args.mode]])
        )
    print(f"Portrait {revision.decode()} verified and selected in {args.mode} mode.")


def main() -> int:
    args = parse_args()
    try:
        upload(args)
    except (OSError, RuntimeError, TimeoutError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
