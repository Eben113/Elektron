// node.cpp
#include "node.h"
#include "freertos/task.h"
#include "esp_timer.h"

Sensor::Sensor(QueueHandle_t q) : sensor(2, 50.0), queue(q) {}

void Sensor::begin() {
    sensor.setSensitivity(SENSITIVITY);
}

void Sensor::startTask() {
    xTaskCreate(taskEntry, "sensor", 4096, this, 5, nullptr);
}

void Sensor::taskEntry(void* self) {
    static_cast<Sensor*>(self)->run();
}

bool Sensor::looksDead(float v) const {
    // Hysteresis: leaving ON needs a low reading, leaving OFF needs a clearly high one.
    return (currState == State::ON) ? (v < DEAD_BELOW) : (v < LIVE_ABOVE);
}

void Sensor::run() {
    uint8_t streak = 0;
    int64_t streakStart = 0;
    State pending = State::UNKNOWN;

    for (;;) {
        float v = sensor.getRmsVoltage(10) * CALIBRATION_FACTOR;
        int64_t t = esp_timer_get_time();
        State candidate = looksDead(v) ? State::OFF : State::ON;

        if (candidate == currState) {
            streak = 0;                       // agrees with current state
            pending = State::UNKNOWN;
        } else {
            if (candidate != pending) {       // new candidate: restart the streak
                pending = candidate;
                streak = 0;
                streakStart = t;
            }
            if (++streak >= CONFIRM_COUNT) {
                currState = candidate;
                StateEvent ev{currState, streakStart};
                if (xQueueSend(queue, &ev, 0) != pdTRUE) {
                    Serial.println("[error] sensor event queue full; state change dropped");
                }
                streak = 0;
                pending = State::UNKNOWN;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
    }
}