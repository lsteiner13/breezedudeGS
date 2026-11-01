#include "power_management.h"

#include <WiFi.h>
#include "esp_wifi.h"
#include <esp_sleep.h>
#include <esp_bt.h>
#include <time.h>

#include "helper.h"
#include "ina219.h"

extern Settings settings;
extern Aprs aprs;
extern PhysicalLayer* radio_phy;
extern volatile bool otaUploadInProgress;
extern const char* ntpServer;
extern void webconsole_print(String in);

static constexpr uint8_t BATTERY_ADC_SAMPLES = 12;
static constexpr uint32_t BATTERY_MEASUREMENT_INTERVAL_MS = 15000UL;
static constexpr float BATTERY_LOW_PASS_ALPHA = 0.18f;
static constexpr float BATTERY_DIVIDER_RATIO = 4.9f;
static constexpr float BATTERY_UNDERVOLTAGE_THRESHOLD = 3.1f; // Volt
static constexpr uint64_t LOW_BATTERY_SLEEP_SECONDS = 12ULL * 60ULL * 60ULL; // 12 hours
static constexpr uint8_t PIN_ADC_CTRL = 37;
static constexpr uint8_t PIN_ADC_IN = 1;

static float filteredBatteryVoltage = NAN;
static float lastBatteryVoltage = NAN;
static uint32_t lastBatteryMeasurementMs = 0;
static bool batteryMeasurementSettled = false;
static uint32_t lastBatterySleepCheckMs = 0;

// ── INA219 power monitor ─────────────────────────────────────────────────────
static constexpr uint32_t INA_SAMPLE_INTERVAL_MS = 5000UL;
static constexpr uint32_t INA_RETRY_INTERVAL_MS = 30000UL;
static constexpr uint32_t INA_FIRST_SAMPLE_DELAY_MS = 300UL; // chip needs ~140 ms for its first averaged conversion
static constexpr uint8_t INA_MAX_FAILURES = 3;
static constexpr float INA_MIN_VOLTAGE = 2.0f;
static constexpr float INA_MAX_VOLTAGE = 26.0f;
static constexpr float INA_VOLTAGE_ALPHA = 0.07f;   // ~70 s time constant at 5 s sampling
static constexpr float INA_CURRENT_ALPHA = 0.0167f; // ~5 min time constant = APRS status interval

static bool inaInitialised = false;  // chip answered and is configured
static bool inaActive = false;       // last reading was valid -> replaces the ADC measurement
static uint8_t inaFailures = 0;
static uint32_t inaLastInitAttemptMs = 0;
static uint32_t inaNextSampleMs = 0;
static float inaBusVoltage = NAN;    // last raw bus voltage
static float inaVoltageAvg = NAN;
static float inaCurrentAvg = NAN;
static float inaPowerAvg = NAN;

static void clearInaValues() {
  inaActive = false;
  inaFailures = 0;
  inaBusVoltage = NAN;
  inaVoltageAvg = NAN;
  inaCurrentAvg = NAN;
  inaPowerAvg = NAN;
  aprs.setIna219Data(NAN, NAN, NAN);
}

static void updateIna219Measurement() {
  if (!settings.ina219Enabled) {
    if (inaInitialised) {
      ina219PowerDown(settings.ina219Sda, settings.ina219Scl); // also stops I2C
      inaInitialised = false;
    }
    if (inaActive || !isnan(inaVoltageAvg)) {
      clearInaValues();
    }
    return;
  }

  const uint32_t now = millis();

  if (!inaInitialised) {
    if (inaLastInitAttemptMs != 0 && (now - inaLastInitAttemptMs) < INA_RETRY_INTERVAL_MS) {
      return;
    }
    inaLastInitAttemptMs = now ? now : 1;
    if (!ina219Begin(settings.ina219Sda, settings.ina219Scl)) {
      Serial.printf("INA219 not found on SDA=%u SCL=%u\n", (unsigned)settings.ina219Sda, (unsigned)settings.ina219Scl);
      return;
    }
    Serial.printf("INA219 ready on SDA=%u SCL=%u\n", (unsigned)settings.ina219Sda, (unsigned)settings.ina219Scl);
    inaInitialised = true;
    inaFailures = 0;
    inaNextSampleMs = now + INA_FIRST_SAMPLE_DELAY_MS;
    return;
  }

  if ((int32_t)(now - inaNextSampleMs) < 0) {
    return;
  }
  inaNextSampleMs = now + INA_SAMPLE_INTERVAL_MS;

  float voltage = NAN;
  float current = NAN;
  if (ina219Read(&voltage, &current) && voltage >= INA_MIN_VOLTAGE && voltage <= INA_MAX_VOLTAGE) {
    const float power = voltage * current;
    inaFailures = 0;
    inaBusVoltage = voltage;
    if (isnan(inaVoltageAvg) || isnan(inaCurrentAvg) || isnan(inaPowerAvg)) {
      inaVoltageAvg = voltage;
      inaCurrentAvg = current;
      inaPowerAvg = power;
    } else {
      inaVoltageAvg += INA_VOLTAGE_ALPHA * (voltage - inaVoltageAvg);
      inaCurrentAvg += INA_CURRENT_ALPHA * (current - inaCurrentAvg);
      inaPowerAvg += INA_CURRENT_ALPHA * (power - inaPowerAvg);
    }
    inaActive = true;
    aprs.setIna219Data(inaVoltageAvg, inaCurrentAvg, inaPowerAvg);
    return;
  }

  if (++inaFailures >= INA_MAX_FAILURES) {
    Serial.println("INA219: no valid readings, falling back to ADC and retrying later");
    ina219End();
    inaInitialised = false;
    inaLastInitAttemptMs = now ? now : 1;
    clearInaValues();
  }
}

static void configureTimeZoneFromLocationImpl() {
  char tzBuffer[40];
  bool inEurope = settings.latitude >= 35.0f && settings.latitude <= 72.0f &&
                  settings.longitude >= -10.0f && settings.longitude <= 40.0f;

  if (inEurope) {
    int roundedOffset = (int)lroundf(settings.longitude / 15.0f);
    if (roundedOffset < 0) {
      roundedOffset = 0;
    }
    if (roundedOffset > 3) {
      roundedOffset = 3;
    }
    snprintf(tzBuffer, sizeof(tzBuffer), "LOC%+dLOCST,M3.5.0/2,M10.5.0/3", -roundedOffset);
  } else {
    int roundedOffset = (int)lroundf(settings.longitude / 15.0f);
    if (roundedOffset > 12) {
      roundedOffset = 12;
    }
    if (roundedOffset < -12) {
      roundedOffset = -12;
    }
    snprintf(tzBuffer, sizeof(tzBuffer), "UTC%+d", -roundedOffset);
  }

  configTzTime(tzBuffer, ntpServer);
}

static void disableBluetoothForPowerSaveImpl() {
  esp_bt_controller_status_t btStatus = esp_bt_controller_get_status();

  if (btStatus == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    esp_err_t err = esp_bt_controller_disable();
    if (err != ESP_OK) {
      Serial.printf("BT disable failed: %d\n", (int)err);
    }
    btStatus = esp_bt_controller_get_status();
  }

  if (btStatus == ESP_BT_CONTROLLER_STATUS_INITED) {
    esp_err_t err = esp_bt_controller_deinit();
    if (err != ESP_OK) {
      Serial.printf("BT deinit failed: %d\n", (int)err);
    }
  }

  esp_err_t rel = esp_bt_mem_release(ESP_BT_MODE_BTDM);
  if (rel != ESP_OK && rel != ESP_ERR_INVALID_STATE) {
    Serial.printf("BT mem release failed: %d\n", (int)rel);
  }
}

static float readBatteryMeasurement() {
//delte me later
pinMode(36, OUTPUT);
  digitalWrite(36, LOW);

  if (!settings.batteryPowered) {
    digitalWrite(PIN_ADC_CTRL, HIGH);
    lastBatteryVoltage = NAN;
    filteredBatteryVoltage = NAN;
    batteryMeasurementSettled = false;
    return NAN;
  }

  float measuredVoltage;
  if (inaActive && !isnan(inaBusVoltage)) {
    // INA219 replaces the ADC voltage divider while it delivers valid readings.
    measuredVoltage = inaBusVoltage;
  } else {
    digitalWrite(PIN_ADC_CTRL, LOW);
    delay(4);

    uint32_t millivoltsSum = 0;
    for (uint8_t i = 0; i < BATTERY_ADC_SAMPLES; i++) {
      millivoltsSum += (uint32_t)analogReadMilliVolts(PIN_ADC_IN);
      delay(2);
    }

    digitalWrite(PIN_ADC_CTRL, HIGH);

    float averageMillivolts = (float)millivoltsSum / (float)BATTERY_ADC_SAMPLES;
    measuredVoltage = (averageMillivolts / 1000.0f) * BATTERY_DIVIDER_RATIO;
    if (measuredVoltage < 2.0f || measuredVoltage > 5.5f) {
      return NAN;
    }
  }

  lastBatteryVoltage = measuredVoltage;
  if (!batteryMeasurementSettled || isnan(filteredBatteryVoltage)) {
    filteredBatteryVoltage = measuredVoltage;
    batteryMeasurementSettled = true;
  } else {
    filteredBatteryVoltage += BATTERY_LOW_PASS_ALPHA * (measuredVoltage - filteredBatteryVoltage);
  }

  aprs.setBattVoltage(filteredBatteryVoltage);
  return filteredBatteryVoltage;
}

static void updateBatteryMeasurement() {
  if (!settings.batteryPowered) {
    if (!isnan(filteredBatteryVoltage)) {
      filteredBatteryVoltage = NAN;
      lastBatteryVoltage = NAN;
      batteryMeasurementSettled = false;
      aprs.setBattVoltage(NAN);
    }
    return;
  }

  if (lastBatteryMeasurementMs != 0 && (millis() - lastBatteryMeasurementMs) < BATTERY_MEASUREMENT_INTERVAL_MS) {
    return;
  }

  lastBatteryMeasurementMs = millis();
  readBatteryMeasurement();
  Serial.printf("Battery voltage: %.2f V\n", filteredBatteryVoltage);
}

static String formatHm(uint16_t totalMinutes) {
  char buffer[6];
  uint16_t normalized = totalMinutes % (24 * 60);
  snprintf(buffer, sizeof(buffer), "%02u:%02u", normalized / 60, normalized % 60);
  return String(buffer);
}

static bool isWithinScheduledSleepWindow(uint16_t nowMinutes, uint16_t offMinutes, uint16_t onMinutes) {
  if (offMinutes == onMinutes) {
    return false;
  }
  if (offMinutes < onMinutes) {
    return nowMinutes >= offMinutes && nowMinutes < onMinutes;
  }
  return nowMinutes >= offMinutes || nowMinutes < onMinutes;
}

static uint64_t computeSecondsUntilWake(uint16_t wakeMinutes, const struct tm& localTime, time_t now) {
  struct tm wakeTime = localTime;
  wakeTime.tm_sec = 0;
  wakeTime.tm_min = wakeMinutes % 60;
  wakeTime.tm_hour = wakeMinutes / 60;

  uint16_t nowMinutes = (uint16_t)(localTime.tm_hour * 60 + localTime.tm_min);
  if (wakeMinutes <= nowMinutes) {
    wakeTime.tm_mday += 1;
  }

  time_t wakeEpoch = mktime(&wakeTime);
  if (wakeEpoch <= now) {
    wakeEpoch = now + 60;
  }
  return (uint64_t)(wakeEpoch - now);
}

static void enterDeepSleepForSeconds(uint64_t sleepSeconds, const String& reason) {
  if (sleepSeconds == 0) {
    return;
  }

  if (settings.sendAPRS && aprs.connected()) {
    String note = reason + " " + String((unsigned long)(sleepSeconds / 60ULL)) + "min";
    if (!aprs.sendSystemStatus(note)) {
      Serial.println("APRS sleep status could not be sent");
    }
  }

  webconsole_print("Entering deep sleep: " + reason + " for " + String((unsigned long)(sleepSeconds / 60ULL)) + " min");
  delay(150);

  if (radio_phy != nullptr) {
    radio_phy->sleep();
    radio_phy = nullptr;
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  if (settings.ina219Enabled) {
    // The INA219 stays powered during deep sleep: put it into power-down (~6 uA instead of ~0.7 mA).
    // ina219Begin() wakes it again after the restart.
    ina219PowerDown(settings.ina219Sda, settings.ina219Scl);
  }
  esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
  esp_deep_sleep_start();
}

void initPowerManagementHardware() {
  pinMode(PIN_ADC_IN, INPUT);
  pinMode(PIN_ADC_CTRL, OUTPUT);
  adcAttachPin(PIN_ADC_IN);
  analogReadResolution(12);

  setCpuFrequencyMhz(80); // power saving, LoRa and WiFi can work fine at 80MHz
  disableBluetoothForPowerSaveImpl();
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

void disableBluetoothForPowerSave() {
  disableBluetoothForPowerSaveImpl();
}

void configureTimeZoneFromLocation() {
  configureTimeZoneFromLocationImpl();
}

float getBatteryVoltage() {
  return filteredBatteryVoltage;
}

bool getIna219Values(float* voltage, float* current, float* power) {
  if (!inaActive || isnan(inaVoltageAvg) || isnan(inaCurrentAvg) || isnan(inaPowerAvg)) {
    return false;
  }
  *voltage = inaVoltageAvg;
  *current = inaCurrentAvg;
  *power = inaPowerAvg;
  return true;
}

void reconfigureIna219() {
  if (inaInitialised) {
    ina219PowerDown(settings.ina219Sda, settings.ina219Scl); // I2C is running, so the pins are not used here
    inaInitialised = false;
  }
  inaLastInitAttemptMs = 0; // retry immediately with the new settings
  clearInaValues();
}

void handleBatteryAndSleep() {
  if (otaUploadInProgress) {
    return;
  }

  updateIna219Measurement(); // independent of the battery mode: also useful on USB / solar supply

  if (!settings.batteryPowered) {
    return;
  }

  updateBatteryMeasurement();
  if (!isnan(filteredBatteryVoltage) && filteredBatteryVoltage < BATTERY_UNDERVOLTAGE_THRESHOLD) {
    char reason[24];
    snprintf(reason, sizeof(reason), "LOWBAT:%.2fV", filteredBatteryVoltage);
    enterDeepSleepForSeconds(LOW_BATTERY_SLEEP_SECONDS, String(reason));
    return;
  }

  if (!settings.sleepScheduleEnabled) {
    return;
  }

  if (lastBatterySleepCheckMs != 0 && (millis() - lastBatterySleepCheckMs) < 10000UL) {
    return;
  }
  lastBatterySleepCheckMs = millis();

  time_t now = time(nullptr);
  if (now < 100000) {
    return;
  }

  struct tm localTime;
  localtime_r(&now, &localTime);
  uint16_t nowMinutes = (uint16_t)(localTime.tm_hour * 60 + localTime.tm_min);
  if (!isWithinScheduledSleepWindow(nowMinutes, settings.sleepOffMinutes, settings.sleepOnMinutes)) {
    return;
  }

  uint64_t sleepSeconds = computeSecondsUntilWake(settings.sleepOnMinutes, localTime, now);
  String reason = "SLEEP:" + formatHm(settings.sleepOffMinutes) + "-" + formatHm(settings.sleepOnMinutes);
  enterDeepSleepForSeconds(sleepSeconds, reason);
}

void handleLowBatteryAtStartup() {
  if (!settings.batteryPowered) {
    return;
  }

  float startupVoltage = readBatteryMeasurement();
  if (!isnan(startupVoltage)) {
    Serial.printf("Battery voltage (startup): %.2f V\n", startupVoltage);
  }

  if (!isnan(startupVoltage) && startupVoltage < BATTERY_UNDERVOLTAGE_THRESHOLD) {
    char reason[24];
    snprintf(reason, sizeof(reason), "LOWBAT:%.2fV", startupVoltage);
    Serial.printf("Startup low-battery guard active (%s) -> deep sleep\n", reason);
    enterDeepSleepForSeconds(LOW_BATTERY_SLEEP_SECONDS, String(reason));
  }
}
