# What the tag broadcasts

## Adverts and messages

An **advert** is a Bluetooth LE advertising packet: a short broadcast on the three
advertising channels that any listener in range hears, with no connection. Bluetooth fixes
its type, which says what a listener may do in reply:

- **Non-connectable** (`ADV_NONCONN_IND`): data only. Beacon mode, and the info packet
  outside connect mode.
  The tag transmits and goes back to sleep.
- **Connectable** (`ADV_IND`): data, and an invitation to connect. Connect mode. After each
  one the tag listens briefly for a connect request, so these cost more than
  non-connectable adverts.

Each advert carries up to 31 bytes of payload. Ours holds a **message**: a manufacturer
data block whose company ID and first byte say what it is. The tag repeats the current
message in every advert until it has a new one, so one message goes out in several adverts:

| | New message | Adverts per message |
|---|---|---|
| Beacon mode | a reading every 60 s | 3, 1.5 s apart, then silence |
| Connect mode | a reading every 5 s | about 5, one a second |
| Button 2 info | once per press | about 150, every 100 ms for 15 s; in connect mode it rides the connectable adverts, one a second |

`sensortag-watch` prints each message once; `--all` also prints every repeat advert heard.

## Messages

All little-endian. Company 0xFFFF is the Bluetooth SIG's ID for testing; we're not
registered, and other devices may use it too.

The first byte of our messages holds the **format** in its low nibble and **flags** in its
high nibble:

| Bits | Meaning |
|---|---|
| 0–3 | format: 1 reading, 2 info; 0 and 3–15 unused |
| 4 | connect mode (so a connect-mode reading starts 0x11) |
| 5–7 | reserved, sent as 0 |

Compatibility rules, so old listeners keep working with newer firmware:

- Fields are only appended to a format, never moved or resized. Decoders accept messages
  longer than they know and ignore the extra bytes.
- Decoders ignore flags they don't know. New flags take the reserved bits.
- A change that can't be appended gets a new format number. Older decoders report it as
  `"unknown"` (from tags they already know, or were asked for by address).

### Reading (company 0xFFFF, format 1), 10 bytes

| Bytes | Field | Decoding |
|---|---|---|
| 1 | sequence | 0–255, one per reading, wraps |
| 2 | HDC1000 temperature | `raw / 65536 × 165 − 40` °C |
| 2 | HDC1000 humidity | `(raw & ~3) / 65536 × 100` %RH |
| 2 | OPT3001 light | `(raw & 0x0FFF) × 0.01 × 2^(raw >> 12)` lux |
| 2 | battery | `raw / 256` V (8.8 fixed point) |

0xFFFF for a sensor means it didn't answer at that reading. A gap in the sequence means
readings were sent but not heard; the watcher reports it as `missed`.
Readings taken during the 15 s info period are never sent; the watcher doesn't count that
gap as missed.

### Info (company 0xFFFF, format 2), 22 bytes

Sent in place of readings for 15 s after a button 2 press.

| Bytes | Field |
|---|---|
| 1 | button 2 presses since boot |
| 2 | app version × 100 (150 = 1.50) |
| 4 | git commit, first 8 hex digits |
| 1 | 1 if built with uncommitted changes |
| 3 | BLE-Stack version (2.2.8) |
| 4 | stack start address in flash (0x10000) |
| 1 | self-test bits: tmp, hum, opt, bar, mov, mag, flash (bit 0 first) |
| 1 | reset source: power on, reset pin, VDDS loss, VDD loss, VDDR loss, clock loss, software reset, warm reset, wake from shutdown |
| 4 | uptime in seconds |

`tmp` never passes on this board: Rev 1.5.1 has no TMP007. A firmware update ends in a
software reset.

### Stock (company 0x000D, first byte 0x03)

What TI's firmware sends: device type 0x03, a key-data ID, then the button state. No
readings. Seeing it means the tag runs TI's image, e.g. after a factory reset.

## Stock TI firmware compared

| | TI 1.50 | Ours |
|---|---|---|
| Most of the time | silent, invisible | beacon mode: a reading a minute, non-connectable |
| Power button | 3 minutes of connectable adverts every 100 ms, then silent again | toggles connect mode: 3 minutes of connectable adverts every second carrying readings, then back to beacon mode |
| Advert content | button state; name "CC2650 SensorTag" in the scan response | a reading or info message |
| Getting readings | connect, switch each sensor on over GATT, subscribe | listen; or connect as with stock |
| GATT services | all sensors, IR temperature, register access, OAD | the same, minus IR temperature and register access |
| Both buttons 6 s | factory reset | factory reset |
