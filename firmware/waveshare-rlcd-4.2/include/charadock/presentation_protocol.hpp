// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <vector>

#include "charadock/scene_model.hpp"

namespace charadock::rlcd {

constexpr size_t kMaximumCharacterNameBytes = 48;
constexpr size_t kMaximumModeLabelBytes = 24;
constexpr size_t kMaximumCaptionBytes = 1024;
constexpr size_t kMaximumActivityBytes = 384;
constexpr size_t kMaximumNextActionBytes = 256;
constexpr size_t kMaximumFooterBytes = 160;

struct TextUpdate {
  uint32_t revision = 0;
  TextTarget target = TextTarget::Caption;
  uint8_t fontSize = 16;
  std::string text;
};

bool decodeDisplayScenePayload(const std::vector<uint8_t> &payload,
                               SceneSnapshot &snapshot);
std::vector<uint8_t> encodeDisplayScenePayload(const SceneSnapshot &snapshot);
bool decodeDisplayTextPayload(const std::vector<uint8_t> &payload,
                              TextUpdate &update);
std::vector<uint8_t> encodeDisplayTextPayload(const TextUpdate &update);
bool decodeDisplayCommitPayload(const std::vector<uint8_t> &payload,
                                uint32_t &revision);
std::vector<uint8_t> encodeDisplayCommitPayload(uint32_t revision);

} // namespace charadock::rlcd
