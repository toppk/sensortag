"""Async listener for SensorTag beacons; sensortag-watch is the command line on top of it.

    import sensortag
    async with sensortag.Watcher() as w:
        async for p in w:  # each new reading or info packet, the dict sensortag-watch --json prints
            ...

docs/programming.md is the guide. Needs bleak.

Our firmware (src/app/sensortag.c) advertises manufacturer data under company 0xFFFF,
all little-endian. The first byte holds the format in its low nibble and flags in its high
nibble (0x10: connect mode; the rest reserved as 0). Fields are only ever appended, so
decoders ignore extra bytes and unknown flags; a layout change gets a new format number,
which older decoders report as "unknown".
  0x01 reading  sequence, HDC1000 raw temperature and humidity, OPT3001 raw light
                (0xFFFF: sensor didn't answer), battery as 8.8 fixed-point volts.
                Beacon mode: taken once a minute, sent as 3 adverts 1.5 s apart.
                Connect mode: taken every 5 s, sent every second.
                A sequence gap means readings were sent but not heard; it's reported as "missed".
                Readings taken while the info packet is up aren't sent, and aren't counted.
  0x02 info     sent for 15 s after a button 2 press: press count, app version x100,
                git commit, dirty flag, BLE SDK version (stack), stack start address,
                self-test map, reset source, uptime in seconds.
Stock TI firmware (company 0x000D) is reported as such, without readings.
Every packet dict has "type": "reading", "info", "stock" or "unknown".
"""
import asyncio
import os
import struct
import time
from contextlib import AsyncExitStack

OUR_COMPANY, TI_COMPANY = 0xFFFF, 0x000D
QUEUE_MAX = 1000
FLAG_CONNECT_MODE = 0x1
RESETS = ["power on", "reset pin", "VDDS loss", "VDD loss", "VDDR loss", "clock loss",
          "software reset", "warm reset", "wake from shutdown"]
TESTS = ["tmp", "hum", "opt", "bar", "mov", "mag", "flash"]


def list_adapters():
    """Bluetooth adapters present, as hciN names in order."""
    try:
        names = os.listdir("/sys/class/bluetooth")
    except FileNotFoundError:
        return []
    return sorted((h for h in names if h[3:].isdigit()), key=lambda h: int(h[3:]))


def decode(mfr):
    """Manufacturer data dict from bleak -> packet dict, or None if it isn't a SensorTag."""
    d = mfr.get(OUR_COMPANY)
    fmt, flags = (d[0] & 0x0F, d[0] >> 4) if d else (None, 0)
    mode = "connect" if flags & FLAG_CONNECT_MODE else "beacon"
    if fmt == 0x01 and len(d) >= 10:
        seq, t, h, lux, batt = struct.unpack_from("<BHHHH", d, 1)
        return {
            "type": "reading",
            "mode": mode,
            "seq": seq,
            "temp_c": None if t == 0xFFFF else round(t / 65536 * 165 - 40, 2),
            "humidity_pct": None if h == 0xFFFF else round((h & ~3) / 65536 * 100, 1),
            "lux": None if lux == 0xFFFF else round((lux & 0x0FFF) * 0.01 * (1 << (lux >> 12)), 2),
            "battery_v": round(batt / 256, 3),
        }
    if fmt == 0x02 and len(d) >= 22:
        presses, ver, git, dirty, sdk, stack, test, reset, up = struct.unpack_from("<BHIB3sIBBI", d, 1)
        return {
            "type": "info",
            "mode": mode,
            "presses": presses,
            "app_version": f"{ver // 100}.{ver % 100:02d}",
            "git": f"{git:08x}",
            "dirty": bool(dirty),
            "sdk_version": ".".join(str(x) for x in sdk),
            "stack_start": f"{stack:#x}",
            "self_test": [n for i, n in enumerate(TESTS) if test & 1 << i],
            "reset_source": RESETS[reset] if reset < len(RESETS) else str(reset),
            "uptime_s": up,
        }
    if d:
        return {"type": "unknown", "format": fmt, "flags": flags, "data": bytes(d).hex()}
    d = mfr.get(TI_COMPANY)
    if d and len(d) >= 3 and d[0] == 0x03:
        return {"type": "stock", "keys": d[2]}
    return None


class Watcher:
    """Scan, decode and keep each tag's latest state; iterate it for new packets.

    addrs: only these tags (default any). adapters: hciN names (default all, so one deaf
    adapter can't hide a tag). every: also yield repeats of a packet already yielded.
    """

    def __init__(self, addrs=(), adapters=None, every=False):
        self.addrs = {a.upper() for a in addrs}
        self.adapters = list(adapters) if adapters else list_adapters()
        self.every = every
        self.tags = {}  # addr -> {"reading": ..., "info": ..., "stock": ..., "heard": t, "rssi": n, "count": n, "missed": n}
        self.queue = asyncio.Queue(maxsize=QUEUE_MAX)  # (fresh, packet); oldest dropped if nobody reads
        self.reset_heard()

    def reset_heard(self):
        self.heard = {h: [0, set()] for h in self.adapters}  # adverts and devices per adapter since the last status

    def seen(self, adapter, dev, adv):
        self.heard[adapter][0] += 1
        self.heard[adapter][1].add(dev.address)
        if self.addrs and dev.address.upper() not in self.addrs:
            return
        p = decode(adv.manufacturer_data)
        if p is None:
            return
        if p["type"] == "unknown" and not self.addrs and dev.address not in self.tags:
            return  # 0xFFFF is the shared test company ID; only tags we know or were asked for
        tag = self.tags.setdefault(dev.address, {"count": 0, "missed": 0})
        prev = tag.get(p["type"])
        key = {"reading": "seq", "info": "presses", "stock": "keys", "unknown": "data"}[p["type"]]
        fresh = prev is None or prev[key] != p[key]
        now = round(time.time(), 1)
        if fresh and p["type"] == "info":
            tag["info_since"] = True
        if fresh and prev and p["type"] == "reading":
            gap = (p["seq"] - prev["seq"] - 1) & 0xFF
            if tag.pop("info_since", False):
                gap = 0  # readings taken while the info packet was up were never sent
            if (gap + 1) * 4 > now - tag["read_at"]:
                gap = 0  # more readings than fit in the time (5 s each, less arrival jitter): the tag rebooted
            if 0 < gap < 128:  # beyond that the 8-bit sequence is ambiguous
                p["missed"] = gap
                tag["missed"] += gap
        if fresh and p["type"] == "reading":
            tag["read_at"] = now  # first copy of this seq; repeats arrive up to 3.5 s later
        p = {"time": now, "addr": dev.address, "rssi": adv.rssi, "via": adapter, **p}
        tag.update({p["type"]: p, "heard": now, "rssi": adv.rssi, "count": tag["count"] + fresh})
        if p["type"] != "stock":
            tag.pop("stock", None)
        if self.queue.full():
            self.queue.get_nowait()
        self.queue.put_nowait((fresh, p))

    def status(self, quiet_s):
        """A line saying what the scanner heard while nothing new came from a SensorTag."""
        per = ", ".join(f"{h} {n} adverts from {len(d)} devices" if n else f"{h} heard nothing (deaf?)"
                        for h, (n, d) in self.heard.items())
        self.reset_heard()
        last = max((t["read_at"] for t in self.tags.values() if "read_at" in t), default=None)
        tag = "no SensorTag reading yet" if last is None else f"last new reading {max(0, time.time() - last):.0f}s ago"
        return f"in {quiet_s:.0f}s: {per}; {tag} (beacon mode sends once a minute)"

    async def __aenter__(self):
        from bleak import BleakScanner
        if not self.adapters:
            raise RuntimeError("no Bluetooth adapters in /sys/class/bluetooth")
        self._stack = AsyncExitStack()
        try:
            for h in self.adapters:
                cb = (lambda h: lambda d, a: self.seen(h, d, a))(h)  # bleak wants exactly two parameters
                # without DuplicateData the adapter drops repeat adverts, and with them whole readings
                await self._stack.enter_async_context(
                    BleakScanner(cb, bluez={"adapter": h, "filters": {"DuplicateData": True}}))
        except BaseException:
            await self._stack.aclose()
            raise
        return self

    async def __aexit__(self, *exc):
        await self._stack.aclose()

    def __aiter__(self):
        return self

    async def __anext__(self):
        while True:
            fresh, p = await self.queue.get()
            if fresh or self.every:
                return p


async def reading(addrs=(), adapters=None):
    """The next new reading from any of addrs (default any tag); wrap in asyncio.timeout()."""
    async with Watcher(addrs, adapters) as w:
        async for p in w:
            if p["type"] == "reading":
                return p
