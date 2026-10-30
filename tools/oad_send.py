#!/usr/bin/python3
"""Send an off-chip OAD image to a CC26xx running TI's BLE-Stack 2.2 OAD profile (what BTool does).

usage: oad_send.py --info ADDR            read-only: firmware revision and OAD characteristics
       oad_send.py --dry-run IMAGE.bin    send to a simulated tag that mirrors TI's oad.c
       oad_send.py ADDR IMAGE.bin         send for real; the tag reboots into the image when done

IMAGE.bin comes from oad_image_tool.py (build.py oad / app --oad): 16-byte metadata, then the image.
"""
import asyncio
import os
import struct
import sys
import time

BASE = "f000{:04x}-0451-4000-b000-000000000000"
IDENTIFY, BLOCK, COUNT, STATUS = (BASE.format(u) for u in (0xFFC1, 0xFFC2, 0xFFC3, 0xFFC4))
FW_REV = "00002a26-0000-1000-8000-00805f9b34fb"
BLOCK_SIZE = 16
WINDOW = int(os.environ.get("OAD_WINDOW", 8))  # blocks in flight; the tag queues writes and stores them in order
STATUSES = {0: "success", 1: "CRC error", 2: "flash error", 3: "block out of order"}


def crc16(data):  # the target's crcCalcDL: CRC-16/XMODEM over everything after the CRC fields
    import binascii
    return binascii.crc_hqx(data, 0)


def parse(img):
    crc, shdw, ver, words, uid, addr, typ, stat = struct.unpack("<HHHH4sHBB", img[:16])
    if len(img) != words * 4 or words % (BLOCK_SIZE // 4):
        sys.exit(f"oad_send: image is {len(img)} bytes but its header says {words * 4}")
    if crc16(img[4:]) != crc:
        sys.exit(f"oad_send: header CRC {crc:#06x} doesn't match the image")
    print(f"image: version {ver}, {len(img)} bytes, {len(img) // BLOCK_SIZE} blocks, "
          f"address {addr * 4:#x}, type {typ}, crc {crc:#06x}")
    return len(img) // BLOCK_SIZE


class SimTag:
    """TI's oad.c and oad_target_external_flash.c checks, against a byte buffer."""

    def __init__(self, events):
        self.events, self.flash, self.total, self.next = events, bytearray(), 0, 0

    async def identify(self, hdr):
        ver, words, uid, addr, typ = struct.unpack("<HH4sHB", hdr[4:15])
        self.total, self.next = words // (BLOCK_SIZE // 4), 0
        if not 0 < self.total <= 0x20000 // BLOCK_SIZE or addr * 4 != 0x1000 and typ == 1 \
                or typ != 3 and not 0x1000 <= addr * 4 <= 0x1F000:
            self.events.put_nowait(("reject", b""))
            return
        self.flash = bytearray(b"\xff" * self.total * BLOCK_SIZE)
        self.events.put_nowait(("req", 0))

    async def block(self, data):
        n = int.from_bytes(data[:2], "little")
        if n != self.next:
            self.events.put_nowait(("status", 3))
            return
        self.flash[n * BLOCK_SIZE:(n + 1) * BLOCK_SIZE] = data[2:]
        self.next += 1
        if self.next < self.total:
            self.events.put_nowait(("req", self.next))
            return
        crc = struct.unpack("<H", self.flash[:2])[0]
        ok = crc not in (0, 0xFFFF) and crc16(bytes(self.flash[4:])) == crc
        self.events.put_nowait(("status", 0 if ok else 1))
        if ok:
            self.events.put_nowait(("disconnect", None))


class Tag:
    def __init__(self, client, events):
        self.client, self.events = client, events

    async def start(self):
        def on(kind, conv):
            return lambda _, d: self.events.put_nowait((kind, conv(d)))
        await self.client.start_notify(IDENTIFY, on("reject", bytes))
        await self.client.start_notify(BLOCK, on("req", lambda d: int.from_bytes(d[:2], "little")))
        await self.client.start_notify(STATUS, on("status", lambda d: d[0]))

    async def identify(self, hdr):
        await self.client.write_gatt_char(IDENTIFY, hdr, response=True)

    async def block(self, data):
        await self.client.write_gatt_char(BLOCK, data, response=False)  # as BTool does; the tag's next-block request paces us


async def next_event(events, timeout):
    try:
        return await asyncio.wait_for(events.get(), timeout)
    except asyncio.TimeoutError:
        return ("timeout", None)


async def send(tag, events, img, total):
    print("waiting for the tag to accept the image", flush=True)
    t0 = time.monotonic()
    await tag.identify(img[:16])
    kind, val = await next_event(events, 60)  # the tag erases the external flash slot first
    if kind != "req" or val != 0:
        sys.exit(f"oad_send: tag didn't start after {time.monotonic() - t0:.1f}s: "
                 f"{kind} {val.hex() if isinstance(val, bytes) else val}"
                 + (" (rejected: it reports its current version, length, id)" if kind == "reject" else ""))
    print(f"accepted after {time.monotonic() - t0:.1f}s", flush=True)
    t0, step, sent, done, shown = time.monotonic(), max(total // 20, 1), 0, 0, 0
    while sent < total:
        while sent < total and sent - done < WINDOW:
            await tag.block(sent.to_bytes(2, "little") + img[sent * BLOCK_SIZE:(sent + 1) * BLOCK_SIZE])
            sent += 1
        if sent == total:
            break
        kind, val = await next_event(events, 2)  # the tag's request for block k means it has stored blocks below k
        if kind == "req":
            done = max(done, val)
        elif kind == "timeout":  # a lost notification costs 2 s, not the update
            done = sent
        elif kind in ("status", "reject", "disconnect"):
            sys.exit(f"oad_send: aborted near block {done}: {kind} {STATUSES.get(val, val)}")
        if done // step > shown:
            shown, el = done // step, time.monotonic() - t0
            print(f"  {done}/{total} blocks, {el:.0f}s, about {el / done * (total - done):.0f}s left", flush=True)
    while True:
        kind, val = await next_event(events, 30)
        if kind == "status":
            print(f"tag reports: {STATUSES.get(val, val)}")
            if val != 0:
                sys.exit(1)
        elif kind in ("disconnect", "timeout"):
            print("tag disconnected; it is rebooting into the new image" if kind == "disconnect"
                  else "no reboot seen within 30 s; check the tag")
            return
        elif kind == "req":
            continue


async def real(addr, path, info):
    from bleak import BleakClient, BleakScanner
    events = asyncio.Queue()
    print(f"scanning for {addr} (press the tag's power button for connect mode)")
    dev = await BleakScanner.find_device_by_address(addr, timeout=30)
    if dev is None:
        sys.exit(f"oad_send: {addr} not found")
    async with BleakClient(dev, disconnected_callback=lambda _: events.put_nowait(("disconnect", None))) as c:
        fw = (await c.read_gatt_char(FW_REV)).decode(errors="replace").rstrip("\0")
        have = {ch.uuid for s in c.services for ch in s.characteristics}
        print(f"connected: firmware {fw}; OAD characteristics: "
              + ", ".join(f"{u[4:8].upper()}{'' if u in have else ' MISSING'}" for u in (IDENTIFY, BLOCK, COUNT, STATUS)))
        if info:
            return
        if not {IDENTIFY, BLOCK, STATUS} <= have:
            sys.exit("oad_send: tag lacks the OAD service")
        img = open(path, "rb").read()
        total = parse(img)
        tag = Tag(c, events)
        await tag.start()
        await send(tag, events, img, total)


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "--info":
        asyncio.run(real(a[1], None, True))
    elif len(a) == 2 and a[0] == "--dry-run":
        img = open(a[1], "rb").read()
        total = parse(img)
        events = asyncio.Queue()
        asyncio.run(send(SimTag(events), events, img, total))
    elif len(a) == 2 and not a[0].startswith("-"):
        asyncio.run(real(a[0], a[1], False))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
