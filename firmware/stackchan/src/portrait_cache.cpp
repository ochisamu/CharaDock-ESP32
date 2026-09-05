// SPDX-License-Identifier: Apache-2.0
#include "charadock/portrait_cache.hpp"

#include <cstring>

#include "charadock/protocol_v2.hpp"

namespace charadock {
namespace {

bool hasTerminator(const char *value, size_t capacity) {
  return std::memchr(value, '\0', capacity) != nullptr;
}

size_t boundedLength(const char *value, size_t capacity) {
  size_t length = 0;
  while (length < capacity && value[length] != '\0')
    ++length;
  return length;
}

} // namespace

bool PortraitCache::attach(void *firstSlot, void *secondSlot,
                           size_t slotBytes) {
  abortTransfer();
  activeSlot_ = -1;
  slots_[0] = nullptr;
  slots_[1] = nullptr;
  slotBytes_ = 0;
  if (!firstSlot || !secondSlot || firstSlot == secondSlot ||
      slotBytes < kByteCount ||
      reinterpret_cast<uintptr_t>(firstSlot) % alignof(uint16_t) != 0 ||
      reinterpret_cast<uintptr_t>(secondSlot) % alignof(uint16_t) != 0) {
    return false;
  }
  slots_[0] = static_cast<uint8_t *>(firstSlot);
  slots_[1] = static_cast<uint8_t *>(secondSlot);
  slotBytes_ = slotBytes;
  return true;
}

PortraitCacheResult
PortraitCache::beginTransfer(const PortraitMetadata &metadata) {
  if (!storageReady())
    return PortraitCacheResult::StorageUnavailable;
  if (!metadataIsValid(metadata))
    return PortraitCacheResult::InvalidMetadata;
  stagingSlot_ = activeSlot_ == 0 ? 1 : 0;
  stagingMetadata_ = metadata;
  receivedBytes_ = 0;
  runningChecksum_ = 0xffffffffu;
  transferActive_ = true;
  return PortraitCacheResult::Ok;
}

PortraitCacheResult PortraitCache::writeChunk(uint32_t offset,
                                              const uint8_t *bytes,
                                              size_t length) {
  if (!transferActive_)
    return PortraitCacheResult::TransferNotActive;
  if (offset != receivedBytes_)
    return PortraitCacheResult::UnexpectedOffset;
  if (length > stagingMetadata_.byteCount - receivedBytes_)
    return PortraitCacheResult::TooLarge;
  if (length && !bytes)
    return PortraitCacheResult::InvalidMetadata;
  if (length) {
    std::memcpy(slots_[stagingSlot_] + receivedBytes_, bytes, length);
    runningChecksum_ = protocol::crc32(bytes, length, runningChecksum_);
    receivedBytes_ += length;
  }
  return PortraitCacheResult::Ok;
}

PortraitCacheResult PortraitCache::finishTransfer() {
  if (!transferActive_)
    return PortraitCacheResult::TransferNotActive;
  if (receivedBytes_ != stagingMetadata_.byteCount)
    return PortraitCacheResult::Incomplete;
  const uint32_t actualChecksum = runningChecksum_ ^ 0xffffffffu;
  if (actualChecksum != stagingMetadata_.checksum) {
    abortTransfer();
    return PortraitCacheResult::ChecksumMismatch;
  }
  activeSlot_ = static_cast<int8_t>(stagingSlot_);
  activeMetadata_ = stagingMetadata_;
  transferActive_ = false;
  receivedBytes_ = 0;
  return PortraitCacheResult::Ok;
}

void PortraitCache::abortTransfer() {
  transferActive_ = false;
  receivedBytes_ = 0;
  runningChecksum_ = 0xffffffffu;
  stagingMetadata_ = PortraitMetadata{};
}

void PortraitCache::invalidate(const uint8_t *revision, size_t revisionLength) {
  if (transferActive_) {
    if (!revision || revisionLength == 0 ||
        (revisionLength <= PortraitMetadata::kMaximumRevisionBytes &&
         boundedLength(stagingMetadata_.revision.data(),
                       stagingMetadata_.revision.size()) == revisionLength &&
         std::memcmp(stagingMetadata_.revision.data(), revision,
                     revisionLength) == 0)) {
      abortTransfer();
    }
  }
  if (activeSlot_ >= 0 && (!revision || revisionLength == 0 ||
                           matchesRevision(revision, revisionLength))) {
    activeSlot_ = -1;
    activeMetadata_ = PortraitMetadata{};
  }
}

bool PortraitCache::storageReady() const {
  return slots_[0] && slots_[1] && slotBytes_ >= kByteCount;
}

bool PortraitCache::available() const { return activeSlot_ >= 0; }

bool PortraitCache::transferActive() const { return transferActive_; }

bool PortraitCache::matchesRevision(const uint8_t *revision,
                                    size_t length) const {
  if (activeSlot_ < 0 || !revision || length == 0 ||
      length > PortraitMetadata::kMaximumRevisionBytes) {
    return false;
  }
  const size_t activeLength = boundedLength(activeMetadata_.revision.data(),
                                            activeMetadata_.revision.size());
  return activeLength == length &&
         std::memcmp(activeMetadata_.revision.data(), revision, length) == 0;
}

size_t PortraitCache::receivedBytes() const { return receivedBytes_; }

const uint16_t *PortraitCache::pixels() const {
  return activeSlot_ >= 0
             ? reinterpret_cast<const uint16_t *>(slots_[activeSlot_])
             : nullptr;
}

const PortraitMetadata *PortraitCache::metadata() const {
  return activeSlot_ >= 0 ? &activeMetadata_ : nullptr;
}

bool PortraitCache::metadataIsValid(const PortraitMetadata &metadata) const {
  return metadata.format == PortraitFormat::Rgb565LittleEndian &&
         metadata.width == kWidth && metadata.height == kHeight &&
         metadata.byteCount == kByteCount && metadata.revision[0] != '\0' &&
         hasTerminator(metadata.revision.data(), metadata.revision.size()) &&
         hasTerminator(metadata.frameName.data(), metadata.frameName.size());
}

const char *portraitCacheResultName(PortraitCacheResult result) {
  switch (result) {
  case PortraitCacheResult::Ok:
    return "ok";
  case PortraitCacheResult::StorageUnavailable:
    return "storage-unavailable";
  case PortraitCacheResult::InvalidMetadata:
    return "invalid-metadata";
  case PortraitCacheResult::TransferNotActive:
    return "transfer-not-active";
  case PortraitCacheResult::UnexpectedOffset:
    return "unexpected-offset";
  case PortraitCacheResult::TooLarge:
    return "too-large";
  case PortraitCacheResult::Incomplete:
    return "incomplete";
  case PortraitCacheResult::ChecksumMismatch:
    return "checksum-mismatch";
  }
  return "unknown";
}

} // namespace charadock
