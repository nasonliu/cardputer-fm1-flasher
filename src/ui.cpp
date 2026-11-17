#include "ui.h"

#include <algorithm>
#include <memory>
#include <stdarg.h>
#include <utility/Adafruit_TCA8418/Adafruit_TCA8418.h>
#include <utility/Keyboard/KeyboardReader/KeyboardReader.h>

#include "art.h"
#include "hub.h"
#include "sdcard.h"
#include "ui_fonts.h"

HWCDC dbg_serial;

static constexpr gpio_num_t PIN_G0 = GPIO_NUM_0;  // G0 button, active low
static constexpr gpio_num_t PIN_TCA_INT = GPIO_NUM_11;

// Polled TCA8418 keyboard reader for Cardputer ADV:
// - Never deadlocks if GPIO11 misses a falling edge
// - Auto-initializes if power switch is turned ON after USB boot
class AdvPolledKeyboardReader : public KeyboardReader {
public:
    void begin() override {
        M5.In_I2C.begin(I2C_NUM_1, GPIO_NUM_8, GPIO_NUM_9);
        pinMode(PIN_TCA_INT, INPUT_PULLUP);
        _try_init();
    }

    void update() override {
        uint32_t now = millis();
        if (!_ready) {
            if (now - _last_try >= 400) {
                _last_try = now;
                _try_init();
            }
            return;
        }
        // Poll whenever INT is low, we have pending events, or every 12 ms
        if (!_has_more && gpio_get_level(PIN_TCA_INT) != 0 && (now - _last_poll < 12)) return;
        _last_poll = now;

        // Read at most 1 event per update() so press & release are always seen
        // as distinct states by Keyboard_Class::isChange().
        uint8_t ev = _tca.getEvent();
        if (ev == 0) {
            _has_more = false;
            _tca.writeRegister8(TCA8418_REG_INT_STAT, 3);
            return;
        }
        _has_more = true;
        bool pressed = (ev & 0x80) != 0;
        uint8_t code = (ev & 0x7F);
        if (code >= 1 && code <= 80) {
            code--;
            uint8_t r = code / 10;
            uint8_t c = code % 10;
            uint8_t col = r * 2 + (c > 3 ? 1 : 0);
            uint8_t row = (c + 4) % 4;
            Point2D_t pt{col, row};
            auto it = std::find(_key_list.begin(), _key_list.end(), pt);
            if (pressed) {
                if (it == _key_list.end()) _key_list.push_back(pt);
            } else {
                if (it != _key_list.end()) _key_list.erase(it);
            }
        }
        _tca.writeRegister8(TCA8418_REG_INT_STAT, 3);
    }

private:
    Adafruit_TCA8418 _tca;
    bool _ready = false;
    bool _has_more = false;
    uint32_t _last_try = 0;
    uint32_t _last_poll = 0;

    void _try_init() {
        if (!M5.In_I2C.scanID(TCA8418_DEFAULT_ADDR)) return;
        if (!_tca.begin()) return;
        _tca.matrix(7, 8);
        _tca.flush();
        _tca.enableDebounce();
        _tca.enableInterrupts();
        _ready = true;
    }
};

// ---------------------------------------------------------------------------
// Log and console

static constexpr int LOG_LINES = 9;
static char log_buf[LOG_LINES][41];
static int log_n;
static const char *con_title = "";
static uint16_t con_color = TFT_WHITE;
static char con_status[41];
static bool con_visible;  // the console is what the screen shows

static void draw_console() {
    lcd.startWrite();
    lcd.fillScreen(TFT_BLACK);
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(2);
    lcd.setTextColor(con_color, TFT_BLACK);
    lcd.setCursor(4, 2);
    lcd.print(con_title);
    lcd.setTextSize(1);
    lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    int first = log_n > LOG_LINES ? log_n - LOG_LINES : 0;
    for (int i = first; i < log_n; i++) {
        lcd.setCursor(2, 22 + (i - first) * 10);
        lcd.print(log_buf[i % LOG_LINES]);
    }
    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setCursor(2, 124);
    lcd.print(con_status);
    lcd.endWrite();
}

void ulog(const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    dbg_serial.println(line);
    if (sd_ok) {
        File f = SD.open("/fm1/log.txt", FILE_APPEND);
        if (f) {
            f.printf("%9lu %s\n", (unsigned long)millis(), line);
            f.close();
        }
    }
    for (size_t off = 0, n = strlen(line); off < n || off == 0; off += 40) {
        snprintf(log_buf[log_n % LOG_LINES], 41, "%s", line + off);
        log_n++;
        if (n <= off + 40) break;
    }
    if (con_visible) draw_console();
}

namespace ui {

void console(const char *title, uint16_t color) {
    con_title = title;
    con_color = color;
    con_visible = true;
    draw_console();
}

void status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(con_status, sizeof con_status, fmt, ap);
    va_end(ap);
    if (con_visible) draw_console();
}

void clear_log() { log_n = 0; }

void show_console() {
    con_title = "Details";
    con_color = TFT_LIGHTGREY;
    snprintf(con_status, sizeof con_status, "Any key: back");
    con_visible = true;
    draw_console();
}

// ---------------------------------------------------------------------------
// Keys

void init_keyboard() {
    // Detach M5Cardputer's default GPIO11 ISR before destroying TCA8418KeyboardReader!
    detachInterrupt(digitalPinToInterrupt(PIN_TCA_INT));
    M5Cardputer.Keyboard.begin(std::make_unique<AdvPolledKeyboardReader>());
}

bool g0_pressed() { return gpio_get_level(PIN_G0) == 0; }

static bool g0_was = false;

int read_key() {
    bool g0 = g0_pressed();
    bool g0_edge = g0 && !g0_was;
    g0_was = g0;
    if (g0_edge) return '\n';
    M5Cardputer.update();
    auto &kb = M5Cardputer.Keyboard;
    if (!kb.isChange() || !kb.isPressed()) return 0;
    auto st = kb.keysState();
    if (st.enter) return '\n';
    if (st.del) return 27;
    if (!st.word.empty()) {
        char c = st.word[0];
        if (c == '`') return 27;
        return tolower(c);
    }
    return 0;
}

void wait_released() {
    do {
        delay(20);
        M5Cardputer.update();
    } while (g0_pressed() || M5Cardputer.Keyboard.isPressed());
    g0_was = false;
    M5Cardputer.Keyboard.isChange();  // Sync _last_key_size to 0
    delay(25);
    M5Cardputer.update();
    M5Cardputer.Keyboard.isChange();
}

bool any_key_cancel() {
    if (g0_pressed()) return true;
    M5Cardputer.update();
    return M5Cardputer.Keyboard.isPressed() > 0;
}

// ---------------------------------------------------------------------------
// Guided pages
//
//  +------------+-----------------------------+
//  |            | Title (bold, accent)        |  240 x 135
//  |  picture   | body text, word-wrapped     |
//  |  100x100   | [progress bar]              |
//  |            |                             |
//  +------------+-----------------------------+
//  | [KEY] what it does                       |  footer
//  +------------------------------------------+

static constexpr int ART_X = 2, ART_Y = 6, ART_W = 100, ART_H = 100;
static constexpr int TEXT_X = 106, TEXT_W = 240 - 106 - 2, TEXT_Y = 4;
static constexpr int FOOT_Y = 113, FOOT_H = 135 - 113;
// the pages' fonts (src/ui_fonts.h: DejaVu Sans, permissive) and line pitches
static const lgfx::IFont *const TITLE = &UiTitle, *const BODY = &UiBody;
static constexpr int TITLE_LINE = 18, LINE = 16;
static int text_end_y;           // where the page's words ended
static uint16_t page_accent;

// Draws s word-wrapped in [x, x + w) from top y, `pitch` apart; returns the next y.
static int wrap(const char *s, int x, int y, int w, int pitch = LINE) {
    char line[64], word[40];
    int ll = 0;
    line[0] = 0;
    auto flush = [&]() {
        if (ll) {
            lcd.drawString(line, x, y);
            y += pitch;
        }
        ll = 0;
        line[0] = 0;
    };
    while (*s) {
        if (*s == '\n') {
            flush();
            s++;
            continue;
        }
        int wl = 0;
        while (*s && *s != ' ' && *s != '\n' && wl < (int)sizeof word - 1) word[wl++] = *s++;
        word[wl] = 0;
        while (*s == ' ') s++;
        char trial[64];
        snprintf(trial, sizeof trial, ll ? "%s %s" : "%s%s", line, word);
        if (ll && lcd.textWidth(trial) > w) {
            flush();
            snprintf(trial, sizeof trial, "%s", word);
        }
        snprintf(line, sizeof line, "%s", trial);
        ll = strlen(line);
    }
    flush();
    return y;
}

void footer(uint16_t color, const char *text) {
    lcd.fillRect(0, FOOT_Y, 240, FOOT_H, TFT_BLACK);
    lcd.drawFastHLine(0, FOOT_Y, 240, 0x2104);
    lcd.setFont(BODY);
    lcd.setTextColor(color, TFT_BLACK);
    lcd.setTextDatum(middle_left);
    lcd.drawString(text, 6, FOOT_Y + FOOT_H / 2 + 1);
    lcd.setTextDatum(top_left);
}

static void key_footer(const char *key, const char *hint, const char *right) {
    lcd.fillRect(0, FOOT_Y, 240, FOOT_H, TFT_BLACK);
    lcd.drawFastHLine(0, FOOT_Y, 240, 0x2104);
    lcd.setFont(TITLE);
    int kw = lcd.textWidth(key) + 12;
    if (kw < 22) kw = 22;
    // the key cap
    lcd.fillRoundRect(4, FOOT_Y + 3, kw, FOOT_H - 5, 4, page_accent);
    lcd.setTextColor(TFT_BLACK, page_accent);
    lcd.setTextDatum(middle_center);
    lcd.drawString(key, 4 + kw / 2, FOOT_Y + FOOT_H / 2 + 1);
    if (hint) {
        lcd.setFont(BODY);
        lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        lcd.setTextDatum(middle_left);
        lcd.drawString(hint, 4 + kw + 6, FOOT_Y + FOOT_H / 2 + 1);
    }
    if (right) {
        lcd.setFont(BODY);
        lcd.setTextColor(GREY, TFT_BLACK);
        lcd.setTextDatum(middle_right);
        lcd.drawString(right, 236, FOOT_Y + FOOT_H / 2 + 1);
    }
    lcd.setTextDatum(top_left);
}

void page(const char *art_name, uint16_t accent, const char *title, const char *body, const char *key,
          const char *hint, const char *right) {
    con_visible = false;
    page_accent = accent;
    lcd.startWrite();
    lcd.fillScreen(TFT_BLACK);
    art::draw(art_name, ART_X, ART_Y, ART_W, ART_H, accent);
    lcd.setTextDatum(top_left);
    lcd.setFont(TITLE);
    lcd.setTextColor(accent, TFT_BLACK);
    int y = wrap(title, TEXT_X, TEXT_Y, TEXT_W, TITLE_LINE);
    lcd.setFont(BODY);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    text_end_y = wrap(body ? body : "", TEXT_X, y + 2, TEXT_W);
    if (key) key_footer(key, hint, right);
    else if (hint) footer(TFT_LIGHTGREY, hint);
    lcd.endWrite();
}

void note(uint16_t color, const char *text) {
    int y = text_end_y + 2;
    lcd.startWrite();
    lcd.fillRect(TEXT_X, y, TEXT_W, LINE + 2, TFT_BLACK);
    lcd.setFont(TITLE);
    lcd.setTextColor(color, TFT_BLACK);
    lcd.drawString(text, TEXT_X, y);
    lcd.endWrite();
}

void progress(uint32_t done, uint32_t total) {
    int y = text_end_y + 4, w = TEXT_W, h = 12;
    if (y + h + 16 > FOOT_Y) y = FOOT_Y - h - 18;
    uint32_t pct = total ? done * 100 / total : 0;
    if (pct > 100) pct = 100;
    lcd.startWrite();
    lcd.drawRoundRect(TEXT_X, y, w, h, 3, TFT_DARKGREY);
    lcd.fillRoundRect(TEXT_X + 2, y + 2, (w - 4) * pct / 100, h - 4, 2, page_accent);
    lcd.fillRect(TEXT_X, y + h + 2, w, 16, TFT_BLACK);
    lcd.setFont(BODY);
    lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    char s[8];
    snprintf(s, sizeof s, "%lu%%", (unsigned long)pct);
    lcd.drawString(s, TEXT_X, y + h + 2);
    lcd.endWrite();
}

void spinner() {
    static uint32_t last;
    static int phase;
    if (millis() - last < 250) return;
    last = millis();
    phase = (phase + 1) % 4;
    int y = text_end_y + 10;
    if (y > FOOT_Y - 8) y = FOOT_Y - 8;
    lcd.startWrite();
    for (int i = 0; i < 3; i++)
        lcd.fillCircle(TEXT_X + 6 + i * 14, y, 4, i < phase ? page_accent : 0x2104);
    lcd.endWrite();
}

static void draw_menu_icon(int idx, int cx, int cy, uint16_t col) {
    if (idx == 0) {
        // Wi-Fi Hub: signal arcs + dot + down arrow
        lcd.fillCircle(cx, cy + 10, 3, col);
        lcd.drawArc(cx, cy + 10, 9, 7, 225, 315, col);
        lcd.drawArc(cx, cy + 10, 15, 13, 225, 315, col);
        lcd.drawArc(cx, cy + 10, 21, 19, 225, 315, col);
    } else if (idx == 1) {
        // SD Flash: MicroSD card outline + pins + bolt
        lcd.drawRoundRect(cx - 12, cy - 14, 24, 28, 3, col);
        lcd.fillRect(cx - 8, cy - 10, 3, 6, col);
        lcd.fillRect(cx - 3, cy - 10, 3, 6, col);
        lcd.fillRect(cx + 2, cy - 10, 3, 6, col);
        lcd.fillRect(cx + 7, cy - 10, 3, 6, col);
        lcd.fillTriangle(cx + 2, cy - 1, cx - 5, cy + 6, cx + 1, cy + 6, col);
        lcd.fillTriangle(cx - 2, cy + 12, cx + 5, cy + 5, cx - 1, cy + 5, col);
    } else if (idx == 2) {
        // Rescue: First-aid cross inside rounded box
        lcd.drawRoundRect(cx - 14, cy - 14, 28, 28, 5, col);
        lcd.fillRect(cx - 3, cy - 9, 6, 18, col);
        lcd.fillRect(cx - 9, cy - 3, 18, 6, col);
    } else {
        // Advanced: Terminal ">_" inside frame
        lcd.drawRoundRect(cx - 15, cy - 13, 30, 26, 4, col);
        lcd.drawWideLine(cx - 9, cy - 5, cx - 3, cy, 2, col);
        lcd.drawWideLine(cx - 3, cy, cx - 9, cy + 5, 2, col);
        lcd.fillRect(cx + 1, cy + 4, 8, 2, col);
    }
}

int home_menu(int initial_sel) {
    static const char *const LABELS[4] = {"Wi-Fi Hub", "SD Flash", "Rescue", "Advanced"};
    static const char *const DESCS[4] = {
        "Download firmwares from thefirmwarehub.com",
        "Flash .fwsc / .bin from SD card to FM-1",
        "Unbrick dead FM-1 / install m-bootloader",
        "Console, backup, MIDI soft-key & self-tests",
    };
    static const uint16_t COLORS[4] = {CYAN, GREEN, ORANGE, YELLOW};

    int sel = (initial_sel >= 0 && initial_sel < 4) ? initial_sel : 0;
    bool has_sd = sd_ready();
    uint32_t last_sd = millis();

    auto draw = [&]() {
        con_visible = false;
        lcd.startWrite();
        lcd.fillScreen(TFT_BLACK);

        // Top bar
        lcd.setFont(&fonts::Font0);
        lcd.setTextSize(1);
        lcd.setTextDatum(top_left);
        lcd.setTextColor(CYAN, TFT_BLACK);
        lcd.drawString("FM-1 Flasher & Hub", 6, 5);
        lcd.setTextDatum(top_right);
        lcd.setTextColor(has_sd ? GREEN : GREY, TFT_BLACK);
        lcd.drawString(has_sd ? "[SD OK]" : "[NO SD]", 234, 5);
        lcd.drawFastHLine(0, 17, 240, 0x2104);

        // 4 Icon Cards
        for (int i = 0; i < 4; i++) {
            int x = 4 + i * 58;
            int y = 21, w = 54, h = 72;
            bool active = (i == sel);
            uint16_t ac = COLORS[i];
            if (active) {
                lcd.fillRoundRect(x, y, w, h, 6, 0x1082);
                lcd.drawRoundRect(x, y, w, h, 6, ac);
                lcd.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 5, ac);
            } else {
                lcd.drawRoundRect(x, y, w, h, 6, 0x3186);
            }
            draw_menu_icon(i, x + w / 2, y + 26, active ? ac : GREY);
            lcd.setFont(&fonts::Font0);
            lcd.setTextSize(1);
            lcd.setTextDatum(top_center);
            lcd.setTextColor(active ? TFT_WHITE : GREY, active ? 0x1082 : TFT_BLACK);
            lcd.drawString(LABELS[i], x + w / 2, y + 57);
        }

        // Description & Footer
        lcd.setTextDatum(top_center);
        lcd.setTextColor( COLORS[sel], TFT_BLACK);
        lcd.drawString(DESCS[sel], 120, 99);

        lcd.drawFastHLine(0, 114, 240, 0x2104);
        lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        lcd.drawString("< , / ; . > Select    [ENTER] Open", 120, 121);
        lcd.setTextDatum(top_left);
        lcd.endWrite();
    };

    draw();
    wait_released();
    for (;;) {
        if (dbg_serial.available()) {
            int ch = dbg_serial.read();
            if (ch == 'T') {
                hub::debug_test();
                draw();
            }
        }
        if (!has_sd && millis() - last_sd > 2500) {
            last_sd = millis();
            if (sd_ready()) {
                has_sd = true;
                draw();
            }
        }
        int k = read_key();
        if (!k) {
            delay(15);
            continue;
        }
        if (k == ',' || k == ';' || k == 'a' || k == 'w' || k == 'h' || k == 'k') {
            sel = (sel + 3) % 4;
            draw();
        } else if (k == '/' || k == '.' || k == 'd' || k == 's' || k == 'l' || k == 'j' || k == ' ') {
            sel = (sel + 1) % 4;
            draw();
        } else if (k == '\n') {
            wait_released();
            return sel;
        } else if (k >= '1' && k <= '4') {
            wait_released();
            return k - '1';
        }
    }
}

}  // namespace ui
