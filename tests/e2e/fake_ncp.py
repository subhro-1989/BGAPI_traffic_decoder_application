"""Fake NCP on a pty: answers every command at its exact API size (result 0),
sends a boot event, scanner reports, and 3 junk bytes once."""
import json, os, sys, time, struct, termios, tty, select
defs = json.load(open(os.path.join(os.path.dirname(__file__), "defs.json")))["msgs"]
rsp = {d[0]: d for d in defs if d[1] == 1}
evt = {d[2]: d for d in defs if d[1] == 2}
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY); tty.setraw(fd)
def frame(d, arr=0, fill=0):
    n = d[3] + (arr if d[4] else 0)
    hdr = d[0] | ((n & 0xff) << 8) | ((n & 0x700) >> 8)
    p = bytearray([fill] * n)
    if d[4]: p[d[3] - d[4]] = arr
    if d[1] == 1: p[0:2] = b"\0\0"
    return struct.pack("<I", hdr) + bytes(p)
os.write(fd, frame(evt["evt_system_boot"]))
buf = bytearray(); t_adv = time.time(); junk_done = False; t0 = time.time()
while time.time() - t0 < float(sys.argv[2]):
    r, _, _ = select.select([fd], [], [], 0.02)
    if r:
        buf += os.read(fd, 4096)
    while len(buf) >= 4:
        hdr = struct.unpack("<I", buf[:4])[0]; n = ((hdr & 7) << 8) | ((hdr >> 8) & 0xff)
        if len(buf) < 4 + n: break
        del buf[:4 + n]
        mid = hdr & 0xffff00f8
        d = rsp.get(mid)
        os.write(fd, frame(d, 2, 0x41) if d else struct.pack("<I", mid | (2 << 8)) + b"\x0f\x00")
    if time.time() - t_adv > 0.4:
        t_adv = time.time()
        os.write(fd, frame(evt["evt_scanner_legacy_advertisement_report"], 11, 0x42))
        if not junk_done and time.time() - t0 > 2:
            os.write(fd, b"\x55\x13\x37"); junk_done = True
