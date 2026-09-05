// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace charadock::rlcd {

enum class MonochromeFormat : uint8_t {
  Raw1Msb = 1,
};

struct MonochromeAssetMetadata {
  static constexpr size_t kMaximumRevisionBytes = 64;
  static constexpr size_t kMaximumFrameNameBytes = 32;

  MonochromeFormat format = MonochromeFormat::Raw1Msb;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t byteCount = 0;
  uint32_t checksum = 0;
  std::array<char, kMaximumRevisionBytes + 1> revision{};
  std::array<char, kMaximumFrameNameBytes + 1> frameName{};
};

enum class MonochromeAssetResult : uint8_t {
  Ok = 0,
  StorageUnavailable = 1,
  InvalidMetadata = 2,
  TransferNotActive = 3,
  UnexpectedOffset = 4,
  TooLarge = 5,
  Incomplete = 6,
  ChecksumMismatch = 7,
};

class MonochromeAssetStore {
public:
  static constexpr uint16_t kMaximumWidth = 400;
  static constexpr uint16_t kMaximumHeight = 300;
  static constexpr size_t kMaximumBytes = 15000;

  bool attach(uint8_t *firstSlot, uint8_t *secondSlot, size_t slotBytes);
  MonochromeAssetResult beginTransfer(
      const MonochromeAssetMetadata &metadata);
  MonochromeAssetResult writeChunk(uint32_t offset, const uint8_t *bytes,
                                   size_t length);
  MonochromeAssetResult finishTransfer();
  void cancelTransfer();
  void invalidate(const uint8_t *revision = nullptr, size_t length = 0);

  bool available() const;
  bool transferActive() const;
  bool matchesRevision(const uint8_t *revision, size_t length) const;
  const uint8_t *pixels() const;
  const MonochromeAssetMetadata *metadata() const;
  size_t receivedBytes() const;

private:
  uint8_t *slots_[2] = {nullptr, nullptr};
  size_t slotBytes_ = 0;
  uint8_t activeSlot_ = 0;
  uint8_t stagingSlot_ = 1;
  bool available_ = false;
  bool transferActive_ = false;
  size_t receivedBytes_ = 0;
  MonochromeAssetMetadata activeMetadata_{};
  MonochromeAssetMetadata stagedMetadata_{};
};

bool decodeMonochromeAssetMetadata(const std::vector<uint8_t> &payload,
                                   MonochromeAssetMetadata &metadata);
std::vector<uint8_t> encodeMonochromeAssetMetadata(
    const MonochromeAssetMetadata &metadata);
const char *monochromeAssetResultName(MonochromeAssetResult result);

} // namespace charadock::rlcd
