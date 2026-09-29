![picokit-31-thermostat](https://raw.githubusercontent.com/mytechnotalent/picokit-31-thermostat/main/picokit-31-thermostat.png)

<br>

## FREE Reverse Engineering Self-Study Course [HERE](https://github.com/mytechnotalent/reverse-engineering)
## FREE Embedded Hacking Course [HERE](https://github.com/mytechnotalent/Embedded-Hacking)

<br>

# PICOKIT-31 THERMOSTAT

### Hysteresis Vent Control with an IR Setpoint and Authenticated Heartbeat
#### Lesson 31 of the Picokit Series

<br>

***
**LEGAL DISCLAIMER:**
The information, tools, and code provided in this repository and course are strictly for educational, research, and defensive purposes only.

You are explicitly prohibited from using any materials contained herein to access, test, modify, or exploit any device, network, or system that you do not own 100% or for which you do not have explicit, documented, and legally binding authorization to interact with.

By using this repository and course, you acknowledge and agree that:

1. Any illegal, unauthorized, or malicious use of this information is solely your responsibility.
2. The author(s) and contributor(s) of this repository and course shall not be held liable for any damages, legal repercussions, criminal charges, or unauthorized actions resulting from the use, misuse, or abuse of the contents herein.
3. You will comply with all applicable local, state, national, and international laws regarding cybersecurity and computer fraud.

**IF YOU DO NOT AGREE WITH THESE TERMS, DO NOT USE THIS REPOSITORY AND COURSE.**
***

<br>
<br>

## Overview

The thirty-first Picokit lesson. The node samples the DHT11 and drives the SG90
vent with a hysteresis thermostat: a reading at or above the setpoint plus the
band opens the vent, a reading at or below the setpoint minus the band closes
it, and anything in between holds. The setpoint is adjustable from the NEC
infrared remote, so you can force a crossing and watch the vent move. Every
five seconds the node transmits an authenticated heartbeat over LoRa with the
vent angle and temperature.

<br>

## What it teaches

- A hysteresis thermostat with an open, close, and hold policy.
- Driving the SG90 vent to fixed open and closed positions.
- Adjusting and clamping a setpoint from the infrared remote.
- Reporting the vent angle and temperature in the heartbeat body.

<br>

## Hardware

| Peripheral | Pico 2 pin | Role |
| --- | --- | --- |
| DHT11 | GP4 | thermostat input |
| VS1838B IR | GP5 | setpoint input |
| SG90 servo | GP14 | vent actuator |
| Red / Yellow / Green | GP16 / GP18 / GP17 | vent status |
| Onboard LED | GP25 | heartbeat, one blink per transmit |
| RYLR998 | GP8 TX / GP9 RX | LoRa heartbeat |
| Debug Probe | SWCLK/SWDIO/GND, GP0/GP1 | SWD and the console |

<br>

## How it works

The node runs `monitor_step` in a loop. Every 2 seconds it samples the DHT11
and applies hysteresis around the setpoint, driving the vent to 90 degrees when
warm enough and 0 degrees when cold enough. Every 5 seconds it transmits an
authenticated heartbeat; the body is `{"n":31,"s":<seq>,"v":<vent>,"t":<tenths>}`
sealed with the field key. Remote `0x45` raises the setpoint by 0.5 C and
`0x46` lowers it, so a crossing can be forced on demand.

<br>

## Build and flash

```bash
cd firmware
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350-arm-s
cmake --build build
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "program build/picokit_31_thermostat.elf verify reset exit"
```

<br>

## Watch the node

Open the console at 115200 and reset:

```text
BOOT
=== PICOKIT-31 THERMOSTAT // HYSTERESIS VENT + AUTHENTICATED HEARTBEAT ===
TEMP 230 SET 200 VENT 90
SETPOINT 265
TEMP 230 SET 265 VENT 0
RX from 0x0001, N bytes
```

<br>

## The gateway

```bash
cd gateway
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python3 listen.py --port /dev/cu.usbserial-A50285BI --hub 0001 --network 18 --db gateway.db
```

It prints `OK node=31 rssi=...` per authenticated heartbeat. The terminal
dashboard `python3 tui.py --db gateway.db` and the web dashboard
`python3 web/app.py --db gateway.db` show the same rows.

<br>

## Verify

```bash
python3 .opencode/skill/embedded-c-standard/audit_c_standard.py
python3 .opencode/skill/embedded-python-standard/audit_python_standard.py
python3 .opencode/skill/iot-readme-standard/validate_readme.py
python3 .opencode/skill/iot-banner-standard/validate_banner.py
python3 scripts/run_tests.py
python3 scripts/check_coverage.py
```

<br>

# Next
[picokit-32-cold-chain-alarm](https://github.com/mytechnotalent/picokit-32-cold-chain-alarm)

<br>

# License
[MIT License](https://github.com/mytechnotalent/picokit-31-thermostat/blob/main/LICENSE)
