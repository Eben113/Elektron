"""Fake grid nodes. Publishes events, heartbeats and retained status per the agreed format."""
import json, os, random, signal, sys, time
import paho.mqtt.client as mqtt
from dotenv import load_dotenv

load_dotenv()


HOST = os.getenv("MQTT_HOST", "localhost")
PREFIX = os.getenv("TOPIC_PREFIX", "grid")
PORT = int(os.getenv("MQTT_PORT", 1883))
USER, PASS = os.getenv("MQTT_USER"), os.getenv("MQTT_PASS")
TLS = os.getenv("MQTT_TLS", "0") == "1"
NODES = int(os.getenv("NODES", 3))
HEARTBEAT_S = int(os.getenv("HEARTBEAT_S", 10))   # use 300 for realism
P_OUTAGE = float(os.getenv("P_OUTAGE", 0.02))      # per second, while ON
P_RESTORE = float(os.getenv("P_RESTORE", 0.10))    # per second, while OFF


class Node:
    def __init__(self, i):
        self.id = f"n{i:03d}"
        self.state, self.seq, self.next_beat = "ON", 0, 0.0
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"sim-{self.id}")
        if USER:
            self.c.username_pw_set(USER, PASS)
        if TLS:
            self.c.tls_set()
        self.c.will_set(f"{PREFIX}/{self.id}/status", "offline", qos=1, retain=True)
        self.c.connect(HOST, PORT, keepalive=30)
        self.c.loop_start()
        self.c.publish(f"{PREFIX}/{self.id}/status", "online", qos=1, retain=True)

    def _msg(self, **extra):
        self.seq += 1
        return json.dumps({"v": 1, "id": self.id, "seq": self.seq, "state": self.state,
                           "ts": int(time.time()), **extra}, separators=(",", ":"))

    def event(self):
        self.c.publish(f"{PREFIX}/{self.id}/event", self._msg(ts_src="ntp"), qos=1)

    def heartbeat(self):
        vbat = round(random.uniform(3.7, 4.1) if self.state == "ON" else random.uniform(3.4, 3.9), 2)
        self.c.publish(f"{PREFIX}/{self.id}/heartbeat",
                       self._msg(vbat=vbat, rssi=random.randint(-95, -60), fw="0.1.0"), qos=1)

    def tick(self, now):
        p = P_OUTAGE if self.state == "ON" else P_RESTORE
        if random.random() < p:
            self.state = "OFF" if self.state == "ON" else "ON"
            self.event()
            print(f"{self.id} -> {self.state}")
        if now >= self.next_beat:
            self.heartbeat()
            self.next_beat = now + HEARTBEAT_S

    def close(self):
        self.c.publish(f"{PREFIX}/{self.id}/status", "offline", qos=1, retain=True).wait_for_publish()
        self.c.loop_stop()
        self.c.disconnect()


nodes = [Node(i + 1) for i in range(NODES)]
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
print(f"{NODES} simulated nodes publishing to {HOST}. Ctrl+C to stop "
      "(kill -9 a node process to test the Last Will).")
try:
    while True:
        now = time.time()
        for n in nodes:
            n.tick(now)
        time.sleep(1)
except (KeyboardInterrupt, SystemExit):
    pass
finally:
    for n in nodes:
        n.close()