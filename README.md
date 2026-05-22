# lunchmoney-esp32

Shows your [Lunch Money](https://lunchmoney.app) budgets for the current month on an ESP32 touch display.

The default build targets the **4.3" ESP32-8048S043** board (800×480 RGB panel).

## Your board

| Label on box | What it is |
|--------------|------------|
| **ES32-8048S043C_I** (or ESP32-8048S043) | 4.3" 800×480 IPS, ESP32-S3, **RGB parallel** display, **GT911** capacitive touch, USB-C |

This is **not** a Cheap Yellow Display (CYD). CYD is a smaller 2.8" 240×320 SPI display — different wiring and code.

Vendor demos live in `~/src/ESP32-8048S043-4INCH-LCD/`.

## Setup

1. Install [PlatformIO](https://platformio.org/).

2. Copy secrets:

   ```bash
   cp include/secrets.h.example include/secrets.h
   ```

   Edit `include/secrets.h` with WiFi and your [Lunch Money API token](https://my.lunchmoney.app/settings/developers).

## Build and upload

```bash
cd ~/src/lunchmoney-esp32
pio run -e 8048s043 -t upload --upload-port /dev/cu.usbserial-110
pio device monitor -e 8048s043 --port /dev/cu.usbserial-110
```

Find your port with `pio device list`. Use the `cu.*` device on macOS.

### Touch / scroll debug (serial)

With the display running, open the serial monitor and drag the budget list:

```bash
pio device monitor -e 8048s043 --port /dev/cu.usbserial-110
```

Look for:

- `touch: GT911 OK at 0x..` — capacitive touch initialized
- `scroll: bottom=N` — if `N` is 0, all budgets fit on screen (nothing to scroll)
- `touch: down raw=(..) mapped=(..) in_list=yes` — finger detected and mapped
- `scroll: drag dy=.. y=..->..` — scroll applied (`delta` should be non-zero when dragging)

To disable touch logging, build with `-D TOUCH_DEBUG=0` in `platformio.ini` `build_flags`.

On success you should see the backlight (GPIO **2**), then **Starting…**, then your budgets. The vendor demo flashes red/green/blue during init; this build goes straight to the UI.

### Other boards (legacy envs)

| Environment | Board |
|-------------|-------|
| `8048s043` | **ESP32-8048S043C** (default) |
| `cyd` | 2.8" Cheap Yellow Display, ESP32 |
| `es3c28p` | 2.8" lcdwiki ES3C28P (USB-C, 240×320) |

Legacy envs may need extra work; `main.cpp` is currently written for `8048s043`.

## What it shows

- Budget categories for the current month (with a budget set)
- Progress bar and spent / left (or over)
- Sorted by least remaining first
- Touch scroll (GT911)
- Refreshes every **3 hours** on WiFi

## Troubleshooting

- **Black screen, ~4.3" display, USB-C**: You need `-e 8048s043`, not `cyd` or `es3c28p`.
- **Port busy**: Close serial monitors, then upload again.
- **Still black after 8048s043 upload**: Press RESET; confirm box says 8048S043; try the vendor `3_3-4_TFT-LVGL-Widgets` demo from `ESP32-8048S043-4INCH-LCD` to verify hardware.

## API

Uses Lunch Money API v1 `GET /v1/budgets` for the current calendar month.

## License

MIT
