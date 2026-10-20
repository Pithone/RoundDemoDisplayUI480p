# Round Demo Display UI

PlatformIO firmware for the Waveshare ESP32-S3-Touch-LCD-2.8C (480 × 480
ST7701 RGB panel, GT911 touch controller, TCA9554 expander, and microSD slot).

## Hardware configuration

Board-specific pins and timings are centralized in [src/board_config.h](src/board_config.h).
The current baseline is:

- RGB panel: 16-bit RGB565, 14 MHz pixel clock, data pins
  `5,45,48,47,21,14,13,12,11,10,9,46,3,8,18,17`; sync `38/39/40/41`.
- ST7701 command interface: MOSI `1`, SCLK `2`, LCD chip-select and reset via
  TCA9554 `0x20`. The known-good register values `0x3A=0x66` and `0x36=0x00`
  are retained.
- GT911: I2C SDA `15`, SCL `7`, INT `16`, address `0x5D`; reset via TCA9554.
- SD_MMC: one-bit mode on CLK `2`, CMD `1`, and D0 `42`; TCA9554 EXIO pin 4
  (zero-based bit 3) enables the SD D3 line.
- Buzzer: TCA9554 P7/EXIO8, active high. Backlight PWM: GPIO `6`.

The SD card uses the board's one-bit SD_MMC interface. The firmware only reads
from the card. If the board revision's schematic differs, update the
corresponding values in `board_config.h` before connecting power.

The target module is specified as 8 MB flash and 8 MB OPI PSRAM. PlatformIO
uses its ESP32-S3-DevKitC-1-N8R8 profile so the selected board metadata
explicitly enables 8 MB OPI PSRAM and 8 MB QIO flash; the project also selects
the included 8 MB partition table. This board profile is used for its matching
memory configuration, not because the Waveshare board is physically a
DevKitC. Do not change the memory mode without checking the module fitted to
the board.

LVGL's 5 MB memory pool is allocated from PSRAM to give the PNG decoder room
for full-resolution decode buffers. A 1 MB image cache retains one decoded
480 × 480 image, avoiding repeated SD reads and PNG decoding during redraws.
LVGL warnings are forwarded to the serial log.

## Build, upload, and monitor

Install PlatformIO Core or the PlatformIO IDE extension, then run these
commands from the project directory:

```powershell
pio project config
pio run -e esp32-s3-touch-lcd-2-8c
pio run -e esp32-s3-touch-lcd-2-8c -t upload
pio device monitor -b 115200
```

For a specific serial port:

```powershell
pio run -e esp32-s3-touch-lcd-2-8c -t upload --upload-port COM5
pio device monitor -b 115200 --port COM5
```

The PlatformIO environment is pinned to Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5
through the matching pioarduino platform release. The ESP-IDF RGB panel's
`on_frame_buf_complete` callback is required by the synchronized LVGL path.

## Startup and behavior

At boot, serial output reports PSRAM, the expander, ST7701, RGB scanout buffers,
touch, SD initialization/mount, and discovered images. The display runs solid
red, green, blue, white, and black for visual color verification before loading
the UI.

Copy ordinary, non-interlaced PNG files named `UI*.png` into the SD card root.
Matching is case-insensitive; filenames are sorted in natural numeric order
(`UI2.png` before `UI10.png`). Files must have valid PNG headers and dimensions
no larger than 480 × 480. Invalid files are skipped with a serial diagnostic.
If the card cannot be mounted or no valid image is found, the display shows a
fallback screen with the cause.

- Swipe left/right to navigate in a circular loop.
- Tap within the top sixth of the screen to open or close settings. A tap is
  recognized when it lasts under 800 ms and moves no more than 35 pixels.
- Tap within the left quarter of the screen to show the previous image, or the
  right quarter to show the next image. Edge taps are disabled while settings
  are open.
- Horizontal navigation swipes require less than 55 pixels of vertical
  movement to avoid triggering navigation accidentally.
- The settings menu controls brightness (20–255), auto-dim (off/30/60/120 s),
  return-to-home (off/15/30/60 s), and automatic image rotation (off/10/5 s).
  Defaults are 90% brightness and 5-second automatic rotation. Automatic
  rotation pauses while settings are open.
- Background color can be cycled with the settings arrows through white, black,
  50% gray, orange (`RGB 233, 131, 0`), and teal (`RGB 0, 130, 148`).
- Hold still for 4 seconds for a short beep; at 5 seconds the panel is put into
  display-off mode and the backlight is turned off. A fresh touch wakes it and
  restores the selected brightness. The GT911 remains active so it can detect
  wake touches.

The firmware only scans and reads image files from the SD card.

## Refresh synchronization and verification status

The RGB peripheral has one driver owner: ESP-IDF's `esp_lcd` RGB panel driver.
It owns two full-screen RGB565 framebuffers in PSRAM and two 10-line DMA bounce
buffers in internal RAM. LVGL direct mode uses the scanout framebuffers, with
its initial render target set to the buffer that is not being scanned. The
driver's frame-buffer-complete event gates the next LVGL render. The flush
callback submits a complete frame only after LVGL finishes all dirty
rectangles. This is the driver-supported double-buffer page-flip path; the two
buffers are not merely partial LVGL render buffers.

Compilation and serial diagnostics cannot prove the physical colors, touch
orientation, SD wiring, or absence of visible tearing. Confirm all five color
screens, card/image behavior, gestures, and repeated full-screen transitions
on the actual board. Until that visual check is complete, tear-free operation
is **not** claimed.

## Troubleshooting

- **No PSRAM / framebuffer allocation error:** verify the module's OPI PSRAM
  and `qio_opi` memory configuration.
- **TCA9554 error:** check I2C pins `15/7`, address `0x20`, and board revision.
- **SD initialization or mount error:** verify SD_MMC pins CLK/CMD/D0
  `2/1/42`, TCA9554 EXIO pin 4, and that the card is FAT formatted.
- **No images:** use the card root and filenames such as `UI01.png`; dimensions
  must not exceed 480 × 480.
- **Unexpected colors or tearing:** first verify the startup color sequence
  and RGB wiring. Do not change the known-good ST7701 format/order settings
  and refresh timing simultaneously.
