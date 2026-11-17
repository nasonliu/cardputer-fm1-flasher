// SD card firmware flasher (.fwsc / .bin) for M-VAVE FM-1 / SMK-37.
// Supports both old (Puya 0x856014) and new (UCUN UC25HQ80 0xB36014) flash chips,
// preserves bootloader (0x00000-0x03FFF) and factory calibration/key_mac (0xFF000-0xFFFFF),
// and supports both USB_KEY (unit OFF) and USB-MIDI Soft-Key (unit ON).
#pragma once

namespace flasher {

void run();
void flash_path(const char *path);

}  // namespace flasher
