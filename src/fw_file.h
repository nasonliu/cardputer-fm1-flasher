// Streaming parser and validator for M-VAVE .fwsc packages and .bin images.
// Designed for low-RAM embedded use (ESP32-S3 without PSRAM) and host unit tests.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace fw {

static constexpr uint32_t SECTOR = 0x1000;
static constexpr uint32_t BOOT_AREA = 0x4000;
static constexpr uint32_t FLASH_SIZE = 0x100000;

struct Stream {
    uint32_t size = 0;
    virtual bool read_at(uint32_t offset, uint8_t *buf, size_t len) = 0;
    virtual ~Stream() = default;
};

enum FileKind {
    KIND_UNKNOWN = 0,
    KIND_FWSC,       // M-VAVE / community .fwsc package (has flash.bin)
    KIND_BIN_FULL,   // 1 MiB full flash dump / backup
    KIND_BIN_APP,    // App-only binary starting at 0x04000
};

struct PackageInfo {
    FileKind kind = KIND_UNKNOWN;
    char identity[64] = {};   // e.g. "FM-1_016"
    char model[48] = {};      // e.g. "FM-1" (identity before last '_')
    uint32_t ufw_woven_blocks = 0;  // number of 48-byte woven blocks at start of .fwsc
    uint32_t flash_off = 0;   // offset of flash.bin inside UFW stream (0 for .bin)
    uint32_t flash_len = 0;   // length of flash.bin (or .bin file size)
    uint16_t flash_crc = 0;   // expected CRC16 of flash.bin
};

uint16_t crc16_update(uint16_t crc, const uint8_t *d, size_t n);
void enc(uint8_t *d, size_t n, uint16_t key);

// Parses and CRC-verifies a .fwsc package or .bin image from `stream`.
// `filename` is used as a hint for extension (.fwsc vs .bin).
bool inspect(Stream &stream, const char *filename, PackageInfo *out, char *err, size_t errlen);

// Reads `len` bytes from the logical flash image inside `pkg` at `flash_addr`
// (where `flash_addr` is 0..pkg.flash_len for FWSC/BIN_FULL, or 0x04000..0x04000+flash_len for BIN_APP).
bool read_flash_bytes(Stream &stream, const PackageInfo &pkg, uint32_t flash_addr, uint8_t *buf, size_t len);

// Verifies that the app area (starting at 0x04000) in `pkg` decrypts with `chip_key`
// and all attr==0x82 files (app.bin, cfg_tool.bin, etc.) pass their CRC16 checks.
bool verify_app_area(Stream &stream, const PackageInfo &pkg, uint16_t chip_key, char *err, size_t errlen);

// Checks whether `pkg.model` is compatible with FM-1 or SMK-37.
bool model_matches(const PackageInfo &pkg, bool is_fm1, bool is_smk37);

}  // namespace fw
