#include <stdio.h>
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "node.h"
#include "publisher.h"

// Events wait here until they are really sent. Oldest is dropped if the link stays down too long.
static const int MAX_PENDING = 32;
static StateEvent pending[MAX_PENDING];
static int head = 0, count = 0;

static void pushPending(const StateEvent& e) {
  if (count == MAX_PENDING) { head = (head + 1) % MAX_PENDING; count--; }
  pending[(head + count) % MAX_PENDING] = e;
  count++;
}

void reportTask(void* arg) {
  QueueHandle_t queue = static_cast<QueueHandle_t>(arg);
  Serial.println("[report] task started");
  const TickType_t HEARTBEAT = pdMS_TO_TICKS(5 * 60 * 1000);
  const TickType_t SLICE     = pdMS_TO_TICKS(20 * 1000);   // wake often enough to keep MQTT alive
  TickType_t lastBeat = xTaskGetTickCount() - HEARTBEAT;   // first heartbeat as soon as state is known
  State lastState = State::UNKNOWN;
  StateEvent ev;

  for (;;) {
    if (xQueueReceive(queue, &ev, SLICE) == pdTRUE) {
      do {
        Serial.printf("[sensor] state=%s at %lld us\n",
                      ev.state == State::ON ? "ON" : "OFF",
                      static_cast<long long>(ev.timestampUs));
        pushPending(ev);
        lastState = ev.state;
      } while (xQueueReceive(queue, &ev, 0) == pdTRUE);     // drain any burst
    }

    // Send in order; stop at the first failure and keep the rest for the next round.
    while (count > 0 && publishEvent(pending[head])) {
      Serial.printf("[mqtt] event sent; pending=%d\n", count - 1);
      head = (head + 1) % MAX_PENDING;
      count--;
    }
    if (count > 0) Serial.printf("[mqtt] event pending; queue=%d\n", count);

    // Heartbeat reports the REAL current state, never a hardcoded one.
    if (count == 0 && lastState != State::UNKNOWN &&
        xTaskGetTickCount() - lastBeat >= HEARTBEAT) {
      if (publishHeartbeat(lastState)) {
        Serial.printf("[mqtt] heartbeat sent; state=%s\n",
                      lastState == State::ON ? "ON" : "OFF");
        lastBeat = xTaskGetTickCount();
      } else {
        Serial.println("[mqtt] heartbeat deferred");
      }
    }

    publisherLoop();
  }
}

extern "C" void app_main(void) {
  initArduino();
  Serial.begin(115200);
  Serial.println("\n[boot] Elektron firmware starting");
  Serial.println("[modem] initializing publisher");
  publisherBegin();
  Serial.println("[modem] publisher initialized");

  QueueHandle_t queue = xQueueCreate(16, sizeof(StateEvent));
  if (queue == nullptr) {
    Serial.println("[error] failed to create sensor event queue");
    return;
  }
  Serial.println("[boot] sensor event queue created (capacity=16)");

  static Sensor sensor(queue);
  sensor.begin();
  Serial.println("[boot] sensor initialized");
  sensor.startTask();                                       // priority 5 in sampling.cpp
  BaseType_t reportTaskResult = xTaskCreate(reportTask, "report", 8192, queue, 4, nullptr);
  if (reportTaskResult != pdPASS) {
    Serial.println("[error] failed to create report task");
    return;
  }
  Serial.println("[boot] report task created");
}