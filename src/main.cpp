#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <TJpg_Decoder.h>

// --- Pins ---
#define I2C_SDA 11
#define I2C_SCL 10
#define LCD_SCK 40
#define LCD_D0  46
#define LCD_D1  45
#define LCD_D2  42
#define LCD_D3  41
#define LCD_CS  21
#define LCD_BL   5

// Backlight brightness, 8-bit PWM duty cycle.
// Range: 0 = off, 255 = max brightness. Below ~10 is nearly invisible; above ~200 is very bright.
// Comfortable indoor range is roughly 40..120.
#define LCD_BL_BRIGHTNESS 10
#define LCD_BL_PWM_FREQ   20000   // Hz, well above audible + above visible flicker

#define TCA9554_ADDR       0x20
#define TCA9554_REG_OUTPUT 0x01
#define TCA9554_REG_CONFIG 0x03
#define EXIO_LCD_RST       2

static uint8_t tca_state = 0xFF;

bool tca_write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}
void tca_set_pin(uint8_t pin, bool high) {
    if (high) tca_state |=  (1 << pin);
    else      tca_state &= ~(1 << pin);
    tca_write_reg(TCA9554_REG_OUTPUT, tca_state);
}

// --- QSPI bus (we'll drive it directly for init, then use it with Arduino_GFX for pixels) ---
// Concrete QSPI type (not Arduino_DataBus*) so writeC8Bytes is visible.
Arduino_ESP32QSPI *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3
);

// --- Init table: command, data pointer, data length, delay-ms ---
struct InitCmd {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t len;
    uint16_t delay_ms;
};

// Helper for readability
#define D(...) (const uint8_t[]){__VA_ARGS__}

static const InitCmd st77916_init[] = {
    {0xF0, D(0x28), 1, 0}, {0xF2, D(0x28), 1, 0}, {0x73, D(0xF0), 1, 0},
    {0x7C, D(0xD1), 1, 0}, {0x83, D(0xE0), 1, 0}, {0x84, D(0x61), 1, 0},
    {0xF2, D(0x82), 1, 0}, {0xF0, D(0x00), 1, 0}, {0xF0, D(0x01), 1, 0},
    {0xF1, D(0x01), 1, 0}, {0xB0, D(0x56), 1, 0}, {0xB1, D(0x4D), 1, 0},
    {0xB2, D(0x24), 1, 0}, {0xB4, D(0x87), 1, 0}, {0xB5, D(0x44), 1, 0},
    {0xB6, D(0x8B), 1, 0}, {0xB7, D(0x40), 1, 0}, {0xB8, D(0x86), 1, 0},
    {0xBA, D(0x00), 1, 0}, {0xBB, D(0x08), 1, 0}, {0xBC, D(0x08), 1, 0},
    {0xBD, D(0x00), 1, 0}, {0xC0, D(0x80), 1, 0}, {0xC1, D(0x10), 1, 0},
    {0xC2, D(0x37), 1, 0}, {0xC3, D(0x80), 1, 0}, {0xC4, D(0x10), 1, 0},
    {0xC5, D(0x37), 1, 0}, {0xC6, D(0xA9), 1, 0}, {0xC7, D(0x41), 1, 0},
    {0xC8, D(0x01), 1, 0}, {0xC9, D(0xA9), 1, 0}, {0xCA, D(0x41), 1, 0},
    {0xCB, D(0x01), 1, 0}, {0xD0, D(0x91), 1, 0}, {0xD1, D(0x68), 1, 0},
    {0xD2, D(0x68), 1, 0}, {0xF5, D(0x00, 0xA5), 2, 0}, {0xDD, D(0x4F), 1, 0},
    {0xDE, D(0x4F), 1, 0}, {0xF1, D(0x10), 1, 0}, {0xF0, D(0x00), 1, 0},
    {0xF0, D(0x02), 1, 0},
    {0xE0, D(0xF0,0x0A,0x10,0x09,0x09,0x36,0x35,0x33,0x4A,0x29,0x15,0x15,0x2E,0x34), 14, 0},
    {0xE1, D(0xF0,0x0A,0x0F,0x08,0x08,0x05,0x34,0x33,0x4A,0x39,0x15,0x15,0x2D,0x33), 14, 0},
    {0xF0, D(0x10), 1, 0}, {0xF3, D(0x10), 1, 0}, {0xE0, D(0x07), 1, 0},
    {0xE1, D(0x00), 1, 0}, {0xE2, D(0x00), 1, 0}, {0xE3, D(0x00), 1, 0},
    {0xE4, D(0xE0), 1, 0}, {0xE5, D(0x06), 1, 0}, {0xE6, D(0x21), 1, 0},
    {0xE7, D(0x01), 1, 0}, {0xE8, D(0x05), 1, 0}, {0xE9, D(0x02), 1, 0},
    {0xEA, D(0xDA), 1, 0}, {0xEB, D(0x00), 1, 0}, {0xEC, D(0x00), 1, 0},
    {0xED, D(0x0F), 1, 0}, {0xEE, D(0x00), 1, 0}, {0xEF, D(0x00), 1, 0},
    {0xF8, D(0x00), 1, 0}, {0xF9, D(0x00), 1, 0}, {0xFA, D(0x00), 1, 0},
    {0xFB, D(0x00), 1, 0}, {0xFC, D(0x00), 1, 0}, {0xFD, D(0x00), 1, 0},
    {0xFE, D(0x00), 1, 0}, {0xFF, D(0x00), 1, 0}, {0x60, D(0x40), 1, 0},
    {0x61, D(0x04), 1, 0}, {0x62, D(0x00), 1, 0}, {0x63, D(0x42), 1, 0},
    {0x64, D(0xD9), 1, 0}, {0x65, D(0x00), 1, 0}, {0x66, D(0x00), 1, 0},
    {0x67, D(0x00), 1, 0}, {0x68, D(0x00), 1, 0}, {0x69, D(0x00), 1, 0},
    {0x6A, D(0x00), 1, 0}, {0x6B, D(0x00), 1, 0}, {0x70, D(0x40), 1, 0},
    {0x71, D(0x03), 1, 0}, {0x72, D(0x00), 1, 0}, {0x73, D(0x42), 1, 0},
    {0x74, D(0xD8), 1, 0}, {0x75, D(0x00), 1, 0}, {0x76, D(0x00), 1, 0},
    {0x77, D(0x00), 1, 0}, {0x78, D(0x00), 1, 0}, {0x79, D(0x00), 1, 0},
    {0x7A, D(0x00), 1, 0}, {0x7B, D(0x00), 1, 0}, {0x80, D(0x48), 1, 0},
    {0x81, D(0x00), 1, 0}, {0x82, D(0x06), 1, 0}, {0x83, D(0x02), 1, 0},
    {0x84, D(0xD6), 1, 0}, {0x85, D(0x04), 1, 0}, {0x86, D(0x00), 1, 0},
    {0x87, D(0x00), 1, 0}, {0x88, D(0x48), 1, 0}, {0x89, D(0x00), 1, 0},
    {0x8A, D(0x08), 1, 0}, {0x8B, D(0x02), 1, 0}, {0x8C, D(0xD8), 1, 0},
    {0x8D, D(0x04), 1, 0}, {0x8E, D(0x00), 1, 0}, {0x8F, D(0x00), 1, 0},
    {0x90, D(0x48), 1, 0}, {0x91, D(0x00), 1, 0}, {0x92, D(0x0A), 1, 0},
    {0x93, D(0x02), 1, 0}, {0x94, D(0xDA), 1, 0}, {0x95, D(0x04), 1, 0},
    {0x96, D(0x00), 1, 0}, {0x97, D(0x00), 1, 0}, {0x98, D(0x48), 1, 0},
    {0x99, D(0x00), 1, 0}, {0x9A, D(0x0C), 1, 0}, {0x9B, D(0x02), 1, 0},
    {0x9C, D(0xDC), 1, 0}, {0x9D, D(0x04), 1, 0}, {0x9E, D(0x00), 1, 0},
    {0x9F, D(0x00), 1, 0}, {0xA0, D(0x48), 1, 0}, {0xA1, D(0x00), 1, 0},
    {0xA2, D(0x05), 1, 0}, {0xA3, D(0x02), 1, 0}, {0xA4, D(0xD5), 1, 0},
    {0xA5, D(0x04), 1, 0}, {0xA6, D(0x00), 1, 0}, {0xA7, D(0x00), 1, 0},
    {0xA8, D(0x48), 1, 0}, {0xA9, D(0x00), 1, 0}, {0xAA, D(0x07), 1, 0},
    {0xAB, D(0x02), 1, 0}, {0xAC, D(0xD7), 1, 0}, {0xAD, D(0x04), 1, 0},
    {0xAE, D(0x00), 1, 0}, {0xAF, D(0x00), 1, 0}, {0xB0, D(0x48), 1, 0},
    {0xB1, D(0x00), 1, 0}, {0xB2, D(0x09), 1, 0}, {0xB3, D(0x02), 1, 0},
    {0xB4, D(0xD9), 1, 0}, {0xB5, D(0x04), 1, 0}, {0xB6, D(0x00), 1, 0},
    {0xB7, D(0x00), 1, 0}, {0xB8, D(0x48), 1, 0}, {0xB9, D(0x00), 1, 0},
    {0xBA, D(0x0B), 1, 0}, {0xBB, D(0x02), 1, 0}, {0xBC, D(0xDB), 1, 0},
    {0xBD, D(0x04), 1, 0}, {0xBE, D(0x00), 1, 0}, {0xBF, D(0x00), 1, 0},
    {0xC0, D(0x10), 1, 0}, {0xC1, D(0x47), 1, 0}, {0xC2, D(0x56), 1, 0},
    {0xC3, D(0x65), 1, 0}, {0xC4, D(0x74), 1, 0}, {0xC5, D(0x88), 1, 0},
    {0xC6, D(0x99), 1, 0}, {0xC7, D(0x01), 1, 0}, {0xC8, D(0xBB), 1, 0},
    {0xC9, D(0xAA), 1, 0}, {0xD0, D(0x10), 1, 0}, {0xD1, D(0x47), 1, 0},
    {0xD2, D(0x56), 1, 0}, {0xD3, D(0x65), 1, 0}, {0xD4, D(0x74), 1, 0},
    {0xD5, D(0x88), 1, 0}, {0xD6, D(0x99), 1, 0}, {0xD7, D(0x01), 1, 0},
    {0xD8, D(0xBB), 1, 0}, {0xD9, D(0xAA), 1, 0}, {0xF3, D(0x01), 1, 0},
    {0xF0, D(0x00), 1, 0},
    {0x3A, D(0x55), 1, 0},   // COLMOD: 16-bit RGB565
    {0x21, NULL, 0, 0},       // display inversion ON (fixes cyan colors)
    {0x11, NULL, 0, 120},     // sleep OUT, wait 120ms
    {0x29, NULL, 0, 0},       // display ON
};
#define INIT_CMD_COUNT (sizeof(st77916_init) / sizeof(st77916_init[0]))

// --- Framebuffer ---
static const int W = 360, H = 360;
uint16_t *framebuffer = nullptr;

// TJpg_Decoder callback: copy each decoded 16x16 block into the framebuffer.
// setSwapBytes(true) ensures pixels are already byte-swapped (big-endian) for
// direct framebuffer use, matching how the ST77916 expects the data.
bool jpg_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    if (y >= H) return false;
    for (uint16_t row = 0; row < h; row++) {
        int16_t fy = y + row;
        if (fy >= H) break;
        uint16_t cols = min((int)(W - x), (int)w);
        if (cols <= 0) continue;
        memcpy(framebuffer + fy * W + x, bitmap + row * w, cols * 2);
    }
    return true;
}

// --- MJPEG state ---
static uint8_t *mjpeg_buf  = nullptr;
static size_t   mjpeg_size = 0;
static size_t   mjpeg_pos  = 0;

// Find the next JPEG frame (SOI=FFD8 … EOI=FFD9) in the MJPEG buffer.
// Advances mjpeg_pos past the found frame. Resets to 0 when EOF is reached
// so playback loops continuously.
bool next_frame(const uint8_t **out, size_t *len) {
    while (mjpeg_pos + 1 < mjpeg_size) {
        if (mjpeg_buf[mjpeg_pos] == 0xFF && mjpeg_buf[mjpeg_pos + 1] == 0xD8) {
            size_t start = mjpeg_pos;
            size_t j = start + 2;
            while (j + 1 < mjpeg_size) {
                if (mjpeg_buf[j] == 0xFF && mjpeg_buf[j + 1] == 0xD9) {
                    *out     = mjpeg_buf + start;
                    *len     = j + 2 - start;
                    mjpeg_pos = j + 2;
                    return true;
                }
                j++;
            }
            // Incomplete frame at end of buffer — stop and loop
            break;
        }
        mjpeg_pos++;
    }
    mjpeg_pos = 0;  // restart for looping
    return false;
}

// Send one init command via the QSPI bus.
// NB: bus->write(byte) is RAMWC (pixel-data continue), NOT a parameter write.
// Use writeC8Bytes so cmd+params go in ONE QSPI transaction (cmd=0x02, addr=cmd<<8, data on tx_buffer).
void sendInitCmd(uint8_t cmd, const uint8_t *data, uint8_t len) {
    bus->beginWrite();
    if (len == 0) {
        bus->writeCommand(cmd);
    } else {
        bus->writeC8Bytes(cmd, (uint8_t*)data, len);
    }
    bus->endWrite();
}

// CASET + RASET + RAMWR: this is your Frame 1 / Frame 2 / Frame 3 by hand!
void setAddrWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    uint8_t caset_data[4] = { (uint8_t)(x1>>8), (uint8_t)(x1&0xFF),
                              (uint8_t)(x2>>8), (uint8_t)(x2&0xFF) };
    uint8_t raset_data[4] = { (uint8_t)(y1>>8), (uint8_t)(y1&0xFF),
                              (uint8_t)(y2>>8), (uint8_t)(y2&0xFF) };
    sendInitCmd(0x2A, caset_data, 4);  // Frame 1: CASET
    sendInitCmd(0x2B, raset_data, 4);  // Frame 2: RASET
}

void pushFramebuffer() {
    setAddrWindow(0, 0, W-1, H-1);
    // Frame 3: RAMWR + all pixels. Arduino_GFX helper handles large QSPI burst.
    bus->beginWrite();
    bus->writeCommand(0x2C);
    bus->writeBytes((uint8_t*)framebuffer, W * H * 2);
    bus->endWrite();
}

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println("\n=== Boot ===");

    ledcAttach(LCD_BL, LCD_BL_PWM_FREQ, 8);
    ledcWrite(LCD_BL, LCD_BL_BRIGHTNESS);

    Wire.begin(I2C_SDA, I2C_SCL);
    if (!tca_write_reg(TCA9554_REG_CONFIG, 0x00)) {
        Serial.println("TCA9554 fail"); while (1) delay(1000);
    }
    tca_set_pin(EXIO_LCD_RST, true);  delay(10);
    tca_set_pin(EXIO_LCD_RST, false); delay(20);
    tca_set_pin(EXIO_LCD_RST, true);  delay(120);
    Serial.println("LCD reset done.");

    bus->begin();
    Serial.println("QSPI bus ready.");

    Serial.printf("Sending %d init commands...\n", (int)INIT_CMD_COUNT);
    for (size_t i = 0; i < INIT_CMD_COUNT; i++) {
        sendInitCmd(st77916_init[i].cmd, st77916_init[i].data, st77916_init[i].len);
        if (st77916_init[i].delay_ms) delay(st77916_init[i].delay_ms);
    }
    Serial.println("Display init complete.");

    framebuffer = (uint16_t*) ps_calloc(W * H, sizeof(uint16_t));
    if (!framebuffer) { Serial.println("FB alloc fail"); while (1) delay(1000); }

    // Mount LittleFS — upload filesystem first with: pio run -t uploadfs
    if (!LittleFS.begin(false)) {
        Serial.println("LittleFS mount failed — run: pio run -t uploadfs");
        while (1) delay(1000);
    }

    File f = LittleFS.open("/output.mjpeg", "r");
    if (!f) { Serial.println("Cannot open /output.mjpeg"); while (1) delay(1000); }
    mjpeg_size = f.size();
    Serial.printf("MJPEG: %u bytes\n", mjpeg_size);

    mjpeg_buf = (uint8_t*) ps_malloc(mjpeg_size);
    if (!mjpeg_buf) { Serial.println("MJPEG buf alloc fail"); while (1) delay(1000); }
    f.read(mjpeg_buf, mjpeg_size);
    f.close();
    Serial.println("MJPEG loaded into PSRAM.");

    TJpgDec.setSwapBytes(true);   // output byte-swapped RGB565 to match our framebuffer
    TJpgDec.setCallback(jpg_output);
    TJpgDec.setJpgScale(1);
    Serial.println("Entering playback loop.");
}

void loop() {
    const uint8_t *frame;
    size_t frame_len;
    if (next_frame(&frame, &frame_len)) {
        TJpgDec.drawJpg(0, 0, frame, frame_len);
        pushFramebuffer();
    }
    // next_frame() resets mjpeg_pos to 0 at EOF → seamless loop
}