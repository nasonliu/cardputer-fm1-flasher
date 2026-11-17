# FM-1 Flasher & Wi-Fi Hub for M5Stack Cardputer ADV

Turn your **[M5Stack Cardputer ADV](https://docs.m5stack.com/en/core/Cardputer-Adv)** (ESP32-S3) into a **completely standalone online firmware browser and flasher for the M-VAVE FM-1** pocket synthesizer — **no computer, phone, or browser required**.

Browse the latest open-source firmwares on **[The Firmware Hub](https://thefirmwarehub.com)** over Wi-Fi, download `.fwsc` packages straight to your microSD card with SHA-256 verification, and flash them directly into your FM-1 over USB-C.

---

## Key Features

- **Standalone Wi-Fi Firmware Hub (`thefirmwarehub.com`)**
  - Scan & select 2.4 GHz Wi-Fi networks directly on the Cardputer (credentials saved in NVS & `/fm1/wifi.txt`).
  - Stream and index the live catalog from `https://thefirmwarehub.com/catalog.json` (200+ firmwares) with search (`/`), chip filter (`C`), and catalog refresh (`U`).
  - Download `.fwsc` firmware packages to `/fm1/firmware/` with automatic **SHA-256** verification, then flash immediately.
- **Full Support for New & Old FM-1 Hardware**
  - Supports both the newer **UC25HQ80 (`0xB36014`)** flash chip (2026+ units) and the older **P25Q80H (`0x856014`)** flash chip (plus SMK-37 `0xEB6014` rescue).
  - Hub view defaults to filtering firmwares compatible with `0xB36014` (`[NEW]`), toggleable to `[ALL]`.
- **SD Card `.fwsc` & `.bin` Flasher**
  - Low-RAM streaming UFW/FWSC de-interleaver and `chip_key` (`0x980F`) CRC16 verifier.
  - Mandatory 1 MiB flash backup (`/fm1/backup-NNN.bin`) before any write.
  - Preserves the bootloader (`0x00000–0x03FFF`) and factory calibration / `key_mac` (`0xFF000–0xFFFFF`), with post-write CRC32 verification.
- **Dual USB-C OTG Entry Modes**
  - **Powered-on FM-1**: Automatically sends USB-MIDI SysEx soft-key (`F0 22 24 35 7D F7`) to enter `WL82 UBOOT1.00` without turning the synth off.
  - **Powered-off or bricked FM-1**: Sends JieLi `0x16EF` `USB_KEY` + hardware-timed RMT 4 us calibration pulses.
- **Graphical Icon Menu**
  - Navigate the 4-card home screen with Cardputer arrow keys (`,` `/` `;` `.`) and `ENTER` (or `G0`):
    1. **Wi-Fi Hub** — Online firmware catalog & downloader
    2. **SD Flash** — Flash `.fwsc` / `.bin` files from `/fm1/firmware/`, `/fm1/`, or `/`
    3. **Rescue** — Guided unbrick & `m-bootloader` installer
    4. **Advanced** — Console log, manual backup, USB-MIDI soft-key, PC handoff, and pin/pulse self-tests
- **Developer-Friendly USB Serial**
  - ESP32-S3 USB-Serial/JTAG stays active in all menus and Wi-Fi Hub so `pio run -t upload` works automatically without holding `G0` (USB pins are only detached during active FM-1 flashing).

---

## Install

Download `firmware.factory.bin` (merged image for offset `0x0`) or `firmware.bin` (app-only for `0x10000`) from **[Releases](../../releases)**.

- **Via [M5Launcher](https://github.com/bmorcelli/Launcher) (SD card, no PC)**:
  Copy `firmware.factory.bin` to a FAT32 microSD card, open M5Launcher, select the file, and install.
- **Via `esptool` from a computer**:
  Put the Cardputer ADV into download mode (hold `G0`, press `Reset` or plug USB-C), then run:
  ```bash
  esptool --chip esp32s3 write-flash 0x0 firmware.factory.bin
  ```

---

## Usage

1. Insert a **FAT32 microSD card** (used to store `/fm1/hub_index.dat`, downloaded `.fwsc` files in `/fm1/firmware/`, and automatic backups `/fm1/backup-NNN.bin`).
2. Power on the Cardputer ADV and use `,` / `/` (or `;` / `.`) + `ENTER` on the home screen:
   - **Wi-Fi Hub**: Pick your 2.4 GHz Wi-Fi network, browse the firmware list (`;` / `.`), press `ENTER` to inspect details, `ENTER` to download & verify SHA-256, and `ENTER` to flash.
   - **SD Flash**: Pick any `.fwsc` or `.bin` on the SD card and connect your FM-1 via a USB-C to USB-C cable (either already powered ON for USB-MIDI soft-key, or powered OFF and switched ON when prompted).
   - **Rescue**: Guided step-by-step unbricking for dead FM-1 / SMK-37 units.

---

## Build from Source

Built with PlatformIO (`pioarduino` / Arduino-ESP32 3.x):

```bash
pio run              # builds .pio/build/cardputer-adv/firmware.bin and firmware.factory.bin
pio run -t upload    # flashes connected Cardputer ADV automatically when in menus
```

### Project Structure

| File | Purpose |
|---|---|
| `src/hub.cpp` | Wi-Fi setup, streaming `thefirmwarehub.com/catalog.json` indexer, SHA-256 downloader |
| `src/flasher.cpp` | SD card `.fwsc` / `.bin` browser, backup, sector diff, and flash workflow |
| `src/fw_file.cpp` | Streaming UFW/FWSC de-interleaver & `0x980F` app CRC16 verifier |
| `src/usbkey.cpp` | JieLi `0x16EF` `USB_KEY`, RMT 4 us calibration pulses, and USB-Serial restore |
| `src/uboot_host.cpp` | ESP32-S3 USB Host BOT (`4C4A:8057`) & USB-MIDI SysEx soft-key |
| `src/jl.cpp`, `src/mflr_loader.h` | ROM & M-Flasher loader protocol |
| `src/writer.cpp`, `src/engine.cpp` | Flash backup, `0xB36014`/`0x856014`/`0xEB6014` detection, safe sector writer |
| `src/ui.cpp`, `src/guided.cpp`, `src/advanced.cpp` | Polled TCA8418 ADV keyboard driver, 4-card icon menu, guided & advanced UI |

---

## Credits & Attribution

- Firmware catalog & packages: **[The Firmware Hub](https://thefirmwarehub.com)** (`CC-BY-4.0`)
- Base Cardputer ADV USB-key & rescue engine: **[jvitkauskas/cardputer-synth-rescue](https://github.com/jvitkauskas/cardputer-synth-rescue)**
- Flash loader & bootloader: **[M-Flasher](https://github.com/jvitkauskas/m-flasher)** & **[m-bootloader](https://github.com/jvitkauskas/m-bootloader)** (`MIT`)
- USB-key & calibration timings: **[FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter)** & **[flipper-fm1-recovery](https://github.com/merthsoft/flipper-fm1-recovery)**
