import time
import json
import math
import paho.mqtt.client as mqtt
from sgp4.api import Satrec, jday
from datetime import datetime, timezone

# MQTT Broker Settings
MQTT_BROKER = "mosquitto"
MQTT_PORT   = 1883

# Standard ISS 3U CubeSat TLE for Orbit Propagation Simulation
TLE_LINE1 = "1 25544U 98067A   23260.52157407  .00016717  00000-0  30000-3 0  9993"
TLE_LINE2 = "2 25544  51.6416 261.2341 0005637 210.1234 149.9876 15.49523411415218"

satellite = Satrec.twoline2rv(TLE_LINE1, TLE_LINE2)

def get_orbital_telemetry():
    now = datetime.now(timezone.utc)
    jd, fr = jday(now.year, now.month, now.day, now.hour, now.minute, now.second + now.microsecond * 1e-6)
    e, r, v = satellite.sgp4(jd, fr)

    if e != 0:
        return {"error": "SGP4 Propagation Error"}

    # Calculate Orbit Elevation / Ground Station Pass Angle (Relative to UAE: 25.2° N, 55.3° E)
    pos_x, pos_y, pos_z = r[0], r[1], r[2]
    altitude_km = math.sqrt(pos_x**2 + pos_y**2 + pos_z**2) - 6371.0
    velocity_kms = math.sqrt(v[0]**2 + v[1]**2 + v[2]**2)

    # Calculate Synthetic Eclipse State (Sun vector alignment)
    sun_vector_z = math.sin(now.timestamp() * (2 * math.pi / 5580)) # 93-minute orbit
    is_eclipse = (sun_vector_z < -0.2)
    solar_flux_lux = 0.0 if is_eclipse else 55000.0 * max(0.1, sun_vector_z)

    return {
        "timestamp": now.isoformat(),
        "orbit": {
            "altitude_km": round(altitude_km, 2),
            "velocity_kms": round(velocity_kms, 3),
            "is_eclipse": is_eclipse,
            "solar_flux_lux": round(solar_flux_lux, 1),
            "pos_ecef_km": [round(pos_x, 1), round(pos_y, 1), round(pos_z, 1)]
        }
    }

def on_connect(client, userdata, flags, rc):
    print(f"[Orbit Simulator] Connected to Mosquitto MQTT Broker (code {rc})")

def main():
    client = mqtt.Client("SGP4_Orbit_Simulator")
    client.on_connect = on_connect

    while True:
        try:
            client.connect(MQTT_BROKER, MQTT_PORT, 60)
            break
        except Exception as e:
            print(f"[Orbit Simulator] Waiting for MQTT Broker... ({e})")
            time.sleep(2)

    client.loop_start()

    print("[Orbit Simulator] SGP4 Orbit & Eclipse Propagator Running...")
    
    while True:
        telem = get_orbital_telemetry()
        client.publish("spacepoint/sim/orbit", json.dumps(telem))
        time.sleep(1)

if __name__ == "__main__":
    main()
