#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import importlib.util
import struct
import sys
import unittest
from pathlib import Path


TOOLS_DIR = Path(__file__).parents[1] / "tools"
sys.path.insert(0, str(TOOLS_DIR))


def load_tool():
    path = TOOLS_DIR / "send_portrait.py"
    spec = importlib.util.spec_from_file_location("send_portrait", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load send_portrait.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tool = load_tool()


class FragmentedSerial:
    def __init__(self, chunks: list[bytes]):
        self.chunks = list(chunks)

    def read(self, _count: int) -> bytes:
        return self.chunks.pop(0) if self.chunks else b""


class ExchangeSerial:
    def __init__(self, response: bytes):
        self.response = bytearray(response)
        self.request = b""

    def write(self, request: bytes) -> int:
        self.request = bytes(request)
        return len(request)

    def flush(self) -> None:
        pass

    def read(self, count: int) -> bytes:
        block = bytes(self.response[:count])
        del self.response[:count]
        return block


class PortraitToolTests(unittest.TestCase):
    def test_frame_matches_cpp_known_vector(self) -> None:
        encoded = tool.encode_frame(tool.FRAME_DISPLAY_MODE, 0x1234, b"\x01")
        self.assertEqual(encoded.hex(), "4344025434120100455f984501")

    def test_reader_skips_text_and_reassembles_fragments(self) -> None:
        encoded = tool.encode_frame(tool.FRAME_ACK, 7, b"\x50\x00\x00\x00")
        serial_port = FragmentedSerial(
            [b"boot log\r\nnoise", encoded[:3], encoded[3:10], encoded[10:]]
        )
        frame_type, sequence, payload = tool.read_frame(serial_port, 0.1)
        self.assertEqual(frame_type, tool.FRAME_ACK)
        self.assertEqual(sequence, 7)
        self.assertEqual(payload, b"\x50\x00\x00\x00")

    def test_acknowledgement_accepts_old_and_audio_aware_payloads(self) -> None:
        for payload, expected_audio in (
            (b"\x50\x00\x00", None),
            (b"\x50\x00\x00\x07", 7),
        ):
            encoded = tool.encode_frame(tool.FRAME_ACK, 9, payload)
            acknowledgement = tool.exchange_frame(
                ExchangeSerial(encoded), tool.FRAME_ASSET_META, 9
            )
            self.assertTrue(acknowledgement.accepted)
            self.assertEqual(acknowledgement.audio_result, expected_audio)

    def test_metadata_layout_is_little_endian_and_bounded(self) -> None:
        pixels = bytes(tool.WIDTH * tool.HEIGHT * 2)
        payload = tool.metadata_payload(pixels, b"revision-1", b"portrait")
        fields = struct.unpack_from("<BHHIIBB", payload)
        self.assertEqual(fields[:4], (0, 320, 240, 153600))
        self.assertEqual(fields[5:], (10, 8))
        self.assertEqual(payload[15:], b"revision-1portrait")

    def test_identifier_policy_matches_firmware(self) -> None:
        self.assertEqual(
            tool.safe_identifier(
                "amber:stackchan-v1", allow_colon=True, maximum=64
            ),
            b"amber:stackchan-v1",
        )
        with self.assertRaises(ValueError):
            tool.safe_identifier("../portrait", allow_colon=False, maximum=32)


if __name__ == "__main__":
    unittest.main()
