#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Shared protocol-v2 serial framing and acknowledgement helpers."""

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
    1: "portrait-completed",
    2: "portrait-cache-hit",
    3: "ignored",
    4: "invalid-payload",
    5: "portrait-rejected",
    6: "audio-rejected",
}
PORTRAIT_RESULTS = {
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
    1: "storage-unavailable",
    2: "invalid-format",
    3: "session-not-active",
    4: "invalid-chunk",
    5: "buffer-full",
    6: "sequence-conflict",
    7: "duplicate",
    8: "already-ended",
}


@dataclass(frozen=True)
class Acknowledgement:
    response_type: int
    sequence: int
    request_type: int
    apply_result: int
    portrait_result: int
    audio_result: int | None

    @property
    def accepted(self) -> bool:
        return self.response_type == FRAME_ACK and self.apply_result in (0, 1, 2)

    def details(self) -> str:
        apply_name = APPLY_RESULTS.get(self.apply_result, str(self.apply_result))
        portrait_name = PORTRAIT_RESULTS.get(
            self.portrait_result, str(self.portrait_result)
        )
        details = f"{apply_name}, portrait={portrait_name}"
        if self.audio_result is not None:
            audio_name = AUDIO_RESULTS.get(self.audio_result, str(self.audio_result))
            details += f", audio={audio_name}"
        return details


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


def read_frame(
    serial_port: BinaryIO, timeout_seconds: float = 3.0
) -> tuple[int, int, bytes]:
    deadline = time.monotonic() + timeout_seconds
    buffer = bytearray()
    while time.monotonic() < deadline:
        block = serial_port.read(512)
        if block:
            buffer.extend(block)
        while True:
            magic = buffer.find(b"CD")
            if magic < 0:
                if buffer[-1:] == b"C":
                    buffer[:] = b"C"
                else:
                    buffer.clear()
                break
            if magic:
                del buffer[:magic]
            if len(buffer) < 12:
                break
            version, frame_type, sequence, payload_length, expected_crc = (
                struct.unpack_from("<BBHHI", buffer, 2)
            )
            if version != PROTOCOL_VERSION or payload_length > MAX_PAYLOAD:
                del buffer[0]
                continue
            frame_length = 12 + payload_length
            if len(buffer) < frame_length:
                break
            payload = bytes(buffer[12:frame_length])
            actual_crc = zlib.crc32(bytes(buffer[2:8]) + payload) & 0xFFFFFFFF
            del buffer[:frame_length]
            if actual_crc == expected_crc:
                return frame_type, sequence, payload
    raise TimeoutError("StackChan did not return a protocol response")


def exchange_frame(
    serial_port: BinaryIO,
    frame_type: int,
    sequence: int,
    payload: bytes = b"",
    *,
    timeout_seconds: float = 3.0,
) -> Acknowledgement:
    serial_port.write(encode_frame(frame_type, sequence, payload))
    serial_port.flush()
    response_type, response_sequence, response_payload = read_frame(
        serial_port, timeout_seconds
    )
    if response_sequence != sequence:
        raise RuntimeError(
            f"response sequence mismatch: sent {sequence}, received "
            f"{response_sequence}"
        )
    # Three-byte responses are accepted for compatibility with the initial
    # portrait-only firmware. Audio-aware firmware appends audio-result byte 3.
    if len(response_payload) not in (3, 4) or response_payload[0] != frame_type:
        raise RuntimeError("malformed acknowledgement payload")
    if response_type not in (FRAME_ACK, FRAME_ERROR):
        raise RuntimeError(f"unexpected response frame: 0x{response_type:02x}")
    return Acknowledgement(
        response_type=response_type,
        sequence=response_sequence,
        request_type=response_payload[0],
        apply_result=response_payload[1],
        portrait_result=response_payload[2],
        audio_result=response_payload[3] if len(response_payload) == 4 else None,
    )


def require_success(acknowledgement: Acknowledgement) -> None:
    if acknowledgement.response_type == FRAME_ERROR:
        raise RuntimeError(
            f"StackChan rejected frame 0x{acknowledgement.request_type:02x}: "
            f"{acknowledgement.details()}"
        )
    if not acknowledgement.accepted:
        raise RuntimeError(
            "acknowledgement carried failure result: "
            f"{acknowledgement.details()}"
        )
