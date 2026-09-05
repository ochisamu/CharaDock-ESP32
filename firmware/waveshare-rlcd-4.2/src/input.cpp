// SPDX-License-Identifier: Apache-2.0
#include "charadock/input.hpp"

namespace charadock::rlcd {
namespace {
constexpr uint32_t kDebounceMs = 25;
}

DebouncedButton::DebouncedButton(ButtonId id, uint32_t longPressMs)
    : id_(id), longPressMs_(longPressMs) {}

std::vector<ButtonEvent> DebouncedButton::update(bool pressed,
                                                 uint32_t nowMs) {
  std::vector<ButtonEvent> events;
  if (pressed != rawPressed_) {
    rawPressed_ = pressed;
    rawChangedAtMs_ = nowMs;
  }
  if (rawPressed_ != stablePressed_ &&
      static_cast<uint32_t>(nowMs - rawChangedAtMs_) >= kDebounceMs) {
    stablePressed_ = rawPressed_;
    if (stablePressed_) {
      pressedAtMs_ = nowMs;
      longReported_ = false;
      events.push_back({id_, ButtonAction::Pressed, 0});
    } else {
      const uint32_t duration = nowMs - pressedAtMs_;
      events.push_back({id_, longReported_ ? ButtonAction::LongRelease
                                          : ButtonAction::ShortPress,
                        duration});
      longReported_ = false;
    }
  }
  if (stablePressed_ && !longReported_ &&
      static_cast<uint32_t>(nowMs - pressedAtMs_) >= longPressMs_) {
    longReported_ = true;
    events.push_back({id_, ButtonAction::LongPress, nowMs - pressedAtMs_});
  }
  return events;
}

bool DebouncedButton::pressed() const { return stablePressed_; }

uint32_t DebouncedButton::pressedFor(uint32_t nowMs) const {
  return stablePressed_ ? nowMs - pressedAtMs_ : 0;
}

} // namespace charadock::rlcd
