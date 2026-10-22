"""Grid monitor backend: MQTT subscriber (HiveMQ) -> InfluxDB, plus a read API for the node map."""
import hmac, json, os, re, threading, time
from contextlib import asynccontextmanager
from pathlib import Path

import paho.mqtt.client as mqtt
from dotenv import load_dotenv
from fastapi import Depends, FastAPI, Header, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from influxdb_client_3 import InfluxDBClient3, Point

load_dotenv()
env = os.environ.get

PREFIX = env("TOPIC_PREFIX", "grid")
HEARTBEAT_S = int(env("HEARTBEAT_S", "120"))
OFFLINE_AFTER_S = int(env("OFFLINE_AFTER_S", str(HEARTBEAT_S * 3)))
LIVE_WINDOW_S = int(env("LIVE_WINDOW_S", "10"))   # power ON reported this recently = shown live
NODES = json.loads(Path(__file__).with_name("nodes.json").read_text())

influx = InfluxDBClient3(host=os.environ["INFLUX_HOST"], token=os.environ["INFLUX_TOKEN"],
                         org=env("INFLUX_ORG", ""), database=os.environ["INFLUX_DATABASE"])

# In-memory current state per node, kept fresh by the MQTT thread.
lock = threading.Lock()
state = {n: {"state": "UNKNOWN", "since": None, "info_ts": 0, "last_seen": None, "online": None, "last_on": None}
         for n in NODES}


def apply_state(node, st, ts):
    """Apply a state report (event or heartbeat); ignore stale/out-of-order ones."""
    s = state[node]
    if ts >= s["info_ts"]:
        if st != s["state"]:
            s["since"] = ts
        s["state"], s["info_ts"] = st, ts
        return True
    return False


def seed_state():
    """Load the latest known state per node from InfluxDB so a restart doesn't lose it."""
    try:
        rows = influx.query(
            "SELECT node, state_val, ts FROM ("
            " SELECT node, state_val, to_unixtime(time) AS ts,"
            " ROW_NUMBER() OVER (PARTITION BY node ORDER BY time DESC) AS rn"
            " FROM grid_event WHERE time >= now() - INTERVAL '30 days') WHERE rn = 1",
            language="sql").to_pylist()
        with lock:
            for r in rows:
                if r["node"] in state:
                    apply_state(r["node"], "ON" if r["state_val"] == 1 else "OFF", int(r["ts"]))
        print(f"seeded {len(rows)} nodes from InfluxDB")
    except Exception as e:  # first run: table doesn't exist yet
        print("seed skipped:", e)


# ---------- MQTT ----------
def on_connect(c, userdata, flags, reason_code, properties):
    print("mqtt connected:", reason_code)
    c.subscribe([(f"{PREFIX}/+/event", 1), (f"{PREFIX}/+/heartbeat", 1), (f"{PREFIX}/+/status", 1)])


def on_message(c, userdata, msg):
    try:
        _, node, kind = msg.topic.split("/")
        if node not in NODES:
            return
        now = time.time()
        if kind == "status":
            online = msg.payload == b"online"
            with lock:
                state[node]["online"] = online
                if online:
                    state[node]["last_seen"] = now
            influx.write(record=Point("grid_status").tag("node", node).field("online", int(online)))
            return
        d = json.loads(msg.payload)
        if d.get("v") != 1 or d.get("id") != node:
            return
        st = "ON" if d["state"] == "ON" else "OFF"
        p = (Point("grid_event" if kind == "event" else "grid_heartbeat")
             .tag("node", node)
             .field("state_val", 1 if st == "ON" else 0)
             .field("seq", int(d["seq"]))
             .time(int(d["ts"]) * 10**9))  # nanoseconds
        if kind == "event":
            p = p.tag("ts_src", d.get("ts_src", "unknown"))
        else:
            p = p.field("vbat", float(d["vbat"])).field("rssi", int(d["rssi"])).field("fw", str(d["fw"]))
        with lock:
            state[node]["last_seen"], state[node]["online"] = now, True
            if apply_state(node, st, int(d["ts"])) and st == "ON":
                state[node]["last_on"] = now      # fresh proof the power is on
        influx.write(record=p)
    except Exception as e:
        print("bad message:", msg.topic, e)


@asynccontextmanager
async def lifespan(app):
    seed_state()
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=env("MQTT_CLIENT_ID", "grid-backend"))
    if env("MQTT_USER"):
        c.username_pw_set(env("MQTT_USER"), env("MQTT_PASS"))
    if env("MQTT_TLS", "1") == "1":
        c.tls_set()
    c.on_connect, c.on_message = on_connect, on_message
    c.reconnect_delay_set(1, 60)
    c.connect_async(os.environ["MQTT_HOST"], int(env("MQTT_PORT", "8883")), keepalive=60)
    c.loop_start()
    yield
    c.loop_stop()
    c.disconnect()


# ---------- API ----------
app = FastAPI(title="Grid Monitor", lifespan=lifespan)
app.add_middleware(CORSMiddleware, allow_origins=env("ALLOWED_ORIGINS", "*").split(","),
                   allow_methods=["GET"], allow_headers=["*"])


def require_key(x_api_key: str | None = Header(default=None)):
    key = env("API_KEY")
    if key and not (x_api_key and hmac.compare_digest(x_api_key, key)):
        raise HTTPException(401, "invalid or missing X-API-Key")


@app.get("/", include_in_schema=False)
def index():
    return FileResponse(Path(__file__).with_name("index.html"))


@app.get("/health")
def health():
    return {"ok": True}


@app.get("/api/map")
def node_map():
    now, out = time.time(), []
    with lock:
        for nid, meta in NODES.items():
            s = state[nid]
            stale = s["last_seen"] is None or now - s["last_seen"] > OFFLINE_AFTER_S
            live = (s["state"] == "ON" and s["last_on"] is not None
                    and now - s["last_on"] <= LIVE_WINDOW_S)
            if s["state"] == "UNKNOWN":
                status = "UNKNOWN"
            elif live:
                status = "ON"               # recent ON report wins over stale offline flags
            elif stale or s["online"] is False:
                status = "OFFLINE"          # node lost contact: not the same as grid down
            else:
                status = s["state"]         # ON = power live, OFF = grid down
            out.append({"id": nid, "name": meta["name"], "area": meta["area"],
                        "lat": meta["lat"], "lng": meta["lng"], "status": status,
                        "since": s["since"], "last_seen": s["last_seen"], "live": live})
    return out


def compute_uptime(events, window_start, now, current):
    """events: [(ts, state_val)] ascending. Returns (uptime %, outage count)."""
    if not events:
        return (100.0 if current == "ON" else 0.0 if current == "OFF" else None), 0
    cur = "ON" if events[0][1] == 0 else "OFF"   # state before the first change
    t, on_secs, outages = window_start, 0, 0
    for ts, v in events:
        if cur == "ON":
            on_secs += ts - t
        t, cur = ts, ("ON" if v == 1 else "OFF")
        outages += cur == "OFF"
    if cur == "ON":
        on_secs += now - t
    return round(100 * on_secs / (now - window_start), 2), outages


@app.get("/api/nodes/{node_id}/history", dependencies=[Depends(require_key)])
def history(node_id: str, days: int = Query(7, ge=1, le=30)):
    if node_id not in NODES or not re.fullmatch(r"[A-Za-z0-9_-]+", node_id):
        raise HTTPException(404, "unknown node")
    rows = influx.query(
        f"SELECT to_unixtime(time) AS ts, state_val FROM grid_event "
        f"WHERE node = '{node_id}' AND time >= now() - INTERVAL '{days} days' ORDER BY time",
        language="sql").to_pylist()
    events = [(int(r["ts"]), int(r["state_val"])) for r in rows]
    now = time.time()
    with lock:
        current = state[node_id]["state"]
    pct, outages = compute_uptime(events, now - days * 86400, now, current)
    return {"id": node_id, "days": days, "uptime_pct": pct, "outage_count": outages,
            "events": [{"ts": t, "state": "ON" if v else "OFF"} for t, v in events]}
