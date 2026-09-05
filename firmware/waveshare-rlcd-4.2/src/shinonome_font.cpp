// SPDX-License-Identifier: Apache-2.0
#include "charadock/shinonome_font.hpp"

#include <cstring>

namespace charadock::rlcd {
namespace {

constexpr size_t kHeaderBytes = 56;
constexpr size_t kEntryBytes = 12;
constexpr uint8_t kFormatVersion = 1;

uint16_t readU16(const uint8_t *bytes) {
  return static_cast<uint16_t>(bytes[0]) |
         static_cast<uint16_t>(bytes[1]) << 8;
}
uint32_t readU32(const uint8_t *bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         static_cast<uint32_t>(bytes[1]) << 8 |
         static_cast<uint32_t>(bytes[2]) << 16 |
         static_cast<uint32_t>(bytes[3]) << 24;
}

} // namespace

bool ShinonomeFont::attach(const uint8_t *bytes, size_t length) {
  bytes_ = nullptr;
  length_ = 0;
  pixelSize_ = halfWidth_ = fullWidth_ = 0;
  glyphCount_ = entriesOffset_ = bitmapsOffset_ = 0;
  if (!bytes || length < kHeaderBytes || std::memcmp(bytes, "CDFN", 4) != 0 ||
      bytes[4] != kFormatVersion || (bytes[5] != 12 && bytes[5] != 16) ||
      bytes[6] == 0 || bytes[7] == 0)
    return false;
  const uint32_t glyphCount = readU32(bytes + 8);
  const uint32_t entriesOffset = readU32(bytes + 12);
  const uint32_t bitmapsOffset = readU32(bytes + 16);
  const uint32_t totalSize = readU32(bytes + 20);
  const uint64_t entriesEnd = static_cast<uint64_t>(entriesOffset) +
                              static_cast<uint64_t>(glyphCount) * kEntryBytes;
  if (glyphCount == 0 || entriesOffset < kHeaderBytes ||
      entriesEnd != bitmapsOffset || bitmapsOffset > totalSize ||
      totalSize != length)
    return false;

  uint32_t previousCodepoint = 0;
  for (uint32_t index = 0; index < glyphCount; ++index) {
    const uint8_t *entry = bytes + entriesOffset + index * kEntryBytes;
    const uint32_t codepoint = readU32(entry);
    const uint32_t bitmapOffset = readU32(entry + 4);
    const uint8_t width = entry[8];
    const uint8_t bytesPerRow = entry[9];
    if ((index && codepoint <= previousCodepoint) || width == 0 ||
        bytesPerRow != (width + 7u) / 8u || readU16(entry + 10) != 0 ||
        static_cast<uint64_t>(bitmapsOffset) + bitmapOffset +
                static_cast<uint64_t>(bytesPerRow) * bytes[5] >
            totalSize)
      return false;
    previousCodepoint = codepoint;
  }

  bytes_ = bytes;
  length_ = length;
  pixelSize_ = bytes[5];
  halfWidth_ = bytes[6];
  fullWidth_ = bytes[7];
  glyphCount_ = glyphCount;
  entriesOffset_ = entriesOffset;
  bitmapsOffset_ = bitmapsOffset;
  return true;
}

bool ShinonomeFont::valid() const { return bytes_ != nullptr; }
uint8_t ShinonomeFont::pixelSize() const { return pixelSize_; }
uint8_t ShinonomeFont::halfWidth() const { return halfWidth_; }
uint8_t ShinonomeFont::fullWidth() const { return fullWidth_; }
uint32_t ShinonomeFont::glyphCount() const { return glyphCount_; }

GlyphView ShinonomeFont::glyph(uint32_t codepoint) const {
  if (!valid())
    return {};
  uint32_t lower = 0;
  uint32_t upper = glyphCount_;
  while (lower < upper) {
    const uint32_t middle = lower + (upper - lower) / 2;
    const uint8_t *entry = bytes_ + entriesOffset_ + middle * kEntryBytes;
    const uint32_t candidate = readU32(entry);
    if (candidate < codepoint)
      lower = middle + 1;
    else
      upper = middle;
  }
  if (lower >= glyphCount_)
    return {};
  const uint8_t *entry = bytes_ + entriesOffset_ + lower * kEntryBytes;
  if (readU32(entry) != codepoint)
    return {};
  const uint32_t bitmapOffset = readU32(entry + 4);
  return {bytes_ + bitmapsOffset_ + bitmapOffset, entry[8], pixelSize_,
          entry[9]};
}

bool ShinonomeFont::contains(uint32_t codepoint) const {
  return static_cast<bool>(glyph(codepoint));
}

} // namespace charadock::rlcd
