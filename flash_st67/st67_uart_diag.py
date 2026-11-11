#!/usr/bin/env python3
"""
st67_uart_diag.py - quick checks before flashing the ST67W611 over USB-UART.
It sends/reads raw bytes only. It never writes to the module's flash.

  python st67_uart_diag.py loopback COM18        # TX<->RX of the ADAPTER shorted, no module
  python st67_uart_diag.py listen   COM18 --baud 115200
  python st67_uart_diag.py probe    COM18 --baud 2000000

listen : prints every byte the module sends. Press EN reset (EN low then high)
         while it runs, in both BOOT=low and BOOT=high. If you never see a
         single byte in ANY case, TX/RX from the module is not reaching the
         adapter (wrong pin, swapped, no GND, no power, wrong voltage).
probe  : sends a burst of 0x55 sync bytes repeatedly and prints any reply.
         The 0x55 sync is the usual Bouffalo/QCC74x-style ROM handshake
         (an assumption: not verified against the ST67 documentation).
         Hold BOOT high, press/release EN while it runs.
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pip install pyserial")


def show(b):
    return b.hex(" ") + "   |" + "".join(chr(c) if 32 <= c < 127 else "." for c in b) + "|"


def loopback(a):
    s = serial.Serial(a.port, a.baud, timeout=1)
    pat = bytes(range(256)) * 4
    s.reset_input_buffer()
    s.write(pat)
    time.sleep(0.5)
    r = s.read(len(pat) + 10)
    ok = r == pat
    print(f"baud {a.baud}: sent {len(pat)}, got {len(r)}, match={ok}")
    if not ok:
        print("FAIL: short TX to RX on the adapter itself (no module). If this fails the "
              "adapter/driver can't do this baud (or TX/RX not shorted).")
    return 0 if ok else 1


def listen(a):
    s = serial.Serial(a.port, a.baud, timeout=0.2)
    s.reset_input_buffer()
    print(f"listening on {a.port} @ {a.baud} for {a.seconds}s - reset the module now (EN low->high)")
    end = time.time() + a.seconds
    total = 0
    while time.time() < end:
        b = s.read(256)
        if b:
            total += len(b)
            print(f"[{time.strftime('%H:%M:%S')}] {len(b)}B  {show(b)}")
    print(f"total bytes received: {total}")
    if total == 0:
        print("nothing received: the module did not transmit (or RX line not connected).")


def probe(a):
    s = serial.Serial(a.port, a.baud, timeout=0.05)
    s.reset_input_buffer()
    print(f"sending 0x55 bursts on {a.port} @ {a.baud} for {a.seconds}s - hold BOOT high, "
          "pulse EN now")
    end = time.time() + a.seconds
    got = b""
    while time.time() < end:
        s.write(b"\x55" * 64)
        time.sleep(0.02)
        r = s.read(256)
        if r:
            got += r
            print(f"[{time.strftime('%H:%M:%S')}] reply {len(r)}B  {show(r)}")
    print(f"total reply bytes: {len(got)}")
    if not got:
        print("no reply to sync.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["loopback", "listen", "probe"])
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=2000000)
    ap.add_argument("--seconds", type=int, default=20)
    a = ap.parse_args()
    try:
        r = {"loopback": loopback, "listen": listen, "probe": probe}[a.mode](a)
    except serial.SerialException as e:
        sys.exit(f"cannot open {a.port}: {e}")
    except KeyboardInterrupt:
        r = 0
    sys.exit(r or 0)


if __name__ == "__main__":
    main()