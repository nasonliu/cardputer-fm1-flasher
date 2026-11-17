// Installs a rescue image (m-bootloader's boot area, see rescue/README.md)
// or a streaming firmware source (.fwsc / .bin) with M-Flasher's safety rules:
// - Never touched: 0xFF000-0xFFFFF (key_mac, and on SMK-37 TRIMCOPY) and, on
//   SMK-37, 0xC0000-0xC1FFF (Bluetooth pairing, factory calibration).
// - When flashing firmware (lo >= 0x04000), 0x00000-0x03FFF (bootloader) is
//   also preserved and verified unchanged at the end.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace writer {

static constexpr uint32_t SECTOR = 0x1000;
static constexpr uint32_t BOOT_AREA = 0x4000;
static constexpr uint32_t END = 0xFF000;  // first byte never written
static constexpr uint32_t SECTORS = END / SECTOR;

typedef void (*Log)(const char *fmt, ...);
typedef void (*Progress)(uint32_t done, uint32_t total);

enum Board { FM1, SMK37 };

struct Image {
    const char *name;            // for the screen
    const uint8_t *boot;         // 0x00000-0x03FFF
    uint32_t boot_len;           // as embedded
    uint32_t boot_crc;           // as built (src/rescue_info.h)
    uint32_t keep_lo, keep_hi;   // a protected range below END (empty: equal)
};

const Image &image(Board b);

// The embedded boot area is intact (their CRC32s match the build's).
bool image_ok(const Image &img, char *err, size_t errlen);

struct Source {
    const char *name;
    uint32_t lo, hi;             // writable range [lo, hi) aligned to SECTOR, <= END
    uint32_t keep_lo, keep_hi;   // protected range inside [lo, hi) (empty if equal)
    bool (*read_sector)(void *ctx, uint32_t addr, uint8_t out[SECTOR], char *err, size_t errlen);
    void *ctx;
};

struct Plan {
    bool change[SECTORS];  // sector differs from the target (never a protected one)
    uint32_t to_write;     // changed sectors that hold data
    uint32_t to_erase;     // changed sectors that are blank (0xFF)
    uint32_t runs;
};

bool plan_source(const Source &src, Plan *p, Progress progress, char *err, size_t errlen);
bool apply_source(const Source &src, const Plan &p, Log log, Progress progress, char *err, size_t errlen);

bool plan(const Image &img, Plan *p, Progress progress, char *err, size_t errlen);
bool apply(const Image &img, const Plan &p, Log log, Progress progress, char *err, size_t errlen);

}  // namespace writer
