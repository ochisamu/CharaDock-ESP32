// SPDX-License-Identifier: Apache-2.0
//
// ES8311 register sequencing is adapted from Espressif esp_codec_dev 1.5.4
// (Apache-2.0) and Waveshare's ESP32-S3-RLCD-4.2 audio example.  See
// THIRD_PARTY_NOTICES.md.  The physical board used for bring-up requires MCLK
// before ES8311 reset/configuration; PA_EN must remain LOW until a muted zero
// DMA prefill has completed.

#include "charadock/audio_output.hpp"

#include <Arduino.h>
#include <Wire.h>

#include <algorithm>
#include <cstring>
#include <new>

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "charadock/board.hpp"

namespace charadock::rlcd {
namespace {

constexpr size_t kRingBytes = 512 * 1024;
constexpr size_t kMonoChunkBytes = 2048;
constexpr size_t kStereoScratchBytes = kMonoChunkBytes * 2;
constexpr size_t kPrebufferBytes = 8192;
constexpr uint32_t kUnknownSampleCount = 0xffffffffu;
constexpr uint32_t kMaximumKnownSamples = kRingBytes / sizeof(int16_t);
constexpr uint32_t kSupportedSampleRate = 16000;
constexpr uint8_t kEs8311MuteRegister = 0x31;
constexpr uint8_t kEs8311VolumeRegister = 0x32;
constexpr uint8_t kEs8311MuteMask = 0x60;
constexpr uint8_t kCodecVolume100 = 0xba;
constexpr uint8_t kCodecVolumeRampStart = 0x58;
constexpr uint8_t kDmaDescriptors = 8;
constexpr uint16_t kDmaFrames = 256;

enum class StreamState : uint8_t {
  Idle,
  Buffering,
  Playing,
  Drained,
  Fault,
};

gpio_num_t gpio(int pin) { return static_cast<gpio_num_t>(pin); }

} // namespace

struct AudioOutput::Impl {
  SemaphoreHandle_t mutex = nullptr;
  SemaphoreHandle_t settled = nullptr;
  TaskHandle_t task = nullptr;
  uint8_t *ring = nullptr;
  int16_t *scratch = nullptr;
  i2s_chan_handle_t tx = nullptr;

  StreamState state = StreamState::Idle;
  AudioPlaybackEvent event = AudioPlaybackEvent::None;
  size_t readOffset = 0;
  size_t writeOffset = 0;
  size_t usedBytes = 0;
  uint32_t expectedSamples = kUnknownSampleCount;
  uint32_t receivedSamples = 0;
  uint32_t underrunCount = 0;
  uint32_t sampleRate = 0;
  uint8_t mouthLevel = 0;
  bool initialized = false;
  bool endReceived = false;
  bool stopRequested = false;
  bool transportReady = false;
  bool codecConfigured = false;

  bool take(TickType_t timeout = portMAX_DELAY) const {
    return mutex && xSemaphoreTake(mutex, timeout) == pdTRUE;
  }

  void give() const {
    if (mutex)
      xSemaphoreGive(mutex);
  }

  bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
      Wire.beginTransmission(address);
      Wire.write(reg);
      Wire.write(value);
      if (Wire.endTransmission() == 0)
        return true;
      delay(2);
    }
    Serial.printf("# RLCD audio I2C write failed reg=0x%02x\r\n", reg);
    return false;
  }

  bool readRegister(uint8_t address, uint8_t reg, uint8_t &value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address, 1) != 1) {
      Serial.printf("# RLCD audio I2C read failed reg=0x%02x\r\n", reg);
      return false;
    }
    value = static_cast<uint8_t>(Wire.read());
    return true;
  }

  bool codecWrite(uint8_t reg, uint8_t value) {
    return writeRegister(i2c::kEs8311Address, reg, value);
  }

  bool codecRead(uint8_t reg, uint8_t &value) {
    return readRegister(i2c::kEs8311Address, reg, value);
  }

  bool codecWriteVerified(uint8_t reg, uint8_t value) {
    uint8_t actual = 0;
    if (!codecWrite(reg, value) || !codecRead(reg, actual) || actual != value) {
      Serial.printf("# RLCD audio codec verify failed reg=0x%02x actual=0x%02x expected=0x%02x\r\n",
                    reg, actual, value);
      return false;
    }
    return true;
  }

  bool setCodecMute(bool muted) {
    uint8_t value = 0;
    if (!codecRead(kEs8311MuteRegister, value))
      return false;
    value = static_cast<uint8_t>(value & ~kEs8311MuteMask);
    if (muted)
      value = static_cast<uint8_t>(value | kEs8311MuteMask);
    if (!codecWrite(kEs8311MuteRegister, value))
      return false;
    uint8_t actual = 0;
    return codecRead(kEs8311MuteRegister, actual) &&
           (actual & kEs8311MuteMask) ==
               (muted ? kEs8311MuteMask : 0);
  }

  bool forceAmplifierLow() {
    const gpio_num_t pin = gpio(pins::kSpeakerEnable);
    if (gpio_set_level(pin, 0) != ESP_OK)
      return false;
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << pins::kSpeakerEnable;
    config.mode = GPIO_MODE_INPUT_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config) == ESP_OK && gpio_set_level(pin, 0) == ESP_OK &&
           gpio_get_level(pin) == 0;
  }

  bool setAmplifier(bool enabled) {
    const gpio_num_t pin = gpio(pins::kSpeakerEnable);
    const int level = enabled ? 1 : 0;
    if (gpio_set_level(pin, level) != ESP_OK || gpio_get_level(pin) != level) {
      gpio_set_level(pin, 0);
      return false;
    }
    return true;
  }

  bool configureCodec(uint32_t rate) {
    if (rate != kSupportedSampleRate)
      return false;

    // Open/reset sequence from esp_codec_dev's ES8311 driver.  MCLK is
    // already running here and PA_EN is still LOW.
    const uint8_t initial[][2] = {
        {0x44, 0x08}, {0x44, 0x08}, {0x01, 0x30}, {0x02, 0x00},
        {0x03, 0x10}, {0x16, 0x24}, {0x04, 0x10}, {0x05, 0x00},
        {0x0b, 0x00}, {0x0c, 0x00}, {0x10, 0x1f}, {0x11, 0x7f},
        {0x00, 0x80},
    };
    for (const auto &item : initial) {
      if (!codecWrite(item[0], item[1]))
        return false;
    }

    uint8_t value = 0;
    if (!codecRead(0x00, value) ||
        !codecWrite(0x00, static_cast<uint8_t>(value & 0xbf)) ||
        !codecWrite(0x01, 0x3f) || !codecRead(0x06, value) ||
        !codecWrite(0x06, static_cast<uint8_t>(value & ~0x20)) ||
        !codecWrite(0x13, 0x10) || !codecWrite(0x1b, 0x0a) ||
        !codecWrite(0x1c, 0x6a) || !codecWrite(0x44, 0x58))
      return false;

    // 16-bit Philips I2S, stereo slots.  The mono host stream is duplicated
    // into both slots immediately before each DMA write.
    uint8_t dacInterface = 0;
    uint8_t adcInterface = 0;
    if (!codecRead(0x09, dacInterface) || !codecRead(0x0a, adcInterface))
      return false;
    dacInterface = static_cast<uint8_t>((dacInterface | 0x0c) & 0xfc);
    adcInterface = static_cast<uint8_t>((adcInterface | 0x0c) & 0xfc);
    if (!codecWrite(0x09, dacInterface) || !codecWrite(0x0a, adcInterface))
      return false;

    // 4.096 MHz MCLK = 16 kHz * 256.  Coefficients match the pinned
    // esp_codec_dev table verified on the physical RLCD 4.2 audio path.
    if (!codecRead(0x02, value) ||
        !codecWrite(0x02, static_cast<uint8_t>(value & 0x07)) ||
        !codecWrite(0x05, 0x00) || !codecRead(0x03, value) ||
        !codecWrite(0x03, static_cast<uint8_t>((value & 0x80) | 0x10)) ||
        !codecRead(0x04, value) ||
        !codecWrite(0x04, static_cast<uint8_t>((value & 0x80) | 0x20)) ||
        !codecRead(0x07, value) ||
        !codecWrite(0x07, static_cast<uint8_t>(value & 0xc0)) ||
        !codecWrite(0x08, 0xff) || !codecRead(0x06, value) ||
        !codecWrite(0x06, static_cast<uint8_t>((value & 0xe0) | 0x03)))
      return false;

    const uint8_t start[][2] = {
        {0x00, 0x80}, {0x01, 0x3f}, {0x17, 0xbf}, {0x0e, 0x02},
        {0x12, 0x00}, {0x14, 0x1a}, {0x0d, 0x01}, {0x15, 0x40},
        {0x37, 0x08}, {0x45, 0x00},
    };
    for (const auto &item : start) {
      if (!codecWrite(item[0], item[1]))
        return false;
    }
    if (!codecRead(0x14, value) ||
        !codecWrite(0x14, static_cast<uint8_t>(value & ~0x40)))
      return false;

    if (!codecWriteVerified(kEs8311VolumeRegister, 0x00) ||
        !setCodecMute(true))
      return false;
    codecConfigured = true;
    return true;
  }

  bool writeStereo(const int16_t *samples, size_t bytes,
                   TickType_t timeout = pdMS_TO_TICKS(250)) {
    if (!tx || !samples || !bytes)
      return false;
    size_t written = 0;
    return i2s_channel_write(tx, samples, bytes, &written, timeout) == ESP_OK &&
           written == bytes;
  }

  bool prefillSilence(uint8_t blocks = kDmaDescriptors) {
    if (!scratch)
      return false;
    std::memset(scratch, 0, kStereoScratchBytes);
    const size_t bytes = kDmaFrames * 2 * sizeof(int16_t);
    for (uint8_t index = 0; index < blocks; ++index) {
      if (!writeStereo(scratch, bytes))
        return false;
    }
    return true;
  }

  bool configureTransport(uint32_t rate) {
    if (!forceAmplifierLow()) {
      Serial.println("# RLCD audio refused: PA_EN low-first check failed");
      return false;
    }
    Wire.beginTransmission(i2c::kEs8311Address);
    if (Wire.endTransmission() != 0) {
      Serial.println("# RLCD audio refused: ES8311 was not found at 0x18");
      return false;
    }

    i2s_chan_config_t channel =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = kDmaDescriptors;
    channel.dma_frame_num = kDmaFrames;
    channel.auto_clear = true;
    if (i2s_new_channel(&channel, &tx, nullptr) != ESP_OK)
      return false;

    i2s_std_config_t config{};
    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk = gpio(pins::kAudioMclk);
    config.gpio_cfg.bclk = gpio(pins::kAudioBclk);
    config.gpio_cfg.ws = gpio(pins::kAudioWordSelect);
    config.gpio_cfg.dout = gpio(pins::kAudioDataOut);
    config.gpio_cfg.din = I2S_GPIO_UNUSED;
    config.gpio_cfg.invert_flags.mclk_inv = false;
    config.gpio_cfg.invert_flags.bclk_inv = false;
    config.gpio_cfg.invert_flags.ws_inv = false;
    if (i2s_channel_init_std_mode(tx, &config) != ESP_OK ||
        i2s_channel_enable(tx) != ESP_OK) {
      Serial.println("# RLCD audio refused: I2S initialization failed");
      return false;
    }
    delay(1); // ES8311 must see MCLK before its reset/config writes.

    if (!configureCodec(rate) || !prefillSilence() || !setAmplifier(true)) {
      Serial.println("# RLCD audio refused: codec/zero-prefill/PA check failed");
      return false;
    }

    // Keep the amplifier at a proven board setting and the host PCM at unity
    // gain.  Software boosting sounded distorted on the physical speaker.
    for (uint16_t volume = kCodecVolumeRampStart;
         volume <= kCodecVolume100; volume += 2) {
      if (!codecWriteVerified(kEs8311VolumeRegister,
                              static_cast<uint8_t>(volume)))
        return false;
      delay(2);
    }
    if (!setCodecMute(false))
      return false;
    transportReady = true;
    Serial.printf("# RLCD audio ready rate=%lu volume=100 PCM=unity prebuffer=%u\r\n",
                  static_cast<unsigned long>(rate),
                  static_cast<unsigned>(kPrebufferBytes));
    return true;
  }

  bool shutdownTransport() {
    bool ok = true;
    if (tx && transportReady)
      ok = prefillSilence() && ok;
    if (codecConfigured) {
      for (int volume = kCodecVolume100; volume >= 0; volume -= 4) {
        if (!codecWrite(kEs8311VolumeRegister,
                        static_cast<uint8_t>(volume)))
          ok = false;
        delay(1);
      }
      ok = codecWriteVerified(kEs8311VolumeRegister, 0x00) && ok;
      ok = setCodecMute(true) && ok;
    }
    ok = setAmplifier(false) && ok;

    if (codecConfigured) {
      const uint8_t suspend[][2] = {
          {0x32, 0x00}, {0x17, 0x00}, {0x0e, 0xff}, {0x12, 0x02},
          {0x14, 0x00}, {0x0d, 0xfa}, {0x15, 0x00}, {0x02, 0x10},
          {0x00, 0x00}, {0x00, 0x1f}, {0x01, 0x30}, {0x01, 0x00},
          {0x45, 0x00}, {0x0d, 0xfc}, {0x02, 0x00},
      };
      for (const auto &item : suspend)
        ok = codecWrite(item[0], item[1]) && ok;
    }
    codecConfigured = false;
    transportReady = false;
    if (tx) {
      const esp_err_t disabled = i2s_channel_disable(tx);
      if (disabled != ESP_OK && disabled != ESP_ERR_INVALID_STATE)
        ok = false;
      if (i2s_del_channel(tx) != ESP_OK)
        ok = false;
      tx = nullptr;
    }
    if (gpio_get_level(gpio(pins::kSpeakerEnable)) != 0) {
      gpio_set_level(gpio(pins::kSpeakerEnable), 0);
      ok = false;
    }
    return ok;
  }

  void copyFromRing(uint8_t *destination, size_t length) {
    const size_t first = std::min(length, kRingBytes - readOffset);
    std::memcpy(destination, ring + readOffset, first);
    if (length > first)
      std::memcpy(destination + first, ring, length - first);
    readOffset = (readOffset + length) % kRingBytes;
    usedBytes -= length;
  }

  void markSettled(StreamState nextState, AudioPlaybackEvent nextEvent) {
    state = nextState;
    event = nextEvent;
    xSemaphoreGive(settled);
  }

  void run() {
    for (;;) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      for (;;) {
        if (!take(pdMS_TO_TICKS(100)))
          break;
        if (stopRequested) {
          stopRequested = false;
          markSettled(StreamState::Idle, AudioPlaybackEvent::None);
          give();
          break;
        }
        if (state == StreamState::Buffering) {
          const bool knownComplete =
              expectedSamples != kUnknownSampleCount && endReceived &&
              receivedSamples == expectedSamples;
          const bool streamingReady =
              expectedSamples == kUnknownSampleCount &&
              (usedBytes >= kPrebufferBytes || (endReceived && usedBytes > 0));
          if (knownComplete || streamingReady) {
            state = StreamState::Playing;
          } else if (endReceived && usedBytes == 0) {
            markSettled(StreamState::Drained, AudioPlaybackEvent::Completed);
            give();
            break;
          } else {
            give();
            break;
          }
        }
        if (state != StreamState::Playing) {
          give();
          break;
        }
        if (!usedBytes) {
          if (endReceived) {
            markSettled(StreamState::Drained, AudioPlaybackEvent::Completed);
          } else {
            ++underrunCount;
            state = StreamState::Buffering;
          }
          give();
          break;
        }

        const size_t monoBytes = std::min(usedBytes, kMonoChunkBytes) & ~1u;
        copyFromRing(reinterpret_cast<uint8_t *>(scratch), monoBytes);
        const size_t samples = monoBytes / sizeof(int16_t);
        uint16_t peak = 0;
        for (size_t index = 0; index < samples; ++index) {
          const int32_t value = scratch[index];
          peak = std::max<uint16_t>(peak, static_cast<uint16_t>(
              std::min<int32_t>(32767, std::abs(value))));
        }
        mouthLevel = peak >= 5200 ? 2 : peak >= 900 ? 1 : 0;
        give();

        for (size_t index = samples; index-- > 0;) {
          const int16_t value = scratch[index];
          scratch[index * 2] = value;
          scratch[index * 2 + 1] = value;
        }
        if (!writeStereo(scratch, samples * 2 * sizeof(int16_t),
                         pdMS_TO_TICKS(500))) {
          if (take(pdMS_TO_TICKS(100))) {
            markSettled(StreamState::Fault, AudioPlaybackEvent::Failed);
            give();
          }
          break;
        }
      }
    }
  }

  static void taskEntry(void *context) {
    static_cast<Impl *>(context)->run();
  }
};

AudioOutput::AudioOutput() = default;

AudioOutput::~AudioOutput() {
  if (!impl_)
    return;
  stopPlayback();
  if (impl_->task)
    vTaskDelete(impl_->task);
  if (impl_->mutex)
    vSemaphoreDelete(impl_->mutex);
  if (impl_->settled)
    vSemaphoreDelete(impl_->settled);
  if (impl_->ring)
    heap_caps_free(impl_->ring);
  if (impl_->scratch)
    heap_caps_free(impl_->scratch);
  delete impl_;
}

bool AudioOutput::begin() {
  if (impl_)
    return impl_->initialized;
  impl_ = new (std::nothrow) Impl();
  if (!impl_)
    return false;
  impl_->mutex = xSemaphoreCreateMutex();
  impl_->settled = xSemaphoreCreateBinary();
  impl_->ring = static_cast<uint8_t *>(
      heap_caps_malloc(kRingBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  impl_->scratch = static_cast<int16_t *>(heap_caps_malloc(
      kStereoScratchBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!impl_->mutex || !impl_->settled || !impl_->ring || !impl_->scratch ||
      !impl_->forceAmplifierLow()) {
    Serial.println("# RLCD audio initialization failed; PA_EN remains LOW");
    return false;
  }
  if (xTaskCreatePinnedToCore(Impl::taskEntry, "rlcd_audio", 6144, impl_, 5,
                             &impl_->task, 0) != pdPASS) {
    Serial.println("# RLCD audio task allocation failed; PA_EN remains LOW");
    return false;
  }
  impl_->initialized = true;
  return true;
}

bool AudioOutput::available() const {
  return impl_ && impl_->initialized;
}

bool AudioOutput::active() const {
  if (!available() || !impl_->take(pdMS_TO_TICKS(20)))
    return false;
  const bool active = impl_->state == StreamState::Buffering ||
                      impl_->state == StreamState::Playing;
  impl_->give();
  return active;
}

uint8_t AudioOutput::mouthLevel() const {
  if (!available() || !impl_->take(pdMS_TO_TICKS(5)))
    return 0;
  const uint8_t value = impl_->mouthLevel;
  impl_->give();
  return value;
}

uint32_t AudioOutput::underruns() const {
  if (!available() || !impl_->take(pdMS_TO_TICKS(20)))
    return 0;
  const uint32_t value = impl_->underrunCount;
  impl_->give();
  return value;
}

AudioApplyResult AudioOutput::startPlayback(uint32_t sampleRate,
                                            uint32_t totalSamples) {
  if (!available())
    return AudioApplyResult::Unavailable;
  if (sampleRate != kSupportedSampleRate || totalSamples == 0 ||
      (totalSamples != kUnknownSampleCount &&
       totalSamples > kMaximumKnownSamples))
    return AudioApplyResult::InvalidFormat;

  const AudioApplyResult stopped = stopPlayback();
  if (stopped == AudioApplyResult::CodecFailure)
    return stopped;
  while (xSemaphoreTake(impl_->settled, 0) == pdTRUE) {
  }
  if (!impl_->configureTransport(sampleRate)) {
    impl_->shutdownTransport();
    return AudioApplyResult::CodecFailure;
  }

  if (!impl_->take(pdMS_TO_TICKS(100))) {
    impl_->shutdownTransport();
    return AudioApplyResult::InvalidState;
  }
  impl_->readOffset = 0;
  impl_->writeOffset = 0;
  impl_->usedBytes = 0;
  impl_->expectedSamples = totalSamples;
  impl_->receivedSamples = 0;
  impl_->underrunCount = 0;
  impl_->mouthLevel = 0;
  impl_->sampleRate = sampleRate;
  impl_->endReceived = false;
  impl_->stopRequested = false;
  impl_->event = AudioPlaybackEvent::None;
  impl_->state = StreamState::Buffering;
  impl_->give();
  return AudioApplyResult::Ok;
}

AudioApplyResult AudioOutput::writePcm16(const uint8_t *bytes,
                                         size_t length) {
  if (!available())
    return AudioApplyResult::Unavailable;
  if (!bytes || !length || (length & 1u) || length > 4096)
    return AudioApplyResult::InvalidFormat;
  if (!impl_->take(pdMS_TO_TICKS(100)))
    return AudioApplyResult::InvalidState;
  if ((impl_->state != StreamState::Buffering &&
       impl_->state != StreamState::Playing) ||
      impl_->endReceived) {
    Serial.printf("# RLCD audio chunk rejected state=%u end=%u received=%lu expected=%lu used=%u\r\n",
                  static_cast<unsigned>(impl_->state),
                  impl_->endReceived ? 1u : 0u,
                  static_cast<unsigned long>(impl_->receivedSamples),
                  static_cast<unsigned long>(impl_->expectedSamples),
                  static_cast<unsigned>(impl_->usedBytes));
    impl_->give();
    return AudioApplyResult::InvalidState;
  }
  const uint32_t samples = static_cast<uint32_t>(length / sizeof(int16_t));
  if (impl_->expectedSamples != kUnknownSampleCount &&
      (samples > impl_->expectedSamples - impl_->receivedSamples)) {
    impl_->give();
    return AudioApplyResult::InvalidFormat;
  }
  if (length > kRingBytes - impl_->usedBytes) {
    impl_->give();
    return AudioApplyResult::BufferFull;
  }
  const size_t first = std::min(length, kRingBytes - impl_->writeOffset);
  std::memcpy(impl_->ring + impl_->writeOffset, bytes, first);
  if (length > first)
    std::memcpy(impl_->ring, bytes + first, length - first);
  impl_->writeOffset = (impl_->writeOffset + length) % kRingBytes;
  impl_->usedBytes += length;
  impl_->receivedSamples += samples;
  impl_->give();
  xTaskNotifyGive(impl_->task);
  return AudioApplyResult::Ok;
}

AudioApplyResult AudioOutput::finishPlayback() {
  if (!available())
    return AudioApplyResult::Unavailable;
  if (!impl_->take(pdMS_TO_TICKS(100)))
    return AudioApplyResult::InvalidState;
  if (impl_->state != StreamState::Buffering &&
      impl_->state != StreamState::Playing) {
    impl_->give();
    return AudioApplyResult::InvalidState;
  }
  if (impl_->expectedSamples != kUnknownSampleCount &&
      impl_->receivedSamples != impl_->expectedSamples) {
    impl_->give();
    stopPlayback();
    return AudioApplyResult::InvalidFormat;
  }
  impl_->endReceived = true;
  impl_->give();
  xTaskNotifyGive(impl_->task);
  return AudioApplyResult::Ok;
}

AudioApplyResult AudioOutput::stopPlayback() {
  if (!available())
    return AudioApplyResult::Unavailable;
  bool waitForTask = false;
  if (impl_->take(pdMS_TO_TICKS(100))) {
    waitForTask = impl_->state == StreamState::Buffering ||
                  impl_->state == StreamState::Playing;
    if (waitForTask)
      impl_->stopRequested = true;
    impl_->event = AudioPlaybackEvent::None;
    impl_->give();
  }
  if (waitForTask) {
    xTaskNotifyGive(impl_->task);
    if (xSemaphoreTake(impl_->settled, pdMS_TO_TICKS(1000)) != pdTRUE) {
      impl_->forceAmplifierLow();
      return AudioApplyResult::CodecFailure;
    }
  } else {
    while (xSemaphoreTake(impl_->settled, 0) == pdTRUE) {
    }
  }
  const bool ok = impl_->shutdownTransport();
  if (impl_->take(pdMS_TO_TICKS(100))) {
    impl_->state = StreamState::Idle;
    impl_->readOffset = 0;
    impl_->writeOffset = 0;
    impl_->usedBytes = 0;
    impl_->endReceived = false;
    impl_->stopRequested = false;
    impl_->mouthLevel = 0;
    impl_->give();
  }
  return ok ? AudioApplyResult::Ok : AudioApplyResult::CodecFailure;
}

AudioPlaybackEvent AudioOutput::pollEvent() {
  if (!available() || !impl_->take(pdMS_TO_TICKS(20)))
    return AudioPlaybackEvent::None;
  AudioPlaybackEvent event = impl_->event;
  if (event == AudioPlaybackEvent::None) {
    impl_->give();
    return event;
  }
  impl_->event = AudioPlaybackEvent::None;
  impl_->give();
  while (xSemaphoreTake(impl_->settled, 0) == pdTRUE) {
  }
  if (!impl_->shutdownTransport())
    event = AudioPlaybackEvent::Failed;
  if (impl_->take(pdMS_TO_TICKS(100))) {
    impl_->state = StreamState::Idle;
    impl_->mouthLevel = 0;
    impl_->give();
  }
  Serial.printf("# RLCD audio %s underruns=%lu samples=%lu\r\n",
                event == AudioPlaybackEvent::Completed ? "completed" : "failed",
                static_cast<unsigned long>(impl_->underrunCount),
                static_cast<unsigned long>(impl_->receivedSamples));
  return event;
}

} // namespace charadock::rlcd
