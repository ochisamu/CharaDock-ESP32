// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <vector>

namespace charadock::rlcd {

enum class ButtonId : uint8_t {
  Key = 0,
  Boot = 1,
};

enum class ButtonAction : uint8_t {
  Pressed = 0,
  ShortPress = 1,
  LongPress = 2,
  LongRelease = 3,
};

struct ButtonEvent {
  ButtonId button = ButtonId::Key;
  ButtonAction action = ButtonAction::Pressed;
  uint32_t durationMs = 0;
};

class DebouncedButton {
public:
  DebouncedButton(ButtonId id, uint32_t longPressMs);
  std::vector<ButtonEvent> update(bool pressed, uint32_t nowMs);
  bool pressed() const;
  uint32_t pressedFor(uint32_t nowMs) const;

private:
  ButtonId id_;
  uint32_t longPressMs_;
  bool rawPressed_ = false;
  bool stablePressed_ = false;
  bool longReported_ = false;
  uint32_t rawChangedAtMs_ = 0;
  uint32_t pressedAtMs_ = 0;
};

} // namespace charadock::rlcd
