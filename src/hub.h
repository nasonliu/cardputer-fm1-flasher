// Wi-Fi Firmware Hub client for M5Stack Cardputer ADV.
// Fetches the open-source FM-1 catalog from https://thefirmwarehub.com/catalog.json,
// filters by hardware chip compatibility (0xB36014 new vs 0x856014 old),
// downloads .fwsc packages to /fm1/firmware/ with SHA-256 verification,
// and hands off directly to the FM-1 flasher.
#pragma once

namespace hub {

void run();
void debug_test();

}  // namespace hub
