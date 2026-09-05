// SPDX-License-Identifier: Apache-2.0
#include "charadock/shinonome_font.hpp"

namespace {

extern const uint8_t shinonome12Start[]
    asm("_binary_src_generated_shinonome12_bin_start");
extern const uint8_t shinonome12End[]
    asm("_binary_src_generated_shinonome12_bin_end");
extern const uint8_t shinonome16Start[]
    asm("_binary_src_generated_shinonome16_bin_start");
extern const uint8_t shinonome16End[]
    asm("_binary_src_generated_shinonome16_bin_end");

} // namespace

namespace charadock::rlcd {

EmbeddedFontAssets embeddedFontAssets() {
  return {shinonome12Start,
          static_cast<size_t>(shinonome12End - shinonome12Start),
          shinonome16Start,
          static_cast<size_t>(shinonome16End - shinonome16Start)};
}

} // namespace charadock::rlcd
