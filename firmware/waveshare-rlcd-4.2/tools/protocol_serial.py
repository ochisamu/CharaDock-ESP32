#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""CharaDock protocol-v2 serial framing and acknowledgement helpers."""

from __future__ import annotations

import struct
import time
import zlib
from dataclasses import dataclass
from typing import BinaryIO

PROTOCOL_VERSION = 2
MAX_PAYLOAD = 4096
FRAME_ACK = 0x7E
FRAME_ERROR = 0x7F

APPLY_RESULTS = {
    0: "applied",
    1: "asset-completed",
    2: "asset-cache-hit",
    3: "ignored",
    4: "invalid-payload",
    5: "asset-rejected",
    6: "audio-rejected",
}
ASSET_RESULTS = {
    0: "ok",
    1: "storage-unavailable",
    2: "invalid-metadata",
    3: "transfer-not-active",
    4: "unexpected-offset",
    5: "too-large",
    6: "incomplete",
    7: "checksum-mismatch",
}
AUDIO_RESULTS = {
    0: "ok",
    1: "unavailable",
    2: "invalid-format",
    3: "invalid-state",
    4: "buffer-full",
    5: "codec-failure",
}


@dataclass(frozen=True)
class Acknowledgement:
    response_type: int
    sequence: int
    request_type: int
    apply_result: int
    asset_result: int
    audio_result: int

    @property
    def accepted(self) -> bool:
        return self.response_type == FRAME_ACK and self.apply_result in (0, 1, 2, 3)

    def details(self) -> str:
        return (
            APPLY_RESULTS.get(self.apply_result, str(self.apply_result))
            + ", asset="
            + ASSET_RESULTS.get(self.asset_result, str(self.asset_result))
            + ", audio="
            + AUDIO_RESULTS.get(self.audio_result, str(self.audio_result))
        )


def encode_frame(frame_type: int, sequence: int, payload: bytes = b"") -> bytes:
    if not 0 <= frame_type <= 0xFF:
        raise ValueError("frame type must fit in one byte")
    if not 0 <= sequence <= 0xFFFF:
        raise ValueError("sequence must fit in two bytes")
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("protocol payload exceeds 4096 bytes")
    prefix = struct.pack(
        "<2sBBHH", b"CD", PROTOCOL_VERSION, frame_type, sequence, len(payload)
    )
    checksum = zlib.crc32(prefix[2:] + payload) & 0xFFFFFFFF
    return prefix + struct.pack("<I", checksum) + payload


class FrameReader:
    """Incremental reader that survives debug text and unsolicited frames."""

    def __init__(self, serial_port: BinaryIO):
        self.serial_port = serial_port
        self.buffer = bytearray()

    def read_frame(self, timeout_seconds: float = 3.0) -> tuple[int, int, bytes]:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            frame = self._extract_frame()
            if frame is not None:
                return frame
            block = self.serial_port.read(512)
            if block:
                self.buffer.extend(block)
        frame = self._extract_frame()
        if frame is not None:
            return frame
        raise TimeoutError("RLCD did not return a protocol response")

    def _extract_frame(self) -> tuple[int, int, bytes] | None:
        while True:
            magic = self.buffer.find(b"CD")
            if magic < 0:
                self.buffer[:] = b"C" if self.buffer[-1:] == b"C" else b""
                return None
            if magic:
                del self.buffer[:magic]
            if len(self.buffer) < 12:
                return None
            version, frame_type, sequence, payload_length, expected_crc = (
                struct.unpack_from("<BBHHI", self.buffer, 2)
            )
            if version != PROTOCOL_VERSION or payload_length > MAX_PAYLOAD:
                del self.buffer[0]
                continue
            frame_length = 12 + payload_length
            if len(self.buffer) < frame_length:
                return None
            payload = bytes(self.buffer[12:frame_length])
            actual_crc = (
                zlib.crc32(bytes(self.buffer[2:8]) + payload) & 0xFFFFFFFF
            )
            del self.buffer[:frame_length]
            if actual_crc == expected_crc:
                return frame_type, sequence, payload


def read_frame(
    serial_port: BinaryIO, timeout_seconds: float = 3.0
) -> tuple[int, int, bytes]:
    return FrameReader(serial_port).read_frame(timeout_seconds)


def exchange_frame(
    serial_port: BinaryIO,
    frame_type: int,
    sequence: int,
    payload: bytes = b"",
    *,
    timeout_seconds: float = 3.0,
    reader: FrameReader | None = None,
) -> Acknowledgement:
    reader = reader or FrameReader(serial_port)
    serial_port.write(encode_frame(frame_type, sequence, payload))
    serial_port.flush()
    deadline = time.monotonic() + timeout_seconds
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("RLCD did not acknowledge the requested frame")
        response_type, response_sequence, response_payload = reader.read_frame(
            remaining
        )
        # Boot hello, sensor reports, input events, and delayed responses may
        # legitimately share one USB read with the acknowledgement we need.
        if response_sequence != sequence or response_type not in (
            FRAME_ACK,
            FRAME_ERROR,
        ):
            continue
        if len(response_payload) != 4 or response_payload[0] != frame_type:
            raise RuntimeError("malformed acknowledgement")
        break
    return Acknowledgement(
        response_type,
        response_sequence,
        response_payload[0],
        response_payload[1],
        response_payload[2],
        response_payload[3],
    )


def request_frame(
    serial_port: BinaryIO,
    frame_type: int,
    sequence: int,
    payload: bytes = b"",
    *,
    response_type: int | None = None,
    timeout_seconds: float = 3.0,
    reader: FrameReader | None = None,
) -> tuple[int, int, bytes]:
    """Request one same-sequence response while ignoring unsolicited frames."""

    reader = reader or FrameReader(serial_port)
    expected_type = frame_type if response_type is None else response_type
    serial_port.write(encode_frame(frame_type, sequence, payload))
    serial_port.flush()
    deadline = time.monotonic() + timeout_seconds
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(
                f"RLCD did not return frame 0x{expected_type:02x}"
            )
        received_type, received_sequence, received_payload = reader.read_frame(
            remaining
        )
        if received_sequence == sequence and received_type == expected_type:
            return received_type, received_sequence, received_payload


def require_success(acknowledgement: Acknowledgement) -> None:
    if not acknowledgement.accepted:
        raise RuntimeError(
            f"RLCD rejected frame 0x{acknowledgement.request_type:02x}: "
            f"{acknowledgement.details()}"
        )
