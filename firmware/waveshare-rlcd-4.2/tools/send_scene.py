#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Send an atomic Home, Conversation, Work, Offline, or Recovery scene."""

from __future__ import annotations

import argparse
import struct
import sys
import time
from datetime import datetime

from protocol_serial import exchange_frame, require_success

FRAME_TIME_SYNC = 0x34
FRAME_DISPLAY_SCENE = 0x55
FRAME_DISPLAY_TEXT = 0x56
FRAME_DISPLAY_COMMIT = 0x59

SCENES = {"home": 0, "conversation": 1, "work": 2, "offline": 3, "recovery": 4}
STATES = {
    "idle": 0,
    "listening": 1,
    "thinking": 2,
    "speaking": 3,
    "error": 4,
    "connecting": 5,
    "working": 6,
    "completed": 7,
    "approval": 8,
    "offline": 9,
}
TEXT_TARGETS = {"caption": 0, "activity": 1, "next": 2, "footer": 3}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Serial port, for example COM7")
    parser.add_argument("--baud", type=int, default=500_000)
    parser.add_argument("--scene", choices=SCENES, default="home")
    parser.add_argument("--state", choices=STATES, default="idle")
    parser.add_argument("--character", default="CharaDock")
    parser.add_argument("--mode-label", default="")
    parser.add_argument("--caption", default="")
    parser.add_argument("--activity", default="")
    parser.add_argument("--next", dest="next_action", default="")
    parser.add_argument("--footer", default="")
    parser.add_argument("--elapsed", type=int, default=0)
    parser.add_argument("--artifacts", type=int, default=0)
    parser.add_argument("--live", action="store_true")
    parser.add_argument("--beatrice", action="store_true")
    parser.add_argument("--approval", action="store_true")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--revision", type=int)
    parser.add_argument("--startup-wait", type=float, default=1.2)
    parser.add_argument("--no-time-sync", action="store_true")
    return parser.parse_args()


def display_text(value: str, *, maximum: int, field: str) -> bytes:
    encoded = value.encode("utf-8")
    if len(encoded) > maximum:
        raise ValueError(f"{field} exceeds {maximum} UTF-8 bytes")
    if any(ord(character) < 0x20 and character != "\n" for character in value):
        raise ValueError(f"{field} contains a control character")
    return encoded


def scene_payload(args: argparse.Namespace, revision: int) -> bytes:
    name = display_text(args.character, maximum=48, field="character")
    mode = display_text(args.mode_label, maximum=24, field="mode label")
    if not name or "\n" in args.character or "\n" in args.mode_label:
        raise ValueError("character must be non-empty and header fields cannot contain newlines")
    flags = (0 if args.offline else 0x01) | (0x02 if args.live else 0)
    flags |= 0x04 if args.beatrice else 0
    flags |= 0x08 if args.approval else 0
    if not 0 <= args.elapsed <= 0xFFFFFFFF or not 0 <= args.artifacts <= 0xFFFF:
        raise ValueError("elapsed or artifact count is out of range")
    return struct.pack(
        "<BBBBIIHBB",
        1,
        SCENES[args.scene],
        STATES[args.state],
        flags,
        revision,
        args.elapsed,
        args.artifacts,
        len(name),
        len(mode),
    ) + name + mode


def text_payload(revision: int, target: str, value: str, font_size: int) -> bytes:
    maximum = {"caption": 1024, "activity": 384, "next": 256, "footer": 160}[target]
    encoded = display_text(value, maximum=maximum, field=target)
    return struct.pack(
        "<BBBBIH", 1, TEXT_TARGETS[target], font_size, 0, revision, len(encoded)
    ) + encoded


def time_sync_payload() -> bytes:
    now = datetime.now().astimezone()
    offset = now.utcoffset()
    offset_minutes = int(offset.total_seconds() // 60) if offset else 0
    return struct.pack("<BQh", 1, int(time.time()), offset_minutes)


def upload(args: argparse.Namespace) -> None:
    try:
        import serial
    except ImportError as error:
        raise RuntimeError("pyserial is required: py -m pip install pyserial") from error

    revision = args.revision or (int(time.time() * 1000) & 0xFFFFFFFF) or 1
    fields = (
        ("caption", args.caption, 16),
        ("activity", args.activity, 16),
        ("next", args.next_action, 12),
        ("footer", args.footer, 12),
    )
    with serial.Serial(args.port, args.baud, timeout=0.1, write_timeout=3.0) as port:
        time.sleep(max(0.0, args.startup_wait))
        port.reset_input_buffer()
        sequence = 1
        if not args.no_time_sync:
            require_success(
                exchange_frame(port, FRAME_TIME_SYNC, sequence, time_sync_payload())
            )
            sequence += 1
        require_success(
            exchange_frame(port, FRAME_DISPLAY_SCENE, sequence, scene_payload(args, revision))
        )
        sequence += 1
        for target, value, font_size in fields:
            if not value:
                continue
            require_success(
                exchange_frame(
                    port,
                    FRAME_DISPLAY_TEXT,
                    sequence,
                    text_payload(revision, target, value, font_size),
                )
            )
            sequence += 1
        require_success(
            exchange_frame(
                port,
                FRAME_DISPLAY_COMMIT,
                sequence,
                struct.pack("<BI", 1, revision),
            )
        )
    print(f"Committed {args.scene} revision {revision}.")


def main() -> int:
    try:
        upload(parse_args())
        return 0
    except (OSError, RuntimeError, TimeoutError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
