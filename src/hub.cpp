#include "hub.h"

#include <HTTPClient.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
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
static constexpr uint32_t INDEX_MAGIC = 0x48554232;  // "HUB2"
static constexpr int MAX_ENTRIES = 256;
static constexpr int MAX_RAM_ENTRIES = 60;

enum ChipCompat : uint8_t { CHIP_OLD = 0, CHIP_NEW = 1, CHIP_ANY = 2 };

struct __attribute__((packed)) HubEntry {
    char name[32];
    char author[36];
    char version[24];
    char pkg[88];
    char sha256[65];
    char kind[16];
    char what[220];
    uint32_t size;
    uint16_t stars;
    uint8_t chip;
};

struct IndexHeader {
    uint32_t magic;
    uint32_t count;
};

enum CatId : uint8_t {
    CAT_ALL = 0,
    CAT_SYNTH = 1,
    CAT_GROOVE = 2,
    CAT_DRUMS = 3,
    CAT_TRACKER = 4,
    CAT_OTHER = 5,
    CAT_COUNT = 6,
};

static const char *const CAT_NAMES[CAT_COUNT] = {"All", "Synth", "Groove", "Drums", "Tracker", "Chords+"};
static const char *const CAT_SUBS[CAT_COUNT] = {
    "Every compatible firmware",
    "Leads, FM, poly & wavetable",
    "Sequencers & grooveboxes",
    "Drum machines & samplers",
    "Pattern & hex trackers",
    "Chord engines, MIDI & tools",
};
static const uint16_t CAT_COLORS[CAT_COUNT] = {
    ui::CYAN, 0x3E7F, ui::GREEN, ui::ORANGE, 0xF81F, ui::YELLOW,
};

// In-memory catalog cache (used ONLY when no SD card is inserted)
static HubEntry *ram_entries = nullptr;
static int ram_count = 0;

// Filtered view indices for the active category
static uint16_t view_idx[MAX_ENTRIES];
static int total_entries = 0;
static int view_count = 0;
static int cat_counts[CAT_COUNT];
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

// Extracts `"key": "val"` from a single line of pretty-printed catalog.json,
// cleaning simple JSON escapes (\u2014 -> '-', etc.) for Font0 ASCII display.
static bool parse_json_str(const char *line, const char *key, char *out, size_t maxlen) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\": \"", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < maxlen) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
                char hex[5] = {p[1], p[2], p[3], p[4], 0};
                unsigned long cp = strtoul(hex, nullptr, 16);
                p += 5;
                if (cp == 0x2013 || cp == 0x2014) out[i++] = '-';
                else if (cp == 0x2018 || cp == 0x2019) out[i++] = '\'';
                else if (cp == 0x201C || cp == 0x201D) out[i++] = '"';
                else if (cp == 0x00FC || cp == 0x00FB) out[i++] = 'u';
                else if (cp == 0x00E9 || cp == 0x00E8) out[i++] = 'e';
                else if (cp >= 32 && cp < 127) out[i++] = (char)cp;
                continue;
            }
        }
        unsigned char c = (unsigned char)*p++;
        out[i++] = (c >= 32 && c < 127) ? (char)c : ' ';
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

static CatId kind_to_cat(const char *kind) {
    if (strcasecmp(kind, "synth") == 0) return CAT_SYNTH;
    if (strcasecmp(kind, "groove") == 0) return CAT_GROOVE;
    if (strcasecmp(kind, "drums") == 0) return CAT_DRUMS;
    if (strcasecmp(kind, "tracker") == 0) return CAT_TRACKER;
    return CAT_OTHER;  // chords, other, dev
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
    bool in_kind_arr = false;
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
            in_kind_arr = false;
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
            in_kind_arr = false;
            return;
        }
        if (!in_entry) return;
        if (in_kind_arr) {
            if (strncmp(ln, "   ]", 4) == 0) {
                in_kind_arr = false;
            } else if (!cur.kind[0]) {
                const char *q1 = strchr(ln, '"');
                if (q1) {
                    q1++;
                    const char *q2 = strchr(q1, '"');
                    if (q2 && q2 > q1) {
                        size_t len = (size_t)(q2 - q1);
                        if (len >= sizeof(cur.kind)) len = sizeof(cur.kind) - 1;
                        memcpy(cur.kind, q1, len);
                        cur.kind[len] = 0;
                    }
                }
            }
            return;
        }
        if (strncmp(ln, "   \"kind\": [", 12) == 0) {
            in_kind_arr = true;
            return;
        }
        if (strncmp(ln, "   \"", 4) == 0) {
            parse_json_str(ln, "name", cur.name, sizeof(cur.name));
            parse_json_str(ln, "author", cur.author, sizeof(cur.author));
            parse_json_str(ln, "version", cur.version, sizeof(cur.version));
            parse_json_str(ln, "pkg", cur.pkg, sizeof(cur.pkg));
            parse_json_str(ln, "sha256", cur.sha256, sizeof(cur.sha256));
            parse_json_str(ln, "kind", cur.kind, sizeof(cur.kind));
            parse_json_str(ln, "what", cur.what, sizeof(cur.what));
            uint32_t v = 0;
            if (parse_json_u32(ln, "size", &v)) cur.size = v;
            if (parse_json_u32(ln, "stars", &v)) cur.stars = (uint16_t)(v > 65535 ? 65535 : v);
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

static bool entry_matches(const HubEntry &e, CatId cat) {
    if (filter_new_only && e.chip == CHIP_OLD) return false;
    if (cat != CAT_ALL && kind_to_cat(e.kind) != cat) return false;
    if (search_q[0] && !ci_contains(e.name, search_q) && !ci_contains(e.author, search_q) &&
        !ci_contains(e.kind, search_q) && !ci_contains(e.what, search_q)) {
        return false;
    }
    return true;
}

static void rebuild_view(CatId cat) {
    total_entries = 0;
    view_count = 0;
    memset(cat_counts, 0, sizeof(cat_counts));

    auto tally = [&](const HubEntry &e, int idx) {
        if (filter_new_only && e.chip == CHIP_OLD) return;
        if (search_q[0] && !ci_contains(e.name, search_q) && !ci_contains(e.author, search_q) &&
            !ci_contains(e.kind, search_q) && !ci_contains(e.what, search_q)) {
            return;
        }
        cat_counts[CAT_ALL]++;
        cat_counts[kind_to_cat(e.kind)]++;
        if (cat == CAT_ALL || kind_to_cat(e.kind) == cat) {
            if (view_count < MAX_ENTRIES) view_idx[view_count++] = (uint16_t)idx;
        }
    };

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
                tally(e, i);
            }
        }
        idx.close();
        return;
    }
    if (!ram_entries) return;
    total_entries = ram_count;
    for (int i = 0; i < total_entries; i++) {
        tally(ram_entries[i], i);
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

// Crisp vector preview artwork for each firmware category inside (x, y, w, h)
static void draw_cat_preview(CatId cat, int x, int y, int w, int h, uint16_t col, bool mini = false) {
    lcd.fillRoundRect(x, y, w, h, 4, 0x0841);
    lcd.drawRoundRect(x, y, w, h, 4, col);
    int cx = x + w / 2;
    int cy = y + h / 2;

    if (cat == CAT_SYNTH) {
        // Mini piano keys + sawtooth waveform
        int kw = w - 10, kx = x + 5, ky = y + h - (mini ? 12 : 18);
        int kh = mini ? 8 : 13;
        int nkeys = mini ? 5 : 7;
        int step = kw / nkeys;
        for (int i = 0; i < nkeys; i++) {
            lcd.fillRect(kx + i * step, ky, step - 1, kh, TFT_WHITE);
            if (i != 2 && i != 6 && i + 1 < nkeys) {
                lcd.fillRect(kx + i * step + step - 2, ky, 3, kh / 2 + 1, TFT_BLACK);
            }
        }
        int wy = y + (mini ? 8 : 13);
        int amp = mini ? 5 : 8;
        lcd.drawLine(x + 6, wy + amp, cx - 6, wy - amp, col);
        lcd.drawLine(cx - 6, wy - amp, cx - 6, wy + amp, col);
        lcd.drawLine(cx - 6, wy + amp, cx + 8, wy - amp, col);
        lcd.drawLine(cx + 8, wy - amp, cx + 8, wy + amp, col);
        lcd.drawLine(cx + 8, wy + amp, x + w - 6, wy - amp, col);
    } else if (cat == CAT_GROOVE) {
        // 4x2 (mini) or 4x3 (full) step sequencer pad grid
        int rows = mini ? 2 : 3;
        int cols = 4;
        int pw = (w - 14) / cols;
        int ph = (h - 10) / rows;
        int sx = x + (w - cols * pw) / 2 + 1;
        int sy = y + (h - rows * ph) / 2 + 1;
        static const uint8_t PAT[12] = {1, 0, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1};
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                bool on = PAT[(r * cols + c) % 12];
                uint16_t pc = on ? (c == 0 ? TFT_WHITE : col) : 0x2104;
                lcd.fillRoundRect(sx + c * pw, sy + r * ph, pw - 2, ph - 2, 2, pc);
            }
        }
    } else if (cat == CAT_DRUMS) {
        // Drum pads + kick circle
        lcd.drawCircle(cx - (mini ? 8 : 12), cy, mini ? 7 : 12, col);
        lcd.fillCircle(cx - (mini ? 8 : 12), cy, mini ? 3 : 5, col);
        int px = cx + (mini ? 2 : 4);
        int sz = mini ? 7 : 11;
        lcd.fillRoundRect(px, cy - sz - 1, sz, sz, 2, TFT_WHITE);
        lcd.fillRoundRect(px + sz + 2, cy - sz - 1, sz, sz, 2, col);
        lcd.fillRoundRect(px, cy + 2, sz, sz, 2, col);
        lcd.fillRoundRect(px + sz + 2, cy + 2, sz, sz, 2, 0x3186);
    } else if (cat == CAT_TRACKER) {
        // Tracker pattern columns
        int rows = mini ? 4 : 6;
        int sy = y + 5;
        int rh = (h - 10) / rows;
        for (int r = 0; r < rows; r++) {
            int ry = sy + r * rh;
            uint16_t rc = (r == 1) ? TFT_WHITE : col;
            lcd.fillRect(x + 5, ry, 8, rh - 2, rc);
            lcd.fillRect(x + 16, ry, (w - 28) / 2, rh - 2, (r % 2 == 0) ? col : 0x2104);
            lcd.fillRect(x + 19 + (w - 28) / 2, ry, (w - 28) / 2, rh - 2, (r % 2 == 1) ? col : 0x2104);
        }
    } else if (cat == CAT_OTHER) {
        // Chords / Tools: 3 stacked harmonic bars + FM-1 badge
        int bw = w - 16;
        int bh = mini ? 4 : 6;
        lcd.fillRoundRect(x + 8, y + 6, bw, bh, 2, col);
        lcd.fillRoundRect(x + 8, y + 6 + bh + 3, bw - 8, bh, 2, TFT_WHITE);
        lcd.fillRoundRect(x + 8, y + 6 + (bh + 3) * 2, bw - 4, bh, 2, col);
    } else {
        // ALL: Globe / Hub signal arcs + grid
        lcd.drawCircle(cx, cy, mini ? 9 : 14, col);
        lcd.drawFastHLine(cx - (mini ? 9 : 14), cy, mini ? 18 : 28, col);
        lcd.drawFastVLine(cx, cy - (mini ? 9 : 14), mini ? 18 : 28, col);
        lcd.drawEllipse(cx, cy, mini ? 5 : 7, mini ? 9 : 14, col);
    }
}

// Word-wraps text in [x, x+w) using Font0 (6x8), starting at line `skip_lines`
static int draw_wrapped(const char *s, int x, int y, int w, int max_lines, uint16_t fg, uint16_t bg,
                        int skip_lines = 0) {
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(1);
    lcd.setTextColor(fg, bg);
    int max_chars = w / 6;
    if (max_chars < 4) max_chars = 4;
    if (max_chars > 38) max_chars = 38;

    int drawn = 0, line_idx = 0;
    const char *p = s;
    while (*p && drawn < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        int len = 0, last_sp = -1;
        while (p[len] && p[len] != '\n' && len < max_chars) {
            if (p[len] == ' ') last_sp = len;
            len++;
        }
        if (p[len] && p[len] != '\n' && last_sp > 0) len = last_sp;
        char buf[40];
        memcpy(buf, p, len);
        buf[len] = 0;
        if (line_idx >= skip_lines) {
            lcd.setCursor(x, y + drawn * 10);
            lcd.print(buf);
            drawn++;
        }
        line_idx++;
        p += len;
        if (*p == '\n') p++;
    }
    return drawn;
}

static void draw_category_menu(int sel) {
    lcd.startWrite();
    lcd.fillScreen(TFT_BLACK);
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(1);
    lcd.setTextColor(ui::CYAN, TFT_BLACK);
    lcd.setCursor(4, 3);
    lcd.print("thefirmwarehub.com");
    lcd.setTextDatum(top_right);
    lcd.setTextColor(filter_new_only ? ui::GREEN : ui::ORANGE, TFT_BLACK);
    lcd.drawString(filter_new_only ? "[NEW 0xB36014]" : "[ALL CHIPS]", 236, 3);
    lcd.setTextDatum(top_left);
    lcd.drawFastHLine(0, 14, 240, 0x2104);

    // 2 rows x 3 cols of category cards
    for (int i = 0; i < CAT_COUNT; i++) {
        int col = i % 3;
        int row = i / 3;
        int x = 4 + col * 78;
        int y = 18 + row * 47;
        int w = 74, h = 43;
        bool active = (i == sel);
        uint16_t ac = CAT_COLORS[i];
        uint16_t bg = active ? 0x1082 : TFT_BLACK;
        if (active) {
            lcd.fillRoundRect(x, y, w, h, 5, bg);
            lcd.drawRoundRect(x, y, w, h, 5, ac);
            lcd.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 4, ac);
        } else {
            lcd.drawRoundRect(x, y, w, h, 5, 0x3186);
        }
        draw_cat_preview((CatId)i, x + 5, y + 5, 24, 20, active ? ac : ui::GREY, true);
        lcd.setFont(&fonts::Font0);
        lcd.setTextSize(1);
        lcd.setTextColor(active ? TFT_WHITE : ui::GREY, bg);
        lcd.setCursor(x + 33, y + 7);
        lcd.printf("%d", cat_counts[i]);
        lcd.setTextColor(active ? ac : TFT_LIGHTGREY, bg);
        lcd.setCursor(x + 5, y + 30);
        lcd.print(CAT_NAMES[i]);
    }

    lcd.drawFastHLine(0, 114, 240, 0x2104);
    lcd.setTextColor(CAT_COLORS[sel], TFT_BLACK);
    lcd.setCursor(4, 117);
    if (search_q[0]) {
        lcd.printf("Search: %-16.16s  /:clr", search_q);
    } else {
        lcd.print(CAT_SUBS[sel]);
    }
    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setCursor(4, 126);
    lcd.print("ENTER open  C chip  / search  U upd  W wifi");
    lcd.endWrite();
}

static void draw_fw_browser(CatId cat, int sel, int scroll) {
    lcd.startWrite();
    lcd.fillScreen(TFT_BLACK);
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(1);
    lcd.setTextColor(CAT_COLORS[cat], TFT_BLACK);
    lcd.setCursor(3, 3);
    lcd.printf("%s (%d)", CAT_NAMES[cat], view_count);
    lcd.setTextDatum(top_right);
    lcd.setTextColor(filter_new_only ? ui::GREEN : ui::ORANGE, TFT_BLACK);
    lcd.drawString(filter_new_only ? "[NEW]" : "[ALL]", 237, 3);
    lcd.setTextDatum(top_left);
    lcd.drawFastHLine(0, 13, 240, 0x2104);

    if (view_count == 0) {
        lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        lcd.setCursor(6, 40);
        lcd.print(total_entries == 0 ? "No catalog loaded." : "No matching firmwares.");
        lcd.setCursor(6, 54);
        lcd.print("Press U to update or C to show ALL.");
        lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
        lcd.setCursor(4, 125);
        lcd.print("U update  C chip  ESC categories");
        lcd.endWrite();
        return;
    }

    // Left pane: Firmware list (x = 0..108)
    lcd.drawFastVLine(109, 14, 105, 0x2104);
    static constexpr int VISIBLE = 9;
    File idx;
    bool use_sd = sd_ready() && SD.exists(INDEX_PATH);
    if (use_sd) idx = SD.open(INDEX_PATH, FILE_READ);

    HubEntry sel_entry{};
    for (int i = 0; i < VISIBLE && scroll + i < view_count; i++) {
        HubEntry e{};
        int raw = view_idx[scroll + i];
        if (use_sd && idx) {
            uint32_t off = sizeof(IndexHeader) + (uint32_t)raw * sizeof(HubEntry);
            idx.seek(off);
            idx.read((uint8_t *)&e, sizeof(e));
        } else if (ram_entries && raw < ram_count) {
            e = ram_entries[raw];
        }
        if (scroll + i == sel) sel_entry = e;

        int y = 16 + i * 11;
        bool active = (scroll + i == sel);
        if (active) {
            lcd.fillRect(0, y - 1, 108, 10, 0x0210);
            lcd.setTextColor(TFT_WHITE, 0x0210);
        } else {
            lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        }
        char line[19];
        snprintf(line, sizeof(line), "%c%-16.16s", active ? '>' : ' ', e.name);
        lcd.setCursor(2, y);
        lcd.print(line);
    }
    if (idx) idx.close();

    // Right pane: Live Preview Card & Description (x = 112..238)
    CatId ecat = kind_to_cat(sel_entry.kind);
    uint16_t ecol = CAT_COLORS[ecat];
    draw_cat_preview(ecat, 113, 16, 46, 36, ecol, true);

    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(1);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setCursor(163, 17);
    lcd.printf("%-12.12s", sel_entry.name);
    lcd.setTextColor(ecol, TFT_BLACK);
    lcd.setCursor(163, 27);
    lcd.printf("v%-11.11s", sel_entry.version);
    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setCursor(163, 37);
    lcd.printf("*%u %luK", (unsigned)sel_entry.stars, (unsigned long)(sel_entry.size / 1024));

    const char *chip_tag = sel_entry.chip == CHIP_OLD ? "OLD" : (sel_entry.chip == CHIP_NEW ? "NEW" : "ANY");
    uint16_t chip_col = sel_entry.chip == CHIP_OLD ? ui::RED : ui::GREEN;
    lcd.setTextColor(chip_col, TFT_BLACK);
    lcd.setCursor(163, 47);
    lcd.printf("[%s] %-7.7s", chip_tag, sel_entry.kind);

    lcd.setTextColor(ui::GREY, TFT_BLACK);
    lcd.setCursor(113, 56);
    lcd.printf("By %-17.17s", sel_entry.author);

    if (sel_entry.what[0]) {
        draw_wrapped(sel_entry.what, 113, 68, 125, 5, TFT_LIGHTGREY, TFT_BLACK);
    } else {
        lcd.setTextColor(ui::GREY, TFT_BLACK);
        lcd.setCursor(113, 72);
        lcd.print("Press ENTER for details");
    }

    lcd.drawFastHLine(0, 120, 240, 0x2104);
    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setCursor(3, 124);
    lcd.print(";/ . move   ENTER details   ESC back");
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
    CatId cat = kind_to_cat(e.kind);
    uint16_t col = CAT_COLORS[cat];
    int desc_scroll = 0;

    auto draw_detail = [&]() {
        lcd.startWrite();
        lcd.fillScreen(TFT_BLACK);
        // Left preview card (4..82)
        draw_cat_preview(cat, 4, 4, 78, 56, col, false);

        lcd.setFont(&fonts::Font0);
        lcd.setTextSize(1);
        lcd.setTextColor(e.chip == CHIP_OLD ? ui::RED : ui::GREEN, TFT_BLACK);
        lcd.setCursor(4, 65);
        lcd.print(e.chip == CHIP_OLD ? "OLD 0x856014" : (e.chip == CHIP_NEW ? "NEW 0xB36014" : "ANY CHIP OK"));
        lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        lcd.setCursor(4, 77);
        lcd.printf("Kind: %s", e.kind[0] ? e.kind : "other");
        lcd.setCursor(4, 89);
        lcd.printf("Size: %lu KB", (unsigned long)(e.size / 1024));
        lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
        lcd.setCursor(4, 101);
        lcd.printf("Stars: %u", (unsigned)e.stars);

        // Right header & full description (88..237)
        lcd.setTextColor(col, TFT_BLACK);
        lcd.setCursor(88, 4);
        lcd.printf("%.24s", e.name);
        lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        lcd.setCursor(88, 15);
        lcd.printf("v%-10.10s by %.11s", e.version, e.author);
        lcd.drawFastHLine(88, 26, 150, 0x2104);

        const char *desc = e.what[0] ? e.what : "Open-source FM-1 firmware from thefirmwarehub.com.";
        draw_wrapped(desc, 88, 31, 150, 8, TFT_LIGHTGREY, TFT_BLACK, desc_scroll);

        lcd.drawFastHLine(0, 117, 240, 0x2104);
        lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
        lcd.setCursor(4, 123);
        lcd.print("ENTER download & flash  ;/. scroll  ESC");
        lcd.endWrite();
    };

    draw_detail();
    ui::wait_released();
    for (;;) {
        int k = ui::read_key();
        if (!k) {
            delay(15);
            continue;
        }
        if (k == 27) return;
        if (k == ';' || k == 'k' || k == ',') {
            if (desc_scroll > 0) {
                desc_scroll--;
                draw_detail();
            }
        } else if (k == '.' || k == 'j' || k == '/') {
            desc_scroll++;
            draw_detail();
        } else if (k == '\n') {
            break;
        }
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

static void browse_category(CatId cat) {
    rebuild_view(cat);
    int sel = 0, scroll = 0;
    static constexpr int VISIBLE = 9;
    draw_fw_browser(cat, sel, scroll);
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
        if (k == 'c') {
            filter_new_only = !filter_new_only;
            rebuild_view(cat);
            sel = scroll = 0;
            draw_fw_browser(cat, sel, scroll);
        } else if (k == 'u') {
            if (fetch_catalog()) rebuild_view(cat);
            sel = scroll = 0;
            draw_fw_browser(cat, sel, scroll);
            ui::wait_released();
        } else if (k == '/' || k == 's') {
            prompt_text("Search Hub", "Name / Author / Desc (empty=all):", search_q, sizeof(search_q), false);
            rebuild_view(cat);
            sel = scroll = 0;
            draw_fw_browser(cat, sel, scroll);
            ui::wait_released();
        } else if (view_count > 0) {
            if (k == ';' || k == 'k' || k == ',') {
                if (sel > 0) sel--;
                if (sel < scroll) scroll = sel;
                draw_fw_browser(cat, sel, scroll);
            } else if (k == '.' || k == 'j') {
                if (sel + 1 < view_count) sel++;
                if (sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;
                draw_fw_browser(cat, sel, scroll);
            } else if (k == '\n') {
                static HubEntry e;
                if (read_entry(view_idx[sel], &e)) {
                    show_detail(e);
                }
                draw_fw_browser(cat, sel, scroll);
                ui::wait_released();
            }
        }
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
        rebuild_view(CAT_ALL);
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
    rebuild_view(CAT_ALL);

    // Always connect Wi-Fi / fetch catalog on first entry if no cache yet
    if (total_entries == 0) {
        if (fetch_catalog()) {
            rebuild_view(CAT_ALL);
        }
    }

    int cat_sel = 0;
    draw_category_menu(cat_sel);
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
        if (k == ',' || k == 'a' || k == 'h') {
            cat_sel = (cat_sel + CAT_COUNT - 1) % CAT_COUNT;
            draw_category_menu(cat_sel);
        } else if (k == '/' || k == 'd' || k == 'l') {
            cat_sel = (cat_sel + 1) % CAT_COUNT;
            draw_category_menu(cat_sel);
        } else if (k == ';' || k == 'w' || k == 'k') {
            cat_sel = (cat_sel + CAT_COUNT - 3) % CAT_COUNT;
            draw_category_menu(cat_sel);
        } else if (k == '.' || k == 's' || k == 'j') {
            cat_sel = (cat_sel + 3) % CAT_COUNT;
            draw_category_menu(cat_sel);
        } else if (k == 'c') {
            filter_new_only = !filter_new_only;
            rebuild_view((CatId)cat_sel);
            draw_category_menu(cat_sel);
        } else if (k == 'u') {
            if (fetch_catalog()) rebuild_view((CatId)cat_sel);
            draw_category_menu(cat_sel);
            ui::wait_released();
        } else if (k == 'w') {
            if (wifi_setup_ui() && fetch_catalog()) rebuild_view((CatId)cat_sel);
            draw_category_menu(cat_sel);
            ui::wait_released();
        } else if (k == '\n') {
            browse_category((CatId)cat_sel);
            rebuild_view((CatId)cat_sel);
            draw_category_menu(cat_sel);
            ui::wait_released();
        }
    }
}

}  // namespace hub
