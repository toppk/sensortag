# Programming with the SensorTag

The tag broadcasts; nothing connects to it. A program listens for its adverts with
`sensortag.py`, an asyncio module over bleak (BlueZ). `sensortag-watch` is the command
line built on the same module.

## Where it can run

On a Linux machine with a working Bluetooth adapter in range of the tag (`device.md`).
Several listeners can run at once; BlueZ shares the scan between them. It needs `bleak`
(`pip install --user bleak`) and access to the system D-Bus, which a desktop user has; a
service account is untested.

Not Python, or another host: run `sensortag-watch --json` and read one JSON object per line.

`sensortag.py` is one file with no dependency but bleak; copying it into your project
works. The message format only grows compatibly (`protocol.md`), and a copy that meets a
newer format says so rather than misreading it.

## The interface

```python
import asyncio
import sys

sys.path.insert(0, "/path/to/sensortag")  # or copy sensortag.py next to your code
import sensortag


async def main():
    async with sensortag.Watcher() as w:  # starts scanning on every adapter; stops on exit
        async for p in w:  # each new packet, a dict
            if p["type"] == "reading":
                print(p["addr"], p["temp_c"], p["humidity_pct"], p["lux"], p["battery_v"])

asyncio.run(main())
```

`Watcher(addrs=(), adapters=None, every=False)`

- `addrs`: only these tags, e.g. `["F0:F8:F2:01:02:03"]`. Default: any tag sending our formats.
- `adapters`: e.g. `["hci1"]`. Default: all, so one deaf adapter can't hide a tag.
- `every`: also yield repeated copies of a packet. You don't want this.

Besides iterating, `w.tags[addr]` always holds that tag's latest `"reading"` and `"info"`
packets, plus `"heard"` (time of the last advert of any kind), `"count"` (new packets) and
`"missed"` (readings lost so far). A program that only wants the current values can keep
the watcher open and read `w.tags` when it needs them; packets nobody iterates are dropped
after 1,000.

`await sensortag.reading(addrs=())` opens a watcher, returns the next reading and closes.
In beacon mode that takes up to a minute, so bound it: `async with asyncio.timeout(90):`.
For anything long-running, keep one `Watcher` open instead.

## Packets

Every packet has `time` (Unix seconds, when first heard), `addr`, `rssi`, `via` (adapter)
and `type`.

**reading**

| key | meaning |
|---|---|
| `seq` | 0–255, wraps; one per reading |
| `temp_c`, `humidity_pct`, `lux` | `None` if that sensor didn't answer this time; keep the last good value. The first reading after a boot has no temperature or humidity |
| `battery_v` | coin cell; dips briefly under radio load |
| `mode` | `"beacon"` or `"connect"`, from the message's connect-mode flag |
| `missed` | readings between this one and the last that weren't heard; absent if none |

**info**, for 15 s after a button 2 press: `presses`, `app_version`, `git`, `dirty`,
`sdk_version`, `stack_start`, `self_test` (list of sensors that passed), `reset_source`,
`uptime_s`.

**stock**: the tag runs TI's firmware and sends no readings. `keys` is its button state.

**unknown**: a message format this copy of `sensortag.py` doesn't know, from newer firmware;
`format`, `flags` and the raw `data` in hex. Update `sensortag.py`. Ignore types you don't
handle.

## Timing

- Beacon mode (normal): a reading once a minute.
- Connect mode (after a power-button press, 3 minutes): a reading every 5 s.
- No new reading for 90 s means something is wrong: the tag, its battery, or the adapter.
  Check `time.time() - w.tags[addr]["reading"]["time"]`.

## Example: keep the latest reading for a hub

```python
async def sensor_task(state):
    w = sensortag.Watcher(["F0:F8:F2:01:02:03"])  # one object, so missed readings count across restarts
    while True:
        try:
            async with w:
                while True:
                    async with asyncio.timeout(180):
                        p = await anext(w)
                    if p["type"] == "reading":
                        state["sensortag"] = p
        except TimeoutError:
            print("sensortag: nothing heard for 3 minutes; restarting the scan")
        except Exception as e:  # adapter unplugged, BlueZ restarted
            print("sensortag:", e or type(e).__name__)
            await asyncio.sleep(30)
```

Restart straight away after a silence: a pause there can swallow the next reading. A
`Watcher` can be entered again after it exits; its `tags` and sequence history carry over,
so a gap across the restart still counts as missed. A gap too large for the time elapsed
(at least 5 s per reading) is taken as a reboot and not counted.

Start it with `asyncio.create_task(sensor_task(state))` and read `state["sensortag"]`,
treating it as stale when its `time` is over 90 s old.
