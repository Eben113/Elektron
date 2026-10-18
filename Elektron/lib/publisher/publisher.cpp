#define TINY_GSM_MODEM_SIM800        // must come before TinyGsmClient.h
#include "publisher.h"
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <time.h>
#include <stdlib.h>
#include "esp_timer.h"

// ---------------- settings ----------------
static const char APN[]       = "internet.ng.airtel.com";   // MTN: web.gprs.mtnnigeria.net
static const char BROKER[]    = "broker.hivemq.com";
static const char PREFIX[]    = "grid-7f3a9c";              // same as TOPIC_PREFIX in the backend
static const char NODE[]      = "n002";

static const char WIFI_SSID[] = "elektron";                         // leave empty to disable the fallback
static const char WIFI_PASS[] = "12345678";
static const uint8_t  GSM_MAX_FAILS = 1;                    // failed GSM attempts before WiFi (1 = try once)
static const uint32_t GSM_RETRY_MS  = 0;                    // while on WiFi, re-probe GSM this often (0 = never, stay on WiFi)

// ---------------- state ----------------
enum class Transport { GSM, WIFI };

static HardwareSerial SerialAT(2);
static TinyGsm modem(SerialAT);
static TinyGsmClient gsm(modem);
static WiFiClient wifi;
static PubSubClient mqtt(gsm);

static Transport transport = Transport::GSM;
static uint8_t  gsmFails = 0;
static uint32_t wifiSinceMs = 0;
static uint32_t seq = 0;
static int64_t  epochOffsetS = 0;      // (unix seconds) - (seconds since boot); 0 = not synced yet
static const char* clockSrc = "boot";  // "gsm" or "ntp", reported as ts_src

// ---------------- clock ----------------
static int64_t networkEpoch() {        // from the cellular network; 0 if the modem clock isn't set
  String s = modem.getGSMDateTime(DATE_FULL);      // "yy/MM/dd,hh:mm:ss+zz" (zz = quarter hours)
  int yy, mo, d, h, mi, sec, tz;
  if (sscanf(s.c_str(), "%d/%d/%d,%d:%d:%d%d", &yy, &mo, &d, &h, &mi, &sec, &tz) != 7) return 0;
  if (yy < 25) return 0;
  struct tm t = {};
  t.tm_sec = sec; t.tm_min = mi; t.tm_hour = h;
  t.tm_mday = d;  t.tm_mon = mo - 1; t.tm_year = yy + 100;
  return (int64_t)mktime(&t) - (int64_t)tz * 900;  // TZ forced to UTC in publisherBegin()
}

static int64_t ntpEpoch() {            // from SNTP (WiFi); 0 until synced
  time_t t = time(nullptr);
  return t > 1700000000 ? (int64_t)t : 0;
}

static void syncClock() {
  bool viaWifi = transport == Transport::WIFI;
  int64_t e = viaWifi ? ntpEpoch() : networkEpoch();
  if (e > 0) {
    epochOffsetS = e - esp_timer_get_time() / 1000000;
    clockSrc = viaWifi ? "ntp" : "gsm";
  }
}

// ---------------- links ----------------
static void useTransport(Transport t) {
  if (t == transport) return;
  mqtt.disconnect();
  if (t == Transport::GSM) mqtt.setClient(gsm); else mqtt.setClient(wifi);
  transport = t;
  Serial.printf("transport -> %s\n", t == Transport::GSM ? "GSM" : "WiFi");
}

static bool gsmLink() {
  if (modem.getSimStatus() != SIM_READY) { Serial.println("GSM: SIM not ready"); return false; }
  if (!modem.isNetworkConnected() && !modem.waitForNetwork(30000)) {
    Serial.println("GSM: not registered"); return false;
  }
  if (!modem.isGprsConnected() && !modem.gprsConnect(APN, "", "")) {
    Serial.println("GSM: data attach failed"); return false;
  }
  return true;
}

static bool wifiLink() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);                                  // power-save causes beacon timeouts
  for (int i = 0; i < 150 && WiFi.status() != WL_CONNECTED; i++) delay(100);   // up to 15 s
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi: connect failed"); return false; }
  static bool ntpStarted = false;
  if (!ntpStarted) { configTime(0, 0, "pool.ntp.org", "time.google.com"); ntpStarted = true; }
  for (int i = 0; i < 50 && !ntpEpoch(); i++) delay(100);                      // up to 5 s
  Serial.println(WiFi.RSSI());
  return true;
}

static bool mqttConnected() {
  if (mqtt.connected()) return true;
  char willT[64], cid[48];
  snprintf(willT, sizeof willT, "%s/%s/status", PREFIX, NODE);
  snprintf(cid, sizeof cid, "%s-%s", PREFIX, NODE);   // unique on a public broker
  if (!mqtt.connect(cid, nullptr, nullptr, willT, 1, true, "offline")) {
    Serial.printf("MQTT connect failed, state=%d\n", mqtt.state());
    return false;
  }
  mqtt.publish(willT, "online", true);                // retained
  return true;
}

static bool linkUp() {
  const bool wifiAllowed = WIFI_SSID[0] != '\0';

  // On WiFi: every GSM_RETRY_MS, make one GSM attempt; a failure sends us straight back.
  if (transport == Transport::WIFI && GSM_RETRY_MS && millis() - wifiSinceMs > GSM_RETRY_MS) {
    useTransport(Transport::GSM);
    gsmFails = GSM_MAX_FAILS - 1;
  }
  // GSM has failed enough times: switch to WiFi.
  if (transport == Transport::GSM && gsmFails >= GSM_MAX_FAILS && wifiAllowed) {
    Serial.println("GSM failed repeatedly, falling back to WiFi");
    useTransport(Transport::WIFI);
    wifiSinceMs = millis();
  }

  bool linked = (transport == Transport::GSM) ? gsmLink() : wifiLink();
  bool ok = linked && mqttConnected();

  if (transport == Transport::GSM) {                  // a failure at any stage counts
    if (ok) gsmFails = 0; else if (gsmFails < 255) gsmFails++;
  }
  if (ok && epochOffsetS == 0) syncClock();
  return ok;
}

// ---------------- public API ----------------
void publisherBegin() {
  setenv("TZ", "UTC0", 1);
  tzset();
  SerialAT.begin(9600, SERIAL_8N1, 16, 17);
  modem.sendAT(GF("+CLTS=1"));      // ask the network for time
  modem.waitResponse();
  modem.sendAT(GF("&W"));
  modem.waitResponse();
  modem.restart();
  for (int i = 0; i < 10 && modem.getSimStatus() != SIM_READY; i++) delay(1000);  // SIM is read after boot
  Serial.printf("SIM status: %d\n", (int)modem.getSimStatus());
  mqtt.setServer(BROKER, 1883);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(256);
}

bool publishEvent(const StateEvent& ev) {
  if (!linkUp() || epochOffsetS == 0) return false;     // hold events until we have real time
  int64_t ts = epochOffsetS + ev.timestampUs / 1000000; // time of the FIRST differing reading
  char topic[64], body[170];
  snprintf(topic, sizeof topic, "%s/%s/event", PREFIX, NODE);
  snprintf(body, sizeof body,
           "{\"v\":1,\"id\":\"%s\",\"seq\":%lu,\"state\":\"%s\",\"ts\":%lld,\"ts_src\":\"%s\"}",
           NODE, (unsigned long)++seq, ev.state == State::ON ? "ON" : "OFF", (long long)ts, clockSrc);
  return mqtt.publish(topic, body);
}

bool publishHeartbeat(State current) {
  if (current == State::UNKNOWN || !linkUp()) return false;
  syncClock();                                          // refresh offset, corrects drift
  if (epochOffsetS == 0) return false;
  int64_t ts = epochOffsetS + esp_timer_get_time() / 1000000;
  int rssi = (transport == Transport::WIFI) ? WiFi.RSSI() : -113 + 2 * modem.getSignalQuality();
  float vbat = modem.getBattVoltage() / 1000.0f;        // modem supply = battery (0 if no reply)
  char topic[64], body[200];
  snprintf(topic, sizeof topic, "%s/%s/heartbeat", PREFIX, NODE);
  snprintf(body, sizeof body,
           "{\"v\":1,\"id\":\"%s\",\"seq\":%lu,\"state\":\"%s\",\"ts\":%lld,"
           "\"vbat\":%.2f,\"rssi\":%d,\"fw\":\"0.1.0\"}",
           NODE, (unsigned long)++seq, current == State::ON ? "ON" : "OFF",
           (long long)ts, vbat, rssi);
  return mqtt.publish(topic, body);
}

void publisherLoop() { mqtt.loop(); }