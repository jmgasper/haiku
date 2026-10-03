#!/usr/bin/env python3
"""Switch the Raspberry Pi 4's smart plug through Home Assistant.

Usage: power.py status|on|off|cycle

The URL and entity IDs come from $RPI4_STATE/homeassistant.json, the token
from $HA_TOKEN or $RPI4_STATE/homeassistant.token. RPI4_STATE defaults to
/mnt/HaikuWork/rpi4/state. None of these are ever committed.
"""

import json
import os
from pathlib import Path
import sys
import time
import urllib.request

STATE = Path(os.environ.get("RPI4_STATE", "/mnt/HaikuWork/rpi4/state"))
CONFIG = json.loads((STATE / "homeassistant.json").read_text())
TOKEN = os.environ.get("HA_TOKEN") or (STATE / "homeassistant.token").read_text().strip()


def call(path, body=None):
    request = urllib.request.Request(
        CONFIG["url"] + path,
        data=None if body is None else json.dumps(body).encode(),
        headers={"Authorization": "Bearer " + TOKEN, "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)


def state(entity):
    return call("/api/states/" + entity)["state"]


def status():
    result = {"switch": state(CONFIG["switch"])}
    if CONFIG.get("power_sensor"):
        result["watts"] = state(CONFIG["power_sensor"])
    return result


def switch(on):
    call("/api/services/switch/turn_" + ("on" if on else "off"), {"entity_id": CONFIG["switch"]})
    wanted = "on" if on else "off"
    deadline = time.monotonic() + 15
    while state(CONFIG["switch"]) != wanted:
        if time.monotonic() > deadline:
            raise RuntimeError("the plug did not turn " + wanted)
        time.sleep(0.5)


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else "status"
    if command == "on":
        switch(True)
    elif command == "off":
        switch(False)
    elif command == "cycle":
        switch(False)
        time.sleep(float(os.environ.get("RPI4_OFF_SECONDS", "5")))
        switch(True)
    elif command != "status":
        sys.exit(__doc__)
    print(json.dumps(status()))


if __name__ == "__main__":
    main()
