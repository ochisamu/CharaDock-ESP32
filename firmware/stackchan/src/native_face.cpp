// SPDX-License-Identifier: Apache-2.0
#include "charadock/native_face.hpp"

#include <algorithm>
#include <cmath>

namespace charadock {
namespace {

constexpr int32_t kWidth = 320;
constexpr int32_t kHeight = 240;
constexpr int32_t kLeftEyeX = 108;
constexpr int32_t kRightEyeX = 212;
constexpr int32_t kEyeY = 100;

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
                                uint32_t nowMs, uint8_t intervalMs) {
  if (!ready_ || nowMs - lastFrameAt_ < intervalMs)
    return;
  lastFrameAt_ = nowMs;
  const uint16_t background = rgb565(snapshot.theme.primary);
  const uint16_t foreground = rgb565(snapshot.theme.secondary);
  const uint16_t accent = rgb565(snapshot.theme.accent);
  canvas_.fillScreen(background);
  const bool warm = snapshot.listeningToVoice || snapshot.expression == Expression::Happy;
  const uint16_t cheek = blend565(background, accent, warm ? 182 : 110);
  canvas_.fillEllipse(kLeftEyeX - 29 + snapshot.faceX, 130 + snapshot.faceY, 11, 4, cheek);
  canvas_.fillEllipse(kRightEyeX + 29 + snapshot.faceX, 130 + snapshot.faceY, 11, 4, cheek);
  drawEyes(snapshot, foreground, background);
  drawMouth(snapshot, foreground, background);
  drawStatus(snapshot, nowMs, foreground, accent);
  canvas_.pushSprite(0, 0);
}

void NativeFaceRenderer::renderPortrait(const uint16_t *rgb565Pixels,
                                        const PresentationSnapshot &snapshot,
                                        uint32_t nowMs) {
  if (!ready_ || !rgb565Pixels || nowMs - lastFrameAt_ < 33)
    return;
  lastFrameAt_ = nowMs;
  const uint16_t foreground = rgb565(0xfff4e4);
  const uint16_t accent = rgb565(snapshot.theme.accent);
  // Plain uint16_t defaults to panel byte order in M5GFX. The portrait is RGB565LE.
  canvas_.setSwapBytes(true);
  canvas_.pushImage(0, 0, kWidth, kHeight, rgb565Pixels);
  canvas_.setSwapBytes(false);
  const uint16_t backing = rgb565(0x101619);
  canvas_.setFont(&fonts::efontJA_12);
  if (name_.length())
    canvas_.fillRoundRect(4, 4, std::min<int>(222, canvas_.textWidth(name_) + 9), 21, 5, backing);
  if (*snapshot.connection)
    canvas_.fillRoundRect(232, 5, 44, 23, 5, backing);
  if (caption_.length())
    canvas_.fillRoundRect(3, 184, 314, 53, 8, backing);
  canvas_.fillRoundRect(274, 3, 43, 36, 8, backing);
  drawStatus(snapshot, nowMs, foreground, accent);
  canvas_.pushSprite(0, 0);
}

void NativeFaceRenderer::renderQuickSettings(uint32_t nowMs, uint8_t volume, bool privacyMuted,
                                             const char *connection, const char *trackingStatus, bool trackingAvailable,
                                             const char *cameraStatus, bool cameraAvailable) {
  if (!ready_ || nowMs - lastFrameAt_ < 40) return;
  lastFrameAt_ = nowMs;
  const auto background = rgb565(0x101619);
  const auto row = rgb565(0x202a30);
  const auto foreground = rgb565(0xf4eee5);
  const auto secondary = rgb565(0xa8bbc0);
  const auto disabled = rgb565(0x82969d);
  canvas_.fillScreen(background);
  canvas_.setTextWrap(false);
  canvas_.setFont(&fonts::efontJA_16);
  canvas_.setTextColor(foreground);
  canvas_.drawString("閉じる", 17, 14);
  canvas_.drawString("設定", 143, 14);
  canvas_.setFont(&fonts::efontJA_12);
  canvas_.setTextColor(secondary);
  canvas_.drawString(connection, 280 - canvas_.textWidth(connection), 17);
  canvas_.setFont(&fonts::efontJA_16);
  canvas_.setTextColor(foreground);
  const auto lockColor = privacyMuted ? rgb565(0xef9e57) : foreground;
  canvas_.drawRoundRect(293, 6, 24, 26, 5, lockColor);
  canvas_.drawRoundRect(privacyMuted ? 301 : 304, 11, 8, 10, 3, lockColor);
  canvas_.fillRoundRect(298, 17, 14, 10, 2, lockColor);
  canvas_.fillRoundRect(12, 51, 296, 52, 10, row);
  canvas_.fillRoundRect(12, 110, 296, 50, 10, row);
  canvas_.fillRoundRect(12, 167, 296, 54, 10, row);
  canvas_.setTextColor(privacyMuted ? disabled : foreground);
  canvas_.drawString("音量", 25, 68);
  canvas_.drawRoundRect(145, 56, 49, 42, 7, !privacyMuted && volume ? secondary : disabled);
  canvas_.drawRoundRect(259, 56, 43, 42, 7, !privacyMuted && volume < 100 ? secondary : disabled);
  canvas_.drawString("-", 165, 68);
  canvas_.drawString("+", 276, 68);
  const String volumeText = String(volume) + "%";
  canvas_.drawString(volumeText, 227 - canvas_.textWidth(volumeText) / 2, 68);
  canvas_.setTextColor(trackingAvailable ? foreground : disabled);
  canvas_.drawString("顔追従", 25, 118);
  canvas_.setFont(&fonts::efontJA_12);
  canvas_.setTextColor(trackingAvailable ? secondary : disabled);
  canvas_.drawString(trackingStatus, 25, 141);
  canvas_.setFont(&fonts::efontJA_16);
  canvas_.setTextColor(cameraAvailable ? foreground : disabled);
  canvas_.drawString("カメラプレビュー", 25, 176);
  canvas_.setFont(&fonts::efontJA_12);
  canvas_.setTextColor(cameraAvailable ? secondary : disabled);
  canvas_.drawString(cameraStatus, 25, 201);
  canvas_.pushSprite(0, 0);
}

bool NativeFaceRenderer::ready() const { return ready_; }

bool NativeFaceRenderer::writeSnapshot(Stream &output) {
  if (!ready_)
    return false;
  output.println("STACKCHAN RGB565LE 320 240");
  lgfx::rgb565_t row[kWidth];
  for (int32_t y = 0; y < kHeight; ++y) {
    canvas_.readRect(0, y, kWidth, 1, row);
    if (output.write(reinterpret_cast<const uint8_t *>(row), sizeof(row)) != sizeof(row))
      return false;
  }
  return true;
}

bool NativeFaceRenderer::setCaption(const char *name, const char *caption) {
  const char *nextName = name ? name : "";
  const char *nextCaption = caption ? caption : "";
  if (name_ == nextName && caption_ == nextCaption) return false;
  name_ = nextName;
  caption_ = nextCaption;
  return true;
}

void NativeFaceRenderer::drawEyes(const PresentationSnapshot &snapshot,
                                  uint16_t foreground, uint16_t background) {
  const bool happy = snapshot.expression == Expression::Happy;
  if (snapshot.eyesClosed || happy || snapshot.expression == Expression::Soft) {
    const int32_t height = happy ? 9 : 5;
    const int32_t offset = happy ? 5 : -4;
    for (const int32_t x : {kLeftEyeX, kRightEyeX}) {
      canvas_.fillEllipse(x + snapshot.faceX, kEyeY + snapshot.faceY, 17, height, foreground);
      canvas_.fillEllipse(x + snapshot.faceX, kEyeY + snapshot.faceY + offset, 18, height, background);
    }
    return;
  }
  const float phase = lastFrameAt_ * 0.0005f;
  const bool thinking = snapshot.state == DeviceState::Thinking;
  const bool listening = snapshot.state == DeviceState::Listening;
  const bool steady = listening || snapshot.state == DeviceState::Speaking;
  const int32_t gazeX = snapshot.faceX + snapshot.attentionX + (thinking ? 7 : steady ? 0 : static_cast<int32_t>(3 * std::sin(phase)));
  const int32_t gazeY = snapshot.faceY + (thinking ? -5 : steady ? 0 : static_cast<int32_t>(2 * std::sin(phase * 0.71f)));
  const int32_t height = snapshot.expression == Expression::Surprised ? 23 :
                         snapshot.listeningToVoice || snapshot.speakingWithEmphasis ? 21 : 19;
  for (const int32_t x : {kLeftEyeX, kRightEyeX}) {
    canvas_.fillEllipse(x + gazeX, kEyeY + gazeY, 17, height, foreground);
    if (snapshot.speakingWithEmphasis)
      canvas_.fillEllipse(x + gazeX, kEyeY + gazeY - 29, 8, 2, foreground);
  }
}

void NativeFaceRenderer::drawMouth(const PresentationSnapshot &snapshot,
                                   uint16_t foreground, uint16_t background) {
  const int32_t x = 160 + snapshot.faceX;
  const int32_t y = 145 + snapshot.faceY;
  const int32_t aperture = snapshot.mouthAperture;
  if (aperture <= 4 && snapshot.mouthLevel == 0) {
    canvas_.fillEllipse(x, y, 17, 8, foreground);
    canvas_.fillEllipse(x, y - (snapshot.listeningToVoice ? 6 : 5), 18, 8, background);
    return;
  }
  // The coarse mouth commands remain usable by USB diagnostics.
  const int32_t open = aperture > 4 ? aperture : snapshot.mouthLevel * 42;
  const int32_t height = 3 + open * 9 / 100;
  canvas_.fillEllipse(x, y + height / 5, 15 + open / 30, height, foreground);
  if (height >= 9)
    canvas_.fillEllipse(x, y + height / 3, 9, height / 3, rgb565(snapshot.theme.accent));
}

void NativeFaceRenderer::drawStatus(const PresentationSnapshot &snapshot,
                                    uint32_t nowMs, uint16_t foreground,
                                    uint16_t accent) {
  canvas_.setFont(&fonts::efontJA_12);
  canvas_.setTextColor(foreground);
  canvas_.setTextWrap(true, false);
  canvas_.setClipRect(0, 0, 226, 30);
  canvas_.setCursor(8, 8);
  canvas_.print(name_);
  canvas_.setClipRect(0, 188, 320, 52);
  canvas_.setCursor(8, 190);
  canvas_.print(caption_);
  canvas_.clearClipRect();
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
  if (*snapshot.connection)
    canvas_.drawString(snapshot.connection, 273 - canvas_.textWidth(snapshot.connection), 11);
  canvas_.fillCircle(282, 17, 3, status);
  const uint16_t lockColor = snapshot.privacyMuted ? rgb565(0xef9e57) : foreground;
  canvas_.drawRoundRect(293, 6, 24, 26, 5, lockColor);
  canvas_.drawRoundRect(snapshot.privacyMuted ? 301 : 304, 11, 8, 10, 3, lockColor);
  canvas_.fillRoundRect(298, 17, 14, 10, 2, lockColor);
  if (snapshot.faceDetection != FaceDetectionStatus::Hidden) {
    const bool detected = snapshot.faceDetection == FaceDetectionStatus::Detected;
    const uint16_t backing = rgb565(detected ? 0x18382f : 0x192329);
    const uint16_t icon = rgb565(detected ? 0x98edc7 : 0xb0c4cc);
    canvas_.fillRoundRect(221, 40, 94, 24, 7, backing);
    canvas_.drawRoundRect(221, 40, 94, 24, 7, rgb565(detected ? 0x64ac91 : 0x51666f));
    if (detected) {
      canvas_.fillCircle(234, 52, 6, icon);
      canvas_.drawLine(231, 52, 233, 54, backing);
      canvas_.drawLine(233, 54, 237, 50, backing);
    } else {
      canvas_.drawRoundRect(229, 47, 11, 11, 3, icon);
      canvas_.fillCircle(234, 52, 1, icon);
    }
    canvas_.setTextColor(icon);
    canvas_.drawString(detected ? "顔を検知" : "顔を探し中", 245, 46);
  }
  if (snapshot.state == DeviceState::Thinking) {
    for (int index = 0; index < 3; ++index) {
      const bool active = ((nowMs / 280) % 3) == static_cast<uint32_t>(index);
      canvas_.fillCircle(281 + index * 10, 219, active ? 4 : 2,
                         active ? accent : foreground);
    }
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
