// SPDX-License-Identifier: Apache-2.0
#include "charadock/scene_renderer.hpp"

#include <algorithm>
#include <cstdio>

#include <Arduino.h>

#include "charadock/utf8.hpp"

namespace charadock::rlcd {
namespace {

constexpr int kHeaderHeight = 24;
constexpr int kFooterHeight = 24;

std::string stateLabel(const SceneSnapshot &scene) {
  if (!scene.modeLabel.empty())
    return scene.modeLabel;
  return deviceStateName(scene.state);
}

} // namespace

bool SceneRenderer::attachFonts(const EmbeddedFontAssets &assets) {
  return font12_.attach(assets.font12, assets.font12Bytes) &&
         font16_.attach(assets.font16, assets.font16Bytes);
}

bool SceneRenderer::fontsReady() const {
  return font12_.valid() && font16_.valid();
}

const ShinonomeFont &SceneRenderer::font(uint8_t pixelSize) const {
  return pixelSize == 12 ? font12_ : font16_;
}

int SceneRenderer::measure(const ShinonomeFont &face,
                           const std::string &value) const {
  int width = 0;
  size_t offset = 0;
  while (offset < value.size()) {
    const auto decoded = utf8::decodeOne(
        reinterpret_cast<const uint8_t *>(value.data()) + offset,
        value.size() - offset);
    offset += decoded.valid ? decoded.bytes : 1;
    if (decoded.codepoint == '\n')
      break;
    GlyphView view = face.glyph(decoded.valid ? decoded.codepoint : 0x25a1);
    if (!view)
      view = face.glyph(0x25a1);
    width += view ? view.width : face.fullWidth();
  }
  return width;
}

void SceneRenderer::glyph(U8G2 &canvas, const ShinonomeFont &face,
                          uint32_t codepoint, int x, int y,
                          bool inverse) const {
  GlyphView view = face.glyph(codepoint);
  if (!view)
    view = face.glyph(0x25a1);
  if (!view)
    return;
  canvas.setDrawColor(inverse ? 0 : 1);
  for (uint8_t row = 0; row < view.height; ++row) {
    const uint8_t *rowBytes = view.bitmap + row * view.bytesPerRow;
    for (uint8_t column = 0; column < view.width; ++column) {
      if (rowBytes[column / 8] & (0x80u >> (column % 8)))
        canvas.drawPixel(x + column, y + row);
    }
  }
  canvas.setDrawColor(1);
}

int SceneRenderer::text(U8G2 &canvas, const ShinonomeFont &face,
                        const std::string &value, int x, int y, int width,
                        int height, uint8_t maximumLines, bool inverse) const {
  if (!face.valid() || width <= 0 || height < face.pixelSize() ||
      maximumLines == 0)
    return 0;
  const int lineHeight = face.pixelSize() + (face.pixelSize() == 16 ? 4 : 3);
  int cursorX = x;
  int cursorY = y;
  uint8_t line = 1;
  size_t offset = 0;
  while (offset < value.size() && line <= maximumLines &&
         cursorY + face.pixelSize() <= y + height) {
    const auto decoded = utf8::decodeOne(
        reinterpret_cast<const uint8_t *>(value.data()) + offset,
        value.size() - offset);
    offset += decoded.valid ? decoded.bytes : 1;
    if (decoded.codepoint == '\n') {
      cursorX = x;
      cursorY += lineHeight;
      ++line;
      continue;
    }
    GlyphView view = face.glyph(decoded.valid ? decoded.codepoint : 0x25a1);
    if (!view)
      view = face.glyph(0x25a1);
    const int advance = view ? view.width : face.fullWidth();
    if (cursorX != x && cursorX + advance > x + width) {
      cursorX = x;
      cursorY += lineHeight;
      ++line;
      if (line > maximumLines || cursorY + face.pixelSize() > y + height)
        break;
    }
    glyph(canvas, face, decoded.valid ? decoded.codepoint : 0x25a1,
          cursorX, cursorY, inverse);
    cursorX += advance;
  }
  return line;
}

void SceneRenderer::portrait(U8G2 &canvas,
                             const MonochromeAssetStore &asset, int x, int y,
                             int width, int height, int contentOffsetX,
                             int contentOffsetY) const {
  const auto *metadata = asset.metadata();
  const uint8_t *pixels = asset.pixels();
  if (!metadata || !pixels || width <= 0 || height <= 0) {
    placeholder(canvas, x, y, width, height);
    return;
  }
  const int assetWidth = std::min<int>(width, metadata->width);
  const int assetHeight = std::min<int>(height, metadata->height);
  const int baseSourceX = std::max<int>(0, (metadata->width - assetWidth) / 2);
  const int baseSourceY = std::max<int>(0, (metadata->height - assetHeight) / 2);
  const int shiftedX = x + (width - assetWidth) / 2 + contentOffsetX;
  const int shiftedY = y + (height - assetHeight) / 2 + contentOffsetY;
  const int destinationX = std::max(x, shiftedX);
  const int destinationY = std::max(y, shiftedY);
  const int right = std::min(x + width, shiftedX + assetWidth);
  const int bottom = std::min(y + height, shiftedY + assetHeight);
  const int visibleWidth = std::max(0, right - destinationX);
  const int visibleHeight = std::max(0, bottom - destinationY);
  const int sourceX = baseSourceX + destinationX - shiftedX;
  const int sourceY = baseSourceY + destinationY - shiftedY;
  const size_t rowBytes = (metadata->width + 7u) / 8u;
  // raw1-msb uses 1 for black ink.  This ST7305 U8g2 setup exposes the panel
  // polarity in reverse (draw color 1 is physically white), so establish a
  // white paper background and render asset ink with draw color 0.
  canvas.setDrawColor(1);
  canvas.drawBox(x, y, width, height);
  canvas.setDrawColor(0);
  for (int row = 0; row < visibleHeight; ++row) {
    const uint8_t *source = pixels + (sourceY + row) * rowBytes;
    for (int column = 0; column < visibleWidth; ++column) {
      const int sourceColumn = sourceX + column;
      if (source[sourceColumn / 8] & (0x80u >> (sourceColumn % 8)))
        canvas.drawPixel(destinationX + column, destinationY + row);
    }
  }
  canvas.setDrawColor(1);
}

void SceneRenderer::portraitScaled(U8G2 &canvas,
                                   const MonochromeAssetStore &asset, int x,
                                   int y, int width, int height) const {
  const auto *metadata = asset.metadata();
  const uint8_t *pixels = asset.pixels();
  canvas.setDrawColor(1);
  canvas.drawBox(x, y, width, height);
  if (!metadata || !pixels || width <= 0 || height <= 0) {
    placeholder(canvas, x, y, width, height);
    return;
  }
  const float scale = std::min(static_cast<float>(width) / metadata->width,
                               static_cast<float>(height) / metadata->height);
  const int targetWidth = std::max(1, static_cast<int>(metadata->width * scale));
  const int targetHeight = std::max(1, static_cast<int>(metadata->height * scale));
  const int destinationX = x + (width - targetWidth) / 2;
  const int destinationY = y + (height - targetHeight) / 2;
  const size_t rowBytes = (metadata->width + 7u) / 8u;
  canvas.setDrawColor(0);
  for (int row = 0; row < targetHeight; ++row) {
    const int sourceY = std::min<int>(metadata->height - 1,
                                      row * metadata->height / targetHeight);
    const uint8_t *source = pixels + sourceY * rowBytes;
    for (int column = 0; column < targetWidth; ++column) {
      const int sourceX = std::min<int>(metadata->width - 1,
                                        column * metadata->width / targetWidth);
      if (source[sourceX / 8] & (0x80u >> (sourceX % 8)))
        canvas.drawPixel(destinationX + column, destinationY + row);
    }
  }
  canvas.setDrawColor(1);
}

void SceneRenderer::placeholder(U8G2 &canvas, int x, int y, int width,
                                int height) const {
  canvas.setDrawColor(1);
  canvas.drawBox(x, y, width, height);
  canvas.setDrawColor(0);
  const int centerX = x + width / 2;
  const int centerY = y + height / 2;
  const int radius = std::max(18, std::min(width, height) / 5);
  canvas.drawCircle(centerX, centerY, radius);
  canvas.drawCircle(centerX - radius / 3, centerY - radius / 5, 2);
  canvas.drawCircle(centerX + radius / 3, centerY - radius / 5, 2);
  canvas.drawLine(centerX - radius / 3, centerY + radius / 3,
                  centerX + radius / 3, centerY + radius / 3);
  canvas.setDrawColor(1);
}

void SceneRenderer::header(U8G2 &canvas, const SceneSnapshot &scene,
                           const SensorSnapshot &sensors) const {
  canvas.setDrawColor(1);
  canvas.drawBox(0, 0, kDisplayWidth, kHeaderHeight);
  text(canvas, font12_, scene.characterName, 12, 6, 176, 12, 1, true);

  char clock[8] = "--:--";
  if (sensors.dateTime.valid)
    std::snprintf(clock, sizeof(clock), "%02u:%02u", sensors.dateTime.hour,
                  sensors.dateTime.minute);
  const int clockWidth = measure(font12_, clock);
  text(canvas, font12_, clock, kDisplayWidth - 12 - clockWidth, 6, clockWidth,
       12, 1, true);

  const std::string label = stateLabel(scene);
  const int labelWidth = measure(font12_, label);
  const int labelX = std::max(196, kDisplayWidth - 82 - labelWidth);
  text(canvas, font12_, label, labelX, 6, labelWidth, 12, 1, true);
  canvas.setDrawColor(1);
}

void SceneRenderer::footer(U8G2 &canvas, const SceneSnapshot &scene,
                           const SensorSnapshot &sensors) const {
  const int top = kDisplayHeight - kFooterHeight;
  canvas.setDrawColor(1);
  canvas.drawHLine(0, top, kDisplayWidth);
  const std::string left = !scene.footer.empty()
                               ? scene.footer
                               : (scene.flags & SceneConnected
                                      ? "CharaDock connected"
                                      : "CharaDock offline");
  text(canvas, font12_, left, 12, top + 6, 220, 12, 1);

  char environment[40] = {};
  if (sensors.shtc3Available && sensors.batteryAvailable) {
    std::snprintf(environment, sizeof(environment), "%.1fC  %.0f%%  %u%%",
                  sensors.temperatureC, sensors.humidityPercent,
                  sensors.batteryPercent);
  } else if (sensors.shtc3Available) {
    std::snprintf(environment, sizeof(environment), "%.1fC  %.0f%%",
                  sensors.temperatureC, sensors.humidityPercent);
  } else if (sensors.batteryAvailable) {
    std::snprintf(environment, sizeof(environment), "BAT %u%%",
                  sensors.batteryPercent);
  }
  if (*environment) {
    const int environmentWidth = measure(font12_, environment);
    text(canvas, font12_, environment,
         kDisplayWidth - 12 - environmentWidth, top + 6, environmentWidth, 12,
         1);
  }
}

void SceneRenderer::home(U8G2 &canvas, const SceneSnapshot &,
                         const MonochromeAssetStore &asset, int offsetX,
                         int offsetY) const {
  portrait(canvas, asset, 0, kHeaderHeight, kDisplayWidth,
           kDisplayHeight - kHeaderHeight - kFooterHeight, offsetX, offsetY);
}

void SceneRenderer::conversation(U8G2 &canvas, const SceneSnapshot &scene,
                                 const MonochromeAssetStore &asset) const {
  if (scene.caption.empty()) {
    home(canvas, scene, asset, 0, 0);
    return;
  }
  constexpr int captionTop = 204;
  portrait(canvas, asset, 0, kHeaderHeight, kDisplayWidth,
           captionTop - kHeaderHeight);
  canvas.setDrawColor(0);
  canvas.drawBox(0, captionTop, kDisplayWidth,
                 kDisplayHeight - captionTop);
  canvas.setDrawColor(1);
  canvas.drawHLine(0, captionTop, kDisplayWidth);
  text(canvas, font(scene.captionFont), scene.caption, 16, captionTop + 10,
       kDisplayWidth - 32, kDisplayHeight - captionTop - 12, 4);
}

void SceneRenderer::ambient(U8G2 &canvas, const SceneSnapshot &scene,
                            const SensorSnapshot &sensors,
                            const MonochromeAssetStore &asset) const {
  constexpr int bodyTop = kHeaderHeight;
  constexpr int bodyHeight = kDisplayHeight - kHeaderHeight - kFooterHeight;
  // Most portraits look toward the left. Put the information in their gaze
  // and retain the source pixel grid by cropping, rather than scaling, the
  // portrait into the larger right-hand pane.
  constexpr int split = 158;
  canvas.setDrawColor(1);
  canvas.drawBox(0, bodyTop, kDisplayWidth, bodyHeight);
  portrait(canvas, asset, split + 1, bodyTop,
           kDisplayWidth - split - 1, bodyHeight);

  // Keep both halves on white paper. A large inverted clock panel made the
  // sparse portrait feel washed out; a compact black badge and rules provide
  // enough visual weight without turning half the reflective display black.
  canvas.setDrawColor(0);
  canvas.drawVLine(split, bodyTop, bodyHeight);
  canvas.drawBox(12, bodyTop + 10, 76, 18);
  const bool waiting = scene.state == DeviceState::Thinking;
  text(canvas, font12_, waiting ? "WAIT" : "STANDBY",
       18, bodyTop + 13, 64, 12, 1);

  const int panelX = 12;
  const int panelWidth = split - 24;

  char clock[8] = "--:--";
  char date[16] = "----/--/--";
  if (sensors.dateTime.valid) {
    std::snprintf(clock, sizeof(clock), "%02u:%02u", sensors.dateTime.hour,
                  sensors.dateTime.minute);
    std::snprintf(date, sizeof(date), "%04u/%02u/%02u",
                  sensors.dateTime.year, sensors.dateTime.month,
                  sensors.dateTime.day);
  }
  canvas.setFont(u8g2_font_logisoso32_tn);
  canvas.setDrawColor(0);
  const int clockWidth = canvas.getStrWidth(clock);
  const int timeX = panelX + std::max(0, (panelWidth - clockWidth) / 2);
  canvas.drawStr(timeX, bodyTop + 70, clock);
  const int dateWidth = measure(font12_, date);
  text(canvas, font12_, date,
       panelX + std::max(0, (panelWidth - dateWidth) / 2), bodyTop + 83,
       std::min(panelWidth, dateWidth), 12, 1, true);
  canvas.setDrawColor(0);
  canvas.drawHLine(panelX, bodyTop + 106, panelWidth);

  text(canvas, font12_, "ROOM", panelX, bodyTop + 120, panelWidth, 12, 1,
       true);
  char temperature[24] = "--.- C";
  char humidity[24] = "-- % RH";
  if (sensors.shtc3Available) {
    std::snprintf(temperature, sizeof(temperature), "%.1f C",
                  sensors.temperatureC);
    std::snprintf(humidity, sizeof(humidity), "%.0f %% RH",
                  sensors.humidityPercent);
  }
  text(canvas, font16_, temperature, panelX, bodyTop + 140, panelWidth, 18, 1,
       true);
  text(canvas, font16_, humidity, panelX, bodyTop + 164, panelWidth, 18, 1,
       true);

  char battery[24] = "BAT --";
  if (sensors.batteryAvailable)
    std::snprintf(battery, sizeof(battery), "BAT %u%%",
                  sensors.batteryPercent);
  canvas.setDrawColor(0);
  canvas.drawHLine(panelX, bodyTop + 188, panelWidth);
  text(canvas, font12_, battery, panelX, bodyTop + 198, panelWidth, 12, 1,
       true);
  const int barY = bodyTop + 218;
  canvas.setDrawColor(0);
  canvas.drawFrame(panelX, barY, panelWidth, 8);
  if (sensors.batteryAvailable && panelWidth > 4) {
    const int fill = std::max(0, std::min(panelWidth - 4,
        (panelWidth - 4) * sensors.batteryPercent / 100));
    if (fill)
      canvas.drawBox(panelX + 2, barY + 2, fill, 4);
  }
  const std::string hint = waiting
      ? (scene.activity.empty() ? "返事を待っています" : scene.activity)
      : (scene.state == DeviceState::Listening ? "音声待ち受け中" : "KEY  PORTRAIT");
  text(canvas, font12_, hint, panelX, bodyTop + 234,
       panelWidth, 27, 2, true);
  canvas.setDrawColor(1);
}

void SceneRenderer::work(U8G2 &canvas, const SceneSnapshot &scene,
                         const MonochromeAssetStore &asset) const {
  constexpr int split = 190;
  portrait(canvas, asset, 0, kHeaderHeight, split,
           kDisplayHeight - kHeaderHeight - kFooterHeight);
  canvas.setDrawColor(0);
  canvas.drawBox(split, kHeaderHeight, kDisplayWidth - split,
                 kDisplayHeight - kHeaderHeight - kFooterHeight);
  canvas.setDrawColor(1);
  canvas.drawVLine(split, kHeaderHeight,
                   kDisplayHeight - kHeaderHeight - kFooterHeight);
  text(canvas, font(scene.activityFont), scene.activity, split + 14,
       kHeaderHeight + 18, kDisplayWidth - split - 28, 90, 4);
  if (!scene.nextAction.empty()) {
    canvas.drawHLine(split + 14, 132, kDisplayWidth - split - 28);
    text(canvas, font12_, "NEXT", split + 14, 142, 44, 12, 1);
    text(canvas, font(scene.nextActionFont), scene.nextAction, split + 14, 162,
         kDisplayWidth - split - 28, 52, 3);
  }
  char details[48] = {};
  std::snprintf(details, sizeof(details), "%02lu:%02lu  ITEMS %u",
                static_cast<unsigned long>(scene.elapsedSeconds / 60),
                static_cast<unsigned long>(scene.elapsedSeconds % 60),
                scene.artifactCount);
  text(canvas, font12_, details, split + 14, 246,
       kDisplayWidth - split - 28, 12, 1);
}

void SceneRenderer::recovery(U8G2 &canvas, const SceneSnapshot &scene) const {
  placeholder(canvas, 0, 36, kDisplayWidth, 142);
  const int nameWidth = measure(font16_, scene.characterName);
  text(canvas, font16_, scene.characterName,
       std::max(12, (static_cast<int>(kDisplayWidth) - nameWidth) / 2), 178,
       std::min(nameWidth, static_cast<int>(kDisplayWidth) - 24), 16, 1);
  const std::string message = !scene.activity.empty()
                                  ? scene.activity
                                  : "PCを探しています…";
  const int messageWidth = measure(font16_, message);
  text(canvas, font16_, message,
       std::max(12, (static_cast<int>(kDisplayWidth) - messageWidth) / 2), 220,
       kDisplayWidth - 24, 20, 1);
}

void SceneRenderer::compose(U8G2 &canvas, const SceneSnapshot &scene,
                            const SensorSnapshot &sensors,
                            const MonochromeAssetStore &portraitAsset,
                            bool keyPressed, bool ambientMode,
                            int8_t portraitOffsetX,
                            int8_t portraitOffsetY) {
  const uint32_t started = micros();
  canvas.clearBuffer();
  canvas.setDrawColor(1);
  if (ambientMode) {
    ambient(canvas, scene, sensors, portraitAsset);
  } else switch (scene.scene) {
  case SceneId::Home:
  case SceneId::Offline:
    home(canvas, scene, portraitAsset, portraitOffsetX, portraitOffsetY);
    break;
  case SceneId::Conversation:
    conversation(canvas, scene, portraitAsset);
    break;
  case SceneId::Work:
    work(canvas, scene, portraitAsset);
    break;
  case SceneId::Recovery:
    recovery(canvas, scene);
    break;
  }
  header(canvas, scene, sensors);
  if (keyPressed) {
    canvas.setDrawColor(0);
    canvas.drawDisc(194, 12, 4);
    canvas.setDrawColor(1);
  }
  if ((ambientMode || scene.scene != SceneId::Conversation) &&
      scene.scene != SceneId::Recovery)
    footer(canvas, scene, sensors);
  lastComposeMicros_ = micros() - started;
}

uint32_t SceneRenderer::lastComposeMicros() const {
  return lastComposeMicros_;
}

} // namespace charadock::rlcd
