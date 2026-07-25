# Tuya T5AI Pixel Robot Dog Head Interaction

Head-petting reactive firmware for the Tuya T5AI Pixel board and the EmbedFire
Xiaozhi CS100A ultrasonic module, designed for a robot dog's head-mounted
32×32 LED matrix.

## Features

- Shows the measured distance in centimetres on the 32×32 LED matrix.
- Updates the distance every 100 ms.
- Uses `-- CM` when no valid echo has been received for two seconds.
- Detects a hand approaching the head (within 20 cm) as a "pet" gesture.
- Immediately shows a smile and plays `happy_levelup` at volume 85.
- Rearms after the hand moves beyond 35 cm.
- Returns to the distance display after a 3-second smile.
- Uses the onboard BME280 temperature reading to control the display colour.
- Subscribes to the Go2 robot dog's MQTT topic and shows action patterns
  (arrows, turn arcs, heart, smile) on the matrix.
- Shows WiFi/MQTT link state as two indicator pixels in the top corners.
- Opens a status menu (WiFi/MQTT state + station IP) on the OK key.

## Wiring

Disconnect the previous RD03 radar before connecting the CS100A.

| CS100A | Tuya T5AI Pixel |
| --- | --- |
| VCC | 3V3 |
| TRIG | P34 |
| ECHO | P35 |
| GND | GND |

The CS100A supports 3–5.5 V operation. This project powers it from 3.3 V so
that the ECHO signal can be connected directly to the T5AI GPIO.

## MQTT display control

The firmware joins the robot dog's WiFi network and subscribes to its display
topic. Broker settings live at the top of
`app/tuya_t5_pixel_distance/src/mqtt_display.c`; WiFi credentials live in the
git-ignored `wifi_credentials.h` (copy `wifi_credentials.h.example` next to it
and fill in your network before building):

| Setting | Value |
| --- | --- |
| WiFi SSID / password | `wifi_credentials.h` (local only, never committed) |
| Broker | `192.168.5.10:1883` (no auth) |
| Topic | `go2/B42D1000Q5SAKA07/display` (QoS 1) |

Payloads map to display patterns:

| Payload | Pattern | Robot action |
| --- | --- | --- |
| `forward` | ↑ green arrow | Move forward |
| `backward` | ↓ orange arrow | Move backward |
| `turn_left` | ↶ cyan arc | Turn left |
| `turn_right` | ↷ cyan arc | Turn right |
| `heart` | ♥ red heart | finger_heart |
| `smile` | ☺ smile + happy sound | happy |
| `idle` | distance display | stop / action done |

Trailing whitespace/CR/LF in the payload is ignored; unknown payloads are
logged and dropped. A pattern stays on screen until the next payload arrives
(send `idle` to return to the distance display). The head-petting smile
briefly overrides MQTT patterns; the OK-key status menu overrides everything.

Test from a PC on the same network:

```sh
mosquitto_pub -h 192.168.5.3 -t 'go2/B42D1000Q5SAKA07/display' -q 1 -m forward
```

The device retries WiFi every 10 s and the broker every 3 s, so no reboot is
needed after network hiccups. WiFi power save is disabled
(`tal_wifi_lp_disable`) because the Beken radio otherwise drops the STA link
a few seconds after DHCP.

## Status indicators and OK-key menu

Two always-on indicator pixels sit in the top corners of the matrix:

| Pixel | State | Meaning |
| --- | --- | --- |
| Top-left | green steady | WiFi connected |
| Top-left | red blinking | WiFi down / reconnecting |
| Top-right | cyan steady | MQTT connected |
| Top-right | orange blinking | MQTT down / reconnecting |

A single click of the onboard **OK key** (GPIO44) toggles a status page:

- `WIFI` row with a green (connected) or red (down) square
- `MQTT` row with the same colour coding
- The station IP address on the two bottom lines (e.g. `10.68.12.` / `191`)

The page closes on a second click or automatically after 10 seconds
(`STATUS_MENU_TIMEOUT_MS` in `water_effect.c`).

## Desktop control GUI

`tools/board_gui.py` is a Tkinter control panel for the board:

```sh
pip install pyserial paho-mqtt
python tools/board_gui.py
```

- **表情控制**: connects to the MQTT broker and sends any of the seven
  payloads (or a custom one) to the display topic with one click.
- **设备配置**: edits the WiFi/broker/topic macros in `mqtt_display.c`, then
  syncs the sources into the local TuyaOpen SDK, rebuilds and flashes the
  board in one click (build output streams into the window).
- **串口监控**: live log viewer (default 460800 baud) that also parses the
  WiFi/MQTT/IP/pattern state out of the log stream.
- **设置**: customize the SDK root, app source dir, app subdir inside the
  SDK, `tyutool_cli` path and chip type (`-d`), with an environment checker.
  Settings persist in `tools/board_gui_settings.json` (git-ignored).

The GUI runs on Windows, Linux and macOS. Builds go through
`export.ps1` + PowerShell on Windows and `export.sh` + bash elsewhere; the
flasher is resolved from the custom path, the SDK's bundled
`tyutool_cli(.exe)`, or `PATH`. By default it auto-detects a `TuyaOpen*`
checkout next to `app/` and `tools/`, but any location can be set in the
设置 tab (the SDK is not part of this repository).

## Prebuilt firmware

The tested QIO image is available at:

`firmware/tuya_t5_pixel_distance_QIO_1.0.0.bin`

Flash it with TuyaOpen's `tyutool`:

```sh
tyutool_cli write \
  -d t5ai \
  -p /dev/cu.usbmodemXXXXXXXXXXX1 \
  -f firmware/tuya_t5_pixel_distance_QIO_1.0.0.bin \
  --plain
```

The exact serial device name varies by computer.

## Build from source

1. Clone [TuyaOpen](https://github.com/tuya/TuyaOpen).
2. Copy `app/tuya_t5_pixel_distance` into
   `TuyaOpen/apps/tuya_t5_pixel/tuya_t5_pixel_distance`.
3. Enter the copied application directory.
4. Build with:

```sh
python ../../../tos.py build
```

The application targets `T5AI / TUYA_T5AI_PIXEL`.

## Runtime thresholds

The main settings are near the top of
`app/tuya_t5_pixel_distance/src/water_effect.c`:

- `ULTRASONIC_ENTER_DISTANCE_CM`: 20 cm (pet detection range)
- `ULTRASONIC_EXIT_DISTANCE_CM`: 35 cm (rearm threshold)
- `ULTRASONIC_SAMPLE_MS`: 100 ms
- `SMILE_MAX_MS`: 3 seconds
- `HAPPY_AUDIO_VOLUME`: 85

