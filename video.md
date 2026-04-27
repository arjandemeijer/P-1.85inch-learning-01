# MJPEG afspelen op 1.85" Waveshare display (ESP32-S3)

De blender MP4 omzetten naar .MJPEG
```
ffmpeg -i t1.mp4 -vf "scale=360:360,fps=24" -q:v 16 -an output.mjpeg
```

```

```

## Doel

Een MJPEG-videobestand frame voor frame decoderen en afspelen op het ronde 360×360 ST77916 display via de `bodmer/TJpg_Decoder` library.

---

## Hardware

- **Board:** Waveshare ESP32-S3 1.85" LCD
- **Display:** ST77916, 360×360, verbonden via QSPI (4-bit)
- **PSRAM:** 8MB OPI (octal), vereist voor framebuffer + MJPEG-buffer
- **I/O expander:** TCA9554 op I2C (adres 0x20) — beheert de LCD reset-pin

### Pinout

| Functie   | GPIO |
|-----------|------|
| I2C SDA   | 11   |
| I2C SCL   | 10   |
| LCD SCK   | 40   |
| LCD D0    | 46   |
| LCD D1    | 45   |
| LCD D2    | 42   |
| LCD D3    | 41   |
| LCD CS    | 21   |
| Backlight | 5    |

---

## Projectstructuur

```
P-1.85inch-learning-01/
├── src/
│   └── main.cpp
├── data/
│   └── output.mjpeg      ← MJPEG-bestand voor LittleFS
├── platformio.ini
└── video.md
```

> `output.mjpeg` stond oorspronkelijk in `src/` maar is verplaatst naar `data/` zodat PlatformIO het kan uploaden naar LittleFS.

---

## platformio.ini

```ini
[env:waveshare-s3-lcd-185]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
upload_speed = 921600

board_build.flash_mode = qio
board_build.flash_size = 16MB
board_build.partitions = default_16MB.csv
board_upload.flash_size = 16MB
board_build.arduino.memory_type = qio_opi   ; PSRAM als OPI — vergeet dit niet!
board_build.filesystem = littlefs           ; nieuw toegevoegd voor MJPEG

build_flags =
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DARDUINO_USB_MODE=1

lib_deps =
    moononournation/GFX Library for Arduino@^1.4.9
    Wire
    bodmer/TJpg_Decoder
```

### Belangrijke instellingen

- **`board_build.arduino.memory_type = qio_opi`** — zonder dit initialiseert PSRAM niet en mislukt `ps_malloc`/`ps_calloc` stilzwijgend.
- **`board_build.filesystem = littlefs`** — vertelt PlatformIO welk filesystem te bouwen voor de `data/` map.
- **`default_16MB.csv`** — reserveert ~10MB flash voor het filesystem, ruim genoeg voor een MJPEG-bestand.

---

## Hoe werkt de code

### Display aansturen

De ST77916 wordt **niet** via Arduino_GFX aangestuurd — de init-sequentie wordt handmatig verstuurd via de QSPI-bus:

```
CASET (0x2A) → kolombereik instellen
RASET (0x2B) → rijbereik instellen
RAMWR (0x2C) → alle pixels in één burst versturen
```

De framebuffer is `360 × 360 × 2 = 259.200 bytes` in PSRAM.

### Byte-volgorde (belangrijk!)

De ST77916 verwacht pixels **big-endian** (hoge byte eerst). De ESP32 is little-endian. Daarom:

- `TJpgDec.setSwapBytes(true)` — TJpg_Decoder swapt de bytes van elk RGB565-pixel zodat ze direct in de framebuffer passen.
- `writeBytes()` stuurt de bytes dan in de juiste volgorde naar het display.

### TJpg_Decoder callback

```cpp
bool jpg_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    // Wordt aangeroepen voor elk 16×16 blok dat de decoder heeft verwerkt.
    // Kopieert het blok rij voor rij naar de juiste positie in de framebuffer.
}
```

### MJPEG parser

Een MJPEG-bestand is een aaneenschakeling van gewone JPEG-bestanden. Elke JPEG:
- Begint met `FF D8` (SOI — Start Of Image)
- Eindigt met `FF D9` (EOI — End Of Image)

`next_frame()` zoekt naar deze markers en geeft een pointer + lengte terug. Na het laatste frame reset `mjpeg_pos` naar 0 voor naadloze herhaling.

### Geheugengebruik

| Buffer       | Grootte         | Locatie |
|--------------|-----------------|---------|
| Framebuffer  | ~259 KB         | PSRAM   |
| MJPEG-buffer | grootte van bestand | PSRAM   |

---

## Uploadvolgorde

```bash
# Stap 1: filesystem uploaden (eenmalig, of als output.mjpeg wijzigt)
~/.platformio/penv/bin/pio run -t uploadfs

# Stap 2: firmware uploaden
~/.platformio/penv/bin/pio run -t upload

# Seriële monitor
~/.platformio/penv/bin/pio device monitor
```

Of via de PlatformIO zijbalk in VS Code: *Project Tasks → uploadfs / upload*.

### `pio` beschikbaar maken in terminal

`pio` zit niet automatisch in je PATH. Voeg dit toe aan `~/.zshrc`:

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
```

---

## Verwachte seriële output bij opstarten

```
=== Boot ===
LCD reset done.
QSPI bus ready.
Sending 97 init commands...
Display init complete.
MJPEG: <grootte> bytes
MJPEG loaded into PSRAM.
Entering playback loop.
```

Als je `LittleFS mount failed` ziet: voer eerst `pio run -t uploadfs` uit.

---

## Aandachtspunten

- **Framerate** — geen expliciete timing. De snelheid wordt bepaald door JPEG-decodering + QSPI-push. Wil je een vaste framerate, voeg dan `delay()` of een tijdstempel toe in `loop()`.
- **Framegrootte** — `TJpgDec.setJpgScale(1)` gaat uit van 360×360 frames. Andere resoluties: gebruik scale `2` (180×180 output) of pas de callback aan.
- **FF D9 in JPEG-data** — de simpele EOI-zoeker werkt in de praktijk goed voor welgevormde MJPEG-bestanden. Voor maximale robuustheid zou je de JPEG-markerstructuur volledig moeten parsen.
