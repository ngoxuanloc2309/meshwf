#!/usr/bin/env python3
"""
st67_uart_diag.py - quick checks over a USB-UART link to the ST67W611.
loopback/listen/probe only send/read raw bytes. `terminal` lets you type lines by hand.
Nothing here writes to the module's flash by itself.

  python st67_uart_diag.py loopback COM18        # TX<->RX of the ADAPTER shorted, no module
  python st67_uart_diag.py listen   COM18 --baud 115200
  python st67_uart_diag.py probe    COM18 --baud 2000000
  python st67_uart_diag.py terminal COM18 --baud 115200

listen : prints every byte the module sends. Press EN reset (EN low then high)
         while it runs, in both BOOT=low and BOOT=high. If you never see a
         single byte in ANY case, TX/RX from the module is not reaching the
         adapter (wrong pin, swapped, no GND, no power, wrong voltage).
probe  : sends a burst of 0x55 sync bytes repeatedly and prints any reply.
         The 0x55 sync is the usual Bouffalo/QCC74x-style ROM handshake
         (an assumption: not verified against the ST67 documentation).
         Hold BOOT high, press/release EN while it runs.
terminal: interactive. Everything the module sends is printed as it arrives.
         Each line you type is sent followed by the line ending chosen with
         --eol (default crlf, the usual AT ending). Extra input commands:
           /hex 55 aa 0d 0a   send raw bytes (hex)
           /eol cr|lf|crlf|none   change the line ending
           /quit              leave (Ctrl+C also works)
         --show-hex also prints received bytes as hex.
         Whether the module answers AT on this UART depends on the firmware
         and on what is wired to it: not verified for this board.
"""
import argparse
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("pip install pyserial")


EOLS = {"crlf": b"\r\n", "cr": b"\r", "lf": b"\n", "none": b""}


def show(b):
    return b.hex(" ") + "   |" + "".join(chr(c) if 32 <= c < 127 else "." for c in b) + "|"


def open_port(port, baud, timeout):
    # serial_for_url accepts a plain name (COM18, /dev/ttyUSB0) and URLs such as loop://
    return serial.serial_for_url(port, baudrate=baud, timeout=timeout)


def loopback(a):
    s = open_port(a.port, a.baud, 1)
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
    s = open_port(a.port, a.baud, 0.2)
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
    s = open_port(a.port, a.baud, 0.05)
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


def terminal(a):
    s = open_port(a.port, a.baud, 0.1)
    s.reset_input_buffer()
    stop = threading.Event()
    lock = threading.Lock()
    eol = EOLS[a.eol]

    def reader():
        while not stop.is_set():
            try:
                b = s.read(256)
            except (serial.SerialException, OSError) as e:
                with lock:
                    print(f"\n[reader] port error: {e}")
                stop.set()
                return
            if not b:
                continue
            with lock:
                sys.stdout.write(b.decode("utf-8", errors="replace"))
                sys.stdout.flush()
                if a.show_hex:
                    sys.stdout.write(f"\n[rx {len(b)}B] {show(b)}\n")
                    sys.stdout.flush()

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    print(f"terminal on {a.port} @ {a.baud}, eol={a.eol}. Type a line + Enter. "
          "/hex .. /eol .. /quit")
    try:
        while not stop.is_set():
            try:
                line = input()
            except EOFError:
                break
            if line.strip() == "/quit":
                break
            if line.startswith("/eol"):
                parts = line.split()
                if len(parts) == 2 and parts[1] in EOLS:
                    eol = EOLS[parts[1]]
                    print(f"[eol = {parts[1]}]")
                else:
                    print("usage: /eol cr|lf|crlf|none")
                continue
            if line.startswith("/hex"):
                try:
                    data = bytes.fromhex(line[4:].strip())
                except ValueError:
                    print("usage: /hex 55 aa 0d 0a")
                    continue
                s.write(data)
                print(f"[tx {len(data)}B] {show(data)}")
                continue
            data = line.encode("utf-8") + eol
            s.write(data)
            if a.show_hex:
                print(f"[tx {len(data)}B] {show(data)}")
    finally:
        stop.set()
        t.join(timeout=1)
        s.close()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["loopback", "listen", "probe", "terminal"])
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=2000000)
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--eol", choices=list(EOLS), default="crlf",
                    help="terminal: line ending appended to each typed line")
    ap.add_argument("--show-hex", action="store_true",
                    help="terminal: also print tx/rx bytes as hex")
    a = ap.parse_args()
    try:
        r = {"loopback": loopback, "listen": listen, "probe": probe,
             "terminal": terminal}[a.mode](a)
    except serial.SerialException as e:
        sys.exit(f"cannot open {a.port}: {e}")
    except KeyboardInterrupt:
        r = 0
    sys.exit(r or 0)


if __name__ == "__main__":
    main()