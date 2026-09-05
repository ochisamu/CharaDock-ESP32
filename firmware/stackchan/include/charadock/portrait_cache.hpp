// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace charadock {

enum class PortraitFormat : uint8_t {
  Rgb565LittleEndian = 0,
};

struct PortraitMetadata {
  static constexpr size_t kMaximumRevisionBytes = 64;
  static constexpr size_t kMaximumFrameNameBytes = 32;

  PortraitFormat format = PortraitFormat::Rgb565LittleEndian;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t byteCount = 0;
  uint32_t checksum = 0;
  std::array<char, kMaximumRevisionBytes + 1> revision{};
  std::array<char, kMaximumFrameNameBytes + 1> frameName{};
};

enum class PortraitCacheResult : uint8_t {
  Ok = 0,
  StorageUnavailable = 1,
  InvalidMetadata = 2,
  TransferNotActive = 3,
  UnexpectedOffset = 4,
  TooLarge = 5,
  Incomplete = 6,
  ChecksumMismatch = 7,
};

class PortraitCache {
public:
  static constexpr uint16_t kWidth = 320;
  static constexpr uint16_t kHeight = 240;
  static constexpr size_t kByteCount =
      static_cast<size_t>(kWidth) * kHeight * sizeof(uint16_t);

  // Both slots must be distinct, 16-bit aligned, and at least kByteCount.
  // The inactive slot is used for staging so a broken transfer never replaces
  // the last verified portrait.
  bool attach(void *firstSlot, void *secondSlot, size_t slotBytes);

  PortraitCacheResult beginTransfer(const PortraitMetadata &metadata);
  PortraitCacheResult writeChunk(uint32_t offset, const uint8_t *bytes,
                                 size_t length);
  PortraitCacheResult finishTransfer();
  void abortTransfer();
  void invalidate(const uint8_t *revision = nullptr, size_t revisionLength = 0);

  bool storageReady() const;
  bool available() const;
  bool transferActive() const;
  bool matchesRevision(const uint8_t *revision, size_t length) const;
  size_t receivedBytes() const;
  const uint16_t *pixels() const;
  const PortraitMetadata *metadata() const;

private:
  bool metadataIsValid(const PortraitMetadata &metadata) const;

  uint8_t *slots_[2] = {nullptr, nullptr};
  int8_t activeSlot_ = -1;
  uint8_t stagingSlot_ = 0;
  size_t slotBytes_ = 0;
  size_t receivedBytes_ = 0;
  uint32_t runningChecksum_ = 0xffffffffu;
  bool transferActive_ = false;
  PortraitMetadata activeMetadata_{};
  PortraitMetadata stagingMetadata_{};
};

const char *portraitCacheResultName(PortraitCacheResult result);

} // namespace charadock
