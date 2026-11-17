#include "flasher.h"

#include <M5Cardputer.h>
#include <stdio.h>
#include <string.h>

#include "engine.h"
#include "fw_file.h"
#include "modes.h"
#include "sdcard.h"
#include "ui.h"
#include "writer.h"

namespace flasher {

static constexpr int MAX_FILES = 48;
static constexpr uint32_t KEY_TIMEOUT_MS = 60000;

struct FileEntry {
    char path[96];
    char name[48];
    uint32_t size;
    bool is_fwsc;
};

static FileEntry files[MAX_FILES];
static int file_count;

struct SdStream : fw::Stream {
    File f;
    bool open(const char *path) {
        f = SD.open(path, FILE_READ);
        if (!f) return false;
        size = (uint32_t)f.size();
        return true;
    }
    void close() {
        if (f) f.close();
    }
    bool read_at(uint32_t offset, uint8_t *buf, size_t len) override {
        if (!f || offset > size || len > size - offset) return false;
        if (!f.seek(offset)) return false;
        return f.read(buf, len) == (int)len;
    }
};

static bool ends_with_ci(const char *s, const char *ext) {
    size_t sl = strlen(s), el = strlen(ext);
    if (sl < el) return false;
    const char *p = s + sl - el;
    for (size_t i = 0; i < el; i++) {
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)ext[i])) return false;
    }
    return true;
}

static void scan_dir(const char *dir_path) {
    File dir = SD.open(dir_path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }
    for (;;) {
        File f = dir.openNextFile();
        if (!f) break;
        if (!f.isDirectory() && file_count < MAX_FILES) {
            const char *raw_name = f.name();
            const char *base = strrchr(raw_name, '/');
            base = base ? base + 1 : raw_name;
            if (base[0] != '.' && (ends_with_ci(base, ".fwsc") || ends_with_ci(base, ".bin"))) {
                FileEntry &e = files[file_count++];
                if (strcmp(dir_path, "/") == 0) {
                    snprintf(e.path, sizeof(e.path), "/%s", base);
                } else {
                    snprintf(e.path, sizeof(e.path), "%s/%s", dir_path, base);
                }
                snprintf(e.name, sizeof(e.name), "%s", base);
                e.size = (uint32_t)f.size();
                e.is_fwsc = ends_with_ci(base, ".fwsc");
            }
        }
        f.close();
    }
    dir.close();
}

static void scan_sd() {
    file_count = 0;
    if (!sd_ready()) return;
    scan_dir("/fm1/firmware");
    scan_dir("/fm1");
    scan_dir("/");
}

static void draw_browser(int sel, int scroll) {
    lcd.startWrite();
    lcd.fillScreen(TFT_BLACK);
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(2);
    lcd.setTextColor(ui::CYAN, TFT_BLACK);
    lcd.setCursor(4, 2);
    lcd.print("SD Firmware");

    lcd.setTextSize(1);
    if (file_count == 0) {
        lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        lcd.setCursor(4, 30);
        lcd.print(sd_ok ? "No .fwsc or .bin found in:" : "No SD card (FAT32) detected.");
        if (sd_ok) {
            lcd.setCursor(4, 44);
            lcd.print("/fm1/firmware/ , /fm1/ , /");
        }
        lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
        lcd.setCursor(4, 124);
        lcd.print("R rescan   ESC back");
        lcd.endWrite();
        return;
    }

    static constexpr int VISIBLE = 9;
    for (int i = 0; i < VISIBLE && (scroll + i) < file_count; i++) {
        int idx = scroll + i;
        const FileEntry &e = files[idx];
        int y = 24 + i * 11;
        if (idx == sel) {
            lcd.fillRect(0, y - 1, 240, 10, 0x0210);
            lcd.setTextColor(TFT_WHITE, 0x0210);
        } else {
            lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        }
        char line[41];
        snprintf(line, sizeof(line), "%c%-26.26s %4luK %s", idx == sel ? '>' : ' ', e.name,
                 (unsigned long)(e.size / 1024), e.is_fwsc ? "FWSC" : "BIN ");
        lcd.setCursor(2, y);
        lcd.print(line);
    }

    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setCursor(2, 124);
    lcd.print(";/ . move  ENTER pick  R scan  ESC back");
    lcd.endWrite();
}

static bool key_poll(const usbkey::KeyStats *s) {
    static uint32_t last_draw;
    uint32_t now = millis();
    if (now - last_draw > 500) {
        last_draw = now;
        ui::status("%lu pkt %lus %s", (unsigned long)s->packets, (unsigned long)((now - 0) / 1000),
                   s->alive ? "alive" : "-");
        vTaskDelay(1);
    }
    return ui::any_key_cancel();
}

static void calibrating(bool) { ui::console("Calibrating", TFT_YELLOW); }
static bool g0_cancel() { return ui::g0_pressed(); }
static void read_progress(uint32_t done, uint32_t total) {
    ui::status("Backup %3lu%%", (unsigned long)(done * 100 / (total ? total : 1)));
}
static void compare_progress(uint32_t done, uint32_t total) {
    ui::status("Comparing %lu/%lu", (unsigned long)done, (unsigned long)total);
}
static void write_progress(uint32_t done, uint32_t total) {
    ui::status("Writing %lu/%lu - keep ON", (unsigned long)done, (unsigned long)total);
}

static int wait_choice(const char *keys) {
    ui::wait_released();
    uint32_t last_ping = millis();
    for (;;) {
        int k = ui::read_key();
        if (k && strchr(keys, k)) return k;
        delay(20);
        if (millis() - last_ping > 1000) {
            last_ping = millis();
            if (!engine::keepalive()) return 0;
        }
    }
}

struct FlashSourceCtx {
    SdStream *stream;
    const fw::PackageInfo *pkg;
};

static bool flash_read_sector(void *ctx_ptr, uint32_t addr, uint8_t out[writer::SECTOR], char *err, size_t errlen) {
    FlashSourceCtx *c = (FlashSourceCtx *)ctx_ptr;
    if (!fw::read_flash_bytes(*c->stream, *c->pkg, addr, out, writer::SECTOR)) {
        snprintf(err, errlen, "SD read failed at 0x%05lX", (unsigned long)addr);
        return false;
    }
    return true;
}

static void flash_selected(const FileEntry &entry) {
    char err[128] = {};
    SdStream stream;
    ui::clear_log();
    ui::console("Checking file", TFT_CYAN);
    ulog("%s (%lu B)", entry.name, (unsigned long)entry.size);
    if (!stream.open(entry.path)) {
        ui::console("Open failed", TFT_RED);
        ulog("Cannot open %s", entry.path);
        ui::status("Any key: back");
        ui::wait_released();
        while (!ui::read_key()) delay(20);
        return;
    }

    fw::PackageInfo pkg;
    ui::status("Verifying package CRC...");
    if (!fw::inspect(stream, entry.name, &pkg, err, sizeof(err))) {
        stream.close();
        ui::console("Invalid file", TFT_RED);
        ulog("%s", err);
        ui::status("Any key: back");
        ui::wait_released();
        while (!ui::read_key()) delay(20);
        return;
    }

    ulog("ID: %s", pkg.identity);
    ulog("Payload: %lu KiB", (unsigned long)(pkg.flash_len / 1024));
    ulog("Connect FM-1 via USB-C.");
    ulog("Unit OFF: switch ON when ready.");
    ulog("Unit ON:  auto MIDI soft-key.");
    ui::status("ENTER connect  ESC cancel");
    ui::wait_released();
    int k = 0;
    while (!(k = ui::read_key()) || !strchr("\n\x1b", k)) delay(20);
    if (k == 27) {
        stream.close();
        return;
    }

    // Connect: try USB_KEY first; if D+ is already high, fall back to USB-MIDI soft key!
    ui::clear_log();
    ui::console("Connecting", TFT_YELLOW);
    ulog("Switch FM-1 ON (or leave ON for MIDI)...");
    usbkey::KeyStats ks;
    engine::EnterResult er = engine::enter_download_mode({key_poll, calibrating, g0_cancel}, KEY_TIMEOUT_MS, &ks);
    jl::Info info;
    engine::ConnectResult cr = engine::CONN_NO_DEVICE;

    if (er == engine::ENTER_ALREADY_ON) {
        ulog("Unit is ON: trying USB-MIDI soft key...");
        ui::status("Sending MIDI soft key...");
        cr = engine::connect_midi(&info, err, sizeof(err));
    } else if (er == engine::ENTER_OK) {
        ui::status("Connecting USB Host...");
        cr = engine::connect(&info, err, sizeof(err));
    } else {
        stream.close();
        ui::console(er == engine::ENTER_CANCELLED ? "Cancelled" : "No answer", TFT_RED);
        ui::status("Any key: back");
        ui::wait_released();
        while (!ui::read_key()) delay(20);
        return;
    }

    if (cr != engine::CONN_OK) {
        stream.close();
        ui::console("Connect failed", TFT_RED);
        ulog("%s", err);
        ui::status("Any key: restart Cardputer");
        ui::wait_released();
        while (!ui::read_key()) delay(20);
        modes::restart_into(modes::GUIDED);
    }

    ui::console("Connected", TFT_GREEN);
    bool fm1 = engine::unit.model == engine::MODEL_FM1;
    bool smk37 = engine::unit.model == engine::MODEL_SMK37;
    ulog("%s | Flash %06lX %s", engine::unit.name, (unsigned long)info.flash_id,
         engine::flash_chip_name(info.flash_id));
    ulog("Chip key: %04X", info.chip_key);

    if (pkg.kind == fw::KIND_FWSC && !fw::model_matches(pkg, fm1, smk37)) {
        // Warn if model string doesn't match FM-1/MiniFM (e.g. SMK-37 package on FM-1)
        if ((smk37 && strstr(pkg.model, "FM-1")) || (fm1 && strstr(pkg.model, "SMK"))) {
            stream.close();
            ui::console("Wrong model!", TFT_RED);
            ulog("Package %s is not for %s.", pkg.model, engine::unit.name);
            ui::status("Any key: restart");
            wait_choice("\n\x1b");
            modes::restart_into(modes::GUIDED);
        }
        ulog("Note: custom ID '%s' (verifying key...)", pkg.model);
    }

    if (!fw::verify_app_area(stream, pkg, info.chip_key, err, sizeof(err))) {
        stream.close();
        ui::console("Key mismatch", TFT_RED);
        ulog("%s", err);
        ui::status("Any key: restart");
        wait_choice("\n\x1b");
        modes::restart_into(modes::GUIDED);
    }
    ulog("App CRC16 OK for key %04X!", info.chip_key);

    // Mandatory backup before writing
    char bak[48] = {};
    if (!engine::backup(info, bak, sizeof(bak), nullptr, read_progress)) {
        stream.close();
        ui::console("Backup failed", TFT_RED);
        ulog("Nothing written.");
        ui::status("Any key: restart");
        wait_choice("\n\x1b");
        modes::restart_into(modes::GUIDED);
    }

    FlashSourceCtx sctx{&stream, &pkg};
    uint32_t hi = (pkg.kind == fw::KIND_BIN_APP) ? (writer::BOOT_AREA + pkg.flash_len) : pkg.flash_len;
    if (hi > writer::END) hi = writer::END;
    writer::Source src{
        entry.name,
        writer::BOOT_AREA,
        hi,
        smk37 ? 0xC0000u : 0u,
        smk37 ? 0xC2000u : 0u,
        flash_read_sector,
        &sctx,
    };

    ui::status("Comparing sectors...");
    writer::Plan plan;
    if (!writer::plan_source(src, &plan, compare_progress, err, sizeof(err))) {
        stream.close();
        ui::console("Compare failed", TFT_RED);
        ulog("%s", err);
        ui::status("Any key: restart");
        wait_choice("\n\x1b");
        modes::restart_into(modes::GUIDED);
    }

    if (!plan.to_write && !plan.to_erase) {
        stream.close();
        ui::console("Already up to date", TFT_GREEN);
        ulog("Flash already matches %s.", entry.name);
        jl::restart(err, sizeof(err));
        ui::status("Any key: restart");
        wait_choice("\n\x1b");
        modes::restart_into(modes::GUIDED);
    }

    ui::clear_log();
    ui::console("Ready to flash", TFT_ORANGE);
    ulog("File: %s", entry.name);
    ulog("Backup: %s", bak);
    ulog("Write: %lu, Erase: %lu sectors", (unsigned long)plan.to_write, (unsigned long)plan.to_erase);
    ulog("Kept: Boot 0x0000-0x3FFF & 0xFF000-");
    ui::status("ENTER / Y flash now   ESC cancel");
    int ch = wait_choice("\ny\x1b");
    if (ch != '\n' && ch != 'y') {
        stream.close();
        ulog("Cancelled: nothing written.");
        jl::restart(err, sizeof(err));
        modes::restart_into(modes::GUIDED);
    }

    ui::clear_log();
    ui::console("Flashing...", TFT_YELLOW);
    uint32_t t0 = millis();
    if (!writer::apply_source(src, plan, ulog, write_progress, err, sizeof(err))) {
        stream.close();
        ui::console("FLASH FAILED", TFT_RED);
        ulog("%s", err);
        ulog("Do NOT switch unit off!");
        ui::status("Any key: restart");
        wait_choice("\n\x1b");
        modes::restart_into(modes::GUIDED);
    }
    stream.close();
    jl::restart(err, sizeof(err));
    ui::console("Flash complete!", TFT_GREEN);
    ulog("Done in %lu s. Unit restarted.", (unsigned long)((millis() - t0) / 1000));
    ui::status("ENTER finish");
    ui::wait_released();
    while (!ui::read_key()) delay(20);
    modes::restart_into(modes::GUIDED);
}

void flash_path(const char *path) {
    if (!sd_ready()) return;
    File f = SD.open(path, FILE_READ);
    if (!f) return;
    FileEntry e{};
    snprintf(e.path, sizeof(e.path), "%s", path);
    const char *base = strrchr(path, '/');
    snprintf(e.name, sizeof(e.name), "%s", base ? base + 1 : path);
    e.size = (uint32_t)f.size();
    e.is_fwsc = ends_with_ci(e.name, ".fwsc");
    f.close();
    flash_selected(e);
}

void run() {
    scan_sd();
    int sel = 0, scroll = 0;
    static constexpr int VISIBLE = 9;
    draw_browser(sel, scroll);
    ui::wait_released();

    for (;;) {
        int k = ui::read_key();
        if (!k) {
            delay(15);
            continue;
        }
        if (k == 27) {
            ui::wait_released();
            return;
        }
        if (k == 'r') {
            scan_sd();
            sel = scroll = 0;
            draw_browser(sel, scroll);
            ui::wait_released();
            continue;
        }
        if (file_count > 0) {
            if (k == ';' || k == 'w' || k == 'k' || k == ',') {
                if (sel > 0) sel--;
                if (sel < scroll) scroll = sel;
                draw_browser(sel, scroll);
            } else if (k == '.' || k == 's' || k == 'j' || k == '/') {
                if (sel + 1 < file_count) sel++;
                if (sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;
                draw_browser(sel, scroll);
            } else if (k == '\n') {
                ui::wait_released();
                flash_selected(files[sel]);
                scan_sd();
                if (sel >= file_count) sel = file_count > 0 ? file_count - 1 : 0;
                draw_browser(sel, scroll);
                ui::wait_released();
            }
        }
    }
}

}  // namespace flasher
