"""
MQTT -> InfluxDB bridge for the ESP32 power quality meter.

Subscribes to power_meter/+/data, parses each JSON message from a meter
and writes it to InfluxDB as measurement "power" with tag "device".

The InfluxDB token is read from meter_token.txt (same folder), which is
listed in .gitignore so it never gets uploaded.
"""
import json
from pathlib import Path

import paho.mqtt.client as mqtt
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS

# ---------- Settings ----------
MQTT_HOST = "localhost"
MQTT_PORT = 1883
MQTT_TOPIC = "power_meter/+/data"   # + = any device name

INFLUX_URL = "http://localhost:8086"
INFLUX_ORG = "vpp"
INFLUX_BUCKET = "power_meter"
TOKEN_FILE = Path(__file__).with_name("meter_token.txt")

FIELDS = ["vrms", "irms", "p", "s", "pf", "p1", "q1", "dpf", "thd_i", "thd_v",
          "h2", "h3", "h4", "h5", "h6", "h7", "h8", "h9"]

# ---------- InfluxDB ----------
token = TOKEN_FILE.read_text().strip()
influx = InfluxDBClient(url=INFLUX_URL, token=token, org=INFLUX_ORG)
write_api = influx.write_api(write_options=SYNCHRONOUS)


def on_connect(client, userdata, flags, reason_code, properties):
    print(f"Connected to MQTT broker ({reason_code}). Subscribing to {MQTT_TOPIC}")
    client.subscribe(MQTT_TOPIC)


def on_message(client, userdata, msg):
    try:
        device = msg.topic.split("/")[1]          # power_meter/<device>/data
        data = json.loads(msg.payload.decode())
        point = Point("power").tag("device", device)
        for name in FIELDS:
            if name in data:
                point = point.field(name, float(data[name]))
        write_api.write(bucket=INFLUX_BUCKET, record=point)
        print(f"Wrote {device}: P={data.get('p')} W, "
              f"Vrms={data.get('vrms')} V, Irms={data.get('irms')} A")
    except Exception as e:
        print(f"Skipped a message: {e}")


client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="pq-bridge")
client.on_connect = on_connect
client.on_message = on_message
client.connect(MQTT_HOST, MQTT_PORT)
client.loop_forever()
