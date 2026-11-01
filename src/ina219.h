#pragma once

#include <Arduino.h>

// Minimal INA219 driver (I2C address 0x40, A0/A1 = GND).
//
// Wiring: shunt in the supply line, VIN+ on the supply/battery side, VIN- on the
// board side. Positive current = flow from supply into the board, negative current
// = flow into the supply (e.g. charging a battery that sits behind the shunt).
//
// Registers are read directly (shunt voltage + bus voltage) and converted in float,
// so the INA219 calibration register is not needed.

// Value of the shunt resistor in ohms.
constexpr float INA219_SHUNT_OHMS = 0.1f;

// Checks that both GPIOs are usable for I2C (valid for this chip and not used by
// radio, LED, button, battery ADC, USB, flash or UART0).
bool ina219ValidPins(int sda, int scl);

// Starts I2C on the given pins and configures the chip.
// Returns false when no INA219 answers on the bus.
bool ina219Begin(int sda, int scl);

// Stops I2C and releases the pins.
void ina219End();

// Puts the chip into power-down mode (~6 uA instead of ~0.7 mA) and stops I2C.
// Works whether or not ina219Begin() was called before: if I2C is not running, it is
// started briefly on the given pins. Call before deep sleep or when the sensor is
// disabled. ina219Begin() wakes the chip again.
void ina219PowerDown(int sda, int scl);

// Reads bus voltage (V) and current (A). Returns false on I2C error, math overflow
// or when no valid conversion is available yet.
bool ina219Read(float* busVolts, float* amps);
