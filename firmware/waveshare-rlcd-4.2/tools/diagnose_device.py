#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run a non-audio USB preflight for CharaDock Waveshare RLCD 4.2."""

from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from datetime import datetime
from typing import Any

from protocol_serial import (
    FrameReader,
    exchange_frame,
    request_frame,
    require_success,
)

FRAME_DEVICE_HELLO = 0x01
FRAME_HOST_HELLO = 0x02
FRAME_CAPABILITIES = 0x05
FRAME_TIME_SYNC = 0x34
FRAME_SENSOR_REPORT = 0x57
EXPECTED_BOARD = "waveshare-esp32-s3-rlcd-4.2"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Serial port, for example COM7; auto-detect when omitted")
    parser.add_argument("--baud", type=int, default=500_000)
    parser.add_argument("--startup-wait", type=float, default=1.2)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--json", action="store_true", help="Print machine-readable JSON")
    return parser.parse_args()


def time_sync_payload() -> bytes:
    now = datetime.now().astimezone()
    offset = now.utcoffset()
    offset_minutes = int(offset.total_seconds() // 60) if offset else 0
    return struct.pack("<BQh", 1, int(time.time()), offset_minutes)


def parse_sensor_report(payload: bytes) -> dict[str, Any]:
    if len(payload) != 18 or payload[0] != 1:
        raise ValueError("sensor report is malformed")
    flags = payload[1]
    year = struct.unpack_from("<H", payload, 9)[0]
    return {
        "available": {
            "temperatureHumidity": bool(flags & 0x01),
            "rtc": bool(flags & 0x02),
            "battery": bool(flags & 0x04),
            "speakerCodec": bool(flags & 0x08),
            "microphoneCodec": bool(flags & 0x10),
        },
        "temperatureC": struct.unpack_from("<h", payload, 2)[0] / 100,
        "humidityPercent": struct.unpack_from("<H", payload, 4)[0] / 100,
        "batteryVolts": struct.unpack_from("<H", payload, 6)[0] / 1000,
        "batteryPercent": payload[8],
        "rtc": {
            "year": year,
            "month": payload[11],
            "day": payload[12],
            "hour": payload[13],
            "minute": payload[14],
            "second": payload[15],
        },
    }


def decode_json(payload: bytes, field: str) -> dict[str, Any]:
    try:
        value = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"{field} is not valid UTF-8 JSON") from error
    if not isinstance(value, dict):
        raise ValueError(f"{field} must be a JSON object")
    return value


def validate_device(hello: dict[str, Any], capabilities: dict[str, Any]) -> None:
    if hello.get("board") != EXPECTED_BOARD:
        raise ValueError(f"unexpected board: {hello.get('board', 'unknown')}")
    if capabilities.get("board") != EXPECTED_BOARD or capabilities.get("protocol") != 2:
        raise ValueError("Device Protocol v2 RLCD capabilities were not returned")
    feature_set = capabilities.get("capabilities")
    if not isinstance(feature_set, dict):
        raise ValueError("capability feature set is missing")
    display = feature_set.get("display")
    if not isinstance(display, dict):
        raise ValueError("display capabilities are missing")
    bitmap = display.get("bitmap")
    if (
        display.get("width") != 400
        or display.get("height") != 300
        or display.get("bitsPerPixel") != 1
        or not isinstance(bitmap, list)
        or "raw1-msb" not in bitmap
    ):
        raise ValueError("display capabilities do not match the 400x300 raw1-msb profile")
    audio = feature_set.get("audio")
    if (
        not isinstance(audio, dict)
        or audio.get("capture") is not True
        or audio.get("playback") is not True
        or audio.get("format") != "pcm-s16le-mono"
        or audio.get("duplex") != "half"
        or 16000 not in audio.get("sampleRates", [])
    ):
        raise ValueError("audio capabilities do not match the half-duplex 16 kHz capture/playback profile")
    network = feature_set.get("network")
    if (
        not isinstance(network, dict)
        or network.get("wifi") is not True
        or network.get("provisioning") != "usb-only"
        or network.get("authentication") != "mutual-hmac-sha256"
    ):
        raise ValueError("network capabilities do not match the USB-provisioned mutual-HMAC Wi-Fi profile")


def auto_detect_port() -> str:
    try:
        from serial.tools import list_ports
    except ImportError as error:
        raise RuntimeError("pyserial is required: py -m pip install pyserial") from error
    candidates = []
    for port in list_ports.comports():
        description = f"{port.description or ''} {port.manufacturer or ''}".lower()
        if port.vid in (0x303A, 0x1A86, 0x10C4) or any(
            token in description for token in ("waveshare", "esp32-s3", "usb jtag/serial")
        ):
            candidates.append(str(port.device))
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise RuntimeError("RLCD 4.2 serial port was not found; connect USB or pass --port")
    raise RuntimeError("multiple ESP32 serial ports were found; pass --port: " + ", ".join(candidates))


def run_preflight(args: argparse.Namespace) -> dict[str, Any]:
    try:
        import serial
    except ImportError as error:
        raise RuntimeError("pyserial is required: py -m pip install pyserial") from error
    port_name = args.port or auto_detect_port()
    with serial.Serial(
        port_name,
        args.baud,
        timeout=0.1,
        write_timeout=max(1.0, args.timeout),
    ) as port:
        time.sleep(max(0.0, args.startup_wait))
        port.reset_input_buffer()
        reader = FrameReader(port)
        sequence = 1
        _, _, hello_payload = request_frame(
            port,
            FRAME_DEVICE_HELLO,
            sequence,
            timeout_seconds=args.timeout,
            reader=reader,
        )
        sequence += 1
        _, _, capabilities_payload = request_frame(
            port,
            FRAME_CAPABILITIES,
            sequence,
            timeout_seconds=args.timeout,
            reader=reader,
        )
        hello = decode_json(hello_payload, "DeviceHello")
        capabilities = decode_json(capabilities_payload, "Capabilities")
        validate_device(hello, capabilities)
        sequence += 1
        require_success(
            exchange_frame(
                port,
                FRAME_TIME_SYNC,
                sequence,
                time_sync_payload(),
                timeout_seconds=args.timeout,
                reader=reader,
            )
        )
        sequence += 1
        _, _, sensor_payload = request_frame(
            port,
            FRAME_SENSOR_REPORT,
            sequence,
            timeout_seconds=args.timeout,
            reader=reader,
        )
        sequence += 1
        require_success(
            exchange_frame(
                port,
                FRAME_HOST_HELLO,
                sequence,
                timeout_seconds=args.timeout,
                reader=reader,
            )
        )
    return {
        "ok": True,
        "port": port_name,
        "baud": args.baud,
        "device": hello,
        "capabilities": capabilities.get("capabilities", {}),
        "sensors": parse_sensor_report(sensor_payload),
        "checks": {
            "deviceHello": True,
            "capabilities": True,
            "timeSync": True,
            "sensorQuery": True,
            "heartbeat": True,
        },
    }


def human_report(result: dict[str, Any]) -> str:
    device = result["device"]
    display = result["capabilities"].get("display", {})
    audio = result["capabilities"].get("audio", {})
    network = result["capabilities"].get("network", {})
    sensors = result["sensors"]
    available = sensors["available"]
    sensor_text = "--"
    if available["temperatureHumidity"]:
        sensor_text = f"{sensors['temperatureC']:.2f} C / {sensors['humidityPercent']:.2f}%"
    battery_text = "--"
    if available["battery"]:
        battery_text = f"{sensors['batteryVolts']:.3f} V / {sensors['batteryPercent']}%"
    return "\n".join(
        (
            "RLCD 4.2 USB preflight: OK",
            f"  Port:       {result['port']} @ {result['baud']}",
            f"  Device:     {device.get('deviceId', '--')}",
            f"  Firmware:   {device.get('firmware', '--')}",
            f"  Display:    {display.get('width')}x{display.get('height')} {display.get('bitsPerPixel')}bit raw1-msb",
            f"  Temp/RH:    {sensor_text}",
            f"  Battery:    {battery_text}",
            f"  RTC:        {'OK' if available['rtc'] else '--'}",
            f"  Codecs:     ES8311={'OK' if available['speakerCodec'] else '--'} / ES7210={'OK' if available['microphoneCodec'] else '--'}",
            f"  Audio gate: capture={audio.get('capture', False)} / playback={audio.get('playback', False)}",
            f"  Network:    Wi-Fi={network.get('wifi', False)} / {network.get('authentication', '--')}",
            "  Protocol:   hello / capabilities / time / sensors / heartbeat OK",
        )
    )


def main() -> int:
    args = parse_args()
    try:
        result = run_preflight(args)
    except (OSError, RuntimeError, TimeoutError, ValueError) as error:
        if args.json:
            print(json.dumps({"ok": False, "error": str(error)}, ensure_ascii=False))
        else:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print(
        json.dumps(result, ensure_ascii=False, indent=2)
        if args.json
        else human_report(result)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
