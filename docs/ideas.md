# Ideas for later

Nothing here is needed; the tag works. Each firmware change costs an update (about 4.5
minutes in connect mode), so batch them. Changes to the message format follow the rules in
`protocol.md`, so existing listeners keep working.

## Firmware

- **Retry the first HDC1000 read.** The first reading after boot has no temperature or
  humidity. If a read fails, wait one conversion time and read again.
- **Use the spare flags.** Three bits in each message's first byte are free
  (`protocol.md`). Candidates: "a sensor read failed", "battery low".
- **Measure the battery at rest.** It's read right after radio activity and dips 0.05 V.
  Reading it before the burst, or averaging, would make the battery trend cleaner.
- **Watchdog.** If the app ever hangs, the tag goes silent until the cell is pulled. The
  CC2650 has a watchdog timer that would reboot it; listeners would see a reboot (sequence
  restarting) instead of silence.
- **Pressure.** The BMP280 is fitted and passes self-test. Four more bytes appended to the
  reading (format 1 stays compatible); costs a conversion per minute.
- **Our own app version.** The info packet still reports TI's 1.50; the git commit is what
  identifies our builds. A version of our own would read better.
- **Low-battery mode.** Below a threshold, read every 5 minutes instead of every minute.
- **Factory image.** Both buttons for 6 s restores TI's image, after which a merged update
  is needed. Putting our image in the factory slot would make a reset recover our firmware,
  at the risk of having no known-good TI image to fall back on.

## Updates (OAD)

- **The stalls at the start of an update.** Sometimes the tag disconnects right after the
  sender asks to start, or seems to wait until the power button is pressed again
  (`device.md`). The sender now prints how long acceptance takes. Next time, also capture
  `btmon -w /tmp/oad.btsnoop` (root) to get the disconnect reason.
- **`--adapter` for `oad_send.py`.** It uses bleak's default adapter, which is fine on
  a machine with one.

## Host side

- **Passive scanning.** bleak scans actively, which makes the tag answer scan requests
  during connect mode. BlueZ's passive scanning needs advertisement monitor patterns;
  not worth it for 3 minutes of connect mode, but would be for more tags.
- **Measure the sleep current.** A multimeter in µA mode in series with the cell gives
  the real average and turns the battery-life estimate in `device.md` into a number.
