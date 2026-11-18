// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdlib>

namespace charadock {

enum class QuickAction { None, Open, Close, PreviewBack, Quieter, Louder, Tracking, Preview };

class QuickSettingsTouch {
public:
  QuickAction update(bool pressed, int x, int y, bool settingsOpen, bool preview, bool asleep) {
    if (pressed) {
      if (!pressed_) {
        startX_ = x;
        startY_ = y;
        ignore_ = asleep || (x >= 276 && y <= 43);
      }
      pressed_ = true;
      endX_ = x;
      endY_ = y;
      return QuickAction::None;
    }
    if (!pressed_) return QuickAction::None;
    pressed_ = false;
    if (ignore_) return QuickAction::None;
    const int deltaX = endX_ - startX_;
    const int deltaY = endY_ - startY_;
    const bool tap = std::abs(deltaX) < 15 && std::abs(deltaY) < 15;
    if (preview) return tap ? QuickAction::PreviewBack : QuickAction::None;
    if (!settingsOpen)
      return startY_ >= 202 && deltaY <= -48 && std::abs(deltaX) < 58
          ? QuickAction::Open : QuickAction::None;
    if (deltaY >= 48 && std::abs(deltaX) < 58) return QuickAction::Close;
    if (!tap || endX_ < 12 || endX_ >= 308) return QuickAction::None;
    if (endY_ < 45 && endX_ < 84) return QuickAction::Close;
    if (endY_ >= 51 && endY_ < 103) {
      if (endX_ >= 259) return QuickAction::Louder;
      if (endX_ >= 145 && endX_ < 195) return QuickAction::Quieter;
    }
    if (endY_ >= 110 && endY_ < 160) return QuickAction::Tracking;
    if (endY_ >= 167 && endY_ < 221) return QuickAction::Preview;
    return QuickAction::None;
  }

private:
  bool pressed_ = false;
  bool ignore_ = false;
  int startX_ = 0, startY_ = 0;
  int endX_ = 0, endY_ = 0;
};

} // namespace charadock
