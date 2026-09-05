// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

#include <U8g2lib.h>

#include "charadock/board.hpp"
#include "charadock/monochrome_asset.hpp"
#include "charadock/scene_model.hpp"
#include "charadock/shinonome_font.hpp"

namespace charadock::rlcd {

struct RenderMetrics {
  uint32_t composeMicros = 0;
  uint32_t flushMicros = 0;
  uint32_t revision = 0;
};

class SceneRenderer {
public:
  bool attachFonts(const EmbeddedFontAssets &assets);
  bool fontsReady() const;
  void compose(U8G2 &canvas, const SceneSnapshot &scene,
               const SensorSnapshot &sensors,
               const MonochromeAssetStore &portrait,
               bool keyPressed = false, bool ambient = false,
               int8_t portraitOffsetX = 0, int8_t portraitOffsetY = 0);
  uint32_t lastComposeMicros() const;

private:
  const ShinonomeFont &font(uint8_t pixelSize) const;
  int measure(const ShinonomeFont &font, const std::string &text) const;
  void glyph(U8G2 &canvas, const ShinonomeFont &font, uint32_t codepoint,
             int x, int y, bool inverse = false) const;
  int text(U8G2 &canvas, const ShinonomeFont &font, const std::string &value,
           int x, int y, int width, int height, uint8_t maximumLines,
           bool inverse = false) const;
  void portrait(U8G2 &canvas, const MonochromeAssetStore &asset, int x, int y,
                int width, int height, int contentOffsetX = 0,
                int contentOffsetY = 0) const;
  void portraitScaled(U8G2 &canvas, const MonochromeAssetStore &asset, int x,
                      int y, int width, int height) const;
  void placeholder(U8G2 &canvas, int x, int y, int width, int height) const;
  void header(U8G2 &canvas, const SceneSnapshot &scene,
              const SensorSnapshot &sensors) const;
  void footer(U8G2 &canvas, const SceneSnapshot &scene,
              const SensorSnapshot &sensors) const;
  void home(U8G2 &canvas, const SceneSnapshot &scene,
            const MonochromeAssetStore &asset, int offsetX = 0,
            int offsetY = 0) const;
  void conversation(U8G2 &canvas, const SceneSnapshot &scene,
                    const MonochromeAssetStore &asset) const;
  void work(U8G2 &canvas, const SceneSnapshot &scene,
            const MonochromeAssetStore &asset) const;
  void recovery(U8G2 &canvas, const SceneSnapshot &scene) const;
  void ambient(U8G2 &canvas, const SceneSnapshot &scene,
               const SensorSnapshot &sensors,
               const MonochromeAssetStore &asset) const;

  ShinonomeFont font12_;
  ShinonomeFont font16_;
  uint32_t lastComposeMicros_ = 0;
};

} // namespace charadock::rlcd
