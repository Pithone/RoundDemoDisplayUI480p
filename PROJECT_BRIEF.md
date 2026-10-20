# Round Demo Display UI — Fresh Project Brief

## Project goal

Create a stable, maintainable PlatformIO firmware project for the
**Waveshare ESP32-S3-Touch-LCD-2.8C**. It should display the project's PNG UI
screens from a microSD card, support touch navigation and basic display
settings, and update frames without visible tearing.

Treat this as a clean implementation. Do not copy forward experimental display
code without checking it against the acceptance criteria below.
The detailed, ready-to-use implementation prompt is in
[REFINED_PROMPT.md](./REFINED_PROMPT.md).

## Hardware baseline

- ESP32-S3, 8 MB flash, 8 MB PSRAM
- 480 × 480 ST7701 RGB display
- GT911 touch controller
- TCA9554 power/control expander at I2C address `0x20`
- microSD, with SPI mode required by the original project specification
- Existing display wiring/configuration discovered in the previous project:
  - RGB data pins: `5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17`
  - HSYNC `38`, VSYNC `39`, DE `40`, PCLK `41`
  - ST7701 command MOSI `1`, SCLK `2`, LCD backlight `6`
  - GT911 SDA `15`, SCL `7`, INT `16`, address `0x5D`
- The earlier firmware used SD_MMC pins CLK `2`, CMD `1`, D0 `42`, but that
  does not verify the SPI card wiring. Confirm the board's SPI host, MISO, MOSI,
  SCLK, and CS pins from its schematic/vendor documentation before using SD.
- Use an 8 MB-compatible partition table. Confirm the selected PlatformIO board
  configuration actually enables the external PSRAM on this board.

## Required features

1. Scan the SD card for sorted `UI*.png` files and display them at 480 × 480.
2. Horizontal touch swipes move between screens.
3. A top-to-bottom center swipe opens/closes settings.
4. Settings allow brightness adjustment, automatic dimming, and return-to-home
   timeout.
5. A stationary long touch can put the display into sleep; touch wakes it.
6. Show an understandable on-screen fallback when the SD card or images are
   missing, and log hardware initialization failures over serial.
7. Keep color accurate and image changes free of visible tearing.

## Lessons verified in the previous project

- The ST7701 initialization used `0x3A = 0x66`; the user confirmed that this
  setting restored the missing red channel in a solid-color test. Preserve the
  known-good initialization unless a deliberate test proves a replacement.
- The panel was configured with RGB order `0x36 = 0x00`. Switching channel
  order made the reported colors worse.
- LVGL's RGB565 pixel data must not be byte-swapped in the flush callback.
  Removing the unnecessary swap corrected severe monochrome/corruption.
- An LVGL image cache of two entries was added. PNG file size is not the
  decoded image size: a 480 × 480 RGB565 image occupies about 450 KiB, while
  RGBA occupies about 900 KiB. Prefer ordinary non-interlaced PNGs for
  predictable decode time; compression size alone does not explain refresh
  speed.
- LVGL's two partial render buffers are **not** two scanout framebuffers.
  With a single RGB scanout buffer, LVGL writing into it while the panel scans
  can cause tearing.
- A previous attempt to layer a second ESP-IDF RGB panel driver on top of
  LovyanGFX caused a black screen. A later replacement using a native
  two-framebuffer driver produced a solid-white screen. Neither attempt
  established a working page-flip implementation. Do not repeat either
  driver-handoff pattern or describe the result as tear-free without visual
  confirmation.
- The last known working display path was LovyanGFX. It displayed images but
  tore. Keep that path as a recoverable baseline while developing a
  single-owner, synchronized refresh solution.

## Implementation guidance

- Build a minimal board/display diagnostic before porting the UI. It should
  initialize the expander and panel, then render solid red, green, blue, white,
  and black using the same final RGB driver and settings.
- Establish and verify one display-driver owner. Do not have LovyanGFX and an
  ESP-IDF RGB driver independently configure or control the RGB peripheral.
- For tear-free updates, use a documented synchronization strategy supported
  by the selected driver, such as two scanout framebuffers with a verified
  VSYNC-boundary page flip. Ensure LVGL renders only into the non-scanning
  buffer and that the driver switches buffers safely. Do not assume that two
  LVGL buffers alone solve scanout tearing.
- Keep display timing, pixel format, channel order, and panel initialization
  unchanged while independently testing synchronization. Change one variable
  at a time.
- Add explicit initialization/allocation error messages; do not silently
  continue with a missing framebuffer, touch device, or SD card.
- Build after each stage. Flash only a build that succeeded; capture serial
  startup output after flashing. Keep the last known working image available
  until the new display path has been verified on the physical screen.

## Acceptance checks

1. PlatformIO inspect and build succeed for the intended Waveshare environment.
2. Startup serial logs confirm expander, display driver, and PSRAM status, with
   actionable errors on failure.
3. The five solid-color diagnostic screens show the expected colors.
4. The SD card mounts, PNG files load, and missing-card/missing-image fallbacks
   are visible.
5. Touch gestures and brightness/sleep settings work.
6. Repeated full-screen transitions show no visible tearing on the physical
   display. Treat this as a user-observed hardware check, not something a
   successful compile or serial log can prove.

## Suggested first prompt for a fresh implementation

> Build a new PlatformIO firmware project for the Waveshare
> ESP32-S3-Touch-LCD-2.8C (480 × 480 ST7701 RGB, GT911 touch, TCA9554 expander,
> 8 MB flash and PSRAM). First create a minimal, testable display bring-up
> using one RGB driver owner and verify red, green, blue, white, and black.
> Preserve the known-good ST7701 settings (`0x3A = 0x66`, `0x36 = 0x00`) and
> do not byte-swap LVGL RGB565 pixels. Then add a VSYNC-synchronized
> tear-free frame-update strategy, proving that LVGL renders into a
> non-scanning buffer before each page flip. Do not layer a second driver on
> top of LovyanGFX; prior handoff attempts caused black and white screens.
> Keep each stage buildable, report initialization errors explicitly, and do
> not claim tear-free operation until it has been checked on the physical
> display. After display/color/refresh are verified, add sorted `UI*.png`
> loading from SD_MMC, swipe navigation, settings, brightness/dimming, and
> touch sleep/wake. Build and test each stage and preserve a recoverable
> known-working firmware baseline.
