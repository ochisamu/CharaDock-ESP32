#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

import importlib.util
import struct
import sys
import tempfile
import unittest
import zlib
from argparse import Namespace
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools"))

import protocol_serial
import diagnose_device
import send_portrait
import send_scene


class ProtocolToolTests(unittest.TestCase):
    def test_known_frame_vector(self) -> None:
        encoded = protocol_serial.encode_frame(0x54, 0x1234, b"\x01")
        self.assertEqual(
            encoded,
            bytes(
                [
                    0x43,
                    0x44,
                    0x02,
                    0x54,
                    0x34,
                    0x12,
                    0x01,
                    0x00,
                    0x45,
                    0x5F,
                    0x98,
                    0x45,
                    0x01,
                ]
            ),
        )

    def test_exchange_skips_debug_text_and_unsolicited_frames(self) -> None:
        request_type = 0x55
        sequence = 42
        unsolicited = protocol_serial.encode_frame(0x57, 99, bytes(18))
        acknowledgement = protocol_serial.encode_frame(
            protocol_serial.FRAME_ACK,
            sequence,
            bytes([request_type, 0, 0, 0]),
        )

        class FakeSerial:
            def __init__(self) -> None:
                self.response = b"# boot debug\r\n" + unsolicited + acknowledgement
                self.written = b""

            def write(self, value: bytes) -> int:
                self.written += value
                return len(value)

            def flush(self) -> None:
                pass

            def read(self, _length: int) -> bytes:
                value, self.response = self.response, b""
                return value

        port = FakeSerial()
        result = protocol_serial.exchange_frame(
            port, request_type, sequence, b"payload", timeout_seconds=0.1
        )
        self.assertTrue(result.accepted)
        self.assertEqual(result.sequence, sequence)
        self.assertEqual(
            port.written,
            protocol_serial.encode_frame(request_type, sequence, b"payload"),
        )

    def test_scene_transaction_payloads_are_bounded(self) -> None:
        args = Namespace(
            character="コハク",
            mode_label="Live + B2",
            offline=False,
            live=True,
            beatrice=True,
            approval=False,
            elapsed=138,
            artifacts=2,
            scene="conversation",
            state="speaking",
        )
        payload = send_scene.scene_payload(args, 21)
        fields = struct.unpack_from("<BBBBIIHBB", payload)
        self.assertEqual(fields[:4], (1, 1, 3, 7))
        self.assertEqual(fields[4:7], (21, 138, 2))
        self.assertIn("コハク".encode(), payload)

        text = send_scene.text_payload(21, "caption", "確認してみよう。", 16)
        self.assertEqual(text[:4], bytes([1, 0, 16, 0]))
        self.assertEqual(struct.unpack_from("<I", text, 4)[0], 21)

    def test_time_sync_includes_signed_timezone(self) -> None:
        payload = send_scene.time_sync_payload()
        self.assertEqual(len(payload), 11)
        version, unix_seconds, offset = struct.unpack("<BQh", payload)
        self.assertEqual(version, 1)
        self.assertGreater(unix_seconds, 1_700_000_000)
        self.assertGreaterEqual(offset, -720)
        self.assertLessEqual(offset, 840)

    def test_diagnostic_sensor_parser_and_capability_validation(self) -> None:
        payload = bytearray(18)
        payload[0] = 1
        payload[1] = 0x1F
        struct.pack_into("<h", payload, 2, 2634)
        struct.pack_into("<H", payload, 4, 5842)
        struct.pack_into("<H", payload, 6, 4012)
        payload[8] = 81
        struct.pack_into("<H", payload, 9, 2026)
        payload[11:16] = bytes([9, 2, 15, 42, 7])
        sensors = diagnose_device.parse_sensor_report(bytes(payload))
        self.assertEqual(sensors["temperatureC"], 26.34)
        self.assertEqual(sensors["humidityPercent"], 58.42)
        self.assertEqual(sensors["batteryVolts"], 4.012)
        self.assertTrue(sensors["available"]["microphoneCodec"])

        hello = {"board": diagnose_device.EXPECTED_BOARD}
        capabilities = {
            "protocol": 2,
            "board": diagnose_device.EXPECTED_BOARD,
            "capabilities": {
                "display": {
                    "width": 400,
                    "height": 300,
                    "bitsPerPixel": 1,
                    "bitmap": ["raw1-msb"],
                },
                "audio": {
                    "capture": True,
                    "playback": True,
                    "format": "pcm-s16le-mono",
                    "sampleRates": [16000],
                    "duplex": "half",
                },
                "network": {
                    "wifi": True,
                    "provisioning": "usb-only",
                    "authentication": "mutual-hmac-sha256",
                },
            },
        }
        diagnose_device.validate_device(hello, capabilities)
        capabilities["capabilities"]["display"]["width"] = 320
        with self.assertRaisesRegex(ValueError, "display capabilities"):
            diagnose_device.validate_device(hello, capabilities)
        capabilities["capabilities"]["display"]["width"] = 400
        capabilities["capabilities"]["audio"]["capture"] = False
        with self.assertRaisesRegex(ValueError, "audio capabilities"):
            diagnose_device.validate_device(hello, capabilities)
        capabilities["capabilities"]["audio"]["capture"] = True
        capabilities["capabilities"]["network"]["authentication"] = "device-only"
        with self.assertRaisesRegex(ValueError, "network capabilities"):
            diagnose_device.validate_device(hello, capabilities)

    def test_portrait_conversion_is_deterministic(self) -> None:
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow is not installed")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.png"
            image = Image.new("L", (40, 30), 255)
            for y in range(8, 22):
                for x in range(10, 30):
                    image.putpixel((x, y), 32)
            image.save(source)
            first = send_portrait.convert_to_raw1(source, "illustration")
            second = send_portrait.convert_to_raw1(source, "illustration")
        self.assertEqual(first, second)
        self.assertEqual(len(first), 15_000)
        self.assertNotEqual(first, b"\0" * len(first))
        metadata = send_portrait.metadata_payload(first, b"revision-1", b"portrait")
        values = struct.unpack_from("<BHHIIBB", metadata)
        self.assertEqual(values[:4], (1, 400, 300, 15_000))
        self.assertEqual(values[4], zlib.crc32(first) & 0xFFFFFFFF)


if __name__ == "__main__":
    unittest.main()
