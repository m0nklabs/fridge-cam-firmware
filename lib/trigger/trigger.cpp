#include "trigger.h"

void triggerInit() {
    pinMode(PIN_LDR, INPUT);
    analogSetAttenuation(ADC_11db);  // Full 0-3.3V range
}

int triggerReadLDR() {
    return analogRead(PIN_LDR);
}

bool triggerIsLightOn() {
    int value = triggerReadLDR();
    Serial.printf("[Trigger] LDR value: %d (threshold: %d)\n", value, LDR_THRESHOLD);
    return value >= LDR_THRESHOLD;
}

void triggerConfigureWakeup() {
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_LDR, 1);  // Wake on HIGH
}
