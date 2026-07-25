# Tuya T5AI Pixel Distance Display

Distance-reactive firmware for the Tuya T5AI Pixel board and the EmbedFire
Xiaozhi CS100A ultrasonic module.

## Features

- Shows the measured distance in centimetres on the 32×32 LED matrix.
- Updates the distance every 100 ms.
- Uses `-- CM` when no valid echo has been received for two seconds.
- Immediately shows a smile and plays `happy_levelup` when an object enters
  45 cm.
- Rearms after the object moves beyond 60 cm.
- Returns to the distance display after the smile timeout.
- Uses the onboard BME280 temperature reading to control the display colour.

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

- `ULTRASONIC_ENTER_DISTANCE_CM`: 45 cm
- `ULTRASONIC_EXIT_DISTANCE_CM`: 60 cm
- `ULTRASONIC_SAMPLE_MS`: 100 ms
- `SMILE_MAX_MS`: 5 seconds

