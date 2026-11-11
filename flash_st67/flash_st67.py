#!/usr/bin/env python3
"""
flash_st67.py - flash the ST67W611 NCP through the Blue Pill USB-CDC <-> UART bridge.

    PC (this script -> QConn_Flash_Cmd) <-USB CDC-> Blue Pill <-USART1 2 Mbaud-> ST67

This script does NOT implement the ST67 ISP protocol itself. It drives ST's
QConn_Flash_Cmd (shipped in x-cube-st67w61) exactly like the official
NCP_update_mission_profile_*.bat/.sh scripts and doc WIFI_NCP_FLASHING.md
(Method B), adding: COM port auto-detect, firmware version / profile selection,
config (.ini) generation, a bridge sanity check, and one automatic retry.

Prerequisites
  * Blue Pill runs main_bridge.c. It enters flash mode (BOOT high, EN pulse)
    at power-up. If it was already powered: press its BTN_FLASH (PA0) first.
  * No other program holds the COM port (PuTTY, TeraTerm, ...).
  * pip install pyserial   (only needed for port listing / sanity check)

Examples
  python flash_st67.py --list
  python flash_st67.py --sdk path/to/x-cube-st67w61 --port COM7
  python flash_st67.py --sdk ... --port COM7 --profile t01 --version 2.0.97
  python flash_st67.py --sdk ... --port COM7 --profile mfg --yes

WARNING: mission binaries are signed; flashing them with efusedata.bin locks the
ST67W611 if it is not locked yet (irreversible). The script asks to confirm.
"""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # only needed for --list / sanity check
    serial = None
    list_ports = None

BAUD = 2000000
REL_SCRIPTS = os.path.join("Projects", "ST67W6X_Scripts")
BLUEPILL_VIDPID = (0x0483, 0x5740)  # ST CDC default (CubeMX USB CDC)


def find_scripts_dir(sdk):
    """Accept the SDK root, .../ST67W6X_Scripts or .../Binaries."""
    cands = [
        os.path.join(sdk, REL_SCRIPTS),
        sdk,
        os.path.dirname(sdk.rstrip("/\\")),
    ]
    for c in cands:
        if os.path.isdir(os.path.join(c, "Binaries", "QConn_Flash")):
            return c
    sys.exit(f"ERROR: cannot find Binaries/QConn_Flash under '{sdk}'. "
             "Pass --sdk <x-cube-st67w61 root>.")


def list_serial():
    if list_ports is None:
        sys.exit("pyserial missing: pip install pyserial")
    ports = list(list_ports.comports())
    for p in ports:
        tag = ""
        if (p.vid, p.pid) == BLUEPILL_VIDPID:
            tag = "  <-- likely Blue Pill CDC"
        print(f"{p.device:12s} {p.vid and f'{p.vid:04x}:{p.pid:04x}' or '----:----'}  "
              f"{p.description}{tag}")
    return ports


def autodetect_port():
    if list_ports is None:
        return None
    hits = [p.device for p in list_ports.comports()
            if (p.vid, p.pid) == BLUEPILL_VIDPID]
    if len(hits) == 1:
        return hits[0]
    return None


def pick_binary(bin_dir, profile, version):
    prefix = f"st67w611m_{'mfg' if profile == 'mfg' else 'mission_' + profile}"
    rx = re.compile(rf"^{re.escape(prefix)}_v([0-9][0-9.]*)\.bin$")
    found = {}
    for f in os.listdir(bin_dir):
        m = rx.match(f)
        if m:
            found[m.group(1)] = f
    if not found:
        sys.exit(f"ERROR: no '{prefix}_v*.bin' in {bin_dir}")
    key = lambda v: [int(x) for x in v.split(".")]
    if version:
        if version not in found:
            sys.exit(f"ERROR: version {version} not available. "
                     f"Have: {', '.join(sorted(found, key=key))}")
        return version, found[version]
    v = max(found, key=key)
    return v, found[v]


def build_ini(bin_dir, profile, binary, use_littlefs):
    """Copy the stock ini, replacing the FW file name (same as the .sh script)."""
    stock = os.path.join(bin_dir, "mfg_flash_prog_cfg.ini" if profile == "mfg"
                         else f"mission_{profile}_flash_prog_cfg.ini")
    if not os.path.isfile(stock):
        sys.exit(f"ERROR: missing {stock}")
    with open(stock, "r", encoding="utf-8") as fh:
        text = fh.read()
    lines = []
    for ln in text.splitlines():
        if re.match(r"^filedir\s*=\s*\./st67w611m_(mfg|mission_t0\d)_v", ln):
            ln = f"filedir = ./{binary}"
        lines.append(ln)
    text = "\n".join(lines) + "\n"

    if "[littlefs]" in text:
        lfs_rel = re.search(r"\[littlefs\]\s*\r?\nfiledir\s*=\s*(\S+)", text)
        lfs_path = os.path.normpath(os.path.join(bin_dir, lfs_rel.group(1))) if lfs_rel else ""
        if not use_littlefs or not os.path.isfile(lfs_path):
            if use_littlefs:
                print(f"WARNING: littlefs image not found ({lfs_path}); "
                      "build it with LittleFS/build.bat|sh or use --no-littlefs. "
                      "Skipping the [littlefs] section.")
            text = re.sub(r"\[littlefs\][^\[]*", "", text)

    out = os.path.join(bin_dir, "st67_bridge_flash_cfg_auto.ini")
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    return out


def sanity_check(port):
    """Open the port once: catches 'port busy' early. Does not talk to the ST67."""
    if serial is None:
        print("(pyserial not installed: skipping port check)")
        return
    try:
        s = serial.Serial(port, BAUD, timeout=0.2)
    except serial.SerialException as e:
        sys.exit(f"ERROR: cannot open {port}: {e}\n"
                 "Close any terminal program using it.")
    try:
        s.reset_input_buffer()
        time.sleep(0.1)
        junk = s.read(256)
        if junk:
            print(f"note: {len(junk)} stray byte(s) received from the bridge "
                  "(boot log of the module). The bridge drops them on flash-mode entry; "
                  "if the flash fails press BTN_FLASH and retry.")
    finally:
        s.close()
    time.sleep(0.3)  # let Windows release the handle before QConn opens it


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    ap.add_argument("--sdk", help="x-cube-st67w61 root (contains Projects/ST67W6X_Scripts)")
    ap.add_argument("--port", help="COM port of the bridge (auto-detect if omitted)")
    ap.add_argument("--baud", type=int, default=BAUD, help="default 2000000 (fixed by bridge)")
    ap.add_argument("--profile", choices=["t01", "t02", "mfg"], default="t02",
                    help="firmware profile (default t02; the board code uses T01 per handoff: "
                         "ST67W611_ARCH=T01 -> choose t01)")
    ap.add_argument("--version", help="e.g. 2.0.106 (default: newest available)")
    ap.add_argument("--no-littlefs", action="store_true", help="do not flash the LittleFS image")
    ap.add_argument("--no-efuse", action="store_true", help="do not pass --efuse (no locking)")
    ap.add_argument("--retries", type=int, default=1)
    ap.add_argument("--dry-run", action="store_true", help="print the command, do not run it")
    ap.add_argument("-y", "--yes", action="store_true", help="skip the confirmation prompt")
    a = ap.parse_args()

    if a.list:
        list_serial()
        return
    if not a.sdk:
        sys.exit("ERROR: --sdk is required (try --list to see ports)")

    scripts = os.path.abspath(find_scripts_dir(os.path.abspath(a.sdk)))
    bins = os.path.join(scripts, "Binaries")
    bin_dir = os.path.join(bins, "NCP_Binaries")
    qdir = os.path.join(bins, "QConn_Flash")
    exe = os.path.join(qdir, "QConn_Flash_Cmd.exe" if platform.system() == "Windows"
                       else "QConn_Flash_Cmd-ubuntu")
    if not os.path.isfile(exe):
        sys.exit(f"ERROR: {exe} not found for this OS")
    if platform.system() != "Windows" and not os.access(exe, os.X_OK):
        os.chmod(exe, os.stat(exe).st_mode | 0o111)

    port = a.port or autodetect_port()
    if not port:
        print("Could not pick a port automatically. Available ports:")
        list_serial()
        sys.exit("ERROR: pass --port")

    version, binary = pick_binary(bin_dir, a.profile, a.version)
    ini = build_ini(bin_dir, a.profile, binary, not a.no_littlefs)
    efuse = os.path.join(bin_dir, "efusedata.bin")

    cmd = [exe, "--interface", "uart", "--port", port, "--baudrate", str(a.baud),
           "--config", ini]
    if not a.no_efuse:
        cmd.append(f"--efuse={efuse}")

    print(f"Port     : {port} @ {a.baud}")
    print(f"Profile  : {a.profile}   version {version}   ({binary})")
    print(f"Config   : {ini}")
    print("Command  : " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    if a.dry_run:
        return

    if not a.no_efuse and not a.yes:
        print("\n!! Signed binary + efuse data: this LOCKS the ST67W611 if not yet locked. !!")
        if input("Proceed? (y/N) ").strip().lower() != "y":
            sys.exit("aborted")

    sanity_check(port)
    print("\nFlashing... (if you get 'shake hand fail': press BTN_FLASH on the bridge, retry)\n")

    rc = 1
    for attempt in range(a.retries + 1):
        rc = subprocess.call(cmd, cwd=qdir)
        if rc == 0:
            break
        if attempt < a.retries:
            print(f"\nQConn_Flash_Cmd failed (rc={rc}); retrying in 1 s ...")
            time.sleep(1)

    try:
        os.remove(ini)
    except OSError:
        pass

    if rc == 0:
        print("\nFlash OK. Press BTN_RUN (PA1) on the bridge, or power-cycle, to run the module normally.")
    else:
        print(f"\nFlash FAILED (rc={rc}). Checklist: only one program on the port; bridge in flash "
              "mode (BTN_FLASH); common GND; ST67 powered; UART TX/RX not swapped.")
    sys.exit(rc)


if __name__ == "__main__":
    main()