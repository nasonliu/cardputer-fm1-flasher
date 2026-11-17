#include "fw_file.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace fw {

static constexpr uint32_t FWSC_BLOCK = 0x30;  // 48 bytes
static constexpr uint32_t FWSC_KEEP = 0x2F;   // 47 bytes payload + 1 woven byte
static constexpr uint32_t MAX_WOVEN_BLOCKS = 256;

uint16_t crc16_update(uint16_t crc, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

void enc(uint8_t *d, size_t n, uint16_t key) {
    for (size_t i = 0; i < n; i++) {
        uint8_t o = d[i] ^ (uint8_t)key;
        key = (uint16_t)((key << 1) ^ ((key & 0x8000) ? 0x1021 : 0));
        d[i] = o;
    }
}

static inline uint16_t u16le(const uint8_t *b, size_t at) {
    return (uint16_t)b[at] | ((uint16_t)b[at + 1] << 8);
}

static inline uint32_t u32le(const uint8_t *b, size_t at) {
    return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) | ((uint32_t)b[at + 3] << 24);
}

static void copy_cstr(char *dst, size_t dst_len, const uint8_t *src, size_t src_len) {
    size_t n = 0;
    while (n < src_len && n + 1 < dst_len && src[n] != 0) {
        dst[n] = (char)src[n];
        n++;
    }
    dst[n] = 0;
}

static bool has_ext_ci(const char *name, const char *ext) {
    if (!name) return false;
    size_t nl = strlen(name), el = strlen(ext);
    if (nl < el) return false;
    const char *p = name + nl - el;
    for (size_t i = 0; i < el; i++) {
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)ext[i])) return false;
    }
    return true;
}

static bool read_ufw(Stream &stream, uint32_t woven_blocks, uint32_t ufw_off, uint8_t *buf, size_t len) {
    uint32_t woven_logical = woven_blocks * FWSC_KEEP;
    while (len > 0) {
        if (ufw_off < woven_logical) {
            uint32_t blk = ufw_off / FWSC_KEEP;
            uint32_t in_blk = ufw_off % FWSC_KEEP;
            size_t take = FWSC_KEEP - in_blk;
            if (take > len) take = len;
            if (!stream.read_at(blk * FWSC_BLOCK + in_blk, buf, take)) return false;
            ufw_off += (uint32_t)take;
            buf += take;
            len -= take;
        } else {
            uint32_t phys = woven_blocks * FWSC_BLOCK + (ufw_off - woven_logical);
            return stream.read_at(phys, buf, len);
        }
    }
    return true;
}

static void extract_model(const char *identity, char *model, size_t model_len) {
    const char *last_us = strrchr(identity, '_');
    size_t n = last_us ? (size_t)(last_us - identity) : strlen(identity);
    if (n >= model_len) n = model_len - 1;
    memcpy(model, identity, n);
    model[n] = 0;
}

static bool inspect_fwsc(Stream &stream, PackageInfo *out, char *err, size_t errlen) {
    if (stream.size < 0x1000) {
        snprintf(err, errlen, "File too small for .fwsc");
        return false;
    }

    uint32_t n = 0;
    uint8_t blk[FWSC_BLOCK];
    bool id_done = false;
    size_t id_len = 0;

    while (n < MAX_WOVEN_BLOCKS) {
        if ((n + 1) * FWSC_BLOCK > stream.size) {
            snprintf(err, errlen, "Not a valid .fwsc package");
            return false;
        }
        if (!stream.read_at(n * FWSC_BLOCK, blk, FWSC_BLOCK)) {
            snprintf(err, errlen, "SD read error");
            return false;
        }
        uint8_t m = blk[FWSC_KEEP];
        if (n >= 20 && m != 0x7D) break;
        if (!id_done) {
            if (m == 0x7D) {
                id_done = true;
            } else if (id_len + 1 < sizeof(out->identity)) {
                out->identity[id_len++] = (char)(uint8_t)(m - (uint8_t)(n + 1));
            }
        }
        n++;
    }
    if (n >= MAX_WOVEN_BLOCKS) {
        snprintf(err, errlen, "Invalid .fwsc woven header");
        return false;
    }
    out->identity[id_len] = 0;
    extract_model(out->identity, out->model, sizeof(out->model));
    out->ufw_woven_blocks = n;

    uint32_t ufw_size = stream.size - n;  // n bytes dropped (1 per woven block)
    uint8_t head[0x40];
    if (!read_ufw(stream, n, 0, head, sizeof(head))) {
        snprintf(err, errlen, "SD read error reading UFW header");
        return false;
    }
    enc(head, sizeof(head), 0xFFFF);
    if (crc16_update(0, head + 2, 0x3E) != u16le(head, 0)) {
        snprintf(err, errlen, "Bad .fwsc header CRC");
        return false;
    }
    uint16_t list_crc = u16le(head, 2);
    uint16_t count = u16le(head, 8);
    if (count == 0 || count > 32 || 0x40 + (uint32_t)count * 0x50 > ufw_size) {
        snprintf(err, errlen, "Bad .fwsc file table");
        return false;
    }

    uint16_t calc_list_crc = 0;
    bool found_flash = false;
    uint8_t entry[0x50];
    for (uint16_t i = 0; i < count; i++) {
        if (!read_ufw(stream, n, 0x40 + (uint32_t)i * 0x50, entry, sizeof(entry))) {
            snprintf(err, errlen, "SD read error reading file list");
            return false;
        }
        calc_list_crc = crc16_update(calc_list_crc, entry, sizeof(entry));
        enc(entry, sizeof(entry), 0xFFFF);
        uint16_t typ = u16le(entry, 0);
        uint16_t dcrc = u16le(entry, 4);
        uint32_t off = u32le(entry, 8);
        uint32_t sz = u32le(entry, 12);
        char name[17];
        copy_cstr(name, sizeof(name), entry + 0x40, 16);
        if (strcmp(name, "flash.bin") == 0 && typ == 0) {
            if (off + sz > ufw_size) {
                snprintf(err, errlen, "flash.bin outside package");
                return false;
            }
            out->flash_off = off;
            out->flash_len = sz;
            out->flash_crc = dcrc;
            found_flash = true;
        }
    }
    if (calc_list_crc != list_crc) {
        snprintf(err, errlen, "Bad .fwsc file list CRC");
        return false;
    }
    if (!found_flash) {
        snprintf(err, errlen, "No flash.bin in .fwsc");
        return false;
    }
    if (out->flash_len <= BOOT_AREA || out->flash_len > FLASH_SIZE || (out->flash_len % SECTOR) != 0) {
        snprintf(err, errlen, "Invalid flash.bin size (%lu)", (unsigned long)out->flash_len);
        return false;
    }

    // Stream-verify flash.bin CRC16
    uint8_t buf[512];
    uint16_t crc = 0;
    uint32_t done = 0;
    while (done < out->flash_len) {
        size_t k = out->flash_len - done < sizeof(buf) ? out->flash_len - done : sizeof(buf);
        if (!read_ufw(stream, n, out->flash_off + done, buf, k)) {
            snprintf(err, errlen, "SD read error verifying flash.bin");
            return false;
        }
        crc = crc16_update(crc, buf, k);
        done += (uint32_t)k;
    }
    if (crc != out->flash_crc) {
        snprintf(err, errlen, "Damaged .fwsc (flash.bin CRC mismatch)");
        return false;
    }
    out->kind = KIND_FWSC;
    return true;
}

bool inspect(Stream &stream, const char *filename, PackageInfo *out, char *err, size_t errlen) {
    *out = PackageInfo{};
    if (has_ext_ci(filename, ".fwsc")) {
        return inspect_fwsc(stream, out, err, errlen);
    }
    if (has_ext_ci(filename, ".bin")) {
        if (stream.size == FLASH_SIZE) {
            out->kind = KIND_BIN_FULL;
            out->flash_len = FLASH_SIZE;
            snprintf(out->identity, sizeof(out->identity), "%s", filename ? filename : "full.bin");
            return true;
        }
        if (stream.size > 0 && stream.size <= FLASH_SIZE - BOOT_AREA && (stream.size % SECTOR) == 0) {
            out->kind = KIND_BIN_APP;
            out->flash_len = stream.size;
            snprintf(out->identity, sizeof(out->identity), "%s", filename ? filename : "app.bin");
            return true;
        }
        snprintf(err, errlen, "Unsupported .bin size (%lu B)", (unsigned long)stream.size);
        return false;
    }
    // Fallback: try .fwsc
    return inspect_fwsc(stream, out, err, errlen);
}

bool read_flash_bytes(Stream &stream, const PackageInfo &pkg, uint32_t flash_addr, uint8_t *buf, size_t len) {
    if (pkg.kind == KIND_FWSC) {
        if (flash_addr > pkg.flash_len || len > pkg.flash_len - flash_addr) return false;
        return read_ufw(stream, pkg.ufw_woven_blocks, pkg.flash_off + flash_addr, buf, len);
    }
    if (pkg.kind == KIND_BIN_FULL) {
        if (flash_addr > pkg.flash_len || len > pkg.flash_len - flash_addr) return false;
        return stream.read_at(flash_addr, buf, len);
    }
    if (pkg.kind == KIND_BIN_APP) {
        if (flash_addr < BOOT_AREA) return false;
        uint32_t rel = flash_addr - BOOT_AREA;
        if (rel > pkg.flash_len || len > pkg.flash_len - rel) return false;
        return stream.read_at(rel, buf, len);
    }
    return false;
}

static bool app_read_stream(Stream &stream, const PackageInfo &pkg, uint32_t off, uint8_t *out, size_t n,
                            uint16_t chip_key) {
    uint32_t a = off - (off % 32);
    uint32_t skip = off % 32;
    size_t written = 0;
    uint8_t blk[32];
    while (written < n) {
        if (!read_flash_bytes(stream, pkg, BOOT_AREA + a, blk, 32)) return false;
        enc(blk, 32, (uint16_t)(chip_key ^ (uint16_t)(a >> 2)));
        size_t start = skip;
        size_t take = 32 - start;
        if (take > n - written) take = n - written;
        memcpy(out + written, blk + start, take);
        written += take;
        skip = 0;
        a += 32;
    }
    return true;
}

bool verify_app_area(Stream &stream, const PackageInfo &pkg, uint16_t chip_key, char *err, size_t errlen) {
    uint32_t files_checked = 0;
    for (uint32_t i = 0; i < 16; i++) {
        uint8_t raw[32];
        if (!app_read_stream(stream, pkg, i * 32, raw, 32, chip_key)) break;
        if (crc16_update(0, raw + 2, 30) != u16le(raw, 0)) break;
        uint16_t data_crc = u16le(raw, 2);
        uint32_t addr = u32le(raw, 4);
        uint32_t len = u32le(raw, 8);
        uint8_t attr = raw[12];
        if (attr == 0x82) {
            uint32_t end_addr = BOOT_AREA + addr + len;
            uint32_t max_end = pkg.kind == KIND_BIN_APP ? BOOT_AREA + pkg.flash_len : pkg.flash_len;
            if (addr > FLASH_SIZE || len > FLASH_SIZE || end_addr > max_end) {
                snprintf(err, errlen, "App file outside image bounds");
                return false;
            }
            uint16_t calc = 0;
            uint8_t buf[256];
            uint32_t done = 0;
            while (done < len) {
                size_t k = len - done < sizeof(buf) ? len - done : sizeof(buf);
                if (!app_read_stream(stream, pkg, addr + done, buf, k, chip_key)) {
                    snprintf(err, errlen, "Failed reading app file");
                    return false;
                }
                calc = crc16_update(calc, buf, k);
                done += (uint32_t)k;
            }
            if (calc != data_crc) {
                snprintf(err, errlen, "App CRC mismatch (wrong chip key 0x%04X?)", chip_key);
                return false;
            }
            files_checked++;
        }
    }
    if (files_checked == 0) {
        snprintf(err, errlen, "No valid app found for key 0x%04X", chip_key);
        return false;
    }
    return true;
}

bool model_matches(const PackageInfo &pkg, bool is_fm1, bool is_smk37) {
    if (pkg.kind != KIND_FWSC) return true;
    if (is_fm1) {
        return strcmp(pkg.model, "FM-1") == 0 || strcmp(pkg.model, "MiniFM") == 0;
    }
    if (is_smk37) {
        return strcmp(pkg.model, "SMK-37 Pro") == 0 || strcmp(pkg.model, "SMK-37 Elite") == 0 ||
               strcmp(pkg.model, "SMK-37+VA") == 0;
    }
    return false;
}

}  // namespace fw
