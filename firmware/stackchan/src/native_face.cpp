// SPDX-License-Identifier: Apache-2.0
#include "charadock/native_face.hpp"

#include <cmath>

namespace charadock {
namespace {

constexpr int32_t kWidth = 320;
constexpr int32_t kHeight = 240;
constexpr int32_t kLeftEyeX = 100;
constexpr int32_t kRightEyeX = 220;
constexpr int32_t kEyeY = 103;

} // namespace

NativeFaceRenderer::NativeFaceRenderer(LGFX_Device &display)
    : display_(display), canvas_(&display) {}

NativeFaceRenderer::~NativeFaceRenderer() {
  if (ready_)
    canvas_.deleteSprite();
}

bool NativeFaceRenderer::begin() {
  display_.setRotation(1);
  canvas_.setColorDepth(16);
  ready_ = canvas_.createSprite(kWidth, kHeight) != nullptr;
  return ready_;
}

void NativeFaceRenderer::render(const PresentationSnapshot &snapshot,
                                uint32_t nowMs) {
  if (!ready_ || nowMs - lastFrameAt_ < 33)
    return;
  lastFrameAt_ = nowMs;
  const uint16_t background = rgb565(snapshot.theme.primary);
  const uint16_t foreground = rgb565(snapshot.theme.secondary);
  const uint16_t accent = rgb565(snapshot.theme.accent);
  canvas_.fillScreen(background);
  drawEyes(snapshot, foreground, accent);
  drawMouth(snapshot, foreground);
  drawStatus(snapshot, nowMs, foreground, accent);
  canvas_.pushSprite(0, 0);
}

void NativeFaceRenderer::renderPortrait(const uint16_t *rgb565Pixels,
                                        const PresentationSnapshot &snapshot,
                                        uint32_t nowMs) {
  if (!ready_ || !rgb565Pixels || nowMs - lastFrameAt_ < 33)
    return;
  lastFrameAt_ = nowMs;
  const uint16_t foreground = rgb565(snapshot.theme.secondary);
  const uint16_t accent = rgb565(snapshot.theme.accent);
  canvas_.pushImage(0, 0, kWidth, kHeight, rgb565Pixels);
  // Keep the UI subordinate to the artwork: one backed corner indicator and
  // the same tiny thinking dots used by Native Face.
  canvas_.fillCircle(302, 18, 9, rgb565(0x171513));
  drawStatus(snapshot, nowMs, foreground, accent);
  canvas_.pushSprite(0, 0);
}

bool NativeFaceRenderer::ready() const { return ready_; }

void NativeFaceRenderer::drawEyes(const PresentationSnapshot &snapshot,
                                  uint16_t foreground, uint16_t accent) {
  if (snapshot.state == DeviceState::Error) {
    drawThickLine(82, 88, 118, 118, 5, foreground);
    drawThickLine(118, 88, 82, 118, 5, foreground);
    drawThickLine(202, 88, 238, 118, 5, foreground);
    drawThickLine(238, 88, 202, 118, 5, foreground);
    return;
  }
  if (snapshot.eyesClosed || snapshot.expression == Expression::Happy ||
      snapshot.expression == Expression::Soft) {
    const int32_t slope = snapshot.expression == Expression::Happy ? 7 : 2;
    drawThickLine(82, kEyeY + slope, 118, kEyeY - slope, 6, foreground);
    drawThickLine(202, kEyeY - slope, 238, kEyeY + slope, 6, foreground);
    return;
  }
  if (snapshot.expression == Expression::Surprised) {
    canvas_.drawCircle(kLeftEyeX, kEyeY, 21, foreground);
    canvas_.drawCircle(kRightEyeX, kEyeY, 21, foreground);
    canvas_.fillCircle(kLeftEyeX, kEyeY, 8, foreground);
    canvas_.fillCircle(kRightEyeX, kEyeY, 8, foreground);
    return;
  }
  int32_t gazeX = snapshot.state == DeviceState::Thinking ? 8 : 0;
  int32_t gazeY = snapshot.state == DeviceState::Thinking ? -5 : 0;
  const int32_t radius = snapshot.state == DeviceState::Listening ? 20 : 17;
  canvas_.fillCircle(kLeftEyeX, kEyeY, radius, foreground);
  canvas_.fillCircle(kRightEyeX, kEyeY, radius, foreground);
  if (snapshot.state == DeviceState::Listening) {
    canvas_.fillCircle(kLeftEyeX + gazeX, kEyeY + gazeY, 7, accent);
    canvas_.fillCircle(kRightEyeX + gazeX, kEyeY + gazeY, 7, accent);
  }
}

void NativeFaceRenderer::drawMouth(const PresentationSnapshot &snapshot,
                                   uint16_t foreground) {
  uint8_t mouth =
      snapshot.state == DeviceState::Speaking ? snapshot.mouthLevel : 0;
  if (snapshot.expression == Expression::Surprised)
    mouth = 2;
  if (mouth == 0) {
    canvas_.fillRoundRect(140, 160, 40, 6, 3, foreground);
  } else if (mouth == 1) {
    canvas_.fillEllipse(160, 163, 20, 9, foreground);
  } else {
    canvas_.fillEllipse(160, 162, 21, 16, foreground);
  }
}

void NativeFaceRenderer::drawStatus(const PresentationSnapshot &snapshot,
                                    uint32_t nowMs, uint16_t foreground,
                                    uint16_t accent) {
  uint16_t status = accent;
  if (snapshot.state == DeviceState::Error)
    status = rgb565(0xd92d20);
  if (snapshot.state == DeviceState::Listening)
    status = rgb565(0x1688e8);
  if (snapshot.state == DeviceState::Thinking)
    status = rgb565(0xe7a929);
  if (snapshot.state == DeviceState::Speaking)
    status = rgb565(0x9b51e0);
  if (snapshot.state == DeviceState::Connecting) {
    const float phase = static_cast<float>(nowMs % 1600) / 1600.0f;
    const uint8_t amount = static_cast<uint8_t>(
        70 + 130 * (0.5f + 0.5f * std::sin(phase * 6.2831853f)));
    status = blend565(rgb565(snapshot.theme.primary), accent, amount);
  }
  canvas_.fillCircle(302, 18, 6, status);
  if (snapshot.state == DeviceState::Thinking) {
    for (int index = 0; index < 3; ++index) {
      const bool active = ((nowMs / 280) % 3) == static_cast<uint32_t>(index);
      canvas_.fillCircle(281 + index * 10, 219, active ? 4 : 2,
                         active ? accent : foreground);
    }
  }
}

void NativeFaceRenderer::drawThickLine(int32_t x0, int32_t y0, int32_t x1,
                                       int32_t y1, uint8_t thickness,
                                       uint16_t color) {
  const int32_t half = thickness / 2;
  for (int32_t offset = -half; offset <= half; ++offset) {
    canvas_.drawLine(x0, y0 + offset, x1, y1 + offset, color);
  }
}

uint16_t NativeFaceRenderer::rgb565(uint32_t rgb) {
  const uint8_t red = static_cast<uint8_t>((rgb >> 16) & 0xff);
  const uint8_t green = static_cast<uint8_t>((rgb >> 8) & 0xff);
  const uint8_t blue = static_cast<uint8_t>(rgb & 0xff);
  return static_cast<uint16_t>(((red & 0xf8) << 8) | ((green & 0xfc) << 3) |
                               (blue >> 3));
}

uint16_t NativeFaceRenderer::blend565(uint16_t from, uint16_t to,
                                      uint8_t amount) {
  const uint16_t inverse = 255 - amount;
  const uint16_t red =
      (((from >> 11) & 0x1f) * inverse + ((to >> 11) & 0x1f) * amount) / 255;
  const uint16_t green =
      (((from >> 5) & 0x3f) * inverse + ((to >> 5) & 0x3f) * amount) / 255;
  const uint16_t blue = ((from & 0x1f) * inverse + (to & 0x1f) * amount) / 255;
  return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

} // namespace charadock
