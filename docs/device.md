# Field notes on the CC2650 SensorTag

What we've learned running one with our firmware. Messages and adverts: `protocol.md`.
Writing programs against it: `programming.md`.

## The board

- CC2650STK rev 1.5.1, CR2032 coin cell.
- Sensors: HDC1000 (temperature, humidity), OPT3001 (light), BMP280 (pressure), MPU9250
  (motion, magnetometer), plus external flash, which holds update images and the factory
  image. No TMP007 (IR temperature) on this revision, so its self-test bit never passes.
- Only temperature, humidity, light and battery go into the beacon.

## Buttons and LEDs

- **Power button** (button 1): toggles connect mode. It doesn't reboot anything.
- **Button 2**: on the right when you look at the side with the LED. TI's code calls it
  the left key. Blinks red 3 times and sends the info packet for 15 s.
- **Both buttons for 6 s**: factory reset. Blinks red 10 times, then the tag boots TI's
  factory image from external flash and sends stock messages.
- **Green blink** about every second while connectable advertising is on. It follows the
  radio, not our mode flag, so it means "connectable now".
- **Quiet during an update**: no blinks while an image is being received.
- At boot the code blinks 5 times for the self-test (green when all pass, red otherwise)
  and 5 green when the radio starts.

## Modes and timing

| | Beacon mode (normal) | Connect mode |
|---|---|---|
| Entered by | boot, power button, end of the window | power button |
| Reading | every 60 s | every 5 s |
| Adverts | 3 non-connectable, 1.5 s apart, then radio off | connectable, one a second |
| Lasts | indefinitely | 3 minutes (TI's limited-discoverable 180 s) |

- The first reading goes out about a second after boot; nothing is advertised before it.
  Its temperature and humidity are empty (`--`): the HDC1000 doesn't answer that first read
  (seen twice, at boot; other boots weren't watched early enough). Light and battery are
  fine, and the next reading is complete.
- After a disconnect, the stack resumes connectable advertising by itself and the tag
  stays in connect mode.
- With a good adapter listening (below), `missed` has stayed at 0.

## Battery

Around 3.0 V on a fresh cell. Readings taken just after radio activity can dip by 0.05 V
(2.93–2.98 V seen) and recover by the next reading; a single low value means nothing.

A CR2032 holds near 3.0 V for most of its life, then falls off a knee, so the voltage says
little until the end. Judge it on the trend over a day, not single readings:

| Typical reading | Meaning |
|---|---|
| 2.9 V and up | healthy; most of the cell's life is spent here |
| below 2.8 V | past the knee; order a cell, weeks to a couple of months left |
| 2.7 V | replace: the HDC1000's minimum supply, so temperature and humidity start coming back empty |

Expected life of a new cell: an estimate, not a measurement. The
radio costs under 1 µA on average (3 adverts a minute); the floor is the sleep current of
the chip and sensors, which we haven't measured. At 5–10 µA a 220 mAh cell lasts 2–4
years; at 20 µA, about a year. Under a year means something isn't sleeping; worth
measuring then (`ideas.md`). Connect mode and button presses don't change this noticeably.

## Firmware updates (OAD)

The tag must be in connect mode (green blinking). Then, from `~/workspace/sensortag`:

    tools/oad_send.py F0:F8:F2:01:02:03 build/app_oad/sensortag_cc2650stk_app_oad.bin

- **App-only image** (`build.py app --oad`), when only `src/app/` changed: 3,700 blocks,
  about 4.5 minutes.
- **Merged image** (`build.py oad`, app and stack), needed after a factory reset or a stack
  change: 7,470 blocks, about 5.5 minutes. TI's prebuilt image: 7,680 blocks, 6.5 minutes.
- The sender keeps 8 blocks in flight (`OAD_WINDOW=1` for the slow one-at-a-time mode).
- At the end the tag checks the CRC, reboots into the image and reports a software reset.
  A failed or interrupted transfer leaves the running firmware untouched; just retry.
- Check the result with a button 2 press: the info line shows the git commit.

Quirks seen so far, causes unknown:

- The first attempt or two can fail while connecting (bleak `TimeoutError` during service
  discovery). Retrying works.
- In a connect window that had already seen failed attempts, the sender
  connected, sent the start request and the tag disconnected at once (`tag didn't start:
  disconnect`), several times. Pressing the power button and retrying worked. Next time,
  run `btmon -w /tmp/oad.btsnoop` as root during an attempt to get the disconnect reason.
- Another update: started it in connect mode; once connected (green blinking
  stops) a second power-button press seemed to let it proceed. The code gives no reason a
  press would matter. The sender now prints how long the tag takes to accept an image;
  note that number when this recurs.

## Flash budget

The app gets 0xF010 bytes. TI's own app leaves 702 bytes free; ours fits only because the
IR temperature and register-access services are built out (about 2 KB free).

## The Linux side

- A TP-Link UB500 USB dongle (RTL8761B) hears the tag well. The onboard Bluetooth of one
  MediaTek MT7927 Wi-Fi card heard nothing at all; if `sensortag-watch` hears nothing
  either, try another adapter before suspecting the tag.
- bleak scans only hci0 unless told otherwise; the watcher listens on every adapter so a
  deaf one can't hide the tag.
- bleak asks BlueZ for duplicate filtering by default, and the adapter then drops repeat
  adverts from a device, sometimes whole readings (2 of 6 lost in connect mode). The library
  turns it off (`DuplicateData`), which delivers about five times as many adverts.
- Several listeners can run at once.
