#include "writer.h"

#include <stdio.h>
#include <string.h>

#include "jl.h"
#include "rescue_info.h"

extern "C" const uint8_t rescue_fm1_boot[], rescue_fm1_boot_end[];
extern "C" const uint8_t rescue_smk37_boot[], rescue_smk37_boot_end[];

namespace writer {

static constexpr uint32_t FLASH_SIZE = 0x100000;
static uint8_t sector_buf[SECTOR];
static uint8_t target_buf[SECTOR];

const Image &image(Board b) {
    static const Image fm1 = {
        "FM-1", rescue_fm1_boot, (uint32_t)(rescue_fm1_boot_end - rescue_fm1_boot), RESCUE_FM1_BOOT_CRC32, 0, 0,
    };
    // BTIF (Bluetooth address, pairings) and USRTRIM (the factory calibration)
    static const Image smk37 = {
        "SMK-37", rescue_smk37_boot, (uint32_t)(rescue_smk37_boot_end - rescue_smk37_boot), RESCUE_SMK37_BOOT_CRC32,
        0xC0000, 0xC2000,
    };
    return b == SMK37 ? smk37 : fm1;
}

static bool is_blank(const uint8_t *d) {
    for (uint32_t i = 0; i < SECTOR; i++)
        if (d[i] != 0xFF) return false;
    return true;
}

static bool is_writable(const Source &src, uint32_t s) {
    uint32_t a = s * SECTOR;
    if (a < src.lo || a >= src.hi || a >= END) return false;
    if (src.keep_hi > src.keep_lo && a >= src.keep_lo && a < src.keep_hi) return false;
    return true;
}

static bool source_crc(const Source &src, uint32_t lo, uint32_t hi, uint32_t *crc, char *err, size_t errlen) {
    uint32_t c = 0;
    for (uint32_t a = lo; a < hi; a += SECTOR) {
        if (!src.read_sector(src.ctx, a, target_buf, err, errlen)) return false;
        c = jl::crc32_update(c, target_buf, SECTOR);
    }
    *crc = c;
    return true;
}

bool image_ok(const Image &img, char *err, size_t errlen) {
    if (img.boot_len != BOOT_AREA) {
        snprintf(err, errlen, "%s rescue image size is wrong (%lu)", img.name, (unsigned long)img.boot_len);
        return false;
    }
    if (jl::crc32_update(0, img.boot, img.boot_len) != img.boot_crc) {
        snprintf(err, errlen, "the %s rescue image in this app is damaged: reinstall the app", img.name);
        return false;
    }
    return true;
}

bool plan_source(const Source &src, Plan *p, Progress progress, char *err, size_t errlen) {
    memset(p, 0, sizeof *p);
    bool prev = false;
    for (uint32_t s = 0; s < SECTORS; s++) {
        bool change = false;
        if (is_writable(src, s)) {
            uint32_t a = s * SECTOR;
            if (!src.read_sector(src.ctx, a, target_buf, err, errlen)) return false;
            if (!jl::read(a, sector_buf, SECTOR, err, errlen)) return false;
            change = memcmp(sector_buf, target_buf, SECTOR) != 0;
        }
        p->change[s] = change;
        if (change) {
            if (is_blank(target_buf)) p->to_erase++;
            else p->to_write++;
            if (!prev) p->runs++;
        }
        prev = change;
        if (progress && (s % 16 == 15)) progress(s + 1, SECTORS);
    }
    if (progress) progress(SECTORS, SECTORS);
    return true;
}

// One run of changed sectors: erase, program, verify with the chip's CRC32.
static bool do_run(const Source &src, uint32_t first, uint32_t count, char *err, size_t errlen) {
    if (!jl::erase(first * SECTOR, count * SECTOR, err, errlen)) return false;
    uint32_t want = 0;
    for (uint32_t s = first; s < first + count; s++) {
        if (!src.read_sector(src.ctx, s * SECTOR, target_buf, err, errlen)) return false;
        want = jl::crc32_update(want, target_buf, SECTOR);
        if (!is_blank(target_buf) && !jl::program(s * SECTOR, target_buf, SECTOR, err, errlen)) return false;
    }
    uint32_t chip;
    if (!jl::crc32(first * SECTOR, count * SECTOR, &chip, err, errlen)) return false;
    if (chip != want) {
        snprintf(err, errlen, "0x%05lX-0x%05lX reads back CRC %08lX, not %08lX", (unsigned long)(first * SECTOR),
                 (unsigned long)((first + count) * SECTOR - 1), (unsigned long)chip, (unsigned long)want);
        return false;
    }
    return true;
}

// [lo, hi) on the chip against the source.
static bool check_written(const Source &src, uint32_t lo, uint32_t hi, char *err, size_t errlen) {
    if (hi <= lo) return true;
    uint32_t chip = 0, want = 0;
    if (!jl::crc32(lo, hi - lo, &chip, err, errlen)) return false;
    if (!source_crc(src, lo, hi, &want, err, errlen)) return false;
    if (chip != want) {
        snprintf(err, errlen, "final check: 0x%05lX-0x%05lX does not match", (unsigned long)lo,
                 (unsigned long)(hi - 1));
        return false;
    }
    return true;
}

bool apply_source(const Source &src, const Plan &p, Log log, Progress progress, char *err, size_t errlen) {
    // Snapshot CRCs of all protected ranges before writing.
    bool keep = src.keep_hi > src.keep_lo && src.keep_lo < src.hi && src.keep_hi > src.lo;
    bool keep_boot = src.lo >= BOOT_AREA;
    uint32_t kept_crc = 0, top_crc = 0, boot_crc = 0;
    if ((keep && !jl::crc32(src.keep_lo, src.keep_hi - src.keep_lo, &kept_crc, err, errlen)) ||
        (keep_boot && !jl::crc32(0, BOOT_AREA, &boot_crc, err, errlen)) ||
        !jl::crc32(END, FLASH_SIZE - END, &top_crc, err, errlen)) {
        return false;
    }

    uint32_t total = p.to_write + p.to_erase, done = 0;
    if (progress) progress(0, total);
    for (uint32_t s = 0; s < SECTORS;) {
        if (!p.change[s]) {
            s++;
            continue;
        }
        uint32_t first = s;
        while (s < SECTORS && p.change[s]) s++;
        uint32_t count = s - first;
        if (log) {
            log("0x%05lX-0x%05lX: %lu sector%s", (unsigned long)(first * SECTOR), (unsigned long)(s * SECTOR - 1),
                (unsigned long)count, count == 1 ? "" : "s");
        }
        if (!do_run(src, first, count, err, errlen)) {
            if (log) log("Not verified (%s): once more", err);
            if (!do_run(src, first, count, err, errlen)) return false;
        }
        done += count;
        if (progress) progress(done, total);
    }

    if (keep) {
        if (!check_written(src, src.lo, src.keep_lo, err, errlen) ||
            !check_written(src, src.keep_hi, src.hi, err, errlen)) {
            return false;
        }
    } else if (!check_written(src, src.lo, src.hi, err, errlen)) {
        return false;
    }

    uint32_t crc;
    if (keep_boot) {
        if (!jl::crc32(0, BOOT_AREA, &crc, err, errlen)) return false;
        if (crc != boot_crc) {
            snprintf(err, errlen, "final check: bootloader 0x00000-0x03FFF changed");
            return false;
        }
    }
    if (keep) {
        if (!jl::crc32(src.keep_lo, src.keep_hi - src.keep_lo, &crc, err, errlen)) return false;
        if (crc != kept_crc) {
            snprintf(err, errlen, "final check: protected 0x%05lX-0x%05lX changed", (unsigned long)src.keep_lo,
                     (unsigned long)(src.keep_hi - 1));
            return false;
        }
    }
    if (!jl::crc32(END, FLASH_SIZE - END, &crc, err, errlen)) return false;
    if (crc != top_crc) {
        snprintf(err, errlen, "final check: protected 0x%05lX- changed", (unsigned long)END);
        return false;
    }
    if (log) {
        log("Verified: 0x%05lX-0x%05lX written, protected areas unchanged",
            (unsigned long)src.lo, (unsigned long)(src.hi - 1));
    }
    return true;
}

static bool rescue_read_sector(void *ctx, uint32_t addr, uint8_t out[SECTOR], char *, size_t) {
    const Image *img = (const Image *)ctx;
    if (addr < BOOT_AREA) {
        memcpy(out, img->boot + addr, SECTOR);
    } else {
        memset(out, 0xFF, SECTOR);
    }
    return true;
}

static Source rescue_source(const Image &img) {
    return {img.name, 0, END, img.keep_lo, img.keep_hi, rescue_read_sector, (void *)&img};
}

bool plan(const Image &img, Plan *p, Progress progress, char *err, size_t errlen) {
    return plan_source(rescue_source(img), p, progress, err, errlen);
}

bool apply(const Image &img, const Plan &p, Log log, Progress progress, char *err, size_t errlen) {
    return apply_source(rescue_source(img), p, log, progress, err, errlen);
}

}  // namespace writer
