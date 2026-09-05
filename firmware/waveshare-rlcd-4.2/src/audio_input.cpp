// SPDX-License-Identifier: Apache-2.0
//
// ES7210 register sequencing is adapted from Espressif esp_codec_dev 1.5.4
// (Apache-2.0) and Waveshare's ESP32-S3-RLCD-4.2 audio example. See
// THIRD_PARTY_NOTICES.md. The ESP32 supplies MCLK before the codec is reset.

#include "charadock/audio_input.hpp"

#include <Arduino.h>
#include <Wire.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>

#include "charadock/board.hpp"

namespace charadock::rlcd {
namespace {

constexpr uint32_t kSampleRate = 16000;
constexpr size_t kMaximumMonoBytes = 1024;
constexpr size_t kStereoBytes = kMaximumMonoBytes * 2;
constexpr uint8_t kMicGain34_5Db = 0x0c;
constexpr uint8_t kDmaDescriptors = 8;
constexpr uint16_t kDmaFrames = 256;

gpio_num_t gpio(int pin) { return static_cast<gpio_num_t>(pin); }

} // namespace

struct AudioInput::Impl {
  i2s_chan_handle_t rx = nullptr;
  int16_t *stereo = nullptr;
  bool initialized = false;
  bool capturing = false;
  bool codecConfigured = false;
  uint8_t clockOff = 0x3f;
  uint16_t lastRms = 0;

  bool writeRegister(uint8_t reg, uint8_t value) {
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
      Wire.beginTransmission(i2c::kEs7210Address);
      Wire.write(reg);
      Wire.write(value);
      if (Wire.endTransmission() == 0)
        return true;
      delay(2);
    }
    Serial.printf("# RLCD microphone I2C write failed reg=0x%02x\r\n", reg);
    return false;
  }

  bool readRegister(uint8_t reg, uint8_t &value) {
    Wire.beginTransmission(i2c::kEs7210Address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0 ||
        Wire.requestFrom(i2c::kEs7210Address, 1) != 1) {
      Serial.printf("# RLCD microphone I2C read failed reg=0x%02x\r\n", reg);
      return false;
    }
    value = static_cast<uint8_t>(Wire.read());
    return true;
  }

  bool updateRegister(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t current = 0;
    return readRegister(reg, current) &&
           writeRegister(reg, static_cast<uint8_t>((current & ~mask) |
                                                   (value & mask)));
  }

  bool selectBoardMicrophones() {
    // Waveshare's board profile uses MIC1 and MIC3 as a stereo pair.
    for (uint8_t reg = 0x43; reg <= 0x46; ++reg) {
      if (!updateRegister(reg, 0x10, 0x00))
        return false;
    }
    return writeRegister(0x4b, 0xff) && writeRegister(0x4c, 0xff) &&
           updateRegister(0x01, 0x0b, 0x00) &&
           writeRegister(0x4b, 0x00) &&
           updateRegister(0x43, 0x1f,
                          static_cast<uint8_t>(0x10 | kMicGain34_5Db)) &&
           updateRegister(0x01, 0x15, 0x00) &&
           writeRegister(0x4c, 0x00) &&
           updateRegister(0x45, 0x1f,
                          static_cast<uint8_t>(0x10 | kMicGain34_5Db)) &&
           writeRegister(0x12, 0x00);
  }

  bool configureCodec() {
    const uint8_t initial[][2] = {
        {0x00, 0xff}, {0x00, 0x41}, {0x01, 0x3f}, {0x09, 0x30},
        {0x0a, 0x30}, {0x23, 0x2a}, {0x22, 0x0a}, {0x20, 0x0a},
        {0x21, 0x2a}, {0x40, 0x43}, {0x41, 0x70}, {0x42, 0x70},
        {0x07, 0x20}, {0x02, 0xc1},
    };
    for (const auto &item : initial) {
      if (!writeRegister(item[0], item[1]))
        return false;
    }
    // Codec is an I2S slave; ESP32 generates BCLK, WS and 256fs MCLK.
    if (!updateRegister(0x08, 0x01, 0x00) || !selectBoardMicrophones())
      return false;

    // 16-bit Philips I2S in two slots. Slave mode needs no internal sample
    // rate divider table; the external clocks define 16 kHz.
    uint8_t interface = 0;
    if (!readRegister(0x11, interface))
      return false;
    interface = static_cast<uint8_t>((interface & 0x1c) | 0x60);
    if (!writeRegister(0x11, interface) || !readRegister(0x01, clockOff))
      return false;

    const uint8_t start[][2] = {
        {0x01, clockOff}, {0x06, 0x00}, {0x40, 0x43}, {0x47, 0x08},
        {0x48, 0x08},     {0x49, 0x08}, {0x4a, 0x08},
    };
    for (const auto &item : start) {
      if (!writeRegister(item[0], item[1]))
        return false;
    }
    if (!selectBoardMicrophones() || !writeRegister(0x40, 0x43) ||
        !writeRegister(0x00, 0x71) || !writeRegister(0x00, 0x41) ||
        !updateRegister(0x14, 0x03, 0x00) ||
        !updateRegister(0x15, 0x03, 0x00))
      return false;
    codecConfigured = true;
    return true;
  }

  void shutdownCodec() {
    if (!codecConfigured)
      return;
    const uint8_t stop[][2] = {
        {0x47, 0xff}, {0x48, 0xff}, {0x49, 0xff}, {0x4a, 0xff},
        {0x4b, 0xff}, {0x4c, 0xff}, {0x40, 0xc0}, {0x01, 0x7f},
        {0x06, 0x07},
    };
    for (const auto &item : stop)
      writeRegister(item[0], item[1]);
    codecConfigured = false;
  }

  void shutdownTransport() {
    shutdownCodec();
    if (rx) {
      const esp_err_t result = i2s_channel_disable(rx);
      if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        Serial.printf("# RLCD microphone I2S disable failed: %s\r\n",
                      esp_err_to_name(result));
      i2s_del_channel(rx);
      rx = nullptr;
    }
    capturing = false;
    lastRms = 0;
  }
};

AudioInput::AudioInput() = default;

AudioInput::~AudioInput() {
  stop();
  if (impl_) {
    if (impl_->stereo)
      heap_caps_free(impl_->stereo);
    delete impl_;
  }
}

bool AudioInput::begin() {
  if (impl_)
    return impl_->initialized;
  impl_ = new (std::nothrow) Impl();
  if (!impl_)
    return false;
  impl_->stereo = static_cast<int16_t *>(
      heap_caps_malloc(kStereoBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!impl_->stereo)
    return false;
  Wire.beginTransmission(i2c::kEs7210Address);
  impl_->initialized = Wire.endTransmission() == 0;
  return impl_->initialized;
}

bool AudioInput::available() const {
  return impl_ && impl_->initialized;
}

bool AudioInput::start() {
  if (!available())
    return false;
  if (impl_->capturing)
    return true;

  // Never energize the loudspeaker while the microphone owns I2S0.
  gpio_set_direction(gpio(pins::kSpeakerEnable), GPIO_MODE_OUTPUT);
  gpio_set_level(gpio(pins::kSpeakerEnable), 0);

  i2s_chan_config_t channel =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channel.dma_desc_num = kDmaDescriptors;
  channel.dma_frame_num = kDmaFrames;
  if (i2s_new_channel(&channel, nullptr, &impl_->rx) != ESP_OK)
    return false;

  i2s_std_config_t config{};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  config.gpio_cfg.mclk = gpio(pins::kAudioMclk);
  config.gpio_cfg.bclk = gpio(pins::kAudioBclk);
  config.gpio_cfg.ws = gpio(pins::kAudioWordSelect);
  config.gpio_cfg.dout = I2S_GPIO_UNUSED;
  config.gpio_cfg.din = gpio(pins::kAudioDataIn);
  if (i2s_channel_init_std_mode(impl_->rx, &config) != ESP_OK ||
      i2s_channel_enable(impl_->rx) != ESP_OK) {
    impl_->shutdownTransport();
    return false;
  }
  delay(1);
  if (!impl_->configureCodec()) {
    impl_->shutdownTransport();
    return false;
  }
  impl_->capturing = true;
  Serial.println("# RLCD microphone ready rate=16000 gain=34.5dB mics=1+3");
  return true;
}

bool AudioInput::readPcm16(uint8_t *destination, size_t capacity,
                           size_t &length, uint32_t timeoutMs) {
  length = 0;
  if (!active() || !destination || capacity < sizeof(int16_t))
    return false;
  const size_t monoBytes = std::min(capacity & ~1u, kMaximumMonoBytes);
  const size_t stereoWanted = monoBytes * 2;
  size_t received = 0;
  const esp_err_t result = i2s_channel_read(
      impl_->rx, impl_->stereo, stereoWanted, &received,
      pdMS_TO_TICKS(timeoutMs));
  if (result == ESP_ERR_TIMEOUT)
    return true;
  if (result != ESP_OK || received < 2 * sizeof(int16_t))
    return false;

  const size_t frames = received / (2 * sizeof(int16_t));
  uint64_t leftEnergy = 0;
  uint64_t rightEnergy = 0;
  for (size_t index = 0; index < frames; ++index) {
    const int32_t left = impl_->stereo[index * 2];
    const int32_t right = impl_->stereo[index * 2 + 1];
    leftEnergy += static_cast<uint32_t>(std::abs(left));
    rightEnergy += static_cast<uint32_t>(std::abs(right));
  }
  const size_t selected = rightEnergy > leftEnergy ? 1 : 0;
  auto *mono = reinterpret_cast<int16_t *>(destination);
  uint64_t squared = 0;
  for (size_t index = 0; index < frames; ++index) {
    const int16_t sample = impl_->stereo[index * 2 + selected];
    mono[index] = sample;
    const int32_t wide = sample;
    squared += static_cast<uint64_t>(wide * wide);
  }
  length = frames * sizeof(int16_t);
  impl_->lastRms = static_cast<uint16_t>(
      std::min<uint32_t>(65535, static_cast<uint32_t>(
                                    std::sqrt(squared / frames))));
  return true;
}

void AudioInput::stop() {
  if (impl_)
    impl_->shutdownTransport();
}

bool AudioInput::active() const {
  return available() && impl_->capturing && impl_->rx;
}

uint16_t AudioInput::rms() const { return available() ? impl_->lastRms : 0; }

} // namespace charadock::rlcd
