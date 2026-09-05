// SPDX-License-Identifier: Apache-2.0
#include "charadock/presentation_protocol.hpp"

#include <cstring>
#include <utility>

#include "charadock/utf8.hpp"

namespace charadock::rlcd {
namespace {

constexpr uint8_t kSchemaVersion = 1;
constexpr size_t kScenePrefixBytes = 16;
constexpr size_t kTextPrefixBytes = 10;

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
void writeU16(uint8_t *bytes, uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value & 0xff);
  bytes[1] = static_cast<uint8_t>(value >> 8);
}
void writeU32(uint8_t *bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value & 0xff);
  bytes[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  bytes[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  bytes[3] = static_cast<uint8_t>(value >> 24);
}

size_t maximumFor(TextTarget target) {
  switch (target) {
  case TextTarget::Caption:
    return kMaximumCaptionBytes;
  case TextTarget::Activity:
    return kMaximumActivityBytes;
  case TextTarget::NextAction:
    return kMaximumNextActionBytes;
  case TextTarget::Footer:
    return kMaximumFooterBytes;
  }
  return 0;
}

bool validSmallText(const std::string &value, size_t maximum) {
  return utf8::validateDisplayText(
      reinterpret_cast<const uint8_t *>(value.data()), value.size(), maximum) &&
         value.find('\n') == std::string::npos;
}

} // namespace

bool decodeDisplayScenePayload(const std::vector<uint8_t> &payload,
                               SceneSnapshot &snapshot) {
  if (payload.size() < kScenePrefixBytes || payload[0] != kSchemaVersion ||
      payload[1] > static_cast<uint8_t>(SceneId::Recovery) ||
      payload[2] > static_cast<uint8_t>(DeviceState::Offline) ||
      (payload[3] & ~0x0fu))
    return false;
  const size_t nameLength = payload[14];
  const size_t modeLength = payload[15];
  if (nameLength == 0 || nameLength > kMaximumCharacterNameBytes ||
      modeLength > kMaximumModeLabelBytes ||
      payload.size() != kScenePrefixBytes + nameLength + modeLength)
    return false;
  const uint8_t *name = payload.data() + kScenePrefixBytes;
  const uint8_t *mode = name + nameLength;
  if (!utf8::validateDisplayText(name, nameLength,
                                 kMaximumCharacterNameBytes) ||
      !utf8::validateDisplayText(mode, modeLength, kMaximumModeLabelBytes))
    return false;
  SceneSnapshot decoded;
  decoded.scene = static_cast<SceneId>(payload[1]);
  decoded.state = static_cast<DeviceState>(payload[2]);
  decoded.flags = payload[3];
  decoded.revision = readU32(payload.data() + 4);
  decoded.elapsedSeconds = readU32(payload.data() + 8);
  decoded.artifactCount = readU16(payload.data() + 12);
  decoded.characterName.assign(reinterpret_cast<const char *>(name),
                               nameLength);
  decoded.modeLabel.assign(reinterpret_cast<const char *>(mode), modeLength);
  if (decoded.revision == 0 ||
      !validSmallText(decoded.characterName, kMaximumCharacterNameBytes) ||
      !validSmallText(decoded.modeLabel, kMaximumModeLabelBytes))
    return false;
  snapshot = std::move(decoded);
  return true;
}

std::vector<uint8_t> encodeDisplayScenePayload(const SceneSnapshot &snapshot) {
  if (snapshot.revision == 0 ||
      static_cast<uint8_t>(snapshot.scene) >
          static_cast<uint8_t>(SceneId::Recovery) ||
      static_cast<uint8_t>(snapshot.state) >
          static_cast<uint8_t>(DeviceState::Offline) ||
      (snapshot.flags & ~0x0fu) ||
      !validSmallText(snapshot.characterName, kMaximumCharacterNameBytes) ||
      !validSmallText(snapshot.modeLabel, kMaximumModeLabelBytes))
    return {};
  std::vector<uint8_t> payload(kScenePrefixBytes + snapshot.characterName.size() +
                               snapshot.modeLabel.size(), 0);
  payload[0] = kSchemaVersion;
  payload[1] = static_cast<uint8_t>(snapshot.scene);
  payload[2] = static_cast<uint8_t>(snapshot.state);
  payload[3] = snapshot.flags;
  writeU32(payload.data() + 4, snapshot.revision);
  writeU32(payload.data() + 8, snapshot.elapsedSeconds);
  writeU16(payload.data() + 12, snapshot.artifactCount);
  payload[14] = static_cast<uint8_t>(snapshot.characterName.size());
  payload[15] = static_cast<uint8_t>(snapshot.modeLabel.size());
  std::memcpy(payload.data() + kScenePrefixBytes,
              snapshot.characterName.data(), snapshot.characterName.size());
  std::memcpy(payload.data() + kScenePrefixBytes + snapshot.characterName.size(),
              snapshot.modeLabel.data(), snapshot.modeLabel.size());
  return payload;
}

bool decodeDisplayTextPayload(const std::vector<uint8_t> &payload,
                              TextUpdate &update) {
  if (payload.size() < kTextPrefixBytes || payload[0] != kSchemaVersion ||
      payload[1] > static_cast<uint8_t>(TextTarget::Footer) ||
      (payload[2] != 12 && payload[2] != 16) || payload[3] != 0)
    return false;
  const auto target = static_cast<TextTarget>(payload[1]);
  const size_t length = readU16(payload.data() + 8);
  if (payload.size() != kTextPrefixBytes + length ||
      !utf8::validateDisplayText(payload.data() + kTextPrefixBytes, length,
                                 maximumFor(target)))
    return false;
  update = TextUpdate{};
  update.target = target;
  update.fontSize = payload[2];
  update.revision = readU32(payload.data() + 4);
  update.text.assign(
      reinterpret_cast<const char *>(payload.data() + kTextPrefixBytes), length);
  return update.revision != 0;
}

std::vector<uint8_t> encodeDisplayTextPayload(const TextUpdate &update) {
  const size_t maximum = maximumFor(update.target);
  if (update.revision == 0 || maximum == 0 ||
      (update.fontSize != 12 && update.fontSize != 16) ||
      !utf8::validateDisplayText(
          reinterpret_cast<const uint8_t *>(update.text.data()),
          update.text.size(), maximum))
    return {};
  std::vector<uint8_t> payload(kTextPrefixBytes + update.text.size(), 0);
  payload[0] = kSchemaVersion;
  payload[1] = static_cast<uint8_t>(update.target);
  payload[2] = update.fontSize;
  writeU32(payload.data() + 4, update.revision);
  writeU16(payload.data() + 8, static_cast<uint16_t>(update.text.size()));
  std::memcpy(payload.data() + kTextPrefixBytes, update.text.data(),
              update.text.size());
  return payload;
}

bool decodeDisplayCommitPayload(const std::vector<uint8_t> &payload,
                                uint32_t &revision) {
  if (payload.size() != 5 || payload[0] != kSchemaVersion)
    return false;
  revision = readU32(payload.data() + 1);
  return revision != 0;
}

std::vector<uint8_t> encodeDisplayCommitPayload(uint32_t revision) {
  if (revision == 0)
    return {};
  std::vector<uint8_t> payload(5, 0);
  payload[0] = kSchemaVersion;
  writeU32(payload.data() + 1, revision);
  return payload;
}

} // namespace charadock::rlcd
