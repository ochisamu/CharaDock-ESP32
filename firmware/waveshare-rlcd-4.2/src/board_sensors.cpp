// SPDX-License-Identifier: Apache-2.0
#include "charadock/board_sensors.hpp"

#include <Arduino.h>
#include <Wire.h>

#include <algorithm>
#include <ctime>

namespace charadock::rlcd {
namespace {

constexpr uint16_t kShtc3Wake = 0x3517;
constexpr uint16_t kShtc3Sleep = 0xb098;
constexpr uint16_t kShtc3MeasureTemperatureFirst = 0x7866;

uint8_t bcdToBinary(uint8_t value) {
  return static_cast<uint8_t>((value >> 4) * 10 + (value & 0x0f));
}
uint8_t binaryToBcd(uint8_t value) {
  return static_cast<uint8_t>((value / 10) << 4 | (value % 10));
}

uint8_t crc8(const uint8_t *bytes, size_t length) {
  uint8_t crc = 0xff;
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc & 0x80u) ? static_cast<uint8_t>((crc << 1) ^ 0x31u)
                          : static_cast<uint8_t>(crc << 1);
  }
  return crc;
}

bool writeCommand(uint8_t address, uint16_t command) {
  Wire.beginTransmission(address);
  Wire.write(static_cast<uint8_t>(command >> 8));
  Wire.write(static_cast<uint8_t>(command & 0xff));
  return Wire.endTransmission() == 0;
}

bool dateTimeValid(const DateTime &value) {
  return value.year >= 2020 && value.year <= 2099 && value.month >= 1 &&
         value.month <= 12 && value.day >= 1 && value.day <= 31 &&
         value.hour <= 23 && value.minute <= 59 && value.second <= 59;
}

} // namespace

bool BoardSensors::begin() {
  if (!Wire.begin(pins::kI2cSda, pins::kI2cScl, 400000))
    return false;
  snapshot_.shtc3Available = probe(i2c::kShtc3Address);
  snapshot_.rtcAvailable = probe(i2c::kRtcAddress);
  snapshot_.es8311Available = probe(i2c::kEs8311Address);
  snapshot_.es7210Available = probe(i2c::kEs7210Address);
  analogReadResolution(12);
  // Arduino-ESP32 3.x configures a channel lazily on the first read. Global
  // attenuation avoids calling the per-pin setter before that registration.
  analogSetAttenuation(ADC_11db);
  update(millis(), true);
  return true;
}

bool BoardSensors::probe(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool BoardSensors::update(uint32_t nowMs, bool force) {
  bool changed = false;
  if (force || static_cast<int32_t>(nowMs - nextEnvironmentAtMs_) >= 0) {
    float temperature = 0;
    float humidity = 0;
    const bool available = readEnvironment(temperature, humidity);
    if (available) {
      snapshot_.temperatureC = temperature;
      snapshot_.humidityPercent = humidity;
    }
    changed |= snapshot_.shtc3Available != available || available;
    snapshot_.shtc3Available = available;
    nextEnvironmentAtMs_ = nowMs + 30000;
  }
  if (force || static_cast<int32_t>(nowMs - nextClockAtMs_) >= 0) {
    const DateTime previous = snapshot_.dateTime;
    DateTime dateTime;
    const bool available = readClock(dateTime);
    if (available)
      snapshot_.dateTime = dateTime;
    changed |= snapshot_.rtcAvailable != available ||
               (available && (previous.second != dateTime.second ||
                              previous.minute != dateTime.minute ||
                              previous.hour != dateTime.hour ||
                              previous.day != dateTime.day));
    snapshot_.rtcAvailable = available;
    nextClockAtMs_ = nowMs + 1000;
  }
  if (force || static_cast<int32_t>(nowMs - nextBatteryAtMs_) >= 0) {
    const uint8_t previous = snapshot_.batteryPercent;
    readBattery(snapshot_.batteryVolts, snapshot_.batteryPercent,
                snapshot_.batteryAvailable);
    changed |= previous != snapshot_.batteryPercent;
    nextBatteryAtMs_ = nowMs + 10000;
  }
  snapshot_.sampledAtMs = nowMs;
  return changed;
}

const SensorSnapshot &BoardSensors::snapshot() const { return snapshot_; }

bool BoardSensors::readEnvironment(float &temperatureC,
                                   float &humidityPercent) {
  if (!probe(i2c::kShtc3Address) ||
      !writeCommand(i2c::kShtc3Address, kShtc3Wake))
    return false;
  delayMicroseconds(300);
  if (!writeCommand(i2c::kShtc3Address,
                    kShtc3MeasureTemperatureFirst)) {
    writeCommand(i2c::kShtc3Address, kShtc3Sleep);
    return false;
  }
  delay(20);
  const size_t received = Wire.requestFrom(i2c::kShtc3Address, 6);
  if (received != 6) {
    writeCommand(i2c::kShtc3Address, kShtc3Sleep);
    return false;
  }
  uint8_t bytes[6] = {};
  for (uint8_t &byte : bytes)
    byte = static_cast<uint8_t>(Wire.read());
  writeCommand(i2c::kShtc3Address, kShtc3Sleep);
  if (crc8(bytes, 2) != bytes[2] || crc8(bytes + 3, 2) != bytes[5])
    return false;
  const uint16_t rawTemperature =
      static_cast<uint16_t>(bytes[0]) << 8 | bytes[1];
  const uint16_t rawHumidity =
      static_cast<uint16_t>(bytes[3]) << 8 | bytes[4];
  temperatureC = -45.0f + 175.0f * rawTemperature / 65535.0f;
  humidityPercent = 100.0f * rawHumidity / 65535.0f;
  return temperatureC > -50.0f && temperatureC < 130.0f &&
         humidityPercent >= 0.0f && humidityPercent <= 100.0f;
}

bool BoardSensors::readClock(DateTime &dateTime) {
  if (!probe(i2c::kRtcAddress))
    return false;
  Wire.beginTransmission(i2c::kRtcAddress);
  Wire.write(0x04);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(i2c::kRtcAddress, 7) != 7)
    return false;
  uint8_t registers[7] = {};
  for (uint8_t &value : registers)
    value = static_cast<uint8_t>(Wire.read());
  if (registers[0] & 0x80u)
    return false;
  DateTime decoded;
  decoded.second = bcdToBinary(registers[0] & 0x7f);
  decoded.minute = bcdToBinary(registers[1] & 0x7f);
  decoded.hour = bcdToBinary(registers[2] & 0x3f);
  decoded.day = bcdToBinary(registers[3] & 0x3f);
  decoded.month = bcdToBinary(registers[5] & 0x1f);
  decoded.year = 2000 + bcdToBinary(registers[6]);
  decoded.valid = dateTimeValid(decoded);
  if (!decoded.valid)
    return false;
  dateTime = decoded;
  return true;
}

bool BoardSensors::writeClock(const DateTime &dateTime) {
  if (!dateTimeValid(dateTime) || !probe(i2c::kRtcAddress))
    return false;
  const uint8_t registers[] = {
      binaryToBcd(dateTime.second), binaryToBcd(dateTime.minute),
      binaryToBcd(dateTime.hour),   binaryToBcd(dateTime.day),
      0,                            binaryToBcd(dateTime.month),
      binaryToBcd(static_cast<uint8_t>(dateTime.year - 2000)),
  };
  Wire.beginTransmission(i2c::kRtcAddress);
  Wire.write(0x04);
  Wire.write(registers, sizeof(registers));
  return Wire.endTransmission() == 0;
}

bool BoardSensors::setUnixTime(uint64_t unixSeconds,
                               int16_t utcOffsetMinutes) {
  if (unixSeconds > 4102444799ULL)
    return false;
  const int64_t localSeconds = static_cast<int64_t>(unixSeconds) +
                               static_cast<int64_t>(utcOffsetMinutes) * 60;
  if (localSeconds < 0)
    return false;
  const time_t value = static_cast<time_t>(localSeconds);
  std::tm fields{};
  if (!gmtime_r(&value, &fields))
    return false;
  DateTime dateTime;
  dateTime.year = static_cast<uint16_t>(fields.tm_year + 1900);
  dateTime.month = static_cast<uint8_t>(fields.tm_mon + 1);
  dateTime.day = static_cast<uint8_t>(fields.tm_mday);
  dateTime.hour = static_cast<uint8_t>(fields.tm_hour);
  dateTime.minute = static_cast<uint8_t>(fields.tm_min);
  dateTime.second = static_cast<uint8_t>(fields.tm_sec);
  dateTime.valid = true;
  if (!writeClock(dateTime))
    return false;
  snapshot_.dateTime = dateTime;
  snapshot_.rtcAvailable = true;
  nextClockAtMs_ = 0;
  return true;
}

void BoardSensors::readBattery(float &volts, uint8_t &percent,
                               bool &available) {
  uint32_t millivolts = 0;
  constexpr uint8_t samples = 8;
  for (uint8_t index = 0; index < samples; ++index)
    millivolts += analogReadMilliVolts(pins::kBatteryAdc);
  volts = (millivolts / static_cast<float>(samples)) * 0.003f;
  available = volts >= 3.0f && volts <= 4.2f;
  if (!available) {
    percent = 0;
    return;
  }
  const float bounded = std::max(3.0f, std::min(4.2f, volts));
  percent = static_cast<uint8_t>((bounded - 3.0f) * 100.0f / 1.2f + 0.5f);
}

} // namespace charadock::rlcd
