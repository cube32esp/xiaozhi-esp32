# CUBE32-S3 Build Guide

Run all commands from the project root (`xiaozhi-esp32`).

## CUBE32-S3 build (A7670 modem: USB or UART selected at runtime)

```sh
idf.py -DBOARD=cube32-s3 set-target esp32s3
idf.py menuconfig
idf.py build
```

The `-DBOARD=cube32-s3` option loads `sdkconfig.defaults.cube32-s3` after the project and ESP32-S3 default configuration files.

There is a single configuration for every modem wiring. The modem connection is **Runtime** (`CONFIG_CUBE32_MODEM_CONNECTION_RUNTIME`): both the USB CDC-ACM and the UART drivers are built, and the connection type is chosen over BLE config (stored in NVS, applied after a restart). Until one is chosen, the BSP default applies, which is USB (`CUBE32_MODEM_RUNTIME_DEFAULT_CONN` in `cube32_config.h`). To lock a build to one type, pick **USB (fixed)** or **UART (fixed)** under **CUBE32 Board Configuration → Modem Configuration → Modem Connection Type** in `menuconfig`. The USB Host settings the USB driver needs are part of the base configuration.

The former `cube32-s3.4g` variant was removed; use `-DBOARD=cube32-s3` instead.

## Partition table (BSP 0.0.17)

Both variants use [partitions/cube32/16m.csv](../../../../partitions/cube32/16m.csv), a copy of the BSP's `partitions/cube32/16m.csv` (5 MB OTA slots, 64 KB `coredump`, `assets` at 0xA20000). Core dump to flash (ELF) is enabled so BSP safe mode can show the crash summary. Keep the file identical to the BSP's.

Moving a device that has the old `partitions/v2/16m.csv` layout needs a USB flash (bootloader, partition table, otadata, app, assets). Afterwards erase the stale assets data, which keeps `nvs`:

```sh
idf.py -p <PORT> erase-region 0xA20000 0x5E0000
```

## Notes

- `idf.py set-target` recreates the build configuration. Run the appropriate command for the desired board variant before opening `menuconfig` or building.
- In `menuconfig`, select **Xiaozhi Assistant → Board → CUBE32 S3 (cube32esp)** if it is not already selected.
- Do not use only `idf.py set-target esp32s3`: without `-DBOARD`, the CUBE32 board defaults are not loaded.
- Release builds use [config.json](config.json) (`sdkconfig_append`), not `sdkconfig.defaults.cube32-s3`. Keep the two in sync (the only intentional differences are `CONFIG_IDF_TARGET` and `CONFIG_PARTITION_TABLE_OFFSET`, which `config.json` gets from `"target"` and the default).

## Status display (240x240 and 320x320)

The panel is auto-detected by the BSP (240x240 ST7789, 240x320 ST7789, 320x320 ST7796S). Screens up to 240 px wide use the 16 px font for the second row, wider screens the 20 px font. Everything else is laid out from the runtime screen size.

```
row 1 (xiaozhi top bar)  [signal]      status / clock       [mute][battery icon]
row 2 (info strip)       A7671G USB      Online / MQTT lost      85%
```

- **Mode text** (left): `WiFi`, or `<modem model> USB` / `<modem model> UART` (`4G` until the model is detected).
- **Link state** (centre): `Online` (green), `MQTT lost` (red, MQTT broker link dropped or reconnect failed), `No link` (red, Wi-Fi not associated or modem PPP down), `Linking` (amber, during startup), `WiFi setup` (amber, Wi-Fi config mode).
- **Battery % and charging** (right): hidden when no battery is present. The % turns green while the PMU reports charging; the row-1 battery icon switches to the charging bolt as before. A full battery on USB power (PMU standby) is neither charging nor discharging.
- The signal icon on row 1 shows "no signal" when the modem PPP link is down, not only when the signal quality is unknown.
- Face mode: the eye area starts below the info strip (`Cube32AttachedLcdDisplay::GetTopInset()`).
- Everything is refreshed from the 1 s clock tick through `UpdateStatusBar()`.

The strip lives in [cube32_lvgl_display.cc](cube32_lvgl_display.cc); the board supplies the data through `SetStripInfoProvider()` in [cube32_s3_board.cc](cube32_s3_board.cc).

## Changes to upstream xiaozhi code

Keep these when upgrading xiaozhi from its original source (search for `CUBE32` in the files below). Everything else for this board is inside this folder.

| File | Change | Why |
|---|---|---|
| [main/protocols/protocol.h](../../../protocols/protocol.h) | Added `virtual bool IsServerLinkLost() const { return false; }` | Lets the UI ask whether the server link is lost |
| [main/protocols/mqtt_protocol.h](../../../protocols/mqtt_protocol.h) | Added `std::atomic<bool> link_lost_` and the `IsServerLinkLost()` override | MQTT link state for the strip |
| [main/protocols/mqtt_protocol.cc](../../../protocols/mqtt_protocol.cc) | `link_lost_ = true` in the MQTT `OnDisconnected` callback and when `mqtt_->Connect()` fails in `StartMqttClient()`; `link_lost_ = false` in `OnConnected` | Upstream only logs an MQTT disconnect; nothing else reports it |
| [main/application.h](../../../application.h) | Added `Application::IsServerLinkLost()` (forwards to `protocol_`) | Public accessor used by the strip |

Upstream behaviour that is left as is: after an MQTT drop the 60 s reconnect timer only runs while Idle, and a failed reconnect raises the usual "Error" alert with sound (`SetError(SERVER_NOT_CONNECTED)` is not guarded by `report_error`).

If the upstream names change (for example `UpdateStatusBar`, `top_bar_`, `LcdDisplay::SetupUI`), also re-check the overrides in `Cube32AttachedLcdDisplay` and `Cube32FaceDisplay`, which depend on the protected members `top_bar_`, `status_bar_`, `bottom_bar_`, `current_theme_` and `width_`.
