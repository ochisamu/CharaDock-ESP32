// SPDX-License-Identifier: Apache-2.0
#include "charadock/camera_preview.hpp"

#include <Arduino.h>
#include <M5Unified.hpp>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <human_face_detect_msr01.hpp>
#include <memory>

#include <algorithm>
#include <atomic>
#include <cstring>

// The Arduino ESP32 camera library owns SCCB only during sensor setup.
extern "C" int SCCB_Deinit(void);

namespace charadock {
namespace {
constexpr size_t kWidth = 320, kHeight = 240;
constexpr size_t kBytes = kWidth * kHeight * sizeof(uint16_t);
std::atomic<bool> requested{false};
std::atomic<bool> running{false};
std::atomic<uint32_t> frames{0};
std::atomic<uint32_t> lastFaceAt{0};
std::atomic<uint32_t> facePositionPacked{0};
std::atomic<uint32_t> inferences{0};
std::atomic<bool> previewMode{true};
std::atomic<bool> detectionPaused{false};
SemaphoreHandle_t frameLock = nullptr;
uint16_t *pixels = nullptr;
bool fresh = false;

camera_config_t cameraConfig() {
  camera_config_t cfg{};
  cfg.pin_pwdn = cfg.pin_reset = cfg.pin_xclk = -1;
  cfg.pin_sccb_sda = 12;
  cfg.pin_sccb_scl = 11;
  cfg.pin_d0 = 39;
  cfg.pin_d1 = 40;
  cfg.pin_d2 = 41;
  cfg.pin_d3 = 42;
  cfg.pin_d4 = 15;
  cfg.pin_d5 = 16;
  cfg.pin_d6 = 48;
  cfg.pin_d7 = 47;
  cfg.pin_vsync = 46;
  cfg.pin_href = 38;
  cfg.pin_pclk = 45;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_RGB565;
  cfg.frame_size = FRAMESIZE_QVGA;
  cfg.fb_count = 1;
  cfg.fb_location = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  return cfg;
}

void captureTask(void *) {
  std::unique_ptr<HumanFaceDetectMSR01> detector;
  uint8_t consecutiveFaces = 0;
  while (requested.load()) {
      if (detectionPaused.load()) {
        consecutiveFaces = 0;
        vTaskDelay(pdMS_TO_TICKS(30));
        continue;
      }
      auto *frame = esp_camera_fb_get();
      if (!frame) {
        if (detectionPaused.load()) continue;
        break;
      }
      if (requested.load() && !detectionPaused.load() && frame->width == kWidth && frame->height == kHeight &&
          frame->format == PIXFORMAT_RGB565 && frame->len == kBytes) {
        if (previewMode.load()) {
          consecutiveFaces = 0;
          xSemaphoreTake(frameLock, portMAX_DELAY);
          std::memcpy(pixels, frame->buf, kBytes);
          fresh = true;
          xSemaphoreGive(frameLock);
        } else {
          if (!detector) detector = std::make_unique<HumanFaceDetectMSR01>(0.5F, 0.5F, 3, 0.2F);
          const auto &faces = detector->infer(reinterpret_cast<uint16_t *>(frame->buf),
                                               {static_cast<int>(kHeight), static_cast<int>(kWidth), 3});
          ++inferences;
          int width = 54;
          int center = kWidth / 2;
          int centerY = kHeight / 2;
          for (const auto &face : faces) {
            if (face.box.size() < 4 || face.box[2] - face.box[0] <= width) continue;
            width = face.box[2] - face.box[0];
            center = (face.box[0] + face.box[2]) / 2;
            centerY = (face.box[1] + face.box[3]) / 2;
          }
          if (width == 54) consecutiveFaces = 0;
          else if (consecutiveFaces == 1) {
            facePositionPacked.store(static_cast<uint32_t>(center) |
                                     (static_cast<uint32_t>(centerY) << 16));
            if (requested.load() && !detectionPaused.load() && !previewMode.load())
              lastFaceAt.store(millis());
          } else consecutiveFaces = 1;
        }
        ++frames;
      }
      esp_camera_fb_return(frame);
      vTaskDelay(pdMS_TO_TICKS(previewMode.load() ? 90 : 230));
  }
  esp_camera_deinit();
  xSemaphoreTake(frameLock, portMAX_DELAY);
  heap_caps_free(pixels);
  pixels = nullptr;
  fresh = false;
  xSemaphoreGive(frameLock);
  requested.store(false);
  lastFaceAt.store(0);
  detector.reset();
  running.store(false);
  vTaskDelete(nullptr);
}
} // namespace

bool CameraPreview::start(bool preview) {
  if (running.load()) {
    if (!requested.load()) return false;
    if (previewMode.exchange(preview) != preview) lastFaceAt.store(0);
    detectionPaused.store(false);
    return true;
  }
  if (!frameLock) frameLock = xSemaphoreCreateMutex();
  if (!frameLock) return false;
  pixels = static_cast<uint16_t *>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!pixels) return false;
  frames.store(0);
  inferences.store(0);
  lastFaceAt.store(0);
  previewMode.store(preview);
  detectionPaused.store(false);
  auto cfg = cameraConfig();
  // The camera SDK and M5Unified share SCL/SDA and use different I2C
  // drivers. Complete SCCB setup on the main loop before returning the
  // bus to touch, microphone and backlight. The worker only consumes DVP.
  M5.In_I2C.release();
  const esp_err_t result = esp_camera_init(&cfg);
  if (result == ESP_OK) {
    if (auto *sensor = esp_camera_sensor_get()) sensor->set_hmirror(sensor, 1);
    SCCB_Deinit();
  }
  M5.In_I2C.begin();
  if (result != ESP_OK) {
    Serial.printf("camera preview init failed: 0x%x\n", result);
    heap_caps_free(pixels);
    pixels = nullptr;
    return false;
  }

  requested.store(true);
  running.store(true);
  if (xTaskCreatePinnedToCore(captureTask, "camera", 12288, nullptr, 1, nullptr, 0) == pdPASS)
    return true;
  requested.store(false);
  running.store(false);
  esp_camera_deinit();
  heap_caps_free(pixels);
  pixels = nullptr;
  return false;
}

void CameraPreview::stop() { requested.store(false); }
void CameraPreview::pauseForConversation() {
  if (active()) stop();
  else setDetectionPaused(true);
}
void CameraPreview::setDetectionPaused(bool paused) {
  if (detecting() && detectionPaused.exchange(paused) != paused) lastFaceAt.store(0);
}
bool CameraPreview::active() const { return requested.load() && previewMode.load(); }
bool CameraPreview::detecting() const { return requested.load() && !previewMode.load(); }
bool CameraPreview::recentFace(uint32_t now) const {
  const uint32_t seen = lastFaceAt.load();
  return detecting() && !detectionPaused.load() && seen && now - seen < 1000;
}
bool CameraPreview::facePosition(uint32_t now, uint32_t &seenAt, int &x, int &y) const {
  seenAt = lastFaceAt.load();
  if (!detecting() || detectionPaused.load() || !seenAt || now - seenAt >= 1000) return false;
  const uint32_t position = facePositionPacked.load();
  x = static_cast<int>(position & 0xffff) - static_cast<int>(kWidth / 2);
  y = static_cast<int>(position >> 16) - static_cast<int>(kHeight / 2);
  return true;
}
int8_t CameraPreview::gazeOffset(uint32_t now) const {
  int x, y;
  uint32_t seenAt;
  return facePosition(now, seenAt, x, y) ? std::clamp(x / 13, -9, 9) : 0;
}
uint32_t CameraPreview::inferenceCount() const { return inferences.load(); }
uint32_t CameraPreview::frameCount() const { return frames.load(); }

void CameraPreview::draw() {
  if (!active() || xSemaphoreTake(frameLock, 0) != pdTRUE) return;
  if (fresh) {
    auto &screen = M5.Display;
    screen.pushImage(0, 0, kWidth, kHeight, pixels);
    fresh = false;
  }
  xSemaphoreGive(frameLock);
}
} // namespace charadock
