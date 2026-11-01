#pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include "types.h"
#include "aprs.h"

void initPowerManagementHardware();
void disableBluetoothForPowerSave();
void configureTimeZoneFromLocation();
float getBatteryVoltage();
// Smoothed INA219 values (V, A, W). Returns false while the sensor is off or not delivering data.
bool getIna219Values(float* voltage, float* current, float* power);
// Call after the INA219 settings (enable / pins) changed.
void reconfigureIna219();
void handleLowBatteryAtStartup();
void handleBatteryAndSleep();
