#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build deterministic Unicode-indexed Shinonome 12/16 bitmap assets."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"CDFN"
FORMAT_VERSION = 1
HEADER = struct.Struct("<4sBBBBIIII32s")
ENTRY = struct.Struct("<IIBBH")

EXPECTED_SHA256 = {
    "shnm6x12r.bdf": "bd54dc33f9caa588183c161a23f374def17be24d28417040cd277c264e4a07e4",
    "shnmk12.bdf": "17b0fa218105027f1d6615af3e5e46c2d0fbb2a6ac2fa2f7e5d39dff92d2116f",
    "shnm8x16r.bdf": "26eae5a2a057c5756b26f6343f235c0967996e6c1316eff38b94ccc31396d078",
    "shnmk16.bdf": "9965accd5bdbe03bf9395b8dd26dfa4465f4ffa6e9f447c5cb8258644fa1f468",
}


@dataclass(frozen=True)
class Glyph:
    codepoint: int
    width: int
    rows: bytes


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Convert Shinonome JIS X 0201/0208 BDF into CDFN assets."
    )
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=repo_root / "third_party" / "shinonome" / "original-bdf",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=(
            repo_root
            / "firmware"
            / "waveshare-rlcd-4.2"
            / "src"
            / "generated"
        ),
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Fail if checked-in generated files differ from a fresh conversion.",
    )
    parser.add_argument(
        "--verify-source",
        action="store_true",
        help="Verify the four imported BDF SHA-256 values and exit.",
    )
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_sources(source_dir: Path) -> dict[str, str]:
    actual: dict[str, str] = {}
    for name, expected in EXPECTED_SHA256.items():
        path = source_dir / name
        if not path.is_file():
            raise FileNotFoundError(f"missing Shinonome source: {path}")
        actual[name] = sha256(path)
        if actual[name] != expected:
            raise ValueError(
                f"source checksum mismatch for {name}: "
                f"expected {expected}, got {actual[name]}"
            )
    return actual


def jis0201_to_unicode(encoding: int) -> tuple[int, ...]:
    if 0x20 <= encoding <= 0x7E:
        aliases: tuple[int, ...] = ()
        if encoding == 0x5C:
            aliases = (0x00A5,)
        elif encoding == 0x7E:
            aliases = (0x203E,)
        return (encoding, *aliases)
    if 0xA1 <= encoding <= 0xDF:
        try:
            return (ord(bytes([encoding]).decode("shift_jis")),)
        except UnicodeDecodeError:
            return ()
    return ()


def jis0208_to_unicode(encoding: int) -> tuple[int, ...]:
    row = (encoding >> 8) & 0xFF
    cell = encoding & 0xFF
    if not (0x21 <= row <= 0x7E and 0x21 <= cell <= 0x7E):
        return ()
    try:
        decoded = bytes([row + 0x80, cell + 0x80]).decode("euc_jp")
    except UnicodeDecodeError:
        return ()
    return (ord(decoded),) if len(decoded) == 1 else ()


def parse_bdf(path: Path, *, pixel_size: int, charset: str) -> list[Glyph]:
    glyphs: list[Glyph] = []
    encoding: int | None = None
    width = 0
    height = 0
    bitmap: list[str] = []
    in_bitmap = False

    def finish() -> None:
        nonlocal encoding, width, height, bitmap, in_bitmap
        if encoding is None or width <= 0 or height <= 0 or not bitmap:
            return
        mappings = (
            jis0201_to_unicode(encoding)
            if charset == "jis0201"
            else jis0208_to_unicode(encoding)
        )
        if not mappings:
            return
        bytes_per_row = (width + 7) // 8
        rows = bytearray(pixel_size * bytes_per_row)
        source_rows = bitmap[:height]
        top = max(0, pixel_size - height)
        for row_index, line in enumerate(source_rows):
            raw = bytes.fromhex(line)
            if len(raw) < bytes_per_row:
                raw += b"\0" * (bytes_per_row - len(raw))
            destination = (top + row_index) * bytes_per_row
            if destination + bytes_per_row <= len(rows):
                rows[destination : destination + bytes_per_row] = raw[
                    :bytes_per_row
                ]
        for codepoint in mappings:
            glyphs.append(Glyph(codepoint, width, bytes(rows)))

    with path.open("r", encoding="ascii", errors="strict") as source:
        for raw_line in source:
            line = raw_line.strip()
            if line.startswith("STARTCHAR "):
                encoding = None
                width = 0
                height = 0
                bitmap = []
                in_bitmap = False
            elif line.startswith("ENCODING "):
                encoding = int(line.split()[1])
            elif line.startswith("BBX "):
                fields = line.split()
                width = int(fields[1])
                height = int(fields[2])
            elif line == "BITMAP":
                in_bitmap = True
            elif line == "ENDCHAR":
                finish()
                in_bitmap = False
            elif in_bitmap:
                bitmap.append(line)
    return glyphs


def replacement_glyph(pixel_size: int, width: int) -> Glyph:
    bytes_per_row = (width + 7) // 8
    rows = bytearray(pixel_size * bytes_per_row)
    left = 1
    right = max(left, width - 2)
    top = 1
    bottom = max(top, pixel_size - 2)
    for y in range(pixel_size):
        for x in range(width):
            if (y in (top, bottom) and left <= x <= right) or (
                x in (left, right) and top <= y <= bottom
            ):
                rows[y * bytes_per_row + x // 8] |= 0x80 >> (x % 8)
    return Glyph(0x25A1, width, bytes(rows))


def build_font(
    source_dir: Path, pixel_size: int, half_name: str, full_name: str
) -> tuple[bytes, int, int]:
    half_width = 6 if pixel_size == 12 else 8
    full_width = pixel_size
    source_paths = [source_dir / half_name, source_dir / full_name]
    source_digest = hashlib.sha256()
    glyph_by_codepoint: dict[int, Glyph] = {}
    for path, charset in zip(source_paths, ("jis0201", "jis0208"), strict=True):
        raw = path.read_bytes()
        source_digest.update(path.name.encode("ascii") + b"\0" + raw)
        for glyph in parse_bdf(path, pixel_size=pixel_size, charset=charset):
            glyph_by_codepoint.setdefault(glyph.codepoint, glyph)
    glyph_by_codepoint.setdefault(
        0x25A1, replacement_glyph(pixel_size, full_width)
    )
    ordered = sorted(glyph_by_codepoint.values(), key=lambda item: item.codepoint)
    entries_offset = HEADER.size
    bitmaps_offset = entries_offset + len(ordered) * ENTRY.size
    entries = bytearray()
    bitmaps = bytearray()
    for glyph in ordered:
        bytes_per_row = (glyph.width + 7) // 8
        expected = pixel_size * bytes_per_row
        if len(glyph.rows) != expected:
            raise ValueError(
                f"invalid glyph byte count U+{glyph.codepoint:04X}: "
                f"expected {expected}, got {len(glyph.rows)}"
            )
        entries.extend(
            ENTRY.pack(
                glyph.codepoint,
                len(bitmaps),
                glyph.width,
                bytes_per_row,
                0,
            )
        )
        bitmaps.extend(glyph.rows)
    total_size = bitmaps_offset + len(bitmaps)
    header = HEADER.pack(
        MAGIC,
        FORMAT_VERSION,
        pixel_size,
        half_width,
        full_width,
        len(ordered),
        entries_offset,
        bitmaps_offset,
        total_size,
        source_digest.digest(),
    )
    return bytes(header + entries + bitmaps), len(ordered), len(bitmaps)


def generated_files(source_dir: Path) -> tuple[dict[str, bytes], dict[str, object]]:
    source_hashes = verify_sources(source_dir)
    font12, glyphs12, bitmap12 = build_font(
        source_dir, 12, "shnm6x12r.bdf", "shnmk12.bdf"
    )
    font16, glyphs16, bitmap16 = build_font(
        source_dir, 16, "shnm8x16r.bdf", "shnmk16.bdf"
    )
    manifest: dict[str, object] = {
        "format": "CDFN",
        "formatVersion": FORMAT_VERSION,
        "upstream": "Shinonome 0.9.11",
        "sources": source_hashes,
        "fonts": {
            "shinonome12.bin": {
                "pixelSize": 12,
                "glyphs": glyphs12,
                "bitmapBytes": bitmap12,
                "sha256": hashlib.sha256(font12).hexdigest(),
            },
            "shinonome16.bin": {
                "pixelSize": 16,
                "glyphs": glyphs16,
                "bitmapBytes": bitmap16,
                "sha256": hashlib.sha256(font16).hexdigest(),
            },
        },
    }
    files = {
        "shinonome12.bin": font12,
        "shinonome16.bin": font16,
        "shinonome_manifest.json": (
            json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True)
            + "\n"
        ).encode("utf-8"),
    }
    return files, manifest


def write_or_check(
    files: dict[str, bytes], output_dir: Path, *, check: bool
) -> None:
    if check:
        stale = [
            name
            for name, expected in files.items()
            if not (output_dir / name).is_file()
            or (output_dir / name).read_bytes() != expected
        ]
        if stale:
            raise ValueError("generated font assets are stale: " + ", ".join(stale))
        return
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, contents in files.items():
        destination = output_dir / name
        with tempfile.NamedTemporaryFile(
            dir=output_dir, prefix=f".{name}.", delete=False
        ) as temporary:
            temporary.write(contents)
            temporary_path = Path(temporary.name)
        temporary_path.replace(destination)


def main() -> int:
    args = parse_args()
    try:
        hashes = verify_sources(args.source_dir)
        if args.verify_source:
            for name, digest in hashes.items():
                print(f"{digest}  {name}")
            return 0
        files, manifest = generated_files(args.source_dir)
        write_or_check(files, args.output_dir, check=args.check)
        action = "verified" if args.check else "generated"
        fonts = manifest["fonts"]
        assert isinstance(fonts, dict)
        print(
            f"{action} Shinonome assets: "
            f"12px={fonts['shinonome12.bin']['glyphs']} glyphs, "
            f"16px={fonts['shinonome16.bin']['glyphs']} glyphs"
        )
        return 0
    except (OSError, UnicodeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
