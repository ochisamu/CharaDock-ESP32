// SPDX-License-Identifier: Apache-2.0
#include "charadock/utf8.hpp"

namespace charadock::rlcd::utf8 {

DecodeResult decodeOne(const uint8_t *bytes, size_t length) {
  if (!bytes || length == 0)
    return {};
  const uint8_t first = bytes[0];
  if (first <= 0x7f)
    return {first, 1, true};

  size_t count = 0;
  uint32_t codepoint = 0;
  uint32_t minimum = 0;
  if ((first & 0xe0u) == 0xc0u) {
    count = 2;
    codepoint = first & 0x1fu;
    minimum = 0x80;
  } else if ((first & 0xf0u) == 0xe0u) {
    count = 3;
    codepoint = first & 0x0fu;
    minimum = 0x800;
  } else if ((first & 0xf8u) == 0xf0u) {
    count = 4;
    codepoint = first & 0x07u;
    minimum = 0x10000;
  } else {
    return {0xfffdu, 1, false};
  }
  if (length < count)
    return {0xfffdu, 1, false};
  for (size_t index = 1; index < count; ++index) {
    if ((bytes[index] & 0xc0u) != 0x80u)
      return {0xfffdu, 1, false};
    codepoint = (codepoint << 6) | (bytes[index] & 0x3fu);
  }
  if (codepoint < minimum || codepoint > 0x10ffffu ||
      (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
    return {0xfffdu, 1, false};
  }
  return {codepoint, count, true};
}

bool validateDisplayText(const uint8_t *bytes, size_t length,
                         size_t maximumBytes) {
  if ((!bytes && length != 0) || length > maximumBytes)
    return false;
  size_t offset = 0;
  while (offset < length) {
    const DecodeResult decoded = decodeOne(bytes + offset, length - offset);
    if (!decoded.valid)
      return false;
    if ((decoded.codepoint < 0x20u && decoded.codepoint != '\n') ||
        decoded.codepoint == 0x7fu)
      return false;
    offset += decoded.bytes;
  }
  return true;
}

size_t codepointCount(const std::string &text) {
  size_t count = 0;
  size_t offset = 0;
  while (offset < text.size()) {
    const DecodeResult decoded = decodeOne(
        reinterpret_cast<const uint8_t *>(text.data()) + offset,
        text.size() - offset);
    offset += decoded.valid ? decoded.bytes : 1;
    ++count;
  }
  return count;
}

} // namespace charadock::rlcd::utf8
