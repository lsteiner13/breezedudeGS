#include "ina219.h"

#include <Wire.h>
#include "driver/gpio.h"

static constexpr uint8_t INA219_ADDR = 0x40;

static constexpr uint8_t REG_CONFIG = 0x00;
static constexpr uint8_t REG_SHUNT  = 0x01; // signed, 10 uV per LSB
static constexpr uint8_t REG_BUS    = 0x02; // bits 15..3 = voltage in 4 mV steps, bit 0 = math overflow

// Config register:
//   BRNG    = 1    bus range 32 V
//   PGA     = 00   shunt range +-40 mV  -> +-4 A with 0.01 ohm, 10 uV (= 1 mA) per step
//   BADC    = 1111 128 samples averaged (68 ms)
//   SADC    = 1111 128 samples averaged (68 ms)
//   MODE    = 111  shunt + bus, continuous
// Strong averaging gives a stable reading of the small currents of this station.
// If you ever need more than 4 A, set PGA to 01 (+-80 mV / 8 A) -> 0x2FFF.
static constexpr uint16_t INA219_CONFIG = 0x27FF;
// Same configuration with MODE = 000 (power-down)
static constexpr uint16_t INA219_CONFIG_POWERDOWN = INA219_CONFIG & ~0x0007;

static bool s_started = false;

static bool writeReg(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(reg);
  Wire.write((uint8_t)(value >> 8));
  Wire.write((uint8_t)(value & 0xFF));
  return Wire.endTransmission() == 0;
}

static bool readReg(uint8_t reg, uint16_t* value) {
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) {
    return false;
  }
  if (Wire.requestFrom((uint8_t)INA219_ADDR, (uint8_t)2) != 2) {
    return false;
  }
  uint16_t hi = Wire.read();
  uint16_t lo = Wire.read();
  *value = (uint16_t)((hi << 8) | lo);
  return true;
}

static bool pinUsable(int pin) {
  if (pin < 0 || !GPIO_IS_VALID_GPIO(pin)) {
    return false;
  }
#if CONFIG_IDF_TARGET_ESP32S3
  // Heltec Wireless Stick (Lite) V3 / ESP32-S3:
  //  0 boot button, 1 battery ADC, 8-14 SX1262, 19/20 USB, 26-32 flash,
  //  35 LED, 36 Vext, 37 ADC control, 43/44 UART0
  if (pin == 0 || pin == 1 || (pin >= 8 && pin <= 14) || pin == 19 || pin == 20 ||
      (pin >= 26 && pin <= 32) || pin == 35 || pin == 36 || pin == 37 || pin == 43 || pin == 44) {
    return false;
  }
#endif
  return true;
}

bool ina219ValidPins(int sda, int scl) {
  return sda != scl && pinUsable(sda) && pinUsable(scl);
}

bool ina219Begin(int sda, int scl) {
  if (s_started) {
    ina219End();
  }
  if (!ina219ValidPins(sda, scl)) {
    return false;
  }

  if (!Wire.begin(sda, scl, 400000UL)) {
    return false;
  }
  s_started = true;
  Wire.setTimeOut(20);

  uint16_t readback = 0;
  if (!writeReg(REG_CONFIG, INA219_CONFIG) ||
      !readReg(REG_CONFIG, &readback) ||
      readback != INA219_CONFIG) {
    ina219End();
    return false;
  }
  return true;
}

void ina219End() {
  if (s_started) {
    Wire.end();
    s_started = false;
  }
}

void ina219PowerDown(int sda, int scl) {
  if (!s_started) {
    if (!ina219ValidPins(sda, scl) || !Wire.begin(sda, scl, 400000UL)) {
      return;
    }
    s_started = true;
    Wire.setTimeOut(20);
  }
  writeReg(REG_CONFIG, INA219_CONFIG_POWERDOWN);
  ina219End();
}

bool ina219Read(float* busVolts, float* amps) {
  if (!s_started) {
    return false;
  }

  uint16_t rawBus = 0;
  uint16_t rawShunt = 0;
  if (!readReg(REG_BUS, &rawBus) || !readReg(REG_SHUNT, &rawShunt)) {
    return false;
  }
  if (rawBus & 0x0001) { // math overflow flag
    return false;
  }

  const uint16_t busSteps = rawBus >> 3;
  if (busSteps == 0) { // no conversion finished yet
    return false;
  }

  *busVolts = busSteps * 0.004f;
  *amps = ((int16_t)rawShunt * 0.00001f) / INA219_SHUNT_OHMS;
  return true;
}
