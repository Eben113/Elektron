// SIM800L diagnostic. Temporary replacement for Elektron.cpp (see notes in chat).
// Runs the AT checks automatically, prints a verdict, then becomes a serial bridge
// so you can type your own AT commands in the monitor.
#include <Arduino.h>

static HardwareSerial modemSerial(2);

static String sendAT(const char* cmd, uint32_t timeoutMs = 2000) {
  while (modemSerial.available()) modemSerial.read();      // drop stale bytes
  modemSerial.print(cmd);
  modemSerial.print("\r\n");
  String resp;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (modemSerial.available()) resp += (char)modemSerial.read();
    if (resp.endsWith("OK\r\n") || resp.indexOf("ERROR") >= 0) break;
    delay(10);
  }
  resp.trim();
  Serial.printf(">> %s\n%s\n\n", cmd, resp.length() ? resp.c_str() : "(no response)");
  return resp;
}

static bool has(const String& r, const char* s) { return r.indexOf(s) >= 0; }

extern "C" void app_main(void) {
  initArduino();
  Serial.begin(115200);
  modemSerial.begin(9600, SERIAL_8N1, 16, 17);   // RX=16 (modem TX), TX=17 (modem RX)
  Serial.println("\n=== SIM800L diagnostic ===");
  Serial.println("Waiting 5 s for the modem to boot...");
  delay(5000);

  // 1. Is it alive?
  bool alive = false;
  for (int i = 0; i < 10 && !alive; i++) alive = has(sendAT("AT", 1000), "OK");
  if (!alive) {
    Serial.println("VERDICT: no reply to AT. Check VCC (3.4-4.4V, 1000uF cap), common GND, "
                   "and TX/RX wiring (ESP32 GPIO16 <- modem TX, GPIO17 -> modem RX via divider).");
  } else {
    sendAT("ATE0");                               // echo off, cleaner output
    String cpin = sendAT("AT+CPIN?");
    sendAT("AT+CCID");
    sendAT("AT+CSDT?");
    sendAT("AT+CFUN?");
    String creg = sendAT("AT+CREG?");
    String csq  = sendAT("AT+CSQ");
    sendAT("AT+COPS?");
    sendAT("AT+CBAND?");
    sendAT("AT+CGATT?");

    Serial.println("=== VERDICT ===");
    if (has(cpin, "READY"))                              Serial.println("SIM: ready");
    else if (has(cpin, "SIM PIN"))                       Serial.println("SIM: PIN-locked, unlock it in a phone first");
    else                                                 Serial.println("SIM: NOT DETECTED. Reseat the micro-SIM (power off first), "
                                                                        "clean the contacts, or try AT+CSDT=1 then AT&W and reboot.");
    int i = creg.indexOf("+CREG:");
    if (i >= 0) {
      int stat = creg.substring(creg.indexOf(',', i) + 1).toInt();
      const char* names[] = {"not registered, not searching", "registered (home)", "searching...",
                             "registration DENIED", "unknown", "registered (roaming)"};
      Serial.printf("Network: %s\n", stat >= 0 && stat <= 5 ? names[stat] : "?");
    }
    int q = csq.indexOf("+CSQ:");
    if (q >= 0) Serial.printf("Signal: %ld (10+ usable, 99 = none)\n", csq.substring(q + 5).toInt());
    Serial.println("Slow scan (up to 60 s): type AT+COPS=? below if still unregistered.");
  }

  Serial.println("\n=== Bridge mode: type AT commands (Enter to send) ===");
  char last = 0;
  for (;;) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\r' || c == '\n') {
        if (!(c == '\n' && last == '\r')) { modemSerial.print("\r\n"); Serial.println(); }
      } else { modemSerial.write(c); Serial.write(c); }   // local echo
      last = c;
    }
    while (modemSerial.available()) Serial.write(modemSerial.read());
    delay(1);
  }
}