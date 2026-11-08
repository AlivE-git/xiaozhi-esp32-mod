# Seeed XIAO ESP32-S3 Plus

Xiaozhi voice assistant on a Seeed Studio XIAO ESP32-S3 Plus. This board is a
straight copy of [bread-compact-wifi](../../bread-compact-wifi/): identical
wiring, buttons, OLED and audio path, only the board identity differs.

- Board type reported by the firmware: `xiao-esp32s3-plus`
- Manufacturer: `seeed`
- Target: `esp32s3`
- Flash: 16 MB, partition table `partitions/v2/16m.csv` (project default,
  inherited from `sdkconfig.defaults`)

## Hardware

| Function | GPIO |
| --- | --- |
| Microphone I2S WS / SCK / DIN | 4 / 5 / 6 |
| Speaker I2S DOUT / BCLK / LRCK | 7 / 15 / 16 |
| OLED I2C SDA / SCL | 41 / 42 |
| Boot button | 0 |
| Capacitive touch pad (push-to-talk) | 1 (touch channel 1) |
| Volume up / down buttons | 2 / 8 |
| Built-in LED | 21 |
| Lamp (MCP demo) | 18 |

The OLED type is selected in `menuconfig` (SSD1306 128x32 by default, SSD1306
128x64 or SH1106 128x64 are also supported).

## Build

```sh
python scripts/build.py seeed/xiao-esp32s3-plus --name xiao-esp32s3-plus
```

Variant `xiao-esp32s3-plus-128x64` builds the same firmware for a 128x64
SSD1306 panel; use the Kconfig OLED type entry when building with `idf.py`
directly.

## Notes

- Audio runs in simplex I2S mode (separate microphone and speaker buses). To use
  a single duplex bus, comment out `AUDIO_I2S_METHOD_SIMPLEX` in
  [config.h](config.h).
- The board uses 16 MB flash. Override `CONFIG_ESPTOOLPY_FLASHSIZE_*` and
  `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` in `sdkconfig_append` if your module
  has a smaller flash chip.
- Push-to-talk uses the ESP32-S3 capacitive touch peripheral (`esp_driver_touch_sens`),
  not a GPIO input: touching the pad starts listening and releasing it stops listening.
  Touch channel N drives GPIO N on this chip, so the channel and GPIO must stay in sync
  (`TOUCH_BUTTON_CHANNEL` / `TOUCH_BUTTON_GPIO` in [config.h](config.h)).
- At boot the firmware scans the untouched pad three times and sets the active threshold
  to `TOUCH_BUTTON_THRESHOLD_RATIO` (2 %) of the measured baseline; the value is printed
  in the boot log. Lower the ratio if touches are missed, raise it if the pad triggers
  spuriously. `TOUCH_BUTTON_THRESHOLD_FALLBACK` is used if no baseline can be read.
