// sampling.h
#pragma once
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "../ZMPT101B/ZMPT101B.h"

enum class State : uint8_t { UNKNOWN, ON, OFF };

struct StateEvent {
    State   state;
    int64_t timestampUs;   // microseconds since boot, of the FIRST differing reading
};

class Sensor {
public:
    explicit Sensor(QueueHandle_t queue);
    void begin();
    void startTask();

private:
    static void taskEntry(void* self);
    void run();
    bool looksDead(float v) const;

    ZMPT101B sensor;
    QueueHandle_t queue;
    State currState = State::UNKNOWN;

    static constexpr float    DEAD_BELOW         = 30.0f;   // measure your noise floor
    static constexpr float    LIVE_ABOVE         = 100.0f;  // well below nominal on purpose
    static constexpr uint8_t  CONFIRM_COUNT      = 4;
    static constexpr uint32_t SAMPLE_INTERVAL_MS = 200;
    static constexpr float    SENSITIVITY        = 500.0f;
    static constexpr float    CALIBRATION_FACTOR = 1.49f;
};