# ST67W611 (NCP) Flashing Guide

The GatewayV2 uses the **ST67W611 Network Coprocessor (NCP)** to provide Wi-Fi capabilities. Because the ST67 is an independent SoC with its own internal Flash memory, it requires a dedicated firmware image (`T01` or `T02`) to function alongside the main STM32 Gateway firmware.

Since the ST67 chip is directly connected to the STM32 and does not have its own USB port, we must use the **STM32 as a UART Bridge** to flash the ST67.

---

## 1. Prerequisites

Before flashing the Wi-Fi chip, ensure you have:
1. A Micro-USB cable connected to the Gateway's main Debug/UART port.
2. The exact COM port number of your Gateway (e.g., `COM5`).
3. The `NCP_Loader` firmware, which turns the STM32 into a high-speed (2,000,000 baud) pass-through bridge.

---

## 2. Flashing Workflow

Flashing the ST67 involves a strict 3-step process. **Do not skip Step 3**, or your Gateway will remain stuck in Bridge Mode!

### Step 2.1: Flash the Bridge Firmware (NCP_Loader)
First, we must temporarily overwrite the Gateway firmware with the `NCP_Loader` firmware.

1. Open a terminal in the `NCP_Loader` directory.
2. Run the build and flash scripts:
   ```cmd
   cd NCP_Loader
   build_ncp_loader_auto.bat
   flash_ncp_loader_auto.bat
   ```
*After this step, the STM32 acts purely as a UART relay between your PC and the ST67.*

### Step 2.2: Flash the ST67 Firmware
You can flash the ST67 using either the provided automated scripts or raw terminal commands using the QConn CLI.

#### Method A: Automated Script (Recommended)
We have provided wrapper scripts that handle firmware version injection and JEDEC verification automatically.

1. Run the flash script and specify your COM port:
   ```cmd
   flash_st67_t02_auto.bat COM5
   ```
   *(By default, this will flash version 2.0.106 at 2,000,000 baud).*

#### Method B: Raw Terminal Command (QConn CLI)
If you need to bypass the `.bat` wrapper (e.g., for debugging or CI/CD pipelines), you can invoke the `QConn_Flash_Cmd.exe` directly from the terminal.

1. Ensure the STM32 is already running the `NCP_Loader` firmware.
2. Run the following command from the GatewayV2 root directory (replace `COM5` with your actual port):
   ```cmd
   .\libs\x-cube-st67w61\Projects\ST67W6X_Scripts\Binaries\QConn_Flash\QConn_Flash_Cmd.exe ^
     --interface uart ^
     --port COM5 ^
     --baudrate 2000000 ^
     --config .\libs\x-cube-st67w61\Projects\ST67W6X_Scripts\Binaries\NCP_Binaries\mission_t02_flash_prog_cfg.ini ^
     --efuse=.\libs\x-cube-st67w61\Projects\ST67W6X_Scripts\Binaries\NCP_Binaries\efusedata.bin
   ```

A successful flash will output `[All Success]` or `Program Finished`.

### Step 2.3: Restore the Main Gateway Firmware
Once the ST67 is successfully flashed, the STM32 is still stuck in Bridge Mode. We must restore the main Industrial Gateway firmware.

1. Return to the root directory.
2. Re-flash the main firmware:
   ```cmd
   cd ..
   flash.bat
   ```

---

## 3. Troubleshooting

- **`shake hand fail` / `Timeout`**: Ensure no other terminal application (like PuTTY or TeraTerm) is holding the COM port open. The baud rate must be exactly `2000000`.
- **`Set flash config fail`**: The ST67 booted successfully but the flash memory wasn't recognized. Check the power supply stability or ensure the `NCP_Loader` is successfully toggling the `ST67_EN` and `ST67_BOOT` pins.
