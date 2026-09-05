// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace charadock::rlcd {

namespace pins {
constexpr int kDisplayClock = 11;
constexpr int kDisplayMosi = 12;
constexpr int kDisplayDc = 5;
constexpr int kDisplayCs = 40;
constexpr int kDisplayReset = 41;
constexpr int kDisplayTearingEffect = 6;
constexpr int kI2cSda = 13;
constexpr int kI2cScl = 14;
constexpr int kKey = 18;
constexpr int kBoot = 0;
constexpr int kBatteryAdc = 4;
constexpr int kAudioMclk = 16;
constexpr int kAudioBclk = 9;
constexpr int kAudioWordSelect = 45;
constexpr int kAudioDataIn = 10;
constexpr int kAudioDataOut = 8;
constexpr int kSpeakerEnable = 46;
} // namespace pins

namespace i2c {
constexpr uint8_t kRtcAddress = 0x51;
constexpr uint8_t kShtc3Address = 0x70;
constexpr uint8_t kEs8311Address = 0x18;
constexpr uint8_t kEs7210Address = 0x40;
} // namespace i2c

constexpr uint16_t kDisplayWidth = 400;
constexpr uint16_t kDisplayHeight = 300;

struct DateTime {
  uint16_t year = 2000;
  uint8_t month = 1;
  uint8_t day = 1;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  bool valid = false;
};

struct SensorSnapshot {
  float temperatureC = 0.0f;
  float humidityPercent = 0.0f;
  float batteryVolts = 0.0f;
  uint8_t batteryPercent = 0;
  DateTime dateTime{};
  bool shtc3Available = false;
  bool rtcAvailable = false;
  bool batteryAvailable = false;
  bool es8311Available = false;
  bool es7210Available = false;
  uint32_t sampledAtMs = 0;
};

} // namespace charadock::rlcd
