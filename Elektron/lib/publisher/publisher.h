#pragma once
#include <Arduino.h>
#include "node.h"   // StateEvent, State (adjust if your header is named differently)

void publisherBegin();                          // serial, modem init, MQTT config
bool publishEvent(const StateEvent& ev);        // true only if handed to the socket
bool publishHeartbeat(State current);           // true only if handed to the socket
void publisherLoop();                           // MQTT keepalive; call every few seconds