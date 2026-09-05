// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "charadock/board.hpp"
#include "charadock/frame_dispatcher.hpp"

namespace charadock::rlcd {

class BoardSensors final : public TimeSyncSink {
public:
  bool begin();
  bool update(uint32_t nowMs, bool force = false);
  const SensorSnapshot &snapshot() const;
  bool setUnixTime(uint64_t unixSeconds,
                   int16_t utcOffsetMinutes) override;

private:
  bool probe(uint8_t address);
  bool readEnvironment(float &temperatureC, float &humidityPercent);
  bool readClock(DateTime &dateTime);
  bool writeClock(const DateTime &dateTime);
  void readBattery(float &volts, uint8_t &percent, bool &available);

  SensorSnapshot snapshot_{};
  uint32_t nextEnvironmentAtMs_ = 0;
  uint32_t nextClockAtMs_ = 0;
  uint32_t nextBatteryAtMs_ = 0;
};

} // namespace charadock::rlcd
