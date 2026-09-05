// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace charadock::rlcd::utf8 {

struct DecodeResult {
  uint32_t codepoint = 0xfffdu;
  size_t bytes = 0;
  bool valid = false;
};

DecodeResult decodeOne(const uint8_t *bytes, size_t length);
bool validateDisplayText(const uint8_t *bytes, size_t length,
                         size_t maximumBytes);
size_t codepointCount(const std::string &text);

} // namespace charadock::rlcd::utf8
