#include "power.h"
#include "trigger.h"

void powerInit() {
    pinMode(PIN_BATTERY_ADC, INPUT);
}

uint16_t powerReadVoltageMV() {
    // 100k+100k divider halves the voltage
    // ESP32-S3 ADC: 12-bit (0-4095), 0-3.3V with 11dB attenuation
    int raw = analogRead(PIN_BATTERY_ADC);
    // Convert to mV: (raw / 4095) * 3300 * 2 (divider ratio)
    uint16_t mv = (uint16_t)((raw * 6600UL) / 4095);
    Serial.printf("[Power] Battery: %d raw → %u mV\n", raw, mv);
    return mv;
}

uint8_t powerVoltageToPct(uint16_t mv) {
    if (mv >= 4200) return 100;
    if (mv <= 3300) return 0;
    return (uint8_t)(((uint32_t)(mv - 3300) * 100) / 900);
}

void powerDeepSleep() {
    Serial.println("[Power] Entering deep sleep...");
    Serial.flush();
    triggerConfigureWakeup();
    esp_deep_sleep_start();
}
