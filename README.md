# sensortag

Custom firmware for the TI CC2650 SensorTag that turns it into an unattended
Bluetooth LE beacon. Once a minute it broadcasts temperature, humidity, light and battery
voltage, then switches the radio off, so it runs for years on its coin cell. Anything in
range can listen; nothing connects.

Also here:

- `build.py`: builds the firmware on Linux from TI's BLE-Stack 2.2.8, without Code
  Composer Studio.
- `tools/oad_send.py`: sends firmware updates over the air from Linux (BlueZ). TI's BTool
  needs extra hardware for this.
- `sensortag.py` and `sensortag-watch`: an asyncio listener library and a command line
  watcher.

Tested on one CC2650STK rev 1.5.1 (the revision without the TMP007 IR sensor), built and
sent from Fedora. Not affiliated with or endorsed by Texas Instruments.

## Getting started

1. Set up the toolchain: [`docs/setup.md`](docs/setup.md). TI's SDK installer needs Windows
   once; everything else is Linux.
2. Build:

       ./build.py stack       # BLE stack; also sets the app/stack boundary in build/config
       ./build.py app --oad   # our app, for over-the-air update
       ./build.py oad         # app + stack in one update image, build/oad_merged.bin

3. Press the tag's power button (green blinking: connect mode), then send the image:

       tools/oad_send.py --info F0:F8:F2:01:02:03            # read-only: what's running
       tools/oad_send.py --dry-run build/oad_merged.bin       # against a simulated tag
       tools/oad_send.py F0:F8:F2:01:02:03 build/oad_merged.bin

   It takes about 5.5 minutes. The first update must be the merged image (app and stack);
   after that `build/app_oad/sensortag_cc2650stk_app_oad.bin` is enough when only `src/app/`
   changed. Details and known quirks: [`docs/device.md`](docs/device.md).

4. Listen:

       ./sensortag-watch                  # a line per new reading
       ./sensortag-watch --json --once    # one JSON reading, for scripts
       ./sensortag-watch --tui            # live table

## Stock firmware

Our changes are optional. To run TI's SensorTag app instead:

- **TI's prebuilt image:** `./build.py ti` turns the SDK's own binary (firmware 1.50) into
  `build/oad_ti_prebuilt.bin`. Send it like any update.
- **TI's source, built here:** `./build.py app --oad --stock` and `./build.py oad --stock`
  build the SDK's SensorTag app unchanged, giving `build/oad_merged_stock.bin`. A starting
  point for your own changes.
- **Factory reset:** holding both buttons for 6 s boots the factory image kept in the tag's
  external flash, which is TI's firmware unless something replaced it.

## What our firmware does

- **Beacon mode** (normal): once a minute it reads the HDC1000 (temperature, humidity),
  OPT3001 (light) and battery, sends the reading in 3 non-connectable adverts 1.5 s apart,
  and keeps the radio off for the rest of the minute.
- **Connect mode**: the power button opens TI's 3-minute connectable window, for updates
  or TI's phone app. Readings come every 5 s. Another press or the timeout returns to
  beacon mode.
- **Button 2** sends a system-info message for 15 s: firmware and stack version, git
  commit, self-test, reset cause, uptime.

To make room, the app is built without the IR temperature service and the register
service (raw I2C access for any connected client). `src/app/` holds TI's files as modified
by us; anything not there comes from the SDK unchanged.

## Docs

- [`docs/setup.md`](docs/setup.md): installing the toolchain, and what replaces CCS.
- [`docs/protocol.md`](docs/protocol.md): the messages, adverts, and stock vs ours.
- [`docs/programming.md`](docs/programming.md): listening from your own programs.
- [`docs/device.md`](docs/device.md): buttons, LEDs, timing, battery, updates, the Linux
  side.
- [`docs/ideas.md`](docs/ideas.md): what could come next.

## License

Files from TI's SDK (`src/app/`, and `tools/` except `oad_send.py`) keep TI's BSD-3-Clause
licence in their headers. Everything else is under the BSD-3-Clause licence in `LICENSE`.
TI's SDK itself isn't included; you download it from TI.
