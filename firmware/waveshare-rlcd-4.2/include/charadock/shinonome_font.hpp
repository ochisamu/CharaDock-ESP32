// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace charadock::rlcd {

struct GlyphView {
  const uint8_t *bitmap = nullptr;
  uint8_t width = 0;
  uint8_t height = 0;
  uint8_t bytesPerRow = 0;

  explicit operator bool() const { return bitmap != nullptr; }
};

class ShinonomeFont {
public:
  bool attach(const uint8_t *bytes, size_t length);
  bool valid() const;
  uint8_t pixelSize() const;
  uint8_t halfWidth() const;
  uint8_t fullWidth() const;
  uint32_t glyphCount() const;
  GlyphView glyph(uint32_t codepoint) const;
  bool contains(uint32_t codepoint) const;

private:
  const uint8_t *bytes_ = nullptr;
  size_t length_ = 0;
  uint8_t pixelSize_ = 0;
  uint8_t halfWidth_ = 0;
  uint8_t fullWidth_ = 0;
  uint32_t glyphCount_ = 0;
  uint32_t entriesOffset_ = 0;
  uint32_t bitmapsOffset_ = 0;
};

struct EmbeddedFontAssets {
  const uint8_t *font12 = nullptr;
  size_t font12Bytes = 0;
  const uint8_t *font16 = nullptr;
  size_t font16Bytes = 0;
};

EmbeddedFontAssets embeddedFontAssets();

} // namespace charadock::rlcd
