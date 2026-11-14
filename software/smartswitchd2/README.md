# SmartSwitch D2 — firmware

Arduino sketch for the ESP32-C6 in the SmartSwitch D2.

## What you need

- **Arduino IDE 2** with the **esp32** board package by Espressif (version 3.x)
- These libraries (Library Manager): **Adafruit Unified Sensor**, **Adafruit BME280**, **Button2**
- **esptool**, to write the sounds and the updater (`pip install esptool`; the esp32 board
  package also ships a copy)
- A USB cable and a **programming adapter** for connector J2 (see below)

## Programming adapter

The ESP32-C6 is programmed over its own USB port, which is wired to **J2** on the main board
(JST SH, 1.0 mm, 8 pins). You need a small adapter from USB-C to J2. It holds no
electronics, only wires:

| J2 pin | Signal | USB-C |
|---|---|---|
| 1, 2 | VCC (5 V) | VBUS |
| 3, 4, 5, 8 | GND | GND |
| 6 | USB D− | D− |
| 7 | USB D+ | D+ |

Build one from a USB-C breakout board (with the 5.1 kΩ resistors on CC1/CC2, as most have)
and a JST SH 8-pin cable, or ask for a ready-made one via
[jongtronics.com](https://jongtronics.com).

![Programming adapter](docs/photos/Programmer.png)
> ⚠️ Program the main board on its own, the adapter powers the board from USB.

## Board settings (Tools menu)

| Setting | Value |
|---|---|
| Board | ESP32C6 Dev Module |
| Partition Scheme | Custom (uses `partitions.csv` from the sketch folder) |
| Zigbee Mode | Zigbee ZCZR (coordinator/router) |
| USB CDC On Boot | Enabled |
| Flash Size | 4MB |

## First flash (USB)

Connect the adapter to J2 and to your PC.

1. **Firmware** — open `smartswitchd2.ino`, choose the settings above and click **Upload**.
2. **Sounds** — download `spiffs.bin` from the [Releases](https://github.com/Jongtronics-Dev/zigbee-smart-switch/releases) page and write it:

   ```
   esptool --chip esp32c6 --port COM6 write_flash 0x2F0000 spiffs.bin
   ```

3. **OTA updater** (only needed for updates over the air, see below) — open
   `updater/updater.ino`, use the same settings but **Zigbee Mode: Disabled**, then
   **Sketch → Export Compiled Binary**. Write the result, from the `updater` folder:

   ```
   esptool --chip esp32c6 --port COM6 write_flash 0x180000 build/esp32.esp32.esp32c6/updater.ino.bin
   ```

Replace `COM6` with your port.

## Pair with Zigbee2MQTT

1. Copy `zigbee2mqtt/smartswitch.mjs` from this repository to the
   `data/external_converters/` folder of Zigbee2MQTT and restart it.
2. Click **Permit join** and power the switch. It joins as **SmartSwitchD2**.
3. Check everything in the Zigbee2MQTT frontend (**Exposes**, **State**).

## Updates over the air (optional)

After the first USB flash, new firmware can be installed over WiFi:

1. Before step 3 above, copy `updater/OtaConfig.example.h` to `updater/OtaConfig.h` and fill
   in your WiFi name, password and server URL (`http://…`, no trailing slash).
2. Export the new firmware (**Sketch → Export Compiled Binary**) and put
   `build/esp32.esp32.esp32c6/smartswitchd2.ino.bin` on any web server as
   `<server URL>/api/ota/<ieee>/app.bin`. `<ieee>` is the switch's IEEE address without
   `0x`, e.g. `404ccafffe462c7c`.
3. In Zigbee2MQTT → **Exposes**, set **update** to `1`.

The switch restarts into the updater, downloads and installs the firmware, and starts it.
If anything fails, it starts the old firmware again.
