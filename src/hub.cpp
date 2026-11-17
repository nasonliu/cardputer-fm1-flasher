#include "hub.h"

#include <HTTPClient.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <esp_netif.h>
#include <lwip/dns.h>
#include <lwip/netif.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flasher.h"
#include "fw_file.h"
#include "sdcard.h"
#include "ui.h"

namespace hub {

static constexpr const char *HUB_HOST = "https://thefirmwarehub.com";
static constexpr const char *CATALOG_URL = "https://thefirmwarehub.com/catalog.json";
static constexpr const char *INDEX_PATH = "/fm1/hub_index.dat";
static constexpr const char *WIFI_PATH = "/fm1/wifi.txt";
static constexpr uint32_t INDEX_MAGIC = 0x48554231;  // "HUB1"
static constexpr int MAX_ENTRIES = 256;
static constexpr int MAX_RAM_ENTRIES = 80;

enum ChipCompat : uint8_t { CHIP_OLD = 0, CHIP_NEW = 1, CHIP_ANY = 2 };

struct __attribute__((packed)) HubEntry {
    char name[32];
    char author[40];
    char version[28];
    char pkg[96];
    char sha256[65];
    char kind[20];
    uint32_t size;
    uint8_t chip;
};

struct IndexHeader {
    uint32_t magic;
    uint32_t count;
};

// In-memory catalog cache (used ONLY when no SD card is inserted)
static HubEntry *ram_entries = nullptr;
static int ram_count = 0;

// Filtered view indices
static uint16_t view_idx[MAX_ENTRIES];
static int total_entries = 0;
static int view_count = 0;
static bool filter_new_only = true;
static char search_q[24] = "";

static void free_ram_buf() {
    if (ram_entries) {
        free(ram_entries);
        ram_entries = nullptr;
    }
    ram_count = 0;
}

static bool ensure_ram_buf() {
    if (!ram_entries) {
        ram_entries = (HubEntry *)calloc(MAX_RAM_ENTRIES, sizeof(HubEntry));
    }
    return ram_entries != nullptr;
}

static void wifi_off() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

static int read_ascii_key(bool *is_enter = nullptr, bool *is_bs = nullptr, bool *is_esc = nullptr) {
    if (is_enter) *is_enter = false;
    if (is_bs) *is_bs = false;
    if (is_esc) *is_esc = false;
    if (ui::g0_pressed()) {
        if (is_enter) *is_enter = true;
        return '\n';
    }
    M5Cardputer.update();
    auto &kb = M5Cardputer.Keyboard;
    if (!kb.isChange() || !kb.isPressed()) return 0;
    auto st = kb.keysState();
    if (st.enter) {
        if (is_enter) *is_enter = true;
        return '\n';
    }
    if (st.del) {
        if (is_bs) *is_bs = true;
        return 8;
    }
    if (!st.word.empty()) {
        char c = st.word[0];
        if (c == '`') {
            if (is_esc) *is_esc = true;
            return 27;
        }
        return (unsigned char)c;
    }
    return 0;
}

static bool prompt_text(const char *title, const char *subtitle, char *out, size_t maxlen, bool mask) {
    size_t len = strlen(out);
    auto redraw = [&]() {
        lcd.startWrite();
        lcd.fillScreen(TFT_BLACK);
        lcd.setFont(&fonts::Font0);
        lcd.setTextSize(2);
        lcd.setTextColor(ui::CYAN, TFT_BLACK);
        lcd.setCursor(4, 4);
        lcd.print(title);
        lcd.setTextSize(1);
        lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        lcd.setCursor(4, 28);
        lcd.print(subtitle);
        lcd.drawRoundRect(4, 48, 232, 24, 4, ui::CYAN);
        lcd.fillRect(6, 50, 228, 20, TFT_BLACK);
        lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        lcd.setCursor(8, 56);
        if (mask) {
            for (size_t i = 0; i < len && i < 35; i++) lcd.print('*');
        } else {
            lcd.print(len > 35 ? out + len - 35 : out);
        }
        lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
        lcd.setCursor(4, 124);
        lcd.print("ENTER confirm  DEL erase  ` cancel");
        lcd.endWrite();
    };
    redraw();
    ui::wait_released();
    for (;;) {
        bool ent = false, bs = false, esc = false;
        int c = read_ascii_key(&ent, &bs, &esc);
        if (!c) {
            delay(15);
            continue;
        }
        if (esc) {
            ui::wait_released();
            return false;
        }
        if (ent) {
            ui::wait_released();
            return true;
        }
        if (bs) {
            if (len > 0) {
                out[--len] = 0;
                redraw();
            }
        } else if (c >= 32 && c < 127 && len + 1 < maxlen) {
            out[len++] = (char)c;
            out[len] = 0;
            redraw();
        }
    }
}

static bool load_wifi_creds(char *ssid, size_t slen, char *pass, size_t plen) {
    Preferences prefs;
    if (prefs.begin("fm1hub", true)) {
        String s = prefs.getString("ssid", "");
        String p = prefs.getString("pass", "");
        prefs.end();
        if (!s.isEmpty()) {
            snprintf(ssid, slen, "%s", s.c_str());
            snprintf(pass, plen, "%s", p.c_str());
            return true;
        }
    }
    if (!sd_ready() || !SD.exists(WIFI_PATH)) return false;
    File f = SD.open(WIFI_PATH, FILE_READ);
    if (!f) return false;
    String s = f.readStringUntil('\n');
    String p = f.readStringUntil('\n');
    f.close();
    s.trim();
    p.trim();
    if (s.isEmpty()) return false;
    snprintf(ssid, slen, "%s", s.c_str());
    snprintf(pass, plen, "%s", p.c_str());
    return true;
}

static void save_wifi_creds(const char *ssid, const char *pass) {
    Preferences prefs;
    if (prefs.begin("fm1hub", false)) {
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();
    }
    if (sd_ready()) {
        File f = SD.open(WIFI_PATH, FILE_WRITE);
        if (f) {
            f.printf("%s\n%s\n", ssid, pass);
            f.close();
        }
    }
}

static bool connect_wifi_with(const char *ssid, const char *pass, bool *want_scan = nullptr) {
    if (want_scan) *want_scan = false;
    ui::clear_log();
    ui::console("Wi-Fi", ui::CYAN);
    ulog("Connecting to: %s", ssid);
    ui::status("W: pick other SSID   ESC: cancel");
    WiFi.mode(WIFI_STA);
    if (strcasecmp(ssid, "office2") == 0) {
        IPAddress ip(192, 168, 5, 218);
        IPAddress gw(192, 168, 5, 3);
        IPAddress mask(255, 255, 255, 0);
        WiFi.config(ip, gw, mask, gw, gw);
        ulog("Static IP: 192.168.5.218");
        ulog("GW & DNS:  192.168.5.3");
    } else {
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }
    delay(50);
    WiFi.begin(ssid, pass);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 > 15000) {
            ulog("Wi-Fi connection timed out.");
            wifi_off();
            return false;
        }
        int k = ui::read_key();
        if (k == 27) {
            ulog("Cancelled.");
            wifi_off();
            return false;
        }
        if (k == 'w') {
            if (want_scan) *want_scan = true;
            WiFi.disconnect();
            return false;
        }
        delay(100);
    }
    ulog("Connected! IP: %s", WiFi.localIP().toString().c_str());
    ulog("GW: %s  DNS: %s", WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP(0).toString().c_str());
    return true;
}

static bool wifi_setup_ui() {
    for (;;) {
        ui::clear_log();
        ui::console("Scanning Wi-Fi", ui::CYAN);
        ulog("Scanning 2.4 GHz networks...");
        ui::status("Please wait (~2s)...");
        WiFi.mode(WIFI_STA);
        delay(100);
        int n = WiFi.scanNetworks(false, false, false, 300);
        if (n <= 0) {
            ui::clear_log();
            ui::console("No Wi-Fi Found", ui::ORANGE);
            ulog("No 2.4 GHz networks detected.");
            ulog("Press R to rescan, T to type SSID,");
            ulog("or ESC to go back.");
            ui::status("R: rescan   T: type SSID   ESC: back");
            ui::wait_released();
            for (;;) {
                int k = ui::read_key();
                if (k == 27) {
                    wifi_off();
                    return false;
                }
                if (k == 'r' || k == '\n') break;
                if (k == 't') {
                    char ssid[64] = "", pass[64] = "";
                    if (!prompt_text("Wi-Fi SSID", "Enter 2.4GHz SSID:", ssid, sizeof(ssid), false)) {
                        wifi_off();
                        return false;
                    }
                    if (!prompt_text("Wi-Fi Password", ssid, pass, sizeof(pass), true)) {
                        wifi_off();
                        return false;
                    }
                    if (connect_wifi_with(ssid, pass)) {
                        save_wifi_creds(ssid, pass);
                        return true;
                    }
                    break;
                }
                delay(20);
            }
            continue;
        }

        int count = n > 24 ? 24 : n;
        int sel = 0, scroll = 0;
        static constexpr int VISIBLE = 8;
        auto draw = [&]() {
            lcd.startWrite();
            lcd.fillScreen(TFT_BLACK);
            lcd.setFont(&fonts::Font0);
            lcd.setTextSize(2);
            lcd.setTextColor(ui::CYAN, TFT_BLACK);
            lcd.setCursor(4, 2);
            lcd.print("Select Wi-Fi");
            lcd.setTextSize(1);
            for (int i = 0; i < VISIBLE && scroll + i < count; i++) {
                int idx = scroll + i;
                int y = 24 + i * 12;
                if (idx == sel) {
                    lcd.fillRect(0, y - 1, 240, 11, 0x0210);
                    lcd.setTextColor(TFT_WHITE, 0x0210);
                } else {
                    lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
                }
                String s = WiFi.SSID(idx);
                if (s.isEmpty()) s = "<Hidden SSID>";
                char line[41];
                snprintf(line, sizeof(line), "%c%-30.30s %4lddB", idx == sel ? '>' : ' ', s.c_str(),
                         (long)WiFi.RSSI(idx));
                lcd.setCursor(2, y);
                lcd.print(line);
            }
            lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
            lcd.setCursor(2, 124);
            lcd.print(";/ . move  ENTER ok  R scan  T type");
            lcd.endWrite();
        };
        draw();
        ui::wait_released();

        bool rescan = false;
        while (!rescan) {
            int k = ui::read_key();
            if (!k) {
                delay(15);
                continue;
            }
            if (k == 27) {
                WiFi.scanDelete();
                wifi_off();
                return false;
            }
            if (k == 'r') {
                WiFi.scanDelete();
                rescan = true;
            } else if (k == ';' || k == 'w' || k == 'k' || k == ',') {
                if (sel > 0) sel--;
                if (sel < scroll) scroll = sel;
                draw();
            } else if (k == '.' || k == 's' || k == 'j' || k == '/') {
                if (sel + 1 < count) sel++;
                if (sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;
                draw();
            } else if (k == 't') {
                char ssid[64] = "", pass[64] = "";
                WiFi.scanDelete();
                if (!prompt_text("Wi-Fi SSID", "Enter SSID:", ssid, sizeof(ssid), false)) {
                    rescan = true;
                    break;
                }
                if (!prompt_text("Wi-Fi Password", ssid, pass, sizeof(pass), true)) {
                    rescan = true;
                    break;
                }
                if (connect_wifi_with(ssid, pass)) {
                    save_wifi_creds(ssid, pass);
                    return true;
                }
                rescan = true;
            } else if (k == '\n') {
                char ssid[64] = "", pass[64] = "";
                snprintf(ssid, sizeof(ssid), "%s", WiFi.SSID(sel).c_str());
                bool open_net = (WiFi.encryptionType(sel) == WIFI_AUTH_OPEN);
                WiFi.scanDelete();
                if (!open_net) {
                    if (!prompt_text("Wi-Fi Password", ssid, pass, sizeof(pass), true)) {
                        rescan = true;
                        break;
                    }
                }
                if (connect_wifi_with(ssid, pass)) {
                    save_wifi_creds(ssid, pass);
                    return true;
                }
                rescan = true;
            }
        }
    }
}

static bool ensure_wifi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    char ssid[64] = "", pass[64] = "";
    if (load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        bool want_scan = false;
        if (connect_wifi_with(ssid, pass, &want_scan)) return true;
    }
    return wifi_setup_ui();
}

// Extracts `"key": "val"` or `"key": 1234` from a single line of pretty-printed catalog.json.
static bool parse_json_str(const char *line, const char *key, char *out, size_t maxlen) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\": \"", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < maxlen) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = 0;
    return true;
}

static bool parse_json_u32(const char *line, const char *key, uint32_t *out) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\": ", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    if (!isdigit((unsigned char)*p)) return false;
    *out = (uint32_t)strtoul(p, nullptr, 10);
    return true;
}

static bool fetch_catalog() {
    bool use_sd = sd_ready();
    if (use_sd) {
        free_ram_buf();
    } else if (!ensure_ram_buf()) {
        return false;
    }
    if (!ensure_wifi()) return false;

    ui::clear_log();
    ui::console("Updating Hub", ui::CYAN);
    ulog("Source: thefirmwarehub.com");
    ulog("GW: %s DNS: %s", WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP(0).toString().c_str());
    IPAddress host_ip;
    int dns_rc = Network.hostByName("thefirmwarehub.com", host_ip);
    ulog("Host IP: %s (dns=%d)", host_ip.toString().c_str(), dns_rc);
    ulog("Fetching catalog.json...");

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(25000);
    client.setHandshakeTimeout(20);
    HTTPClient http;
    http.useHTTP10(true);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setConnectTimeout(25000);
    http.setTimeout(25000);
    if (!http.begin(client, CATALOG_URL)) {
        ulog("HTTP begin failed.");
        return false;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        ulog("HTTP error: %d (%s)", code, http.errorToString(code).c_str());
        http.end();
        ui::status("Any key: continue");
        ui::wait_released();
        while (!ui::read_key()) delay(20);
        return false;
    }

    File idx;
    bool has_sd_idx = false;
    if (use_sd) {
        idx = SD.open(INDEX_PATH, FILE_WRITE);
        if (idx) {
            IndexHeader hdr{INDEX_MAGIC, 0};
            idx.write((const uint8_t *)&hdr, sizeof(hdr));
            has_sd_idx = true;
        }
    }

    int parsed_count = 0;
    ram_count = 0;
    WiFiClient *stream = http.getStreamPtr();
    int total_len = http.getSize();
    int bytes_read = 0;

    bool in_firmwares = false;
    bool done_firmwares = false;
    bool in_entry = false;
    HubEntry cur{};
    char line[1024];
    uint8_t net_buf[1024];
    size_t lpos = 0;

    auto process_line = [&](const char *ln) {
        if (done_firmwares) return;
        if (!in_firmwares) {
            if (strstr(ln, "\"firmwares\": [")) in_firmwares = true;
            return;
        }
        if (strncmp(ln, " ],", 3) == 0 || strcmp(ln, " ]") == 0) {
            in_firmwares = false;
            done_firmwares = true;
            return;
        }
        if (strcmp(ln, "  {") == 0) {
            memset(&cur, 0, sizeof(cur));
            in_entry = true;
            return;
        }
        if (strcmp(ln, "  },") == 0 || strcmp(ln, "  }") == 0) {
            if (in_entry && cur.pkg[0] && parsed_count < MAX_ENTRIES) {
                parsed_count++;
                if (has_sd_idx) {
                    idx.write((const uint8_t *)&cur, sizeof(cur));
                } else if (ram_entries && ram_count < MAX_RAM_ENTRIES) {
                    ram_entries[ram_count++] = cur;
                }
            }
            in_entry = false;
            return;
        }
        if (in_entry && strncmp(ln, "   \"", 4) == 0) {
            parse_json_str(ln, "name", cur.name, sizeof(cur.name));
            parse_json_str(ln, "author", cur.author, sizeof(cur.author));
            parse_json_str(ln, "version", cur.version, sizeof(cur.version));
            parse_json_str(ln, "pkg", cur.pkg, sizeof(cur.pkg));
            parse_json_str(ln, "sha256", cur.sha256, sizeof(cur.sha256));
            parse_json_str(ln, "kind", cur.kind, sizeof(cur.kind));
            uint32_t sz = 0;
            if (parse_json_u32(ln, "size", &sz)) cur.size = sz;
            char chip_s[16];
            if (parse_json_str(ln, "chip", chip_s, sizeof(chip_s))) {
                if (strcmp(chip_s, "new") == 0) cur.chip = CHIP_NEW;
                else if (strcmp(chip_s, "any") == 0) cur.chip = CHIP_ANY;
                else cur.chip = CHIP_OLD;
            }
        }
    };

    uint32_t last_ui = 0;
    uint32_t last_data = millis();
    while (!done_firmwares && (http.connected() || stream->available() > 0) &&
           (total_len <= 0 || bytes_read < total_len)) {
        int avail = stream->available();
        if (avail <= 0) {
            if (millis() - last_data > 15000) break;
            delay(5);
            continue;
        }
        last_data = millis();
        int n = stream->read(net_buf, sizeof(net_buf));
        if (n <= 0) {
            delay(2);
            continue;
        }
        bytes_read += n;
        for (int i = 0; i < n; i++) {
            char c = (char)net_buf[i];
            if (c == '\n') {
                line[lpos] = 0;
                process_line(line);
                lpos = 0;
                if (done_firmwares) break;
            } else if (c != '\r' && lpos + 1 < sizeof(line)) {
                line[lpos++] = c;
            }
        }
        if (millis() - last_ui > 250) {
            last_ui = millis();
            ui::status("Parsed %d fw (%d KB)", parsed_count, bytes_read / 1024);
        }
    }
    if (!done_firmwares && lpos > 0) {
        line[lpos] = 0;
        process_line(line);
    }

    if (has_sd_idx) {
        IndexHeader hdr{INDEX_MAGIC, (uint32_t)parsed_count};
        idx.seek(0);
        idx.write((const uint8_t *)&hdr, sizeof(hdr));
        idx.close();
    }
    http.end();

    ulog("Indexed %d firmwares!", parsed_count);
    return parsed_count > 0;
}

static bool ci_contains(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    size_t nl = strlen(needle);
    for (size_t i = 0; hay[i]; i++) {
        size_t j = 0;
        while (j < nl && hay[i + j] && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) j++;
        if (j == nl) return true;
    }
    return false;
}

static void rebuild_view() {
    total_entries = 0;
    view_count = 0;
    if (sd_ready() && SD.exists(INDEX_PATH)) {
        free_ram_buf();
        File idx = SD.open(INDEX_PATH, FILE_READ);
        if (!idx) return;
        IndexHeader hdr{};
        if (idx.read((uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr) && hdr.magic == INDEX_MAGIC) {
            total_entries = hdr.count > MAX_ENTRIES ? MAX_ENTRIES : (int)hdr.count;
            for (int i = 0; i < total_entries; i++) {
                HubEntry e;
                if (idx.read((uint8_t *)&e, sizeof(e)) != sizeof(e)) break;
                if (filter_new_only && e.chip == CHIP_OLD) continue;
                if (search_q[0] && !ci_contains(e.name, search_q) && !ci_contains(e.author, search_q) &&
                    !ci_contains(e.kind, search_q)) {
                    continue;
                }
                view_idx[view_count++] = (uint16_t)i;
            }
        }
        idx.close();
        return;
    }
    if (!ram_entries) return;
    total_entries = ram_count;
    for (int i = 0; i < total_entries; i++) {
        const HubEntry &e = ram_entries[i];
        if (filter_new_only && e.chip == CHIP_OLD) continue;
        if (search_q[0] && !ci_contains(e.name, search_q) && !ci_contains(e.author, search_q) &&
            !ci_contains(e.kind, search_q)) {
            continue;
        }
        view_idx[view_count++] = (uint16_t)i;
    }
}

static bool read_entry(int raw_idx, HubEntry *out) {
    if (sd_ready() && SD.exists(INDEX_PATH)) {
        File idx = SD.open(INDEX_PATH, FILE_READ);
        if (!idx) return false;
        uint32_t off = sizeof(IndexHeader) + (uint32_t)raw_idx * sizeof(HubEntry);
        bool ok = idx.seek(off) && idx.read((uint8_t *)out, sizeof(HubEntry)) == sizeof(HubEntry);
        idx.close();
        return ok;
    }
    if (!ram_entries || raw_idx < 0 || raw_idx >= ram_count) return false;
    *out = ram_entries[raw_idx];
    return true;
}

static void draw_hub_list(int sel, int scroll) {
    lcd.startWrite();
    lcd.fillScreen(TFT_BLACK);
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(1);
    lcd.setTextColor(ui::CYAN, TFT_BLACK);
    lcd.setCursor(2, 2);
    char hdr[41];
    snprintf(hdr, sizeof(hdr), "thefirmwarehub.com [%s] (%d)", filter_new_only ? "NEW" : "ALL", view_count);
    lcd.print(hdr);
    if (search_q[0]) {
        lcd.setTextColor(ui::GREEN, TFT_BLACK);
        lcd.setCursor(2, 12);
        lcd.printf("Filter: %s", search_q);
    } else {
        lcd.setTextColor(ui::GREY, TFT_BLACK);
        lcd.setCursor(2, 12);
        lcd.print("C:chip  /:search  U:update  W:wifi");
    }
    lcd.drawFastHLine(0, 22, 240, 0x2104);

    if (view_count == 0) {
        lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        lcd.setCursor(4, 42);
        lcd.print(total_entries == 0 ? "No catalog. Press U to download." : "No matching firmwares.");
        lcd.setCursor(4, 56);
        lcd.print("Press W to select Wi-Fi SSID.");
        lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
        lcd.setCursor(2, 124);
        lcd.print("W wifi  U update  C filter  ESC back");
        lcd.endWrite();
        return;
    }

    static constexpr int VISIBLE = 9;
    File idx;
    bool use_sd = sd_ready() && SD.exists(INDEX_PATH);
    if (use_sd) idx = SD.open(INDEX_PATH, FILE_READ);
    for (int i = 0; i < VISIBLE && scroll + i < view_count; i++) {
        HubEntry e{};
        if (use_sd && idx) {
            uint32_t off = sizeof(IndexHeader) + (uint32_t)view_idx[scroll + i] * sizeof(HubEntry);
            idx.seek(off);
            idx.read((uint8_t *)&e, sizeof(e));
        } else if (ram_entries && view_idx[scroll + i] < ram_count) {
            e = ram_entries[view_idx[scroll + i]];
        }
        int y = 25 + i * 11;
        if (scroll + i == sel) {
            lcd.fillRect(0, y - 1, 240, 10, 0x0210);
            lcd.setTextColor(TFT_WHITE, 0x0210);
        } else {
            lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        }
        const char *tag = e.chip == CHIP_OLD ? "OLD" : (e.chip == CHIP_NEW ? "NEW" : "ANY");
        char line[41];
        snprintf(line, sizeof(line), "%c%-17.17s %-8.8s %-8.8s %s", scroll + i == sel ? '>' : ' ', e.name, e.version,
                 e.author, tag);
        lcd.setCursor(2, y);
        lcd.print(line);
    }
    if (idx) idx.close();

    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setCursor(2, 124);
    lcd.print(";/ . move  ENTER select  W wifi  ESC");
    lcd.endWrite();
}

static bool download_firmware(const HubEntry &e, char *dst_path, size_t dst_len) {
    while (!sd_ready()) {
        ui::clear_log();
        ui::console("SD Card Needed", ui::ORANGE);
        ulog("Insert a FAT32 microSD card to save");
        ulog("%s (%lu KB).", e.name, (unsigned long)(e.size / 1024));
        ui::status("ENTER: retry SD   ESC: cancel");
        ui::wait_released();
        for (;;) {
            int k = ui::read_key();
            if (k == 27) return false;
            if (k == '\n') break;
            delay(20);
        }
    }
    free_ram_buf();

    if (!ensure_wifi()) return false;
    const char *base = strrchr(e.pkg, '/');
    base = base ? base + 1 : e.pkg;
    snprintf(dst_path, dst_len, "/fm1/firmware/%s", base);

    char url[160];
    snprintf(url, sizeof(url), "%s/%s", HUB_HOST, e.pkg);

    ui::clear_log();
    ui::console("Downloading", ui::CYAN);
    ulog("%s %s", e.name, e.version);
    ulog("By %s", e.author);
    IPAddress host_ip;
    int dns_rc = Network.hostByName("thefirmwarehub.com", host_ip);
    ulog("Host IP: %s (dns=%d)", host_ip.toString().c_str(), dns_rc);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(25000);
    client.setHandshakeTimeout(20);
    HTTPClient http;
    http.useHTTP10(true);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setConnectTimeout(25000);
    http.setTimeout(25000);
    if (!http.begin(client, url)) {
        ulog("HTTP begin failed.");
        return false;
    }
    int code = http.GET();
    if (code < 0) {
        char ssl_err[80] = "";
        int le = client.lastError(ssl_err, sizeof(ssl_err));
        ulog("Retry (err %d, ssl %d, heap %lu)...", code, le, (unsigned long)ESP.getFreeHeap());
        http.end();
        delay(500);
        if (!http.begin(client, url)) {
            ulog("HTTP begin failed.");
            return false;
        }
        code = http.GET();
    }
    if (code != HTTP_CODE_OK) {
        char ssl_err[80] = "";
        int le = client.lastError(ssl_err, sizeof(ssl_err));
        ulog("HTTP err %d (%s)", code, http.errorToString(code).c_str());
        if (le) ulog("SSL err %d: %s", le, ssl_err);
        http.end();
        return false;
    }

    int len = http.getSize();
    if (len <= 0 && e.size > 0) len = (int)e.size;

    File out = SD.open(dst_path, FILE_WRITE);
    if (!out) {
        ulog("SD write failed.");
        http.end();
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    WiFiClient *stream = http.getStreamPtr();
    static uint8_t dl_buf[2048];
    int done = 0;
    uint32_t last_data = millis(), last_ui = 0;
    while ((http.connected() || stream->available() > 0) && (len <= 0 || done < len)) {
        int avail = stream->available();
        if (avail <= 0) {
            if (millis() - last_data > 15000) break;
            delay(2);
            continue;
        }
        last_data = millis();
        int n = stream->read(dl_buf, sizeof(dl_buf));
        if (n <= 0) {
            delay(2);
            continue;
        }
        if (out.write(dl_buf, n) != (size_t)n) {
            out.close();
            http.end();
            mbedtls_sha256_free(&sha);
            SD.remove(dst_path);
            ulog("SD disk write error.");
            return false;
        }
        mbedtls_sha256_update(&sha, dl_buf, n);
        done += n;
        if (millis() - last_ui > 200) {
            last_ui = millis();
            int pct = len > 0 ? (done * 100 / len) : 0;
            ui::status("%d%% (%d/%d KB)", pct, done / 1024, len / 1024);
        }
    }
    out.close();
    http.end();

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (e.sha256[0] && strcasecmp(hex, e.sha256) != 0) {
        SD.remove(dst_path);
        ulog("SHA-256 mismatch (%d/%d B)!", done, len);
        return false;
    }

    ulog("Download & SHA-256 OK!");
    return true;
}

static void show_detail(const HubEntry &e) {
    ui::clear_log();
    ui::console(e.name, ui::CYAN);
    ulog("Version: %s", e.version);
    ulog("Maker:   %s", e.author);
    ulog("Kind:    %s (%lu KB)", e.kind, (unsigned long)(e.size / 1024));
    ulog("Chip:    %s", e.chip == CHIP_OLD ? "OLD 0x856014 ONLY"
                        : (e.chip == CHIP_NEW ? "NEW 0xB36014 OK" : "ANY (0x856014/0xB36014)"));
    ulog("Hub:     thefirmwarehub.com");
    if (e.chip == CHIP_OLD) ulog("WARN: marked OLD chip only!");
    ui::status("ENTER download   ESC back");
    ui::wait_released();
    for (;;) {
        int k = ui::read_key();
        if (k == 27) return;
        if (k == '\n') break;
        delay(20);
    }

    char path[128];
    if (!download_firmware(e, path, sizeof(path))) {
        ui::status("Any key: back");
        ui::wait_released();
        while (!ui::read_key()) delay(20);
        return;
    }

    // Turn Wi-Fi off before USB Host / flashing to save battery & RAM
    wifi_off();
    ui::status("ENTER flash FM-1  ESC back");
    ui::wait_released();
    for (;;) {
        int k = ui::read_key();
        if (k == 27) return;
        if (k == '\n') {
            flasher::flash_path(path);
            return;
        }
        delay(20);
    }
}

void debug_test() {
    ulog("=== HUB DEBUG TEST ===");
    ulog("Free heap: %lu, MaxAlloc: %lu", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
    char ssid[64] = "", pass[64] = "";
    if (!load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ulog("No saved Wi-Fi creds in NVS/SD.");
        return;
    }
    ulog("Loaded SSID: %s", ssid);
    if (!ensure_wifi()) {
        ulog("ensure_wifi() failed");
        return;
    }
    ulog("After WiFi heap: %lu, MaxAlloc: %lu", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
    if (!fetch_catalog()) {
        ulog("fetch_catalog() failed");
    } else {
        ulog("fetch_catalog() OK: total=%d", total_entries);
    }
    HubEntry test_e{};
    snprintf(test_e.name, sizeof(test_e.name), "Hortator");
    snprintf(test_e.version, sizeof(test_e.version), "1.0.1");
    snprintf(test_e.author, sizeof(test_e.author), "DEADACTIVE");
    snprintf(test_e.pkg, sizeof(test_e.pkg), "fw/hortator-1.0.1.fwsc");
    test_e.size = 609649;
    char dst[128];
    bool ok = download_firmware(test_e, dst, sizeof(dst));
    ulog("download_firmware() => %s", ok ? "OK" : "FAIL");
    wifi_off();
}

void run() {
    rebuild_view();

    // Always connect Wi-Fi / show SSID selection when entering Wi-Fi Hub if not yet loaded
    if (total_entries == 0) {
        if (fetch_catalog()) {
            rebuild_view();
        }
    }

    int sel = 0, scroll = 0;
    static constexpr int VISIBLE = 9;
    draw_hub_list(sel, scroll);
    ui::wait_released();

    for (;;) {
        int k = ui::read_key();
        if (!k) {
            delay(15);
            continue;
        }
        if (k == 27) {
            wifi_off();
            ui::wait_released();
            return;
        }
        if (k == 'c') {
            filter_new_only = !filter_new_only;
            rebuild_view();
            sel = scroll = 0;
            draw_hub_list(sel, scroll);
        } else if (k == 'u') {
            if (fetch_catalog()) rebuild_view();
            sel = scroll = 0;
            draw_hub_list(sel, scroll);
            ui::wait_released();
        } else if (k == 'w') {
            if (wifi_setup_ui()) {
                if (fetch_catalog()) rebuild_view();
            }
            draw_hub_list(sel, scroll);
            ui::wait_released();
        } else if (k == '/' || k == 's') {
            prompt_text("Search Hub", "Name / Author / Kind (empty=all):", search_q, sizeof(search_q), false);
            rebuild_view();
            sel = scroll = 0;
            draw_hub_list(sel, scroll);
            ui::wait_released();
        } else if (view_count > 0) {
            if (k == ';' || k == 'k' || k == ',') {
                if (sel > 0) sel--;
                if (sel < scroll) scroll = sel;
                draw_hub_list(sel, scroll);
            } else if (k == '.' || k == 'j') {
                if (sel + 1 < view_count) sel++;
                if (sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;
                draw_hub_list(sel, scroll);
            } else if (k == '\n') {
                static HubEntry e;
                if (read_entry(view_idx[sel], &e)) {
                    show_detail(e);
                }
                draw_hub_list(sel, scroll);
                ui::wait_released();
            }
        }
    }
}

}  // namespace hub
